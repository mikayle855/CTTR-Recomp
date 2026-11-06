#pragma once

#include "ps2_runtime.h"
#include "runtime/pad_config.h"
#include <string>
#include <vector>
#include <array>
#include <cstdint>

struct ImFont;

class PS2SettingsOverlay
{
public:
    struct Settings
    {
        float masterVolume = 1.0f;
        float musicVolume = 1.0f;
        float sfxVolume = 1.0f;
        bool gpuRenderer = true;
        // [renderer] 0 = OpenGL (our GS renderer), 1 = software rasterizer, 2 = paraLLEl-GS (Vulkan compute; only when
        // the backend is built in). gpuRenderer stays in sync (renderer != 1) for the code that still reads it.
        static constexpr int kRendererOpenGL = 0, kRendererSoftware = 1, kRendererParallelGS = 2;
#if defined(PS2X_HAVE_PGS)
        static constexpr int kRendererDefault = 2;
#else
        static constexpr int kRendererDefault = 0;
#endif
        int renderer = kRendererDefault;
        bool glow = true;
        bool glowFix = true;   // [glowfix] BT3's bloom/glow chain (Kaioken aura); applies on restart
        bool bilinear = true;
        bool halfTexel = true;
        bool skipPost = true;
        bool skipStaleVram = true;
        int renderScale = 1; // built-in internal resolution multiplier derived from the window size: 720p=1x, 1080p=2x, 1440p+=3x
        float deadzone = 0.15f;
        bool fullscreen = false;  // [defaults-sync] do not force fullscreen on first launch (rig + user surprise); one toggle away in the overlay
        bool widescreen = false;
        bool outline = true;
        bool fps60 = false;   // [fps60] 60 fps mode: fight step 1 + the pacing table (applies between fights)
        int inkStrength = 199;   // [inkstrength] cel-outline darkener, % of Cs (199 = hardware 255/128)
        int inkWidth = 100;      // [pgsink] paraLLEl-GS: outline stroke width, % of a PS2 texel (100 = native, 25 = thinnest)
        unsigned inkColor = 0;   // [pgsink] paraLLEl-GS: outline colour 0xRRGGBB (0 = the game's black)
        bool shadows = true;
        bool dofBlur = true;
        int dofZFar = 200000;
        int windowW = 0, windowH = 0; // 0 = keep the default host window size
        int windowX = -100000, windowY = -100000;   // [winmem] last windowed position (-100000 = centre)
        bool forceBilinear = true;    // PCSX2-style forced texture filtering (default ON)
        int cttrAspect = 0;           // [cttraspect] 0 Auto Standard, 1 Fit, 2 4:3, 3 16:9, 4 10:7
        int cttrMovieAspect = 0;      // [movieaspect] movies: 0 same as the picture, 1 always 4:3, 2 fill the window
        bool cttrSkipIntro = false;   // [skipintro] the game's own SkipIntroMovies option (boot logo movies)
        int cttrLanguage = 1;         // [cttrlang] PS2 system language the game sees: 1 En, 2 Fr, 3 Es, 4 De, 5 It, 6 Nl (next start)
        bool pauseUnfocused = true;   // [focuspause] CTTR: freeze the game + sound while the window is not focused
        int pgsAniso = 1;             // [aniso] paraLLEl-GS anisotropic filtering: 1 = off, 2/4/8/16
        // [postfx] present-time image controls (all neutral = the plain blit)
        bool fxaa = false;
        bool perfOverlay = false;   // [perfhud] corner readout: fps, CPU, GPU, RAM/VRAM (F10 toggles)
        int perfCorner = 2;         // [perfhud] 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
        int sharpen = 0;       // 0..100 %
        int brightness = 0;    // -50..50
        int contrast = 100;    // 50..150 %
        int saturation = 100;  // 0..200 %
        int gamma = 100;       // 50..200
        bool overlayEnabled = true;   // [launcher] master switch: false = in-game overlay never opens
        int hudLayout = 0;            // widescreen HUD: 0=centered 4:3, 1=edge-pinned, 2=custom
        int hudOffL = 0, hudOffC = 0, hudOffR = 0;  // custom layout x-offsets (512-space px)
        // Overlay launch / close binding. Each is a SET of inputs that must ALL be held
        // together (1+ entries). Captured with a 3s hold during binding.
        std::vector<int> overlayPadBtns = {13, 15};  // default: Select (Back) + Start
        std::vector<int> overlayKeys   = {340, 258}; // default: Left Shift + Tab
        // [loglevel] diagnostic verbosity applied at startup: 0=off, 1=profile+mclog+sched,
        // 2=+ftspike+fightprobe+reveal, 3=+frameprof+camprobe. Drives which PS2X_* env vars
        // main() exports; stderr is redirected to <deploy>/logs/bt3.log on any level > 0.
        int logLevel = 1;
        bool meter = false;   // [f1] their meter: "60 fps  speed 100%" in the top-left corner
        int display = 0;      // [f1] 0 = the monitor the desktop chooses, else monitor n (1-based)
        // [cheats] settings.toml [cheats]; the env (PS2X_ABILITY_BONUS / PS2X_ITEMCAP / PS2X_EXTRA_ITEM_ROWS) wins
        int abilityBonus = 0;   // Evolution Z ability limit + n
        int itemCap = 20;       // fight item stat levels up to +n (20 = the game's cap)
        int extraItemRows = 0;  // extra pages of 7 Z items per custom set (0..2)
        bool modWidescreen = false;   // [cttrmods] F1 > Mods: true 16:9 (wider 3D view, 16:9 picture)
        int cttrCheats = 0;     // [cttrcheats] CTTR cheat toggles kept on (bit = cheat, game_overrides.cpp CttrCheat*)

        bool operator==(const Settings &o) const;
    };

    static bool isWidescreen() { return s_widescreen; }
    static int getLogLevel() { return s_logLevel; }
    static int getStartupLogLevel() { return s_startupLogLevel; }

    static void preloadSettings();
    void syncFromRuntime();   // seed m_settings from live runtime state
    // Set the directory that holds settings.toml (portable dist: <root>/savedata).
    static void setConfigDirectory(const std::string &dir);

    struct DeviceInfo
    {
        std::string name;          // "Microsoft Xbox Controller (event257)"
        int glfwSlot = -1;         // GLFW slot index, -1 if not GLFW
        bool isEvdev = false;      // true if evdev native matches this device
        ps2_stubs::PadDeviceKind kind = ps2_stubs::PadDeviceKind::None;
    };

    void initialize();
    void draw(PS2Runtime &runtime);
    void shutdown();

private:
    static bool s_widescreen;
    static std::string s_configDir;
    static int s_logLevel;        // [loglevel] effective level for the live overlay UI
    static int s_startupLogLevel; // [loglevel] captured before init so main() can apply env vars

    bool m_visible = false;
    bool m_initialized = false;
    bool m_dirty = false;
    Settings m_settings;
    Settings m_settingsAtBoot;
    std::string m_configPath;
    int m_activeTab = 0;

    // Open/close deploy animation. m_animT is an eased 0..1 value: 0 = fully
    // closed/hidden, 1 = fully open. It chases m_visible every frame, so the
    // panel keeps rendering (fading + sliding out) for a few frames AFTER
    // m_visible flips to false, instead of popping away instantly.
    float m_animT = 0.0f;

    // Capsule HUD display font (Russo One), loaded once in initialize().
    // Null (falls back to the default ImGui font) if the .ttf couldn't be found.
    ImFont *m_fontHudTitle = nullptr; // title bar
    ImFont *m_fontHudLabel = nullptr; // tab labels + section headers

    // Settings dump logging — which areas are captured to the dump log.
    bool m_dumpAudio = true;
    bool m_dumpVideo = true;
    bool m_dumpControllers = true;
    bool m_dumpRuntime = true;
    bool m_dumpGamepad = false;   // live test state, only meaningful while capture is read

    // Device selection
    std::vector<DeviceInfo> m_deviceList;
    int m_selectedDevice = 0;

    // Binding capture state
    int m_captureAction = -1;
    bool m_captureWaitRelease = false;
    int m_editPlayer = 0;

    // Edge detection state (always updated, even when not capturing)
    std::array<uint8_t, 32> m_prevBtnDown{};
    std::array<float, 6> m_prevAxis{};

    // Overlay-toggle combo capture state (3s hold to bind one or more inputs).
    int m_captureComboSlot = -1;          // 0 = gamepad, 1 = keyboard, -1 = none
    float m_captureComboTimer = 0.0f;     // seconds remaining in the capture window
    bool m_captureComboHadAny = false;    // whether something was captured yet
    std::vector<int> m_capturedBtns;      // buttons held during capture
    std::vector<int> m_capturedKeys;      // keys held during capture
    std::array<uint8_t, 32> m_prevComboBtn{};

    // Gamepad toggle combo edge state
    bool m_prevToggleCombo = false;

    // Whether the Bindings / Overlay-settings sub-window (popup) is open.
    bool m_showBindingsPopup = false;
    bool m_f1Mode = false;   // [f1] the Controls tab is being drawn inside the F1 window (bindings inline)

    void toggleVisible();
    void resetCaptureState();
    void loadSettings();
    void saveSettings() const;
    void applySettings();
    void applyDeadzone();
    void buildDeviceList();
    void readGamepadStateForDevice(const DeviceInfo &dev,
                                   std::array<uint8_t, 32> &btnDown,
                                   std::array<float, 6> &axis);
    void drawAudioTab();
    void drawVideoTab();
    void drawControllersTab();
    void drawLoggingTab();
    void drawPerfHud();   // [perfhud]
    void drawF1Window();  // [f1] Settings: Video, Effects, Audio, Controls, Cheats (Tenkaichi3Decomp's window)
    void drawMeter();     // [f1] their frame rate / speed line
    bool shotNoticeActive();   // [screenshot] the "saved" notice is showing
    void drawShotNotice();     // [screenshot]
    bool winButtonsWanted();   // [winbtn]
    void drawWinButtons();     // [winbtn]
    void drawAboutTab();
    void drawNetplayTab();   // [netplay]
    void drawGamepadTestArea(const std::array<uint8_t, 32> &btnDown,
                             const std::array<float, 6> &axis);
    void drawBindingsTable();
    void drawOverlayToggle();
    void drawBindingsPopup();
    void dumpSettingsToFile();
};
