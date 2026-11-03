// Toolchain resolution and compiler-target selection. Every path can be given
// on the command line; the defaults mirror the lab layout so an existing
// checkout keeps working, but nothing here depends on a lab directory.
#pragma once
#include <windows.h>
#include <d3d12compiler.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#include "common.hpp"
#include "compiler.hpp"
#include "util.hpp"

namespace lab {
namespace gen {

using Microsoft::WRL::ComPtr;

struct Paths {
    std::string compiler_dir;  // directory holding D3D12StateObjectCompiler.dll
    std::string plugin_dll;    // amdxc_compiler.dll
    std::string hlsl_dir;      // carriers (model_carrier.hlsl, pbptr_abi.hlsl)
    std::string dxc;           // dxc.exe
};

inline Paths resolve_paths(const Args& args, const std::string& repo) {
    Paths p;
    p.compiler_dir = args.get("compiler-dir", repo + "/third_party/agility/build/native/bin/x64");
    p.plugin_dll = args.get("plugin", repo + "/third_party/amd-plugin/build/native/bin/x64/amdxc_compiler.dll");
    p.hlsl_dir = args.get("shaders", repo + "/shaders/hlsl");
    p.dxc = args.get("dxc", dxc_path());
    return p;
}

inline void require_paths(const Paths& p) {
    require(!p.compiler_dir.empty() && !p.plugin_dll.empty() && !p.hlsl_dir.empty() && !p.dxc.empty(),
            "toolchain paths are empty");
    require(GetFileAttributesW(wide(p.compiler_dir + "/D3D12StateObjectCompiler.dll").c_str()) != INVALID_FILE_ATTRIBUTES,
            "D3D12StateObjectCompiler.dll not found in " + p.compiler_dir);
    require(GetFileAttributesW(wide(p.plugin_dll).c_str()) != INVALID_FILE_ATTRIBUTES,
            "amdxc_compiler.dll not found: " + p.plugin_dll);
    require(GetFileAttributesW(wide(p.dxc).c_str()) != INVALID_FILE_ATTRIBUTES, "dxc.exe not found: " + p.dxc);
}

struct CompilerRuntime {
    HMODULE module = nullptr;
    ComPtr<ID3D12CompilerFactory> factory;
};

inline CompilerRuntime load_compiler(const Paths& p) {
    CompilerRuntime rt;
    rt.module = LoadLibraryExW(wide(p.compiler_dir + "/D3D12StateObjectCompiler.dll").c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    require(rt.module != nullptr, "load official compiler runtime: " + p.compiler_dir + "/D3D12StateObjectCompiler.dll");
    auto create = reinterpret_cast<PFN_D3D12_COMPILER_CREATE_FACTORY>(
        GetProcAddress(rt.module, "D3D12CompilerCreateFactory"));
    require(create != nullptr, "D3D12CompilerCreateFactory export");
    check(create(wide(p.plugin_dll).c_str(), IID_PPV_ARGS(&rt.factory)), "compiler factory");
    return rt;
}

struct Target {
    D3D12_COMPILER_TARGET value = {};
    std::string family;
    UINT64 abi = 0;
    UINT index = 0;
};

// --family takes a substring of the adapter family name (default gfx1201,
// i.e. AMD_NAVI48:gfx1201), matching what the lab databases were built for.
inline Target select_target(ID3D12CompilerFactory* factory, const Args& args) {
    std::string want = args.get("family", "gfx1201");
    std::wstring wantw = wide(want);
    for (UINT i = 0; i < 128; ++i) {
        D3D12_ADAPTER_FAMILY family = {};
        if (FAILED(factory->EnumerateAdapterFamilies(i, &family))) break;
        if (wantw.empty() || std::wstring(family.szAdapterFamily).find(wantw) != std::wstring::npos) {
            UINT count = 0;
            check(factory->EnumerateAdapterFamilyABIVersions(i, &count, nullptr), "ABI count");
            std::vector<UINT64> versions(count);
            check(factory->EnumerateAdapterFamilyABIVersions(i, &count, versions.data()), "ABI versions");
            require(count > 0, "no ABI");
            Target t;
            t.value = {i, versions.back()};
            t.family = wide_to_utf8(family.szAdapterFamily);
            t.abi = versions.back();
            t.index = i;
            return t;
        }
    }
    throw std::runtime_error("compiler target not found for --family " + want);
}

// Evidence: every family the installed compiler advertises, with its ABI range.
inline void report_targets(ID3D12CompilerFactory* factory, Json& j) {
    j.arr("compiler_targets");
    for (UINT i = 0; i < 128; ++i) {
        D3D12_ADAPTER_FAMILY family = {};
        if (FAILED(factory->EnumerateAdapterFamilies(i, &family))) break;
        UINT count = 0;
        if (FAILED(factory->EnumerateAdapterFamilyABIVersions(i, &count, nullptr))) break;
        std::vector<UINT64> versions(count);
        if (count && FAILED(factory->EnumerateAdapterFamilyABIVersions(i, &count, versions.data()))) break;
        j.obj(nullptr);
        j.s("family", wide_to_utf8(family.szAdapterFamily));
        j.arr("abis");
        for (UINT64 v : versions) j.u(nullptr, v);
        j.end_arr();
        j.end_obj();
    }
    j.end_arr();
}

// Toolchain identity recorded next to every generated database.
inline void report_toolchain(const Paths& p, Json& j) {
    j.obj("toolchain");
    j.s("compiler_dll", p.compiler_dir + "/D3D12StateObjectCompiler.dll");
    j.s("compiler_dll_sha256", sha256_file(p.compiler_dir + "/D3D12StateObjectCompiler.dll"));
    j.s("plugin_dll", p.plugin_dll);
    j.s("plugin_dll_sha256", sha256_file(p.plugin_dll));
    j.s("dxc", p.dxc);
    j.s("dxc_sha256", sha256_file(p.dxc));
    j.end_obj();
}

}  // namespace gen
}  // namespace lab
