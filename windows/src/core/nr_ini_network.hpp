#pragma once
// [Network] in dlssnr-amd.ini: Model (which model the network runs) and Motion Without Depth (where the pre block
// reads the motion vectors), read by the runtime when it builds the network. ini_network: -1 for no file, no
// section or no readable value (the runtime's default, CC_SMALL_FP8).
#include <cctype>
#include <cstdio>
#include <string>
#include "nr_path_u8.hpp"

namespace nr {
// The networks a user picks, by the DLL's own codenames (dlssnr-amd.ini [Network] Model, NR Control, the log).
// CC_SMALL_FP8_OPTIMIZED and CC_SILVER_AARDWOLD_OPTIMIZED are ours, not the DLL's: CC_SMALL_FP8 and CC_SILVER_AARDWOLD
// made faster (ReLU, fixed attention on some decoder layers, ...), their weights fitted from the model's at install.
// CC_SMALL_FP8_OPTIMIZED runs at three levels ([CC_SMALL_FP8_OPTIMIZED] Layers): Quality (kSmallFp8Optimized,
// CC_SMALL_FP8_OPTIMIZED/; also for Balanced), Performance (kSmallFp8OptimizedPerformance,
// CC_SMALL_FP8_OPTIMIZED-performance/): fixed attention on the pre block and, on 4K-class frames, pooled keys in some of
// the ViT's attention heads, and Ultra Performance (kSmallFp8OptimizedUltra, CC_SMALL_FP8_OPTIMIZED-ultra/): Performance
// with the ViT's int4 layers (at 1440p-class frames, whose ViT padding is all the image's
// edge, only its later blocks', with tables of their own; none where that padding is columns only). One pack
// (CC_SMALL_FP8_OPTIMIZED.bin; Performance and Ultra Performance read its "performance/" entries in place of some,
// "performance-sink/" at 1440p-class extents with an edge-padded row), and two int4 packs for Ultra Performance
// (CC_SMALL_FP8_OPTIMIZED_INT4.bin and _INT4_SINK.bin, made at install; "sink": at those extents a few ViT heads attend
// almost only to the padding tokens, an attention sink, nr_graph.cpp g_vit_sink). CC_SILVER_AARDWOLD_OPTIMIZED runs at
// one of four levels of its optimized layers ([CC_SILVER_AARDWOLD_OPTIMIZED] Layers:
// Quality, Balanced, Performance, Ultra Performance; RDNA4, RDNA3 and RDNA3.5): four networks here (each built, kept and switched to as a
// network), one in the lists. Quality: none of its layers in int4 (kSilverAardwoldOptimizedFp8); Balanced: some of them (kSilverAardwoldOptimized, CC_SILVER_AARDWOLD_OPTIMIZED-balanced/); Performance: Balanced with fixed attention on the first full-resolution
// layers (kSilverAardwoldOptimizedPerformance, CC_SILVER_AARDWOLD_OPTIMIZED-performance/); Ultra Performance, the default: Performance with more
// int4 layers (kSilverAardwoldOptimizedUltra, CC_SILVER_AARDWOLD_OPTIMIZED-ultra/). One FP8 pack for the four (CC_SILVER_AARDWOLD_OPTIMIZED.bin; Performance and
// Ultra Performance read its "performance/" entries in place of some), one int4 pack for the three int4 levels
// (CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin).
enum NetworkChoice : int {
    kSmallFp8 = 0, kSilverAardwold = 1, kSilverAardwoldOptimized = 2, kSmallFp8Optimized = 3,
    kSilverAardwoldOptimizedFp8 = 4, kSilverAardwoldOptimizedPerformance = 5, kSilverAardwoldOptimizedUltra = 6,
    kSmallFp8OptimizedPerformance = 7, kSmallFp8OptimizedUltra = 8, kNetworkChoices = 9
};
// [CC_SILVER_AARDWOLD_OPTIMIZED] Layers: 0 Quality, 1 Balanced, 2 Performance, 3 Ultra Performance (the default).
// [CC_SMALL_FP8_OPTIMIZED] Layers (its own level, the same values): Quality the default.
inline constexpr int kLayersQuality = 0, kLayersBalanced = 1, kLayersPerformance = 2, kLayersUltra = 3,
                     kLayersDefault = kLayersUltra, kSmallFp8LayersDefault = kLayersQuality, kLayersCount = 4;
// CC_SILVER_AARDWOLD_OPTIMIZED at any of its levels
inline bool network_silver_aardwold_optimized(int n) {
    return n == kSilverAardwoldOptimized || n == kSilverAardwoldOptimizedFp8 || n == kSilverAardwoldOptimizedPerformance ||
           n == kSilverAardwoldOptimizedUltra;
}
// CC_SMALL_FP8_OPTIMIZED at any of its levels
inline bool network_small_fp8_optimized(int n) {
    return n == kSmallFp8Optimized || n == kSmallFp8OptimizedPerformance || n == kSmallFp8OptimizedUltra;
}
// a network whose level its Layers key picks
inline bool network_levelled(int n) { return network_silver_aardwold_optimized(n) || network_small_fp8_optimized(n); }
// The optimized networks at a level of their optimized layers (other networks as they are), and the other way.
// CC_SMALL_FP8_OPTIMIZED has three: Balanced runs Quality.
inline int network_with_layers(int n, int level) {
    if (network_small_fp8_optimized(n))
        return level == kLayersUltra ? kSmallFp8OptimizedUltra
             : level == kLayersPerformance ? kSmallFp8OptimizedPerformance : kSmallFp8Optimized;
    return !network_silver_aardwold_optimized(n) ? n : level == kLayersQuality ? kSilverAardwoldOptimizedFp8
         : level == kLayersPerformance ? kSilverAardwoldOptimizedPerformance
         : level == kLayersUltra ? kSilverAardwoldOptimizedUltra : kSilverAardwoldOptimized;
}
// The same with each optimized network's own level ([CC_SILVER_AARDWOLD_OPTIMIZED] Layers, [CC_SMALL_FP8_OPTIMIZED]
// Layers).
inline int network_with_levels(int n, int silver_aardwold_layers, int small_fp8_layers) {
    return network_with_layers(n, network_small_fp8_optimized(n) ? small_fp8_layers : silver_aardwold_layers);
}
inline int network_layers(int n) {
    if (network_small_fp8_optimized(n))
        return n == kSmallFp8OptimizedUltra ? kLayersUltra : n == kSmallFp8OptimizedPerformance ? kLayersPerformance
             : kLayersQuality;
    return n == kSilverAardwoldOptimizedFp8 ? kLayersQuality : n == kSilverAardwoldOptimizedPerformance ? kLayersPerformance
         : n == kSilverAardwoldOptimizedUltra ? kLayersUltra : kLayersBalanced;
}
// A level's word (the Layers keys also take these; the logs and the OSD show them).
inline const char* layers_word(int level) {
    return level == kLayersQuality ? "quality" : level == kLayersPerformance ? "performance"
         : level == kLayersUltra ? "ultra performance" : "balanced";
}
// The order the networks are listed in (CC_SMALL_FP8's two together, then CC_SILVER_AARDWOLD's; each optimized network
// once, as its Layers key picks its level) and the hotkey cycles them in. kNetworkOrder: every network, for the
// tables that show each one apart (video memory).
inline constexpr int kNetworkListed = 4;
inline constexpr int kNetworkList[kNetworkListed] = {kSmallFp8, kSmallFp8Optimized, kSilverAardwold,
                                                     kSilverAardwoldOptimized};
inline constexpr int kNetworkOrder[kNetworkChoices] = {kSmallFp8, kSmallFp8Optimized, kSmallFp8OptimizedPerformance,
    kSmallFp8OptimizedUltra, kSilverAardwold, kSilverAardwoldOptimizedFp8, kSilverAardwoldOptimized, kSilverAardwoldOptimizedPerformance,
    kSilverAardwoldOptimizedUltra};
// The list's entry of a network (an optimized network's levels: its own entry).
inline int network_listed(int n) {
    return network_silver_aardwold_optimized(n) ? kSilverAardwoldOptimized : network_small_fp8_optimized(n) ? kSmallFp8Optimized : n;
}
// Where a network is in the list (the optimized ones at their network's place).
inline int network_rank(int n) {
    n = network_listed(n);
    for (int i = 0; i < kNetworkListed; ++i) if (kNetworkList[i] == n) return i;
    return 0;
}
// The codename (the optimized ones: CC_SILVER_AARDWOLD_OPTIMIZED; the ini keeps the level apart).
inline const char* network_codename(int n) {
    return n == kSmallFp8 ? "CC_SMALL_FP8" : n == kSilverAardwold ? "CC_SILVER_AARDWOLD"
         : network_silver_aardwold_optimized(n) ? "CC_SILVER_AARDWOLD_OPTIMIZED"
         : network_small_fp8_optimized(n) ? "CC_SMALL_FP8_OPTIMIZED" : "?";
}
// The name shown where the optimized ones must be told apart (logs, the OSD, NR Control's tables).
inline const char* network_label(int n) {
    return n == kSilverAardwoldOptimizedFp8 ? "CC_SILVER_AARDWOLD_OPTIMIZED (layers: quality)"
         : n == kSilverAardwoldOptimized ? "CC_SILVER_AARDWOLD_OPTIMIZED (layers: balanced)"
         : n == kSilverAardwoldOptimizedPerformance ? "CC_SILVER_AARDWOLD_OPTIMIZED (layers: performance)"
         : n == kSilverAardwoldOptimizedUltra ? "CC_SILVER_AARDWOLD_OPTIMIZED (layers: ultra performance)"
         : n == kSmallFp8Optimized ? "CC_SMALL_FP8_OPTIMIZED (layers: quality)"
         : n == kSmallFp8OptimizedPerformance ? "CC_SMALL_FP8_OPTIMIZED (layers: performance)"
         : n == kSmallFp8OptimizedUltra ? "CC_SMALL_FP8_OPTIMIZED (layers: ultra performance)"
         : network_codename(n);
}
// A codename; -1 for anything else (case-insensitive). CC_SILVER_AARDWOLD_OPTIMIZED: kSilverAardwoldOptimized,
// CC_SMALL_FP8_OPTIMIZED: kSmallFp8Optimized (their Layers keys pick the level).
inline int network_choice(std::string v) {
    for (auto& c : v) c = char(std::toupper(static_cast<unsigned char>(c)));
    if (v == "CC_SMALL_FP8") return kSmallFp8;
    if (v == "CC_SILVER_AARDWOLD") return kSilverAardwold;
    if (v == "CC_SILVER_AARDWOLD_OPTIMIZED") return kSilverAardwoldOptimized;
    if (v == "CC_SMALL_FP8_OPTIMIZED") return kSmallFp8Optimized;
    return -1;
}
// RuntimeConfig::network's numbering: 1 CC_SMALL_FP8, 2 CC_SMALL_FP8_OPTIMIZED, 3 CC_SILVER_AARDWOLD_OPTIMIZED, 0
// CC_SILVER_AARDWOLD, -1 anything else.
inline int network_value(std::string v) {
    const int n = network_choice(v);
    return n == kSmallFp8 ? 1 : network_small_fp8_optimized(n) ? 2 : network_silver_aardwold_optimized(n) ? 3 : n < 0 ? -1 : 0;
}
inline int ini_network(const std::string& path) {
    FILE* f = nr::u8::fopen(path.c_str(), "r");
    if (!f) return -1;
    auto trim = [](std::string s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        size_t i = 0; while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        return s.substr(i);
    };
    auto lower = [](std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; };
    int r = -1;
    std::string section; char line[512];
    while (std::fgets(line, sizeof line, f)) {
        std::string t = line;
        const auto c = t.find_first_of(";#");
        if (c != std::string::npos) t.resize(c);
        t = trim(t);
        if (t.empty()) continue;
        if (t.front() == '[') { const auto e = t.find(']'); section = lower(trim(t.substr(1, e == std::string::npos ? std::string::npos : e - 1))); continue; }
        const auto eq = t.find('=');
        if (eq == std::string::npos || section != "network") continue;
        if (lower(trim(t.substr(0, eq))) == "model") r = network_value(trim(t.substr(eq + 1)));
    }
    std::fclose(f);
    return r;
}
// Another [Network] key's value, trimmed ("" when there is none): `key` lower case, as the file's keys are compared.
inline std::string ini_network_key(const std::string& path, const std::string& key) {
    FILE* f = nr::u8::fopen(path.c_str(), "r");
    if (!f) return {};
    auto trim = [](std::string s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        size_t i = 0; while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        return s.substr(i);
    };
    auto lower = [](std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; };
    std::string r, section; char line[512];
    while (std::fgets(line, sizeof line, f)) {
        std::string t = line;
        const auto c = t.find_first_of(";#");
        if (c != std::string::npos) t.resize(c);
        t = trim(t);
        if (t.empty()) continue;
        if (t.front() == '[') { const auto e = t.find(']'); section = lower(trim(t.substr(1, e == std::string::npos ? std::string::npos : e - 1))); continue; }
        const auto eq = t.find('=');
        if (eq == std::string::npos || section != "network") continue;
        if (lower(trim(t.substr(0, eq))) == key) r = trim(t.substr(eq + 1));
    }
    std::fclose(f);
    return r;
}
}  // namespace nr
