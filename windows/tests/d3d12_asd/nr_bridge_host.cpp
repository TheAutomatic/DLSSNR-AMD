// The real bridge, offline: a D3D12 device and the shared textures the network runs on, with the same model tree as
// the package. This is the D3D12 -> Vulkan shared-resource/fence path (nr_pe_bridge.hpp), not the pure-Vulkan
// reference host (nr_parity_host.cpp), and the only shape in which a frame time says anything about the bridge.
//
//   nr_bridge_host <dlssnr-amd root> <width> <height>
//
// First slice: create the bridge device on the D3D12 adapter's LUID and its two shared images (proxy, neural), then
// hand a job over the shared fence; the frame's own D3D12 commands follow.
#include "nr_pe_bridge.hpp"
#include "nr_renodx.hpp"
#include "nr_runtime.hpp"

#include <windows.h>
#include <d3d12.h>

#include "nr_pe_config.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <memory>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("usage: %s <dlssnr-amd root> <width> <height> [output-dump]\n", argv[0]);
        return 2;
    }
    const uint32_t W = uint32_t(std::atoi(argv[2])), H = uint32_t(std::atoi(argv[3]));
    ID3D12Device* d3d = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d))) || !d3d) {
        std::printf("no D3D12 device\n");
        return 3;
    }
    const LUID luid = d3d->GetAdapterLuid();
    std::string why;
    // Before the device: ACO Mode is enabled per device as it is created.
    nr::pe::configure_network(argv[1]);
    nr::pe::bridge::Device* dev = nr::pe::bridge::Device::get(luid, &why);
    if (!dev) {
        std::printf("bridge: %s\n", why.c_str());
        return 4;
    }
    nr::pe::bridge::Image colour{}, output{};
    if (!dev->create_d3d12(d3d, W, H, DXGI_FORMAT_R10G10B10A2_UNORM, &colour, &why)) {
        std::printf("bridge colour: %s\n", why.c_str());
        return 5;
    }
    if (!dev->create_d3d12(d3d, W, H, DXGI_FORMAT_R10G10B10A2_UNORM, &output, &why)) {
        std::printf("bridge output: %s\n", why.c_str());
        return 6;
    }
    nr::pe::bridge::Image motion{};
    if (!dev->create_d3d12(d3d, W, H, DXGI_FORMAT_R16G16_FLOAT, &motion, &why)) {
        std::printf("bridge motion: %s\n", why.c_str());
        return 6;
    }
    nr::pe::bridge::Image depth{};
    if (!dev->create_d3d12(d3d, W, H, DXGI_FORMAT_R32_FLOAT, &depth, &why)) {
        std::printf("bridge depth: %s\n", why.c_str());
        return 6;
    }
    std::printf("bridge: %ux%u colour %p output %p (rdna3 %d)\n", W, H, static_cast<void*>(colour.image),
                static_cast<void*>(output.image), dev->rdna3() ? 1 : 0);

    // The network on the bridge's own Vulkan device, then one job over the shared fence.
    nr::HostDevice host{};
    host.instance = dev->handles().instance;
    host.physical = dev->handles().physical;
    host.device = dev->handles().device;
    host.queue = dev->queue();
    host.queue_family = dev->family();
    host.buffer_device_address = true;   // the bridge device is created with the network's features
    host.rdna3 = dev->rdna3();
    nr::RuntimeConfig cfg;
    cfg.root = argv[1];
    cfg.width = W;
    cfg.height = H;
    cfg.colour_format = VK_FORMAT_R16G16B16A16_SFLOAT;
    cfg.network = 1;
    nr::TemporalConfig temporal;
    temporal.enable = true;
    nr::ControlMaskConfig mask;
    std::unique_ptr<nr::Runtime> rt_holder;
    try {
        rt_holder = std::make_unique<nr::Runtime>(host, cfg, mask, temporal);
    } catch (const std::exception& x) {
        std::printf("runtime: %s\n", x.what());
        return 12;
    }
    nr::Runtime& rt = *rt_holder;
    std::printf("network built on the bridge's device\n");

    // The shared images are filled on the D3D12 side and that queue is waited on before the GPU is asked to read
    // them: motion zero, neural zero, the frame the same pattern the D3D12 test writes, as half floats.
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    if (FAILED(d3d->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) || !queue ||
        FAILED(d3d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) || !alloc ||
        FAILED(d3d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list))) || !list ||
        FAILED(d3d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) || !fence) {
        std::printf("no D3D12 queue/allocator/list/fence\n");
        return 9;
    }
    auto upload = [&](const nr::pe::bridge::Image& img, const std::vector<uint8_t>& bytes, uint32_t bpp) {
        ID3D12Resource* res = nullptr;
        if (FAILED(img.d3d->QueryInterface(IID_PPV_ARGS(&res))) || !res) return false;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = bytes.size(); bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* up = nullptr;
        if (FAILED(d3d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                nullptr, IID_PPV_ARGS(&up))) || !up) { res->Release(); return false; }
        void* m = nullptr;
        bool ok = SUCCEEDED(up->Map(0, nullptr, &m)) && m;
        if (ok) { std::memcpy(m, bytes.data(), bytes.size()); up->Unmap(0, nullptr); }
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.pResource = res; dst.SubresourceIndex = 0;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.pResource = up;
        src.PlacedFootprint.Footprint = {img.dxgi, img.width, img.height, 1, UINT(uint64_t(img.width) * bpp)};
        if (ok) list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        res->Release();
        // the upload buffer is kept alive by the command list's reference to the copy
        return ok;
    };
    {
        const uint64_t px = uint64_t(W) * H;
        std::vector<uint8_t> frame(size_t(px * 4)), zero(size_t(px * 4));
        auto* u32 = reinterpret_cast<uint32_t*>(frame.data());
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x)
                u32[uint64_t(y) * W + x] = ((x * 13u + y * 7u) & 0x3FF) | (((x * 5u + y * 11u) & 0x3FF) << 10) |
                                           (((x * 3u + y * 17u) & 0x3FF) << 20) | (0x3FFu << 30);
        if (!upload(colour, frame, 4) || !upload(output, zero, 4) || !upload(motion, zero, 4) || !upload(depth, zero, 4)) {
            std::printf("the shared images could not be filled\n");
            return 10;
        }
        if (FAILED(list->Close())) { std::printf("list close failed\n"); return 11; }
        ID3D12CommandList* ls[] = {list};
        queue->ExecuteCommandLists(1, ls);
        queue->Signal(fence, 1);
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(1, ev);
        const bool ok = WaitForSingleObject(ev, 60000) == WAIT_OBJECT_0;
        CloseHandle(ev);
        if (!ok) { std::printf("the shared images did not finish\n"); return 11; }
    }
    std::printf("shared images filled on the D3D12 queue\n");

    // The full reference stage on the bridge: its preprocess, encode, the network in the shared job, and the resolve,
    // exactly as the 0.0.8.1 route records them (Session::record_renodx's shape).
    nr::renodx::StageConfig sc;
    sc.shaders = std::string(argv[1]) + "\\dlssnr-amd\\shaders\\renodx";
    sc.width = W; sc.height = H;
    sc.nr_width = W; sc.nr_height = H;
    sc.frame_format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    sc.hdr_flag = true;
    sc.present = 2;
    std::unique_ptr<nr::renodx::Stage> stage;
    try {
        stage = std::make_unique<nr::renodx::Stage>(host, sc);
    } catch (const std::exception& x) {
        std::printf("stage: %s\n", x.what());
        return 13;
    }
    nr::pe::bridge::Job* job = dev->begin(&why, stage.get());
    if (!job) {
        std::printf("bridge job: %s\n", why.c_str());
        return 7;
    }
    const auto frame_of = [&](const nr::pe::bridge::Image& img) {
        nr::ColourFrame f;
        f.image = img.image;
        f.format = img.format;
        f.width = img.width;
        f.height = img.height;
        f.before = f.after = VK_IMAGE_LAYOUT_GENERAL;
        f.before_stage = f.after_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        f.before_access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        f.after_access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        return f;
    };
    dev->acquire(job, {&colour, &output, &motion, &depth});   // the ownership barrier before anything reads them
    nr::renodx::StageFrame sf;
    sf.colour = frame_of(colour);
    sf.output = frame_of(output);
    sf.motion = frame_of(motion);
    sf.motion_x = 0; sf.motion_y = 0;
    sf.motion_scale_x = 1.0f / float(W);
    sf.motion_scale_y = 1.0f / float(H);
    sf.present_white_nits = 203.f;
    const VkCommandBuffer cmd = dev->command_buffer(job);
    const nr::renodx::Stage::Network net = [&](uint32_t k, const nr::ColourFrame& proxy, const nr::ColourFrame& neural,
                                               bool reset) {
        nr::EngineFrame e{};
        e.colour = proxy;
        e.target = neural;
        e.motion = frame_of(motion);
        e.motion_scale_x = 1.0f / float(W);
        e.motion_scale_y = 1.0f / float(H);
        e.depth = frame_of(depth);
        e.reset = reset;
        e.feature = uint64_t(k) + 1;
        nr::Controls controls;
        rt.record_engine(cmd, e, controls);
    };
    const uint32_t passes = stage->record(cmd, sf, net);
    std::printf("stage recorded %u pass(es) in the shared job\n", passes);
    if (!dev->end(job, &why)) {
        std::printf("bridge end: %s\n", why.c_str());
        return 8;
    }
    const uint64_t gen = dev->generation(job);
    const uint64_t mark = dev->hand_over_mark();   // where the queue is before this frame is given to it
    const bool ran = dev->run_between(queue, job, gen);
    std::printf("run_between -> %d\n", ran ? 1 : 0);
    dev->wait_idle();
    // The D3D12 queue must be past the hand-over before the shared resources go (D3D12 does not keep them alive):
    // a signal of its own behind everything run_between queued, waited on, is that completion.
    queue->Signal(fence, 2);
    {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(2, ev);
        const bool ok = WaitForSingleObject(ev, 60000) == WAIT_OBJECT_0;
        CloseHandle(ev);
        std::printf("D3D12 queue past the hand-over -> %d, hand_over_passed -> %d\n", ok ? 1 : 0,
                    dev->hand_over_passed(mark) ? 1 : 0);
    }
    // Read the result back on the D3D12 side: the frame this route produced, for the A/B against the ASD route.
    if (argc >= 5) {
        ID3D12Resource* out_res = nullptr;
        if (SUCCEEDED(output.d3d->QueryInterface(IID_PPV_ARGS(&out_res))) && out_res) {
            const uint64_t bytes = uint64_t(W) * H * 4;
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = bytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
            bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ID3D12Resource* rb = nullptr;
            if (SUCCEEDED(d3d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                       nullptr, IID_PPV_ARGS(&rb))) && rb) {
                D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.pResource = rb;
                dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R10G10B10A2_UNORM, W, H, 1, UINT(uint64_t(W) * 4)};
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.pResource = out_res;
                // The shared image is left in COMMON by run_between's hand-back: say what it is before reading it,
                // and put it back, or the next consumer writes into a resource D3D12 thinks is a copy source.
                D3D12_RESOURCE_BARRIER bar{};
                bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bar.Transition.pResource = out_res;
                bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                HRESULT hr = alloc->Reset();
                if (SUCCEEDED(hr)) hr = list->Reset(alloc, nullptr);
                if (SUCCEEDED(hr)) { list->ResourceBarrier(1, &bar); }
                if (SUCCEEDED(hr)) list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                if (SUCCEEDED(hr)) { list->ResourceBarrier(1, &bar); }
                if (SUCCEEDED(hr)) hr = list->Close();
                bool done = false;
                if (SUCCEEDED(hr)) {
                    ID3D12CommandList* ls[] = {list};
                    queue->ExecuteCommandLists(1, ls);
                    hr = queue->Signal(fence, 3);
                }
                if (SUCCEEDED(hr)) {
                    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    done = fence->SetEventOnCompletion(3, ev) == S_OK && WaitForSingleObject(ev, 60000) == WAIT_OBJECT_0;
                    CloseHandle(ev);
                }
                if (!done) {
                    std::printf("FAIL: the readback did not complete (hr 0x%08lX)\n", static_cast<unsigned long>(hr));
                    rb->Release();
                    out_res->Release();
                    return 14;
                }
                {
                    void* m = nullptr;
                    if (FAILED(rb->Map(0, nullptr, &m)) || !m) {
                        std::printf("FAIL: the readback could not be mapped\n");
                        rb->Release();
                        out_res->Release();
                        return 15;
                    }
                    FILE* f = std::fopen(argv[4], "wb");
                    const bool wrote = f && std::fwrite(m, 1, size_t(bytes), f) == size_t(bytes);
                    if (f) std::fclose(f);
                    rb->Unmap(0, nullptr);
                    if (!wrote) {
                        std::printf("FAIL: the dump could not be written to %s\n", argv[4]);
                        rb->Release();
                        out_res->Release();
                        return 16;
                    }
                    std::printf("bridge output written to %s\n", argv[4]);
                }
                rb->Release();
            }
            out_res->Release();
        }
    }
    queue->Release();
    dev->destroy(colour);
    dev->destroy(output);
    dev->destroy(motion);
    dev->destroy(depth);
    fence->Release();
    list->Release();
    alloc->Release();
    d3d->Release();
    return ran ? 0 : 10;
}
