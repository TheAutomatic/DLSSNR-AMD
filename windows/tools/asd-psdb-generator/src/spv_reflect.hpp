// Minimal SPIR-V reflection for the ACO shells: compute LocalSize and the set-0
// binding layout (storage buffers, combined image samplers, storage images, bare
// samplers). Enough for the carrier/descriptor-table work; not a general reflector.
#pragma once
#include "pbptr_elf.hpp"  // Bytes, require

#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace lab {
namespace spv {

struct Binding {
    uint32_t index = 0;
    // "buffer" | "combined" | "image" | "sampler"
    std::string kind;
};

struct Reflection {
    uint32_t local[3] = {0, 0, 0};
    std::vector<Binding> bindings;  // ascending binding index, one entry per used slot
    bool has_local = false;
};

inline uint32_t ruword(const Bytes& b, size_t i) {
    uint32_t v = 0;
    memcpy(&v, b.data() + i * 4, 4);
    return v;
}

// Walks the module once. A variable's kind follows its pointer pointee:
// Block-decorated struct -> storage buffer; OpTypeSampledImage -> combined;
// OpTypeImage with Sampled=2 -> storage image; OpTypeSampler -> sampler.
inline Reflection reflect(const Bytes& code) {
    require(code.size() >= 20 && ruword(code, 0) == 0x07230203u, "not a SPIR-V module");
    const size_t n = code.size() / 4;
    Reflection r;
    std::map<uint32_t, uint32_t> pointee;    // pointer type id -> pointee type id (StorageBuffer/Uniform/UniformConstant ptrs)
    std::map<uint32_t, uint32_t> image_sampled;   // image type id -> Sampled operand
    std::set<uint32_t> sampled_image_ty, sampler_ty, block_structs;
    std::map<uint32_t, uint32_t> binding_of;      // variable id -> Binding literal
    std::vector<std::pair<uint32_t, uint32_t>> vars;  // (var id, pointer type id) module order
    size_t p = 5;
    while (p < n) {
        uint32_t w = ruword(code, p);
        uint32_t op = w & 0xffffu, wc = w >> 16;
        if (wc == 0 || p + wc > n) break;
        if (op == 16 && wc == 6 && ruword(code, p + 2) == 17) {  // OpExecutionMode LocalSize
            for (int i = 0; i < 3; ++i) r.local[i] = ruword(code, p + 3 + i);
            r.has_local = true;
        } else if (op == 71 && wc >= 4 && ruword(code, p + 2) == 33) {  // OpDecorate Binding
            binding_of[ruword(code, p + 1)] = ruword(code, p + 3);
        } else if (op == 71 && wc >= 3 && ruword(code, p + 2) == 2) {  // OpDecorate Block
            block_structs.insert(ruword(code, p + 1));
        } else if (op == 25 && wc >= 9) {  // OpTypeImage: id, sampled ty, dim, depth, arrayed, ms, sampled, fmt
            image_sampled[ruword(code, p + 1)] = ruword(code, p + 7);
        } else if (op == 26) {  // OpTypeSampler
            sampler_ty.insert(ruword(code, p + 1));
        } else if (op == 27) {  // OpTypeSampledImage
            sampled_image_ty.insert(ruword(code, p + 1));
        } else if (op == 32 && wc == 4) {  // OpTypePointer: id, storage, pointee
            pointee[ruword(code, p + 1)] = ruword(code, p + 3);
        } else if (op == 59 && wc >= 4) {  // OpVariable: type, id, storage, [init]
            uint32_t storage = ruword(code, p + 3);
            if (storage == 12 || storage == 2 || storage == 0)  // StorageBuffer, Uniform, UniformConstant
                vars.emplace_back(ruword(code, p + 2), ruword(code, p + 1));
        }
        p += wc;
    }
    std::map<uint32_t, Binding> by_index;
    for (auto [var, pty] : vars) {
        auto it = pointee.find(pty);
        if (it == pointee.end()) continue;
        uint32_t pointee_ty = it->second;
        std::string kind;
        if (sampled_image_ty.count(pointee_ty)) kind = "combined";
        else if (sampler_ty.count(pointee_ty)) kind = "sampler";
        else if (image_sampled.count(pointee_ty)) kind = "image";  // Sampled=2 (storage); shells have no Sampled=1 bare images
        else if (block_structs.count(pointee_ty)) kind = "buffer";
        else continue;  // non-block struct in Uniform (push mirrors etc. never land here)
        auto b = binding_of.find(var);
        if (b == binding_of.end()) continue;
        Binding& slot = by_index[b->second];
        // Duplicated declarations at one binding (source templating) collapse; the
        // driver layout has one descriptor per binding number.
        if (slot.kind.empty() || (slot.kind == "buffer" && kind != "buffer"))
            slot = {b->second, kind};
        else if (kind != "buffer" && slot.kind == "buffer")
            slot = {b->second, kind};  // an image wins over a same-number buffer declaration
    }
    for (auto& [idx, b] : by_index) (void)idx, r.bindings.push_back(b);
    return r;
}

}  // namespace spv
}  // namespace lab
