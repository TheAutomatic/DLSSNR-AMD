#pragma once
// D3DCompile of the system's d3dcompiler_47.dll (System32), never a game's: games ship old copies of the same name in
// their folder (one without shader model 5.1 failed every cs_5_1 program with X3506), and LoadLibrary by name takes
// the game folder's first. Null when the system has none.
#include <windows.h>
#include <d3dcommon.h>
#include <string>

namespace nr::pe {

using PFN_D3DCompile47 = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                          UINT, UINT, ID3DBlob**, ID3DBlob**);

inline HMODULE system_d3dcompiler() {
    static const HMODULE m = [] {
        wchar_t dir[MAX_PATH] = {};
        const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
        if (!n || n >= MAX_PATH) return HMODULE(nullptr);
        const std::wstring path = std::wstring(dir) + L"\\d3dcompiler_47.dll";
        return LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }();
    return m;
}
inline PFN_D3DCompile47 system_d3dcompile() {
    static const PFN_D3DCompile47 fn = system_d3dcompiler()
        ? reinterpret_cast<PFN_D3DCompile47>(reinterpret_cast<void*>(GetProcAddress(system_d3dcompiler(), "D3DCompile")))
        : nullptr;
    return fn;
}

}  // namespace nr::pe
