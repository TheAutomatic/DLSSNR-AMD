#pragma once
// The native D3D12 ASD backend of the .asi (M5 shell): NR's network as ACO machine code delivered to the game's own
// D3D12 device through the public Advanced Shader Delivery path - a state object database (SODB) and a precompiled
// shader database (PSDB) registered for the game's exe with the D3D Shader Cache Registration API, found by the D3D12
// runtime when the pipelines are created. No hook of the driver, no driver-version or adapter checks.
//
// Off unless <plugins>\dlssnr-amd-asd\dlssnr-amd-asd.ini says [D3D12ASD] Enabled = 1 (the folder beside this .asi);
// with it off nothing here runs and the .asi behaves as 0.0.8.1.
//
//   1. registration (start, from InitializeASI): a thread, so not under the loader lock, registers the game's exe
//      (GetModuleFileNameW(nullptr)) with the SODB and PSDB of the folder - earlier entries of ours for this exe are
//      removed first, so every launch leaves one - and releases the installer at once. Before the game makes its D3D12
//      device it serves this launch; after, the next one. [D3D12ASD] Register = 0 (NR Control's Unregister sets it)
//      registers nothing.
//   2. the canary (on_device): once, on the game's device, a queue of ours, the g_pbptr pipeline exactly as stored in the SODB (built in; no SODB reader at run time) - the M1 pbptr
//      carrier - run and read back: ASD_HIT when it ran the ACO kernel of the PSDB, ASD_MISS when the driver compiled
//      the HLSL carrier, ASD_FAIL otherwise. Only ASD_HIT lets the backend run.
//   3. the backend (nr_d3d12_asd_backend.hpp): the evaluate's frame copied into linear buffers, the network
//      (D3d12AsdNetwork), the result written back - or, at any failure, the 0.0.8.1 path.
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
void note_network(const char* name);

// The folder, the ini, the files (UTF-16, for the D3D12 APIs).
const std::wstring& folder();
std::wstring sodb_path();
std::wstring psdb_path();
std::wstring ini_path();
bool debug_tint();
void draw_panel();
// Tools/tests: a pipeline's serialized root signature and DXIL as stored in the SODB under `key` (needs a runtime with
// the SODB reader, Agility 1.619+; the ASI itself does not use it). Makes nr_d3d12_asd_canary_blob.h.
bool sodb_pipeline(const char* key, std::vector<uint8_t>* root, std::vector<uint8_t>* cs, unsigned* flags,
                   unsigned* node_mask, std::string* why);   // NR Control's Diagnostics lines (nr_d3d12_asd_panel.cpp)   // [D3D12ASD] DebugTint = 1: the placeholder's output tinted, to see in game that the path runs

}  // namespace nr::pe::asd
