#pragma once
// [CC_SILVER_AARDWOLD_OPTIMIZED] Layers in dlssnr-amd.ini: its level, 0 Quality (none of its layers in int4), 1
// Balanced, 2 Performance, 3 Ultra Performance (the default; nr_ini_network.hpp); the words quality / balanced /
// performance / ultra performance (or ultraperformance, ultra) too. [CC_SMALL_FP8_OPTIMIZED] Layers the same for it (0 /
// 1 Quality, 2 Performance, 3 Ultra Performance; its default Quality); no other network has levels. Read by the runtime
// when its caller does not say (RuntimeConfig::layers), and by the config parser. -1: no file, no such key or no
// readable value - the network's default.
#include <cctype>
#include <cstdio>
#include <string>
#include "nr_path_u8.hpp"

namespace nr {
inline int layers_value(std::string v) {
    for (auto& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (v == "0" || v == "quality") return 0;
    if (v == "1" || v == "balanced") return 1;
    if (v == "2" || v == "performance") return 2;
    if (v == "3" || v == "ultra performance" || v == "ultraperformance" || v == "ultra") return 3;
    return -1;
}
// section: the network's codename, lower case ("cc_silver_aardwold_optimized", "cc_small_fp8_optimized")
inline int ini_layers(const std::string& path, const char* section_name) {
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
        if (eq == std::string::npos || section != section_name || lower(trim(t.substr(0, eq))) != "layers") continue;
        if (const int v = layers_value(trim(t.substr(eq + 1))); v >= 0) r = v;
    }
    std::fclose(f);
    return r;
}
}  // namespace nr
