// [eeprof] PS2X_EEPROF=<interval ms>: a sampling profiler for the guest threads that needs no external tool,
// so a Windows user can answer "what is the game thread doing" from a log. A sampler thread reads the
// instruction pointer of every registered guest thread every <interval> ms (Windows: SuspendThread +
// GetThreadContext; Linux: a per-thread CPU-time timer delivering SIGPROF whose handler records RIP) and
// attributes it: recompiled EE/overlay functions by guest address through the dense function tables,
// everything else by module + 4 KB offset bucket. Every 10 s it prints, per thread, the sample count, the
// EE share and the top entries. Off unless the variable is set; costs nothing when off.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
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
#elif defined(__linux__) && defined(__x86_64__)
#include <dlfcn.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#include "ps2_waitprof.h"
std::atomic<uint64_t> g_ps2xWaitNs[WP_COUNT];
std::atomic<uint64_t> g_ps2xWaitN[WP_COUNT];
#if (defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))) || (defined(__linux__) && defined(__x86_64__))
bool g_ps2xWaitProfOn = [](){ const char *e = std::getenv("PS2X_EEPROF"); return e && e[0] && e[0] != '0'; }();
#else
bool g_ps2xWaitProfOn = false;
#endif

#if (defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))) || (defined(__linux__) && defined(__x86_64__))
extern "C" void ps2xEeProfCollectTable(void (*cb)(uintptr_t fnptr, uint32_t guestAddr, int which, void *user), void *user);

namespace
{
    constexpr size_t kRing = 1u << 16;

    struct Target
    {
        std::string name;
        std::vector<std::array<uintptr_t, 16>> stacks;   // [eeprofstack] full stacks of this window's samples
        std::atomic<uint32_t> head{0};
        uint32_t tail = 0;
        uintptr_t *ring = nullptr;
#if defined(_WIN32)
        HANDLE handle = nullptr;
#else
        timer_t timer{};
        bool timerOn = false;
#endif
    };

    std::mutex g_mx;
    std::vector<Target *> g_targets;
    std::atomic<bool> g_started{false};
    int g_intervalMs = 1;

    int intervalFromEnv()
    {
        static const int v = [](){ const char *e = std::getenv("PS2X_EEPROF"); if (!e || !e[0] || e[0] == '0') return 0; const int n = std::atoi(e); return n > 0 ? n : 1; }();
        return v;
    }

    // [eeprofstack] PS2X_EEPROF_STACK=<text>: for samples whose function name contains <text>, print the most common
    // call chains (Windows x64). E.g. PS2X_EEPROF_STACK=WaitForSynchronization names who waits on GPU fences.
    const char *stackFilter()
    {
        static const char *v = [](){ const char *e = std::getenv("PS2X_EEPROF_STACK"); return (e && e[0]) ? e : nullptr; }();
        return v;
    }

    // ---- attribution ---------------------------------------------------------------------------
    struct FnEntry { uintptr_t ptr; uint32_t guest; int which; };
    std::vector<FnEntry> g_fns;   // sorted by ptr
    bool g_fnsBuilt = false;

    void collectCb(uintptr_t p, uint32_t g, int which, void *user)
    {
        auto *v = static_cast<std::vector<FnEntry> *>(user);
        v->push_back({p, g, which});
    }

    void buildTable()
    {
        std::vector<FnEntry> v;
        ps2xEeProfCollectTable(collectCb, &v);
        std::sort(v.begin(), v.end(), [](const FnEntry &a, const FnEntry &b){ return a.ptr != b.ptr ? a.ptr < b.ptr : a.guest < b.guest; });
        // one entry per distinct pointer (the lowest guest address that maps to it)
        std::vector<FnEntry> u;
        for (const FnEntry &e : v) if (u.empty() || u.back().ptr != e.ptr) u.push_back(e);
        g_fns.swap(u);
        g_fnsBuilt = true;
    }

    // Name for a sampled instruction pointer: "ee:0x<guest>" / "ovl:0x<guest>" when it falls inside a
    // recompiled function (the nearest registered entry below it, with the next entry above it), else
    // "rt:<module>+0x<offset rounded to 4 KB>".
    std::string nameFor(uintptr_t rip)
    {
        // [eeprofsym] PS2X_EEPROF_SYM=1: name every sample from the .pdb instead (the nearest-entry rule charges
        // unregistered code laid out after a tiny stub to that stub)
        static const bool s_symOnly = [](){ const char *v = std::getenv("PS2X_EEPROF_SYM"); return v && v[0] == '1'; }();
        if (!s_symOnly && !g_fns.empty())
        {
            auto it = std::upper_bound(g_fns.begin(), g_fns.end(), rip, [](uintptr_t r, const FnEntry &e){ return r < e.ptr; });
            if (it != g_fns.begin())
            {
                const FnEntry &e = *(it - 1);
                // a registered function extends to the next registered pointer, capped at 256 KB (the largest
                // generated functions), and the last entry of a table is followed by unrelated runtime code:
                // cap that one at 64 KB so runtime samples are not charged to it.
                const bool last = (it == g_fns.end()) || (it->which != e.which);
                const uintptr_t next = last ? (e.ptr + (64u << 10)) : std::min<uintptr_t>(it->ptr, e.ptr + (256u << 10));
                if (rip >= e.ptr && rip < next)
                {
                    char buf[40]; std::snprintf(buf, sizeof(buf), "%s:0x%06x", e.which ? "ovl" : "ee", e.guest);
                    return buf;
                }
            }
        }
        char buf[96];
#if defined(_WIN32)
        HMODULE mod = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)rip, &mod) && mod)
        {
            char path[MAX_PATH] = {0}; GetModuleFileNameA(mod, path, sizeof(path));
            const char *base = std::strrchr(path, '\\'); base = base ? base + 1 : path;
            {   // [eeprofsym] name the function: our own code from the .pdb next to the exe (PS2X_DEBUG_SYMBOLS
                // builds), system DLLs from their exports. Falls back to the 4 KB page when there is no symbol.
                // Only the sampler thread calls nameFor; dbghelp is single-threaded, so it stays here.
                static const bool s_sym = [](){
                    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
                    return SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE; }();
                if (s_sym)
                {
                    alignas(SYMBOL_INFO) char sbuf[sizeof(SYMBOL_INFO) + 256];
                    SYMBOL_INFO *si = reinterpret_cast<SYMBOL_INFO *>(sbuf);
                    si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
                    DWORD64 disp = 0;
                    if (SymFromAddr(GetCurrentProcess(), (DWORD64)rip, &disp, si) && si->Name[0])
                    {
                        std::snprintf(buf, sizeof(buf), "rt:%s", si->Name);
                        return buf;
                    }
                }
            }
            std::snprintf(buf, sizeof(buf), "rt:%s+0x%llx", base, (unsigned long long)((rip - (uintptr_t)mod) & ~(uintptr_t)0xFFF));
            return buf;
        }
#else
        Dl_info di{};
        if (dladdr((void *)rip, &di) && di.dli_fname)
        {
            const char *base = std::strrchr(di.dli_fname, '/'); base = base ? base + 1 : di.dli_fname;
            if (di.dli_sname) { std::snprintf(buf, sizeof(buf), "rt:%s", di.dli_sname); return buf; }
            std::snprintf(buf, sizeof(buf), "rt:%s+0x%llx", base, (unsigned long long)((rip - (uintptr_t)di.dli_fbase) & ~(uintptr_t)0xFFF));
            return buf;
        }
#endif
        std::snprintf(buf, sizeof(buf), "rt:?+0x%llx", (unsigned long long)(rip & ~(uintptr_t)0xFFF));
        return buf;
    }

    // ---- sampling ------------------------------------------------------------------------------
#if !defined(_WIN32)
    thread_local Target *t_self = nullptr;
    void sigprofHandler(int, siginfo_t *, void *uc)
    {
        Target *t = t_self;
        if (!t) return;
        const uintptr_t rip = (uintptr_t)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
        const uint32_t h = t->head.load(std::memory_order_relaxed);
        t->ring[h & (kRing - 1)] = rip;
        t->head.store(h + 1, std::memory_order_release);
    }
#endif

    void reportLoop()
    {
        using clock = std::chrono::steady_clock;
        auto last = clock::now();
        std::vector<std::unordered_map<std::string, uint64_t>> hist;
        std::vector<uint64_t> total;
        for (;;)
        {
#if defined(_WIN32)
            Sleep((DWORD)g_intervalMs);
            {
                std::lock_guard<std::mutex> lk(g_mx);
                for (Target *t : g_targets)
                {
                    if (!t->handle) continue;
                    if (SuspendThread(t->handle) == (DWORD)-1) continue;
                    alignas(16) CONTEXT c; std::memset(&c, 0, sizeof(c)); c.ContextFlags = stackFilter() ? CONTEXT_FULL : CONTEXT_CONTROL;
                    uintptr_t rip = 0;
                    if (GetThreadContext(t->handle, &c)) rip = (uintptr_t)c.Rip;
                    if (rip && stackFilter() && t->stacks.size() < 20000u)
                    {   // [eeprofstack] unwind while the thread is still suspended (x64 unwind tables, no symbols needed)
                        std::array<uintptr_t, 16> st{};
                        CONTEXT u = c;
                        for (int d = 0; d < 16 && u.Rip; ++d)
                        {
                            st[d] = (uintptr_t)u.Rip;
                            DWORD64 imageBase = 0;
                            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(u.Rip, &imageBase, nullptr);
                            if (!fn) { u.Rip = *reinterpret_cast<DWORD64 *>(u.Rsp); u.Rsp += 8; continue; }   // leaf function
                            void *hd = nullptr; DWORD64 ef = 0;
                            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, u.Rip, fn, &u, &hd, &ef, nullptr);
                        }
                        t->stacks.push_back(st);
                    }
                    ResumeThread(t->handle);
                    if (rip) { const uint32_t h = t->head.load(std::memory_order_relaxed); t->ring[h & (kRing - 1)] = rip; t->head.store(h + 1, std::memory_order_release); }
                }
            }
#else
            std::this_thread::sleep_for(std::chrono::milliseconds(50));   // the timers sample; this thread only drains
#endif
            std::lock_guard<std::mutex> lk(g_mx);
            if (!g_fnsBuilt) buildTable();
            if (hist.size() < g_targets.size()) { hist.resize(g_targets.size()); total.resize(g_targets.size(), 0); }
            for (size_t i = 0; i < g_targets.size(); ++i)
            {
                Target *t = g_targets[i];
                const uint32_t h = t->head.load(std::memory_order_acquire);
                if (h - t->tail > kRing) t->tail = h - kRing;   // overrun: drop the oldest
                for (; t->tail != h; ++t->tail) { ++hist[i][nameFor(t->ring[t->tail & (kRing - 1)])]; ++total[i]; }
            }
            const auto now = clock::now();
            if (std::chrono::duration<double>(now - last).count() < 10.0) continue;
            last = now;
            {   // [waitprof] blocked time per wait site over the window, ms/s
                static const char *const kSite[WP_COUNT] = { "framegate", "kickq_frames", "kickq_full", "kick_drain", "sched_yield", "sched_slot", "handoff", "sema", "sync_other", "sleep", "worker_idle", "fence_syncpath", "fence_storeimg", "barrier_post", "barrier_upload", "decpool", "stage2_idle" };
                static uint64_t lastNs[WP_COUNT] = {0}, lastN[WP_COUNT] = {0};
                std::string line;
                for (int k = 0; k < WP_COUNT; ++k)
                {
                    const uint64_t ns = g_ps2xWaitNs[k].load(std::memory_order_relaxed), n = g_ps2xWaitN[k].load(std::memory_order_relaxed);
                    char b[96]; std::snprintf(b, sizeof(b), " %s=%.1f(%llu)", kSite[k], (double)(ns - lastNs[k]) / 1e7, (unsigned long long)(n - lastN[k]));
                    line += b; lastNs[k] = ns; lastN[k] = n;
                }
                std::fprintf(stderr, "[waitprof] ms/s(waits/10s):%s\n", line.c_str());
            }
            for (size_t i = 0; i < g_targets.size(); ++i)
            {
                if (total[i] == 0) continue;
                uint64_t ee = 0, ovl = 0;
                std::vector<std::pair<uint64_t, std::string>> top;
                for (auto &kv : hist[i]) { top.push_back({kv.second, kv.first}); if (kv.first.compare(0, 3, "ee:") == 0) ee += kv.second; else if (kv.first.compare(0, 4, "ovl:") == 0) ovl += kv.second; }
                std::sort(top.begin(), top.end(), [](auto &a, auto &b){ return a.first > b.first; });
                std::string line;
                for (size_t k = 0; k < top.size() && k < 24; ++k)
                {
                    char b[128]; std::snprintf(b, sizeof(b), " %s=%.1f%%", top[k].second.c_str(), 100.0 * (double)top[k].first / (double)total[i]);
                    line += b;
                }
                std::fprintf(stderr, "[eeprof] %s: %llu samples/10s | ee %.1f%% ovl %.1f%% other %.1f%% |%s\n", g_targets[i]->name.c_str(),
                             (unsigned long long)total[i], 100.0 * (double)ee / (double)total[i], 100.0 * (double)ovl / (double)total[i],
                             100.0 * (double)(total[i] - ee - ovl) / (double)total[i], line.c_str());
#if defined(_WIN32)
                if (stackFilter() && !g_targets[i]->stacks.empty())
                {
                    std::unordered_map<std::string, uint64_t> chains; uint64_t matched = 0;
                    for (const auto &st : g_targets[i]->stacks)
                    {
                        if (nameFor(st[0]).find(stackFilter()) == std::string::npos) continue;
                        ++matched;
                        std::string chain;
                        for (int d = 1, shown = 0; d < 16 && st[d] && shown < 6; ++d)
                        {
                            const std::string n = nameFor(st[d] - 1);
                            if (n.compare(0, 3, "rt:") == 0 && (n.find("Nt") == 3 || n.find("Zw") == 3)) continue;
                            chain += (shown++ ? " < " : "") + n.substr(n.compare(0, 3, "rt:") == 0 ? 3 : 0);
                        }
                        ++chains[chain];
                    }
                    std::vector<std::pair<uint64_t, std::string>> tc;
                    for (auto &kv : chains) tc.push_back({kv.second, kv.first});
                    std::sort(tc.begin(), tc.end(), [](auto &a, auto &b){ return a.first > b.first; });
                    for (size_t k = 0; k < tc.size() && k < 8; ++k)
                        std::fprintf(stderr, "[eeprofstack] %s %.1f%% of samples: %s\n", g_targets[i]->name.c_str(),
                                     100.0 * (double)tc[k].first / (double)g_targets[i]->stacks.size(), tc[k].second.c_str());
                    (void)matched;
                }
                g_targets[i]->stacks.clear();
#endif
                hist[i].clear(); total[i] = 0;
            }
        }
    }

    void ensureStarted()
    {
        if (g_started.exchange(true)) return;
        g_intervalMs = intervalFromEnv();
        std::thread(reportLoop).detach();
        std::fprintf(stderr, "[eeprof] sampling guest threads every %d ms (PS2X_EEPROF)\n", g_intervalMs);
    }
}

// Call from the thread to be sampled, once, after it has a name.
extern "C" void ps2xEeProfAddCurrentThread(const char *name)
{
    if (intervalFromEnv() == 0) return;
    Target *t = new Target();
    t->name = name ? name : "?";
    t->ring = new uintptr_t[kRing];
#if defined(_WIN32)
    t->handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
    if (!t->handle) { std::fprintf(stderr, "[eeprof] OpenThread failed for %s (error %lu)\n", t->name.c_str(), (unsigned long)GetLastError()); }
#else
    t_self = t;
    struct sigaction sa; std::memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigprofHandler; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, nullptr);
    struct sigevent sev; std::memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD_ID; sev.sigev_signo = SIGPROF; sev._sigev_un._tid = (pid_t)syscall(SYS_gettid);
    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &t->timer) == 0)
    {
        const int ms = intervalFromEnv();
        struct itimerspec its; its.it_interval.tv_sec = 0; its.it_interval.tv_nsec = (long)ms * 1000000L; its.it_value = its.it_interval;
        t->timerOn = (timer_settime(t->timer, 0, &its, nullptr) == 0);
    }
    if (!t->timerOn) std::fprintf(stderr, "[eeprof] per-thread timer failed for %s\n", t->name.c_str());
#endif
    { std::lock_guard<std::mutex> lk(g_mx); g_targets.push_back(t); }
    ensureStarted();
}
#else
// The sampler requires Windows x64 thread contexts or Linux x64 CPU timers.
// Keep the exported entry point on other hosts without starting a reporter.
extern "C" void ps2xEeProfAddCurrentThread(const char *)
{
    static std::once_flag once;
    std::call_once(once, [] {
        const char *e = std::getenv("PS2X_EEPROF");
        if (e && e[0] && e[0] != '0')
            std::fprintf(stderr, "[eeprof] sampling unavailable on this platform; use PS2X_GUESTPROF=1\n");
    });
}
#endif
