#!/usr/bin/env python3
"""CPU regression: python3 windows/tools/test_aco_push_layout.py (needs C++17 and libvulkan)."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def fields(body):
    body = re.sub(r"//[^\n]*", "", body)
    body = re.sub(r"^\s*#.*$", "", body, flags=re.M)
    body = body.replace("NR_CHAIN_FIELDS", "").replace("NR_TCHAIN_FIELDS", "uint tc_rec;")
    return [name.strip() for declaration in re.findall(r"\b(?:uint32_t|uint)\s+([^;]+);", body)
            for name in declaration.split(",")]


graph = (ROOT / "windows/src/core/nr_graph.cpp").read_text()
structs = []
for host, shader in (("PushGemm", "gemm1x1.comp"), ("PushVAttn", "vit_attn.comp")):
    definition = re.search(r"struct " + host + r"\s*\{.*?\};", graph, re.S).group()
    structs.append(definition)
    host_fields = fields(definition)
    for tree in ("windows", "linux"):
        source = (ROOT / tree / "shaders/rdna4" / shader).read_text()
        block = re.search(r"layout\(push_constant\).*?\{(.*?)\} pc;", source, re.S).group(1)
        shader_fields = fields(block)
        expected = host_fields if tree == "windows" else [f for f in host_fields if f not in ("act_lo", "act_hi")]
        assert shader_fields == expected + ["tc_rec"], (tree, shader, shader_fields, expected)

# Exercise the D3D12 planner's actual serialization without constructing a model or device.
runtime = (ROOT / "windows/src/core/nr_runtime.cpp").read_text()
serialization = re.search(r"const size_t gap = .*?\n\s*else if \(gemm_push_kern.*?;", runtime, re.S).group()
gemm_predicate = re.search(r"static bool gemm_push_kern[^\n]+", graph).group()

HARNESS = r'''
#include "nrvk.hpp"
#include <cstddef>
#include <iostream>

@STRUCTS@
@GEMM_PREDICATE@
struct Disp { std::string kern; std::vector<uint8_t> push; };
struct PlanPush { std::vector<uint8_t> push; int act_at = -1; };
PlanPush d3d12_push(const Disp& d, bool int4) {
    PlanPush x; x.push = d.push;
    @SERIALIZATION@
    return x;
}

std::vector<uint8_t> recorded;
unsigned calls = 0, failures = 0, cases = 0;
std::set<std::string> reported;
VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer, VkPipelineLayout,
        VkShaderStageFlags, uint32_t offset, uint32_t size, const void* data) {
    if (offset) throw std::runtime_error("unexpected offset");
    const auto* p = static_cast<const uint8_t*>(data);
    recorded.assign(p, p + size); ++calls;
}
void check(bool ok, const std::string& message) {
    ++cases;
    if (!ok) {
        ++failures;
        auto key = message.substr(0, message.find(' ')) + (message.find("no-BDA") != std::string::npos ? " no-BDA" : " BDA");
        if (reported.insert(key).second) std::cout << "FAIL: " << message << '\n';
    }
}
int main() {
    unsetenv("NR_ACO_DIR"); unsetenv("NR_I4_WEIGHTS");
    for (bool int4 : {false, true}) {
        std::map<std::string, std::string> cfg;
        if (int4) cfg["NR_I4_WEIGHTS"] = "test-weights";
        nr::BuildCfgScope scope(&cfg);
        for (unsigned mode : {0u, 1u, 2u}) {
            nr::binary::configure(mode ? "test-aco" : "", {}, "", {});
            setenv("NR_ACO_NATIVE_KERNELS", mode == 2 ? "*" : "", 1);
            for (bool bda : {false, true}) for (bool vit : {false, true}) {
                const size_t base = vit ? sizeof(PushVAttn) : sizeof(PushGemm);
                const size_t gap = vit ? offsetof(PushVAttn, act_lo) : offsetof(PushGemm, act_lo);
                const std::string name = vit ? "vitattn" : "gemmprojc";
                const std::string label = name + (int4 ? " INT4" : " FP8") + (bda ? " BDA" : " no-BDA") + " mode=" + std::to_string(mode);
                for (size_t tail : {size_t(0), size_t(4), size_t(20)}) {
                    Disp d{name, std::vector<uint8_t>(base + tail)};
                    for (size_t i = 0; i < d.push.size(); i += 4) {
                        uint32_t word = 0x100u + uint32_t(i / 4);
                        std::memcpy(d.push.data() + i, &word, 4);
                    }
                    uint64_t address = bda ? 0x1122334455667788ull : 0;
                    std::memcpy(d.push.data() + gap, &address, 8);
                    auto compact = d.push;
                    compact.erase(compact.begin() + gap, compact.begin() + gap + 8);
                    compact.resize(d.push.size(), 0);
                    nrvk::Context ctx;
                    nrvk::Kernel k;
                    try {
                        k.create(ctx, "__aco_push_layout_missing__/g_" + name + ".spv", {}, uint32_t(d.push.size()));
                        throw std::runtime_error("missing SPIR-V unexpectedly opened");
                    } catch (const std::runtime_error& e) {
                        if (std::string(e.what()).find("cannot open __aco_push_layout_missing__/") != 0) throw;
                    }
                    k.push_constants(VkCommandBuffer{}, uint32_t(d.push.size()), d.push.data());
                    check(recorded == (mode == 1 ? compact : d.push), "Vulkan " + label);
                    auto plan = d3d12_push(d, int4);
                    check(plan.push == compact && plan.act_at == -1, "D3D12 " + label);
                }
            }
        }
    }
    nrvk::Kernel k;
    const auto before = calls;
    k.push_constants(VkCommandBuffer{}, 0, nullptr);
    check(calls == before, "empty push records nothing");
    k.aco_push_gap = 80;
    for (uint32_t bytes : {84u, 132u}) {
        bool threw = false;
        try { k.push_constants(VkCommandBuffer{}, bytes, nullptr); }
        catch (const std::runtime_error&) { threw = true; }
        check(threw, "invalid compact push size rejected");
    }
    std::cout << (failures ? "FAIL: " : "PASS: ") << cases << " checks, " << failures
              << " failures (NR_INT4=" << NR_INT4 << "); FP8/INT4, BDA/no-BDA, ACO/native, Vulkan/D3D12, tails, empty/error\n";
    return failures ? 1 : 0;
}
'''

status = 0
with tempfile.TemporaryDirectory(prefix="nr-aco-push-") as tmp:
    source = Path(tmp) / "test.cpp"
    source.write_text(HARNESS.replace("@STRUCTS@", "\n".join(structs))
                      .replace("@GEMM_PREDICATE@", gemm_predicate).replace("@SERIALIZATION@", serialization))
    for int4 in (0, 1):
        binary = Path(tmp) / f"test-{int4}"
        subprocess.run(shlex.split(os.environ.get("CXX", "g++")) + ["-std=c++17", "-O1", f"-DNR_INT4={int4}",
                       "-I" + str(ROOT / "windows/src/core"), "-I" + str(ROOT / "toolchain/Vulkan-Headers/include"),
                       str(source), "-Wl,-l:libvulkan.so.1", "-o", str(binary)], check=True)
        status |= subprocess.run([str(binary)], cwd=ROOT).returncode
raise SystemExit(status)
