#pragma once
// Compute-state-only D3D12 command list tracker and restoration for the Direct Passthrough path.
// Observes D3D12Core methods via MinHook without touching descriptor heaps or graphics root state.
#include "nr_gpu_completion.hpp"

#include <d3d12.h>
#include <string>

namespace nr::pe::d3d12_direct {

// Installs D3D12Core hooks on command list, command allocator, device, and command queue.
bool install_hooks(ID3D12Device* dev, ID3D12GraphicsCommandList* cl = nullptr, std::string* why = nullptr);

// Returns true once hooks are active and `cl` has observed a clean Reset/Create state in this process.
// When false with `waiting_reset != nullptr`, sets *waiting_reset to true if waiting for first Reset.
bool ready(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, std::string* why = nullptr, bool* waiting_reset = nullptr);

// RAII scope marking Direct compute recording on `cl`. Supports nesting; restores host compute state on outermost exit.
class Scope {
public:
    Scope() = default;
    explicit Scope(ID3D12GraphicsCommandList* cl, bool restore = true);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&& other) noexcept : cl_(other.cl_), restore_(other.restore_), entered_(other.entered_) {
        other.cl_ = nullptr;
        other.restore_ = false;
        other.entered_ = false;
    }
    Scope& operator=(Scope&& other) noexcept {
        if (this != &other) {
            release();
            cl_ = other.cl_;
            restore_ = other.restore_;
            entered_ = other.entered_;
            other.cl_ = nullptr;
            other.restore_ = false;
            other.entered_ = false;
        }
        return *this;
    }
    void dismiss() { restore_ = false; }
    void release();

private:
    ID3D12GraphicsCommandList* cl_ = nullptr;
    bool restore_ = false;
    bool entered_ = false;
};

// Recording lifetime tickets tracked via hooked ExecuteCommandLists / Reset / Release.
nr::gpu::Ticket book(ID3D12CommandList* list);
bool completed(const nr::gpu::Ticket& ticket);
void note_execution(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
void begin_split_execution();
void end_split_execution(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
void seal(const nr::gpu::Ticket& ticket);
void invalidate(const nr::gpu::Ticket& ticket);

// Most recent DIRECT command queue observed by ExecuteCommandLists on the current thread.
ID3D12CommandQueue* last_direct_queue();

// Read-only diagnostics for telemetry and regression tests.
bool hooks_installed();
uint64_t host_table_replays();
uint64_t nr_set_heap_count();
uint64_t nr_set_table_count();
void reset_counters();
size_t tracked_list_count();
size_t tracked_recording_count();
size_t tracked_queue_fence_count();

}  // namespace nr::pe::d3d12_direct
