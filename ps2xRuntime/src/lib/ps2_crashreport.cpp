// [crashreport] Crash / hang / soft-failure reports a player can hand over.
//
// <exe dir>\crashes\<yyyy-mm-dd_hh-mm-ss>_<reason>\
//     crash.dmp   minidump of every thread (open in Visual Studio or WinDbg with ps2EntryRunner.pdb)
//     info.txt    reason, time, thread, exception, last guest frame
// and <exe dir>\crashes\last.txt names the newest report, so the launcher can attach the log tail and tell the
// player. Reasons: "exception" (the unhandled-exception filter), "terminate"/"abort", "gamethread-exit" (the guest
// main thread returned while the window is still open), "missing-function" (first "No exact recompiled
// function"), "hang" (the watchdog: no guest frame for PS2X_HANGSECS seconds, default 45).
// One report per reason per run; nothing after the window close request. PS2X_CRASHREPORT=0 disables.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

extern std::atomic<uint64_t> g_bt3FrameCount;   // game_overrides.cpp: the guest frame counter
extern std::atomic<uint64_t> g_ps2xGameFrames;  // [cttrmeter] ps2_perfhud.cpp: CTTR's frames (its frame limiter)
extern std::atomic<bool> g_ps2xFocusPaused;     // [focuspause] ps2_settings_overlay.cpp: the game is paused on purpose
// [crashreport] BT3 counts g_bt3FrameCount, CTTR g_ps2xGameFrames (the BT3 one stays 0 there, so the hang watchdog
// never armed and the crash test never fired for CTTR)
static uint64_t guestFrames()
{
    return g_bt3FrameCount.load(std::memory_order_relaxed) + g_ps2xGameFrames.load(std::memory_order_relaxed);
}

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

namespace
{
    std::atomic<bool> g_shutdown{false};
    std::atomic<uint32_t> g_doneMask{0};
    std::mutex g_mtx;

    bool enabled()
    {
        static const bool on = [](){ const char *v = std::getenv("PS2X_CRASHREPORT"); return !(v && v[0] == '0'); }();
        return on;
    }

    std::string exeDir()
    {
        if (const char *e = std::getenv("PS2X_EXEDIR"); e && e[0])
        {
            std::string d(e);
            if (d.back() != '\\' && d.back() != '/') d += '\\';
            return d;
        }
        char p[MAX_PATH] = {0};
        GetModuleFileNameA(nullptr, p, MAX_PATH);
        if (char *s = std::strrchr(p, '\\')) s[1] = 0;
        return p;
    }

    uint32_t reasonBit(const char *r)
    {
        static const char *const k[] = {"exception", "terminate", "abort", "gamethread-exit", "missing-function", "hang"};
        for (uint32_t i = 0; i < sizeof(k) / sizeof(k[0]); ++i) if (std::strcmp(r, k[i]) == 0) return 1u << i;
        return 1u << 31;
    }
}

extern "C" void ps2xCrashReportShutdown() { g_shutdown.store(true, std::memory_order_relaxed); }

extern "C" void ps2xCrashReport(const char *reason, void *exceptionPointers)
{
    if (!enabled() || g_shutdown.load(std::memory_order_relaxed)) return;
    const uint32_t bit = reasonBit(reason);
    if (g_doneMask.fetch_or(bit) & bit) return;   // once per reason per run
    std::lock_guard<std::mutex> lk(g_mtx);

    char stamp[32];
    std::time_t t = std::time(nullptr); std::tm tmv{}; localtime_s(&tmv, &t);
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", &tmv);
    const std::string root = exeDir() + "crashes\\";
    const std::string dir = root + stamp + "_" + reason + "\\";
    CreateDirectoryA(root.c_str(), nullptr);
    CreateDirectoryA(dir.c_str(), nullptr);

    // The dump: all threads, their stacks and the memory they point at -- enough for call stacks and locals,
    // not the whole 2+ GB address space (the PS2 RAM and VRAM mirrors would make it huge).
    bool dumped = false;
    HANDLE f = CreateFileA((dir + "crash.dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = static_cast<EXCEPTION_POINTERS *>(exceptionPointers);
        mei.ClientPointers = FALSE;
        const auto type = MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
                                        MiniDumpWithUnloadedModules | MiniDumpWithHandleData);
        dumped = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, type,
                                   exceptionPointers ? &mei : nullptr, nullptr, nullptr) != FALSE;
        CloseHandle(f);
    }

    if (std::FILE *i = std::fopen((dir + "info.txt").c_str(), "w"))
    {
        std::fprintf(i, "reason: %s\ntime: %s\nthread: %lu\nguest frame: %llu\ndump: %s\n", reason, stamp,
                     GetCurrentThreadId(), (unsigned long long)guestFrames(),
                     dumped ? "crash.dmp" : "FAILED");
        if (auto *ep = static_cast<EXCEPTION_POINTERS *>(exceptionPointers); ep && ep->ExceptionRecord)
        {
            void *addr = ep->ExceptionRecord->ExceptionAddress;
            HMODULE mod = nullptr; char name[MAX_PATH] = "?";
            if (addr && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &mod) && mod)
                GetModuleFileNameA(mod, name, sizeof name);
            const char *base = std::strrchr(name, '\\');
            std::fprintf(i, "exception: 0x%08lx at %p (%s+0x%llx)\n", ep->ExceptionRecord->ExceptionCode, addr, base ? base + 1 : name,
                         mod ? (unsigned long long)((const char *)addr - (const char *)mod) : 0ull);
        }
        std::fprintf(i, "\nThe stack of the crashing thread is in run_tail.log ([wincrash] lines).\n"
                        "The launcher adds run_tail.log (the end of the game log) when the game exits.\n"
                        "Open crash.dmp in Visual Studio / WinDbg with ps2EntryRunner.pdb beside the exe.\n");
        std::fclose(i);
    }
    if (std::FILE *l = std::fopen((root + "last.txt").c_str(), "w")) { std::fprintf(l, "%s\n", dir.c_str()); std::fclose(l); }
    std::fprintf(stderr, "[crashreport] %s: report written to %s (dump %s)\n", reason, dir.c_str(), dumped ? "ok" : "FAILED");
    std::fflush(stderr);
}

extern "C" void ps2xCrashWatchdogStart()
{   // [crashreport] hang watchdog: the guest frame counter stops while the window is still open
    if (!enabled()) return;
    static bool started = false;
    if (started) return;
    started = true;
    const int secs = [](){ const char *v = std::getenv("PS2X_HANGSECS"); const int s = v && v[0] ? std::atoi(v) : 45; return s > 5 ? s : 45; }();
    // PS2X_CRASHTEST=exception|hang: exercise the report path once the game has run 600 frames (bt4test checks it):
    // "exception" faults on this thread, "hang" makes the watchdog see a frozen frame counter.
    const std::string test = [](){ const char *v = std::getenv("PS2X_CRASHTEST"); return std::string(v ? v : ""); }();
    std::thread([secs, test]() {
        uint64_t last = guestFrames();
        auto since = std::chrono::steady_clock::now();
        bool frozen = false;
        while (!g_shutdown.load(std::memory_order_relaxed))
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            uint64_t now = guestFrames();
            if (g_ps2xFocusPaused.load(std::memory_order_relaxed))
            {   // [focuspause] paused on purpose (window in the background): not a hang
                last = now;
                since = std::chrono::steady_clock::now();
                continue;
            }
            if (!test.empty() && now >= 600)
            {
                if (test == "exception") { std::fprintf(stderr, "[crashreport] PS2X_CRASHTEST=exception\n"); std::fflush(stderr); *(volatile int *)nullptr = 0; }
                if (test == "hang") { if (!frozen) { std::fprintf(stderr, "[crashreport] PS2X_CRASHTEST=hang\n"); frozen = true; } now = last; }
            }
            if (now != last || now == 0) { last = now; since = std::chrono::steady_clock::now(); continue; }   // 0: still booting
            if (std::chrono::steady_clock::now() - since >= std::chrono::seconds(secs))
            {
                std::fprintf(stderr, "[crashreport] no guest frame for %d s (frame %llu): hang\n", secs, (unsigned long long)now);
                ps2xCrashReport("hang", nullptr);
                return;
            }
        }
    }).detach();
}
#else
extern "C" void ps2xCrashReportShutdown() {}
extern "C" void ps2xCrashReport(const char *, void *) {}
extern "C" void ps2xCrashWatchdogStart() {}
#endif
