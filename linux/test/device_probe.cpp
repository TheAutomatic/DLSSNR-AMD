// The T0 device probe: create the network's Vulkan device the way the runtime would and
// print what it got — the selected device, the driver, the target's matrix configuration.
// Run with NR_DEVICE_ID=<vendor:device> (e.g. 1002:1900 for the 780M under investigation),
// NR_GPU_UUID=<32 hex digits>, or NR_GPU=<enumeration index>; with none of them the target's
// default picks. Exit code 0 means the device came up with everything the target needs.
// Build (from the repo root, on a machine whose Vulkan loader can reach the GPU):
//
//   g++ -std=c++17 -O2 -DNR_ARCH_RDNA3=1 \
//       -Itoolchain/Vulkan-Headers/include -Ilinux/src -Ilinux/src/core \
//       linux/test/device_probe.cpp -o device_probe_rdna3 -lvulkan
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
    std::printf("device create: OK\n");
    return 0;
}
