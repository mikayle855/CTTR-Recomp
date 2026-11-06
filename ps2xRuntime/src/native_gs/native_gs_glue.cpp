// [native_gs] Glue between the PS2Recomp runtime and the renderer transplanted from the Tenkaichi3Decomp PC port
// (MIT, see LICENSE.Tenkaichi3Decomp; upstream commit in UPSTREAM_COMMIT). Their files stay as upstream wrote them
// apart from the [ps2x] entry points at the end of gs_core.c; everything they expect from their port lives here:
//  * the SDL3 subset gs_draw.c / gs_gl.c use (compat/SDL3/SDL.h), over the GLFW raylib links: one hidden window
//    whose GL 3.3 core context shares objects with raylib's (created on the main thread, NativeGs_CreateContext);
//  * their settings (Port_Setting...), globals (gPort*), the settings window (Ui_*), the texture pack (TexPack_*,
//    off for now) and the Vulkan back end (absent: the GL one is used);
//  * a worker thread that owns that context: the runtime pushes the VIF1 stream of each DMA transfer, the GIF
//    channel's data and the DISPFB1 writes (NativeGs_Push*), the worker feeds them to the renderer in order. The
//    DISPFB1 write (the display flip) ends the frame: BT4 sends several VIF1 chains per frame, so a chain's end (their
//    port's frame boundary, one chain per frame in BT3) is not one.
// PS2X_NATIVE_GS=1: shadow mode (the renderer runs beside paraLLEl-GS; BT3_SHOT=<n> dumps its picture every n frames
// to <cwd>\port\build\shots\gpu_NNNNN.ppm).
#include "native_gs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <GLFW/glfw3.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
static LONG nativeGsCrashFilter(EXCEPTION_POINTERS *ep);
#endif

#include "runtime/ps2_gs_gpu.h"         // GSTex0Reg / GSTexaReg / GSTexClutReg
#include "runtime/ps2_gs_rasterizer.h"  // the palette as the texture pack hashes it

extern "C" {
#include "compat/SDL3/SDL.h"
#include "gs_draw.h"
void NativeGs_TexRegs(uint64_t *tex0, uint64_t *texa);
void Gs_StoreImage(unsigned bp, unsigned w, unsigned h, unsigned char *rgb);
#include "ui.h"   // PortVideo, GsGpu_GetSettings / GsGpu_SetSettings
extern int gsInterp;
extern int gNativeGsInterpSkip;
#include "gs_texpack.h"
#include "ui.h"

void NativeGs_VifStream(const uint32_t *w, uint32_t count);
void NativeGs_GifStream(const uint8_t *p, uint32_t qwc);
void NativeGs_FrameEnd(void);
extern void (*gNativeGsMskPath3)(int masked);
extern unsigned gGsFrame;
const uint32_t *NativeGs_Vram(void);
unsigned NativeGs_GlShown(int *w, int *h);
void NativeGs_DumpShown(const char *name);
extern int gNativeGsNoPresent;
extern void (*gNativeGsOnShown)(void);
extern unsigned gPortVBlanks;
void GsGpu_InterpPump(int ahead_ms, int all);
}
extern "C" int gGsMainFbp = -1;   // gs_sony.c upstream: the frame buffer shown (from DISPFB1 here)

int s_aspectMilliGlue = 1333;
std::atomic<unsigned> g_packLookups{0}, g_packIdent{0}, g_packHits{0}, g_packLoaded{0};   // texture pack bridge counters  // their picture aspect (Port_AspectMilli), from our widescreen at each frame end
extern float g_ps2xWsScale;   // ps2_runtime.cpp [truews]: window aspect / 4:3 while widescreen is on, else 1
namespace
{
    GLFWwindow *g_window = nullptr;   // the renderer's hidden window (its context shares with raylib's)
    std::string g_error;
    std::atomic<bool> g_enabled{false};
    const uint8_t *g_guestVram = nullptr;   // our runtime's GS memory (PS2X_NATIVE_GS_VRAMCMP)

    struct Job
    {
        enum Kind : uint8_t { Vif1, Gif, Flip, FrameEnd, StoreImage } kind;
        uint32_t value = 0;
        std::vector<uint8_t> data;
    };
    std::mutex g_mu;
    std::condition_variable g_cv;
    std::deque<Job> g_jobs;
    size_t g_queuedBytes = 0;
    std::thread g_thread;
    std::atomic<uint64_t> g_dropped{0};
    constexpr size_t kMaxQueuedBytes = 256u << 20;   // shadow mode: drop frames rather than grow without bound

    void push(Job &&j)
    {
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_queuedBytes + j.data.size() > kMaxQueuedBytes)
            {
                if (g_dropped.fetch_add(1) % 1000u == 0u)
                    std::fprintf(stderr, "[native_gs] queue full (%zu MB): dropped %llu jobs so far\n", g_queuedBytes >> 20, (unsigned long long)g_dropped.load());
                return;
            }
            g_queuedBytes += j.data.size();
            g_jobs.push_back(std::move(j));
        }
        g_cv.notify_one();
    }

    // ---- present: the shown target's picture, copied into one of three textures the main thread draws ----
    typedef unsigned GLuintT;
    typedef void *GLsyncT;
    struct Gl
    {
        void (__stdcall *GenTextures)(int, GLuintT *);
        void (__stdcall *BindTexture)(unsigned, GLuintT);
        void (__stdcall *TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
        void (__stdcall *TexParameteri)(unsigned, unsigned, int);
        void (__stdcall *DeleteTextures)(int, const GLuintT *);
        void (__stdcall *GenFramebuffers)(int, GLuintT *);
        void (__stdcall *BindFramebuffer)(unsigned, GLuintT);
        void (__stdcall *FramebufferTexture2D)(unsigned, unsigned, unsigned, GLuintT, int);
        void (__stdcall *BlitFramebuffer)(int, int, int, int, int, int, int, int, unsigned, unsigned);
        GLsyncT (__stdcall *FenceSync)(unsigned, unsigned);
        void (__stdcall *WaitSync)(GLsyncT, unsigned, uint64_t);
        unsigned (__stdcall *ClientWaitSync)(GLsyncT, unsigned, uint64_t);
        void (__stdcall *DeleteSync)(GLsyncT);
        void (__stdcall *Flush)(void);
        bool ok = false;
        void load()
        {
            auto P = [](const char *n) { return glfwGetProcAddress(n); };
            GenTextures = (decltype(GenTextures))P("glGenTextures"); BindTexture = (decltype(BindTexture))P("glBindTexture");
            TexImage2D = (decltype(TexImage2D))P("glTexImage2D"); TexParameteri = (decltype(TexParameteri))P("glTexParameteri");
            DeleteTextures = (decltype(DeleteTextures))P("glDeleteTextures"); GenFramebuffers = (decltype(GenFramebuffers))P("glGenFramebuffers");
            BindFramebuffer = (decltype(BindFramebuffer))P("glBindFramebuffer"); FramebufferTexture2D = (decltype(FramebufferTexture2D))P("glFramebufferTexture2D");
            BlitFramebuffer = (decltype(BlitFramebuffer))P("glBlitFramebuffer"); FenceSync = (decltype(FenceSync))P("glFenceSync");
            WaitSync = (decltype(WaitSync))P("glWaitSync"); ClientWaitSync = (decltype(ClientWaitSync))P("glClientWaitSync"); DeleteSync = (decltype(DeleteSync))P("glDeleteSync"); Flush = (decltype(Flush))P("glFlush");
            ok = GenTextures && BindTexture && TexImage2D && TexParameteri && DeleteTextures && GenFramebuffers && BindFramebuffer &&
                 FramebufferTexture2D && BlitFramebuffer && FenceSync && WaitSync && ClientWaitSync && DeleteSync && Flush;
        }
    };
    Gl g_gl;   // loaded on the worker; the same entry points serve the main thread (one driver, one pixel format)
    constexpr unsigned GL_TEXTURE_2D_ = 0x0DE1, GL_RGBA8_ = 0x8058, GL_RGBA_ = 0x1908, GL_UNSIGNED_BYTE_ = 0x1401,
                       GL_TEXTURE_MIN_FILTER_ = 0x2801, GL_TEXTURE_MAG_FILTER_ = 0x2800, GL_LINEAR_ = 0x2601,
                       GL_TEXTURE_WRAP_S_ = 0x2802, GL_TEXTURE_WRAP_T_ = 0x2803, GL_CLAMP_TO_EDGE_ = 0x812F,
                       GL_READ_FRAMEBUFFER_ = 0x8CA8, GL_DRAW_FRAMEBUFFER_ = 0x8CA9, GL_COLOR_ATTACHMENT0_ = 0x8CE0,
                       GL_COLOR_BUFFER_BIT_ = 0x4000, GL_NEAREST_ = 0x2600, GL_SYNC_GPU_COMMANDS_COMPLETE_ = 0x9117;
    struct PresentSlot { GLuintT tex = 0; int w = 0, h = 0; GLsyncT fence = nullptr; };
    PresentSlot g_slots[3];
    GLuintT g_readFbo = 0, g_drawFbo = 0;
    std::mutex g_presentMu;
    int g_latest = -1;          // newest finished slot (the main thread takes it)
    int g_shownByMain = -1;     // the slot the main thread is drawing (never written while shown)
    uint64_t g_presentSeq = 0;
    GLsyncT g_lastFence = nullptr;   // the newest picture's fence (worker side)
    std::deque<GLsyncT> g_inFlight;  // one fence per picture handed to the GPU, oldest first (pacing only)
    unsigned g_gpuBehind = 0, g_inbetweenSkipped = 0;
    // the GPU is still drawing the last picture: in-between pictures would only queue up behind it (6x on a laptop GPU)
    bool gpuBusy()
    {
        if (!g_lastFence || !g_gl.ClientWaitSync) return false;
        constexpr unsigned GL_TIMEOUT_EXPIRED_ = 0x911B, GL_WAIT_FAILED_ = 0x911D;
        const unsigned r = g_gl.ClientWaitSync(g_lastFence, 0, 0);
        return r == GL_TIMEOUT_EXPIRED_ || r == GL_WAIT_FAILED_;
    }
    bool g_presenting = false;  // PS2X_NATIVE_GS=2: our window shows this renderer's picture

    // at most two pictures in flight: the renderer waits for the older one before drawing more, so it never runs
    // ahead of the GPU (a deep driver queue made the window's own swap wait behind it: 5 pictures/s at 6x)
    // Returns whether it had to wait: the GPU is then behind the pictures being asked of it.
    bool paceInFlight()
    {
        constexpr unsigned GL_SYNC_FLUSH_COMMANDS_BIT_ = 1, GL_ALREADY_SIGNALED_ = 0x911A;
        bool waited = false;
        while (g_inFlight.size() > 1)
        {
            if (g_gl.ClientWaitSync(g_inFlight.front(), 0, 0) != GL_ALREADY_SIGNALED_)
            {
                const auto t0 = std::chrono::steady_clock::now();
                g_gl.ClientWaitSync(g_inFlight.front(), GL_SYNC_FLUSH_COMMANDS_BIT_, 100000000ull);   // at most 100 ms each
                if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(2)) waited = true;
            }
            g_gl.DeleteSync(g_inFlight.front());
            g_inFlight.pop_front();
        }
        return waited;
    }

    void publishFrame()
    {
        if (!g_gl.ok) return;
        int w = 0, h = 0;
        const GLuintT src = NativeGs_GlShown(&w, &h);
        if (!src || w <= 0 || h <= 0) return;
        int slot = 0;
        {
            std::lock_guard<std::mutex> lk(g_presentMu);
            for (int k = 0; k < 3; ++k) if (k != g_latest && k != g_shownByMain) { slot = k; break; }
        }
        PresentSlot &s = g_slots[slot];
        if (s.fence) { if (s.fence == g_lastFence) g_lastFence = nullptr; g_gl.DeleteSync(s.fence); s.fence = nullptr; }
        if (!s.tex || s.w != w || s.h != h)
        {
            if (s.tex) g_gl.DeleteTextures(1, &s.tex);
            g_gl.GenTextures(1, &s.tex);
            g_gl.BindTexture(GL_TEXTURE_2D_, s.tex);
            g_gl.TexImage2D(GL_TEXTURE_2D_, 0, (int)GL_RGBA8_, w, h, 0, GL_RGBA_, GL_UNSIGNED_BYTE_, nullptr);
            g_gl.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_, (int)GL_LINEAR_);
            g_gl.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MAG_FILTER_, (int)GL_LINEAR_);
            g_gl.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_S_, (int)GL_CLAMP_TO_EDGE_);
            g_gl.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_T_, (int)GL_CLAMP_TO_EDGE_);
            s.w = w; s.h = h;
        }
        if (!g_readFbo) { g_gl.GenFramebuffers(1, &g_readFbo); g_gl.GenFramebuffers(1, &g_drawFbo); }
        g_gl.BindFramebuffer(GL_READ_FRAMEBUFFER_, g_readFbo);
        g_gl.FramebufferTexture2D(GL_READ_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_, src, 0);
        g_gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER_, g_drawFbo);
        g_gl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_, s.tex, 0);
        g_gl.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT_, GL_NEAREST_);
        g_gl.BindFramebuffer(GL_READ_FRAMEBUFFER_, 0);
        g_gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER_, 0);
        s.fence = g_gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE_, 0);
        g_lastFence = s.fence;
        g_inFlight.push_back(g_gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE_, 0));
        g_gl.Flush();   // the fence must reach the GPU before another context waits on it
        std::lock_guard<std::mutex> lk(g_presentMu);
        g_latest = slot;
        ++g_presentSeq;
        {   // PS2X_NATIVE_GS_PICBURST=<n>: when picburst.go appears (working directory), the next n pictures shown,
            // in-between ones too, go to pic_NNN_<ms>.ppm; then the file is removed (finding flicker on screen)
            static const int s_n = [] { const char *v = std::getenv("PS2X_NATIVE_GS_PICBURST"); return v ? std::atoi(v) : 0; }();
            static int s_left = 0, s_k = 0, s_poll = 0;
            if (s_n > 0 && s_left == 0 && ++s_poll % 15 == 0)
                if (FILE *go = std::fopen("picburst.go", "rb")) { std::fclose(go); s_left = s_n; s_k = 0; }
            if (s_left > 0)
            {
                char name[64];
                const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                std::snprintf(name, sizeof name, "pic_%03d_s%d_f%u_%lld.ppm", s_k++, (int)gNativeGsInterpSkip, gGsFrame, ms % 10000000);
                NativeGs_DumpShown(name);
                if (--s_left == 0) std::remove("picburst.go");
            }
        }
    }

    // [ps2x] path 3 as the GIF arbiter has it: while the VIF1 list masks it (MSKPATH3), GIF-channel uploads wait
    // and go to the GS at the point the list unmasks it. Worker thread only.
    double g_lastFlipGapMs = 0.0;   // [ps2x] the last game tick's length (loads: hundreds of ms)
    bool g_path3Masked = false;
    std::deque<std::vector<uint8_t>> g_path3Held;
    unsigned g_mskCount[2] = {0, 0}, g_heldCount = 0;
    void path3Flush()
    {
        while (!g_path3Held.empty())
        {
            const std::vector<uint8_t> &d = g_path3Held.front();
            NativeGs_GifStream(d.data(), (uint32_t)(d.size() / 16u));
            g_path3Held.pop_front();
        }
    }
    void onMskPath3(int masked)
    {
        g_mskCount[masked ? 1 : 0]++;
        g_path3Masked = masked != 0;
        if (!g_path3Masked) path3Flush();
    }

    void worker();
    void workerGuarded()
    {
#ifdef _WIN32
        __try { worker(); }
        __except (nativeGsCrashFilter(GetExceptionInformation())) {}
#else
        worker();
#endif
    }

    void worker()
    {
        glfwMakeContextCurrent(g_window);
        gNativeGsMskPath3 = onMskPath3;
        g_gl.load();
        gNativeGsNoPresent = 1;
        if (g_presenting) gNativeGsOnShown = publishFrame;
        std::vector<uint32_t> words;
        for (;;)
        {
            Job j;
            {
                std::unique_lock<std::mutex> lk(g_mu);
                g_cv.wait(lk, [] { return !g_jobs.empty(); });
                j = std::move(g_jobs.front());
                g_jobs.pop_front();
                g_queuedBytes -= j.data.size();
            }
            {   // PS2X_NATIVE_GS_JOBLOG=<frame>: the jobs of that frame, in order
                static const int s_jl = [] { const char *v = std::getenv("PS2X_NATIVE_GS_JOBLOG"); return v ? std::atoi(v) : -1; }();
                if (s_jl >= 0 && (int)gGsFrame >= s_jl && (int)gGsFrame < s_jl + 2)
                    std::fprintf(stderr, "[native_gs] frame %u job %s %zu bytes value %x\n", gGsFrame,
                                 j.kind == Job::Vif1 ? "VIF1" : j.kind == Job::Gif ? "GIF" : j.kind == Job::Flip ? "FLIP" : "END", j.data.size(), j.value);
            }
            switch (j.kind)
            {
            case Job::Vif1:
                words.resize(j.data.size() / 4u);
                if (!words.empty()) std::memcpy(words.data(), j.data.data(), words.size() * 4u);
                NativeGs_VifStream(words.data(), (uint32_t)words.size());
                break;
            case Job::Gif:
                if (g_path3Masked) { g_path3Held.push_back(std::move(j.data)); g_heldCount++; }
                else NativeGs_GifStream(j.data.data(), (uint32_t)(j.data.size() / 16u));
                break;
            case Job::Flip:
            {
                gGsMainFbp = (int)(j.value & 0x1FFu);
                {   // their vertical-blank count drives the in-between pictures: the blanks this game frame lasted
                    static auto s_prev = std::chrono::steady_clock::now();
                    const auto now = std::chrono::steady_clock::now();
                    const double ms = std::chrono::duration<double, std::milli>(now - s_prev).count();
                    g_lastFlipGapMs = ms;
                    s_prev = now;
                    int blanks = (int)(ms / 16.683 + 0.5);
                    gPortVBlanks += (unsigned)(blanks < 1 ? 1 : blanks > 4 ? 4 : blanks);
                }
                {   // their 2D (HUD) placement for a wide picture follows our widescreen setting
                    const float ws = g_ps2xWsScale > 0.5f && g_ps2xWsScale < 4.0f ? g_ps2xWsScale : 1.0f;
                    s_aspectMilliGlue = (int)(1333.333f * ws + 0.5f);
                }
                if (g_presenting)
                {
                    const bool busy = paceInFlight();
                    // the GPU is behind: this tick shows only its real picture. Uneven 17/33 ms pictures look worse than
                    // a steady 30, so more than 3 misses in 30 ticks pause smooth motion for 90 ticks (3 s), then retry.
                    // Each retry that fails again doubles the pause (3, 6, 12, 24 s): a GPU that cannot keep up at this
                    // resolution settles to a steady 30 instead of switching between 60 and 30 every few seconds (8x on an
                    // RTX 3050: 60/30 flips every ~3 s). 10 s of in-between pictures without trouble forget the failures.
                    static unsigned s_ticks = 0, s_misses = 0, s_pause = 0, s_fails = 0, s_good = 0, s_grace = 0;
                    // A load (a tick over 100 ms) is no sign of a slow GPU: the new stage's textures are going up and the
                    // first pictures after it are late once. Counted as misses, they paused smooth motion for the first
                    // 3-6 s of every fight's intro (seen: 4-6 s at 30 pictures/s after each map load).
                    if (g_lastFlipGapMs > 100.0) { s_pause = 0; s_ticks = s_misses = 0; s_grace = 15; }
                    if (busy && s_grace == 0) ++s_misses;
                    if (s_grace > 0) --s_grace;
                    if (s_pause == 0 && ++s_good >= 300) { s_fails = 0; s_good = 300; }
                    if (++s_ticks >= 30)
                    {
                        if (s_misses > 3) { s_pause = 90u << (s_fails < 3 ? s_fails : 3); ++s_fails; s_good = 0; }
                        s_ticks = s_misses = 0;
                    }
                    gNativeGsInterpSkip = (busy || s_pause > 0) ? 1 : 0;
                    if (s_pause > 0) --s_pause;
                    if (gNativeGsInterpSkip) ++g_inbetweenSkipped;
                }
                NativeGs_FrameEnd();   // shows the first in-between picture of a 30 fps frame (interpolation on)
                if (g_presenting)
                {
                    // the rest and the real picture, each at its time -- unless the GPU has not finished the last picture
                    // yet: then only the real one, at once (smooth motion steps down instead of the screen stalling)
                    if (gNativeGsInterpSkip) GsGpu_InterpPump(0, 1);
                    else GsGpu_InterpPump(40, 0);
                }
                {   // PS2X_NATIVE_GS_VRAMCMP=<frame>: the 8 KB pages where their GS memory differs from ours
                    static const int s_vc = [] { const char *v = std::getenv("PS2X_NATIVE_GS_VRAMCMP"); return v ? std::atoi(v) : -1; }();
                    if (s_vc >= 0 && (int)gGsFrame == s_vc && g_guestVram)
                    {
                        const uint8_t *theirs = reinterpret_cast<const uint8_t *>(NativeGs_Vram());
                        std::string line;
                        for (int pg = 0; pg < 512; ++pg)
                        {
                            int diff = 0;
                            for (int k = 0; k < 8192; k += 4) diff += std::memcmp(theirs + pg * 8192 + k, g_guestVram + pg * 8192 + k, 4) != 0;
                            if (diff) { char b[32]; std::snprintf(b, sizeof b, " %03x:%d%%", pg, diff * 100 / 2048); line += b; }
                        }
                        std::fprintf(stderr, "[native_gs] frame %u VRAM pages that differ (page:percent of words):%s\n", gGsFrame, line.c_str());
                    }
                }
                static auto s_last = std::chrono::steady_clock::now();
                const auto now = std::chrono::steady_clock::now();
                if (std::getenv("PS2X_NATIVE_GS_LOG") && now - s_last >= std::chrono::seconds(1))
                {
                    s_last = now;
                    std::fprintf(stderr, "[native_gs] per second: pack lookups %u hashed %u found %u loaded %u | in-between skipped (GPU busy) %u | queue %zu MB\n",
                                 g_packLookups.exchange(0), g_packIdent.exchange(0), g_packHits.exchange(0), g_packLoaded.exchange(0), g_inbetweenSkipped, g_queuedBytes >> 20);
                    g_inbetweenSkipped = 0;
                    g_mskCount[0] = g_mskCount[1] = g_heldCount = 0;
                }
                break;
            }
            case Job::FrameEnd:
                NativeGs_FrameEnd();
                break;
            case Job::StoreImage:
            {   // the game reads a frame back (its cross-fade): kept on the card here, the game was handed black
                const uint32_t w = j.data.size() >= 8 ? *reinterpret_cast<const uint32_t *>(j.data.data()) : 0;
                const uint32_t h = j.data.size() >= 8 ? *reinterpret_cast<const uint32_t *>(j.data.data() + 4) : 0;
                static std::vector<unsigned char> s_rgb;
                s_rgb.resize(size_t(std::min<uint32_t>(w, 512u)) * h * 3u + 16u);
                if (w && h) Gs_StoreImage(j.value, w, h, s_rgb.data());
                break;
            }
            }
        }
    }
}

#ifdef _WIN32
// A crash on the renderer's thread: where (module + offset, for llvm-symbolizer with the exe's PDB), then the process
// ends as before. (The runtime's crash reporter watches the game's threads, not this one.)
static LONG nativeGsCrashFilter(EXCEPTION_POINTERS *ep)
{
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    HMODULE mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCSTR>(er->ExceptionAddress), &mod);
    char name[MAX_PATH] = "?";
    if (mod) GetModuleFileNameA(mod, name, MAX_PATH);
    std::fprintf(stderr, "[native_gs] exception: code 0x%08lx at %p = %s+0x%llx (access %llu at %p)\n",
                 er->ExceptionCode, er->ExceptionAddress, name,
                 (unsigned long long)((const char *)er->ExceptionAddress - (const char *)mod),
                 er->NumberParameters > 0 ? (unsigned long long)er->ExceptionInformation[0] : 0ull,
                 er->NumberParameters > 1 ? (void *)er->ExceptionInformation[1] : nullptr);
    void *frames[32];
    const USHORT n = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
    for (USHORT i = 0; i < n; ++i)
    {
        HMODULE m = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)frames[i], &m);
        std::fprintf(stderr, "[native_gs]   #%u +0x%llx%s\n", i, (unsigned long long)((const char *)frames[i] - (const char *)m), m == mod ? "" : " (other module)");
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

#ifdef _WIN32
// PS2X_NATIVE_GS_VEH=1 (diagnostics): the first access violations on ANY thread, first chance, with module + offset
// and a short stack; nothing is handled (the process goes on to its normal handling).
static LONG CALLBACK nativeGsVeh(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode != 0xC0000005u) return EXCEPTION_CONTINUE_SEARCH;
    static std::atomic<int> s_n{0};
    if (s_n.fetch_add(1) >= 3) return EXCEPTION_CONTINUE_SEARCH;
    std::fprintf(stderr, "[native_gs] access violation on thread %lu:\n", GetCurrentThreadId());
    nativeGsCrashFilter(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

bool NativeGs_Enabled() { return g_enabled.load(std::memory_order_relaxed); }

// ---- the renderer's settings: <exe dir>\savedata\native_renderer.txt (key = value), the F1 window writes it ----
extern "C" volatile int gNativeGsAniso;   // gs_gl.c [ps2x] anisotropic filtering
namespace
{
    NativeGsSettings g_cfg;   // defaults in native_gs.h
    std::string cfgPath()
    {
        const char *d = std::getenv("PS2X_EXEDIR");
        std::string base = d && d[0] ? std::string(d) : std::string(".");
        if (!base.empty() && base.back() != '\\' && base.back() != '/') base += '\\';
        return base + "savedata\\native_renderer.txt";
    }
    void cfgLoad()
    {
        FILE *f = std::fopen(cfgPath().c_str(), "r");
        if (!f) return;
        char line[256];
        while (std::fgets(line, sizeof line, f))
        {
            char key[64]; int v = 0;
            if (std::sscanf(line, " %63[a-z_0-9] = %d", key, &v) != 2) continue;
            const std::string k = key;
            if (k == "enabled") g_cfg.enabled = v != 0;
            else if (k == "scale") g_cfg.scale = std::clamp(v, 1, 8);
            else if (k == "smooth") g_cfg.smooth = std::clamp(v, 0, 3);
            else if (k == "smooth_2d") g_cfg.smooth2d = v != 0;
            else if (k == "fx_off") g_cfg.fxOff = v & 63;
            else if (k == "glow") g_cfg.glow = std::clamp(v, 0, 200);
            else if (k == "aniso") g_cfg.aniso = std::clamp(v, 1, 16);
        }
        std::fclose(f);
    }
    void cfgSave()
    {
        FILE *f = std::fopen(cfgPath().c_str(), "w");
        if (!f) return;
        std::fprintf(f, "# the native renderer (from the Tenkaichi3Decomp PC port); F1 in game changes these\n");
        std::fprintf(f, "enabled = %d\nscale = %d\nsmooth = %d\nsmooth_2d = %d\nfx_off = %d\nglow = %d\naniso = %d\n",
                     g_cfg.enabled ? 1 : 0, g_cfg.scale, g_cfg.smooth, g_cfg.smooth2d ? 1 : 0, g_cfg.fxOff, g_cfg.glow, g_cfg.aniso);
        std::fclose(f);
    }
}

void NativeGs_GetSettings(NativeGsSettings &s) { s = g_cfg; }

void NativeGs_SetSettings(const NativeGsSettings &s)
{
    const bool live = NativeGs_Presenting();
    g_cfg = s;
    g_cfg.scale = std::clamp(g_cfg.scale, 1, 8);
    g_cfg.smooth = std::clamp(g_cfg.smooth, 0, 3);
    g_cfg.fxOff &= 63;
    g_cfg.glow = std::clamp(g_cfg.glow, 0, 200);
    g_cfg.aniso = std::clamp(g_cfg.aniso, 1, 16);
    gNativeGsAniso = g_cfg.aniso;
    cfgSave();
    if (!live) return;
    PortVideo v{};
    GsGpu_GetSettings(&v);   // (their window, display and volumes stay as they are)
    v.scale = g_cfg.scale;
    v.fxOff = g_cfg.fxOff;
    v.glow = g_cfg.glow;
    v.smooth2d = g_cfg.smooth2d ? 1 : 0;
    GsGpu_SetSettings(&v);   // the resolution changes between two frames
    gsInterp = g_cfg.smooth;
}
void NativeGs_SetGuestVram(const uint8_t *vram) { g_guestVram = vram; }

bool NativeGs_CreateContext()
{
    static bool s_done = false;
    if (s_done) return g_window != nullptr;
    s_done = true;
    cfgLoad();
    gNativeGsAniso = g_cfg.aniso;
#ifdef _WIN32
    if (const char *v = std::getenv("PS2X_NATIVE_GS_VEH")) if (v[0] == '1') AddVectoredExceptionHandler(1, nativeGsVeh);
#endif
    const char *mode = std::getenv("PS2X_NATIVE_GS");
    if ((!mode || !mode[0]) && g_cfg.enabled) mode = "2";   // native_renderer.txt: enabled = 1
    if (!mode || !mode[0] || mode[0] == '0') return false;
    GLFWwindow *share = glfwGetCurrentContext();
    if (!share) { std::fprintf(stderr, "[native_gs] no current GL context to share with: off\n"); return false; }
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    g_window = glfwCreateWindow(1280, 896, "native_gs", nullptr, share);
    glfwDefaultWindowHints();
    glfwMakeContextCurrent(share);   // raylib's context stays current on the main thread
    if (!g_window) { std::fprintf(stderr, "[native_gs] could not create the shared GL 3.3 context: off\n"); return false; }
    g_presenting = mode[0] == '2';
    g_thread = std::thread(workerGuarded);
    g_thread.detach();
    g_enabled.store(true);
    std::fprintf(stderr, "[native_gs] renderer from the Tenkaichi3Decomp port: %s (PS2X_NATIVE_GS=%s)\n", g_presenting ? "presenting" : "shadow mode", mode);
    return true;
}

bool NativeGs_Presenting() { return NativeGs_Enabled() && g_presenting; }

bool NativeGs_SkipOurVu1()
{
    static const bool s_run = [] { const char *v = std::getenv("PS2X_NATIVE_GS_RUNVU1"); return v && v[0] == '1'; }();
    return !s_run && NativeGs_Presenting();
}

bool NativeGs_TakePresent(unsigned &tex, int &w, int &h, uint64_t &seq)
{
    if (!NativeGs_Presenting()) return false;
    std::lock_guard<std::mutex> lk(g_presentMu);
    if (g_latest < 0) return false;
    PresentSlot &s = g_slots[g_latest];
    if (s.fence) g_gl.WaitSync(s.fence, 0, ~0ull);   // GPU-side wait in the caller's (raylib's) context
    g_shownByMain = g_latest;
    tex = s.tex; w = s.w; h = s.h; seq = g_presentSeq;
    return true;
}

void NativeGs_PushVif1(const uint8_t *data, size_t bytes, bool chain)
{
    if (!NativeGs_Enabled() || !data || bytes < 4) return;
    Job j{Job::Vif1};
    j.data.assign(data, data + (bytes & ~size_t(3)));
    (void)chain;
    push(std::move(j));
}

void NativeGs_PushGif(const uint8_t *data, size_t bytes)
{
    if (!NativeGs_Enabled() || !data || bytes < 16) return;
    Job j{Job::Gif};
    j.data.assign(data, data + (bytes & ~size_t(15)));
    push(std::move(j));
}

void NativeGs_PushStoreImage(uint32_t vramAddr, uint32_t w, uint32_t h)
{
    if (!NativeGs_Enabled()) return;
    Job j{Job::StoreImage};
    j.value = vramAddr;
    j.data.resize(8);
    std::memcpy(j.data.data(), &w, 4);
    std::memcpy(j.data.data() + 4, &h, 4);
    push(std::move(j));
}

void NativeGs_PushDispFb(uint64_t dispfb1)
{
    if (!NativeGs_Enabled()) return;
    Job j{Job::Flip};
    j.value = (uint32_t)(dispfb1 & 0x1FFu);
    push(std::move(j));
}

// ---- what the transplanted renderer expects from its port ----------------------------------------------------
extern "C" {

int gPortResim = 0;
volatile uint32_t gPortDataStripped = 1;   // "a release program": the GPU renderer starts without BT3_GS=gpu
int gPortMenuMode = 0;
int gPortMusicPercent = 100;
int gPortSePercent = 100;
unsigned long long gPortSleptNs = 0;
unsigned gPortVBlanks = 0;
volatile int gUiPresentX, gUiPresentY, gUiPresentW, gUiPresentH;

int Port_AspectMilli(void) { return s_aspectMilliGlue; }
void Port_SetAspectMilli(int v) { s_aspectMilliGlue = v; }
int Port_IsWide(void) { return s_aspectMilliGlue > 1400; }
int Port_Setting(const char *name, int def)
{
    if (std::strcmp(name, "gpu_api") == 0) return 1;   // the OpenGL back end (the Vulkan one needs SDL3)
    if (std::strcmp(name, "texture_pack") == 0) return 1;   // ps2tex applies the user's switch (Mod folders always on)
    // their native depth tint (fx bit 4, BT3's GfxDepthFog through the fog ramp) turns BT4's far scenery nearly black:
    // off by default until it is matched to BT4 (BT3_FX_OFF overrides)
    if (std::strcmp(name, "fx_off") == 0) return g_cfg.fxOff;
    // in-between pictures (60 fps from the game's 30 fps fights): on unless PS2X_NATIVE_GS_INTERP=0
    if (std::strcmp(name, "interp") == 0) { const char *v = std::getenv("PS2X_NATIVE_GS_INTERP"); return v ? std::atoi(v) : (g_presenting ? g_cfg.smooth : 0); }
    if (std::strcmp(name, "scale") == 0) { const char *v = std::getenv("PS2X_NATIVE_GS_SCALE"); return v ? std::atoi(v) : g_cfg.scale; }
    if (std::strcmp(name, "glow") == 0) return g_cfg.glow;
    if (std::strcmp(name, "smooth_2d") == 0) return g_cfg.smooth2d ? 1 : 0;
    return def;
}
void Port_SettingSave(const char *, int) {}
void Port_SettingsWrite(void) {}
void Port_AudioRefresh(void) {}
int Port_DataState(void) { return 0; }                 // their disc-data check (plat_verify.c): not ours
void ScrXfade_RequestCapture(void) {}                 // their game's fight cross-fade (a BT3_XFADE_AT test hook only)
void ScrXfade_Start(int, uint32_t) {}
int Port_VBlankUsLeft(void) { return 0; }

// The Vulkan back end (gs_gpu.c, SDL3 GPU) is not built: its init fails, so the GL one is taken.
static int vk_init(void) { return 0; }
GsBackend sVulkanBackend = {"vulkan", vk_init};

// The settings window (ui.cpp) is not built.
int Ui_Init(SDL_Window *, SDL_GPUDevice *, void *) { return 1; }
void Ui_DrawGL(void) {}
int Ui_Event(const SDL_Event *) { return 0; }
void Ui_Toggle(void) {}

// ---- texture packs: removed (CTTR draws its own textures); the native renderer is told there is none ----
void TexPack_Init(void) {}
int TexPack_Count(void) { return 0; }
const char *TexPack_Lookup(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *) { return nullptr; }
int TexPack_LoadAsync(const char *, TexPackImage *) { return 0; }
int TexPack_Load(const char *, TexPackImage *) { return 0; }
void TexPack_Free(TexPackImage *img) { if (img) { std::free(img->file); img->file = nullptr; } }

// ---- SDL3 subset over GLFW ----
bool SDL_Init(uint32_t) { return true; }
const char *SDL_GetError(void) { return g_error.c_str(); }
SDL_DisplayID *SDL_GetDisplays(int *count) { if (count) *count = 0; return nullptr; }
bool SDL_GetDisplayBounds(SDL_DisplayID, SDL_Rect *r) { if (r) *r = SDL_Rect{0, 0, 1920, 1080}; return true; }
const char *SDL_GetDisplayName(SDL_DisplayID) { return "display"; }
SDL_PropertiesID SDL_CreateProperties(void) { return 1; }
void SDL_DestroyProperties(SDL_PropertiesID) {}
bool SDL_SetStringProperty(SDL_PropertiesID, const char *, const char *) { return true; }
bool SDL_SetNumberProperty(SDL_PropertiesID, const char *, int64_t) { return true; }
bool SDL_SetBooleanProperty(SDL_PropertiesID, const char *, bool) { return true; }
SDL_Window *SDL_CreateWindowWithProperties(SDL_PropertiesID) { return reinterpret_cast<SDL_Window *>(g_window); }
bool SDL_SetWindowSize(SDL_Window *, int, int) { return true; }
bool SDL_SetWindowTitle(SDL_Window *, const char *) { return true; }
bool SDL_GetWindowSize(SDL_Window *, int *x, int *y) { if (x) *x = 1280; if (y) *y = 896; return true; }
bool SDL_GetWindowSizeInPixels(SDL_Window *w, int *x, int *y) { return SDL_GetWindowSize(w, x, y); }
bool SDL_SetWindowFullscreen(SDL_Window *, bool) { return true; }
void SDL_free(void *p) { std::free(p); }
void *SDL_malloc(size_t n) { return std::malloc(n); }
void SDL_DelayPrecise(uint64_t ns) { std::this_thread::sleep_for(std::chrono::nanoseconds(ns)); }
bool SDL_PollEvent(SDL_Event *) { return false; }
bool SDL_GL_SetAttribute(SDL_GLAttr, int) { return true; }
SDL_GLContext SDL_GL_CreateContext(SDL_Window *w) { return w; }
bool SDL_GL_MakeCurrent(SDL_Window *w, SDL_GLContext) { glfwMakeContextCurrent(reinterpret_cast<GLFWwindow *>(w)); return true; }
bool SDL_GL_SetSwapInterval(int) { return true; }
void *SDL_GL_GetProcAddress(const char *name) { return reinterpret_cast<void *>(glfwGetProcAddress(name)); }
bool SDL_GL_ExtensionSupported(const char *ext) { return glfwExtensionSupported(ext) == GLFW_TRUE; }
bool SDL_GL_SwapWindow(SDL_Window *) { return true; }   // hidden window: nothing to show
}
