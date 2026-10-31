#pragma once
// The public Advanced Shader Delivery interfaces the D3D12 ASD backend (nr_d3d12_asd.cpp) uses, declared here because
// mingw's d3d12.h has none of them: the D3D Shader Cache Registration (D3DSCR) API (d3dshadercacheregistration.h of
// the D3D12 Agility SDK 1.619) and the state object database (SODB) reader of d3d12.h. Method order, GUIDs and structs
// as the Agility SDK headers declare them; nothing here is driver-specific. Both are reached through the documented
// D3D12GetInterface export of d3d12.dll (resolved at run time: an older system d3d12.dll without it only turns the
// backend off).
#include <windows.h>
#include <unknwn.h>
#include <d3d12.h>

namespace nr::pe::asd::api {

// ---- D3DSCR (d3dshadercacheregistration.h) --------------------------------------------------------------------------
// CLSID_D3DShaderCacheInstallerFactory
inline constexpr GUID kClsidInstallerFactory = {0x16195a0b, 0x607c, 0x41f1, {0xbf, 0x03, 0xc7, 0x69, 0x4d, 0x60, 0xa8, 0xd4}};
inline constexpr GUID kIidInstallerFactory = {0x09b2dfe4, 0x840f, 0x401a, {0x80, 0x4c, 0x0d, 0xd8, 0xaa, 0xdc, 0x9e, 0x9f}};
inline constexpr GUID kIidInstaller = {0xbbe30de1, 0x6318, 0x4526, {0xae, 0x17, 0x77, 0x66, 0x93, 0x19, 0x1b, 0xb4}};
inline constexpr GUID kIidApplication = {0xfc688ee2, 0x1b35, 0x4913, {0x93, 0xbe, 0x1c, 0xa3, 0xfa, 0x7d, 0xf3, 0x9e}};
inline constexpr GUID kIidComponent = {0xeed1bf00, 0xf5c7, 0x4cf7, {0x88, 0x5c, 0xd0, 0xf9, 0xc0, 0xcb, 0x48, 0x28}};

enum Scope : int { kScopeUser = 0, kScopeSystem = 1 };
enum TargetFlags : int { kTargetNone = 0 };

union VersionNumber {
    UINT64 Version;
    UINT16 VersionParts[4];
};
struct PsdbProperties {
    const wchar_t* pAdapterFamily;
    const wchar_t* pPsdbPath;
};
struct CompilerProperties {
    wchar_t szAdapterFamily[128];
    UINT64 MinimumABISupportVersion;
    UINT64 MaximumABISupportVersion;
    VersionNumber CompilerVersion;
    VersionNumber ApplicationProfileVersion;
};
struct ApplicationDesc {
    const wchar_t* pExeFilename;
    const wchar_t* pName;
    VersionNumber Version;
    const wchar_t* pEngineName;
    VersionNumber EngineVersion;
};

struct Installer;
// Implemented by us; no IUnknown base (as the SDK declares it). Must outlive the installer made with it.
struct InstallerClient {
    virtual HRESULT STDMETHODCALLTYPE GetInstallerName(SIZE_T* pNameLength, wchar_t* pName) = 0;
    virtual Scope STDMETHODCALLTYPE GetInstallerScope() = 0;
    virtual HRESULT STDMETHODCALLTYPE HandleDriverUpdate(Installer* pInstaller) = 0;
};
struct Component : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetComponentName(const wchar_t** pName) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStateObjectDatabasePath(const wchar_t** pPath) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPrecompiledCachePath(const wchar_t* pAdapterFamily, const wchar_t** pPath) = 0;
    virtual UINT STDMETHODCALLTYPE GetPrecompiledShaderDatabaseCount() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPrecompiledShaderDatabases(UINT ArraySize, PsdbProperties* pPSDBs) = 0;
};
struct Application : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetExePath(const wchar_t** pExePath) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDesc(ApplicationDesc* pApplicationDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE RegisterComponent(const wchar_t* pName, const wchar_t* pStateObjectDBPath, UINT NumPSDB,
                                                        const PsdbProperties* pPSDBs, REFIID riid, void** ppvComponent) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveComponent(Component* pComponent) = 0;
    virtual UINT STDMETHODCALLTYPE GetComponentCount() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetComponent(UINT index, REFIID riid, void** ppvComponent) = 0;
    virtual UINT STDMETHODCALLTYPE GetPrecompileTargetCount(TargetFlags flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPrecompileTargets(UINT ArraySize, CompilerProperties* pArray, TargetFlags flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetInstallerName(const wchar_t** pInstallerName) = 0;
};
struct Installer : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE RegisterDriverUpdateListener() = 0;
    virtual HRESULT STDMETHODCALLTYPE UnregisterDriverUpdateListener() = 0;
    virtual HRESULT STDMETHODCALLTYPE RegisterServiceDriverUpdateTrigger(void* hServiceHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnregisterServiceDriverUpdateTrigger(void* hServiceHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE RegisterApplication(const wchar_t* pExePath, const ApplicationDesc* pApplicationDesc,
                                                          REFIID riid, void** ppvApp) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveApplication(Application* pApplication) = 0;
    virtual UINT STDMETHODCALLTYPE GetApplicationCount() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetApplication(UINT index, REFIID riid, void** ppvApp) = 0;
    virtual HRESULT STDMETHODCALLTYPE ClearAllState() = 0;   // never called: it would drop every app of this installer
    virtual UINT STDMETHODCALLTYPE GetMaxPrecompileTargetCount() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPrecompileTargets(const ApplicationDesc* pApplicationDesc, UINT ArraySize,
                                                           CompilerProperties* pArray, TargetFlags flags) = 0;
};
struct InstallerFactory : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateInstaller(InstallerClient* pClient, REFIID riid, void** ppvInstaller) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateExplorer(IUnknown* pUnknown, REFIID riid, void** ppvExplorer) = 0;
};

// ---- the state object database (d3d12.h, Agility SDK 1.619) ---------------------------------------------------------
// CLSID_D3D12StateObjectFactory
inline constexpr GUID kClsidStateObjectFactory = {0x54e1c9f3, 0x1303, 0x4112, {0xbf, 0x8e, 0x7b, 0xf2, 0xbb, 0x60, 0x6a, 0x73}};
inline constexpr GUID kIidStateObjectDatabaseFactory = {0xf5b066f0, 0x648a, 0x4611, {0xbd, 0x41, 0x27, 0xfd, 0x09, 0x48, 0xb9, 0xeb}};
inline constexpr GUID kIidStateObjectDatabase = {0xc56060b7, 0xb5fc, 0x4135, {0x98, 0xe0, 0xa1, 0xe9, 0x99, 0x7e, 0xac, 0xe0}};
enum DatabaseFlags : int { kDatabaseNone = 0, kDatabaseReadOnly = 1 };
// D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SERIALIZED_ROOT_SIGNATURE (29 in the 1.619 d3d12.h; not in mingw's)
inline constexpr int kSubobjectSerializedRootSignature = 29;
inline constexpr int kSubobjectCs = 6;   // D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS
struct SerializedRootSignatureDesc {
    const void* pSerializedBlob;
    SIZE_T SerializedBlobSizeInBytes;
};
using D3D12AppDesc = ApplicationDesc;   // D3D12_APPLICATION_DESC has the same layout as D3D_SHADER_CACHE_APPLICATION_DESC
using PipelineStateFunc = void(__stdcall*)(const void* pKey, UINT KeySize, UINT Version,
                                           const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc, void* pContext);
using ApplicationDescFunc = void(__stdcall*)(const D3D12AppDesc* pApplicationDesc, void* pContext);
struct StateObjectDatabase : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetApplicationDesc(const D3D12AppDesc* pApplicationDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetApplicationDesc(ApplicationDescFunc CallbackFunc, void* pContext) = 0;
    virtual HRESULT STDMETHODCALLTYPE StorePipelineStateDesc(const void* pKey, UINT KeySize, UINT Version,
                                                             const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindPipelineStateDesc(const void* pKey, UINT KeySize, PipelineStateFunc CallbackFunc,
                                                            void* pContext) = 0;
    // StoreStateObjectDesc, FindStateObjectDesc, FindObjectVersion follow; not used.
};
struct StateObjectDatabaseFactory : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateStateObjectDatabaseFromFile(LPCWSTR pDatabaseFile, DatabaseFlags flags, REFIID riid,
                                                                        void** ppv) = 0;
};

// d3d12.dll's D3D12GetInterface, resolved at run time (null: this d3d12.dll has none).
using PFN_GetInterface = HRESULT(WINAPI*)(REFCLSID rclsid, REFIID riid, void** ppv);
inline PFN_GetInterface get_interface() {
    HMODULE m = GetModuleHandleW(L"d3d12.dll");
    if (!m) m = LoadLibraryW(L"d3d12.dll");
    return m ? reinterpret_cast<PFN_GetInterface>(reinterpret_cast<void*>(GetProcAddress(m, "D3D12GetInterface"))) : nullptr;
}

}  // namespace nr::pe::asd::api
