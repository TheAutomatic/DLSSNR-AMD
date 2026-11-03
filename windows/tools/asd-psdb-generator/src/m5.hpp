// M5: the databases the ASI's D3D12 network runs from. CPU only.
//
//   --m5-build  ONE native.psdb + carrier.sodb + public-copy.psdb with the M2
//               identity and target holding
//                 * the canary group "g_pbptr", copied unchanged from the seed
//                   databases produced by --canary-seed: its SODB desc via
//                   FindPipelineStateDesc, its PSDB values via
//                   FindGroupValueKeys + FindValue (the ASI's built-in canary
//                   blob keeps matching);
//                 * every usable manifest record exactly as --m2-build makes it
//                   (carrier TAG=tag, CompilePipelineState, wrapped ACO ELF);
//               and net-carriers.bin: the root signature and, per record, tag,
//               dims, src, stem and the carrier DXIL as stored in the SODB, so
//               the ASI creates each PSO from exactly the stored desc.
// Format of net-carriers.bin (little endian):
//   "NRCAR001" u32 rs_bytes rs[...] u32 count
//   count x { u32 tag, u32 dims[3], u32 n, char stem[n], u32 n, char src[n], u32 n, u8 dxil[n] }
#pragma once
#include "m2.hpp"

namespace lab {
namespace m5 {

struct DescCopy {
    ID3D12StateObjectDatabase* dst = nullptr;
    HRESULT hr = E_FAIL;
    bool called = false;
};
// The desc the database hands back is opaque (it is not the stream that was
// stored), so it is re-stored verbatim and not decoded here.
inline void __stdcall copy_desc(const void* key, UINT key_size, UINT version,
                                const D3D12_PIPELINE_STATE_STREAM_DESC* desc, void* ctx) {
    auto* c = static_cast<DescCopy*>(ctx);
    c->called = true;
    c->hr = c->dst->StorePipelineStateDesc(key, key_size, version, desc);
}

inline void put_u32(Bytes& b, uint32_t v) { const uint8_t* p = reinterpret_cast<const uint8_t*>(&v); b.insert(b.end(), p, p + 4); }
inline void put_str(Bytes& b, const std::string& s) { put_u32(b, uint32_t(s.size())); b.insert(b.end(), s.begin(), s.end()); }

inline int build(const Args& args, const std::string& repo, const std::string& out) {
    std::filesystem::create_directories(out + "/keys");
    require(!args.get("manifest").empty(), "--manifest required (manifest.tsv from --m2-manifest)");
    require(!args.get("canary-sodb").empty() && !args.get("canary-psdb").empty(),
            "--canary-sodb/--canary-psdb required (produced by --canary-seed; rebuilds g_pbptr, no machine history)");
    // Work on copies: opening a database may write to it.
    std::filesystem::copy_file(args.get("canary-sodb"), out + "/canary-seed.sodb",
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(args.get("canary-psdb"), out + "/canary-seed.psdb",
                               std::filesystem::copy_options::overwrite_existing);
    for (const char* f : {"/native.psdb", "/carrier.sodb", "/public-copy.psdb"})
        std::filesystem::remove(out + f);

    gen::Paths paths = gen::resolve_paths(args, repo);
    gen::require_paths(paths);
    gen::CompilerRuntime rt = gen::load_compiler(paths);
    gen::Target selected = gen::select_target(rt.factory.Get(), args);
    D3D12_COMPILER_TARGET target = selected.value;
    D3D12_APPLICATION_DESC app = {};
    fill_app_identity(args, app.pExeFilename, app.pName, app.Version.Version, app.pEngineName, app.EngineVersion.Version);
    auto flags = D3D12_COMPILER_VALUE_TYPE_FLAGS(D3D12_COMPILER_VALUE_TYPE_FLAGS_OBJECT_CODE |
                                                  D3D12_COMPILER_VALUE_TYPE_FLAGS_METADATA);
    auto dbpath = wide(out + "/native.psdb");
    D3D12_COMPILER_DATABASE_PATH path{flags, dbpath.c_str()};
    ComPtr<ID3D12CompilerCacheSession> session;
    check(rt.factory->CreateCompilerCacheSession(&path, 1, &target, &app, IID_PPV_ARGS(&session)), "native cache session");
    ComPtr<ID3D12Compiler> compiler;
    check(rt.factory->CreateCompiler(session.Get(), IID_PPV_ARGS(&compiler)), "CreateCompiler");
    ComPtr<ID3D12StateObjectDatabaseFactory> sodbFactory;
    check(D3D12GetInterface(CLSID_D3D12StateObjectFactory, IID_PPV_ARGS(&sodbFactory)), "SODB factory");
    auto sodbPath = wide(out + "/carrier.sodb");
    ComPtr<ID3D12StateObjectDatabase> sodb;
    check(sodbFactory->CreateStateObjectDatabaseFromFile(sodbPath.c_str(), D3D12_STATE_OBJECT_DATABASE_FLAG_NONE,
                                                          IID_PPV_ARGS(&sodb)), "Create SODB");
    check(sodb->SetApplicationDesc(&app), "SODB app desc");
    auto copiedPath = wide(out + "/public-copy.psdb");
    D3D12_COMPILER_DATABASE_PATH copyPath{flags, copiedPath.c_str()};
    ComPtr<ID3D12CompilerCacheSession> copied;
    check(rt.factory->CreateCompilerCacheSession(&copyPath, 1, &target, &app, IID_PPV_ARGS(&copied)), "copy cache session");

    Json j;
    j.obj(nullptr);
    j.s("api", "M5: canary rebuilt by --canary-seed + every M2 record, ONE PSDB + ONE SODB");
    j.u("abi", target.ABIVersion);
    j.s("family", selected.family);
    j.s("canary_seed_sodb", args.get("canary-sodb"));
    j.s("canary_seed_sodb_sha256", sha256_file(args.get("canary-sodb")));
    j.s("canary_seed_psdb", args.get("canary-psdb"));
    j.s("canary_seed_psdb_sha256", sha256_file(args.get("canary-psdb")));

    // 1. The canary, unchanged.
    {
        static const char kCanary[] = "g_pbptr";
        D3D12_COMPILER_CACHE_GROUP_KEY group{kCanary, UINT(sizeof kCanary)};
        auto seedSodbPath = wide(out + "/canary-seed.sodb");
        ComPtr<ID3D12StateObjectDatabase> old_sodb;
        check(sodbFactory->CreateStateObjectDatabaseFromFile(seedSodbPath.c_str(), D3D12_STATE_OBJECT_DATABASE_FLAG_NONE,
                                                              IID_PPV_ARGS(&old_sodb)), "open canary seed SODB");
        DescCopy dc;
        dc.dst = sodb.Get();
        check(old_sodb->FindPipelineStateDesc(group.pKey, group.KeySize, copy_desc, &dc), "canary SODB FindPipelineStateDesc(g_pbptr)");
        require(dc.called, "canary SODB has no g_pbptr desc");
        check(dc.hr, "StorePipelineStateDesc(g_pbptr)");
        j.b("canary_desc_found", dc.called);

        auto seedPsdbPath = wide(out + "/canary-seed.psdb");
        D3D12_COMPILER_DATABASE_PATH oldPath{flags, seedPsdbPath.c_str()};
        ComPtr<ID3D12CompilerCacheSession> old_psdb;
        check(rt.factory->CreateCompilerCacheSession(&oldPath, 1, &target, &app, IID_PPV_ARGS(&old_psdb)), "canary seed PSDB session");
        ValueCapture captured;
        check(old_psdb->FindGroupValueKeys(&group, nullptr, collect_key, &captured), "canary PSDB FindGroupValueKeys(g_pbptr)");
        require(!captured.keys.empty(), "canary PSDB has no g_pbptr keys");
        std::vector<D3D12_COMPILER_CACHE_VALUE_KEY> keys;
        for (size_t i = 0; i < captured.keys.size(); ++i) {
            const Bytes& kb = captured.keys[i];
            D3D12_COMPILER_CACHE_VALUE_KEY vkey{kb.data(), UINT(kb.size())};
            D3D12_COMPILER_CACHE_TYPED_VALUE values[2] = {};
            values[0].Type = D3D12_COMPILER_VALUE_TYPE_OBJECT_CODE;
            values[1].Type = D3D12_COMPILER_VALUE_TYPE_METADATA;
            check(old_psdb->FindValue(&vkey, values, 2, allocate_value, nullptr), "canary PSDB FindValue(g_pbptr)");
            std::vector<D3D12_COMPILER_CACHE_TYPED_CONST_VALUE> stored;
            for (auto& v : values)
                if (v.Value.pValue && v.Value.ValueSize) stored.push_back({v.Type, {v.Value.pValue, v.Value.ValueSize}});
            require(!stored.empty(), "g_pbptr values empty");
            check(copied->StoreValue(&vkey, stored.data(), UINT(stored.size())), "StoreValue(g_pbptr)");
            for (auto& v : values) {
                if (v.Value.pValue && v.Type == D3D12_COMPILER_VALUE_TYPE_OBJECT_CODE)
                    j.s("canary_code_sha256", sha256_hex(v.Value.pValue, v.Value.ValueSize));
                free(v.Value.pValue);
            }
            write_file(out + "/keys/canary-" + std::to_string(i) + ".key", kb);
            keys.push_back(vkey);
        }
        check(copied->StoreGroupValueKeys(&group, 1, keys.data(), UINT(keys.size())), "StoreGroupValueKeys(g_pbptr)");
        j.u("canary_keys", keys.size());
    }

    // 2. Every usable record, exactly as m2::build_psdb.
    Bytes rs = serialize_root_constants(1, 1);
    Bytes table;
    table.insert(table.end(), {'N', 'R', 'C', 'A', 'R', '0', '0', '1'});
    put_u32(table, uint32_t(rs.size()));
    table.insert(table.end(), rs.begin(), rs.end());
    Bytes body;
    uint32_t built = 0, skipped = 0;
    std::string jsonl;
    auto entries = m2::load_manifest_tsv(args.get("manifest"));
    for (auto& e : entries) {
        if (!e.note.empty()) { ++skipped; continue; }
        model::NrRecord rec = model::parse_nrpal001(read_file(m2::record_path(args, e).string()));
        rec.dims[0] = e.dims[0]; rec.dims[1] = e.dims[1]; rec.dims[2] = e.dims[2];
        CaseDef carrier;
        carrier.hlsl_path = paths.hlsl_dir + "/model_carrier.hlsl";
        carrier.threads = e.dims[0] * e.dims[1] * e.dims[2];
        carrier.extra_defines.push_back({"TAG", std::to_string(e.tag)});
        carrier.extra_defines.push_back({"NTX", std::to_string(e.dims[0])});
        carrier.extra_defines.push_back({"NTY", std::to_string(e.dims[1])});
        carrier.extra_defines.push_back({"NTZ", std::to_string(e.dims[2])});
        Bytes dxil = compile_hlsl(carrier, "");
        CompilerStream stream = {};
        stream.root.value = {rs.data(), rs.size()};
        stream.cs.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS;
        stream.cs.cs = {dxil.data(), dxil.size()};
        D3D12_PIPELINE_STATE_STREAM_DESC pso_desc{sizeof(stream), &stream};
        std::string group_name = "m2-" + std::to_string(e.tag);
        D3D12_COMPILER_CACHE_GROUP_KEY group{group_name.c_str(), UINT(group_name.size() + 1)};
        check(sodb->StorePipelineStateDesc(group.pKey, group.KeySize, 1, &pso_desc), "StorePipelineStateDesc");
        check(compiler->CompilePipelineState(&group, 1, &pso_desc), "CompilePipelineState");
        ValueCapture captured;
        check(session->FindGroupValueKeys(&group, nullptr, collect_key, &captured), "FindGroupValueKeys");
        require(captured.keys.size() == 1, e.file + " group key count " + std::to_string(captured.keys.size()));
        const Bytes& key = captured.keys[0];
        D3D12_COMPILER_CACHE_VALUE_KEY vkey{key.data(), UINT(key.size())};
        D3D12_COMPILER_CACHE_TYPED_VALUE values[2] = {};
        values[0].Type = D3D12_COMPILER_VALUE_TYPE_OBJECT_CODE;
        values[1].Type = D3D12_COMPILER_VALUE_TYPE_METADATA;
        check(session->FindValue(&vkey, values, 2, allocate_value, nullptr), "FindValue");
        Bytes carrier_elf, metadata_in;
        for (auto& value : values) {
            if (!value.Value.pValue || !value.Value.ValueSize) continue;
            const auto* p = static_cast<const uint8_t*>(value.Value.pValue);
            if (value.Type == D3D12_COMPILER_VALUE_TYPE_OBJECT_CODE) carrier_elf.assign(p, p + value.Value.ValueSize);
            else metadata_in.assign(p, p + value.Value.ValueSize);
        }
        for (auto& value : values) free(value.Value.pValue);
        require(!carrier_elf.empty() && !metadata_in.empty(), e.file + " compiler values empty");
        model::BuiltElf built_elf = model::build_model_elf(carrier_elf.data(), carrier_elf.size(), rec);
        Bytes metadata_out = metadata_in;
        bool metadata_replaced = model::retarget_metadata(metadata_out, rec, nullptr);
        const Bytes& meta = metadata_replaced ? metadata_out : metadata_in;
        std::vector<D3D12_COMPILER_CACHE_TYPED_CONST_VALUE> stored;
        stored.push_back({D3D12_COMPILER_VALUE_TYPE_OBJECT_CODE, {built_elf.elf.data(), UINT(built_elf.elf.size())}});
        stored.push_back({D3D12_COMPILER_VALUE_TYPE_METADATA, {meta.data(), UINT(meta.size())}});
        check(copied->StoreValue(&vkey, stored.data(), UINT(stored.size())), "StoreValue");
        check(copied->StoreGroupValueKeys(&group, 1, &vkey, 1), "StoreGroupValueKeys");
        write_file(out + "/keys/" + std::to_string(e.tag) + "-" + e.file + ".key", key);
        put_u32(body, e.tag);
        for (uint32_t d : e.dims) put_u32(body, d);
        put_str(body, e.file);
        put_str(body, e.src);
        put_u32(body, uint32_t(dxil.size()));
        body.insert(body.end(), dxil.begin(), dxil.end());
        jsonl += "{\"tag\":" + std::to_string(e.tag) + ",\"file\":\"" + e.file + "\",\"src\":\"" + e.src +
                 "\",\"shell\":\"" + e.shell + "\",\"key\":\"" + model::hex_bytes(key.data(), key.size()) +
                 "\",\"aco_elf_sha256\":\"" + sha256_hex(built_elf.elf.data(), built_elf.elf.size()) +
                 "\",\"dxil_sha256\":\"" + sha256_hex(dxil.data(), dxil.size()) + "\"}\n";
        ++built;
        if (built % 25 == 0) printf("m5 build: %u records\n", built);
    }
    put_u32(table, built);
    table.insert(table.end(), body.begin(), body.end());
    write_file(out + "/net-carriers.bin", table);
    // Committing the compiler cache database is deferred to session release;
    // do it while the process is alive so the next process can read it.
    // Commit the cache database while the process is alive.
    copied.Reset();
    session.Reset();
    write_file(out + "/build.jsonl", jsonl);
    j.u("records_total", entries.size());
    j.u("built", built);
    j.u("skipped", skipped);
    j.s("psdb", out + "/public-copy.psdb");
    j.s("sodb", out + "/carrier.sodb");
    j.s("carriers", out + "/net-carriers.bin");
    gen::report_toolchain(paths, j);
    gen::report_targets(rt.factory.Get(), j);
    j.s("classification", "PASS_M5_BUILD");
    j.end_obj();
    write_json(out + "/m5-build.json", j);
    printf("%s\n", j.str().c_str());
    return 0;
}

}  // namespace m5
}  // namespace lab
