#pragma once
// Diagnostic capture for the D3D12 (ASD) route, for the frames where the network's first dispatches go in.
//
// A game that dies on the GPU leaves no exception, no exit line and - with WerSvc off - no dump. What it does leave is
// the driver's own account of the failure, but only if the process asked for it before the first device existed: the
// D3D12 debug layer (which validates every recorded call and names the resource in the message), DRED (auto
// breadcrumbs: the last commands each command list reached; page faults: the address and every allocation it may
// belong to). Everything is written as it happens, into <game>\nr-diag\, so a reproduction leaves one folder.
//
// Off unless the ASD route is on: [D3D12ASD] Capture in dlssnr-amd-asd.ini (default 1 while the route is young) or
// NR_ASD_CAPTURE=0 to skip it for a run.
#include <windows.h>
#include <d3d12.h>

#include <string>

namespace nr::pe::capture {
// Before any device is made: the debug layer, GPU validation, DRED, and the run's folder. Logs each step and whether
// it took, so a reproduction that captures nothing says so at the top of the log instead of at the end.
void start(const std::string& game_folder, bool wanted);
bool on();
const std::wstring& folder();
// The first device: the info queue (debug messages) and the adapter, for the run's header.
void adopt(ID3D12Device* device);
// Drains new debug-layer messages into the run's folder and the log. Cheap to call every frame.
void drain();
// A PIX marker on the game's list: DRED's breadcrumbs keep it, so the last marker before a fault names the stage.
void mark(ID3D12GraphicsCommandList* cmd, const wchar_t* what);
// One line of the schedule (call, dispatch, resource) and one key/value pair for the run's header, written at once.
void schedule(const std::string& line);
void note(const char* key, const std::string& value);
// The device came back removed: reason, DRED breadcrumbs, page faults and a minidump, into the run's folder.
void device_removed(HRESULT why);
// "nr:act" style names on our objects: a debug-layer message or a DRED allocation then points at the resource.
void name(ID3D12Object* object, const wchar_t* name);
}  // namespace nr::pe::capture
