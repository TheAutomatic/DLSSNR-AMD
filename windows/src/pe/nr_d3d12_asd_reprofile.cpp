// See nr_d3d12_asd_reprofile.hpp. Public APIs only: D3D12CompilerCreateFactory, ID3D12CompilerFactory /
// ID3D12Compiler / ID3D12CompilerCacheSession (d3d12compiler.h) and the SODB reader (D3D12GetInterface).
#include "nr_d3d12_asd_reprofile.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "nr_d3d12_asd_compiler_api.h"

namespace nr::pe::asd {
namespace {

std::string u8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::string hx(HRESULT hr) { char b[16]; std::snprintf(b, sizeof b, "0x%08X", unsigned(hr)); return b; }
std::string hx64(UINT64 v) { char b[24]; std::snprintf(b, sizeof b, "0x%016llX", (unsigned long long)v); return b; }
std::string hexbytes(const std::vector<uint8_t>& v) {
    std::string s; char b[4];
    for (uint8_t c : v) { std::snprintf(b, sizeof b, "%02x", c); s += b; }
    return s;
}
template <class T> void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

void* __stdcall alloc_value(SIZE_T n, void*) { return std::malloc(n ? n : 1); }
void __stdcall collect_key(const capi::ValueKey* k, void* context) {
    auto* keys = static_cast<std::vector<std::vector<uint8_t>>*>(context);
    const auto* p = static_cast<const uint8_t*>(k->pKey);
    keys->emplace_back(p, p + k->KeySize);
}
struct CompileCall {
    capi::Compiler* compiler = nullptr;
    const capi::GroupKey* group = nullptr;
    HRESULT hr = E_FAIL;
    UINT version = 0;
    bool called = false;
};
void __stdcall compile_from_sodb(const void*, UINT, UINT version, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, void* context) {
    auto* c = static_cast<CompileCall*>(context);
    c->called = true;
    c->version = version;
    c->hr = desc ? c->compiler->CompilePipelineState(c->group, version, desc) : E_POINTER;
}
// Object code + metadata of one value.
struct Values { std::vector<uint8_t> code, meta; HRESULT hr = E_FAIL; };
Values find_value(capi::CompilerCacheSession* s, const std::vector<uint8_t>& key) {
    Values v;
    const capi::ValueKey k{key.data(), UINT(key.size())};
    capi::TypedValue t[2] = {};
    t[0].Type = capi::kValueObjectCode;
    t[1].Type = capi::kValueMetadata;
    v.hr = s->FindValue(&k, t, 2, &alloc_value, nullptr);
    for (auto& x : t) {
        if (SUCCEEDED(v.hr) && x.Value.pValue && x.Value.ValueSize) {
            const auto* p = static_cast<const uint8_t*>(x.Value.pValue);
            (x.Type == capi::kValueObjectCode ? v.code : v.meta).assign(p, p + x.Value.ValueSize);
        }
        std::free(x.Value.pValue);
    }
    return v;
}

}  // namespace

ReprofileResult reprofile_psdb(const ReprofileJob& job, bool profile_only) {
    ReprofileResult r;
    auto fail = [&](const std::string& why) { r.text += (r.text.empty() ? "" : "; ") + why; r.ok = false; return r; };
    auto note = [&](const std::string& s) { r.text += (r.text.empty() ? "" : "; ") + s; };

    HMODULE m = LoadLibraryExW(job.compiler_dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!m) return fail("cannot load " + u8(job.compiler_dll) + " (error " + std::to_string(GetLastError()) + ")");
    auto create = reinterpret_cast<capi::PFN_CreateFactory>(reinterpret_cast<void*>(GetProcAddress(m, "D3D12CompilerCreateFactory")));
    if (!create) return fail("no D3D12CompilerCreateFactory export");
    capi::CompilerFactory* factory = nullptr;
    HRESULT hr = create(job.plugin_dll.c_str(), capi::kIidCompilerFactory, reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) return fail("D3D12CompilerCreateFactory(" + u8(job.plugin_dll) + ") " + hx(hr));

    capi::Target target{};
    bool found = false;
    for (UINT i = 0; i < 256 && !found; ++i) {
        capi::AdapterFamily f{};
        if (FAILED(factory->EnumerateAdapterFamilies(i, &f))) break;
        if (job.family != f.szAdapterFamily) continue;
        UINT32 n = 0;
        if (FAILED(factory->EnumerateAdapterFamilyABIVersions(i, &n, nullptr)) || !n) continue;
        std::vector<UINT64> abi(n);
        if (FAILED(factory->EnumerateAdapterFamilyABIVersions(i, &n, abi.data())) || !n) continue;
        target = {i, abi[n - 1]};   // the newest ABI, as the source PSDB (asd_probe's compile)
        found = true;
    }
    if (!found) { rel(factory); return fail("the plugin has no family " + u8(job.family)); }
    api::VersionNumber cv{};
    factory->EnumerateAdapterFamilyCompilerVersion(target.AdapterFamilyIndex, &cv);

    api::ApplicationDesc src{}, dst{};
    src.pExeFilename = job.src_exe.c_str(); dst.pExeFilename = job.exe.c_str();
    src.pName = dst.pName = job.name.c_str();
    src.Version.Version = dst.Version.Version = job.version;
    src.pEngineName = dst.pEngineName = job.engine.c_str();
    src.EngineVersion.Version = dst.EngineVersion.Version = job.engine_version;
    api::VersionNumber p0{}, p1{};
    const HRESULT h0 = factory->GetApplicationProfileVersion(&target, &src, &p0);
    const HRESULT h1 = factory->GetApplicationProfileVersion(&target, &dst, &p1);
    r.src_profile = p0.Version; r.new_profile = p1.Version;
    note("plugin " + u8(job.family) + " ABI " + std::to_string(target.ABIVersion) + " compiler " + hx64(cv.Version) +
         "; profile \"" + u8(job.src_exe) + "\" " + hx64(p0.Version) + " (" + hx(h0) + "), \"" + u8(job.exe) + "\" " +
         hx64(p1.Version) + " (" + hx(h1) + ")");
    if (FAILED(h0) || FAILED(h1)) { rel(factory); return fail("GetApplicationProfileVersion failed"); }
    if (profile_only) { rel(factory); r.ok = true; return r; }

    // The SODB (pipeline descs): through d3d12.dll's D3D12GetInterface.
    const auto get = api::get_interface();
    if (!get) { rel(factory); return fail("d3d12.dll has no D3D12GetInterface"); }
    api::StateObjectDatabaseFactory* sf = nullptr;
    api::StateObjectDatabase* sodb = nullptr;
    hr = get(api::kClsidStateObjectFactory, api::kIidStateObjectDatabaseFactory, reinterpret_cast<void**>(&sf));
    if (SUCCEEDED(hr) && sf)
        hr = sf->CreateStateObjectDatabaseFromFile(job.sodb.c_str(), api::kDatabaseReadOnly, api::kIidStateObjectDatabase,
                                                   reinterpret_cast<void**>(&sodb));
    rel(sf);
    if (FAILED(hr) || !sodb) { rel(factory); return fail("SODB " + u8(job.sodb) + " " + hx(hr) + " (needs a runtime with the SODB reader)"); }

    // Sessions: the source PSDB (a temporary copy, never written), the plugin's compile for the new identity
    // (temporary), and the output.
    const std::wstring src_copy = job.out_psdb + L".src.tmp", native = job.out_psdb + L".native.tmp", out_tmp = job.out_psdb + L".tmp";
    DeleteFileW(src_copy.c_str()); DeleteFileW(native.c_str()); DeleteFileW(out_tmp.c_str());
    if (!CopyFileW(job.src_psdb.c_str(), src_copy.c_str(), FALSE)) {
        rel(sodb); rel(factory);
        return fail("cannot copy " + u8(job.src_psdb) + " (error " + std::to_string(GetLastError()) + ")");
    }
    const auto flags = capi::ValueTypeFlags(capi::kFlagsObjectCode | capi::kFlagsMetadata);
    const capi::DatabasePath ps{flags, src_copy.c_str()}, pn{flags, native.c_str()}, po{flags, out_tmp.c_str()};
    capi::CompilerCacheSession *ss = nullptr, *ns = nullptr, *os = nullptr;
    capi::Compiler* compiler = nullptr;
    HRESULT a = factory->CreateCompilerCacheSession(&ps, 1, &target, &src, capi::kIidCompilerCacheSession, reinterpret_cast<void**>(&ss));
    HRESULT b = factory->CreateCompilerCacheSession(&pn, 1, &target, &dst, capi::kIidCompilerCacheSession, reinterpret_cast<void**>(&ns));
    HRESULT c = factory->CreateCompilerCacheSession(&po, 1, &target, &dst, capi::kIidCompilerCacheSession, reinterpret_cast<void**>(&os));
    HRESULT d = ns ? factory->CreateCompiler(ns, capi::kIidCompiler, reinterpret_cast<void**>(&compiler)) : E_FAIL;
    bool good = SUCCEEDED(a) && SUCCEEDED(b) && SUCCEEDED(c) && SUCCEEDED(d) && ss && ns && os && compiler;
    if (!good) note("sessions " + hx(a) + " " + hx(b) + " " + hx(c) + ", CreateCompiler " + hx(d));

    size_t stored = 0, same_key = 0;
    for (size_t g = 0; good && g < job.groups.size(); ++g) {
        const std::string& name = job.groups[g];
        const capi::GroupKey gk{name.c_str(), UINT(name.size() + 1)};
        CompileCall call;
        call.compiler = compiler; call.group = &gk;
        hr = sodb->FindPipelineStateDesc(name.c_str(), UINT(name.size() + 1), &compile_from_sodb, &call);
        if (FAILED(hr) || !call.called || FAILED(call.hr)) {
            note("group " + name + ": FindPipelineStateDesc " + hx(hr) + ", CompilePipelineState " + hx(call.hr));
            good = false; break;
        }
        std::vector<std::vector<uint8_t>> old_keys, new_keys;
        const HRESULT k0 = ss->FindGroupValueKeys(&gk, nullptr, &collect_key, &old_keys);
        const HRESULT k1 = ns->FindGroupValueKeys(&gk, nullptr, &collect_key, &new_keys);
        if (FAILED(k0) || FAILED(k1) || old_keys.empty() || old_keys.size() != new_keys.size()) {
            note("group " + name + ": value keys source " + hx(k0) + " x" + std::to_string(old_keys.size()) + ", new " + hx(k1) +
                 " x" + std::to_string(new_keys.size()));
            good = false; break;
        }
        std::vector<capi::ValueKey> keys;
        for (size_t i = 0; i < new_keys.size(); ++i) {
            const Values ov = find_value(ss, old_keys[i]), nv = find_value(ns, new_keys[i]);
            if (FAILED(ov.hr) || FAILED(nv.hr) || ov.code.empty()) {
                note("group " + name + " value " + std::to_string(i) + ": FindValue source " + hx(ov.hr) + ", new " + hx(nv.hr));
                good = false; break;
            }
            same_key += old_keys[i] == new_keys[i];
            // Our object code (the source PSDB's), the new profile's key and metadata.
            capi::TypedConstValue v[2] = {{capi::kValueObjectCode, {ov.code.data(), UINT(ov.code.size())}},
                                          {capi::kValueMetadata, {nv.meta.data(), UINT(nv.meta.size())}}};
            const capi::ValueKey k{new_keys[i].data(), UINT(new_keys[i].size())};
            hr = os->StoreValue(&k, v, nv.meta.empty() ? 1 : 2);
            note("group " + name + " value " + std::to_string(i) + ": key " + hexbytes(old_keys[i]) + " -> " + hexbytes(new_keys[i]) +
                 ", code " + std::to_string(ov.code.size()) + " B ours (plugin's " + std::to_string(nv.code.size()) + " B" +
                 (ov.code == nv.code ? ", IDENTICAL to ours" : "") + "), metadata " + hexbytes(ov.meta) + " -> " + hexbytes(nv.meta) +
                 ", StoreValue " + hx(hr));
            if (FAILED(hr)) { good = false; break; }
            ++stored;
        }
        if (!good) break;
        for (auto& k : new_keys) keys.push_back({k.data(), UINT(k.size())});
        hr = os->StoreGroupValueKeys(&gk, call.version, keys.data(), UINT(keys.size()));
        if (FAILED(hr)) { note("group " + name + ": StoreGroupValueKeys " + hx(hr)); good = false; }
    }
    rel(compiler); rel(ss); rel(ns); rel(os); rel(sodb); rel(factory);
    DeleteFileW(src_copy.c_str()); DeleteFileW(native.c_str());
    if (good && stored) {
        good = MoveFileExW(out_tmp.c_str(), job.out_psdb.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        if (!good) note("cannot write " + u8(job.out_psdb) + " (error " + std::to_string(GetLastError()) + ")");
    }
    DeleteFileW(out_tmp.c_str());
    r.ok = good && stored;
    note(std::to_string(stored) + " values, " + std::to_string(same_key) + " keys unchanged" + (r.ok ? ", written " + u8(job.out_psdb) : ", FAILED"));
    return r;
}

}  // namespace nr::pe::asd
