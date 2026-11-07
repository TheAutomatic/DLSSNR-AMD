#pragma once
// The D3D12 passthrough of the .asi: on the OptiScaler route in a D3D12 game, NR's network runs on the game's own
// D3D12 device as ACO's machine code (the ACO Mode records, dlssnr-amd\aco\records) instead of through the Vulkan
// bridge. Each record is wrapped into a PAL ELF in memory (nr_d3d12_asd_elf.hpp) and made into a pipeline by AMD's
// driver extension (amdxc64.dll, IAmdExtD3DDevice5::CreateComputePipelineFromElf). RDNA4 only.
//
// dlssnr-amd.ini [D3D12Passthrough] Enabled (read at launch; NR Control's Diagnostics writes it):
//   1. the canary (on_device): once, on the game's device and its direct queue (or a queue of ours on it), a small
//      ACO kernel made into a pipeline the same way, run and read back: kHit when the GPU computed exactly its
//      values, kFail otherwise (not an RDNA4 GPU, no extension, another result). Only kHit lets the backend run.
//   2. the backend (nr_d3d12_asd_backend.hpp): the evaluate's frame, the network (D3d12AsdNetwork), the result
//      written back - or, at any failure, the Vulkan bridge.
#include <windows.h>
#include <d3d12.h>
#include <cstdint>
#include <string>

namespace nr::pe::asd {

// [D3D12Passthrough] Enabled when dlssnr-amd.ini does not have it.
inline constexpr bool kDefaultEnabled = true;

enum class Canary { kNotRun, kRunning, kHit, kFail };
const char* canary_name(Canary c);   // ok, failed, running, not run

// From InitializeASI, after dlssnr-amd.ini is in place (reads it, nothing else).
void start();
// [D3D12Passthrough] Enabled as read at start.
bool enabled();
// The game's device, from the evaluate (every call; the first one starts the canary on a thread of ours).
void on_device(ID3D12Device* device);
Canary canary();
// The game's GPU is RDNA3 (gfx11): the network runs as the bundle's RDNA3 records (dlssnr-amd\aco\rdna3), wrapped
// into gfx11 PAL ELFs; RDNA4's canary is not run there (its machine code is gfx12's). Set by on_device.
bool rdna3();
// The gfx11 EF_AMDGPU_MACH the driver took (the first ELF made tries them in turn); 0 not known yet.
uint32_t gfx11_mach();
void set_gfx11_mach(uint32_t mach);
// The texels one row of a linear buffer holds that the network reads as an image: RDNA4's image descriptor takes the
// width as the row pitch; RDNA3's has no pitch and reads rows 256-byte aligned (RADV's linear images on gfx11: rows
// of 32 RGBA16F / 16 RGBA32F texels). Every linear buffer the passthrough hands the network is laid out so.
inline uint32_t lin_pitch(uint32_t width, uint32_t bytes_per_texel) {
    return rdna3() ? (width * bytes_per_texel + 255u) / 256u * 256u / bytes_per_texel : width;
}

// NR Control's switch (Network page): Enabled written to dlssnr-amd.ini (takes effect at the next launch).
void set_enabled(bool on);

// The ACO records' folder, dlssnr-amd\aco beside OptiScaler (UTF-16).
std::wstring aco_folder();

// What NR Control shows (Diagnostics), and the backend's runs.
struct Status {
    bool enabled = false;
    std::string adapter, canary, backend, network;
    uint64_t runs = 0, fallbacks = 0;
};
Status status();
void note_backend(const char* line, bool ran);   // the backend's last state, in words
// Whether the last frame the backend recorded actually carried NR (a copy-through frame carries none and is still a
// success: the caller reports that as its own result instead of claiming the network ran).
void note_backend_produced(bool produced);
bool backend_produced();
void note_network(const char* name);

// NR Control (nr_d3d12_asd_panel.cpp): the switch and a short state on the Network page, the details in Diagnostics.
void draw_network();
void draw_panel();

}  // namespace nr::pe::asd
