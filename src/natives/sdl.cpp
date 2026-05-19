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

#undef MOD

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

    // context + 12 methods + obj = 14
    for (int i = 0; i < 14; i++) zym_popRoot(vm);

    return obj;
}
