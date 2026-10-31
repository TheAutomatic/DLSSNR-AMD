// The native D3D12 ASD backend's registration and canary (nr_d3d12_asd.hpp). Public APIs only: D3DSCR and the SODB
// reader through d3d12.dll's D3D12GetInterface, and the game's own D3D12 device and queue.
#include "nr_d3d12_asd.hpp"
#include "nr_d3d12_asd_api.h"
#include "nr_d3d12_asd_canary_blob.h"
#include "nr_pe_d3d12split.hpp"
#include "nr_pe_log.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace nr::pe::asd {
namespace {

using nr::pe::log;

std::mutex g_lock;            // the strings below
std::wstring g_folder, g_ini;
bool g_enabled = false, g_register = true, g_tint = false;
std::wstring g_sodb_name = L"dlssnr-amd-asd.sodb", g_psdb_name = L"dlssnr-amd-asd-gfx1201.psdb";
std::wstring g_family = L"AMD_NAVI48:gfx1201";
std::string g_registration = "not run", g_backend = "not run", g_network;
std::atomic<int> g_canary{int(Canary::kNotRun)};
std::atomic<bool> g_canary_started{false};
std::string g_canary_line = "not run";
std::atomic<uint64_t> g_runs{0}, g_fallbacks{0};
const wchar_t kInstallerName[] = L"DLSSNR-AMD-ASD";

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::string hr_text(HRESULT hr) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08lX", static_cast<unsigned long>(hr));
    return b;
}
std::wstring ini_string(const wchar_t* key, const std::wstring& def) {
    wchar_t v[512] = {};
    GetPrivateProfileStringW(L"D3D12ASD", key, def.c_str(), v, 512, g_ini.c_str());
    return v;
}
bool exists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
void set_registration(const std::string& s) {
    { std::lock_guard<std::mutex> g(g_lock); g_registration = s; }
    log("[asd] registration: %s", s.c_str());
}
std::wstring exe_path() {
    std::wstring p(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, p.data(), DWORD(p.size()));
    p.resize(n);
    return p;
}
bool same_path(const wchar_t* a, const std::wstring& b) {
    wchar_t fa[MAX_PATH * 4] = {}, fb[MAX_PATH * 4] = {};
    if (!GetFullPathNameW(a, MAX_PATH * 4, fa, nullptr) || !GetFullPathNameW(b.c_str(), MAX_PATH * 4, fb, nullptr))
        return _wcsicmp(a, b.c_str()) == 0;
    return _wcsicmp(fa, fb) == 0;
}
bool under_folder(const wchar_t* path) {
    if (!path) return false;
    const std::wstring f = g_folder + L"\\";
    return _wcsnicmp(path, f.c_str(), f.size()) == 0;
}
template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct Client final : api::InstallerClient {
    std::wstring installer = kInstallerName;
    api::Scope scope = api::kScopeUser;
    HRESULT STDMETHODCALLTYPE GetInstallerName(SIZE_T* length, wchar_t* name) override {
        if (!length) return E_INVALIDARG;
        const SIZE_T need = installer.size() + 1;
        if (!name) { *length = need; return S_OK; }
        if (*length < need) return E_INVALIDARG;
        std::memcpy(name, installer.c_str(), need * sizeof(wchar_t));
        *length = need;
        return S_OK;
    }
    api::Scope STDMETHODCALLTYPE GetInstallerScope() override { return scope; }
    HRESULT STDMETHODCALLTYPE HandleDriverUpdate(api::Installer*) override { return E_NOTIMPL; }
};

// The SODB's application identity: the registration must give the runtime the same one the SODB (and the PSDB made
// with it) was stored with.
struct Identity {
    std::wstring exe, name, engine;
    UINT64 version = 0, engine_version = 0;
    bool found = false;
};
void __stdcall take_identity(const api::D3D12AppDesc* d, void* context) {
    auto* id = static_cast<Identity*>(context);
    if (!d) return;
    id->exe = d->pExeFilename ? d->pExeFilename : L"";
    id->name = d->pName ? d->pName : L"";
    id->engine = d->pEngineName ? d->pEngineName : L"";
    id->version = d->Version.Version;
    id->engine_version = d->EngineVersion.Version;
    id->found = true;
}
// PLACEHOLDER: the lab canary's identity (the M4 compile sets the product's, here and in its SODB/PSDB alike).
Identity builtin_identity() {
    Identity id;
    id.exe = L"asd_probe.exe"; id.name = L"ACO public entry probe"; id.version = 1;
    id.engine = L"ACO lab"; id.engine_version = 1; id.found = true;
    return id;
}
api::StateObjectDatabase* open_sodb(api::PFN_GetInterface get, std::string* why) {
    api::StateObjectDatabaseFactory* factory = nullptr;
    HRESULT hr = get(api::kClsidStateObjectFactory, api::kIidStateObjectDatabaseFactory, reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) { *why = "D3D12GetInterface(CLSID_D3D12StateObjectFactory) " + hr_text(hr); return nullptr; }
    api::StateObjectDatabase* db = nullptr;
    hr = factory->CreateStateObjectDatabaseFromFile(sodb_path().c_str(), api::kDatabaseReadOnly, api::kIidStateObjectDatabase,
                                                    reinterpret_cast<void**>(&db));
    factory->Release();
    if (FAILED(hr) || !db) { *why = "CreateStateObjectDatabaseFromFile(read only) " + hr_text(hr); return nullptr; }
    return db;
}

// Earlier registrations of this exe that are ours (by our installer's name, or a component whose SODB is in our
// folder) are removed; one of another installer's (a store's own ASD for the game) is left as it is, and then nothing
// is registered over it. Returns the number removed; `foreign` set when such an entry is there.
UINT remove_ours(api::Installer* installer, const std::wstring& exe, bool* foreign, std::string* why,
                 const wchar_t* mine = kInstallerName) {
    UINT removed = 0;
    *foreign = false;
    for (UINT i = installer->GetApplicationCount(); i-- > 0;) {
        api::Application* app = nullptr;
        if (FAILED(installer->GetApplication(i, api::kIidApplication, reinterpret_cast<void**>(&app))) || !app) continue;
        const wchar_t* path = nullptr;
        if (FAILED(app->GetExePath(&path)) || !path || !same_path(path, exe)) { app->Release(); continue; }
        bool ours = false;
        const wchar_t* by = nullptr;
        if (SUCCEEDED(app->GetInstallerName(&by)) && by && (std::wcscmp(by, mine) == 0 || std::wcsncmp(by, kInstallerName, 14) == 0)) ours = true;
        for (UINT c = 0, n = app->GetComponentCount(); !ours && c < n; ++c) {
            api::Component* comp = nullptr;
            if (FAILED(app->GetComponent(c, api::kIidComponent, reinterpret_cast<void**>(&comp))) || !comp) continue;
            const wchar_t* sodb = nullptr;
            if (SUCCEEDED(comp->GetStateObjectDatabasePath(&sodb)) && under_folder(sodb)) ours = true;
            comp->Release();
        }
        if (!ours) {
            *foreign = true;
            log("[asd] registration: an entry for this exe by another installer (%s) is left as it is",
                by ? utf8(by).c_str() : "unknown");
            app->Release();
            continue;
        }
        const HRESULT hr = installer->RemoveApplication(app);
        log("[asd] registration: RemoveApplication(earlier entry of ours) %s", hr_text(hr).c_str());
        if (SUCCEEDED(hr)) ++removed;
        else *why = "RemoveApplication " + hr_text(hr);
        app->Release();
    }
    return removed;
}

api::Installer* make_installer(api::PFN_GetInterface get, Client& client, api::InstallerFactory** factory_out, std::string* why) {
    api::InstallerFactory* factory = nullptr;
    HRESULT hr = get(api::kClsidInstallerFactory, api::kIidInstallerFactory, reinterpret_cast<void**>(&factory));
    log("[asd] registration: D3D12GetInterface(CLSID_D3DShaderCacheInstallerFactory) %s", hr_text(hr).c_str());
    if (FAILED(hr) || !factory) { *why = "no D3DSCR installer factory (" + hr_text(hr) + ")"; return nullptr; }
    api::Installer* installer = nullptr;
    hr = factory->CreateInstaller(&client, api::kIidInstaller, reinterpret_cast<void**>(&installer));
    log("[asd] registration: CreateInstaller(%s scope) %s", client.scope == api::kScopeSystem ? "system" : "user", hr_text(hr).c_str());
    if (FAILED(hr) || !installer) { factory->Release(); *why = "CreateInstaller " + hr_text(hr); return nullptr; }
    *factory_out = factory;
    return installer;
}

// One installer at a time (a second live installer of the same name fails with ERROR_ALREADY_EXISTS), and an
// Unregister made while the registration still runs wins: it waits for it, then removes what it registered.
std::mutex g_installer_lock;
DWORD registration_work();
DWORD WINAPI registration_thread(void*) {
    std::lock_guard<std::mutex> installer(g_installer_lock);
    {
        std::lock_guard<std::mutex> g(g_lock);
        if (!g_register) { g_registration = "not registered: Register = 0 (NR Control's Unregister)"; return 0; }
    }
    return registration_work();
}
// The diagnostic options of the M5 v2 package ([D3D12ASD] in the ini):
//   CopyToLocal = 1 (default): the SODB and PSDB copied to %LOCALAPPDATA%\dlssnr-amd-asd and registered from there
//                (not from the game's folder: a WindowsApps/XboxGames folder may not be readable where the runtime reads it)
//   ExePath = both (default) | module | final: the exe path registered - GetModuleFileNameW's (C:\Program Files\WindowsApps\...
//             for a store game), the final path of the exe's file (C:\XboxGames\...\Content\..., the path the Xbox app's
//             own ASD entry uses), or both (one entry each, under installer DLSSNR-AMD-ASD and DLSSNR-AMD-ASD-FINAL)
//   RemoveFirst = 1 (default): earlier entries of ours for the exe removed before registering; 0 registers over them
std::wstring final_exe_path() {
    const std::wstring exe = exe_path();
    HANDLE f = CreateFileW(exe.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (f == INVALID_HANDLE_VALUE) return exe;
    std::wstring p(32768, L'\0');
    const DWORD n = GetFinalPathNameByHandleW(f, p.data(), DWORD(p.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(f);
    if (!n || n >= p.size()) return exe;
    p.resize(n);
    if (p.rfind(L"\\\\?\\UNC\\", 0) == 0) p = L"\\" + p.substr(7);
    else if (p.rfind(L"\\\\?\\", 0) == 0) p = p.substr(4);
    return p;
}
bool copy_to_local(std::wstring* sodb, std::wstring* psdb) {
    wchar_t base[MAX_PATH] = L"";
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return false;
    const std::wstring dir = std::wstring(base) + L"\\dlssnr-amd-asd";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring s2 = dir + L"\\" + g_sodb_name, p2 = dir + L"\\" + g_psdb_name;
    const BOOL a = CopyFileW(sodb_path().c_str(), s2.c_str(), FALSE), b = CopyFileW(psdb_path().c_str(), p2.c_str(), FALSE);
    log("[asd] registration: CopyToLocal: %s -> %s (%s), %s -> %s (%s)", utf8(sodb_path()).c_str(), utf8(s2).c_str(),
        a ? "ok" : ("error " + std::to_string(GetLastError())).c_str(), utf8(psdb_path()).c_str(), utf8(p2).c_str(),
        b ? "ok" : "error");
    if (!a || !b) return false;
    *sodb = s2;
    *psdb = p2;
    return true;
}
std::string register_one(api::PFN_GetInterface get, const wchar_t* installer_name, const std::wstring& exe,
                         const std::wstring& sodb, const std::wstring& psdb, bool remove_first, bool core_loaded);
DWORD registration_work() {
    const std::wstring exe = exe_path();
    const bool d3d12_loaded = GetModuleHandleW(L"d3d12.dll") != nullptr;
    const bool core_loaded = GetModuleHandleW(L"D3D12Core.dll") != nullptr;
    log("[asd] registration for %s: d3d12.dll %s, D3D12Core.dll %s before it (%s)", utf8(exe).c_str(),
        d3d12_loaded ? "loaded" : "not loaded", core_loaded ? "loaded" : "not loaded",
        core_loaded ? "the game may have its D3D12 device already: then this launch misses and the next one hits"
                    : "no D3D12 device yet: this launch can hit");
    if (!exists(sodb_path()) || !exists(psdb_path())) {
        set_registration("not registered: " + utf8(sodb_path()) + " or " + utf8(psdb_path()) + " is missing");
        return 0;
    }
    const auto get = api::get_interface();
    if (!get) { set_registration("not registered: d3d12.dll has no D3D12GetInterface"); return 0; }
    std::string why;
    // The identity the SODB and PSDB were made with (asd_probe's fill_app_identity), built in: the in-box runtime that
    // games with an older Agility SDK run on has no state object database reader (CLSID_D3D12StateObjectFactory), so
    // it is not read from the SODB (the M1 615/in-box runs did not read it either).
    const Identity id = builtin_identity();
    log("[asd] registration: identity (built in): exe \"%s\", name \"%s\" v%llu, engine \"%s\" v%llu", utf8(id.exe).c_str(),
        utf8(id.name).c_str(), (unsigned long long)id.version, utf8(id.engine).c_str(), (unsigned long long)id.engine_version);
    if (api::StateObjectDatabase* db = open_sodb(get, &why)) {   // diagnostics only, where the runtime has the reader
        Identity sodb_id;
        db->GetApplicationDesc(&take_identity, &sodb_id);
        db->Release();
        if (sodb_id.found && (sodb_id.exe != id.exe || sodb_id.name != id.name || sodb_id.engine != id.engine ||
                              sodb_id.version != id.version || sodb_id.engine_version != id.engine_version))
            log("[asd] registration: warning: the SODB was stored with another identity (exe \"%s\", name \"%s\"): the PSDB "
                "will not be found", utf8(sodb_id.exe).c_str(), utf8(sodb_id.name).c_str());
    }

    std::wstring sodb = sodb_path(), psdb = psdb_path();
    if (GetPrivateProfileIntW(L"D3D12ASD", L"CopyToLocal", 1, g_ini.c_str()) == 1 && !copy_to_local(&sodb, &psdb))
        log("[asd] registration: CopyToLocal failed: the game folder's files are registered");
    const std::wstring mode = ini_string(L"ExePath", L"both");
    const bool remove_first = GetPrivateProfileIntW(L"D3D12ASD", L"RemoveFirst", 1, g_ini.c_str()) != 0;
    const std::wstring final_exe = final_exe_path();
    log("[asd] registration: ExePath = %s, RemoveFirst = %d; module path %s, final path %s", utf8(mode).c_str(),
        remove_first ? 1 : 0, utf8(exe).c_str(), utf8(final_exe).c_str());
    std::string result;
    if (mode != L"final") result = register_one(get, kInstallerName, exe, sodb, psdb, remove_first, core_loaded);
    if (mode == L"final" || (mode == L"both" && !same_path(final_exe.c_str(), exe))) {
        const std::string r2 = register_one(get, L"DLSSNR-AMD-ASD-FINAL", final_exe, sodb, psdb, remove_first, core_loaded);
        result = result.empty() ? r2 : result + " | final path: " + r2;
    }
    set_registration(result);
    return 0;
}
std::string register_one(api::PFN_GetInterface get, const wchar_t* installer_name, const std::wstring& exe,
                         const std::wstring& sodb, const std::wstring& psdb, bool remove_first, bool core_loaded) {
    const Identity id = builtin_identity();
    std::string why;
    log("[asd] registration: installer %s, exe %s", utf8(installer_name).c_str(), utf8(exe).c_str());
    Client client;   // outlives the installer
    client.installer = installer_name;
    api::InstallerFactory* factory = nullptr;
    api::Installer* installer = make_installer(get, client, &factory, &why);
    if (!installer) return "not registered: " + why;
    const UINT before = installer->GetApplicationCount();
    bool foreign = false;
    const UINT removed = remove_first ? remove_ours(installer, exe, &foreign, &why, installer_name) : 0;
    std::string result;
    if (foreign) {
        result = "not registered: this exe has an ASD entry of another installer (left as it is)";
    } else {
        api::ApplicationDesc desc{};
        desc.pExeFilename = id.exe.c_str();
        desc.pName = id.name.c_str();
        desc.Version.Version = id.version;
        desc.pEngineName = id.engine.c_str();
        desc.EngineVersion.Version = id.engine_version;
        api::Application* app = nullptr;
        HRESULT hr = installer->RegisterApplication(exe.c_str(), &desc, api::kIidApplication, reinterpret_cast<void**>(&app));
        log("[asd] registration: RegisterApplication %s", hr_text(hr).c_str());
        if (FAILED(hr) || !app) {
            result = "RegisterApplication failed " + hr_text(hr);
        } else {
            bool reported = false;
            const UINT count = app->GetPrecompileTargetCount(api::kTargetNone);
            std::vector<api::CompilerProperties> targets(count);
            if (count) {
                hr = app->GetPrecompileTargets(count, targets.data(), api::kTargetNone);
                log("[asd] registration: GetPrecompileTargets(%u) %s", count, hr_text(hr).c_str());
            }
            for (const auto& t : targets) {
                log("[asd] registration: target %s, ABI %llu..%llu, compiler 0x%016llX, app profile 0x%016llX",
                    utf8(t.szAdapterFamily).c_str(), (unsigned long long)t.MinimumABISupportVersion,
                    (unsigned long long)t.MaximumABISupportVersion, (unsigned long long)t.CompilerVersion.Version,
                    (unsigned long long)t.ApplicationProfileVersion.Version);
                reported |= g_family == t.szAdapterFamily;
            }
            if (!reported)
                log("[asd] registration: the system reports no target %s (the PSDB's family): expect ASD_MISS",
                    utf8(g_family).c_str());
            const api::PsdbProperties property{g_family.c_str(), psdb.c_str()};
            api::Component* component = nullptr;
            hr = app->RegisterComponent(L"default", sodb.c_str(), 1, &property, api::kIidComponent,
                                        reinterpret_cast<void**>(&component));
            log("[asd] registration: RegisterComponent(default) %s", hr_text(hr).c_str());
            if (SUCCEEDED(hr) && component) {
                const wchar_t* readback = nullptr;
                const HRESULT rb = component->GetPrecompiledCachePath(g_family.c_str(), &readback);
                log("[asd] registration: GetPrecompiledCachePath %s: %s", hr_text(rb).c_str(),
                    readback ? utf8(readback).c_str() : "");
                result = std::string("registered (") + (core_loaded ? "D3D12 was loaded already: hits from the next launch"
                                                                    : "before the game's device") + ")";
                component->Release();
            } else {
                result = "RegisterComponent failed " + hr_text(hr);
            }
            app->Release();
        }
    }
    const UINT after = installer->GetApplicationCount();
    installer->Release();   // before the game's device, where it can be
    factory->Release();
    char counts[96];
    std::snprintf(counts, sizeof counts, "; apps %u -> %u, %u earlier entr%s removed", before, after, removed,
                  removed == 1 ? "y" : "ies");
    return result + counts;
}

DWORD WINAPI unregister_thread(void*) {
    WritePrivateProfileStringW(L"D3D12ASD", L"Register", L"0", g_ini.c_str());
    { std::lock_guard<std::mutex> g(g_lock); g_register = false; }
    std::lock_guard<std::mutex> installer_lock(g_installer_lock);
    const auto get = api::get_interface();
    if (!get) { set_registration("unregister: d3d12.dll has no D3D12GetInterface"); return 0; }
    std::string why;
    bool foreign = false;
    UINT removed = 0;
    for (const wchar_t* name : {kInstallerName, L"DLSSNR-AMD-ASD-FINAL"}) {
        Client client;
        client.installer = name;
        api::InstallerFactory* factory = nullptr;
        api::Installer* installer = make_installer(get, client, &factory, &why);
        if (!installer) { set_registration("unregister: " + why); return 0; }
        bool f = false;
        removed += remove_ours(installer, exe_path(), &f, &why, name);
        if (final_exe_path() != exe_path()) removed += remove_ours(installer, final_exe_path(), &f, &why, name);
        foreign |= f;
        installer->Release();
        factory->Release();
    }
    char line[160];
    std::snprintf(line, sizeof line, "unregistered (%u entr%s removed; Register = 0: no launch registers again)%s", removed,
                  removed == 1 ? "y" : "ies", foreign ? "; another installer's entry left as it is" : "");
    set_registration(line);
    return 0;
}

// ---- the canary: the M1 pbptr carrier (lab src/main.cpp exec_pbptr) on the game's device --------------------------
constexpr uint32_t kN = 128, kWords = 32, kDescWord3 = 0x22014facu;
void fill_push(uint32_t* p) {
    p[0] = 3; p[1] = 0x21; p[2] = kN; p[3] = 0x100;
    for (uint32_t k = 4; k < kWords; ++k) p[k] = 0x30 + k;
}
uint32_t aco_value(uint32_t i, uint32_t in, const uint32_t* p) {
    uint32_t x = in ^ (p[3] + i);
    for (uint32_t k = 4; k < kWords; ++k) x ^= p[k] + (i << (k & 7));
    return x * p[0] + p[1] + i;
}
void put_desc(uint32_t* d, uint64_t va, uint32_t bytes) {
    d[0] = uint32_t(va); d[1] = uint32_t((va >> 32) & 0xffffu); d[2] = bytes; d[3] = kDescWord3;
}

struct Pipeline { std::vector<uint8_t> root, cs; UINT flags = 0, node_mask = 0; bool found = false; std::string why; };
// A pipeline's stream as the runtime keeps it in the SODB: the runtime's normalised form (every subobject type, most
// at their defaults), each { type, value } aligned to a pointer (CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT), the value
// at its own alignment. Sizes are x64 D3D12 (Agility 1.619) structs.
bool value_layout(int type, size_t* offset, size_t* size) {
    size_t o = 4, n = 0;
    switch (type) {
    case 0: o = 8; n = 8; break;                                      // ROOT_SIGNATURE (a pointer of the storing process)
    case 1: case 2: case 3: case 4: case 5: case 6: case 24: case 25: // VS PS DS HS GS CS AS MS: D3D12_SHADER_BYTECODE
    case 19: case 29: o = 8; n = 16; break;                           // CACHED_PSO, SERIALIZED_ROOT_SIGNATURE
    case 7: o = 8; n = 32; break;                                     // STREAM_OUTPUT
    case 8: n = 328; break;                                           // BLEND
    case 9: case 13: case 14: case 16: case 18: case 20: n = 4; break; // SAMPLE_MASK IB_STRIP_CUT TOPOLOGY DSV_FORMAT NODE_MASK FLAGS
    case 10: case 27: n = 44; break;                                  // RASTERIZER, RASTERIZER1
    case 28: n = 40; break;                                           // RASTERIZER2
    case 11: n = 52; break;                                           // DEPTH_STENCIL
    case 21: n = 56; break;                                           // DEPTH_STENCIL1
    case 26: n = 60; break;                                           // DEPTH_STENCIL2
    case 12: o = 8; n = 16; break;                                    // INPUT_LAYOUT
    case 15: n = 36; break;                                           // RENDER_TARGET_FORMATS
    case 17: n = 8; break;                                            // SAMPLE_DESC
    case 22: o = 8; n = 24; break;                                    // VIEW_INSTANCING
    default: return false;
    }
    *offset = o; *size = n; return true;
}
void __stdcall take_pipeline(const void*, UINT, UINT, const D3D12_PIPELINE_STATE_STREAM_DESC* d, void* context) {
    auto* p = static_cast<Pipeline*>(context);
    if (!d || !d->pPipelineStateSubobjectStream) { p->why = "empty stream"; return; }
    const auto* base = static_cast<const uint8_t*>(d->pPipelineStateSubobjectStream);
    size_t at = 0;
    while (at + sizeof(int) <= d->SizeInBytes) {
        int type = 0;
        std::memcpy(&type, base + at, sizeof type);
        size_t offset = 0, size = 0;
        if (!value_layout(type, &offset, &size)) {
            p->why = "unknown subobject type " + std::to_string(type) + " in the canary's stream";
            return;
        }
        const size_t value = at + offset;
        if (value + size > d->SizeInBytes) { p->why = "the stream ends inside a subobject"; return; }
        if (type == 18 || type == 20) {
            std::memcpy(type == 18 ? &p->node_mask : &p->flags, base + value, 4);
        } else if ((type >= 1 && type <= 5) || type == 24 || type == 25) {
            SIZE_T bytes = 0;
            std::memcpy(&bytes, base + value + sizeof(void*), sizeof bytes);
            if (bytes) { p->why = "a graphics/mesh shader (subobject type " + std::to_string(type) + ") in the canary's stream"; return; }
        } else if (type == api::kSubobjectSerializedRootSignature || type == api::kSubobjectCs) {
            const void* blob = nullptr;
            SIZE_T bytes = 0;
            std::memcpy(&blob, base + value, sizeof blob);
            std::memcpy(&bytes, base + value + sizeof(void*), sizeof bytes);
            auto& into = type == api::kSubobjectCs ? p->cs : p->root;
            if (blob && bytes) into.assign(static_cast<const uint8_t*>(blob), static_cast<const uint8_t*>(blob) + bytes);
        }
        at = (value + size + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
    }
    p->found = !p->root.empty() && !p->cs.empty();
    if (!p->found) p->why = "no serialized root signature or CS in the stream";
}

ID3D12Resource* buffer(ID3D12Device* device, D3D12_HEAP_TYPE heap, uint64_t size, D3D12_RESOURCE_STATES state, bool uav) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* r = nullptr;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    return r;
}

}  // namespace

bool sodb_pipeline(const char* key, std::vector<uint8_t>* root, std::vector<uint8_t>* cs, unsigned* flags,
                   unsigned* node_mask, std::string* why) {
    const auto get = api::get_interface();
    if (!get) { *why = "d3d12.dll has no D3D12GetInterface"; return false; }
    api::StateObjectDatabase* db = open_sodb(get, why);
    if (!db) return false;
    Pipeline pl;
    const HRESULT hr = db->FindPipelineStateDesc(key, UINT(std::strlen(key) + 1), &take_pipeline, &pl);
    db->Release();
    if (FAILED(hr)) { *why = std::string("FindPipelineStateDesc(") + key + ") " + hr_text(hr); return false; }
    if (!pl.found) { *why = pl.why; return false; }
    *root = pl.root; *cs = pl.cs; *flags = pl.flags; *node_mask = pl.node_mask;
    return true;
}

namespace {

// Diagnostics, logged once before the canary (all public APIs): this process's package identity, the D3D12Core.dll in
// use, whether the PSDB and SODB can be read by this process, and the registration the D3DSCR explorer finds for this
// exe - the entry the runtime goes by (its installer and PSDB paths), or none.
std::string module_path(const wchar_t* name) {
    HMODULE m = GetModuleHandleW(name);
    if (!m) return "not loaded";
    std::wstring p(32768, L'\0');
    p.resize(GetModuleFileNameW(m, p.data(), DWORD(p.size())));
    return utf8(p);
}
#ifndef APPMODEL_ERROR_NO_PACKAGE
#define APPMODEL_ERROR_NO_PACKAGE 15700L
#endif
std::string package_name() {
    using Fn = LONG(WINAPI*)(UINT32*, PWSTR);
    auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetCurrentPackageFullName")));
    if (!fn) return "unknown (no GetCurrentPackageFullName)";
    UINT32 n = 0;
    LONG r = fn(&n, nullptr);
    if (r == APPMODEL_ERROR_NO_PACKAGE) return "none (not a packaged process)";
    std::wstring w(n, L'\0');
    r = fn(&n, w.data());
    if (r != ERROR_SUCCESS) return "unknown (error " + std::to_string(r) + ")";
    w.resize(n ? n - 1 : 0);
    return utf8(w);
}
std::string readable(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "NOT readable (error " + std::to_string(GetLastError()) + ")";
    char b[16];
    DWORD got = 0;
    const BOOL ok = ReadFile(f, b, sizeof b, &got, nullptr);
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    CloseHandle(f);
    return ok && got ? "readable (" + std::to_string(size.QuadPart) + " bytes)" : "opened, read failed";
}
void log_lookup(ID3D12Device* device) {
    log("[asd] diag: package identity: %s", package_name().c_str());
    log("[asd] diag: D3D12Core.dll in use: %s; d3d12.dll: %s", module_path(L"D3D12Core.dll").c_str(),
        module_path(L"d3d12.dll").c_str());
    log("[asd] diag: PSDB %s: %s", utf8(psdb_path()).c_str(), readable(psdb_path()).c_str());
    log("[asd] diag: SODB %s: %s", utf8(sodb_path()).c_str(), readable(sodb_path()).c_str());
    {
        wchar_t base[MAX_PATH] = L"";
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) {
            const std::wstring p2 = std::wstring(base) + L"\\dlssnr-amd-asd\\" + g_psdb_name;
            log("[asd] diag: local PSDB %s: %s", utf8(p2).c_str(), readable(p2).c_str());
        }
    }
    const auto get = api::get_interface();
    if (!get) return;
    api::InstallerFactory* factory = nullptr;
    HRESULT hr = get(api::kClsidInstallerFactory, api::kIidInstallerFactory, reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) { log("[asd] diag: no D3DSCR factory %s", hr_text(hr).c_str()); return; }
    api::Explorer* explorer = nullptr;
    hr = factory->CreateExplorer(device, api::kIidExplorer, reinterpret_cast<void**>(&explorer));
    if (FAILED(hr) || !explorer) {
        log("[asd] diag: CreateExplorer(device) %s", hr_text(hr).c_str());
        hr = factory->CreateExplorer(nullptr, api::kIidExplorer, reinterpret_cast<void**>(&explorer));
    }
    log("[asd] diag: CreateExplorer %s", hr_text(hr).c_str());
    if (SUCCEEDED(hr) && explorer) {
        const std::wstring exe = exe_path();
        api::Application* app = nullptr;
        hr = explorer->GetApplicationFromExePath(exe.c_str(), api::kIidApplication, reinterpret_cast<void**>(&app));
        log("[asd] diag: explorer GetApplicationFromExePath(%s) %s", utf8(exe).c_str(), hr_text(hr).c_str());
        if (SUCCEEDED(hr) && app) {
            const wchar_t *by = nullptr, *path = nullptr;
            app->GetInstallerName(&by);
            app->GetExePath(&path);
            api::ApplicationDesc d{};
            app->GetDesc(&d);
            log("[asd] diag: the runtime's entry for this exe: installer \"%s\", exe path %s, app \"%s\" (exe name \"%s\")",
                by ? utf8(by).c_str() : "?", path ? utf8(path).c_str() : "?", d.pName ? utf8(d.pName).c_str() : "?",
                d.pExeFilename ? utf8(d.pExeFilename).c_str() : "?");
            for (UINT c = 0, n = app->GetComponentCount(); c < n; ++c) {
                api::Component* comp = nullptr;
                if (FAILED(app->GetComponent(c, api::kIidComponent, reinterpret_cast<void**>(&comp))) || !comp) continue;
                const wchar_t *cn = nullptr, *sodb = nullptr;
                comp->GetComponentName(&cn);
                comp->GetStateObjectDatabasePath(&sodb);
                const UINT np = comp->GetPrecompiledShaderDatabaseCount();
                std::vector<api::PsdbProperties> ps(np);
                if (np) comp->GetPrecompiledShaderDatabases(np, ps.data());
                log("[asd] diag:   component \"%s\", SODB %s, %u PSDB(s)", cn ? utf8(cn).c_str() : "?",
                    sodb ? utf8(sodb).c_str() : "", np);
                for (const auto& x : ps)
                    log("[asd] diag:     %s: %s", x.pAdapterFamily ? utf8(x.pAdapterFamily).c_str() : "?",
                        x.pPsdbPath ? utf8(x.pPsdbPath).c_str() : "?");
                comp->Release();
            }
            app->Release();
        }
        explorer->Release();
    }
    factory->Release();
}

struct CanaryJob { ID3D12Device* device; ID3D12CommandQueue* queue; };
void finish_canary(Canary c, const std::string& line) {
    { std::lock_guard<std::mutex> g(g_lock); g_canary_line = std::string(canary_name(c)) + ": " + line; }
    g_canary = int(c);
    log("[asd] canary %s: %s%s", canary_name(c), line.c_str(),
        c == Canary::kHit ? " - the D3D12 ASD backend may run" : " - the D3D12 ASD backend stays off (the 0.0.8.1 path runs)");
}

DWORD WINAPI canary_thread(void* p) {
    CanaryJob job = *static_cast<CanaryJob*>(p);
    delete static_cast<CanaryJob*>(p);
    ID3D12Device* device = job.device;
    log_lookup(device);
    // The pipeline exactly as stored in the SODB (its serialized root signature and DXIL, built into the ASI: no SODB
    // reader needed, as asd_probe's --execute in the M1 615/in-box runs): the runtime looks the PSDB up by this desc.
    Pipeline pl;
    pl.root.assign(canary_blob::kRoot, canary_blob::kRoot + sizeof canary_blob::kRoot);
    pl.cs.assign(canary_blob::kCs, canary_blob::kCs + sizeof canary_blob::kCs);
    pl.flags = canary_blob::kFlags;
    pl.node_mask = canary_blob::kNodeMask;
    pl.found = true;
    std::string why;
    ID3D12RootSignature* rs = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12Resource *in = nullptr, *out = nullptr, *table = nullptr, *readback = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE event = nullptr;
    Canary result = Canary::kFail;
    std::string line;
    do {
        if (!pl.found) { line = why; break; }
        HRESULT hr = device->CreateRootSignature(0, pl.root.data(), pl.root.size(), IID_PPV_ARGS(&rs));
        if (FAILED(hr)) { line = "CreateRootSignature " + hr_text(hr); break; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = rs;
        pd.CS.pShaderBytecode = pl.cs.data();
        pd.CS.BytecodeLength = pl.cs.size();
        pd.NodeMask = pl.node_mask;
        pd.Flags = D3D12_PIPELINE_STATE_FLAGS(pl.flags);
        const ULONGLONG t0 = GetTickCount64();
        hr = device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
        log("[asd] canary: CreateComputePipelineState %s in %llu ms", hr_text(hr).c_str(),
            (unsigned long long)(GetTickCount64() - t0));
        if (FAILED(hr)) { line = "CreateComputePipelineState " + hr_text(hr); break; }
        in = buffer(device, D3D12_HEAP_TYPE_UPLOAD, kN * 4, D3D12_RESOURCE_STATE_GENERIC_READ, false);
        table = buffer(device, D3D12_HEAP_TYPE_UPLOAD, 4096, D3D12_RESOURCE_STATE_GENERIC_READ, false);
        out = buffer(device, D3D12_HEAP_TYPE_DEFAULT, kN * 4, D3D12_RESOURCE_STATE_COMMON, true);
        readback = buffer(device, D3D12_HEAP_TYPE_READBACK, kN * 4, D3D12_RESOURCE_STATE_COPY_DEST, false);
        if (!in || !table || !out || !readback) { line = "the canary's buffers could not be made"; break; }
        uint32_t input[kN], push[kWords] = {};
        for (uint32_t i = 0; i < kN; ++i) input[i] = 0x11110000u + i;
        fill_push(push);
        const uint64_t in_va = in->GetGPUVirtualAddress(), out_va = out->GetGPUVirtualAddress();
        const uint64_t table_va = table->GetGPUVirtualAddress(), push_va = table_va + 256;
        if ((in_va >> 48) || (out_va >> 48) || (table_va >> 32) != (push_va >> 32)) { line = "buffer addresses out of range"; break; }
        void* map = nullptr;
        const D3D12_RANGE none{0, 0};
        if (FAILED(in->Map(0, &none, &map))) { line = "Map(input)"; break; }
        std::memcpy(map, input, sizeof input);
        in->Unmap(0, nullptr);
        if (FAILED(table->Map(0, &none, &map))) { line = "Map(table)"; break; }
        std::memset(map, 0, 4096);
        put_desc(static_cast<uint32_t*>(map), in_va, kN * 4);
        put_desc(reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(map) + 16), out_va, kN * 4);
        std::memcpy(static_cast<uint8_t*>(map) + 256, push, sizeof push);
        table->Unmap(0, nullptr);
        uint32_t roots[11] = {};
        roots[0] = uint32_t(table_va);
        roots[1] = uint32_t(push_va);
        for (uint32_t i = 0; i < 8; ++i) roots[2 + i] = push[i];
        roots[10] = uint32_t(table_va >> 32);

        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) ||
            !(event = CreateEventW(nullptr, FALSE, FALSE, nullptr))) { line = "the canary's command list could not be made"; break; }
        list->SetComputeRootSignature(rs);
        list->SetPipelineState(pso);
        list->SetComputeRoot32BitConstants(0, 11, roots, 0);
        list->SetComputeRootUnorderedAccessView(1, out_va);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = out;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        list->ResourceBarrier(1, &b);
        list->Dispatch(2, 1, 1);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b);
        list->CopyBufferRegion(readback, 0, out, 0, kN * 4);
        if (FAILED(list->Close())) { line = "Close(the canary's list)"; break; }
        ID3D12CommandList* lists[] = {list};
        job.queue->ExecuteCommandLists(1, lists);
        if (FAILED(job.queue->Signal(fence, 1)) || FAILED(fence->SetEventOnCompletion(1, event))) { line = "Signal"; break; }
        if (WaitForSingleObject(event, 5000) != WAIT_OBJECT_0) {
            line = "the GPU did not finish the canary within 5 s";
            // what the GPU may still use is left as it is (never released under it)
            in = out = table = readback = nullptr; list = nullptr; alloc = nullptr; pso = nullptr; rs = nullptr;
            break;
        }
        if (FAILED(readback->Map(0, nullptr, &map))) { line = "Map(readback)"; break; }
        uint32_t got[kN];
        std::memcpy(got, map, sizeof got);
        readback->Unmap(0, nullptr);
        uint32_t acc = 0;
        for (uint32_t r : roots) acc += r;
        uint32_t aco_mis = 0, hlsl_mis = 0, first = ~0u;
        for (uint32_t i = 0; i < kN; ++i) {
            if (got[i] != aco_value(i, input[i], push)) { ++aco_mis; if (first == ~0u) first = i; }
            if (got[i] != (acc ^ i)) ++hlsl_mis;
        }
        char text[200];
        std::snprintf(text, sizeof text, "%u values: %u differ from the ACO kernel's, %u from the HLSL carrier's (first ACO "
                      "difference at %d)", kN, aco_mis, hlsl_mis, first == ~0u ? -1 : int(first));
        line = text;
        result = aco_mis == 0 ? Canary::kHit : hlsl_mis == 0 ? Canary::kMiss : Canary::kFail;
    } while (false);
    if (event) CloseHandle(event);
    release(fence); release(list); release(alloc); release(readback); release(table); release(out); release(in);
    release(pso); release(rs);
    job.queue->Release();
    device->Release();
    finish_canary(result, line);
    return 0;
}

}  // namespace

const char* canary_name(Canary c) {
    switch (c) {
    case Canary::kHit: return "ASD_HIT";
    case Canary::kMiss: return "ASD_MISS";
    case Canary::kFail: return "ASD_FAIL";
    case Canary::kRunning: return "running";
    default: return "not run";
    }
}

const std::wstring& folder() { return g_folder; }
std::wstring ini_path() { return g_ini; }
std::wstring sodb_path() { return g_folder + L"\\" + g_sodb_name; }
std::wstring psdb_path() { return g_folder + L"\\" + g_psdb_name; }
bool enabled() { return g_enabled; }
bool debug_tint() { return g_tint; }
Canary canary() { return Canary(g_canary.load()); }

void start(HMODULE self) { start(self, true); }
void start(HMODULE self, bool register_now) {
    std::wstring here(32768, L'\0');
    here.resize(GetModuleFileNameW(self, here.data(), DWORD(here.size())));
    const size_t slash = here.find_last_of(L"\\/");
    here = slash == std::wstring::npos ? L"." : here.substr(0, slash);
    g_folder = here + L"\\dlssnr-amd-asd";
    g_ini = g_folder + L"\\dlssnr-amd-asd.ini";
    if (!exists(g_ini)) return;   // no folder, no ini: off, and not a line in the log (0.0.8.1 as it was)
    g_enabled = GetPrivateProfileIntW(L"D3D12ASD", L"Enabled", 0, g_ini.c_str()) == 1;
    g_register = GetPrivateProfileIntW(L"D3D12ASD", L"Register", 1, g_ini.c_str()) != 0;
    g_tint = GetPrivateProfileIntW(L"D3D12ASD", L"DebugTint", 0, g_ini.c_str()) == 1;
    g_sodb_name = ini_string(L"Sodb", g_sodb_name);
    g_psdb_name = ini_string(L"Psdb", g_psdb_name);
    g_family = ini_string(L"AdapterFamily", g_family);
    log("[asd] D3D12 ASD backend %s (%s: Enabled %d, Register %d)", g_enabled ? "on" : "off", utf8(g_ini).c_str(),
        g_enabled ? 1 : 0, g_register ? 1 : 0);
    if (!g_enabled || !register_now) return;
    if (!g_register) { set_registration("not registered: Register = 0 (NR Control's Unregister)"); return; }
    // Under the loader lock here: the thread runs once OptiScaler's DllMain has returned (as the watchdog's).
    if (HANDLE t = CreateThread(nullptr, 0, &registration_thread, nullptr, 0, nullptr)) CloseHandle(t);
    else set_registration("not registered: no thread");
}

void on_device(ID3D12Device* device) {
    if (!g_enabled || !device || g_canary_started.load()) return;
    // The game's direct queue when this thread has handed it frames (as NR's Present layers find it); otherwise a
    // compute queue of ours on the game's device. Never a device of ours.
    ID3D12CommandQueue* queue = nr::pe::split::last_direct_queue();
    if (queue) {
        ID3D12Device* owner = nullptr;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&owner))) || !owner) queue = nullptr;
        else {
            if (owner != device) queue = nullptr;
            owner->Release();
        }
    }
    if (g_canary_started.exchange(true)) return;
    g_canary = int(Canary::kRunning);
    if (queue) {
        queue->AddRef();
    } else {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        const HRESULT hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
        if (FAILED(hr) || !queue) { finish_canary(Canary::kFail, "CreateCommandQueue " + hr_text(hr)); return; }
        log("[asd] canary: on a queue of ours (the game's direct queue is not known on this thread)");
    }
    device->AddRef();
    auto* job = new CanaryJob{device, queue};
    if (HANDLE t = CreateThread(nullptr, 0, &canary_thread, job, 0, nullptr)) {
        CloseHandle(t);
    } else {
        queue->Release();
        device->Release();
        delete job;
        finish_canary(Canary::kFail, "no thread");
    }
}

// Tools (dlssnr-amd-asd-reg.exe, run elevated once): the same entry at SYSTEM scope for `exe`, the files copied to
// C:\ProgramData\dlssnr-amd-asd. A packaged game with a system-scope ASD entry of its store (the Xbox app's
// GRTS-XboxContent-*) is looked up there; one entry per exe is used, by installer name order.
std::string system_registration(const std::wstring& exe, bool remove_only, const wchar_t* installer_name) {
    const auto get = api::get_interface();
    if (!get) return "no D3D12GetInterface";
    const std::wstring dir = L"C:\\ProgramData\\dlssnr-amd-asd";
    const std::wstring sodb = dir + L"\\" + g_sodb_name, psdb = dir + L"\\" + g_psdb_name;
    if (!remove_only) {
        CreateDirectoryW(dir.c_str(), nullptr);
        if (!CopyFileW(sodb_path().c_str(), sodb.c_str(), FALSE) || !CopyFileW(psdb_path().c_str(), psdb.c_str(), FALSE))
            return "copy to " + utf8(dir) + " failed (error " + std::to_string(GetLastError()) + ")";
    }
    const Identity id = builtin_identity();
    std::string why;
    Client client;
    client.scope = api::kScopeSystem;
    client.installer = installer_name;
    api::InstallerFactory* factory = nullptr;
    api::Installer* installer = make_installer(get, client, &factory, &why);
    if (!installer) return why + " (run it as administrator)";
    bool foreign = false;
    const UINT removed = remove_ours(installer, exe, &foreign, &why, installer_name);
    std::string result = std::string("installer ") + utf8(installer_name) + ", SODB " + utf8(sodb) + ", PSDB " + utf8(psdb) + "; removed " + std::to_string(removed);
    if (!remove_only) {
        api::ApplicationDesc desc{};
        desc.pExeFilename = id.exe.c_str();
        desc.pName = id.name.c_str();
        desc.Version.Version = id.version;
        desc.pEngineName = id.engine.c_str();
        desc.EngineVersion.Version = id.engine_version;
        api::Application* app = nullptr;
        HRESULT hr = installer->RegisterApplication(exe.c_str(), &desc, api::kIidApplication, reinterpret_cast<void**>(&app));
        result += "; RegisterApplication(system) " + hr_text(hr);
        if (SUCCEEDED(hr) && app) {
            const api::PsdbProperties property{g_family.c_str(), psdb.c_str()};
            api::Component* component = nullptr;
            hr = app->RegisterComponent(L"default", sodb.c_str(), 1, &property, api::kIidComponent,
                                        reinterpret_cast<void**>(&component));
            result += "; RegisterComponent " + hr_text(hr);
            if (component) component->Release();
            app->Release();
        }
    }
    installer->Release();
    factory->Release();
    return result;
}

void unregister_async() {
    if (g_ini.empty()) return;
    if (HANDLE t = CreateThread(nullptr, 0, &unregister_thread, nullptr, 0, nullptr)) CloseHandle(t);
}
void set_enabled(bool on) {
    if (g_ini.empty()) return;
    WritePrivateProfileStringW(L"D3D12ASD", L"Enabled", on ? L"1" : L"0", g_ini.c_str());
    log("[asd] Enabled = %d written (from the next launch)", on ? 1 : 0);
}
void allow_register() {
    if (g_ini.empty()) return;
    WritePrivateProfileStringW(L"D3D12ASD", L"Register", L"1", g_ini.c_str());
    { std::lock_guard<std::mutex> g(g_lock); g_register = true; }
    log("[asd] Register = 1 written (registers from the next launch)");
}

Status status() {
    Status s;
    std::lock_guard<std::mutex> g(g_lock);
    s.enabled = g_enabled;
    s.register_on = g_register;
    s.files_found = !g_folder.empty() && exists(sodb_path()) && exists(psdb_path());
    s.folder = utf8(g_folder);
    s.registration = g_registration;
    s.canary = g_canary_line;
    s.backend = g_backend;
    s.network = g_network;
    s.runs = g_runs.load();
    s.fallbacks = g_fallbacks.load();
    return s;
}
void note_backend(const char* line, bool ran) {
    (ran ? g_runs : g_fallbacks)++;
    std::lock_guard<std::mutex> g(g_lock);
    if (g_backend != line) {
        g_backend = line;
        log("[asd] backend: %s", line);
    }
}
void note_network(const char* name) {
    std::lock_guard<std::mutex> g(g_lock);
    g_network = name ? name : "";
}

}  // namespace nr::pe::asd
