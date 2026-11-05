// Canary seed: rebuild the g_pbptr PSDB/SODB pair from source through the public
// compiler, instead of copying a database left on some machine.
//
// The canary is the M1 pbptr carrier: shaders/hlsl/pbptr_abi.hlsl, 11 root
// constants + 1 root UAV, one ACO body (canary/gfx1201_g_pbptr.bin, 424 bytes)
// wrapped into the carrier ELF the official compiler returned for this target.
// --m5-build then copies this group unchanged into the network databases, so the
// ASI's built-in canary blob keeps matching.
#pragma once
#include <filesystem>

#include "common.hpp"
#include "compiler.hpp"
#include "genenv.hpp"
#include "pbptr_elf.hpp"
#include "util.hpp"

namespace lab {
namespace canary {

inline int build_seed(const Args& args, const std::string& repo, const std::string& out) {
    std::filesystem::create_directories(out);
    std::string aco_path = args.get("canary-aco", repo + "/canary/gfx1201_g_pbptr.bin");
    Bytes aco = read_file(aco_path);
    require(aco.size() == pbptr::kAcoBytes, "canary ACO body size is not 424 bytes: " + aco_path);
    require(sha256_hex(aco.data(), aco.size()) == pbptr::kAcoSha, "canary ACO body sha256 mismatch: " + aco_path);
    for (const char* f : {"/native.psdb", "/canary.sodb", "/canary.psdb"}) std::filesystem::remove(out + f);

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

    CaseDef carrier;
    carrier.hlsl_path = paths.hlsl_dir + "/pbptr_abi.hlsl";
    Bytes dxil = compile_hlsl(carrier, "");
    Bytes rs = serialize_root_constants(11, 1);
    CompilerStream stream = {};
    stream.root.value = {rs.data(), rs.size()};
    stream.cs.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS;
    stream.cs.cs = {dxil.data(), dxil.size()};
    D3D12_PIPELINE_STATE_STREAM_DESC desc = {sizeof(stream), &stream};
    const char groupName[] = "g_pbptr";
    D3D12_COMPILER_CACHE_GROUP_KEY group = {groupName, sizeof(groupName)};

    ComPtr<ID3D12StateObjectDatabaseFactory> sodbFactory;
    check(D3D12GetInterface(CLSID_D3D12StateObjectFactory, IID_PPV_ARGS(&sodbFactory)), "SODB factory");
    auto sodbPath = wide(out + "/canary.sodb");
    ComPtr<ID3D12StateObjectDatabase> sodb;
    check(sodbFactory->CreateStateObjectDatabaseFromFile(sodbPath.c_str(), D3D12_STATE_OBJECT_DATABASE_FLAG_NONE,
                                                          IID_PPV_ARGS(&sodb)), "Create SODB");
    check(sodb->SetApplicationDesc(&app), "SODB app desc");
    check(sodb->StorePipelineStateDesc(group.pKey, group.KeySize, 1, &desc), "StorePipelineStateDesc");
    HRESULT compiled = compiler->CompilePipelineState(&group, 1, &desc);
    check(compiled, "CompilePipelineState");

    ValueCapture captured;
    check(session->FindGroupValueKeys(&group, nullptr, collect_key, &captured), "FindGroupValueKeys");
    require(!captured.keys.empty(), "g_pbptr produced no value keys");

    auto copiedPath = wide(out + "/canary.psdb");
    D3D12_COMPILER_DATABASE_PATH copyPath{flags, copiedPath.c_str()};
    ComPtr<ID3D12CompilerCacheSession> copied;
    check(rt.factory->CreateCompilerCacheSession(&copyPath, 1, &target, &app, IID_PPV_ARGS(&copied)), "copy cache session");

    Json j;
    j.obj(nullptr);
    j.s("api", "canary seed: pbptr carrier through CompilePipelineState + wrapped ACO body");
    j.u("abi", target.ABIVersion);
    j.s("family", selected.family);
    j.s("canary_aco", aco_path);
    j.s("canary_aco_sha256", sha256_hex(aco.data(), aco.size()));
    j.s("carrier_dxil_sha256", sha256_hex(dxil.data(), dxil.size()));
    j.arr("values");
    for (size_t i = 0; i < captured.keys.size(); ++i) {
        const Bytes& keyBytes = captured.keys[i];
        D3D12_COMPILER_CACHE_VALUE_KEY key = {keyBytes.data(), UINT(keyBytes.size())};
        D3D12_COMPILER_CACHE_TYPED_VALUE values[2] = {};
        values[0].Type = D3D12_COMPILER_VALUE_TYPE_OBJECT_CODE;
        values[1].Type = D3D12_COMPILER_VALUE_TYPE_METADATA;
        check(session->FindValue(&key, values, 2, allocate_value, nullptr), "FindValue");
        std::vector<D3D12_COMPILER_CACHE_TYPED_CONST_VALUE> stored;
        Bytes elf;
        j.obj(nullptr);
        j.u("index", i);
        j.u("key_bytes", keyBytes.size());
        write_file(out + "/canary-" + std::to_string(i) + ".key", keyBytes);
        for (auto& value : values) {
            if (!value.Value.pValue || !value.Value.ValueSize) continue;
            const auto* p = static_cast<const uint8_t*>(value.Value.pValue);
            std::string kind = value.Type == D3D12_COMPILER_VALUE_TYPE_OBJECT_CODE ? "code" : "metadata";
            write_file(out + "/canary-" + std::to_string(i) + "-" + kind + ".bin",
                       Bytes(p, p + value.Value.ValueSize));
            j.u((kind + "_bytes").c_str(), value.Value.ValueSize);
            if (value.Type == D3D12_COMPILER_VALUE_TYPE_OBJECT_CODE) {
                pbptr::Built built = pbptr::build_loaded_elf(p, value.Value.ValueSize, aco.data(), aco.size(), 4);
                elf = std::move(built.elf);
                write_file(out + "/canary-" + std::to_string(i) + "-aco.elf", elf);
                j.s("aco_elf_sha256", sha256_hex(elf.data(), elf.size()));
                stored.push_back({value.Type, {elf.data(), UINT(elf.size())}});
            } else {
                stored.push_back({value.Type, {value.Value.pValue, UINT(value.Value.ValueSize)}});
            }
        }
        HRESULT storedHr = copied->StoreValue(&key, stored.data(), UINT(stored.size()));
        for (auto& value : values) free(value.Value.pValue);
        j.s("store_hr", hr_name(storedHr));
        j.end_obj();
        check(storedHr, "StoreValue");
    }
    j.end_arr();
    std::vector<D3D12_COMPILER_CACHE_VALUE_KEY> keys;
    for (auto& key : captured.keys) keys.push_back({key.data(), UINT(key.size())});
    check(copied->StoreGroupValueKeys(&group, 1, keys.data(), UINT(keys.size())), "StoreGroupValueKeys");
    // Commit the cache database while the process is alive.
    copied.Reset();
    session.Reset();
    gen::report_toolchain(paths, j);
    gen::report_targets(rt.factory.Get(), j);
    j.s("sodb", out + "/canary.sodb");
    j.s("psdb", out + "/canary.psdb");
    j.s("classification", "PASS_CANARY_SEED");
    j.end_obj();
    write_json(out + "/canary-seed.json", j);
    printf("%s\n", j.str().c_str());
    return 0;
}

}  // namespace canary
}  // namespace lab
