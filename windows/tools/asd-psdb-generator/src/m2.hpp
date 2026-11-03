// M2: generic ELF wrapping for every gfx12 NRPAL001 record in a release bundle.
// CPU only: this file builds the manifest and the PSDB; the GPU sweep that lived
// in the lab (--m2-execute) is not part of the generator package.
//
//   --m2-manifest  CPU: join aco/records + aco/int4/records with their shell
//                  SPIR-V (fnv hash), reflect LocalSize + set-0 bindings, write
//                  manifest.json (evidence) + manifest.tsv (tool input).
//   --m2-build     CPU: per record one carrier (TAG=tag, numthreads=dims) through
//                  the official compiler, FindValue template, wrapped ACO ELF,
//                  StoreValue into ONE PSDB. Every record gets its own key.
#pragma once
#include "compiler.hpp"
#include "genenv.hpp"
#include "model_elf.hpp"
#include "spv_reflect.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <set>
#include <sstream>

namespace lab {
namespace m2 {

struct Entry {
    uint32_t tag = 0;
    std::string file;       // record file stem (fnv hex)
    std::string src;        // "root" | "int4"
    std::string shell;      // shell spv file name
    std::string spv_path;   // absolute path of the record's own SPIR-V
    std::string note;       // empty when the entry is usable
    uint32_t dims[3] = {0, 0, 0};
    uint32_t tidig = 0, tgid = 0, tgsize = 0, lds = 0, shared = 0, user = 0, push_bytes = 0, vgpr = 0, exec = 0;
    int push_reg = -1, grid_reg = -1;
    std::vector<std::pair<int, int>> inlines;
    std::vector<spv::Binding> bindings;
    model::NrRecord rec;
};


// ---------------------------------------------------------------------------
// Manifest

inline void hash_spv_dir(const std::filesystem::path& dir,
                         std::map<uint64_t, std::filesystem::path>& by_hash, size_t& counted) {
    if (!std::filesystem::exists(dir)) return;
    for (auto it = std::filesystem::recursive_directory_iterator(dir); it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (it->is_directory() || it->path().extension() != ".spv") continue;
        Bytes b = read_file(it->path().string());
        by_hash.emplace(model::fnv_lab(b.data(), b.size()), it->path());
        ++counted;
    }
}

inline std::string kind_size(const std::string& kind) {
    if (kind == "buffer") return "16";
    if (kind == "combined") return "48";  // image SRD (32) + sampler SRD (16)
    if (kind == "image") return "32";
    return "16";  // bare sampler
}

inline int build_manifest(const Args& args, const std::string& repo, const std::string& out) {
    std::string aco = args.get("aco-root");
    require(!aco.empty(), "--aco-root required (the release dlssnr-amd/aco directory)");
    std::string rel = args.get("release-root");
    if (rel.empty()) rel = std::filesystem::path(aco).parent_path().string();
    // Normalize separators so manifest.tsv (and its hash) does not depend on
    // whether the caller passed forward or back slashes.
    std::filesystem::path aco_dir = std::filesystem::path(aco).make_preferred();
    std::filesystem::path rel_dir = std::filesystem::path(rel).make_preferred();
    // SPIR-V search: the record's own hash keys one of these directories.
    std::map<uint64_t, std::filesystem::path> by_hash;
    size_t spv_count = 0;
    hash_spv_dir(aco_dir / "shaders", by_hash, spv_count);
    hash_spv_dir(aco_dir / "int4" / "shaders", by_hash, spv_count);
    hash_spv_dir(rel_dir / "experimental" / "shaders", by_hash, spv_count);

    std::vector<Entry> entries;
    auto take = [&](const std::filesystem::path& rec_dir, const char* src_name) {
        for (auto& f : std::filesystem::directory_iterator(rec_dir)) {
            if (f.path().extension() != ".nrp") continue;
            Entry e;
            e.file = f.path().stem().string();
            e.src = src_name;
            Bytes b = read_file(f.path().string());
            try {
                e.rec = model::parse_nrpal001(b);
            } catch (const std::exception& ex) {
                e.note = std::string("record parse: ") + ex.what();
                entries.push_back(std::move(e));
                continue;
            }
            auto it = by_hash.find(e.rec.spv_hash);
            if (it == by_hash.end()) {
                e.note = "no shell SPIR-V for this record hash";
            } else {
                e.spv_path = it->second.string();
                e.shell = it->second.filename().string();
                Bytes spv = read_file(e.spv_path);
                spv::Reflection r = spv::reflect(spv);
                if (!r.has_local) e.note = "shell has no literal LocalSize";
                else {
                    e.dims[0] = r.local[0]; e.dims[1] = r.local[1]; e.dims[2] = r.local[2];
                    e.rec.dims[0] = r.local[0]; e.rec.dims[1] = r.local[1]; e.rec.dims[2] = r.local[2];
                    e.bindings = r.bindings;
                    // Bindings must be buffers 0..nb-1 then images (Mochi's set-0 model).
                    uint32_t expect = 0; bool ok = true;
                    for (auto& bd : e.bindings) {
                        if (bd.index != expect) { ok = false; break; }
                        ++expect;
                    }
                    if (e.bindings.empty()) e.note = "reflection found no set-0 bindings";
                    else if (!ok) e.note = "binding layout is not buffers-then-images";
                    else if (e.dims[0] * e.dims[1] * e.dims[2] != e.rec.wg) e.note = "LocalSize product != wg";
                    else {
                        uint32_t want_tidig = e.dims[1] > 1 ? (e.dims[2] > 1 ? 2u : 1u) : 0u;
                        if (e.rec.tidig() != want_tidig) e.note = "tidig_comp_cnt does not match LocalSize";
                    }
                }
            }
            e.tidig = e.rec.tidig();
            // gfx11 (rdna3) records are out of this milestone by the gfx12-only scope:
            // RDNA4 only. The NRPAL002 parse in model_elf.hpp stays for the day
            // a gfx11 compiler target exists (pending hardware).
            e.tgid = (e.rec.rsrc2 >> 7) & 7u;
            e.tgsize = (e.rec.rsrc2 >> 10) & 1u;
            e.lds = e.rec.lds_hw();
            e.shared = e.rec.shared;
            e.user = e.rec.user;
            e.push_bytes = e.rec.push_bytes;
            e.push_reg = e.rec.push;
            e.grid_reg = e.rec.grid;
            e.inlines = e.rec.inlines;
            e.vgpr = ((e.rec.rsrc1 & 63u) + 1u) * 8u;
            entries.push_back(std::move(e));
        }
    };
    take(aco_dir / "records", "root");
    take(aco_dir / "int4" / "records", "int4");
    // Tags are stable across runs: sort by (src, file).
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return std::tie(a.src, a.file) < std::tie(b.src, b.file);
    });
    for (size_t i = 0; i < entries.size(); ++i) entries[i].tag = uint32_t(i);

    std::string tsv = "tag\tfile\tsrc\tshell\tspv\tnote\tdims\ttidig\ttgid\ttgsize\tuser\tpush_bytes\tpush_reg\tgrid_reg\tlds\tvgpr\texec\tinlines\tbindings\n";
    Json j;
    j.obj(nullptr);
    j.s("aco_root", aco);
    j.s("release_root", rel);
    j.u("spv_files_indexed", spv_count);
    j.u("records", entries.size());
    size_t usable = 0;
    j.arr("entries");
    for (auto& e : entries) {
        std::string inl, bnd;
        for (auto& [reg, dw] : e.inlines) inl += (inl.empty() ? "" : ",") + std::to_string(reg) + ":" + std::to_string(dw);
        for (auto& b : e.bindings) bnd += (bnd.empty() ? "" : ",") + std::to_string(b.index) + ":" + b.kind + ":" + kind_size(b.kind);
        tsv += std::to_string(e.tag) + "\t" + e.file + "\t" + e.src + "\t" + e.shell + "\t" + e.spv_path + "\t" +
               (e.note.empty() ? "-" : e.note) + "\t" + std::to_string(e.dims[0]) + "x" + std::to_string(e.dims[1]) + "x" + std::to_string(e.dims[2]) + "\t" +
               std::to_string(e.tidig) + "\t" + std::to_string(e.tgid) + "\t" + std::to_string(e.tgsize) + "\t" + std::to_string(e.user) + "\t" +
               std::to_string(e.push_bytes) + "\t" + std::to_string(e.push_reg) + "\t" + std::to_string(e.grid_reg) + "\t" +
               std::to_string(e.lds) + "\t" + std::to_string(e.vgpr) + "\t" + std::to_string(e.exec) + "\t" + inl + "\t" + bnd + "\n";
        j.obj(nullptr);
        j.u("tag", e.tag);
        j.s("file", e.file);
        j.s("src", e.src);
        j.s("shell", e.shell);
        j.s("note", e.note);
        j.u("dims_x", e.dims[0]);
        j.u("dims_y", e.dims[1]);
        j.u("dims_z", e.dims[2]);
        j.u("tidig", e.tidig);
        j.u("tgid", e.tgid);
        j.u("tgsize", e.tgsize);
        j.u("user", e.user);
        j.u("push_bytes", e.push_bytes);
        j.i("push_reg", e.push_reg);
        j.i("grid_reg", e.grid_reg);
        j.u("lds", e.lds);
        j.u("shared", e.shared);
        j.u("vgpr", e.vgpr);
        j.u("exec", e.exec);
        j.u("code_bytes", e.rec.code.size());
        j.end_obj();
        if (e.note.empty()) ++usable;
    }
    j.end_arr();
    j.u("usable", usable);
    j.s("classification", "PASS_M2_MANIFEST");
    j.end_obj();
    write_file(out + "/manifest.tsv", tsv);
    write_json(out + "/manifest.json", j);
    printf("%s\n", j.str().c_str());
    return usable == entries.size() ? 0 : 2;
}

// ---------------------------------------------------------------------------
// TSV loading (build + execute run as separate processes)

inline std::vector<Entry> load_manifest_tsv(const std::string& path) {
    Bytes raw = read_file(path);
    std::string all(reinterpret_cast<const char*>(raw.data()), raw.size());
    std::vector<Entry> out;
    size_t pos = all.find('\n');  // header
    require(pos != std::string::npos, "manifest.tsv header");
    pos += 1;
    while (pos < all.size()) {
        size_t eol = all.find('\n', pos);
        if (eol == std::string::npos) eol = all.size();
        std::string line = all.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.empty()) continue;
        std::vector<std::string> f;
        size_t s = 0;
        while (true) {
            size_t t = line.find('\t', s);
            if (t == std::string::npos) { f.push_back(line.substr(s)); break; }
            f.push_back(line.substr(s, t - s));
            s = t + 1;
        }
        require(f.size() == 19, "manifest.tsv column count");
        Entry e;
        e.tag = uint32_t(std::stoul(f[0]));
        e.file = f[1];
        e.src = f[2];
        e.shell = f[3];
        e.spv_path = f[4];
        e.note = f[5] == "-" ? "" : f[5];
        {
            uint32_t d[3] = {1, 1, 1};
            require(sscanf(f[6].c_str(), "%ux%ux%u", &d[0], &d[1], &d[2]) == 3, "dims column");
            e.dims[0] = d[0]; e.dims[1] = d[1]; e.dims[2] = d[2];
        }
        e.tidig = uint32_t(std::stoul(f[7]));
        e.tgid = uint32_t(std::stoul(f[8]));
        e.tgsize = uint32_t(std::stoul(f[9]));
        e.user = uint32_t(std::stoul(f[10]));
        e.push_bytes = uint32_t(std::stoul(f[11]));
        e.push_reg = std::stoi(f[12]);
        e.grid_reg = std::stoi(f[13]);
        e.lds = uint32_t(std::stoul(f[14]));
        e.vgpr = uint32_t(std::stoul(f[15]));
        e.exec = uint32_t(std::stoul(f[16]));
        for (auto& tok : f[17].empty() ? std::vector<std::string>{} : [&] {
             std::vector<std::string> v; size_t s2 = 0;
             while (true) { size_t t = f[17].find(',', s2); if (t == std::string::npos) { v.push_back(f[17].substr(s2)); break; } v.push_back(f[17].substr(s2, t - s2)); s2 = t + 1; }
             return v; }()) {
            int reg = 0, dw = 0;
            require(sscanf(tok.c_str(), "%d:%d", &reg, &dw) == 2, "inlines column");
            e.inlines.emplace_back(reg, dw);
        }
        for (auto& tok : f[18].empty() ? std::vector<std::string>{} : [&] {
             std::vector<std::string> v; size_t s2 = 0;
             while (true) { size_t t = f[18].find(',', s2); if (t == std::string::npos) { v.push_back(f[18].substr(s2)); break; } v.push_back(f[18].substr(s2, t - s2)); s2 = t + 1; }
             return v; }()) {
            spv::Binding b;
            char kind[24];
            require(sscanf(tok.c_str(), "%u:%23[^:]:%*u", &b.index, kind) == 2, "bindings column");
            b.kind = kind;
            e.bindings.push_back(b);
        }
        out.push_back(std::move(e));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Build: one carrier per record through the official compiler, wrapped ELF per
// record, everything into ONE PSDB + ONE SODB.

inline std::filesystem::path record_path(const Args& args, const Entry& e) {
    std::filesystem::path aco(args.get("aco-root"));
    require(!aco.empty(), "--aco-root required");
    if (e.src == "int4") return aco / "int4" / "records" / (e.file + ".nrp");
    if (e.src == "rdna3") return aco / "rdna3" / "records" / (e.file + ".nrp");
    return aco / "records" / (e.file + ".nrp");
}

inline int build_psdb(const Args& args, const std::string& repo, const std::string& out) {
    std::filesystem::create_directories(out + "/keys");
    require(!args.get("manifest").empty(), "--manifest required (manifest.tsv from --m2-manifest)");
    uint32_t skip = args.get("skip").empty() ? 0u : uint32_t(std::stoul(args.get("skip")));
    uint32_t limit = args.get("limit").empty() ? ~0u : uint32_t(std::stoul(args.get("limit")));
    std::string only = args.get("only");
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
                                                          IID_PPV_ARGS(&sodb)),
          "Create SODB");
    check(sodb->SetApplicationDesc(&app), "SODB app desc");
    auto copiedPath = wide(out + "/public-copy.psdb");
    D3D12_COMPILER_DATABASE_PATH copyPath{flags, copiedPath.c_str()};
    ComPtr<ID3D12CompilerCacheSession> copied;
    check(rt.factory->CreateCompilerCacheSession(&copyPath, 1, &target, &app, IID_PPV_ARGS(&copied)), "copy cache session");

    Bytes rs = serialize_root_constants(1, 1);
    std::string jsonl;
    size_t built = 0, skipped = 0;
    auto entries = load_manifest_tsv(args.get("manifest"));
    for (auto& e : entries) {
        if (e.tag < skip || e.tag >= limit) continue;
        if (!only.empty() && e.file != only) continue;
        if (!e.note.empty()) { ++skipped; continue; }
        model::NrRecord rec = model::parse_nrpal001(read_file(record_path(args, e).string()));
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
        // The group key must be unique per entry: two records can share a file
        // stem (root and int4 variants of one shell), and StorePipelineStateDesc
        // returns DXGI_ERROR_ALREADY_EXISTS for a repeated group.
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
        write_file(out + "/keys/" + std::to_string(e.tag) + "-aco.elf", built_elf.elf);
        write_file(out + "/keys/" + std::to_string(e.tag) + "-prefix.bin", built_elf.prefix);
        write_file(out + "/keys/" + std::to_string(e.tag) + "-carrier.elf", carrier_elf);
        write_file(out + "/keys/" + std::to_string(e.tag) + "-metadata.bin", meta);
        jsonl += "{\"tag\":" + std::to_string(e.tag) + ",\"file\":\"" + e.file + "\",\"src\":\"" + e.src +
                  "\",\"shell\":\"" + e.shell + "\",\"key\":\"" + model::hex_bytes(key.data(), key.size()) +
                  "\",\"key_bytes\":" + std::to_string(key.size()) +
                  ",\"aco_elf_bytes\":" + std::to_string(built_elf.elf.size()) +
                  ",\"aco_elf_sha256\":\"" + sha256_hex(built_elf.elf.data(), built_elf.elf.size()) +
                  "\",\"prefix_bytes\":" + std::to_string(built_elf.prefix.size()) +
                  ",\"metadata_replaced\":" + (metadata_replaced ? "true" : "false") + "}\n";
        ++built;
        if (built % 25 == 0) printf("m2 build: %zu records wrapped\n", built);
    }
    write_file(out + "/build.jsonl", jsonl);
    Json j;
    j.obj(nullptr);
    j.s("api", "ASD CompilePipelineState + StoreValue, one carrier TAG per record");
    j.u("abi", target.ABIVersion);
    j.s("family", selected.family);
    j.s("aco_root", args.get("aco-root"));
    j.u("records_total", entries.size());
    j.u("built", built);
    j.u("skipped", skipped);
    j.s("psdb", out + "/public-copy.psdb");
    j.s("sodb", out + "/carrier.sodb");
    gen::report_toolchain(paths, j);
    gen::report_targets(rt.factory.Get(), j);
    j.s("classification", built + skipped == entries.size() && skipped == 0 ? "PASS_M2_BUILD" : "PARTIAL_M2_BUILD");
    j.end_obj();
    write_json(out + "/m2-build.json", j);
    printf("%s\n", j.str().c_str());
    return skipped == 0 ? 0 : 2;
}

}  // namespace m2
}  // namespace lab
