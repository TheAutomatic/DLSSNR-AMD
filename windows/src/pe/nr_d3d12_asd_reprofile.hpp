#pragma once
// A PSDB of ours for another driver application profile, made with public APIs only (D3D12StateObjectCompiler.dll +
// the vendor's public ASD compiler plugin, d3d12compiler.h): the driver keys a PSDB's values by the application
// profile it applies to the running exe (D3DSCR GetPrecompileTargets reports it per target), so a PSDB compiled for
// profile 0 misses in a game the driver has a profile for. For each group (pipeline key) of the SODB: the SODB's own
// pipeline desc compiled by the plugin for the new identity (exe name) gives that profile's value keys and metadata;
// the object code stored under them is the one of the source PSDB (our ACO machine code, never the plugin's).
#include <cstdint>
#include <string>
#include <vector>
#include "nr_d3d12_asd_api.h"

namespace nr::pe::asd {

struct ReprofileJob {
    std::wstring compiler_dll;   // D3D12StateObjectCompiler.dll (Agility SDK)
    std::wstring plugin_dll;     // the vendor's ASD compiler plugin (AMD: amdxc_compiler.dll)
    std::wstring family;         // e.g. AMD_NAVI48:gfx1201 (the source PSDB's)
    std::wstring sodb, src_psdb, out_psdb;
    std::wstring exe, name, engine;   // the identity; exe = the running exe's file name for the new profile
    UINT64 version = 0, engine_version = 0;
    std::wstring src_exe;        // the exe name the source PSDB was compiled for (its identity's)
    // SODB / PSDB group keys (NUL-terminated strings). Empty: every group of ours, as the lab's m5-build names them - the
    // canary "g_pbptr" and "m2-<tag>" for each record of net-carriers.bin beside src_psdb (or beside sodb); the canary alone
    // when there is no net-carriers.bin (the placeholder package).
    std::vector<std::string> groups;
};
struct ReprofileResult {
    bool ok = false;
    UINT64 src_profile = 0, new_profile = 0;
    std::string text;
};
// The application profile the plugin applies to `exe` (with the identity of `job`) for `job.family`.
ReprofileResult reprofile_psdb(const ReprofileJob& job, bool profile_only = false);

}  // namespace nr::pe::asd
