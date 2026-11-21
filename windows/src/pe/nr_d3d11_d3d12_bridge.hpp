#pragma once
// Synchronous D3D11 <-> D3D12 NT shared-texture + shared-fence bridge: a D3D11 device's NR on AMD
// RDNA3/RDNA4 runs on Zero-Heap D3D12 ACO ELF compute pipelines (the D3D12 ACO Passthrough), without
// Vulkan or SPIR-V shader caches.
// The route is chosen once per D3D11 device (decide): the bridge, or the D3D11 Vulkan route where the
// bridge cannot serve the device at all (DXVK, non-AMD, canary fail, NR_BRIDGE=1, D3D11.4 unavailable,
// a D3D12 game's NR in the same process). A device the bridge owns never changes route: an error
// there is logged and that frame goes on without NR's work.
#include "nr_pe_bridge.hpp"

#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
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

// What a texture handed across is for. Each role has one shared pair (a D3D12 texture and the same texture opened on
// the D3D11 device), made again when its size or format changes; the back buffer and its hudless frame keep up to four
// (the least recently used goes first).
enum class Role : uint8_t {
    kBack,         // the swap chain's back buffer (Present layers)
    kHudless,      // HUD FIX's frame before the HUD
    kColour,       // the evaluate's colour region (layers before the upscaler)
    kOutput,       // the evaluate's output (layers after the upscaler)
    kDepth,        // the evaluate's depth, for its own layers
    kMotion,       // the evaluate's motion vectors, for its own layers
    kExposure,     // the evaluate's exposure texture
};

// One texture of a handoff: the game's D3D11 texture and what happens to it on the way to D3D12 and back.
struct Handoff {
    Role role = Role::kBack;
    uint32_t set = 0;                      // tells two textures of one role apart
    ID3D11Texture2D* tex11 = nullptr;      // the game's texture (mip 0 of a plain 2D, one-sample texture)
    uint32_t x = 0, y = 0, w = 0, h = 0;   // its region handed over (w or h 0: all of it)
    bool in = true;         // copied into the shared texture before the D3D12 work
    bool back = false;      // copied back into the game's texture (the same region) after the D3D12 work ran
    ID3D11Texture2D* back_to = nullptr;   // with `back`: copied into this texture at its origin instead (the game's
                                          // own is never written)
    bool depth = false;     // a depth buffer: read into R32_FLOAT by a D3D11 compute pass first (depth and typeless
                            // formats shared across the APIs are not relied on)
    bool writable = false;  // D3D12 writes it in place: made with unordered access where its format allows
    bool optional = false;  // a texture that cannot be handed over is left out (tex12 null) instead of stopping the frame
    // Filled by the bridge: the shared texture on D3D12 (in COMMON, and left there), null when left out.
    ID3D12Resource* tex12 = nullptr;
};

struct FrameContext {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    // Which binding of the bridge to a D3D11 device this frame is of (never 0): what one binding's frames left
    // behind - guides - is not another's.
    uint64_t binding = 0;
    // Every texture of the handoff, and the one of a role (null: not handed over).
    Handoff* items = nullptr;
    size_t count = 0;
    ID3D12Resource* find(Role role, uint32_t set = 0) const {
        for (size_t i = 0; i < count; ++i)
            if (items[i].role == role && items[i].set == set) return items[i].tex12;
        return nullptr;
    }
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

    // One synchronous round trip: every `in` texture copied into its shared twin by the device's immediate context,
    // D3D11 signal -> D3D12 wait -> `fn` (records and submits on the bridge's queue) -> D3D12 signal -> D3D11 wait,
    // then every `back` texture copied back. `present`: a Present of the device's swap chain (the presenting device
    // has the bridge); else one of its evaluates.
    Status execute(ID3D11Device* d11_dev, Handoff* items, size_t count, bool present, const PresentFn& fn);
    // The Present layers' handoff: the back buffer there and back, HUD FIX's hudless frame there.
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
    struct Entry {
        Role role = Role::kBack;
        uint32_t set = 0;
        uint32_t w = 0;
        uint32_t h = 0;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;   // the shared texture's
        bool uav = false;
        ID3D12Resource* tex12 = nullptr;
        ID3D11Texture2D* tex11 = nullptr;
        uint64_t last_used = 0;
    };

    static constexpr size_t kMaxSlots = 4;

    Route decide_locked(ID3D11Device* d11_dev, bool d3d12_game);
    void latch_locked(ID3D11Device* d11_dev, Route route, const char* why);
    bool ensure_d12_locked(IDXGIAdapter* adapter, const LUID& luid, std::string* why);
    bool bind_d11_locked(ID3D11Device* d11_dev, std::string* why);
    bool ready_locked(ID3D11Device* d11_dev, bool present);
    void fail_locked(const std::string& what);
    bool ensure_entry_locked(Role role, uint32_t set, uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool uav, size_t* index);
    bool prepare_locked(Handoff& item, size_t* index, std::string* why);
    bool copy_in_locked(const Handoff& item, const Entry& e, std::string* why);
    void release_entry(Entry& e);
    uint32_t cached_slot_count_locked() const;
    bool drain_d12_queue_locked();
    void retire_locked(Entry& e, bool drained);
    bool uav_supported_locked(DXGI_FORMAT fmt);
    void release_d11_side_locked();
    void release_all_locked();

    mutable std::mutex mu_;
    std::atomic<bool> abandoned_{false};
    // The devices' routes are kept with the devices (their private data); reset makes all of them stale.
    std::atomic<uint32_t> epoch_{0};
    bool waiting_said_ = false;
    ULONGLONG retry_ms_ = 0;          // an error that stops the bridge for a while: no work before then
    ULONGLONG last_present_ms_ = 0;   // the bound device's last Present through the bridge
    std::string last_logged_;

    ID3D11Device* bound_d11_ = nullptr;
    uint64_t binding_ = 0;            // counts the bindings to a D3D11 device (never 0 once bound)
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
    std::vector<Entry> pool_;
    std::vector<Entry> late_;                 // out of the pool, not yet known to be out of the queue's work
    std::map<DXGI_FORMAT, bool> uav_ok_;      // the D3D12 device's unordered access, by format
    // The D3D11 depth pass and the R32_FLOAT texture it writes (copied into the shared one), on the bound device.
    std::unique_ptr<nr::pe::bridge::DepthCopyD3D11> depth_pass_;
    ID3D11Texture2D* depth_stage11_ = nullptr;
};

}  // namespace nr::pe::d11_bridge
