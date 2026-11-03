// dlssnr-amd-asd-unregister.exe: removes the D3D12 passthrough's shader cache registrations (nr_d3d12_asd.cpp) of the
// game in a folder - every application entry of our installers (DLSSNR-AMD-ASD, DLSSNR-AMD-ASD-FINAL) whose exe is in
// that folder or below it. install.ps1 runs it when it uninstalls; another installer's entry is never touched.
//   dlssnr-amd-asd-unregister.exe <game folder>
// Exit 0: done (also when there was nothing to remove or the system has no registration API), 1: an entry could not be
// removed, 2: usage.
#include "nr_d3d12_asd_api.h"

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

namespace api = nr::pe::asd::api;

namespace {

struct Client final : api::InstallerClient {
    std::wstring installer;
    HRESULT STDMETHODCALLTYPE GetInstallerName(SIZE_T* length, wchar_t* name) override {
        if (!length) return E_INVALIDARG;
        const SIZE_T need = installer.size() + 1;
        if (!name) { *length = need; return S_OK; }
        if (*length < need) return E_INVALIDARG;
        std::memcpy(name, installer.c_str(), need * sizeof(wchar_t));
        *length = need;
        return S_OK;
    }
    api::Scope STDMETHODCALLTYPE GetInstallerScope() override { return api::kScopeUser; }
    HRESULT STDMETHODCALLTYPE HandleDriverUpdate(api::Installer*) override { return E_NOTIMPL; }
};

std::wstring full(const std::wstring& p) {
    wchar_t b[MAX_PATH * 4] = {};
    if (!GetFullPathNameW(p.c_str(), MAX_PATH * 4, b, nullptr)) return p;
    std::wstring s = b;
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}
bool under(const wchar_t* path, const std::wstring& folder) {
    if (!path) return false;
    const std::wstring p = full(path), f = folder + L"\\";
    return p.size() > f.size() && _wcsnicmp(p.c_str(), f.c_str(), f.size()) == 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) { std::fwprintf(stderr, L"usage: dlssnr-amd-asd-unregister.exe <game folder>\n"); return 2; }
    const std::wstring folder = full(argv[1]);
    const auto get = api::get_interface();
    if (!get) { std::wprintf(L"no shader cache registration on this system: nothing to remove\n"); return 0; }
    api::InstallerFactory* factory = nullptr;
    if (FAILED(get(api::kClsidInstallerFactory, api::kIidInstallerFactory, reinterpret_cast<void**>(&factory))) || !factory) {
        std::wprintf(L"no shader cache registration on this system: nothing to remove\n");
        return 0;
    }
    int rc = 0;
    UINT removed = 0;
    for (const wchar_t* name : {L"DLSSNR-AMD-ASD", L"DLSSNR-AMD-ASD-FINAL"}) {
        Client client;   // outlives the installer
        client.installer = name;
        api::Installer* installer = nullptr;
        if (FAILED(factory->CreateInstaller(&client, api::kIidInstaller, reinterpret_cast<void**>(&installer))) || !installer)
            continue;
        for (UINT i = installer->GetApplicationCount(); i-- > 0;) {
            api::Application* app = nullptr;
            if (FAILED(installer->GetApplication(i, api::kIidApplication, reinterpret_cast<void**>(&app))) || !app) continue;
            const wchar_t *path = nullptr, *by = nullptr;
            const bool ours = SUCCEEDED(app->GetInstallerName(&by)) && by && std::wcscmp(by, name) == 0;
            if (ours && SUCCEEDED(app->GetExePath(&path)) && under(path, folder)) {
                const std::wstring exe = path;
                const HRESULT hr = installer->RemoveApplication(app);
                std::wprintf(L"%ls: %ls (%ls)\n", SUCCEEDED(hr) ? L"removed" : L"could not remove", exe.c_str(), name);
                if (SUCCEEDED(hr)) ++removed; else rc = 1;
            }
            app->Release();
        }
        installer->Release();
    }
    factory->Release();
    std::wprintf(L"%u registration(s) removed\n", removed);
    return rc;
}
