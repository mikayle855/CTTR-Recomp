#pragma once
// [postfx] Present-time post-processing for the PC port: FXAA, adaptive sharpening (CAS-style) and colour
// controls. Applied to the finished frame in the single present blit (ps2_runtime.cpp), so it covers every
// renderer and every scene. Settings live in savedata/settings.toml ([video] fxaa, sharpen, brightness,
// contrast, saturation, gamma) and the Shift+Tab overlay; all neutral = the old plain blit, bit for bit.
#include "raylib.h"

namespace ps2x_postfx
{
    struct Settings
    {
        bool fxaa = false;
        int sharpen = 0;      // 0..100 %
        int brightness = 0;   // -50..50 (added, in % of full scale)
        int contrast = 100;   // 50..150 %
        int saturation = 100; // 0..200 %
        int gamma = 100;      // 50..200 (% of 1.0; >100 brightens the mid-tones)
    };

    void set(const Settings &s);   // any thread; picked up at the next present
    Settings get();
    bool active();                 // anything non-neutral

    // Render thread, inside BeginDrawing with the caller's blend mode set. Draws tex(src) into dst with the
    // effects; returns false (and draws nothing) when inactive or the shaders failed, so the caller does its
    // plain DrawTexturePro.
    bool draw(const Texture2D &tex, Rectangle src, Rectangle dst);
}
