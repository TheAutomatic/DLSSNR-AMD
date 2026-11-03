// Record-driven D3D12 prefix for one NRPAL001 compute body.
// Does not call pbptr::retarget_note or pbptr::build_loaded_elf: those force wave64.
#pragma once
#include "pbptr_elf.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace lab {
namespace model {

struct NrRecord {
    uint64_t spv_hash = 0, code_hash = 0;
    uint32_t rsrc1 = 0, rsrc2 = 0, rsrc3 = 0, exec = 0, wg = 0, shared = 0, push_bytes = 0, user = 0;
    int set0 = -1, push = -1, grid = -1;
    std::vector<std::pair<int, int>> inlines;
    // Threadgroup dimensions from the record's shell SPIR-V (LocalSize). Product must
    // equal wg; 2D shells (e.g. noise field 8x8) set y > 1 and tidig_comp_cnt = 1.
    uint32_t dims[3] = {0, 0, 0};
    Bytes code;
    uint32_t lds_hw() const { return ((rsrc2 >> 15) & 511u) * 512u; }  // rsrc2 LDS allocation
    uint32_t tidig() const { return (rsrc2 >> 11) & 3u; }
    // NRPAL002 only: EF_AMDGPU_MACH (0x48/0x4e gfx12, gfx11 ids otherwise),
    // scratch allocation. gfx12 NRPAL001 leaves mach 0.
    uint32_t mach = 0, scratch_bytes = 0;
    bool scratch_en = false;
    bool is_gfx11() const { return mach != 0 && !(mach == 0x48 || mach == 0x4e); }
};
inline uint64_t fnv_lab(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

inline uint32_t ru32(const Bytes& b, size_t o) {
    uint32_t v = 0;
    memcpy(&v, b.data() + o, 4);
    return v;
}
inline uint64_t ru64(const Bytes& b, size_t o) {
    uint64_t v = 0;
    memcpy(&v, b.data() + o, 8);
    return v;
}
inline int32_t ri32(const Bytes& b, size_t o) {
    int32_t v = 0;
    memcpy(&v, b.data() + o, 4);
    return v;
}

inline NrRecord parse_nrpal001(const Bytes& b) {
    require(b.size() >= 96, "record too short");
    const bool v2 = memcmp(b.data(), "NRPAL002", 8) == 0;
    require(v2 || memcmp(b.data(), "NRPAL001", 8) == 0, "want NRPAL001 or NRPAL002");
    NrRecord r;
    r.spv_hash = ru64(b, 8);
    r.code_hash = ru64(b, 16);
    r.rsrc1 = ru32(b, 24);
    r.rsrc2 = ru32(b, 28);
    r.rsrc3 = ru32(b, 32);
    r.exec = ru32(b, 36);
    r.wg = ru32(b, 40);
    r.shared = ru32(b, 44);
    r.push_bytes = ru32(b, 48);
    r.user = ru32(b, 52);
    r.set0 = ri32(b, 56);
    r.push = ri32(b, 60);
    r.grid = ri32(b, 64);
    uint32_t ninline = ru32(b, 68);
    uint32_t sz = ru32(b, 72);
    require(ninline <= 32 && sz && sz % 4 == 0, "NRPAL001 sizes");
    require(96ull + 8ull * ninline + sz == b.size(), "NRPAL001 length");
    if (v2) {
        // NRPAL002 (the bundle's rdna3 set): machine id + scratch allocation.
        r.mach = ru32(b, 76);
        r.scratch_bytes = ru32(b, 80);
        r.scratch_en = (r.rsrc2 & 1u) != 0;
        for (size_t i = 84; i < 96; ++i) require(b[i] == 0, "NRPAL002 reserved field is not zero");
        require(!r.scratch_en || r.scratch_bytes > 0, "NRPAL002 scratch_en without bytes");
    } else {
        for (size_t i = 76; i < 96; ++i) require(b[i] == 0, "NRPAL001 reserved field is not zero");
    }
    for (uint32_t i = 0; i < ninline; ++i)
        r.inlines.emplace_back(ri32(b, 96 + 8 * i), ri32(b, 100 + 8 * i));
    r.code.assign(b.begin() + 96 + 8 * ninline, b.end());
    require(fnv_lab(r.code.data(), r.code.size()) == r.code_hash, "NRPAL001 code checksum");
    require(r.exec && r.exec <= sz && r.exec % 4 == 0, "NRPAL001 exec size");
    require(r.wg && r.wg <= 1024, "NRPAL001 workgroup size");
    require(!r.scratch_en, "record scratch_en is set (scratch records need a gfx11 target)");
    require(r.set0 == 0, "NRPAL001 descriptor base is not s0");
    // Same adapter-argument contract the Vulkan splicer enforces: USER_SGPRS is
    // rsrc2[5:1], no unknown config bits, declared LDS fits the rsrc2 allocation.
    require(!(r.rsrc2 & ~0x00ff9fbfu) && !(r.rsrc3 & ~0xfffu), "NRPAL001 unsupported rsrc bits");
    require(r.user == ((r.rsrc2 >> 1) & 31u), "NRPAL001 user_sgprs does not match rsrc2");
    require(r.shared <= r.lds_hw(), "NRPAL001 LDS does not match rsrc2");
    return r;
}

inline void put_u32(Bytes& o, uint32_t v) {
    size_t n = o.size();
    o.resize(n + 4);
    memcpy(o.data() + n, &v, 4);
}

// s_mov_b32 dst, src. m0 is register 125.
inline void emit_mov(Bytes& o, uint32_t dst, uint32_t src) {
    put_u32(o, 0xBE800000u | (dst << 16) | src);
}

// s_add_co_u32 dst, src, literal. 256 is not an inline constant.
inline void emit_add_lit(Bytes& o, uint32_t dst, uint32_t src, uint32_t lit) {
    put_u32(o, 0x80000000u | (dst << 16) | (255u << 8) | src);
    put_u32(o, lit);
}

// s_load_b32 dst, s[base:base+1], byte_off. base is even.
inline void emit_load_b32(Bytes& o, uint32_t dst, uint32_t base, uint32_t byte_off) {
    require((base & 1u) == 0, "s_load base is odd");
    put_u32(o, 0xF4000000u | (dst << 6) | (base >> 1));
    put_u32(o, 0xF8000000u | byte_off);
}

struct UavSgpr {
    uint32_t lo = 0, hi = 0;
};

// Root signature is one 32-bit constant plus one root UAV (two user dwords).
// The VA is the last two SGPRs of the single contiguous user-dword run.
inline UavSgpr find_uav_sgpr(const pbptr::Mp& map) {
    require(map.kind == pbptr::Mp::Arr, "user_data_reg_map is not an array");
    std::vector<uint32_t> v(map.items.size(), 0xffffffffu);
    for (size_t i = 0; i < map.items.size(); ++i) {
        if (map.items[i].kind == pbptr::Mp::UInt && map.items[i].u != 0xffffffffull)
            v[i] = uint32_t(map.items[i].u);
    }
    struct Run {
        uint32_t begin, len;
    };
    std::vector<Run> runs;
    for (size_t i = 0; i < v.size();) {
        if (v[i] == 0xffffffffu) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j + 1 < v.size() && v[j + 1] != 0xffffffffu && v[j + 1] == v[j] + 1) ++j;
        uint32_t len = uint32_t(j - i + 1);
        if (len >= 2) runs.push_back({uint32_t(i), len});
        i = j + 1;
    }
    if (runs.size() != 1)
        throw std::runtime_error("expected one user-dword run containing the root UAV, got " +
                                  std::to_string(runs.size()) + " map " + pbptr::map_line(map));
    uint32_t hi = runs[0].begin + runs[0].len - 1;
    return {hi - 1, hi};
}

// Keep the DX12 user-data map. Raise the resource counts to the ACO record.
// Wave64 is a hard error: do not rewrite it to 32.
inline void apply_pal(pbptr::Mp& root, const NrRecord& rec, std::string* map_out) {
    pbptr::Mp& pipes = pbptr::mp_need(root, "amdpal.pipelines");
    require(pipes.kind == pbptr::Mp::Arr && pipes.items.size() == 1, "pipeline count");
    pbptr::Mp& pipe = pipes.items[0];
    pbptr::Mp& cs = pbptr::mp_need(pbptr::mp_need(pipe, ".hardware_stages"), ".cs");
    require(pbptr::mp_need(cs, ".entry_point_symbol").s == "_amdgpu_cs_main", "entry symbol");
    pbptr::Mp& wave = pbptr::mp_need(cs, ".wavefront_size");
    require(wave.kind == pbptr::Mp::UInt, "wavefront type");
    if (wave.u == 64)
        throw std::runtime_error("carrier wavefront is 64; WaveSize(32) did not stick");
    require(wave.u == 32, "carrier wavefront is not 32");
    if (map_out) *map_out = pbptr::map_line(pbptr::mp_need(cs, ".user_data_reg_map"));
    uint64_t sgpr = 108;
    if (pbptr::Mp* cur = pbptr::mp_field(cs, ".sgpr_count"))
        if (cur->kind == pbptr::Mp::UInt && cur->u > sgpr) sgpr = cur->u;
    pbptr::mp_set_uint(cs, ".sgpr_count", sgpr);
    pbptr::mp_set_uint(cs, ".user_sgprs", rec.user);
    uint32_t vgpr = ((rec.rsrc1 & 63u) + 1u) * 8u;
    pbptr::mp_set_uint(cs, ".vgpr_count", vgpr);
    pbptr::mp_set_uint(cs, ".vgpr_count_max", vgpr);
    pbptr::mp_set_uint(cs, ".lds_size", rec.lds_hw());
    pbptr::mp_set_uint(cs, ".float_mode", (rec.rsrc1 >> 12) & 255u);
    pbptr::mp_set_bool(cs, ".wgp_mode", ((rec.rsrc1 >> 29) & 1u) != 0);
    pbptr::mp_set_bool(cs, ".mem_ordered", ((rec.rsrc1 >> 30) & 1u) != 0);
    pbptr::mp_set_bool(cs, ".forward_progress", ((rec.rsrc1 >> 31) & 1u) != 0);
    pbptr::mp_set_bool(cs, ".fp16_overflow", ((rec.rsrc1 >> 26) & 1u) != 0);
    pbptr::mp_set_bool(cs, ".scratch_en", false);
    pbptr::mp_set_uint(cs, ".scratch_memory_size", 0);
    // Threadgroup dimensions come from the record's shell (LocalSize); the carrier
    // is compiled with the same numthreads so the hardware TID packing matches the
    // ACO body. 2D shells (tidig_comp_cnt != 0) must pass v0 through untouched.
    require(rec.dims[0] && rec.dims[0] * rec.dims[1] * rec.dims[2] == rec.wg,
            "record threadgroup dimensions do not match wg");
    pbptr::Mp dims;
    dims.kind = pbptr::Mp::Arr;
    dims.items = {pbptr::Mp::uint(rec.dims[0]), pbptr::Mp::uint(rec.dims[1]), pbptr::Mp::uint(rec.dims[2])};
    pbptr::mp_need(cs, ".threadgroup_dimensions") = dims;
    if (pbptr::mp_field(cs, ".orig_threadgroup_dimensions"))
        pbptr::mp_need(cs, ".orig_threadgroup_dimensions") = dims;
    pbptr::Mp& cr = pbptr::mp_need(pipe, ".compute_registers");
    pbptr::mp_set_uint(cr, ".x_interleave", 0);
    pbptr::mp_set_uint(cr, ".y_interleave", 0);
    pbptr::mp_set_bool(cr, ".tgid_x_en", ((rec.rsrc2 >> 7) & 1u) != 0);
    pbptr::mp_set_bool(cr, ".tgid_y_en", ((rec.rsrc2 >> 8) & 1u) != 0);
    pbptr::mp_set_bool(cr, ".tgid_z_en", ((rec.rsrc2 >> 9) & 1u) != 0);
    pbptr::mp_set_bool(cr, ".tg_size_en", ((rec.rsrc2 >> 10) & 1u) != 0);
    pbptr::mp_set_uint(cr, ".tidig_comp_cnt", (rec.rsrc2 >> 11) & 3u);
    // Same as the Vulkan splicer: a pipeline that takes a push pointer uses the
    // spill mechanism, and PAL's threshold field says so (1); otherwise the
    // no-spill sentinel 65535.
    pbptr::Mp& pipe_ref = pipe;
    if (pbptr::mp_field(pipe_ref, ".spill_threshold"))
        pbptr::mp_set_uint(pipe_ref, ".spill_threshold", rec.push >= 0 ? 1u : 65535u);
    if (pbptr::mp_field(cs, ".shader_spill_threshold"))
        pbptr::mp_set_uint(cs, ".shader_spill_threshold", rec.push >= 0 ? 1u : 65535u);
}

inline bool blob_has(const Bytes& b, const char* s) {
    size_t n = strlen(s);
    return b.size() >= n && std::search(b.begin(), b.end(), s, s + n) != b.end();
}

// Rewrite an AMDGPU note or a raw amdpal.pipelines blob. Returns false when
// the bytes are neither, leaving them unchanged.
inline bool retarget_metadata(Bytes& blob, const NrRecord& rec, std::string* map_out) {
    if (blob.size() >= 12) {
        size_t pos = 0;
        bool found = false;
        bool any = false;
        Bytes out;
        while (pos + 12 <= blob.size()) {
            uint32_t ns = 0, ds = 0, ty = 0;
            memcpy(&ns, blob.data() + pos, 4);
            memcpy(&ds, blob.data() + pos + 4, 4);
            memcpy(&ty, blob.data() + pos + 8, 4);
            size_t name_off = pos + 12;
            size_t name_pad = (size_t(ns) + 3) & ~size_t(3);
            size_t desc_off = name_off + name_pad;
            size_t desc_pad = (size_t(ds) + 3) & ~size_t(3);
            if (ns > 64 || desc_off + ds > blob.size()) break;
            any = true;
            std::string owner(reinterpret_cast<const char*>(blob.data() + name_off),
                              strnlen(reinterpret_cast<const char*>(blob.data() + name_off), ns));
            if (ty == 32 && owner == "AMDGPU") {
                require(!found, "multiple AMDGPU metadata notes");
                size_t p = 0;
                pbptr::Mp root = pbptr::mp_decode(blob.data() + desc_off, ds, p);
                require(p == ds, "metadata did not consume the note");
                apply_pal(root, rec, map_out);
                Bytes md;
                pbptr::mp_encode(md, root);
                auto put = [&](uint32_t v) {
                    size_t at = out.size();
                    out.resize(at + 4);
                    memcpy(out.data() + at, &v, 4);
                };
                put(ns);
                put(uint32_t(md.size()));
                put(ty);
                out.insert(out.end(), blob.begin() + name_off, blob.begin() + name_off + ns);
                while (out.size() % 4) out.push_back(0);
                out.insert(out.end(), md.begin(), md.end());
                while (out.size() % 4) out.push_back(0);
                found = true;
            } else {
                size_t end = desc_off + desc_pad;
                if (end > blob.size()) break;
                out.insert(out.end(), blob.begin() + pos, blob.begin() + end);
            }
            pos = desc_off + desc_pad;
        }
        // A short tail is section padding, same as the g_pbptr note walk.
        if (found && pos + 12 > blob.size()) {
            blob.swap(out);
            return true;
        }
        (void)any;
    }
    if (!blob_has(blob, "amdpal.pipelines")) return false;
    size_t p = 0;
    pbptr::Mp root = pbptr::mp_decode(blob.data(), blob.size(), p);
    if (p != blob.size() || !pbptr::mp_field(root, "amdpal.pipelines")) return false;
    apply_pal(root, rec, map_out);
    Bytes md;
    pbptr::mp_encode(md, root);
    blob.swap(md);
    return true;
}
// Descriptor-table layout shared by the prefix and the host table builder:
// descriptors first (binding order, 16/48/32 bytes each), then the push block,
// then NumWorkGroups. 512 bytes leave room for the widest shell (336 B today).
inline constexpr uint32_t kTablePushOff = 512;
inline constexpr uint32_t kTableGridOff = 640;


inline Bytes build_prefix(const NrRecord& rec, UavSgpr uav) {
    Bytes o;
    put_u32(o, 0xB0802009u);  // s_version
    if (rec.tidig() == 0) {
        // 1D shell: keep only TIDIG.X. 2D/3D shells must pass packed v0 through
        // (x | y<<10 | z<<20); masking it would drop the y/z components.
        put_u32(o, 0x360000FFu);  // v_and_b32 v0, 0x3ff, v0
        put_u32(o, 0x000003FFu);
        put_u32(o, 0xBF870001u);  // s_delay_alu instid0(VALU_DEP_1)
    }
    // Save both halves before inline arguments overwrite carrier SGPRs 3 and 4.
    emit_mov(o, 96, uav.lo);
    emit_mov(o, 97, uav.hi);
    emit_mov(o, 0, 96);
    emit_mov(o, 125, 97);  // m0: shared address32 high word
    if (rec.push >= 0)
        emit_add_lit(o, uint32_t(rec.push), 96, kTablePushOff);
    put_u32(o, 0xBF800002u);
    // A record can require BOTH a push pointer and inline push arguments.
    for (auto [reg, dw] : rec.inlines)
        emit_load_b32(o, uint32_t(reg), 96, kTablePushOff + uint32_t(dw) * 4);
    // NumWorkGroups is distinct from the hardware workgroup IDs.
    if (rec.grid >= 0)
        for (uint32_t j=0; j<3; ++j)
            emit_load_b32(o, uint32_t(rec.grid)+j, 96, kTableGridOff+j*4);
    put_u32(o, 0xBFC70000u);  // s_wait_kmcnt 0
    put_u32(o, 0xBF800003u);
    return o;
}
struct BuiltElf {
    Bytes elf;
    Bytes prefix;
    std::string map;
    uint32_t uav_lo = 0, uav_hi = 0;
    uint32_t vgpr = 0;
    uint32_t lds = 0;
};

inline BuiltElf build_model_elf(const uint8_t* carrier_elf, size_t n, const NrRecord& rec) {
    pbptr::ElfView e = pbptr::parse_elf(carrier_elf, n);
    pbptr::ElfView::Sec* text = pbptr::find_sec(e, ".text");
    pbptr::ElfView::Sec* note = pbptr::find_sec(e, ".note");
    require(text && note, "carrier ELF is missing .text or .note");
    std::string map;
    require(retarget_metadata(note->data, rec, &map), "carrier note is not AMDGPU metadata");
    // retarget_metadata already rewrote the note. Decode the map from the saved line
    // by parsing again is unnecessary: recover the UAV from a second walk of the
    // original map string only after we still have it. Re-read the updated note.
    size_t pos = 0;
    uint32_t ns = 0, ds = 0;
    memcpy(&ns, note->data.data() + pos, 4);
    memcpy(&ds, note->data.data() + pos + 4, 4);
    size_t desc = 12 + ((size_t(ns) + 3) & ~size_t(3));
    size_t p = 0;
    pbptr::Mp root = pbptr::mp_decode(note->data.data() + desc, ds, p);
    require(p == ds, "retargeted note did not round-trip");
    pbptr::Mp& cs = pbptr::mp_need(pbptr::mp_need(pbptr::mp_need(root, "amdpal.pipelines").items[0],
                                                   ".hardware_stages"),
                                    ".cs");
    UavSgpr uav = find_uav_sgpr(pbptr::mp_need(cs, ".user_data_reg_map"));
    Bytes prefix = build_prefix(rec, uav);
    text->data = prefix;
    text->data.insert(text->data.end(), rec.code.begin(), rec.code.end());
    // Symbol covers executable bytes. The section also keeps the 0xbf9f0000 pad.
    pbptr::patch_symbol_size(e, prefix.size() + rec.exec);
    BuiltElf b;
    b.elf = pbptr::link_elf(e);
    b.prefix = std::move(prefix);
    b.map = std::move(map);
    b.uav_lo = uav.lo;
    b.uav_hi = uav.hi;
    b.vgpr = ((rec.rsrc1 & 63u) + 1u) * 8u;
    b.lds = rec.shared;
    return b;
}

inline std::string hex_bytes(const uint8_t* p, size_t n) {
    static const char* k = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        s[i * 2] = k[p[i] >> 4];
        s[i * 2 + 1] = k[p[i] & 15];
    }
    return s;
}

}  // namespace model
}  // namespace lab
