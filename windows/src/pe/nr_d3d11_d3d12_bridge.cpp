// Synchronous D3D11 <-> D3D12 NT shared-texture + shared-fence bridge for no-guide D3D11 Present
// layers (see nr_d3d11_d3d12_bridge.hpp).
#include "nr_d3d11_d3d12_bridge.hpp"

#include "nr_d3d12_passthrough.hpp"
#include "nr_dlssnr_model.hpp"
#include "nr_pe_log.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nr::pe::d11_bridge {

namespace {

// DXVK's IDXGIVkInteropDevice UUID (matches nr_pe_interop.hpp without pulling Vulkan headers).
constexpr GUID kIID_IDXGIVkInteropDevice = {
    0xe2ef5fa5u, 0xdc21u, 0x4af7u, {0x90, 0xc4, 0xf6, 0x7e, 0xf6, 0xa0, 0x93, 0x23}
};

constexpr UINT kAmdVendorId = 0x1002u;

// A device's route, kept with the device itself (its private data): it lasts exactly as long as the device does -
// any number of devices, and none mistaken for one that lived at the same address before.
const GUID kLatchData = {
    0x6b0f6c6du, 0x3a52u, 0x4c1eu, {0x9d, 0x41, 0x2e, 0x7a, 0x55, 0xc3, 0x18, 0x0f}
};
struct Latch {
    const void* owner;   // the Bridge that chose it
    uint32_t epoch;      // that Bridge's reset count then
    Route route;
};

bool env_flag_eq(const char* name, const char* expected) {
    char buf[16]{};
    const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return n > 0 && n < sizeof(buf) && std::strcmp(buf, expected) == 0;
}

bool is_dxvk_device(ID3D11Device* d11_dev) {
    if (!d11_dev) return false;
    IUnknown* interop = nullptr;
    if (SUCCEEDED(d11_dev->QueryInterface(kIID_IDXGIVkInteropDevice, reinterpret_cast<void**>(&interop))) && interop) {
        interop->Release();
        return true;
    }
    return false;
}

DXGI_FORMAT writable_format(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
        default: return fmt;
    }
}

bool d3d12_uav_supported(ID3D12Device* dev, DXGI_FORMAT fmt) {
    if (!dev || fmt == DXGI_FORMAT_UNKNOWN) return false;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = 4;
    d.Height = 4;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* probe = nullptr;
    if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                            nullptr, IID_PPV_ARGS(&probe))) || !probe) {
        return false;
    }
    probe->Release();
    return true;
}

template <class T>
void safe_release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

struct NoVkFallbackGuard {
    explicit NoVkFallbackGuard(bool on) {
        (void)nr::dlssnr::take_d3d12_backend_declined();
        nr::dlssnr::set_d3d12_no_vk_fallback(on);
    }
    ~NoVkFallbackGuard() { nr::dlssnr::set_d3d12_no_vk_fallback(false); }
};

using PFN_D3D12CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

// The hardware AMD adapter `d11_dev` renders with (a reference) and its LUID; null with `why` otherwise.
IDXGIAdapter* amd_adapter_of(ID3D11Device* d11_dev, LUID* luid, std::string* why) {
    IDXGIDevice* dxgi_dev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    if (FAILED(d11_dev->QueryInterface(IID_PPV_ARGS(&dxgi_dev))) || !dxgi_dev ||
        FAILED(dxgi_dev->GetAdapter(&adapter)) || !adapter) {
        if (dxgi_dev) dxgi_dev->Release();
        *why = "the device's adapter could not be asked";
        return nullptr;
    }
    dxgi_dev->Release();
    IDXGIAdapter1* adapter1 = nullptr;
    DXGI_ADAPTER_DESC1 desc1{};
    bool ok = false;
    if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter1))) && adapter1) {
        if (SUCCEEDED(adapter1->GetDesc1(&desc1)))
            ok = desc1.VendorId == kAmdVendorId && (desc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0;
        adapter1->Release();
    }
    if (!ok) {
        adapter->Release();
        char buf[96];
        std::snprintf(buf, sizeof(buf), "a non-AMD or software adapter (vendor 0x%04x)", desc1.VendorId);
        *why = buf;
        return nullptr;
    }
    *luid = desc1.AdapterLuid;
    return adapter;
}

}  // namespace

Bridge::~Bridge() {
    if (abandoned_.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(mu_);
    release_all_locked();
}

void Bridge::abandon_at_exit() {
    abandoned_.store(true, std::memory_order_release);
}

void Bridge::reset() {
    if (abandoned_.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(mu_);
    release_all_locked();
    epoch_.fetch_add(1, std::memory_order_acq_rel);   // every device's route is chosen again
    retry_ms_ = 0;
    waiting_said_ = false;
    last_logged_.clear();
}

bool Bridge::active() const {
    std::lock_guard<std::mutex> lk(mu_);
    return d12_dev_ != nullptr && d11_dev5_ != nullptr && shared_fence11_ != nullptr;
}

uint32_t Bridge::cached_slot_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return cached_slot_count_locked();
}

uint32_t Bridge::cached_slot_count_locked() const {
    return static_cast<uint32_t>(std::count_if(pool_.begin(), pool_.end(),
                                               [](const Entry& e) { return e.role == Role::kBack; }));
}

std::string Bridge::last_error() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_error_;
}

void Bridge::release_entry(Entry& e) {
    safe_release(e.tex11);
    safe_release(e.tex12);
}

// Asked once per format and device: the answer is a resource made and released, too much for every handoff.
bool Bridge::uav_supported_locked(DXGI_FORMAT fmt) {
    const auto it = uav_ok_.find(fmt);
    if (it != uav_ok_.end()) return it->second;
    const bool ok = d3d12_uav_supported(d12_dev_, fmt);
    uav_ok_.emplace(fmt, ok);
    return ok;
}

// Everything submitted to the bridge's queue so far is done (or its device is removed: nothing runs there any
// more). False: not known to be - the wait ran out or could not be made - and nothing the queue may still use is
// released then. On a fence of the queue's own: the shared one also moves with D3D11's signals, which say nothing
// of the D3D12 queue.
bool Bridge::drain_d12_queue_locked() {
    if (!d12_queue_ || !d12_dev_) return true;
    if (FAILED(d12_dev_->GetDeviceRemovedReason())) return true;
    if (!drain_event_) drain_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!drain_fence_ && FAILED(d12_dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&drain_fence_))))
        drain_fence_ = nullptr;
    if (!drain_event_ || !drain_fence_) return false;
    const uint64_t v = ++drain_val_;
    bool done = false;
    if (SUCCEEDED(d12_queue_->Signal(drain_fence_, v)) && SUCCEEDED(drain_fence_->SetEventOnCompletion(v, drain_event_))) {
        // (the event may still be set by an earlier value's wait that ran out: the fence's value decides, not the wake)
        const uint64_t until = GetTickCount64() + 5000;
        while (!(done = drain_fence_->GetCompletedValue() >= v)) {
            const uint64_t now = GetTickCount64();
            if (now >= until || WaitForSingleObject(drain_event_, DWORD(until - now)) != WAIT_OBJECT_0) break;
        }
    }
    if (!done) done = FAILED(d12_dev_->GetDeviceRemovedReason());
    if (done) {
        for (Entry& e : late_) release_entry(e);
        late_.clear();
    }
    return done;
}

// A shared pair leaves the pool: released once the queue is known to be done with it, kept until then (late_,
// released by the next drain that succeeds).
void Bridge::retire_locked(Entry& e, bool drained) {
    if (drained) {
        release_entry(e);
    } else {
        if (late_.empty())
            nr::pe::log("[nr] d11->d3d12 bridge: the D3D12 queue did not finish in time; the shared textures it may "
                        "still use are kept until it has");
        late_.push_back(e);
    }
    e = {};
}

void Bridge::release_d11_side_locked() {
    const bool drained = pool_.empty() || drain_d12_queue_locked();
    for (Entry& e : pool_) retire_locked(e, drained);
    pool_.clear();
    depth_pass_.reset();
    safe_release(depth_stage11_);
    safe_release(shared_fence11_);
    safe_release(ctx4_);
    safe_release(d11_dev5_);
    bound_d11_ = nullptr;
}

void Bridge::release_all_locked() {
    release_d11_side_locked();
    // What the queue never finished in time (late_) is left to the process: releasing it under running GPU work is
    // the worse end.
    if (!late_.empty() && !drain_d12_queue_locked()) late_.clear();
    uav_ok_.clear();
    pending_output_ = 0;
    pending_signal_queued_ = false;
    if (drain_event_) {
        CloseHandle(drain_event_);
        drain_event_ = nullptr;
    }
    safe_release(drain_fence_);
    safe_release(shared_fence12_);
    safe_release(d12_queue_);
    safe_release(d12_dev_);
    d12_adapter_luid_ = {};
    has_d12_adapter_luid_ = false;
}

void Bridge::fail_locked(const std::string& what) {
    last_error_ = what;
    if (what == last_logged_) return;   // (an error that repeats every frame is said once)
    last_logged_ = what;
    nr::pe::log("[nr] d11->d3d12 bridge: %s; that frame goes on without NR's work", what.c_str());
}

Route Bridge::route(ID3D11Device* d11_dev) const {
    if (!d11_dev) return Route::kUndecided;
    Latch latch{};
    UINT size = sizeof(latch);
    if (FAILED(d11_dev->GetPrivateData(kLatchData, &size, &latch)) || size != sizeof(latch)) return Route::kUndecided;
    if (latch.owner != this || latch.epoch != epoch_.load(std::memory_order_acquire)) return Route::kUndecided;
    return latch.route;
}

void Bridge::latch_locked(ID3D11Device* d11_dev, Route route, const char* why) {
    const Latch latch{this, epoch_.load(std::memory_order_acquire), route};
    if (FAILED(d11_dev->SetPrivateData(kLatchData, sizeof(latch), &latch)))
        nr::pe::log("[nr] D3D11 route: the route of D3D11 device %p could not be kept with it; it is chosen again at "
                    "each use", static_cast<void*>(d11_dev));
    if (route == Route::kBridge) {
        nr::pe::log("[nr] D3D11 route: NR on D3D11 device %p runs on D3D12 (the D3D11 -> D3D12 bridge), chosen once "
                    "for this device", static_cast<void*>(d11_dev));
    } else {
        last_error_ = why;
        nr::pe::log("[nr] D3D11 route: NR on D3D11 device %p runs on the Vulkan route (%s), chosen once for this device",
                    static_cast<void*>(d11_dev), why);
    }
}

Route Bridge::decide(ID3D11Device* d11_dev, bool d3d12_game) {
    if (!d11_dev || abandoned_.load(std::memory_order_acquire)) return Route::kUndecided;
    const Route have = route(d11_dev);
    if (have != Route::kUndecided) return have;
    std::lock_guard<std::mutex> lk(mu_);
    return decide_locked(d11_dev, d3d12_game);
}

// The first use of a D3D11 device: everything that says whether the bridge can serve it at all, once. What fails
// here is the Vulkan route's device; what fails later on a device of the bridge's is an error (fail_locked).
Route Bridge::decide_locked(ID3D11Device* d11_dev, bool d3d12_game) {
    const Route have = route(d11_dev);
    if (have != Route::kUndecided) return have;
    const auto vulkan = [&](const std::string& why) {
        latch_locked(d11_dev, Route::kVulkan, why.c_str());
        return Route::kVulkan;
    };
    if (env_flag_eq("NR_BRIDGE", "1")) return vulkan("NR_BRIDGE=1");
    if (env_flag_eq("NR_D11_D3D12_BRIDGE", "0")) return vulkan("NR_D11_D3D12_BRIDGE=0");
    if (!nr::pe::d3d12_passthrough::enabled()) return vulkan("[D3D12Passthrough] Enabled = 0");
    if (d3d12_game) return vulkan("a D3D12 game's NR runs in this process");
    if (is_dxvk_device(d11_dev)) return vulkan("a DXVK device");

    std::string why;
    LUID luid{};
    IDXGIAdapter* adapter = amd_adapter_of(d11_dev, &luid, &why);
    if (!adapter) return vulkan(why);
    {   // D3D11.4: the shared fence and the context's Signal / Wait
        ID3D11Device5* dev5 = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        ID3D11DeviceContext4* ctx4 = nullptr;
        d11_dev->GetImmediateContext(&ctx);
        const bool ok = SUCCEEDED(d11_dev->QueryInterface(IID_PPV_ARGS(&dev5))) && dev5 && ctx &&
                        SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx4))) && ctx4;
        safe_release(ctx4);
        safe_release(ctx);
        safe_release(dev5);
        if (!ok) {
            adapter->Release();
            return vulkan("ID3D11Device5 / ID3D11DeviceContext4 unavailable");
        }
    }
    const bool made = ensure_d12_locked(adapter, luid, &why);
    adapter->Release();
    if (!made) return vulkan(why);

    nr::pe::d3d12_passthrough::on_device(d12_dev_);
    const auto canary = nr::pe::d3d12_passthrough::canary();
    if (canary == nr::pe::d3d12_passthrough::Canary::kFail) return vulkan("the D3D12 passthrough's canary failed");
    if (canary != nr::pe::d3d12_passthrough::Canary::kHit) {
        if (!waiting_said_) {
            waiting_said_ = true;
            nr::pe::log("[nr] D3D11 route: waiting for the D3D12 passthrough's canary before choosing the route of "
                        "D3D11 device %p; its frames pass untouched meanwhile", static_cast<void*>(d11_dev));
        }
        return Route::kUndecided;
    }
    // The shared fence on this device, unless another device's frames are being served right now (then at this
    // device's first frame: ready_locked).
    if (d11_dev != bound_d11_ && (!bound_d11_ || GetTickCount64() - last_present_ms_ >= 1000) &&
        !bind_d11_locked(d11_dev, &why))
        return vulkan(why);
    latch_locked(d11_dev, Route::kBridge, "");
    return Route::kBridge;
}

// Our D3D12 device, queue and shared fence on the adapter the D3D11 device renders with.
bool Bridge::ensure_d12_locked(IDXGIAdapter* adapter, const LUID& luid, std::string* why) {
    if (d12_dev_ && FAILED(d12_dev_->GetDeviceRemovedReason())) release_all_locked();
    if (d12_dev_ && has_d12_adapter_luid_ && luid.LowPart == d12_adapter_luid_.LowPart &&
        luid.HighPart == d12_adapter_luid_.HighPart)
        return true;
    release_all_locked();

    const auto failed = [&](const char* what, HRESULT hr) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s (0x%08lx)", what, static_cast<unsigned long>(hr));
        *why = buf;
        release_all_locked();
        return false;
    };
    HMODULE d3d12_mod = GetModuleHandleW(L"d3d12.dll");
    if (!d3d12_mod) d3d12_mod = LoadLibraryW(L"d3d12.dll");
    auto create_fn = d3d12_mod
        ? reinterpret_cast<PFN_D3D12CreateDevice>(GetProcAddress(d3d12_mod, "D3D12CreateDevice"))
        : nullptr;
    HRESULT hr = create_fn ? create_fn(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12_dev_)) : E_FAIL;
    if (FAILED(hr) || !d12_dev_) return failed("D3D12CreateDevice failed", hr);
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = d12_dev_->CreateCommandQueue(&qd, IID_PPV_ARGS(&d12_queue_));
    if (FAILED(hr) || !d12_queue_) return failed("CreateCommandQueue failed", hr);
    hr = d12_dev_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&shared_fence12_));
    if (FAILED(hr) || !shared_fence12_) return failed("CreateFence(SHARED) failed", hr);
    d12_adapter_luid_ = luid;
    has_d12_adapter_luid_ = true;
    fence_val_ = 0;
    return true;
}

// The D3D11 side on `d11_dev`: its D3D11.4 interfaces and our fence opened on it. What another device had goes.
bool Bridge::bind_d11_locked(ID3D11Device* d11_dev, std::string* why) {
    release_d11_side_locked();
    const auto failed = [&](const char* what, HRESULT hr) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s (0x%08lx)", what, static_cast<unsigned long>(hr));
        *why = buf;
        release_d11_side_locked();
        return false;
    };
    HRESULT hr = d11_dev->QueryInterface(IID_PPV_ARGS(&d11_dev5_));
    if (FAILED(hr) || !d11_dev5_) return failed("ID3D11Device5 unavailable", hr);
    ID3D11DeviceContext* raw_ctx = nullptr;
    d11_dev->GetImmediateContext(&raw_ctx);
    hr = raw_ctx ? raw_ctx->QueryInterface(IID_PPV_ARGS(&ctx4_)) : E_FAIL;
    if (raw_ctx) raw_ctx->Release();
    if (FAILED(hr) || !ctx4_) return failed("ID3D11DeviceContext4 unavailable", hr);
    HANDLE h_fence = nullptr;
    hr = d12_dev_->CreateSharedHandle(shared_fence12_, nullptr, GENERIC_ALL, nullptr, &h_fence);
    if (FAILED(hr) || !h_fence) return failed("CreateSharedHandle(fence) failed", hr);
    hr = d11_dev5_->OpenSharedFence(h_fence, IID_PPV_ARGS(&shared_fence11_));
    CloseHandle(h_fence);
    if (FAILED(hr) || !shared_fence11_) return failed("OpenSharedFence failed", hr);
    bound_d11_ = d11_dev;
    ++binding_;
    fence_val_ = std::max(fence_val_, shared_fence12_->GetCompletedValue());
    return true;
}

// A frame of a device the bridge owns: our D3D12 device alive (made again 5 s after it was lost) and the D3D11 side
// on `d11_dev`. The presenting device has the bridge: another device's evaluates do not take it away while it presents.
bool Bridge::ready_locked(ID3D11Device* d11_dev, bool present) {
    const ULONGLONG now = GetTickCount64();
    if (now < retry_ms_) return false;
    std::string why;
    if (d12_dev_ && FAILED(d12_dev_->GetDeviceRemovedReason())) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "our D3D12 device was removed (0x%08lx); made again in 5 s",
                      static_cast<unsigned long>(d12_dev_->GetDeviceRemovedReason()));
        release_all_locked();
        retry_ms_ = now + 5000;
        fail_locked(buf);
        return false;
    }
    if (!d12_dev_) {
        LUID luid{};
        IDXGIAdapter* adapter = amd_adapter_of(d11_dev, &luid, &why);
        const bool made = adapter && ensure_d12_locked(adapter, luid, &why);
        if (adapter) adapter->Release();
        if (!made) {
            retry_ms_ = now + 5000;
            fail_locked(why + "; tried again in 5 s");
            return false;
        }
        nr::pe::d3d12_passthrough::on_device(d12_dev_);
    }
    if (nr::pe::d3d12_passthrough::canary() != nr::pe::d3d12_passthrough::Canary::kHit) return false;
    if (d11_dev != bound_d11_) {
        if (!present && bound_d11_ && now - last_present_ms_ < 1000) {
            fail_locked("another D3D11 device presents through the bridge; this device's evaluates are left alone");
            return false;
        }
        if (!bind_d11_locked(d11_dev, &why)) {
            retry_ms_ = now + 5000;
            fail_locked(why + "; tried again in 5 s");
            return false;
        }
    }
    if (present) last_present_ms_ = now;
    return true;
}

// The shared pair of `role` at this size and format: found, or made (the least recently used of its
// role going first when the role is full).
bool Bridge::ensure_entry_locked(Role role, uint32_t set, uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool uav,
                                 size_t* index) {
    size_t same = 0, oldest = pool_.size();
    for (size_t i = 0; i < pool_.size(); ++i) {
        Entry& e = pool_[i];
        if (e.role != role || e.set != set) continue;
        if (e.w == w && e.h == h && e.fmt == fmt && e.uav == uav) {
            e.last_used = ++use_seq_;
            *index = i;
            return true;
        }
        ++same;
        if (oldest == pool_.size() || e.last_used < pool_[oldest].last_used) oldest = i;
    }
    // The role full: its oldest pair goes, once the queue is done with it (D3D12 does not keep a resource alive for
    // its GPU work). Not known to be: the pair stays and this handoff is left out - nothing piles up behind a queue
    // that does not move.
    const size_t cap = role == Role::kBack || role == Role::kHudless ? kMaxSlots : 1;
    if (same >= cap && !drain_d12_queue_locked()) {
        fail_locked("the D3D12 queue did not finish in time; its shared textures stay as they are");
        return false;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = fmt;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    if (uav) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    const auto failed = [&](const char* what, HRESULT hr) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s (%ux%u fmt=%d) failed (0x%08lx)", what, w, h, int(fmt),
                      static_cast<unsigned long>(hr));
        fail_locked(buf);
        return false;
    };
    ID3D12Resource* tex12 = nullptr;
    HRESULT hr = d12_dev_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &rd,
                                                   D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                   IID_PPV_ARGS(&tex12));
    if (FAILED(hr) || !tex12) return failed("CreateCommittedResource", hr);
    HANDLE h_tex = nullptr;
    hr = d12_dev_->CreateSharedHandle(tex12, nullptr, GENERIC_ALL, nullptr, &h_tex);
    if (FAILED(hr) || !h_tex) {
        tex12->Release();
        return failed("CreateSharedHandle(tex)", hr);
    }
    ID3D11Texture2D* tex11 = nullptr;
    hr = d11_dev5_->OpenSharedResource1(h_tex, IID_PPV_ARGS(&tex11));
    CloseHandle(h_tex);
    if (FAILED(hr) || !tex11) {
        tex12->Release();
        return failed("OpenSharedResource1", hr);
    }

    const Entry made{role, set, w, h, fmt, uav, tex12, tex11, ++use_seq_};
    if (same >= cap) {
        release_entry(pool_[oldest]);
        pool_[oldest] = made;
        *index = oldest;
        return true;
    }
    pool_.push_back(made);
    *index = pool_.size() - 1;
    if (role == Role::kBack)
        nr::pe::log("[nr] d11->d3d12 bridge active: %ux%u fmt=%d (slots=%u)", w, h, int(fmt), cached_slot_count_locked());
    return true;
}

// The shared pair for `item`, its region clipped to the texture.
bool Bridge::prepare_locked(Handoff& item, size_t* index, std::string* why) {
    item.tex12 = nullptr;
    if (!item.tex11) { *why = "no texture"; return false; }
    D3D11_TEXTURE2D_DESC d{};
    item.tex11->GetDesc(&d);
    if (!d.Width || !d.Height || d.ArraySize != 1 || d.SampleDesc.Count != 1) {
        *why = "a multisampled, array or empty texture";
        return false;
    }
    if (!item.w || !item.h || item.depth) { item.x = item.y = 0; item.w = d.Width; item.h = d.Height; }
    if (item.x >= d.Width || item.y >= d.Height) { *why = "a region outside its texture"; return false; }
    item.w = std::min(item.w, d.Width - item.x);
    item.h = std::min(item.h, d.Height - item.y);
    // The back buffer and its hudless frame in their own format (the Present layers map it); the rest in the format
    // D3D12 can write (an sRGB format's UNORM twin: D3D11 copies between the two).
    const bool as_is = item.role == Role::kBack || item.role == Role::kHudless;
    const DXGI_FORMAT fmt = item.depth ? DXGI_FORMAT_R32_FLOAT : as_is ? d.Format : writable_format(d.Format);
    if (item.role == Role::kBack && !uav_supported_locked(writable_format(d.Format))) {
        *why = "back buffer format " + std::to_string(int(d.Format)) + " lacks D3D12 unordered access";
        return false;
    }
    const bool uav = item.writable && uav_supported_locked(fmt);
    if (!ensure_entry_locked(item.role, item.set, item.w, item.h, fmt, uav, index)) { *why = last_error_; return false; }
    item.tex12 = pool_[*index].tex12;
    return true;
}

bool Bridge::copy_in_locked(const Handoff& item, const Entry& e, std::string* why) {
    D3D11_TEXTURE2D_DESC d{};
    item.tex11->GetDesc(&d);
    // A depth buffer that already is one 32-bit float per pixel is the shared texture's own format family: copied
    // as it is. The depth pass is for the formats that are not (D24S8, D16, D32S8).
    // (D3D11 copies a depth buffer only whole.)
    const bool plain_depth = item.depth && d.MipLevels == 1 && d.SampleDesc.Count == 1 && item.w == d.Width &&
                             item.h == d.Height &&
                             (d.Format == DXGI_FORMAT_R32_TYPELESS || d.Format == DXGI_FORMAT_D32_FLOAT ||
                              d.Format == DXGI_FORMAT_R32_FLOAT);
    if (item.depth && !plain_depth) {
        // D3D11's depth pass writes a texture of ours (unordered access is not asked of the shared one)
        D3D11_TEXTURE2D_DESC have{};
        if (depth_stage11_) depth_stage11_->GetDesc(&have);
        if (!depth_stage11_ || have.Width != e.w || have.Height != e.h) {
            safe_release(depth_stage11_);
            depth_pass_.reset();   // (its views are of the old texture)
            D3D11_TEXTURE2D_DESC d{};
            d.Width = e.w; d.Height = e.h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R32_FLOAT;
            d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            if (FAILED(d11_dev5_->CreateTexture2D(&d, nullptr, &depth_stage11_)) || !depth_stage11_) {
                depth_stage11_ = nullptr;
                *why = "the depth pass's texture could not be made";
                return false;
            }
        }
        if (!depth_pass_) depth_pass_.reset(new nr::pe::bridge::DepthCopyD3D11());
        if (!depth_pass_->run(d11_dev5_, ctx4_, item.tex11, depth_stage11_, why)) return false;
        ctx4_->CopyResource(e.tex11, depth_stage11_);
        return true;
    }
    if (d.MipLevels == 1 && item.w == d.Width && item.h == d.Height) {
        ctx4_->CopyResource(e.tex11, item.tex11);
    } else {
        const D3D11_BOX box{item.x, item.y, 0, item.x + item.w, item.y + item.h, 1};
        ctx4_->CopySubresourceRegion(e.tex11, 0, 0, 0, 0, item.tex11, 0, &box);
    }
    return true;
}

Status Bridge::execute(ID3D11Device* d11_dev, ID3D11Texture2D* back11, ID3D11Texture2D* hudless11,
                       const PresentFn& fn) {
    if (!back11) return Status::kPassthrough;
    Handoff items[2];
    size_t count = 1;
    items[0].role = Role::kBack;
    items[0].tex11 = back11;
    items[0].back = true;
    if (hudless11) {
        D3D11_TEXTURE2D_DESC bd{}, hd{};
        back11->GetDesc(&bd);
        hudless11->GetDesc(&hd);
        if (hd.Width == bd.Width && hd.Height == bd.Height) {
            items[1].role = Role::kHudless;
            items[1].tex11 = hudless11;
            items[1].optional = true;
            count = 2;
        }
    }
    return execute(d11_dev, items, count, true, fn);
}

Status Bridge::execute(ID3D11Device* d11_dev, Handoff* items, size_t count, bool present, const PresentFn& fn) {
    if (abandoned_.load(std::memory_order_acquire)) return Status::kPassthrough;
    if (!d11_dev || !items || !count || !fn) return Status::kPassthrough;

    std::lock_guard<std::mutex> lk(mu_);
    const Route route = decide_locked(d11_dev, false);
    if (route == Route::kVulkan) return Status::kFallback;
    if (route == Route::kUndecided) return Status::kPassthrough;
    // From here the device is the bridge's: nothing below sends its frame to another route.
    // No new D3D11 writes, fence values or slot eviction until the previous D3D12 submission is done.
    if (pending_output_) {
        const uint64_t done = shared_fence12_->GetCompletedValue();
        if (done == UINT64_MAX) {
            if (d11_dev == bound_d11_) return Status::kPassthrough;
            release_all_locked();   // the removed device cannot still be using its slots
        } else if (!pending_signal_queued_) {
            pending_signal_queued_ = SUCCEEDED(d12_queue_->Signal(shared_fence12_, pending_output_));
            return Status::kPassthrough;
        } else if (done < pending_output_) {
            return Status::kPassthrough;
        } else {
            pending_output_ = 0;
        }
    }
    if (!ready_locked(d11_dev, present)) return Status::kPassthrough;

    // Every pair first (making one may wait for the D3D12 queue), then the copies.
    std::vector<size_t> entry(count, SIZE_MAX);
    std::string why;
    for (size_t i = 0; i < count; ++i) {
        if (prepare_locked(items[i], &entry[i], &why)) continue;
        entry[i] = SIZE_MAX;
        items[i].tex12 = nullptr;
        if (items[i].optional) continue;
        fail_locked(why);
        return Status::kPassthrough;
    }
    for (size_t i = 0; i < count; ++i) {
        if (entry[i] == SIZE_MAX || !items[i].in) continue;
        if (copy_in_locked(items[i], pool_[entry[i]], &why)) continue;
        entry[i] = SIZE_MAX;
        items[i].tex12 = nullptr;
        if (items[i].optional) continue;
        fail_locked(why);
        return Status::kPassthrough;
    }

    const uint64_t in_val = ++fence_val_;
    const uint64_t out_val = ++fence_val_;
    HRESULT hr = ctx4_->Signal(shared_fence11_, in_val);
    if (FAILED(hr)) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "ctx4->Signal failed (0x%08lx)", static_cast<unsigned long>(hr));
        fail_locked(buf);
        return Status::kPassthrough;
    }
    // Flush D3D11 immediately so `in_val` reaches WDDM before D3D12 waits on it.
    ctx4_->Flush();
    hr = d12_queue_->Wait(shared_fence12_, in_val);
    if (FAILED(hr)) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "d12_queue->Wait failed (0x%08lx)", static_cast<unsigned long>(hr));
        fail_locked(buf);
        return Status::kPassthrough;
    }

    FrameContext fc{};
    fc.device = d12_dev_;
    fc.queue = d12_queue_;
    fc.binding = binding_;
    fc.handoff = present ? handoffs_ : ++handoffs_;
    fc.items = items;
    fc.count = count;
    fc.back = fc.find(Role::kBack);
    fc.back_state = D3D12_RESOURCE_STATE_COMMON;
    fc.hudless = fc.find(Role::kHudless);
    fc.signal_out_fence = &shared_fence12_;
    fc.signal_out_val = out_val;

    {
        NoVkFallbackGuard no_vk(true);
        fn(fc);
    }
    const bool backend_declined = nr::dlssnr::take_d3d12_backend_declined();

    // Only wait on `shared_fence11_` when D3D12 actually submitted and signalled `out_val`;
    // waiting unconditionally on an early return would deadlock D3D11.
    if (fc.submitted) {
        const bool signalled = SUCCEEDED(fc.output_signal);
        hr = signalled ? ctx4_->Wait(shared_fence11_, out_val) : fc.output_signal;
        if (FAILED(hr)) {
            pending_output_ = out_val;
            // The failed frame stays dropped. Give the next handoff a chance to reuse the textures.
            pending_signal_queued_ = signalled || SUCCEEDED(d12_queue_->Signal(shared_fence12_, out_val));
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s failed (0x%08lx); skipping NR until the D3D12 queue completes",
                          signalled ? "ctx4->Wait" : "d12_queue->Signal", static_cast<unsigned long>(hr));
            fail_locked(buf);
            return Status::kPassthrough;
        }
        if (!backend_declined) {
            for (size_t i = 0; i < count; ++i) {
                if (entry[i] == SIZE_MAX || !items[i].back) continue;
                const Entry& e = pool_[entry[i]];
                if (items[i].back_to) {
                    ctx4_->CopySubresourceRegion(items[i].back_to, 0, 0, 0, 0, e.tex11, 0, nullptr);
                    continue;
                }
                D3D11_TEXTURE2D_DESC d{};
                items[i].tex11->GetDesc(&d);
                if (d.MipLevels == 1 && e.w == d.Width && e.h == d.Height) ctx4_->CopyResource(items[i].tex11, e.tex11);
                else ctx4_->CopySubresourceRegion(items[i].tex11, 0, items[i].x, items[i].y, 0, e.tex11, 0, nullptr);
            }
        }
    }

    if (backend_declined) {
        // The D3D12 backend would not take the frame: the bridge rests for 5 s, and the device stays its own.
        retry_ms_ = GetTickCount64() + 5000;
        fail_locked("the D3D12 backend declined the frame; tried again in 5 s");
        return Status::kPassthrough;
    }
    return Status::kRan;
}

}  // namespace nr::pe::d11_bridge
