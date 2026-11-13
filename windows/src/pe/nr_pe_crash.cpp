#include "nr_path_win.hpp"
#include "nr_pe_crash.hpp"

#include "nr_pe_log.hpp"

#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

namespace nr::pe {
namespace {

PVOID handler = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER prev_uef = nullptr;
HMODULE dbghelp = nullptr;
uintptr_t g_self_begin = 0;
uintptr_t g_self_end = 0;
uintptr_t g_host_begin = 0;
uintptr_t g_host_end = 0;
std::atomic<int> reports{0};
std::atomic<bool> dumped{false};

struct LastErrorGuard {
    DWORD err = GetLastError();
    ~LastErrorGuard() { SetLastError(err); }
};

void module_bounds(HMODULE m, uintptr_t& begin, uintptr_t& end) {
    begin = 0;
    end = 0;
    if (!m) return;
    const auto base = reinterpret_cast<uintptr_t>(m);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + static_cast<uintptr_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE || !nt->OptionalHeader.SizeOfImage) return;
    begin = base;
    end = base + static_cast<uintptr_t>(nt->OptionalHeader.SizeOfImage);
}

bool in_our_modules(uintptr_t addr) {
    return (addr >= g_self_begin && addr < g_self_end) || (addr >= g_host_begin && addr < g_host_end);
}

bool is_our_context(const EXCEPTION_RECORD* rec, const CONTEXT* ctx) {
    if (rec && in_our_modules(reinterpret_cast<uintptr_t>(rec->ExceptionAddress))) return true;
#if defined(_M_X64) || defined(__x86_64__)
    if (ctx) {
        if (in_our_modules(static_cast<uintptr_t>(ctx->Rip))) return true;
        const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
        if (tib) {
            const auto lo = reinterpret_cast<uintptr_t>(tib->StackLimit);
            const auto hi = reinterpret_cast<uintptr_t>(tib->StackBase);
            uintptr_t sp = static_cast<uintptr_t>(ctx->Rsp) & ~uintptr_t(7);
            if (sp >= lo && sp < hi) {
                for (int i = 0; i < 32 && sp + 8 <= hi; ++i, sp += 8) {
                    if (in_our_modules(*reinterpret_cast<const uintptr_t*>(sp))) return true;
                }
            }
        }
    }
#endif
    return false;
}

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
    if (dumped.exchange(true)) return;
    HMODULE mod = dbghelp ? dbghelp : GetModuleHandleW(L"dbghelp.dll");
    if (!mod) mod = LoadLibraryW(L"dbghelp.dll");
    if (!mod) return;
    using Fn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                             PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(mod, "MiniDumpWriteDump"));
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

void report_exception(const char* label, EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* rec = ep ? ep->ExceptionRecord : nullptr;
    if (!rec) return;
    if (reports.fetch_add(1) >= 3) return;
    const DWORD64 at = reinterpret_cast<DWORD64>(rec->ExceptionAddress);
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
        log("[nr] %s 0x%08lX (%s 0x%llx) at %s, thread %lu", label, rec->ExceptionCode,
            rec->ExceptionInformation[0] == 1 ? "write" : rec->ExceptionInformation[0] == 8 ? "execute" : "read",
            static_cast<unsigned long long>(rec->ExceptionInformation[1]), where(at).c_str(), GetCurrentThreadId());
    else
        log("[nr] %s 0x%08lX at %s, thread %lu", label, rec->ExceptionCode, where(at).c_str(), GetCurrentThreadId());
#if defined(_M_X64) || defined(__x86_64__)
    if (ep->ContextRecord) {
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
    }
#endif
    write_dump(ep);
}

LONG CALLBACK on_exception(EXCEPTION_POINTERS* ep) {
    LastErrorGuard guard;
    const EXCEPTION_RECORD* rec = ep ? ep->ExceptionRecord : nullptr;
    if (!rec) return EXCEPTION_CONTINUE_SEARCH;
    if (!is_our_context(rec, ep->ContextRecord)) return EXCEPTION_CONTINUE_SEARCH;
    if (!fatal(rec->ExceptionCode)) {
        // After the first evaluate (trace_arm): the first exceptions of any kind but the usual notices (thread names,
        // debug output, breakpoints), first-chance - a C++ exception nobody catches ends the game in abort's fast fail,
        // which no handler sees (PBZ: a game that ended with nothing logged)
        const DWORD code = rec->ExceptionCode;
        if (code != 0x406D1388 && code != 0x40010006 && code != 0x4001000A && code != 0x80000003) {
            if (const int n = trace_exception())
                log("[nr] trace e%d: exception 0x%08lX (first chance) at %s, thread %lu", n, code,
                    where(reinterpret_cast<DWORD64>(rec->ExceptionAddress)).c_str(), GetCurrentThreadId());
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    report_exception("exception", ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI on_unhandled_exception(EXCEPTION_POINTERS* ep) {
    LastErrorGuard guard;
    report_exception("unhandled exception", ep);
    return prev_uef ? prev_uef(ep) : EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

std::string where_address(const void* at) { return where(reinterpret_cast<DWORD64>(at)); }

void install_crash_watch() {
    if (handler) return;
    HMODULE self_mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&install_crash_watch), &self_mod))
        module_bounds(self_mod, g_self_begin, g_self_end);
    HMODULE host_mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(__builtin_return_address(0)), &host_mod) &&
        host_mod != self_mod)
        module_bounds(host_mod, g_host_begin, g_host_end);
    // Do not call LoadLibraryW under loader lock in InitializeASI; inspect already-loaded dbghelp only.
    dbghelp = GetModuleHandleW(L"dbghelp.dll");
    // First in line: a game's or another hook's own handler may end the process before a later one is asked (PBZ:
    // games ended with nothing logged). Filtered to our own modules and stack so benign host SEH is untouched.
    handler = AddVectoredExceptionHandler(1, &on_exception);
    prev_uef = SetUnhandledExceptionFilter(&on_unhandled_exception);
}

void remove_crash_watch() {
    if (handler) RemoveVectoredExceptionHandler(handler);
    handler = nullptr;
    auto cur = SetUnhandledExceptionFilter(prev_uef);
    if (cur != &on_unhandled_exception) SetUnhandledExceptionFilter(cur);
    prev_uef = nullptr;
}

}  // namespace nr::pe
