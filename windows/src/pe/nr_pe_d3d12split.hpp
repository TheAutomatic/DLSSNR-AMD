#pragma once
// Cutting a D3D12 command list in two, so that work on another API can run between the halves.
//
// An upscaler's evaluate is recorded into the game's command list, between the game's work that made
// its inputs and the work that reads its output. Under vkd3d-proton that list is a VkCommandBuffer
// and the network records into it. On the native runtime it is not, and nothing can be put in the
// middle of a D3D12 command list that waits for another API: fences are queue operations. So the list
// is cut at the evaluate:
//
//   - everything the list holds so far stays in it, and it is closed there;
//   - every call the game (and the upscaler after us) makes on the list from then on is redirected
//     to a continuation list of the same class, created on the list's own device, which starts with
//     the list's state as it was when the evaluate began;
//   - when the game executes the list, the call is expanded on the same queue into
//         list -> signal -> [our work, waiting on the signal] -> wait -> continuation -> rest.
//
// The redirection is a patch of the list class's vtable: every recording method goes through a small
// stub that swaps `this` for the continuation while a cut is active, and the state-setting methods go
// through hooks that also remember the list's state since its last Reset (descriptor heaps, root
// signatures and arguments, pipeline, render targets, viewports, input assembly, ...), which is what
// the continuation is given. ExecuteCommandLists and Reset are hooked the same way.
//
// x86-64 only.
#include <d3d12.h>

#include "nr_gpu_completion.hpp"

#include <memory>

#include <cstdint>
#include <functional>
#include <vector>

#include <functional>
#include <memory>
#include <string>

namespace nr::pe::split {

// The completion evidence this layer already keeps: the per-queue fence signalled after each expansion the hooks
// execute, and how far the GPU has got through it. A recording made into a list of this queue may be released once
// the value it was booked at has completed - the same watermark the continuations are reused by, not another one.
uint64_t queue_value(ID3D12CommandQueue* queue);      // the last value signalled on that queue's fence (0: none)
uint64_t queue_completed(ID3D12CommandQueue* queue);  // the value the GPU has finished (0: unknown)

// A recording's ticket: booked when work is recorded into the list, stamped by every ExecuteCommandLists that runs
// it (the plain path included), ended by the list's Reset. completed() is the GPU's answer for all the stamps; a
// discarded (Reset without execution) recording invalidates its history instead. 0: booking failed.
// The rule completed() applies, as its own function so the sequences (never submitted, Reset failed, sealed with a
// pending fence, executed twice, discarded after execution) can be checked without a device. `receipt_done` holds one
// entry per receipt: whether that execution has finished.
inline bool tickets_complete(bool sealed, bool discarded, const std::vector<bool>& receipt_done) {
    // Nothing was ever submitted: the recording is finished once it is over (a successful Reset or a discard).
    if (receipt_done.empty()) return sealed || discarded;
    for (bool d : receipt_done) if (!d) return false;   // discarded stops future submissions, not these waits
    return true;
}

std::shared_ptr<nr::gpu::Recording> book(ID3D12CommandList* list);
bool completed(const std::shared_ptr<nr::gpu::Recording>& rec);
void invalidate(const std::shared_ptr<nr::gpu::Recording>& rec);
// The step ExecuteCommandLists takes for the lists it runs (the hooks call this), and the one Reset takes when it
// succeeded - public so a test can drive the same path without the hooks.
void note_execution(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
void seal(const std::shared_ptr<nr::gpu::Recording>& rec);

// Hooks the list's class (once per class) and its queues'. False when lists of this class cannot be
// cut, or when this list's state since its last Reset is not known yet - the hooks were installed
// after it was reset - in which case the frame passes through and the next one is cut.
bool prepare(ID3D12GraphicsCommandList* list, std::string* why);
// The cut is off for good in this process (a class could not be hooked, or a cut list was reset unexecuted):
// prepare() never succeeds again. False while a refusal is only this list's or this moment's.
bool disabled();

// The list's recorded state at this moment, for cut().
struct Snapshot;
std::shared_ptr<Snapshot> snapshot(ID3D12GraphicsCommandList* list);

// What runs between the halves, called inside the queue's ExecuteCommandLists after the first half
// has been handed to the queue. It must leave the queue waiting (on the GPU) for its work, or do
// nothing at all. `discard` is called instead when the list is reset or the cut abandoned without the
// list ever being executed.
struct Job {
    std::function<void(ID3D12CommandQueue* queue)> run;
    std::function<void()> discard;
};

// Put `state` back on `list` itself (the evaluate bound things of its own and then could not cut).
void restore(ID3D12GraphicsCommandList* list, const std::shared_ptr<Snapshot>& state);

// Cut `list` here. The continuation starts with `state` (taken when the evaluate began, so the game
// gets its own state back, not whatever the evaluate bound).
bool cut(ID3D12GraphicsCommandList* list, Job job, const std::shared_ptr<Snapshot>& state, std::string* why);

// The direct queue this thread last handed command lists to, once the queue class is hooked (null: none seen). For
// NR Control (nr_pe_menu.cpp): a D3D12 swap chain does not say which queue presents it, and a game presents on the
// thread that has just executed its frame on that queue.
ID3D12CommandQueue* last_direct_queue();

}  // namespace nr::pe::split
