// [perfhud] The overlay's performance readout: game fps, CPU (this process / whole PC), RAM, GPU (this process /
// whole GPU), dedicated VRAM, and the GPU temperature on NVIDIA. Sampled once a second on its own thread so the
// render loop only copies a struct. GPU numbers come from Windows' "GPU Engine" / "GPU Process Memory" performance
// counters (any vendor, Windows 10 1709+); the temperature from nvml.dll when the NVIDIA driver provides it.
#include "runtime/ps2_perfhud.h"
#include <atomic>
#include <cstdint>
// [perfhud] pictures that reached the screen (the native renderer's present counts each new one)
std::atomic<uint64_t> g_ps2xShownPictures{0};

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern std::atomic<uint64_t> g_bt3FrameCount;
std::atomic<uint64_t> g_ps2xGameFrames{0};   // [cttrmeter] CTTR game frames (its frame limiter, game_overrides.cpp)

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#pragma comment(lib, "pdh.lib")

namespace
{
    std::mutex g_mx;
    PerfHudStats g_stats;
    std::atomic<bool> g_started{false};

    uint64_t ft(const FILETIME &f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

    struct Nvml
    {   // optional: nvml.dll ships with the NVIDIA driver (System32)
        HMODULE dll = nullptr; void *dev = nullptr;
        int (*getTemp)(void *, int, unsigned *) = nullptr;
        bool init()
        {
            dll = LoadLibraryW(L"nvml.dll");
            if (!dll) return false;
            auto initFn = reinterpret_cast<int (*)()>(GetProcAddress(dll, "nvmlInit_v2"));
            auto byIdx = reinterpret_cast<int (*)(unsigned, void **)>(GetProcAddress(dll, "nvmlDeviceGetHandleByIndex_v2"));
            getTemp = reinterpret_cast<int (*)(void *, int, unsigned *)>(GetProcAddress(dll, "nvmlDeviceGetTemperature"));
            if (!initFn || !byIdx || !getTemp || initFn() != 0 || byIdx(0, &dev) != 0) { getTemp = nullptr; return false; }
            return true;
        }
        int temp() { unsigned t = 0; return (getTemp && getTemp(dev, 0 /*GPU*/, &t) == 0) ? int(t) : -1; }
    };

    // Sum of a wildcard counter's instances, split into "ours" (instance name holds pid_<pid>_) and all.
    void sumCounter(PDH_HCOUNTER c, const std::wstring &pidTag, const wchar_t *mustHave, double &mine, double &all)
    {
        mine = all = 0.0;
        DWORD bytes = 0, n = 0;
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &n, nullptr) != PDH_MORE_DATA) return;
        std::vector<unsigned char> buf(bytes);
        auto *items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(buf.data());
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &n, items) != ERROR_SUCCESS) return;
        for (DWORD i = 0; i < n; ++i)
        {
            if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
            const std::wstring name = items[i].szName ? items[i].szName : L"";
            if (mustHave && name.find(mustHave) == std::wstring::npos) continue;
            all += items[i].FmtValue.doubleValue;
            if (name.find(pidTag) != std::wstring::npos) mine += items[i].FmtValue.doubleValue;
        }
    }

    void sampler()
    {
        const DWORD pid = GetCurrentProcessId();
        const std::wstring pidTag = L"pid_" + std::to_wstring(pid) + L"_";
        SYSTEM_INFO si; GetSystemInfo(&si);
        const double ncpu = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;

        PDH_HQUERY q = nullptr; PDH_HCOUNTER cEng = nullptr, cMem = nullptr;
        bool pdhOk = PdhOpenQueryW(nullptr, 0, &q) == ERROR_SUCCESS &&
                     PdhAddEnglishCounterW(q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &cEng) == ERROR_SUCCESS;
        if (pdhOk) PdhAddEnglishCounterW(q, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &cMem);
        if (pdhOk) PdhCollectQueryData(q);
        Nvml nv; const bool nvOk = nv.init();

        FILETIME c0, e0, k0, u0, si0, sk0, su0;
        GetProcessTimes(GetCurrentProcess(), &c0, &e0, &k0, &u0);
        GetSystemTimes(&si0, &sk0, &su0);
        uint64_t frames0 = g_bt3FrameCount.load(std::memory_order_relaxed) + g_ps2xGameFrames.load(std::memory_order_relaxed);
        uint64_t pics0 = g_ps2xShownPictures.load(std::memory_order_relaxed);
        auto t0 = std::chrono::steady_clock::now();
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            const auto t1 = std::chrono::steady_clock::now();
            const double dt = std::chrono::duration<double>(t1 - t0).count();
            PerfHudStats s;
            const uint64_t frames1 = g_bt3FrameCount.load(std::memory_order_relaxed) + g_ps2xGameFrames.load(std::memory_order_relaxed);
            s.fps = dt > 0 ? double(frames1 - frames0) / dt : 0.0;
            s.frameMs = s.fps > 0.5 ? 1000.0 / s.fps : 0.0;
            const uint64_t pics1 = g_ps2xShownPictures.load(std::memory_order_relaxed);
            s.pictures = dt > 0 ? double(pics1 - pics0) / dt : 0.0;
            pics0 = pics1;

            FILETIME c1, e1, k1, u1, si1, sk1, su1;
            GetProcessTimes(GetCurrentProcess(), &c1, &e1, &k1, &u1);
            GetSystemTimes(&si1, &sk1, &su1);
            const double proc = double((ft(k1) - ft(k0)) + (ft(u1) - ft(u0)));        // 100 ns units, all threads
            s.cpuGame = 100.0 * proc / (dt * 1e7 * ncpu);
            const double idle = double(ft(si1) - ft(si0)), busy = double((ft(sk1) - ft(sk0)) + (ft(su1) - ft(su0)));
            s.cpuTotal = busy > 0 ? 100.0 * (busy - idle) / busy : 0.0;                 // kernel time includes idle

            PROCESS_MEMORY_COUNTERS pmc{}; pmc.cb = sizeof(pmc);
            if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) s.ramMB = double(pmc.WorkingSetSize) / (1024.0 * 1024.0);

            if (pdhOk && PdhCollectQueryData(q) == ERROR_SUCCESS)
            {
                double mine = 0, all = 0;
                sumCounter(cEng, pidTag, L"engtype_3D", mine, all);
                s.gpuGame = mine; s.gpuTotal = all; s.haveGpu = true;
                if (cMem) { double vm = 0, va = 0; sumCounter(cMem, pidTag, nullptr, vm, va); s.vramMB = vm / (1024.0 * 1024.0); }
            }
            if (nvOk) s.gpuTempC = nv.temp();
            for (double *v : { &s.cpuGame, &s.cpuTotal, &s.gpuGame, &s.gpuTotal }) *v = *v < 0 ? 0 : (*v > 100 ? 100 : *v);
            {
                std::lock_guard<std::mutex> lk(g_mx);
                g_stats = s;
            }
            c0 = c1; e0 = e1; k0 = k1; u0 = u1; si0 = si1; sk0 = sk1; su0 = su1; frames0 = frames1; t0 = t1;
        }
    }
}

PerfHudStats ps2xPerfHudStats()
{
    if (!g_started.exchange(true)) std::thread(sampler).detach();   // starts on first use: no cost unless shown
    std::lock_guard<std::mutex> lk(g_mx);
    return g_stats;
}
#else
PerfHudStats ps2xPerfHudStats()
{   // fps only off Windows
    static uint64_t f0 = 0; static auto t0 = std::chrono::steady_clock::now(); static PerfHudStats s;
    const auto t1 = std::chrono::steady_clock::now(); const double dt = std::chrono::duration<double>(t1 - t0).count();
    if (dt >= 1.0)
    {
        const uint64_t f1 = g_bt3FrameCount.load(std::memory_order_relaxed);
        s.fps = (f1 - f0) / dt; s.frameMs = s.fps > 0.5 ? 1000.0 / s.fps : 0.0; f0 = f1; t0 = t1;
    }
    return s;
}
#endif
