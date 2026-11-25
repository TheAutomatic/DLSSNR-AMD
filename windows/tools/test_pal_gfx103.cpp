// Verify gfx10.3 NRPAL002 record parsing, PAL ELF synthesis, and non-string MessagePack key round-tripping.
#include "nr_pal_binary.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

void test_msgpack_nonstring_key_roundtrip() {
    // Map of 2 entries: { uint16(0x2e08): uint(42), "ok": uint(7) }
    const nr::pal::Bytes raw = {
        0x82,
        0xcd, 0x2e, 0x08, 0x2a,
        0xa2, 'o', 'k', 0x07,
    };
    const nr::pal::Value v = nr::pal::unpack(raw);
    assert(v.kind == nr::pal::Value::Map);
    assert(v.fields.size() == 2);
    assert(!v.fields[0].first.empty() && v.fields[0].first[0] == '\0');
    assert(v.fields[1].first == "ok" && v.fields[1].second.number() == 7);

    nr::pal::Bytes encoded;
    nr::pal::encode(encoded, v);
    const nr::pal::Value v2 = nr::pal::unpack(encoded);
    assert(v2.fields.size() == 2);
    assert(v2.fields[0].first == v.fields[0].first);
    assert(v2.fields[0].second.raw == v.fields[0].second.raw);
    assert(v2.fields[1].first == "ok" && v2.fields[1].second.number() == 7);
}

void test_synthesize_record(const std::filesystem::path& nrp_path) {
    const nr::pal::Bytes rec_bytes = nr::pal::load(nrp_path.string());
    const nr::pal::Record r = nr::pal::Record::parse(rec_bytes);
    assert(nr::pal::is_gfx103(r.mach));
    assert(!r.scratch_en && r.scratch_bytes == 0);

    const unsigned push_bytes = r.push_bytes;
    const std::array<uint32_t, 3> wg{r.wg, 1u, 1u};
    for (uint32_t mach : {0x36u, 0x37u, 0x53u}) {
        for (unsigned push_base : {1u, 2u}) {
            const nr::pal::Bytes elf_bytes = nr::pal::synthesize(r, mach, push_bytes, wg, push_base);
            const nr::pal::Elf elf(elf_bytes);
            assert(elf.mach == mach);

            const size_t text_idx = elf.find(".text");
            assert(elf.data[text_idx].size() >= 256u + r.code.size());
            assert((elf.data[text_idx].size() - r.code.size()) % 256u == 0u);
            assert(std::equal(r.code.begin(), r.code.end(),
                              elf.data[text_idx].end() - std::ptrdiff_t(r.code.size())));

            const nr::pal::Value meta = nr::pal::pal_metadata(elf.data[elf.find(".note")]);
            const auto& pipe = meta.at("amdpal.pipelines").items.at(0);
            assert(pipe.at(".user_data_limit").number() == push_base + push_bytes / 4 + 1);
            const auto& cs = pipe.at(".hardware_stages").at(".cs");
            assert(cs.at(".wavefront_size").number() == 32);
            assert(cs.at(".scratch_en").raw == nr::pal::Bytes{0xc2});
            assert(cs.at(".scratch_memory_size").number() == 0);
            const auto& dims = cs.at(".threadgroup_dimensions");
            assert(dims.items.size() == 3 && dims.items[0].number() == wg[0] &&
                   dims.items[1].number() == 1 && dims.items[2].number() == 1);

            const nr::pal::XglLayout layout = nr::pal::xgl_layout(meta, push_bytes);
            assert(layout.push_base == push_base);
            assert(layout.descriptor_slot == push_base + push_bytes / 4);

            const nr::pal::Arguments args(r, layout.descriptor_slot, push_base);
            std::array<int, 102> regs{};
            for (int i = 0; i < 102; ++i) regs[size_t(i)] = 1000 + i;
            for (auto [dst, src] : args.moves) {
                assert(dst >= 0 && dst < 102 && src >= 0 && src < 102);
                regs[size_t(dst)] = regs[size_t(src)];
            }
            assert(regs[size_t(r.set0)] == 1001);
            unsigned next_pal_inline = 2;
            const bool spill_needed = r.push >= 0 || 2 + r.inlines.size() + (r.grid >= 0 ? 2 : 0) + (r.push >= 0 ? 1 : 0) > 16;
            const unsigned limit = 16 - (r.grid >= 0 ? 2u : 0u) - (spill_needed ? 1u : 0u);
            for (auto [dst, dw] : r.inlines) {
                if (next_pal_inline < limit) assert(regs[size_t(dst)] == 1000 + int(next_pal_inline++));
                (void)dw;
            }
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    test_msgpack_nonstring_key_roundtrip();

    const std::filesystem::path rec_dir = argc > 1 ? std::filesystem::path(argv[1])
                                                   : std::filesystem::path("windows/data/aco/rdna2/records");
    std::vector<std::filesystem::path> nrps;
    for (const auto& entry : std::filesystem::directory_iterator(rec_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".nrp")
            nrps.push_back(entry.path());
    }
    assert(!nrps.empty());
    for (const auto& p : nrps) test_synthesize_record(p);

    std::printf("PASS: tested %zu gfx10.3 NRPAL002 records (mach 0x36/0x37/0x53, push_base 1/2, msgpack keys)\n",
                nrps.size());
    return 0;
}
