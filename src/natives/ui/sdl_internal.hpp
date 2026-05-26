#pragma once
//
// sdl_internal.hpp
//
// Cross-native handle shared between `src/natives/sdl.cpp` (which owns
// the lifetime) and `src/natives/ui.cpp` (which needs to reach the
// `SDL_Window*` + `SDL_Renderer*` behind a script-facing Window value
// to drive ImGui).
//
// Only included when both ZYM_SDL_ENABLED and ZYM_UI_ENABLED are
// defined — the only consumer right now is `ui.cpp`. Keep the header
// minimal; nothing here is part of any public surface.

#if defined(ZYM_SDL_ENABLED)

#include "zym/zym.h"

#include <SDL3/SDL.h>

struct WindowHandle {
    SDL_Window*   window      = nullptr;
    SDL_Renderer* renderer    = nullptr;
    bool          shouldClose = false;
    // ImGui per-window context, lazily created by ui.cpp on first
    // `ui.frame(win, ...)` targeting this window. nullptr when the
    // window has never been used as an ImGui target. Owned by ui.cpp.
    // Must be torn down (via `g_sdl_uiContextDestructor` if set)
    // BEFORE SDL_DestroyRenderer / SDL_DestroyWindow run, because
    // ImGui's SDL3 backend holds pointers into both.
    void*         imguiContext = nullptr;
    // ImPlot per-window context, created lazily alongside the ImGui
    // context the first time `ui.frame(win, ...)` runs (see ui.cpp
    // `ensureWindowContext`). Owned by ui.cpp; torn down by
    // `destroyUiContext` BEFORE the ImGui context goes away, since
    // ImPlot keeps a pointer to the owning ImGui context internally.
    void*         implotContext = nullptr;
};

// Optional hook so `ui.cpp` can register a destructor for the ImGui
// context held in a `WindowHandle`. `sdl.cpp` calls this before
// tearing down the SDL_Renderer/SDL_Window if the pointer is set.
// Defined as a single non-inline `WindowHandle*`-taking callback to
// avoid hard-coupling sdl.cpp against ImGui headers.
using SdlUiContextDestructor = void (*)(WindowHandle*);
extern SdlUiContextDestructor g_sdl_uiContextDestructor;

// Optional hook so `ui.cpp` can be told about every SDL_Event the
// script pulls via `sdl.pollEvent` / `sdl.waitEvent` BEFORE we marshal
// it to Zym. ImGui's SDL3 backend needs to see every event for input
// routing (focus, hover, keyboard, mouse) to work; rather than forcing
// scripts to manually forward events, sdl.cpp invokes this hook when
// it's set. ui.cpp installs it at module-creation time.
using SdlUiEventForwarder = void (*)(const SDL_Event*);
extern SdlUiEventForwarder g_sdl_uiEventForwarder;

// Fetch the `WindowHandle*` behind a script-facing Window value.
// Window values are maps with a `__win__` slot whose native context
// userdata is the `WindowHandle*`. Returns nullptr if `v` doesn't
// match the expected shape — callers must check.
inline WindowHandle* sdlGetWindowHandle(ZymVM* vm, ZymValue v) {
    if (!zym_isMap(v)) return nullptr;
    if (!zym_mapHas(v, "__win__")) return nullptr;
    ZymValue ctx = zym_mapGet(vm, v, "__win__");
    return static_cast<WindowHandle*>(zym_getNativeData(ctx));
}

// Texture handle shape — declared here (not just inside sdl.cpp) so
// `ui.cpp` can pull the `SDL_Texture*` + cached size out of a
// script-facing Texture value for `UI.image` / DrawList image methods.
// Definition must stay in lockstep with the one used by sdl.cpp's
// texture factories; the layout is part of the cross-TU contract.
struct TextureHandle {
    SDL_Texture*  texture       = nullptr;
    WindowHandle* owner         = nullptr;
    int           w             = 0;
    int           h             = 0;
    // Opaque `SurfaceHandle*` — declared as `void*` here because the
    // SurfaceHandle struct lives inside sdl.cpp's anonymous namespace
    // and isn't part of the cross-TU contract. sdl.cpp casts back to
    // its own SurfaceHandle*; ui.cpp never dereferences this pointer.
    void*         linkedSurface = nullptr;
    ZymValue      linkedValue;
};

// Fetch the `TextureHandle*` behind a script-facing Texture value.
// Texture values are maps with a `__tex__` slot whose native context
// userdata is the `TextureHandle*`. Returns nullptr if `v` doesn't
// match the expected shape — callers must check.
inline TextureHandle* sdlGetTextureHandle(ZymVM* vm, ZymValue v) {
    if (!zym_isMap(v)) return nullptr;
    if (!zym_mapHas(v, "__tex__")) return nullptr;
    ZymValue ctx = zym_mapGet(vm, v, "__tex__");
    return static_cast<TextureHandle*>(zym_getNativeData(ctx));
}

#endif // ZYM_SDL_ENABLED
