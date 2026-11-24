#pragma once
// Adaptive push-constant layout reconciliation between Windows host structs
// (PushGemm / PushVAttn, which carry act_lo/act_hi at `gap`) and precompiled
// ACO machine-code records (.nrp). FP8 records preserve the 8-byte BDA gap,
// while INT4 records omit it (Linux layout). Detecting this once from the
// record header removes directory heuristics and preserves zero frame overhead.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nr {

inline bool aco_needs_compact_push(uint32_t gap, uint32_t host_push_bytes, uint32_t record_push_bytes) {
    return gap != 0 && host_push_bytes >= gap + 8u && record_push_bytes > gap &&
           record_push_bytes + 8u <= host_push_bytes;
}

inline void aco_compact_push(uint8_t* data, size_t bytes, uint32_t gap) {
    if (!data || bytes < size_t(gap) + 8u) return;
    std::memmove(data + gap, data + gap + 8u, bytes - gap - 8u);
    std::memset(data + bytes - 8u, 0, 8u);
}

}  // namespace nr
