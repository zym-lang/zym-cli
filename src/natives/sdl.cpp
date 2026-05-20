// SDL3 native — Slice 1 (window + event pump only).
//
// Scope per future/gui.md §1: open a window, pump events, query input
// state. No drawing, no textures, no audio, no clipboard, no dialogs —
// those are explicit future-slice scope.
//
// Compiled only when ZYM_SDL_ENABLED is defined (see CMakeLists.txt:
// `ZYM_SDL` flag). When disabled, this TU is excluded from the build
// and `cli_catalog.cpp` omits the `sdl` row.

#include "natives.hpp"
#include "sdl_internal.hpp"  // shared `WindowHandle` + `sdlGetWindowHandle`

#include <SDL3/SDL.h>
#if defined(ZYM_SDL_IMAGE_ENABLED)
#  include <SDL3_image/SDL_image.h>
#endif

#include <algorithm>
#include <cstring>
#include <string>

// ---- module-level state -------------------------------------------------
//
// SDL3 is process-wide. We init lazily on first use (sdl.init or
// sdl.createWindow) and never SDL_Quit during a VM lifetime — shutdown
// happens at process exit. The refcount lets nested VMs share the
// subsystem without re-initialising.

namespace {

int g_sdl_initRefs = 0;
SDL_InitFlags g_sdl_initFlags = 0;
} // namespace

// File-scope storage for the optional ImGui hooks declared `extern`
// in sdl_internal.hpp. ui.cpp assigns these at module-creation time
// when ZYM_UI is built; null otherwise.
SdlUiContextDestructor g_sdl_uiContextDestructor = nullptr;
SdlUiEventForwarder    g_sdl_uiEventForwarder    = nullptr;

namespace {

bool ensureInit(SDL_InitFlags wanted) {
    SDL_InitFlags need = wanted & ~g_sdl_initFlags;
    if (need == 0) { g_sdl_initRefs++; return true; }
    if (!SDL_InitSubSystem(need)) return false;
    g_sdl_initFlags |= need;
    g_sdl_initRefs++;
    return true;
}

// ---- handle types ---------------------------------------------------------
//
// `WindowHandle` lives in sdl_internal.hpp so ui.cpp (PR 2) can reach
// the SDL_Window + SDL_Renderer behind a script-facing Window value
// to drive ImGui. sdl.cpp owns the lifetime; ui.cpp owns the
// `imguiContext` field.

void windowFinalizer(ZymVM*, void* data) {
    auto* w = static_cast<WindowHandle*>(data);
    if (!w) return;
    // ImGui context (if any) must be torn down BEFORE the renderer
    // and window — the SDL3 ImGui backend keeps pointers into both.
    if (w->imguiContext && g_sdl_uiContextDestructor) {
        g_sdl_uiContextDestructor(w);
        w->imguiContext = nullptr;
    }
    if (w->renderer) SDL_DestroyRenderer(w->renderer);
    if (w->window)   SDL_DestroyWindow(w->window);
    delete w;
}

WindowHandle* unwrapWindow(ZymValue ctx) {
    return static_cast<WindowHandle*>(zym_getNativeData(ctx));
}

bool reqWindow(ZymVM* vm, ZymValue ctx, const char* where, WindowHandle** out) {
    auto* w = unwrapWindow(ctx);
    if (!w || !w->window) {
        zym_runtimeError(vm, "%s: invalid Window handle", where);
        return false;
    }
    *out = w;
    return true;
}

// ---- arg helpers ----------------------------------------------------------

bool reqStr(ZymVM* vm, ZymValue v, const char* where, std::string* out) {
    if (!zym_isString(v)) { zym_runtimeError(vm, "%s expects a string", where); return false; }
    *out = zym_asCString(v);
    return true;
}

bool reqNum(ZymVM* vm, ZymValue v, const char* where, double* out) {
    if (!zym_isNumber(v)) { zym_runtimeError(vm, "%s expects a number", where); return false; }
    *out = zym_asNumber(v);
    return true;
}

bool reqBool(ZymVM* vm, ZymValue v, const char* where, bool* out) {
    if (!zym_isBool(v)) { zym_runtimeError(vm, "%s expects a bool", where); return false; }
    *out = zym_asBool(v);
    return true;
}

ZymValue strToZym(ZymVM* vm, const char* s) {
    if (!s) s = "";
    return zym_newStringN(vm, s, (int)std::strlen(s));
}

// Optional bool field on a map; returns `dflt` if missing or not a bool.
bool mapBool(ZymVM* vm, ZymValue map, const char* key, bool dflt) {
    if (!zym_isMap(map)) return dflt;
    if (!zym_mapHas(map, key)) return dflt;
    ZymValue v = zym_mapGet(vm, map, key);
    return zym_isBool(v) ? zym_asBool(v) : dflt;
}

bool mapNum(ZymVM* vm, ZymValue map, const char* key, double* out) {
    if (!zym_isMap(map)) return false;
    if (!zym_mapHas(map, key)) return false;
    ZymValue v = zym_mapGet(vm, map, key);
    if (!zym_isNumber(v)) return false;
    *out = zym_asNumber(v);
    return true;
}

// ---- module-level functions ----------------------------------------------

ZymValue s_init(ZymVM* vm, ZymValue /*self*/) {
    // Slice 1: video + events. SDL3 implicitly enables `events` with
    // `video`, but we ask explicitly for clarity.
    return zym_newBool(ensureInit(SDL_INIT_VIDEO));
}

ZymValue s_quit(ZymVM*, ZymValue /*self*/) {
    // Per locked decision (§1.0 lifetime): nested-VM `sdl.quit()` is a
    // no-op; real teardown happens at process exit. We refcount and
    // never actually call SDL_Quit so other VMs / late-finalised
    // windows can't tear out the subsystem under us.
    if (g_sdl_initRefs > 0) g_sdl_initRefs--;
    return zym_newNull();
}

ZymValue s_lastError(ZymVM* vm, ZymValue /*self*/) {
    const char* msg = SDL_GetError();
    return strToZym(vm, msg ? msg : "");
}

ZymValue s_version(ZymVM* vm, ZymValue /*self*/) {
    int v = SDL_GetVersion();
    ZymValue m = zym_newMap(vm);
    zym_pushRoot(vm, m);
    zym_mapSet(vm, m, "major", zym_newNumber(SDL_VERSIONNUM_MAJOR(v)));
    zym_mapSet(vm, m, "minor", zym_newNumber(SDL_VERSIONNUM_MINOR(v)));
    zym_mapSet(vm, m, "patch", zym_newNumber(SDL_VERSIONNUM_MICRO(v)));
    zym_popRoot(vm);
    return m;
}

// ---- Window instance methods (declared up here so makeWindowInstance can wire them) ----

ZymValue w_shouldClose(ZymVM*, ZymValue ctx) {
    auto* w = unwrapWindow(ctx);
    return zym_newBool(!w || !w->window || w->shouldClose);
}

ZymValue w_setTitle(ZymVM* vm, ZymValue ctx, ZymValue tv) {
    WindowHandle* w; if (!reqWindow(vm, ctx, "Window.setTitle(s)", &w)) return ZYM_ERROR;
    std::string s; if (!reqStr(vm, tv, "Window.setTitle(s)", &s)) return ZYM_ERROR;
    SDL_SetWindowTitle(w->window, s.c_str());
    return zym_newNull();
}

ZymValue w_getTitle(ZymVM* vm, ZymValue ctx) {
    WindowHandle* w; if (!reqWindow(vm, ctx, "Window.getTitle()", &w)) return ZYM_ERROR;
    return strToZym(vm, SDL_GetWindowTitle(w->window));
}

ZymValue w_setSize(ZymVM* vm, ZymValue ctx, ZymValue wv, ZymValue hv) {
    WindowHandle* w; if (!reqWindow(vm, ctx, "Window.setSize(w, h)", &w)) return ZYM_ERROR;
    double dw, dh;
    if (!reqNum(vm, wv, "Window.setSize(w, h)", &dw)) return ZYM_ERROR;
    if (!reqNum(vm, hv, "Window.setSize(w, h)", &dh)) return ZYM_ERROR;
    SDL_SetWindowSize(w->window, (int)dw, (int)dh);
    return zym_newNull();
}

ZymValue w_getSize(ZymVM* vm, ZymValue ctx) {
    WindowHandle* w; if (!reqWindow(vm, ctx, "Window.getSize()", &w)) return ZYM_ERROR;
    int ww = 0, hh = 0; SDL_GetWindowSize(w->window, &ww, &hh);
    ZymValue m = zym_newMap(vm); zym_pushRoot(vm, m);
    zym_mapSet(vm, m, "w", zym_newNumber(ww));
    zym_mapSet(vm, m, "h", zym_newNumber(hh));
    zym_popRoot(vm);
    return m;
}

ZymValue w_setPosition(ZymVM* vm, ZymValue ctx, ZymValue xv, ZymValue yv) {
    WindowHandle* w; if (!reqWindow(vm, ctx, "Window.setPosition(x, y)", &w)) return ZYM_ERROR;
    double x, y;
    if (!reqNum(vm, xv, "Window.setPosition(x, y)", &x)) return ZYM_ERROR;
    if (!reqNum(vm, yv, "Window.setPosition(x, y)", &y)) return ZYM_ERROR;
    SDL_SetWindowPosition(w->window, (int)x, (int)y);
    return zym_newNull();
}

ZymValue w_getPosition(ZymVM* vm, ZymValue ctx) {
    WindowHandle* w; if (!reqWindow(vm, ctx, "Window.getPosition()", &w)) return ZYM_ERROR;
    int x = 0, y = 0; SDL_GetWindowPosition(w->window, &x, &y);
    ZymValue m = zym_newMap(vm); zym_pushRoot(vm, m);
    zym_mapSet(vm, m, "x", zym_newNumber(x));
    zym_mapSet(vm, m, "y", zym_newNumber(y));
    zym_popRoot(vm);
    return m;
}

ZymValue w_show(ZymVM* vm, ZymValue ctx)    { WindowHandle* w; if (!reqWindow(vm, ctx, "Window.show()",     &w)) return ZYM_ERROR; SDL_ShowWindow(w->window);     return zym_newNull(); }
ZymValue w_hide(ZymVM* vm, ZymValue ctx)    { WindowHandle* w; if (!reqWindow(vm, ctx, "Window.hide()",     &w)) return ZYM_ERROR; SDL_HideWindow(w->window);     return zym_newNull(); }
ZymValue w_minimize(ZymVM* vm, ZymValue ctx){ WindowHandle* w; if (!reqWindow(vm, ctx, "Window.minimize()", &w)) return ZYM_ERROR; SDL_MinimizeWindow(w->window); return zym_newNull(); }
ZymValue w_maximize(ZymVM* vm, ZymValue ctx){ WindowHandle* w; if (!reqWindow(vm, ctx, "Window.maximize()", &w)) return ZYM_ERROR; SDL_MaximizeWindow(w->window); return zym_newNull(); }
ZymValue w_restore(ZymVM* vm, ZymValue ctx) { WindowHandle* w; if (!reqWindow(vm, ctx, "Window.restore()",  &w)) return ZYM_ERROR; SDL_RestoreWindow(w->window);  return zym_newNull(); }

ZymValue w_setFullscreen(ZymVM* vm, ZymValue ctx, ZymValue bv) {
    WindowHandle* w; if (!reqWindow(vm, ctx, "Window.setFullscreen(b)", &w)) return ZYM_ERROR;
    bool b; if (!reqBool(vm, bv, "Window.setFullscreen(b)", &b)) return ZYM_ERROR;
    return zym_newBool(SDL_SetWindowFullscreen(w->window, b));
}

ZymValue w_setVSync(ZymVM* vm, ZymValue ctx, ZymValue bv) {
    WindowHandle* w; if (!reqWindow(vm, ctx, "Window.setVSync(b)", &w)) return ZYM_ERROR;
    bool b; if (!reqBool(vm, bv, "Window.setVSync(b)", &b)) return ZYM_ERROR;
    if (!w->renderer) return zym_newBool(false);
    return zym_newBool(SDL_SetRenderVSync(w->renderer, b ? 1 : 0));
}

ZymValue w_free(ZymVM* vm, ZymValue ctx) {
    auto* w = unwrapWindow(ctx);
    if (!w) return zym_newNull();
    // Same teardown order as `windowFinalizer`: ImGui context (if any)
    // first, then renderer, then window.
    if (w->imguiContext && g_sdl_uiContextDestructor) {
        g_sdl_uiContextDestructor(w);
        w->imguiContext = nullptr;
    }
    if (w->renderer) { SDL_DestroyRenderer(w->renderer); w->renderer = nullptr; }
    if (w->window)   { SDL_DestroyWindow(w->window);     w->window   = nullptr; }
    (void)vm;
    return zym_newNull();
}

// Forward decls for Texture factories (defined later in this file,
// after SurfaceHandle / parseRect / etc. are available).
ZymValue w_createTexture(ZymVM* vm, ZymValue ctx, ZymValue wv, ZymValue hv, ZymValue ov);
ZymValue w_textureFromSurface(ZymVM* vm, ZymValue ctx, ZymValue sv, ZymValue ov);

ZymValue makeWindowInstance(ZymVM* vm, WindowHandle* w) {
    ZymValue ctx = zym_createNativeContext(vm, w, windowFinalizer);
    zym_pushRoot(vm, ctx);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__win__", ctx);

#define M(name, sig, fn) do { \
    ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctx); \
    zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
} while (0)

    M("shouldClose",   "shouldClose()",   w_shouldClose);
    M("setTitle",      "setTitle(s)",     w_setTitle);
    M("getTitle",      "getTitle()",      w_getTitle);
    M("setSize",       "setSize(w, h)",   w_setSize);
    M("getSize",       "getSize()",       w_getSize);
    M("setPosition",   "setPosition(x, y)", w_setPosition);
    M("getPosition",   "getPosition()",   w_getPosition);
    M("show",          "show()",          w_show);
    M("hide",          "hide()",          w_hide);
    M("minimize",      "minimize()",      w_minimize);
    M("maximize",      "maximize()",      w_maximize);
    M("restore",       "restore()",       w_restore);
    M("setFullscreen", "setFullscreen(b)", w_setFullscreen);
    M("setVSync",      "setVSync(b)",     w_setVSync);
    // Texture factories (Slice 2 PR 6). Renderer ownership is visible
    // at the call site: textures are bound to the Window's renderer
    // and can't be shared across windows.
    M("createTexture",      "createTexture(w, h, opts)",       w_createTexture);
    M("textureFromSurface", "textureFromSurface(surface, opts)", w_textureFromSurface);
    M("free",          "free()",          w_free);

#undef M

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}

ZymValue s_createWindow(ZymVM* vm, ZymValue /*self*/, ZymValue tv, ZymValue wv, ZymValue hv, ZymValue ov) {
    std::string title; double dw, dh;
    if (!reqStr(vm, tv, "sdl.createWindow(title, w, h, opts?)", &title)) return ZYM_ERROR;
    if (!reqNum(vm, wv, "sdl.createWindow(title, w, h, opts?)", &dw))    return ZYM_ERROR;
    if (!reqNum(vm, hv, "sdl.createWindow(title, w, h, opts?)", &dh))    return ZYM_ERROR;

    if (!ensureInit(SDL_INIT_VIDEO)) return zym_newNull();

    SDL_WindowFlags flags = 0;
    bool wantPos = false;
    int posX = SDL_WINDOWPOS_CENTERED, posY = SDL_WINDOWPOS_CENTERED;
    if (zym_isMap(ov)) {
        if (mapBool(vm, ov, "resizable",   false)) flags |= SDL_WINDOW_RESIZABLE;
        if (mapBool(vm, ov, "borderless",  false)) flags |= SDL_WINDOW_BORDERLESS;
        if (mapBool(vm, ov, "hidden",      false)) flags |= SDL_WINDOW_HIDDEN;
        if (mapBool(vm, ov, "alwaysOnTop", false)) flags |= SDL_WINDOW_ALWAYS_ON_TOP;
        if (mapBool(vm, ov, "hidpi",       false)) flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
        double dx, dy;
        if (mapNum(vm, ov, "posX", &dx)) { posX = (int)dx; wantPos = true; }
        if (mapNum(vm, ov, "posY", &dy)) { posY = (int)dy; wantPos = true; }
    }

    SDL_Window* win = SDL_CreateWindow(title.c_str(), (int)dw, (int)dh, flags);
    if (!win) return zym_newNull();
    if (wantPos) SDL_SetWindowPosition(win, posX, posY);

    // Create the default renderer up front. ImGui's
    // imgui_impl_sdlrenderer3 backend (Slice 1, PR 2) renders into
    // this. Passing `nullptr` as the driver name lets SDL pick the
    // best available (Vulkan/Metal/D3D12/GL).
    SDL_Renderer* ren = SDL_CreateRenderer(win, nullptr);
    if (!ren) {
        SDL_DestroyWindow(win);
        return zym_newNull();
    }

    auto* h = new WindowHandle();
    h->window   = win;
    h->renderer = ren;
    // Stash the WindowHandle* in the SDL window's properties so the
    // event pump can locate it from a windowID and stamp the sticky
    // close flag (see `maybeStampClose`).
    if (auto props = SDL_GetWindowProperties(win)) {
        SDL_SetPointerProperty(props, "zym.handle", h);
    }
    return makeWindowInstance(vm, h);
}

// ---- events --------------------------------------------------------------
//
// Event marshalling: SDL_Event -> Zym map with `type` (dot-namespaced
// string) and per-type payload sub-maps.

ZymValue eventToZym(ZymVM* vm, const SDL_Event& e) {
    ZymValue m = zym_newMap(vm);
    zym_pushRoot(vm, m);

    // IMPORTANT: heap-allocated values (strings, lists, maps) passed to
    // zym_mapSet must be GC-rooted across the insert. zym_mapSet may
    // resize the underlying hash table, which can trigger GC, and an
    // un-rooted just-allocated value on the C stack is invisible to
    // the collector. See process.cpp's `p_getEnvAll` for the canonical
    // pattern (push/mapSet/pop).
    auto setStr = [&](const char* k, const char* v) {
        ZymValue s = strToZym(vm, v ? v : "");
        zym_pushRoot(vm, s);
        zym_mapSet(vm, m, k, s);
        zym_popRoot(vm);
    };
    auto setNum = [&](const char* k, double v) {
        // Numbers are inline / non-heap; safe without rooting.
        zym_mapSet(vm, m, k, zym_newNumber(v));
    };
    auto setBool = [&](const char* k, bool v) {
        // Bools are inline / non-heap; safe without rooting.
        zym_mapSet(vm, m, k, zym_newBool(v));
    };

    switch (e.type) {
        case SDL_EVENT_QUIT:
            setStr("type", "quit");
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            setStr("type", e.type == SDL_EVENT_KEY_DOWN ? "key.down" : "key.up");
            setNum("scancode", (double)e.key.scancode);
            setNum("key",      (double)e.key.key);
            setNum("mod",      (double)e.key.mod);
            setBool("repeat",  e.key.repeat);
            setNum("window",   (double)e.key.windowID);
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            setStr("type", e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? "mouse.down" : "mouse.up");
            setNum("button", (double)e.button.button);
            setNum("x",      (double)e.button.x);
            setNum("y",      (double)e.button.y);
            setNum("clicks", (double)e.button.clicks);
            setNum("window", (double)e.button.windowID);
            break;
        }
        case SDL_EVENT_MOUSE_MOTION: {
            setStr("type", "mouse.motion");
            setNum("x",     (double)e.motion.x);
            setNum("y",     (double)e.motion.y);
            setNum("dx",    (double)e.motion.xrel);
            setNum("dy",    (double)e.motion.yrel);
            setNum("window",(double)e.motion.windowID);
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            setStr("type", "mouse.wheel");
            setNum("x",      (double)e.wheel.x);
            setNum("y",      (double)e.wheel.y);
            setNum("window", (double)e.wheel.windowID);
            break;
        }
        case SDL_EVENT_TEXT_INPUT: {
            setStr("type", "text.input");
            setStr("text", e.text.text);
            setNum("window", (double)e.text.windowID);
            break;
        }
        case SDL_EVENT_WINDOW_RESIZED: {
            setStr("type", "window.resize");
            setNum("w",      (double)e.window.data1);
            setNum("h",      (double)e.window.data2);
            setNum("window", (double)e.window.windowID);
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            setStr("type", "window.focus");
            setNum("window", (double)e.window.windowID);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            setStr("type", "window.blur");
            setNum("window", (double)e.window.windowID);
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            setStr("type", "window.close");
            setNum("window", (double)e.window.windowID);
            break;
        default:
            setStr("type", "other");
            setNum("raw", (double)e.type);
            break;
    }

    zym_popRoot(vm);
    return m;
}

// Stamp the close flag on any window we own that matches this event's
// windowID. Lets `Window.shouldClose()` work without the script having
// to react to the event explicitly (the locked "both patterns" rule).
void maybeStampClose(const SDL_Event& e) {
    if (e.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED) return;
    SDL_Window* w = SDL_GetWindowFromID(e.window.windowID);
    if (!w) return;
    // We stash WindowHandle* in window's user data via SDL_SetPointerProperty?
    // Simpler: walk SDL's known windows isn't exposed. Instead, store
    // WindowHandle* in a Properties slot keyed on the window itself.
    auto props = SDL_GetWindowProperties(w);
    if (!props) return;
    void* p = SDL_GetPointerProperty(props, "zym.handle", nullptr);
    if (p) static_cast<WindowHandle*>(p)->shouldClose = true;
}

ZymValue s_pollEvent(ZymVM* vm, ZymValue /*self*/) {
    SDL_Event e;
    if (!SDL_PollEvent(&e)) return zym_newNull();
    maybeStampClose(e);
    // Forward to ImGui (when ui native is loaded) so its SDL3 backend
    // can route input. Hook is null when ZYM_UI is off or `ui` hasn't
    // been imported yet.
    if (g_sdl_uiEventForwarder) g_sdl_uiEventForwarder(&e);
    return eventToZym(vm, e);
}

ZymValue s_waitEvent(ZymVM* vm, ZymValue /*self*/, ZymValue tv) {
    SDL_Event e;
    bool ok;
    if (zym_isNull(tv)) {
        ok = SDL_WaitEvent(&e);
    } else {
        double ms;
        if (!reqNum(vm, tv, "sdl.waitEvent(timeoutMs?)", &ms)) return ZYM_ERROR;
        ok = SDL_WaitEventTimeout(&e, (int)ms);
    }
    if (!ok) return zym_newNull();
    maybeStampClose(e);
    if (g_sdl_uiEventForwarder) g_sdl_uiEventForwarder(&e);
    return eventToZym(vm, e);
}

ZymValue s_pushEvent(ZymVM* /*vm*/, ZymValue /*self*/, ZymValue /*ev*/) {
    // Minimal stub for user-defined events. Real payload-mapping back
    // to SDL_Event is deferred; for now we push a no-op USEREVENT so
    // a waiting `pollEvent` wakes.
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_USER;
    return zym_newBool(SDL_PushEvent(&e));
}

// ---- input state queries --------------------------------------------------

ZymValue s_keyDown(ZymVM* vm, ZymValue /*self*/, ZymValue kv) {
    double k;
    if (!reqNum(vm, kv, "sdl.keyDown(scancode)", &k)) return ZYM_ERROR;
    int numKeys = 0;
    const bool* state = SDL_GetKeyboardState(&numKeys);
    int idx = (int)k;
    if (!state || idx < 0 || idx >= numKeys) return zym_newBool(false);
    return zym_newBool(state[idx]);
}

ZymValue s_mousePos(ZymVM* vm, ZymValue /*self*/) {
    float x = 0, y = 0;
    SDL_GetMouseState(&x, &y);
    ZymValue m = zym_newMap(vm); zym_pushRoot(vm, m);
    zym_mapSet(vm, m, "x", zym_newNumber(x));
    zym_mapSet(vm, m, "y", zym_newNumber(y));
    zym_popRoot(vm);
    return m;
}

ZymValue s_mouseButtons(ZymVM* vm, ZymValue /*self*/) {
    float x, y;
    SDL_MouseButtonFlags b = SDL_GetMouseState(&x, &y);
    ZymValue m = zym_newMap(vm); zym_pushRoot(vm, m);
    zym_mapSet(vm, m, "left",   zym_newBool((b & SDL_BUTTON_LMASK) != 0));
    zym_mapSet(vm, m, "right",  zym_newBool((b & SDL_BUTTON_RMASK) != 0));
    zym_mapSet(vm, m, "middle", zym_newBool((b & SDL_BUTTON_MMASK) != 0));
    zym_popRoot(vm);
    return m;
}

ZymValue s_modifiers(ZymVM* vm, ZymValue /*self*/) {
    SDL_Keymod k = SDL_GetModState();
    ZymValue m = zym_newMap(vm); zym_pushRoot(vm, m);
    zym_mapSet(vm, m, "shift", zym_newBool((k & SDL_KMOD_SHIFT) != 0));
    zym_mapSet(vm, m, "ctrl",  zym_newBool((k & SDL_KMOD_CTRL)  != 0));
    zym_mapSet(vm, m, "alt",   zym_newBool((k & SDL_KMOD_ALT)   != 0));
    zym_mapSet(vm, m, "super", zym_newBool((k & SDL_KMOD_GUI)   != 0));
    zym_popRoot(vm);
    return m;
}

// ---- Surface (CPU-side image, mutable) -----------------------------------
//
// Compiled in unconditionally when ZYM_SDL_ENABLED is on. The decode
// path (loadImage / saveImage / saveImageToBuffer for PNG/JPG) routes
// through SDL_image which is built when ZYM_SDL_IMAGE_ENABLED is on
// (currently the default whenever ZYM_SDL is on). BMP routes through
// SDL3 core's built-in SDL_LoadBMP / SDL_SaveBMP, which is always
// available.

struct SurfaceHandle {
    SDL_Surface* surface = nullptr;
};

void surfaceFinalizer(ZymVM*, void* data) {
    auto* s = static_cast<SurfaceHandle*>(data);
    if (!s) return;
    if (s->surface) SDL_DestroySurface(s->surface);
    delete s;
}

SurfaceHandle* unwrapSurface(ZymValue ctx) {
    return static_cast<SurfaceHandle*>(zym_getNativeData(ctx));
}

bool reqSurface(ZymVM* vm, ZymValue ctx, const char* where, SurfaceHandle** out) {
    auto* s = unwrapSurface(ctx);
    if (!s || !s->surface) {
        zym_runtimeError(vm, "%s: invalid Surface handle", where);
        return false;
    }
    *out = s;
    return true;
}

// Extract a SurfaceHandle* from a Zym `Surface` value (map with __surface__).
// Returns nullptr on shape mismatch — callers must check.
SurfaceHandle* surfaceFromValue(ZymVM* vm, ZymValue v) {
    if (!zym_isMap(v)) return nullptr;
    if (!zym_mapHas(v, "__surface__")) return nullptr;
    ZymValue ctx = zym_mapGet(vm, v, "__surface__");
    return unwrapSurface(ctx);
}

// Parse a color argument. Accepts either:
//   - a 3/4-element list of numbers (floats 0..1 or ints 0..255 — we
//     auto-detect by looking for any value > 1.0), or
//   - a single packed number (0xAARRGGBB-shaped).
// Returns SDL_Color (RGBA in [0..255]).
bool parseColor(ZymVM* vm, ZymValue v, const char* where, SDL_Color* out) {
    if (zym_isList(v)) {
        int n = zym_listLength(v);
        if (n != 3 && n != 4) {
            zym_runtimeError(vm, "%s: color list must have 3 or 4 elements", where);
            return false;
        }
        double comps[4] = {0,0,0,1};
        bool anyAboveOne = false;
        for (int i = 0; i < n; i++) {
            ZymValue e = zym_listGet(vm, v, i);
            if (!zym_isNumber(e)) {
                zym_runtimeError(vm, "%s: color component %d not a number", where, i);
                return false;
            }
            comps[i] = zym_asNumber(e);
            if (comps[i] > 1.0) anyAboveOne = true;
        }
        auto clamp01 = [](double d){ return d < 0 ? 0.0 : (d > 1.0 ? 1.0 : d); };
        auto clamp255 = [](double d){ return d < 0 ? 0.0 : (d > 255.0 ? 255.0 : d); };
        if (anyAboveOne) {
            out->r = (Uint8)clamp255(comps[0]);
            out->g = (Uint8)clamp255(comps[1]);
            out->b = (Uint8)clamp255(comps[2]);
            out->a = (Uint8)clamp255(comps[3]);
        } else {
            out->r = (Uint8)(clamp01(comps[0]) * 255.0 + 0.5);
            out->g = (Uint8)(clamp01(comps[1]) * 255.0 + 0.5);
            out->b = (Uint8)(clamp01(comps[2]) * 255.0 + 0.5);
            out->a = (Uint8)(clamp01(comps[3]) * 255.0 + 0.5);
        }
        if (n == 3) out->a = 255;
        return true;
    }
    if (zym_isNumber(v)) {
        uint32_t packed = (uint32_t)zym_asNumber(v);
        out->a = (Uint8)((packed >> 24) & 0xFF);
        out->r = (Uint8)((packed >> 16) & 0xFF);
        out->g = (Uint8)((packed >> 8)  & 0xFF);
        out->b = (Uint8)((packed >> 0)  & 0xFF);
        return true;
    }
    zym_runtimeError(vm, "%s: color must be a list or a packed number", where);
    return false;
}

// Parse a rect argument: a map with { x, y, w, h }. Returns true on
// success; on failure, leaves *out untouched and raises a runtime error.
bool parseRect(ZymVM* vm, ZymValue v, const char* where, SDL_Rect* out) {
    if (!zym_isMap(v)) {
        zym_runtimeError(vm, "%s: rect must be a map { x, y, w, h }", where);
        return false;
    }
    double x=0, y=0, w=0, h=0;
    if (!mapNum(vm, v, "x", &x) || !mapNum(vm, v, "y", &y) ||
        !mapNum(vm, v, "w", &w) || !mapNum(vm, v, "h", &h)) {
        zym_runtimeError(vm, "%s: rect needs numeric x, y, w, h", where);
        return false;
    }
    out->x = (int)x; out->y = (int)y; out->w = (int)w; out->h = (int)h;
    return true;
}

// Build a Zym { x, y, w, h } map from an SDL_Rect.
ZymValue rectToZym(ZymVM* vm, const SDL_Rect& r) {
    ZymValue m = zym_newMap(vm); zym_pushRoot(vm, m);
    zym_mapSet(vm, m, "x", zym_newNumber(r.x));
    zym_mapSet(vm, m, "y", zym_newNumber(r.y));
    zym_mapSet(vm, m, "w", zym_newNumber(r.w));
    zym_mapSet(vm, m, "h", zym_newNumber(r.h));
    zym_popRoot(vm);
    return m;
}

// Build a Zym [r, g, b, a] list of floats 0..1 from a packed pixel value.
ZymValue colorToZym(ZymVM* vm, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    ZymValue l = zym_newList(vm); zym_pushRoot(vm, l);
    zym_listAppend(vm, l, zym_newNumber(r / 255.0));
    zym_listAppend(vm, l, zym_newNumber(g / 255.0));
    zym_listAppend(vm, l, zym_newNumber(b / 255.0));
    zym_listAppend(vm, l, zym_newNumber(a / 255.0));
    zym_popRoot(vm);
    return l;
}

SDL_BlendMode parseBlendMode(const std::string& s) {
    if (s == "none")  return SDL_BLENDMODE_NONE;
    if (s == "blend") return SDL_BLENDMODE_BLEND;
    if (s == "add")   return SDL_BLENDMODE_ADD;
    if (s == "mod")   return SDL_BLENDMODE_MOD;
    if (s == "mul")   return SDL_BLENDMODE_MUL;
    return SDL_BLENDMODE_BLEND;
}

const char* blendModeName(SDL_BlendMode m) {
    switch (m) {
        case SDL_BLENDMODE_NONE:  return "none";
        case SDL_BLENDMODE_BLEND: return "blend";
        case SDL_BLENDMODE_ADD:   return "add";
        case SDL_BLENDMODE_MOD:   return "mod";
        case SDL_BLENDMODE_MUL:   return "mul";
        default:                  return "custom";
    }
}

// Format-name helper. SDL3 ships SDL_GetPixelFormatName which returns
// "SDL_PIXELFORMAT_RGBA32" etc; strip the prefix for the script API.
const char* shortFormatName(SDL_PixelFormat fmt) {
    const char* n = SDL_GetPixelFormatName(fmt);
    if (!n) return "UNKNOWN";
    const char* p = std::strstr(n, "SDL_PIXELFORMAT_");
    return p ? p + std::strlen("SDL_PIXELFORMAT_") : n;
}

SDL_PixelFormat parseFormatName(const std::string& name, SDL_PixelFormat dflt) {
    if (name.empty()) return dflt;
    std::string full = "SDL_PIXELFORMAT_" + name;
    // Quick lookup: probe a small set we actually support / are likely
    // to encounter. SDL3 has no string→enum helper.
    struct Row { const char* name; SDL_PixelFormat fmt; };
    static const Row rows[] = {
        { "SDL_PIXELFORMAT_RGBA32",  SDL_PIXELFORMAT_RGBA32  },
        { "SDL_PIXELFORMAT_ARGB32",  SDL_PIXELFORMAT_ARGB32  },
        { "SDL_PIXELFORMAT_BGRA32",  SDL_PIXELFORMAT_BGRA32  },
        { "SDL_PIXELFORMAT_ABGR32",  SDL_PIXELFORMAT_ABGR32  },
        { "SDL_PIXELFORMAT_RGB24",   SDL_PIXELFORMAT_RGB24   },
        { "SDL_PIXELFORMAT_BGR24",   SDL_PIXELFORMAT_BGR24   },
        { "SDL_PIXELFORMAT_RGBX32",  SDL_PIXELFORMAT_RGBX32  },
        { "SDL_PIXELFORMAT_XRGB8888",SDL_PIXELFORMAT_XRGB8888},
        { "SDL_PIXELFORMAT_INDEX8",  SDL_PIXELFORMAT_INDEX8  },
    };
    for (auto& r : rows) {
        if (full == r.name) return r.fmt;
    }
    return dflt;
}

// ---- image I/O (module-level) --------------------------------------------

// Forward: build a Surface instance map wrapping a SurfaceHandle*.
ZymValue makeSurfaceInstance(ZymVM* vm, SurfaceHandle* s);

// Decide the encoded format from an explicit format string (preferred)
// or a path extension (fallback). Returns one of "png" / "jpg" / "bmp"
// (lowercased) or "" on unrecognised input.
std::string pickFormat(const std::string& explicitFmt, const std::string& path) {
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });
        return s;
    };
    if (!explicitFmt.empty()) {
        std::string f = lower(explicitFmt);
        if (f == "jpeg") f = "jpg";
        return f;
    }
    // Fall back to extension parsing.
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string f = lower(path.substr(dot + 1));
    if (f == "jpeg") f = "jpg";
    return f;
}

SDL_Surface* loadSurfaceFromIO(SDL_IOStream* io, const std::string& fmt) {
    if (!io) return nullptr;
    if (fmt == "bmp") {
        return SDL_LoadBMP_IO(io, true);
    }
#if defined(ZYM_SDL_IMAGE_ENABLED)
    // IMG_Load_IO sniffs the magic bytes for PNG/JPG/... when type is
    // null; IMG_LoadTyped_IO forces a particular decoder. We prefer
    // the typed variant when the script gave us a hint.
    if (!fmt.empty()) {
        return IMG_LoadTyped_IO(io, true, fmt.c_str());
    }
    return IMG_Load_IO(io, true);
#else
    SDL_CloseIO(io);
    SDL_SetError("SDL_image not built (PNG/JPG decoders unavailable)");
    return nullptr;
#endif
}

bool saveSurfaceToIO(SDL_Surface* surf, SDL_IOStream* io, const std::string& fmt) {
    if (!surf || !io) return false;
    if (fmt == "bmp") {
        return SDL_SaveBMP_IO(surf, io, true);
    }
#if defined(ZYM_SDL_IMAGE_ENABLED)
    if (fmt == "png" || fmt.empty()) {
        return IMG_SavePNG_IO(surf, io, true);
    }
    if (fmt == "jpg") {
        return IMG_SaveJPG_IO(surf, io, true, 90);
    }
    SDL_CloseIO(io);
    SDL_SetError("unsupported image format: %s", fmt.c_str());
    return false;
#else
    SDL_CloseIO(io);
    SDL_SetError("SDL_image not built; only BMP can be saved");
    return false;
#endif
}

ZymValue s_loadImage(ZymVM* vm, ZymValue /*self*/, ZymValue pv) {
    std::string path;
    if (!reqStr(vm, pv, "sdl.loadImage(path)", &path)) return ZYM_ERROR;
    if (!ensureInit(SDL_INIT_VIDEO)) return zym_newNull();
    std::string fmt = pickFormat("", path);
    SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "rb");
    if (!io) return zym_newNull();
    SDL_Surface* surf = loadSurfaceFromIO(io, fmt);
    if (!surf) return zym_newNull();
    auto* h = new SurfaceHandle();
    h->surface = surf;
    return makeSurfaceInstance(vm, h);
}

ZymValue s_loadImageFromBuffer(ZymVM* vm, ZymValue /*self*/, ZymValue bv, ZymValue fv) {
    const char* data = nullptr;
    size_t size = 0;
    if (!readBufferBytes(vm, bv, &data, &size)) {
        zym_runtimeError(vm, "sdl.loadImageFromBuffer(buf, format?): first arg must be a Buffer");
        return ZYM_ERROR;
    }
    std::string fmt;
    if (!zym_isNull(fv)) {
        if (!reqStr(vm, fv, "sdl.loadImageFromBuffer(buf, format?)", &fmt)) return ZYM_ERROR;
    }
    fmt = pickFormat(fmt, "");
    if (!ensureInit(SDL_INIT_VIDEO)) return zym_newNull();
    SDL_IOStream* io = SDL_IOFromConstMem(data, size);
    if (!io) return zym_newNull();
    SDL_Surface* surf = loadSurfaceFromIO(io, fmt);
    if (!surf) return zym_newNull();
    auto* h = new SurfaceHandle();
    h->surface = surf;
    return makeSurfaceInstance(vm, h);
}

ZymValue s_saveImage(ZymVM* vm, ZymValue /*self*/, ZymValue sv, ZymValue pv, ZymValue fv) {
    auto* sh = surfaceFromValue(vm, sv);
    if (!sh || !sh->surface) {
        zym_runtimeError(vm, "sdl.saveImage(surface, path, format?): first arg must be a Surface");
        return ZYM_ERROR;
    }
    std::string path;
    if (!reqStr(vm, pv, "sdl.saveImage(surface, path, format?)", &path)) return ZYM_ERROR;
    std::string fmt;
    if (!zym_isNull(fv)) {
        if (!reqStr(vm, fv, "sdl.saveImage(surface, path, format?)", &fmt)) return ZYM_ERROR;
    }
    fmt = pickFormat(fmt, path);
    if (fmt.empty()) fmt = "png";
    SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "wb");
    if (!io) return zym_newBool(false);
    return zym_newBool(saveSurfaceToIO(sh->surface, io, fmt));
}

ZymValue s_saveImageToBuffer(ZymVM* vm, ZymValue /*self*/, ZymValue sv, ZymValue fv) {
    auto* sh = surfaceFromValue(vm, sv);
    if (!sh || !sh->surface) {
        zym_runtimeError(vm, "sdl.saveImageToBuffer(surface, format?): first arg must be a Surface");
        return ZYM_ERROR;
    }
    std::string fmt;
    if (!zym_isNull(fv)) {
        if (!reqStr(vm, fv, "sdl.saveImageToBuffer(surface, format?)", &fmt)) return ZYM_ERROR;
    }
    fmt = pickFormat(fmt, "");
    if (fmt.empty()) fmt = "png";
    SDL_IOStream* io = SDL_IOFromDynamicMem();
    if (!io) return zym_newNull();
    // saveSurfaceToIO closes `io` on success or failure (closeio=true).
    // We need the encoded bytes BEFORE the close happens — so on success
    // we have to grab the dynamic memory pointer + size via the IO
    // properties first, then let the save close it. Trick: stash the
    // properties handle now so we can read it after the call... but
    // SDL_CloseIO frees the dynamic buffer. So instead: do the write
    // ourselves and avoid letting saveSurfaceToIO close the stream.
    //
    // Roll a copy of the save dispatch with closeio=false so we own
    // the buffer extraction.
    bool ok = false;
    if (fmt == "bmp") {
        ok = SDL_SaveBMP_IO(sh->surface, io, false);
    }
#if defined(ZYM_SDL_IMAGE_ENABLED)
    else if (fmt == "png") {
        ok = IMG_SavePNG_IO(sh->surface, io, false);
    } else if (fmt == "jpg") {
        ok = IMG_SaveJPG_IO(sh->surface, io, false, 90);
    } else {
        SDL_SetError("unsupported image format: %s", fmt.c_str());
    }
#else
    else {
        SDL_SetError("SDL_image not built; only BMP can be saved");
    }
#endif
    if (!ok) {
        SDL_CloseIO(io);
        return zym_newNull();
    }
    // Read back the encoded bytes from the dynamic-memory IOStream.
    Sint64 size64 = SDL_GetIOSize(io);
    SDL_PropertiesID props = SDL_GetIOProperties(io);
    void* mem = SDL_GetPointerProperty(props, SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, nullptr);
    ZymValue buf = zym_newNull();
    if (mem && size64 >= 0) {
        buf = makeBufferFromBytes(vm, (const char*)mem, (size_t)size64);
    }
    SDL_CloseIO(io);
    return buf;
}

// ---- Surface instance methods --------------------------------------------

ZymValue sf_size(ZymVM* vm, ZymValue ctx) {
    SurfaceHandle* s; if (!reqSurface(vm, ctx, "Surface.size()", &s)) return ZYM_ERROR;
    ZymValue m = zym_newMap(vm); zym_pushRoot(vm, m);
    zym_mapSet(vm, m, "w", zym_newNumber(s->surface->w));
    zym_mapSet(vm, m, "h", zym_newNumber(s->surface->h));
    zym_popRoot(vm);
    return m;
}

ZymValue sf_format(ZymVM* vm, ZymValue ctx) {
    SurfaceHandle* s; if (!reqSurface(vm, ctx, "Surface.format()", &s)) return ZYM_ERROR;
    return strToZym(vm, shortFormatName(s->surface->format));
}

ZymValue sf_pitch(ZymVM* vm, ZymValue ctx) {
    SurfaceHandle* s; if (!reqSurface(vm, ctx, "Surface.pitch()", &s)) return ZYM_ERROR;
    return zym_newNumber(s->surface->pitch);
}

ZymValue sf_clone(ZymVM* vm, ZymValue ctx) {
    SurfaceHandle* s; if (!reqSurface(vm, ctx, "Surface.clone()", &s)) return ZYM_ERROR;
    SDL_Surface* dup = SDL_DuplicateSurface(s->surface);
    if (!dup) return zym_newNull();
    auto* h = new SurfaceHandle();
    h->surface = dup;
    return makeSurfaceInstance(vm, h);
}

ZymValue sf_convert(ZymVM* vm, ZymValue ctx, ZymValue fv) {
    SurfaceHandle* s; if (!reqSurface(vm, ctx, "Surface.convert(format)", &s)) return ZYM_ERROR;
    std::string fmt;
    if (!reqStr(vm, fv, "Surface.convert(format)", &fmt)) return ZYM_ERROR;
    SDL_PixelFormat target = parseFormatName(fmt, SDL_PIXELFORMAT_UNKNOWN);
    if (target == SDL_PIXELFORMAT_UNKNOWN) {
        zym_runtimeError(vm, "Surface.convert: unknown format '%s'", fmt.c_str());
        return ZYM_ERROR;
    }
    SDL_Surface* out = SDL_ConvertSurface(s->surface, target);
    if (!out) return zym_newNull();
    auto* h = new SurfaceHandle();
    h->surface = out;
    return makeSurfaceInstance(vm, h);
}

ZymValue sf_fill(ZymVM* vm, ZymValue ctx, ZymValue cv, ZymValue rv) {
    SurfaceHandle* s; if (!reqSurface(vm, ctx, "Surface.fill(color, rect?)", &s)) return ZYM_ERROR;
    SDL_Color c;
    if (!parseColor(vm, cv, "Surface.fill(color, rect?)", &c)) return ZYM_ERROR;
    const SDL_PixelFormatDetails* det = SDL_GetPixelFormatDetails(s->surface->format);
    Uint32 packed = det ? SDL_MapRGBA(det, nullptr, c.r, c.g, c.b, c.a) : 0;
    if (zym_isNull(rv)) {
        return zym_newBool(SDL_FillSurfaceRect(s->surface, nullptr, packed));
    }
    SDL_Rect r;
    if (!parseRect(vm, rv, "Surface.fill(color, rect?)", &r)) return ZYM_ERROR;
    return zym_newBool(SDL_FillSurfaceRect(s->surface, &r, packed));
}

ZymValue sf_clear(ZymVM* vm, ZymValue ctx, ZymValue cv) {
    SurfaceHandle* s; if (!reqSurface(vm, ctx, "Surface.clear(color?)", &s)) return ZYM_ERROR;
    SDL_Color c = { 0, 0, 0, 0 };
    if (!zym_isNull(cv)) {
        if (!parseColor(vm, cv, "Surface.clear(color?)", &c)) return ZYM_ERROR;
    }
    const SDL_PixelFormatDetails* det = SDL_GetPixelFormatDetails(s->surface->format);
    Uint32 packed = det ? SDL_MapRGBA(det, nullptr, c.r, c.g, c.b, c.a) : 0;
    return zym_newBool(SDL_FillSurfaceRect(s->surface, nullptr, packed));
}

ZymValue sf_getPixel(ZymVM* vm, ZymValue ctx, ZymValue xv, ZymValue yv) {
    SurfaceHandle* s; if (!reqSurface(vm, ctx, "Surface.getPixel(x, y)", &s)) return ZYM_ERROR;
    double dx, dy;
    if (!reqNum(vm, xv, "Surface.getPixel(x, y)", &dx)) return ZYM_ERROR;
    if (!reqNum(vm, yv, "Surface.getPixel(x, y)", &dy)) return ZYM_ERROR;
    Uint8 r=0, g=0, b=0, a=255;
    if (!SDL_ReadSurfacePixel(s->surface, (int)dx, (int)dy, &r, &g, &b, &a)) {
        return zym_newNull();
    }
    return colorToZym(vm, r, g, b, a);
}

ZymValue sf_setPixel(ZymVM* vm, ZymValue ctx, ZymValue xv, ZymValue yv, ZymValue cv) {
    SurfaceHandle* s; if (!reqSurface(vm, ctx, "Surface.setPixel(x, y, color)", &s)) return ZYM_ERROR;
    double dx, dy;
    if (!reqNum(vm, xv, "Surface.setPixel(x, y, color)", &dx)) return ZYM_ERROR;
    if (!reqNum(vm, yv, "Surface.setPixel(x, y, color)", &dy)) return ZYM_ERROR;
    SDL_Color c;
    if (!parseColor(vm, cv, "Surface.setPixel(x, y, color)", &c)) return ZYM_ERROR;
    return zym_newBool(SDL_WriteSurfacePixel(s->surface, (int)dx, (int)dy, c.r, c.g, c.b, c.a));
}

ZymValue sf_blit(ZymVM* vm, ZymValue ctx, ZymValue srcV, ZymValue srcRectV, ZymValue dstRectV) {
    SurfaceHandle* dst; if (!reqSurface(vm, ctx, "Surface.blit(src, srcRect?, dstRect?)", &dst)) return ZYM_ERROR;
    SurfaceHandle* src = surfaceFromValue(vm, srcV);
    if (!src || !src->surface) {
        zym_runtimeError(vm, "Surface.blit: src must be a Surface");
        return ZYM_ERROR;
    }
    SDL_Rect sR, dR;
    const SDL_Rect* sP = nullptr;
    const SDL_Rect* dP = nullptr;
    if (!zym_isNull(srcRectV)) {
        if (!parseRect(vm, srcRectV, "Surface.blit", &sR)) return ZYM_ERROR;
        sP = &sR;
    }
    if (!zym_isNull(dstRectV)) {
        if (!parseRect(vm, dstRectV, "Surface.blit", &dR)) return ZYM_ERROR;
        dP = &dR;
    }
    // SDL_BlitSurface takes a non-const dstRect (it gets updated with
    // clipped extent), but treats it as an in/out — passing a copy is
    // fine.
    SDL_Rect dRCopy;
    SDL_Rect* dRWrite = nullptr;
    if (dP) { dRCopy = *dP; dRWrite = &dRCopy; }
    return zym_newBool(SDL_BlitSurface(src->surface, sP, dst->surface, dRWrite));
}

ZymValue sf_blitScaled(ZymVM* vm, ZymValue ctx, ZymValue srcV, ZymValue srcRectV, ZymValue dstRectV, ZymValue modeV) {
    SurfaceHandle* dst; if (!reqSurface(vm, ctx, "Surface.blitScaled", &dst)) return ZYM_ERROR;
    SurfaceHandle* src = surfaceFromValue(vm, srcV);
    if (!src || !src->surface) {
        zym_runtimeError(vm, "Surface.blitScaled: src must be a Surface");
        return ZYM_ERROR;
    }
    SDL_Rect sR, dR;
    const SDL_Rect* sP = nullptr;
    const SDL_Rect* dP = nullptr;
    if (!zym_isNull(srcRectV)) {
        if (!parseRect(vm, srcRectV, "Surface.blitScaled", &sR)) return ZYM_ERROR;
        sP = &sR;
    }
    if (!zym_isNull(dstRectV)) {
        if (!parseRect(vm, dstRectV, "Surface.blitScaled", &dR)) return ZYM_ERROR;
        dP = &dR;
    }
    SDL_ScaleMode mode = SDL_SCALEMODE_LINEAR;
    if (!zym_isNull(modeV)) {
        std::string m;
        if (!reqStr(vm, modeV, "Surface.blitScaled", &m)) return ZYM_ERROR;
        if (m == "nearest") mode = SDL_SCALEMODE_NEAREST;
        else if (m == "linear") mode = SDL_SCALEMODE_LINEAR;
    }
    return zym_newBool(SDL_BlitSurfaceScaled(src->surface, sP, dst->surface, dP, mode));
}

ZymValue sf_setBlendMode(ZymVM* vm, ZymValue ctx, ZymValue mv) {
    SurfaceHandle* s; if (!reqSurface(vm, ctx, "Surface.setBlendMode(mode)", &s)) return ZYM_ERROR;
    std::string m;
    if (!reqStr(vm, mv, "Surface.setBlendMode(mode)", &m)) return ZYM_ERROR;
    return zym_newBool(SDL_SetSurfaceBlendMode(s->surface, parseBlendMode(m)));
}

ZymValue sf_getBlendMode(ZymVM* vm, ZymValue ctx) {
    SurfaceHandle* s; if (!reqSurface(vm, ctx, "Surface.getBlendMode()", &s)) return ZYM_ERROR;
    SDL_BlendMode m = SDL_BLENDMODE_NONE;
    SDL_GetSurfaceBlendMode(s->surface, &m);
    return strToZym(vm, blendModeName(m));
}

ZymValue sf_setAlphaMod(ZymVM* vm, ZymValue ctx, ZymValue av) {
    SurfaceHandle* s; if (!reqSurface(vm, ctx, "Surface.setAlphaMod(a)", &s)) return ZYM_ERROR;
    double a;
    if (!reqNum(vm, av, "Surface.setAlphaMod(a)", &a)) return ZYM_ERROR;
    if (a < 0) a = 0; if (a > 255) a = 255;
    return zym_newBool(SDL_SetSurfaceAlphaMod(s->surface, (Uint8)a));
}

ZymValue sf_setColorMod(ZymVM* vm, ZymValue ctx, ZymValue rv, ZymValue gv, ZymValue bv) {
    SurfaceHandle* s; if (!reqSurface(vm, ctx, "Surface.setColorMod(r,g,b)", &s)) return ZYM_ERROR;
    double r, g, b;
    if (!reqNum(vm, rv, "Surface.setColorMod(r,g,b)", &r)) return ZYM_ERROR;
    if (!reqNum(vm, gv, "Surface.setColorMod(r,g,b)", &g)) return ZYM_ERROR;
    if (!reqNum(vm, bv, "Surface.setColorMod(r,g,b)", &b)) return ZYM_ERROR;
    auto cl = [](double v){ if (v<0) v=0; if (v>255) v=255; return (Uint8)v; };
    return zym_newBool(SDL_SetSurfaceColorMod(s->surface, cl(r), cl(g), cl(b)));
}

ZymValue sf_setColorKey(ZymVM* vm, ZymValue ctx, ZymValue cv) {
    SurfaceHandle* s; if (!reqSurface(vm, ctx, "Surface.setColorKey(color|null)", &s)) return ZYM_ERROR;
    if (zym_isNull(cv)) {
        return zym_newBool(SDL_SetSurfaceColorKey(s->surface, false, 0));
    }
    SDL_Color c;
    if (!parseColor(vm, cv, "Surface.setColorKey(color|null)", &c)) return ZYM_ERROR;
    const SDL_PixelFormatDetails* det = SDL_GetPixelFormatDetails(s->surface->format);
    Uint32 packed = det ? SDL_MapRGBA(det, nullptr, c.r, c.g, c.b, c.a) : 0;
    return zym_newBool(SDL_SetSurfaceColorKey(s->surface, true, packed));
}

ZymValue sf_getClipRect(ZymVM* vm, ZymValue ctx) {
    SurfaceHandle* s; if (!reqSurface(vm, ctx, "Surface.getClipRect()", &s)) return ZYM_ERROR;
    SDL_Rect r{};
    SDL_GetSurfaceClipRect(s->surface, &r);
    return rectToZym(vm, r);
}

ZymValue sf_setClipRect(ZymVM* vm, ZymValue ctx, ZymValue rv) {
    SurfaceHandle* s; if (!reqSurface(vm, ctx, "Surface.setClipRect(rect|null)", &s)) return ZYM_ERROR;
    if (zym_isNull(rv)) {
        return zym_newBool(SDL_SetSurfaceClipRect(s->surface, nullptr));
    }
    SDL_Rect r;
    if (!parseRect(vm, rv, "Surface.setClipRect(rect|null)", &r)) return ZYM_ERROR;
    return zym_newBool(SDL_SetSurfaceClipRect(s->surface, &r));
}

// applyMask: combine the alpha channel of `mask` into `dst` per `mode`.
// Modes:
//   "alpha"     : dst.a := dst.a * mask.a / 255          (default)
//   "luminance" : dst.a := dst.a * luma(mask.rgb) / 255
//   "key"       : dst.a := 0 where mask has its colorkey-equivalent (alpha 0 here)
// Implementation walks both surfaces pixel-by-pixel via SDL_ReadSurfacePixel
// / SDL_WriteSurfacePixel. Slow but correct on any surface format.
ZymValue sf_applyMask(ZymVM* vm, ZymValue ctx, ZymValue maskV, ZymValue modeV) {
    SurfaceHandle* dst; if (!reqSurface(vm, ctx, "Surface.applyMask(mask, mode?)", &dst)) return ZYM_ERROR;
    SurfaceHandle* mask = surfaceFromValue(vm, maskV);
    if (!mask || !mask->surface) {
        zym_runtimeError(vm, "Surface.applyMask: mask must be a Surface");
        return ZYM_ERROR;
    }
    std::string mode = "alpha";
    if (!zym_isNull(modeV)) {
        if (!reqStr(vm, modeV, "Surface.applyMask(mask, mode?)", &mode)) return ZYM_ERROR;
    }
    int w = std::min(dst->surface->w, mask->surface->w);
    int h = std::min(dst->surface->h, mask->surface->h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            Uint8 dr=0, dg=0, db=0, da=255;
            Uint8 mr=0, mg=0, mb=0, ma=255;
            SDL_ReadSurfacePixel(dst->surface,  x, y, &dr, &dg, &db, &da);
            SDL_ReadSurfacePixel(mask->surface, x, y, &mr, &mg, &mb, &ma);
            Uint8 newA = da;
            if (mode == "luminance") {
                // BT.601 luma; cheap + perceptual enough for masks.
                int luma = (77 * mr + 150 * mg + 29 * mb) >> 8;
                newA = (Uint8)((da * luma) / 255);
            } else if (mode == "key") {
                newA = (ma == 0) ? 0 : da;
            } else {
                // "alpha" (default)
                newA = (Uint8)((da * ma) / 255);
            }
            SDL_WriteSurfacePixel(dst->surface, x, y, dr, dg, db, newA);
        }
    }
    return zym_newBool(true);
}

ZymValue sf_free(ZymVM* /*vm*/, ZymValue ctx) {
    auto* s = unwrapSurface(ctx);
    if (!s) return zym_newNull();
    if (s->surface) {
        SDL_DestroySurface(s->surface);
        s->surface = nullptr;
    }
    return zym_newNull();
}

ZymValue makeSurfaceInstance(ZymVM* vm, SurfaceHandle* s) {
    ZymValue ctx = zym_createNativeContext(vm, s, surfaceFinalizer);
    zym_pushRoot(vm, ctx);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__surface__", ctx);

#define M(name, sig, fn) do { \
    ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctx); \
    zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
} while (0)

    M("size",         "size()",                       sf_size);
    M("format",       "format()",                     sf_format);
    M("pitch",        "pitch()",                      sf_pitch);
    M("clone",        "clone()",                      sf_clone);
    M("convert",      "convert(format)",              sf_convert);
    M("fill",         "fill(color, rect)",            sf_fill);
    M("clear",        "clear(color)",                 sf_clear);
    M("getPixel",     "getPixel(x, y)",               sf_getPixel);
    M("setPixel",     "setPixel(x, y, color)",        sf_setPixel);
    M("blit",         "blit(src, srcRect, dstRect)",  sf_blit);
    M("blitScaled",   "blitScaled(src, srcRect, dstRect, mode)", sf_blitScaled);
    M("setBlendMode", "setBlendMode(mode)",           sf_setBlendMode);
    M("getBlendMode", "getBlendMode()",               sf_getBlendMode);
    M("setAlphaMod",  "setAlphaMod(a)",               sf_setAlphaMod);
    M("setColorMod",  "setColorMod(r, g, b)",         sf_setColorMod);
    M("setColorKey",  "setColorKey(color)",           sf_setColorKey);
    M("getClipRect",  "getClipRect()",                sf_getClipRect);
    M("setClipRect",  "setClipRect(rect)",            sf_setClipRect);
    M("applyMask",    "applyMask(mask, mode)",        sf_applyMask);
    M("free",         "free()",                       sf_free);

#undef M

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}

// ---- Surface module-level factories --------------------------------------

ZymValue s_surfaceNew(ZymVM* vm, ZymValue /*self*/, ZymValue wv, ZymValue hv, ZymValue fv) {
    double w, h;
    if (!reqNum(vm, wv, "SDL.Surface.new(w, h, format?)", &w)) return ZYM_ERROR;
    if (!reqNum(vm, hv, "SDL.Surface.new(w, h, format?)", &h)) return ZYM_ERROR;
    SDL_PixelFormat fmt = SDL_PIXELFORMAT_RGBA32;
    if (!zym_isNull(fv)) {
        std::string f;
        if (!reqStr(vm, fv, "SDL.Surface.new(w, h, format?)", &f)) return ZYM_ERROR;
        fmt = parseFormatName(f, SDL_PIXELFORMAT_RGBA32);
    }
    SDL_Surface* surf = SDL_CreateSurface((int)w, (int)h, fmt);
    if (!surf) return zym_newNull();
    auto* sh = new SurfaceHandle();
    sh->surface = surf;
    return makeSurfaceInstance(vm, sh);
}

ZymValue s_surfaceFromBuffer(ZymVM* vm, ZymValue /*self*/, ZymValue bv, ZymValue wv, ZymValue hv, ZymValue pv, ZymValue fv) {
    const char* data = nullptr;
    size_t size = 0;
    if (!readBufferBytes(vm, bv, &data, &size)) {
        zym_runtimeError(vm, "SDL.Surface.fromBuffer: first arg must be a Buffer");
        return ZYM_ERROR;
    }
    double w, h, pitch;
    if (!reqNum(vm, wv, "SDL.Surface.fromBuffer", &w))     return ZYM_ERROR;
    if (!reqNum(vm, hv, "SDL.Surface.fromBuffer", &h))     return ZYM_ERROR;
    if (!reqNum(vm, pv, "SDL.Surface.fromBuffer", &pitch)) return ZYM_ERROR;
    SDL_PixelFormat fmt = SDL_PIXELFORMAT_RGBA32;
    if (!zym_isNull(fv)) {
        std::string f;
        if (!reqStr(vm, fv, "SDL.Surface.fromBuffer", &f)) return ZYM_ERROR;
        fmt = parseFormatName(f, SDL_PIXELFORMAT_RGBA32);
    }
    // CreateSurfaceFrom doesn't copy — the caller's buffer must outlive
    // the surface. Buffer values are GC-managed, so we copy into a
    // freshly-owned SDL surface to keep lifetimes sane.
    SDL_Surface* dst = SDL_CreateSurface((int)w, (int)h, fmt);
    if (!dst) return zym_newNull();
    int rowBytes = (int)pitch;
    int copyRowBytes = std::min(rowBytes, dst->pitch);
    for (int y = 0; y < (int)h; y++) {
        if ((size_t)((y + 1) * rowBytes) > size) break;
        std::memcpy(
            (Uint8*)dst->pixels + y * dst->pitch,
            data + y * rowBytes,
            copyRowBytes);
    }
    auto* sh = new SurfaceHandle();
    sh->surface = dst;
    return makeSurfaceInstance(vm, sh);
}

// ---- Texture (GPU-side, stable handle) -----------------------------------
//
// Textures are bound to a Window's renderer. Factories live on the
// Window (win.createTexture / win.textureFromSurface) so renderer
// ownership is visible at the call site. The handle is stable for the
// lifetime of the Texture: update() / updateRect() / refresh() mutate
// the GPU contents behind the handle, never invalidate it. This is
// what makes the "user gives texture to UI once, then keeps editing the
// upstream Surface" workflow work — see future/gui.md §2.0.
//
// linkSurface (opt-in via `link: true` on textureFromSurface, or the
// `linkSurface` opt on createTexture) keeps the source Surface alive
// via a back-reference Zym map slot so tex.refresh() is meaningful.
// Without it, the texture is independent and texture.source() returns
// null.

struct TextureHandle {
    SDL_Texture*  texture = nullptr;
    // The owning Window's renderer. We capture the WindowHandle*
    // rather than the SDL_Renderer* directly because we want to
    // detect "renderer was destroyed under us" (use after free of
    // the window) and raise a clean Zym runtime error.
    WindowHandle* owner   = nullptr;
    // Cached width/height so size() doesn't need a live renderer.
    int w = 0;
    int h = 0;
    // Linked source surface (opt-in via `link: true` / `linkSurface`).
    // Kept GC-rooted via the `__link__` slot on the Texture instance
    // map so the Surface stays alive for the texture's lifetime; the
    // pointer here is for tx_refresh / tx_source access without going
    // back through the map.
    SurfaceHandle* linkedSurface = nullptr;
    // The Zym `Surface` value behind `linkedSurface`, returned as-is
    // by tx_source() so scripts get the same map they passed in.
    // ZymValue is a plain handle/word; storing it in a C struct does
    // not root it for GC — we additionally stash it in the texture
    // map's `__link__` slot for rooting (see makeTextureInstance).
    ZymValue       linkedValue;
};

void textureFinalizer(ZymVM*, void* data) {
    auto* t = static_cast<TextureHandle*>(data);
    if (!t) return;
    // Only destroy if the owning renderer is still alive — otherwise
    // the SDL_Renderer already tore the texture down with it.
    if (t->texture && t->owner && t->owner->renderer) {
        SDL_DestroyTexture(t->texture);
    }
    delete t;
}

TextureHandle* unwrapTexture(ZymValue ctx) {
    return static_cast<TextureHandle*>(zym_getNativeData(ctx));
}

bool reqTexture(ZymVM* vm, ZymValue ctx, const char* where, TextureHandle** out) {
    auto* t = unwrapTexture(ctx);
    if (!t || !t->texture) {
        zym_runtimeError(vm, "%s: invalid Texture handle", where);
        return false;
    }
    if (!t->owner || !t->owner->renderer) {
        zym_runtimeError(vm, "%s: owning Window/renderer has been destroyed", where);
        return false;
    }
    *out = t;
    return true;
}

SDL_ScaleMode parseScaleMode(const std::string& s) {
    if (s == "nearest") return SDL_SCALEMODE_NEAREST;
    return SDL_SCALEMODE_LINEAR;
}

const char* scaleModeName(SDL_ScaleMode m) {
    switch (m) {
        case SDL_SCALEMODE_NEAREST: return "nearest";
        case SDL_SCALEMODE_LINEAR:  return "linear";
        default:                    return "linear";
    }
}

// Forward: build a Texture instance map wrapping a TextureHandle*. The
// optional `linkSurface` value (a Surface map) is stored in a hidden
// `__link__` slot so it stays GC-reachable for the texture's lifetime.
ZymValue makeTextureInstance(ZymVM* vm, TextureHandle* t, ZymValue linkSurface);

ZymValue tx_size(ZymVM* vm, ZymValue ctx) {
    TextureHandle* t; if (!reqTexture(vm, ctx, "Texture.size()", &t)) return ZYM_ERROR;
    ZymValue m = zym_newMap(vm); zym_pushRoot(vm, m);
    zym_mapSet(vm, m, "w", zym_newNumber(t->w));
    zym_mapSet(vm, m, "h", zym_newNumber(t->h));
    zym_popRoot(vm);
    return m;
}

ZymValue tx_source(ZymVM* /*vm*/, ZymValue ctx) {
    auto* t = unwrapTexture(ctx);
    if (!t || !t->linkedSurface) return zym_newNull();
    return t->linkedValue;
}

// Internal helper: do a full or partial update from a Surface.
bool textureUpdateFromSurface(SDL_Texture* tex, SDL_Surface* surf, const SDL_Rect* dstRect) {
    if (!tex || !surf) return false;
    // SDL_UpdateTexture needs the source pixels to match the texture's
    // pixel format. If the surface format differs, convert in-flight.
    SDL_PixelFormat texFmt;
    {
        // Query the texture's format via its properties.
        SDL_PropertiesID props = SDL_GetTextureProperties(tex);
        texFmt = (SDL_PixelFormat)SDL_GetNumberProperty(
            props, SDL_PROP_TEXTURE_FORMAT_NUMBER, SDL_PIXELFORMAT_RGBA32);
    }
    SDL_Surface* src = surf;
    SDL_Surface* tmp = nullptr;
    if (src->format != texFmt) {
        tmp = SDL_ConvertSurface(src, texFmt);
        if (!tmp) return false;
        src = tmp;
    }
    bool ok = false;
    if (dstRect) {
        // SDL_UpdateTexture's rect describes the region of the texture
        // to write; pixels start at the top-left of `src` and step by
        // src->pitch. We rely on the caller having sized the surface
        // (or sub-rect) appropriately.
        ok = SDL_UpdateTexture(tex, dstRect, src->pixels, src->pitch);
    } else {
        ok = SDL_UpdateTexture(tex, nullptr, src->pixels, src->pitch);
    }
    if (tmp) SDL_DestroySurface(tmp);
    return ok;
}

ZymValue tx_update(ZymVM* vm, ZymValue ctx, ZymValue sv, ZymValue rv) {
    TextureHandle* t; if (!reqTexture(vm, ctx, "Texture.update(surface, dstRect?)", &t)) return ZYM_ERROR;
    auto* sh = surfaceFromValue(vm, sv);
    if (!sh || !sh->surface) {
        zym_runtimeError(vm, "Texture.update: first arg must be a Surface");
        return ZYM_ERROR;
    }
    if (zym_isNull(rv)) {
        return zym_newBool(textureUpdateFromSurface(t->texture, sh->surface, nullptr));
    }
    SDL_Rect r;
    if (!parseRect(vm, rv, "Texture.update(surface, dstRect?)", &r)) return ZYM_ERROR;
    return zym_newBool(textureUpdateFromSurface(t->texture, sh->surface, &r));
}

ZymValue tx_updateRect(ZymVM* vm, ZymValue ctx, ZymValue sv, ZymValue xv, ZymValue yv, ZymValue wv, ZymValue hv) {
    TextureHandle* t; if (!reqTexture(vm, ctx, "Texture.updateRect(surface, x, y, w, h)", &t)) return ZYM_ERROR;
    auto* sh = surfaceFromValue(vm, sv);
    if (!sh || !sh->surface) {
        zym_runtimeError(vm, "Texture.updateRect: first arg must be a Surface");
        return ZYM_ERROR;
    }
    double x, y, w, h;
    if (!reqNum(vm, xv, "Texture.updateRect", &x)) return ZYM_ERROR;
    if (!reqNum(vm, yv, "Texture.updateRect", &y)) return ZYM_ERROR;
    if (!reqNum(vm, wv, "Texture.updateRect", &w)) return ZYM_ERROR;
    if (!reqNum(vm, hv, "Texture.updateRect", &h)) return ZYM_ERROR;
    SDL_Rect r{ (int)x, (int)y, (int)w, (int)h };
    return zym_newBool(textureUpdateFromSurface(t->texture, sh->surface, &r));
}

// refresh() re-uploads from the linked source surface. Requires that
// the texture was created with `link: true` / `linkSurface: ...`. The
// linked surface is held both as a SurfaceHandle* (for direct access)
// and as a Zym Surface value stashed in the texture map's `__link__`
// slot so the GC keeps it alive — see makeTextureInstance.
ZymValue tx_refresh(ZymVM* vm, ZymValue ctx, ZymValue rv) {
    TextureHandle* t; if (!reqTexture(vm, ctx, "Texture.refresh(dirtyRect?)", &t)) return ZYM_ERROR;
    if (!t->linkedSurface || !t->linkedSurface->surface) {
        zym_runtimeError(vm, "Texture.refresh: texture has no linked Surface (create with { link: true })");
        return ZYM_ERROR;
    }
    if (zym_isNull(rv)) {
        return zym_newBool(textureUpdateFromSurface(t->texture, t->linkedSurface->surface, nullptr));
    }
    SDL_Rect r;
    if (!parseRect(vm, rv, "Texture.refresh(dirtyRect?)", &r)) return ZYM_ERROR;
    return zym_newBool(textureUpdateFromSurface(t->texture, t->linkedSurface->surface, &r));
}

ZymValue tx_setBlendMode(ZymVM* vm, ZymValue ctx, ZymValue mv) {
    TextureHandle* t; if (!reqTexture(vm, ctx, "Texture.setBlendMode(mode)", &t)) return ZYM_ERROR;
    std::string m;
    if (!reqStr(vm, mv, "Texture.setBlendMode(mode)", &m)) return ZYM_ERROR;
    return zym_newBool(SDL_SetTextureBlendMode(t->texture, parseBlendMode(m)));
}

ZymValue tx_setScaleMode(ZymVM* vm, ZymValue ctx, ZymValue mv) {
    TextureHandle* t; if (!reqTexture(vm, ctx, "Texture.setScaleMode(mode)", &t)) return ZYM_ERROR;
    std::string m;
    if (!reqStr(vm, mv, "Texture.setScaleMode(mode)", &m)) return ZYM_ERROR;
    return zym_newBool(SDL_SetTextureScaleMode(t->texture, parseScaleMode(m)));
}

ZymValue tx_setAlphaMod(ZymVM* vm, ZymValue ctx, ZymValue av) {
    TextureHandle* t; if (!reqTexture(vm, ctx, "Texture.setAlphaMod(a)", &t)) return ZYM_ERROR;
    double a;
    if (!reqNum(vm, av, "Texture.setAlphaMod(a)", &a)) return ZYM_ERROR;
    int clamped = (int)a; if (clamped < 0) clamped = 0; if (clamped > 255) clamped = 255;
    return zym_newBool(SDL_SetTextureAlphaMod(t->texture, (Uint8)clamped));
}

ZymValue tx_setColorMod(ZymVM* vm, ZymValue ctx, ZymValue rv, ZymValue gv, ZymValue bv) {
    TextureHandle* t; if (!reqTexture(vm, ctx, "Texture.setColorMod(r, g, b)", &t)) return ZYM_ERROR;
    double r, g, b;
    if (!reqNum(vm, rv, "Texture.setColorMod", &r)) return ZYM_ERROR;
    if (!reqNum(vm, gv, "Texture.setColorMod", &g)) return ZYM_ERROR;
    if (!reqNum(vm, bv, "Texture.setColorMod", &b)) return ZYM_ERROR;
    auto clamp = [](double d){ int i=(int)d; return (Uint8)(i<0?0:(i>255?255:i)); };
    return zym_newBool(SDL_SetTextureColorMod(t->texture, clamp(r), clamp(g), clamp(b)));
}

ZymValue tx_free(ZymVM* /*vm*/, ZymValue ctx) {
    auto* t = unwrapTexture(ctx);
    if (!t) return zym_newNull();
    if (t->texture && t->owner && t->owner->renderer) {
        SDL_DestroyTexture(t->texture);
    }
    t->texture = nullptr;
    return zym_newNull();
}

ZymValue makeTextureInstance(ZymVM* vm, TextureHandle* t, ZymValue linkSurface) {
    ZymValue ctx = zym_createNativeContext(vm, t, textureFinalizer);
    zym_pushRoot(vm, ctx);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__tex__", ctx);
    // Stash the linked Surface (if any) so it stays GC-reachable for
    // the texture's lifetime. tx_source / tx_refresh look here.
    zym_mapSet(vm, obj, "__link__", linkSurface);

#define M(name, sig, fn) do { \
    ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctx); \
    zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
} while (0)

    M("size",         "size()",                          tx_size);
    M("update",       "update(surface, dstRect)",        tx_update);
    M("updateRect",   "updateRect(surface, x, y, w, h)", tx_updateRect);
    M("refresh",      "refresh(dirtyRect)",              tx_refresh);
    M("source",       "source()",                        tx_source);
    M("setBlendMode", "setBlendMode(mode)",              tx_setBlendMode);
    M("setScaleMode", "setScaleMode(mode)",              tx_setScaleMode);
    M("setAlphaMod",  "setAlphaMod(a)",                  tx_setAlphaMod);
    M("setColorMod",  "setColorMod(r, g, b)",            tx_setColorMod);
    M("free",         "free()",                          tx_free);

#undef M

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}

// Parse the `opts` map for createTexture. Returns the SDL_TextureAccess
// + format (defaults: STREAMING + RGBA32), and reads `linkSurface` /
// `link` flags. The actual Surface back-reference is plumbed via the
// `outLink` out-param (left as null when no link is requested).
bool parseTextureOpts(ZymVM* vm, ZymValue ov, SDL_TextureAccess* outAccess,
                      SDL_PixelFormat* outFormat, ZymValue* outLink,
                      const char* where) {
    *outAccess = SDL_TEXTUREACCESS_STREAMING;
    *outFormat = SDL_PIXELFORMAT_RGBA32;
    *outLink   = zym_newNull();
    if (!zym_isMap(ov)) return true;
    if (zym_mapHas(ov, "access")) {
        ZymValue av = zym_mapGet(vm, ov, "access");
        std::string a;
        if (!reqStr(vm, av, where, &a)) return false;
        if      (a == "static")    *outAccess = SDL_TEXTUREACCESS_STATIC;
        else if (a == "streaming") *outAccess = SDL_TEXTUREACCESS_STREAMING;
        else if (a == "target")    *outAccess = SDL_TEXTUREACCESS_TARGET;
        else {
            zym_runtimeError(vm, "%s: unknown access mode '%s' (use static/streaming/target)", where, a.c_str());
            return false;
        }
    }
    if (zym_mapHas(ov, "format")) {
        ZymValue fv = zym_mapGet(vm, ov, "format");
        if (!zym_isNull(fv)) {
            std::string f;
            if (!reqStr(vm, fv, where, &f)) return false;
            *outFormat = parseFormatName(f, SDL_PIXELFORMAT_RGBA32);
        }
    }
    if (zym_mapHas(ov, "linkSurface")) {
        ZymValue lv = zym_mapGet(vm, ov, "linkSurface");
        if (!zym_isNull(lv)) {
            // Validate it's actually a Surface — refuse silently-bad input.
            if (!surfaceFromValue(vm, lv)) {
                zym_runtimeError(vm, "%s: linkSurface is not a Surface value", where);
                return false;
            }
            *outLink = lv;
        }
    }
    return true;
}

// Window.createTexture(w, h, opts?) — bound on the Window instance.
ZymValue w_createTexture(ZymVM* vm, ZymValue ctx, ZymValue wv, ZymValue hv, ZymValue ov) {
    WindowHandle* wh;
    if (!reqWindow(vm, ctx, "win.createTexture(w, h, opts?)", &wh)) return ZYM_ERROR;
    if (!wh->renderer) {
        zym_runtimeError(vm, "win.createTexture: window has no renderer");
        return ZYM_ERROR;
    }
    double w, h;
    if (!reqNum(vm, wv, "win.createTexture(w, h, opts?)", &w)) return ZYM_ERROR;
    if (!reqNum(vm, hv, "win.createTexture(w, h, opts?)", &h)) return ZYM_ERROR;
    SDL_TextureAccess access;
    SDL_PixelFormat   format;
    ZymValue          link;
    if (!parseTextureOpts(vm, ov, &access, &format, &link, "win.createTexture")) return ZYM_ERROR;

    SDL_Texture* tex = SDL_CreateTexture(wh->renderer, format, access, (int)w, (int)h);
    if (!tex) return zym_newNull();
    auto* th = new TextureHandle();
    th->texture = tex;
    th->owner   = wh;
    th->w       = (int)w;
    th->h       = (int)h;
    if (!zym_isNull(link)) {
        th->linkedSurface = surfaceFromValue(vm, link);
        th->linkedValue   = link;
    } else {
        th->linkedValue = zym_newNull();
    }
    return makeTextureInstance(vm, th, link);
}

// Window.textureFromSurface(surface, opts?) — bound on the Window
// instance. `opts.link = true` keeps the source Surface as a GC-rooted
// back-reference so tex.refresh() / tex.source() work.
ZymValue w_textureFromSurface(ZymVM* vm, ZymValue ctx, ZymValue sv, ZymValue ov) {
    WindowHandle* wh;
    if (!reqWindow(vm, ctx, "win.textureFromSurface(surface, opts?)", &wh)) return ZYM_ERROR;
    if (!wh->renderer) {
        zym_runtimeError(vm, "win.textureFromSurface: window has no renderer");
        return ZYM_ERROR;
    }
    auto* sh = surfaceFromValue(vm, sv);
    if (!sh || !sh->surface) {
        zym_runtimeError(vm, "win.textureFromSurface: first arg must be a Surface");
        return ZYM_ERROR;
    }
    bool link = false;
    if (zym_isMap(ov)) {
        link = mapBool(vm, ov, "link", false);
    }
    SDL_Texture* tex = SDL_CreateTextureFromSurface(wh->renderer, sh->surface);
    if (!tex) return zym_newNull();
    auto* th = new TextureHandle();
    th->texture = tex;
    th->owner   = wh;
    th->w       = sh->surface->w;
    th->h       = sh->surface->h;
    if (link) {
        th->linkedSurface = sh;
        th->linkedValue   = sv;
    } else {
        th->linkedValue = zym_newNull();
    }
    return makeTextureInstance(vm, th, link ? sv : zym_newNull());
}

} // namespace

// ---- module assembly -----------------------------------------------------

ZymValue nativeSdl_create(ZymVM* vm) {
    ZymValue context = zym_createNativeContext(vm, nullptr, nullptr);
    zym_pushRoot(vm, context);

#define MOD(name, sig, fn) \
    ZymValue name = zym_createNativeClosure(vm, sig, (void*)fn, context); \
    zym_pushRoot(vm, name);

    MOD(init,        "init()",                              s_init)
    MOD(quit,        "quit()",                              s_quit)
    MOD(lastError,   "lastError()",                         s_lastError)
    MOD(version,     "version()",                           s_version)
    MOD(createWindow,"createWindow(title, w, h, opts)",     s_createWindow)
    MOD(pollEvent,   "pollEvent()",                         s_pollEvent)
    MOD(waitEvent,   "waitEvent(timeoutMs)",                s_waitEvent)
    MOD(pushEvent,   "pushEvent(event)",                    s_pushEvent)
    MOD(keyDown,     "keyDown(scancode)",                   s_keyDown)
    MOD(mousePos,    "mousePos()",                          s_mousePos)
    MOD(mouseButtons,"mouseButtons()",                      s_mouseButtons)
    MOD(modifiers,   "modifiers()",                         s_modifiers)
    // Image I/O (PR 5).
    MOD(loadImage,         "loadImage(path)",                 s_loadImage)
    MOD(loadImageFromBuffer,"loadImageFromBuffer(buf, fmt)",  s_loadImageFromBuffer)
    MOD(saveImage,         "saveImage(surf, path, fmt)",      s_saveImage)
    MOD(saveImageToBuffer, "saveImageToBuffer(surf, fmt)",    s_saveImageToBuffer)
    // Surface factories (live under the nested SDL.Surface namespace).
    MOD(surfaceNew,        "new(w, h, fmt)",                  s_surfaceNew)
    MOD(surfaceFromBuffer, "fromBuffer(buf, w, h, pitch, fmt)",s_surfaceFromBuffer)

#undef MOD

    // Nested namespace: SDL.Surface = { new, fromBuffer }.
    ZymValue surfaceNs = zym_newMap(vm);
    zym_pushRoot(vm, surfaceNs);
    zym_mapSet(vm, surfaceNs, "new",        surfaceNew);
    zym_mapSet(vm, surfaceNs, "fromBuffer", surfaceFromBuffer);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

    zym_mapSet(vm, obj, "init",         init);
    zym_mapSet(vm, obj, "quit",         quit);
    zym_mapSet(vm, obj, "lastError",    lastError);
    zym_mapSet(vm, obj, "version",      version);
    zym_mapSet(vm, obj, "createWindow", createWindow);
    zym_mapSet(vm, obj, "pollEvent",    pollEvent);
    zym_mapSet(vm, obj, "waitEvent",    waitEvent);
    zym_mapSet(vm, obj, "pushEvent",    pushEvent);
    zym_mapSet(vm, obj, "keyDown",      keyDown);
    zym_mapSet(vm, obj, "mousePos",     mousePos);
    zym_mapSet(vm, obj, "mouseButtons", mouseButtons);
    zym_mapSet(vm, obj, "modifiers",    modifiers);
    zym_mapSet(vm, obj, "loadImage",          loadImage);
    zym_mapSet(vm, obj, "loadImageFromBuffer", loadImageFromBuffer);
    zym_mapSet(vm, obj, "saveImage",          saveImage);
    zym_mapSet(vm, obj, "saveImageToBuffer",  saveImageToBuffer);
    zym_mapSet(vm, obj, "Surface",      surfaceNs);

    // context + 18 closures + surfaceNs + obj = 21
    for (int i = 0; i < 21; i++) zym_popRoot(vm);

    return obj;
}
