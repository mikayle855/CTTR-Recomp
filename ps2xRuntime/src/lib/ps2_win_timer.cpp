// [wintimer] Windows only: ask for a 1 ms scheduler tick for the life of the process.
//
// The runtime paces frames and polls for barrier work with short timed waits (a few hundred
// microseconds to a few milliseconds). On Windows every such wait rounds up to the timer tick,
// 15.6 ms by default, so the GL thread presented 3-30 frames a second in fights where Linux
// presents 50-60. timeBeginPeriod(1) is what every game/emulator does; it is process-wide
// and undone at exit. Lives in its own file because <windows.h> collides with raylib's names.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

extern "C" void ps2xWinTimerBegin() { timeBeginPeriod(1); }
extern "C" void ps2xWinTimerEnd() { timeEndPeriod(1); }
#endif

// [wincrash] Windows dies silently on an access violation; print what we can before it does.
#if defined(_WIN32)
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <csignal>
#include <exception>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
// [wincrash] Which thread died? The stack usually says, but the one-line summary is what gets
// pasted into a bug report. Recorded at install time (called from main), so this needs no
// registration at any other thread's entry point -- deliberately, to keep this Windows-only
// file self-contained. File scope, NOT inside an extern "C" function: clang-cl rejects a C++
// global declared there and the Linux build cannot catch that class of error.
static DWORD g_ps2xMainThreadId = 0;
extern "C" void ps2xCrashReport(const char *reason, void *exceptionPointers);   // [crashreport] ps2_crashreport.cpp
extern "C" void ps2xCrashWatchdogStart();
static const char *ps2xWinThreadRole()
{
    if (!g_ps2xMainThreadId) return "unknown (handler installed off-main?)";
    return (GetCurrentThreadId() == g_ps2xMainThreadId) ? "MAIN (GL / present / guest under PS2X_SCHED)"
                                                        : "NOT main -- worker (async kick, sound, or a host thread)";
}
static LONG WINAPI ps2xWinCrashFilter(EXCEPTION_POINTERS *ep)
{
    const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    void *addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    HMODULE mod = nullptr; char name[MAX_PATH] = "?";
    if (addr && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &mod) && mod)
        GetModuleFileNameA(mod, name, sizeof name);
    const unsigned long long off = (mod && addr) ? (unsigned long long)((const char *)addr - (const char *)mod) : 0ull;
    void *fault = (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) ? (void *)ep->ExceptionRecord->ExceptionInformation[1] : nullptr;
    std::fprintf(stderr, "[wincrash] exception 0x%08lx at %p (%s+0x%llx) thread %lu%s%p\n", (unsigned long)code, addr, name, off, GetCurrentThreadId(),
                 fault ? " fault address " : "", fault);
    std::fprintf(stderr, "[wincrash] thread role: %s\n", ps2xWinThreadRole());

    // [wincrashstack] One faulting address is not diagnosable: two user crash reports (2026-09-07)
    // gave only "+0x49cf5ee" and "+0x49cd16e" in different builds, which resolve to nothing without
    // the matching PDB. Walk the stack so every report carries module+RVA per frame -- usable even
    // with no symbols, because the reporter's own build resolves it -- and symbol+line when a PDB
    // sits beside the exe. Everything here is best-effort: a crash handler that itself faults tells
    // us nothing, so every call is checked and failure just prints less.
    if (ep && ep->ContextRecord)
    {
        HANDLE proc = GetCurrentProcess(), thr = GetCurrentThread();
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
        const BOOL haveSym = SymInitialize(proc, nullptr, TRUE);
        CONTEXT ctx = *ep->ContextRecord;   // StackWalk64 MUTATES the context -- never pass the original
        STACKFRAME64 fr{};
        DWORD machine;
#if defined(_M_X64) || defined(__x86_64__)
        machine = IMAGE_FILE_MACHINE_AMD64;
        fr.AddrPC.Offset = ctx.Rip; fr.AddrFrame.Offset = ctx.Rbp; fr.AddrStack.Offset = ctx.Rsp;
#elif defined(_M_ARM64) || defined(__aarch64__)
        machine = IMAGE_FILE_MACHINE_ARM64;
        fr.AddrPC.Offset = ctx.Pc; fr.AddrFrame.Offset = ctx.Fp; fr.AddrStack.Offset = ctx.Sp;
#else
        machine = IMAGE_FILE_MACHINE_I386;
        fr.AddrPC.Offset = ctx.Eip; fr.AddrFrame.Offset = ctx.Ebp; fr.AddrStack.Offset = ctx.Esp;
#endif
        fr.AddrPC.Mode = fr.AddrFrame.Mode = fr.AddrStack.Mode = AddrModeFlat;
        alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 512] = {};
        SYMBOL_INFO *sym = (SYMBOL_INFO *)symBuf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 511;
        for (int depth = 0; depth < 48; ++depth)
        {
            if (!StackWalk64(machine, proc, thr, &fr, &ctx, nullptr,
                             SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
            const DWORD64 pc = fr.AddrPC.Offset;
            if (!pc) break;
            HMODULE fm = nullptr; char fn[MAX_PATH] = "?";
            unsigned long long frva = 0ull;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)pc, &fm) && fm)
            {
                GetModuleFileNameA(fm, fn, sizeof fn);
                frva = (unsigned long long)(pc - (DWORD64)(uintptr_t)fm);
                const char *slash = std::strrchr(fn, '\\');
                if (slash) std::memmove(fn, slash + 1, std::strlen(slash + 1) + 1);
            }
            char where[600] = "";
            DWORD64 disp = 0;
            if (haveSym && SymFromAddr(proc, pc, &disp, sym))
            {
                IMAGEHLP_LINE64 line{}; line.SizeOfStruct = sizeof line; DWORD ldisp = 0;
                if (SymGetLineFromAddr64(proc, pc, &ldisp, &line) && line.FileName)
                    std::snprintf(where, sizeof where, "  %s + 0x%llx  (%s:%lu)", sym->Name,
                                  (unsigned long long)disp, line.FileName, (unsigned long)line.LineNumber);
                else
                    std::snprintf(where, sizeof where, "  %s + 0x%llx", sym->Name, (unsigned long long)disp);
            }
            std::fprintf(stderr, "[wincrash]  #%02d %s+0x%llx%s\n", depth, fn, frva, where);
        }
        if (!haveSym)
            std::fprintf(stderr, "[wincrash] (no symbols loaded -- put the build's .pdb beside the .exe for names and line numbers)\n");
    }
    std::fflush(stderr); std::fflush(stdout);
    ps2xCrashReport("exception", ep);   // [crashreport] after the log lines: they are the first thing anyone reads
    return EXCEPTION_CONTINUE_SEARCH;
}
static void ps2xWinTerminate()
{   // [wincrash] std::terminate (an uncaught C++ exception) never reaches the SEH filter: a run that died with no
    // [wincrash] line at all (new4.txt, 2026-09-08) took this path or abort()
    std::fprintf(stderr, "[wincrash] std::terminate on thread %lu (%s): uncaught C++ exception or abort\n", GetCurrentThreadId(), ps2xWinThreadRole());
    std::fflush(stderr); std::fflush(stdout);
    ps2xCrashReport("terminate", nullptr);
    std::abort();
}
static void ps2xWinAbortSignal(int)
{
    std::fprintf(stderr, "[wincrash] SIGABRT on thread %lu (%s)\n", GetCurrentThreadId(), ps2xWinThreadRole());
    std::fflush(stderr); std::fflush(stdout);
    ps2xCrashReport("abort", nullptr);
}
extern "C" void ps2xWinCrashHandlerInstall()
{
    g_ps2xMainThreadId = GetCurrentThreadId();
    SetUnhandledExceptionFilter(ps2xWinCrashFilter);
    std::set_terminate(ps2xWinTerminate);
    std::signal(SIGABRT, ps2xWinAbortSignal);
    ps2xCrashWatchdogStart();   // [crashreport] hang watchdog
}
#endif

// [wincpu] per-thread CPU time (the Linux build uses CLOCK_THREAD_CPUTIME_ID) and a one-line host description.
#if defined(_WIN32)
extern "C" unsigned long long ps2xWinThreadCpuNs()
{
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0ull;
    const unsigned long long kt = ((unsigned long long)k.dwHighDateTime << 32) | k.dwLowDateTime;
    const unsigned long long ut = ((unsigned long long)u.dwHighDateTime << 32) | u.dwLowDateTime;
    return (kt + ut) * 100ull;   // 100 ns units -> ns
}
extern "C" void ps2xWinHostInfo()
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    char cpu[128] = "?"; DWORD len = sizeof cpu; DWORD mhz = 0; DWORD mlen = sizeof mhz; HKEY k;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &k) == ERROR_SUCCESS)
    {
        RegQueryValueExA(k, "ProcessorNameString", nullptr, nullptr, (LPBYTE)cpu, &len);
        RegQueryValueExA(k, "~MHz", nullptr, nullptr, (LPBYTE)&mhz, &mlen);
        RegCloseKey(k);
    }
    std::fprintf(stderr, "[host] Windows, %lu logical cpus, %s (~%lu MHz)\n", (unsigned long)si.dwNumberOfProcessors, cpu, (unsigned long)mhz);
}
// [threadtune] PS2X_THREADPRIO=1: the three pipeline threads (GameThread -> KickWorker -> GsThread) and the present loop
// run ABOVE_NORMAL with distinct ideal processors on different physical cores (a hint, not a pin), so background work
// and each other's hyperthread siblings preempt them less. 0 (default) leaves Windows' defaults.
extern "C" void ps2xThreadTune(const char *role)
{
    static const int s_mode = [](){ const char *v = std::getenv("PS2X_THREADPRIO"); return v && v[0] ? std::atoi(v) : 0; }();
    if (s_mode <= 0 || !role) return;
    SYSTEM_INFO si; GetSystemInfo(&si);
    const DWORD n = si.dwNumberOfProcessors;
    DWORD ideal = MAXIMUM_PROCESSORS;
    if (!std::strcmp(role, "GameThread")) ideal = 2;
    else if (!std::strcmp(role, "KickWorker")) ideal = 4;
    else if (!std::strcmp(role, "GsThread")) ideal = 6;
    else if (!std::strcmp(role, "Present")) ideal = 8;
    if (ideal != MAXIMUM_PROCESSORS && ideal < n) SetThreadIdealProcessor(GetCurrentThread(), ideal);   // even = one per physical core (SMT pairs)
    const BOOL ok = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    std::fprintf(stderr, "[threadtune] %s: priority above-normal %s, ideal cpu %lu\n", role, ok ? "ok" : "FAILED", (unsigned long)(ideal < n ? ideal : 0));
}
#pragma comment(lib, "advapi32.lib")
#endif
#if !defined(_WIN32)
extern "C" void ps2xThreadTune(const char *) {}
#endif

// [screenshot] the user's Pictures folder (wherever Windows keeps it, OneDrive included); false = unknown
#if defined(_WIN32)
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
extern "C" bool ps2xPicturesDir(char *out, unsigned n)
{
    char buf[MAX_PATH] = {0};
    if (SHGetFolderPathA(nullptr, CSIDL_MYPICTURES | CSIDL_FLAG_CREATE, nullptr, SHGFP_TYPE_CURRENT, buf) != S_OK || !buf[0]) return false;
    std::snprintf(out, n, "%s", buf);
    return true;
}
#else
extern "C" bool ps2xPicturesDir(char *, unsigned) { return false; }
#endif

// [allocprof] PS2X_ALLOCPROF=1: who calls operator new (main.cpp hands every call here), by return address. Every 10 s:
// allocations per second and the 20 busiest call sites (module+offset, symbol, line). Lock-free open-addressing table.
#if defined(_WIN32)
#include <atomic>
#include <thread>
#include <vector>
#include <algorithm>
namespace
{
    constexpr int kApDepth = 6;   // call-chain frames kept per site (operator new's callers)
    struct ApSlot { std::atomic<uintptr_t> ra{0}; std::atomic<uint64_t> n{0}; std::atomic<uint64_t> bytes{0}; void *chain[kApDepth] = {}; };
    ApSlot g_apSlots[8192];
    std::atomic<uint64_t> g_apTotal{0}, g_apLost{0};
}
extern "C" void ps2xAllocProfRecord(void *retAddr, size_t bytes)
{
    g_apTotal.fetch_add(1, std::memory_order_relaxed);
    (void)retAddr;
    void *fr[kApDepth + 2] = {};
    const USHORT got = RtlCaptureStackBackTrace(2, kApDepth, fr, nullptr);   // skip this function and ps2xNew
    uintptr_t ra = 0;
    for (USHORT i = 0; i < got; ++i) ra = ra * 0x100000001B3ull ^ reinterpret_cast<uintptr_t>(fr[i]);
    if (!ra) ra = 1;
    size_t h = (ra >> 2) * 0x9E3779B97F4A7C15ull >> 51;   // 13 bits
    for (int probe = 0; probe < 32; ++probe, h = (h + 1) & 8191u)
    {
        ApSlot &s = g_apSlots[h];
        uintptr_t cur = s.ra.load(std::memory_order_relaxed);
        if (cur == 0 && s.ra.compare_exchange_strong(cur, ra)) { for (int i = 0; i < kApDepth; ++i) s.chain[i] = fr[i]; cur = ra; }
        if (cur == ra)
        {
            s.n.fetch_add(1, std::memory_order_relaxed);
            s.bytes.fetch_add(bytes, std::memory_order_relaxed);
            return;
        }
    }
    g_apLost.fetch_add(1, std::memory_order_relaxed);
}
extern "C" void ps2xAllocProfStart()
{
    std::thread([]() {
        HANDLE proc = GetCurrentProcess();
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
        const BOOL haveSym = SymInitialize(proc, nullptr, TRUE);
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            struct Row { uintptr_t ra; uint64_t n, bytes; void *const *chain; };
            std::vector<Row> rows;
            for (ApSlot &s : g_apSlots)
                if (uintptr_t ra = s.ra.load(std::memory_order_relaxed))
                {
                    const uint64_t n = s.n.exchange(0, std::memory_order_relaxed), b = s.bytes.exchange(0, std::memory_order_relaxed);
                    if (n) rows.push_back({ra, n, b, s.chain});
                }
            std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.n > b.n; });
            std::fprintf(stderr, "[allocprof] 10 s: %.0f operator new/s (table full: %llu)\n", g_apTotal.exchange(0) / 10.0,
                         (unsigned long long)g_apLost.exchange(0));
            alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 512] = {};
            SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO *>(symBuf);
            for (size_t i = 0; i < rows.size() && i < 12; ++i)
            {
                std::fprintf(stderr, "[allocprof]   %8.0f/s %8.0f B/s\n", rows[i].n / 10.0, rows[i].bytes / 10.0);
                for (int k = 0; k < kApDepth && rows[i].chain[k]; ++k)
                {
                    const DWORD64 a = reinterpret_cast<DWORD64>(rows[i].chain[k]);
                    char where[400] = "?";
                    sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 511;
                    DWORD64 disp = 0;
                    if (haveSym && SymFromAddr(proc, a, &disp, sym))
                    {
                        IMAGEHLP_LINE64 line{}; line.SizeOfStruct = sizeof line; DWORD ld = 0;
                        const char *nm = sym->Name; const size_t L = std::strlen(nm);
                        const char *shortNm = L > 90 ? nm + L - 90 : nm;
                        if (SymGetLineFromAddr64(proc, a, &ld, &line) && line.FileName)
                        {
                            const char *f = std::strrchr(line.FileName, '\\');
                            std::snprintf(where, sizeof where, "%s (%s:%lu)", shortNm, f ? f + 1 : line.FileName, (unsigned long)line.LineNumber);
                        }
                        else std::snprintf(where, sizeof where, "%s", shortNm);
                    }
                    std::fprintf(stderr, "[allocprof]       %s\n", where);
                }
            }
            std::fflush(stderr);
        }
    }).detach();
}
#else
extern "C" void ps2xAllocProfRecord(void *, size_t) {}
extern "C" void ps2xAllocProfStart() {}
#endif
