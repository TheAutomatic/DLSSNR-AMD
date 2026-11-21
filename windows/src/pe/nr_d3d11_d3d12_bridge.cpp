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
    for (size_t i = 0; i < kLatches; ++i) latch_dev_[i].store(nullptr, std::memory_order_release);
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
    return static_cast<uint32_t>(slots_.size());
}

std::string Bridge::last_error() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_error_;
}

void Bridge::release_slot(Slot& s) {
    safe_release(s.hud11);
    safe_release(s.hud12);
    safe_release(s.tex11);
    safe_release(s.tex12);
}

void Bridge::drain_d12_queue_locked() {
    if (!d12_queue_ || !shared_fence12_) return;
    if (!drain_event_) drain_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!drain_event_) return;
    const uint64_t v = ++fence_val_;
    if (SUCCEEDED(d12_queue_->Signal(shared_fence12_, v)) && shared_fence12_->GetCompletedValue() < v) {
        ResetEvent(drain_event_);
        if (SUCCEEDED(shared_fence12_->SetEventOnCompletion(v, drain_event_))) {
            WaitForSingleObject(drain_event_, 5000);
        }
    }
}

void Bridge::release_d11_side_locked() {
    if (!slots_.empty()) drain_d12_queue_locked();
    for (Slot& s : slots_) release_slot(s);
    slots_.clear();
    safe_release(shared_fence11_);
    safe_release(ctx4_);
    safe_release(d11_dev5_);
    bound_d11_ = nullptr;
}

void Bridge::release_all_locked() {
    release_d11_side_locked();
    pending_output_ = 0;
    pending_signal_queued_ = false;
    if (drain_event_) {
        CloseHandle(drain_event_);
        drain_event_ = nullptr;
    }
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
    for (size_t i = 0; i < kLatches; ++i)
        if (latch_dev_[i].load(std::memory_order_acquire) == d11_dev)
            return static_cast<Route>(latch_route_[i].load(std::memory_order_acquire));
    return Route::kUndecided;
}

void Bridge::latch_locked(ID3D11Device* d11_dev, Route route, const char* why) {
    const size_t slot = latch_next_++ % kLatches;
    latch_dev_[slot].store(nullptr, std::memory_order_release);
    latch_route_[slot].store(static_cast<int>(route), std::memory_order_release);
    latch_dev_[slot].store(d11_dev, std::memory_order_release);
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

Bridge::Slot* Bridge::ensure_slot(uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
    for (Slot& s : slots_) {
        if (s.w == w && s.h == h && s.fmt == fmt) {
            s.last_used = ++use_seq_;
            return &s;
        }
    }

    const DXGI_FORMAT wfmt = writable_format(fmt);
    if (!d3d12_uav_supported(d12_dev_, wfmt)) {
        fail_locked("back buffer format " + std::to_string(int(fmt)) + " lacks D3D12 UAV support");
        return nullptr;
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

    ID3D12Resource* tex12 = nullptr;
    HRESULT hr = d12_dev_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &rd,
                                                   D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                   IID_PPV_ARGS(&tex12));
    if (FAILED(hr) || !tex12) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "CreateCommittedResource(%ux%u fmt=%d) failed (0x%08lx)",
                      w, h, int(fmt), static_cast<unsigned long>(hr));
        fail_locked(buf);
        return nullptr;
    }

    HANDLE h_tex = nullptr;
    hr = d12_dev_->CreateSharedHandle(tex12, nullptr, GENERIC_ALL, nullptr, &h_tex);
    if (FAILED(hr) || !h_tex) {
        tex12->Release();
        char buf[128];
        std::snprintf(buf, sizeof(buf), "CreateSharedHandle(tex) failed (0x%08lx)", static_cast<unsigned long>(hr));
        fail_locked(buf);
        return nullptr;
    }

    ID3D11Texture2D* tex11 = nullptr;
    hr = d11_dev5_->OpenSharedResource1(h_tex, IID_PPV_ARGS(&tex11));
    CloseHandle(h_tex);
    if (FAILED(hr) || !tex11) {
        tex12->Release();
        char buf[128];
        std::snprintf(buf, sizeof(buf), "OpenSharedResource1 failed (0x%08lx)", static_cast<unsigned long>(hr));
        fail_locked(buf);
        return nullptr;
    }

    if (slots_.size() >= kMaxSlots) {
        auto victim = std::min_element(slots_.begin(), slots_.end(),
                                       [](const Slot& a, const Slot& b) { return a.last_used < b.last_used; });
        if (victim != slots_.end()) {
            drain_d12_queue_locked();
            release_slot(*victim);
            *victim = Slot{w, h, fmt, tex12, tex11, 0, 0, DXGI_FORMAT_UNKNOWN, nullptr, nullptr, ++use_seq_};
            return &(*victim);
        }
    }

    slots_.push_back(Slot{w, h, fmt, tex12, tex11, 0, 0, DXGI_FORMAT_UNKNOWN, nullptr, nullptr, ++use_seq_});
    nr::pe::log("[nr] d11->d3d12 bridge active: %ux%u fmt=%d (slots=%zu)", w, h, int(fmt), slots_.size());
    return &slots_.back();
}

bool Bridge::ensure_hudless_in_slot(Slot& s, uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
    if (s.hud12 && s.hud11 && s.hud_w == w && s.hud_h == h && s.hud_fmt == fmt) return true;
    if (s.hud12 || s.hud11) drain_d12_queue_locked();
    safe_release(s.hud11);
    safe_release(s.hud12);
    s.hud_w = 0;
    s.hud_h = 0;
    s.hud_fmt = DXGI_FORMAT_UNKNOWN;

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

    ID3D12Resource* hud12 = nullptr;
    if (FAILED(d12_dev_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &rd,
                                                 D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&hud12))) || !hud12) {
        return false;
    }
    HANDLE h_hud = nullptr;
    if (FAILED(d12_dev_->CreateSharedHandle(hud12, nullptr, GENERIC_ALL, nullptr, &h_hud)) || !h_hud) {
        hud12->Release();
        return false;
    }
    ID3D11Texture2D* hud11 = nullptr;
    const HRESULT open_hr = d11_dev5_->OpenSharedResource1(h_hud, IID_PPV_ARGS(&hud11));
    CloseHandle(h_hud);
    if (FAILED(open_hr) || !hud11) {
        hud12->Release();
        return false;
    }
    s.hud_w = w;
    s.hud_h = h;
    s.hud_fmt = fmt;
    s.hud12 = hud12;
    s.hud11 = hud11;
    return true;
}

Status Bridge::execute(ID3D11Device* d11_dev, ID3D11Texture2D* back11, ID3D11Texture2D* hudless11,
                       const PresentFn& fn) {
    if (abandoned_.load(std::memory_order_acquire)) return Status::kPassthrough;
    if (!d11_dev || !back11 || !fn) return Status::kPassthrough;

    D3D11_TEXTURE2D_DESC bd{};
    back11->GetDesc(&bd);

    std::lock_guard<std::mutex> lk(mu_);
    const Route route = decide_locked(d11_dev, false);
    if (route == Route::kVulkan) return Status::kFallback;
    if (route == Route::kUndecided) return Status::kPassthrough;
    // From here the device is the bridge's: nothing below sends its frame to another route.
    if (bd.Width == 0 || bd.Height == 0 || bd.SampleDesc.Count > 1) {
        fail_locked("a multisampled or empty back buffer");
        return Status::kPassthrough;
    }
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
    if (!ready_locked(d11_dev, true)) return Status::kPassthrough;

    Slot* slot = ensure_slot(bd.Width, bd.Height, bd.Format);
    if (!slot) return Status::kPassthrough;

    ID3D12Resource* hudless12 = nullptr;
    if (hudless11) {
        D3D11_TEXTURE2D_DESC hd{};
        hudless11->GetDesc(&hd);
        if (hd.Width == bd.Width && hd.Height == bd.Height && hd.SampleDesc.Count == 1 &&
            ensure_hudless_in_slot(*slot, hd.Width, hd.Height, hd.Format)) {
            hudless12 = slot->hud12;
        }
    }

    ctx4_->CopyResource(slot->tex11, back11);
    if (hudless12 && slot->hud11) ctx4_->CopyResource(slot->hud11, hudless11);

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
    fc.back = slot->tex12;
    fc.back_state = D3D12_RESOURCE_STATE_COMMON;
    fc.hudless = hudless12;
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
            // The failed frame stays dropped. Give the next Present a chance to reuse the slot.
            pending_signal_queued_ = signalled || SUCCEEDED(d12_queue_->Signal(shared_fence12_, out_val));
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s failed (0x%08lx); skipping NR until the D3D12 queue completes",
                          signalled ? "ctx4->Wait" : "d12_queue->Signal", static_cast<unsigned long>(hr));
            fail_locked(buf);
            return Status::kPassthrough;
        }
        if (!backend_declined) {
            ctx4_->CopyResource(back11, slot->tex11);
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
