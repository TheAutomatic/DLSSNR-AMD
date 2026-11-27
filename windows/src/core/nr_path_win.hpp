#pragma once
// Windows' file functions with UTF-8 paths (nr_path_u8.hpp: why): each calls the wide ("W") function. The Windows
// builds only (the .asi, the ReShade add-on, the NGX DLLs).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

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

// ---- The game's folder not writable (Game Pass, Program Files without administrator rights) ----
// NR's own files - dlssnr-amd.ini (copied there once), dlssnr-amd.log, the crash dump, NR Screenshots, the diagnostics
// package and report, dlssnr-amd\'s caches (ACO, pipeline caches) - go to Documents\DLSSNR-AMD\<the game's exe name>\
// instead (TheAutomatic's e51d5d8 put them under %LOCALAPPDATA%; the user wanted a place easy to find). Decided once per
// folder, by writing a file there. Documents not writable either (Controlled Folder Access): nothing else is tried, the
// files stay where they were (their writes fail; the log and NR Control say so). No logging here (the log uses it).
inline std::string dir_key(std::string p) {
    for (char& c : p) if (c == '/') c = '\\';
    if (p.rfind("\\\\?\\", 0) == 0 && p.size() > 6 && p[5] == ':') p.erase(0, 4);   // (\\?\C:\..., as canonical() gives)
    while (p.size() > 1 && p.back() == '\\' && !(p.size() == 3 && p[1] == ':')) p.pop_back();
    return p;
}
inline bool make_dirs(const std::string& path) {
    const std::string p = dir_key(path);
    if (p.empty()) return false;
    const DWORD a = attributes(p);
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    for (size_t at = p.find('\\', 3); at != std::string::npos; at = p.find('\\', at + 1)) make_dir(p.substr(0, at));
    make_dir(p);
    const DWORD b = attributes(p);
    return b != INVALID_FILE_ATTRIBUTES && (b & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
inline bool dir_writable(const std::string& dir) {
    // a name of this process's, thread's and call's own: the .asi and the ReShade add-on (each with this code) may probe
    // one folder at once
    static std::atomic<unsigned> serial{0};
    const std::string probe = dir_key(dir.empty() ? std::string(".") : dir) + "\\.dlssnr-amd-write-" +
                              std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetCurrentThreadId()) + "-" +
                              std::to_string(serial.fetch_add(1, std::memory_order_relaxed)) + ".tmp";
    const HANDLE h = create_file(probe, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}
// The user's Documents folder as Explorer has it (moved, or OneDrive's): User Shell Folders' Personal, the value the
// Shell itself reads for it, its environment variables expanded; that value not there, the folder's own default
// %USERPROFILE%\Documents. Empty when not known. The registry and the environment only, no Shell / COM: this runs
// inside DllMain (OptiScaler calls the .asi's InitializeASI from its DllMain, the add-on starts in its own), under the
// loader lock, where the Shell's known-folder lookup may load and wait for other modules.
inline std::string documents_folder() {
    wchar_t raw[MAX_PATH * 4] = {};
    DWORD bytes = sizeof raw;
    const wchar_t* from = L"%USERPROFILE%\\Documents";
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",
                     L"Personal", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, raw, &bytes) == ERROR_SUCCESS &&
        raw[0])
        from = raw;
    wchar_t full[MAX_PATH * 4] = {};
    const DWORD n = ExpandEnvironmentStringsW(from, full, DWORD(MAX_PATH * 4));
    if (!n || n > MAX_PATH * 4) return {};
    // an absolute path only (C:\... or \\server\...): a variable the process does not have stays as %NAME% - no folder
    const bool drive = ((full[0] >= L'A' && full[0] <= L'Z') || (full[0] >= L'a' && full[0] <= L'z')) && full[1] == L':' &&
                       (full[2] == L'\\' || full[2] == L'/');
    const bool unc = full[0] == L'\\' && full[1] == L'\\' && full[2];
    if (!drive && !unc) return {};
    return dir_key(narrow(std::wstring(full)));
}
// A folder's identity (its volume and file index): one folder under two names - a junction and its target, \\?\ or
// not - is one. Empty when it cannot be opened. (kernel32 only: also inside DllMain)
inline std::string folder_identity(const std::string& dir) {
    const HANDLE h = create_file(dir_key(dir), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION i{};
    const BOOL ok = GetFileInformationByHandle(h, &i);
    CloseHandle(h);
    if (!ok) return {};
    char b[40];
    std::snprintf(b, sizeof b, "%08lx:%08lx%08lx", static_cast<unsigned long>(i.dwVolumeSerialNumber),
                  static_cast<unsigned long>(i.nFileIndexHigh), static_cast<unsigned long>(i.nFileIndexLow));
    return b;
}
struct WritableFolder {
    std::string path;    // where NR's files go: the game's folder, or the one in Documents
    bool moved = false;  // the game's folder cannot be written and the files are in Documents
    bool refused = false;  // neither can be written: the files stay in the game's folder (their writes fail)
};
inline const WritableFolder& writable_folder_of(const std::string& game) {
    static std::mutex lock;
    static std::map<std::string, const WritableFolder*> by_name;   // by the name asked for (lower case)
    static std::map<std::string, WritableFolder> made;           // by the folder's identity (else its name)
    std::string key = dir_key(game);
    for (char& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
    std::lock_guard<std::mutex> guard(lock);
    if (const auto it = by_name.find(key); it != by_name.end()) return *it->second;
    const std::string id = folder_identity(game);
    if (const auto it = made.find(id.empty() ? key : id); it != made.end()) return *(by_name[key] = &it->second);
    WritableFolder w;
    w.path = game;
    // NR_TEST_READONLY_GAME=1 (tests: a root process writes anywhere): the game's folder taken as not writable
    static const bool test_readonly = [] {
        char b[4] = {};
        return GetEnvironmentVariableA("NR_TEST_READONLY_GAME", b, sizeof b) && b[0] == '1';
    }();
    if (test_readonly || !dir_writable(game)) {
        w.refused = true;
        const std::string docs = documents_folder();
        // the game's exe name (its file name without .exe; a name of letters Windows does not allow in a folder name
        // has them as _), and - for another game with an exe of the same name - " (2)" ... " (9)": game-folder.txt in each
        // names the game's folder it is for
        std::string stem = "game";
        const std::string exe = module_file(nullptr);
        if (const size_t cut = exe.find_last_of("\\/"); cut != std::string::npos && cut + 1 < exe.size()) {
            stem = exe.substr(cut + 1);
            if (const size_t dot = stem.rfind('.'); dot != std::string::npos && dot > 0) stem.resize(dot);
            for (char& c : stem)
                if (std::strchr("<>:\"/\\|?*", c) || static_cast<unsigned char>(c) < 32) c = '_';
        }
        // first the folder already this game's (game-folder.txt naming its folder - or the same folder under another name,
        // a junction), wherever it is among the nine; else the first free one, claimed by making game-folder.txt there
        // (CREATE_NEW: of two starting together one gets it - the other reads it again: the same game takes it too) and
        // taken only when that file is written. A game-folder.txt that cannot be read is another game's; a slot that
        // cannot be made or written is passed over.
        const auto dir_of = [&](int n) {
            return docs + "\\DLSSNR-AMD\\" + stem + (n == 1 ? std::string() : " (" + std::to_string(n) + ")");
        };
        const auto take = [&](const std::string& dir) {
            if (!make_dirs(dir) || !dir_writable(dir)) return false;
            w.path = dir;
            w.moved = true;
            w.refused = false;
            return true;
        };
        // this game's: game-folder.txt there names its folder (or the same folder under another name)
        const auto ours = [&](const std::string& mark) {
            std::string named;
            if (FILE* f = fopen(mark, "rb")) {
                char b[2048];
                const size_t got = std::fread(b, 1, sizeof b, f);
                std::fclose(f);
                named.assign(b, got);
                while (!named.empty() && (named.back() == '\n' || named.back() == '\r')) named.pop_back();
            }
            return !named.empty() && (_stricmp(dir_key(named).c_str(), dir_key(game).c_str()) == 0 ||
                                      (!id.empty() && folder_identity(named) == id));
        };
        bool done = docs.empty();
        for (int n = 1; !done && n <= 9; ++n) {
            const std::string mark = dir_of(n) + "\\game-folder.txt";
            if (attributes(mark) != INVALID_FILE_ATTRIBUTES && ours(mark)) done = take(dir_of(n)) || true;
        }
        for (int n = 1; !done && n <= 9; ++n) {
            const std::string dir = dir_of(n), mark = dir + "\\game-folder.txt";
            if (attributes(mark) != INVALID_FILE_ATTRIBUTES) continue;   // (another game's)
            if (!make_dirs(dir) || !dir_writable(dir)) continue;
            const HANDLE h = create_file(mark, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) {   // claimed just now: this game's too (it started twice), or another's
                if (GetLastError() == ERROR_FILE_EXISTS) {
                    Sleep(20);   // (its text written in the moment after it was made)
                    if (ours(mark)) done = take(dir) || true;
                }
                continue;
            }
            const std::string text = dir_key(game) + "\r\n";
            DWORD put = 0;
            const bool written = WriteFile(h, text.data(), DWORD(text.size()), &put, nullptr) && put == text.size();
            CloseHandle(h);
            if (!written) { DeleteFileW(wide(mark).c_str()); continue; }
            done = take(dir);
        }
    }
    const WritableFolder& kept = made.emplace(id.empty() ? key : id, std::move(w)).first->second;
    return *(by_name[key] = &kept);
}
inline std::string writable_folder(const std::string& game, bool* moved = nullptr) {
    const WritableFolder& w = writable_folder_of(game);
    if (moved) *moved = w.moved;
    return w.moved ? w.path : game;   // (not moved: the caller's own name of the folder)
}
// A whole file written through a temporary beside it and a replacing move (a failed write never leaves it cut short):
// 0, else the Windows error.
inline DWORD write_whole(const std::string& path, const std::string& data) {
    const std::wstring to = wide(path), tmp = to + L".nrtmp";
    const HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD put = 0, e = 0;
    if (!WriteFile(h, data.data(), DWORD(data.size()), &put, nullptr) || put != data.size()) e = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
    if (!CloseHandle(h) && !e) e = GetLastError();
    if (!e && !MoveFileExW(tmp.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) e = GetLastError();
    if (e) DeleteFileW(tmp.c_str());
    return e;
}
// A copied file made writable (a game's files may carry the read-only attribute)
inline void clear_readonly(const std::string& p) {
    const DWORD a = attributes(p);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(wide(p).c_str(), a & ~DWORD(FILE_ATTRIBUTE_READONLY));
}
// The dlssnr-amd.ini NR reads and writes: the game's folder's; moved, the one in Documents, made from the game folder's
// (else from dlssnr-amd\dlssnr-amd.default.ini, the settings as installed) when the folder is decided. Asked on every
// evaluate (the NR chain's file, the network switch): decided once per folder, then the answer read without a lock (a
// deleted file is put back by restore_ini, nr_pe_config.cpp, as in the game's folder).
inline std::string ini_file(const std::string& game) {
    struct Answer { std::string game, ini; };
    static std::atomic<const Answer*> kept[4] = {};   // (one per folder string asked for - one or two a process - kept)
    for (const auto& k : kept)
        if (const Answer* a = k.load(std::memory_order_acquire); a && a->game == game) return a->ini;
    const WritableFolder& w = writable_folder_of(game);
    std::string ini = !w.moved ? (game.empty() ? std::string("dlssnr-amd.ini") : game + "\\dlssnr-amd.ini")
                               : w.path + "\\dlssnr-amd.ini";
    if (w.moved && attributes(ini) == INVALID_FILE_ATTRIBUTES) {
        const std::string base = dir_key(game);
        for (const std::string& from : {base + "\\dlssnr-amd.ini", base + "\\dlssnr-amd\\dlssnr-amd.default.ini"})
            if (attributes(from) != INVALID_FILE_ATTRIBUTES && copy_file(from, ini, TRUE)) {
                clear_readonly(ini);
                break;
            }
    }
    const Answer* made = new Answer{game, ini};
    for (auto& k : kept) {
        const Answer* none = nullptr;
        if (k.compare_exchange_strong(none, made, std::memory_order_acq_rel)) return ini;
        if (none->game == game) break;   // (another thread kept the same answer first)
    }
    delete made;   // (kept already, or more than four folder strings: decided again next time, the same way)
    return ini;
}
// Where the files NR makes under dlssnr-amd\ (its caches, the report) go: the game's dlssnr-amd\, or moved, the
// dlssnr-amd\ of the folder in Documents (made)
inline std::string writable_data(const std::string& game) {
    const WritableFolder& w = writable_folder_of(game);
    if (!w.moved) return game.empty() ? std::string("dlssnr-amd") : game + "\\dlssnr-amd";
    const std::string d = w.path + "\\dlssnr-amd";
    make_dirs(d);
    return d;
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

}  // namespace nr::u8
