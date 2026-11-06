// [postfx] FXAA + adaptive sharpening + colour controls on the present blit. See runtime/ps2_postfx.h.
//
// ONE pass, straight into the window: the shader samples the finished frame at its own resolution (texel
// uniform), so FXAA and the sharpen kernel work on source pixels while the blit scales to the window. No
// offscreen target -- raylib's EndTextureMode() rebuilds the projection without the high-DPI screen scale,
// which shrank and shifted every later draw on a 125 % Windows display.
#include "runtime/ps2_postfx.h"
#include "rlgl.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace ps2x_postfx
{
namespace
{
    std::mutex g_mx;
    Settings g_set;
    std::atomic<bool> g_active{false};

    bool neutral(const Settings &s)
    {
        return !s.fxaa && s.sharpen <= 0 && s.brightness == 0 && s.contrast == 100 && s.saturation == 100 && s.gamma == 100;
    }

    // raylib's default vertex shader feeds fragTexCoord / fragColor; texture0 is the drawn texture.
    const char *kFs = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec2 texel;        // 1 / source texture size
uniform float fxaaOn;      // 0 / 1
uniform float sharp;       // 0..1
uniform float bright;      // -0.5..0.5
uniform float contrast;    // 0.5..1.5
uniform float sat;         // 0..2
uniform float invGamma;    // 1/gamma
out vec4 finalColor;

const vec3 LUMA = vec3(0.299, 0.587, 0.114);
vec3 px(vec2 uv) { return texture(texture0, uv).rgb; }

// FXAA 3.x "lite": edge direction from the diagonal neighbours, two blends along it, keep the one that
// stays inside the local luma range.
vec3 fxaa(vec2 uv, vec3 rgbM)
{
    const float SPAN_MAX = 8.0, REDUCE_MUL = 1.0 / 8.0, REDUCE_MIN = 1.0 / 128.0;
    vec3 rgbNW = px(uv + vec2(-1.0, -1.0) * texel), rgbNE = px(uv + vec2(1.0, -1.0) * texel);
    vec3 rgbSW = px(uv + vec2(-1.0,  1.0) * texel), rgbSE = px(uv + vec2(1.0,  1.0) * texel);
    float lNW = dot(rgbNW, LUMA), lNE = dot(rgbNE, LUMA), lSW = dot(rgbSW, LUMA), lSE = dot(rgbSE, LUMA), lM = dot(rgbM, LUMA);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));
    float dirReduce = max((lNW + lNE + lSW + lSE) * (0.25 * REDUCE_MUL), REDUCE_MIN);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, vec2(-SPAN_MAX), vec2(SPAN_MAX)) * texel;
    vec3 rgbA = 0.5 * (px(uv + dir * (1.0 / 3.0 - 0.5)) + px(uv + dir * (2.0 / 3.0 - 0.5)));
    vec3 rgbB = rgbA * 0.5 + 0.25 * (px(uv - dir * 0.5) + px(uv + dir * 0.5));
    float lB = dot(rgbB, LUMA);
    return (lB < lMin || lB > lMax) ? rgbA : rgbB;
}

void main()
{
    vec2 uv = fragTexCoord;
    vec3 raw = px(uv);
    vec3 c = fxaaOn > 0.5 ? fxaa(uv, raw) : raw;
    if (sharp > 0.0)
    {   // contrast-adaptive sharpening (AMD CAS idea): less where the neighbourhood already has contrast, so
        // edges do not ring. The detail it adds is computed on the source and added to the (anti-aliased) colour.
        vec3 n = px(uv + vec2(0.0, -texel.y)), s = px(uv + vec2(0.0, texel.y));
        vec3 e = px(uv + vec2(texel.x, 0.0)),  w = px(uv + vec2(-texel.x, 0.0));
        vec3 mn = min(raw, min(min(n, s), min(e, w)));
        vec3 mx = max(raw, max(max(n, s), max(e, w)));
        vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0));
        vec3 wt = amp * (-1.0 / mix(8.0, 5.0, sharp));
        vec3 sharpened = clamp((raw + (n + s + e + w) * wt) / (1.0 + 4.0 * wt), 0.0, 1.0);
        c = clamp(c + (sharpened - raw), 0.0, 1.0);
    }
    c = c + bright;
    c = (c - 0.5) * contrast + 0.5;
    c = mix(vec3(dot(c, LUMA)), c, sat);
    c = pow(clamp(c, 0.0, 1.0), vec3(invGamma));
    finalColor = vec4(c, 1.0);
}
)";

    struct Gpu
    {
        bool tried = false, ok = false;
        Shader sh{};
        int texel = -1, fxaaOn = -1, sharp = -1, bright = -1, contrast = -1, sat = -1, invGamma = -1;
    } g_gpu;

    bool ensureGpu()
    {
        if (g_gpu.tried) return g_gpu.ok;
        g_gpu.tried = true;
        g_gpu.sh = LoadShaderFromMemory(nullptr, kFs);
        if (!IsShaderValid(g_gpu.sh) || g_gpu.sh.id == rlGetShaderIdDefault())
        {
            std::fprintf(stderr, "[postfx] shader compile failed -- effects disabled\n");
            return false;
        }
        g_gpu.texel = GetShaderLocation(g_gpu.sh, "texel");
        g_gpu.fxaaOn = GetShaderLocation(g_gpu.sh, "fxaaOn");
        g_gpu.sharp = GetShaderLocation(g_gpu.sh, "sharp");
        g_gpu.bright = GetShaderLocation(g_gpu.sh, "bright");
        g_gpu.contrast = GetShaderLocation(g_gpu.sh, "contrast");
        g_gpu.sat = GetShaderLocation(g_gpu.sh, "sat");
        g_gpu.invGamma = GetShaderLocation(g_gpu.sh, "invGamma");
        g_gpu.ok = true;
        std::fprintf(stderr, "[postfx] shaders ready\n");
        return true;
    }
}

void set(const Settings &s)
{
    std::lock_guard<std::mutex> lk(g_mx);
    g_set = s;
    g_active.store(!neutral(s), std::memory_order_relaxed);
}

Settings get()
{
    std::lock_guard<std::mutex> lk(g_mx);
    return g_set;
}

bool active() { return g_active.load(std::memory_order_relaxed); }

bool draw(const Texture2D &tex, Rectangle src, Rectangle dst)
{
    if (!active() || tex.id == 0) return false;
    if (!ensureGpu()) return false;
    const Settings s = get();

    const float texel[2] = { 1.0f / float(tex.width), 1.0f / float(tex.height) };
    const float fxaaOn = s.fxaa ? 1.0f : 0.0f;
    const float sharp = std::clamp(s.sharpen, 0, 100) / 100.0f;
    const float bright = std::clamp(s.brightness, -50, 50) / 100.0f;
    const float contrast = std::clamp(s.contrast, 50, 150) / 100.0f;
    const float sat = std::clamp(s.saturation, 0, 200) / 100.0f;
    const float invGamma = 100.0f / float(std::clamp(s.gamma, 50, 200));

    // PS2X_POSTFX_SPLIT=1 (A/B): the original frame everywhere, the processed one over the LEFT half only.
    static const bool s_split = [](){ const char *v = std::getenv("PS2X_POSTFX_SPLIT"); return v && v[0] == '1'; }();
    if (s_split)
    {
        DrawTexturePro(tex, src, dst, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
        src.width *= 0.5f;
        dst.width *= 0.5f;
    }
    // FXAA and the sharpen kernel take fractional-texel taps: with the POINT filter the present uses for upscales
    // those snap to whole texels and FXAA turns edges grainy. Bilinear for the filtered taps (the caller re-sets
    // the filter every present, so the plain path is untouched; a trilinear downscale keeps its mips).
    if ((s.fxaa || s.sharpen > 0) && tex.mipmaps <= 1)
        SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
    BeginShaderMode(g_gpu.sh);
    SetShaderValue(g_gpu.sh, g_gpu.texel, texel, SHADER_UNIFORM_VEC2);
    SetShaderValue(g_gpu.sh, g_gpu.fxaaOn, &fxaaOn, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_gpu.sh, g_gpu.sharp, &sharp, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_gpu.sh, g_gpu.bright, &bright, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_gpu.sh, g_gpu.contrast, &contrast, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_gpu.sh, g_gpu.sat, &sat, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_gpu.sh, g_gpu.invGamma, &invGamma, SHADER_UNIFORM_FLOAT);
    DrawTexturePro(tex, src, dst, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
    EndShaderMode();
    return true;
}
}
