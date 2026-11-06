#pragma once
// [gsmirror] Pack mode parses every GIF packet twice on the GS thread: our state-only GS (VRAM, palettes, page stamps
// for the texture-pack hook) and then paraLLEl-GS. This runs our parse on its own thread, one packet behind or level
// with the backend, never ahead of it:
//   - the GS thread pushes packet N, then hands N to the backend; the mirror cannot see N+1 before the backend is done
//     with N, so whenever the backend asks (the replacement hook), sync() leaves our state exactly where the serial
//     order had it (our parse of N complete, nothing later);
//   - sync() before anything else that reads our GS (swap, GS apply jobs, the channel-busy release, going idle).
// PS2X_GSMIRROR=0 keeps the serial order.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

struct GifArbiterPacket;

class GsMirror
{
public:
    using Fn = std::function<void(const GifArbiterPacket &)>;
    static bool wanted();   // pack mode and not PS2X_GSMIRROR=0

    explicit GsMirror(Fn fn);
    ~GsMirror();

    void push(const GifArbiterPacket *pkt);   // the producer (GS thread) only; pkt stays alive until sync()
    void sync();                              // wait until every pushed packet is parsed

private:
    void loop();
    static constexpr uint32_t kRing = 8192u;
    const GifArbiterPacket *m_ring[kRing] = {};
    std::atomic<uint64_t> m_head{0}, m_tail{0};
    std::atomic<bool> m_sleeping{false}, m_stop{false};
    Fn m_fn;
    std::thread m_thread;
};

// The replacement hook's sync (ps2_gs_pgs.cpp): a no-op unless a mirror is running.
void ps2xGsMirrorSync();
