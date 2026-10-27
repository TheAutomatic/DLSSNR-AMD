// The T0 device probe: create the network's Vulkan device the way the runtime would and
// print what it got — the selected device, the driver, the target's matrix configuration.
// Run on Windows with NR_DEVICE_ID=1002:1900 (the 780M) or NR_GPU=<index>; exit code 0 means
// the device came up with everything the target needs. Build (cross, from the repo root):
//
//   x86_64-w64-mingw32-g++ -std=c++17 -O2 -DNR_ARCH_RDNA3=1 \
//       -Itoolchain/Vulkan-Headers/include -Iwindows/src -Iwindows/src/core \
//       windows/test/device_probe.cpp -o device_probe_rdna3.exe -lvulkan-1 -static -static-libgcc -static-libstdc++
//
// (leave out -DNR_ARCH_RDNA3=1 for the rdna4-fp8 probe).
#include "nrvk.hpp"
#include <cstdio>

int main() {
    nrvk::Context ctx;
    try {
        ctx.create();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "device create failed: %s\n", e.what());
        return 1;
    }
    std::printf("target: %s\n", nrvk::nr_target_name());
    std::printf("device: %s\n", ctx.gpu_name.c_str());
    std::printf("driver: %s\n", ctx.driver_name.c_str());
    std::printf("queue family: %u\n", ctx.family);
    std::printf("buffer device address: %s\n", ctx.buffer_device_address ? "yes" : "no");
    std::printf("pipeline binaries (ACO): %s\n", nr::binary::enabled(ctx.device) ? "enabled" : "off");
    std::printf("device create: OK\n");
    return 0;
}
