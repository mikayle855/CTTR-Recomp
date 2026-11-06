// [native_gs] The runtime's side of the renderer transplanted from the Tenkaichi3Decomp PC port (native_gs_glue.cpp).
#pragma once
#include <cstddef>
#include <cstdint>

bool NativeGs_Enabled();
void NativeGs_SetGuestVram(const uint8_t *vram);   // diagnostics: our GS memory (4 MB)
// Main thread, with raylib's GL context current: makes the renderer's shared context and starts its thread when
// PS2X_NATIVE_GS is set. False = off.
bool NativeGs_CreateContext();
// Any thread, in stream order: the VIF1 stream of one DMA transfer (chain = a whole frame's list: its end ends the
// frame), data for the GIF channel (path 3), and a DISPFB1 write (which frame buffer is shown).
void NativeGs_PushVif1(const uint8_t *data, size_t bytes, bool chain);
void NativeGs_PushGif(const uint8_t *data, size_t bytes);
void NativeGs_PushDispFb(uint64_t dispfb1);
// sceGsExecStoreImage (the game reading a frame back for its cross-fade), in stream order: vram_addr as the game gave it
void NativeGs_PushStoreImage(uint32_t vramAddr, uint32_t w, uint32_t h);
// PS2X_NATIVE_GS=2: the runtime presents this renderer's picture. Main thread (raylib's context current): the newest
// finished frame (GL texture, its size, a sequence number that changes with every new frame); false = none yet. The
// texture holds the scene top row first (draw it without a vertical flip) and stays valid until a newer one is taken.
bool NativeGs_Presenting();
bool NativeGs_TakePresent(unsigned &tex, int &w, int &h, uint64_t &seq);
// While this renderer presents, our own VU1 programs only feed a GS nobody shows: skip running them
// (PS2X_NATIVE_GS_RUNVU1=1 keeps them).
bool NativeGs_SkipOurVu1();
// The renderer's settings (<exe dir>\savedata\native_renderer.txt; F1 in game). enabled = the next start uses this
// renderer (PS2X_NATIVE_GS overrides). Set applies the rest at once while it presents and saves the file.
struct NativeGsSettings
{
    bool enabled = false;
    int scale = 2;        // internal resolution 1..8 (x 512 x 448)
    int smooth = 1;       // in-between pictures: 0 off (30), 1 = 60, 2 = 120, 3 = 240 per second
    bool smooth2d = true; // 2D art filtering above 1x: smooth (true) or sharp blocks
    int fxOff = 4;        // effects switched off (bits): 1 outline, 2 see-through tint, 4 depth tint, 8 glare/glow,
                          // 16 distance blur, 32 movement blur (depth tint off: it blackens BT4's far scenery)
    int glow = 60;        // glow strength %
    int aniso = 1;        // anisotropic filtering: 1 off, 2/4/8/16 samples
};
void NativeGs_GetSettings(NativeGsSettings &s);
void NativeGs_SetSettings(const NativeGsSettings &s);
