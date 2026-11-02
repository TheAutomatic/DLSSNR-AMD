// Offline parity host: the reference (0.0.8.1, Vulkan) route on its own device, so a frame's output can be compared
// with the D3D12 ASD route's (nr_d3d12_asd_test.cpp, NR_TEST_DUMP) for the same input. Same model tree, same frame,
// same settings; the network is the product's Runtime (nr_runtime.hpp), not a re-implementation.
//
//   nr_parity_host <dlssnr-amd root> <width> <height> [dump-dir]
//
// Builds the network through Runtime (weights uploaded on this device's queue) and reports the dispatch count; a
// dump directory also runs one frame through the reference RenoDX stage and writes its output as R10G10B10A2, the
// format the D3D12 side dumps.
#include "nr_renodx.hpp"
#include "nr_runtime.hpp"
#include "nr_pe_config.hpp"
#include "nrvk.hpp"
#include "nr_pipeline_binary.hpp"

#include <d3d12.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" void nr_vk_load(void);   // vulkan_lazy.sh: the vk* entry points from vulkan-1.dll

namespace {
nrvk::Context& vkctx() {
    static nrvk::Context c;
    return c;
}

// The production device (nrvk::Context::create: every feature and extension the network is measured with), on the
// adapter the D3D12 device renders with, found by LUID and handed over as the index NR_GPU names.
bool make_device(std::string* why) {
    nr_vk_load();
    // The D3D12 device's adapter LUID: the same GPU the ASD route runs on.
    ID3D12Device* d3d = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d))) || !d3d) {
        *why = "no D3D12 device for the LUID";
        return false;
    }
    const LUID luid = d3d->GetAdapterLuid();   // D3D12's own: no DXGI interop (an Agility core may not expose it)
    d3d->Release();

    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &ai;
    VkInstance probe{};
    if (vkCreateInstance(&ii, nullptr, &probe) != VK_SUCCESS) { *why = "no Vulkan instance"; return false; }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(probe, &n, nullptr);
    std::vector<VkPhysicalDevice> all(n);
    vkEnumeratePhysicalDevices(probe, &n, all.data());
    int index = -1;
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
        vkGetPhysicalDeviceProperties2(all[i], &p2);
        std::printf("vulkan device %u: %s (LUID %s)\n", i, p2.properties.deviceName,
                    id.deviceLUIDValid && std::memcmp(id.deviceLUID, &luid, sizeof luid) == 0 ? "match" : "");
        if (index < 0 && id.deviceLUIDValid && std::memcmp(id.deviceLUID, &luid, sizeof luid) == 0) index = int(i);
    }
    vkDestroyInstance(probe, nullptr);
    if (index < 0) { *why = "no Vulkan device with the D3D12 device's LUID"; return false; }
    char idx[16];
    std::snprintf(idx, sizeof idx, "%d", index);
    SetEnvironmentVariableA("NR_GPU", idx);
    _putenv_s("NR_GPU", idx);
    try {
        vkctx().create();
    } catch (const std::exception& x) {
        *why = x.what();
        return false;
    }
    if (vkctx().gpu_name != "AMD Radeon RX 9070 XT") {
        *why = std::string("the LUID's adapter is ") + vkctx().gpu_name;
        return false;
    }
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("usage: %s <dlssnr-amd root> <width> <height> [dump-dir]\n", argv[0]);
        return 2;
    }
    const std::string root = argv[1];
    const uint32_t W = uint32_t(std::atoi(argv[2])), H = uint32_t(std::atoi(argv[3]));
    std::string why;
    nr::pe::configure_network(root);   // before the device: ACO Mode is enabled per device as it is created
    if (!make_device(&why)) { std::printf("device: %s\n", why.c_str()); return 3; }
    nr::HostDevice host{};
    // The production device's own handles and its reported support (not an assumption about the field).
    host.instance = vkctx().instance;
    host.physical = vkctx().physical;
    host.device = vkctx().device;
    host.queue = vkctx().queue;
    host.queue_family = vkctx().family;
    host.buffer_device_address = vkctx().buffer_device_address;
    host.rdna3 = vkctx().rdna3;
    // The device-level answer, not the configuration's: ACO's records are used by a device marked enabled.
    std::printf("ACO records active on this device: %d\n", nr::binary::enabled(host.device) ? 1 : 0);
    nr::RuntimeConfig cfg;
    cfg.root = root;
    cfg.width = W;
    cfg.height = H;
    // The network runs on the stage's proxy, which is RGBA16F (nr_renodx.cpp's m.proxy): the runtime's colour format
    // is the proxy's, not the frame's.
    cfg.colour_format = VK_FORMAT_R16G16B16A16_SFLOAT;
    cfg.network = 1;   // the small model
    try {
        // The RenoDX stage hands the network a temporal frame (history, guides): the runtime must carry that path.
        {
            const std::string why = nr::binary::disabled_why();
            std::printf("ACO Mode: %s\n", why.empty() ? "configured (a record exists for each pipeline's SPIR-V)"
                                                      : why.c_str());
        }
        nr::TemporalConfig temporal;
        temporal.enable = true;
        nr::ControlMaskConfig mask;
        nr::Runtime rt(host, cfg, mask, temporal);
        std::printf("network built at %ux%u (root %s)\n", W, H, root.c_str());
        if (argc < 5) return 0;
        // One frame through the reference RenoDX stage with the same settings and the same input pattern the D3D12
        // test writes, then the output as R10G10B10A2 bytes for a pixel comparison.
        const uint32_t width = W, height = H;
        VkCommandPool pool{};
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = vkctx().family;
        if (vkCreateCommandPool(vkctx().device, &pci, nullptr, &pool) != VK_SUCCESS) return 6;
        VkCommandBuffer cmd{};
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        vkAllocateCommandBuffers(vkctx().device, &cai, &cmd);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence{};
        vkCreateFence(vkctx().device, &fci, nullptr, &fence);

        auto image = [&](bool storage, VkImage* out, VkDeviceMemory* mem, VkFormat fmt = VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
            VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ici.imageType = VK_IMAGE_TYPE_2D;
            ici.format = fmt;
            ici.extent = {width, height, 1};
            ici.mipLevels = 1; ici.arrayLayers = 1;
            ici.samples = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling = VK_IMAGE_TILING_OPTIMAL;
            ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(vkctx().device, &ici, nullptr, out) != VK_SUCCESS) return false;
            VkMemoryRequirements req{};
            vkGetImageMemoryRequirements(vkctx().device, *out, &req);
            VkPhysicalDeviceMemoryProperties mp{};
            vkGetPhysicalDeviceMemoryProperties(vkctx().physical, &mp);
            uint32_t type = UINT32_MAX;
            for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
                if ((req.memoryTypeBits & (1u << i)) &&
                    (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type = i; break; }
            VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            mai.allocationSize = req.size; mai.memoryTypeIndex = type;
            if (vkAllocateMemory(vkctx().device, &mai, nullptr, mem) != VK_SUCCESS) return false;
            return vkBindImageMemory(vkctx().device, *out, *mem, 0) == VK_SUCCESS;
        };
        VkImage colour{}, output{}, motion{};
        VkDeviceMemory colour_mem{}, output_mem{}, motion_mem{};
        // The same pattern the D3D12 test writes (x*13+y*7 etc. at 10 bits) through a staging buffer - or, with
        // NR_TEST_INPUT_HALF, the very RGBA16F bytes that test uploads, so both routes start from one decode.
        const char* input_half = std::getenv("NR_TEST_INPUT_HALF");
        const bool half_input = input_half && input_half[0];
        const VkFormat frame_fmt = half_input ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        const uint64_t bytes = uint64_t(width) * height * (half_input ? 8 : 4);
        if (!image(true, &colour, &colour_mem, frame_fmt) || !image(true, &output, &output_mem, frame_fmt)) {
            std::printf("images failed\n");
            return 7;
        }
        auto buffer = [&](VkBufferUsageFlags usage, VkMemoryPropertyFlags props, uint64_t size, VkBuffer* b, VkDeviceMemory* m) {
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(vkctx().device, &bi, nullptr, b) != VK_SUCCESS) return false;
            VkMemoryRequirements r{};
            vkGetBufferMemoryRequirements(vkctx().device, *b, &r);
            VkPhysicalDeviceMemoryProperties mp{};
            vkGetPhysicalDeviceMemoryProperties(vkctx().physical, &mp);
            uint32_t type = UINT32_MAX;
            for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
                if ((r.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) { type = i; break; }
            VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            mi.allocationSize = r.size; mi.memoryTypeIndex = type;
            if (vkAllocateMemory(vkctx().device, &mi, nullptr, m) != VK_SUCCESS) return false;
            return vkBindBufferMemory(vkctx().device, *b, *m, 0) == VK_SUCCESS;
        };
        VkBuffer up{}, down{};
        VkDeviceMemory up_mem{}, down_mem{};
        if (!buffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    bytes, &up, &up_mem) ||
            !buffer(VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    bytes, &down, &down_mem)) {
            std::printf("buffers failed\n");
            return 8;
        }
        {
            void* m = nullptr;
            vkMapMemory(vkctx().device, up_mem, 0, bytes, 0, &m);
            if (half_input) {
                if (FILE* f = std::fopen(input_half, "rb")) {
                    const size_t got = std::fread(m, 1, size_t(bytes), f);
                    std::fclose(f);
                    if (got != size_t(bytes)) { std::printf("%s is not %llu bytes\n", input_half, (unsigned long long)bytes); return 8; }
                } else {
                    std::printf("cannot read %s\n", input_half);
                    return 8;
                }
            } else {
                auto* px = static_cast<uint32_t*>(m);
                for (uint32_t y = 0; y < height; ++y)
                    for (uint32_t x = 0; x < width; ++x)
                        px[uint64_t(y) * width + x] = ((x * 13u + y * 7u) & 0x3FF) | (((x * 5u + y * 11u) & 0x3FF) << 10) |
                                                      (((x * 3u + y * 17u) & 0x3FF) << 20) | (0x3FFu << 30);
            }
            vkUnmapMemory(vkctx().device, up_mem);
        }
        VkCommandBufferBeginInfo bi2{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi2.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi2);
        auto barrier = [&](VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
                           VkPipelineStageFlags s, VkPipelineStageFlags d) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = from; b.newLayout = to;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            b.srcAccessMask = src; b.dstAccessMask = dst;
            vkCmdPipelineBarrier(cmd, s, d, 0, 0, nullptr, 0, nullptr, 1, &b);
        };
        barrier(colour, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(cmd, up, colour, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier(colour, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        barrier(output, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

        // The guides the D3D12 test makes: bpp 4, bytes {0,0} then (x*3+y*5)&0xFF twice, same for both images.
        struct Guide { VkImage img{}; VkDeviceMemory mem{}; VkFormat fmt{}; VkBuffer up{}; VkDeviceMemory up_mem{}; uint64_t bytes = 0; };
        Guide guides[2];
        guides[0].fmt = VK_FORMAT_R16G16_SFLOAT;
        guides[1].fmt = VK_FORMAT_R32_SFLOAT;
        for (auto& g : guides) {
            if (!image(false, &g.img, &g.mem, g.fmt)) { std::printf("guide image failed\n"); return 10; }
            g.bytes = bytes;
            if (!buffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, g.bytes, &g.up, &g.up_mem)) {
                std::printf("guide buffer failed\n");
                return 10;
            }
            void* m = nullptr;
            vkMapMemory(vkctx().device, g.up_mem, 0, g.bytes, 0, &m);
            auto* p8 = static_cast<uint8_t*>(m);
            for (uint32_t y = 0; y < height; ++y)
                for (uint32_t x = 0; x < width; ++x) {
                    uint8_t* px = p8 + uint64_t(y) * uint64_t(width) * 4 + uint64_t(x) * 4;
                    px[0] = 0; px[1] = 0;
                    px[2] = uint8_t((x * 3u + y * 5u) & 0xFF);
                    px[3] = px[2];
                }
            vkUnmapMemory(vkctx().device, g.up_mem);
            barrier(g.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy gr{};
            gr.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            gr.imageExtent = {width, height, 1};
            vkCmdCopyBufferToImage(cmd, g.up, g.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &gr);
            barrier(g.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        }

        // The uploads are their own submission: a frame's history needs the previous frame submitted first.
        vkEndCommandBuffer(cmd);
        {
            VkSubmitInfo us{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            us.commandBufferCount = 1; us.pCommandBuffers = &cmd;
            if (vkQueueSubmit(vkctx().queue, 1, &us, fence) != VK_SUCCESS ||
                vkWaitForFences(vkctx().device, 1, &fence, VK_TRUE, 60000000000ull) != VK_SUCCESS) {
                std::printf("the uploads did not submit\n");
                return 18;
            }
        }
        nr::renodx::StageConfig sc;
        sc.shaders = root + "\\dlssnr-amd\\shaders\\renodx";
        sc.width = width; sc.height = height;
        sc.nr_width = width; sc.nr_height = height;
        sc.frame_format = frame_fmt;
        sc.hdr_flag = true;             // the frame the route sees: linear HDR (16F) or HDR10 (10-bit)
        sc.present = half_input ? 3 : 2;   // 3 the 16-bit linear scRGB of a JPEG XR capture, 2 HDR10
        nr::renodx::StageConfig& scc = sc;
        (void)scc;
        // The noise field the pre block reads, from the same region the D3D12 route dumps (wgt at noise[0].off*4).
        if (std::getenv("NR_RUNTIME_NOISE_DUMP")) rt.capture_noise();
        if (std::getenv("NR_RUNTIME_PRE_DUMP")) rt.capture_pre();
        if (std::getenv("NR_RUNTIME_KERN_DUMP")) rt.capture_kernel();
        if (std::getenv("NR_RUNTIME_WGT_DUMP")) rt.capture_weights();
        nr::renodx::Stage stage(host, sc);
        // The stage's own network boundary, for a comparison with the D3D12 route's capture (RGBA16F, W*H*8).
        VkBuffer bproxy{}, bneural{};
        VkDeviceMemory bproxy_mem{}, bneural_mem{};
        const bool probe_f32 = std::getenv("NR_ENCODE_PROBE_F32") != nullptr;
        const uint64_t bbytes = uint64_t(width) * height * (probe_f32 ? 16 : 8);
        if (!buffer(VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, bbytes, &bproxy, &bproxy_mem) ||
            !buffer(VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, bbytes, &bneural, &bneural_mem)) {
            std::printf("capture buffers failed\n");
            return 17;
        }
        nr::renodx::StageFrame sf;
        sf.colour.image = colour;
        sf.colour.format = frame_fmt;
        sf.colour.width = width; sf.colour.height = height;
        // The caller describes what the images are in and what they must be left in (nr_renodx.cpp's contract).
        sf.colour.before = sf.colour.after = VK_IMAGE_LAYOUT_GENERAL;
        sf.colour.before_stage = sf.colour.after_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        sf.colour.before_access = sf.colour.after_access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        sf.output.image = output;
        sf.output.format = sf.colour.format;
        sf.output.width = width; sf.output.height = height;
        sf.output.before = sf.output.after = VK_IMAGE_LAYOUT_GENERAL;
        sf.output.before_stage = sf.output.after_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        sf.output.before_access = sf.output.after_access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        sf.present_white_nits = 203.f;
        sf.motion.image = guides[0].img;
        sf.motion.format = guides[0].fmt;
        sf.motion.width = width; sf.motion.height = height;
        sf.motion.before = sf.motion.after = VK_IMAGE_LAYOUT_GENERAL;
        sf.motion.before_stage = sf.motion.after_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        sf.motion.before_access = sf.motion.after_access = VK_ACCESS_SHADER_READ_BIT;
        sf.motion_x = 0; sf.motion_y = 0;
        sf.motion_scale_x = 1.0f / float(width);
        sf.motion_scale_y = 1.0f / float(height);
        uint32_t passes = 0;
        const nr::renodx::Stage::Network net = [&](uint32_t k, const nr::ColourFrame& proxy,
                                                  const nr::ColourFrame& neural, bool reset) {
            nr::EngineFrame e{};
            e.motion = sf.motion;
            e.motion_scale_x = 1.0f / float(width);
            e.motion_scale_y = 1.0f / float(height);
            e.depth.image = guides[1].img;
            e.depth.format = guides[1].fmt;
            e.depth.width = width;
            e.depth.height = height;
            e.colour = proxy;
            e.target = neural;
            e.reset = reset;
            e.feature = uint64_t(k) + 1;
            // The ASD route's own settings (dlssnr-amd-asd.ini: NetNoiseEveryFrame 1, automatic mask off), so the
            // two boundaries are compared under the same controls and not just the same proxy.
            nr::Controls controls;
            std::fprintf(stderr, "host controls: intensity %g style %d tone %g structure %g skin %g mask %d detail %g colour %g env %g\n",
                         double(controls.intensity), controls.style, double(controls.local_tone),
                         double(controls.local_structure), double(controls.skin_structure),
                         controls.automatic_mask ? 1 : 0, double(controls.skin_detail), double(controls.skin_colour),
                         double(controls.environment_detail));
            controls.noise_every_frame =
                [] { const char* n = std::getenv("NR_STAGE_NOISE_EVERY_FRAME"); return !(n && n[0] == '0'); }();
            controls.automatic_mask = false;
            // The probe observes the actual pre-encode F32 store; the normal network keeps its half boundary.
            if (!probe_f32) rt.record_engine(cmd, e, controls);
            // The network's input and answer, recorded in the same command buffer.
            const auto grab = [&](const nr::ColourFrame& src, VkBuffer dst) {
                barrier(src.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy r{};
                r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                r.imageExtent = {src.width, src.height, 1};
                vkCmdCopyImageToBuffer(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &r);
                barrier(src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            };
            grab(proxy, bproxy);
            if (!probe_f32) grab(neural, bneural);
        };
        // Two frames: the second one runs with history, so a first-frame noise initialisation can be told apart
        // from an arithmetic difference.
        for (int fi = 0; fi < 2; ++fi) {
            vkBeginCommandBuffer(cmd, &bi2);
            vkResetFences(vkctx().device, 1, &fence);
            passes = stage.record(cmd, sf, net);
            barrier(output, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
            vkCmdCopyImageToBuffer(cmd, output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, down, 1, &region);
            barrier(output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            vkEndCommandBuffer(cmd);
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
            if (vkQueueSubmit(vkctx().queue, 1, &si, fence) != VK_SUCCESS ||
                vkWaitForFences(vkctx().device, 1, &fence, VK_TRUE, 60000000000ull) != VK_SUCCESS) {
                std::printf("submit failed (frame %d)\n", fi);
                return 9;
            }
            char ref_name[64];
            std::snprintf(ref_name, sizeof ref_name, "reference_frame%d.bin", fi);
            {
                void* m = nullptr;
                vkMapMemory(vkctx().device, down_mem, 0, bytes, 0, &m);
                const std::string path = std::string(argv[4]) + "\\" + ref_name;
                if (FILE* f = std::fopen(path.c_str(), "wb")) {
                    std::fwrite(m, 1, size_t(bytes), f);
                    std::fclose(f);
                    std::printf("reference output written to %s\n", path.c_str());
                }
                vkUnmapMemory(vkctx().device, down_mem);
            }
            if (const char* sd = std::getenv("NR_STAGE_DUMP"); sd && sd[0]) {
                const VkBuffer bufs[2] = {bproxy, bneural};
                const VkDeviceMemory mems[2] = {bproxy_mem, bneural_mem};
                const char* names[] = {"vulkan_proxy", "vulkan_neural"};
                for (int i = 0; i < (probe_f32 ? 1 : 2); ++i) {
                    void* m = nullptr;
                    if (vkMapMemory(vkctx().device, mems[i], 0, bbytes, 0, &m) == VK_SUCCESS) {
                        char nm[64];
                        std::snprintf(nm, sizeof nm, "%s_f%d.bin", names[i], fi);
                        const std::string path = std::string(sd) + "\\" + nm;
                        if (FILE* f = std::fopen(path.c_str(), "wb")) {
                            std::fwrite(m, 1, size_t(bbytes), f);
                            std::fclose(f);
                            std::printf("boundary written to %s\n", path.c_str());
                        }
                        vkUnmapMemory(vkctx().device, mems[i]);
                    }
                }
            }
        }
        if (const char* wd = std::getenv("NR_RUNTIME_WGT_DUMP"); wd && wd[0]) {
            std::string cw;
            const std::string path = std::string(wd) + "\\vulkan_wgt.bin";
            const bool wrote = rt.write_weights_capture(path, &cw);
            std::printf("weights capture %s (%s)\n", wrote ? "written" : "FAILED", cw.c_str());
        }
        if (const char* kd = std::getenv("NR_RUNTIME_KERN_DUMP"); kd && kd[0]) {
            std::string cw;
            const std::string path = std::string(argv[4]) + "\\vulkan_kern.bin";
            const bool wrote = rt.write_kernel_capture(path, &cw);
            std::printf("kernel capture %s (%s)\n", wrote ? "written" : "FAILED", cw.c_str());
        }
        if (const char* pd = std::getenv("NR_RUNTIME_PRE_DUMP"); pd && pd[0]) {
            std::string cw;
            const char* off = std::getenv("NR_RUNTIME_PRE_OFF");
            const std::string path = std::string(pd) + "\\vulkan_pre" + (off ? "_off" + std::string(off) : "") + ".bin";
            const bool wrote = rt.write_pre_capture(path, &cw);
            std::printf("pre capture %s (%s)\n", wrote ? "written" : "FAILED", cw.c_str());
        }
        if (const char* nd = std::getenv("NR_RUNTIME_NOISE_DUMP"); nd && nd[0]) {
            std::string cw;
            const std::string path = std::string(nd) + "\\vulkan_noise.bin";
            const bool wrote = rt.write_noise_capture(path, &cw);
            std::printf("noise capture %s (%s)\n", wrote ? "written" : "FAILED", cw.c_str());
        }
        std::printf("stage: %s\n", stage.recordings_done() ? "recorded" : "no recordings");
    } catch (const std::exception& x) {
        std::printf("runtime: %s\n", x.what());
        return 5;
    }
    return 0;
}
