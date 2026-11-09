#pragma once
#include <cstdint>
#include <vector>
namespace nr::hudcap {
struct DescriptorRange { uint32_t type, count, offset; }; // D3D12 range type; UINT_MAX = APPEND/unbounded
struct DescriptorSpan { uint64_t offset, count; };
// Resolve APPEND across ALL types, then retain only SRVs. Never overflow an unbounded predecessor.
inline std::vector<DescriptorSpan> srv_spans(const std::vector<DescriptorRange>& ranges) {
    std::vector<DescriptorSpan> out;
    uint64_t next=0;
    for (const auto& r:ranges) {
        const uint64_t at=r.offset==UINT32_MAX ? next : r.offset;
        const uint64_t end=r.count==UINT32_MAX ? UINT64_MAX : at>UINT64_MAX-r.count ? UINT64_MAX : at+r.count;
        if (r.type==0 && at!=UINT64_MAX && r.count) out.push_back({at,r.count==UINT32_MAX ? UINT64_MAX : r.count});
        next=end;
    }
    return out;
}
inline DescriptorSpan clip_span(DescriptorSpan s, uint64_t available) {
    if (s.offset>=available) return {0,0};
    const uint64_t left=available-s.offset;
    return {s.offset,s.count<left?s.count:left};
}
}
