#pragma once
// The native D3D12 ASD backend of the .asi: NR's network as ACO machine code delivered to the game's own
// D3D12 device through the public Advanced Shader Delivery path - a state object database (SODB) and a precompiled
// shader database (PSDB) registered for the game's exe with the D3D Shader Cache Registration API, found by the D3D12
// runtime when the pipelines are created. No hook of the driver, no driver-version or adapter checks.
//
// Off unless <plugins>\dlssnr-amd-asd\dlssnr-amd-asd.ini says [D3D12ASD] Enabled = 1 (the folder beside this .asi);
// with it off nothing here runs.
//
//   1. registration (start, from InitializeASI): a thread, so not under the loader lock, registers the game's exe
//      (GetModuleFileNameW(nullptr)) with the SODB and PSDB of the folder - earlier entries of ours for this exe are
//      removed first, so every launch leaves one - and releases the installer at once. Before the game makes its D3D12
//      device it serves this launch; after, the next one. [D3D12ASD] Register = 0 (NR Control's Unregister sets it)
//      registers nothing.
//   2. the canary (on_device): once, on the game's device and its direct queue (or a queue of ours on it), the
//      g_pbptr pipeline exactly as stored in the SODB (built in; no SODB reader at run time), run and read back:
//      ASD_HIT when it ran the ACO kernel of the PSDB, ASD_MISS when the driver compiled the HLSL carrier, ASD_FAIL
//      otherwise. Only ASD_HIT lets the backend run.
//   3. the backend (nr_d3d12_asd_backend.hpp): the evaluate's frame copied into linear buffers, the network
//      (D3d12AsdNetwork), the result written back - or, at any failure, the Vulkan bridge.
#include <windows.h>
#include <d3d12.h>
#include <cstdint>
#include <string>
#include <vector>

namespace nr::pe::asd {

enum class Canary { kNotRun, kRunning, kHit, kMiss, kFail };
const char* canary_name(Canary c);   // ASD_HIT, ASD_MISS, ASD_FAIL, running, not run

// From InitializeASI (under the loader lock: reads the ini, starts a thread, nothing else). `self` is this .asi.
void start(HMODULE self);
// [D3D12ASD] Enabled as read at start.
bool enabled();
// The game's device, from the evaluate (every call; the first one starts the canary on a thread of ours).
void on_device(ID3D12Device* device);
Canary canary();

// NR Control: the registration removed (on a thread) and Register = 0 written, so no launch registers again; Enabled
// written (takes effect at the next launch: the registration has to come before the game's device).
void unregister_async();
void set_enabled(bool on);
void allow_register();   // Register = 1 again

// What NR Control shows (Diagnostics), and the backend's runs.
struct Status {
    bool enabled = false, register_on = true, files_found = false;
    std::string folder, registration, canary, backend, network;
    uint64_t runs = 0, fallbacks = 0;
};
Status status();
void note_backend(const char* line, bool ran);   // the backend's last state, in words
// Whether the last frame the backend recorded actually carried NR (a copy-through frame carries none and is still a
// success: the caller reports that as its own result instead of claiming the network ran).
void note_backend_produced(bool produced);
bool backend_produced();
void note_network(const char* name);

// The folder, the ini, the SODB (UTF-16, for the D3D12 APIs).
const std::wstring& folder();
std::wstring sodb_path();
std::wstring ini_path();
// NR Control's Diagnostics lines for the backend (nr_d3d12_asd_panel.cpp).
void draw_panel();

}  // namespace nr::pe::asd
