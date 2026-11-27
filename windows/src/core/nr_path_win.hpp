#pragma once
// Windows' file functions with UTF-8 paths (nr_path_u8.hpp: why): each calls the wide ("W") function. The Windows
// builds only (the .asi, the ReShade add-on, the NGX DLLs).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "nr_path_u8.hpp"

namespace nr::u8 {

// GetModuleFileNameA's shape: the module's file, UTF-8, into `out` (`size` bytes with the NUL); its length, 0 when
// it failed or does not fit. No heap use: the crash handlers call it.
inline DWORD module_file(HMODULE m, char* out, DWORD size) {
    if (size) out[0] = 0;
    wchar_t w[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(m, w, MAX_PATH * 4);
    if (!n || n >= MAX_PATH * 4 || !size) return 0;
    const int got = WideCharToMultiByte(CP_UTF8, 0, w, int(n), out, int(size) - 1, nullptr, nullptr);
    if (got <= 0) { out[0] = 0; return 0; }
    out[got] = 0;
    return DWORD(got);
}
inline std::string module_file(HMODULE m) {
    std::wstring w(32768, L'\0');
    const DWORD n = GetModuleFileNameW(m, w.data(), DWORD(w.size()));
    if (!n || n >= w.size()) return {};
    w.resize(n);
    return narrow(w);
}

// Each leaves the call's GetLastError for its caller: the wide strings are freed first, then the error put back.
inline DWORD attributes(const std::string& p) {
    DWORD r, e;
    { const std::wstring w = wide(p); r = GetFileAttributesW(w.c_str()); e = GetLastError(); }
    SetLastError(e);
    return r;
}
inline BOOL attributes_ex(const std::string& p, WIN32_FILE_ATTRIBUTE_DATA* d) {
    BOOL r; DWORD e;
    { const std::wstring w = wide(p); r = GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, d); e = GetLastError(); }
    SetLastError(e);
    return r;
}
inline BOOL copy_file(const std::string& from, const std::string& to, BOOL fail_if_exists) {
    BOOL r; DWORD e;
    { const std::wstring a = wide(from), b = wide(to); r = CopyFileW(a.c_str(), b.c_str(), fail_if_exists); e = GetLastError(); }
    SetLastError(e);
    return r;
}
inline BOOL make_dir(const std::string& p, SECURITY_ATTRIBUTES* sa = nullptr) {
    BOOL r; DWORD e;
    { const std::wstring w = wide(p); r = CreateDirectoryW(w.c_str(), sa); e = GetLastError(); }
    SetLastError(e);
    return r;
}
inline HANDLE create_file(const std::string& p, DWORD access, DWORD share, SECURITY_ATTRIBUTES* sa, DWORD disposition,
                          DWORD flags, HANDLE templ) {
    HANDLE r; DWORD e;
    { const std::wstring w = wide(p); r = CreateFileW(w.c_str(), access, share, sa, disposition, flags, templ); e = GetLastError(); }
    SetLastError(e);
    return r;
}

// A folder's entries for a pattern ("C:\\x\\*.txt"): f(name in UTF-8, attributes) for each
template <class F> inline void list(const std::string& pattern, F f) {
    WIN32_FIND_DATAW fd{};
    const HANDLE h = FindFirstFileW(wide(pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do f(narrow(fd.cFileName), fd.dwFileAttributes);
    while (FindNextFileW(h, &fd));
    FindClose(h);
}

// GetPrivateProfileStringA's shape for our ini files in UTF-8 (GetPrivateProfileString reads a file without a BOM in
// the ANSI code page), a UTF-16 one (saved as "Unicode" by Notepad) read too: the key's value from the first matching
// section, names compared without case, the value trimmed and one pair of quotes around it taken off; `def` when there
// is none. Into `out` (`size` bytes); its length.
inline DWORD profile(const char* section, const char* key, const char* def, char* out, DWORD size, const char* file) {
    const auto lower = [](std::string s) { for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; };
    const auto trim = [](std::string s) {
        const size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return std::string();
        return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
    };
    std::string value = def ? def : "";
    std::string text;
    if (FILE* f = fopen(file, "rb")) {
        char buf[4096];
        for (size_t got; (got = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, got);
        std::fclose(f);
    }
    if (text.size() >= 2 && uint8_t(text[0]) == 0xFF && uint8_t(text[1]) == 0xFE) {   // UTF-16 LE
        std::wstring w((text.size() - 2) / 2, L'\0');
        for (size_t i = 0; i < w.size(); ++i) w[i] = wchar_t(uint8_t(text[2 + 2 * i]) | (uint8_t(text[3 + 2 * i]) << 8));
        text = narrow(w);
    } else if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        text.erase(0, 3);
    }
    const std::string want_sec = lower(section ? section : ""), want_key = lower(key ? key : "");
    bool in = false;
    for (size_t at = 0; at < text.size();) {
        const size_t eol = text.find('\n', at);
        std::string line = trim(text.substr(at, eol == std::string::npos ? std::string::npos : eol - at));
        at = eol == std::string::npos ? text.size() : eol + 1;
        if (line.empty()) continue;
        if (line.front() == '[') {
            const size_t close = line.find(']');
            const bool was = in;
            in = lower(trim(line.substr(1, close == std::string::npos ? std::string::npos : close - 1))) == want_sec;
            if (was && !in) break;   // the first matching section only
            continue;
        }
        const size_t eq = line.find('=');
        if (!in || eq == std::string::npos || lower(trim(line.substr(0, eq))) != want_key) continue;
        value = trim(line.substr(eq + 1));
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
            value = value.substr(1, value.size() - 2);
        break;
    }
    if (!size) return 0;
    const size_t n = std::min<size_t>(value.size(), size - 1);
    std::memcpy(out, value.data(), n);
    out[n] = 0;
    return DWORD(n);
}
inline DWORD profile(const char* section, const char* key, const char* def, char* out, DWORD size, const std::string& file) {
    return profile(section, key, def, out, size, file.c_str());
}

inline bool make_dirs(const char* path) {
    if (!path || !*path) return false;
    std::wstring w = wide(path);
    if (w.empty()) return false;
    for (wchar_t& c : w) if (c == L'/') c = L'\\';
    while (w.size() > 1 && w.back() == L'\\') {
        if (w.size() == 3 && w[1] == L':') break;
        w.pop_back();
    }
    const DWORD existing = GetFileAttributesW(w.c_str());
    if (existing != INVALID_FILE_ATTRIBUTES) return (existing & FILE_ATTRIBUTE_DIRECTORY) != 0;

    size_t start = 0;
    if (w.rfind(L"\\\\?\\", 0) == 0) {
        start = (w.size() >= 6 && w[5] == L':') ? 7 : 4;
    } else if (w.size() >= 2 && w[1] == L':') {
        start = (w.size() >= 3 && w[2] == L'\\') ? 3 : 2;
    } else if (w.rfind(L"\\\\", 0) == 0) {
        const size_t s1 = w.find(L'\\', 2);
        const size_t s2 = (s1 != std::wstring::npos) ? w.find(L'\\', s1 + 1) : std::wstring::npos;
        start = (s2 != std::wstring::npos) ? (s2 + 1) : w.size();
    } else if (w[0] == L'\\') {
        start = 1;
    }

    for (size_t pos = start; pos <= w.size(); ++pos) {
        if (pos == w.size() || w[pos] == L'\\') {
            if (pos > start) {
                const std::wstring sub = w.substr(0, pos);
                CreateDirectoryW(sub.c_str(), nullptr);
            }
        }
    }
    const DWORD created = GetFileAttributesW(w.c_str());
    return created != INVALID_FILE_ATTRIBUTES && (created & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

inline bool dir_writable(const char* path) {
    const char* target = (path && *path) ? path : ".";
    if (!make_dirs(target)) return false;
    static std::atomic<uint32_t> seq{0};
    std::wstring probe = wide(target);
    if (probe.empty()) return false;
    for (wchar_t& c : probe) if (c == L'/') c = L'\\';
    if (probe.back() != L'\\') probe.push_back(L'\\');
    probe += L".nr_write_";
    probe += std::to_wstring(GetCurrentProcessId());
    probe += L"_";
    probe += std::to_wstring(seq.fetch_add(1, std::memory_order_relaxed));
    probe += L".tmp";
    const HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    DeleteFileW(probe.c_str());
    return true;
}

inline std::string local_app_data_root() {
    auto env_w = [](const wchar_t* name) -> std::wstring {
        const DWORD need = GetEnvironmentVariableW(name, nullptr, 0);
        if (!need) return {};
        std::wstring out(need, L'\0');
        const DWORD got = GetEnvironmentVariableW(name, out.data(), need);
        if (!got || got >= need) return {};
        out.resize(got);
        return out;
    };
    std::wstring base = env_w(L"LOCALAPPDATA");
    if (base.empty()) {
        base = env_w(L"USERPROFILE");
        if (!base.empty()) {
            if (base.back() != L'\\') base.push_back(L'\\');
            base += L"AppData\\Local";
        }
    }
    if (base.empty()) {
        wchar_t tmp[MAX_PATH * 4]{};
        const DWORD n = GetTempPathW(MAX_PATH * 4, tmp);
        if (n && n < MAX_PATH * 4) base.assign(tmp, n);
    }
    if (base.empty()) return "DLSSNR-AMD";
    while (base.size() > 3 && base.back() == L'\\') base.pop_back();
    if (base.back() != L'\\') base.push_back(L'\\');
    base += L"DLSSNR-AMD";
    return narrow(base);
}

inline std::string normalize_dir_key(const std::string& p) {
    std::string k = p;
    for (char& c : k) if (c == '/') c = '\\';
    while (k.size() > 1 && k.back() == '\\') {
        if (k.size() == 3 && k[1] == ':') break;
        k.pop_back();
    }
    return k;
}

inline std::string app_storage_tag(const std::string& preferred) {
    std::string stem = "app";
    std::wstring exe(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, exe.data(), DWORD(exe.size()));
    if (n && n < exe.size()) {
        exe.resize(n);
        std::string u = narrow(exe);
        const size_t slash = u.find_last_of("\\/");
        if (slash != std::string::npos) u = u.substr(slash + 1);
        const size_t dot = u.rfind('.');
        if (dot != std::string::npos) u.resize(dot);
        std::string clean;
        clean.reserve(u.size());
        for (unsigned char c : u) {
            if (std::isalnum(c) || c == '-' || c == '_') clean.push_back(char(c));
            else clean.push_back('_');
        }
        if (!clean.empty()) {
            if (clean.size() > 32) clean.resize(32);
            stem = std::move(clean);
        }
    }
    const std::string norm = normalize_dir_key(preferred);
    uint32_t h = 2166136261u;
    for (unsigned char c : norm) {
        h ^= static_cast<unsigned char>(std::tolower(c));
        h *= 16777619u;
    }
    char hex[9];
    for (int i = 7; i >= 0; --i) {
        const unsigned nib = (h >> (i * 4)) & 0xfu;
        hex[7 - i] = char(nib < 10 ? '0' + nib : 'a' + (nib - 10));
    }
    hex[8] = '\0';
    return stem + "_" + hex;
}

inline std::string writable_cache_root(const std::string& preferred, bool* fell_back = nullptr) {
    struct Entry { std::string path; bool fb; };
    static std::mutex mu;
    static std::unordered_map<std::string, Entry> memo;
    const std::string key = normalize_dir_key(preferred);
    {
        std::lock_guard<std::mutex> lk(mu);
        const auto it = memo.find(key);
        if (it != memo.end()) {
            if (fell_back) *fell_back = it->second.fb;
            return it->second.path;
        }
    }
    Entry e;
    if (!key.empty() && dir_writable(key.c_str())) {
        e = {key, false};
    } else {
        std::string fb = local_app_data_root() + "\\cache";
        make_dirs(fb.c_str());
        e = {std::move(fb), true};
    }
    std::lock_guard<std::mutex> lk(mu);
    const auto [it, _] = memo.emplace(key, std::move(e));
    if (fell_back) *fell_back = it->second.fb;
    return it->second.path;
}

inline std::string writable_app_dir(const std::string& preferred, bool* fell_back = nullptr) {
    struct Entry { std::string path; bool fb; };
    static std::mutex mu;
    static std::unordered_map<std::string, Entry> memo;
    const std::string key = normalize_dir_key(preferred);
    {
        std::lock_guard<std::mutex> lk(mu);
        const auto it = memo.find(key);
        if (it != memo.end()) {
            if (fell_back) *fell_back = it->second.fb;
            return it->second.path;
        }
    }
    Entry e;
    if (dir_writable(key.empty() ? "." : key.c_str())) {
        e = {key, false};
    } else {
        std::string fb = local_app_data_root() + "\\apps\\" + app_storage_tag(key);
        make_dirs(fb.c_str());
        e = {std::move(fb), true};
    }
    std::lock_guard<std::mutex> lk(mu);
    const auto [it, _] = memo.emplace(key, std::move(e));
    if (fell_back) *fell_back = it->second.fb;
    return it->second.path;
}

inline void clear_readonly(const std::string& p) {
    const std::wstring w = wide(p);
    const DWORD a = GetFileAttributesW(w.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY) != 0)
        SetFileAttributesW(w.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
}

inline bool seed_writable_ini(const std::string& src_dir, const std::string& target_ini) {
    if (target_ini.empty() || attributes(target_ini) != INVALID_FILE_ATTRIBUTES) return false;
    const size_t cut = target_ini.find_last_of("\\/");
    if (cut != std::string::npos) make_dirs(target_ini.substr(0, cut).c_str());
    const std::string base = src_dir.empty() ? std::string(".") : normalize_dir_key(src_dir);
    const std::string sep = (!base.empty() && base.back() == '\\') ? "" : "\\";
    const std::string src_ini = base + sep + "dlssnr-amd.ini";
    if (attributes(src_ini) != INVALID_FILE_ATTRIBUTES && copy_file(src_ini, target_ini, TRUE)) {
        clear_readonly(target_ini);
        return true;
    }
    const std::string src_def = base + sep + "dlssnr-amd\\dlssnr-amd.default.ini";
    if (attributes(src_def) != INVALID_FILE_ATTRIBUTES && copy_file(src_def, target_ini, TRUE)) {
        clear_readonly(target_ini);
        return true;
    }
    return false;
}

inline std::string writable_ini_path(const std::string& folder, bool* fell_back = nullptr) {
    struct Entry { std::string path; bool fb; };
    static std::mutex mu;
    static std::unordered_map<std::string, Entry> memo;
    const std::string key = normalize_dir_key(folder);
    std::lock_guard<std::mutex> lk(mu);
    if (const auto it = memo.find(key); it != memo.end()) {
        if (fell_back) *fell_back = it->second.fb;
        return it->second.path;
    }
    const auto join_ini = [](const std::string& d) {
        if (d.empty()) return std::string("dlssnr-amd.ini");
        return d + (d.back() == '\\' ? "" : "\\") + "dlssnr-amd.ini";
    };
    bool fb = false;
    const std::string app_dir = writable_app_dir(key, &fb);
    Entry e;
    if (!fb) {
        e = {join_ini(key), false};
    } else {
        std::string target = join_ini(app_dir);
        seed_writable_ini(key, target);
        e = {std::move(target), true};
    }
    const auto [it, _] = memo.emplace(key, std::move(e));
    if (fell_back) *fell_back = it->second.fb;
    return it->second.path;
}

}  // namespace nr::u8
