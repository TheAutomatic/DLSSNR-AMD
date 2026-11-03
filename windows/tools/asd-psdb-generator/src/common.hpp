// Shared plumbing for the CPU-only PSDB generator: command line, app identity
// used by the SODB and the compiler cache sessions, and the PSO stream layout.
// No D3D12 device is ever created by this tool.
#pragma once
#include <windows.h>
#include <d3d12.h>

#include <map>
#include <string>
#include <vector>

namespace lab {

struct Args {
    std::map<std::string, std::string> kv;
    std::vector<std::string> positional;
    std::string get(const std::string& k, const std::string& d = "") const {
        auto it = kv.find(k);
        return it == kv.end() ? d : it->second;
    }
    bool has(const std::string& k) const { return kv.count(k) != 0; }
    uint32_t u32(const std::string& k, uint32_t d) const {
        auto it = kv.find(k);
        if (it == kv.end()) return d;
        return uint32_t(strtoul(it->second.c_str(), nullptr, 0));
    }
};

inline Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("--", 0) == 0) {
            std::string key = s.substr(2);
            std::string val;
            if (i + 1 < argc && argv[i + 1][0] != '-') { val = argv[++i]; }
            a.kv[key] = val;
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

inline std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

inline std::wstring wide(const std::string& s) { return utf8_to_wide(s); }

// The SODB application description and every compiler cache session must carry
// the same identity. Defaults reproduce the probe identity the M1/M5 databases
// were built with; override with --app-exe/--app-name/--app-version/
// --app-engine/--app-engine-version when the database belongs to another host.
inline void fill_app_identity(const Args& args, LPCWSTR& exe, LPCWSTR& name, UINT64& version, LPCWSTR& engine,
                              UINT64& engineVersion) {
    static std::wstring s_exe, s_name, s_engine;
    s_exe = utf8_to_wide(args.get("app-exe", "asd_probe.exe"));
    s_name = utf8_to_wide(args.get("app-name", "ACO public entry probe"));
    s_engine = utf8_to_wide(args.get("app-engine", "ACO lab"));
    exe = s_exe.c_str();
    name = s_name.c_str();
    engine = s_engine.c_str();
    version = std::stoull(args.get("app-version", "1"));
    engineVersion = std::stoull(args.get("app-engine-version", "1"));
}

// D3D12 pipeline state stream: serialized root signature + CS bytecode. The
// stream is stored verbatim in the SODB and replayed by the ASI.
struct alignas(void*) SerializedRootSub {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SERIALIZED_ROOT_SIGNATURE;
    D3D12_SERIALIZED_ROOT_SIGNATURE_DESC value = {};
};
struct alignas(void*) PpCsSub {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS;
    D3D12_SHADER_BYTECODE cs = {};
};
struct CompilerStream {
    SerializedRootSub root;
    PpCsSub cs;
};
static_assert(sizeof(void*) == 8, "the stored stream is the x64 layout");
static_assert(offsetof(SerializedRootSub, value) == 8, "serialized root signature subobject");
static_assert(offsetof(PpCsSub, cs) == 8, "CS subobject");

// FindGroupValueKeys callback capture: raw key bytes, one vector per key.
struct ValueCapture {
    std::vector<std::vector<uint8_t>> keys;
};
inline void __stdcall collect_key(const D3D12_COMPILER_CACHE_VALUE_KEY* key, void* context) {
    auto* c = static_cast<ValueCapture*>(context);
    auto* data = static_cast<const uint8_t*>(key->pKey);
    c->keys.emplace_back(data, data + key->KeySize);
}
inline void* __stdcall allocate_value(SIZE_T size, void*) { return malloc(size); }

}  // namespace lab
