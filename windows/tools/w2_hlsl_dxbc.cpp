// The wilsjo2 programs for the native D3D12 route (M5 v2), last step: each SPIRV-Cross HLSL program (from the
// Vulkan stage's own SPIR-V, w2_spv_d3d12.py) compiled to DXBC (cs_5_0) by d3dcompiler_47.dll, as the ASI would at
// run time - ahead of time, so no frame waits on the compiler. IEEE strictness (/Gis): the programs' NaN tests
// (isnan) are kept, as the SPIR-V has them, and no arithmetic is reordered. Run on Windows.
//   w2_hlsl_dxbc.exe <in.hlsl> <out.dxbc>
#include <windows.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <vector>

using PFN_D3DCompile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                        UINT, UINT, ID3DBlob**, ID3DBlob**);

int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: w2_hlsl_dxbc <in.hlsl> <out.dxbc>\n"); return 2; }
    HMODULE dll = LoadLibraryA("d3dcompiler_47.dll");
    auto compile = dll ? reinterpret_cast<PFN_D3DCompile>(GetProcAddress(dll, "D3DCompile")) : nullptr;
    if (!compile) { std::fprintf(stderr, "d3dcompiler_47.dll not found\n"); return 1; }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::fprintf(stderr, "%s: cannot open\n", argv[1]); return 1; }
    std::vector<char> src;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) src.insert(src.end(), buf, buf + n);
    std::fclose(f);
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = compile(src.data(), src.size(), argv[1], nullptr, nullptr, "main", "cs_5_0",
                               D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS, 0, &code, &errors);
    if (errors) {
        std::fprintf(stderr, "%.*s", int(errors->GetBufferSize()), static_cast<const char*>(errors->GetBufferPointer()));
        errors->Release();
    }
    if (FAILED(hr) || !code) { std::fprintf(stderr, "%s: does not compile (0x%08lX)\n", argv[1], hr); return 1; }
    FILE* o = std::fopen(argv[2], "wb");
    if (!o) { std::fprintf(stderr, "%s: cannot write\n", argv[2]); return 1; }
    std::fwrite(code->GetBufferPointer(), 1, code->GetBufferSize(), o);
    std::fclose(o);
    std::printf("%s: %zu bytes\n", argv[2], size_t(code->GetBufferSize()));
    code->Release();
    return 0;
}
