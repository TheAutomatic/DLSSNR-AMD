// Offline D3D12 benchmark and check of the D3D12 passthrough against the Vulkan bridge, with no game:
// CC_SILVER_AARDWOLD_OPTIMIZED (layers: ultra performance), a 3840x2160 RenoDX Present stage.
// Put it in a game folder where the OptiScaler route is installed (beside dlssnr-amd.ini and dlssnr-amd\) and run
//   test_d3d12_offline.exe [frames] [--vulkan] [--hdr] [--model NAME] [--layers LEVEL] [--dump FILE]
//   test_d3d12_offline.exe --compare A B
// once without and once with --vulkan (SDR; again with --hdr for HDR10): the two runs print the GPU times and the
// output's statistics. --model / --layers pick the network on the bridge (DLSSNR_MODEL / DLSSNR_INT4: a codename;
// 0 quality .. 3 ultra performance), written to [Network] Model / OptimizedLayers of the folder's dlssnr-amd.ini as NR
// Control would (the session's choice wins over the environment) - an RX 7000 / Radeon 700M-8000S runs its own ACO
// records that way; --dump
// writes the first frame's output (3840x2160 R10G10B10A2, rows packed), --compare prints the PSNR of two dumps (the
// int4 levels against Quality: a level whose output is far from Quality's is broken, not just less precise).
// Built against the plugin's objects (windows/build/build_asi.sh's, all but asi_main.o).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include "nr_d3d12_passthrough.hpp"
#include "nr_d3d12_passthrough_backend.hpp"
#include "nr_d3d12_passthrough_net.hpp"
#include "nr_d3d12_net_plan.hpp"
#include "nr_dlssnr_model.hpp"
#include "nr_ini_int4.hpp"
#include "nr_ini_network.hpp"
#include "nr_pe_config.hpp"
#include "nr_pe_d3d12split.hpp"
#include "nr_pe_log.hpp"
#include "nr_pe_renodx_route.hpp"
#include "nr_pe_session.hpp"
#include "nr_renodx.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_model_choice = nr::kSilverAardwoldOptimizedUltra;
static int query_model() { return g_model_choice; }

static ID3D12Resource* make_tex2d(ID3D12Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool uav,
                                  D3D12_RESOURCE_STATES init_state) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* res = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, init_state, nullptr, IID_PPV_ARGS(&res))))
        return nullptr;
    return res;
}

static ID3D12Resource* make_buf(ID3D12Device* dev, D3D12_HEAP_TYPE type, uint64_t bytes, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = type;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* res = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&res))))
        return nullptr;
    return res;
}

// One [Network] key of dlssnr-amd.ini, everything else as it was (as NR Control's NetworkSwitch::write_key).
static void write_network_key(const std::string& ini, const std::string& key, const std::string& value) {
    std::string text;
    if (FILE* f = std::fopen(ini.c_str(), "rb")) {
        char b[4096];
        size_t n;
        while ((n = std::fread(b, 1, sizeof b, f)) > 0) text.append(b, n);
        std::fclose(f);
    }
    auto low = [](std::string x) { for (auto& c : x) c = char(std::tolower(static_cast<unsigned char>(c))); return x; };
    auto trim = [](const std::string& x) {
        const size_t a = x.find_first_not_of(" \t\r\n"), b = x.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : x.substr(a, b - a + 1);
    };
    std::string out, section;
    bool done = false;
    for (size_t at = 0; at < text.size();) {
        const size_t eol = text.find('\n', at);
        const std::string line = text.substr(at, eol == std::string::npos ? std::string::npos : eol - at + 1);
        at = eol == std::string::npos ? text.size() : eol + 1;
        const std::string t = trim(line.substr(0, line.find_first_of(";#")));
        if (!t.empty() && t.front() == '[') {
            if (!done && low(section) == "network") { out += key + " = " + value + "\r\n"; done = true; }
            section = trim(t.substr(1, t.find(']') - 1));
        } else if (!done && low(section) == "network" && t.find('=') != std::string::npos &&
                   low(trim(t.substr(0, t.find('=')))) == low(key)) {
            out += key + " = " + value + "\r\n";
            done = true;
            continue;
        }
        out += line;
    }
    if (!done) out += "\r\n[Network]\r\n" + key + " = " + value + "\r\n";
    if (FILE* f = std::fopen(ini.c_str(), "wb")) { std::fwrite(out.data(), 1, out.size(), f); std::fclose(f); }
    std::printf("dlssnr-amd.ini: [Network] %s = %s\n", key.c_str(), value.c_str());
}

// --compare: PSNR (10-bit RGB) and the largest difference of two --dump files
static int compare_dumps(const char* a, const char* b) {
    auto load = [](const char* p, std::vector<uint32_t>* v) {
        FILE* f = std::fopen(p, "rb");
        if (!f) return false;
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        v->resize(size_t(n > 0 ? n : 0) / 4);
        const bool ok = n > 0 && std::fread(v->data(), 4, v->size(), f) == v->size();
        std::fclose(f);
        return ok;
    };
    std::vector<uint32_t> x, y;
    if (!load(a, &x) || !load(b, &y) || x.size() != y.size()) { std::printf("FAIL: cannot read both dumps (same size)\n"); return 1; }
    double se = 0;
    int worst = 0;
    for (size_t i = 0; i < x.size(); ++i)
        for (int c = 0; c < 3; ++c) {
            const int d = int((x[i] >> (10 * c)) & 1023u) - int((y[i] >> (10 * c)) & 1023u);
            se += double(d) * d;
            worst = std::max(worst, std::abs(d));
        }
    const double mse = se / (3.0 * double(x.size()));
    std::printf("%s vs %s: PSNR %.2f dB, largest difference %d / 1023\n", a, b,
                mse > 0 ? 10.0 * std::log10(1023.0 * 1023.0 / mse) : 999.0, worst);
    return 0;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 4 && std::strcmp(argv[1], "--compare") == 0) return compare_dumps(argv[2], argv[3]);
    const uint32_t W = 3840, H = 2160;
    const char* dump = nullptr;
    const char *model_arg = nullptr, *layers_arg = nullptr;
    int adapter_arg = -1;
    const int total_frames = argc > 1 && argv[1][0] != '-' ? std::atoi(argv[1]) : 60;
    bool vulkan_bridge = false;
    bool hdr_mode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--vulkan") == 0) vulkan_bridge = true;
        if (std::strcmp(argv[i], "--hdr") == 0) hdr_mode = true;
        if (std::strcmp(argv[i], "--adapter") == 0 && i + 1 < argc) adapter_arg = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--model") == 0 && i + 1 < argc) model_arg = argv[++i];
        else if (std::strcmp(argv[i], "--layers") == 0 && i + 1 < argc) layers_arg = argv[++i];
        else if (std::strcmp(argv[i], "--dump") == 0 && i + 1 < argc) dump = argv[++i];
    }
    if (model_arg) {
        const int mv = nr::network_value(model_arg);
        const int lv = layers_arg ? nr::optimized_layers_value(layers_arg) : nr::kLayersDefault;
        g_model_choice = mv == 1 ? nr::kSmallFp8 : mv == 2 ? nr::kSmallFp8Experimental
                       : mv == 3 ? nr::network_with_layers(nr::kSilverAardwoldOptimized, lv >= 0 ? lv : nr::kLayersQuality)
                       : nr::kSilverAardwold;
    }

    std::printf("=== Offline D3D12 %s Test (%s) ===\n", vulkan_bridge ? "Vulkan Bridge" : "Direct ELF",
                hdr_mode ? "HDR v6" : "SDR legacy");
    std::printf("Target: CC_SILVER_AARDWOLD_OPTIMIZED (ultra performance), %ux%u RenoDX Present stage\n", W, H);

    HMODULE sys_dxgi = LoadLibraryW(L"C:\\Windows\\System32\\dxgi.dll");
    using PFN_CreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);
    auto fn_CreateDXGIFactory1 = sys_dxgi
        ? reinterpret_cast<PFN_CreateDXGIFactory1>(reinterpret_cast<void*>(GetProcAddress(sys_dxgi, "CreateDXGIFactory1")))
        : nullptr;

    HMODULE self_exe = GetModuleHandleW(nullptr);
    nr::pe::set_module(self_exe);   // the game folder: this program sits in it
    {
        const std::string ini = std::string(nr::pe::module_folder()) + "\\dlssnr-amd.ini";
        if (model_arg) write_network_key(ini, "Model", model_arg);
        if (layers_arg) write_network_key(ini, "OptimizedLayers", layers_arg);
    }
    std::printf("module_folder(): %s\n", nr::pe::module_folder());
    nr::pe::configure_network(nr::pe::module_folder());
    nr::dlssnr::keep_caller_state();

    if (!vulkan_bridge) {
        nr::pe::d3d12_passthrough::start();
        if (!nr::pe::d3d12_passthrough::enabled()) {
            std::printf("FAIL: nr::pe::d3d12_passthrough::enabled() is false\n");
            return 1;
        }
        nr::pe::d3d12_passthrough::set_net_planner(
            [](uint32_t w, uint32_t h, int model, bool experimental, nr::D3d12NetPlan* out, std::string* why) {
                return nr::plan_d3d12_network(nr::pe::module_folder(), w, h, experimental, out, why, model,
                                              nr::pe::d3d12_passthrough::rdna3());
            },
            &nr::d3d12_net_controls, &query_model);
        nr::dlssnr::set_d3d12_backend(&nr::pe::d3d12_passthrough::evaluate_backend);
        nr::pe::set_d3d12_native(true);
    }

    IDXGIFactory4* factory = nullptr;
    if (!fn_CreateDXGIFactory1 || FAILED(fn_CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        std::printf("FAIL: CreateDXGIFactory1\n");
        return 1;
    }
    // The AMD adapter with the most dedicated video memory (an integrated GPU often enumerates first)
    IDXGIAdapter1* adapter = nullptr;
    {
        SIZE_T best = 0;
        IDXGIAdapter1* a = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            a->GetDesc1(&desc);
            std::wprintf(L"Seen Adapter [%u]: %ls (Vendor 0x%04x, Device 0x%04x, VRAM: %llu MB)\n", i, desc.Description,
                         desc.VendorId, desc.DeviceId, (unsigned long long)(desc.DedicatedVideoMemory / (1024 * 1024)));
            const bool pick = adapter_arg >= 0 ? (int(i) == adapter_arg)
                            : (desc.VendorId == 0x1002 && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && desc.DedicatedVideoMemory > best);
            if (pick) {
                if (adapter) adapter->Release();
                adapter = a;
                best = desc.DedicatedVideoMemory;
                std::wprintf(L"Adapter [%u]: %ls (VRAM: %llu MB)\n", i, desc.Description,
                             (unsigned long long)(desc.DedicatedVideoMemory / (1024 * 1024)));
            } else {
                a->Release();
            }
        }
    }
    if (!adapter) {
        std::printf("FAIL: No AMD adapter found\n");
        return 1;
    }

    ID3D12Device* device = nullptr;
    if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
        std::printf("FAIL: D3D12CreateDevice\n");
        return 1;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cmd = nullptr;
    ID3D12Fence* fence = nullptr;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cmd))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        std::printf("FAIL: queue/allocator/cmd/fence creation\n");
        return 1;
    }
    HANDLE fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    uint64_t fence_val = 0;
    auto wait_gpu = [&]() {
        queue->Signal(fence, ++fence_val);
        if (fence->GetCompletedValue() < fence_val) {
            fence->SetEventOnCompletion(fence_val, fence_event);
            WaitForSingleObject(fence_event, 10000);
        }
    };

    // Hook command list / queue class via split::prepare, then Close + Reset so state is valid
    std::string why;
    nr::pe::split::prepare(cmd, &why);
    cmd->Close();
    alloc->Reset();
    cmd->Reset(alloc, nullptr);
    if (!nr::pe::split::prepare(cmd, &why)) {
        std::printf("FAIL: split::prepare after Reset: %s\n", why.c_str());
        return 1;
    }

    if (!vulkan_bridge) {
        nr::pe::d3d12_passthrough::on_device(device);
        namespace pt = nr::pe::d3d12_passthrough;
        for (int i = 0; i < 100 && pt::canary() == pt::Canary::kRunning; ++i) Sleep(20);
        std::printf("Canary: %s (%s)\n", nr::pe::d3d12_passthrough::canary_name(nr::pe::d3d12_passthrough::canary()),
                    nr::pe::d3d12_passthrough::status().canary.c_str());
        if (nr::pe::d3d12_passthrough::canary() != nr::pe::d3d12_passthrough::Canary::kHit) {
            std::printf("FAIL: Canary did not hit!\n");
            return 1;
        }
    }

    ID3D12Resource* target = make_tex2d(device, W, H, DXGI_FORMAT_R10G10B10A2_UNORM, true, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* init_tex = make_tex2d(device, W, H, DXGI_FORMAT_R10G10B10A2_UNORM, false, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* motion = make_tex2d(device, W, H, DXGI_FORMAT_R16G16_FLOAT, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* depth  = make_tex2d(device, W, H, DXGI_FORMAT_R32_FLOAT, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!target || !init_tex || !motion || !depth) {
        std::printf("FAIL: Could not allocate 3840x2160 test textures\n");
        return 1;
    }

    D3D12_RESOURCE_DESC td = target->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 row_bytes = 0, total_bytes = 0;
    device->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &row_bytes, &total_bytes);
    ID3D12Resource* upload = make_buf(device, D3D12_HEAP_TYPE_UPLOAD, total_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource* readback = make_buf(device, D3D12_HEAP_TYPE_READBACK, total_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<uint32_t> init_pixels(W * H);
    {
        uint8_t* mapped = nullptr;
        upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
        for (uint32_t y = 0; y < H; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(mapped + uint64_t(y) * fp.Footprint.RowPitch);
            for (uint32_t x = 0; x < W; ++x) {
                uint32_t r = ((x * 1023) / W) & 1023u;
                uint32_t g = ((y * 1023) / H) & 1023u;
                uint32_t b = (((x ^ y) * 4) + 256) & 1023u;
                uint32_t px = r | (g << 10) | (b << 20) | (3u << 30);
                row[x] = px;
                init_pixels[size_t(y) * W + x] = px;
            }
        }
        upload->Unmap(0, nullptr);
    }

    // Upload once into VRAM-resident `init_tex` before the benchmark loop
    {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = init_tex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = upload;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp;
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = init_tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cmd->ResourceBarrier(1, &b);
        cmd->Close();
        ID3D12CommandList* lists[] = {cmd};
        queue->ExecuteCommandLists(1, lists);
        wait_gpu();
        alloc->Reset();
        cmd->Reset(alloc, nullptr);
    }

    auto seed_target = [&](ID3D12GraphicsCommandList* l, D3D12_RESOURCE_STATES cur_state) {
        if (cur_state != D3D12_RESOURCE_STATE_COPY_DEST) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = target;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = cur_state;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            l->ResourceBarrier(1, &b);
        }
        l->CopyResource(target, init_tex);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = target;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        l->ResourceBarrier(1, &b);
    };

    int rc = 0;
    nr::dlssnr::Feature* feat = nr::dlssnr::create_d3d12(device, cmd, nullptr, W, H, 0, 1, nr::dlssnr::Controls6{}, &rc);
    if (!feat) {
        std::printf("FAIL: create_d3d12 failed (%d)\n", rc);
        return 1;
    }

    nr::renodx::Settings rs{};
    rs.nr_enabled = true;
    rs.intensity = 1.0f;
    rs.passes = 1;
    rs.look_mode = 0;
    rs.hook_point = 2;
    rs.color_strength = 1.0f;
    rs.transfer_strength = 1.0f;
    rs.detail_stability = 1;

    auto run_one_eval = [&](int reset) {
        nr::pe::renodx_route::Frame rf{};
        rf.known = true;
        rf.present = hdr_mode ? 3 : 1;
        rf.present_white_nits = 203.0f;
        nr::pe::renodx_route::begin_run(rf);
        nr::pe::renodx_route::note_encode(hdr_mode, W, H);
        nr::pe::renodx_route::set_direct(target);
        {
            nr::pe::renodx_route::CallStyle style(true);
            nr::pe::renodx_route::CallSettings settings(&rs);
            nr::pe::renodx_route::CallNetwork network(g_model_choice);
            nr::pe::renodx_route::CallLayer number(1);
            const nr::dlssnr::Rect whole{0, 0, W, H};
            nr::dlssnr::evaluate_d3d12(cmd, feat, nullptr, target, depth, motion, target, W, H, whole, whole, 1, reset,
                                       nr::dlssnr::Controls6{}, 1.0f, 1.0f);
        }
        nr::pe::renodx_route::end_run();
    };

    auto check_target_modified = [&]() -> double {
        alloc->Reset();
        cmd->Reset(alloc, nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = target;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cmd->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = target;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        cmd->ResourceBarrier(1, &b);
        cmd->Close();
        ID3D12CommandList* lists[] = {cmd};
        queue->ExecuteCommandLists(1, lists);
        wait_gpu();

        uint8_t* mapped = nullptr;
        readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
        uint64_t changed = 0;
        for (uint32_t y = 0; y < H; y += 16) {
            const auto* row = reinterpret_cast<const uint32_t*>(mapped + uint64_t(y) * fp.Footprint.RowPitch);
            for (uint32_t x = 0; x < W; x += 16) {
                if ((row[x] & 0x3fffffffu) != (init_pixels[size_t(y) * W + x] & 0x3fffffffu)) ++changed;
            }
        }
        readback->Unmap(0, nullptr);
        return double(changed);
    };

    // Trigger background build and wait until first frame produces NR output
    const auto t_build_start = std::chrono::steady_clock::now();
    bool built = false;
    for (int poll = 0; poll < 600; ++poll) {
        alloc->Reset();
        cmd->Reset(alloc, nullptr);
        seed_target(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        run_one_eval(poll == 0 ? 1 : 0);
        cmd->Close();
        ID3D12CommandList* lists[] = {cmd};
        queue->ExecuteCommandLists(1, lists);
        wait_gpu();

        if (check_target_modified() > 0) {
            built = true;
            break;
        }
        Sleep(50);
    }
    const double build_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_build_start).count();
    if (!built) {
        std::printf("FAIL: Network build timed out after %.2f s\n", build_sec);
        return 1;
    }
    std::printf("First frame + network build succeeded in %.2f s!\n", build_sec);

    // Full readback check
    {
        uint8_t* mapped = nullptr;
        readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
        uint64_t changed = 0, black = 0;
        double diff_sum = 0.0;
        for (uint32_t y = 0; y < H; ++y) {
            const auto* row = reinterpret_cast<const uint32_t*>(mapped + uint64_t(y) * fp.Footprint.RowPitch);
            for (uint32_t x = 0; x < W; ++x) {
                uint32_t out_px = row[x];
                uint32_t in_px = init_pixels[size_t(y) * W + x];
                if ((out_px & 0x3fffffffu) != (in_px & 0x3fffffffu)) ++changed;
                if ((out_px & 0x3fffffffu) == 0) ++black;
                int dr = int(out_px & 1023u) - int(in_px & 1023u);
                int dg = int((out_px >> 10) & 1023u) - int((in_px >> 10) & 1023u);
                int db = int((out_px >> 20) & 1023u) - int((in_px >> 20) & 1023u);
                diff_sum += (std::abs(dr) + std::abs(dg) + std::abs(db)) / (3.0 * 1023.0);
            }
        }
        if (dump) {
            if (FILE* f = std::fopen(dump, "wb")) {
                for (uint32_t y = 0; y < H; ++y) std::fwrite(mapped + uint64_t(y) * fp.Footprint.RowPitch, 4, W, f);
                std::fclose(f);
                std::printf("Output written to %s\n", dump);
            }
        }
        readback->Unmap(0, nullptr);
        const double total_px = double(W) * H;
        std::printf("Readback check (3840x2160): %.2f%% pixels modified by NR, mean |diff| = %.5f, black pixels = %.2f%%\n",
                    100.0 * double(changed) / total_px, diff_sum / total_px, 100.0 * double(black) / total_px);
    }

    // Run timed loop for `total_frames` frames
    // Note: for Vulkan Bridge, `cut()` splits `cmd` into `cmd` + Vulkan queue submit + continuation list inside `ExecuteCommandLists`.
    // Therefore, an EndQuery recorded before `run_one_eval` lands in `cmd` (before the cut), and an EndQuery recorded after
    // `run_one_eval` lands in the continuation list (after the Vulkan bridge fence wait)! So the D3D12 timestamp pair
    // across `run_one_eval` measures the exact end-to-end GPU + interop wall time on the D3D12 queue!
    ID3D12QueryHeap* ts_heap = nullptr;
    D3D12_QUERY_HEAP_DESC qhd{};
    qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qhd.Count = 2;
    device->CreateQueryHeap(&qhd, IID_PPV_ARGS(&ts_heap));
    ID3D12Resource* ts_buf = make_buf(device, D3D12_HEAP_TYPE_READBACK, 16, D3D12_RESOURCE_STATE_COPY_DEST);
    UINT64 freq = 0;
    queue->GetTimestampFrequency(&freq);

    std::vector<double> gpu_ms_list;
    std::vector<double> cpu_ms_list;
    std::vector<double> wall_ms_list;
    for (int f = 0; f < total_frames; ++f) {
        alloc->Reset();
        cmd->Reset(alloc, nullptr);
        seed_target(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        cmd->EndQuery(ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        const auto c0 = std::chrono::steady_clock::now();
        run_one_eval(0);
        const double cpu_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
        cmd->EndQuery(ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        cmd->ResolveQueryData(ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, ts_buf, 0);

        cmd->Close();
        ID3D12CommandList* lists[] = {cmd};
        const auto w0 = std::chrono::steady_clock::now();
        queue->ExecuteCommandLists(1, lists);
        wait_gpu();
        const double wall_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count();

        uint64_t t[2] = {};
        void* m = nullptr;
        ts_buf->Map(0, nullptr, &m);
        std::memcpy(t, m, 16);
        ts_buf->Unmap(0, nullptr);
        const double gpu_ms = double(t[1] - t[0]) * 1000.0 / double(freq);
        // Print per-frame progression for the first 10 frames and last 5 frames
        if (f < 10 || f >= total_frames - 5) {
            std::printf("  frame %2d: D3D12 GPU span = %6.3f ms, Submit+Wait wall = %6.3f ms, CPU rec = %5.3f ms\n",
                        f, gpu_ms, wall_ms, cpu_ms);
        }
        if (f >= 5) {
            gpu_ms_list.push_back(gpu_ms);
            cpu_ms_list.push_back(cpu_ms);
            wall_ms_list.push_back(wall_ms);
        }
    }

    double gpu_sum = 0, gpu_min = 1e9, gpu_max = 0;
    double cpu_sum = 0, wall_sum = 0;
    for (size_t i = 0; i < gpu_ms_list.size(); ++i) {
        gpu_sum += gpu_ms_list[i];
        gpu_min = std::min(gpu_min, gpu_ms_list[i]);
        gpu_max = std::max(gpu_max, gpu_ms_list[i]);
        cpu_sum += cpu_ms_list[i];
        wall_sum += wall_ms_list[i];
    }
    double vk_stage_ms = 0, vk_net_ms = 0;
    if (vulkan_bridge) {
        nr::dlssnr::with_d3d12_session([&](nr::pe::Session& s) {
            vk_stage_ms = s.gpu_ms();
            vk_net_ms = s.network_ms();
        });
    } else {
        const auto ns = nr::pe::d3d12_passthrough::backend_net_status();
        vk_stage_ms = ns.stage_ms;
        vk_net_ms = ns.gpu_ms;
    }

    std::printf("\n=== Benchmark Summary [%s] (%zu measured frames after 5 warmup) ===\n",
                vulkan_bridge ? "Vulkan Bridge" : "Direct ELF", gpu_ms_list.size());
    std::printf("D3D12 Queue End-to-End GPU span:               avg %.3f ms (min %.3f ms, max %.3f ms)\n",
                gpu_sum / gpu_ms_list.size(), gpu_min, gpu_max);
    std::printf("Submit + Fence Wait Wall Time:                 avg %.3f ms\n", wall_sum / wall_ms_list.size());
    std::printf("  - Pure Network GPU avg (internal):           %.3f ms\n", vk_net_ms);
    std::printf("  - RenoDX Stage + Network GPU avg (internal): %.3f ms\n", vk_stage_ms);
    std::printf("CPU evaluate_d3d12 recording time:             avg %.3f ms\n", cpu_sum / cpu_ms_list.size());
    return 0;
}
