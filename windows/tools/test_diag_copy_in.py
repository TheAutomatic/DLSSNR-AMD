#!/usr/bin/env python3
"""Capture the production Vulkan copy-in commands without a GPU."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "windows/src/core/nr_renodx.cpp").read_text()
session = (ROOT / "windows/src/pe/nr_pe_session.cpp").read_text()


def function(text, signature):
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


run = function(session, "VkImage Session::run_vulkan(")
assert run.index("!frame.colour") < run.index("if (frame.diagnostic_copy_only)")
assert run.index("if (frame.diagnostic_copy_only)") < run.index("EngineFrame engine{}")
gate = function(run, "if (frame.diagnostic_copy_only)")
assert "renodx_stage->diagnostic_copy_in(cmd, in);" in gate
assert "catch (const std::exception& e)" in gate
assert gate.rstrip().endswith("return frame.colour;\n    }")

colour = function((ROOT / "windows/src/core/nr_runtime.hpp").read_text(), "struct ColourFrame {") + ";"
helpers = "\n".join(function(source, signature) for signature in (
    "static void memory_barrier(", "static void image_barrier(", "void copy_in("))
diagnostic = function(source, "void Stage::diagnostic_copy_in(")
program = r'''
#include <vulkan/vulkan.h>
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
std::string events;
std::vector<VkImageMemoryBarrier> barriers;
std::vector<VkMemoryBarrier> memory;
std::vector<VkPipelineStageFlags> stages;
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags src,
    VkPipelineStageFlags dst, VkDependencyFlags flags, uint32_t nm, const VkMemoryBarrier* m,
    uint32_t nb, const VkBufferMemoryBarrier*, uint32_t ni, const VkImageMemoryBarrier* im) {
    assert(flags == 0 && nb == 0);
    stages.insert(stages.end(), {src, dst});
    if (ni) { assert(ni == 1 && nm == 0); events += 'B'; barriers.push_back(*im); }
    else { assert(nm == 1); events += 'M'; memory.push_back(*m); }
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer, VkImage src, VkImageLayout sl,
    VkImage dst, VkImageLayout dl, uint32_t count, const VkImageCopy* r) {
    assert(src == VkImage(1) && dst == VkImage(2));
    assert(sl == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && dl == VK_IMAGE_LAYOUT_GENERAL);
    assert(count == 1 && r->extent.width == 1280 && r->extent.height == 720 && r->extent.depth == 1);
    assert(r->srcSubresource.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);
    assert(r->srcSubresource.mipLevel == 0 && r->srcSubresource.baseArrayLayer == 0 && r->srcSubresource.layerCount == 1);
    assert(r->dstSubresource.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && r->dstSubresource.layerCount == 1);
    events += 'C';
}
''' + colour + r'''
struct Stage {
    struct Impl {
        struct Image { VkImage handle{}; } original;
        struct Uses { void touch() {} } gpu_uses;
        unsigned recordings = 0;
        VkFormat frame_fmt = VK_FORMAT_R8G8B8A8_UNORM;
        std::vector<VkImage> fresh{VkImage(99)};
        bool fail = false;
        void collect() {}
        Image make_image(uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage) {
            if (fail) throw std::runtime_error("allocation");
            assert(w == 1280 && h == 720 && f == frame_fmt && usage == VK_IMAGE_USAGE_SAMPLED_BIT);
            fresh.push_back(VkImage(2)); return {VkImage(2)};
        }
        void mark(VkCommandBuffer, const char*) { events += 'P'; }
''' + helpers + r'''
    };
    Impl* impl_;
    void diagnostic_copy_in(VkCommandBuffer, const ColourFrame&);
};
''' + diagnostic + r'''
void reset() { events.clear(); barriers.clear(); memory.clear(); stages.clear(); }
int main() {
    ColourFrame colour{};
    colour.image = VkImage(1); colour.width = 1280; colour.height = 720;
    for (auto layout : {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR}) {
        Stage::Impl impl;
        Stage stage{&impl};
        colour.before = layout;
        reset(); stage.diagnostic_copy_in({}, colour);
        assert(events == "BBMCBM");
        assert(impl.fresh.size() == 1 && impl.fresh[0] == VkImage(99));
        assert(barriers[0].image == VkImage(2) && barriers[0].oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
        assert(barriers[0].newLayout == VK_IMAGE_LAYOUT_GENERAL);
        reset(); stage.diagnostic_copy_in({}, colour);
        assert(events == "BMCBM" && impl.recordings == 2);
        assert(barriers[0].image == colour.image && barriers[0].oldLayout == layout);
        assert(barriers[0].newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        assert(barriers[0].srcAccessMask == (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT));
        assert(barriers[0].dstAccessMask == VK_ACCESS_TRANSFER_READ_BIT);
        assert(barriers[1].oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && barriers[1].newLayout == layout);
        assert(barriers[1].srcAccessMask == 0);
        assert(barriers[1].dstAccessMask == (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT));
        assert((stages == std::vector<VkPipelineStageFlags>{VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT}));
        assert(memory[0].srcAccessMask == VK_ACCESS_SHADER_READ_BIT && memory[0].dstAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT);
        assert(memory[1].srcAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT);
        assert(memory[1].dstAccessMask == (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT));
        for (const auto& b : barriers) {
            assert(b.srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED && b.dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
            assert(b.subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && b.subresourceRange.baseMipLevel == 0);
            assert(b.subresourceRange.levelCount == 1 && b.subresourceRange.baseArrayLayer == 0 && b.subresourceRange.layerCount == 1);
        }
        reset(); impl.copy_in({}, colour);
        assert(events == "BMCPBM");
    }
    Stage::Impl failing; failing.fail = true;
    Stage stage{&failing};
    reset();
    try { stage.diagnostic_copy_in({}, colour); assert(false); } catch (const std::runtime_error&) {}
    assert(events.empty() && failing.original.handle == VK_NULL_HANDLE && failing.fresh.size() == 1);
    std::puts("PASS: copy-in first/repeated frame, exact barriers/copy, original layout restored, no profiling, allocation error; stage-4 early-return/empty-input guards");
}
'''
with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / "test.cpp"
    binary = Path(directory) / "test"
    cpp.write_text(program)
    subprocess.run(["g++", "-std=c++17", "-I" + str(ROOT / "toolchain/Vulkan-Headers/include"), str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
