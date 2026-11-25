#include "nr_pe_roi.hpp"

#include <cassert>
#include <cstdio>

int main() {
    using namespace nr::pe::roi;
    const auto full = region_at(3440, 1440, Percent{});
    assert(full.x == 1344 && full.y == 1062 && full.width == 64 && full.height == 64);
    const auto configured = region(3440, 1440);
    const auto expected = region_at(3440, 1440, configured_percent());
    assert(configured.x == expected.x && configured.y == expected.y &&
           configured.width == expected.width && configured.height == expected.height);
    const auto small = region_at(10, 8, Percent{});
    assert(small.x == 0 && small.y == 0 && small.width == 10 && small.height == 8);
    const auto empty = region_at(0, 0, {50, 50});
    assert(empty.x == 0 && empty.y == 0 && empty.width == 0 && empty.height == 0);

    const auto centre = region_at(1920, 1080, parse_percent("50,50"));
    assert(centre.x == 928 && centre.y == 508 && centre.width == 64 && centre.height == 64);
    const auto corners = region_at(1920, 1080, parse_percent("0x100"));
    assert(corners.x == 0 && corners.y == 1016 && corners.width == 64 && corners.height == 64);

    for (const char* bad : {static_cast<const char*>(nullptr), "", "50", "50,", "-1,50", "50,101", "50,50abc", "abc,def"}) {
        const auto p = parse_percent(bad);
        assert(p.x == 40 && p.y == 76);
    }

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
