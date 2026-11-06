#pragma once

// SPU2 sound core as libsd drives it: 2 cores x 24 voices playing ADPCM from 2 MB of sound RAM, the core
// registers libsd exposes (sceSdSetParam / SetSwitch / SetAddr) and its voice transfers. The CTTR IOP driver's
// block-transfer streams (Syscalls/CttrIop.cpp) and the game's own AUDIO.IRX (run by Iop/IopModuleVm.cpp) both
// end up here, and the host output callback renders it.
//
// Entry encoding is libsd's: bit 0 = core, bits 1-5 = voice, bits 8+ = register; master registers set bit 7,
// voice address registers bit 6.

#include <cstdint>
#include <string>

namespace ps2_spu2
{
    void reset();

    void setParam(uint16_t entry, uint16_t value);
    uint16_t getParam(uint16_t entry);
    void setSwitch(uint16_t entry, uint32_t value);
    uint32_t getSwitch(uint16_t entry);
    void setAddr(uint16_t entry, uint32_t value);
    uint32_t getAddr(uint16_t entry);

    // sceSdVoiceTrans: mode bit 0 = 1 reads SPU2 RAM into IOP RAM, 0 writes. Returns the bytes moved.
    uint32_t voiceTrans(uint8_t *iopRam, uint32_t iopRamSize, uint16_t mode, uint32_t iopAddr, uint32_t spuAddr, uint32_t size);

    // The 2 MB of sound RAM (for debug dumps).
    const uint8_t *soundRam();

    // Adds `frames` 48 kHz stereo frames of each core's dry voice output (before the master volume) to
    // core0[2*frames] and core1[2*frames], as float samples in int16 units.
    void renderVoices(uint32_t frames, float *core0, float *core1);

    // "[spu2] KON n, voices playing n/n, voice transfers n KB" since the last call (for the [iopvm] status line).
    std::string statusLine();

    // Core volumes for the output stage (SPU2 register values).
    uint16_t masterVolume(int core, int side);
    uint16_t inputVolume(int core, int side);
}
