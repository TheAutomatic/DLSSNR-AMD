// Load the gfx1201 g_pbptr ACO body into a D3D12 PAL ELF.
// A 60-byte prefix is the entry adapter. The 424-byte body is copied whole,
// except one store-scope bit applied in build_loaded_elf.
#pragma once
#include "util.hpp"

#include <cstring>
#include <sstream>
#include <string>

namespace lab {
namespace pbptr {

inline constexpr size_t kAcoBytes = 424;
inline constexpr char kAcoSha[] =
    "fb081569011274e43c272858b129169a81f2f9737469bc3e8be68b50cc440419";
// The 9070 XT DXIL compiler puts 11 root constants at s2..s12 (user dwords
// 4..14) and the root UAV at s13..s14. g_pbptr wants desc low, push low,
// w[0..7], and the VA high in s0, s1, s2..s9, and m0. This prefix moves that
// proven window. s_version stays first. The final delay's SKIP_2 is the body's
// v_lshl_add (three instructions later: two scalar moves and one s_load).
//   s_version
//   s_mov_b32 s0, s2 / s1, s3 / m0, s12
//   s_mov_b32 s2, s4 / s3, s5 / s4, s6 / s5, s7 / s6, s8 / s7, s9 / s8, s10 / s9, s11
//   v_and_b32 v0, 0x3ff, v0
//   s_delay_alu instskip(SKIP_2) | instid1(VALU_DEP_1)
inline constexpr uint8_t kPrefix[] = {
    0x09, 0x20, 0x80, 0xb0, 0x02, 0x00, 0x80, 0xbe, 0x03, 0x00, 0x81, 0xbe, 0x0c, 0x00, 0xfd, 0xbe,
    0x04, 0x00, 0x82, 0xbe, 0x05, 0x00, 0x83, 0xbe, 0x06, 0x00, 0x84, 0xbe, 0x07, 0x00, 0x85, 0xbe,
    0x08, 0x00, 0x86, 0xbe, 0x09, 0x00, 0x87, 0xbe, 0x0a, 0x00, 0x88, 0xbe, 0x0b, 0x00, 0x89, 0xbe,
    0xff, 0x00, 0x00, 0x36, 0xff, 0x03, 0x00, 0x00, 0xb0, 0x00, 0x87, 0xbf,
};

struct Mp {
    enum Kind { Nil, Bool, UInt, Neg, Str, Arr, Map, Raw } kind = Nil;
    uint64_t u = 0;
    int64_t sval = 0;
    bool b = false;
    std::string s;
    Bytes raw;
    std::vector<Mp> items;
    std::vector<std::pair<Mp, Mp>> fields;
    static Mp uint(uint64_t v) {
        Mp m;
        m.kind = UInt;
        m.u = v;
        return m;
    }
    static Mp boolean(bool v) {
        Mp m;
        m.kind = Bool;
        m.b = v;
        return m;
    }
};

inline uint64_t mp_big(const uint8_t* b, size_t n, size_t& p, size_t width) {
    require(p + width <= n, "msgpack truncated");
    uint64_t v = 0;
    for (size_t i = 0; i < width; ++i) v = (v << 8) | b[p++];
    return v;
}

inline Mp mp_decode(const uint8_t* b, size_t n, size_t& p, unsigned depth = 0) {
    require(depth < 40 && p < n, "msgpack nesting or eof");
    unsigned c = b[p++];
    Mp m;
    if (c <= 0x7f) return Mp::uint(c);
    if (c >= 0xe0) {
        m.kind = Mp::Neg;
        m.sval = int64_t(int8_t(c));
        return m;
    }
    if (c == 0xc0) return m;
    if (c == 0xc2 || c == 0xc3) return Mp::boolean(c == 0xc3);
    if (c >= 0xcc && c <= 0xcf) return Mp::uint(mp_big(b, n, p, size_t(1) << (c - 0xcc)));
    if (c >= 0xd0 && c <= 0xd3) {
        size_t w = size_t(1) << (c - 0xd0);
        uint64_t u = mp_big(b, n, p, w);
        int64_t s = int64_t(u);
        if (w < 8 && (u & (1ull << (w * 8 - 1)))) s = int64_t(u | (~0ull << (w * 8)));
        m.kind = Mp::Neg;
        m.sval = s;
        return m;
    }
    auto take_str = [&](size_t len) {
        require(p + len <= n, "msgpack string truncated");
        m.kind = Mp::Str;
        m.s.assign(reinterpret_cast<const char*>(b + p), len);
        p += len;
        return m;
    };
    if (c >= 0xa0 && c <= 0xbf) return take_str(c & 31);
    if (c >= 0xd9 && c <= 0xdb) return take_str(size_t(mp_big(b, n, p, size_t(1) << (c - 0xd9))));
    if ((c & 0xf0) == 0x90 || c == 0xdc || c == 0xdd) {
        size_t len = (c & 0xf0) == 0x90 ? (c & 15) : size_t(mp_big(b, n, p, c == 0xdc ? 2 : 4));
        m.kind = Mp::Arr;
        for (size_t i = 0; i < len; ++i) m.items.push_back(mp_decode(b, n, p, depth + 1));
        return m;
    }
    if ((c & 0xf0) == 0x80 || c == 0xde || c == 0xdf) {
        size_t len = (c & 0xf0) == 0x80 ? (c & 15) : size_t(mp_big(b, n, p, c == 0xde ? 2 : 4));
        m.kind = Mp::Map;
        for (size_t i = 0; i < len; ++i) {
            Mp k = mp_decode(b, n, p, depth + 1);
            Mp v = mp_decode(b, n, p, depth + 1);
            m.fields.emplace_back(std::move(k), std::move(v));
        }
        return m;
    }
    size_t raw_n = 0;
    size_t start = p - 1;
    if (c == 0xca) raw_n = 4;
    else if (c == 0xcb) raw_n = 8;
    else if (c >= 0xc4 && c <= 0xc6) raw_n = size_t(mp_big(b, n, p, size_t(1) << (c - 0xc4)));
    else require(false, "unsupported msgpack tag");
    require(p + raw_n <= n, "msgpack raw truncated");
    p += raw_n;
    m.kind = Mp::Raw;
    m.raw.assign(b + start, b + p);
    return m;
}

inline void mp_putbig(Bytes& o, uint64_t v, size_t w) {
    for (size_t i = 0; i < w; ++i) o.push_back(uint8_t(v >> (8 * (w - 1 - i))));
}

inline void mp_encode(Bytes& o, const Mp& m) {
    switch (m.kind) {
    case Mp::Nil: o.push_back(0xc0); break;
    case Mp::Bool: o.push_back(m.b ? 0xc3 : 0xc2); break;
    case Mp::Neg: {
        int64_t s = m.sval;
        if (s >= 0) {
            mp_encode(o, Mp::uint(uint64_t(s)));
            break;
        }
        if (s >= -32) o.push_back(uint8_t(int8_t(s)));
        else if (s >= -128) { o.push_back(0xd0); o.push_back(uint8_t(int8_t(s))); }
        else if (s >= -32768) { o.push_back(0xd1); mp_putbig(o, uint64_t(uint16_t(int16_t(s))), 2); }
        else if (s >= int64_t(-2147483647 - 1)) { o.push_back(0xd2); mp_putbig(o, uint64_t(uint32_t(int32_t(s))), 4); }
        else { o.push_back(0xd3); mp_putbig(o, uint64_t(s), 8); }
        break;
    }
    case Mp::UInt:
        if (m.u <= 0x7f) o.push_back(uint8_t(m.u));
        else if (m.u <= 0xff) { o.push_back(0xcc); o.push_back(uint8_t(m.u)); }
        else if (m.u <= 0xffff) { o.push_back(0xcd); mp_putbig(o, m.u, 2); }
        else if (m.u <= 0xffffffffu) { o.push_back(0xce); mp_putbig(o, m.u, 4); }
        else { o.push_back(0xcf); mp_putbig(o, m.u, 8); }
        break;
    case Mp::Str: {
        size_t n = m.s.size();
        if (n <= 31) o.push_back(uint8_t(0xa0 | n));
        else if (n <= 0xff) { o.push_back(0xd9); o.push_back(uint8_t(n)); }
        else if (n <= 0xffff) { o.push_back(0xda); mp_putbig(o, n, 2); }
        else { o.push_back(0xdb); mp_putbig(o, n, 4); }
        o.insert(o.end(), m.s.begin(), m.s.end());
        break;
    }
    case Mp::Arr: {
        size_t n = m.items.size();
        if (n <= 15) o.push_back(uint8_t(0x90 | n));
        else if (n <= 0xffff) { o.push_back(0xdc); mp_putbig(o, n, 2); }
        else { o.push_back(0xdd); mp_putbig(o, n, 4); }
        for (const auto& x : m.items) mp_encode(o, x);
        break;
    }
    case Mp::Map: {
        size_t n = m.fields.size();
        if (n <= 15) o.push_back(uint8_t(0x80 | n));
        else if (n <= 0xffff) { o.push_back(0xde); mp_putbig(o, n, 2); }
        else { o.push_back(0xdf); mp_putbig(o, n, 4); }
        for (const auto& f : m.fields) { mp_encode(o, f.first); mp_encode(o, f.second); }
        break;
    }
    case Mp::Raw: o.insert(o.end(), m.raw.begin(), m.raw.end()); break;
    }
}

inline Mp* mp_field(Mp& m, const char* key) {
    if (m.kind != Mp::Map) return nullptr;
    for (auto& f : m.fields)
        if (f.first.kind == Mp::Str && f.first.s == key) return &f.second;
    return nullptr;
}

inline Mp& mp_need(Mp& m, const char* key) {
    Mp* f = mp_field(m, key);
    require(f != nullptr, std::string("missing metadata field ") + key);
    return *f;
}

inline void mp_set_uint(Mp& m, const char* key, uint64_t v) {
    require(m.kind == Mp::Map, "metadata set on a non-map");
    if (Mp* f = mp_field(m, key)) *f = Mp::uint(v);
    else {
        Mp k;
        k.kind = Mp::Str;
        k.s = key;
        m.fields.emplace_back(std::move(k), Mp::uint(v));
    }
}

inline void mp_set_bool(Mp& m, const char* key, bool v) {
    require(m.kind == Mp::Map, "metadata set on a non-map");
    if (Mp* f = mp_field(m, key)) *f = Mp::boolean(v);
    else {
        Mp k;
        k.kind = Mp::Str;
        k.s = key;
        m.fields.emplace_back(std::move(k), Mp::boolean(v));
    }
}

struct ElfView {
    Bytes bytes;
    uint16_t shnum = 0, shstrndx = 0;
    struct Sec {
        uint32_t name = 0, type = 0, link = 0, info = 0;
        uint64_t flags = 0, addr = 0, align = 0, entsize = 0;
        Bytes data;
        std::string label;
    };
    std::vector<Sec> secs;
};

inline uint64_t rd_u64(const uint8_t* p) {
    uint64_t v = 0;
    memcpy(&v, p, 8);
    return v;
}
inline uint32_t rd_u32(const uint8_t* p) {
    uint32_t v = 0;
    memcpy(&v, p, 4);
    return v;
}
inline uint16_t rd_u16(const uint8_t* p) {
    uint16_t v = 0;
    memcpy(&v, p, 2);
    return v;
}

inline ElfView parse_elf(const uint8_t* elf, size_t n) {
    require(n >= 64 && elf[0] == 0x7f && elf[1] == 'E' && elf[2] == 'L' && elf[3] == 'F', "not an ELF");
    require(elf[4] == 2 && elf[5] == 1, "not ELF64 LE");
    ElfView e;
    e.bytes.assign(elf, elf + n);
    uint64_t shoff = rd_u64(elf + 40);
    require(rd_u16(elf + 58) == 64, "unexpected section header size");
    e.shnum = rd_u16(elf + 60);
    e.shstrndx = rd_u16(elf + 62);
    require(e.shnum > 0 && e.shnum < 64 && e.shstrndx < e.shnum, "bad section count");
    require(shoff + uint64_t(e.shnum) * 64 <= n, "section table outside ELF");
    e.secs.resize(e.shnum);
    for (uint16_t i = 0; i < e.shnum; ++i) {
        const uint8_t* sh = elf + shoff + uint64_t(i) * 64;
        auto& s = e.secs[i];
        s.name = rd_u32(sh);
        s.type = rd_u32(sh + 4);
        s.flags = rd_u64(sh + 8);
        s.addr = rd_u64(sh + 16);
        uint64_t off = rd_u64(sh + 24);
        uint64_t sz = rd_u64(sh + 32);
        s.link = rd_u32(sh + 40);
        s.info = rd_u32(sh + 44);
        s.align = rd_u64(sh + 48);
        s.entsize = rd_u64(sh + 56);
        require(off <= n && sz <= n - off, "section outside ELF");
        s.data.assign(elf + off, elf + off + sz);
    }
    const auto& strs = e.secs[e.shstrndx].data;
    for (auto& s : e.secs) {
        require(s.name < strs.size(), "section name outside shstrtab");
        size_t end = s.name;
        while (end < strs.size() && strs[end] != 0) ++end;
        s.label.assign(reinterpret_cast<const char*>(strs.data() + s.name), end - s.name);
    }
    return e;
}

inline ElfView::Sec* find_sec(ElfView& e, const char* name) {
    for (auto& s : e.secs)
        if (s.label == name) return &s;
    return nullptr;
}

inline std::string map_line(const Mp& map) {
    std::ostringstream o;
    require(map.kind == Mp::Arr, "user_data_reg_map is not an array");
    for (size_t i = 0; i < map.items.size(); ++i) {
        if (map.items[i].kind == Mp::UInt && map.items[i].u != 0xffffffffu)
            o << "sgpr" << i << "=" << map.items[i].u << " ";
    }
    return o.str();
}

inline void retarget_note(Bytes& note, uint32_t ud_base, std::string* original_map) {
    require(note.size() >= 12, "note truncated");
    size_t pos = 0;
    Bytes out;
    bool found = false;
    while (pos + 12 <= note.size()) {
        uint32_t ns = rd_u32(note.data() + pos);
        uint32_t ds = rd_u32(note.data() + pos + 4);
        uint32_t ty = rd_u32(note.data() + pos + 8);
        size_t name_off = pos + 12;
        size_t name_pad = (size_t(ns) + 3) & ~size_t(3);
        size_t desc_off = name_off + name_pad;
        size_t desc_pad = (size_t(ds) + 3) & ~size_t(3);
        require(desc_off + ds <= note.size(), "note descriptor outside section");
        std::string owner(reinterpret_cast<const char*>(note.data() + name_off),
                          strnlen(reinterpret_cast<const char*>(note.data() + name_off), ns));
        if (ty == 32 && owner == "AMDGPU") {
            require(!found, "multiple AMDGPU metadata notes");
            size_t p = 0;
            Mp root = mp_decode(note.data() + desc_off, ds, p);
            require(p == ds, "metadata did not consume the note");
            Mp& pipes = mp_need(root, "amdpal.pipelines");
            require(pipes.kind == Mp::Arr && pipes.items.size() == 1, "pipeline count");
            Mp& pipe = pipes.items[0];
            Mp& stages = mp_need(pipe, ".hardware_stages");
            Mp& cs = mp_need(stages, ".cs");
            require(mp_need(cs, ".entry_point_symbol").s == "_amdgpu_cs_main", "entry symbol");
            require(mp_need(cs, ".wavefront_size").u == 64, "wavefront size");
            if (original_map) *original_map = map_line(mp_need(cs, ".user_data_reg_map"));
            // Keep the DXIL compiler's user-dword map. Rewriting it to s0 left
            // every lane unstored on the 9070 XT. Only the allocation counts
            // and wgp mode have to grow to fit this kernel.
            (void)ud_base;
            mp_set_uint(cs, ".sgpr_count", 108);
            mp_set_uint(cs, ".vgpr_count", 48);
            mp_set_uint(cs, ".vgpr_count_max", 48);
            mp_set_bool(cs, ".wgp_mode", true);
            Mp& cr = mp_need(pipe, ".compute_registers");
            mp_set_bool(cr, ".tgid_x_en", true);
            mp_set_bool(cr, ".tgid_y_en", false);
            mp_set_bool(cr, ".tgid_z_en", false);
            Mp dims;
            dims.kind = Mp::Arr;
            dims.items = {Mp::uint(64), Mp::uint(1), Mp::uint(1)};
            mp_need(cs, ".threadgroup_dimensions") = dims;
            if (mp_field(cs, ".orig_threadgroup_dimensions"))
                mp_need(cs, ".orig_threadgroup_dimensions") = dims;
            Bytes md;
            mp_encode(md, root);
            auto put_u32 = [&](uint32_t v) {
                size_t at = out.size();
                out.resize(at + 4);
                memcpy(out.data() + at, &v, 4);
            };
            put_u32(ns);
            put_u32(uint32_t(md.size()));
            put_u32(ty);
            out.insert(out.end(), note.begin() + name_off, note.begin() + name_off + ns);
            while (out.size() % 4) out.push_back(0);
            out.insert(out.end(), md.begin(), md.end());
            while (out.size() % 4) out.push_back(0);
            found = true;
        } else {
            size_t end = desc_off + desc_pad;
            require(end <= note.size(), "note padding outside section");
            out.insert(out.end(), note.begin() + pos, note.begin() + end);
        }
        pos = desc_off + desc_pad;
    }
    require(found, "AMDGPU metadata note not found");
    note.swap(out);
}

inline void patch_symbol_size(ElfView& e, uint64_t text_size) {
    ElfView::Sec* sym = find_sec(e, ".symtab");
    ElfView::Sec* str = find_sec(e, ".strtab");
    require(sym && str && sym->entsize == 24 && sym->link < e.secs.size(), "symtab");
    require(&e.secs[sym->link] == str, "symtab link");
    int found = 0;
    for (size_t p = 0; p + 24 <= sym->data.size(); p += 24) {
        uint32_t name = rd_u32(sym->data.data() + p);
        require(name < str->data.size(), "symbol name");
        if (strcmp(reinterpret_cast<const char*>(str->data.data() + name), "_amdgpu_cs_main") != 0)
            continue;
        memcpy(sym->data.data() + p + 16, &text_size, 8);
        ++found;
    }
    require(found == 1, "compute entry symbol");
}

inline Bytes link_elf(const ElfView& e) {
    Bytes out(e.bytes.begin(), e.bytes.begin() + 64);
    std::vector<uint64_t> offs(e.secs.size()), sizes(e.secs.size());
    for (size_t i = 0; i < e.secs.size(); ++i) {
        uint64_t align = e.secs[i].align;
        if (align == 0 || align > 16) align = 1;
        while (out.size() % align) out.push_back(0);
        offs[i] = out.size();
        sizes[i] = e.secs[i].data.size();
        out.insert(out.end(), e.secs[i].data.begin(), e.secs[i].data.end());
    }
    while (out.size() % 8) out.push_back(0);
    uint64_t shoff = out.size();
    for (size_t i = 0; i < e.secs.size(); ++i) {
        uint8_t sh[64] = {};
        const auto& s = e.secs[i];
        memcpy(sh + 0, &s.name, 4);
        memcpy(sh + 4, &s.type, 4);
        memcpy(sh + 8, &s.flags, 8);
        memcpy(sh + 16, &s.addr, 8);
        memcpy(sh + 24, &offs[i], 8);
        memcpy(sh + 32, &sizes[i], 8);
        memcpy(sh + 40, &s.link, 4);
        memcpy(sh + 44, &s.info, 4);
        memcpy(sh + 48, &s.align, 8);
        memcpy(sh + 56, &s.entsize, 8);
        out.insert(out.end(), sh, sh + 64);
    }
    memcpy(out.data() + 40, &shoff, 8);
    return out;
}

struct Built {
    Bytes elf;
    std::string original_map;
};

inline Built build_loaded_elf(const uint8_t* elf, size_t n, const uint8_t* code, size_t code_n, uint32_t ud_base) {
    require(code_n == kAcoBytes, "g_pbptr size");
    require(memcmp(code, "\x01\x00\x8a\xbe", 4) == 0, "g_pbptr entry is not s_mov s10, s1");
    require(memcmp(code + 4, "\x7d\x00\x8b\xbe", 4) == 0, "g_pbptr hi patch 0 changed");
    require(memcmp(code + 36, "\x7d\x00\x81\xbe", 4) == 0, "g_pbptr hi patch 1 changed");
    require(ud_base <= 16, "user-dword base");
    ElfView e = parse_elf(elf, n);
    ElfView::Sec* text = find_sec(e, ".text");
    ElfView::Sec* note = find_sec(e, ".note");
    require(text && note, "ELF is missing .text or .note");
    text->data.assign(kPrefix, kPrefix + sizeof kPrefix);
    text->data.insert(text->data.end(), code, code + code_n);
    // The ACO store is buffer_store_b32 without a device scope. The 9070 XT
    // store that CopyBufferRegion observes sets that one bit (llvm-mc
    // scope:SCOPE_DEV). Every other body byte stays as compiled.
    constexpr size_t kStore = 0x184;
    uint8_t* store = text->data.data() + sizeof kPrefix + kStore;
    require(memcmp(store, "\x7c\x80\x06\xc4\x00\x38\x80\x40\x01\x00\x00\x00", 12) == 0,
            "g_pbptr store encoding changed");
    store[6] = 0x88;
    patch_symbol_size(e, text->data.size());
    Built b;
    retarget_note(note->data, ud_base, &b.original_map);
    b.elf = link_elf(e);
    return b;
}

inline std::string loaded_summary(const Bytes& elf) {
    ElfView e = parse_elf(elf.data(), elf.size());
    ElfView::Sec* note = find_sec(e, ".note");
    ElfView::Sec* text = find_sec(e, ".text");
    require(note && text, "rebuilt ELF sections");
    require(note->data.size() >= 12, "rebuilt note");
    uint32_t ns = rd_u32(note->data.data());
    uint32_t ds = rd_u32(note->data.data() + 4);
    size_t desc = 12 + ((size_t(ns) + 3) & ~size_t(3));
    size_t p = 0;
    Mp root = mp_decode(note->data.data() + desc, ds, p);
    Mp& cs = mp_need(mp_need(mp_need(root, "amdpal.pipelines").items.at(0), ".hardware_stages"), ".cs");
    Mp& cr = mp_need(mp_need(root, "amdpal.pipelines").items.at(0), ".compute_registers");
    std::ostringstream o;
    o << "text=" << text->data.size()
      << " sgpr=" << mp_need(cs, ".sgpr_count").u
      << " vgpr=" << mp_need(cs, ".vgpr_count").u
      << " user=" << mp_need(cs, ".user_sgprs").u
      << " limit=" << mp_need(mp_need(root, "amdpal.pipelines").items.at(0), ".user_data_limit").u
      << " tgid_x=" << (mp_need(cr, ".tgid_x_en").b ? 1 : 0)
      << " kept=[" << map_line(mp_need(cs, ".user_data_reg_map")) << "]"
      << " prefix=" << (text->data.size() >= sizeof kPrefix && memcmp(text->data.data(), kPrefix, sizeof kPrefix) == 0)
      << " body=" << (text->data.size() == sizeof kPrefix + kAcoBytes);
    return o.str();
}

}  // namespace pbptr
}  // namespace lab
