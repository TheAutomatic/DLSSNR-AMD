#define VK_USE_PLATFORM_XLIB_KHR
#define VK_USE_PLATFORM_XCB_KHR
#define VK_USE_PLATFORM_WAYLAND_KHR
// NR Control: the OptiScaler routes' own menu (End), a Linux Vulkan layer under winevulkan that draws over the game's
// swap chain at Present. It edits the game's dlssnr-amd.ini - [RenoDX] (every NR setting with the RenoDX-style
// option on), [Preprocess], [Int4Mixed] - which our DLL in the game re-reads about once a second, and shows what NR
// is doing from <game>/dlssnr-amd/control.bin (nr_control_shared.hpp). OptiScaler's own menu (Insert) stays as it is.
//
// Input: on an X11 surface (XWayland, Proton's default) the validated menu input of the native route (MenuInput:
// End opens and closes, keyboard and mouse grabbed while open). A layer cannot read input on Wayland
// (PROTON_ENABLE_WAYLAND=1), so there our DLL reads the game window's keyboard and mouse and hands them over through
// control.bin, and keeps them from the game while the menu is open.
//
// Lives in <game>/dlssnr-amd/control/layer and is enabled from the launch options (VK_ADD_LAYER_PATH +
// VK_INSTANCE_LAYERS): nothing is installed outside the game. Without the menu open every Present goes straight
// through; nothing else of the game's is touched.
#include "nr_control_shared.hpp"
#include "nr_ini_network.hpp"
#include "nr_control_ui.hpp"
#include "nr_menu.hpp"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "menu_compose_frag.h"
#include "menu_compose_vert.h"

#include <vulkan/vk_layer.h>

#include <X11/Xlib.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

void say(const std::string& s) { std::fprintf(stderr, "[nr] NR Control: %s\n", s.c_str()); }
void check(VkResult r, const char* op) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(op) + ": " + std::to_string(r));
}
void check_imgui(VkResult r) {
    if (r != VK_SUCCESS) throw std::runtime_error("ImGui Vulkan " + std::to_string(r));
}
template <class T> T* loader_link(const void* p, VkStructureType type, VkLayerFunction f) {
    for (auto n = static_cast<const VkBaseInStructure*>(p); n; n = n->pNext)
        if (n->sType == type && reinterpret_cast<const T*>(n)->function == f)
            return reinterpret_cast<T*>(const_cast<VkBaseInStructure*>(n));
    return nullptr;
}

// <game>/dlssnr-amd/control/layer/libVkLayer_dlssnr_control.so -> <game>
std::filesystem::path game_dir() {
    Dl_info info{};
    if (!dladdr(reinterpret_cast<const void*>(&say), &info) || !info.dli_fname) return {};
    // The loader names it from the manifest's library_path ("<dir>/./libVkLayer_dlssnr_control.so"): normalise first.
    std::error_code ec;
    auto so = std::filesystem::weakly_canonical(info.dli_fname, ec);
    if (ec) so = std::filesystem::path(info.dli_fname).lexically_normal();
    return so.parent_path().parent_path().parent_path().parent_path();
}

// The device's functions below this layer (a layer never calls the loader's exported entry points: they would enter the
// chain from the top again, and the loader's physical-device handles are not the ones a layer sees).
struct Vk {
#define NR_VK_DEVICE(X) X(CreateRenderPass) X(CreateImageView) X(CreateFramebuffer) X(DestroyFramebuffer) X(DestroyImageView) X(DestroyRenderPass) X(CmdPipelineBarrier) X(CmdBeginRenderPass) X(CmdEndRenderPass) X(WaitForFences) X(DestroyFence) X(DestroySemaphore) X(DestroyCommandPool) X(CreateCommandPool) X(AllocateCommandBuffers) X(CreateFence) X(CreateSemaphore) X(CreateBuffer) X(DestroyBuffer) X(FreeMemory) X(GetBufferMemoryRequirements) X(AllocateMemory) X(BindBufferMemory) X(CmdCopyImageToBuffer) X(MapMemory) X(UnmapMemory) X(ResetCommandBuffer) X(BeginCommandBuffer) X(EndCommandBuffer) X(GetFenceStatus) X(ResetFences) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) X(CreateSampler) X(DestroySampler) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateShaderModule) X(DestroyShaderModule) X(CreateGraphicsPipelines) X(DestroyPipeline) X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdDraw)
#define NR_VK_MEMBER(n) PFN_vk##n n{};
    NR_VK_DEVICE(NR_VK_MEMBER)
#undef NR_VK_MEMBER
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties{};
    bool load(VkDevice d, PFN_vkGetDeviceProcAddr gdpa, VkInstance i, PFN_vkGetInstanceProcAddr gipa) {
        bool ok = true;
#define NR_VK_LOAD(n) ok &= (n = reinterpret_cast<PFN_vk##n>(gdpa(d, "vk" #n))) != nullptr;
        NR_VK_DEVICE(NR_VK_LOAD)
#undef NR_VK_LOAD
        GetPhysicalDeviceMemoryProperties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(i, "vkGetPhysicalDeviceMemoryProperties"));
        return ok && GetPhysicalDeviceMemoryProperties;
    }
};

// ---- control.bin ----
nr::control::Shared* shared() {
    static nr::control::Shared* s = [] () -> nr::control::Shared* {
        const auto path = game_dir() / "dlssnr-amd" / "control.bin";
        const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd < 0) { say("cannot open " + path.string()); return nullptr; }
        struct stat st{};
        if (fstat(fd, &st) == 0 && st.st_size < off_t(sizeof(nr::control::Shared)) &&
            ftruncate(fd, sizeof(nr::control::Shared)) != 0) {
            ::close(fd);
            return nullptr;
        }
        void* p = mmap(nullptr, sizeof(nr::control::Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (p == MAP_FAILED) return nullptr;
        auto* s = static_cast<nr::control::Shared*>(p);
        if (s->magic != nr::control::kMagic || s->version != nr::control::kVersion) {
            std::memset(s, 0, sizeof *s);
            s->version = nr::control::kVersion;
            s->magic = nr::control::kMagic;
        }
        // What an earlier run of the game left is not this run's state (our DLL writes its own from its first frame).
        s->route = s->renodx = s->nr_on = 0;
        s->gpu_ms = s->network_ms = 0.f;
        s->width = s->height = s->nr_width = s->nr_height = s->passes = s->building = 0;
        s->net_running = s->net_want = s->building_net = 3;
        s->net_known = s->panel_lines = 0;
        s->state[0] = s->screenshot[0] = 0;
        return s;
    }();
    return s;
}

// ---- dlssnr-amd.ini, edited in place: a value's line keeps its key and spacing, comments stay ----
class Ini {
  public:
    explicit Ini(std::filesystem::path p) : path_(std::move(p)) {}
    // Re-read when the file changed (the user's editor, our DLL writing defaults). True when it did.
    bool refresh() {
        struct stat st{};
        if (stat(path_.c_str(), &st) != 0) return false;
        const int64_t stamp = int64_t(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
        if (stamp == stamp_ && !lines_.empty()) return false;
        std::ifstream f(path_);
        if (!f) return false;
        lines_.clear();
        std::string l;
        while (std::getline(f, l)) lines_.push_back(l);
        stamp_ = stamp;
        return true;
    }
    bool has_section(const std::string& sec) const { return find_section(sec) >= 0; }
    std::string get(const std::string& sec, const std::string& key, const std::string& def) const {
        const int at = find_key(sec, key);
        if (at < 0) return def;
        const std::string& l = lines_[size_t(at)];
        std::string v = l.substr(l.find('=') + 1);
        const auto c = v.find_first_of(";#");
        if (c != std::string::npos) v = v.substr(0, c);
        return trim(v);
    }
    void set(const std::string& sec, const std::string& key, const std::string& value) {
        const int at = find_key(sec, key);
        if (at >= 0) {
            std::string& l = lines_[size_t(at)];
            const auto eq = l.find('=');
            const bool spaced = eq + 1 < l.size() && l[eq + 1] == ' ';
            l = l.substr(0, eq + 1) + (spaced ? " " : "") + value;
        } else {
            int s = find_section(sec);
            if (s < 0) {
                lines_.push_back("");
                lines_.push_back("[" + sec + "]");
                s = int(lines_.size()) - 1;
            }
            size_t end = size_t(s) + 1;
            while (end < lines_.size() && !(trim(lines_[end]).rfind("[", 0) == 0)) ++end;
            lines_.insert(lines_.begin() + std::ptrdiff_t(end), key + " = " + value);
        }
        dirty_ = true;
    }
    // Written whole through a temporary and a rename: our DLL never reads half a file.
    void save() {
        if (!dirty_) return;
        const auto tmp = path_.string() + ".nrcontrol";
        {
            std::ofstream f(tmp, std::ios::trunc);
            for (const auto& l : lines_) f << l << '\n';
        }
        std::error_code ec;
        std::filesystem::rename(tmp, path_, ec);
        dirty_ = false;
        struct stat st{};
        if (stat(path_.c_str(), &st) == 0) stamp_ = int64_t(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
    }

  private:
    static std::string trim(std::string s) {
        const auto a = s.find_first_not_of(" \t\r\n");
        const auto b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    }
    static std::string lower(std::string s) {
        for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }
    int find_section(const std::string& sec) const {
        for (size_t i = 0; i < lines_.size(); ++i)
            if (lower(trim(lines_[i])) == "[" + lower(sec) + "]") return int(i);
        return -1;
    }
    int find_key(const std::string& sec, const std::string& key) const {
        const int s = find_section(sec);
        if (s < 0) return -1;
        for (size_t i = size_t(s) + 1; i < lines_.size(); ++i) {
            const std::string t = trim(lines_[i]);
            if (t.rfind("[", 0) == 0) break;
            if (t.empty() || t[0] == ';' || t[0] == '#') continue;
            const auto eq = t.find('=');
            if (eq != std::string::npos && lower(trim(t.substr(0, eq))) == lower(key)) return int(i);
        }
        return -1;
    }
    std::filesystem::path path_;
    std::vector<std::string> lines_;
    int64_t stamp_ = -1;
    bool dirty_ = false;
};

// [RenoDX]'s values for nr_control_ui.hpp, read from and written to the ini by the table's names.
struct RenoDXValues {
    std::vector<uint8_t> bools;
    std::vector<uint32_t> uints;
    std::vector<float> floats;
    std::vector<std::string> texts;
    bool& b(int i) { return reinterpret_cast<bool&>(bools[size_t(i)]); }
    uint32_t& u(int i) { return uints[size_t(i)]; }
    float& f(int i) { return floats[size_t(i)]; }
    std::string& s(int i) { return texts[size_t(i)]; }
    void load(const Ini& ini) {
        using namespace nr::renodx_keys;
        bools.assign(kCount, 0); uints.assign(kCount, 0); floats.assign(kCount, 0.f); texts.assign(kCount, {});
        for (int i = 0; i < kCount; ++i) {
            const Key& k = kKeys[i];
            const std::string v = ini.get("RenoDX", k.name, "");
            switch (k.kind) {
                case kBool: bools[size_t(i)] = v.empty() ? k.def != 0.f : (v == "1" || v == "true"); break;
                case kUint: uints[size_t(i)] = v.empty() ? uint32_t(k.def) : uint32_t(std::strtoul(v.c_str(), nullptr, 10)); break;
                case kFloat: floats[size_t(i)] = v.empty() ? k.def : std::strtof(v.c_str(), nullptr); break;
                case kText: texts[size_t(i)] = std::string(v); break;
            }
        }
    }
    void store(Ini& ini) const {
        using namespace nr::renodx_keys;
        for (int i = 0; i < kCount; ++i) {
            const Key& k = kKeys[i];
            char v[64];
            switch (k.kind) {
                case kBool: std::snprintf(v, sizeof v, "%d", bools[size_t(i)] ? 1 : 0); break;
                case kUint: std::snprintf(v, sizeof v, "%u", uints[size_t(i)]); break;
                case kFloat: std::snprintf(v, sizeof v, "%.*f", int(k.decimals), double(floats[size_t(i)])); break;
                case kText: std::snprintf(v, sizeof v, "%s", texts[size_t(i)].c_str()); break;
            }
            if (ini.get("RenoDX", k.name, "\x01") != v) ini.set("RenoDX", k.name, v);
        }
    }
};

// ---- input ----
struct Input {
    virtual ~Input() = default;
    virtual void update(ImGuiIO& io, VkExtent2D extent) = 0;
    virtual bool visible() const = 0;
    virtual void close() = 0;   // the window's close button
};

// NR_CONTROL_AT=<n>: NR_CONTROL_SET and NR_CONTROL_DUMP wait until the menu has been drawn n times (default 0).
uint64_t control_at() {
    const char* e = std::getenv("NR_CONTROL_AT");
    return e ? std::strtoull(e, nullptr, 10) : 0;
}
uint64_t g_drawn = 0;   // frames the menu was drawn in

// NR_CONTROL_OPEN=1: the menu starts open (a test without a keyboard).
bool open_at_start() {
    const char* e = std::getenv("NR_CONTROL_OPEN");
    return e && e[0] == '1';
}

// X11: the native route's menu input (End, grabs while open). Its F10 switch is not used here.
struct X11Input : Input {
    std::shared_ptr<nr::MenuInput> m;
    bool unused = true, forced = open_at_start();
    explicit X11Input(std::shared_ptr<nr::MenuInput> in) : m(std::move(in)) {}
    void update(ImGuiIO& io, VkExtent2D) override {
        if (auto* s = shared()) m->set_focus_hint(s->game_focused != 0);
        m->update(io, unused);
        if (auto* s = shared()) s->menu_open = visible() ? 1 : 0;
    }
    bool visible() const override { return forced || m->visible(); }
    void close() override { forced = false; m->close(); }
};

// Wayland: what our DLL reads from the game's window (control.bin).
struct WaylandInput : Input {
    bool open = open_at_start();
    uint32_t keys_before[8]{};
    int32_t wheel_seen = 0;
    uint32_t chars_seen = 0;
    bool primed = false;
    static ImGuiKey key_of(unsigned vk) {
        if (vk >= 'A' && vk <= 'Z') return ImGuiKey(ImGuiKey_A + int(vk - 'A'));
        if (vk >= '0' && vk <= '9') return ImGuiKey(ImGuiKey_0 + int(vk - '0'));
        switch (vk) {
            case 0x08: return ImGuiKey_Backspace; case 0x09: return ImGuiKey_Tab; case 0x0D: return ImGuiKey_Enter;
            case 0x1B: return ImGuiKey_Escape; case 0x20: return ImGuiKey_Space; case 0x21: return ImGuiKey_PageUp;
            case 0x22: return ImGuiKey_PageDown; case 0x23: return ImGuiKey_End; case 0x24: return ImGuiKey_Home;
            case 0x25: return ImGuiKey_LeftArrow; case 0x26: return ImGuiKey_UpArrow; case 0x27: return ImGuiKey_RightArrow;
            case 0x28: return ImGuiKey_DownArrow; case 0x2D: return ImGuiKey_Insert; case 0x2E: return ImGuiKey_Delete;
            case 0xA0: return ImGuiKey_LeftShift; case 0xA1: return ImGuiKey_RightShift;
            case 0xA2: return ImGuiKey_LeftCtrl; case 0xA3: return ImGuiKey_RightCtrl;
            case 0xA4: return ImGuiKey_LeftAlt; case 0xA5: return ImGuiKey_RightAlt;
            default: return ImGuiKey_None;
        }
    }
    void update(ImGuiIO& io, VkExtent2D extent) override {
        auto* s = shared();
        if (!s) return;
        s->wayland = 1;
        auto down = [&](unsigned vk) { return (s->keys[vk / 32] >> (vk % 32)) & 1u; };
        auto was = [&](unsigned vk) { return (keys_before[vk / 32] >> (vk % 32)) & 1u; };
        if (!primed) {   // what was already held or typed before the first look is not input to the menu
            std::memcpy(keys_before, s->keys, sizeof keys_before);
            wheel_seen = s->wheel;
            chars_seen = s->chars_written;
            primed = true;
        }
        if (down(0x23) && !was(0x23)) open = !open;   // End
        if (open && down(0x1B) && !was(0x1B) && !io.WantTextInput) open = false;   // Esc
        s->menu_open = open ? 1 : 0;
        // the game hid its cursor (mouse look): the menu draws one (X11 shows the grab's cursor instead)
        io.MouseDrawCursor = open && !s->cursor_shown;
        if (open) {
            const float sx = s->client_w ? float(extent.width) / float(s->client_w) : 1.f;
            const float sy = s->client_h ? float(extent.height) / float(s->client_h) : 1.f;
            io.AddMousePosEvent(float(s->mouse_x) * sx, float(s->mouse_y) * sy);
            for (int b = 0; b < 3; ++b) io.AddMouseButtonEvent(b, (s->buttons >> b) & 1u);
            const int32_t wheel = s->wheel;
            if (wheel != wheel_seen) io.AddMouseWheelEvent(0.f, float(wheel - wheel_seen) / 120.f);
            for (unsigned vk = 8; vk < 256; ++vk)
                if (down(vk) != was(vk))
                    if (ImGuiKey k = key_of(vk); k != ImGuiKey_None) io.AddKeyEvent(k, down(vk) != 0);
            io.AddKeyEvent(ImGuiMod_Ctrl, down(0x11) != 0);
            io.AddKeyEvent(ImGuiMod_Shift, down(0x10) != 0);
            io.AddKeyEvent(ImGuiMod_Alt, down(0x12) != 0);
            for (uint32_t c = chars_seen; c != s->chars_written; ++c) io.AddInputCharacter(s->chars[c % 64]);
        }
        wheel_seen = s->wheel;
        chars_seen = s->chars_written;
        std::memcpy(keys_before, s->keys, sizeof keys_before);
    }
    bool visible() const override { return open; }
    void close() override {
        open = false;
        if (auto* s = shared()) s->menu_open = 0;
    }
};

// ---- the menu itself ----
std::mutex imgui_mutex;   // ImGui's current context is process-wide

// How the menu's colours are written into the game's swap chain (menu_compose.frag's modes).
struct Encoding { uint32_t mode; float scale; const char* name; };
Encoding encoding_of(VkFormat format, VkColorSpaceKHR space) {
    constexpr float kWhiteNits = 203.f;   // the menu's white in HDR (BT.2408's graphics white)
    if (space == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) return {2, kWhiteNits / 80.f, "scRGB, white at 203 nits"};
    if (space == VK_COLOR_SPACE_HDR10_ST2084_EXT) return {3, kWhiteNits / 10000.f, "HDR10 (PQ), white at 203 nits"};
    switch (format) {
        case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
            return {1, 1.f, "sRGB format"};
        default: return {0, 1.f, "SDR"};
    }
}

struct Overlay {
    const Vk& vk;
    VkDevice device{};
    VkExtent2D extent{};
    Encoding encoding{};
    std::vector<VkImage> images;   // the swap chain's
    // The menu is drawn by ImGui into an 8-bit image of its own (`menu_*`, `draw_pass`), then laid over the swap chain
    // image in its encoding (`compose_*`, menu_compose.frag): the same menu on SDR, sRGB-format and HDR swap chains.
    VkImage menu_image{};
    VkDeviceMemory menu_memory{};
    VkImageView menu_view{};
    VkFramebuffer menu_frame{};
    VkRenderPass draw_pass{}, compose_pass{};
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> frames;
    VkSampler sampler{};
    VkDescriptorSetLayout set_layout{};
    VkDescriptorPool descriptors{};
    VkDescriptorSet set{};
    VkPipelineLayout layout{};
    VkPipeline compose{};
    ImGuiContext* context{};
    bool backend = false;
    std::chrono::steady_clock::time_point previous = std::chrono::steady_clock::now();
    Ini ini{game_dir() / "dlssnr-amd.ini"};
    RenoDXValues renodx;
    int64_t loaded = -1;

    Overlay(const Vk& functions, VkDevice dev) : vk(functions), device(dev) {}

    // Everything the menu needs; whatever was made before a failure is released by the destructor.
    void init(VkInstance instance, PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice physical, PFN_vkGetDeviceProcAddr gdpa,
              VkQueue queue, uint32_t family, VkFormat format, VkColorSpaceKHR space, VkExtent2D ext,
              std::vector<VkImage> imgs) {
        extent = ext;
        images = std::move(imgs);
        encoding = encoding_of(format, space);
        say("menu on a " + std::to_string(extent.width) + "x" + std::to_string(extent.height) + " swap chain, format " +
            std::to_string(int(format)) + ", colour space " + std::to_string(int(space)) + ": " + encoding.name);
        std::lock_guard<std::mutex> lock(imgui_mutex);
        context = ImGui::CreateContext();
        ImGui::SetCurrentContext(context);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigDebugHighlightIdConflicts = false;   // a developer aid, not a player's message
        ImGui::StyleColorsDark();
        auto& style = ImGui::GetStyle();
        style.WindowRounding = 5;
        style.FrameRounding = 3;
        style.Colors[ImGuiCol_WindowBg] = {.07f, .08f, .10f, .96f};

        // the menu's own image
        constexpr VkFormat kMenuFormat = VK_FORMAT_R8G8B8A8_UNORM;
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = kMenuFormat; ii.extent = {extent.width, extent.height, 1};
        ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(vk.CreateImage(device, &ii, nullptr, &menu_image), "menu image");
        VkMemoryRequirements req{};
        vk.GetImageMemoryRequirements(device, menu_image, &req);
        VkPhysicalDeviceMemoryProperties mem{};
        vk.GetPhysicalDeviceMemoryProperties(physical, &mem);
        uint32_t type = mem.memoryTypeCount;
        for (uint32_t i = 0; i < mem.memoryTypeCount && type == mem.memoryTypeCount; ++i)
            if ((req.memoryTypeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = i;
        for (uint32_t i = 0; i < mem.memoryTypeCount && type == mem.memoryTypeCount; ++i)
            if (req.memoryTypeBits & (1u << i)) type = i;
        if (type == mem.memoryTypeCount) throw std::runtime_error("no memory type for the menu image");
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size; ai.memoryTypeIndex = type;
        check(vk.AllocateMemory(device, &ai, nullptr, &menu_memory), "menu memory");
        check(vk.BindImageMemory(device, menu_image, menu_memory, 0), "menu memory bind");
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = menu_image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = kMenuFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vk.CreateImageView(device, &vi, nullptr, &menu_view), "menu view");

        // ImGui draws into it from transparent black; the composite pass reads it
        VkAttachmentDescription a{};
        a.format = kMenuFormat; a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; a.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference colour{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &colour;
        VkSubpassDependency deps[2]{};
        // after the previous frame's composite has read it; before this frame's composite reads it
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL; deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; deps[0].srcAccessMask = 0;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        deps[1].srcSubpass = 0; deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1; rp.pAttachments = &a; rp.subpassCount = 1; rp.pSubpasses = &sub;
        rp.dependencyCount = 2; rp.pDependencies = deps;
        check(vk.CreateRenderPass(device, &rp, nullptr, &draw_pass), "menu render pass");
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = draw_pass; fi.attachmentCount = 1; fi.pAttachments = &menu_view;
        fi.width = extent.width; fi.height = extent.height; fi.layers = 1;
        check(vk.CreateFramebuffer(device, &fi, nullptr, &menu_frame), "menu framebuffer");

        // the composite pass: over the swap chain image as the game left it
        a.format = format;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        rp.dependencyCount = 0; rp.pDependencies = nullptr;
        check(vk.CreateRenderPass(device, &rp, nullptr, &compose_pass), "composite render pass");
        for (VkImage image : images) {
            vi.image = image; vi.format = format;
            VkImageView view{};
            check(vk.CreateImageView(device, &vi, nullptr, &view), "image view");
            views.push_back(view);
            fi.renderPass = compose_pass; fi.pAttachments = &views.back();
            VkFramebuffer fb{};
            check(vk.CreateFramebuffer(device, &fi, nullptr, &fb), "framebuffer");
            frames.push_back(fb);
        }

        // the composite pipeline
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check(vk.CreateSampler(device, &si, nullptr, &sampler), "sampler");
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &sampler};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1; li.pBindings = &binding;
        check(vk.CreateDescriptorSetLayout(device, &li, nullptr, &set_layout), "descriptor set layout");
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 1; pi.poolSizeCount = 1; pi.pPoolSizes = &size;
        check(vk.CreateDescriptorPool(device, &pi, nullptr, &descriptors), "descriptor pool");
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = descriptors; da.descriptorSetCount = 1; da.pSetLayouts = &set_layout;
        check(vk.AllocateDescriptorSets(device, &da, &set), "descriptor set");
        VkDescriptorImageInfo image_info{sampler, menu_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &image_info;
        vk.UpdateDescriptorSets(device, 1, &w, 0, nullptr);
        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1; pl.pSetLayouts = &set_layout; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &push;
        check(vk.CreatePipelineLayout(device, &pl, nullptr, &layout), "pipeline layout");
        VkShaderModule modules[2]{};
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = sizeof kMenuComposeVert; mi.pCode = kMenuComposeVert;
        check(vk.CreateShaderModule(device, &mi, nullptr, &modules[0]), "vertex shader");
        mi.codeSize = sizeof kMenuComposeFrag; mi.pCode = kMenuComposeFrag;
        const VkResult fr = vk.CreateShaderModule(device, &mi, nullptr, &modules[1]);
        if (fr != VK_SUCCESS) { vk.DestroyShaderModule(device, modules[0], nullptr); check(fr, "fragment shader"); }
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = modules[0]; stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = modules[1]; stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport{0, 0, float(extent.width), float(extent.height), 0, 1};
        VkRect2D scissor{{0, 0}, extent};
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1; vp.pViewports = &viewport; vp.scissorCount = 1; vp.pScissors = &scissor;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable = VK_TRUE;   // premultiplied over the game's colour; the game's alpha kept
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1; cb.pAttachments = &blend;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2; gp.pStages = stages; gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pColorBlendState = &cb;
        gp.layout = layout; gp.renderPass = compose_pass; gp.subpass = 0;
        const VkResult pr = vk.CreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &compose);
        vk.DestroyShaderModule(device, modules[0], nullptr);
        vk.DestroyShaderModule(device, modules[1], nullptr);
        check(pr, "composite pipeline");

        // ImGui's Vulkan functions, from below this layer too (device ones first, then the instance's).
        struct Next { VkDevice d; PFN_vkGetDeviceProcAddr gdpa; VkInstance i; PFN_vkGetInstanceProcAddr gipa; } next{device, gdpa, instance, gipa};
        if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, [](const char* name, void* user) {
                const auto* n = static_cast<const Next*>(user);
                PFN_vkVoidFunction f = n->gdpa(n->d, name);
                return f ? f : n->gipa(n->i, name);
            }, &next))
            throw std::runtime_error("ImGui Vulkan functions missing");
        ImGui_ImplVulkan_InitInfo init{};
        init.ApiVersion = VK_API_VERSION_1_3;
        init.Instance = instance; init.PhysicalDevice = physical; init.Device = device;
        init.QueueFamily = family; init.Queue = queue; init.RenderPass = draw_pass;
        init.MinImageCount = 2; init.ImageCount = std::max(2u, uint32_t(images.size()));
        init.MSAASamples = VK_SAMPLE_COUNT_1_BIT; init.DescriptorPoolSize = 32;
        init.CheckVkResultFn = check_imgui;
        if (!ImGui_ImplVulkan_Init(&init)) throw std::runtime_error("ImGui Vulkan initialization failed");
        backend = true;
    }
    ~Overlay() {
        {
            std::lock_guard<std::mutex> lock(imgui_mutex);
            if (context) {
                ImGui::SetCurrentContext(context);
                if (backend) ImGui_ImplVulkan_Shutdown();
                ImGui::DestroyContext(context);
            }
        }
        if (compose) vk.DestroyPipeline(device, compose, nullptr);
        if (layout) vk.DestroyPipelineLayout(device, layout, nullptr);
        if (descriptors) vk.DestroyDescriptorPool(device, descriptors, nullptr);
        if (set_layout) vk.DestroyDescriptorSetLayout(device, set_layout, nullptr);
        if (sampler) vk.DestroySampler(device, sampler, nullptr);
        for (auto f : frames) vk.DestroyFramebuffer(device, f, nullptr);
        for (auto v : views) vk.DestroyImageView(device, v, nullptr);
        if (menu_frame) vk.DestroyFramebuffer(device, menu_frame, nullptr);
        if (compose_pass) vk.DestroyRenderPass(device, compose_pass, nullptr);
        if (draw_pass) vk.DestroyRenderPass(device, draw_pass, nullptr);
        if (menu_view) vk.DestroyImageView(device, menu_view, nullptr);
        if (menu_image) vk.DestroyImage(device, menu_image, nullptr);
        if (menu_memory) vk.FreeMemory(device, menu_memory, nullptr);
    }

    // One frame of input and, while open, the menu. True when it is to be drawn into this image.
    bool frame(Input& input) {
        if (auto* sh = shared()) ++sh->menu_seq;
        std::lock_guard<std::mutex> lock(imgui_mutex);
        ImGui::SetCurrentContext(context);
        auto& io = ImGui::GetIO();
        const auto now = std::chrono::steady_clock::now();
        io.DeltaTime = std::clamp(std::chrono::duration<float>(now - previous).count(), .001f, .1f);
        previous = now;
        io.DisplaySize = {float(extent.width), float(extent.height)};
        input.update(io, extent);
        if (!input.visible()) {   // the menu closed: only the OSD, when it is due
            if (!osd_due(now)) return false;
            const bool cursor = io.MouseDrawCursor;
            io.MouseDrawCursor = false;
            ImGui_ImplVulkan_NewFrame();
            ImGui::NewFrame();
            draw_osd();
            ImGui::Render();
            io.MouseDrawCursor = cursor;
            return true;
        }
        if (ini.refresh() || loaded < 0) { renodx.load(ini); loaded = 1; }
        // NR_CONTROL_SET=Section.Key=value,...: written once through the menu's own ini path (a test without a mouse).
        static bool set_done = false;
        if (const char* e = std::getenv("NR_CONTROL_SET"); e && !set_done && g_drawn >= control_at()) {
            set_done = true;
            std::stringstream ss(e);
            std::string item;
            while (std::getline(ss, item, ',')) {
                const auto dot = item.find('.'), eq = item.find('=');
                if (dot != std::string::npos && eq != std::string::npos && dot < eq)
                    ini.set(item.substr(0, dot), item.substr(dot + 1, eq - dot - 1), item.substr(eq + 1));
            }
            ini.save();
            renodx.load(ini);
            say("NR_CONTROL_SET written at menu frame " + std::to_string(g_drawn) + ": " + e);
        }
        // NR_CONTROL_SHOT=1: the screenshot button, pressed once at NR_CONTROL_AT.
        static bool shot_done = false;
        if (const char* e = std::getenv("NR_CONTROL_SHOT"); e && e[0] == '1' && !shot_done && g_drawn >= control_at()) {
            shot_done = true;
            if (auto* sh = shared()) ++sh->screenshot_request;
            say("NR_CONTROL_SHOT: the screenshot button pressed at menu frame " + std::to_string(g_drawn));
        }
        ImGui_ImplVulkan_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({24, 24}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({560, std::min(float(extent.height) - 48.f, 820.f)}, ImGuiCond_FirstUseEver);
        bool open = true;
        if (ImGui::Begin("NR Control", &open)) draw();
        ImGui::End();
        ImGui::Render();
        if (!open) input.close();
        ini.save();
        return true;
    }

    // [Network] OSD with the menu closed: while a switch builds, its progress, then the network now running for 3 s
    // (switch, the default); the running network all the time (always); nothing (off). The mode is read from the file
    // once a second.
    int osd_mode = 1;
    uint32_t osd_want = ~0u, osd_running = ~0u;
    std::chrono::steady_clock::time_point osd_until{}, osd_read{};
    bool osd_due(std::chrono::steady_clock::time_point now) {
        if (now - osd_read > std::chrono::seconds(1)) {
            osd_read = now;
            if (ini.refresh()) loaded = -1;   // the menu loads the rest when it opens
            const std::string m = ini.get("Network", "OSD", "switch");
            osd_mode = m == "off" ? 0 : m == "always" ? 2 : 1;
        }
        const nr::control::Shared* s = shared();
        if (!s || !s->route || !s->net_known) return false;
        if (s->net_want != osd_want || (s->net_running < 3 && s->net_running != osd_running)) {
            if (osd_want != ~0u && osd_running != ~0u) osd_until = now + std::chrono::seconds(3);
            osd_want = s->net_want;
            if (s->net_running < 3) osd_running = s->net_running;
        }
        if (osd_mode == 0 || s->net_running >= 3) return false;
        return osd_mode == 2 || switching(s) || now < osd_until;
    }
    static bool switching(const nr::control::Shared* s) {
        return s->building && s->building_net < 3 && s->building_net != s->net_running;
    }
    void draw_osd() {
        const nr::control::Shared* s = shared();
        ImGui::SetNextWindowPos({24, 24});
        ImGui::SetNextWindowBgAlpha(0.65f);
        ImGui::Begin("##nr-osd", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        if (switching(s))
            ImGui::Text("NR: switching to %s ... %.0f%%", nr::network_codename(int(s->building_net)),
                        double(s->build_progress) * 100.0);
        else
            ImGui::Text("NR: %s", s->nr_on ? nr::network_codename(int(s->net_running)) : "off");
        ImGui::End();
    }

    // A 0/1 key as a checkbox, written at once.
    void flag(const char* label, const char* section, const char* key, const char* def, const char* help) {
        bool on = ini.get(section, key, def) == "1";
        if (ImGui::Checkbox(label, &on)) ini.set(section, key, on ? "1" : "0");
        nr::control_ui::help(help);
    }

    // A hotkey as text, written when the field is let go (the DLL reads the file again and takes it at once).
    void hotkey_text(const char* label, const char* section, const char* help) {
        static std::unordered_map<std::string, std::string> editing;
        auto it = editing.find(label);
        std::string& t = it != editing.end() ? it->second : (editing[label] = ini.get(section, "Hotkey", ""));
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s", t.c_str());
        if (ImGui::InputText(label, buf, sizeof buf)) t = buf;
        if (ImGui::IsItemDeactivatedAfterEdit()) { ini.set(section, "Hotkey", t); editing.erase(label); }
        else if (!ImGui::IsItemActive()) editing.erase(label);
        nr::control_ui::help(help);
    }

    void draw() {
        const nr::control::Shared* s = shared();
        const bool renodx_running = s && s->renodx;
        if (s && s->route) {
            ImGui::Text("Neural Rendering: %s%s", s->nr_on ? "on" : "off", renodx_running ? " (RenoDX style)" : "");
            // As OptiScaler shows it: the game's render extent -> the upscaler's output (NR's frame after the
            // upscaler; the swap chain when NR runs before it), then where NR runs.
            if (s->width) {
                // The game's own render extent and upscaler output where known; else NR after the upscaler: its frame
                // is the upscaler's output (before it, the output is not NR's to know).
                const bool before = s->placement == 2 ||
                                    (s->placement == 0 && s->render_width == s->width && s->render_height == s->height);
                const uint32_t ow = s->output_width ? s->output_width : before ? 0u : s->width;
                const uint32_t oh = s->output_width ? s->output_height : before ? 0u : s->height;
                if (s->render_width && ow && (s->render_width != ow || s->render_height != oh))
                    ImGui::Text("Upscaling: %ux%u -> %ux%u (%.2fx)", s->render_width, s->render_height, ow, oh,
                                double(oh) / double(std::max(s->render_height, 1u)));
                else if (s->render_width && ow)
                    ImGui::Text("No upscaling: %ux%u", ow, oh);
                else if (s->render_width)
                    ImGui::Text("Render: %ux%u", s->render_width, s->render_height);
                const char* where = s->placement == 1 ? " (after the upscaler)" : s->placement == 2 ? " (before the upscaler)" : "";
                ImGui::Text("NR on %ux%u%s, network %ux%u, %u pass%s", s->width, s->height, where, s->nr_width,
                            s->nr_height, s->passes, s->passes == 1 ? "" : "es");
            }
            // as the ReShade page: the network itself, then the work around it, with the whole
            if (s->gpu_ms > 0.f && s->network_ms > 0.f) {
                ImGui::Text("NR cost: %.2f ms/frame", double(s->network_ms));
                ImGui::Text("%s: %.2f ms/frame   (total %.2f ms)", renodx_running ? "RenoDX processing" : "OptiScaler route",
                            double(std::max(s->gpu_ms - s->network_ms, 0.f)), double(s->gpu_ms));
            } else if (s->gpu_ms > 0.f) {
                ImGui::Text("NR cost: %.2f ms/frame", double(s->gpu_ms));
            } else {
                ImGui::TextUnformatted("NR cost: --");
            }
            if (s->net_running < 3) ImGui::Text("Network: %s", nr::network_codename(int(s->net_running)));
            if (s->building) ImGui::TextDisabled("Building a network...");
            if (s->state[0]) ImGui::TextWrapped("%s", s->state);
        } else {
            ImGui::TextDisabled("Waiting for NR to run (DLSS in the game's settings, OptiScaler's menu: Insert).");
        }
        // [RenoDX]: every NR setting while the RenoDX-style option runs; on the plain OptiScaler route NR's settings stay
        // in OptiScaler's menu and this menu has int4 mixed and Preprocess only (before NR first runs: as the file
        // says, which is what the DLL read at load)
        const bool renodx_route = s && s->route ? renodx_running : ini.get("RenoDX", "Enabled", "0") == "1";
        if (!renodx_route) ImGui::TextDisabled("NR's other settings: OptiScaler's menu (Insert).");
        ImGui::Separator();
        if (renodx_route && nr::control_ui::draw_renodx(renodx, nr::control_ui::Route::kOptiScaler, renodx_route, [s] {
                if (ImGui::Button("Take a screenshot now") && s) ++const_cast<nr::control::Shared*>(s)->screenshot_request;
                if (s && s->screenshot[0]) ImGui::TextWrapped("Screenshot: %s", s->screenshot);
            }))
            renodx.store(ini);
        // [Network]: which network runs (the three by the DLL's codenames); one that cannot run in this run is
        // greyed out with the reason the DLL found (net_why), and the DLL skips it
        // (not "Network": the RenoDX table has a group of that name, and the same label is the same header)
        if (ImGui::CollapsingHeader("Network model and options", ImGuiTreeNodeFlags_DefaultOpen)) {
            const int file_model = std::max(0, nr::network_choice(ini.get("Network", "Model", "CC_SMALL_FP8")));
            const int shown = s && s->net_known && s->net_want < 3 ? int(s->net_want) : file_model;
            if (ImGui::BeginCombo("Network##model", nr::network_codename(shown))) {
                for (int n = 0; n < nr::kNetworkChoices; ++n) {
                    const char* why = s && s->net_known ? s->net_why[n] : "";
                    const bool ok = !why[0];
                    if (ImGui::Selectable(nr::network_codename(n), n == shown, ok ? 0 : ImGuiSelectableFlags_Disabled))
                        ini.set("Network", "Model", nr::network_codename(n));
                    if (!ok) { ImGui::SameLine(); ImGui::TextDisabled("- %s", why); }
                }
                ImGui::EndCombo();
            }
            nr::control_ui::help("[Network] Model. CC_SMALL_FP8: 310.9.16's default network, the fastest. CC_SILVER_AARDWOLD: "
                                 "310.8.0's network (310.9.16's preset 1). CC_SILVER_AARDWOLD_INT4_MIXED: that network with part "
                                 "of its arithmetic in int4, faster, a slightly different picture. Switching builds the new "
                                 "network in the background; the running one serves until it is ready.");
            for (int n = 0; n < nr::kNetworkChoices; ++n)
                if (s && s->net_known && s->net_why[n][0]) ImGui::TextDisabled("%s: %s", nr::network_codename(n), s->net_why[n]);
            hotkey_text("Hotkey##net", "Network", "[Network] Hotkey: cycles through the networks that can run. Ctrl, Shift, "
                        "Alt and one key, e.g. Ctrl+F11 or Alt+F9; empty: no hotkey.");
            bool sound = ini.get("Network", "Sound", "1") == "1";
            if (ImGui::Checkbox("Sound##net", &sound)) ini.set("Network", "Sound", sound ? "1" : "0");
            nr::control_ui::help("[Network] Sound. A cue on every switch: one note CC_SMALL_FP8, two going down "
                                 "CC_SILVER_AARDWOLD, two going up CC_SILVER_AARDWOLD_INT4_MIXED.");
            static const char* const residencies[] = {"auto", "resident", "offload", "minimal"};
            const std::string r = ini.get("Network", "Residency", "auto");
            int ri = 0;
            for (int k = 0; k < 4; ++k) if (r == residencies[k]) ri = k;
            if (ImGui::Combo("Residency", &ri, residencies, 4)) ini.set("Network", "Residency", residencies[ri]);
            nr::control_ui::help("[Network] Residency: what is kept of a network not in use. The networks share one "
                                 "activation memory, so mostly their weights. auto (default): the weights stay in video "
                                 "memory, switching back is quick; moved to RAM when video memory runs short, freed when RAM "
                                 "does too. resident: kept whole, switching back is instant, the most video memory. offload: "
                                 "the weights in RAM, all video memory freed. minimal: everything freed, switching back builds "
                                 "anew. While the other network gets ready, the running one keeps serving. NR off for 10 s: "
                                 "offload and minimal do the same to the running network, auto only when video memory is short.");
            static const char* const osds[] = {"off", "switch", "always"};
            const std::string o = ini.get("Network", "OSD", "switch");
            int oi = 1;
            for (int k = 0; k < 3; ++k) if (o == osds[k]) oi = k;
            if (ImGui::Combo("OSD", &oi, osds, 3)) ini.set("Network", "OSD", osds[oi]);
            nr::control_ui::help("[Network] OSD: the running network in the corner of the picture. switch (default): while "
                                 "a switch builds, its progress; then the new network for 3 s. always: all the time. off: never.");
            flag("Style grade (Natural / Cinematic)", "Network", "Style Grade", "1",
                 "[Network] Style Grade. On (default): Natural and Cinematic are graded after the network as NVIDIA's DLL "
                 "does. Off: the picture these styles had up to 0.0.6, without that grade. Standard is not affected. "
                 "Takes effect at once.");
            flag("Motion without depth", "Network", "Motion Without Depth", "1",
                 "[Network] Motion Without Depth. Where the network's previous-frame lookup reads its motion vector.\n"
                 "On (default): the pixel's own vector, depth not used, as NVIDIA's DLL does.\n"
                 "Off: with depth, the vector of the nearest of the pixel and its four diagonal neighbours.\n"
                 "Only moving edges between frames with motion vectors and depth differ. Takes effect after a restart.");
            flag("Resize history", "Network", "Resize History", "0",
                 "[Network] Resize History. Only when NR runs before the upscaler and the game changes its render size\n"
                 "(dynamic resolution):\n"
                 "Off (default): NR's previous result is dropped and NR starts again at the new size, as NVIDIA's DLL does.\n"
                 "On: the previous result is scaled to the new size and kept. Steadier on our tests when the size changes\n"
                 "often, but not what NVIDIA's DLL does. Games with jittered input (most DLSS games) hide most of the\n"
                 "restart, so the default is off.");
        }
        // What the game hands NR, and who covers what it does not (the DLL's lines, nr_engine_panel.hpp)
        if (s && s->route && s->panel_lines && ImGui::CollapsingHeader("Engine data")) {
            nr::control_ui::engine_legend();
            for (uint32_t i = 0; i < std::min(s->panel_lines, nr::control::kPanelLines); ++i) {
                char line[sizeof s->panel[0] + 1];
                std::snprintf(line, sizeof line, "%s", s->panel[i]);
                nr::control_ui::engine_line(line);
            }
        }
        // [Preprocess]
        if (ImGui::CollapsingHeader("Preprocess")) {
            bool on = ini.get("Preprocess", "Enabled", "0") == "1";
            if (ImGui::Checkbox("Preprocess##on", &on)) ini.set("Preprocess", "Enabled", on ? "1" : "0");
            nr::control_ui::help("[Preprocess] Enabled. Changes the picture the network is shown.");
            static const char* const exposures[] = {"off", "auto", "fixed"};
            static const char* const curves[] = {"none", "neutral", "reinhard", "filmic", "gt", "aces", "agx"};
            auto combo = [&](const char* label, const char* key, const char* const* items, int n, const char* help) {
                const std::string v = ini.get("Preprocess", key, items[0]);
                int i = 0;
                for (int k = 0; k < n; ++k) if (v == items[k]) i = k;
                if (ImGui::Combo(label, &i, items, n)) ini.set("Preprocess", key, items[i]);
                nr::control_ui::help(help);
            };
            auto slider = [&](const char* label, const char* key, float lo, float hi, float def, const char* help) {
                static std::unordered_map<std::string, float> pending;
                const auto it = pending.find(key);
                float v = it != pending.end() ? it->second : std::strtof(ini.get("Preprocess", key, "").c_str(), nullptr);
                if (it == pending.end() && ini.get("Preprocess", key, "").empty()) v = def;
                if (ImGui::SliderFloat(label, &v, lo, hi, "%.2f", ImGuiSliderFlags_AlwaysClamp)) pending[key] = v;
                if (ImGui::IsItemDeactivatedAfterEdit() && pending.count(key)) {
                    char t[32];
                    std::snprintf(t, sizeof t, "%.2f", double(pending[key]));
                    ini.set("Preprocess", key, t);
                    pending.erase(key);
                }
                nr::control_ui::help(help);
            };
            combo("Exposure", "Exposure", exposures, 3, "[Preprocess] Exposure. auto: Unreal-style auto exposure; fixed: Exposure bias only.");
            slider("Exposure bias (EV)", "ExposureBias", -8.f, 8.f, 0.f, "[Preprocess] ExposureBias.");
            combo("Curve", "Curve", curves, 7, "[Preprocess] Curve. The display curve of the picture NR is shown.");
            slider("Contrast", "Contrast", 0.5f, 2.f, 1.f, "[Preprocess] Contrast.");
            slider("Saturation", "Saturation", 0.05f, 2.f, 1.f, "[Preprocess] Saturation.");
            hotkey_text("Hotkey##prep", "Preprocess", "[Preprocess] Hotkey: switches the whole preprocess on or off for "
                        "this run (not written to the file). Ctrl, Shift, Alt and one key; empty: no hotkey.");
            flag("Sound##prep", "Preprocess", "Sound", "1",
                 "[Preprocess] Sound. A cue when it switches: two notes going up = on, going down = off.");
        }
        // [Log]: read once when the game starts
        if (ImGui::CollapsingHeader("Log")) {
            flag("Write the log", "Log", "Enabled", "1",
                 "[Log] Enabled. dlssnr-amd.log in the game's folder. Takes effect after a restart.");
            flag("Clear on start", "Log", "ClearOnStart", "1",
                 "[Log] ClearOnStart. On: each start of the game begins a new log; off: appends to the last one. "
                 "Takes effect after a restart.");
        }
        ImGui::Separator();
        ImGui::TextDisabled("End: close. Changes are saved to dlssnr-amd.ini at once.");
    }

    void record(VkCommandBuffer cmd, uint32_t index) {
        std::lock_guard<std::mutex> lock(imgui_mutex);
        ImGui::SetCurrentContext(context);
        VkClearValue clear{};
        VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rb.renderPass = draw_pass; rb.framebuffer = menu_frame; rb.renderArea = {{0, 0}, extent};
        rb.clearValueCount = 1; rb.pClearValues = &clear;
        vk.CmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        vk.CmdEndRenderPass(cmd);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.image = images[index];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                              nullptr, 0, nullptr, 1, &b);
        rb.renderPass = compose_pass; rb.framebuffer = frames[index];
        rb.clearValueCount = 0; rb.pClearValues = nullptr;
        vk.CmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
        vk.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose);
        vk.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        const struct { uint32_t mode; float scale; } push{encoding.mode, encoding.scale};
        vk.CmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
        vk.CmdDraw(cmd, 3, 1, 0, 0);
        vk.CmdEndRenderPass(cmd);
        b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; b.dstAccessMask = 0;
        vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                              nullptr, 0, nullptr, 1, &b);
    }
};

// ---- the layer's objects ----
struct Instance {
    VkInstance handle{};
    PFN_vkGetInstanceProcAddr next{};
    template <class T> T fn(const char* s) { return reinterpret_cast<T>(next(handle, s)); }
};
struct Frame { VkCommandBuffer cmd{}; VkFence fence{}; VkSemaphore done{}; };
// NR_CONTROL_DUMP=<file>: the first image the menu is drawn into (from NR_CONTROL_AT), written as a PPM (a test without a
// display).
struct Dump {
    std::string path;
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    VkFence fence{};
    bool pending = false, done = false;
    VkExtent2D extent{};
    VkFormat format{};
    VkColorSpaceKHR space{};
};
Dump g_dump{[] { const char* e = std::getenv("NR_CONTROL_DUMP"); return std::string(e ? e : ""); }()};
struct Swap {
    VkSwapchainKHR handle{};
    VkExtent2D extent{};
    VkFormat format{};
    VkColorSpaceKHR space{};
    bool drawable = false;   // the images take colour-attachment use
    std::shared_ptr<Input> input;
    VkQueue queue{};
    VkCommandPool pool{};
    std::vector<Frame> frames;
    std::unique_ptr<Overlay> overlay;
    bool failed = false;
};
struct Device {
    VkDevice handle{};
    VkPhysicalDevice physical{};
    std::shared_ptr<Instance> instance;
    PFN_vkGetDeviceProcAddr next{};
    PFN_vkSetDeviceLoaderData set_loader_data{};   // for the command buffers the menu allocates
    Vk vk;
    bool usable = false;
    std::vector<VkQueueFamilyProperties> families;
    std::mutex mutex;
    std::unordered_map<VkSwapchainKHR, std::unique_ptr<Swap>> swaps;
    template <class T> T fn(const char* s) { return reinterpret_cast<T>(next(handle, s)); }
};
struct Queue { std::shared_ptr<Device> device; uint32_t family{}; };

std::mutex objects;
std::unordered_map<VkInstance, std::shared_ptr<Instance>> instances;
std::unordered_map<VkPhysicalDevice, std::shared_ptr<Instance>> physicals;
std::unordered_map<VkDevice, std::shared_ptr<Device>> devices;
std::unordered_map<VkQueue, Queue> queues;
std::unordered_map<VkSurfaceKHR, std::shared_ptr<Input>> surfaces;

std::shared_ptr<Instance> find_instance(VkInstance h) {
    std::lock_guard<std::mutex> lock(objects);
    auto it = instances.find(h);
    return it == instances.end() ? nullptr : it->second;
}
std::shared_ptr<Device> find_device(VkDevice h) {
    std::lock_guard<std::mutex> lock(objects);
    auto it = devices.find(h);
    return it == devices.end() ? nullptr : it->second;
}

void retire(Device& d, Swap& s) {
    for (auto& f : s.frames)
        if (f.fence) d.vk.WaitForFences(d.handle, 1, &f.fence, VK_TRUE, UINT64_MAX);
    s.overlay.reset();
    for (auto& f : s.frames) {
        if (f.fence) d.vk.DestroyFence(d.handle, f.fence, nullptr);
        if (f.done) d.vk.DestroySemaphore(d.handle, f.done, nullptr);
    }
    if (s.pool) d.vk.DestroyCommandPool(d.handle, s.pool, nullptr);
    s.pool = {};
    s.frames.clear();
}

void initialize(Device& d, Swap& s, VkQueue q, uint32_t family) {
    uint32_t count = 0;
    auto get = d.fn<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR");
    check(get(d.handle, s.handle, &count, nullptr), "image count");
    std::vector<VkImage> images(count);
    check(get(d.handle, s.handle, &count, images.data()), "images");
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.queueFamilyIndex = family;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    check(d.vk.CreateCommandPool(d.handle, &pool, nullptr, &s.pool), "command pool");
    s.frames.resize(count);
    for (auto& f : s.frames) {
        VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        a.commandPool = s.pool; a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; a.commandBufferCount = 1;
        check(d.vk.AllocateCommandBuffers(d.handle, &a, &f.cmd), "command buffer");
        if (d.set_loader_data) check(d.set_loader_data(d.handle, f.cmd), "command buffer loader data");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        check(d.vk.CreateFence(d.handle, &fi, nullptr, &f.fence), "fence");
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(d.vk.CreateSemaphore(d.handle, &si, nullptr, &f.done), "semaphore");
    }
    s.queue = q;
    s.overlay = std::make_unique<Overlay>(d.vk, d.handle);
    s.overlay->init(d.instance->handle, d.instance->next, d.physical, d.next, q, family, s.format, s.space, s.extent,
                    std::move(images));
}

}  // namespace

extern "C" {
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nr_control_GetInstanceProcAddr(VkInstance, const char*);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nr_control_GetDeviceProcAddr(VkDevice, const char*);
}

namespace {
VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out) {
    auto* link = loader_link<VkLayerInstanceCreateInfo>(ci->pNext, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO, VK_LAYER_LINK_INFO);
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    auto next = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto create = reinterpret_cast<PFN_vkCreateInstance>(next(VK_NULL_HANDLE, "vkCreateInstance"));
    const VkResult r = create(ci, a, out);
    if (r != VK_SUCCESS) return r;
    auto inst = std::make_shared<Instance>();
    inst->handle = *out; inst->next = next;
    uint32_t n = 0;
    auto enumerate = inst->fn<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
    enumerate(*out, &n, nullptr);
    std::vector<VkPhysicalDevice> phys(n);
    enumerate(*out, &n, phys.data());
    std::lock_guard<std::mutex> lock(objects);
    instances[*out] = inst;
    for (auto p : phys) physicals[p] = inst;
    return r;
}
VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance h, const VkAllocationCallbacks* a) {
    auto inst = find_instance(h);
    if (!inst) return;
    {
        std::lock_guard<std::mutex> lock(objects);
        for (auto it = physicals.begin(); it != physicals.end();) it = it->second == inst ? physicals.erase(it) : std::next(it);
        instances.erase(h);
    }
    inst->fn<PFN_vkDestroyInstance>("vkDestroyInstance")(h, a);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice p, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* out) {
    auto* link = loader_link<VkLayerDeviceCreateInfo>(ci->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, VK_LAYER_LINK_INFO);
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    auto gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    auto gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto* data = loader_link<VkLayerDeviceCreateInfo>(ci->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, VK_LOADER_DATA_CALLBACK);
    std::shared_ptr<Instance> inst;
    {
        std::lock_guard<std::mutex> lock(objects);
        auto it = physicals.find(p);
        if (it != physicals.end()) inst = it->second;
    }
    auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(inst ? inst->handle : VK_NULL_HANDLE, "vkCreateDevice"));
    const VkResult r = create(p, ci, a, out);
    if (r != VK_SUCCESS || !inst) return r;
    auto d = std::make_shared<Device>();
    d->handle = *out; d->physical = p; d->instance = inst; d->next = gdpa;
    if (data) d->set_loader_data = data->u.pfnSetDeviceLoaderData;
    d->usable = d->vk.load(*out, gdpa, inst->handle, inst->next);
    if (!d->usable) say("a Vulkan function is missing below the layer: no menu on this device");
    uint32_t n = 0;
    auto props = inst->fn<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
    props(p, &n, nullptr);
    d->families.resize(n);
    props(p, &n, d->families.data());
    std::lock_guard<std::mutex> lock(objects);
    devices[*out] = d;
    return r;
}
VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice h, const VkAllocationCallbacks* a) {
    auto d = find_device(h);
    if (!d) return;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        for (auto& [k, s] : d->swaps) retire(*d, *s);
        d->swaps.clear();
    }
    {
        std::lock_guard<std::mutex> lock(objects);
        for (auto it = queues.begin(); it != queues.end();) it = it->second.device == d ? queues.erase(it) : std::next(it);
        devices.erase(h);
    }
    d->fn<PFN_vkDestroyDevice>("vkDestroyDevice")(h, a);
}
void note_queue(VkDevice h, uint32_t family, VkQueue q) {
    auto d = find_device(h);
    if (!d || !q) return;
    std::lock_guard<std::mutex> lock(objects);
    queues[q] = {d, family};
}
VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice h, uint32_t family, uint32_t index, VkQueue* q) {
    auto d = find_device(h);
    if (!d) return;
    d->fn<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(h, family, index, q);
    note_queue(h, family, *q);
}
VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(VkDevice h, const VkDeviceQueueInfo2* info, VkQueue* q) {
    auto d = find_device(h);
    if (!d) return;
    d->fn<PFN_vkGetDeviceQueue2>("vkGetDeviceQueue2")(h, info, q);
    note_queue(h, info->queueFamilyIndex, *q);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateXlibSurfaceKHR(VkInstance h, const VkXlibSurfaceCreateInfoKHR* ci,
                                                    const VkAllocationCallbacks* a, VkSurfaceKHR* out) {
    auto inst = find_instance(h);
    if (!inst) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult r = inst->fn<PFN_vkCreateXlibSurfaceKHR>("vkCreateXlibSurfaceKHR")(h, ci, a, out);
    if (r == VK_SUCCESS) try {
        auto in = std::make_shared<X11Input>(std::make_shared<nr::MenuInput>(XDisplayString(ci->dpy), uint32_t(ci->window)));
        std::lock_guard<std::mutex> lock(objects);
        surfaces[*out] = in;
    } catch (const std::exception& e) { say(std::string("X11 input: ") + e.what()); }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL CreateXcbSurfaceKHR(VkInstance h, const VkXcbSurfaceCreateInfoKHR* ci,
                                                   const VkAllocationCallbacks* a, VkSurfaceKHR* out) {
    auto inst = find_instance(h);
    if (!inst) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult r = inst->fn<PFN_vkCreateXcbSurfaceKHR>("vkCreateXcbSurfaceKHR")(h, ci, a, out);
    if (r == VK_SUCCESS) try {
        const char* display = std::getenv("DISPLAY");
        auto in = std::make_shared<X11Input>(std::make_shared<nr::MenuInput>(display ? display : "", ci->window));
        std::lock_guard<std::mutex> lock(objects);
        surfaces[*out] = in;
    } catch (const std::exception& e) { say(std::string("XCB input: ") + e.what()); }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL CreateWaylandSurfaceKHR(VkInstance h, const VkWaylandSurfaceCreateInfoKHR* ci,
                                                       const VkAllocationCallbacks* a, VkSurfaceKHR* out) {
    auto inst = find_instance(h);
    if (!inst) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult r = inst->fn<PFN_vkCreateWaylandSurfaceKHR>("vkCreateWaylandSurfaceKHR")(h, ci, a, out);
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(objects);
        surfaces[*out] = std::make_shared<WaylandInput>();
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL DestroySurfaceKHR(VkInstance h, VkSurfaceKHR surface, const VkAllocationCallbacks* a) {
    auto inst = find_instance(h);
    if (!inst) return;
    {
        std::lock_guard<std::mutex> lock(objects);
        surfaces.erase(surface);
    }
    inst->fn<PFN_vkDestroySurfaceKHR>("vkDestroySurfaceKHR")(h, surface, a);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice h, const VkSwapchainCreateInfoKHR* ci,
                                                  const VkAllocationCallbacks* a, VkSwapchainKHR* out) {
    auto d = find_device(h);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    auto s = std::make_unique<Swap>();
    s->extent = ci->imageExtent;
    s->format = ci->imageFormat;
    s->space = ci->imageColorSpace;
    {
        std::lock_guard<std::mutex> lock(objects);
        auto it = surfaces.find(ci->surface);
        if (it != surfaces.end()) s->input = it->second;
    }
    VkSwapchainCreateInfoKHR patched = *ci;
    VkSurfaceCapabilitiesKHR caps{};
    auto get_caps = d->instance->fn<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    if (s->input && ci->imageArrayLayers == 1 && get_caps && get_caps(d->physical, ci->surface, &caps) == VK_SUCCESS &&
        (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        patched.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        s->drawable = true;
        if (!g_dump.path.empty() && (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
            patched.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    const VkResult r = d->fn<PFN_vkCreateSwapchainKHR>("vkCreateSwapchainKHR")(h, &patched, a, out);
    if (r == VK_SUCCESS) {
        s->handle = *out;
        std::lock_guard<std::mutex> lock(d->mutex);
        d->swaps[*out] = std::move(s);
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(VkDevice h, VkSwapchainKHR swap, const VkAllocationCallbacks* a) {
    auto d = find_device(h);
    if (!d) return;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        auto it = d->swaps.find(swap);
        if (it != d->swaps.end()) { retire(*d, *it->second); d->swaps.erase(it); }
    }
    d->fn<PFN_vkDestroySwapchainKHR>("vkDestroySwapchainKHR")(h, swap, a);
}
void dump_record(Device& d, Swap& s, VkCommandBuffer cmd, uint32_t index) {
    const VkDeviceSize bytes = VkDeviceSize(s.extent.width) * s.extent.height * (s.format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = bytes; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (d.vk.CreateBuffer(d.handle, &bi, nullptr, &g_dump.buffer) != VK_SUCCESS) { g_dump.done = true; return; }
    VkMemoryRequirements req{}; d.vk.GetBufferMemoryRequirements(d.handle, g_dump.buffer, &req);
    VkPhysicalDeviceMemoryProperties mem{}; d.vk.GetPhysicalDeviceMemoryProperties(d.physical, &mem);
    uint32_t type = mem.memoryTypeCount;
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            type = i; break;
        }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size; ai.memoryTypeIndex = type;
    if (type == mem.memoryTypeCount || d.vk.AllocateMemory(d.handle, &ai, nullptr, &g_dump.memory) != VK_SUCCESS) {
        g_dump.done = true;
        return;
    }
    d.vk.BindBufferMemory(d.handle, g_dump.buffer, g_dump.memory, 0);
    VkImage image = s.overlay->images[index];
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.image = image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    d.vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {s.extent.width, s.extent.height, 1};
    d.vk.CmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_dump.buffer, 1, &region);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b.dstAccessMask = 0;
    d.vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    g_dump.extent = s.extent; g_dump.format = s.format; g_dump.space = s.space; g_dump.pending = true;
}
// One pixel of the dumped image as 8-bit sRGB for a look: HDR is shown with 203 nits as white (clipped above).
void dump_pixel(const uint8_t* px, uint8_t rgb[3]) {
    float c[3];
    auto oetf = [](float v) {
        v = std::clamp(v, 0.f, 1.f);
        return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
    };
    auto half = [](uint16_t h) {
        const uint32_t e = (h >> 10) & 31u, m = h & 1023u;
        const float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? 65504.f : std::ldexp(float(m | 1024u), int(e) - 25);
        return (h & 0x8000u) ? -v : v;
    };
    auto pq_nits = [](float e) {
        const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
        const float p = std::pow(std::max(e, 0.f), 1.f / m2);
        return 10000.f * std::pow(std::max(p - c1, 0.f) / (c2 - c3 * p), 1.f / m1);
    };
    switch (g_dump.format) {
        case VK_FORMAT_R16G16B16A16_SFLOAT: {
            uint16_t h[3];
            std::memcpy(h, px, sizeof h);
            const bool linear = g_dump.space == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
            for (int i = 0; i < 3; ++i) c[i] = linear ? oetf(half(h[i]) * 80.f / 203.f) : std::clamp(half(h[i]), 0.f, 1.f);
            break;
        }
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32: {
            uint32_t w;
            std::memcpy(&w, px, 4);
            const bool bgr = g_dump.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
            const float v[3] = {float(w & 1023u) / 1023.f, float((w >> 10) & 1023u) / 1023.f, float((w >> 20) & 1023u) / 1023.f};
            for (int i = 0; i < 3; ++i) {
                const float e = v[bgr ? 2 - i : i];
                c[i] = g_dump.space == VK_COLOR_SPACE_HDR10_ST2084_EXT ? oetf(pq_nits(e) / 203.f) : e;   // BT.2020 primaries kept
            }
            break;
        }
        default: {
            const bool bgra = g_dump.format == VK_FORMAT_B8G8R8A8_UNORM || g_dump.format == VK_FORMAT_B8G8R8A8_SRGB;
            for (int i = 0; i < 3; ++i) c[i] = px[bgra ? 2 - i : i] / 255.f;
        }
    }
    for (int i = 0; i < 3; ++i) rgb[i] = uint8_t(std::lround(std::clamp(c[i], 0.f, 1.f) * 255.f));
}
void dump_write(Device& d) {
    void* p = nullptr;
    if (d.vk.MapMemory(d.handle, g_dump.memory, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS) {
        if (FILE* f = std::fopen(g_dump.path.c_str(), "wb")) {
            std::fprintf(f, "P6\n%u %u\n255\n", g_dump.extent.width, g_dump.extent.height);
            const auto* px = static_cast<const uint8_t*>(p);
            const size_t step = g_dump.format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
            for (size_t i = 0; i < size_t(g_dump.extent.width) * g_dump.extent.height; ++i) {
                uint8_t rgb[3];
                dump_pixel(px + i * step, rgb);
                std::fwrite(rgb, 1, 3, f);
            }
            std::fclose(f);
            say("drawn frame written to " + g_dump.path + " (format " + std::to_string(int(g_dump.format)) + ", colour space " +
                std::to_string(int(g_dump.space)) + ")");
        }
        d.vk.UnmapMemory(d.handle, g_dump.memory);
    }
    d.vk.DestroyBuffer(d.handle, g_dump.buffer, nullptr);
    d.vk.FreeMemory(d.handle, g_dump.memory, nullptr);
    d.vk.DestroyFence(d.handle, g_dump.fence, nullptr);
    g_dump.buffer = {}; g_dump.memory = {}; g_dump.fence = {};
    g_dump.done = true;
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue q, const VkPresentInfoKHR* info) {
    Queue queue{};
    {
        std::lock_guard<std::mutex> lock(objects);
        auto it = queues.find(q);
        if (it != queues.end()) queue = it->second;
    }
    auto d = queue.device;
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    auto present = d->fn<PFN_vkQueuePresentKHR>("vkQueuePresentKHR");
    std::lock_guard<std::mutex> lock(d->mutex);
    const bool graphics = queue.family < d->families.size() && (d->families[queue.family].queueFlags & VK_QUEUE_GRAPHICS_BIT);
    std::vector<VkCommandBuffer> commands;
    std::vector<VkSemaphore> signals;
    std::vector<Frame*> used;
    for (uint32_t i = 0; i < info->swapchainCount; ++i) {
        auto it = d->swaps.find(info->pSwapchains[i]);
        if (it == d->swaps.end()) continue;
        Swap& s = *it->second;
        if (!s.input || !s.drawable || s.failed || !graphics || !d->usable) continue;
        try {
            if (!s.pool) initialize(*d, s, q, queue.family);
            if (s.queue != q) continue;   // the menu stays on the queue it was made on
            if (!s.overlay->frame(*s.input)) continue;
            Frame& f = s.frames[info->pImageIndices[i]];
            check(d->vk.WaitForFences(d->handle, 1, &f.fence, VK_TRUE, UINT64_MAX), "fence");
            check(d->vk.ResetCommandBuffer(f.cmd, 0), "command reset");
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check(d->vk.BeginCommandBuffer(f.cmd, &begin), "command begin");
            s.overlay->record(f.cmd, info->pImageIndices[i]);
            ++g_drawn;
            if (!g_dump.path.empty() && !g_dump.done && !g_dump.pending && g_drawn > control_at()) dump_record(*d, s, f.cmd, info->pImageIndices[i]);
            check(d->vk.EndCommandBuffer(f.cmd), "command end");
            commands.push_back(f.cmd);
            signals.push_back(f.done);
            used.push_back(&f);
        } catch (const std::exception& e) {
            s.failed = true;
            say(std::string("the menu is off for this swap chain: ") + e.what());
        }
    }
    if (g_dump.pending && !g_dump.done && g_dump.fence && d->vk.GetFenceStatus(d->handle, g_dump.fence) == VK_SUCCESS) dump_write(*d);
    if (commands.empty()) return present(q, info);
    std::vector<VkPipelineStageFlags> stages(info->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = info->waitSemaphoreCount;
    submit.pWaitSemaphores = info->pWaitSemaphores;
    submit.pWaitDstStageMask = stages.data();
    submit.commandBufferCount = uint32_t(commands.size());
    submit.pCommandBuffers = commands.data();
    submit.signalSemaphoreCount = uint32_t(signals.size());
    submit.pSignalSemaphores = signals.data();
    auto submit_fn = d->fn<PFN_vkQueueSubmit>("vkQueueSubmit");
    // One submit for the menus, then an empty one per image to fence its command buffer.
    for (Frame* f : used) d->vk.ResetFences(d->handle, 1, &f->fence);
    VkResult r = submit_fn(q, 1, &submit, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) return r;
    for (Frame* f : used)
        if ((r = submit_fn(q, 0, nullptr, f->fence)) != VK_SUCCESS) return r;
    if (g_dump.pending && !g_dump.fence) {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        d->vk.CreateFence(d->handle, &fi, nullptr, &g_dump.fence);
        submit_fn(q, 0, nullptr, g_dump.fence);
    }
    VkPresentInfoKHR patched = *info;
    patched.waitSemaphoreCount = uint32_t(signals.size());
    patched.pWaitSemaphores = signals.data();
    return present(q, &patched);
}
}  // namespace

extern "C" {
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nr_control_GetDeviceProcAddr(VkDevice h, const char* name) {
#define OURS(n) if (std::strcmp(name, "vk" #n) == 0) return reinterpret_cast<PFN_vkVoidFunction>(&n)
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&nr_control_GetDeviceProcAddr);
    OURS(DestroyDevice); OURS(GetDeviceQueue); OURS(GetDeviceQueue2); OURS(CreateSwapchainKHR); OURS(DestroySwapchainKHR);
    OURS(QueuePresentKHR);
    auto d = find_device(h);
    return d ? d->next(h, name) : nullptr;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nr_control_GetInstanceProcAddr(VkInstance h, const char* name) {
    if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&nr_control_GetInstanceProcAddr);
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&nr_control_GetDeviceProcAddr);
    OURS(CreateInstance); OURS(DestroyInstance); OURS(CreateDevice); OURS(DestroyDevice); OURS(GetDeviceQueue);
    OURS(GetDeviceQueue2); OURS(CreateXlibSurfaceKHR); OURS(CreateXcbSurfaceKHR); OURS(CreateWaylandSurfaceKHR);
    OURS(DestroySurfaceKHR); OURS(CreateSwapchainKHR); OURS(DestroySwapchainKHR); OURS(QueuePresentKHR);
#undef OURS
    auto inst = find_instance(h);
    return inst ? inst->next(h, name) : nullptr;
}
}
