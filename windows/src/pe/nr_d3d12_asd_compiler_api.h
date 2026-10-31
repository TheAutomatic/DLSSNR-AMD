#pragma once
// The public D3D12 shader compiler plugin interfaces (d3d12compiler.h of the D3D12 Agility SDK 1.619), declared here
// because mingw has none of them. Used only to make a PSDB for another application profile (nr_d3d12_asd_reprofile):
// D3D12StateObjectCompiler.dll's D3D12CompilerCreateFactory with the vendor's public ASD compiler plugin. Method order,
// GUIDs and structs as the SDK header declares them (mingw = the !_MSC_VER && _WIN32 variant of GetCompilerTarget).
#include "nr_d3d12_asd_api.h"

namespace nr::pe::asd::capi {

inline constexpr GUID kIidCompilerCacheSession = {0x5704e5e6, 0x054b, 0x4738, {0xb6, 0x61, 0x7b, 0x0d, 0x68, 0xd8, 0xdd, 0xe2}};
inline constexpr GUID kIidCompiler = {0x8c403c12, 0x993b, 0x4583, {0x80, 0xf1, 0x68, 0x24, 0x13, 0x8f, 0xa6, 0x8e}};
inline constexpr GUID kIidCompilerFactory = {0xc1ee4b59, 0x3f59, 0x47a5, {0x9b, 0x4e, 0xa8, 0x55, 0xc8, 0x58, 0xa8, 0x78}};

enum ValueType : int { kValueObjectCode = 0, kValueMetadata = 1, kValueDebugPdb = 2, kValuePerformanceData = 3 };
enum ValueTypeFlags : int { kFlagsObjectCode = 1, kFlagsMetadata = 2 };
struct AdapterFamily { wchar_t szAdapterFamily[128]; };
struct DatabasePath { ValueTypeFlags Types; const wchar_t* pPath; };
struct GroupKey { const void* pKey; UINT KeySize; };
struct ValueKey { const void* pKey; UINT KeySize; };
struct Value { void* pValue; UINT ValueSize; };
struct TypedValue { ValueType Type; Value Value; };
struct ConstValue { const void* pValue; UINT ValueSize; };
struct TypedConstValue { ValueType Type; ConstValue Value; };
struct Target { UINT AdapterFamilyIndex; UINT64 ABIVersion; };
using AllocationFunc = void*(__stdcall*)(SIZE_T SizeInBytes, void* pContext);
using GroupValueKeysFunc = void(__stdcall*)(const ValueKey* pValueKey, void* pContext);
using GroupValuesFunc = void(__stdcall*)(UINT ValueKeyIndex, const TypedConstValue* pTypedValue, void* pContext);

struct CompilerFactoryChild : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetFactory(REFIID riid, void** ppv) = 0;
};
struct CompilerCacheSession : CompilerFactoryChild {
    virtual HRESULT STDMETHODCALLTYPE FindGroup(const GroupKey* pGroupKey, UINT* pGroupVersion) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindGroupValueKeys(const GroupKey* pGroupKey, const UINT* pExpectedGroupVersion,
                                                         GroupValueKeysFunc CallbackFunc, void* pContext) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindGroupValues(const GroupKey* pGroupKey, const UINT* pExpectedGroupVersion,
                                                      ValueTypeFlags ValueTypeFlags, GroupValuesFunc CallbackFunc, void* pContext) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindValue(const ValueKey* pValueKey, TypedValue* pTypedValues, UINT NumTypedValues,
                                                AllocationFunc pCallbackFunc, void* pContext) = 0;
    virtual const api::ApplicationDesc* STDMETHODCALLTYPE GetApplicationDesc() = 0;
    virtual Target* STDMETHODCALLTYPE GetCompilerTarget(Target* RetVal) = 0;   // not used
    virtual ValueTypeFlags STDMETHODCALLTYPE GetValueTypes() = 0;
    virtual HRESULT STDMETHODCALLTYPE StoreGroupValueKeys(const GroupKey* pGroupKey, UINT GroupVersion, const ValueKey* pValueKeys,
                                                          UINT NumValueKeys) = 0;
    virtual HRESULT STDMETHODCALLTYPE StoreValue(const ValueKey* pValueKey, const TypedConstValue* pTypedValues,
                                                 UINT NumTypedValues) = 0;
};
struct Compiler : CompilerFactoryChild {
    virtual HRESULT STDMETHODCALLTYPE CompilePipelineState(const GroupKey* pGroupKey, UINT GroupVersion,
                                                           const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc) = 0;
    // CompileStateObject, CompileAddToStateObject, GetCacheSession follow; not used.
};
struct CompilerFactory : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE EnumerateAdapterFamilies(UINT AdapterFamilyIndex, AdapterFamily* pAdapterFamily) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnumerateAdapterFamilyABIVersions(UINT AdapterFamilyIndex, UINT32* pNumABIVersions,
                                                                        UINT64* pABIVersions) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnumerateAdapterFamilyCompilerVersion(UINT AdapterFamilyIndex,
                                                                            api::VersionNumber* pCompilerVersion) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetApplicationProfileVersion(const Target* pTarget, const api::ApplicationDesc* pApplicationDesc,
                                                                   api::VersionNumber* pApplicationProfileVersion) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateCompilerCacheSession(const DatabasePath* pPaths, UINT NumPaths, const Target* pTarget,
                                                                 const api::ApplicationDesc* pApplicationDesc, REFIID riid,
                                                                 void** ppCompilerCacheSession) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateCompiler(CompilerCacheSession* pCompilerCacheSession, REFIID riid, void** ppv) = 0;
};
using PFN_CreateFactory = HRESULT(WINAPI*)(const wchar_t* pPluginCompilerDllPath, REFIID riid, void** ppFactory);

}  // namespace nr::pe::asd::capi
