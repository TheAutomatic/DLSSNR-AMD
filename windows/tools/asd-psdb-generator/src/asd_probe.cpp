// Offline PSDB generator for the D3D12 ASD/ACO path.
//
// Windows x64, CPU only: this program never creates a D3D12 device, a command
// queue, or a PSO. It loads the official Microsoft ASD compiler runtime
// (D3D12StateObjectCompiler.dll) with the AMD compiler plugin
// (amdxc_compiler.dll), compiles one carrier per ACO record on the CPU, and
// stores the official ELF/metadata blobs with the wrapped ACO body into
// D3D12 compiler databases (PSDB) plus a state object database (SODB).
//
//   --m2-manifest  release SPIR-V + ACO records -> manifest.tsv / manifest.json
//   --m2-build     manifest.tsv -> carrier.sodb + public-copy.psdb (per record)
//   --canary-seed  s_mov canary carrier + 424-byte ACO body -> canary.sodb/psdb
//   --m5-build     canary seed + manifest -> carrier.sodb + public-copy.psdb
//                  + net-carriers.bin for the ASI
//
// Paths: --root is the checkout/repo root used only to resolve defaults for
// --compiler-dir, --plugin, --shaders, --dxc; every one can be given explicitly.
#include <initguid.h>
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3d12compiler.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>

#include "canary.hpp"
#include "common.hpp"
#include "compiler.hpp"
#include "genenv.hpp"
#include "m2.hpp"
#include "m5.hpp"
#include "model_elf.hpp"
#include "verify.hpp"

// The D3D12 compiler and state object database interfaces come from the Agility
// SDK runtime, not from the in-box d3d12.dll. Selecting it app-locally mirrors
// the lab probe: D3D12Core.dll ships next to this exe (the build script copies
// it from the SDK when present).
#ifndef ASD_SDK_VERSION
#define ASD_SDK_VERSION 619
#endif
#define ASD_STR2(x) #x
#define ASD_STR(x) ASD_STR2(x)
extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = ASD_SDK_VERSION;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\";
}

int main(int argc, char** argv) {
    try {
        lab::Args args = lab::parse_args(argc, argv);
        std::string repo = args.get("root", ".");
        std::string out = args.get("out");
        lab::require(!out.empty(), "--out required");
        std::filesystem::create_directories(out);
        if (!args.get("dxc").empty()) lab::dxc_path() = args.get("dxc");
        if (args.has("m2-manifest")) return lab::m2::build_manifest(args, repo, out);
        if (args.has("m2-build")) return lab::m2::build_psdb(args, repo, out);
        if (args.has("canary-seed")) return lab::canary::build_seed(args, repo, out);
        if (args.has("m5-build")) return lab::m5::build(args, repo, out);
        if (args.has("verify")) return lab::verify::run(args, repo, out);
        fprintf(stderr,
                "usage: asd_psdb_gen --out DIR [--root REPO] "
                "[--m2-manifest --aco-root ... --release-root ... | --m2-build --manifest ... | "
                "--canary-seed | --m5-build --manifest ... --canary-sodb ... --canary-psdb ... | "
                "--verify --manifest ... --sodb ... --psdb ... --carriers ...]\n"
                "toolchain: [--compiler-dir DIR] [--plugin amdxc_compiler.dll] [--shaders DIR] [--dxc dxc.exe] "
                "[--family gfx1201]\n");
        return 1;
    } catch (const std::exception& e) {
        fprintf(stderr, "asd_psdb_gen: %s\n", e.what());
        return 1;
    }
}
