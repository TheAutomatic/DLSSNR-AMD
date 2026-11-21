#pragma once
// Synchronous D3D11 <-> D3D12 NT shared-texture + shared-fence bridge: a D3D11 device's NR on AMD
// RDNA3/RDNA4 runs on Zero-Heap D3D12 ACO ELF compute pipelines (the D3D12 ACO Passthrough), without
// Vulkan or SPIR-V shader caches.
// The route is chosen once per D3D11 device (decide): the bridge, or the D3D11 Vulkan route where the
// bridge cannot serve the device at all (DXVK, non-AMD, canary fail, NR_BRIDGE=1, D3D11.4 unavailable,
// a D3D12 game's NR in the same process). A device the bridge owns never changes route: an error
// there is logged and that frame goes on without NR's work.
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace nr::pe::d11_bridge {

enum class Route {
    kUndecided,  // not chosen yet (the D3D12 passthrough's canary still runs): frames pass untouched meanwhile
    kBridge,     // the device's NR runs on D3D12 through this bridge
    kVulkan,     // the device's NR runs on the D3D11 Vulkan route
};

enum class Status {
    kRan,          // D3D12 Present layers ran and copied the result synchronously back to `back11`
    kPassthrough,  // Frame left untouched without Vulkan (warming up, an error, or recovering a handoff).
    kFallback,     // The device's route is Vulkan (decide): the caller uses the D3D11 Vulkan route
};

struct FrameContext {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Resource* back = nullptr;
    D3D12_RESOURCE_STATES back_state = D3D12_RESOURCE_STATE_COMMON;
    // Optional HUD FIX captured hudless texture copied into D3D12 (shader-resource/copyable); null when absent.
    ID3D12Resource* hudless = nullptr;
    // Output fence notified by `fn` ONLY if it actually executes GPU work on `queue`.
    ID3D12Fence** signal_out_fence = nullptr;
    uint64_t signal_out_val = 0;
    bool submitted = false;
    HRESULT output_signal = E_PENDING;
};

class Bridge {
public:
    using PresentFn = std::function<void(FrameContext&)>;

    Bridge() = default;
    ~Bridge();
    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;

    // The route of `d11_dev`, chosen at its first use and kept for as long as the process runs (reset
    // forgets it). `d3d12_game`: a D3D12 game's NR is active in this process (its Present path is not
    // shared: Vulkan). Takes the bridge's lock: never called with the caller's own lock held.
    Route decide(ID3D11Device* d11_dev, bool d3d12_game);
    // The route as chosen so far, without a lock.
    Route route(ID3D11Device* d11_dev) const;

    Status execute(ID3D11Device* d11_dev, ID3D11Texture2D* back11, ID3D11Texture2D* hudless11,
                   const PresentFn& fn);

    // Process shutdown guard (DLL_PROCESS_DETACH with lpvReserved != nullptr): skip COM/mutex teardown.
    void abandon_at_exit();

    // Clears cached resources cleanly (also exposed for explicit teardown/tests).
    void reset();

    // Diagnostics / test inspection.
    bool active() const;
    uint32_t cached_slot_count() const;
    std::string last_error() const;

private:
    struct Slot {
        uint32_t w = 0;
        uint32_t h = 0;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        ID3D12Resource* tex12 = nullptr;
        ID3D11Texture2D* tex11 = nullptr;
        // Optional HUD FIX captured hudless pair (allocated lazily only when needed for this slot).
        uint32_t hud_w = 0;
        uint32_t hud_h = 0;
        DXGI_FORMAT hud_fmt = DXGI_FORMAT_UNKNOWN;
        ID3D12Resource* hud12 = nullptr;
        ID3D11Texture2D* hud11 = nullptr;
        uint64_t last_used = 0;
    };

    static constexpr size_t kMaxSlots = 4;

    Route decide_locked(ID3D11Device* d11_dev, bool d3d12_game);
    void latch_locked(ID3D11Device* d11_dev, Route route, const char* why);
    bool ensure_d12_locked(IDXGIAdapter* adapter, const LUID& luid, std::string* why);
    bool bind_d11_locked(ID3D11Device* d11_dev, std::string* why);
    bool ready_locked(ID3D11Device* d11_dev, bool present);
    void fail_locked(const std::string& what);
    Slot* ensure_slot(uint32_t w, uint32_t h, DXGI_FORMAT fmt);
    bool ensure_hudless_in_slot(Slot& slot, uint32_t w, uint32_t h, DXGI_FORMAT fmt);
    void release_slot(Slot& slot);
    void drain_d12_queue_locked();
    void release_d11_side_locked();
    void release_all_locked();

    mutable std::mutex mu_;
    std::atomic<bool> abandoned_{false};
    // The devices' routes: written under mu_, read without it (route).
    static constexpr size_t kLatches = 4;
    std::atomic<ID3D11Device*> latch_dev_[kLatches]{};
    std::atomic<int> latch_route_[kLatches]{};
    size_t latch_next_ = 0;
    bool waiting_said_ = false;
    ULONGLONG retry_ms_ = 0;          // an error that stops the bridge for a while: no work before then
    ULONGLONG last_present_ms_ = 0;   // the bound device's last Present through the bridge
    std::string last_logged_;

    ID3D11Device* bound_d11_ = nullptr;
    LUID d12_adapter_luid_{};
    bool has_d12_adapter_luid_ = false;

    ID3D12Device* d12_dev_ = nullptr;
    ID3D12CommandQueue* d12_queue_ = nullptr;
    ID3D12Fence* shared_fence12_ = nullptr;

    ID3D11Device5* d11_dev5_ = nullptr;
    ID3D11DeviceContext4* ctx4_ = nullptr;
    ID3D11Fence* shared_fence11_ = nullptr;

    uint64_t fence_val_ = 0;
    uint64_t pending_output_ = 0;
    bool pending_signal_queued_ = false;
    uint64_t use_seq_ = 0;
    HANDLE drain_event_ = nullptr;
    std::string last_error_;
    std::vector<Slot> slots_;
};

}  // namespace nr::pe::d11_bridge
