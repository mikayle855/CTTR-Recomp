#include "Spu2.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace ps2_spu2
{
    namespace
    {
        constexpr uint32_t kRamSize = 2u * 1024u * 1024u;
        constexpr int kVoices = 24;

        // libsd register numbers (entry >> 8)
        enum : uint32_t
        {
            VP_VOLL = 0x00, VP_VOLR = 0x01, VP_PITCH = 0x02, VP_ADSR1 = 0x03, VP_ADSR2 = 0x04, VP_ENVX = 0x05,
            VP_VOLXL = 0x06, VP_VOLXR = 0x07,
            P_MMIX = 0x08, P_MVOLL = 0x09, P_MVOLR = 0x0a, P_EVOLL = 0x0b, P_EVOLR = 0x0c, P_AVOLL = 0x0d,
            P_AVOLR = 0x0e, P_BVOLL = 0x0f, P_BVOLR = 0x10, P_MVOLXL = 0x11, P_MVOLXR = 0x12,
            S_PMON = 0x13, S_NON = 0x14, S_KON = 0x15, S_KOFF = 0x16, S_ENDX = 0x17, S_VMIXL = 0x18,
            S_VMIXEL = 0x19, S_VMIXR = 0x1a, S_VMIXER = 0x1b,
            A_ESA = 0x1c, A_EEA = 0x1d, A_TSA = 0x1e, A_IRQA = 0x1f,
            VA_SSA = 0x20, VA_LSAX = 0x21, VA_NAX = 0x22,
        };

        enum class Phase : uint8_t { Off, Attack, Decay, Sustain, Release };

        struct Voice
        {
            uint16_t volL = 0, volR = 0, pitch = 0, adsr1 = 0, adsr2 = 0;
            uint32_t ssa = 0, lsa = 0, nax = 0;   // byte addresses
            bool lsaSet = false;                  // LSAX written by software since key-on
            Phase phase = Phase::Off;
            int32_t env = 0;                      // 0..0x7fff
            uint32_t envCounter = 0;
            uint32_t counter = 0;                 // pitch accumulator, 12-bit fraction
            int16_t block[28] = {};
            int blockPos = 28;                    // next sample of `block`; 28 = decode the next block
            int32_t prev1 = 0, prev2 = 0;         // ADPCM history
            int16_t hist[4] = {};                 // last output samples, newest last (interpolation)
            uint8_t blockFlags = 0;
            bool stopAfterBlock = false;          // the block just decoded ended the sample without a repeat
            uint32_t blocksSinceKon = 0;          // [spu2log]
            uint64_t lsaxAt = 0;                  // [spu2log] g_frames when LSAX was last written, and to what
            uint32_t lsaxValue = 0;
            float peakRaw = 0.0f;                 // [spu2voices] peak decoded sample since the last status line
        };

        struct Core
        {
            Voice v[kVoices];
            uint16_t mmix = 0x0ff0, mvol[2] = {0x3fff, 0x3fff}, evol[2] = {0, 0}, avol[2] = {0, 0}, bvol[2] = {0x3fff, 0x3fff};
            uint32_t endx = 0, vmixl = 0xffffff, vmixr = 0xffffff, vmixel = 0, vmixer = 0, pmon = 0, non = 0;
            uint32_t esa = 0, eea = 0, tsa = 0, irqa = 0;
        };

        std::mutex g_m;
        Core g_core[2];
        std::vector<uint8_t> g_ram(kRamSize, 0);
        uint32_t g_konCount = 0;          // [spu2] status line counters, under g_m
        uint32_t g_staleLoops = 0;
        uint32_t g_staleLogged = 0;
        uint64_t g_transBytes = 0;
        uint64_t g_frames = 0;            // [spu2log] 48 kHz frames rendered
        uint32_t g_jumpLogged = 0;

        bool logOn()
        {
            static const bool s_on = [](){ const char *v = std::getenv("PS2X_SPU2LOG"); return v && v[0] && v[0] != '0'; }();
            return s_on;
        }

        // ---- ADSR ------------------------------------------------------------------------------------------------
        // One envelope step at `rate` (0..0x7f), as the PS1/PS2 SPU does: step and period from the rate's low/high bits;
        // exponential increase slows above 0x6000, exponential decrease scales the step by the level.
        void envStep(Voice &v, uint32_t rate, bool decrease, bool exponential)
        {
            const uint32_t shift = rate >> 2;
            int32_t step = decrease ? -8 + int32_t(rate & 3) : 7 - int32_t(rate & 3);
            uint32_t period = 1;
            if (shift < 11)
                step <<= (11 - shift);
            else
                period = 1u << (shift - 11);
            if (exponential && !decrease && v.env > 0x6000)
                period *= 4;
            if (exponential && decrease)
                step = (step * v.env) >> 15;
            if (++v.envCounter < period)
                return;
            v.envCounter = 0;
            v.env = std::clamp(v.env + step, 0, 0x7fff);
        }

        void envTick(Voice &v)
        {
            switch (v.phase)
            {
            case Phase::Attack:
                envStep(v, (v.adsr1 >> 8) & 0x7f, false, (v.adsr1 & 0x8000) != 0);
                if (v.env >= 0x7fff) { v.env = 0x7fff; v.phase = Phase::Decay; v.envCounter = 0; }
                break;
            case Phase::Decay:
            {
                envStep(v, ((v.adsr1 >> 4) & 0xf) << 2, true, true);
                const int32_t sustain = std::min<int32_t>(0x7fff, (int32_t(v.adsr1 & 0xf) + 1) * 0x800);
                if (v.env <= sustain) { v.phase = Phase::Sustain; v.envCounter = 0; }
                break;
            }
            case Phase::Sustain:
                envStep(v, (v.adsr2 >> 6) & 0x7f, (v.adsr2 & 0x4000) != 0, (v.adsr2 & 0x8000) != 0);
                break;
            case Phase::Release:
                envStep(v, (v.adsr2 & 0x1f) << 2, true, (v.adsr2 & 0x20) != 0);
                if (v.env <= 0) { v.env = 0; v.phase = Phase::Off; }
                break;
            case Phase::Off:
                break;
            }
        }

        // ---- ADPCM ----------------------------------------------------------------------------------------------
        void decodeBlock(Core &c, int voiceIdx, Voice &v)
        {
            static const int32_t f0[5] = {0, 60, 115, 98, 122};
            static const int32_t f1[5] = {0, 0, -52, -55, -60};
            const uint32_t a = v.nax & (kRamSize - 1) & ~15u;
            const uint8_t *b = &g_ram[a];
            const uint8_t header = b[0];
            v.blockFlags = b[1];
            int shift = header & 0xf;
            if (shift > 12)
                shift = 9;
            const int filter = std::min<int>((header >> 4) & 0x7, 4);
            if ((v.blockFlags & 4) && !v.lsaSet)
            {   // loop start -- unless software wrote LSAX since key-on: that address sticks (as the SPU2 and
                // PCSX2's LoopMode do). AUDIO.IRX points LSAX at the next page while clip data carries its own
                // loop-start flags; letting those win dragged voices back into short loops (a constant ringing).
                v.lsa = a;
            }
            for (int i = 0; i < 28; ++i)
            {
                const int nib = (b[2 + i / 2] >> ((i & 1) * 4)) & 0xf;
                int32_t s = int32_t(int16_t(uint16_t(nib << 12))) >> shift;
                s += (v.prev1 * f0[filter] + v.prev2 * f1[filter] + 32) >> 6;
                s = std::clamp(s, -32768, 32767);
                v.prev2 = v.prev1;
                v.prev1 = s;
                v.block[i] = int16_t(s);
            }
            v.blockPos = 0;
            // Advance to the next block now; the end/loop flags take effect after this block's samples.
            ++v.blocksSinceKon;
            if (v.blockFlags & 1)
            {
                if (logOn() && !(c.endx & (1u << voiceIdx)))
                    std::fprintf(stderr, "[spu2] core%d voice %d ENDX at 0x%x flags %x (ssa 0x%x lsa 0x%x, %u blocks since KON)\n",
                                 int(&c - g_core), voiceIdx, a, v.blockFlags, v.ssa, v.lsa, v.blocksSinceKon);
                c.endx |= 1u << voiceIdx;
                // [staleloop] a looping end block whose loop address still points into the chunk just played: the
                // driver did not move LSAX on in time, so the voice replays old data (heard as a stutter)
                if ((v.blockFlags & 2) && v.lsa <= a && a - v.lsa < 0x1000u)
                {
                    if (logOn() && g_staleLogged < 3000u && !(v.lsa >= 0x6000u && v.lsa < 0x6800u) && a - v.lsa > 0x40u)
                    {
                        ++g_staleLogged;
                        std::fprintf(stderr, "[spu2] stale loop core%d voice %d end 0x%x -> lsa 0x%x (ssa 0x%x, %u blocks since KON, pitch 0x%x)\n",
                                     int(&c - g_core), voiceIdx, a, v.lsa, v.ssa, v.blocksSinceKon, v.pitch);
                    }
                    // [staleloop] count only real replays: a voice parked on AUDIO.IRX's silence page loops there by design
                    if (!(v.lsa >= 0x6000u && v.lsa < 0x6800u))
                        ++g_staleLoops;
                }
                if (logOn() && g_jumpLogged < 6000u && !(a >= 0x6000u && a < 0x6800u))
                {   // [spu2log] every page end of a voice outside the silence: where it goes and how fresh LSAX is
                    ++g_jumpLogged;
                    std::fprintf(stderr, "[spu2] jump core%d voice %d t=%llu end 0x%x -> 0x%x%s (LSAX 0x%x written %lld frames ago, %u blocks since KON, pitch 0x%x, vol %04x/%04x env %04x mix %d%d)\n",
                                 int(&c - g_core), voiceIdx, (unsigned long long)g_frames, a, v.lsa,
                                 (v.lsa >= 0x6000u && v.lsa < 0x6800u) ? " SILENCE" : (v.lsa <= a && a - v.lsa < 0x1000u) ? " REPEAT" : "",
                                 v.lsaxValue, (long long)(g_frames - v.lsaxAt), v.blocksSinceKon, v.pitch, v.volL, v.volR, unsigned(v.env), int((c.vmixl >> voiceIdx) & 1), int((c.vmixr >> voiceIdx) & 1));
                }
                if (v.blockFlags & 2)
                    v.nax = v.lsa;
                else
                {   // end without repeat: the voice goes silent once this block has played
                    v.nax = v.lsa;
                    v.stopAfterBlock = true;
                }
            }
            else
                v.nax = (a + 16) & (kRamSize - 1);
        }

        int16_t nextSample(Core &c, int voiceIdx, Voice &v)
        {
            if (v.blockPos >= 28)
            {
                if (v.stopAfterBlock)
                {
                    v.stopAfterBlock = false;
                    v.phase = Phase::Off;
                    v.env = 0;
                    return 0;
                }
                decodeBlock(c, voiceIdx, v);
            }
            return v.block[v.blockPos++];
        }

        float volume(uint16_t v)
        {
            if (v & 0x8000)
                return 1.0f;   // sweep modes are not modelled: full scale
            return float(int16_t(uint16_t(v << 1))) / 32768.0f;
        }

        void keyOn(Core &c, int idx)
        {
            Voice &v = c.v[idx];
            v.nax = v.ssa;
            if (!v.lsaSet)
                v.lsa = v.ssa;
            v.lsaSet = false;
            v.phase = Phase::Attack;
            v.env = 0;
            v.envCounter = 0;
            v.counter = 0;
            v.blockPos = 28;
            v.stopAfterBlock = false;
            v.blocksSinceKon = 0;
            v.prev1 = v.prev2 = 0;
            std::memset(v.hist, 0, sizeof v.hist);
            c.endx &= ~(1u << idx);
        }
    }

    void reset()
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_core[0] = Core{};
        g_core[1] = Core{};
        std::fill(g_ram.begin(), g_ram.end(), 0);
    }

    void setParam(uint16_t entry, uint16_t value)
    {
        std::lock_guard<std::mutex> lock(g_m);
        Core &c = g_core[entry & 1];
        const uint32_t reg = entry >> 8;
        if (entry & 0x80)
        {
            switch (reg)
            {
            case P_MMIX: c.mmix = value; break;
            case P_MVOLL: c.mvol[0] = value; break;
            case P_MVOLR: c.mvol[1] = value; break;
            case P_EVOLL: c.evol[0] = value; break;
            case P_EVOLR: c.evol[1] = value; break;
            case P_AVOLL: c.avol[0] = value; break;
            case P_AVOLR: c.avol[1] = value; break;
            case P_BVOLL: c.bvol[0] = value; break;
            case P_BVOLR: c.bvol[1] = value; break;
            default: break;
            }
            return;
        }
        Voice &v = c.v[((entry >> 1) & 0x1f) % kVoices];
        switch (reg)
        {
        case VP_VOLL: v.volL = value; break;
        case VP_VOLR: v.volR = value; break;
        case VP_PITCH: v.pitch = value & 0x3fff; break;
        case VP_ADSR1: v.adsr1 = value; break;
        case VP_ADSR2: v.adsr2 = value; break;
        case VP_ENVX: v.env = value & 0x7fff; break;
        default: break;
        }
    }

    uint16_t getParam(uint16_t entry)
    {
        std::lock_guard<std::mutex> lock(g_m);
        Core &c = g_core[entry & 1];
        const uint32_t reg = entry >> 8;
        if (entry & 0x80)
        {
            switch (reg)
            {
            case P_MMIX: return c.mmix;
            case P_MVOLL: case P_MVOLXL: return c.mvol[0];
            case P_MVOLR: case P_MVOLXR: return c.mvol[1];
            case P_BVOLL: return c.bvol[0];
            case P_BVOLR: return c.bvol[1];
            default: return 0;
            }
        }
        const Voice &v = c.v[((entry >> 1) & 0x1f) % kVoices];
        switch (reg)
        {
        case VP_VOLL: case VP_VOLXL: return v.volL;
        case VP_VOLR: case VP_VOLXR: return v.volR;
        case VP_PITCH: return v.pitch;
        case VP_ADSR1: return v.adsr1;
        case VP_ADSR2: return v.adsr2;
        case VP_ENVX: return uint16_t(v.env);
        default: return 0;
        }
    }

    void setSwitch(uint16_t entry, uint32_t value)
    {
        std::lock_guard<std::mutex> lock(g_m);
        Core &c = g_core[entry & 1];
        value &= 0xffffff;
        switch (entry >> 8)
        {
        case S_PMON: c.pmon = value; break;
        case S_NON: c.non = value; break;
        case S_KON:
            for (int i = 0; i < kVoices; ++i)
                if (value & (1u << i))
                {
                    keyOn(c, i);
                    ++g_konCount;
                }
            if (logOn() && value)
                for (int i = 0; i < kVoices; ++i)
                    if (value & (1u << i))
                        std::fprintf(stderr, "[spu2] core%d KON voice %d t=%llu ssa 0x%x pitch 0x%x\n", entry & 1, i, (unsigned long long)g_frames, c.v[i].ssa, c.v[i].pitch);
            break;
        case S_KOFF:
            for (int i = 0; i < kVoices; ++i)
                if ((value & (1u << i)) && c.v[i].phase != Phase::Off)
                {
                    c.v[i].phase = Phase::Release;
                    c.v[i].envCounter = 0;
                }
            break;
        case S_ENDX:   // a write clears the flags
            if (logOn()) std::fprintf(stderr, "[spu2] core%d ENDX write %06x (was %06x)\n", entry & 1, value, c.endx);
            c.endx = 0;
            break;
        case S_VMIXL: c.vmixl = value; break;
        case S_VMIXEL: c.vmixel = value; break;
        case S_VMIXR: c.vmixr = value; break;
        case S_VMIXER: c.vmixer = value; break;
        default: break;
        }
    }

    uint32_t getSwitch(uint16_t entry)
    {
        std::lock_guard<std::mutex> lock(g_m);
        const Core &c = g_core[entry & 1];
        switch (entry >> 8)
        {
        case S_PMON: return c.pmon;
        case S_NON: return c.non;
        case S_ENDX:
            if (logOn() && c.endx) { static uint32_t s_n = 0; if (++s_n <= 200) std::fprintf(stderr, "[spu2] core%d ENDX read %06x\n", entry & 1, c.endx); }
            return c.endx;
        case S_VMIXL: return c.vmixl;
        case S_VMIXEL: return c.vmixel;
        case S_VMIXR: return c.vmixr;
        case S_VMIXER: return c.vmixer;
        default: return 0;
        }
    }

    void setAddr(uint16_t entry, uint32_t value)
    {
        std::lock_guard<std::mutex> lock(g_m);
        Core &c = g_core[entry & 1];
        value &= kRamSize - 1;
        if (logOn() && (entry & 1) == 0 && ((entry >> 1) & 0x1f) == 0 && (entry >> 8) >= VA_SSA)   // [spu2log] core 0 voice 0
            std::fprintf(stderr, "[spu2] core0 voice 0 %s = 0x%x\n", (entry >> 8) == VA_SSA ? "SSA" : (entry >> 8) == VA_LSAX ? "LSAX" : "NAX", value);
        switch (entry >> 8)
        {
        case A_ESA: c.esa = value; break;
        case A_EEA: c.eea = value; break;
        case A_TSA: c.tsa = value; break;
        case A_IRQA: c.irqa = value; break;
        case VA_SSA: c.v[((entry >> 1) & 0x1f) % kVoices].ssa = value & ~15u; break;
        case VA_LSAX:
        {
            Voice &v = c.v[((entry >> 1) & 0x1f) % kVoices];
            v.lsa = value & ~15u;
            v.lsaSet = true;
            v.lsaxAt = g_frames;
            v.lsaxValue = v.lsa;
            break;
        }
        case VA_NAX: c.v[((entry >> 1) & 0x1f) % kVoices].nax = value & ~15u; break;
        default: break;
        }
    }

    uint32_t getAddr(uint16_t entry)
    {
        std::lock_guard<std::mutex> lock(g_m);
        const Core &c = g_core[entry & 1];
        switch (entry >> 8)
        {
        case A_ESA: return c.esa;
        case A_EEA: return c.eea;
        case A_TSA: return c.tsa;
        case A_IRQA: return c.irqa;
        case VA_SSA: return c.v[((entry >> 1) & 0x1f) % kVoices].ssa;
        case VA_LSAX: return c.v[((entry >> 1) & 0x1f) % kVoices].lsa;
        case VA_NAX: return c.v[((entry >> 1) & 0x1f) % kVoices].nax;
        default: return 0;
        }
    }

    const uint8_t *soundRam() { return g_ram.data(); }

    uint32_t voiceTrans(uint8_t *iopRam, uint32_t iopRamSize, uint16_t mode, uint32_t iopAddr, uint32_t spuAddr, uint32_t size)
    {
        std::lock_guard<std::mutex> lock(g_m);
        iopAddr &= 0x1fffff;
        spuAddr &= kRamSize - 1;
        if (!iopRam || iopAddr >= iopRamSize)
            return 0;
        size = std::min({size, iopRamSize - iopAddr, kRamSize - spuAddr});
        if (mode & 1)
            std::memcpy(iopRam + iopAddr, &g_ram[spuAddr], size);
        else
            std::memcpy(&g_ram[spuAddr], iopRam + iopAddr, size);
        g_transBytes += size;
        if (logOn())
            std::fprintf(stderr, "[spu2] voice trans %s %u bytes iop 0x%x spu 0x%x\n", (mode & 1) ? "read" : "write", size, iopAddr, spuAddr);
        return size;
    }

    void renderVoices(uint32_t frames, float *core0, float *core1)
    {
        std::lock_guard<std::mutex> lock(g_m);
        float *out[2] = {core0, core1};
        for (int ci = 0; ci < 2; ++ci)
        {
            Core &c = g_core[ci];
            const bool dryL = (c.mmix & 0x800) != 0, dryR = (c.mmix & 0x400) != 0;   // MSNDL / MSNDR
            for (int vi = 0; vi < kVoices; ++vi)
            {
                Voice &v = c.v[vi];
                if (v.phase == Phase::Off)
                    continue;
                const float gl = (dryL && (c.vmixl & (1u << vi))) ? volume(v.volL) : 0.0f;
                const float gr = (dryR && (c.vmixr & (1u << vi))) ? volume(v.volR) : 0.0f;
                for (uint32_t f = 0; f < frames && v.phase != Phase::Off; ++f)
                {
                    v.counter += v.pitch;
                    while (v.counter >= 0x1000)
                    {
                        v.counter -= 0x1000;
                        v.hist[0] = v.hist[1]; v.hist[1] = v.hist[2]; v.hist[2] = v.hist[3];
                        v.hist[3] = nextSample(c, vi, v);
                    }
                    // 4-point interpolation between hist[1] and hist[2] (the SPU2 uses a Gaussian table; cubic here)
                    const float t = float(v.counter) / 4096.0f;
                    const float p0 = v.hist[0], p1 = v.hist[1], p2 = v.hist[2], p3 = v.hist[3];
                    const float s = p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
                    const float e = float(v.env) / 32768.0f;
                    v.peakRaw = std::max(v.peakRaw, std::fabs(s));
                    out[ci][f * 2] += s * e * gl;
                    out[ci][f * 2 + 1] += s * e * gr;
                    envTick(v);
                }
            }
        }
        g_frames += frames;
    }

    std::string statusLine()
    {
        std::lock_guard<std::mutex> lock(g_m);
        int playing[2] = {0, 0};
        for (int c = 0; c < 2; ++c)
            for (const Voice &v : g_core[c].v)
                playing[c] += v.phase != Phase::Off ? 1 : 0;
        char b[160];
        std::snprintf(b, sizeof b, "[spu2] KON %u, voices playing %d/%d, voice transfers %llu KB, stale loops %u", g_konCount, playing[0], playing[1],
                      (unsigned long long)(g_transBytes / 1024), g_staleLoops);
        g_konCount = 0;
        g_staleLoops = 0;
        g_transBytes = 0;
        std::string s = b;
        static const bool s_voices = [](){ const char *e = std::getenv("PS2X_SPU2VOICES"); return e && e[0] && e[0] != '0'; }();
        if (s_voices)
        {   // [spu2voices] every keyed voice: where it plays, its envelope and volumes (who is actually audible)
            for (int ci = 0; ci < 2; ++ci)
                for (int vi = 0; vi < kVoices; ++vi)
                {
                    Voice &v = g_core[ci].v[vi];
                    if (v.phase == Phase::Off)
                        continue;
                    char l[192];
                    std::snprintf(l, sizeof l, "\n[spu2voices] t=%llus core%d v%-2d nax 0x%05x lsa 0x%05x pitch 0x%04x env %04x ph %d vol %04x/%04x mix %d%d peak %d",
                                  (unsigned long long)(g_frames / 48000), ci, vi, v.nax, v.lsa, v.pitch, unsigned(v.env), int(v.phase),
                                  v.volL, v.volR, int((g_core[ci].vmixl >> vi) & 1), int((g_core[ci].vmixr >> vi) & 1), int(v.peakRaw));
                    v.peakRaw = 0.0f;
                    s += l;
                }
        }
        return s;
    }

    uint16_t masterVolume(int core, int side)
    {
        std::lock_guard<std::mutex> lock(g_m);
        return g_core[core & 1].mvol[side & 1];
    }

    uint16_t inputVolume(int core, int side)
    {
        std::lock_guard<std::mutex> lock(g_m);
        return g_core[core & 1].bvol[side & 1];
    }
}
