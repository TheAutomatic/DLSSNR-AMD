// Shared helpers: files, hashing, JSON writing. CPU-only, no D3D12.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace lab {

using Bytes = std::vector<uint8_t>;

inline void require(bool ok, const std::string& why) {
    if (!ok) throw std::runtime_error(why);
}

inline Bytes read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    require(bool(f), "cannot open file: " + p);
    auto n = f.tellg();
    require(n > 0 && n <= 256 * 1024 * 1024, "file size outside limit: " + p);
    Bytes b(static_cast<size_t>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(b.data()), n);
    require(bool(f), "cannot read file: " + p);
    return b;
}

inline void write_file(const std::string& p, const void* data, size_t n) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    require(bool(f), "cannot create file: " + p);
    if (n) f.write(reinterpret_cast<const char*>(data), std::streamsize(n));
    require(bool(f), "cannot write file: " + p);
}

inline void write_file(const std::string& p, const std::string& s) {
    write_file(p, s.data(), s.size());
}

template <class T>
inline void write_file(const std::string& p, const std::vector<T>& v) {
    write_file(p, v.data(), v.size() * sizeof(T));
}

inline std::string wide_to_utf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    require(n > 0, "WideCharToMultiByte failed");
    std::string s(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// ---------- hashing ----------
inline uint64_t fnv64(const void* data, size_t n) {
    uint64_t h = 1469598103934665603ull;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    static constexpr uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };
    static uint32_t ror(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
    void block(const uint8_t* p) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
                   (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hp = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t t1 = hp + S1 + ch + k[i] + w[i];
            uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
            uint32_t maj = (a & b) ^ (b & c) ^ (a & c);
            uint32_t t2 = S0 + maj;
            hp = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hp;
    }
    std::string hex() const {
        char buf[65];
        for (int i = 0; i < 8; ++i)
            snprintf(buf + i * 8, 9, "%08x", h[i]);
        return std::string(buf, 64);
    }
};

inline std::string sha256_hex(const void* data, size_t len) {
    Sha256 s;
    const auto* p = static_cast<const uint8_t*>(data);
    const size_t full = len / 64;
    for (size_t i = 0; i < full; ++i) s.block(p + i * 64);
    uint8_t tail[128] = {};
    const size_t rem = len - full * 64;
    memcpy(tail, p + full * 64, rem);
    tail[rem] = 0x80;
    const size_t blocks = rem >= 56 ? 2 : 1;
    const uint64_t bits = uint64_t(len) * 8;
    for (int i = 0; i < 8; ++i) tail[blocks * 64 - 1 - i] = uint8_t(bits >> (i * 8));
    for (size_t i = 0; i < blocks; ++i) s.block(tail + i * 64);
    return s.hex();
}

inline std::string sha256_file(const std::string& p) {
    auto b = read_file(p);
    return sha256_hex(b.data(), b.size());
}

// ---------- minimal JSON writer ----------
struct Json {
    std::ostringstream o;
    bool first = true;
    std::string str() const { return o.str(); }
    Json& raw(const char* k, const std::string& raw_json) {
        if (!first) o << ",";
        first = false;
        if (k) o << "\"" << k << "\":";
        o << raw_json;
        return *this;
    }
    static std::string esc(const std::string& s) {
        std::string out = "\"";
        for (char c : s) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (uint8_t(c) < 0x20) {
                    char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b;
                } else out += c;
            }
        }
        return out + "\"";
    }
    Json& s(const char* k, const std::string& v) { return raw(k, esc(v)); }
    Json& u(const char* k, uint64_t v) {
        if (!first) o << ",";
        first = false;
        if (k) o << "\"" << k << "\":";
        o << v;
        return *this;
    }
    Json& i(const char* k, int64_t v) {
        if (!first) o << ",";
        first = false;
        if (k) o << "\"" << k << "\":";
        o << v;
        return *this;
    }
    Json& b(const char* k, bool v) { return raw(k, v ? "true" : "false"); }
    Json& n(const char* k) {
        if (!first) o << ",";
        first = false;
        if (k) o << "\"" << k << "\":";
        o << "null";
        return *this;
    }
    Json& obj(const char* k) { if (!first) o << ","; first = false; if (k) o << "\"" << k << "\":"; o << "{"; first = true; return *this; }
    Json& arr(const char* k) { if (!first) o << ","; first = false; if (k) o << "\"" << k << "\":"; o << "["; first = true; return *this; }
    Json& end_obj() { o << "}"; first = false; return *this; }
    Json& end_arr() { o << "]"; first = false; return *this; }
};

inline void write_json(const std::string& path, const Json& j) {
    write_file(path, j.str() + "\n");
}

}  // namespace lab
