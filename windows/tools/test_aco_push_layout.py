#!/usr/bin/env python3
"""Verify ACO push-constant layout reconciliation across FP8 and INT4 records."""

from pathlib import Path
import os
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
RUNTIME_CPP = ROOT / "windows" / "src" / "core" / "nr_runtime.cpp"
GRAPH_CPP = ROOT / "windows" / "src" / "core" / "nr_graph.cpp"
ACO_PUSH_HPP = ROOT / "windows" / "src" / "core" / "nr_aco_push.hpp"
NRVK_HPP = ROOT / "windows" / "src" / "core" / "nrvk.hpp"
D3D12_NET_CPP = ROOT / "windows" / "src" / "pe" / "nr_d3d12_passthrough_net.cpp"

SHADER_PAIRS = [
    ("gemm", ROOT / "windows/shaders/rdna4/gemm1x1.comp", ROOT / "linux/shaders/rdna4/gemm1x1.comp"),
    ("vitattn", ROOT / "windows/shaders/rdna4/vit_attn.comp", ROOT / "linux/shaders/rdna4/vit_attn.comp"),
]

ACO_DIRS = [
    ("fp8-rdna4", ROOT / "windows/data/aco/records", True),
    ("fp8-rdna3", ROOT / "windows/data/aco/rdna3/records", True),
    ("int4-rdna4", ROOT / "windows/data/aco/int4/records", False),
    ("int4-rdna3", ROOT / "windows/data/aco/rdna3/int4/records", False),
]


def py_aco_needs_compact_push(gap: int, host_push_bytes: int, record_push_bytes: int) -> bool:
    return (
        gap != 0
        and host_push_bytes >= gap + 8
        and record_push_bytes > gap
        and record_push_bytes + 8 <= host_push_bytes
    )


def extract_block(text: str, marker: str) -> str:
    idx = text.find(marker)
    assert idx != -1, f"missing marker {marker!r}"
    open_b = text.find("{", idx)
    assert open_b != -1, f"missing '{{' after {marker!r}"
    depth = 0
    for i in range(open_b, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                end = i + 1
                if end < len(text) and text[end] == ";":
                    end += 1
                return text[idx:end]
    raise AssertionError(f"unbalanced braces for {marker!r}")


def parse_push_block(path: Path) -> list[tuple[str, str]]:
    txt = path.read_text(encoding="utf-8")
    m = re.search(r"layout\s*\(\s*push_constant\s*\)\s*uniform\s+\w+\s*\{([^}]*)\}", txt, re.S)
    assert m, f"missing push_constant block in {path}"
    fields = []
    for raw in m.group(1).splitlines():
        line = re.sub(r"//.*", "", raw).strip()
        if not line or line.startswith("#"):
            continue
        if re.fullmatch(r"NR_[A-Z0-9_]+", line):
            fields.append(("macro", line))
            continue
        fm = re.match(r"([A-Za-z_]\w*)\s+(.+?)\s*;", line)
        assert fm, f"unparsed push_constant line in {path}: {raw!r}"
        typ = fm.group(1)
        for var in fm.group(2).split(","):
            fields.append((typ, re.sub(r"\s+", "", var)))
    return fields


def check_shaders() -> None:
    for name, win_path, lin_path in SHADER_PAIRS:
        wf = parse_push_block(win_path)
        lf = parse_push_block(lin_path)
        w_names = [f[1] for f in wf]
        l_names = [f[1] for f in lf]
        assert ("uint", "act_lo") in wf and ("uint", "act_hi") in wf, (
            f"{name} Windows shader must declare act_lo/act_hi: {wf}"
        )
        assert "act_lo" not in l_names and "act_hi" not in l_names, (
            f"{name} Linux shader must not declare act_lo/act_hi: {lf}"
        )
        w_no_act = [f for f in wf if f[1] not in ("act_lo", "act_hi")]
        assert w_no_act == lf, f"{name} shader push mismatch beyond act_lo/act_hi: {w_no_act} vs {lf}"
        assert w_names.index("act_hi") == w_names.index("act_lo") + 1, (
            f"{name} act_lo/act_hi not adjacent: {w_names}"
        )


def check_source_invariants(runtime_txt: str, d3d12_net_txt: str) -> None:
    assert "offsetof(PushVAttn, act_lo)" in runtime_txt, "nr_runtime.cpp missing PushVAttn act_at"
    assert "offsetof(PushGemm, act_lo)" in runtime_txt, "nr_runtime.cpp missing PushGemm act_at"
    plan_start = runtime_txt.find("plan_d3d12_network(")
    assert plan_start != -1, "missing plan_d3d12_network in nr_runtime.cpp"
    plan_slice = runtime_txt[plan_start : plan_start + 6000]
    assert "memmove" not in plan_slice, (
        "plan_d3d12_network must not unconditionally memmove push constants"
    )
    assert "aco_needs_compact_push" in d3d12_net_txt and "aco_compact_push" in d3d12_net_txt, (
        "nr_d3d12_passthrough_net.cpp must use nr::aco_needs_compact_push / nr::aco_compact_push"
    )


def parse_nrp_header(path: Path) -> tuple[int, list[tuple[int, int]]]:
    data = path.read_bytes()
    assert len(data) >= 96, f"{path}: truncated NRPAL header ({len(data)} bytes)"
    push_bytes = struct.unpack_from("<I", data, 48)[0]
    ninline = struct.unpack_from("<I", data, 68)[0]
    assert len(data) >= 96 + 8 * ninline, f"{path}: truncated inline table (ninline={ninline})"
    inlines = [struct.unpack_from("<II", data, 96 + 8 * i) for i in range(ninline)]
    return push_bytes, inlines


def load_dir_records(dir_path: Path) -> dict[str, tuple[int, list[int]]]:
    nrps = sorted(dir_path.glob("*.nrp"))
    assert nrps, f"no .nrp records found in {dir_path}"
    out: dict[str, tuple[int, list[int]]] = {}
    for p in nrps:
        pb, inlines = parse_nrp_header(p)
        out[p.name] = (pb, [dw for _, dw in inlines])
    return out


def check_nrp_records() -> None:
    seen = {
        "fp8_pb96": 0,
        "fp8_pb32_dw7": 0,
        "int4_pb88": 0,
        "int4_pb24_dw5": 0,
        "diff_pairs": 0,
    }
    by_label = {}
    for label, dir_path, is_fp8 in ACO_DIRS:
        recs = load_dir_records(dir_path)
        by_label[label] = recs
        for name, (pb, dws) in recs.items():
            max_dw = max(dws, default=-1)
            if is_fp8:
                assert pb != 88, f"{label}/{name}: unexpected push_bytes=88 in FP8 folder"
                if pb == 96:
                    assert max_dw == 23 and 22 in dws and 23 in dws, (
                        f"{label}/{name}: FP8 TCHAIN GEMM expected dw 22,23, got {sorted(set(dws))}"
                    )
                    assert not py_aco_needs_compact_push(80, 96, pb)
                    seen["fp8_pb96"] += 1
                elif pb == 32 and max_dw == 7:
                    assert not py_aco_needs_compact_push(20, 32, pb)
                    seen["fp8_pb32_dw7"] += 1
                if pb <= 72:
                    assert not py_aco_needs_compact_push(80, 96, pb)
                if pb <= 20:
                    assert not py_aco_needs_compact_push(20, 32, pb)
            else:
                assert pb != 96, f"{label}/{name}: unexpected push_bytes=96 in INT4 folder"
                if pb == 88:
                    assert max_dw <= 21, (
                        f"{label}/{name}: INT4 pb=88 expected max_dw<=21, got {sorted(set(dws))}"
                    )
                    if max_dw >= 20:
                        assert max_dw == 21 and 20 in dws and 21 in dws, (
                            f"{label}/{name}: INT4 TCHAIN GEMM expected dw 20,21, got {sorted(set(dws))}"
                        )
                        assert py_aco_needs_compact_push(80, 96, pb)
                        seen["int4_pb88"] += 1
                elif pb == 24 and max_dw == 5:
                    assert py_aco_needs_compact_push(20, 32, pb)
                    seen["int4_pb24_dw5"] += 1
                if pb <= 72:
                    assert not py_aco_needs_compact_push(80, 96, pb)

    for arch in ("rdna4", "rdna3"):
        fp8_recs = by_label[f"fp8-{arch}"]
        int4_recs = by_label[f"int4-{arch}"]
        for name in sorted(set(fp8_recs) & set(int4_recs)):
            pb_f, dws_f = fp8_recs[name]
            pb_i, dws_i = int4_recs[name]
            if pb_f == pb_i:
                continue
            assert pb_f - pb_i == 8, f"{arch}/{name}: expected 8-byte BDA delta, got fp8={pb_f} int4={pb_i}"
            seen["diff_pairs"] += 1
            if pb_f == 96:
                assert pb_i == 88
                assert not py_aco_needs_compact_push(80, 96, pb_f)
                assert py_aco_needs_compact_push(80, 96, pb_i)
            elif pb_f == 32:
                assert pb_i == 24
                assert max(dws_f, default=-1) == 7 and max(dws_i, default=-1) == 5
                assert not py_aco_needs_compact_push(20, 32, pb_f)
                assert py_aco_needs_compact_push(20, 32, pb_i)
            else:
                assert pb_f <= 72 and pb_i <= 64
                assert not py_aco_needs_compact_push(80, 96, pb_f)
                assert not py_aco_needs_compact_push(80, 96, pb_i)

    for key, count in seen.items():
        assert count > 0, f"missing expected NRP category {key}: {seen}"
    print(f"NRP audit passed across {len(ACO_DIRS)} directories: {seen}")


HARNESS = r"""
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

static uint8_t g_last_push[128] = {};
static uint32_t g_last_push_size = 0;

extern "C" {
VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(
    VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags,
    uint32_t offset, uint32_t size, const void* pValues) {
    assert(offset == 0 && size <= sizeof(g_last_push));
    std::memset(g_last_push, 0xCC, sizeof(g_last_push));
    std::memcpy(g_last_push, pValues, size);
    g_last_push_size = size;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice, VkImage, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice, VkImageView, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL vkDestroySampler(VkDevice, VkSampler, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL vkDestroyFence(VkDevice, VkFence, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL vkDestroyQueryPool(VkDevice, VkQueryPool, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(VkDevice, VkCommandPool, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice, VkDescriptorSetLayout, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(VkDevice, VkDescriptorPool, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(VkDevice, VkPipelineLayout, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice, VkPipeline, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice, VkShaderModule, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*) { return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(VkDevice, VkCommandPool, uint32_t, const VkCommandBuffer*) {}
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo*) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue, uint32_t, const VkSubmitInfo*, VkFence) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice) { return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(VkCommandBuffer, uint32_t, uint32_t, uint32_t) {}
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t, const VkImageMemoryBarrier*) {}
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {}
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*) {}
}

#include "nr_aco_push.hpp"
#include "nrvk.hpp"

namespace nr {
__PUSH_GEMM_STRUCT__
__PUSH_VATTN_STRUCT__
__GEMM_PUSH_KERN__
struct PushGemmTChain { PushGemm base{}; uint32_t tc_rec = 0; };
struct PushVAttnTChain { PushVAttn base{}; uint32_t tc_rec = 0; };
}  // namespace nr

static uint32_t rd32(const uint8_t* p, size_t off) {
    uint32_t v = 0;
    std::memcpy(&v, p + off, 4);
    return v;
}

static void plan_sim(const char* kern, std::vector<uint8_t>& push, int& act_at) {
    act_at = -1;
    if (std::string(kern) == "vitattn" && push.size() >= sizeof(nr::PushVAttn)) {
        act_at = int(offsetof(nr::PushVAttn, act_lo));
    } else if (nr::gemm_push_kern(kern) && push.size() >= sizeof(nr::PushGemm)) {
        act_at = int(offsetof(nr::PushGemm, act_lo));
    }
}

static void d3d12_build_sim(uint32_t nrp_push_bytes, std::vector<uint8_t>& push, int& act_at) {
    if (act_at >= 0 && nr::aco_needs_compact_push(uint32_t(act_at), uint32_t(push.size()), nrp_push_bytes)) {
        nr::aco_compact_push(push.data(), push.size(), uint32_t(act_at));
        act_at = -1;
    }
}

int main() {
    using nr::PushGemm;
    using nr::PushGemmTChain;
    using nr::PushVAttn;
    using nr::PushVAttnTChain;

    static_assert(sizeof(PushGemm) == 92, "PushGemm size mismatch");
    static_assert(offsetof(PushGemm, act_lo) == 80, "PushGemm act_lo offset mismatch");
    static_assert(offsetof(PushGemm, remap) == 88, "PushGemm remap offset mismatch");
    static_assert(sizeof(PushGemmTChain) == 96, "PushGemmTChain size mismatch");
    static_assert(offsetof(PushGemmTChain, tc_rec) == 92, "PushGemmTChain tc_rec offset mismatch");

    static_assert(sizeof(PushVAttn) == 28, "PushVAttn size mismatch");
    static_assert(offsetof(PushVAttn, act_lo) == 20, "PushVAttn act_lo offset mismatch");
    static_assert(sizeof(PushVAttnTChain) == 32, "PushVAttnTChain size mismatch");
    static_assert(offsetof(PushVAttnTChain, tc_rec) == 28, "PushVAttnTChain tc_rec offset mismatch");

    // 1. Adaptive heuristic edge cases.
    assert(!nr::aco_needs_compact_push(80, 96, 96));   // FP8 TCHAIN GEMM
    assert( nr::aco_needs_compact_push(80, 96, 88));   // INT4 TCHAIN GEMM
    assert(!nr::aco_needs_compact_push(80, 92, 92));   // FP8 non-TCHAIN GEMM
    assert( nr::aco_needs_compact_push(80, 92, 84));   // INT4 non-TCHAIN GEMM with remap
    assert(!nr::aco_needs_compact_push(80, 96, 72));   // FP8 chain GEMM without remap/tchain
    assert(!nr::aco_needs_compact_push(80, 96, 64));   // INT4 chain GEMM without remap/tchain
    assert(!nr::aco_needs_compact_push(20, 32, 32));   // FP8 vitattn
    assert( nr::aco_needs_compact_push(20, 32, 24));   // INT4 vitattn
    assert(!nr::aco_needs_compact_push(0,  96, 88));   // zero gap
    assert(!nr::aco_needs_compact_push(80, 88, 80));   // host smaller than gap+8

    // 2. Vulkan nrvk::Kernel::push_constants for FP8 (gap=0) and INT4 (gap=80/20).
    PushGemmTChain pg{};
    pg.base.M = 128; pg.base.K = 64; pg.base.N = 64;
    pg.base.act_lo = 0x11223344u; pg.base.act_hi = 0x55667788u;
    pg.base.remap = 0xAABBCCDDu; pg.tc_rec = 0xDEADBEEFu;

    nrvk::Kernel k_fp8{};
    k_fp8.push_range = sizeof(PushGemmTChain);
    k_fp8.aco_push_gap = 0;
    k_fp8.push_constants(nullptr, sizeof(PushGemmTChain), &pg);
    assert(g_last_push_size == 96);
    assert(rd32(g_last_push, offsetof(PushGemm, M)) == 128);
    assert(rd32(g_last_push, 80) == 0x11223344u);
    assert(rd32(g_last_push, 84) == 0x55667788u);
    assert(rd32(g_last_push, 88) == 0xAABBCCDDu);
    assert(rd32(g_last_push, 92) == 0xDEADBEEFu);

    nrvk::Kernel k_int4{};
    k_int4.push_range = sizeof(PushGemmTChain);
    k_int4.aco_push_gap = uint32_t(offsetof(PushGemm, act_lo));
    k_int4.push_constants(nullptr, sizeof(PushGemmTChain), &pg);
    assert(g_last_push_size == 96);
    assert(rd32(g_last_push, offsetof(PushGemm, M)) == 128);
    assert(rd32(g_last_push, 80) == 0xAABBCCDDu);
    assert(rd32(g_last_push, 84) == 0xDEADBEEFu);
    assert(rd32(g_last_push, 88) == 0u);
    assert(rd32(g_last_push, 92) == 0u);

    PushVAttnTChain pv{};
    pv.base.tokens = 1024;
    pv.base.act_lo = 0x12345678u; pv.base.act_hi = 0x9ABCDEF0u;
    pv.tc_rec = 0xCAFEBABEu;

    nrvk::Kernel v_fp8{};
    v_fp8.push_range = sizeof(PushVAttnTChain);
    v_fp8.aco_push_gap = 0;
    v_fp8.push_constants(nullptr, sizeof(PushVAttnTChain), &pv);
    assert(g_last_push_size == 32);
    assert(rd32(g_last_push, offsetof(PushVAttn, tokens)) == 1024);
    assert(rd32(g_last_push, 20) == 0x12345678u);
    assert(rd32(g_last_push, 24) == 0x9ABCDEF0u);
    assert(rd32(g_last_push, 28) == 0xCAFEBABEu);

    nrvk::Kernel v_int4{};
    v_int4.push_range = sizeof(PushVAttnTChain);
    v_int4.aco_push_gap = uint32_t(offsetof(PushVAttn, act_lo));
    v_int4.push_constants(nullptr, sizeof(PushVAttnTChain), &pv);
    assert(g_last_push_size == 32);
    assert(rd32(g_last_push, offsetof(PushVAttn, tokens)) == 1024);
    assert(rd32(g_last_push, 20) == 0xCAFEBABEu);
    assert(rd32(g_last_push, 24) == 0u);
    assert(rd32(g_last_push, 28) == 0u);

    // 3. D3D12 planner + build-time adaptive reconciliation.
    assert(nr::gemm_push_kern("gemmtc"));
    assert(nr::gemm_push_kern("gemm2k"));
    assert(nr::gemm_push_kern("gemm1x1"));
    assert(!nr::gemm_push_kern("ffwd3"));

    for (bool int4 : {false, true}) {
        std::vector<uint8_t> gemm_push(sizeof(PushGemmTChain));
        std::memcpy(gemm_push.data(), &pg, sizeof(PushGemmTChain));
        int gemm_act = -1;
        plan_sim("gemm1x1", gemm_push, gemm_act);
        assert(gemm_act == 80);
        d3d12_build_sim(int4 ? 88u : 96u, gemm_push, gemm_act);
        if (!int4) {
            assert(gemm_act == 80);
            assert(rd32(gemm_push.data(), 88) == 0xAABBCCDDu);
            assert(rd32(gemm_push.data(), 92) == 0xDEADBEEFu);
        } else {
            assert(gemm_act == -1);
            assert(rd32(gemm_push.data(), 80) == 0xAABBCCDDu);
            assert(rd32(gemm_push.data(), 84) == 0xDEADBEEFu);
            assert(rd32(gemm_push.data(), 88) == 0u);
            assert(rd32(gemm_push.data(), 92) == 0u);
        }

        std::vector<uint8_t> gemm_92(sizeof(PushGemm));
        std::memcpy(gemm_92.data(), &pg.base, sizeof(PushGemm));
        int gemm92_act = -1;
        plan_sim("gemmproj", gemm_92, gemm92_act);
        assert(gemm92_act == 80);
        d3d12_build_sim(int4 ? 84u : 92u, gemm_92, gemm92_act);
        if (!int4) {
            assert(gemm92_act == 80);
            assert(rd32(gemm_92.data(), 88) == 0xAABBCCDDu);
        } else {
            assert(gemm92_act == -1);
            assert(rd32(gemm_92.data(), 80) == 0xAABBCCDDu);
            assert(rd32(gemm_92.data(), 84) == 0u);
            assert(rd32(gemm_92.data(), 88) == 0u);
        }

        std::vector<uint8_t> vattn_push(sizeof(PushVAttnTChain));
        std::memcpy(vattn_push.data(), &pv, sizeof(PushVAttnTChain));
        int vattn_act = -1;
        plan_sim("vitattn", vattn_push, vattn_act);
        assert(vattn_act == 20);
        d3d12_build_sim(int4 ? 24u : 32u, vattn_push, vattn_act);
        if (!int4) {
            assert(vattn_act == 20);
            assert(rd32(vattn_push.data(), 28) == 0xCAFEBABEu);
        } else {
            assert(vattn_act == -1);
            assert(rd32(vattn_push.data(), 20) == 0xCAFEBABEu);
            assert(rd32(vattn_push.data(), 24) == 0u);
            assert(rd32(vattn_push.data(), 28) == 0u);
        }
    }

    return 0;
}
"""


def run_cpp_harness(graph_txt: str) -> None:
    push_gemm = extract_block(graph_txt, "struct PushGemm")
    push_vattn = extract_block(graph_txt, "struct PushVAttn")
    m = re.search(r"(?:static\s+inline\s+|static\s+|inline\s+)?bool\s+gemm_push_kern\s*\(", graph_txt)
    assert m, "missing gemm_push_kern in nr_graph.cpp"
    gemm_fn = extract_block(graph_txt[m.start() :], "bool gemm_push_kern")

    src = (
        HARNESS.replace("__PUSH_GEMM_STRUCT__", push_gemm)
        .replace("__PUSH_VATTN_STRUCT__", push_vattn)
        .replace("__GEMM_PUSH_KERN__", gemm_fn)
    )

    vk_inc = os.environ.get("NR_VULKAN_INCLUDE") or str(ROOT / "toolchain" / "Vulkan-Headers" / "include")
    with tempfile.TemporaryDirectory() as tmp:
        cpp = Path(tmp) / "test_aco_push_layout.cpp"
        exe = Path(tmp) / "test_aco_push_layout"
        cpp.write_text(src, encoding="utf-8", newline="\n")
        cmd = [
            "g++",
            "-std=c++17",
            "-O2",
            f"-I{ROOT / 'windows' / 'src' / 'core'}",
        ]
        if Path(vk_inc).is_dir():
            cmd.append(f"-I{vk_inc}")
        cmd.extend([str(cpp), "-o", str(exe)])
        subprocess.run(cmd, check=True)
        subprocess.run([str(exe)], check=True)


def main() -> None:
    runtime_txt = RUNTIME_CPP.read_text(encoding="utf-8")
    graph_txt = GRAPH_CPP.read_text(encoding="utf-8")
    d3d12_net_txt = D3D12_NET_CPP.read_text(encoding="utf-8")
    assert ACO_PUSH_HPP.is_file(), f"missing {ACO_PUSH_HPP}"
    assert NRVK_HPP.is_file(), f"missing {NRVK_HPP}"

    check_shaders()
    check_source_invariants(runtime_txt, d3d12_net_txt)
    check_nrp_records()
    run_cpp_harness(graph_txt)
    print("ACO push layout tests PASSED")


if __name__ == "__main__":
    main()
