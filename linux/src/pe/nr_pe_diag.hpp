#pragma once
// NR Control's diagnostic package (Diagnostics > Save a diagnostic package): one zip in the game's folder,
// dlssnr-amd-diag-<date>-<time>.zip, for a bug report - the report (nr_control_pages.hpp report_text, with the session's
// run statistics), the identity of the files that run (size and SHA-256 of the programs; the data folders listed), the
// system (Windows or Wine version, the GPU and its driver as Vulkan names them), every log and settings file in the game's
// folder (logs above 20 MB: their last 20 MB) and the crash dump if there is one; with "screenshots" ticked also the
// screenshot pair F5 takes of the current frame. Written on a thread of its own (the game keeps running); stored, not
// compressed (no compression library in the program; a zip tool shrinks it further). Nothing is sent anywhere.
#include "nr_path_u8.hpp"
#include "nr_pe_log.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nr::pe::diag {

// ---- CRC-32 (zip) and SHA-256 (file identity) ----
inline uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static std::once_flag once;
    std::call_once(once, [] {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    });
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
inline std::string sha256_hex(const std::string& data) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
        0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
        0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string m = data;
    const uint64_t bits = uint64_t(data.size()) * 8;
    m += char(0x80);
    while (m.size() % 64 != 56) m += char(0);
    for (int i = 7; i >= 0; --i) m += char((bits >> (i * 8)) & 0xFF);
    const auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(uint8_t(m[off + 4 * i])) << 24 | uint32_t(uint8_t(m[off + 4 * i + 1])) << 16 |
                   uint32_t(uint8_t(m[off + 4 * i + 2])) << 8 | uint32_t(uint8_t(m[off + 4 * i + 3]));
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    char out[65];
    for (int i = 0; i < 8; ++i) std::snprintf(out + 8 * i, 9, "%08x", h[i]);
    return std::string(out, 64);
}

// ---- a zip of stored entries (method 0), UTF-8 names ----
class Zip {
public:
    explicit Zip(const std::filesystem::path& path) { f_ = _wfopen(path.c_str(), L"wb"); }
    ~Zip() { if (f_) std::fclose(f_); }
    bool ok() const { return f_ && !failed_; }
    void add(const std::string& name, const std::string& data) {
        if (!f_ || data.size() >= 0xFFFFFFFFu) return;
        Entry e{name, crc32(reinterpret_cast<const uint8_t*>(data.data()), data.size()), uint32_t(data.size()),
                uint32_t(std::ftell(f_))};
        std::string h;
        put32(h, 0x04034b50); put16(h, 20); put16(h, 0x0800); put16(h, 0); put16(h, time_); put16(h, date_);
        put32(h, e.crc); put32(h, e.size); put32(h, e.size); put16(h, uint16_t(name.size())); put16(h, 0);
        h += name;
        write(h);
        write(data);
        entries_.push_back(e);
    }
    bool finish() {
        if (!f_) return false;
        const uint32_t start = uint32_t(std::ftell(f_));
        std::string c;
        for (const Entry& e : entries_) {
            put32(c, 0x02014b50); put16(c, 20); put16(c, 20); put16(c, 0x0800); put16(c, 0); put16(c, time_);
            put16(c, date_); put32(c, e.crc); put32(c, e.size); put32(c, e.size); put16(c, uint16_t(e.name.size()));
            put16(c, 0); put16(c, 0); put16(c, 0); put16(c, 0); put32(c, 0); put32(c, e.offset);
            c += e.name;
        }
        put32(c, 0x06054b50); put16(c, 0); put16(c, 0); put16(c, uint16_t(entries_.size()));
        put16(c, uint16_t(entries_.size())); put32(c, uint32_t(c.size())); put32(c, start); put16(c, 0);
        // (the central directory's size is the bytes before the end record)
        const uint32_t dir = uint32_t(c.size() - 22);
        std::memcpy(&c[c.size() - 10], &dir, 4);
        write(c);
        const bool good = !failed_ && std::fclose(f_) == 0;
        f_ = nullptr;
        return good;
    }
    void stamp(const SYSTEMTIME& t) {
        time_ = uint16_t(t.wHour << 11 | t.wMinute << 5 | t.wSecond / 2);
        date_ = uint16_t((t.wYear - 1980) << 9 | t.wMonth << 5 | t.wDay);
    }

private:
    struct Entry { std::string name; uint32_t crc, size, offset; };
    static void put16(std::string& s, uint16_t v) { s += char(v & 0xFF); s += char(v >> 8); }
    static void put32(std::string& s, uint32_t v) { put16(s, uint16_t(v & 0xFFFF)); put16(s, uint16_t(v >> 16)); }
    void write(const std::string& s) { if (std::fwrite(s.data(), 1, s.size(), f_) != s.size()) failed_ = true; }
    FILE* f_ = nullptr;
    bool failed_ = false;
    uint16_t time_ = 0, date_ = 0x21;
    std::vector<Entry> entries_;
};

// A file's bytes (at most `cap` of them, the last ones: a log's newest lines), and its whole size.
inline bool read_file(const std::filesystem::path& p, std::string& out, uint64_t cap, uint64_t* size = nullptr) {
    out.clear();
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END);
    const int64_t n = _ftelli64(f);
    if (size) *size = uint64_t(std::max<int64_t>(n, 0));
    const int64_t from = n > int64_t(cap) ? n - int64_t(cap) : 0;
    _fseeki64(f, from, SEEK_SET);
    out.resize(size_t(n - from));
    const size_t got = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), f);
    out.resize(got);
    std::fclose(f);
    return true;
}

// What the package is made from: taken on the menu's thread, the rest on the package's own.
struct Request {
    std::filesystem::path game;     // the game's folder (dlssnr-amd.ini's)
    std::filesystem::path log;      // dlssnr-amd.log
    std::string report;             // report_text (with the run statistics)
    std::string gpu, driver;        // as Vulkan names them
    bool shots = false;             // the screenshot pair of the current frame too
    std::function<void()> request_shot;   // asks for that pair (NR's screenshot, as F5)
};

struct State {
    std::mutex m;
    std::string said;               // the last package's outcome, for the menu
    std::atomic<bool> busy{false};
};
inline State& state() { static State s; return s; }
inline std::string said() { std::lock_guard<std::mutex> g(state().m); return state().said; }
inline bool busy() { return state().busy.load(); }

inline std::string system_text(const Request& r) {
    std::string t;
    char b[512];
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    if (const auto fn = ntdll ? reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion"))) : nullptr;
        fn && fn(&v) == 0) {
        std::snprintf(b, sizeof b, "Windows version: %lu.%lu.%lu\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
        t += b;
    }
    // Wine (Proton) says what it is through ntdll's own exports
    using WineVersionFn = const char*(CDECL*)();
    using WineHostFn = void(CDECL*)(const char**, const char**);
    if (const auto wine = ntdll ? reinterpret_cast<WineVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "wine_get_version"))) : nullptr) {
        t += std::string("Wine: ") + wine() + "\n";
        if (const auto host = reinterpret_cast<WineHostFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "wine_get_host_version")))) {
            const char* sys = nullptr;
            const char* rel = nullptr;
            host(&sys, &rel);
            t += std::string("Host: ") + (sys ? sys : "?") + " " + (rel ? rel : "?") + "\n";
        }
    }
    for (const char* name : {"PROTON_VERSION", "SteamAppId", "WINEDLLOVERRIDES", "DXVK_ASYNC", "VKD3D_CONFIG", "RADV_PERFTEST",
                             "PROTON_FSR4_UPGRADE", "PROTON_ENABLE_NVAPI"}) {
        char value[512] = {};
        if (GetEnvironmentVariableA(name, value, sizeof value)) t += std::string(name) + "=" + value + "\n";
    }
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof mem;
    if (GlobalMemoryStatusEx(&mem)) {
        std::snprintf(b, sizeof b, "RAM: %.1f GB, %.1f GB free; commit limit %.1f GB, %.1f GB free\n",
                      double(mem.ullTotalPhys) / (1 << 30), double(mem.ullAvailPhys) / (1 << 30),
                      double(mem.ullTotalPageFile) / (1 << 30), double(mem.ullAvailPageFile) / (1 << 30));
        t += b;
    }
    t += "GPU: " + (r.gpu.empty() ? std::string("not seen yet") : r.gpu) + "\n";
    t += "Driver: " + (r.driver.empty() ? std::string("not seen yet") : r.driver) + "\n";
    return t;
}

// The programs that run (size and SHA-256) and the data folders (a listing), relative to the game's folder.
inline std::string identity_text(const std::filesystem::path& game) {
    std::string t;
    char b[512];
    std::error_code ec;
    const auto program = [](const std::filesystem::path& p) {
        std::string ext = p.extension().string();
        for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
        return ext == ".dll" || ext == ".asi" || ext == ".exe" || ext == ".addon64" || ext == ".addon32" || ext == ".ini" ||
               ext == ".txt" || ext == ".json";
    };
    std::vector<std::filesystem::path> files;
    for (const char* dir : {"", "OptiScaler", "OptiScaler/plugins", "dlssnr-amd", "reshade-shaders/Shaders"})
        for (std::filesystem::directory_iterator it(game / nr::u8::path(dir), ec), end; !ec && it != end;
             it.increment(ec))
            if (it->is_regular_file(ec) && program(it->path())) files.push_back(it->path());
    std::sort(files.begin(), files.end());
    t += "Programs and settings (bytes, SHA-256):\n";
    for (const auto& p : files) {
        std::string data;
        uint64_t size = 0;
        if (!read_file(p, data, 256ull << 20, &size)) continue;
        std::snprintf(b, sizeof b, "  %12llu %s %s\n", static_cast<unsigned long long>(size),
                      size <= (256ull << 20) ? sha256_hex(data).c_str() : "(too big to hash)",
                      nr::u8::narrow(std::filesystem::relative(p, game, ec).wstring()).c_str());
        t += b;
    }
    t += "\nData (bytes):\n";
    for (const char* dir : {"dlssnr-amd"}) {
        for (std::filesystem::recursive_directory_iterator it(game / dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            std::snprintf(b, sizeof b, "  %12llu %s\n", static_cast<unsigned long long>(it->file_size(ec)),
                          nr::u8::narrow(std::filesystem::relative(it->path(), game, ec).wstring()).c_str());
            t += b;
        }
    }
    return t;
}

// The package, on a thread of its own; false when one is being written already.
inline bool start(Request r) {
    if (state().busy.exchange(true)) return false;
    { std::lock_guard<std::mutex> g(state().m); state().said = "writing the diagnostic package..."; }
    const auto shot_from = std::filesystem::file_time_type::clock::now();
    if (r.shots && r.request_shot) r.request_shot();
    std::thread([r = std::move(r), shot_from] {
        std::string outcome;
        try {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            char stamp[32];
            std::snprintf(stamp, sizeof stamp, "%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour,
                          now.wMinute, now.wSecond);
            const std::filesystem::path out = r.game / (std::string("dlssnr-amd-diag-") + stamp + ".zip");
            Zip zip(out);
            zip.stamp(now);
            zip.add("report.txt", r.report);
            zip.add("system.txt", system_text(r));
            zip.add("files.txt", identity_text(r.game));
            constexpr uint64_t kCap = 20ull << 20;
            std::error_code ec;
            std::vector<std::filesystem::path> taken;
            const auto take = [&](const std::filesystem::path& p, const std::string& as) {
                if (std::find(taken.begin(), taken.end(), p) != taken.end()) return;
                std::string data;
                uint64_t size = 0;
                if (!read_file(p, data, kCap, &size)) return;
                taken.push_back(p);
                zip.add(as, size > kCap ? "(the last 20 MB of " + std::to_string(size) + " bytes)\n" + data : data);
            };
            take(r.log, "logs/" + nr::u8::narrow(r.log.filename().wstring()));
            for (std::filesystem::directory_iterator it(r.game, ec), end; !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                std::string ext = it->path().extension().string();
                for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
                const std::string name = nr::u8::narrow(it->path().filename().wstring());
                if (ext == ".log") take(it->path(), "logs/" + name);
                else if (ext == ".ini" || name == "dlssnr-amd-install.txt") take(it->path(), "settings/" + name);
                else if (ext == ".dmp") take(it->path(), "crash/" + name);
            }
            take(r.game / "dlssnr-amd" / "nr-report.txt", "settings/nr-report.txt");
            if (r.shots) {   // NR's screenshot pair of the current frame: written within a few seconds of the request
                const std::filesystem::path shots = r.game / "NR Screenshots";
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
                std::vector<std::filesystem::path> fresh;
                while (fresh.size() < 2 && std::chrono::steady_clock::now() < until) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    fresh.clear();
                    for (std::filesystem::directory_iterator it(shots, ec), end; !ec && it != end; it.increment(ec))
                        if (it->is_regular_file(ec) && it->last_write_time(ec) >= shot_from) fresh.push_back(it->path());
                }
                std::this_thread::sleep_for(std::chrono::seconds(1));   // (the pair's second file may still be written)
                for (const auto& p : fresh) take(p, "screenshots/" + nr::u8::narrow(p.filename().wstring()));
                if (fresh.empty()) zip.add("screenshots/none.txt", "No screenshot was written within 8 s of the request.\n");
            }
            outcome = zip.finish() ? "saved " + nr::u8::narrow(out.wstring()) : "the package could not be written to " +
                                                                                   nr::u8::narrow(out.wstring());
        } catch (const std::exception& e) {
            outcome = std::string("the package could not be written: ") + e.what();
        }
        nr::pe::log("[nr] diagnostic package: %s", outcome.c_str());
        { std::lock_guard<std::mutex> g(state().m); state().said = outcome; }
        state().busy = false;
    }).detach();
    return true;
}

}  // namespace nr::pe::diag
