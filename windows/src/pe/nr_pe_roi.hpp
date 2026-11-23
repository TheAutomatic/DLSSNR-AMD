#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace nr::pe::roi {

inline constexpr uint32_t side = 64, slots = 32;
inline constexpr unsigned stages = 3;

struct Region { uint32_t x, y, width, height; };

inline Region region(uint32_t width, uint32_t height) {
    const uint32_t w = std::min(side, width), h = std::min(side, height);
    // Centre of sampler block B63 at 3440x1440, where both polarities were reported.
    const auto start = [](uint32_t size, uint32_t extent, uint32_t percent) {
        return std::clamp<int64_t>(int64_t(size) * percent / 100 - extent / 2, 0, size - extent);
    };
    return {uint32_t(start(width, w, 40)), uint32_t(start(height, h, 76)), w, h};
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
