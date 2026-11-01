// The real bridge, offline: a D3D12 device and the shared textures the network runs on, with the same model tree as
// the package. This is the D3D12 -> Vulkan shared-resource/fence path (nr_pe_bridge.hpp), not the pure-Vulkan
// reference host (nr_parity_host.cpp), and the only shape in which a frame time says anything about the bridge.
//
//   nr_bridge_host <dlssnr-amd root> <width> <height>
//
// First slice: create the bridge device on the D3D12 adapter's LUID and its two shared images (proxy, neural), then
// hand a job over the shared fence; the frame's own D3D12 commands follow.
#include "nr_pe_bridge.hpp"
#include "nr_runtime.hpp"

#include <windows.h>
#include <d3d12.h>

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("usage: %s <dlssnr-amd root> <width> <height>\n", argv[0]);
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
    nr::pe::bridge::Device* dev = nr::pe::bridge::Device::get(luid, &why);
    if (!dev) {
        std::printf("bridge: %s\n", why.c_str());
        return 4;
    }
    nr::pe::bridge::Image proxy{}, neural{};
    if (!dev->create_d3d12(d3d, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, &proxy, &why)) {
        std::printf("bridge proxy: %s\n", why.c_str());
        return 5;
    }
    if (!dev->create_d3d12(d3d, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, &neural, &why)) {
        std::printf("bridge neural: %s\n", why.c_str());
        return 6;
    }
    std::printf("bridge: %ux%u proxy %p neural %p (rdna3 %d)\n", W, H, static_cast<void*>(proxy.image),
                static_cast<void*>(neural.image), dev->rdna3() ? 1 : 0);
    dev->destroy(proxy);
    dev->destroy(neural);
    d3d->Release();
    return 0;
}
