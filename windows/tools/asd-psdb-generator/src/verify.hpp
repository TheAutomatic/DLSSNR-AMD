// Integrity check for a generated set: opens the SODB and PSDB again through the
// official interfaces, walks every group the manifest names, and writes a
// canonical dump of what the databases actually contain. The PSDB file bytes are
// not reproducible run to run (database header/ephemeral pages), the canonical
// dump is: compare it to prove two builds hold the same keys and values.
#pragma once
#include <filesystem>
#include <map>

#include "common.hpp"
#include "compiler.hpp"
#include "genenv.hpp"
#include "m2.hpp"

namespace lab {
namespace verify {

struct DescCapture {
    bool called = false;
};

inline void __stdcall capture_desc(const void* key, UINT key_size, UINT version,
                                   const D3D12_PIPELINE_STATE_STREAM_DESC* desc, void* ctx) {
    (void)key;
    (void)key_size;
    (void)version;
    (void)desc;
    static_cast<DescCapture*>(ctx)->called = true;
}

struct CarrierEntry {
    uint32_t tag = 0, dims[3] = {0, 0, 0};
    std::string stem, src;
    Bytes dxil;
};

inline uint32_t rd_u32(const Bytes& b, size_t& p) {
    require(p + 4 <= b.size(), "carriers truncated");
    uint32_t v;
    memcpy(&v, b.data() + p, 4);
    p += 4;
    return v;
}

inline std::vector<CarrierEntry> read_carriers(const Bytes& b) {
    require(b.size() >= 8 && memcmp(b.data(), "NRCAR001", 8) == 0, "carriers magic");
    size_t p = 8;
    uint32_t rs = rd_u32(b, p);
    require(p + rs <= b.size(), "carriers root signature truncated");
    p += rs;
    uint32_t count = rd_u32(b, p);
    std::vector<CarrierEntry> out;
    for (uint32_t i = 0; i < count; ++i) {
        CarrierEntry e;
        e.tag = rd_u32(b, p);
        for (uint32_t& d : e.dims) d = rd_u32(b, p);
        uint32_t n = rd_u32(b, p);
        require(p + n <= b.size(), "carriers stem truncated");
        e.stem.assign(reinterpret_cast<const char*>(b.data() + p), n);
        p += n;
        n = rd_u32(b, p);
        require(p + n <= b.size(), "carriers src truncated");
        e.src.assign(reinterpret_cast<const char*>(b.data() + p), n);
        p += n;
        n = rd_u32(b, p);
        require(p + n <= b.size(), "carriers dxil truncated");
        e.dxil.assign(b.data() + p, b.data() + p + n);
        p += n;
        out.push_back(std::move(e));
    }
    require(p == b.size(), "carriers trailing bytes");
    return out;
}

inline int run(const Args& args, const std::string& repo, const std::string& out) {
    std::filesystem::create_directories(out);
    require(!args.get("manifest").empty() && !args.get("sodb").empty() && !args.get("psdb").empty() &&
                !args.get("carriers").empty(),
            "--manifest, --sodb, --psdb, --carriers required");
    gen::Paths paths = gen::resolve_paths(args, repo);
    gen::require_paths(paths);
    gen::CompilerRuntime rt = gen::load_compiler(paths);
    gen::Target selected = gen::select_target(rt.factory.Get(), args);
    D3D12_COMPILER_TARGET target = selected.value;
    D3D12_APPLICATION_DESC app = {};
    fill_app_identity(args, app.pExeFilename, app.pName, app.Version.Version, app.pEngineName, app.EngineVersion.Version);
    auto flags = D3D12_COMPILER_VALUE_TYPE_FLAGS(D3D12_COMPILER_VALUE_TYPE_FLAGS_OBJECT_CODE |
                                                  D3D12_COMPILER_VALUE_TYPE_FLAGS_METADATA);

    ComPtr<ID3D12StateObjectDatabaseFactory> sodbFactory;
    check(D3D12GetInterface(CLSID_D3D12StateObjectFactory, IID_PPV_ARGS(&sodbFactory)), "SODB factory");
    ComPtr<ID3D12StateObjectDatabase> sodb;
    check(sodbFactory->CreateStateObjectDatabaseFromFile(wide(args.get("sodb")).c_str(),
                                                          D3D12_STATE_OBJECT_DATABASE_FLAG_NONE,
                                                          IID_PPV_ARGS(&sodb)),
          "open SODB");
    std::wstring psdbFile = wide(args.get("psdb"));   // must outlive psdbPath
    D3D12_COMPILER_DATABASE_PATH psdbPath{flags, psdbFile.c_str()};
    ComPtr<ID3D12CompilerCacheSession> psdb;
    check(rt.factory->CreateCompilerCacheSession(&psdbPath, 1, &target, &app, IID_PPV_ARGS(&psdb)), "open PSDB");
    // Probe the canary group once so an empty/mismatched database fails with a
    // clear message before the 176-group walk.
    {
        static const char kCanaryProbe[] = "g_pbptr";
        D3D12_COMPILER_CACHE_GROUP_KEY probeGroup{kCanaryProbe, UINT(sizeof kCanaryProbe)};
        ValueCapture probeKeys;
        HRESULT probe = psdb->FindGroupValueKeys(&probeGroup, nullptr, collect_key, &probeKeys);
        require(SUCCEEDED(probe),
                "PSDB has no g_pbptr group (empty database, wrong family/ABI, or wrong app identity): " +
                    hr_name(probe));
    }

    auto entries = m2::load_manifest_tsv(args.get("manifest"));
    auto carriers = read_carriers(read_file(args.get("carriers")));
    require(carriers.size() == entries.size(), "carrier entry count != manifest records");

    std::map<uint32_t, const CarrierEntry*> by_tag;
    for (const auto& c : carriers) by_tag[c.tag] = &c;
    std::string canonical;
    size_t groups_ok = 0, groups_bad = 0;
    Json j;
    j.obj(nullptr);
    j.s("api", "reopen SODB + PSDB, walk every manifest group, canonical value dump");
    j.u("abi", target.ABIVersion);
    j.s("family", selected.family);
    j.arr("groups");
    auto check_group = [&](const std::string& name, const std::string& stem, const std::string& src) {
        D3D12_COMPILER_CACHE_GROUP_KEY group{name.c_str(), UINT(name.size() + 1)};
        DescCapture dc;
        check(sodb->FindPipelineStateDesc(group.pKey, group.KeySize, capture_desc, &dc), "FindPipelineStateDesc");
        ValueCapture captured;
        check(psdb->FindGroupValueKeys(&group, nullptr, collect_key, &captured), "FindGroupValueKeys");
        std::string codesha, metasha, keysha;
        size_t value_bytes = 0;
        if (!captured.keys.empty()) keysha = sha256_hex(captured.keys[0].data(), captured.keys[0].size());
        for (size_t i = 0; i < captured.keys.size(); ++i) {
            D3D12_COMPILER_CACHE_VALUE_KEY key{captured.keys[i].data(), UINT(captured.keys[i].size())};
            D3D12_COMPILER_CACHE_TYPED_VALUE values[2] = {};
            values[0].Type = D3D12_COMPILER_VALUE_TYPE_OBJECT_CODE;
            values[1].Type = D3D12_COMPILER_VALUE_TYPE_METADATA;
            check(psdb->FindValue(&key, values, 2, allocate_value, nullptr), "FindValue");
            for (auto& v : values) {
                if (!v.Value.pValue || !v.Value.ValueSize) continue;
                value_bytes += v.Value.ValueSize;
                if (v.Type == D3D12_COMPILER_VALUE_TYPE_OBJECT_CODE)
                    codesha = sha256_hex(v.Value.pValue, v.Value.ValueSize);
                else
                    metasha = sha256_hex(v.Value.pValue, v.Value.ValueSize);
                free(v.Value.pValue);
            }
        }
        bool ok = dc.called && captured.keys.size() == 1 && !codesha.empty() && !metasha.empty();
        if (ok) ++groups_ok; else ++groups_bad;
        canonical += "{\"group\":\"" + name + "\",\"stem\":\"" + stem + "\",\"src\":\"" + src +
                     "\",\"key\":\"" + keysha + "\",\"code\":\"" + codesha + "\",\"meta\":\"" + metasha + "}\n";
        j.obj(nullptr);
        j.s("group", name);
        j.b("desc_found", dc.called);
        j.u("keys", captured.keys.size());
        j.s("key_sha256", keysha);
        j.s("code_sha256", codesha);
        j.s("metadata_sha256", metasha);
        j.s("carrier_stem", stem);
        j.s("carrier_src", src);
        j.u("value_bytes", value_bytes);
        j.end_obj();
    };
    for (const auto& e : entries) {
        if (!e.note.empty()) continue;
        auto it = by_tag.find(e.tag);
        require(it != by_tag.end(), "carrier missing for tag " + std::to_string(e.tag));
        require(it->second->stem == e.file && it->second->src == e.src, "carrier stem/src mismatch");
        check_group("m2-" + std::to_string(e.tag), e.file, e.src);
    }
    j.end_arr();
    // Canary group.
    {
        static const char kCanary[] = "g_pbptr";
        D3D12_COMPILER_CACHE_GROUP_KEY group{kCanary, UINT(sizeof kCanary)};
        DescCapture dc;
        sodb->FindPipelineStateDesc(group.pKey, group.KeySize, capture_desc, &dc);
        ValueCapture captured;
        psdb->FindGroupValueKeys(&group, nullptr, collect_key, &captured);
        std::string codesha;
        for (size_t i = 0; i < captured.keys.size(); ++i) {
            D3D12_COMPILER_CACHE_VALUE_KEY key{captured.keys[i].data(), UINT(captured.keys[i].size())};
            D3D12_COMPILER_CACHE_TYPED_VALUE values[2] = {};
            values[0].Type = D3D12_COMPILER_VALUE_TYPE_OBJECT_CODE;
            values[1].Type = D3D12_COMPILER_VALUE_TYPE_METADATA;
            if (FAILED(psdb->FindValue(&key, values, 2, allocate_value, nullptr))) continue;
            for (auto& v : values) {
                if (v.Value.pValue && v.Type == D3D12_COMPILER_VALUE_TYPE_OBJECT_CODE)
                    codesha = sha256_hex(v.Value.pValue, v.Value.ValueSize);
                if (v.Value.pValue) free(v.Value.pValue);
            }
        }
        canonical = std::string("{\"group\":\"g_pbptr\",\"key\":\"") +
                    (captured.keys.empty() ? "" : sha256_hex(captured.keys[0].data(), captured.keys[0].size())) +
                    "\",\"code\":\"" + codesha + "\"}\n" + canonical;
        j.obj("canary");
        j.b("desc_found", dc.called);
        j.u("keys", captured.keys.size());
        j.s("code_sha256", codesha);
        j.end_obj();
        if (!dc.called || captured.keys.empty() || codesha.empty()) ++groups_bad;
    }
    write_file(out + "/canonical.jsonl", canonical);
    j.u("groups_ok", groups_ok);
    j.u("groups_bad", groups_bad);
    j.s("canonical_sha256", sha256_hex(canonical.data(), canonical.size()));
    j.s("classification", groups_bad == 0 && groups_ok == carriers.size() ? "PASS_VERIFY" : "FAIL_VERIFY");
    j.end_obj();
    write_json(out + "/verify.json", j);
    printf("%s\n", j.str().c_str());
    return groups_bad == 0 ? 0 : 1;
}

}  // namespace verify
}  // namespace lab
