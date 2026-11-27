// dlssnr-int4-weights: makes dlssnr-amd/CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin at install time (package/install.ps1)
// from CC_SILVER_AARDWOLD_OPTIMIZED.bin, and dlssnr-amd/CC_SMALL_FP8_OPTIMIZED_INT4.bin (Ultra Performance's ViT int4
// layers) from CC_SMALL_FP8_OPTIMIZED.bin and dlssnr-amd/CC_SMALL_FP8_OPTIMIZED-ultra/: CC_SMALL_FP8's plan at
// 3840x2160 (the matrices do not depend on the extent).
// The int4 weights are not shipped: they are rebuilt from the network's weights and the package's recovery tables
// (<level>/vit, <level>/swin: *.fix.bin, *.mu.bin) by the graph builder's own decoder, run here exactly as the
// runtime's int4 build runs it (same plan, same settings), stopping before any GPU work.
// --sink: dlssnr-amd/CC_SMALL_FP8_OPTIMIZED_INT4_SINK.bin, the same from CC_SMALL_FP8_OPTIMIZED-ultra/vit-sink/ (the
// tables of the 1440p-class frames, whose ViT padding is all the image's edge: a few ViT heads attend almost only to the
// padding tokens there, an attention sink; nr_graph.cpp g_vit_sink).
//   dlssnr-int4-weights --network CC_SILVER_AARDWOLD_OPTIMIZED | CC_SMALL_FP8_OPTIMIZED [--sink] <the network's .bin>
//                       <int4 data: vit/, swin/, settings.txt, shaders/> <output file>
#define NR_NO_MAIN
#include "../core/nr_graph.cpp"
#include "nr_native_plan.hpp"
#include <sstream>

// The arguments in UTF-8 (wmain below: the install passes the package folder and %TEMP%, which hold the user's
// letters - a Chinese user name - that the ANSI code page may not have)
static int run(int argc, char** argv) try {
    const char* usage = "usage: %s --network CC_SILVER_AARDWOLD_OPTIMIZED | CC_SMALL_FP8_OPTIMIZED [--sink] <the network's .bin> "
                        "<int4 data dir> <output file>\n";
    const char* self = argv[0];
    if (argc < 3 || std::string(argv[1]) != "--network" ||
        (std::string(argv[2]) != "CC_SILVER_AARDWOLD_OPTIMIZED" && std::string(argv[2]) != "CC_SMALL_FP8_OPTIMIZED")) {
        std::fprintf(stderr, usage, self); return 2;
    }
    const bool small_fp8 = std::string(argv[2]) == "CC_SMALL_FP8_OPTIMIZED";
    argc -= 2; argv += 2;
    const bool sink = small_fp8 && argc > 1 && std::string(argv[1]) == "--sink";
    if (sink) { --argc; ++argv; }
    if (argc != 4) { std::fprintf(stderr, usage, self); return 2; }
    const std::filesystem::path pack = std::filesystem::absolute(nr::u8::path(argv[1])),
                                i4 = std::filesystem::absolute(nr::u8::path(argv[2]));
    std::map<std::string, std::string> cfg;
    cfg["NR_I4_DIR"] = (i4 / (sink ? "vit-sink" : "vit")).string();   // built at 3840x2160, where NR_I4_DIR is read
    if (std::filesystem::is_directory(i4 / "swin")) cfg["NR_SWI4_DIR"] = (i4 / "swin").string();
    {   // the runtime's reading of <level>/settings.txt (nr_runtime.cpp)
        const auto t = slurp((i4 / "settings.txt").string());
        if (t.empty()) throw std::runtime_error("missing " + (i4 / "settings.txt").string());
        std::istringstream lines(std::string(t.begin(), t.end()));
        for (std::string l; std::getline(lines, l);) {
            while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
            const auto eq = l.find('=');
            if (l.empty() || l[0] == '#' || eq == std::string::npos) continue;
            cfg[l.substr(0, eq)] = l.substr(eq + 1);
        }
    }
    nr::set_log_sink([](const char* m) { std::fprintf(stderr, "%s\n", m); });
    const uint32_t w = small_fp8 ? 3840 : 1920, h = small_fp8 ? 2160 : 1080;   // the matrices do not depend on the extent
    const auto native = nr::make_native_plan(w, h, small_fp8 ? nr::NetworkModel::SmallFp8 : nr::NetworkModel::SilverAardwold);
    std::istringstream plan_stream(native.text);
    const auto plan = parse_plan(plan_stream);
    std::vector<std::string> a = {"dlssnr-int4-weights", "--plan", "compiled native descriptor",
        "--unpacked", (pack.parent_path() / "model").string(), "--spv-dir", (i4 / "shaders").string(),
        "--host-boundary", "--no-reuse", "--source-width", std::to_string(w), "--source-height", std::to_string(h), "--accumulation", "fp32",
        "--model-pack", pack.string(), "--int4-weights-out", argv[3]};
    std::vector<char*> av;
    for (auto& s : a) av.push_back(s.data());
    const nr::BuildCfgScope scope(&cfg);
    NrSession s;
    const int rc = s.build(int(av.size()), av.data(), &plan);
    if (rc != 2) { std::fprintf(stderr, "dlssnr-int4-weights: the build did not reach the int4 weights (%d)\n", rc); return 1; }
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "dlssnr-int4-weights: %s\n", e.what());
    return 1;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** wargv) {   // (linked with -municode)
    std::vector<std::string> u8;
    for (int i = 0; i < argc; ++i) u8.push_back(nr::u8::narrow(wargv[i]));
    std::vector<char*> argv;
    for (auto& a : u8) argv.push_back(a.data());
    return run(argc, argv.data());
}
#else
int main(int argc, char** argv) { return run(argc, argv); }
#endif
