#include "nr_pe_roi.hpp"

#include <cassert>
#include <cstdio>

int main() {
    using namespace nr::pe::roi;
    const auto full = region(3440, 1440);
    assert(full.x == 1344 && full.y == 1062 && full.width == 64 && full.height == 64);
    const auto small = region(10, 8);
    assert(small.x == 0 && small.y == 0 && small.width == 10 && small.height == 8);

    const uint8_t image[] = {0, 0, 0, 255, 255, 255, 99, 99, 255, 0, 0, 255, 0, 0, 99, 99};
    const auto rgb = [](const uint8_t* p, float* rgb) {
        for (int i = 0; i < 3; ++i) rgb[i] = p[i] / 255.f;
        return true;
    };
    const auto m = measure(image, 8, 3, 2, 2, rgb);
    const double mean = (1 + 2 * 0.2126) / 4;
    const double sd = std::sqrt((1 + 2 * 0.2126 * 0.2126) / 4 - mean * mean);
    assert(m.valid && std::abs(m.mean - mean) < 1e-6 && std::abs(m.sd - sd) < 1e-6);
    assert(!measure(image, 8, 3, 0, 2, rgb).valid);
    const auto unsupported = [](const uint8_t*, float*) { return false; };
    assert(!measure(image, 8, 3, 2, 2, unsupported).valid);
    std::puts("ReShade ROI region and luma: 0 failures");
}
