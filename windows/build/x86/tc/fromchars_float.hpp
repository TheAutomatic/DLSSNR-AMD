#pragma once
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <cerrno>
namespace std {
inline from_chars_result from_chars(const char* first, const char* last, float& v, chars_format = chars_format::general) {
    char buf[64]; size_t n = size_t(last - first); if (n >= sizeof buf) n = sizeof buf - 1;
    std::memcpy(buf, first, n); buf[n] = 0;
    errno = 0; v = std::strtof(buf, nullptr);
    return {last, errno == ERANGE ? errc::result_out_of_range : errc()};
}
inline from_chars_result from_chars(const char* first, const char* last, double& v, chars_format = chars_format::general) {
    char buf[64]; size_t n = size_t(last - first); if (n >= sizeof buf) n = sizeof buf - 1;
    std::memcpy(buf, first, n); buf[n] = 0;
    errno = 0; v = std::strtod(buf, nullptr);
    return {last, errno == ERANGE ? errc::result_out_of_range : errc()};
}
}

// mingw 10.3's d3d12.h has no D3D12_SAMPLE_POSITION (the SDK's does).
struct D3D12_SAMPLE_POSITION { signed char X; signed char Y; };
