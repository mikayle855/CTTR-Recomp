#include "runtime/ps2_gs_mirror.h"
#include "runtime/ps2_gs_pgs.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#define PS2X_PAUSE() _mm_pause()
#else
#define PS2X_PAUSE() std::this_thread::yield()
#endif

extern "C" void ps2xEeProfAddCurrentThread(const char *name);
extern "C" void ps2xThreadTune(const char *role);

static std::atomic<GsMirror *> g_mirror{nullptr};

void ps2xGsMirrorSync()
{
    if (GsMirror *m = g_mirror.load(std::memory_order_acquire)) m->sync();
}

bool GsMirror::wanted()
{   // only the (removed) paraLLEl-GS texture-pack mode ran our GS parse on its own thread
    return false;
}

GsMirror::GsMirror(Fn fn) : m_fn(std::move(fn))
{
    m_thread = std::thread([this]() { loop(); });
    g_mirror.store(this, std::memory_order_release);
}

GsMirror::~GsMirror()
{
    sync();
    g_mirror.store(nullptr, std::memory_order_release);
    m_stop.store(true, std::memory_order_seq_cst);
    m_head.fetch_add(1, std::memory_order_seq_cst);   // wakes the loop, which sees m_stop before it reads the slot
    m_head.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void GsMirror::push(const GifArbiterPacket *pkt)
{
    const uint64_t h = m_head.load(std::memory_order_relaxed);
    while (h - m_tail.load(std::memory_order_acquire) >= kRing) PS2X_PAUSE();   // full: the mirror is 8192 packets behind
    m_ring[h % kRing] = pkt;
    m_head.store(h + 1, std::memory_order_seq_cst);
    if (m_sleeping.load(std::memory_order_seq_cst)) m_head.notify_one();
}

void GsMirror::sync()
{
    const uint64_t target = m_head.load(std::memory_order_acquire);
    for (uint32_t spins = 0; m_tail.load(std::memory_order_acquire) < target; ++spins)
    {
        if (spins < 4096u) PS2X_PAUSE();
        else std::this_thread::yield();
    }
}

void GsMirror::loop()
{
    ps2xEeProfAddCurrentThread("GsMirror");   // [eeprof]
    ps2xThreadTune("GsMirror");
    for (;;)
    {
        const uint64_t t = m_tail.load(std::memory_order_relaxed);
        uint64_t h = m_head.load(std::memory_order_acquire);
        if (h == t)
        {   // spin briefly (packets arrive ~1 us apart while a frame is drawn), then sleep until push() wakes us. A long
            // spin kept a laptop core busy 70% of the time; PS2X_GSMIRROR_SPIN = pause iterations (default 256)
            static const uint32_t s_spin = [](){ const char *v = std::getenv("PS2X_GSMIRROR_SPIN"); return v && v[0] ? uint32_t(std::strtoul(v, nullptr, 10)) : 256u; }();
            for (uint32_t i = 0; i < s_spin && h == t; ++i) { PS2X_PAUSE(); h = m_head.load(std::memory_order_acquire); }
            if (h == t)
            {
                m_sleeping.store(true, std::memory_order_seq_cst);
                while (m_head.load(std::memory_order_seq_cst) == t)
                    m_head.wait(t, std::memory_order_seq_cst);
                m_sleeping.store(false, std::memory_order_relaxed);
            }
        }
        if (m_stop.load(std::memory_order_acquire)) return;
        m_fn(*m_ring[t % kRing]);
        m_tail.store(t + 1, std::memory_order_release);
    }
}
