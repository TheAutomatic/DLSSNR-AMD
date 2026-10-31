// windows/tests/wilsjo2_d3d12: the reference for nr_wilsjo2_d3d12_test.cpp - the same flow on the Vulkan stage
// (nr_wilsjo2.cpp's own, the 0.0.8.1 route's: the shipped wilsjo2_<name>.spv compiled by the Vulkan driver), on the
// same GPU (the D3D12 device's LUID), with the same identity network. Included by the test only.
#pragma once
#include <map>
#include <vector>

extern "C" void nr_vk_load(void);   // vulkan_lazy.sh: the vk* entry points from vulkan-1.dll

namespace vkref {
VkInstance g_inst{};
VkPhysicalDevice g_phys{};
VkDevice g_dev{};
VkQueue g_queue{};
uint32_t g_family = 0;
VkCommandPool g_pool{};
VkCommandBuffer g_cmd{};
VkFence g_fence{};
VkPhysicalDeviceMemoryProperties g_mem{};
bool g_robust = false;

bool ok(VkResult r, const char* what) {
    if (r != VK_SUCCESS) std::printf("vulkan: %s failed (%d)\n", what, int(r));
    return r == VK_SUCCESS;
}

bool init(const LUID& luid, std::string* why) {
    nr_vk_load();
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.apiVersion = VK_API_VERSION_1_4;   // pipeline robustness core there (the stage uses it when the device has it)
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &ai;
    if (!ok(vkCreateInstance(&ii, nullptr, &g_inst), "instance")) { *why = "no Vulkan instance"; return false; }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_inst, &n, nullptr);
    std::vector<VkPhysicalDevice> all(n);
    vkEnumeratePhysicalDevices(g_inst, &n, all.data());
    for (VkPhysicalDevice p : all) {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
        vkGetPhysicalDeviceProperties2(p, &p2);
        if (id.deviceLUIDValid && std::memcmp(id.deviceLUID, &luid, sizeof luid) == 0) {
            g_phys = p;
            std::printf("vulkan reference: %s\n", p2.properties.deviceName);
        }
    }
    if (!g_phys) { *why = "no Vulkan device with the D3D12 device's LUID"; return false; }
    vkGetPhysicalDeviceMemoryProperties(g_phys, &g_mem);
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &n, nullptr);
    std::vector<VkQueueFamilyProperties> qf(n);
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &n, qf.data());
    g_family = UINT32_MAX;
    for (uint32_t i = 0; i < n; ++i)
        if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { g_family = i; break; }
    if (g_family == UINT32_MAX) { *why = "no graphics+compute queue"; return false; }
    // every core feature the device has; VK_EXT_pipeline_robustness when it is there (the stage uses it then)
    VkPhysicalDevicePipelineRobustnessFeaturesEXT prf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &prf};
    vkGetPhysicalDeviceFeatures2(g_phys, &f2);
    g_robust = prf.pipelineRobustness != 0;
    bool robust_ext = false;
    {
        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(g_phys, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> e(en);
        vkEnumerateDeviceExtensionProperties(g_phys, nullptr, &en, e.data());
        for (const auto& x : e) robust_ext = robust_ext || std::strcmp(x.extensionName, VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME) == 0;
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(g_phys, &pp);
        if (!robust_ext && pp.apiVersion < VK_API_VERSION_1_4) g_robust = false;
    }
    const char* ext[] = {VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME};
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = g_family; qi.queueCount = 1; qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    if (!g_robust) f2.pNext = nullptr;
    di.pNext = &f2;
    di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = g_robust && robust_ext ? 1 : 0; di.ppEnabledExtensionNames = ext;
    if (!ok(vkCreateDevice(g_phys, &di, nullptr, &g_dev), "device")) { *why = "no Vulkan device"; return false; }
    vkGetDeviceQueue(g_dev, g_family, 0, &g_queue);
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pi.queueFamilyIndex = g_family;
    vkCreateCommandPool(g_dev, &pi, nullptr, &g_pool);
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = g_pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
    vkAllocateCommandBuffers(g_dev, &ca, &g_cmd);
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(g_dev, &fi, nullptr, &g_fence);
    return true;
}
nr::HostDevice host() {
    nr::HostDevice h;
    h.instance = g_inst; h.physical = g_phys; h.device = g_dev; h.queue = g_queue; h.queue_family = g_family;
    return h;
}
void begin() {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(g_cmd, &bi);
}
bool submit() {
    vkEndCommandBuffer(g_cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &g_cmd;
    if (!ok(vkQueueSubmit(g_queue, 1, &si, g_fence), "submit")) return false;
    const VkResult r = vkWaitForFences(g_dev, 1, &g_fence, VK_TRUE, 20000000000ull);
    vkResetFences(g_dev, 1, &g_fence);
    vkResetCommandBuffer(g_cmd, 0);
    return ok(r, "wait");
}
uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_mem.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (g_mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0;
}
VkFormat vk_of(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case DXGI_FORMAT_R10G10B10A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;   // the same bits
        case DXGI_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
        case DXGI_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case DXGI_FORMAT_R16G16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
        default: return VK_FORMAT_UNDEFINED;
    }
}
struct Tex { VkImage image{}; VkDeviceMemory mem{}; DXGI_FORMAT dxgi{}; uint32_t w{}, h{}; };
std::map<VkImage, Tex> g_tex;

void barrier_all() {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
}
// A 2D image in GENERAL layout (sampled, storage, copies).
VkImage texture(DXGI_FORMAT f, uint32_t w, uint32_t h) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = vk_of(f); ci.extent = {w, h, 1}; ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Tex t;
    t.dxgi = f; t.w = w; t.h = h;
    if (!ok(vkCreateImage(g_dev, &ci, nullptr, &t.image), "image")) return VK_NULL_HANDLE;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_dev, t.image, &mr);
    VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mi.allocationSize = mr.size; mi.memoryTypeIndex = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(g_dev, &mi, nullptr, &t.mem);
    vkBindImageMemory(g_dev, t.image, t.mem, 0);
    begin();
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = t.image; ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    ib.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &ib);
    submit();
    g_tex[t.image] = t;
    return t.image;
}
void destroy(VkImage i) {
    auto it = g_tex.find(i);
    if (it == g_tex.end()) return;
    vkDestroyImage(g_dev, i, nullptr);
    vkFreeMemory(g_dev, it->second.mem, nullptr);
    g_tex.erase(it);
}
nr::wilsjo2::Image image(VkImage i) {
    const Tex& t = g_tex.at(i);
    nr::wilsjo2::Image im;
    im.image = i; im.format = vk_of(t.dxgi); im.width = t.w; im.height = t.h; im.layout = VK_IMAGE_LAYOUT_GENERAL;
    return im;
}
struct Buf { VkBuffer b{}; VkDeviceMemory m{}; };
Buf host_buffer(VkDeviceSize bytes) {
    Buf r;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = bytes; bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    vkCreateBuffer(g_dev, &bi, nullptr, &r.b);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g_dev, r.b, &mr);
    VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mi.allocationSize = mr.size;
    mi.memoryTypeIndex = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(g_dev, &mi, nullptr, &r.m);
    vkBindBufferMemory(g_dev, r.b, r.m, 0);
    return r;
}
void free_buffer(Buf& b) { vkDestroyBuffer(g_dev, b.b, nullptr); vkFreeMemory(g_dev, b.m, nullptr); }
// `pack` / `unpack` / `texel_bytes`: the test's (the same bits as the D3D12 formats)
void upload(VkImage i, const std::vector<float>& px) {
    const Tex& t = g_tex.at(i);
    const size_t tb = texel_bytes(t.dxgi);
    Buf b = host_buffer(VkDeviceSize(t.w) * t.h * tb);
    uint8_t* m = nullptr;
    vkMapMemory(g_dev, b.m, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&m));
    for (size_t k = 0; k < size_t(t.w) * t.h; ++k) pack(t.dxgi, &px[k * 4], m + k * tb);
    vkUnmapMemory(g_dev, b.m);
    begin();
    barrier_all();
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {t.w, t.h, 1};
    vkCmdCopyBufferToImage(g_cmd, b.b, i, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    barrier_all();
    submit();
    free_buffer(b);
}
std::vector<float> download(VkImage i) {
    const Tex& t = g_tex.at(i);
    const size_t tb = texel_bytes(t.dxgi);
    Buf b = host_buffer(VkDeviceSize(t.w) * t.h * tb);
    begin();
    barrier_all();
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {t.w, t.h, 1};
    vkCmdCopyImageToBuffer(g_cmd, i, VK_IMAGE_LAYOUT_GENERAL, b.b, 1, &c);
    barrier_all();
    std::vector<float> px;
    if (submit()) {
        px.resize(size_t(t.w) * t.h * 4);
        uint8_t* m = nullptr;
        vkMapMemory(g_dev, b.m, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&m));
        for (size_t k = 0; k < size_t(t.w) * t.h; ++k) unpack(t.dxgi, m + k * tb, &px[k * 4]);
        vkUnmapMemory(g_dev, b.m);
    }
    free_buffer(b);
    return px;
}

// The flow's Vulkan side as the bridge has it (every image GENERAL; the stage's barrier after each dispatch), the
// network the same identity.
class Io final : public nr::wilsjo2::FlowIo {
public:
    Io(nr::wilsjo2::Stage& stage, DXGI_FORMAT frame) : stage_(stage), frame_(frame) {}
    nr::wilsjo2::Surf make(nr::wilsjo2::Fmt f, uint32_t w, uint32_t h, bool) override {
        return {reinterpret_cast<void*>(texture(dxgi_of(f, frame_), w, h))};
    }
    void retire(nr::wilsjo2::Surf& s) override {
        if (s.h) retired_.push_back(reinterpret_cast<VkImage>(s.h));
        s = {};
    }
    void use(nr::wilsjo2::Surf, nr::wilsjo2::Access) override {}
    bool record(const Recorder& fn) override {
        begin();
        bool r = false;
        try {
            r = fn(g_cmd, stage_, [](nr::wilsjo2::Surf s) { return image(reinterpret_cast<VkImage>(s.h)); });
        } catch (const std::exception& x) {
            std::printf("vulkan record: %s\n", x.what());
        }
        barrier_all();
        r = submit() && r;
        stage_.end_recording();
        return r;
    }
    bool has(nr::wilsjo2::Mode m) const override { return stage_.has(m); }
    bool has_fast() const override { return false; }
    bool stage_failed() const override { return false; }
    nr::wilsjo2::PassReady pass(uint32_t, const nr::wilsjo2::PassControls&, uint32_t, uint32_t) override {
        return nr::wilsjo2::PassReady::kReady;
    }
    void release_passes(size_t) override {}
    int network(const nr::wilsjo2::NetCall& c) override {
        return record([&](VkCommandBuffer vk, nr::wilsjo2::Stage& stage, const ImageOf& image_of) {
            nr::wilsjo2::Dispatch d;
            d.t0 = image_of(c.input);
            d.u0 = image_of(c.output);
            d.constants = nr::wilsjo2::copy_constants(nr::wilsjo2::kCopy, 0, 0, 0, 0, c.width, c.height);
            stage.record(vk, d);
            return true;
        }) ? 0 : 1;
    }
    void say(const char*) override {}
    std::vector<VkImage> retired_;

private:
    nr::wilsjo2::Stage& stage_;
    DXGI_FORMAT frame_;
};
}  // namespace vkref
