#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace nr::pe::roi {

inline constexpr uint32_t side = 64, slots = 32;
inline constexpr unsigned stages = 3;

struct Region { uint32_t x, y, width, height; };

// ROI patch centre as integer percentages [0, 100] of the frame dimensions.
// Default (40%, 76%) matches sampler block B63 at 3440x1440 (JFO flicker investigation);
// override with NR_DIAG_ROI_POS=X,Y (e.g. NR_DIAG_ROI_POS=50,50 for screen centre).
struct Percent { uint32_t x = 40, y = 76; };

inline Percent parse_percent(const char* spec) {
    Percent out{};
    if (!spec || !*spec) return out;
    char* end_x{};
    const long x = std::strtol(spec, &end_x, 10);
    if (end_x == spec || (*end_x != ',' && *end_x != 'x' && *end_x != 'X')) return out;
    const char* y_str = end_x + 1;
    char* end_y{};
    const long y = std::strtol(y_str, &end_y, 10);
    if (end_y == y_str || *end_y != '\0') return out;
    if (x < 0 || x > 100 || y < 0 || y > 100) return out;
    return {uint32_t(x), uint32_t(y)};
}

inline Percent configured_percent() {
    static const Percent p = parse_percent(std::getenv("NR_DIAG_ROI_POS"));
    return p;
}

inline Region region_at(uint32_t width, uint32_t height, Percent pos) {
    const uint32_t w = std::min(side, width), h = std::min(side, height);
    const auto start = [](uint32_t size, uint32_t extent, uint32_t percent) {
        return std::clamp<int64_t>(int64_t(size) * percent / 100 - extent / 2, 0, size - extent);
    };
    return {uint32_t(start(width, w, pos.x)), uint32_t(start(height, h, pos.y)), w, h};
}

inline Region region(uint32_t width, uint32_t height) {
    return region_at(width, height, configured_percent());
}

struct Luma {
    double mean{}, sd{};
    bool valid{};
};

template<class Decode>
Luma measure(const uint8_t* pixels, size_t pitch, size_t stride, uint32_t width, uint32_t height, Decode decode) {
    double sum = 0, sum_sq = 0;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            float rgb[3];
            if (!decode(pixels + y * pitch + x * stride, rgb)) return {};
            const double luma = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
            if (!std::isfinite(luma)) return {};
            sum += luma;
            sum_sq += luma * luma;
        }
    }
    const auto n = width * height;
    if (!n) return {};
    const double mean = sum / n;
    return {mean, std::sqrt(std::max(0.0, sum_sq / n - mean * mean)), true};
}

}  // namespace nr::pe::roi
