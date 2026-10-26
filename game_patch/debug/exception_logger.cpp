#include <windows.h>
#include <dbghelp.h>
#include <cstdint>
#include <format>
#include <string>
#include <xlog/xlog.h>
#include "debug.h"

// Logs first-chance C++ exceptions and access violations before any SEH frame can
// swallow them.
//
// Red Faction wraps large parts of the frame loop in its own __try/__except. When an
// exception is raised in there the process dies (or silently continues) with nothing in
// AlpineFaction.log: the log simply stops mid-frame. crash_handler_stub uses
// SetUnhandledExceptionFilter, which by definition only runs for exceptions nobody
// handled, so it never sees these either.
//
// A vectored exception handler runs ahead of every SEH frame, so it observes the throw
// before RF gets a chance to eat it. This one only logs and always returns
// EXCEPTION_CONTINUE_SEARCH, so exception handling behaviour is unchanged.
//
// These are *first-chance* exceptions: most are caught and handled a moment later, and
// seeing them in the log is normal. They are logged at warn level and capped, so a hot
// loop that throws cannot flood the file.

static int g_num_logged = 0;
constexpr int max_logged = 40;

static void copy_str(char* dst, int dst_size, const char* src)
{
    int i = 0;
    if (!src) {
        dst[0] = 0;
        return;
    }
    while (i < dst_size - 1 && src[i]) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

// Reading the type name out of a C++ throw is MSVC-specific in two ways, so the whole
// probe is compiled only for MSVC:
//
//   - The 0xE06D7363 exception code and the ThrowInfo structures below are the MSVC C++
//     ABI. A MinGW build uses a different mechanism entirely, so this parsing would be
//     reading garbage.
//   - Walking those structures means chasing pointers in a process that is already in
//     trouble, so it needs SEH protection. __try/__except is MSVC syntax; GCC rejects it.
//
// Everything else in this file — access violations, the stack walk, symbol resolution —
// is plain Win32 and works the same either way. On MinGW a C++ throw is still logged,
// just without the type name.
#ifdef _MSC_VER

// Layout of the MSVC throw metadata (x86: all pointers are absolute).
struct MsvcTypeDescriptor
{
    const void* vftable;
    void* spare;
    char name[1];
};
struct MsvcCatchableType
{
    unsigned properties;
    MsvcTypeDescriptor* type_desc;
};
struct MsvcCatchableTypeArray
{
    int count;
    MsvcCatchableType* types[1];
};
struct MsvcThrowInfo
{
    unsigned attributes;
    void* unwind;
    void* forward_compat;
    MsvcCatchableTypeArray* catchable;
};

// True for mangled names ending in "@std@@", i.e. types inside namespace std.
// Only those are safe to poke for what() through the vtable.
static bool name_is_std(const char* name)
{
    int len = 0;
    while (name[len] && len < 240) {
        ++len;
    }
    return len >= 6 && name[len - 6] == '@' && name[len - 5] == 's' && name[len - 4] == 't'
        && name[len - 3] == 'd' && name[len - 2] == '@' && name[len - 1] == '@';
}

// Must stay free of objects with destructors, otherwise MSVC rejects __try (C2712).
static void probe_throw(const EXCEPTION_RECORD* rec, char* type_out, int type_size,
                           char* what_out, int what_size)
{
    copy_str(type_out, type_size, "?");
    copy_str(what_out, what_size, "");
    if (rec->NumberParameters < 3) {
        return;
    }
    __try {
        const MsvcThrowInfo* ti = reinterpret_cast<const MsvcThrowInfo*>(rec->ExceptionInformation[2]);
        if (!ti || !ti->catchable || ti->catchable->count <= 0) {
            return;
        }
        const char* name = ti->catchable->types[0]->type_desc->name;
        copy_str(type_out, type_size, name);
        if (name_is_std(name)) {
            void* obj = reinterpret_cast<void*>(rec->ExceptionInformation[1]);
            if (obj) {
                // std::exception vtable: [0] destructor, [1] what()
                typedef const char*(__thiscall* WhatFn)(void*);
                WhatFn fn = reinterpret_cast<WhatFn>((*reinterpret_cast<void***>(obj))[1]);
                copy_str(what_out, what_size, fn(obj));
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        copy_str(what_out, what_size, "<probe faulted>");
    }
}

#else  // !_MSC_VER

static void probe_throw(const EXCEPTION_RECORD*, char* type_out, int type_size,
                        char* what_out, int what_size)
{
    copy_str(type_out, type_size, "<needs MSVC>");
    copy_str(what_out, what_size, "");
}

#endif  // _MSC_VER

// dbghelp is loaded dynamically so the build system needs no extra link library.
typedef DWORD(WINAPI* SymSetOptionsFn)(DWORD);
typedef BOOL(WINAPI* SymInitializeFn)(HANDLE, PCSTR, BOOL);
typedef BOOL(WINAPI* SymFromAddrFn)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
typedef BOOL(WINAPI* SymGetLineFn)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);
typedef BOOL(WINAPI* StackWalkFn)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID,
    PREAD_PROCESS_MEMORY_ROUTINE64, PFUNCTION_TABLE_ACCESS_ROUTINE64,
    PGET_MODULE_BASE_ROUTINE64, PTRANSLATE_ADDRESS_ROUTINE64);

static SymFromAddrFn g_sym_from_addr = nullptr;
static SymGetLineFn g_sym_get_line = nullptr;
static StackWalkFn g_stack_walk = nullptr;
static PFUNCTION_TABLE_ACCESS_ROUTINE64 g_table_access = nullptr;
static PGET_MODULE_BASE_ROUTINE64 g_module_base = nullptr;

static void sym_init()
{
    static bool tried = false;
    if (tried) {
        return;
    }
    tried = true;
    HMODULE lib = LoadLibraryA("dbghelp.dll");
    if (!lib) {
        return;
    }
    auto set_options = reinterpret_cast<SymSetOptionsFn>(GetProcAddress(lib, "SymSetOptions"));
    auto initialize = reinterpret_cast<SymInitializeFn>(GetProcAddress(lib, "SymInitialize"));
    g_sym_from_addr = reinterpret_cast<SymFromAddrFn>(GetProcAddress(lib, "SymFromAddr"));
    g_sym_get_line = reinterpret_cast<SymGetLineFn>(GetProcAddress(lib, "SymGetLineFromAddr64"));
    g_stack_walk = reinterpret_cast<StackWalkFn>(GetProcAddress(lib, "StackWalk64"));
    g_table_access = reinterpret_cast<PFUNCTION_TABLE_ACCESS_ROUTINE64>(
        GetProcAddress(lib, "SymFunctionTableAccess64"));
    g_module_base = reinterpret_cast<PGET_MODULE_BASE_ROUTINE64>(
        GetProcAddress(lib, "SymGetModuleBase64"));
    if (set_options) {
        set_options(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    }
    if (initialize) {
        // nullptr search path + invade: picks up the .pdb sitting next to each module
        initialize(GetCurrentProcess(), nullptr, TRUE);
    }
    xlog::info("[exception-logger] dbghelp: sym={} line={} walk={} tbl={} base={}",
        g_sym_from_addr != nullptr, g_sym_get_line != nullptr, g_stack_walk != nullptr,
        g_table_access != nullptr, g_module_base != nullptr);
}

// Resolve to "func+off [file:line]" when a PDB is available, else "module+RVA".
static std::string addr_str(void* addr)
{
    sym_init();
    if (g_sym_from_addr) {
        char storage[sizeof(SYMBOL_INFO) + 256] = {};
        SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(storage);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 sym_off = 0;
        const DWORD64 addr64 = static_cast<DWORD64>(reinterpret_cast<uintptr_t>(addr));
        if (g_sym_from_addr(GetCurrentProcess(), addr64, &sym_off, sym)) {
            std::string out = std::format("{}+{:#x}", sym->Name, static_cast<uintptr_t>(sym_off));
            if (g_sym_get_line) {
                IMAGEHLP_LINE64 line = {};
                line.SizeOfStruct = sizeof(line);
                DWORD line_off = 0;
                if (g_sym_get_line(GetCurrentProcess(), addr64, &line_off, &line)) {
                    out += std::format(" [{}:{}]", line.FileName, line.LineNumber);
                }
            }
            return out;
        }
    }
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &mod)
        && mod) {
        char path[MAX_PATH] = "";
        GetModuleFileNameA(mod, path, MAX_PATH);
        const char* base = path;
        for (const char* p = path; *p; ++p) {
            if (*p == '\\') {
                base = p + 1;
            }
        }
        return std::format("{}+{:#x}", base,
            reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod));
    }
    return std::format("{:#x}", reinterpret_cast<uintptr_t>(addr));
}

static LONG CALLBACK exception_logger(EXCEPTION_POINTERS* ep)
{
    const EXCEPTION_RECORD* rec = ep->ExceptionRecord;
    const DWORD code = rec->ExceptionCode;
    const bool is_cpp = code == 0xE06D7363u;
    const bool is_av = code == 0xC0000005u;
    if ((!is_cpp && !is_av) || g_num_logged >= max_logged) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    ++g_num_logged;
    // Load dbghelp now rather than lazily from addr_str(): the C++ throw branch below
    // reaches the stack walk without ever calling addr_str(), and StackWalk64 would be
    // skipped on the very first exception.
    sym_init();

    if (is_cpp) {
        char type_name[192];
        char what_msg[256];
        probe_throw(rec, type_name, sizeof(type_name), what_msg, sizeof(what_msg));
        xlog::warn("[first-chance {}] C++ throw type={} what={}", g_num_logged, type_name, what_msg);
    }
    else {
        const char* op = "read";
        if (rec->NumberParameters >= 1) {
            if (rec->ExceptionInformation[0] == 1) {
                op = "write";
            }
            else if (rec->ExceptionInformation[0] == 8) {
                op = "execute";
            }
        }
        const uintptr_t bad = rec->NumberParameters >= 2 ? rec->ExceptionInformation[1] : 0;
        xlog::warn("[first-chance {}] access violation {} at {:#x}, pc={}", g_num_logged, op, bad,
            addr_str(rec->ExceptionAddress));
    }

    // Release x86 omits frame pointers, so a naive EBP walk stops after a few frames.
    // StackWalk64 uses the FPO/unwind data from the PDB and gets the whole chain.
    std::string trace;
    if (g_stack_walk && g_table_access && g_module_base) {
        CONTEXT ctx = *ep->ContextRecord;  // StackWalk64 modifies it, so work on a copy
        STACKFRAME64 sf = {};
        sf.AddrPC.Offset = ctx.Eip;
        sf.AddrPC.Mode = AddrModeFlat;
        sf.AddrFrame.Offset = ctx.Ebp;
        sf.AddrFrame.Mode = AddrModeFlat;
        sf.AddrStack.Offset = ctx.Esp;
        sf.AddrStack.Mode = AddrModeFlat;
        for (int i = 0; i < 28; ++i) {
            if (!g_stack_walk(IMAGE_FILE_MACHINE_I386, GetCurrentProcess(), GetCurrentThread(),
                    &sf, &ctx, nullptr, g_table_access, g_module_base, nullptr)) {
                break;
            }
            if (sf.AddrPC.Offset == 0) {
                break;
            }
            trace += "\n    ";
            trace += addr_str(reinterpret_cast<void*>(static_cast<uintptr_t>(sf.AddrPC.Offset)));
        }
    }
    else {
        void* frames[32] = {};
        USHORT n = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
        for (USHORT i = 0; i < n; ++i) {
            trace += "\n    ";
            trace += addr_str(frames[i]);
        }
    }
    xlog::warn("[first-chance {}]   stack:{}", g_num_logged, trace);

    return EXCEPTION_CONTINUE_SEARCH;
}

void exception_logger_init()
{
    AddVectoredExceptionHandler(1, exception_logger);
}
