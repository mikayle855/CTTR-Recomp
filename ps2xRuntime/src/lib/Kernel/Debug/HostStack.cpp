// [hoststack] PS2X_HOSTSTACK=<seconds>: after that many seconds, print the native call stack of every runner thread
// (function, file:line from the PDB), one thread at a time. A bring-up aid for host-side hangs, where the guest
// probes only show the last guest pc. Always installed as well: [crashstack], the stack of a thread that dies of an
// unhandled native exception (access violation etc.), printed before Windows Error Reporting freezes the process.
// Windows only; started from PS2Runtime::loadELF.

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#pragma comment(lib, "dbghelp.lib")
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#if defined(_WIN32)
namespace
{
    bool symInit(HANDLE process)
    {
        static const bool s_ok = [process]() {
            SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            return SymInitialize(process, nullptr, TRUE) != FALSE;
        }();
        return s_ok;
    }

    void walk(HANDLE process, HANDLE thread, CONTEXT ctx, const char *tag)
    {
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = ctx.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = ctx.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = ctx.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        for (int i = 0; i < 40; ++i)
        {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx, nullptr, SymFunctionTableAccess64,
                             SymGetModuleBase64, nullptr) || frame.AddrPC.Offset == 0)
                break;
            alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256]{};
            auto *sym = reinterpret_cast<SYMBOL_INFO *>(buf);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            DWORD64 symOff = 0;
            const char *name = SymFromAddr(process, frame.AddrPC.Offset, &symOff, sym) ? sym->Name : "?";
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineOff = 0;
            if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineOff, &line))
                std::fprintf(stderr, "[%s]   %s  %s:%lu\n", tag, name, line.FileName, static_cast<unsigned long>(line.LineNumber));
            else
                std::fprintf(stderr, "[%s]   %s +0x%llx\n", tag, name, static_cast<unsigned long long>(symOff));
        }
    }

    // [crashchain] main() installs the full crash handler first (ps2_win_timer.cpp: stack walk + crashes\ report);
    // this one, installed later at ELF load, used to REPLACE it, so a crash in play wrote no report. Hand over to it.
    LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;

    LONG WINAPI crashFilter(EXCEPTION_POINTERS *ep)
    {
        if (g_prevFilter)
            return g_prevFilter(ep);
        static LONG s_once = 0;
        if (InterlockedExchange(&s_once, 1) == 0)
        {
            HANDLE process = GetCurrentProcess();
            std::fprintf(stderr, "[crashstack] unhandled exception 0x%08lx at %p (thread %lu)\n",
                         static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode), ep->ExceptionRecord->ExceptionAddress,
                         static_cast<unsigned long>(GetCurrentThreadId()));
            if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
                std::fprintf(stderr, "[crashstack] %s address 0x%llx\n", ep->ExceptionRecord->ExceptionInformation[0] ? "write to" : "read of",
                             static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[1]));
            if (symInit(process))
                walk(process, GetCurrentThread(), *ep->ContextRecord, "crashstack");
            std::fflush(stderr);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void dumpThread(HANDLE process, DWORD tid)
    {
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
        if (!thread)
            return;
        if (SuspendThread(thread) == static_cast<DWORD>(-1))
        {
            CloseHandle(thread);
            return;
        }
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(thread, &ctx))
        {
            std::fprintf(stderr, "[hoststack] ---- thread %lu ----\n", static_cast<unsigned long>(tid));
            walk(process, thread, ctx, "hoststack");
        }
        ResumeThread(thread);
        CloseHandle(thread);
    }

    void watchdog(int seconds)
    {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        HANDLE process = GetCurrentProcess();
        if (!symInit(process))
        {
            std::fprintf(stderr, "[hoststack] SymInitialize failed (%lu)\n", GetLastError());
            return;
        }
        const DWORD self = GetCurrentThreadId();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te))
        {
            do
            {
                if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != self)
                    dumpThread(process, te.th32ThreadID);
            } while (Thread32Next(snap, &te));
        }
        if (snap != INVALID_HANDLE_VALUE)
            CloseHandle(snap);
        std::fflush(stderr);
    }
}
#endif

void ps2xStartHostStackWatchdog()
{
#if defined(_WIN32)
    static bool s_installed = false;
    if (!s_installed)
    {
        s_installed = true;
        g_prevFilter = SetUnhandledExceptionFilter(crashFilter);   // [crashstack] [crashchain]
    }
    const char *v = std::getenv("PS2X_HOSTSTACK");
    const int seconds = v && v[0] ? std::atoi(v) : 0;
    if (seconds > 0)
        std::thread(watchdog, seconds).detach();
#endif
}
