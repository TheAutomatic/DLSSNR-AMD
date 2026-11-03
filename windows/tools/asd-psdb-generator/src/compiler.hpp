// CPU-only compiler helpers: HLSL -> DXIL through dxc.exe, and a serialized
// root signature through D3D12SerializeRootSignature (no device involved).
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <string>
#include <utility>
#include <vector>

#include "util.hpp"

namespace lab {

using Microsoft::WRL::ComPtr;

inline std::string hr_name(HRESULT hr) {
    char b[64];
    snprintf(b, sizeof b, "0x%08X", uint32_t(hr));
    return b;
}
inline void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) throw std::runtime_error(std::string(what) + " failed: " + hr_name(hr));
}

// Only the fields compile_hlsl consumes. Values match the lab harness defaults
// so byte-identical DXIL is produced for the same HLSL and defines.
struct CaseDef {
    std::string hlsl_path;
    int shader_mode = 0;
    uint32_t elements = 65539;
    uint32_t threads = 64;
    uint32_t const_value = 0x00c0ffee;
    uint32_t uavs = 1;
    std::vector<std::pair<std::string, std::string>> extra_defines;
};

inline std::string& dxc_path() {
    static std::string path = "C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.26100.0\\x64\\dxc.exe";
    return path;
}

// Same command line as the lab harness: -T cs_6_6 -E main -O3 -HV 2021.
inline Bytes compile_hlsl(const CaseDef& c, const std::string& out_cso) {
    require(!c.hlsl_path.empty(), "no hlsl source given");
    require(!dxc_path().empty(), "dxc path not set");
    char tmp[MAX_PATH], tmpdir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpdir);
    GetTempFileNameA(tmpdir, "lab", 0, tmp);
    std::string cso = out_cso.empty() ? std::string(tmp) + ".cso" : out_cso;
    char cb[16];
    snprintf(cb, sizeof cb, "%x", c.const_value);
    std::string args = "-T cs_6_6 -E main -O3 -HV 2021 -no-warnings" +
                       std::string(" -D MODE=") + std::to_string(c.shader_mode) +
                       " -D ELEMENTS=" + std::to_string(c.elements) +
                       " -D THREADS=" + std::to_string(c.threads) +
                       " -D CONST_VALUE=0x" + cb +
                       " -D UAVS=" + std::to_string(c.uavs);
    for (const auto& kv : c.extra_defines) args += " -D " + kv.first + "=" + kv.second;
    args += " -Fo \"" + cso + "\" \"" + c.hlsl_path + "\"";
    std::string cmdline = "\"" + dxc_path() + "\" " + args;
    STARTUPINFOA si = {sizeof(STARTUPINFOA)};
    SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE errrd = nullptr;
    std::string errfile = std::string(tmp) + ".err";
    errrd = CreateFileA(errfile.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdError = errrd;
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    require(ok, "CreateProcess(dxc) failed: " + dxc_path());
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    std::string errs;
    if (errrd != INVALID_HANDLE_VALUE) {
        SetFilePointer(errrd, 0, nullptr, FILE_BEGIN);
        char buf[4096]; DWORD n = 0;
        while (ReadFile(errrd, buf, sizeof buf, &n, nullptr) && n) errs.append(buf, n);
        CloseHandle(errrd);
    }
    DeleteFileA(errfile.c_str());
    if (code != 0) {
        DeleteFileA(cso.c_str());
        if (out_cso.empty()) DeleteFileA(tmp);
        throw std::runtime_error("dxc failed (" + std::to_string(code) + "): " + errs);
    }
    Bytes blob = lab::read_file(cso);
    if (out_cso.empty()) DeleteFileA(cso.c_str());
    DeleteFileA(tmp);
    return blob;
}

// Root signature used by every carrier and by the ASI: N root constants at b0
// plus one root UAV at u0. Serialized on the CPU, stored in the SODB.
inline Bytes serialize_root_constants(uint32_t nconst, uint32_t uavs) {
    require(nconst >= 1 && nconst <= 64, "root constant count");
    require(uavs <= 8, "uav count out of range");
    std::vector<D3D12_ROOT_PARAMETER> params;
    D3D12_ROOT_PARAMETER c = {};
    c.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    c.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    c.Constants.ShaderRegister = 0;
    c.Constants.RegisterSpace = 0;
    c.Constants.Num32BitValues = nconst;
    params.push_back(c);
    for (uint32_t u = 0; u < uavs; ++u) {
        D3D12_ROOT_PARAMETER p = {};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        p.Descriptor.ShaderRegister = u;
        p.Descriptor.RegisterSpace = 0;
        params.push_back(p);
    }
    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = UINT(params.size());
    desc.pParameters = params.data();
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> sig, err;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &sig, &err);
    if (FAILED(hr)) {
        std::string e = err ? std::string(static_cast<char*>(err->GetBufferPointer()), err->GetBufferSize()) : "";
        throw std::runtime_error("D3D12SerializeRootSignature failed: " + e);
    }
    return Bytes(static_cast<uint8_t*>(sig->GetBufferPointer()),
                 static_cast<uint8_t*>(sig->GetBufferPointer()) + sig->GetBufferSize());
}

}  // namespace lab
