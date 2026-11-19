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
    permanently_failed_ = false;
}

bool Bridge::active() const {
    std::lock_guard<std::mutex> lk(mu_);
    return d12_dev_ != nullptr && d11_dev5_ != nullptr && shared_fence11_ != nullptr && !permanently_failed_;
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

bool Bridge::ensure_device(ID3D11Device* d11_dev, bool* passthrough_wait) {
    *passthrough_wait = false;
    if (!d11_dev || permanently_failed_) return false;

    auto fail_dev = [&](const char* msg, HRESULT hr = S_OK) {
        char buf[256];
        if (FAILED(hr)) {
            std::snprintf(buf, sizeof(buf), "%s (0x%08lx)", msg, static_cast<unsigned long>(hr));
        } else {
            std::snprintf(buf, sizeof(buf), "%s", msg);
        }
        last_error_ = buf;
        if (!unsupported_logged_) {
            unsupported_logged_ = true;
            nr::pe::log("[nr] d11->d3d12 bridge: %s; using Vulkan bridge", buf);
        }
        permanently_failed_ = true;
        return false;
    };

    if (!nr::pe::d3d12_passthrough::enabled()) {
        return fail_dev("disabled ([D3D12Passthrough] Enabled = 0)");
    }

    if (d12_dev_ && FAILED(d12_dev_->GetDeviceRemovedReason())) {
        const HRESULT rr = d12_dev_->GetDeviceRemovedReason();
        release_all_locked();
        return fail_dev("D3D12 device removed", rr);
    }

    if (d11_dev != bound_d11_) {
        release_d11_side_locked();

        if (is_dxvk_device(d11_dev)) {
            return fail_dev("DXVK device detected");
        }

        IDXGIDevice* dxgi_dev = nullptr;
        HRESULT hr = d11_dev->QueryInterface(IID_PPV_ARGS(&dxgi_dev));
        if (FAILED(hr) || !dxgi_dev) {
            return fail_dev("QueryInterface(IDXGIDevice) failed", hr);
        }
        IDXGIAdapter* adapter = nullptr;
        hr = dxgi_dev->GetAdapter(&adapter);
        dxgi_dev->Release();
        if (FAILED(hr) || !adapter) {
            return fail_dev("IDXGIDevice::GetAdapter failed", hr);
        }

        IDXGIAdapter1* adapter1 = nullptr;
        DXGI_ADAPTER_DESC1 desc1{};
        bool adapter_ok = false;
        if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter1))) && adapter1) {
            if (SUCCEEDED(adapter1->GetDesc1(&desc1))) {
                adapter_ok = (desc1.VendorId == kAmdVendorId) && ((desc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0);
            }
            adapter1->Release();
        }
        if (!adapter_ok) {
            adapter->Release();
            char buf[128];
            std::snprintf(buf, sizeof(buf), "non-AMD or software adapter (vendor=0x%04x)", desc1.VendorId);
            return fail_dev(buf);
        }

        const bool same_adapter = d12_dev_ && has_d12_adapter_luid_ &&
                                  desc1.AdapterLuid.LowPart == d12_adapter_luid_.LowPart &&
                                  desc1.AdapterLuid.HighPart == d12_adapter_luid_.HighPart;
        if (!same_adapter) {
            safe_release(shared_fence12_);
            safe_release(d12_queue_);
            safe_release(d12_dev_);
            has_d12_adapter_luid_ = false;

            HMODULE d3d12_mod = GetModuleHandleW(L"d3d12.dll");
            if (!d3d12_mod) d3d12_mod = LoadLibraryW(L"d3d12.dll");
            auto create_fn = d3d12_mod
                ? reinterpret_cast<PFN_D3D12CreateDevice>(GetProcAddress(d3d12_mod, "D3D12CreateDevice"))
                : nullptr;
            hr = create_fn ? create_fn(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12_dev_)) : E_FAIL;
            if (FAILED(hr) || !d12_dev_) {
                adapter->Release();
                return fail_dev("D3D12CreateDevice failed", hr);
            }
            d12_adapter_luid_ = desc1.AdapterLuid;
            has_d12_adapter_luid_ = true;

            D3D12_COMMAND_QUEUE_DESC qd{};
            qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            hr = d12_dev_->CreateCommandQueue(&qd, IID_PPV_ARGS(&d12_queue_));
            if (FAILED(hr) || !d12_queue_) {
                adapter->Release();
                release_all_locked();
                return fail_dev("CreateCommandQueue failed", hr);
            }
            hr = d12_dev_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&shared_fence12_));
            if (FAILED(hr) || !shared_fence12_) {
                adapter->Release();
                release_all_locked();
                return fail_dev("CreateFence(SHARED) failed", hr);
            }
        }
        adapter->Release();

        hr = d11_dev->QueryInterface(IID_PPV_ARGS(&d11_dev5_));
        if (FAILED(hr) || !d11_dev5_) {
            release_d11_side_locked();
            return fail_dev("ID3D11Device5 unavailable", hr);
        }
        ID3D11DeviceContext* raw_ctx = nullptr;
        d11_dev->GetImmediateContext(&raw_ctx);
        if (raw_ctx) {
            hr = raw_ctx->QueryInterface(IID_PPV_ARGS(&ctx4_));
            raw_ctx->Release();
        }
        if (!ctx4_) {
            release_d11_side_locked();
            return fail_dev("ID3D11DeviceContext4 unavailable", raw_ctx ? hr : E_FAIL);
        }

        HANDLE h_fence = nullptr;
        hr = d12_dev_->CreateSharedHandle(shared_fence12_, nullptr, GENERIC_ALL, nullptr, &h_fence);
        if (FAILED(hr) || !h_fence) {
            release_d11_side_locked();
            return fail_dev("CreateSharedHandle(fence) failed", hr);
        }
        hr = d11_dev5_->OpenSharedFence(h_fence, IID_PPV_ARGS(&shared_fence11_));
        CloseHandle(h_fence);
        if (FAILED(hr) || !shared_fence11_) {
            release_d11_side_locked();
            return fail_dev("OpenSharedFence failed", hr);
        }
        bound_d11_ = d11_dev;
        fence_val_ = shared_fence12_->GetCompletedValue();
    }

    nr::pe::d3d12_passthrough::on_device(d12_dev_);
    const auto canary = nr::pe::d3d12_passthrough::canary();
    if (canary == nr::pe::d3d12_passthrough::Canary::kFail) {
        if (!canary_fail_logged_) {
            canary_fail_logged_ = true;
            nr::pe::log("[nr] d11->d3d12 bridge: D3D12 passthrough canary failed; falling back to Vulkan bridge");
        }
        last_error_ = "D3D12 passthrough canary failed";
        permanently_failed_ = true;
        return false;
    }
    if (canary != nr::pe::d3d12_passthrough::Canary::kHit) {
        *passthrough_wait = true;
        return false;
    }
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
        last_error_ = "D3D12 UAV format unsupported";
        nr::pe::log("[nr] d11->d3d12 bridge: format %d lacks D3D12 UAV support; using Vulkan bridge", int(fmt));
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
        last_error_ = buf;
        nr::pe::log("[nr] d11->d3d12 bridge: %s", buf);
        return nullptr;
    }

    HANDLE h_tex = nullptr;
    hr = d12_dev_->CreateSharedHandle(tex12, nullptr, GENERIC_ALL, nullptr, &h_tex);
    if (FAILED(hr) || !h_tex) {
        tex12->Release();
        char buf[128];
        std::snprintf(buf, sizeof(buf), "CreateSharedHandle(tex) failed (0x%08lx)", static_cast<unsigned long>(hr));
        last_error_ = buf;
        nr::pe::log("[nr] d11->d3d12 bridge: %s", buf);
        return nullptr;
    }

    ID3D11Texture2D* tex11 = nullptr;
    hr = d11_dev5_->OpenSharedResource1(h_tex, IID_PPV_ARGS(&tex11));
    CloseHandle(h_tex);
    if (FAILED(hr) || !tex11) {
        tex12->Release();
        char buf[128];
        std::snprintf(buf, sizeof(buf), "OpenSharedResource1 failed (0x%08lx)", static_cast<unsigned long>(hr));
        last_error_ = buf;
        nr::pe::log("[nr] d11->d3d12 bridge: %s", buf);
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
    if (abandoned_.load(std::memory_order_acquire)) return Status::kFallback;
    if (!d11_dev || !back11 || !fn) return Status::kFallback;
    if (env_flag_eq("NR_BRIDGE", "1") || env_flag_eq("NR_D11_D3D12_BRIDGE", "0")) return Status::kFallback;

    D3D11_TEXTURE2D_DESC bd{};
    back11->GetDesc(&bd);
    if (bd.Width == 0 || bd.Height == 0 || bd.SampleDesc.Count > 1) return Status::kFallback;

    std::lock_guard<std::mutex> lk(mu_);
    bool passthrough_wait = false;
    if (!ensure_device(d11_dev, &passthrough_wait)) {
        return passthrough_wait ? Status::kPassthrough : Status::kFallback;
    }

    Slot* slot = ensure_slot(bd.Width, bd.Height, bd.Format);
    if (!slot) {
        return Status::kFallback;
    }

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
        last_error_ = buf;
        nr::pe::log("[nr] d11->d3d12 bridge: %s", buf);
        return Status::kFallback;
    }
    // Flush D3D11 immediately so `in_val` reaches WDDM before D3D12 waits on it.
    ctx4_->Flush();
    hr = d12_queue_->Wait(shared_fence12_, in_val);
    if (FAILED(hr)) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "d12_queue->Wait failed (0x%08lx)", static_cast<unsigned long>(hr));
        last_error_ = buf;
        nr::pe::log("[nr] d11->d3d12 bridge: %s", buf);
        return Status::kFallback;
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
    if (fc.submitted && fc.signalled) {
        hr = ctx4_->Wait(shared_fence11_, out_val);
        if (FAILED(hr)) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "ctx4->Wait failed (0x%08lx)", static_cast<unsigned long>(hr));
            last_error_ = buf;
            nr::pe::log("[nr] d11->d3d12 bridge: %s", buf);
            return Status::kFallback;
        }
        if (!backend_declined) {
            ctx4_->CopyResource(back11, slot->tex11);
        }
    }

    if (backend_declined) {
        last_error_ = "D3D12 backend declined";
        nr::pe::log("[nr] d11->d3d12 bridge: D3D12 backend declined; falling back to Vulkan bridge");
        permanently_failed_ = true;
        return Status::kFallback;
    }
    return Status::kRan;
}

}  // namespace nr::pe::d11_bridge
