#pragma once
#include <cstdint>

namespace nr::hudcap {
enum class IndirectKind : uint32_t { Unknown, Raster, Dispatch };

// D3D12_INDIRECT_ARGUMENT_TYPE values. Deliberately keep newer/unknown actions
// conservative (including mesh/ray dispatch). Only ordinary compute is admitted.
template<class Argument>
IndirectKind indirect_kind(uint32_t count, const Argument* args) {
    if (!args || !count || count > 256) return IndirectKind::Unknown;
    bool graphics_binding = false;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t type = uint32_t(args[i].Type);
        if (type <= 2) {
            if (i + 1 != count) return IndirectKind::Unknown;
            if (type != 2) return IndirectKind::Raster;
            return graphics_binding ? IndirectKind::Unknown : IndirectKind::Dispatch;
        }
        if (type == 3 || type == 4) graphics_binding = true; // vertex/index buffer
        else if (type < 5 || type > 8) return IndirectKind::Unknown;
    }
    return IndirectKind::Unknown;
}

// A compute shader cannot write a texture without ALLOW_UNORDERED_ACCESS.
// Merely leaving that texture bound as an RTV is not an indirect draw on it.
inline bool indirect_cannot_write(IndirectKind kind, uint32_t max_count, bool allow_uav) {
    return max_count == 0 || (kind == IndirectKind::Dispatch && !allow_uav);
}

// A compute dispatch writes a texture only through a UAV view of it, and only while the texture is in
// D3D12_RESOURCE_STATE_UNORDERED_ACCESS (0x8; legacy barriers). `known`: the state is the one this command list's own
// last transition of that texture left it in (seen while armed, no split begin / aliasing / enhanced barrier since).
// COMMON (0, also PRESENT) is not taken as proof.
inline bool compute_cannot_write_by_state(bool known, uint32_t state) {
    return known && state != 0 && (state & 0x8u) == 0;
}
// ... or the texture never had a UAV view made: `followed` - every UAV view creation of its device was seen (hooked
// before the device was handed out); `maybe_view` - a view of it may have been made (a set-only Bloom filter: false is
// exact). Asked after the list's submission (a view may be made between recording and submission).
inline bool compute_cannot_write_by_views(bool followed, bool maybe_view) {
    return followed && !maybe_view;
}
} // namespace nr::hudcap
