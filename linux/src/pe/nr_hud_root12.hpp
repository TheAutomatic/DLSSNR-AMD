#pragma once
#include "nr_hud_descriptors.hpp"
#include <d3d12.h>
#include <atomic>
#include <memory>
namespace nr::hudcap {
struct RootLayout { std::vector<std::vector<DescriptorSpan>> tables; };
inline const GUID kRootLayoutId={0x069cb498,0xa1ae,0x4ca9,{0xa2,0x4b,0x12,0x0d,0x3e,0xb2,0xc9,0x81}};
struct RootData final: IUnknown {
    std::atomic<ULONG> refs{1}; std::shared_ptr<const RootLayout> layout;
    explicit RootData(std::shared_ptr<const RootLayout> p):layout(std::move(p)){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER; *out=nullptr;
        if (id!=__uuidof(IUnknown)) return E_NOINTERFACE;
        *out=this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override {auto n=--refs;if(!n)delete this;return n;}
};
inline std::shared_ptr<const RootLayout> root_layout(ID3D12RootSignature* root) {
    if (!root) return {};
    RootData* data=nullptr; UINT bytes=sizeof(data);
    if (FAILED(root->GetPrivateData(kRootLayoutId,&bytes,&data)) || !data) return {};
    auto layout=data->layout; data->Release(); return layout;
}
inline bool attach_root_layout(ID3D12RootSignature* root,const void* bytes,SIZE_T size) {
    using Deserialize=HRESULT(WINAPI*)(LPCVOID,SIZE_T,REFIID,void**);
    auto module=GetModuleHandleW(L"d3d12.dll");
    auto deserialize=module ? reinterpret_cast<Deserialize>(GetProcAddress(module,"D3D12CreateVersionedRootSignatureDeserializer")) : nullptr;
    if (!deserialize) return false;
    ID3D12VersionedRootSignatureDeserializer* decoder=nullptr;
    if (FAILED(deserialize(bytes,size,IID_PPV_ARGS(&decoder))) || !decoder) return false;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc=nullptr;
    const HRESULT hr=decoder->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_0,&desc);
    if (FAILED(hr) || !desc) {decoder->Release();return false;}
    auto result=std::make_shared<RootLayout>();
    const auto& d=desc->Desc_1_0;
    result->tables.resize(d.NumParameters);
    if (!(d.Flags&D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS))
        for (UINT i=0;i<d.NumParameters;++i) {
            const auto& p=d.pParameters[i];
            if (p.ParameterType!=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE ||
                (p.ShaderVisibility!=D3D12_SHADER_VISIBILITY_ALL && p.ShaderVisibility!=D3D12_SHADER_VISIBILITY_PIXEL)) continue;
            std::vector<DescriptorRange> ranges;
            for (UINT j=0;j<p.DescriptorTable.NumDescriptorRanges;++j) {
                const auto& r=p.DescriptorTable.pDescriptorRanges[j];
                ranges.push_back({uint32_t(r.RangeType),r.NumDescriptors,r.OffsetInDescriptorsFromTableStart});
            }
            result->tables[i]=srv_spans(ranges);
        }
    decoder->Release();
    auto* data=new RootData(std::move(result));
    const HRESULT saved=root->SetPrivateDataInterface(kRootLayoutId,data);data->Release();
    return SUCCEEDED(saved);
}
}
