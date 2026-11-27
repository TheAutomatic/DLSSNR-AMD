#pragma once
#include <cstdint>
#include <string>
namespace nr {
struct NativePlan {
    uint32_t width{}, height{}, reductions_x{}, reductions_y{};
    std::string text, model;
};
// Source and returned working dimensions are raster dimensions. Plan scalars
// preserve the recovered host order (height, width); pixels are never rotated.
// The network the plan walks: CC_SILVER_AARDWOLD (310.8, also preset 1 of the 310.9 DLL) or CC_SMALL_FP8 (310.9,
// preset 2): same layer shapes and kernels, fewer blocks.
enum class NetworkModel { SilverAardwold, SmallFp8 };
NativePlan make_native_plan(uint32_t source_width, uint32_t source_height,
                            NetworkModel model = NetworkModel::SilverAardwold);
}
