#ifdef PS2X_HAVE_NATIVE_GS
#include "native_gs.h"   // [native_gs] the F1 window
#endif
#include "ps2_runtime.h"   // [fps60] ps2Set60Fps
#include "runtime/ps2_postfx.h"   // [postfx]
#include "runtime/ps2_perfhud.h"   // [perfhud]
#include "ps2_settings_overlay.h"
#include "runtime/ps2_netplay.h"   // [netplay]
#include "runtime/ps2_gs_pgs.h"   // [pgsink] backend ink width
#include "runtime/ps2_gs_gpu_renderer.h"
#include "runtime/ps2_render_scale.h"
#include "runtime/ps2_audio.h"
#include "runtime/pad_config.h"
#include "runtime/ps2_host_pad.h"
#if defined(__linux__)
#include "runtime/pad_evdev_linux.h"
#endif

#include "imgui.h"
#include "rlImGui.h"
#include "raylib.h"

#include "runtime/ps2_toml.h"

#include <fstream>
#include <sstream>
#include <filesystem>
#include <atomic>
#include <mutex>
#include <thread>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

static const char *kConfigFileName = "settings.toml";        // launcher + overlay + FMV share this
static const char *kLegacyConfigFileName = "bt3_settings.ini"; // 0.x format, migrated on first load
extern bool g_ps2xIsCttr;   // [gamegate] ps2_runtime.cpp
static const char *kDumpFileName = "bt3_settings_dump.log";

static const char *kConfigHeader =
    "# Crash Tag Team Racing - PC port\n"
    "# User settings - written by the launcher and the in-game overlay.\n"
    "# Delete this file to reset everything to defaults.\n\n";

namespace
{
    // --- settings.toml helpers -----------------------------------------------
    const char *rendererName(int r)
    {
        switch (r) { case 0: return "opengl"; case 1: return "software"; case 2: return "parallel-gs"; default: return "parallel-gs"; }
    }
    int nameToRenderer(const std::string &s, int def)
    {
        if (s == "opengl" || s == "gl") return 0;
        if (s == "software" || s == "sw") return 1;
        if (s == "parallel-gs" || s == "parallel_gs" || s == "pgs") return 2;
        return def;
    }
    std::string colorToHex(unsigned c)
    {
        char b[8]; std::snprintf(b, sizeof b, "#%06x", c & 0xFFFFFFu); return b;
    }
    unsigned hexToColor(const std::string &s, unsigned def)
    {
        if (s.empty()) return def;
        try { return static_cast<unsigned>(std::stoul(s[0] == '#' ? s.substr(1) : s, nullptr, 16)) & 0xFFFFFFu; }
        catch (...) { return def; }
    }
} // namespace

// Deploy/retract animation timing for the overlay window (see PS2SettingsOverlay::draw).
static constexpr float kOverlayAnimDuration = 0.16f; // seconds, open or close

// Ease-out cubic: fast start, gentle settle. Used both ways (open + close) so the
// deploy and retract read as mirror images of the same motion.
static float overlayAnimEase(float t)
{
    t = std::max(0.0f, std::min(1.0f, t));
    const float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

// RAII guard for ImGui style pushes: pops exactly what it pushed on destruction,
// even across exceptions/early returns — this is what prevents the overlay from
// corrupting ImGui's style stack and crashing.
struct ScopedStyleColor
{
    ScopedStyleColor(ImGuiCol idx, const ImVec4 &col) { ImGui::PushStyleColor(idx, col); }
    ScopedStyleColor(ImGuiCol idx, ImU32 col) { ImGui::PushStyleColor(idx, col); }
    ~ScopedStyleColor() { ImGui::PopStyleColor(); }
};

struct ScopedStyleVar
{
    ScopedStyleVar(ImGuiStyleVar idx, float v) { ImGui::PushStyleVar(idx, v); }
    ScopedStyleVar(ImGuiStyleVar idx, const ImVec2 &v) { ImGui::PushStyleVar(idx, v); }
    ~ScopedStyleVar() { ImGui::PopStyleVar(); }
};

namespace
{
    std::string nowTimestamp()
    {
        const std::time_t t = std::time(nullptr);
        char buf[64];
        std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        return buf;
    }

    // True when none of the given buttons are currently held.
    bool allButtonsReleased(const std::array<uint8_t, 32> &curBtnDown,
                            const std::vector<int> &btns)
    {
        for (int b : btns)
            if (b >= 0 && b < 32 && curBtnDown[b]) return false;
        return true;
    }

    // True when none of the given keys are currently held.
    bool allKeysReleased(const std::vector<int> &keys)
    {
        for (int k : keys)
            if (IsKeyDown(k)) return false;
        return true;
    }
}

// --- "Capsule HUD" accent palette -------------------------------------------
// Dark near-black tech panel with a single orange accent (still Dragon Ball Z's
// orange/gold, just applied in a flatter, squared-off, HUD-readout style instead
// of the earlier rounded "energy glow" look).
namespace
{
    constexpr float DBZ_R = 1.00f, DBZ_G = 0.62f, DBZ_B = 0.10f;   // HUD orange
    constexpr float GOLD_R = 1.00f, GOLD_G = 0.80f, GOLD_B = 0.30f;

    ImVec4 dbz(float r, float g, float b, float a = 1.0f)
    {
        return ImVec4(r, g, b, a);
    }
    ImVec4 accent(float a = 1.0f) { return dbz(DBZ_R, DBZ_G, DBZ_B, a); }
    ImVec4 gold(float a = 1.0f)   { return dbz(GOLD_R, GOLD_G, GOLD_B, a); }

    void pushDbzTheme()
    {
        ImGuiStyle &s = ImGui::GetStyle();
        s.WindowPadding    = ImVec2(14, 10);
        s.FramePadding     = ImVec2(6, 4);
        s.ItemSpacing      = ImVec2(8, 6);
        s.ItemInnerSpacing = ImVec2(5, 4);
        s.ScrollbarSize    = 12.0f;
        // Capsule HUD: squared-off panel, thin border, no soft rounding anywhere —
        // reads as a technical readout rather than a glowing energy aura.
        s.WindowRounding   = 2.0f;
        s.FrameRounding    = 2.0f;
        s.GrabRounding     = 1.0f;
        s.TabRounding      = 0.0f;
        s.ScrollbarRounding= 2.0f;
        s.WindowBorderSize = 1.0f;
        s.FrameBorderSize  = 1.0f;
        s.TabBarBorderSize = 1.0f;
        s.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    }

    // RAII scope for the whole DBZ theme: pushes every style colour and pops exactly
    // that many on destruction, so the style stack can never leak (the previous code
    // pushed 40 but popped only 36, corrupting ImGui's stack and crashing the overlay).
    struct DbzThemeScope
    {
        static constexpr int kColors = 40;
        DbzThemeScope()
        {
            // Capsule HUD: near-black navy panel, thin orange edges, flat readout
            // rows instead of the previous purple-tinted "glow" surfaces.
            ImGui::PushStyleColor(ImGuiCol_WindowBg,            dbz(0.04f, 0.06f, 0.08f, 0.97f));
            ImGui::PushStyleColor(ImGuiCol_ChildBg,             dbz(0.06f, 0.08f, 0.10f, 0.60f));
            ImGui::PushStyleColor(ImGuiCol_PopupBg,             dbz(0.04f, 0.06f, 0.08f, 0.98f));
            ImGui::PushStyleColor(ImGuiCol_Border,              accent(0.55f));
            ImGui::PushStyleColor(ImGuiCol_BorderShadow,        dbz(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_TitleBg,             dbz(0.04f, 0.06f, 0.08f));
            ImGui::PushStyleColor(ImGuiCol_TitleBgActive,       dbz(0.04f, 0.06f, 0.08f));
            ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed,    dbz(0.04f, 0.06f, 0.08f));
            ImGui::PushStyleColor(ImGuiCol_Text,                dbz(0.84f, 0.89f, 0.92f));
            ImGui::PushStyleColor(ImGuiCol_TextDisabled,        dbz(0.29f, 0.39f, 0.44f));
            ImGui::PushStyleColor(ImGuiCol_TextSelectedBg,      accent(0.30f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg,             dbz(0.07f, 0.09f, 0.11f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,      dbz(0.10f, 0.13f, 0.15f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive,       dbz(1.00f, 0.62f, 0.10f, 0.20f));
            ImGui::PushStyleColor(ImGuiCol_SliderGrab,          accent());
            ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,    gold());
            ImGui::PushStyleColor(ImGuiCol_CheckMark,           accent());
            ImGui::PushStyleColor(ImGuiCol_Button,              dbz(0.07f, 0.09f, 0.11f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,       dbz(1.00f, 0.62f, 0.10f, 0.18f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,        dbz(1.00f, 0.62f, 0.10f, 0.30f));
            ImGui::PushStyleColor(ImGuiCol_Header,              dbz(0.09f, 0.11f, 0.13f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered,       accent(0.22f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive,        accent(0.35f));
            ImGui::PushStyleColor(ImGuiCol_Separator,           dbz(0.12f, 0.16f, 0.19f));
            ImGui::PushStyleColor(ImGuiCol_SeparatorHovered,    accent(0.55f));
            ImGui::PushStyleColor(ImGuiCol_SeparatorActive,     gold());
            // Flat "ghost" tabs with a thin orange border (TabBarBorderSize above)
            // instead of a filled rounded-pill active tab — reads as a HUD section
            // switcher rather than a browser-style tab strip.
            ImGui::PushStyleColor(ImGuiCol_Tab,                 dbz(0.04f, 0.06f, 0.08f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_TabHovered,          accent(0.20f));
            ImGui::PushStyleColor(ImGuiCol_TabSelected,         accent(0.14f));
            ImGui::PushStyleColor(ImGuiCol_TabDimmed,           dbz(0.04f, 0.06f, 0.08f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected,   accent(0.10f));
            ImGui::PushStyleColor(ImGuiCol_TableHeaderBg,       dbz(0.08f, 0.10f, 0.12f));
            ImGui::PushStyleColor(ImGuiCol_TableRowBg,          dbz(0.05f, 0.07f, 0.09f, 0.50f));
            ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt,       dbz(1.00f, 0.62f, 0.10f, 0.04f));
            ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,         dbz(0.04f, 0.06f, 0.08f, 0.60f));
            ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,       accent(0.45f));
            ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered,accent(0.70f));
            ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, gold());
            ImGui::PushStyleColor(ImGuiCol_PlotLines,           accent());
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram,       accent());
        }
        ~DbzThemeScope() { ImGui::PopStyleColor(kColors); }
        DbzThemeScope(const DbzThemeScope &) = delete;
        DbzThemeScope &operator=(const DbzThemeScope &) = delete;
    };

    // --- Capsule HUD font loading -------------------------------------------
    // Resolved font path lives in a file-scope static so loadOverlayFonts()
    // (called from initialize()) can read it without user-data arguments.
    std::string s_hudFontPath;
    bool s_fontLoaded = false;
    ImFont *s_fontSemibold = nullptr;   // [cttrmenu] headings / navigation
    ImFont *s_fontBlack = nullptr;      // [cttrmenu] the window title

    void loadOverlayFonts()
    {
        ImGuiIO &io = ImGui::GetIO();
        // Wipe the atlas built by rlImGui (default font + FontAwesome) and
        // replace it with a single Russo One face.  Because Russo One becomes
        // Fonts[0] (the ImGui "default" font), every piece of text in the
        // overlay — title, tabs, section headers, body — renders in it
        // automatically without any PushFont calls.
        io.Fonts->Clear();
        // [cttrmenu] Segoe UI (Windows) for the menu text when the bundled face is missing, plus Semibold for headings
        // and Black for the title; any that is missing falls back to the default font.
        auto winFont = [](const char *file) -> std::string {
            const char *windir = std::getenv("WINDIR");
            std::filesystem::path p = std::filesystem::path(windir && *windir ? windir : "C:\\Windows") / "Fonts" / file;
            std::error_code ec;
            return std::filesystem::is_regular_file(p, ec) ? p.string() : std::string();
        };
        if (!s_hudFontPath.empty())
        {
            io.Fonts->AddFontFromFileTTF(s_hudFontPath.c_str(), 16.0f);
            s_fontLoaded = true;
        }
        else if (const std::string ui = winFont("segoeui.ttf"); !ui.empty())
        {
            io.Fonts->AddFontFromFileTTF(ui.c_str(), 17.0f);
            s_fontLoaded = false;
        }
        else
        {
            io.Fonts->AddFontDefault();
            s_fontLoaded = false;
        }
        const std::string sb = winFont("seguisb.ttf"), bl = winFont("seguibl.ttf");
        s_fontSemibold = sb.empty() ? nullptr : io.Fonts->AddFontFromFileTTF(sb.c_str(), 17.0f);
        s_fontBlack = bl.empty() ? s_fontSemibold : io.Fonts->AddFontFromFileTTF(bl.c_str(), 22.0f);
    }

    // Blinking alpha for "Press..." capture hints.
    float blinkAlpha()
    {
        return 0.5f + 0.5f * std::sin(ImGui::GetTime() * 6.0f);
    }

    // Section header helper: small uppercase accent text (Russo One, when loaded)
    // with a leading orange bar.
    void sectionHeader(const char *label)
    {
        ImGui::Spacing();
        ImGui::Spacing();
        {
            ScopedStyleColor c(ImGuiCol_Text, accent());
            ImGui::TextUnformatted(label);
        }
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 100);
        {
            ScopedStyleColor c(ImGuiCol_Separator, accent(0.35f));
            ImGui::Separator();
        }
        ImGui::Spacing();
    }

    // Simple toggle-switch look: checkbox with an ON/OFF trailing badge.
    bool toggleSwitch(const char *label, bool *v)
    {
        ImGui::PushID(label);   // unique ID per toggle -> no ON/OFF ID conflicts
        bool changed = ImGui::Checkbox(label, v);
        ImGui::SameLine();
        const bool on = *v;
        {
            ScopedStyleColor c0(ImGuiCol_Button, on ? accent() : dbz(0.25f, 0.25f, 0.32f));
            ScopedStyleColor c1(ImGuiCol_ButtonHovered, on ? gold() : dbz(0.30f, 0.30f, 0.38f));
            ScopedStyleColor c2(ImGuiCol_ButtonActive, on ? gold() : dbz(0.30f, 0.30f, 0.38f));
            ScopedStyleColor c3(ImGuiCol_Text, on ? dbz(0.10f, 0.07f, 0.03f) : dbz(0.85f, 0.85f, 0.85f));
            ImGui::SmallButton(on ? "ON" : "OFF");
        }
        ImGui::PopID();
        return changed;
    }
}

bool PS2SettingsOverlay::s_widescreen = false;

// [fsnative] Fullscreen at the MONITOR's resolution. raylib's ToggleFullscreen() keeps the window's current size as
// the video mode (1024x768 from the INI); on Wayland the compositor then stretches that 4:3 surface across the 16:9
// panel while the game still sees a 4:3 screen -- neither the true-widescreen FOV patch nor the HUD squeeze engage
// and the whole picture is stretched. Size the window to the monitor first; restore the saved size on the way out.
// PS2X_FSNATIVE=0 restores the old toggle.
// [winfit] a windowed size never larger than the monitor's usable area: the saved/picked size is the window's inner
// size, so 1920x1080 on a 1920x1080 desktop put the title bar and bottom edge under the taskbar / off screen.
// Shrink to fit (aspect kept), then centre on the window's monitor.
// [winmem] the last windowed position (settings window_x / window_y), restored after every windowed (re)size when the
// window would still be on a connected monitor; -100000 = centre as before
static int s_winX = -100000, s_winY = -100000;
static bool ps2xRestoreWindowPos(int w, int h)
{
    if (s_winX == -100000 || s_winY == -100000) return false;
    for (int m = 0; m < GetMonitorCount(); ++m)
    {   // the title bar must be reachable: its middle has to lie on a monitor
        const Vector2 mp = GetMonitorPosition(m);
        const int mx = int(mp.x), my = int(mp.y), mw = GetMonitorWidth(m), mh = GetMonitorHeight(m);
        const int cx = s_winX + w / 2, ty = s_winY + 8;
        if (cx >= mx && cx < mx + mw && ty >= my && ty < my + mh - 40)
        {
            SetWindowPosition(s_winX, s_winY);
            return true;
        }
    }
    return false;
}
static void ps2xApplyWindowSize(int w, int h)
{
    if (w < 320 || h < 240) { ps2xRestoreWindowPos(GetScreenWidth(), GetScreenHeight()); return; }
    const int m = GetCurrentMonitor();
    const int mw = GetMonitorWidth(m), mh = GetMonitorHeight(m);
    if (mw >= 320 && mh >= 240)
    {
        const int maxW = mw - 16, maxH = mh - 110;   // window frame + title bar + taskbar
        if (w > maxW || h > maxH)
        {
            const double k = std::min(double(maxW) / w, double(maxH) / h);
            w = int(w * k); h = int(h * k);
        }
        SetWindowSize(w, h);
        if (ps2xRestoreWindowPos(w, h)) return;
        const Vector2 mp = GetMonitorPosition(m);
        SetWindowPosition(int(mp.x) + (mw - w) / 2, int(mp.y) + std::max(32, (mh - 48 - h) / 2));
        return;
    }
    SetWindowSize(w, h);
}
static void ps2xSetFullscreen(bool on, int windowW, int windowH)
{
    static const bool s_native = [](){ const char *v = std::getenv("PS2X_FSNATIVE"); return !(v && v[0] == '0'); }();
    if (!s_native) { ToggleFullscreen(); return; }
    if (on)
    {
        if (IsWindowFullscreen()) return;
        const int m = GetCurrentMonitor();
        const int mw = GetMonitorWidth(m), mh = GetMonitorHeight(m);
        if (mw >= 320 && mh >= 240) SetWindowSize(mw, mh);
        ToggleFullscreen();
    }
    else
    {
        if (IsWindowFullscreen()) ToggleFullscreen();
        ps2xApplyWindowSize(windowW, windowH);
    }
}
// [postfx] the present-time image controls
static void pushPostFx(const PS2SettingsOverlay::Settings &st)
{
    ps2x_postfx::Settings p;
    if (!g_ps2xIsCttr)   // [cttrmenu] CTTR has no Picture tab any more: always the game's own picture
    {
        p.fxaa = st.fxaa; p.sharpen = st.sharpen; p.brightness = st.brightness;
        p.contrast = st.contrast; p.saturation = st.saturation; p.gamma = st.gamma;
    }
    ps2x_postfx::set(p);
}
// [wshudmap] live HUD-layout state, defined in ps2_gs_gpu_renderer.cpp
// [winbtn] fullscreen has no title bar: minimise / window / close buttons in the top-right corner while the mouse
// is near it. Minimise goes through the F8 path ([minimize]); close ends the run like the window's own X.
std::atomic<bool> g_ps2xCloseRequested{false};
std::atomic<bool> g_ps2xShotRequest{false};   // [screenshot] F12 -> the present loop (ps2_runtime.cpp) grabs the next picture
namespace
{
    std::mutex g_shotMx;
    std::string g_shotMsg;          // the notice shown for a moment after a save (or a failure)
    double g_shotMsgUntil = -1.0;   // GetTime() until which it shows
}
extern "C" bool ps2xPicturesDir(char *out, unsigned n);   // ps2_win_timer.cpp
// [screenshot] Called by the present loop right after the game picture is drawn and BEFORE the Settings window, the
// readouts or the notice go on top: grabs the window, keeps only the picture's rectangle (no letterbox bars) and writes
// a PNG to <Pictures>\Crash Tag Team Racing\CTTR_<date>_<time>.png on a worker thread (encoding ~100 ms would hitch).
void ps2xSaveScreenshot(float x, float y, float w, float h)
{
    Image im = LoadImageFromScreen();
    // the window may be scaled (high-DPI): map the picture rectangle from screen units to framebuffer pixels
    const float sx = GetScreenWidth() > 0 ? float(im.width) / float(GetScreenWidth()) : 1.0f;
    const float sy = GetScreenHeight() > 0 ? float(im.height) / float(GetScreenHeight()) : 1.0f;
    float cx = std::max(0.0f, x * sx), cy = std::max(0.0f, y * sy);
    float cw = std::min(float(im.width) - cx, w * sx), ch = std::min(float(im.height) - cy, h * sy);
    if (cw >= 16.0f && ch >= 16.0f) ImageCrop(&im, Rectangle{std::floor(cx), std::floor(cy), std::floor(cw), std::floor(ch)});
    char pics[512] = {0};
    std::string dir = ps2xPicturesDir(pics, sizeof pics) ? std::string(pics) : std::string(".");
    dir += "\\Crash Tag Team Racing";
    char name[64];
    std::time_t t = std::time(nullptr); std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    std::strftime(name, sizeof name, "CTTR_%Y-%m-%d_%H-%M-%S", &tmv);
    std::string path = dir + "\\" + name + ".png";
    for (int n = 2; std::filesystem::exists(path) && n < 100; ++n) path = dir + "\\" + name + "_" + std::to_string(n) + ".png";
    std::thread([im, dir, path]() mutable {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const bool ok = ExportImage(im, path.c_str());
        UnloadImage(im);
        std::fprintf(stderr, "[screenshot] %s %s\n", ok ? "saved" : "FAILED", path.c_str());
        std::lock_guard<std::mutex> lk(g_shotMx);
        g_shotMsg = ok ? "Screenshot saved to Pictures\\Crash Tag Team Racing" : "Screenshot could not be saved";
        g_shotMsgUntil = -2.0;   // the overlay starts the 2.5 s on its own thread (GetTime belongs to it)
    }).detach();
}
std::atomic<bool> g_ps2xFocusPaused{false};
std::atomic<int> g_cttrModWide{0};   // [cttrmods] F1 > Mods (game_overrides.cpp, ps2_runtime.cpp)
std::atomic<int> g_ps2xOsdLanguage{-1};   // [cttrlang] read once by the OSD config (Kernel/Syscalls/Helpers/Runtime.h); -1 = English   // [focuspause] read by the vblank worker (Interrupt.cpp) and the SPU2 output (CttrIop.cpp)
std::atomic<int> g_ps2xBigkickArm{0};   // [bigkicktag] F9 arms the VU1 kick dump (ps2_vu1.cpp)
static bool s_minimizeRequested = false;
extern std::atomic<int> g_wsHudLayout;
extern std::atomic<int> g_wsHudOffLQ, g_wsHudOffCQ, g_wsHudOffRQ;
static void pushHudLayout(const PS2SettingsOverlay::Settings &st)
{
    // an explicit PS2X_WSHUDLAYOUT env (rig lever) outranks the INI/UI
    if (std::getenv("PS2X_WSHUDLAYOUT")) return;
    g_wsHudOffLQ.store(st.hudOffL * 16, std::memory_order_relaxed);
    g_wsHudOffCQ.store(st.hudOffC * 16, std::memory_order_relaxed);
    g_wsHudOffRQ.store(st.hudOffR * 16, std::memory_order_relaxed);
    g_wsHudLayout.store(st.hudLayout, std::memory_order_relaxed);
}
std::string PS2SettingsOverlay::s_configDir;
int PS2SettingsOverlay::s_logLevel = 1;        // [loglevel] default: profile+mclog+sched (see header)
int PS2SettingsOverlay::s_startupLogLevel = 1; // [loglevel] captured by preloadSettings for main()

void PS2SettingsOverlay::setConfigDirectory(const std::string &dir)
{
    s_configDir = dir;
    if (!s_configDir.empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(s_configDir, ec);
    }
}

void PS2SettingsOverlay::initialize()
{
    if (m_initialized)
        return;
    // --- Capsule HUD display font (Russo One) -------------------------------
    // Shipped under assets/fonts next to the executable (portable dist), same
    // layout convention as savedata/ (see getExecutableDirectory() in main.cpp).
    // s_configDir is "<exeDir>/savedata", so its parent is <exeDir>.
    //
    // rlImGui builds its font atlas inside rlImGuiSetup() -> rlImGuiBeginInitImGui(),
    // which calls AddFontDefault() for us UNLESS we register our own callback via
    // rlImGuiSetLoadFontsCallback() first (rlImGui has no separate "reload fonts"
    // entry point — the callback is the supported hook for adding extra fonts). So
    // the path must be resolved and the callback installed BEFORE rlImGuiSetup runs.
    if (!s_configDir.empty())
    {
        const char *assetDir = std::getenv("PS2X_ASSETDIR");
        const std::filesystem::path assets = assetDir && *assetDir
            ? std::filesystem::path(assetDir)
            : std::filesystem::path(s_configDir).parent_path() / "assets";
        const std::filesystem::path fontPath = assets / "fonts" / "RussoOne-Regular.ttf";
        std::error_code ec;
        if (std::filesystem::is_regular_file(fontPath, ec) && !ec)
            s_hudFontPath = fontPath.string();
        else
            std::fprintf(stderr, "[overlay] Russo One font not found at %s, falling back to default font\n",
                        fontPath.string().c_str());
    }
    rlImGuiSetup(true);
    // The rlImGui version used here has no rlImGuiSetLoadFontsCallback() hook, so load
    // the Capsule HUD fonts directly after setup — ImGui rebuilds the atlas lazily on
    // the first frame.
    loadOverlayFonts();

    ImGuiIO &io = ImGui::GetIO();
    // Do NOT enable NavEnableGamepad: raylib feeds the connected gamepad's axes/buttons
    // straight into ImGui, so with the nav flag on a joystick at rest would yank sliders
    // (e.g. volume) to 0 the instant they are clicked. The overlay is mouse/keyboard only.
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;

    m_initialized = true;
    m_configPath = s_configDir.empty()
        ? (std::filesystem::current_path() / kConfigFileName).string()
        : (std::filesystem::path(s_configDir) / kConfigFileName).string();
    loadSettings();
    // Apply the saved window size (before any fullscreen toggle, so it sizes the
    // windowed state the user returns to). 0 = keep the default host window.
    // [f1] the chosen monitor (Display) first, so the size is fitted to that monitor
    if (m_settings.display > 0 && m_settings.display <= GetMonitorCount())
        SetWindowMonitor(m_settings.display - 1);
    s_winX = m_settings.windowX; s_winY = m_settings.windowY;   // [winmem] (only CTTR ever saves one; the game is not identified yet here)
    ps2xApplyWindowSize(m_settings.windowW, m_settings.windowH);
    // Apply fullscreen on startup if the INI says so (or the default is true).
    if (m_settings.fullscreen)
        ps2xSetFullscreen(true, m_settings.windowW, m_settings.windowH);
    // Build the device list up front so the gamepad toggle combo works before the
    // overlay is opened for the first time (m_deviceList is otherwise only populated
    // when the overlay opens via resetCaptureState/buildDeviceList).
    buildDeviceList();
    // Apply all loaded settings (glow, volume, etc.) at startup.
    applySettings();
    // Snapshot the persisted settings so shutdown() only rewrites the ini when the
    // session actually changed something (keeps launcher-authored values intact).
    m_settingsAtBoot = m_settings;
}

void PS2SettingsOverlay::shutdown()
{
    if (!m_initialized)
        return;
    // Persist only when something changed this session OR no ini exists yet
    // (first boot seeds the default file). Otherwise leave the existing
    // ini untouched so launcher-authored settings survive a play session.
    if (!(m_settings == m_settingsAtBoot) || !std::filesystem::exists(m_configPath))
        saveSettings();
    rlImGuiShutdown();
    m_initialized = false;
}

bool PS2SettingsOverlay::Settings::operator==(const Settings &o) const
{
    return masterVolume == o.masterVolume &&
           musicVolume == o.musicVolume &&
           sfxVolume == o.sfxVolume &&
           gpuRenderer == o.gpuRenderer &&
           renderer == o.renderer &&
           glow == o.glow &&
           glowFix == o.glowFix &&
           bilinear == o.bilinear &&
           halfTexel == o.halfTexel &&
           skipPost == o.skipPost &&
           skipStaleVram == o.skipStaleVram &&
           renderScale == o.renderScale &&
           deadzone == o.deadzone &&
           fullscreen == o.fullscreen &&
           widescreen == o.widescreen &&
           outline == o.outline &&
           fps60 == o.fps60 &&
           inkStrength == o.inkStrength &&
           shadows == o.shadows &&
           dofBlur == o.dofBlur &&
           dofZFar == o.dofZFar &&
           windowW == o.windowW &&
           windowH == o.windowH && windowX == o.windowX && windowY == o.windowY &&
           forceBilinear == o.forceBilinear && pgsAniso == o.pgsAniso && cttrAspect == o.cttrAspect && cttrMovieAspect == o.cttrMovieAspect && cttrSkipIntro == o.cttrSkipIntro && cttrLanguage == o.cttrLanguage && pauseUnfocused == o.pauseUnfocused && cttrCheats == o.cttrCheats && modWidescreen == o.modWidescreen &&
           fxaa == o.fxaa && perfOverlay == o.perfOverlay && perfCorner == o.perfCorner && sharpen == o.sharpen && brightness == o.brightness &&
           contrast == o.contrast && saturation == o.saturation && gamma == o.gamma &&
           overlayEnabled == o.overlayEnabled &&
           hudLayout == o.hudLayout &&
           hudOffL == o.hudOffL &&
           hudOffC == o.hudOffC &&
           hudOffR == o.hudOffR &&
           overlayPadBtns == o.overlayPadBtns &&
           overlayKeys == o.overlayKeys &&
           logLevel == o.logLevel &&
           meter == o.meter && display == o.display;
}

void PS2SettingsOverlay::resetCaptureState()
{
    m_captureAction = -1;
    m_captureWaitRelease = false;
    m_prevBtnDown = {};
    m_prevAxis = {};
    buildDeviceList();
}

static bool s_nativeF1 = false;   // [menu] the Settings window (F1 / Shift+Tab) is open

void PS2SettingsOverlay::toggleVisible()
{
#ifdef PS2X_HAVE_NATIVE_GS
    // [menu] one Settings window: Shift+Tab / Select+Start open the same window as F1
    s_nativeF1 = !s_nativeF1;
    if (s_nativeF1) resetCaptureState();
    return;
#endif
    m_visible = !m_visible;
    ps2_stubs::PadConfig::setInputSuspended(m_visible);
    if (m_visible)
    {
        saveSettings();
        resetCaptureState();
    }
}

// [envwins] An env var the USER set explicitly (present, and not one main() defaulted --
// PS2X_DEFAULTED lists those) outranks the saved INI: play.sh-style launches and A/B runs
// must behave as commanded regardless of what the overlay saved last session.
static bool m_sawRendererKey = false;   // [renderer] set while parsing the ini
static bool envUserSet(const char *name)
{
    if (!std::getenv(name)) return false;
    const char *d = std::getenv("PS2X_DEFAULTED");
    if (!d) return true;
    std::string needle = std::string(",") + name + ",";
    return std::string(d).find(needle) == std::string::npos;
}

void PS2SettingsOverlay::loadSettings()
{
    m_sawRendererKey = false;
    // [defaults-sync] Seed from LIVE runtime state (env + main()'s baked defaults) so a
    // missing INI -- or a key the INI doesn't mention -- never pushes this struct's
    // hardcoded values over the validated configuration.
    syncFromRuntime();

    std::ifstream file(m_configPath);
    if (!file.is_open())
        return;

    ps2x_toml::Document doc;
    doc.parse(file);

    m_settings.masterVolume = std::clamp((float)doc.getD("audio.master_volume", m_settings.masterVolume), 0.0f, 1.0f);
    m_settings.musicVolume = std::clamp((float)doc.getD("audio.music_volume", m_settings.musicVolume), 0.0f, 1.0f);
    m_settings.sfxVolume = std::clamp((float)doc.getD("audio.sfx_volume", m_settings.sfxVolume), 0.0f, 1.0f);

    {
        int r = nameToRenderer(doc.getS("video.renderer", rendererName(m_settings.renderer)), m_settings.renderer);
#if !defined(PS2X_HAVE_PGS)
        if (r == Settings::kRendererParallelGS) r = Settings::kRendererOpenGL;
#endif
        if (r >= 0 && r <= 2) { m_settings.renderer = r; m_sawRendererKey = true; }
    }
    if (!envUserSet("PS2X_GLOW")) m_settings.glow = doc.getB("video.glow", m_settings.glow);
    if (!envUserSet("PS2X_GLOWFIX")) m_settings.glowFix = doc.getB("video.glowfix", m_settings.glowFix);
    if (!envUserSet("PS2X_INKSTRENGTH") && !envUserSet("PS2X_ADGS"))
        m_settings.inkStrength = std::clamp(doc.getI("video.ink_strength", m_settings.inkStrength), 100, 400);
    m_settings.inkWidth = std::clamp(doc.getI("video.ink_width", m_settings.inkWidth), 25, 100);
    m_settings.inkColor = hexToColor(doc.getS("video.ink_color", colorToHex(m_settings.inkColor)), m_settings.inkColor);
    if (!envUserSet("PS2X_BILINEAR")) m_settings.bilinear = doc.getB("video.bilinear", m_settings.bilinear);
    if (!envUserSet("PS2X_HALFTEXEL")) m_settings.halfTexel = doc.getB("video.halftexel", m_settings.halfTexel);
    if (!envUserSet("PS2X_SKIPPOST")) m_settings.skipPost = doc.getB("video.skippost", m_settings.skipPost);
    if (!envUserSet("PS2X_SKIP_STALE_VRAM")) m_settings.skipStaleVram = doc.getB("video.skip_stale_vram", m_settings.skipStaleVram);
    if (!envUserSet("PS2X_RENDER_SCALE"))
    {
        const int s = doc.getI("video.render_scale", m_settings.renderScale);
        m_settings.renderScale = (s >= 1 && s <= 4) ? s : 1;
    }
    m_settings.pgsAniso = std::clamp(doc.getI("video.anisotropic", m_settings.pgsAniso), 1, 16);   // [aniso]
    m_settings.cttrAspect = std::clamp(doc.getI("video.aspect_mode", m_settings.cttrAspect), 0, 4);   // [cttraspect]
    m_settings.cttrMovieAspect = std::clamp(doc.getI("video.movie_aspect", m_settings.cttrMovieAspect), 0, 2);   // [movieaspect]
    m_settings.cttrSkipIntro = doc.getB("video.skip_intro_movies", m_settings.cttrSkipIntro);   // [skipintro]
    m_settings.cttrLanguage = std::clamp(doc.getI("video.language", m_settings.cttrLanguage), 1, 6);   // [cttrlang]
    m_settings.pauseUnfocused = doc.getB("video.pause_unfocused", m_settings.pauseUnfocused);   // [focuspause]
    if (!envUserSet("PS2X_OUTLINE")) m_settings.outline = doc.getB("video.outline", m_settings.outline);
    if (!envUserSet("PS2X_SHADOWS")) m_settings.shadows = doc.getB("video.shadows", m_settings.shadows);
    if (!envUserSet("PS2X_DOFMASK")) m_settings.dofBlur = doc.getB("video.dof_blur", m_settings.dofBlur);
    m_settings.fxaa = doc.getB("video.fxaa", m_settings.fxaa);   // [postfx]
    m_settings.perfOverlay = doc.getB("video.perf_overlay", m_settings.perfOverlay);   // [perfhud]
    m_settings.perfCorner = std::clamp(doc.getI("video.perf_corner", m_settings.perfCorner), 0, 3);
    m_settings.meter = doc.getB("video.meter", m_settings.meter);                       // [f1]
    m_settings.display = std::clamp(doc.getI("video.display", m_settings.display), 0, 16);
    m_settings.sharpen = std::clamp(doc.getI("video.sharpen", m_settings.sharpen), 0, 100);
    m_settings.brightness = std::clamp(doc.getI("video.brightness", m_settings.brightness), -50, 50);
    m_settings.contrast = std::clamp(doc.getI("video.contrast", m_settings.contrast), 50, 150);
    m_settings.saturation = std::clamp(doc.getI("video.saturation", m_settings.saturation), 0, 200);
    m_settings.gamma = std::clamp(doc.getI("video.gamma", m_settings.gamma), 50, 200);
    if (!envUserSet("PS2X_DOFZFAR")) m_settings.dofZFar = std::clamp(doc.getI("video.dof_zfar", m_settings.dofZFar), 20000, 800000);
    m_settings.fullscreen = doc.getB("video.fullscreen", m_settings.fullscreen);
    m_settings.widescreen = doc.getB("video.widescreen", m_settings.widescreen);
    m_settings.fps60 = doc.getB("video.fps60", m_settings.fps60);
    m_settings.windowW = doc.getI("video.window_w", m_settings.windowW);
    m_settings.windowH = doc.getI("video.window_h", m_settings.windowH);
    m_settings.windowX = doc.getI("video.window_x", m_settings.windowX);   // [winmem]
    m_settings.windowY = doc.getI("video.window_y", m_settings.windowY);
    m_settings.forceBilinear = doc.getB("video.force_bilinear", m_settings.forceBilinear);
    m_settings.hudLayout = doc.getI("video.hud.layout", m_settings.hudLayout);
    m_settings.hudOffL = doc.getI("video.hud.offset_left", m_settings.hudOffL);
    m_settings.hudOffC = doc.getI("video.hud.offset_center", m_settings.hudOffC);
    m_settings.hudOffR = doc.getI("video.hud.offset_right", m_settings.hudOffR);

    {   // [cheats] the env (from an old launcher or a test) wins over the file
        auto envInt = [](const char *n, int d) { const char *e = std::getenv(n); return e && e[0] ? std::atoi(e) : d; };
        m_settings.abilityBonus = std::clamp(envInt("PS2X_ABILITY_BONUS", doc.getI("cheats.ability_bonus", m_settings.abilityBonus)), 0, 99);
        m_settings.itemCap = std::clamp(envInt("PS2X_ITEMCAP", doc.getI("cheats.item_cap", m_settings.itemCap)), 20, 80);
        m_settings.extraItemRows = std::clamp(envInt("PS2X_EXTRA_ITEM_ROWS", doc.getI("cheats.extra_item_rows", m_settings.extraItemRows)), 0, 2);
        m_settings.modWidescreen = doc.getB("mods.widescreen", m_settings.modWidescreen);   // [cttrmods]
        m_settings.cttrCheats = doc.getI("cheats.cttr_toggles", m_settings.cttrCheats) & 0xbf;   // [cttrcheats] bit 6 (Japanese Crash) is an outfit the game saves itself: never re-applied at boot [japanoutfit]
    }
    m_settings.deadzone = std::clamp((float)doc.getD("controllers.deadzone", m_settings.deadzone), 0.0f, 0.5f);
    m_settings.overlayEnabled = doc.getB("controllers.overlay_enabled", m_settings.overlayEnabled);
    m_selectedDevice = std::clamp(doc.getI("controllers.device", m_selectedDevice), 0, 100);
    {
        std::vector<int> pb = doc.getIA("controllers.hotkey.pad_btns", m_settings.overlayPadBtns);
        for (int &b : pb) b = std::clamp(b, 0, 31);
        if (!pb.empty()) m_settings.overlayPadBtns = pb;
        std::vector<int> keys = doc.getIA("controllers.hotkey.keys", m_settings.overlayKeys);
        for (int &k : keys) k = std::clamp(k, 32, 348);
        if (!keys.empty()) m_settings.overlayKeys = keys;
    }

    m_dumpAudio = doc.getB("logging.dump_audio", m_dumpAudio);
    m_dumpVideo = doc.getB("logging.dump_video", m_dumpVideo);
    m_dumpControllers = doc.getB("logging.dump_controllers", m_dumpControllers);
    m_dumpRuntime = doc.getB("logging.dump_runtime", m_dumpRuntime);
    m_dumpGamepad = doc.getB("logging.dump_gamepad", m_dumpGamepad);
    m_settings.logLevel = std::clamp(doc.getI("logging.log_level", m_settings.logLevel), 0, 3);
    s_logLevel = m_settings.logLevel;

    // [renderer] keep gpuRenderer coherent for the older readers.
    m_settings.gpuRenderer = (m_settings.renderer != Settings::kRendererSoftware);
}

// [renderer] The paraLLEl-GS backend reads PS2X_PGS* lazily at its first packet, so the ini choice is exported as
// environment DEFAULTS here (setenv(...,0): an explicit user override still wins). renderer 2 = backend on, exclusive
// (our GS parse skipped) unless a texture pack is on, where the backend needs our state-only parse for the hashes.
static void setEnvDefault(const char *name, const char *value)
{   // never overwrites: an explicit user env override wins
    if (std::getenv(name)) return;
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 0);
#endif
}
static void exportRendererEnv(int renderer, bool forceBilinear)
{
#if defined(PS2X_HAVE_PGS)
    if (renderer == 2)
    {
        setEnvDefault("PS2X_PGS", "1");
        setEnvDefault("PS2X_PGS_EXCLUSIVE", "1");   // our GS parse is skipped: paraLLEl-GS draws everything
        if (forceBilinear) setEnvDefault("PS2X_PGS_FORCE_BILINEAR", "1");
    }
    else
        setEnvDefault("PS2X_PGS", "0");
#else
    (void)renderer; (void)forceBilinear;
#endif
}

void PS2SettingsOverlay::preloadSettings()
{
    const std::string configPath = s_configDir.empty()
        ? (std::filesystem::current_path() / kConfigFileName).string()
        : (std::filesystem::path(s_configDir) / kConfigFileName).string();
    int rendererPre = Settings::kRendererDefault;   // [renderer] exported below even when no toml exists yet
    bool forceBilinearPre = true;

    std::ifstream file(configPath);
    if (!file.is_open())
    {
        // 0.x legacy INI: the launcher imports it and writes the TOML (dropping the old
        // file). Running the runner directly, just clear a stray leftover.
        const std::string legacy = s_configDir.empty()
            ? (std::filesystem::current_path() / kLegacyConfigFileName).string()
            : (std::filesystem::path(s_configDir) / kLegacyConfigFileName).string();
        std::error_code ec;
        std::filesystem::remove(legacy, ec);
        exportRendererEnv(rendererPre, forceBilinearPre);
        return;
    }

    ps2x_toml::Document doc;
    doc.parse(file);

    s_widescreen = doc.getB("video.widescreen", s_widescreen);
    rendererPre = nameToRenderer(doc.getS("video.renderer", rendererName(rendererPre)), rendererPre);
    forceBilinearPre = doc.getB("video.force_bilinear", forceBilinearPre);
    {   // [rscale] authoritative startup application -- runs before anything reads the
        // live scale, so the TOML value wins the lazy-init race.
        const int rs = doc.getI("video.render_scale", 0);
        if (rs >= 1 && rs <= 4 && !envUserSet("PS2X_RENDER_SCALE") && !envUserSet("PS2X_RENDERSCALE"))
        {
            GsGpuRenderer::setRenderScale(rs);
            if (!envUserSet("PS2X_PGS_SSAA")) ps2x_pgs::setRenderScale(rs);   // [pgslive] backend starts at the file scale
        }
    }
    {
        // [skipintro] before the boot script asks OptionManager_Get().GetSkipIntroMovies()
        if (!envUserSet("PS2X_SKIPINTRO")) { extern std::atomic<int> g_cttrSkipIntro; g_cttrSkipIntro.store(doc.getB("video.skip_intro_movies", false) ? 1 : 0); }
        // [cttrlang] before the game first asks for the console's language
        g_ps2xOsdLanguage.store(std::clamp(doc.getI("video.language", 1), 1, 6));
    }
    {
        // [loglevel] capture for main(): must be visible before runtime init so the
        // PS2X_* diagnostic env vars (and the stderr redirect to logs/bt3.log) apply.
        const int lvl = std::clamp(doc.getI("logging.log_level", s_startupLogLevel), 0, 3);
        s_logLevel = lvl;
        s_startupLogLevel = lvl;
    }
    {   // [cheats] the game hooks read these when they are installed (an explicit env still wins)
        char b[16];
        std::snprintf(b, sizeof b, "%d", std::clamp(doc.getI("cheats.ability_bonus", 0), 0, 99)); setEnvDefault("PS2X_ABILITY_BONUS", b);
        std::snprintf(b, sizeof b, "%d", std::clamp(doc.getI("cheats.item_cap", 20), 20, 80)); setEnvDefault("PS2X_ITEMCAP", b);
        std::snprintf(b, sizeof b, "%d", std::clamp(doc.getI("cheats.extra_item_rows", 0), 0, 2)); setEnvDefault("PS2X_EXTRA_ITEM_ROWS", b);
    }
    exportRendererEnv(rendererPre, forceBilinearPre);
}

void PS2SettingsOverlay::saveSettings() const
{
    using ps2x_toml::fmtBool;
    using ps2x_toml::fmtDbl;
    using ps2x_toml::fmtInt;
    using ps2x_toml::fmtIntArray;
    using ps2x_toml::fmtStr;

    std::ostringstream os;
    os << kConfigHeader << "\n";

    os << "[audio]\n";
    os << "master_volume = " << fmtDbl(m_settings.masterVolume) << "\n";
    os << "music_volume = " << fmtDbl(m_settings.musicVolume) << "\n";
    os << "sfx_volume = " << fmtDbl(m_settings.sfxVolume) << "\n\n";

    os << "[video]\n";
    os << "renderer = " << fmtStr(rendererName(m_settings.renderer)) << "\n";
    os << "glow = " << fmtBool(m_settings.glow) << "\n";
    os << "glowfix = " << fmtBool(m_settings.glowFix) << "\n";
    os << "ink_strength = " << fmtInt(m_settings.inkStrength) << "\n";
    os << "ink_width = " << fmtInt(m_settings.inkWidth) << "\n";
    os << "ink_color = " << fmtStr(colorToHex(m_settings.inkColor)) << "\n";
    os << "bilinear = " << fmtBool(m_settings.bilinear) << "\n";
    os << "halftexel = " << fmtBool(m_settings.halfTexel) << "\n";
    os << "skippost = " << fmtBool(m_settings.skipPost) << "\n";
    os << "skip_stale_vram = " << fmtBool(m_settings.skipStaleVram) << "\n";
    os << "render_scale = " << fmtInt(m_settings.renderScale) << "\n";
    os << "anisotropic = " << fmtInt(m_settings.pgsAniso) << "\n";   // [aniso]
    os << "aspect_mode = " << fmtInt(m_settings.cttrAspect) << "\n";   // [cttraspect]
    os << "movie_aspect = " << fmtInt(m_settings.cttrMovieAspect) << "\n";   // [movieaspect]
    os << "skip_intro_movies = " << fmtBool(m_settings.cttrSkipIntro) << "\n";   // [skipintro]
    os << "language = " << fmtInt(m_settings.cttrLanguage) << "\n";   // [cttrlang]
    os << "pause_unfocused = " << fmtBool(m_settings.pauseUnfocused) << "\n";   // [focuspause]
    os << "outline = " << fmtBool(m_settings.outline) << "\n";
    os << "shadows = " << fmtBool(m_settings.shadows) << "\n";
    os << "dof_blur = " << fmtBool(m_settings.dofBlur) << "\n";
    os << "fxaa = " << fmtBool(m_settings.fxaa) << "\n";   // [postfx]
    os << "perf_overlay = " << fmtBool(m_settings.perfOverlay) << "\n";   // [perfhud] (read at load; was never written back)
    os << "perf_corner = " << fmtInt(m_settings.perfCorner) << "\n";
    os << "meter = " << fmtBool(m_settings.meter) << "\n";        // [f1]
    os << "display = " << fmtInt(m_settings.display) << "\n";
    os << "sharpen = " << fmtInt(m_settings.sharpen) << "\n";
    os << "brightness = " << fmtInt(m_settings.brightness) << "\n";
    os << "contrast = " << fmtInt(m_settings.contrast) << "\n";
    os << "saturation = " << fmtInt(m_settings.saturation) << "\n";
    os << "gamma = " << fmtInt(m_settings.gamma) << "\n";
    os << "dof_zfar = " << fmtInt(m_settings.dofZFar) << "\n";
    os << "fullscreen = " << fmtBool(m_settings.fullscreen) << "\n";
    os << "widescreen = " << fmtBool(m_settings.widescreen) << "\n";
    os << "window_w = " << fmtInt(m_settings.windowW) << "\n";
    os << "window_h = " << fmtInt(m_settings.windowH) << "\n";
    os << "window_x = " << fmtInt(m_settings.windowX) << "\n";   // [winmem]
    os << "window_y = " << fmtInt(m_settings.windowY) << "\n";
    os << "force_bilinear = " << fmtBool(m_settings.forceBilinear) << "\n";
    os << "fps60 = " << fmtBool(m_settings.fps60) << "\n\n";

    os << "[video.hud]\n";
    os << "layout = " << fmtInt(m_settings.hudLayout) << "\n";
    os << "offset_left = " << fmtInt(m_settings.hudOffL) << "\n";
    os << "offset_center = " << fmtInt(m_settings.hudOffC) << "\n";
    os << "offset_right = " << fmtInt(m_settings.hudOffR) << "\n\n";

    os << "[cheats]\n";   // [cheats] Settings > Cheats and the launcher's Cheats tab
    os << "ability_bonus = " << fmtInt(m_settings.abilityBonus) << "\n";
    os << "item_cap = " << fmtInt(m_settings.itemCap) << "\n";
    os << "extra_item_rows = " << fmtInt(m_settings.extraItemRows) << "\n";
    os << "cttr_toggles = " << fmtInt(m_settings.cttrCheats) << "\n\n";   // [cttrcheats] F1 > Cheats switches (bit mask)

    os << "[mods]\n";   // [cttrmods] F1 > Mods (changes to the game; off = the PS2 original)
    os << "widescreen = " << fmtBool(m_settings.modWidescreen) << "\n";
    os << "\n";

    os << "[controllers]\n";
    os << "device = " << fmtInt(m_selectedDevice) << "\n";
    os << "deadzone = " << fmtDbl(m_settings.deadzone) << "\n";
    os << "overlay_enabled = " << fmtBool(m_settings.overlayEnabled) << "\n\n";

    os << "[controllers.hotkey]\n";
    os << "pad_btns = " << fmtIntArray(m_settings.overlayPadBtns) << "\n";
    os << "keys = " << fmtIntArray(m_settings.overlayKeys) << "\n\n";

    os << "[logging]\n";
    os << "log_level = " << fmtInt(m_settings.logLevel) << "\n";
    os << "dump_audio = " << fmtBool(m_dumpAudio) << "\n";
    os << "dump_video = " << fmtBool(m_dumpVideo) << "\n";
    os << "dump_controllers = " << fmtBool(m_dumpControllers) << "\n";
    os << "dump_runtime = " << fmtBool(m_dumpRuntime) << "\n";
    os << "dump_gamepad = " << fmtBool(m_dumpGamepad) << "\n";

    std::ofstream file(m_configPath, std::ios::trunc);
    if (!file.is_open())
        return;
    file << os.str();
}

void PS2SettingsOverlay::applyDeadzone()
{
    auto &pcfg = ps2_stubs::PadConfig::instance();
    for (size_t p = 0; p < ps2_stubs::PadConfig::kPlayerCount; ++p)
    {
        auto cfg = pcfg.snapshot(p);
        bool changed = false;
        for (size_t a = 0; a < static_cast<size_t>(ps2_stubs::PadAction::Count); ++a)
        {
            if (cfg.binds[a].deadzone != m_settings.deadzone)
            {
                cfg.binds[a].deadzone = m_settings.deadzone;
                changed = true;
            }
        }
        if (changed)
        {
            for (size_t a = 0; a < static_cast<size_t>(ps2_stubs::PadAction::Count); ++a)
                pcfg.setBind(p, static_cast<ps2_stubs::PadAction>(a), cfg.binds[a]);
        }
    }
}

void PS2SettingsOverlay::syncFromRuntime()
{
    m_settings.masterVolume = PS2AudioBackend::masterVolume();
    m_settings.musicVolume = PS2AudioBackend::musicVolume();
    m_settings.sfxVolume = PS2AudioBackend::sfxVolume();
    m_settings.gpuRenderer = GsGpuRenderer::enabled();
    m_settings.glow = GsGpuRenderer::glowEnabled();
    m_settings.glowFix = GsGpuRenderer::glowFixEnabled();
    m_settings.inkStrength = GsGpuRenderer::inkStrengthPct();
    m_settings.bilinear = GsGpuRenderer::bilinearEnabled();
    m_settings.halfTexel = GsGpuRenderer::halfTexelEnabled();
    m_settings.skipPost = GsGpuRenderer::skipPostEnabled();
    m_settings.skipStaleVram = GsGpuRenderer::skipStaleVramEnabled();
    m_settings.renderScale = GsGpuRenderer::renderScale();
    m_settings.outline = GsGpuRenderer::outlineEnabled();
    m_settings.shadows = GsGpuRenderer::shadowsEnabled();
    m_settings.dofBlur = GsGpuRenderer::dofBlurEnabled();
    m_settings.dofZFar = GsGpuRenderer::dofZFar();
}

void PS2SettingsOverlay::applySettings()
{
    ps2Set60Fps(m_settings.fps60, nullptr);   // [fps60]
    s_widescreen = m_settings.widescreen;
    PS2AudioBackend::setMasterVolume(m_settings.masterVolume);
    PS2AudioBackend::setMusicVolume(m_settings.musicVolume);
    PS2AudioBackend::setSfxVolume(m_settings.sfxVolume);
    m_settings.gpuRenderer = (m_settings.renderer != Settings::kRendererSoftware);   // [renderer]
    GsGpuRenderer::setEnabled(m_settings.gpuRenderer);
    GsGpuRenderer::setGlow(m_settings.glow);
    // [glowfix] applied at STARTUP only: two of its four parts (the fbp224/fbp336 size caps)
    // are decided when the FBO is allocated, so flipping it mid-run would leave a half-applied
    // state -- and a partial glow fix is a REGRESSION (it washes the frame out).
    GsGpuRenderer::setGlowFix(m_settings.glowFix);
    GsGpuRenderer::setInkStrengthPct(m_settings.inkStrength);   // [inkstrength] live: it is one shader uniform
    ps2x_pgs::setInkWidthPct(m_settings.inkWidth);   // [pgsink] backend stroke width
    ps2x_pgs::setInkColor(m_settings.inkColor);       // [pgsink] backend stroke colour
    GsGpuRenderer::setBilinear(m_settings.bilinear);
    GsGpuRenderer::setHalfTexel(m_settings.halfTexel);
    GsGpuRenderer::setSkipPost(m_settings.skipPost);
    GsGpuRenderer::setSkipStaleVram(m_settings.skipStaleVram);
    // [rscale] NOT applied here -- see preloadSettings (startup-only). applySettings runs
    // on live changes too, and a live scale store would desync the pipeline.
    GsGpuRenderer::setOutline(m_settings.outline);
    if (!envUserSet("PS2X_PGS_SSAA")) ps2x_pgs::setRenderScale(m_settings.renderScale); // [pgslive] (an explicit launcher SSAA wins until the combo is touched)
    if (!envUserSet("PS2X_PGS_ANISO")) ps2x_pgs::setAniso(m_settings.pgsAniso);         // [aniso]
    { extern std::atomic<int> g_cttrAspectMode; g_cttrAspectMode.store(m_settings.cttrAspect); }   // [cttraspect]
    { extern std::atomic<int> g_cttrModWide; g_cttrModWide.store(m_settings.modWidescreen ? 1 : 0); }   // [cttrmods]
    { extern std::atomic<int> g_cttrMovieAspect; g_cttrMovieAspect.store(m_settings.cttrMovieAspect); }   // [movieaspect]
    if (!envUserSet("PS2X_SKIPINTRO")) { extern std::atomic<int> g_cttrSkipIntro; g_cttrSkipIntro.store(m_settings.cttrSkipIntro ? 1 : 0); }   // [skipintro]
    { extern std::atomic<uint32_t> g_cttrCheatWant; g_cttrCheatWant.store((uint32_t)m_settings.cttrCheats); }   // [cttrcheats]
    GsGpuRenderer::setShadows(m_settings.shadows);
    GsGpuRenderer::setDofBlur(m_settings.dofBlur);
    GsGpuRenderer::setDofZFar(m_settings.dofZFar);
    pushPostFx(m_settings);   // [postfx]
    { extern void ps2xSetForceBilinear(bool); ps2xSetForceBilinear(m_settings.forceBilinear); }
    pushHudLayout(m_settings);
    applyDeadzone();

    // Apply selected device to PadConfig
    if (m_selectedDevice >= 0 && m_selectedDevice < static_cast<int>(m_deviceList.size()))
    {
        auto &dev = m_deviceList[m_selectedDevice];
        auto &pcfg = ps2_stubs::PadConfig::instance();
        for (size_t p = 0; p < ps2_stubs::PadConfig::kPlayerCount; ++p)
        {
            auto cfg = pcfg.snapshot(p);
            // Persist the launcher's index convention ("Gamepad N"), never the raw
            // GLFW slot: the slot layout (1 for the Xbox here) != what pad_pN.conf
            // names, and a bare slot made pad_pN.conf point at a dead controller.
            int idx = ps2_stubs::padGamepadIndex(dev.glfwSlot);
            if (dev.kind == ps2_stubs::PadDeviceKind::Gamepad && idx < 0)
            {
                continue; // slot not (yet) a controller; leave the player alone
            }
            if (cfg.device.kind != dev.kind)
            {   // [padbinds] a different KIND of device gets that kind's default bindings (keyboard keys vs
                // gamepad buttons) instead of the old ones, which no longer refer to anything on it
                pcfg.setPlayerDefaults(p, dev.kind);
                pcfg.setDevice(p, ps2_stubs::PadDevice{dev.kind, idx});
            }
            else if (cfg.device.gamepad != idx)
                pcfg.setDevice(p, ps2_stubs::PadDevice{dev.kind, idx});
        }
    }

    // Dump current settings whenever they're applied (a BT3 support aid; CTTR's save folder stays clean).
    if (!g_ps2xIsCttr)
        dumpSettingsToFile();
}

void PS2SettingsOverlay::buildDeviceList()
{
    m_deviceList.clear();

    // 0: Auto (Any)
    m_deviceList.push_back({"Auto (Any gamepad + keyboard)", -1, false, ps2_stubs::PadDeviceKind::None});

    // 1: Keyboard
    m_deviceList.push_back({"Keyboard", -1, false, ps2_stubs::PadDeviceKind::Keyboard});

    // 2+: host gamepad slots
    for (int g = 0; g < ps2x_pad::kMaxSlots; ++g)
    {
        if (!ps2x_pad::available(g))
            continue;

        const char *name = ps2x_pad::name(g);
        std::string devName = (name && name[0]) ? name : ("Gamepad slot " + std::to_string(g));

#if defined(__linux__)
        bool evdevMatch = false;
        auto &native = ps2_stubs::PadEvdevLinux::instance();
        if (native.isAvailable() && native.matchesName(name))
        {
            evdevMatch = true;
            devName += " (" + native.node() + ")";
        }
        m_deviceList.push_back({devName, g, evdevMatch, ps2_stubs::PadDeviceKind::Gamepad});
#else
        m_deviceList.push_back({devName, g, false, ps2_stubs::PadDeviceKind::Gamepad});
#endif
    }

#if defined(__linux__)
    // If evdev is available but doesn't match any GLFW slot, add it separately
    auto &native = ps2_stubs::PadEvdevLinux::instance();
    if (native.isAvailable())
    {
        bool found = false;
        for (auto &d : m_deviceList)
        {
            if (d.isEvdev) { found = true; break; }
        }
        if (!found)
        {
            std::string evdevName = native.name() + " (" + native.node() + ")";
            m_deviceList.push_back({evdevName, -1, true, ps2_stubs::PadDeviceKind::Gamepad});
        }
    }
#endif

    // Clamp selection
    if (m_selectedDevice < 0 || m_selectedDevice >= static_cast<int>(m_deviceList.size()))
        m_selectedDevice = 0;
}

void PS2SettingsOverlay::readGamepadStateForDevice(
    const DeviceInfo &dev,
    std::array<uint8_t, 32> &btnDown,
    std::array<float, 6> &axis)
{
    btnDown = {};
    axis = {};

    if (dev.kind == ps2_stubs::PadDeviceKind::None)
    {
        // Auto: merge all host gamepads + evdev
        for (int g = 0; g < ps2x_pad::kMaxSlots; ++g)
        {
            if (!ps2x_pad::available(g))
                continue;
            for (int b = 0; b < 32; ++b)
                if (ps2x_pad::buttonDown(g, b))
                    btnDown[b] = 1;
            for (int a = 0; a < 6; ++a)
            {
                float v = ps2x_pad::axis(g, a);
                if (std::fabs(v) > std::fabs(axis[a]))
                    axis[a] = v;
            }
        }
#if defined(__linux__)
        auto &native = ps2_stubs::PadEvdevLinux::instance();
        if (native.isAvailable())
        {
            for (int b = 0; b < 32; ++b)
                if (native.isButtonDown(b))
                    btnDown[b] = 1;
            for (int a = 0; a < 6; ++a)
            {
                float v = native.getRawAxis(a);
                if (std::fabs(v) > std::fabs(axis[a]))
                    axis[a] = v;
            }
        }
#endif
    }
    else if (dev.kind == ps2_stubs::PadDeviceKind::Gamepad)
    {
        // Read from the specific host slot
        if (dev.glfwSlot >= 0 && ps2x_pad::available(dev.glfwSlot))
        {
            for (int b = 0; b < 32; ++b)
                if (ps2x_pad::buttonDown(dev.glfwSlot, b))
                    btnDown[b] = 1;
            for (int a = 0; a < 6; ++a)
                axis[a] = ps2x_pad::axis(dev.glfwSlot, a);
        }
        // Also read from evdev if it matches
        if (dev.isEvdev)
        {
#if defined(__linux__)
            auto &native = ps2_stubs::PadEvdevLinux::instance();
            if (native.isAvailable())
            {
                for (int b = 0; b < 32; ++b)
                    if (native.isButtonDown(b))
                        btnDown[b] = 1;
                for (int a = 0; a < 6; ++a)
                {
                    float v = native.getRawAxis(a);
                    if (std::fabs(v) > std::fabs(axis[a]))
                        axis[a] = v;
                }
            }
#endif
        }
    }
    // Keyboard: no gamepad axes/buttons to read
}

#ifdef PS2X_HAVE_NATIVE_GS
// [cttrmenu] F1 / Shift+Tab: the CTTR Settings window. A sidebar (Display, Graphics, Audio, Controls, Cheats, Help)
// and one page of settings rows (name and a one-line note on the left, the control on the right). Everything is
// saved to savedata\settings.toml when the window closes.
namespace cttrui
{
    // the palette: night-blue panels, Crash orange, Wumpa gold
    const ImVec4 kBg(0.067f, 0.075f, 0.106f, 0.97f);
    const ImVec4 kSide(0.047f, 0.055f, 0.078f, 1.0f);
    const ImVec4 kPanel(0.106f, 0.118f, 0.165f, 1.0f);
    const ImVec4 kPanelHi(0.141f, 0.157f, 0.216f, 1.0f);
    const ImVec4 kLine(0.18f, 0.20f, 0.26f, 1.0f);
    const ImVec4 kText(0.93f, 0.94f, 0.96f, 1.0f);
    const ImVec4 kMuted(0.56f, 0.59f, 0.67f, 1.0f);
    const ImVec4 kOrange(0.976f, 0.553f, 0.098f, 1.0f);
    const ImVec4 kGold(1.0f, 0.78f, 0.22f, 1.0f);
    inline ImVec4 alpha(const ImVec4 &c, float a) { return ImVec4(c.x, c.y, c.z, a); }
}

struct F1Style
{
    int colors = 0, vars = 0;
    void col(ImGuiCol c, const ImVec4 &v) { ImGui::PushStyleColor(c, v); ++colors; }
    void var(ImGuiStyleVar s, float f) { ImGui::PushStyleVar(s, f); ++vars; }
    void var(ImGuiStyleVar s, const ImVec2 &v) { ImGui::PushStyleVar(s, v); ++vars; }
    F1Style()
    {
        using namespace cttrui;
        var(ImGuiStyleVar_WindowRounding, 12.0f);
        var(ImGuiStyleVar_ChildRounding, 8.0f);
        var(ImGuiStyleVar_FrameRounding, 6.0f);
        var(ImGuiStyleVar_GrabRounding, 6.0f);
        var(ImGuiStyleVar_PopupRounding, 8.0f);
        var(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        var(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 7.0f));
        var(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 8.0f));
        var(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 9.0f));
        var(ImGuiStyleVar_WindowBorderSize, 1.0f);
        var(ImGuiStyleVar_ChildBorderSize, 0.0f);
        var(ImGuiStyleVar_PopupBorderSize, 1.0f);
        var(ImGuiStyleVar_ScrollbarSize, 10.0f);
        var(ImGuiStyleVar_ScrollbarRounding, 6.0f);
        col(ImGuiCol_Text, kText);
        col(ImGuiCol_TextDisabled, kMuted);
        col(ImGuiCol_WindowBg, kBg);
        col(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        col(ImGuiCol_PopupBg, ImVec4(0.09f, 0.10f, 0.14f, 0.99f));
        col(ImGuiCol_Border, kLine);
        col(ImGuiCol_FrameBg, kPanel);
        col(ImGuiCol_FrameBgHovered, kPanelHi);
        col(ImGuiCol_FrameBgActive, kPanelHi);
        col(ImGuiCol_Button, kPanel);
        col(ImGuiCol_ButtonHovered, kPanelHi);
        col(ImGuiCol_ButtonActive, alpha(kOrange, 0.55f));
        col(ImGuiCol_CheckMark, kOrange);
        col(ImGuiCol_SliderGrab, kOrange);
        col(ImGuiCol_SliderGrabActive, kGold);
        col(ImGuiCol_Header, alpha(kOrange, 0.18f));
        col(ImGuiCol_HeaderHovered, alpha(kOrange, 0.28f));
        col(ImGuiCol_HeaderActive, alpha(kOrange, 0.40f));
        col(ImGuiCol_Separator, kLine);
        col(ImGuiCol_Tab, kPanel);
        col(ImGuiCol_TabHovered, alpha(kOrange, 0.55f));
        col(ImGuiCol_TabSelected, alpha(kOrange, 0.80f));
        col(ImGuiCol_TabSelectedOverline, kOrange);
        col(ImGuiCol_TableHeaderBg, kPanel);
        col(ImGuiCol_TableRowBgAlt, ImVec4(1, 1, 1, 0.02f));
        col(ImGuiCol_TableBorderLight, kLine);
        col(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
        col(ImGuiCol_ScrollbarGrab, kLine);
        col(ImGuiCol_ScrollbarGrabHovered, kPanelHi);
        col(ImGuiCol_ScrollbarGrabActive, kOrange);
        col(ImGuiCol_NavCursor, kOrange);
        col(ImGuiCol_ResizeGrip, ImVec4(0, 0, 0, 0));
    }
    ~F1Style()
    {
        ImGui::PopStyleColor(colors);
        ImGui::PopStyleVar(vars);
    }
};

// [screenshot] "Screenshot saved ..." for 2.5 s, top centre
bool PS2SettingsOverlay::shotNoticeActive()
{
    std::lock_guard<std::mutex> lk(g_shotMx);
    if (g_shotMsgUntil == -2.0) g_shotMsgUntil = GetTime() + 2.5;
    return g_shotMsgUntil > GetTime();
}
void PS2SettingsOverlay::drawShotNotice()
{
    if (!shotNoticeActive()) return;
    std::string msg;
    { std::lock_guard<std::mutex> lk(g_shotMx); msg = g_shotMsg; }
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, 8.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 12.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("##shotnotice", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                              ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings))
        ImGui::TextUnformatted(msg.c_str());
    ImGui::End();
    ImGui::PopStyleVar(3);
}

// The frame-rate meter, top-left: the game's frames per second and its speed against what it aims for
// (CTTR is PAL: 25 in a race, 50 in some menus).
void PS2SettingsOverlay::drawMeter()
{
    const PerfHudStats st = ps2xPerfHudStats();
    const double shown = st.pictures > 0.5 ? st.pictures : st.fps;
    const double aim = st.fps > 37.5 ? 50.0 : 25.0;
    const double speed = st.fps > 0.5 ? std::min(100.0, st.fps / aim * 100.0 + 0.5) : 0.0;
    ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 16.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("##meter", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::Text("%.0f fps   speed %.0f%%", shown, speed);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

// One Settings window for the whole game ([menu], [cttrmenu]): F1 and Shift+Tab (or the pad combo) open and close it,
// so does Esc while it is open. The native renderer's options apply at once; everything is saved to
// savedata\settings.toml when the window closes (and at exit).
void PS2SettingsOverlay::drawF1Window()
{
    if (!s_nativeF1) return;
    using namespace cttrui;
    char label[160];
    static int s_page = [](){ const char *v = std::getenv("PS2X_F1PAGE"); return v && v[0] ? std::clamp(std::atoi(v), 0, 6) : 0; }();   // tests
    enum { PageDisplay, PageGraphics, PageAudio, PageControls, PageCheats, PageMods, PageHelp, PageCount };
    static const char *const kPages[PageCount] = {"Display", "Graphics", "Audio", "Controls", "Cheats", "Mods", "Help"};
    static const char *const kPageNotes[PageCount] = {
        "Resolution, picture shape, window and on-screen readouts.",
        "Texture filtering.",
        "Sound volume.",
        "Controller, button bindings and stick dead zone.",
        "The game's own cheat codes, and progress unlocks.",
        "Changes to the game itself, beyond the PS2 original. All off by default.",
        "Keys, what each page does, and where files are kept."};

    F1Style style;
    const ImVec2 vp = ImGui::GetMainViewport()->Size;
    const ImVec2 size(std::min(920.0f, vp.x * 0.96f), std::min(640.0f, vp.y * 0.94f));
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    if (!ImGui::Begin("##cttrsettings", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::End();
        return;
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const float headerH = 66.0f, footerH = 42.0f, sideW = 196.0f;
    const ImU32 cOrange = ImGui::GetColorU32(kOrange), cMuted = ImGui::GetColorU32(kMuted), cText = ImGui::GetColorU32(kText);

    // ---------------------------------------------------------------- header: title, accent line, close button
    dl->AddRectFilled(wp, ImVec2(wp.x + size.x, wp.y + headerH), ImGui::GetColorU32(kSide), 12.0f, ImDrawFlags_RoundCornersTop);
    dl->AddRectFilledMultiColor(ImVec2(wp.x, wp.y + headerH - 2.0f), ImVec2(wp.x + size.x, wp.y + headerH),
                                cOrange, ImGui::GetColorU32(alpha(kGold, 0.0f)), ImGui::GetColorU32(alpha(kGold, 0.0f)), cOrange);
    {
        ImGui::PushFont(s_fontBlack, 24.0f);
        const char *title = "CRASH TAG TEAM RACING";
        const ImVec2 ts = ImGui::CalcTextSize(title);
        const ImVec2 tp(wp.x + 26.0f, wp.y + (headerH - ts.y) * 0.5f - 1.0f);
        dl->AddText(ImVec2(tp.x + 1.5f, tp.y + 1.5f), IM_COL32(0, 0, 0, 140), title);
        dl->AddText(tp, cOrange, title);
        ImGui::PopFont();
        ImGui::PushFont(s_fontSemibold, 0.0f);
        const ImVec2 ss = ImGui::CalcTextSize("SETTINGS");
        dl->AddText(ImVec2(tp.x + ts.x + 14.0f, wp.y + (headerH - ss.y) * 0.5f), cMuted, "SETTINGS");
        ImGui::PopFont();
    }
    {   // close (X drawn with lines: no icon font needed)
        const float bs = 34.0f;
        ImGui::SetCursorPos(ImVec2(size.x - bs - 16.0f, (headerH - bs) * 0.5f));
        const bool hit = ImGui::InvisibleButton("##close", ImVec2(bs, bs));
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        if (ImGui::IsItemHovered()) dl->AddRectFilled(a, b, ImGui::GetColorU32(kPanelHi), 8.0f);
        const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        const ImU32 xc = ImGui::IsItemHovered() ? cText : cMuted;
        dl->AddLine(ImVec2(c.x - 6, c.y - 6), ImVec2(c.x + 6, c.y + 6), xc, 2.0f);
        dl->AddLine(ImVec2(c.x - 6, c.y + 6), ImVec2(c.x + 6, c.y - 6), xc, 2.0f);
        ImGui::SetItemTooltip("Close (F1, Shift+Tab or Esc)");
        if (hit) s_nativeF1 = false;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && m_captureAction < 0 && m_captureComboSlot < 0)
        s_nativeF1 = false;

    const float bodyH = size.y - headerH - footerH;
    // ---------------------------------------------------------------- sidebar
    ImGui::SetCursorPos(ImVec2(0.0f, headerH));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kSide);
    ImGui::BeginChild("##side", ImVec2(sideW, bodyH), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    {
        ImDrawList *sd = ImGui::GetWindowDrawList();
        ImGui::SetCursorPos(ImVec2(12.0f, 16.0f));
        ImGui::PushFont(s_fontSemibold, 0.0f);
        for (int i = 0; i < PageCount; ++i)
        {
            ImGui::SetCursorPosX(12.0f);
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##nav", ImVec2(sideW - 24.0f, 40.0f))) s_page = i;
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            const bool sel = s_page == i, hov = ImGui::IsItemHovered();
            if (sel) sd->AddRectFilled(a, b, ImGui::GetColorU32(alpha(kOrange, 0.14f)), 8.0f);
            else if (hov) sd->AddRectFilled(a, b, ImGui::GetColorU32(kPanel), 8.0f);
            if (sel) sd->AddRectFilled(ImVec2(a.x, a.y + 9.0f), ImVec2(a.x + 3.0f, b.y - 9.0f), cOrange, 2.0f);
            const float th = ImGui::GetTextLineHeight();
            sd->AddText(ImVec2(a.x + 18.0f, a.y + (40.0f - th) * 0.5f), sel ? cOrange : (hov ? cText : ImGui::GetColorU32(alpha(kText, 0.78f))), kPages[i]);
            ImGui::PopID();
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
        }
        ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ---------------------------------------------------------------- page
    ImGui::SetCursorPos(ImVec2(sideW, headerH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(30.0f, 22.0f));
    ImGui::BeginChild("##page", ImVec2(size.x - sideW, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();

    // page title + note
    ImGui::PushFont(s_fontBlack, 22.0f);
    ImGui::TextUnformatted(kPages[s_page]);
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted(kPageNotes[s_page]);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    const float ctrlW = std::min(300.0f, ImGui::GetContentRegionAvail().x * 0.48f);
    bool inTable = false;
    auto endTable = [&]() { if (inTable) { ImGui::EndTable(); inTable = false; } };
    auto section = [&](const char *name) {
        endTable();
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::PushFont(s_fontSemibold, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, kOrange);
        ImGui::TextUnformatted(name);
        ImGui::PopStyleColor();
        ImGui::PopFont();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y - 2.0f), ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y - 2.0f), ImGui::GetColorU32(kLine), 1.0f);
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        ImGui::BeginTable(name, 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_NoSavedSettings);
        ImGui::TableSetupColumn("##l", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##c", ImGuiTableColumnFlags_WidthFixed, ctrlW);
        inTable = true;
    };
    // a row: the name and a short note on the left; the control goes in the right-hand cell, full width
    auto row = [&](const char *name, const char *note) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(name);
        if (note && note[0])
        {
            ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 16.0f);
            ImGui::TextUnformatted(note);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
    };
    // an on/off switch, right-aligned in the control cell
    auto toggle = [&](const char *id, bool *v) -> bool {
        const float w = 46.0f, h = 24.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.0f);
        const bool hit = ImGui::InvisibleButton(id, ImVec2(w, h));
        if (hit) *v = !*v;
        const ImVec2 a = ImGui::GetItemRectMin();
        ImDrawList *d = ImGui::GetWindowDrawList();
        const bool hov = ImGui::IsItemHovered();
        d->AddRectFilled(a, ImVec2(a.x + w, a.y + h), ImGui::GetColorU32(*v ? (hov ? kGold : kOrange) : (hov ? kPanelHi : kPanel)), h * 0.5f);
        if (!*v) d->AddRect(a, ImVec2(a.x + w, a.y + h), ImGui::GetColorU32(kLine), h * 0.5f);
        const float r = h * 0.5f - 3.0f, cx = *v ? a.x + w - r - 3.0f : a.x + r + 3.0f;
        d->AddCircleFilled(ImVec2(cx, a.y + h * 0.5f), r, *v ? IM_COL32(24, 18, 8, 255) : ImGui::GetColorU32(kMuted));
        return hit;
    };
    auto accentButton = [&](const char *text, float w) -> bool {
        ImGui::PushStyleColor(ImGuiCol_Button, kOrange);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kGold);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kGold);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.10f, 0.07f, 0.03f, 1.0f));
        ImGui::PushFont(s_fontSemibold, 0.0f);
        const bool r = ImGui::Button(text, ImVec2(w, 0.0f));
        ImGui::PopFont();
        ImGui::PopStyleColor(4);
        return r;
    };
    auto rightButton = [&](const char *text, bool enabled) -> bool {
        const float w = 120.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w);
        if (!enabled) ImGui::BeginDisabled();
        const bool r = accentButton(text, w);
        if (!enabled) ImGui::EndDisabled();
        return r;
    };

    switch (s_page)
    {
    // ================================================================ Display
    case PageDisplay:
    {
        section("Picture");
        {   // [cttrres] paraLLEl-GS super-sampling = the internal resolution ("3x" = 8 samples shown at 2x)
            // PCSX2-style names. paraLLEl-GS renders at 1x, 2x or 4x only (4/16 samples); setting 3 is 2x with 8 samples.
            static const char *const kRes[] = {"Native (PS2) (Default)", "2x Native (~1080px/FHD)", "2x Native + 8x Smoothing (~1080px/FHD)",
                                               "4x Native (~2160px/4K UHD)"};
            row("Internal resolution", "How finely the game is drawn. Native is the PS2's 640 x 512; 2x is 1280 x 1024, 4x is 2560 x 2048.");
            int rs = std::clamp(m_settings.renderScale, 1, 4) - 1;
            if (ImGui::Combo("##res", &rs, kRes, 4))
            {
                m_settings.renderScale = rs + 1;
                ps2x_pgs::setRenderScale(m_settings.renderScale);
                m_dirty = true;
            }
        }
        {   // [cttraspect] PCSX2's Aspect Ratio list
            // Listed in PCSX2's order and wording; the stored mode numbers are unchanged (0 Auto, 1 Fit, 2 4:3, 3 16:9, 4 10:7).
            static const char *const kAsp[] = {"Fit to Window / Fullscreen", "Auto Standard (4:3 Interlaced / 3:2 Progressive)",
                                               "Standard (4:3)", "Widescreen (16:9)", "Native/Full (10:7)"};
            static const int kItemToMode[5] = {1, 0, 2, 3, 4};
            static const int kModeToItem[5] = {1, 0, 2, 3, 4};
            row("Aspect ratio", "The shape of the picture. Auto Standard is 4:3, as the game shows on a TV (it is interlaced).");
            int item = kModeToItem[std::clamp(m_settings.cttrAspect, 0, 4)];
            if (ImGui::Combo("##asp", &item, kAsp, 5))
            {
                const int am = kItemToMode[std::clamp(item, 0, 4)];
                m_settings.cttrAspect = am;
                extern std::atomic<int> g_cttrAspectMode;
                g_cttrAspectMode.store(am);
                m_dirty = true;
            }
        }
        {   // [movieaspect] the logo and story movies (Bink, 4:3 pictures)
            static const char *const kMov[] = {"Same as the picture", "Always 4:3 (their original shape)", "Fill the window"};
            row("Movies", "The shape of the logo and story movies. They are made in 4:3, so with a 16:9 picture they are stretched unless this says 4:3.");
            int mi = std::clamp(m_settings.cttrMovieAspect, 0, 2);
            if (ImGui::Combo("##movasp", &mi, kMov, 3))
            {
                m_settings.cttrMovieAspect = mi;
                extern std::atomic<int> g_cttrMovieAspect;
                g_cttrMovieAspect.store(mi);
                m_dirty = true;
            }
        }
        section("Window");
        row("Full screen", "F11 switches at any time; F8 minimises the game.");
        if (toggle("##fs", &m_settings.fullscreen))
        {
            ps2xSetFullscreen(m_settings.fullscreen, m_settings.windowW, m_settings.windowH);
            m_dirty = true;
        }
        if (g_ps2xIsCttr)
        {   // [focuspause]
            row("Pause when in background", "Freezes the game and its sound while you use another window. Click the game to carry on.");
            if (toggle("##focuspause", &m_settings.pauseUnfocused)) m_dirty = true;
        }
        {
            row("Monitor", "The screen the game opens on.");
            const int count = GetMonitorCount();
            if (m_settings.display > count) m_settings.display = 0;
            if (m_settings.display == 0) std::snprintf(label, sizeof label, "Chosen by Windows");
            else std::snprintf(label, sizeof label, "%d: %s", m_settings.display, GetMonitorName(m_settings.display - 1));
            if (ImGui::BeginCombo("##mon", label))
            {
                int pick = -1;
                if (ImGui::Selectable("Chosen by Windows", m_settings.display == 0)) pick = 0;
                for (int i = 0; i < count; i++)
                {
                    std::snprintf(label, sizeof label, "%d: %s", i + 1, GetMonitorName(i));
                    if (ImGui::Selectable(label, m_settings.display == i + 1)) pick = i + 1;
                }
                ImGui::EndCombo();
                if (pick >= 0 && pick != m_settings.display)
                {
                    m_settings.display = pick;
                    if (pick > 0)
                    {   // move now: out of full screen, to that monitor, and back at its size
                        const bool fs = IsWindowFullscreen();
                        if (fs) ps2xSetFullscreen(false, m_settings.windowW, m_settings.windowH);
                        SetWindowMonitor(pick - 1);
                        if (fs) ps2xSetFullscreen(true, m_settings.windowW, m_settings.windowH);
                    }
                    m_dirty = true;
                }
            }
        }
        {   // window-size presets (windowed only); larger than the screen is fitted to it ([winfit])
            static const int kSz[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3440, 1440}, {3840, 2160}, {1024, 768}};
            static const char *kSzNames[] = {"1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3440 x 1440 (ultrawide)", "3840 x 2160 (4K)", "1024 x 768 (4:3)"};
            row("Window size", "Windowed mode only. Sizes bigger than the screen are fitted to it.");
            const int w = GetScreenWidth(), h = GetScreenHeight();
            int cur = -1;
            for (int i = 0; i < 7; ++i) if (kSz[i][0] == w && kSz[i][1] == h) cur = i;
            std::snprintf(label, sizeof label, "%d x %d", w, h);
            const bool fs = IsWindowFullscreen();
            if (fs) ImGui::BeginDisabled();
            if (ImGui::BeginCombo("##wsz", cur >= 0 ? kSzNames[cur] : label))
            {
                for (int i = 0; i < 7; ++i)
                    if (ImGui::Selectable(kSzNames[i], i == cur) && i != cur)
                    {
                        ps2xApplyWindowSize(kSz[i][0], kSz[i][1]);
                        m_settings.windowW = kSz[i][0]; m_settings.windowH = kSz[i][1];
                        m_dirty = true;
                    }
                ImGui::EndCombo();
            }
            if (fs) ImGui::EndDisabled();
        }
        if (g_ps2xIsCttr)
        {   // [skipintro]
            section("Start-up");
            row("Skip intro movies", "Skips the logo movies at start-up (the game's own option). Starts with the next launch.");
            if (toggle("##skipintro", &m_settings.cttrSkipIntro)) m_dirty = true;
            // [cttrlang] the PAL disc's six languages; the game reads the console language once, at start-up
            static const char *const kLangs[] = {"English", "Fran\xc3\xa7" "ais", "Espa\xc3\xb1ol", "Deutsch", "Italiano", "Nederlands"};
            row("Language", "Text and speech, from the game's own translations. Starts with the next launch.");
            const int li = std::clamp(m_settings.cttrLanguage, 1, 6) - 1;
            if (ImGui::BeginCombo("##lang", kLangs[li]))
            {
                for (int i = 0; i < 6; ++i)
                    if (ImGui::Selectable(kLangs[i], i == li) && i != li) { m_settings.cttrLanguage = i + 1; m_dirty = true; }
                ImGui::EndCombo();
            }
        }
        section("On-screen readouts");
        row("Frame rate", "Frames per second and game speed in the top-left corner.");
        if (toggle("##meter", &m_settings.meter)) m_dirty = true;
        row("Performance readout", "CPU, GPU and memory use. F10 switches it at any time.");
        if (toggle("##perf", &m_settings.perfOverlay)) m_dirty = true;
        if (m_settings.perfOverlay)
        {
            static const char *kCorners[] = {"Top left", "Top right", "Bottom left", "Bottom right"};
            row("Readout position", nullptr);
            if (ImGui::Combo("##corner", &m_settings.perfCorner, kCorners, 4)) m_dirty = true;
        }
        break;
    }
    // ================================================================ Graphics
    case PageGraphics:
    {
        section("Textures");
        {   // [aniso] paraLLEl-GS: extra bilinear taps along slanted surfaces (3D geometry only; HUD and text untouched)
            static const int kAniso[] = {1, 2, 4, 8, 16};
            static const char *const kAnisoN[] = {"Off (PS2)", "2x", "4x", "8x", "16x"};
            row("Anisotropic filtering", "Keeps roads and ground sharp as they stretch into the distance. 8x and 16x cost a little GPU time.");
            int ai = 0;
            for (int i = 0; i < 5; ++i) if (kAniso[i] == m_settings.pgsAniso) ai = i;
            if (ImGui::Combo("##aniso", &ai, kAnisoN, 5))
            {
                m_settings.pgsAniso = kAniso[ai];
                ps2x_pgs::setAniso(m_settings.pgsAniso);
                m_dirty = true;
            }
        }
        row("Smooth textures", "Filters textures the game draws as hard pixels, like PCSX2's forced bilinear. Off is the console look.");
        if (toggle("##bilin", &m_settings.forceBilinear))
        {
            extern void ps2xSetForceBilinear(bool);
            ps2xSetForceBilinear(m_settings.forceBilinear);
            m_dirty = true;
        }
        break;
    }
    // ================================================================ Audio
    case PageAudio:
    {
        section("Volume");
        row("Master volume", "Music, effects and voices together.");
        int p = (int)std::lround(m_settings.masterVolume * 100.0f);
        if (ImGui::SliderInt("##vol", &p, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) { m_settings.masterVolume = p / 100.0f; m_dirty = true; }
        break;
    }
    // ================================================================ Controls
    case PageControls:
    {
        endTable();
        m_f1Mode = true;
        drawControllersTab();
        m_f1Mode = false;
        break;
    }
    // ================================================================ Cheats
    case PageCheats:
    {   // [cttrcheats] game side: cttrCheatFrame in game_overrides.cpp
        extern std::atomic<uint32_t> g_cttrCheatWant, g_cttrCheatLive, g_cttrCheatOff, g_cttrCheatOnce;
        extern std::atomic<int32_t> g_cttrCheatCoinsSet, g_cttrCheatCrystalsSet, g_cttrCheatCoins, g_cttrCheatCrystals;
        extern std::atomic<int32_t> g_cttrCheatSkins, g_cttrCheatSkinsTotal, g_cttrCheatCars, g_cttrCheatDors;
        struct Code { const char *name, *keys; int bit; };
        static const Code kCodes[] = {
            // notes = when the game picks the flag up (from the ELF: speed per frame in the car code, toy cars at
            // vehicle load, chicken/Japan at character load, KO in the collision code, free camera when the level's
            // camera system is built (Controllers/CameraMap "Toggle" = Select; 0x2a2e30 skips it with 2+ racers, so on foot only), bumper camera in the race camera input)
            {"High speed mode", "Your car goes faster. Works straight away in races.", 3},
            {"Toy block cars", "Cars are built from toy blocks. Starts with the next race.", 2},
            {"Japanese Crash outfit", "Puts Crash in his outfit from the Japanese release (shows after the next loading screen). "
                                      "It is saved like any outfit: pick another Crash outfit, or switch this off, to change back.", 6},
            {"Chicken-headed people", "The park visitors walking around the hub get chicken heads (not Crash or the racers). Starts after the next loading screen.", 5},
            {"Instant KO on rival collision", "Bumping into a rival car wrecks it. Works straight away in races.", 4},
            {"Disable HUD", "Hides the on-screen HUD straight away.", 0},
            {"Free camera", "On foot only. Press Select to detach the camera, then move it with the sticks and "
                            "L1/R1. Starts after the next loading screen.", 1},
            {"Bumper camera", "A hidden code. In a race, hold L3 + L1 and press Select for a camera on the bumper; "
                              "R1 or Triangle goes back.", 7},
        };
        section("Cheat codes");
        const uint32_t live = g_cttrCheatLive.load();
        for (const Code &c : kCodes)
        {
            const uint32_t b = 1u << c.bit;
            row(c.name, c.keys);
            bool on = (live & b) != 0;
            ImGui::PushID(c.bit);
            if (toggle("##t", &on))
            {
                if (on) { if (c.bit != 6) m_settings.cttrCheats |= (int)b; g_cttrCheatWant.fetch_or(b); }   // [japanoutfit] not kept
                else { m_settings.cttrCheats &= ~(int)b; g_cttrCheatWant.fetch_and(~b); g_cttrCheatOff.fetch_or(b); }
                m_dirty = true;
            }
            ImGui::PopID();
        }
        endTable();
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted("The same as entering the game's own L1+R1 codes. Cheats switched on here stay on, including the next time you play.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();

        const int coins = g_cttrCheatCoins.load(), crystals = g_cttrCheatCrystals.load();
        const bool ready = coins >= 0;
        section("Progress");
        static int s_coins = 0, s_crystals = 96;
        static bool s_seeded = false;   // the coin box starts at what you have
        if (ready && !s_seeded) { s_seeded = true; s_coins = coins; }
        auto status = [&](const char *fmt, int a, int b) { std::snprintf(label, sizeof label, fmt, a, b); return label; };
        row("Unlock all outfits", ready ? status("%d of %d outfits unlocked.", g_cttrCheatSkins.load(), g_cttrCheatSkinsTotal.load()) : "Every character's outfits, as if bought.");
        if (rightButton("Unlock##skins", ready)) g_cttrCheatOnce.fetch_or(1u << 0);
        row("Unlock all vehicles", ready ? status("%d of %d cars unlocked (8 characters, 3 cars each).", g_cttrCheatCars.load(), 24) : "Every character's three cars.");
        if (rightButton("Unlock##cars", ready)) g_cttrCheatOnce.fetch_or(1u << 1);
        row("Complete all Die-O-Ramas", ready ? status("%d of %d done. All 34 also unlock Crash's diaper outfit.", g_cttrCheatDors.load(), 34)
                                              : "All 34. Finding every one also unlocks Crash's diaper outfit.");
        if (rightButton("Complete##dors", ready)) g_cttrCheatOnce.fetch_or(1u << 2);
        auto amount = [&](const char *id, int *v, int maxV, const char *btn, uint32_t once, std::atomic<int32_t> &target) {
            const float bw = 120.0f, gap = 8.0f;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - bw - gap);
            if (!ready) ImGui::BeginDisabled();
            ImGui::InputInt(id, v, 0, 0);
            *v = std::clamp(*v, 0, maxV);
            ImGui::SameLine(0.0f, gap);
            if (accentButton(btn, bw)) { target.store(*v); g_cttrCheatOnce.fetch_or(1u << once); }
            if (!ready) ImGui::EndDisabled();
        };
        row("Power crystals", ready ? status("You have %d. The game has %d in all.", crystals, 96) : "Set how many you have. The game has 96 in all.");
        amount("##cry", &s_crystals, 999, "Set##cry", 4, g_cttrCheatCrystalsSet);
        row("Wumpa coins", ready ? status("You have %d.", coins, 0) : "Set how many you have.");
        amount("##coin", &s_coins, 999999, "Set##coin", 3, g_cttrCheatCoinsSet);
        endTable();
        ImGui::PushStyleColor(ImGuiCol_Text, ready ? kMuted : kGold);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(ready ? "Progress changes go into your save the next time the game saves (it auto-saves as you play)."
                                     : "Load your save first: progress changes apply to the profile that is playing.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        break;
    }
    // ================================================================ Mods
    case PageMods:
    {   // [cttrmods] game side: game_overrides.cpp (cttrWideView / cttrWideSetFov, the frame limiter hook)
        extern std::atomic<int> g_cttrModWide;
        section("Picture");
        row("True widescreen (16:9)", "Shows more of the track at the sides instead of stretching the 4:3 picture, "
                                      "and shows the game in 16:9. The menus and the HUD are still drawn for 4:3, so "
                                      "they look a little wide. Movies stay 4:3 (see Display > Movies).");
        if (toggle("##modwide", &m_settings.modWidescreen)) { g_cttrModWide.store(m_settings.modWidescreen ? 1 : 0); m_dirty = true; }
        break;
    }
    // ================================================================ Help
    case PageHelp:
    {
        endTable();
        auto help = [&](const char *head, const char *body) {
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            ImGui::PushFont(s_fontSemibold, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, kOrange);
            ImGui::TextUnformatted(head);
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(body);
            ImGui::PopTextWrapPos();
        };
        help("Keys", "F1 or Shift+Tab (pad: Select+Start): open / close this window. Esc also closes it. The game ignores "
                     "your input while it is open.\nF11: full screen / window.   F8: minimise.   F10: performance readout.   F12: screenshot (saved in "
                     "Pictures\\Crash Tag Team Racing).");
        help("Display", "Internal resolution: how finely the game is drawn (Native = 640 x 512, up to 4x). Aspect ratio: "
                        "the shape of the picture. Full screen, monitor and window size: where the game shows. Readouts: "
                        "performance numbers on screen.");
        help("Graphics", "Anisotropic filtering: sharper textures on surfaces seen at a slant. Smooth textures: filters "
                         "textures the game draws as hard pixels.");
        help("Audio", "The game's volume (music, effects and voices together).");
        help("Controls", "Choose the controller, change bindings (click a binding, then press the new key or button), set "
                         "the stick dead zone and test the pad.");
        help("Cheats", "Cheat codes: the game's own codes as switches. Progress: unlock outfits, vehicles and Die-O-Ramas, "
                       "and set your crystals and coins. Progress is kept by the game's next save.");
        help("Mods", "Changes beyond the PS2 original, all off by default. True widescreen: a wider 3D view shown in 16:9.");
        help("Files", "savedata\\settings.toml: these settings.   savedata\\BESLES-53439-*: your memory card save.");
        help("If the game is not smooth", "Lower Internal resolution first, then Anisotropic filtering, and close other "
                                          "programs that use the graphics card.");
        break;
    }
    }
    endTable();
    {   // tests: PS2X_F1SCROLL=1 shows the bottom of the page
        static const bool s_scroll = [](){ const char *v = std::getenv("PS2X_F1SCROLL"); return v && v[0] == '1'; }();
        if (s_scroll) ImGui::SetScrollY(ImGui::GetScrollMaxY());
    }
    ImGui::EndChild();

    // ---------------------------------------------------------------- footer
    {
        const float fy = wp.y + size.y - footerH;
        dl->AddLine(ImVec2(wp.x + sideW, fy), ImVec2(wp.x + size.x, fy), ImGui::GetColorU32(kLine), 1.0f);
        dl->AddRectFilled(ImVec2(wp.x, fy), ImVec2(wp.x + sideW, wp.y + size.y), ImGui::GetColorU32(kSide), 12.0f, ImDrawFlags_RoundCornersBottomLeft);
        const float th = ImGui::GetTextLineHeight();
        dl->AddText(ImVec2(wp.x + sideW + 30.0f, fy + (footerH - th) * 0.5f), cMuted, "Changes apply straight away and are saved when you close this window.");
        const char *keys = "F1  \xC2\xB7  Esc to close";
        const float kw = ImGui::CalcTextSize(keys).x;
        dl->AddText(ImVec2(wp.x + size.x - kw - 26.0f, fy + (footerH - th) * 0.5f), cMuted, keys);
    }
    ImGui::End();
}
#endif

void PS2SettingsOverlay::draw(PS2Runtime &runtime)
{
    if (!m_initialized)
        return;

    // [launcher] In-game overlay master switch: when disabled the panel never
    // deploys (toggle combos are also skipped below via m_visible staying false),
    // so a play-session config can keep the HUD out entirely.
    if (!m_settings.overlayEnabled)
        return;

    if (std::getenv("PS2X_COMBO_DIAG") && m_selectedDevice >= 0 &&
        m_selectedDevice < static_cast<int>(m_deviceList.size()))
    {
        static int s_lastDev = -2;
        if (s_lastDev != m_selectedDevice)
        {
            s_lastDev = m_selectedDevice;
            std::fprintf(stderr, "[combo] selectedDevice=%d kind=%d glfwSlot=%d evdev=%d name=%s\n",
                         m_selectedDevice, (int)m_deviceList[m_selectedDevice].kind,
                         m_deviceList[m_selectedDevice].glfwSlot,
                         m_deviceList[m_selectedDevice].isEvdev ? 1 : 0,
                         m_deviceList[m_selectedDevice].name.c_str());
        }
    }

    // --- Toggle: keyboard combo (configurable; all bound keys held + edge on last) ---
    if (!m_settings.overlayKeys.empty())
    {
        bool allDown = true;
        for (int k : m_settings.overlayKeys)
            if (!IsKeyDown(k)) { allDown = false; break; }
        const int lastKey = m_settings.overlayKeys.back();
        if (allDown && IsKeyPressed(lastKey))
            toggleVisible();
    }

    // --- F11: toggle fullscreen / windowed ---
    if (IsKeyPressed(KEY_F11))
    {
        m_settings.fullscreen = !m_settings.fullscreen;
        ps2xSetFullscreen(m_settings.fullscreen, m_settings.windowW, m_settings.windowH);
        m_dirty = true;
    }

    // --- [focuspause] CTTR: freeze the game while the window is in the background (setting "Pause when in background").
    // The vblank worker stops delivering vblanks (Interrupt.cpp), so every guest thread that waits on one -- the game's
    // frame limiter, its sound and timer threads -- stops where it is, and the SPU2 output (CttrIop.cpp) plays silence
    // without advancing its voices or the music stream. Clicking the window resumes exactly there. Only after the
    // window has had focus once, so a game started behind another window still boots. PS2X_FOCUSPAUSE=0 = never.
    {
        static const bool s_env = [](){ const char *v = std::getenv("PS2X_FOCUSPAUSE"); return !(v && v[0] == '0'); }();
        static bool s_hadFocus = false;
        bool focused = IsWindowFocused() && !IsWindowMinimized();
        // PS2X_FOCUSPAUSE_SIM=<file>: scripted tests -- real focus is ignored, the window counts as in the background
        // exactly while <file> exists
        static const char *s_sim = std::getenv("PS2X_FOCUSPAUSE_SIM");
        if (s_sim && s_sim[0])
        {
            static double s_lastCheck = -1.0;
            static bool s_simBg = false;
            if (GetTime() - s_lastCheck > 0.25) { s_lastCheck = GetTime(); s_simBg = std::filesystem::exists(s_sim); }
            focused = !s_simBg;
        }
        if (focused) s_hadFocus = true;
        const bool pause = g_ps2xIsCttr && s_env && m_settings.pauseUnfocused && s_hadFocus && !focused;
        if (pause != g_ps2xFocusPaused.load(std::memory_order_relaxed))
        {
            g_ps2xFocusPaused.store(pause, std::memory_order_relaxed);
            std::fprintf(stderr, "[focuspause] %s\n", pause ? "paused (window in background)" : "resumed");
        }
    }

    // --- [winmem] CTTR: remember the window's size and position (dragged or resized by hand), once they have been still
    // for a second, windowed only. Saved with the settings at exit; applied at the next start.
    if (g_ps2xIsCttr && !IsWindowFullscreen() && !IsWindowMinimized() && !IsWindowMaximized() && IsWindowReady())
    {
        static int s_lx = -1, s_ly = -1, s_lw = -1, s_lh = -1;
        static double s_since = -1.0;
        const Vector2 p = GetWindowPosition();
        const int x = int(p.x), y = int(p.y), w = GetScreenWidth(), h = GetScreenHeight();
        if (x != s_lx || y != s_ly || w != s_lw || h != s_lh) { s_lx = x; s_ly = y; s_lw = w; s_lh = h; s_since = GetTime(); }
        else if (s_since >= 0.0 && GetTime() - s_since > 1.0)
        {
            s_since = -1.0;
            if (w >= 320 && h >= 240 &&
                (x != m_settings.windowX || y != m_settings.windowY || w != m_settings.windowW || h != m_settings.windowH))
            {
                m_settings.windowX = x; m_settings.windowY = y; m_settings.windowW = w; m_settings.windowH = h;
                s_winX = x; s_winY = y;
                m_dirty = true;
            }
        }
    }

    // --- F8 / focus loss: minimise ([minimize]) ---
    // An exclusive-fullscreen window cannot be minimised (and Alt+Tab leaves it stuck on top), so drop to a window
    // first, minimise, and go back to fullscreen once the window is restored and focused. Losing focus for half a
    // second in fullscreen (Alt+Tab, Windows key) does the same when PS2X_FOCUSMIN=1 (off by default: it also fired on
    // a click on another monitor, e.g. to take a screenshot).
    {
        static const bool s_focusMin = [](){ const char *v = std::getenv("PS2X_FOCUSMIN"); return v && v[0] == '1'; }();
        static bool s_wasFs = false;
        static double s_unfocusedSince = -1.0;
        const bool fs = IsWindowFullscreen();
        if (fs && !IsWindowFocused()) { if (s_unfocusedSince < 0.0) s_unfocusedSince = GetTime(); }
        else s_unfocusedSince = -1.0;
        const bool lostFocus = s_focusMin && fs && s_unfocusedSince >= 0.0 && GetTime() - s_unfocusedSince > 0.5;
        const bool btnMin = s_minimizeRequested; s_minimizeRequested = false;   // [winbtn]
        if (IsKeyPressed(KEY_F8) || lostFocus || btnMin)
        {
            if (fs) { ps2xSetFullscreen(false, m_settings.windowW, m_settings.windowH); s_wasFs = true; }
            MinimizeWindow();
            s_unfocusedSince = -1.0;
        }
        else if (s_wasFs && !IsWindowMinimized() && IsWindowFocused())
        {
            ps2xSetFullscreen(true, m_settings.windowW, m_settings.windowH);
            s_wasFs = false;
        }
    }

    // --- F9: debug capture ([captrigger]) -- only when started with PS2X_PGS_CAPTRIGGER=<file>: creates that file, and the
    // renderer then records the next frames' GS stream, scanouts and VRAM (the fog-debug launcher sets this up) ---
    if (IsKeyPressed(KEY_F9) && std::getenv("PS2X_BIGKICK_TAG"))
    {   // [bigkicktag] arm the VU1 kick dump (ps2_vu1.cpp)
        g_ps2xBigkickArm.store(1);
        std::fprintf(stderr, "[bigkick] F9: armed\n");
    }
    if (IsKeyPressed(KEY_F9))
        if (const char *trig = std::getenv("PS2X_PGS_CAPTRIGGER"); trig && trig[0])
            if (FILE *t = std::fopen(trig, "wb"))
            {
                std::fclose(t);
                std::fprintf(stderr, "[captrigger] F9: capture requested\n");
            }

    // --- F12: screenshot of the game picture ([screenshot]) ---
    if (g_ps2xIsCttr && IsKeyPressed(KEY_F12))
        g_ps2xShotRequest.store(true);

    // --- F10: performance overlay on/off ([perfhud]) ---
    if (IsKeyPressed(KEY_F10))
    {
        m_settings.perfOverlay = !m_settings.perfOverlay;
        m_dirty = true;
    }
#ifdef PS2X_HAVE_NATIVE_GS
    // --- F1: the native renderer's settings window ([native_gs]); the game gets no input while it is open ---
    {
        static bool s_wasOpen = false;
        static bool s_init = [] { const char *v = std::getenv("PS2X_NATIVE_GS_F1"); if (v && v[0] == '1') s_nativeF1 = true; return true; }();
        (void)s_init;
        {   // tests: PS2X_F1_AT=<sec> opens the window once that long after start
            static const double s_at = [](){ const char *v = std::getenv("PS2X_F1_AT"); return v && v[0] ? std::atof(v) : -1.0; }();
            static bool s_atDone = false;
            if (s_at > 0.0 && !s_atDone && GetTime() >= s_at) { s_atDone = true; s_nativeF1 = true; }
        }
        if (IsKeyPressed(KEY_F1))
        {
            s_nativeF1 = !s_nativeF1;
            std::fprintf(stderr, "[native_gs] F1: renderer settings %s\n", s_nativeF1 ? "open" : "closed");
        }
        if (s_nativeF1 != s_wasOpen)
        {
            ps2_stubs::PadConfig::setInputSuspended(s_nativeF1 || m_visible);
            if (!s_nativeF1) { saveSettings(); ps2_stubs::PadConfig::instance().save(); }   // [menu] closed: keep what was changed (bindings too)
            s_wasOpen = s_nativeF1;
        }
    }
#endif

    // --- Toggle: gamepad combo (configurable; all bound buttons held) ---
    {
        bool comboDown = false;
        if (!m_settings.overlayPadBtns.empty())
        {
            std::array<uint8_t, 32> cb;
            std::array<float, 6> ca;
            if (m_selectedDevice >= 0 && m_selectedDevice < static_cast<int>(m_deviceList.size()))
                readGamepadStateForDevice(m_deviceList[m_selectedDevice], cb, ca);
            else
                cb = {};
            comboDown = true;
            for (int b : m_settings.overlayPadBtns)
                if (b < 0 || b >= 32 || !cb[b]) { comboDown = false; break; }
        }
        if (comboDown && !m_prevToggleCombo)
            toggleVisible();
        m_prevToggleCombo = comboDown;
    }

    // Apply GPU/audio settings on change
    if (m_dirty)
    {
        PS2AudioBackend::setMasterVolume(m_settings.masterVolume);
        PS2AudioBackend::setMusicVolume(m_settings.musicVolume);
        PS2AudioBackend::setSfxVolume(m_settings.sfxVolume);
        m_settings.gpuRenderer = (m_settings.renderer != Settings::kRendererSoftware);   // [renderer]
        GsGpuRenderer::setEnabled(m_settings.gpuRenderer);
        GsGpuRenderer::setGlow(m_settings.glow);
        m_dirty = false;
    }

    // --- Deploy / retract animation ------------------------------------------
    // m_animT eases toward 1 (open) or 0 (closed) every frame instead of snapping,
    // so closing still needs a few frames to fade + slide away even though m_visible
    // has already flipped to false below. Keep advancing it even while !m_visible so
    // the retract animation can finish; only bail once it's fully settled at 0.
    {
        const float dt = ImGui::GetIO().DeltaTime;
        const float target = m_visible ? 1.0f : 0.0f;
        const float step = dt > 0.0f ? (dt / kOverlayAnimDuration) : 1.0f;
        if (m_animT < target)
            m_animT = std::min(target, m_animT + step);
        else if (m_animT > target)
            m_animT = std::max(target, m_animT - step);
    }

    if (m_animT <= 0.0001f)
    {
        const bool btns = winButtonsWanted();
#ifdef PS2X_HAVE_NATIVE_GS
        const bool nativeWin = s_nativeF1;
#else
        const bool nativeWin = false;
#endif
        if (m_settings.perfOverlay || btns || nativeWin || m_settings.meter || shotNoticeActive())
        {   // [perfhud] panel closed: a frame of its own just for the readout (and the [winbtn] buttons, the F1 window)
            rlImGuiBegin();
            try
            {
                if (m_settings.meter) drawMeter();   // [f1]
                drawShotNotice();                    // [screenshot]
                if (m_settings.perfOverlay) drawPerfHud();
                if (btns) drawWinButtons();
#ifdef PS2X_HAVE_NATIVE_GS
                drawF1Window();
#endif
            }
            catch (...) {}
            rlImGuiEnd();
        }
        return;
    }

    const float animEase = overlayAnimEase(m_animT);

    // --- Draw overlay ---
    // Capture the pre-frame visibility so we can detect a true->false transition caused
    // by the Close button or the title-bar X (both set *p_open directly, bypassing
    // toggleVisible()). Must be read BEFORE the button handlers can flip m_visible.
    const bool wasVisible = m_visible;
    try
    {
        rlImGuiBegin();

        pushDbzTheme();
        DbzThemeScope dbzTheme;   // pops all 40 style colours on scope exit

        // Fade the whole window (and the bindings popup, if open) in/out with the
        // deploy animation.
        ScopedStyleVar animAlpha(ImGuiStyleVar_Alpha, animEase);

        // [fitscale] Clamp the panel to the CURRENT viewport: the logical screen can be
        // anything from the 640x448 classic window to a fullscreen stretch of it, and a
        // fixed 1080px panel hangs off-screen there (user report).
        const float vpW = ImGui::GetMainViewport()->Size.x;
        const float panelW = std::min(1080.0f, std::max(320.0f, vpW * 0.96f));
        const ImVec2 winSize(panelW, 0);
        ImGui::SetNextWindowSize(winSize, ImGuiCond_Always);
        {
            const ImVec2 vp = ImGui::GetMainViewport()->Size;
            const float maxH = std::max(180.0f, vp.y * 0.92f);
            ImGui::SetNextWindowSizeConstraints(ImVec2(panelW, 180), ImVec2(panelW, maxH));
        }

        // Slide the panel in from just above centre as it deploys (and back up as it
        // retracts) instead of popping in place — reads like the HUD "powering up"
        // rather than an abrupt toggle.
        ImVec2 pos = ImGui::GetMainViewport()->GetCenter();
        pos.y += (1.0f - animEase) * -26.0f;
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

        ImGui::Begin("DBZ BT3 // SETTINGS##overlay", &m_visible,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);

        // Ignore clicks while the panel is only mid-retract (fading/sliding out after
        // Close/X was already pressed) so a stray click can't land on a control that's
        // about to disappear.
        ImGui::BeginDisabled(!m_visible);

        // --- Capsule HUD corner brackets ---
        {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            if (dl)
            {
                const ImVec2 wMin = ImGui::GetWindowPos();
                const ImVec2 wMax = ImVec2(wMin.x + ImGui::GetWindowWidth(), wMin.y + ImGui::GetWindowHeight());
                const float bl = 14.0f;
                const ImU32 bracketCol = IM_COL32(255, 158, 26, 200);
                dl->AddLine(ImVec2(wMin.x + 6, wMin.y + 6), ImVec2(wMin.x + 6 + bl, wMin.y + 6), bracketCol, 2.0f);
                dl->AddLine(ImVec2(wMin.x + 6, wMin.y + 6), ImVec2(wMin.x + 6, wMin.y + 6 + bl), bracketCol, 2.0f);
                dl->AddLine(ImVec2(wMax.x - 6, wMin.y + 6), ImVec2(wMax.x - 6 - bl, wMin.y + 6), bracketCol, 2.0f);
                dl->AddLine(ImVec2(wMax.x - 6, wMin.y + 6), ImVec2(wMax.x - 6, wMin.y + 6 + bl), bracketCol, 2.0f);
                dl->AddLine(ImVec2(wMin.x + 6, wMax.y - 6), ImVec2(wMin.x + 6 + bl, wMax.y - 6), bracketCol, 2.0f);
                dl->AddLine(ImVec2(wMin.x + 6, wMax.y - 6), ImVec2(wMin.x + 6, wMax.y - 6 - bl), bracketCol, 2.0f);
                dl->AddLine(ImVec2(wMax.x - 6, wMax.y - 6), ImVec2(wMax.x - 6 - bl, wMax.y - 6), bracketCol, 2.0f);
                dl->AddLine(ImVec2(wMax.x - 6, wMax.y - 6), ImVec2(wMax.x - 6, wMax.y - 6 - bl), bracketCol, 2.0f);
            }
        }
        ImGui::Spacing();

        // --- Tabs ---
        // BeginTabItem draws its header label immediately (using whatever font is
        // bound at that call); the tab's *content* is only drawn afterwards, once we
        // call drawXTab(). So the HUD font is pushed/popped tightly around each
        // BeginTabItem call only — section headers inside each tab push it again
        // themselves (see sectionHeader()) — leaving the body text in the default font.
        {
            ScopedStyleVar v(ImGuiStyleVar_TabBorderSize, 0.0f);
            if (ImGui::BeginTabBar("SettingsTabs"))
            {
                if (ImGui::BeginTabItem("  Audio"))
                {
                    m_activeTab = 0;
                    drawAudioTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("  Video"))
                {
                    m_activeTab = 1;
                    drawVideoTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("  Controllers"))
                {
                    m_activeTab = 2;
                    drawControllersTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("  Netplay"))
                {
                    m_activeTab = 4;
                    drawNetplayTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("  Logging"))
                {
                    m_activeTab = 3;
                    drawLoggingTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("  About"))
                {
                    m_activeTab = 4;
                    drawAboutTab();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }

        // --- Footer ---
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        {
            const char *hint = "Shift+Tab / Select+Start to toggle";
            float tw = ImGui::CalcTextSize(hint).x;
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() / 2.0f - tw / 2.0f - 8);
            {
                ScopedStyleColor c(ImGuiCol_Text, dbz(0.6f, 0.6f, 0.7f));
                ImGui::TextUnformatted(hint);
            }

            ImGui::SameLine(ImGui::GetWindowWidth() - 170);
            {
                ScopedStyleColor c0(ImGuiCol_Button, dbz(0.15f, 0.30f, 0.55f));
                ScopedStyleColor c1(ImGuiCol_ButtonHovered, dbz(0.20f, 0.40f, 0.70f));
                ScopedStyleColor c2(ImGuiCol_ButtonActive, dbz(0.18f, 0.35f, 0.60f));
                if (ImGui::Button("Apply", ImVec2(64, 0)))
                    applySettings();
            }
            ImGui::SameLine();
            {
                ScopedStyleColor c0(ImGuiCol_Button, dbz(0.45f, 0.12f, 0.10f));
                ScopedStyleColor c1(ImGuiCol_ButtonHovered, dbz(0.60f, 0.16f, 0.12f));
                ScopedStyleColor c2(ImGuiCol_ButtonActive, dbz(0.50f, 0.14f, 0.11f));
                if (ImGui::Button("Close", ImVec2(64, 0)))
                    m_visible = false;
            }
        }

        // Bindings / Overlay-settings sub-window (opened from the Controllers tab).
        // OpenPopup must be called in the same ID scope as BeginPopupModal (after the
        // tab bar) — calling it inside the tab item would prefix the popup ID with the
        // tab's own ID, so BeginPopupModal could never find it.
        if (m_showBindingsPopup && !ImGui::IsPopupOpen("Controller Bindings"))
            ImGui::OpenPopup("Controller Bindings");
        drawBindingsPopup();

        ImGui::EndDisabled();

        // Detect closing via the Close button or the title-bar X: ImGui sets *p_open
        // (m_visible) to false directly. That path doesn't go through toggleVisible(),
        // so the input-suspended flag would stay true and the game would never capture
        // the controller again. Release it whenever m_visible transitions true -> false.
        ImGui::End();
        if (wasVisible && !m_visible)
            ps2_stubs::PadConfig::setInputSuspended(false);
        if (m_settings.meter) drawMeter();            // [f1]
        drawShotNotice();                             // [screenshot]
        if (m_settings.perfOverlay) drawPerfHud();   // [perfhud]
        if (winButtonsWanted()) drawWinButtons();     // [winbtn]
#ifdef PS2X_HAVE_NATIVE_GS
        drawF1Window();                               // [native_gs] F1
#endif
    }
    catch (...)
    {
        // Never let an overlay rendering fault kill the whole game.
        m_visible = false;
    }
    rlImGuiEnd();
}

void PS2SettingsOverlay::drawAudioTab()
{
    ImGui::Spacing();

    auto volumeSlider = [&](const char *label, float *v)
    {
        char buf[16];
        snprintf(buf, sizeof buf, "%.0f%%", (*v) * 100.0f);
        ImGui::TextUnformatted(label);
        ImGui::SameLine(120);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70.0f);
        if (ImGui::SliderFloat(("##" + std::string(label)).c_str(), v, 0.0f, 1.0f, buf,
                            ImGuiSliderFlags_NoRoundToFormat))
            m_dirty = true;
    };

    sectionHeader("MASTER VOLUME");
    volumeSlider("Master", &m_settings.masterVolume);
    ImGui::TextDisabled("Global output volume.");

    sectionHeader("MIXER");
    volumeSlider("Music", &m_settings.musicVolume);
    volumeSlider("SFX", &m_settings.sfxVolume);
    ImGui::TextDisabled("Music = BGM streams. SFX = voices, effects and one-shots.");

    ImGui::Spacing();
}

void PS2SettingsOverlay::drawVideoTab()
{
    ImGui::Spacing();

    // Renderer + Effects (flat, compact — no card borders)
    sectionHeader("RENDERER");
    {   // [renderer] backend dropdown
#if defined(PS2X_HAVE_PGS)
        static const char *const kRenderers[] = { "OpenGL", "Software rasterizer", "paraLLEl-GS (Vulkan compute)" };
        const int nRenderers = 3;
#else
        static const char *const kRenderers[] = { "OpenGL", "Software rasterizer" };
        const int nRenderers = 2;
#endif
        int r = std::clamp(m_settings.renderer, 0, nRenderers - 1);
        ImGui::TextUnformatted("Renderer");
        ImGui::SameLine(180.0f);
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::Combo("##renderer", &r, kRenderers, nRenderers))
        {
            m_settings.renderer = r;
            m_settings.gpuRenderer = (r != Settings::kRendererSoftware);
            m_dirty = true;
        }
    }
    ImGui::TextDisabled("Takes full effect after restart.");
    if (toggleSwitch("Cel Outline", &m_settings.outline))
        m_dirty = true;
    if (toggleSwitch("60 FPS (experimental)", &m_settings.fps60))
    {   // [fps60] step 1 + the pacing table; the runtime applies it between fights, never mid-fight
        ps2Set60Fps(m_settings.fps60, nullptr);
        m_dirty = true;
    }
    if (m_settings.outline)
    {   // [inkstrength] how hard the outline darkener subtracts. 199% is the exact GS
        // strength (it divides Ad by 128 where GL divides by 255); 100% is the old,
        // washed-out line. Applies live -- it is a single shader uniform.
        ImGui::Text("Ink Strength");
        ImGui::SameLine(120);
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderInt("##inkstrength", &m_settings.inkStrength, 100, 400, "%d %%",
                             ImGuiSliderFlags_AlwaysClamp))
        {
            GsGpuRenderer::setInkStrengthPct(m_settings.inkStrength);   // live preview
            m_dirty = true;
        }
        ImGui::TextDisabled("199%% matches the console line. Higher = darker ink.");
        if (m_settings.renderer == 2)
        {   // [pgsink] paraLLEl-GS: the stroke width is the outline chain's edge-detect shift, rewritten in the stream
            ImGui::Text("Ink Width");
            ImGui::SameLine(120);
            ImGui::SetNextItemWidth(220);
            if (ImGui::SliderInt("##inkwidth", &m_settings.inkWidth, 25, 100, "%d %%", ImGuiSliderFlags_AlwaysClamp))
            {
                ps2x_pgs::setInkWidthPct(m_settings.inkWidth);   // live
                m_dirty = true;
            }
            ImGui::TextDisabled("100%% = the console's one-pixel stroke; lower = thinner (paraLLEl-GS only).");
            {   // [pgsink] the darkener subtracts its colour from the scene, so the picker sets the complement it keeps
                float rgb[3] = { ((m_settings.inkColor >> 16) & 0xFFu) / 255.0f, ((m_settings.inkColor >> 8) & 0xFFu) / 255.0f, (m_settings.inkColor & 0xFFu) / 255.0f };
                ImGui::Text("Ink Color");
                ImGui::SameLine(120);
                ImGui::SetNextItemWidth(220);
                if (ImGui::ColorEdit3("##inkcolor", rgb, ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_Uint8))
                {
                    auto b = [](float f) { return static_cast<unsigned>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f); };
                    m_settings.inkColor = (b(rgb[0]) << 16) | (b(rgb[1]) << 8) | b(rgb[2]);
                    ps2x_pgs::setInkColor(m_settings.inkColor);   // live
                    m_dirty = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Black")) { m_settings.inkColor = 0; ps2x_pgs::setInkColor(0); m_dirty = true; }
                ImGui::TextDisabled("Exact on light backgrounds; darker scenes tint toward it (paraLLEl-GS only).");
            }
        }
    }
    if (toggleSwitch("Character Shadows", &m_settings.shadows))
        m_dirty = true;
    if (toggleSwitch("Depth-of-Field Blur", &m_settings.dofBlur))
        m_dirty = true;
    if (m_settings.dofBlur)
    {
        ImGui::Text("Blur Reach");
        ImGui::SameLine(120);
        ImGui::SetNextItemWidth(220);
        int reach = m_settings.dofZFar / 1000;   // present in "k" units for a readable slider
        if (ImGui::SliderInt("##dofreach", &reach, 50, 400, "%d k", ImGuiSliderFlags_AlwaysClamp))
        {
            m_settings.dofZFar = reach * 1000;
            m_dirty = true;
        }
        if (m_settings.renderer == 2) ImGui::TextDisabled("paraLLEl-GS: off keeps the aura glow (the game blurs through the same pass,\nso a soft halo stays around a charging aura); reach is OpenGL-only.");
        ImGui::TextDisabled("Lower = blur reaches nearer to the camera. 200k matches the console look.");
    }
    {   // [postfx] image controls on the final frame; all apply live
        sectionHeader("IMAGE");
        bool pfx = false;
        if (toggleSwitch("FXAA (edge smoothing)", &m_settings.fxaa)) pfx = true;
        ImGui::TextDisabled("Smooths jagged edges cheaply. Pair it with a lower Internal Resolution for speed.");
        if (toggleSwitch("Performance overlay (FPS / CPU / GPU)", &m_settings.perfOverlay)) m_dirty = true;   // [perfhud]
        if (m_settings.perfOverlay)
        {
            static const char *kCorners[] = { "Top left", "Top right", "Bottom left", "Bottom right" };
            ImGui::Text("Position"); ImGui::SameLine(120); ImGui::SetNextItemWidth(180);
            if (ImGui::Combo("##perfcorner", &m_settings.perfCorner, kCorners, 4)) m_dirty = true;
        }
        ImGui::TextDisabled("Shows fps, CPU, GPU, RAM and VRAM in a corner of the screen. F10 toggles it in game.");
        auto row = [&](const char *label, const char *id, int *v, int lo, int hi, const char *fmt) {
            ImGui::Text("%s", label);
            ImGui::SameLine(120);
            ImGui::SetNextItemWidth(220);
            if (ImGui::SliderInt(id, v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp)) pfx = true;
        };
        row("Sharpen", "##pfxsharpen", &m_settings.sharpen, 0, 100, "%d %%");
        row("Brightness", "##pfxbright", &m_settings.brightness, -50, 50, "%+d");
        row("Contrast", "##pfxcontrast", &m_settings.contrast, 50, 150, "%d %%");
        row("Saturation", "##pfxsat", &m_settings.saturation, 0, 200, "%d %%");
        row("Gamma", "##pfxgamma", &m_settings.gamma, 50, 200, "%d %%");
        if (ImGui::SmallButton("Reset image"))
        {
            m_settings.fxaa = false; m_settings.sharpen = 0; m_settings.brightness = 0;
            m_settings.contrast = 100; m_settings.saturation = 100; m_settings.gamma = 100;
            pfx = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Vivid preset"))
        {
            m_settings.sharpen = 35; m_settings.contrast = 108; m_settings.saturation = 125; m_settings.gamma = 100;
            pfx = true;
        }
        if (pfx) { pushPostFx(m_settings); m_dirty = true; }
    }
    // (Glow / Skip Post / Half-Texel / Skip Stale VRAM toggles removed: replay A/B
    //  measured them at 0.000 frame diff in fights -- their draw classes are
    //  superseded by the current serving pipeline. Env vars still work for devs.)
    {   // [glowfix] BT3's bloom/glow chain -- the Kaioken aura and every attack glow.
        const bool was = m_settings.glowFix;
        if (toggleSwitch("Glow (Kaioken aura)", &m_settings.glowFix))
            m_dirty = true;
        if (m_settings.glowFix != GsGpuRenderer::glowFixEnabled())
            ImGui::TextDisabled("(applies on restart)");
        else if (was) ImGui::TextDisabled("Character/attack bloom. Off = the pre-fix look.");
    }

    sectionHeader("FILTERING");
    if (toggleSwitch("Bilinear Filter", &m_settings.bilinear))
        m_dirty = true;
    {   // PCSX2-style forced filtering: smooth even textures the game point-samples
        // (far-terrain tiles -- the pixelated mountains at high internal resolution).
        extern void ps2xSetForceBilinear(bool);
        if (toggleSwitch("Force Filtering (smooth terrain)", &m_settings.forceBilinear))
        {
            ps2xSetForceBilinear(m_settings.forceBilinear);
            m_dirty = true;
        }
    }

    // Display
    sectionHeader("DISPLAY");
    if (toggleSwitch("Fullscreen", &m_settings.fullscreen))
    {
        ps2xSetFullscreen(m_settings.fullscreen, m_settings.windowW, m_settings.windowH);
        // [builtin-res] the internal render scale follows the resolution: 720p=1x,
        // 1080p=2x, 1440p+=3x. Derive it from the current screen in fullscreen.
        const int h = GetScreenHeight();
        m_settings.renderScale = ps2xRenderScaleForHeight(h);
        if (!envUserSet("PS2X_PGS_SSAA")) ps2x_pgs::setRenderScale(m_settings.renderScale);   // [pgslive]
        m_dirty = true;
    }
    if (toggleSwitch("Widescreen (true FOV)", &m_settings.widescreen))
    {
        s_widescreen = m_settings.widescreen;
        m_dirty = true;
    }
    if (m_settings.widescreen)
    {   // widescreen HUD layout: where the corrected-proportion HUD sits on the wide frame
        static const char *kHudLayouts[] = {"Centered (4:3 block)", "Edge-pinned (wide)", "Custom (sliders)"};
        ImGui::TextUnformatted("HUD Layout");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        int hl = m_settings.hudLayout;
        if (ImGui::Combo("##hudlayout", &hl, kHudLayouts, 3))
        {
            m_settings.hudLayout = hl;
            pushHudLayout(m_settings);
            m_dirty = true;
        }
        if (m_settings.hudLayout == 2)
        {
            bool ch = false;
            ch |= ImGui::SliderInt("Left cluster", &m_settings.hudOffL, -120, 120, "%d px");
            ch |= ImGui::SliderInt("Timer", &m_settings.hudOffC, -120, 120, "%d px");
            ch |= ImGui::SliderInt("Right cluster", &m_settings.hudOffR, -120, 120, "%d px");
            if (ch) { pushHudLayout(m_settings); m_dirty = true; }
            ImGui::TextDisabled("offsets from the edge-pinned layout; bars auto-stretch");
        }
        if (m_settings.hudLayout != 0)
            ImGui::TextDisabled("note: in stretched layouts the damage flash can briefly show at both bar ends");
    }

    // Window-size presets. The projection FOV follows the window aspect (see [truews]
    // in ps2_runtime.cpp), so wider windows genuinely show more stage.
    {
        static const int kRes[][2] = {{1024, 768}, {1280, 720}, {1360, 768},
                                      {1366, 768}, {1440, 900}, {1600, 900},
                                      {1920, 1080}, {2560, 1440}, {3440, 1440}, {3840, 2160}};
        static const char *kResNames[] = {"1024 x 768 (4:3)", "1280 x 720", "1360 x 768",
                                          "1366 x 768", "1440 x 900", "1600 x 900",
                                          "1920 x 1080", "2560 x 1440", "3440 x 1440 (ultrawide)",
                                          "3840 x 2160 (4K)"};
        constexpr int kResCount = 10;
        int cur = -1;
        const int w = GetScreenWidth(), h = GetScreenHeight();
        for (int i = 0; i < kResCount; ++i)
            if (kRes[i][0] == w && kRes[i][1] == h) { cur = i; break; }
        char curLabel[32];
        std::snprintf(curLabel, sizeof(curLabel), "%d x %d", w, h);
        ImGui::TextUnformatted("Window Size");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::BeginCombo("##winsize", cur >= 0 ? kResNames[cur] : curLabel))
        {
            for (int i = 0; i < kResCount; ++i)
            {
                if (ImGui::Selectable(kResNames[i], i == cur) && i != cur && !IsWindowFullscreen())
                {
                    ps2xApplyWindowSize(kRes[i][0], kRes[i][1]);
                    m_settings.windowW = kRes[i][0];
                    m_settings.windowH = kRes[i][1];
                    // [builtin-res] the internal render scale is built into the resolution:
                    // 720p=1x, 1080p=2x, 1440p+=3x. paraLLEl-GS replays live at the new SSAA.
                    m_settings.renderScale = ps2xRenderScaleForHeight(kRes[i][1]);
                    if (!envUserSet("PS2X_PGS_SSAA")) ps2x_pgs::setRenderScale(m_settings.renderScale);   // [pgslive]
                    m_dirty = true;
                }
            }
            ImGui::EndCombo();
        }
        if (IsWindowFullscreen())
            ImGui::TextDisabled("(windowed mode only)");
    }
}

void PS2SettingsOverlay::drawControllersTab()
{
    // [cttrmenu] F1 Controls page: the same look as the other pages (orange headings with a hairline, a name + note
    // on the left and the control on the right). The old overlay layout below stays for the legacy window.
    auto f1Section = [&](const char *name) {
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::PushFont(s_fontSemibold, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, cttrui::kOrange);
        ImGui::TextUnformatted(name);
        ImGui::PopStyleColor();
        ImGui::PopFont();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y - 2.0f), ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y - 2.0f),
                                            ImGui::GetColorU32(cttrui::kLine), 1.0f);
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
    };
    auto f1Note = [&](const char *text) {
        ImGui::PushStyleColor(ImGuiCol_Text, cttrui::kMuted);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    };
    if (m_f1Mode)
    {
        const float ctrlW = std::min(300.0f, ImGui::GetContentRegionAvail().x * 0.48f);
        auto f1Row = [&](const char *name, const char *note) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(name);
            ImGui::PushStyleColor(ImGuiCol_Text, cttrui::kMuted);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 16.0f);
            ImGui::TextUnformatted(note);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
        };
        f1Section("Controller");
        if (ImGui::BeginTable("##ctl", 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_NoSavedSettings))
        {
            ImGui::TableSetupColumn("##l", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##c", ImGuiTableColumnFlags_WidthFixed, ctrlW);
            f1Row("Player", "Whose controls you are changing.");
            const char *playerNames[] = {"Player 1", "Player 2"};
            ImGui::Combo("##player", &m_editPlayer, playerNames, 2);
            f1Row("Device", "The keyboard or gamepad this player uses.");
            {
                std::vector<const char *> labels;
                labels.reserve(m_deviceList.size());
                for (auto &d : m_deviceList)
                    labels.push_back(d.name.c_str());
                if (!labels.empty() &&
                    ImGui::Combo("##device", &m_selectedDevice, labels.data(), static_cast<int>(labels.size())))
                    applySettings();
            }
            f1Row("Stick dead zone", "How far a stick moves before the game notices. Raise it if a worn stick steers on its own.");
            int dz = (int)std::lround(m_settings.deadzone * 100.0f);
            if (ImGui::SliderInt("##dz", &dz, 0, 50, "%d%%", ImGuiSliderFlags_AlwaysClamp))
            {
                m_settings.deadzone = dz / 100.0f;
                applyDeadzone();
                saveSettings();
            }
            ImGui::EndTable();
        }
        f1Section("Button bindings");
        f1Note("Click Bind, then press the new key, button or stick direction.");
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        drawBindingsTable();
    }
    else
    {
        ImGui::Spacing();

        // --- Device / Player selectors ---
        sectionHeader("DEVICE");

        // Player
        {
            ImGui::Text("Player");
            ImGui::SameLine(90);
            ImGui::SetNextItemWidth(110);
            const char *playerNames[] = {"P1", "P2"};
            ImGui::Combo("##player", &m_editPlayer, playerNames, 2);
        }

        // Device
        {
            ImGui::Text("Device");
            ImGui::SameLine(90);
            std::vector<const char *> labels;
            labels.reserve(m_deviceList.size());
            for (auto &d : m_deviceList)
                labels.push_back(d.name.c_str());
            if (!labels.empty() &&
                ImGui::Combo("##device", &m_selectedDevice, labels.data(), static_cast<int>(labels.size())))
            {
                applySettings();
            }
        }

        // Deadzone
        {
            ImGui::Text("Deadzone");
            ImGui::SameLine(90);
            ImGui::SetNextItemWidth(180);
            if (ImGui::SliderFloat("##dz", &m_settings.deadzone, 0.0f, 0.5f, "%.2f"))
            {
                applyDeadzone();
                saveSettings();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(%.0f%%)", m_settings.deadzone * 100.0f);
        }

        // --- Bindings popup trigger button ---
        sectionHeader("BINDINGS");
        ImGui::Spacing();
        {
            const float btnW = 220.0f;
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - btnW) / 2.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, dbz(0.20f, 0.17f, 0.12f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accent());
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, dbz(0.85f, 0.55f, 0.15f));
            if (m_f1Mode)
                ImGui::SetCursorPosX(ImGui::GetCursorStartPos().x);   // [f1] the table itself, no popup
            else if (ImGui::Button("Button Bindings", ImVec2(btnW, 34)))
            {
                m_showBindingsPopup = true;
            }
            if (m_f1Mode)
            {
                ImGui::PopStyleColor(3);
                ImGui::TextDisabled("Bind, then press the new key, button or stick direction.");
                drawBindingsTable();
                ImGui::PushStyleColor(ImGuiCol_Button, dbz(0.20f, 0.17f, 0.12f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accent());
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, dbz(0.85f, 0.55f, 0.15f));
            }
            ImGui::PopStyleColor(3);
        }
        ImGui::Spacing();
    }

    // --- Capture logic ---
    // ALWAYS read gamepad state and update edge tracking (independent of UI visibility)
    std::array<uint8_t, 32> curBtnDown;
    std::array<float, 6> curAxis;
    {
        if (m_selectedDevice >= 0 && m_selectedDevice < static_cast<int>(m_deviceList.size()))
            readGamepadStateForDevice(m_deviceList[m_selectedDevice], curBtnDown, curAxis);
        else
        {
            curBtnDown = {};
            curAxis = {};
        }

        const bool capturingAction = m_captureAction >= 0 && m_captureAction < static_cast<int>(ps2_stubs::PadAction::Count);
        const bool capturingCombo = m_captureComboSlot >= 0;
        if (capturingAction)
        {
            if (m_captureWaitRelease)
            {
                bool anyDown = false;
                for (int b = 0; b < 32 && !anyDown; ++b)
                    if (curBtnDown[b]) anyDown = true;
                for (int a = 0; a < 6 && !anyDown; ++a)
                    if (std::fabs(curAxis[a]) > 0.3f) anyDown = true;
                for (int k = 32; k <= 348 && !anyDown; ++k)
                    if (IsKeyDown(k)) anyDown = true;
                if (!anyDown)
                    m_captureWaitRelease = false;
            }
            else
            {
                ps2_stubs::PadBind bind;
                int capturedKey = -1;

                // 1. Keyboard
                for (int k = 32; k <= 348 && bind.kind == ps2_stubs::PadBindKind::None; ++k)
                    if (IsKeyPressed(k))
                    {
                        bind = ps2_stubs::PadBind{ps2_stubs::PadBindKind::Key, k, 1.0f, m_settings.deadzone};
                        capturedKey = k;
                    }

                // 2. Gamepad buttons — edge
                for (int b = 0; b < 32 && bind.kind == ps2_stubs::PadBindKind::None; ++b)
                    if (curBtnDown[b] && !m_prevBtnDown[b])
                        bind = ps2_stubs::PadBind{ps2_stubs::PadBindKind::Button, b, 1.0f, m_settings.deadzone};

                // 3. Gamepad axes — edge
                for (int a = 0; a < 6 && bind.kind == ps2_stubs::PadBindKind::None; ++a)
                    if (std::fabs(curAxis[a]) > 0.5f && std::fabs(m_prevAxis[a]) <= 0.5f)
                        bind = ps2_stubs::PadBind{ps2_stubs::PadBindKind::Axis, a, curAxis[a] > 0 ? 1.0f : -1.0f, m_settings.deadzone};

                if (bind.kind != ps2_stubs::PadBindKind::None)
                {
                    auto &pcfg = ps2_stubs::PadConfig::instance();
                    const auto act = static_cast<ps2_stubs::PadAction>(m_captureAction);
                    pcfg.setBind(m_editPlayer, act, bind);
                    resetCaptureState();
                }
            }
        }
        else if (capturingCombo)
        {
            // 3-second capture window: record every input held during it, then save.
            const float dt = ImGui::GetIO().DeltaTime;
            m_captureComboTimer -= dt;

            if (m_captureComboSlot == 0)
            {
                // Gamepad: gather buttons currently held.
                for (int b = 0; b < 32; ++b)
                    if (curBtnDown[b])
                    {
                        bool present = false;
                        for (int x : m_capturedBtns) if (x == b) { present = true; break; }
                        if (!present) m_capturedBtns.push_back(b);
                    }
            }
            else
            {
                // Keyboard: gather keys currently held.
                for (int k = 32; k <= 348; ++k)
                    if (IsKeyDown(k))
                    {
                        bool present = false;
                        for (int x : m_capturedKeys) if (x == k) { present = true; break; }
                        if (!present) m_capturedKeys.push_back(k);
                    }
            }

            if (m_capturedBtns.size() > 0 || m_capturedKeys.size() > 0)
                m_captureComboHadAny = true;

            // Commit when the window expires or, if nothing was captured yet, once the
            // user lets go of everything (so a tap on a single button still works).
            const bool expired = (m_captureComboTimer <= 0.0f);
            const bool released = (m_captureComboSlot == 0 && !m_capturedBtns.empty())
                                ? allButtonsReleased(curBtnDown, m_capturedBtns)
                                : (m_captureComboSlot == 1 && !m_capturedKeys.empty()
                                   ? allKeysReleased(m_capturedKeys)
                                   : false);
            if (expired || (m_captureComboHadAny && released))
            {
                if (m_captureComboSlot == 0 && !m_capturedBtns.empty())
                    m_settings.overlayPadBtns = m_capturedBtns;
                else if (m_captureComboSlot == 1 && !m_capturedKeys.empty())
                    m_settings.overlayKeys = m_capturedKeys;

                m_dirty = true;
                saveSettings();
                m_captureComboSlot = -1;
                m_captureComboTimer = 0.0f;
                m_captureComboHadAny = false;
                m_capturedBtns.clear();
                m_capturedKeys.clear();
                m_captureWaitRelease = true;
            }
        }

        // ALWAYS update edge state for next frame
        m_prevBtnDown = curBtnDown;
        m_prevAxis = curAxis;
    }

    if (m_f1Mode)
    {
        f1Section("Pad test");
        drawGamepadTestArea(curBtnDown, curAxis);
        return;
    }
    // --- Gamepad test area (always visible) ---
    sectionHeader("GAMEPAD TEST");
    drawGamepadTestArea(curBtnDown, curAxis);

    // --- Apply / Save / Reload buttons ---
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const float totalW = 100.0f * 3 + 12 * 2;
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x / 2.0f - totalW / 2.0f);

    ImGui::PushStyleColor(ImGuiCol_Button, dbz(0.30f, 0.30f, 0.36f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, dbz(0.40f, 0.40f, 0.46f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, dbz(0.35f, 0.35f, 0.40f));
    if (ImGui::Button("Reload", ImVec2(100, 30)))
        resetCaptureState();
    ImGui::PopStyleColor(3);

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, dbz(0.15f, 0.30f, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, dbz(0.20f, 0.40f, 0.70f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, dbz(0.18f, 0.35f, 0.60f));
    if (ImGui::Button("Apply", ImVec2(100, 30)))
        applySettings();
    ImGui::PopStyleColor(3);

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, dbz(0.15f, 0.45f, 0.25f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, dbz(0.20f, 0.55f, 0.32f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, dbz(0.18f, 0.50f, 0.28f));
    if (ImGui::Button("Save", ImVec2(100, 30)))
    {
        applySettings();
        saveSettings();
        if (m_f1Mode) ps2_stubs::PadConfig::instance().save();   // [f1] the bindings edited inline
    }
    ImGui::PopStyleColor(3);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("Overlay Shortcuts");
    ImGui::Text("Keyboard:  Shift + Tab");
    bool anyPad = false;
    for (int g = 0; g < ps2x_pad::kMaxSlots && !anyPad; ++g)
        anyPad = ps2x_pad::available(g);
    if (anyPad)
        ImGui::Text("Gamepad:   Select + Start");
    else
        ImGui::TextDisabled("Gamepad:   (no gamepad detected)");
}

// --- Bindings table (shown inside the Bindings popup) ---
void PS2SettingsOverlay::drawBindingsTable()
{
    auto &pcfg = ps2_stubs::PadConfig::instance();
    auto cfg = pcfg.snapshot(m_editPlayer);

    const ImGuiTableFlags bindFlags = m_f1Mode
        ? (ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY)   // [cttrmenu] F1 look
        : (ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY);
    if (ImGui::BeginTable("##binds", 3, bindFlags, ImVec2(0, m_f1Mode ? 300.0f : 260.0f)))
    {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, m_f1Mode ? 150.0f : 110.0f);
        ImGui::TableSetupColumn("Binding", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##btn", ImGuiTableColumnFlags_WidthFixed, 56);
        ImGui::TableSetupScrollFreeze(0, 1);
        // Capsule HUD: readout-style column headers (Russo One, when loaded).
        ImGui::TableHeadersRow();

        for (size_t a = 0; a < static_cast<size_t>(ps2_stubs::PadAction::Count); ++a)
        {
            const auto action = static_cast<ps2_stubs::PadAction>(a);
            const bool capturing = (m_captureAction == static_cast<int>(a));

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (capturing)
                ImGui::TextColored(gold(), "%s", ps2_stubs::padActionName(action));
            else
                ImGui::TextUnformatted(ps2_stubs::padActionName(action));

            ImGui::TableNextColumn();
            if (capturing)
            {
                const float a = blinkAlpha();
                ImGui::TextColored(accent(0.5f + 0.5f * a), "Press key / button / stick...");
            }
            else
                ImGui::TextUnformatted(ps2_stubs::padBindDisplay(cfg.binds[a]).c_str());

            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(a));
            if (capturing)
            {
                if (ImGui::SmallButton("Cancel"))
                    m_captureAction = -1;
            }
            else
            {
                if (ImGui::SmallButton("Bind"))
                {
                    m_captureAction = static_cast<int>(a);
                    if (m_selectedDevice >= 0 && m_selectedDevice < static_cast<int>(m_deviceList.size()))
                        readGamepadStateForDevice(m_deviceList[m_selectedDevice], m_prevBtnDown, m_prevAxis);
                    else
                    {
                        m_prevBtnDown = {};
                        m_prevAxis = {};
                    }
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// --- Overlay toggle combo (shown inside the Bindings popup) ---
void PS2SettingsOverlay::drawOverlayToggle()
{
    auto btnsLabel = [](const std::vector<int> &btns) -> std::string
    {
        if (btns.empty()) return "None";
        std::string s;
        for (size_t i = 0; i < btns.size(); ++i)
            s += (i ? " + " : "") + std::string("Button ") + std::to_string(btns[i]);
        return s;
    };
    auto keysLabel = [](const std::vector<int> &keys) -> std::string
    {
        if (keys.empty()) return "None";
        std::string s;
        for (size_t i = 0; i < keys.size(); ++i)
            s += (i ? " + " : "") + std::string("Key ") + std::to_string(keys[i]);
        return s;
    };

    auto slotRow = [&](int slot, const char *label, const std::string &display)
    {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(150);
        const bool capturing = (m_captureComboSlot == slot);
        if (capturing)
        {
            const float t = std::max(0.0f, m_captureComboTimer);
            ImGui::TextColored(accent(0.5f + 0.5f * blinkAlpha()), "Hold input... %.1fs", t);
        }
        else
        {
            ImGui::TextUnformatted(display.c_str());
        }
        ImGui::SameLine(420);
        ImGui::PushID(slot + 200);
        if (capturing)
        {
            if (ImGui::SmallButton("Cancel"))
            {
                m_captureComboSlot = -1;
                m_captureComboTimer = 0.0f;
                m_captureComboHadAny = false;
                m_capturedBtns.clear();
                m_capturedKeys.clear();
            }
        }
        else
        {
            if (ImGui::SmallButton("Bind"))
            {
                m_captureComboSlot = slot;
                m_captureComboTimer = 3.0f;   // 3-second capture window
                m_captureComboHadAny = false;
                m_capturedBtns.clear();
                m_capturedKeys.clear();
                if (m_selectedDevice >= 0 && m_selectedDevice < static_cast<int>(m_deviceList.size()))
                    readGamepadStateForDevice(m_deviceList[m_selectedDevice], m_prevBtnDown, m_prevAxis);
                else
                    m_prevBtnDown = {};
            }
        }
        ImGui::PopID();
    };

    ImGui::TextWrapped("Hold the button(s) / key(s) for 3 seconds to bind. All held inputs "
                       "must be pressed together to open or close the overlay.");
    ImGui::Spacing();
    slotRow(0, "Gamepad Launch", btnsLabel(m_settings.overlayPadBtns));
    ImGui::Spacing();
    slotRow(1, "Alt Launch (Keys)", keysLabel(m_settings.overlayKeys));
}

// --- Bindings popup modal (Bindings + Overlay Settings in tabs) ---
void PS2SettingsOverlay::drawBindingsPopup()
{
    if (!m_showBindingsPopup)
        return;

    {   // [fitscale] clamp + centre the modal inside the viewport
        const ImVec2 vp = ImGui::GetMainViewport()->Size;
        ImGui::SetNextWindowSize(ImVec2(std::min(560.0f, vp.x * 0.92f),
                                        std::min(380.0f, vp.y * 0.88f)), ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    }
    if (ImGui::BeginPopupModal("Controller Bindings", &m_showBindingsPopup,
                               ImGuiWindowFlags_NoResize))
    {
        // Capsule HUD corner brackets, matching the main panel — keeps the popup
        // reading as part of the same HUD rather than a plain generic dialog.
        {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            if (dl)
            {
                const ImVec2 wMin = ImGui::GetWindowPos();
                const ImVec2 wMax = ImVec2(wMin.x + ImGui::GetWindowWidth(), wMin.y + ImGui::GetWindowHeight());
                const float bl = 12.0f;
                const ImU32 bracketCol = IM_COL32(255, 158, 26, 170);
                dl->AddLine(ImVec2(wMin.x + 5, wMin.y + 5), ImVec2(wMin.x + 5 + bl, wMin.y + 5), bracketCol, 2.0f);
                dl->AddLine(ImVec2(wMin.x + 5, wMin.y + 5), ImVec2(wMin.x + 5, wMin.y + 5 + bl), bracketCol, 2.0f);
                dl->AddLine(ImVec2(wMax.x - 5, wMin.y + 5), ImVec2(wMax.x - 5 - bl, wMin.y + 5), bracketCol, 2.0f);
                dl->AddLine(ImVec2(wMax.x - 5, wMin.y + 5), ImVec2(wMax.x - 5, wMin.y + 5 + bl), bracketCol, 2.0f);
                dl->AddLine(ImVec2(wMin.x + 5, wMax.y - 5), ImVec2(wMin.x + 5 + bl, wMax.y - 5), bracketCol, 2.0f);
                dl->AddLine(ImVec2(wMin.x + 5, wMax.y - 5), ImVec2(wMin.x + 5, wMax.y - 5 - bl), bracketCol, 2.0f);
                dl->AddLine(ImVec2(wMax.x - 5, wMax.y - 5), ImVec2(wMax.x - 5 - bl, wMax.y - 5), bracketCol, 2.0f);
                dl->AddLine(ImVec2(wMax.x - 5, wMax.y - 5), ImVec2(wMax.x - 5, wMax.y - 5 - bl), bracketCol, 2.0f);
            }
        }

        if (ImGui::BeginTabBar("##bindtabs"))
        {
            // Same pattern as the main tab bar: push the HUD font only around the
            // tab label itself, not the tab's content.
            if (ImGui::BeginTabItem("Bindings"))
            {
                ImGui::Spacing();
                drawBindingsTable();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Overlay Settings"))
            {
                ImGui::Spacing();
                drawOverlayToggle();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        // Flat HUD buttons — dark navy body, thin orange border via the theme's
        // default Border colour, orange tint on hover/active — replacing the old
        // grey "generic dialog button" look so this matches the main panel.
        ImGui::PushStyleColor(ImGuiCol_Button, dbz(0.07f, 0.09f, 0.11f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, dbz(1.00f, 0.62f, 0.10f, 0.18f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, dbz(1.00f, 0.62f, 0.10f, 0.30f));
        if (ImGui::Button("Close", ImVec2(100, 30)))
            m_showBindingsPopup = false;
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, accent(0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accent());
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, gold());
        ImGui::PushStyleColor(ImGuiCol_Text, dbz(0.05f, 0.03f, 0.01f));
        if (ImGui::Button("Save", ImVec2(100, 30)))
        {
            applySettings();
            saveSettings();
            // [launcher] persist per-action bindings too (pad.conf), not just the
            // overlay-side cfg: bindings edited here must survive a restart and be
            // visible to the Qt launcher's Bindings tab.
            ps2_stubs::PadConfig::instance().save();
        }
        ImGui::PopStyleColor(4);

        ImGui::EndPopup();
    }
}

// [netplay] Host / Join without a terminal. The peer's address is remembered in the ini --
// typing an IP on a gamepad is miserable, so recall matters more than a text field.
// Join does BOTH things the user asked for: it connects AND, on the host, kicks off the canned
// menu sequence so both sides land on character select together (see ps2NetBeginAutoStart).
void PS2SettingsOverlay::drawNetplayTab()
{
    static char s_peer[64] = "127.0.0.1";
    static int  s_port = 7777;
    static bool s_loaded = false;
    if (!s_loaded)
    {
        s_loaded = true;
        if (const char *e = std::getenv("PS2X_NET_PEER")) { std::snprintf(s_peer, sizeof s_peer, "%s", e); }
    }

    ImGui::TextUnformatted("Online play (deterministic lockstep)");
    ImGui::Separator();

    if (ps2NetActive())
    {
        ImGui::Text("Status: %s", ps2NetPeerConnected() ? "CONNECTED" : "waiting for peer...");
        ImGui::Text("You are player %d", ps2NetLocalPlayer());
        ImGui::Text("Input delay: %u frames (%u ms at 30 fps)", ps2NetDelay(), ps2NetDelay() * 33u);
        { const char *bn[] = {"Single Battle","Team Battle","DP Battle"};
          const int bt = ps2NetBattleType();
          const char *tn[] = {"60 s","90 s","180 s","240 s","no limit"};
          const int tl = ps2NetTimeLimit();
          const char *dn[] = {"10 DP","15 DP","20 DP"};
          const int dp = ps2NetDpLimit();
          ImGui::Text("Game mode: %s%s%s   |   time limit: %s",
                      (bt >= 0 && bt < 3) ? bn[bt] : "?",
                      bt == 2 ? " / " : "", (bt == 2 && dp >= 0 && dp < 3) ? dn[dp] : "",
                      (tl >= 0 && tl < 5) ? tn[tl] : "?"); }
        if (ps2NetAutoJump()) ImGui::TextUnformatted("Will jump to character select on connect.");
        if (ps2NetRollbackWindow()) ImGui::Text("Rollback window: %u frames%s", ps2NetRollbackWindow(),
                                                ps2NetSyncPending() ? "   |   state sync in progress..." : (ps2NetSyncOn() ? "   |   state synced" : ""));
        else ImGui::TextUnformatted("Lockstep (no rollback)");
        ImGui::Separator();
        if (ImGui::Button("Disconnect"))
            ps2NetDisconnect("overlay");
        ImGui::SameLine();
        ImGui::TextDisabled("restores local pads and splitscreen");
        ImGui::Separator();
        ImGui::TextWrapped("Only buttons cross the wire. Each side renders its own player "
                           "full-screen.");
        return;
    }

    static int  s_delay = 2;
    static int  s_battle = 0;
    static int  s_time = 3;
    static int  s_dp = 0;          // DP Battle budget: 0 = 10 DP, 1 = 15, 2 = 20
    static bool s_jump = true;
    static int  s_rollback = ps2NetRollbackSetting();   // [rollback] env default, 0 = lockstep
    static bool s_sync = ps2NetSyncSetting();            // [statesync]
    const bool syncLive = s_sync && s_rollback > 0;
    ImGui::Checkbox("Go to character select once connected", &s_jump);
    if (syncLive) ImGui::TextDisabled("The HOST's choice applies to both. Both sides jump, then the host's state is synced into the joiner.");
    else          ImGui::TextDisabled("The HOST's choice applies to both; the menus are hidden while it happens.");
    ImGui::Separator();
    ImGui::SliderInt("Rollback window (frames)", &s_rollback, 0, 30);
    ImGui::TextDisabled("0 = lockstep (every frame waits for the peer's input). 4-8 = rollback: a missing input is");
    ImGui::TextDisabled("predicted and the game rewinds when the real one differs. Needs PS2X_FIBERS=1.");
    if (s_rollback > 0)
    {
        ImGui::Checkbox("Sync game state on connect", &s_sync);
        ImGui::TextDisabled("The host sends its game state (40 MB) to the joiner, so both play the same match from");
        ImGui::TextDisabled("wherever the host is. Connect while both are on the same screen (title or main menu).");
    }
    ImGui::Separator();
    // Only Join uses the address: hosting binds the port and learns the peer from its first
    // packet, which is why only one side needs a reachable port.
    ImGui::InputText("Host address (Join only)", s_peer, sizeof s_peer);
    ImGui::InputInt("Port", &s_port);
    const char *kBattle[] = { "Single Battle", "Team Battle", "DP Battle" };
    ImGui::Combo("Game mode", &s_battle, kBattle, 3);
    // DP Battle's point budget is a SEPARATE row of the versus menu (duelObj+0x118, committed to
    // stateObj+0x630 = RetroAchievements' 0x6af7b0). Selecting DP without it left the screen
    // playing like Team Battle: the right type with no budget behind it.
    if (s_battle == 2)
    {
        const char *kDp[] = { "10 DP", "15 DP", "20 DP" };
        ImGui::Combo("DP limit", &s_dp, kDp, 3);
    }
    // Battle Settings time-limit indices, confirmed in game:
    //   0 = 60 s, 1 = 90 s, 2 = 180 s, 3 = 240 s (default), 4 = no limit
    const char *kTime[] = { "60 seconds", "90 seconds", "180 seconds", "240 seconds (default)", "No limit" };
    ImGui::Combo("Time limit", &s_time, kTime, 5);
    ImGui::TextDisabled("The HOST's choices apply to both players.");
    ImGui::SliderInt("Input delay (frames)", &s_delay, 1, 10);
    ImGui::TextDisabled("BT3 runs at 30 fps, so each frame is 33 ms. Use 1 on the same machine,");
    ImGui::TextDisabled("2 on a LAN. Raise it only if you see stalls.");
    if (s_port < 1 || s_port > 65535) s_port = 7777;

    if (ImGui::Button("Host (you are Player 1)"))
    {
        ps2NetSetAutoJump(s_jump); ps2NetSetDelay(s_delay); ps2NetSetBattleType(s_battle);
        ps2NetSetTimeLimit(s_time); ps2NetSetDpLimit(s_dp);
        ps2NetSetRollback(s_rollback); ps2NetSetSync(s_sync);
        ps2NetHost(s_port, 1);
    }
    ImGui::SameLine();
    if (ImGui::Button("Join (you are Player 2)"))
    {
        ps2NetSetAutoJump(s_jump); ps2NetSetDelay(s_delay);   // the host's game mode wins
        ps2NetSetRollback(s_rollback); ps2NetSetSync(s_sync);
        char hp[96]; std::snprintf(hp, sizeof hp, "%s:%d", s_peer, s_port);
        ps2NetJoin(hp, 2);
    }
    ImGui::Separator();
    ImGui::TextWrapped("HOST: just press Host -- leave the address blank, give the other player "
                       "your IP and this port. JOIN: type the host's IP above, then press Join. "
                       "Only the host needs the UDP port reachable.");
}

void PS2SettingsOverlay::drawLoggingTab()
{
    ImGui::Spacing();

    sectionHeader("SETTINGS DUMP LOG");

    ImGui::TextWrapped(
        "When a setting changes (Apply / Save), the current state of the enabled areas "
        "below is written to \"%s\" in the savedata folder. Enable the areas you want "
        "captured, then hit Save — from the next run everything is captured automatically.",
        kDumpFileName);
    ImGui::Spacing();

    if (ImGui::BeginChild("##logging", ImVec2(-1, 0), ImGuiChildFlags_Borders))
    {
        ImGui::Spacing();
        if (ImGui::Checkbox("Audio", &m_dumpAudio))
            m_dirty = true;
        if (ImGui::Checkbox("Video", &m_dumpVideo))
            m_dirty = true;
        if (ImGui::Checkbox("Controllers (players + bindings)", &m_dumpControllers))
            m_dirty = true;
        if (ImGui::Checkbox("Runtime (renderer state)", &m_dumpRuntime))
            m_dirty = true;
        if (ImGui::Checkbox("Live Gamepad state", &m_dumpGamepad))
            m_dirty = true;
        ImGui::Spacing();
    }
    ImGui::EndChild();

    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Button, dbz(0.15f, 0.35f, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, dbz(0.20f, 0.45f, 0.70f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, dbz(0.18f, 0.40f, 0.60f));
    if (ImGui::Button("Dump Now", ImVec2(-1, 34)))
        dumpSettingsToFile();
    ImGui::PopStyleColor(3);

    ImGui::Spacing();
    ImGui::TextDisabled("The dump file is appended with a timestamp on every write.");
}

void PS2SettingsOverlay::drawAboutTab()
{
    ImGui::Spacing();

    sectionHeader("ABOUT");
    ImGui::TextWrapped("Dragon Ball Z: Budokai Tenkaichi 3 - Recompiled");
    ImGui::TextWrapped(
        "A statically recompiled, native PC port built on PS2Recomp. The game's MIPS code "
        "is translated to C++ at build time from your own disc image; no game content is "
        "distributed with this project.");
    ImGui::Spacing();

    sectionHeader("CREDITS");
    ImGui::TextWrapped("z3xox - owner / lead developer");
    ImGui::TextDisabled("  recompiler, runtime (EE/GS/VU1/scheduler), renderer, game overrides, generators");
    ImGui::TextWrapped("RexxColder - supporter / colaborador");
    ImGui::TextDisabled("  optimizacion (perf/async), launcher + install wizard, input & gamepads, "
                        "build/release, deploy, game-data (AFS/AFL), docs");
    ImGui::TextWrapped("valenvivaldi - colaborador");
    ImGui::TextDisabled("  port macOS arm64, packaging, audio");
    ImGui::Spacing();

    sectionHeader("THIRD-PARTY");
    if (ImGui::BeginChild("##about_third", ImVec2(-1, 0), ImGuiChildFlags_Borders))
    {
        ImGui::TextWrapped("ran-j/PS2Recomp - static recompiler (upstream, GPL-3.0)");
        ImGui::TextWrapped("ViveTheModder - NTSC-U AFS file lists (Apache-2.0)");
        ImGui::TextWrapped("Arntzen Software - paraLLEl-GS (LGPL-3.0-or-later)");
        ImGui::Spacing();
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::TextDisabled("GPL-3.0. Not affiliated with Spike or Bandai Namco.");
    ImGui::TextDisabled("github.com/z3xox/BT3-Recomp");
}

void PS2SettingsOverlay::dumpSettingsToFile()
{
    const std::string dumpPath = s_configDir.empty()
        ? (std::filesystem::current_path() / kDumpFileName).string()
        : (std::filesystem::path(s_configDir) / kDumpFileName).string();

    std::ofstream file(dumpPath, std::ios::app);
    if (!file.is_open())
    {
        std::fprintf(stderr, "[dump] could not open %s\n", dumpPath.c_str());
        return;
    }

    file << "\n";
    file << "============================== " << nowTimestamp() << " ==============================\n";

    if (m_dumpAudio)
    {
        file << "[Audio]\n";
        file << "  master_volume = " << m_settings.masterVolume << "\n";
        file << "  music_volume  = " << m_settings.musicVolume << "\n";
        file << "  sfx_volume    = " << m_settings.sfxVolume << "\n";
        file << "\n";
    }

    if (m_dumpVideo)
    {
        file << "[Video]\n";
        file << "  gpu_renderer = " << (m_settings.gpuRenderer ? "1" : "0") << "\n";
        file << "  glow         = " << (m_settings.glow ? "1" : "0") << "\n";
        file << "  bilinear     = " << (m_settings.bilinear ? "1" : "0") << "\n";
        file << "  halftexel    = " << (m_settings.halfTexel ? "1" : "0") << "\n";
        file << "  skip_post    = " << (m_settings.skipPost ? "1" : "0") << "\n";
        file << "  skip_stale   = " << (m_settings.skipStaleVram ? "1" : "0") << "\n";
        file << "  render_scale = " << m_settings.renderScale << "\n";
        file << "  fullscreen   = " << (m_settings.fullscreen ? "1" : "0") << "\n";
        file << "  widescreen   = " << (m_settings.widescreen ? "1" : "0") << "\n";
        file << "\n";
    }

    if (m_dumpControllers)
    {
        file << "[Controllers]\n";
        file << "  selected_device = " << m_selectedDevice
             << " (" << (m_selectedDevice >= 0 && m_selectedDevice < (int)m_deviceList.size()
                         ? m_deviceList[m_selectedDevice].name : "?") << ")\n";
        file << "  edit_player     = " << m_editPlayer << "\n";
        file << "  deadzone        = " << m_settings.deadzone << "\n";
        file << "  overlay_gamepad = ";
        for (size_t i = 0; i < m_settings.overlayPadBtns.size(); ++i)
            file << (i ? "+" : "") << m_settings.overlayPadBtns[i];
        file << "\n";
        file << "  overlay_keys    = ";
        for (size_t i = 0; i < m_settings.overlayKeys.size(); ++i)
            file << (i ? "+" : "") << m_settings.overlayKeys[i];
        file << "\n";
        file << "\n";

        auto &pcfg = ps2_stubs::PadConfig::instance();
        for (size_t p = 0; p < ps2_stubs::PadConfig::kPlayerCount; ++p)
        {
            auto cfg = pcfg.snapshot(p);
            file << "  [Player " << (p + 1) << "]\n";
            file << "    device       = " << ps2_stubs::padDeviceDisplay(cfg.device) << "\n";
            for (size_t a = 0; a < static_cast<size_t>(ps2_stubs::PadAction::Count); ++a)
            {
                const auto act = static_cast<ps2_stubs::PadAction>(a);
                file << "    " << ps2_stubs::padActionName(act) << " = "
                     << ps2_stubs::padBindDisplay(cfg.binds[a]) << "\n";
            }
            file << "\n";
        }
    }

    if (m_dumpRuntime)
    {
        file << "[Runtime]\n";
        file << "  gpu_renderer_enabled = " << (GsGpuRenderer::enabled() ? "1" : "0") << "\n";
        file << "  glow_enabled         = " << (GsGpuRenderer::glowEnabled() ? "1" : "0") << "\n";
        file << "  bilinear_enabled     = " << (GsGpuRenderer::bilinearEnabled() ? "1" : "0") << "\n";
        file << "  halftexel_enabled    = " << (GsGpuRenderer::halfTexelEnabled() ? "1" : "0") << "\n";
        file << "  skip_post_enabled    = " << (GsGpuRenderer::skipPostEnabled() ? "1" : "0") << "\n";
        file << "  skip_stale_enabled   = " << (GsGpuRenderer::skipStaleVramEnabled() ? "1" : "0") << "\n";
        file << "  render_scale         = " << GsGpuRenderer::renderScale() << "\n";
        file << "  master_volume        = " << PS2AudioBackend::masterVolume() << "\n";
        file << "  music_volume         = " << PS2AudioBackend::musicVolume() << "\n";
        file << "  sfx_volume           = " << PS2AudioBackend::sfxVolume() << "\n";
        file << "  widescreen           = " << (PS2SettingsOverlay::isWidescreen() ? "1" : "0") << "\n";
        file << "\n";
    }

    if (m_dumpGamepad)
    {
        file << "[Gamepad Live]\n";
        std::array<uint8_t, 32> btn;
        std::array<float, 6> axis;
        if (m_selectedDevice >= 0 && m_selectedDevice < (int)m_deviceList.size())
            readGamepadStateForDevice(m_deviceList[m_selectedDevice], btn, axis);
        else
        {
            btn = {};
            axis = {};
        }
        file << "  buttons =";
        for (int b = 0; b < 32; ++b)
            if (btn[b]) file << " " << b;
        file << "\n";
        static const char *axN[6] = { "LX", "LY", "RX", "RY", "LT", "RT" };
        for (int a = 0; a < 6; ++a)
            file << "  " << axN[a] << " = " << axis[a] << "\n";
        file << "\n";
    }

    file << "========================================================================\n";
    file.close();

    std::fprintf(stderr, "[dump] settings written to %s\n", dumpPath.c_str());
}

void PS2SettingsOverlay::drawGamepadTestArea(const std::array<uint8_t, 32> &btnDown,
                                             const std::array<float, 6> &axis)
{
    // Buttons: [idx] label — matches the evdev codeToButton mapping used by the game.
    static const struct { int idx; const char *label; } kButtons[] = {
        { 13, "Select" }, { 15, "Start" }, { 14, "Guide" },
        { 1, "D-Up" },    { 3, "D-Down" }, { 4, "D-Left" }, { 2, "D-Right" },
        { 5, "Y / Tri" }, { 6, "B / Cir" }, { 7, "A / X" }, { 8, "X / Sq" },
        { 9, "LB / L1" }, { 11, "RB / R1" },
        { 16, "L3" },     { 17, "R3" },
    };
    static const int kNumBtns = static_cast<int>(sizeof(kButtons) / sizeof(kButtons[0]));

    const bool hasDevice = (m_selectedDevice >= 0 && m_selectedDevice < static_cast<int>(m_deviceList.size()));

    if (!hasDevice || m_deviceList[m_selectedDevice].kind == ps2_stubs::PadDeviceKind::Keyboard)
    {
        ImGui::TextDisabled("Select a gamepad device to test inputs.");
        return;
    }

    if (ImGui::BeginChild("##gpad_test", ImVec2(-1, 0), ImGuiChildFlags_Borders))
    {
        ImGui::Spacing();

        // --- Button grid ---
        ImGui::Text("Buttons");
        ImGui::Spacing();
        const float availX = ImGui::GetContentRegionAvail().x;
        const float btnW = std::max(24.0f, (availX - 8.0f * 9) / 8.0f);
        const float btnH = m_f1Mode ? 30.0f : 22.0f;   // [cttrmenu] F1 frame padding is taller
        int col = 0;
        for (int i = 0; i < kNumBtns; ++i)
        {
            const bool down = kButtons[i].idx >= 0 && kButtons[i].idx < 32 && btnDown[kButtons[i].idx] != 0;
            const ImVec2 sz(btnW, btnH);

            if (down)
            {
                ScopedStyleColor c0(ImGuiCol_Button, accent());
                ScopedStyleColor c1(ImGuiCol_ButtonHovered, gold());
                ScopedStyleColor c2(ImGuiCol_ButtonActive, gold());
                ScopedStyleColor c3(ImGuiCol_Text, dbz(0.10f, 0.07f, 0.03f));
                ImGui::Button(kButtons[i].label, sz);
            }
            else
            {
                ScopedStyleColor c0(ImGuiCol_Button, dbz(0.13f, 0.13f, 0.20f));
                ScopedStyleColor c1(ImGuiCol_ButtonHovered, dbz(0.18f, 0.18f, 0.26f));
                ScopedStyleColor c2(ImGuiCol_ButtonActive, dbz(0.20f, 0.20f, 0.28f));
                ScopedStyleColor c3(ImGuiCol_Text, dbz(0.55f, 0.55f, 0.62f));
                ImGui::Button(kButtons[i].label, sz);
            }

            ++col;
            if (col < 8)
                ImGui::SameLine();
            else
                col = 0;
        }
        ImGui::Spacing();

        // --- Unified stick / trigger visual ---
        ImGui::Text("Axes");
        ImGui::Spacing();

        const float stickR = 28.0f;
        const float gap = 14.0f;
        const float stickBox = stickR * 2.0f;
        ImDrawList *dl = ImGui::GetWindowDrawList();

        // --- Sticks: reserve a square per stick with Dummy, draw the circle inside it,
        //     keep labels in normal ImGui flow so the child window sizes correctly. ---
        auto drawStick = [&](int axX, int axY) {
            const float sx = axis[axX], sy = axis[axY];
            ImGui::Dummy(ImVec2(stickBox, stickBox));
            const ImVec2 boxMin = ImGui::GetItemRectMin();
            const ImVec2 boxMax = ImGui::GetItemRectMax();
            const ImVec2 c((boxMin.x + boxMax.x) * 0.5f, (boxMin.y + boxMax.y) * 0.5f);
            const float r = stickR;

            // Background circle + crosshair
            dl->AddCircleFilled(c, r, IM_COL32(24, 24, 38, 255), 48);
            dl->AddCircle(c, r, IM_COL32(255, 255, 255, 50), 48, 1.5f);
            dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), IM_COL32(255, 255, 255, 40), 1.0f);
            dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), IM_COL32(255, 255, 255, 40), 1.0f);

            // Position dot (clamped to circle)
            const float mag = std::sqrt(sx * sx + sy * sy);
            float dx = sx, dy = sy;
            if (mag > 1.0f) { dx /= mag; dy /= mag; }
            const ImVec2 pos(c.x + dx * r * 0.85f, c.y + dy * r * 0.85f);
            dl->AddCircleFilled(pos, 7.0f, IM_COL32(240, 160, 48, 255), 24);
            dl->AddCircle(pos, 7.0f, IM_COL32(255, 220, 120, 255), 24, 1.5f);
        };

        // Stick row (centered)
        const float rowW = stickBox * 2 + gap;
        const float rowOff = (ImGui::GetContentRegionAvail().x - rowW) / 2.0f;
        ImGui::Dummy(ImVec2(rowOff, 0));
        ImGui::SameLine();
        drawStick(0, 1);   // LX, LY
        ImGui::SameLine();
        drawStick(2, 3);   // RX, RY

        // Stick labels row (normal flow, centered)
        ImGui::Spacing();
        {
            char lbl[64];
            snprintf(lbl, sizeof lbl, "LX %+0.2f   RX %+0.2f", axis[0], axis[2]);
            float tw = ImGui::CalcTextSize(lbl).x;
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - tw) / 2.0f);
            ImGui::TextColored(gold(0.9f), "%s", lbl);
            snprintf(lbl, sizeof lbl, "LY %+0.2f   RY %+0.2f", axis[1], axis[3]);
            tw = ImGui::CalcTextSize(lbl).x;
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - tw) / 2.0f);
            ImGui::TextColored(gold(0.9f), "%s", lbl);
        }
        ImGui::Spacing();

        // --- Triggers: vertical bars, each with a Dummy for its box, labels normal. ---
        const float trH = 42.0f, trW = 16.0f;
        const float trigRowW = trW * 2 + gap;
        const float trigOff = (ImGui::GetContentRegionAvail().x - trigRowW) / 2.0f;
        ImGui::Dummy(ImVec2(trigOff, 0));
        ImGui::SameLine();

        auto drawTrigger = [&](int t) {
            const float v = axis[4 + t];
            ImGui::Dummy(ImVec2(trW, trH));
            const ImVec2 boxMin = ImGui::GetItemRectMin();
            const ImVec2 boxMax = ImGui::GetItemRectMax();

            // Track (bottom-up fill)
            dl->AddRectFilled(boxMin, boxMax, IM_COL32(30, 30, 45, 255), 4.0f);
            const float fillH = trH * std::min(1.0f, std::fabs(v));
            if (fillH > 1.0f)
                dl->AddRectFilled(ImVec2(boxMin.x, boxMax.y - fillH),
                                  boxMax,
                                  ImGui::ColorConvertFloat4ToU32(accent(0.6f + 0.4f * std::fabs(v))), 4.0f);
        };

        drawTrigger(0);   // LT
        ImGui::SameLine();
        drawTrigger(1);   // RT

        // Trigger labels (normal flow, centered)
        ImGui::Spacing();
        {
            char lbl[32];
            snprintf(lbl, sizeof lbl, "LT %.2f    RT %.2f", axis[4], axis[5]);
            float tw = ImGui::CalcTextSize(lbl).x;
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - tw) / 2.0f);
            ImGui::TextColored(gold(0.9f), "%s", lbl);
        }

        ImGui::Spacing();
        ImGui::Spacing();
    }
    ImGui::EndChild();
}

// [winbtn] shown in fullscreen while the mouse is in the top-right corner (and a moment after it leaves)
bool PS2SettingsOverlay::winButtonsWanted()
{
    static double s_until = -1.0;
    if (!IsWindowFullscreen() || !IsWindowFocused()) { s_until = -1.0; return false; }
    const Vector2 m = GetMousePosition();
    const float w = (float)GetScreenWidth();
    if (m.x >= w - 220.0f && m.y >= 0.0f && m.y <= 60.0f) s_until = GetTime() + 1.0;
    const bool shown = GetTime() < s_until;
    // the click is taken here from raylib's own mouse state, on the press, so another overlay window (the perf
    // readout shares the corner) or ImGui's window order can never swallow it
    if (shown && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && m.y >= 0.0f && m.y <= 32.0f && m.x >= w - 138.0f && m.x < w)
    {
        const int i = std::min(2, (int)((m.x - (w - 138.0f)) / 46.0f));
        std::fprintf(stderr, "[winbtn] %s\n", i == 0 ? "minimise" : i == 1 ? "window" : "close");
        if (i == 0) s_minimizeRequested = true;
        else if (i == 1) { m_settings.fullscreen = false; ps2xSetFullscreen(false, m_settings.windowW, m_settings.windowH); m_dirty = true; s_until = -1.0; }
        else g_ps2xCloseRequested.store(true);
    }
    return shown;
}
void PS2SettingsOverlay::drawWinButtons()
{
    const ImVec2 vp = ImGui::GetMainViewport()->Size;
    const float bw = 46.0f, bh = 32.0f;
    ImGui::SetNextWindowPos(ImVec2(vp.x, 0.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.65f);
    ImGui::SetNextWindowFocus();   // above the perf readout that shares the corner
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##winbtn", nullptr, fl))
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImU32 fg = IM_COL32(235, 235, 235, 255);
        for (int i = 0; i < 3; ++i)
        {
            if (i) ImGui::SameLine();
            ImGui::PushID(i);
            const bool hit = ImGui::InvisibleButton("##b", ImVec2(bw, bh));
            const bool hov = ImGui::IsItemHovered();
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            if (hov) dl->AddRectFilled(a, b, i == 2 ? IM_COL32(196, 43, 28, 255) : IM_COL32(255, 255, 255, 40));
            const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            if (i == 0) dl->AddLine(ImVec2(c.x - 5.0f, c.y), ImVec2(c.x + 5.0f, c.y), fg, 1.0f);
            else if (i == 1) dl->AddRect(ImVec2(c.x - 5.0f, c.y - 5.0f), ImVec2(c.x + 5.0f, c.y + 5.0f), fg, 0.0f, 0, 1.0f);
            else { dl->AddLine(ImVec2(c.x - 5.0f, c.y - 5.0f), ImVec2(c.x + 5.0f, c.y + 5.0f), fg, 1.0f);
                   dl->AddLine(ImVec2(c.x - 5.0f, c.y + 5.0f), ImVec2(c.x + 5.0f, c.y - 5.0f), fg, 1.0f); }
            if (hov) ImGui::SetTooltip("%s", i == 0 ? "Minimise (F8)" : i == 1 ? "Window (F11)" : "Close the game");
            (void)hit;   // the click itself is handled in winButtonsWanted()
            ImGui::PopID();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
}
void PS2SettingsOverlay::drawPerfHud()
{   // [perfhud] small translucent box in the top-left corner; numbers refresh once a second
    const PerfHudStats st = ps2xPerfHudStats();
    const ImVec2 vp = ImGui::GetMainViewport()->Size;
    const int corner = m_settings.perfCorner & 3;
    ImGui::SetNextWindowPos(ImVec2((corner & 1) ? vp.x - 10.0f : 10.0f, (corner & 2) ? vp.y - 10.0f : 10.0f), ImGuiCond_Always,
                            ImVec2((corner & 1) ? 1.0f : 0.0f, (corner & 2) ? 1.0f : 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    const ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##perfhud", nullptr, fl))
    {
        const ImVec4 good(0.55f, 1.0f, 0.55f, 1.0f), warn(1.0f, 0.85f, 0.35f, 1.0f), bad(1.0f, 0.45f, 0.40f, 1.0f);
        ImGui::TextColored(st.fps >= 29.0 ? good : st.fps >= 25.0 ? warn : bad, "FPS  %4.1f  (%4.1f ms)", st.fps, st.frameMs);
        if (st.pictures > 0.5)   // the native renderer's smooth motion: pictures on screen per second (their meter)
            ImGui::TextColored(st.pictures >= 55.0 ? good : st.pictures >= 40.0 ? warn : bad, "Shown %4.1f pictures/s", st.pictures);
        ImGui::Text("CPU  %3.0f%% game  %3.0f%% total", st.cpuGame, st.cpuTotal);
        if (st.haveGpu) ImGui::Text("GPU  %3.0f%% game  %3.0f%% total", st.gpuGame, st.gpuTotal);
        else ImGui::TextDisabled("GPU  n/a");
        if (st.gpuTempC >= 0) ImGui::Text("GPU  %d C", st.gpuTempC);
        ImGui::Text("RAM  %.0f MB   VRAM %.0f MB", st.ramMB, st.vramMB);
    }
    ImGui::End();
}
