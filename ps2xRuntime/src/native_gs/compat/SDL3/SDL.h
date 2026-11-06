/* [native_gs] The subset of SDL3 the transplanted renderer (gs_draw.c / gs_gl.c) uses, implemented over the GLFW
   that raylib already links (native_gs_glue.cpp). The renderer gets its own hidden GLFW window whose GL context shares
   objects with raylib's: it never shows a window or reads input; our runtime presents its picture. */
#ifndef NATIVE_GS_FAKE_SDL3_H
#define NATIVE_GS_FAKE_SDL3_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* every function renamed: the runtime also links the real SDL2, whose symbols must not collide with these */
#define SDL_CreateProperties NativeGs_SDL_CreateProperties
#define SDL_CreateWindowWithProperties NativeGs_SDL_CreateWindowWithProperties
#define SDL_DelayPrecise NativeGs_SDL_DelayPrecise
#define SDL_DestroyProperties NativeGs_SDL_DestroyProperties
#define SDL_GL_CreateContext NativeGs_SDL_GL_CreateContext
#define SDL_GL_ExtensionSupported NativeGs_SDL_GL_ExtensionSupported
#define SDL_GL_GetProcAddress NativeGs_SDL_GL_GetProcAddress
#define SDL_GL_MakeCurrent NativeGs_SDL_GL_MakeCurrent
#define SDL_GL_SetAttribute NativeGs_SDL_GL_SetAttribute
#define SDL_GL_SetSwapInterval NativeGs_SDL_GL_SetSwapInterval
#define SDL_GL_SwapWindow NativeGs_SDL_GL_SwapWindow
#define SDL_GetDisplayBounds NativeGs_SDL_GetDisplayBounds
#define SDL_GetDisplayName NativeGs_SDL_GetDisplayName
#define SDL_GetDisplays NativeGs_SDL_GetDisplays
#define SDL_GetError NativeGs_SDL_GetError
#define SDL_GetWindowSize NativeGs_SDL_GetWindowSize
#define SDL_GetWindowSizeInPixels NativeGs_SDL_GetWindowSizeInPixels
#define SDL_Init NativeGs_SDL_Init
#define SDL_PollEvent NativeGs_SDL_PollEvent
#define SDL_SetBooleanProperty NativeGs_SDL_SetBooleanProperty
#define SDL_SetNumberProperty NativeGs_SDL_SetNumberProperty
#define SDL_SetStringProperty NativeGs_SDL_SetStringProperty
#define SDL_SetWindowFullscreen NativeGs_SDL_SetWindowFullscreen
#define SDL_SetWindowSize NativeGs_SDL_SetWindowSize
#define SDL_SetWindowTitle NativeGs_SDL_SetWindowTitle
#define SDL_free NativeGs_SDL_free
#define SDL_malloc NativeGs_SDL_malloc

typedef struct SDL_Window SDL_Window;
typedef void *SDL_GLContext;
typedef struct SDL_GPUDevice SDL_GPUDevice;
typedef struct SDL_GPUCommandBuffer SDL_GPUCommandBuffer;
typedef struct SDL_GPUTexture SDL_GPUTexture;
typedef uint32_t SDL_DisplayID;
typedef uint32_t SDL_PropertiesID;
typedef struct SDL_Rect { int x, y, w, h; } SDL_Rect;

typedef struct SDL_KeyboardEvent { uint32_t type; uint32_t key; bool repeat; bool down; } SDL_KeyboardEvent;
typedef union SDL_Event { uint32_t type; SDL_KeyboardEvent key; uint8_t pad[128]; } SDL_Event;
#define SDL_EVENT_QUIT 0x100u
#define SDL_EVENT_KEY_DOWN 0x300u
#define SDLK_ESCAPE 0x1Bu
#define SDLK_F1 0x4000003Au
#define SDLK_F9 0x40000042u
#define SDLK_F11 0x40000044u

typedef enum SDL_GLAttr {
    SDL_GL_CONTEXT_MAJOR_VERSION, SDL_GL_CONTEXT_MINOR_VERSION, SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_FLAGS
} SDL_GLAttr;
#define SDL_GL_CONTEXT_PROFILE_CORE 1
#define SDL_GL_CONTEXT_DEBUG_FLAG 1

#define SDL_INIT_VIDEO 0x20u
#define SDL_PROP_WINDOW_CREATE_TITLE_STRING "title"
#define SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER "width"
#define SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER "height"
#define SDL_PROP_WINDOW_CREATE_X_NUMBER "x"
#define SDL_PROP_WINDOW_CREATE_Y_NUMBER "y"
#define SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN "resizable"
#define SDL_PROP_WINDOW_CREATE_OPENGL_BOOLEAN "opengl"
#define SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN "fullscreen"
#define SDL_WINDOWPOS_CENTERED_DISPLAY(x) (0x2FFF0000u | (x))

bool SDL_Init(uint32_t flags);
const char *SDL_GetError(void);
SDL_DisplayID *SDL_GetDisplays(int *count);
bool SDL_GetDisplayBounds(SDL_DisplayID id, SDL_Rect *r);
const char *SDL_GetDisplayName(SDL_DisplayID id);
SDL_PropertiesID SDL_CreateProperties(void);
void SDL_DestroyProperties(SDL_PropertiesID p);
bool SDL_SetStringProperty(SDL_PropertiesID p, const char *name, const char *v);
bool SDL_SetNumberProperty(SDL_PropertiesID p, const char *name, int64_t v);
bool SDL_SetBooleanProperty(SDL_PropertiesID p, const char *name, bool v);
SDL_Window *SDL_CreateWindowWithProperties(SDL_PropertiesID p);
bool SDL_SetWindowSize(SDL_Window *w, int x, int y);
bool SDL_SetWindowTitle(SDL_Window *w, const char *title);
bool SDL_GetWindowSize(SDL_Window *w, int *x, int *y);
bool SDL_GetWindowSizeInPixels(SDL_Window *w, int *x, int *y);
bool SDL_SetWindowFullscreen(SDL_Window *w, bool on);
void SDL_free(void *p);
void *SDL_malloc(size_t n);
void SDL_DelayPrecise(uint64_t ns);
bool SDL_PollEvent(SDL_Event *ev);

bool SDL_GL_SetAttribute(SDL_GLAttr a, int v);
SDL_GLContext SDL_GL_CreateContext(SDL_Window *w);
bool SDL_GL_MakeCurrent(SDL_Window *w, SDL_GLContext c);
bool SDL_GL_SetSwapInterval(int i);
void *SDL_GL_GetProcAddress(const char *name);
bool SDL_GL_ExtensionSupported(const char *ext);
bool SDL_GL_SwapWindow(SDL_Window *w);

#ifdef __cplusplus
}
#endif
#endif
