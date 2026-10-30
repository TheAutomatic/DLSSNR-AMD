#include "nr_path_win.hpp"
#include "nr_pe_crash.hpp"

#include "nr_pe_log.hpp"

#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

namespace nr::pe {
namespace {

PVOID handler = nullptr;
HMODULE dbghelp = nullptr;
std::atomic<int> reports{0};
std::atomic<bool> dumped{false};

bool fatal(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_PRIV_INSTRUCTION: case EXCEPTION_IN_PAGE_ERROR:
        case 0xC0000374:   // heap corruption
            return true;
        default:
            return false;
    }
}

// "module+0xoffset", or the bare address when it is in no module.
std::string where(DWORD64 at) {
    HMODULE m = nullptr;
    char path[MAX_PATH * 4] = {};
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(at), &m) || !m || !nr::u8::module_file(m, path, sizeof path)) {
        char b[32];
        std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(at));
        return b;
    }
    const char* name = std::strrchr(path, '\\');
    char b[MAX_PATH * 4 + 32];
    std::snprintf(b, sizeof b, "%s+0x%llx", name ? name + 1 : path,
                  static_cast<unsigned long long>(at - reinterpret_cast<DWORD64>(m)));
    return b;
}

void write_dump(EXCEPTION_POINTERS* ep) {
    if (!dbghelp || dumped.exchange(true)) return;
    using Fn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                             PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    if (!fn) return;
    // (the wide path on the stack: no heap use here, the heap may be what broke)
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    if (!n || n >= MAX_PATH * 2 - 24) return;
    if (wchar_t* s = std::wcsrchr(path, L'\\')) std::wcscpy(s + 1, L"dlssnr-amd-crash.dmp");
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), ep, FALSE};
    const BOOL ok = fn(GetCurrentProcess(), GetCurrentProcessId(), f,
                       MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                                     MiniDumpWithUnloadedModules),
                       &info, nullptr, nullptr);
    CloseHandle(f);
    log("[nr]   minidump %s: %s", path, ok ? "written" : "FAILED");
}

LONG CALLBACK on_exception(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* rec = ep ? ep->ExceptionRecord : nullptr;
    if (!rec || !fatal(rec->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    if (reports.fetch_add(1) >= 3) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD64 at = reinterpret_cast<DWORD64>(rec->ExceptionAddress);
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
        log("[nr] exception 0x%08lX (%s 0x%llx) at %s, thread %lu", rec->ExceptionCode,
            rec->ExceptionInformation[0] == 1 ? "write" : rec->ExceptionInformation[0] == 8 ? "execute" : "read",
            static_cast<unsigned long long>(rec->ExceptionInformation[1]), where(at).c_str(), GetCurrentThreadId());
    else
        log("[nr] exception 0x%08lX at %s, thread %lu", rec->ExceptionCode, where(at).c_str(), GetCurrentThreadId());
#if defined(_M_X64) || defined(__x86_64__)
    CONTEXT c = *ep->ContextRecord;
    for (int i = 0; i < 20 && c.Rip; ++i) {
        DWORD64 base = 0;
        if (PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &base, nullptr)) {
            PVOID data = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fe, &c, &data, &frame, nullptr);
        } else {
            DWORD64 ret = 0;
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(c.Rsp), &ret, sizeof ret, &got) ||
                got != sizeof ret)
                break;
            c.Rip = ret;
            c.Rsp += 8;
        }
        if (!c.Rip) break;
        log("[nr]   from %s", where(c.Rip).c_str());
    }
#endif
    write_dump(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void install_crash_watch() {
    if (handler) return;
    // Loaded now: nothing may be loaded from inside the handler.
    dbghelp = LoadLibraryW(L"dbghelp.dll");
    // First in line: a game's or another hook's own handler may end the process before a later one is asked (games
    // on Windows ended with nothing logged)
    handler = AddVectoredExceptionHandler(1, &on_exception);
}

void remove_crash_watch() {
    if (handler) RemoveVectoredExceptionHandler(handler);
    handler = nullptr;
}

}  // namespace nr::pe
