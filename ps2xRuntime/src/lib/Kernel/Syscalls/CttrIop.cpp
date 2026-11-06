// Host-side (HLE) stand-in for the IOP driver Crash Tag Team Racing (SLES_534.39) ships inside its ELF.
//
// The game DMAs the driver to the IOP, loads it with sceSifLoadModuleBuffer, registers a SIF command handler
// (FUN_004675f0) and then talks to it with sceSifSendCmd. We cannot run IOP code, so without this the driver never
// says "ready" and boot spins in FUN_00468510 forever. The driver does the game's streamed file reads as well as
// audio. Every packet is a 16-byte SIF command header with the message type in the header's opt word (+0x0c),
// followed by the payload:
//
//   EE -> IOP                                              IOP -> EE (reply, delivered to the game's handler)
//   0 read  +10 handle +14 EE dest +18 IOP buf +1c block   0 done  +10 bytes (0 = failed) +14 IOP state +18 EE dest
//   1 open  name in the extra data (EE src = $a3)          1 done  +10 ok (!=0) +14 handle +18 IOP state +1c size
//   2 close +10 handle                                     2 ack (the game spins until it arrives)
//   4 audio stream: +10 channel +14 on +18 IOP buffer       3 ready (sent once the module is "loaded")
//   5 driver settings                                      4 timer tick (no payload)
//                                                          5 audio data request: +10 channel +14 IOP dest
//
// A read moves one 0x1000-byte block. A handle of 0x80000000|LBN means raw disc reads, and the block number is then
// the absolute disc offset / 0x1000; open returns handles of that form, so one path serves both.
//
// The ticker thread is the driver's clock. A timer tick runs the game's millisecond timers (it times them itself
// with COP0 Count). A data request makes the game fill 0x800 bytes of a channel through its stream callbacks (Bink
// movies, music) and SIF-DMA them to the IOP, so the request rate is the playback clock: without it a movie stops
// once its audio buffers are full.
//
// Audio. The driver uses no SPU2 voices at all: channel N is SPU2 core N playing a looping 0x1000-byte block
// transfer (sceSdBlockTrans mode 0x10) of 48 kHz 16-bit PCM, laid out as 0x200 bytes left / 0x200 bytes right.
// Its transfer interrupt asks the EE to refill the half that just finished (first half first). Cmd 5 sets the
// core's master volume (+14/+18) and block-input volume (+1c/+20), -1 = unchanged; input volume starts at 0x3fff.
// Here the refilled half is read back from IOP RAM after the game's handler has DMAed it and is mixed to an SDL
// device, and requests are paced by how much of each channel is still buffered there, so the sound device is the
// clock as the SPU2 is on hardware. Core 0's output runs through core 1's master volume as on the SPU2. Without a
// device, requests fall back to wall-clock pacing (PS2X_CTTRIOP_AUDIO_BPS overrides the bytes per second).
//
// Replies go through a queue and a host thread that runs the game's handler under the guest-execution lock on its
// own context and stack, the way an interrupt would arrive. Answering inside sceSifSendCmd instead would nest one
// level per block (the handler issues the next read), i.e. hundreds of levels for one file.

#include "Common.h"
#include "ps2_runtime_macros.h"

#if defined(PS2X_HAVE_SDL2)
#include "../../ps2_host_sdl.h"
#endif
#include "../Iop/Spu2.h"
#include "runtime/ps2_audio.h"   // [cttrvol] the Settings window's volume

#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <atomic>
#include <cstring>
extern std::atomic<bool> g_ps2xFocusPaused;   // [focuspause] ps2_settings_overlay.cpp

namespace ps2_stubs
{
    uint32_t sifCmdHandlerFor(uint32_t cid);   // Stubs/SIF.cpp
}

namespace ps2_syscalls
{
    namespace
    {
        // the module image CTTR loads: FNV-1a 64 of its first 2048 bytes (ELF file offset 0x4a49c0), as System.cpp tags it
        constexpr const char *kDriverTag = "iopbuf:fnv64:3a40da6a1f41a158";
        constexpr uint32_t kCidAddr = 0x005e66a0u;   // DAT_005e66a0: the command id the game uses both ways
        constexpr uint32_t kBlock = 0x1000u;
        constexpr uint32_t kSector = 0x800u;

        struct Reply
        {
            uint32_t type = 0;
            uint32_t payload[4] = {0, 0, 0, 0};     // +0x10..+0x1f
            std::vector<uint8_t> data;              // copied to dataDest before the handler runs
            uint32_t dataDest = 0;
            std::chrono::steady_clock::time_point queued{};   // [cttrdeliver] latency stats
        };

        struct State
        {
            std::mutex mutex;
            std::condition_variable cv;
            std::deque<Reply> queue;
            bool active = false;
            uint32_t cid = 0;
            uint32_t gp = 0;
            uint32_t packetAddr = 0;
            uint32_t stackTop = 0;
            std::mutex discMutex;
            std::ifstream disc;
            bool streamOn[2] = {false, false};      // guarded by mutex
            uint32_t streamIopBuf[2] = {0, 0};
            uint32_t streamHalf[2] = {0, 0};        // next half to refill
        };
        State g_cttr;

        // The two SPU2 cores as far as this driver drives them. Guarded by its own mutex, which is only ever taken
        // after g_cttr.mutex, never before.
        constexpr uint32_t kHalf = 0x800u;
        constexpr uint32_t kHalfFrames = kHalf / 4u;   // 512 stereo frames
        // Buffered + requested per channel (~85 ms). The SPU2 itself holds only the two halves, but its interrupt is
        // answered at once; here the answer waits for the guest lock, so a deeper queue keeps the device fed.
        constexpr size_t kTargetFrames = 8u * kHalfFrames;
        constexpr uint32_t kIopRamSize = 0x200000u;
        struct Spu
        {
            std::mutex mutex;
            std::deque<int16_t> fifo[2];            // interleaved L,R
            uint32_t inFlight[2] = {0, 0};          // requests queued but not yet captured
            bool deviceTried = false;
            bool deviceOk = false;
        };
        Spu g_spu;
        std::atomic<bool> g_cttrStreamOnRelaxed[2] = {false, false};   // copy of streamOn the audio callback may read

        // Master volume (MVOL) is in volume-slide format: bit 15 clear = fixed, 15-bit signed level with 0x3fff ~ 1.0.
        float spuGain(uint16_t v)
        {
            if (v & 0x8000u)
                return 1.0f;   // sweep modes: this driver never sets them
            return static_cast<float>(static_cast<int16_t>(static_cast<uint16_t>(v << 1))) / 32768.0f;
        }

        // Block-input volume (BVOL) is a plain signed 16-bit level, 0x7fff ~ 1.0.
        float spuInputGain(uint16_t v)
        {
            return static_cast<float>(static_cast<int16_t>(v)) / 32768.0f;
        }

        // [cttraudio] every ~10 s: blocks captured, peak level, frames the device found missing.
        struct SpuStats
        {
            uint32_t blocks[2] = {0, 0};
            int32_t peak[2] = {0, 0};
            uint64_t underrunFrames = 0;
        };
        SpuStats g_spuStats;   // guarded by g_spu.mutex

#if defined(PS2X_HAVE_SDL2)
        void SDLCALL spuCallback(void *, Uint8 *stream, int len)
        {
            int16_t *out = reinterpret_cast<int16_t *>(stream);
            const size_t frames = static_cast<size_t>(len) / 4u;
            // Each core: its voices (Iop/Spu2.cpp, driven by AUDIO.IRX) plus its block-input stream at BVOL, through its
            // MVOL; core 0's output feeds core 1, whose output is the console's.
            {   // [focuspause] window in the background: silence, voices and the music stream stay where they are
                if (g_ps2xFocusPaused.load(std::memory_order_relaxed))
                {
                    std::memset(stream, 0, static_cast<size_t>(len));
                    return;
                }
            }
            static thread_local std::vector<float> s_core[2];
            for (auto &v : s_core) v.assign(frames * 2u, 0.0f);
            ps2_spu2::renderVoices(uint32_t(frames), s_core[0].data(), s_core[1].data());
            {   // [spu2out] every ~10 s: the peak level of the SPU2 voices (AUDIO.IRX: music, sound effects) per core
                static float s_peak[2] = {0.0f, 0.0f};
                static auto s_t = std::chrono::steady_clock::now();
                for (int c = 0; c < 2; ++c)
                    for (float x : s_core[c]) s_peak[c] = std::max(s_peak[c], std::fabs(x));
                if (std::chrono::steady_clock::now() - s_t >= std::chrono::seconds(10))
                {
                    s_t = std::chrono::steady_clock::now();
                    std::fprintf(stderr, "[spu2out] 10 s: voice peak core0 %d core1 %d\n", int(s_peak[0]), int(s_peak[1]));
                    s_peak[0] = s_peak[1] = 0.0f;
                }
            }
            float in[2][2], master[2][2];
            for (int c = 0; c < 2; ++c)
                for (int s = 0; s < 2; ++s)
                {
                    in[c][s] = spuInputGain(ps2_spu2::inputVolume(c, s));
                    master[c][s] = spuGain(ps2_spu2::masterVolume(c, s));
                }
            // [cttrvol] the Settings window's Volume on the console's output (music and effects share the SPU2 voices,
            // so CTTR offers one level)
            const float userVol = std::clamp(PS2AudioBackend::masterVolume(), 0.0f, 1.0f);
            master[1][0] *= userVol;
            master[1][1] *= userVol;
            std::lock_guard<std::mutex> lock(g_spu.mutex);
            for (size_t f = 0; f < frames; ++f)
            {
                for (uint32_t c = 0; c < 2; ++c)
                {
                    std::deque<int16_t> &q = g_spu.fifo[c];
                    if (q.size() < 2u)
                    {
                        if (g_cttrStreamOnRelaxed[c].load(std::memory_order_relaxed))
                            ++g_spuStats.underrunFrames;
                        continue;
                    }
                    s_core[c][f * 2u] += q.front() * in[c][0];
                    q.pop_front();
                    s_core[c][f * 2u + 1u] += q.front() * in[c][1];
                    q.pop_front();
                }
                for (uint32_t s = 0; s < 2; ++s)
                {
                    const float core1 = s_core[1][f * 2u + s] + s_core[0][f * 2u + s] * master[0][s];
                    out[f * 2u + s] = static_cast<int16_t>(std::clamp(core1 * master[1][s], -32768.0f, 32767.0f));
                }
            }
        }
#endif

        bool spuDevice()
        {
            std::lock_guard<std::mutex> lock(g_spu.mutex);
            if (g_spu.deviceTried)
                return g_spu.deviceOk;
            g_spu.deviceTried = true;
#if defined(PS2X_HAVE_SDL2)
            if (const char *v = std::getenv("PS2X_CTTR_AUDIO"); v && v[0] == '0')
                return false;
            SDL_SetMainReady();
            if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
            {
                std::fprintf(stderr, "[cttriop] SDL audio init failed: %s (silent)\n", SDL_GetError());
                return false;
            }
            SDL_AudioSpec want{}, have{};
            want.freq = 48000;
            want.format = AUDIO_S16SYS;
            want.channels = 2;
            want.samples = 512;
            want.callback = spuCallback;
            const SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
            if (dev == 0)
            {
                std::fprintf(stderr, "[cttriop] SDL_OpenAudioDevice failed: %s (silent)\n", SDL_GetError());
                return false;
            }
            SDL_PauseAudioDevice(dev, 0);
            g_spu.deviceOk = true;
            std::fprintf(stderr, "[cttriop] SPU2 output: SDL %s, 48000 Hz stereo\n",
                         SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");
#endif
            return g_spu.deviceOk;
        }

        // A data request for `ch` has been answered: the game's handler has DMAed the half at `dest` to IOP RAM.
        void spuCapture(PS2Runtime *runtime, uint32_t ch, uint32_t dest)
        {
            if (ch >= 2u)
                return;
            bool on = false;
            {
                std::lock_guard<std::mutex> lock(g_cttr.mutex);
                on = g_cttr.streamOn[ch];
            }
            const uint8_t *iop = runtime->memory().getIOPRAM();
            const uint32_t off = dest & 0x1FFFFFFFu;
            std::lock_guard<std::mutex> lock(g_spu.mutex);
            if (g_spu.inFlight[ch] > 0u)
                --g_spu.inFlight[ch];
            if (!on || !g_spu.deviceOk || !iop || off >= kIopRamSize || kHalf > kIopRamSize - off)
                return;
            std::deque<int16_t> &q = g_spu.fifo[ch];
            for (uint32_t blk = 0; blk < kHalf; blk += 0x400u)
            {
                const uint8_t *l = iop + off + blk;
                const uint8_t *r = l + 0x200u;
                for (uint32_t i = 0; i < 0x200u; i += 2u)
                {
                    int16_t sl, sr;
                    std::memcpy(&sl, l + i, 2);
                    std::memcpy(&sr, r + i, 2);
                    q.push_back(sl);
                    q.push_back(sr);
                    g_spuStats.peak[ch] = std::max({g_spuStats.peak[ch], std::abs(int32_t(sl)), std::abs(int32_t(sr))});
                }
            }
            ++g_spuStats.blocks[ch];
            static auto s_last = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            if (now - s_last >= std::chrono::seconds(10))
            {
                s_last = now;
                std::fprintf(stderr, "[cttraudio] 10 s: core0 %u blocks peak %d | core1 %u blocks peak %d | underrun frames %llu"
                                     " | bvol1 %04x/%04x mvol1 %04x/%04x\n",
                             g_spuStats.blocks[0], g_spuStats.peak[0], g_spuStats.blocks[1], g_spuStats.peak[1],
                             (unsigned long long)g_spuStats.underrunFrames, ps2_spu2::inputVolume(1, 0), ps2_spu2::inputVolume(1, 1),
                             ps2_spu2::masterVolume(1, 0), ps2_spu2::masterVolume(1, 1));
                g_spuStats = SpuStats{};
            }
        }

        bool logOn()
        {
            static const bool s_on = [](){ const char *v = std::getenv("PS2X_CTTRIOP_LOG"); return v && v[0] && v[0] != '0'; }();
            return s_on;
        }

        uint32_t rd32(uint8_t *rdram, uint32_t addr)
        {
            uint32_t v = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, addr))
                std::memcpy(&v, p, 4);
            return v;
        }

        void wr32(uint8_t *rdram, uint32_t addr, uint32_t v)
        {
            if (uint8_t *p = getMemPtr(rdram, addr))
                std::memcpy(p, &v, 4);
        }

        // Read bytes at an absolute disc offset from PS2X_CD_IMAGE. Returns the number read.
        uint32_t readDisc(uint64_t offset, uint8_t *out, uint32_t size)
        {
            std::lock_guard<std::mutex> lock(g_cttr.discMutex);
            if (!g_cttr.disc.is_open())
            {
                const std::filesystem::path image = PS2Runtime::getIoPaths().cdImage;
                if (image.empty())
                {
                    std::cerr << "[cttriop] no disc image: set PS2X_CD_IMAGE" << std::endl;
                    return 0;
                }
                g_cttr.disc.open(image, std::ios::binary);
                if (!g_cttr.disc.is_open())
                {
                    std::cerr << "[cttriop] cannot open disc image " << image.string() << std::endl;
                    return 0;
                }
            }
            g_cttr.disc.clear();
            g_cttr.disc.seekg(static_cast<std::streamoff>(offset));
            g_cttr.disc.read(reinterpret_cast<char *>(out), size);
            return static_cast<uint32_t>(g_cttr.disc.gcount());
        }

        // ISO9660 path lookup ("cdrom0:\\DIR\\FILE.EXT;1", any case or separator). Returns false when not found.
        bool findOnDisc(std::string path, uint32_t &lbn, uint32_t &size)
        {
            if (auto colon = path.find(':'); colon != std::string::npos)
                path = path.substr(colon + 1);
            if (auto semi = path.find(';'); semi != std::string::npos)
                path = path.substr(0, semi);
            for (char &c : path)
                c = (c == '/') ? '\\' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

            uint8_t sector[kSector];
            if (readDisc(16ull * kSector, sector, kSector) != kSector)
                return false;
            uint32_t dirLbn = 0, dirSize = 0;
            std::memcpy(&dirLbn, sector + 156 + 2, 4);
            std::memcpy(&dirSize, sector + 156 + 10, 4);

            size_t pos = 0;
            while (pos < path.size())
            {
                while (pos < path.size() && path[pos] == '\\')
                    ++pos;
                const size_t end = std::min(path.find('\\', pos), path.size());
                const std::string part = path.substr(pos, end - pos);
                pos = end;
                if (part.empty())
                    break;
                bool found = false;
                for (uint32_t done = 0; done < dirSize && !found; done += kSector)
                {
                    if (readDisc(static_cast<uint64_t>(dirLbn) * kSector + done, sector, kSector) != kSector)
                        return false;
                    for (uint32_t off = 0; off < kSector && sector[off] != 0; off += sector[off])
                    {
                        const uint8_t *rec = sector + off;
                        std::string name(reinterpret_cast<const char *>(rec + 33), rec[32]);
                        if (auto semi = name.find(';'); semi != std::string::npos)
                            name = name.substr(0, semi);
                        if (name != part)
                            continue;
                        std::memcpy(&dirLbn, rec + 2, 4);
                        std::memcpy(&dirSize, rec + 10, 4);
                        found = true;
                        break;
                    }
                }
                if (!found)
                    return false;
            }
            lbn = dirLbn;
            size = dirSize;
            return true;
        }

        void push(Reply &&r)
        {
            r.queued = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(g_cttr.mutex);
                g_cttr.queue.push_back(std::move(r));
            }
            g_cttr.cv.notify_one();
        }

        void deliverLoop(uint8_t *rdram, PS2Runtime *runtime)
        {
            g_currentThreadId = -1;   // handlers run as interrupt context
            while (!runtime->isStopRequested())
            {
                Reply r;
                {
                    std::unique_lock<std::mutex> lock(g_cttr.mutex);
                    g_cttr.cv.wait_for(lock, std::chrono::milliseconds(50), []() { return !g_cttr.queue.empty(); });
                    if (g_cttr.queue.empty())
                        continue;
                    r = std::move(g_cttr.queue.front());
                    g_cttr.queue.pop_front();
                }
                const uint32_t handler = ps2_stubs::sifCmdHandlerFor(g_cttr.cid);
                if (handler == 0u || !runtime->hasFunction(handler))
                {
                    std::cerr << "[cttriop] no EE handler for cid 0x" << std::hex << g_cttr.cid << std::dec
                              << ", dropping reply type " << r.type << std::endl;
                    continue;
                }

                const auto lockStart = std::chrono::steady_clock::now();
                PS2Runtime::GuestExecutionScope guard(runtime);
                {   // [cttrdeliver] PS2X_CTTRDELIVER=1: per-10 s reply counts, queue->handler latency and guest-lock wait
                    static const bool s_on = [](){ const char *v = std::getenv("PS2X_CTTRDELIVER"); return v && v[0] && v[0] != '0'; }();
                    if (s_on)
                    {
                        using ms = std::chrono::duration<double, std::milli>;
                        static uint32_t s_count[8] = {};
                        static double s_latMax[8] = {}, s_latSum[8] = {}, s_lockMax = 0.0, s_lockSum = 0.0;
                        static size_t s_depthMax = 0;
                        static auto s_last = std::chrono::steady_clock::now();
                        const auto now = std::chrono::steady_clock::now();
                        const uint32_t t = r.type < 8u ? r.type : 7u;
                        const double lat = ms(now - r.queued).count(), lk = ms(now - lockStart).count();
                        ++s_count[t]; s_latSum[t] += lat; s_latMax[t] = std::max(s_latMax[t], lat);
                        s_lockSum += lk; s_lockMax = std::max(s_lockMax, lk);
                        { std::lock_guard<std::mutex> ql(g_cttr.mutex); s_depthMax = std::max(s_depthMax, g_cttr.queue.size()); }
                        if (now - s_last >= std::chrono::seconds(10))
                        {
                            s_last = now;
                            uint32_t total = 0; for (uint32_t i = 0; i < 8; ++i) total += s_count[i];
                            std::fprintf(stderr, "[cttrdeliver] 10 s: read %u (lat avg %.2f max %.1f ms) tick %u (avg %.2f max %.1f) stream %u (avg %.2f max %.1f)"
                                                 " | lock wait avg %.3f max %.1f ms | queue max %zu\n",
                                         s_count[0], s_count[0] ? s_latSum[0] / s_count[0] : 0.0, s_latMax[0],
                                         s_count[4], s_count[4] ? s_latSum[4] / s_count[4] : 0.0, s_latMax[4],
                                         s_count[5], s_count[5] ? s_latSum[5] / s_count[5] : 0.0, s_latMax[5],
                                         total ? s_lockSum / total : 0.0, s_lockMax, s_depthMax);
                            std::fill(std::begin(s_count), std::end(s_count), 0u);
                            std::fill(std::begin(s_latMax), std::end(s_latMax), 0.0);
                            std::fill(std::begin(s_latSum), std::end(s_latSum), 0.0);
                            s_lockMax = s_lockSum = 0.0; s_depthMax = 0;
                        }
                    }
                }
                if (!r.data.empty())
                    if (uint8_t *dst = getMemPtr(rdram, r.dataDest))
                        std::memcpy(dst, r.data.data(), r.data.size());
                const uint32_t pkt = g_cttr.packetAddr;
                wr32(rdram, pkt + 0x00u, 0x20u);   // psize 0x20, no extra data
                wr32(rdram, pkt + 0x04u, 0u);
                wr32(rdram, pkt + 0x08u, g_cttr.cid);
                wr32(rdram, pkt + 0x0cu, r.type);
                for (uint32_t i = 0; i < 4; ++i)
                    wr32(rdram, pkt + 0x10u + 4u * i, r.payload[i]);

                R5900Context hctx{};
                SET_GPR_U32(&hctx, 28, g_cttr.gp);
                SET_GPR_U32(&hctx, 29, g_cttr.stackTop);
                SET_GPR_U32(&hctx, 31, 0u);
                SET_GPR_U32(&hctx, 4, pkt);
                SET_GPR_U32(&hctx, 5, 0u);
                hctx.pc = handler;
                uint32_t steps = 0;
                while (hctx.pc != 0u && !runtime->isStopRequested() && ++steps < 1000000u)
                {
                    PS2Runtime::RecompiledFunction step = runtime->lookupFunction(hctx.pc);
                    if (!step)
                        break;
                    step(rdram, &hctx, runtime);
                }
                if (r.type == 5u)
                    spuCapture(runtime, r.payload[0], r.payload[1]);
            }
        }

        void tickerLoop(PS2Runtime *runtime)
        {
            using clock = std::chrono::steady_clock;
            static const double s_bps = [](){ const char *v = std::getenv("PS2X_CTTRIOP_AUDIO_BPS");
                                              const double d = v && v[0] ? std::strtod(v, nullptr) : 0.0;
                                              return d > 0.0 ? d : 48000.0 * 4.0; }();
            const auto blockTime = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(kHalf / s_bps));
            const auto tickTime = std::chrono::milliseconds(4);
            const bool device = spuDevice();
            auto nextTick = clock::now();
            clock::time_point nextBlock[2] = {nextTick, nextTick};
            bool wasOn[2] = {false, false};
            static const bool s_threadDump = [](){ const char *v = std::getenv("PS2X_THREADDUMP"); return v && v[0] && v[0] != '0'; }();
            auto nextDump = clock::now() + std::chrono::seconds(10);
            while (!runtime->isStopRequested())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                const auto now = clock::now();
                {   // [ramtrigger] PS2X_RAMTRIGGER=<file>: when it appears (checked twice a second), delete it and write the 32 MB of
                    // guest RAM to <file>.bin -- inspect a live hang's data structures offline
                    static const std::string s_rt = [](){ const char *v = std::getenv("PS2X_RAMTRIGGER"); return std::string(v ? v : ""); }();
                    static auto s_next = clock::now();
                    if (!s_rt.empty() && now >= s_next)
                    {
                        s_next = now + std::chrono::milliseconds(500);
                        if (FILE *t = std::fopen(s_rt.c_str(), "rb"))
                        {
                            std::fclose(t);
                            std::remove(s_rt.c_str());
                            if (const uint8_t *ram = runtime->memory().getRDRAM())
                                if (FILE *f = std::fopen((s_rt + ".bin").c_str(), "wb"))
                                {
                                    std::fwrite(ram, 1, 32u * 1024u * 1024u, f);
                                    std::fclose(f);
                                    std::fprintf(stderr, "[ramtrigger] wrote %s.bin\n", s_rt.c_str());
                                }
                            if (const uint8_t *iop = runtime->memory().getIOPRAM())
                                if (FILE *f = std::fopen((s_rt + ".iop").c_str(), "wb"))
                                {   // and the IOP's 2 MB to <file>.iop, the sound RAM to <file>.spu
                                    std::fwrite(iop, 1, 2u * 1024u * 1024u, f);
                                    std::fclose(f);
                                }
                            if (FILE *f = std::fopen((s_rt + ".spu").c_str(), "wb"))
                            {
                                std::fwrite(ps2_spu2::soundRam(), 1, 2u * 1024u * 1024u, f);
                                std::fclose(f);
                            }
                        }
                    }
                }
                if (s_threadDump && now >= nextDump)
                {   // [threaddump] PS2X_THREADDUMP=1: every EE thread's state every 10 s (who waits on what)
                    nextDump = now + std::chrono::seconds(10);
                    std::fprintf(stderr, "[threaddump] %s\n", dumpAllThreadStates().c_str());
                }
                if (g_ps2xFocusPaused.load(std::memory_order_relaxed))
                {   // [focuspause] the game stands still: send it no IOP ticks or stream requests (they piled up and
                    // were worked off in a burst after the resume = a few seconds below full speed); restart the tick clock
                    nextTick = now;
                    continue;
                }
                std::vector<Reply> out;
                {   // [cttrstream] every 5 s: each channel's on flag, requests in flight, buffered frames, requests sent
                    static auto s_nextStat = clock::now() + std::chrono::seconds(5);
                    if (now >= s_nextStat)
                    {
                        s_nextStat = now + std::chrono::seconds(5);
                        std::lock_guard<std::mutex> lock(g_cttr.mutex);
                        std::lock_guard<std::mutex> spuLock(g_spu.mutex);
                        std::fprintf(stderr, "[cttrstream] ch0 on=%d inflight=%u fifo=%zu | ch1 on=%d inflight=%u fifo=%zu | queue %zu | sent %u/%u\n",
                                     g_cttr.streamOn[0] ? 1 : 0, g_spu.inFlight[0], g_spu.fifo[0].size() / 2u,
                                     g_cttr.streamOn[1] ? 1 : 0, g_spu.inFlight[1], g_spu.fifo[1].size() / 2u, g_cttr.queue.size(),
                                     g_cttr.streamHalf[0], g_cttr.streamHalf[1]);
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(g_cttr.mutex);
                    if (g_cttr.queue.size() > 64u)   // the guest is busy: don't pile up stale ticks
                        continue;
                    if (now >= nextTick)
                    {
                        Reply t;
                        t.type = 4;
                        out.push_back(std::move(t));
                        nextTick = now + tickTime;
                    }
                    auto request = [&](uint32_t ch) {
                        Reply r;
                        r.type = 5;
                        r.payload[0] = ch;
                        r.payload[1] = g_cttr.streamIopBuf[ch] + (g_cttr.streamHalf[ch] & 1u) * kHalf;
                        ++g_cttr.streamHalf[ch];
                        out.push_back(std::move(r));
                    };
                    for (uint32_t ch = 0; ch < 2; ++ch)
                    {
                        if (!g_cttr.streamOn[ch])
                        {
                            wasOn[ch] = false;
                            continue;
                        }
                        if (device)
                        {
                            std::lock_guard<std::mutex> spuLock(g_spu.mutex);
                            if (!wasOn[ch])
                                g_spu.fifo[ch].clear();
                            wasOn[ch] = true;
                            while (g_spu.fifo[ch].size() / 2u + g_spu.inFlight[ch] * kHalfFrames < kTargetFrames)
                            {
                                ++g_spu.inFlight[ch];
                                request(ch);
                            }
                            continue;
                        }
                        if (!wasOn[ch] || now - nextBlock[ch] > std::chrono::milliseconds(200))
                            nextBlock[ch] = now;   // (re)start, or resync after a long guest stall
                        wasOn[ch] = true;
                        while (now >= nextBlock[ch])
                        {
                            request(ch);
                            nextBlock[ch] += blockTime;
                        }
                    }
                }
                for (Reply &r : out)
                    push(std::move(r));
            }
        }
    }

    // A whole file from the disc image ("cdrom0:\IRX\AUDIO.IRX;1"), for IOP modules that run as they are.
    bool cttrReadDiscFile(const std::string &path, std::vector<uint8_t> &out)
    {
        uint32_t lbn = 0, size = 0;
        if (!findOnDisc(path, lbn, size))
            return false;
        out.resize(size);
        return readDisc(uint64_t(lbn) * kSector, out.data(), size) == size;
    }

    void cttrIopOnModuleLoadBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const std::string &tag)
    {
        if (tag != kDriverTag || !runtime)
            return;
        {
            std::lock_guard<std::mutex> lock(g_cttr.mutex);
            if (g_cttr.active)
                return;
            g_cttr.active = true;
            g_cttr.cid = rd32(rdram, kCidAddr);
            g_cttr.gp = getRegU32(ctx, 28);
            g_cttr.packetAddr = runtime->reserveAsyncCallbackStack(0x100u, 64u) + 0x10u - 0x100u;
            g_cttr.stackTop = runtime->reserveAsyncCallbackStack(0x8000u, 16u);
        }
        std::cerr << "[cttriop] CTTR IOP driver (HLE) up: cid=0x" << std::hex << g_cttr.cid
                  << " handler=0x" << ps2_stubs::sifCmdHandlerFor(g_cttr.cid) << std::dec << std::endl;
        std::thread(deliverLoop, rdram, runtime).detach();
        std::thread(tickerLoop, runtime).detach();
        Reply ready;
        ready.type = 3;
        push(std::move(ready));
    }

    bool cttrIopOnSendCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t cid, uint32_t pkt,
                          uint32_t srcExtra, uint32_t sizeExtra)
    {
        (void)ctx;
        (void)runtime;
        if (!g_cttr.active || cid != g_cttr.cid)
            return false;

        const uint32_t type = rd32(rdram, pkt + 0x0cu);
        const uint32_t a = rd32(rdram, pkt + 0x10u), b = rd32(rdram, pkt + 0x14u), d = rd32(rdram, pkt + 0x1cu);
        Reply r;
        r.type = type;
        switch (type)
        {
        case 0:   // read one block
        {
            const uint32_t handle = a, dest = b, block = d;
            uint32_t n = 0;
            if ((handle & 0xFF000000u) == 0x80000000u)
            {
                r.data.resize(kBlock);
                n = readDisc(static_cast<uint64_t>(block) * kBlock, r.data.data(), kBlock);
                r.data.resize(n);
            }
            r.dataDest = dest;
            r.payload[0] = n;
            r.payload[2] = dest;
            if (logOn() || n == 0)
                std::fprintf(stderr, "[cttriop] read handle=0x%x block=0x%x -> 0x%x: %u bytes\n", handle, block, dest, n);
            break;
        }
        case 1:   // open by name
        {
            std::string name;
            const uint32_t limit = sizeExtra ? sizeExtra : 256u;
            for (uint32_t i = 0; i < limit; ++i)
            {
                const uint8_t *p = getConstMemPtr(rdram, srcExtra + i);
                if (!p || *p == 0)
                    break;
                name.push_back(static_cast<char>(*p));
            }
            uint32_t lbn = 0, size = 0;
            const bool ok = findOnDisc(name, lbn, size);
            r.payload[0] = ok ? 1u : 0u;
            r.payload[1] = ok ? (0x80000000u | lbn) : 0u;
            r.payload[3] = size;
            std::fprintf(stderr, "[cttriop] open \"%s\" -> %s lbn=0x%x size=0x%x\n", name.c_str(), ok ? "ok" : "NOT FOUND", lbn, size);
            break;
        }
        case 2:   // close: acknowledge
            break;
        case 4:   // audio stream on/off
            if (a < 2u)
            {
                std::lock_guard<std::mutex> lock(g_cttr.mutex);
                g_cttr.streamOn[a] = (b != 0u);
                g_cttrStreamOnRelaxed[a].store(b != 0u, std::memory_order_relaxed);
                if (b != 0u)
                {
                    g_cttr.streamIopBuf[a] = rd32(rdram, pkt + 0x18u);
                    g_cttr.streamHalf[a] = 0u;
                }
            }
            [[fallthrough]];
        case 5:   // volumes
        {
            if (type == 5u && a < 2u)
            {
                // the driver's sceSdSetParam calls: MVOLL, MVOLR, BVOLL, BVOLR of core `a`
                const uint32_t vols[4] = {b, rd32(rdram, pkt + 0x18u), d, rd32(rdram, pkt + 0x20u)};
                const uint16_t regs[4] = {0x0980, 0x0a80, 0x0f80, 0x1080};
                for (uint32_t i = 0; i < 4; ++i)
                    if (vols[i] != 0xFFFFFFFFu)
                        ps2_spu2::setParam(uint16_t(regs[i] | a), static_cast<uint16_t>(vols[i]));
            }
            static std::atomic<uint32_t> s_n{0};
            if (logOn() || s_n.fetch_add(1u) < 64u)
                std::fprintf(stderr, "[cttriop] audio cmd %u: %08x %08x %08x %08x %08x\n", type, a, b, rd32(rdram, pkt + 0x18u), d,
                             rd32(rdram, pkt + 0x20u));
            return true;
        }
        default:
            std::fprintf(stderr, "[cttriop] unknown cmd type %u: %08x %08x %08x %08x\n", type, a, b, rd32(rdram, pkt + 0x18u), d);
            return true;
        }
        push(std::move(r));
        return true;
    }
}
