// Dear ImGui native — Slice 1 PR 2a (minimum viable end-to-end).
//
// Scope per future/gui.md §1.3 split:
//   * ui.frame(win, body)           — per-frame begin/render/swap
//   * ui.window(name, body)         — Begin/End scoped via callback
//   * ui.button(label)              — returns clicked bool
//   * ui.text(s)                    — Text
//   * ui.lastError()                — string of last error stamped here
//
// Slice 1 PR 2b will add the rest of the widget surface (inputs,
// sliders, combos, tables, popups, menus, layout, ref convention,
// tree). PR 2c adds style stacks + themes + fonts + drawList.
//
// Compiled only when ZYM_UI_ENABLED is defined (see CMakeLists.txt).
// When disabled this TU is excluded and `cli_catalog.cpp` omits the
// `ui` row.

#include "natives.hpp"
#include "sdl_internal.hpp"  // WindowHandle + hooks

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <cfloat>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---- module-level state -------------------------------------------------
//
// ImGui's per-window context is created lazily on the first
// `ui.frame(win, ...)` targeting that window. The context owns the
// SDL3 + SDLRenderer3 backend state — both need init when the context
// is created and shutdown before SDL_DestroyRenderer / DestroyWindow.

std::string g_ui_lastError;

void setError(const char* msg) {
    g_ui_lastError = msg ? msg : "";
}

// Destroy the ImGui context attached to a WindowHandle, if any.
// Called from sdl.cpp's window finalizer / Window.free() via the
// `g_sdl_uiContextDestructor` hook so teardown happens BEFORE the
// SDL renderer/window go away.
void destroyUiContext(WindowHandle* w) {
    if (!w || !w->imguiContext) return;
    auto* ctx = static_cast<ImGuiContext*>(w->imguiContext);
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(ctx);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(ctx);
    // Restore the previous context (may be null), but never reinstate
    // the freed one.
    if (prev != ctx) ImGui::SetCurrentContext(prev);
    else             ImGui::SetCurrentContext(nullptr);
    w->imguiContext = nullptr;
}

// Forward every SDL event we see through to ImGui's SDL3 backend.
// The backend internally routes the event into the current ImGui
// context based on the event's windowID; if multiple windows have
// ImGui contexts we have to switch the current context per event.
void forwardEvent(const SDL_Event* e) {
    if (!e) return;
    // Locate the window the event is for (if any) so we can pick the
    // right ImGui context. Most events with a windowID expose it on
    // `e->window.windowID`, but key/mouse events have their own
    // windowID fields. SDL_GetWindowFromEvent handles all the cases.
    SDL_Window* w = nullptr;
    switch (e->type) {
        case SDL_EVENT_WINDOW_SHOWN:
        case SDL_EVENT_WINDOW_HIDDEN:
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_WINDOW_MOVED:
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_MOUSE_ENTER:
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            w = SDL_GetWindowFromID(e->window.windowID);
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        case SDL_EVENT_TEXT_INPUT:
        case SDL_EVENT_TEXT_EDITING:
            w = SDL_GetWindowFromID(e->key.windowID);
            break;
        case SDL_EVENT_MOUSE_MOTION:
            w = SDL_GetWindowFromID(e->motion.windowID);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            w = SDL_GetWindowFromID(e->button.windowID);
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            w = SDL_GetWindowFromID(e->wheel.windowID);
            break;
        default:
            break;
    }

    // ONLY forward when we can resolve the event's target window to a
    // WindowHandle that has a fully-initialised ImGui context+backend.
    //
    // The previous "broadcast to current context as a fallback" path
    // was unsafe in two ways:
    //   1. ImGui_ImplSDL3_ProcessEvent dereferences the SDL3 backend
    //      data (`bd`) without a runtime guard in Release (the
    //      IM_ASSERT is compiled out by -DNDEBUG), so calling it
    //      against a context whose backend isn't initialised for THIS
    //      event's window segfaults.
    //   2. Events for SDL-internal windows (display/system) or events
    //      whose windowID has already been destroyed have no business
    //      reaching ImGui regardless.
    //
    // The "global" events ImGui actually cares about (gamepad add/
    // remove, etc.) are all reachable through the per-window forward
    // path once ImGui_ImplSDL3 has registered them — we don't need a
    // broadcast.
    if (!w) return;
    auto props = SDL_GetWindowProperties(w);
    if (!props) return;
    void* p = SDL_GetPointerProperty(props, "zym.handle", nullptr);
    auto* wh = static_cast<WindowHandle*>(p);
    if (!wh || !wh->imguiContext) return;

    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(wh->imguiContext));
    ImGui_ImplSDL3_ProcessEvent(e);
    ImGui::SetCurrentContext(prev);
}

// ---- arg helpers ---------------------------------------------------------

bool reqStr(ZymVM* vm, ZymValue v, const char* where, std::string* out) {
    if (!zym_isString(v)) {
        zym_runtimeError(vm, "%s expects a string", where);
        return false;
    }
    *out = zym_asCString(v);
    return true;
}

bool reqCallable(ZymVM* vm, ZymValue v, const char* where) {
    if (zym_isClosure(v) || zym_isFunction(v)) return true;
    zym_runtimeError(vm, "%s expects a callback function", where);
    return false;
}

bool reqNum(ZymVM* vm, ZymValue v, const char* where, double* out) {
    if (!zym_isNumber(v)) { zym_runtimeError(vm, "%s expects a number", where); return false; }
    *out = zym_asNumber(v); return true;
}
bool reqInt(ZymVM* vm, ZymValue v, const char* where, int* out) {
    double d; if (!reqNum(vm, v, where, &d)) return false;
    *out = (int)d; return true;
}
bool reqBool(ZymVM* vm, ZymValue v, const char* where, bool* out) {
    if (!zym_isBool(v)) { zym_runtimeError(vm, "%s expects a bool", where); return false; }
    *out = zym_asBool(v); return true;
}

// Optional arg helpers — used by widgets with default values.
double optNum(ZymValue v, double fallback) {
    if (zym_isNumber(v)) return zym_asNumber(v);
    return fallback;
}
int optInt(ZymValue v, int fallback) {
    if (zym_isNumber(v)) return (int)zym_asNumber(v);
    return fallback;
}
// Unsigned-32 variant — used for packed colors (IM_COL32) which routinely
// exceed INT_MAX. Casting a double > INT_MAX directly to `int` is UB and
// in practice produces 0/INT_MIN on GCC, which makes drawList colors look
// frozen regardless of what the script passes. Round through uint32_t.
uint32_t optU32(ZymValue v, uint32_t fallback) {
    if (zym_isNumber(v)) {
        double d = zym_asNumber(v);
        if (d < 0.0) d = 0.0;
        if (d > 4294967295.0) d = 4294967295.0;
        return (uint32_t)d;
    }
    return fallback;
}
bool optBool(ZymValue v, bool fallback) {
    if (zym_isBool(v)) return zym_asBool(v);
    return fallback;
}
const char* optStr(ZymValue v, const char* fallback) {
    if (zym_isString(v)) return zym_asCString(v);
    return fallback;
}

// Single-element list "ref" helpers — script passes `[0]` / `[false]` /
// `[0.0]` / `["text"]` and the bridge reads/writes index 0 in place.
bool refReadInt(ZymVM* vm, ZymValue ref, const char* where, int* out) {
    if (!zym_isList(ref) || zym_listLength(ref) < 1) {
        zym_runtimeError(vm, "%s expects a single-element list ref like [0]", where);
        return false;
    }
    ZymValue v = zym_listGet(vm, ref, 0);
    if (!zym_isNumber(v)) {
        zym_runtimeError(vm, "%s ref must contain a number", where);
        return false;
    }
    *out = (int)zym_asNumber(v);
    return true;
}
bool refReadFloat(ZymVM* vm, ZymValue ref, const char* where, float* out) {
    if (!zym_isList(ref) || zym_listLength(ref) < 1) {
        zym_runtimeError(vm, "%s expects a single-element list ref like [0.0]", where);
        return false;
    }
    ZymValue v = zym_listGet(vm, ref, 0);
    if (!zym_isNumber(v)) {
        zym_runtimeError(vm, "%s ref must contain a number", where);
        return false;
    }
    *out = (float)zym_asNumber(v);
    return true;
}
bool refReadBool(ZymVM* vm, ZymValue ref, const char* where, bool* out) {
    if (!zym_isList(ref) || zym_listLength(ref) < 1) {
        zym_runtimeError(vm, "%s expects a single-element list ref like [false]", where);
        return false;
    }
    ZymValue v = zym_listGet(vm, ref, 0);
    if (!zym_isBool(v)) {
        zym_runtimeError(vm, "%s ref must contain a bool", where);
        return false;
    }
    *out = zym_asBool(v);
    return true;
}
bool refWriteInt(ZymVM* vm, ZymValue ref, int val) {
    return zym_listSet(vm, ref, 0, zym_newNumber((double)val));
}
bool refWriteFloat(ZymVM* vm, ZymValue ref, float val) {
    return zym_listSet(vm, ref, 0, zym_newNumber((double)val));
}
bool refWriteBool(ZymVM* vm, ZymValue ref, bool val) {
    return zym_listSet(vm, ref, 0, zym_newBool(val));
}

// Color ref — list of 3 or 4 floats in [r, g, b] / [r, g, b, a] form.
// Returns the count actually read (3 or 4).
int refReadColor(ZymVM* vm, ZymValue ref, const char* where, float out[4]) {
    if (!zym_isList(ref)) {
        zym_runtimeError(vm, "%s expects a color list ref [r,g,b] or [r,g,b,a]", where);
        return 0;
    }
    int n = zym_listLength(ref);
    if (n != 3 && n != 4) {
        zym_runtimeError(vm, "%s color ref must have 3 or 4 elements", where);
        return 0;
    }
    for (int i = 0; i < n; i++) {
        ZymValue v = zym_listGet(vm, ref, i);
        if (!zym_isNumber(v)) {
            zym_runtimeError(vm, "%s color ref element %d must be a number", where, i);
            return 0;
        }
        out[i] = (float)zym_asNumber(v);
    }
    if (n == 3) out[3] = 1.0f;
    return n;
}
void refWriteColor(ZymVM* vm, ZymValue ref, const float c[4], int count) {
    for (int i = 0; i < count; i++) {
        zym_listSet(vm, ref, i, zym_newNumber((double)c[i]));
    }
}

// Frame-context guard used by every widget — they all need an active
// ImGui frame, which `ui.frame(win, body)` is responsible for opening.
bool requireFrame(ZymVM* vm, const char* where) {
    if (!ImGui::GetCurrentContext()) {
        zym_runtimeError(vm, "%s: called outside ui.frame(...)", where);
        return false;
    }
    return true;
}

// ---- frame ---------------------------------------------------------------

// Lazily attach an ImGui context (with SDL3 + SDLRenderer3 backends)
// to a window. Returns the context or nullptr on failure (with
// g_ui_lastError set).
ImGuiContext* ensureWindowContext(WindowHandle* w) {
    if (w->imguiContext) return static_cast<ImGuiContext*>(w->imguiContext);
    if (!w->window || !w->renderer) {
        setError("ui: window has no renderer");
        return nullptr;
    }

    IMGUI_CHECKVERSION();
    ImGuiContext* ctx = ImGui::CreateContext();
    if (!ctx) { setError("ui: ImGui::CreateContext failed"); return nullptr; }

    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Disable INI persistence by default — scripts that want it can
    // opt back in later via `ui.setIniFilename(...)` (post-Slice-1).
    io.IniFilename = nullptr;

    if (!ImGui_ImplSDL3_InitForSDLRenderer(w->window, w->renderer)) {
        setError("ui: ImGui_ImplSDL3_InitForSDLRenderer failed");
        ImGui::DestroyContext(ctx);
        return nullptr;
    }
    if (!ImGui_ImplSDLRenderer3_Init(w->renderer)) {
        setError("ui: ImGui_ImplSDLRenderer3_Init failed");
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext(ctx);
        return nullptr;
    }

    w->imguiContext = ctx;
    return ctx;
}

// `ui.frame(win, body)` — begin a frame for `win`, invoke `body`, then
// render + present. The bridge owns the begin/end so scripts can never
// desync the pair (locked decision §1.0).
ZymValue u_frame(ZymVM* vm, ZymValue /*self*/, ZymValue winV, ZymValue bodyV) {
    WindowHandle* w = sdlGetWindowHandle(vm, winV);
    if (!w || !w->window || !w->renderer) {
        zym_runtimeError(vm, "ui.frame(win, body): invalid window handle");
        return ZYM_ERROR;
    }
    if (!reqCallable(vm, bodyV, "ui.frame(win, body)")) return ZYM_ERROR;

    ImGuiContext* ctx = ensureWindowContext(w);
    if (!ctx) {
        zym_runtimeError(vm, "ui.frame: %s", g_ui_lastError.c_str());
        return ZYM_ERROR;
    }
    ImGui::SetCurrentContext(ctx);

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    // Root the body closure across the re-entrant call: it lives in the
    // caller's argument slot, but re-entry into the VM can shuffle the
    // stack window in ways that leave the slot below the live GC root
    // set. An explicit temp root is the safe pattern (matches
    // `zym_native.cpp`'s callback paths).
    zym_pushRoot(vm, bodyV);
    ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
    zym_popRoot(vm);

    ImGui::Render();
    ImGuiIO& io = ImGui::GetIO();
    SDL_SetRenderScale(w->renderer, io.DisplayFramebufferScale.x,
                       io.DisplayFramebufferScale.y);
    // Clear to a neutral dark gray; scripts can call any `sdl.*`
    // drawing they want later (future slice) before we paint ImGui.
    SDL_SetRenderDrawColor(w->renderer, 30, 30, 35, 255);
    SDL_RenderClear(w->renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), w->renderer);
    SDL_RenderPresent(w->renderer);

    if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    return zym_newNull();
}

// ---- window scope --------------------------------------------------------

// `ui.window(name, body) -> bool` — wraps `Begin`/`End`. The bool
// matches ImGui's `Begin` return (window visible / not collapsed).
// `body` runs only when the bool is true.
ZymValue u_window(ZymVM* vm, ZymValue /*self*/, ZymValue nameV, ZymValue bodyV) {
    std::string name;
    if (!reqStr(vm, nameV, "ui.window(name, body)", &name)) return ZYM_ERROR;
    if (!reqCallable(vm, bodyV, "ui.window(name, body)")) return ZYM_ERROR;
    if (!ImGui::GetCurrentContext()) {
        zym_runtimeError(vm, "ui.window: called outside ui.frame(...)");
        return ZYM_ERROR;
    }

    bool open = ImGui::Begin(name.c_str(), nullptr, 0);
    if (open) {
        // Root the body closure across the re-entrant call (see
        // `u_frame` for the rationale).
        zym_pushRoot(vm, bodyV);
        ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
        zym_popRoot(vm);
        ImGui::End();
        if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    } else {
        // ImGui requires End() even when Begin() returns false.
        ImGui::End();
    }
    return zym_newBool(open);
}

// `ui.window(name, flags, body) -> bool` — overload accepting an
// ImGuiWindowFlags bitmask so scripts can request NoTitleBar/NoResize/
// NoMove/NoCollapse etc. to mold a window into a full-window root pane.
ZymValue u_windowFlags(ZymVM* vm, ZymValue /*self*/, ZymValue nameV,
                       ZymValue flagsV, ZymValue bodyV) {
    std::string name;
    if (!reqStr(vm, nameV, "ui.window(name, flags, body)", &name)) return ZYM_ERROR;
    int flags;
    if (!reqInt(vm, flagsV, "ui.window(name, flags, body)", &flags)) return ZYM_ERROR;
    if (!reqCallable(vm, bodyV, "ui.window(name, flags, body)")) return ZYM_ERROR;
    if (!ImGui::GetCurrentContext()) {
        zym_runtimeError(vm, "ui.window: called outside ui.frame(...)");
        return ZYM_ERROR;
    }

    bool open = ImGui::Begin(name.c_str(), nullptr, (ImGuiWindowFlags)flags);
    if (open) {
        zym_pushRoot(vm, bodyV);
        ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
        zym_popRoot(vm);
        ImGui::End();
        if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    } else {
        ImGui::End();
    }
    return zym_newBool(open);
}

// Positioning / sizing the next window — used to pin a window to a
// specific rectangle (e.g. the full SDL window for full-pane apps).
ZymValue u_setNextWindowPos(ZymVM* vm, ZymValue, ZymValue xV, ZymValue yV) {
    double x, y;
    if (!reqNum(vm, xV, "ui.setNextWindowPos", &x)) return ZYM_ERROR;
    if (!reqNum(vm, yV, "ui.setNextWindowPos", &y)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.setNextWindowPos")) return ZYM_ERROR;
    ImGui::SetNextWindowPos(ImVec2((float)x, (float)y));
    return zym_newNull();
}
ZymValue u_setNextWindowSize(ZymVM* vm, ZymValue, ZymValue wV, ZymValue hV) {
    double w, h;
    if (!reqNum(vm, wV, "ui.setNextWindowSize", &w)) return ZYM_ERROR;
    if (!reqNum(vm, hV, "ui.setNextWindowSize", &h)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.setNextWindowSize")) return ZYM_ERROR;
    ImGui::SetNextWindowSize(ImVec2((float)w, (float)h));
    return zym_newNull();
}

// ---- widgets: text -------------------------------------------------------

ZymValue u_text(ZymVM* vm, ZymValue, ZymValue sV) {
    std::string s; if (!reqStr(vm, sV, "ui.text(s)", &s)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.text")) return ZYM_ERROR;
    ImGui::TextUnformatted(s.c_str());
    return zym_newNull();
}
ZymValue u_textColored(ZymVM* vm, ZymValue, ZymValue colorV, ZymValue sV) {
    std::string s; if (!reqStr(vm, sV, "ui.textColored", &s)) return ZYM_ERROR;
    float c[4]; if (!refReadColor(vm, colorV, "ui.textColored", c)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.textColored")) return ZYM_ERROR;
    ImGui::TextColored(ImVec4(c[0],c[1],c[2],c[3]), "%s", s.c_str());
    return zym_newNull();
}
ZymValue u_textWrapped(ZymVM* vm, ZymValue, ZymValue sV) {
    std::string s; if (!reqStr(vm, sV, "ui.textWrapped", &s)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.textWrapped")) return ZYM_ERROR;
    ImGui::TextWrapped("%s", s.c_str()); return zym_newNull();
}
ZymValue u_textDisabled(ZymVM* vm, ZymValue, ZymValue sV) {
    std::string s; if (!reqStr(vm, sV, "ui.textDisabled", &s)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.textDisabled")) return ZYM_ERROR;
    ImGui::TextDisabled("%s", s.c_str()); return zym_newNull();
}
ZymValue u_labelText(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue valueV) {
    std::string label, value;
    if (!reqStr(vm, labelV, "ui.labelText", &label)) return ZYM_ERROR;
    if (!reqStr(vm, valueV, "ui.labelText", &value)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.labelText")) return ZYM_ERROR;
    ImGui::LabelText(label.c_str(), "%s", value.c_str());
    return zym_newNull();
}
ZymValue u_bulletText(ZymVM* vm, ZymValue, ZymValue sV) {
    std::string s; if (!reqStr(vm, sV, "ui.bulletText", &s)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.bulletText")) return ZYM_ERROR;
    ImGui::BulletText("%s", s.c_str()); return zym_newNull();
}
ZymValue u_bullet(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.bullet")) return ZYM_ERROR;
    ImGui::Bullet(); return zym_newNull();
}

// ---- widgets: buttons ----------------------------------------------------

ZymValue u_button(ZymVM* vm, ZymValue, ZymValue labelV) {
    std::string label; if (!reqStr(vm, labelV, "ui.button", &label)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.button")) return ZYM_ERROR;
    return zym_newBool(ImGui::Button(label.c_str()));
}
ZymValue u_buttonSized(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue wV, ZymValue hV) {
    std::string label; if (!reqStr(vm, labelV, "ui.button", &label)) return ZYM_ERROR;
    float w = (float)optNum(wV, 0.0), h = (float)optNum(hV, 0.0);
    if (!requireFrame(vm, "ui.button")) return ZYM_ERROR;
    return zym_newBool(ImGui::Button(label.c_str(), ImVec2(w, h)));
}
ZymValue u_smallButton(ZymVM* vm, ZymValue, ZymValue labelV) {
    std::string label; if (!reqStr(vm, labelV, "ui.smallButton", &label)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.smallButton")) return ZYM_ERROR;
    return zym_newBool(ImGui::SmallButton(label.c_str()));
}
ZymValue u_invisibleButton(ZymVM* vm, ZymValue, ZymValue idV, ZymValue wV, ZymValue hV) {
    std::string id; if (!reqStr(vm, idV, "ui.invisibleButton", &id)) return ZYM_ERROR;
    double dw, dh;
    if (!reqNum(vm, wV, "ui.invisibleButton", &dw)) return ZYM_ERROR;
    if (!reqNum(vm, hV, "ui.invisibleButton", &dh)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.invisibleButton")) return ZYM_ERROR;
    return zym_newBool(ImGui::InvisibleButton(id.c_str(), ImVec2((float)dw, (float)dh)));
}
ZymValue u_arrowButton(ZymVM* vm, ZymValue, ZymValue idV, ZymValue dirV) {
    std::string id; if (!reqStr(vm, idV, "ui.arrowButton", &id)) return ZYM_ERROR;
    int d; if (!reqInt(vm, dirV, "ui.arrowButton", &d)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.arrowButton")) return ZYM_ERROR;
    return zym_newBool(ImGui::ArrowButton(id.c_str(), (ImGuiDir)d));
}

// ---- widgets: toggles ----------------------------------------------------

ZymValue u_checkbox(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue refV) {
    std::string label; if (!reqStr(vm, labelV, "ui.checkbox", &label)) return ZYM_ERROR;
    bool b; if (!refReadBool(vm, refV, "ui.checkbox", &b)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.checkbox")) return ZYM_ERROR;
    bool changed = ImGui::Checkbox(label.c_str(), &b);
    if (changed) refWriteBool(vm, refV, b);
    return zym_newBool(changed);
}
ZymValue u_radioButton(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue activeV) {
    std::string label; if (!reqStr(vm, labelV, "ui.radioButton", &label)) return ZYM_ERROR;
    bool active; if (!reqBool(vm, activeV, "ui.radioButton", &active)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.radioButton")) return ZYM_ERROR;
    return zym_newBool(ImGui::RadioButton(label.c_str(), active));
}
ZymValue u_selectable(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue selV, ZymValue flagsV) {
    std::string label; if (!reqStr(vm, labelV, "ui.selectable", &label)) return ZYM_ERROR;
    bool sel = optBool(selV, false); int flags = optInt(flagsV, 0);
    if (!requireFrame(vm, "ui.selectable")) return ZYM_ERROR;
    return zym_newBool(ImGui::Selectable(label.c_str(), sel, flags));
}

// ---- layout flat calls ---------------------------------------------------

ZymValue u_sameLine(ZymVM* vm, ZymValue, ZymValue offsetV, ZymValue spacingV) {
    if (!requireFrame(vm, "ui.sameLine")) return ZYM_ERROR;
    ImGui::SameLine((float)optNum(offsetV, 0.0), (float)optNum(spacingV, -1.0));
    return zym_newNull();
}
ZymValue u_newLine(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.newLine")) return ZYM_ERROR;
    ImGui::NewLine(); return zym_newNull();
}
ZymValue u_separator(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.separator")) return ZYM_ERROR;
    ImGui::Separator(); return zym_newNull();
}
ZymValue u_spacing(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.spacing")) return ZYM_ERROR;
    ImGui::Spacing(); return zym_newNull();
}
ZymValue u_dummy(ZymVM* vm, ZymValue, ZymValue wV, ZymValue hV) {
    double w, h;
    if (!reqNum(vm, wV, "ui.dummy", &w)) return ZYM_ERROR;
    if (!reqNum(vm, hV, "ui.dummy", &h)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.dummy")) return ZYM_ERROR;
    ImGui::Dummy(ImVec2((float)w, (float)h)); return zym_newNull();
}
ZymValue u_indent(ZymVM* vm, ZymValue, ZymValue pxV) {
    if (!requireFrame(vm, "ui.indent")) return ZYM_ERROR;
    ImGui::Indent((float)optNum(pxV, 0.0)); return zym_newNull();
}
ZymValue u_unindent(ZymVM* vm, ZymValue, ZymValue pxV) {
    if (!requireFrame(vm, "ui.unindent")) return ZYM_ERROR;
    ImGui::Unindent((float)optNum(pxV, 0.0)); return zym_newNull();
}

// ---- state queries -------------------------------------------------------

ZymValue u_isItemHovered(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.isItemHovered")) return ZYM_ERROR;
    return zym_newBool(ImGui::IsItemHovered());
}
ZymValue u_isItemClicked(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.isItemClicked")) return ZYM_ERROR;
    return zym_newBool(ImGui::IsItemClicked());
}
ZymValue u_isItemActive(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.isItemActive")) return ZYM_ERROR;
    return zym_newBool(ImGui::IsItemActive());
}
ZymValue u_isItemFocused(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.isItemFocused")) return ZYM_ERROR;
    return zym_newBool(ImGui::IsItemFocused());
}
ZymValue u_isWindowHovered(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.isWindowHovered")) return ZYM_ERROR;
    return zym_newBool(ImGui::IsWindowHovered());
}
ZymValue u_isWindowFocused(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.isWindowFocused")) return ZYM_ERROR;
    return zym_newBool(ImGui::IsWindowFocused());
}
ZymValue u_wantCaptureMouse(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.wantCaptureMouse")) return ZYM_ERROR;
    return zym_newBool(ImGui::GetIO().WantCaptureMouse);
}
ZymValue u_wantCaptureKeyboard(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.wantCaptureKeyboard")) return ZYM_ERROR;
    return zym_newBool(ImGui::GetIO().WantCaptureKeyboard);
}

// ---- inline tooltip + progress -------------------------------------------

ZymValue u_tooltip(ZymVM* vm, ZymValue, ZymValue sV) {
    std::string s; if (!reqStr(vm, sV, "ui.tooltip", &s)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.tooltip")) return ZYM_ERROR;
    ImGui::SetTooltip("%s", s.c_str()); return zym_newNull();
}
ZymValue u_progressBar(ZymVM* vm, ZymValue, ZymValue fracV, ZymValue wV, ZymValue hV, ZymValue overlayV) {
    double f; if (!reqNum(vm, fracV, "ui.progressBar", &f)) return ZYM_ERROR;
    float w = (float)optNum(wV, -1.0f), h = (float)optNum(hV, 0.0f);
    const char* overlay = optStr(overlayV, nullptr);
    if (!requireFrame(vm, "ui.progressBar")) return ZYM_ERROR;
    ImGui::ProgressBar((float)f, ImVec2(w, h), overlay);
    return zym_newNull();
}
ZymValue u_collapsingHeader(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue flagsV) {
    std::string label; if (!reqStr(vm, labelV, "ui.collapsingHeader", &label)) return ZYM_ERROR;
    int flags = optInt(flagsV, 0);
    if (!requireFrame(vm, "ui.collapsingHeader")) return ZYM_ERROR;
    return zym_newBool(ImGui::CollapsingHeader(label.c_str(), flags));
}

// ---- widgets: inputs / sliders / drags / combo --------------------------

ZymValue u_inputInt(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue refV, ZymValue stepV) {
    std::string label; if (!reqStr(vm, labelV, "ui.inputInt", &label)) return ZYM_ERROR;
    int v; if (!refReadInt(vm, refV, "ui.inputInt", &v)) return ZYM_ERROR;
    int step = optInt(stepV, 1);
    if (!requireFrame(vm, "ui.inputInt")) return ZYM_ERROR;
    bool changed = ImGui::InputInt(label.c_str(), &v, step);
    if (changed) refWriteInt(vm, refV, v);
    return zym_newBool(changed);
}

ZymValue u_inputFloat(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue refV, ZymValue stepV, ZymValue fmtV) {
    std::string label; if (!reqStr(vm, labelV, "ui.inputFloat", &label)) return ZYM_ERROR;
    float v; if (!refReadFloat(vm, refV, "ui.inputFloat", &v)) return ZYM_ERROR;
    float step = (float)optNum(stepV, 0.0);
    const char* fmt = optStr(fmtV, "%.3f");
    if (!requireFrame(vm, "ui.inputFloat")) return ZYM_ERROR;
    bool changed = ImGui::InputFloat(label.c_str(), &v, step, 0.0f, fmt);
    if (changed) refWriteFloat(vm, refV, v);
    return zym_newBool(changed);
}

ZymValue u_sliderInt(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue refV, ZymValue minV, ZymValue maxV, ZymValue fmtV) {
    std::string label; if (!reqStr(vm, labelV, "ui.sliderInt", &label)) return ZYM_ERROR;
    int v; if (!refReadInt(vm, refV, "ui.sliderInt", &v)) return ZYM_ERROR;
    int mn, mx;
    if (!reqInt(vm, minV, "ui.sliderInt", &mn)) return ZYM_ERROR;
    if (!reqInt(vm, maxV, "ui.sliderInt", &mx)) return ZYM_ERROR;
    const char* fmt = optStr(fmtV, "%d");
    if (!requireFrame(vm, "ui.sliderInt")) return ZYM_ERROR;
    bool changed = ImGui::SliderInt(label.c_str(), &v, mn, mx, fmt);
    if (changed) refWriteInt(vm, refV, v);
    return zym_newBool(changed);
}

ZymValue u_sliderFloat(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue refV, ZymValue minV, ZymValue maxV, ZymValue fmtV) {
    std::string label; if (!reqStr(vm, labelV, "ui.sliderFloat", &label)) return ZYM_ERROR;
    float v; if (!refReadFloat(vm, refV, "ui.sliderFloat", &v)) return ZYM_ERROR;
    double mn, mx;
    if (!reqNum(vm, minV, "ui.sliderFloat", &mn)) return ZYM_ERROR;
    if (!reqNum(vm, maxV, "ui.sliderFloat", &mx)) return ZYM_ERROR;
    const char* fmt = optStr(fmtV, "%.3f");
    if (!requireFrame(vm, "ui.sliderFloat")) return ZYM_ERROR;
    bool changed = ImGui::SliderFloat(label.c_str(), &v, (float)mn, (float)mx, fmt);
    if (changed) refWriteFloat(vm, refV, v);
    return zym_newBool(changed);
}

ZymValue u_dragInt(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue refV, ZymValue speedV, ZymValue minV, ZymValue maxV) {
    std::string label; if (!reqStr(vm, labelV, "ui.dragInt", &label)) return ZYM_ERROR;
    int v; if (!refReadInt(vm, refV, "ui.dragInt", &v)) return ZYM_ERROR;
    float speed = (float)optNum(speedV, 1.0);
    int mn = optInt(minV, 0), mx = optInt(maxV, 0);
    if (!requireFrame(vm, "ui.dragInt")) return ZYM_ERROR;
    bool changed = ImGui::DragInt(label.c_str(), &v, speed, mn, mx);
    if (changed) refWriteInt(vm, refV, v);
    return zym_newBool(changed);
}

ZymValue u_dragFloat(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue refV, ZymValue speedV, ZymValue minV, ZymValue maxV) {
    std::string label; if (!reqStr(vm, labelV, "ui.dragFloat", &label)) return ZYM_ERROR;
    float v; if (!refReadFloat(vm, refV, "ui.dragFloat", &v)) return ZYM_ERROR;
    float speed = (float)optNum(speedV, 1.0);
    float mn = (float)optNum(minV, 0.0), mx = (float)optNum(maxV, 0.0);
    if (!requireFrame(vm, "ui.dragFloat")) return ZYM_ERROR;
    bool changed = ImGui::DragFloat(label.c_str(), &v, speed, mn, mx);
    if (changed) refWriteFloat(vm, refV, v);
    return zym_newBool(changed);
}

// inputText — Buffer-backed. Buffer holds raw bytes; we treat them as a
// NUL-terminated UTF-8 string for ImGui purposes and write back the
// edited contents on change.
// Helper: copy an edited ImGui input buffer back into the Zym Buffer
// **preserving the original allocated capacity**. ImGui's InputText
// edits a NUL-terminated C string in-place; the actual string length
// is usually much smaller than the buffer capacity. If we wrote back
// only the string length, the underlying PackedByteArray would shrink
// to that length, leaving no room for ImGui to edit further bytes on
// the next frame — which presents as "buffer keeps clearing after one
// character." Instead we write back the full capacity: the user's
// edited string followed by zero-padding up to the original size.
static void writeInputTextBack(ZymVM* vm, ZymValue bufV, const char* edited, size_t capacity) {
    if (capacity == 0) return;
    // Find the actual string length within capacity.
    size_t len = strnlen(edited, capacity);
    if (len > capacity) len = capacity; // defensive (shouldn't happen)
    if (len >= capacity) len = capacity - 1;
    // Build a capacity-sized buffer with the edited text then zero
    // padding, so the PBA stays at its original allocated size.
    std::vector<char> out(capacity, 0);
    if (len > 0 && len < capacity) memcpy(out.data(), edited, len);
    writeBufferBytes(vm, bufV, out.data(), capacity);
}

ZymValue u_inputText(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue bufV, ZymValue flagsV) {
    std::string label; if (!reqStr(vm, labelV, "ui.inputText", &label)) return ZYM_ERROR;
    const char* bytes = nullptr; size_t size = 0;
    if (!readBufferBytes(vm, bufV, &bytes, &size)) {
        zym_runtimeError(vm, "ui.inputText expects a Buffer");
        return ZYM_ERROR;
    }
    int flags = optInt(flagsV, 0);
    if (!requireFrame(vm, "ui.inputText")) return ZYM_ERROR;
    // Stage into a local growable buffer ImGui can edit in place. The
    // Buffer's full allocated size is the capacity — keep it intact
    // across edits so ImGui always has room to grow.
    if (size == 0) size = 1;
    std::vector<char> tmp(size);
    if (bytes) memcpy(tmp.data(), bytes, size);
    tmp[size - 1] = 0; // ensure NUL terminator
    bool changed = ImGui::InputText(label.c_str(), tmp.data(), tmp.size(), flags);
    if (changed) writeInputTextBack(vm, bufV, tmp.data(), size);
    return zym_newBool(changed);
}

ZymValue u_inputTextMultiline(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue bufV, ZymValue wV, ZymValue hV, ZymValue flagsV) {
    std::string label; if (!reqStr(vm, labelV, "ui.inputTextMultiline", &label)) return ZYM_ERROR;
    const char* bytes = nullptr; size_t size = 0;
    if (!readBufferBytes(vm, bufV, &bytes, &size)) {
        zym_runtimeError(vm, "ui.inputTextMultiline expects a Buffer");
        return ZYM_ERROR;
    }
    float w = (float)optNum(wV, 0.0), h = (float)optNum(hV, 0.0);
    int flags = optInt(flagsV, 0);
    if (!requireFrame(vm, "ui.inputTextMultiline")) return ZYM_ERROR;
    if (size == 0) size = 1;
    std::vector<char> tmp(size);
    if (bytes) memcpy(tmp.data(), bytes, size);
    tmp[size - 1] = 0;
    bool changed = ImGui::InputTextMultiline(label.c_str(), tmp.data(), tmp.size(),
                                             ImVec2(w, h), flags);
    if (changed) writeInputTextBack(vm, bufV, tmp.data(), size);
    return zym_newBool(changed);
}

// combo — items is a list of strings; currentIdx is single-element list ref.
ZymValue u_combo(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue idxRefV, ZymValue itemsV) {
    std::string label; if (!reqStr(vm, labelV, "ui.combo", &label)) return ZYM_ERROR;
    int cur; if (!refReadInt(vm, idxRefV, "ui.combo", &cur)) return ZYM_ERROR;
    if (!zym_isList(itemsV)) {
        zym_runtimeError(vm, "ui.combo: items must be a list of strings");
        return ZYM_ERROR;
    }
    if (!requireFrame(vm, "ui.combo")) return ZYM_ERROR;
    int n = zym_listLength(itemsV);
    // Collect strings and pointers.
    std::vector<std::string> store; store.reserve(n);
    std::vector<const char*> ptrs;  ptrs.reserve(n);
    for (int i = 0; i < n; i++) {
        ZymValue iv = zym_listGet(vm, itemsV, i);
        if (!zym_isString(iv)) {
            zym_runtimeError(vm, "ui.combo: item %d is not a string", i);
            return ZYM_ERROR;
        }
        store.emplace_back(zym_asCString(iv));
        ptrs.push_back(store.back().c_str());
    }
    bool changed = ImGui::Combo(label.c_str(), &cur,
                                ptrs.empty() ? nullptr : ptrs.data(), n);
    if (changed) refWriteInt(vm, idxRefV, cur);
    return zym_newBool(changed);
}

// ---- batch 3: scoped containers (callback-based) ------------------------
//
// Pattern: each call validates args, opens the corresponding ImGui
// region, optionally invokes the body closure, and always closes the
// region (when ImGui requires an unconditional End/Pop). The body
// closure is rooted across the re-entrant call exactly like u_window.

// `ui.child(id, w, h, border, body) -> bool`
// Wraps BeginChild/EndChild. The `border` arg is a bool (0/false for
// no border, true for ImGuiChildFlags_Border). For the simple form
// (id + body) callers can pass 0 width/height (= "stretch") and
// false border. We expose two arities below via dispatcher.
ZymValue u_child5(ZymVM* vm, ZymValue, ZymValue idV, ZymValue wV, ZymValue hV,
                  ZymValue borderV, ZymValue bodyV) {
    std::string id;
    if (!reqStr(vm, idV, "ui.child(id, w, h, border, body)", &id)) return ZYM_ERROR;
    if (!reqCallable(vm, bodyV, "ui.child(id, w, h, border, body)")) return ZYM_ERROR;
    float w = (float)optNum(wV, 0.0), h = (float)optNum(hV, 0.0);
    bool border = optBool(borderV, false);
    if (!requireFrame(vm, "ui.child")) return ZYM_ERROR;

    ImGuiChildFlags cflags = border ? ImGuiChildFlags_Borders : 0;
    bool open = ImGui::BeginChild(id.c_str(), ImVec2(w, h), cflags, 0);
    if (open) {
        zym_pushRoot(vm, bodyV);
        ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
        zym_popRoot(vm);
        ImGui::EndChild();
        if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    } else {
        ImGui::EndChild();
    }
    return zym_newBool(open);
}
// `ui.child(id, body)` shortcut.
ZymValue u_child2(ZymVM* vm, ZymValue self, ZymValue idV, ZymValue bodyV) {
    return u_child5(vm, self, idV, zym_newNumber(0.0), zym_newNumber(0.0),
                    zym_newBool(false), bodyV);
}
// `ui.child(id, opts, body)` — opts map with optional `w`, `h`, `border`.
ZymValue u_child3(ZymVM* vm, ZymValue self, ZymValue idV, ZymValue optsV,
                  ZymValue bodyV) {
    if (!zym_isMap(optsV)) {
        zym_runtimeError(vm,
            "ui.child(id, opts, body): opts must be a map");
        return ZYM_ERROR;
    }
    ZymValue wV      = zym_mapHas(optsV, "w")      ? zym_mapGet(vm, optsV, "w")      : zym_newNumber(0.0);
    ZymValue hV      = zym_mapHas(optsV, "h")      ? zym_mapGet(vm, optsV, "h")      : zym_newNumber(0.0);
    ZymValue borderV = zym_mapHas(optsV, "border") ? zym_mapGet(vm, optsV, "border") : zym_newBool(false);
    return u_child5(vm, self, idV, wV, hV, borderV, bodyV);
}

// `ui.group(body)` — BeginGroup/EndGroup (always paired, no bool).
ZymValue u_group(ZymVM* vm, ZymValue, ZymValue bodyV) {
    if (!reqCallable(vm, bodyV, "ui.group(body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.group")) return ZYM_ERROR;
    ImGui::BeginGroup();
    zym_pushRoot(vm, bodyV);
    ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
    zym_popRoot(vm);
    ImGui::EndGroup();
    if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    return zym_newNull();
}

// `ui.treeNode(label, body) -> bool` — TreeNode/TreePop. TreePop is
// only called when TreeNode returned true (ImGui requirement).
ZymValue u_treeNode(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue bodyV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.treeNode(label, body)", &label)) return ZYM_ERROR;
    if (!reqCallable(vm, bodyV, "ui.treeNode(label, body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.treeNode")) return ZYM_ERROR;
    bool open = ImGui::TreeNode(label.c_str());
    if (open) {
        zym_pushRoot(vm, bodyV);
        ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
        zym_popRoot(vm);
        ImGui::TreePop();
        if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    }
    return zym_newBool(open);
}

// `ui.disabled(cond, body)` — BeginDisabled/EndDisabled (always
// paired). `cond` is the disable flag (true → widgets in body are
// disabled).
ZymValue u_disabled(ZymVM* vm, ZymValue, ZymValue condV, ZymValue bodyV) {
    bool cond = optBool(condV, true);
    if (!reqCallable(vm, bodyV, "ui.disabled(cond, body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.disabled")) return ZYM_ERROR;
    ImGui::BeginDisabled(cond);
    zym_pushRoot(vm, bodyV);
    ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
    zym_popRoot(vm);
    ImGui::EndDisabled();
    if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    return zym_newNull();
}

// `ui.id(idValue, body)` — PushID/PopID. Accepts string or int id.
ZymValue u_id(ZymVM* vm, ZymValue, ZymValue idV, ZymValue bodyV) {
    if (!reqCallable(vm, bodyV, "ui.id(idValue, body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.id")) return ZYM_ERROR;
    if (zym_isString(idV)) {
        ImGui::PushID(zym_asCString(idV));
    } else if (zym_isNumber(idV)) {
        ImGui::PushID((int)zym_asNumber(idV));
    } else {
        zym_runtimeError(vm, "ui.id: idValue must be a string or number");
        return ZYM_ERROR;
    }
    zym_pushRoot(vm, bodyV);
    ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
    zym_popRoot(vm);
    ImGui::PopID();
    if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    return zym_newNull();
}

// `ui.clip(x, y, w, h, body)` — PushClipRect/PopClipRect on the
// current window's draw list. `intersect` defaults to true (intersect
// with parent clip rect rather than replace).
ZymValue u_clip(ZymVM* vm, ZymValue, ZymValue xV, ZymValue yV,
                ZymValue wV, ZymValue hV, ZymValue bodyV) {
    double x, y, w, h;
    if (!reqNum(vm, xV, "ui.clip", &x)) return ZYM_ERROR;
    if (!reqNum(vm, yV, "ui.clip", &y)) return ZYM_ERROR;
    if (!reqNum(vm, wV, "ui.clip", &w)) return ZYM_ERROR;
    if (!reqNum(vm, hV, "ui.clip", &h)) return ZYM_ERROR;
    if (!reqCallable(vm, bodyV, "ui.clip(x, y, w, h, body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.clip")) return ZYM_ERROR;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!dl) {
        zym_runtimeError(vm, "ui.clip: no current draw list");
        return ZYM_ERROR;
    }
    ImVec2 mn((float)x, (float)y);
    ImVec2 mx((float)(x + w), (float)(y + h));
    dl->PushClipRect(mn, mx, true);
    zym_pushRoot(vm, bodyV);
    ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
    zym_popRoot(vm);
    dl->PopClipRect();
    if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    return zym_newNull();
}

// `ui.tooltipScope(body)` — BeginTooltip/EndTooltip. Only opens
// the tooltip if ImGui's begin returns true; pairs the end regardless.
ZymValue u_tooltipScope(ZymVM* vm, ZymValue, ZymValue bodyV) {
    if (!reqCallable(vm, bodyV, "ui.tooltipScope(body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.tooltipScope")) return ZYM_ERROR;
    if (ImGui::BeginTooltip()) {
        zym_pushRoot(vm, bodyV);
        ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
        zym_popRoot(vm);
        ImGui::EndTooltip();
        if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    }
    return zym_newNull();
}

// ---- batch 4: tables ----------------------------------------------------
//
// ImGui tables are powerful and verbose. The Zym surface exposes the
// 80% case via a scoped callback wrapper (`ui.table`) plus the row /
// column / setup helpers as flat calls. The legacy columns API is
// also exposed as a flat helper for scripts that don't want the full
// table machinery.

// `ui.table(id, columns, body) -> bool`
// Wraps BeginTable/EndTable. `columns` is the column count (int >=1).
// EndTable is called only when BeginTable returned true (ImGui rule).
ZymValue u_table3(ZymVM* vm, ZymValue, ZymValue idV, ZymValue columnsV,
                  ZymValue bodyV) {
    std::string id;
    if (!reqStr(vm, idV, "ui.table(id, columns, body)", &id)) return ZYM_ERROR;
    int cols;
    if (!reqInt(vm, columnsV, "ui.table(id, columns, body)", &cols)) return ZYM_ERROR;
    if (cols < 1) {
        zym_runtimeError(vm, "ui.table: columns must be >= 1 (got %d)", cols);
        return ZYM_ERROR;
    }
    if (!reqCallable(vm, bodyV, "ui.table(id, columns, body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.table")) return ZYM_ERROR;

    bool open = ImGui::BeginTable(id.c_str(), cols, 0);
    if (open) {
        zym_pushRoot(vm, bodyV);
        ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
        zym_popRoot(vm);
        ImGui::EndTable();
        if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    }
    return zym_newBool(open);
}

// `ui.table(id, columns, flags, body) -> bool` — same with flags.
ZymValue u_table4(ZymVM* vm, ZymValue, ZymValue idV, ZymValue columnsV,
                  ZymValue flagsV, ZymValue bodyV) {
    std::string id;
    if (!reqStr(vm, idV, "ui.table(id, columns, flags, body)", &id)) return ZYM_ERROR;
    int cols;
    if (!reqInt(vm, columnsV, "ui.table(id, columns, flags, body)", &cols)) return ZYM_ERROR;
    if (cols < 1) {
        zym_runtimeError(vm, "ui.table: columns must be >= 1 (got %d)", cols);
        return ZYM_ERROR;
    }
    int flags = optInt(flagsV, 0);
    if (!reqCallable(vm, bodyV, "ui.table(id, columns, flags, body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.table")) return ZYM_ERROR;

    bool open = ImGui::BeginTable(id.c_str(), cols, (ImGuiTableFlags)flags);
    if (open) {
        zym_pushRoot(vm, bodyV);
        ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
        zym_popRoot(vm);
        ImGui::EndTable();
        if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    }
    return zym_newBool(open);
}

// `ui.tableNextRow()` — advance to the next row (no args form).
ZymValue u_tableNextRow0(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.tableNextRow")) return ZYM_ERROR;
    ImGui::TableNextRow();
    return zym_newNull();
}

// `ui.tableNextRow(minHeight)` — opt min row height.
ZymValue u_tableNextRow1(ZymVM* vm, ZymValue, ZymValue heightV) {
    if (!requireFrame(vm, "ui.tableNextRow")) return ZYM_ERROR;
    float h = (float)optNum(heightV, 0.0);
    ImGui::TableNextRow(0, h);
    return zym_newNull();
}

// `ui.tableNextColumn() -> bool` — move to next column; returns whether
// the column is visible (ImGui's clipper may skip drawing it).
ZymValue u_tableNextColumn(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.tableNextColumn")) return ZYM_ERROR;
    return zym_newBool(ImGui::TableNextColumn());
}

// `ui.tableSetColumnIndex(idx) -> bool` — jump directly to a column.
ZymValue u_tableSetColumnIndex(ZymVM* vm, ZymValue, ZymValue idxV) {
    int idx;
    if (!reqInt(vm, idxV, "ui.tableSetColumnIndex(idx)", &idx)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.tableSetColumnIndex")) return ZYM_ERROR;
    return zym_newBool(ImGui::TableSetColumnIndex(idx));
}

// `ui.tableSetupColumn(label)` / `(label, flags)` / `(label, flags, width)`.
ZymValue u_tableSetupColumn1(ZymVM* vm, ZymValue, ZymValue labelV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.tableSetupColumn(label)", &label)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.tableSetupColumn")) return ZYM_ERROR;
    ImGui::TableSetupColumn(label.c_str(), 0, 0.0f);
    return zym_newNull();
}
ZymValue u_tableSetupColumn2(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue flagsV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.tableSetupColumn(label, flags)", &label)) return ZYM_ERROR;
    int flags = optInt(flagsV, 0);
    if (!requireFrame(vm, "ui.tableSetupColumn")) return ZYM_ERROR;
    ImGui::TableSetupColumn(label.c_str(), (ImGuiTableColumnFlags)flags, 0.0f);
    return zym_newNull();
}
ZymValue u_tableSetupColumn3(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue flagsV,
                              ZymValue widthV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.tableSetupColumn(label, flags, width)", &label)) return ZYM_ERROR;
    int flags = optInt(flagsV, 0);
    float width = (float)optNum(widthV, 0.0);
    if (!requireFrame(vm, "ui.tableSetupColumn")) return ZYM_ERROR;
    ImGui::TableSetupColumn(label.c_str(), (ImGuiTableColumnFlags)flags, width);
    return zym_newNull();
}

// `ui.tableSetupScrollFreeze(cols, rows)` — freeze leading cols/rows.
ZymValue u_tableSetupScrollFreeze(ZymVM* vm, ZymValue, ZymValue colsV, ZymValue rowsV) {
    int cols, rows;
    if (!reqInt(vm, colsV, "ui.tableSetupScrollFreeze(cols, rows)", &cols)) return ZYM_ERROR;
    if (!reqInt(vm, rowsV, "ui.tableSetupScrollFreeze(cols, rows)", &rows)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.tableSetupScrollFreeze")) return ZYM_ERROR;
    ImGui::TableSetupScrollFreeze(cols, rows);
    return zym_newNull();
}

// `ui.tableHeadersRow()` — render a header row using the labels from
// tableSetupColumn calls.
ZymValue u_tableHeadersRow(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.tableHeadersRow")) return ZYM_ERROR;
    ImGui::TableHeadersRow();
    return zym_newNull();
}

// `ui.tableHeader(label)` — manual single header cell.
ZymValue u_tableHeader(ZymVM* vm, ZymValue, ZymValue labelV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.tableHeader(label)", &label)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.tableHeader")) return ZYM_ERROR;
    ImGui::TableHeader(label.c_str());
    return zym_newNull();
}

// `ui.tableGetRowIndex() -> int`, `ui.tableGetColumnIndex() -> int`,
// `ui.tableGetColumnCount() -> int`.
ZymValue u_tableGetRowIndex(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.tableGetRowIndex")) return ZYM_ERROR;
    return zym_newNumber((double)ImGui::TableGetRowIndex());
}
ZymValue u_tableGetColumnIndex(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.tableGetColumnIndex")) return ZYM_ERROR;
    return zym_newNumber((double)ImGui::TableGetColumnIndex());
}
ZymValue u_tableGetColumnCount(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.tableGetColumnCount")) return ZYM_ERROR;
    return zym_newNumber((double)ImGui::TableGetColumnCount());
}

// Legacy columns API — flat, no scoped pair in ImGui (Columns(0)
// implicitly ends the current set).
// `ui.columns(count)` / `(count, id)` / `(count, id, border)`.
ZymValue u_columns1(ZymVM* vm, ZymValue, ZymValue countV) {
    int count;
    if (!reqInt(vm, countV, "ui.columns(count)", &count)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.columns")) return ZYM_ERROR;
    ImGui::Columns(count);
    return zym_newNull();
}
ZymValue u_columns2(ZymVM* vm, ZymValue, ZymValue countV, ZymValue idV) {
    int count;
    if (!reqInt(vm, countV, "ui.columns(count, id)", &count)) return ZYM_ERROR;
    const char* id = optStr(idV, nullptr);
    if (!requireFrame(vm, "ui.columns")) return ZYM_ERROR;
    ImGui::Columns(count, id, true);
    return zym_newNull();
}
ZymValue u_columns3(ZymVM* vm, ZymValue, ZymValue countV, ZymValue idV,
                    ZymValue borderV) {
    int count;
    if (!reqInt(vm, countV, "ui.columns(count, id, border)", &count)) return ZYM_ERROR;
    const char* id = optStr(idV, nullptr);
    bool border = optBool(borderV, true);
    if (!requireFrame(vm, "ui.columns")) return ZYM_ERROR;
    ImGui::Columns(count, id, border);
    return zym_newNull();
}

// `ui.nextColumn()` — advance to next column in legacy Columns API.
ZymValue u_nextColumn(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.nextColumn")) return ZYM_ERROR;
    ImGui::NextColumn();
    return zym_newNull();
}

// ---- batch 5: popups / menus -------------------------------------------
//
// All begin/end pairs follow the locked scoped-callback rule: the
// bridge owns BeginX/EndX, EndX is only called when BeginX returned
// true (ImGui requirement for popups/menus), and the body closure is
// rooted across the re-entrant call.

// `ui.popup(id, body) -> bool` — BeginPopup/EndPopup.
ZymValue u_popup2(ZymVM* vm, ZymValue, ZymValue idV, ZymValue bodyV) {
    std::string id;
    if (!reqStr(vm, idV, "ui.popup(id, body)", &id)) return ZYM_ERROR;
    if (!reqCallable(vm, bodyV, "ui.popup(id, body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.popup")) return ZYM_ERROR;
    bool open = ImGui::BeginPopup(id.c_str(), 0);
    if (open) {
        zym_pushRoot(vm, bodyV);
        ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
        zym_popRoot(vm);
        ImGui::EndPopup();
        if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    }
    return zym_newBool(open);
}

// `ui.popup(id, flags, body) -> bool` — with flags.
ZymValue u_popup3(ZymVM* vm, ZymValue, ZymValue idV, ZymValue flagsV,
                  ZymValue bodyV) {
    std::string id;
    if (!reqStr(vm, idV, "ui.popup(id, flags, body)", &id)) return ZYM_ERROR;
    int flags = optInt(flagsV, 0);
    if (!reqCallable(vm, bodyV, "ui.popup(id, flags, body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.popup")) return ZYM_ERROR;
    bool open = ImGui::BeginPopup(id.c_str(), (ImGuiWindowFlags)flags);
    if (open) {
        zym_pushRoot(vm, bodyV);
        ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
        zym_popRoot(vm);
        ImGui::EndPopup();
        if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    }
    return zym_newBool(open);
}

// `ui.popupModal(name, body) -> bool` — BeginPopupModal/EndPopup.
ZymValue u_popupModal2(ZymVM* vm, ZymValue, ZymValue nameV, ZymValue bodyV) {
    std::string name;
    if (!reqStr(vm, nameV, "ui.popupModal(name, body)", &name)) return ZYM_ERROR;
    if (!reqCallable(vm, bodyV, "ui.popupModal(name, body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.popupModal")) return ZYM_ERROR;
    bool open = ImGui::BeginPopupModal(name.c_str(), nullptr, 0);
    if (open) {
        zym_pushRoot(vm, bodyV);
        ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
        zym_popRoot(vm);
        ImGui::EndPopup();
        if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    }
    return zym_newBool(open);
}

// `ui.popupModal(name, flags, body) -> bool` — with flags.
ZymValue u_popupModal3(ZymVM* vm, ZymValue, ZymValue nameV, ZymValue flagsV,
                       ZymValue bodyV) {
    std::string name;
    if (!reqStr(vm, nameV, "ui.popupModal(name, flags, body)", &name)) return ZYM_ERROR;
    int flags = optInt(flagsV, 0);
    if (!reqCallable(vm, bodyV, "ui.popupModal(name, flags, body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.popupModal")) return ZYM_ERROR;
    bool open = ImGui::BeginPopupModal(name.c_str(), nullptr,
                                       (ImGuiWindowFlags)flags);
    if (open) {
        zym_pushRoot(vm, bodyV);
        ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
        zym_popRoot(vm);
        ImGui::EndPopup();
        if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    }
    return zym_newBool(open);
}

// `ui.openPopup(id)` / `ui.openPopup(id, flags)` — request a popup to open
// on the next frame. Not a scoped pair (no End); just a flat request.
ZymValue u_openPopup1(ZymVM* vm, ZymValue, ZymValue idV) {
    std::string id;
    if (!reqStr(vm, idV, "ui.openPopup(id)", &id)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.openPopup")) return ZYM_ERROR;
    ImGui::OpenPopup(id.c_str(), 0);
    return zym_newNull();
}
ZymValue u_openPopup2(ZymVM* vm, ZymValue, ZymValue idV, ZymValue flagsV) {
    std::string id;
    if (!reqStr(vm, idV, "ui.openPopup(id, flags)", &id)) return ZYM_ERROR;
    int flags = optInt(flagsV, 0);
    if (!requireFrame(vm, "ui.openPopup")) return ZYM_ERROR;
    ImGui::OpenPopup(id.c_str(), (ImGuiPopupFlags)flags);
    return zym_newNull();
}

// `ui.closeCurrentPopup()` — close the currently-open popup from inside
// its body (typically after a MenuItem click).
ZymValue u_closeCurrentPopup(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.closeCurrentPopup")) return ZYM_ERROR;
    ImGui::CloseCurrentPopup();
    return zym_newNull();
}

// `ui.menuBar(body) -> bool` — per-window menu bar.
// Wraps BeginMenuBar/EndMenuBar. Body runs only if the window was
// created with the MenuBar flag *and* the begin returned true.
ZymValue u_menuBar(ZymVM* vm, ZymValue, ZymValue bodyV) {
    if (!reqCallable(vm, bodyV, "ui.menuBar(body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.menuBar")) return ZYM_ERROR;
    bool open = ImGui::BeginMenuBar();
    if (open) {
        zym_pushRoot(vm, bodyV);
        ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
        zym_popRoot(vm);
        ImGui::EndMenuBar();
        if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    }
    return zym_newBool(open);
}

// `ui.mainMenuBar(body) -> bool` — viewport-attached main menu bar.
ZymValue u_mainMenuBar(ZymVM* vm, ZymValue, ZymValue bodyV) {
    if (!reqCallable(vm, bodyV, "ui.mainMenuBar(body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.mainMenuBar")) return ZYM_ERROR;
    bool open = ImGui::BeginMainMenuBar();
    if (open) {
        zym_pushRoot(vm, bodyV);
        ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
        zym_popRoot(vm);
        ImGui::EndMainMenuBar();
        if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    }
    return zym_newBool(open);
}

// `ui.menu(label, body) -> bool` — drop-down menu inside a menu bar.
ZymValue u_menu2(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue bodyV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.menu(label, body)", &label)) return ZYM_ERROR;
    if (!reqCallable(vm, bodyV, "ui.menu(label, body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.menu")) return ZYM_ERROR;
    bool open = ImGui::BeginMenu(label.c_str(), true);
    if (open) {
        zym_pushRoot(vm, bodyV);
        ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
        zym_popRoot(vm);
        ImGui::EndMenu();
        if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    }
    return zym_newBool(open);
}

// `ui.menu(label, enabled, body) -> bool` — with enabled flag.
ZymValue u_menu3(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue enabledV,
                 ZymValue bodyV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.menu(label, enabled, body)", &label)) return ZYM_ERROR;
    bool enabled = optBool(enabledV, true);
    if (!reqCallable(vm, bodyV, "ui.menu(label, enabled, body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.menu")) return ZYM_ERROR;
    bool open = ImGui::BeginMenu(label.c_str(), enabled);
    if (open) {
        zym_pushRoot(vm, bodyV);
        ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
        zym_popRoot(vm);
        ImGui::EndMenu();
        if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    }
    return zym_newBool(open);
}

// `ui.menuItem(label) -> bool`
// `ui.menuItem(label, shortcut) -> bool`
// `ui.menuItem(label, shortcut, selected) -> bool`
// `ui.menuItem(label, shortcut, selected, enabled) -> bool`
// `selected` may be null/false or a single-element list ref `[bool]`
// (in which case the ref is mutated when the item is clicked).
ZymValue u_menuItem1(ZymVM* vm, ZymValue, ZymValue labelV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.menuItem(label)", &label)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.menuItem")) return ZYM_ERROR;
    return zym_newBool(ImGui::MenuItem(label.c_str(), nullptr, false, true));
}
ZymValue u_menuItem2(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue shortcutV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.menuItem(label, shortcut)", &label)) return ZYM_ERROR;
    const char* sc = optStr(shortcutV, nullptr);
    if (!requireFrame(vm, "ui.menuItem")) return ZYM_ERROR;
    return zym_newBool(ImGui::MenuItem(label.c_str(), sc, false, true));
}
ZymValue u_menuItem3(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue shortcutV,
                     ZymValue selectedV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.menuItem(label, shortcut, selected)", &label)) return ZYM_ERROR;
    const char* sc = optStr(shortcutV, nullptr);
    if (!requireFrame(vm, "ui.menuItem")) return ZYM_ERROR;
    if (zym_isList(selectedV)) {
        bool sel = false;
        if (!refReadBool(vm, selectedV,
                         "ui.menuItem(label, shortcut, selectedRef)", &sel)) {
            return ZYM_ERROR;
        }
        bool clicked = ImGui::MenuItem(label.c_str(), sc, &sel, true);
        if (clicked) refWriteBool(vm, selectedV, sel);
        return zym_newBool(clicked);
    }
    bool sel = optBool(selectedV, false);
    return zym_newBool(ImGui::MenuItem(label.c_str(), sc, sel, true));
}
ZymValue u_menuItem4(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue shortcutV,
                     ZymValue selectedV, ZymValue enabledV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.menuItem(label, shortcut, selected, enabled)", &label)) return ZYM_ERROR;
    const char* sc = optStr(shortcutV, nullptr);
    bool enabled = optBool(enabledV, true);
    if (!requireFrame(vm, "ui.menuItem")) return ZYM_ERROR;
    if (zym_isList(selectedV)) {
        bool sel = false;
        if (!refReadBool(vm, selectedV,
                         "ui.menuItem(label, shortcut, selectedRef, enabled)", &sel)) {
            return ZYM_ERROR;
        }
        bool clicked = ImGui::MenuItem(label.c_str(), sc, &sel, enabled);
        if (clicked) refWriteBool(vm, selectedV, sel);
        return zym_newBool(clicked);
    }
    bool sel = optBool(selectedV, false);
    return zym_newBool(ImGui::MenuItem(label.c_str(), sc, sel, enabled));
}

// ---- batch 6: plots / color / drawList / demo --------------------------

// Helper: read a list of numbers into a float vector.
static bool readFloatList(ZymVM* vm, ZymValue v, const char* where,
                          std::vector<float>* out) {
    if (!zym_isList(v)) {
        zym_runtimeError(vm, "%s expects a list of numbers", where);
        return false;
    }
    int n = zym_listLength(v);
    out->resize((size_t)n);
    for (int i = 0; i < n; i++) {
        ZymValue e = zym_listGet(vm, v, i);
        if (!zym_isNumber(e)) {
            zym_runtimeError(vm, "%s list element %d must be a number", where, i);
            return false;
        }
        (*out)[i] = (float)zym_asNumber(e);
    }
    return true;
}

// `ui.plotLines(label, values)` / `ui.plotLines(label, values, overlay)`
// / `ui.plotLines(label, values, overlay, scaleMin, scaleMax)`
ZymValue u_plotLines2(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue valsV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotLines(label, values)", &label)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.plotLines")) return ZYM_ERROR;
    std::vector<float> vs;
    if (!readFloatList(vm, valsV, "ui.plotLines(label, values)", &vs)) return ZYM_ERROR;
    ImGui::PlotLines(label.c_str(), vs.data(), (int)vs.size());
    return zym_newNull();
}
ZymValue u_plotLines3(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue valsV,
                     ZymValue overlayV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotLines(label, values, overlay)", &label)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.plotLines")) return ZYM_ERROR;
    std::vector<float> vs;
    if (!readFloatList(vm, valsV, "ui.plotLines(label, values, overlay)", &vs)) return ZYM_ERROR;
    const char* ov = optStr(overlayV, nullptr);
    ImGui::PlotLines(label.c_str(), vs.data(), (int)vs.size(), 0, ov);
    return zym_newNull();
}
ZymValue u_plotLines5(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue valsV,
                     ZymValue overlayV, ZymValue minV, ZymValue maxV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotLines(label, values, overlay, min, max)", &label)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.plotLines")) return ZYM_ERROR;
    std::vector<float> vs;
    if (!readFloatList(vm, valsV, "ui.plotLines(label, values, overlay, min, max)", &vs)) return ZYM_ERROR;
    const char* ov = optStr(overlayV, nullptr);
    float mn = (float)optNum(minV, FLT_MAX);
    float mx = (float)optNum(maxV, FLT_MAX);
    ImGui::PlotLines(label.c_str(), vs.data(), (int)vs.size(), 0, ov, mn, mx);
    return zym_newNull();
}

// `ui.plotHistogram(label, values)` / +overlay / +overlay+min+max
ZymValue u_plotHist2(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue valsV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotHistogram(label, values)", &label)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.plotHistogram")) return ZYM_ERROR;
    std::vector<float> vs;
    if (!readFloatList(vm, valsV, "ui.plotHistogram(label, values)", &vs)) return ZYM_ERROR;
    ImGui::PlotHistogram(label.c_str(), vs.data(), (int)vs.size());
    return zym_newNull();
}
ZymValue u_plotHist3(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue valsV,
                    ZymValue overlayV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotHistogram(label, values, overlay)", &label)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.plotHistogram")) return ZYM_ERROR;
    std::vector<float> vs;
    if (!readFloatList(vm, valsV, "ui.plotHistogram(label, values, overlay)", &vs)) return ZYM_ERROR;
    const char* ov = optStr(overlayV, nullptr);
    ImGui::PlotHistogram(label.c_str(), vs.data(), (int)vs.size(), 0, ov);
    return zym_newNull();
}
ZymValue u_plotHist5(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue valsV,
                    ZymValue overlayV, ZymValue minV, ZymValue maxV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotHistogram(label, values, overlay, min, max)", &label)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.plotHistogram")) return ZYM_ERROR;
    std::vector<float> vs;
    if (!readFloatList(vm, valsV, "ui.plotHistogram(label, values, overlay, min, max)", &vs)) return ZYM_ERROR;
    const char* ov = optStr(overlayV, nullptr);
    float mn = (float)optNum(minV, FLT_MAX);
    float mx = (float)optNum(maxV, FLT_MAX);
    ImGui::PlotHistogram(label.c_str(), vs.data(), (int)vs.size(), 0, ov, mn, mx);
    return zym_newNull();
}

// ---- color edit / picker ------------------------------------------------
//
// Color ref convention: `[r, g, b]` (3-elem) or `[r, g, b, a]` (4-elem).
// The bridge picks RGB vs RGBA based on the ref length, and the matching
// 3-vs-4 ImGui call. The ref is mutated in place when the user edits.

ZymValue u_colorEdit(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue refV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.colorEdit(label, ref)", &label)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.colorEdit")) return ZYM_ERROR;
    float c[4] = {0, 0, 0, 1};
    int n = refReadColor(vm, refV, "ui.colorEdit(label, ref)", c);
    if (n == 0) return ZYM_ERROR;
    bool changed = (n == 4)
        ? ImGui::ColorEdit4(label.c_str(), c)
        : ImGui::ColorEdit3(label.c_str(), c);
    if (changed) refWriteColor(vm, refV, c, n);
    return zym_newBool(changed);
}

ZymValue u_colorPicker(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue refV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.colorPicker(label, ref)", &label)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.colorPicker")) return ZYM_ERROR;
    float c[4] = {0, 0, 0, 1};
    int n = refReadColor(vm, refV, "ui.colorPicker(label, ref)", c);
    if (n == 0) return ZYM_ERROR;
    bool changed = (n == 4)
        ? ImGui::ColorPicker4(label.c_str(), c)
        : ImGui::ColorPicker3(label.c_str(), c);
    if (changed) refWriteColor(vm, refV, c, n);
    return zym_newBool(changed);
}

// `ui.colorButton(id, color)` — small swatch button.
ZymValue u_colorButton(ZymVM* vm, ZymValue, ZymValue idV, ZymValue refV) {
    std::string id;
    if (!reqStr(vm, idV, "ui.colorButton(id, color)", &id)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.colorButton")) return ZYM_ERROR;
    float c[4] = {0, 0, 0, 1};
    int n = refReadColor(vm, refV, "ui.colorButton(id, color)", c);
    if (n == 0) return ZYM_ERROR;
    return zym_newBool(ImGui::ColorButton(id.c_str(), ImVec4(c[0], c[1], c[2], c[3])));
}

// ---- DrawList primitives -----------------------------------------------
//
// Per the locked decision in §1.2, the draw list is part of ImGui (not
// SDL), so it lives in the `ui` native. Rather than expose a separate
// DrawList handle type, we expose flat `ui.draw*` helpers that operate
// on the current window's draw list. Scripts call them inside a
// `ui.window(...)` body to draw custom shapes/text into that window.
//
// Color parameters here are packed 32-bit ABGR ints (IM_COL32 order),
// matching ImDrawList's API. Scripts build them with `ui.color(r,g,b,a)`.

// Pack [0..255] r,g,b,a into IM_COL32 ABGR.
ZymValue u_color(ZymVM* vm, ZymValue, ZymValue rV, ZymValue gV, ZymValue bV,
                 ZymValue aV) {
    int r = optInt(rV, 0), g = optInt(gV, 0), b = optInt(bV, 0);
    int a = optInt(aV, 255);
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    if (a < 0) a = 0; if (a > 255) a = 255;
    ImU32 c = IM_COL32(r, g, b, a);
    return zym_newNumber((double)c);
    (void)vm;
}

static ImDrawList* curDL(ZymVM* vm, const char* where) {
    if (!requireFrame(vm, where)) return nullptr;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!dl) {
        zym_runtimeError(vm, "%s: no current draw list", where);
        return nullptr;
    }
    return dl;
}

ZymValue u_drawLine(ZymVM* vm, ZymValue, ZymValue x1V, ZymValue y1V,
                    ZymValue x2V, ZymValue y2V, ZymValue colV) {
    ImDrawList* dl = curDL(vm, "ui.drawLine");
    if (!dl) return ZYM_ERROR;
    dl->AddLine(ImVec2((float)optNum(x1V, 0), (float)optNum(y1V, 0)),
                ImVec2((float)optNum(x2V, 0), (float)optNum(y2V, 0)),
                optU32(colV, 0xFFFFFFFFu), 1.0f);
    return zym_newNull();
}

ZymValue u_drawRect(ZymVM* vm, ZymValue, ZymValue xV, ZymValue yV,
                    ZymValue wV, ZymValue hV, ZymValue colV) {
    ImDrawList* dl = curDL(vm, "ui.drawRect");
    if (!dl) return ZYM_ERROR;
    float x = (float)optNum(xV, 0), y = (float)optNum(yV, 0);
    float w = (float)optNum(wV, 0), h = (float)optNum(hV, 0);
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h),
                optU32(colV, 0xFFFFFFFFu));
    return zym_newNull();
}

ZymValue u_drawRectFilled(ZymVM* vm, ZymValue, ZymValue xV, ZymValue yV,
                          ZymValue wV, ZymValue hV, ZymValue colV) {
    ImDrawList* dl = curDL(vm, "ui.drawRectFilled");
    if (!dl) return ZYM_ERROR;
    float x = (float)optNum(xV, 0), y = (float)optNum(yV, 0);
    float w = (float)optNum(wV, 0), h = (float)optNum(hV, 0);
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h),
                      optU32(colV, 0xFFFFFFFFu));
    return zym_newNull();
}

ZymValue u_drawCircle(ZymVM* vm, ZymValue, ZymValue cxV, ZymValue cyV,
                      ZymValue rV, ZymValue colV) {
    ImDrawList* dl = curDL(vm, "ui.drawCircle");
    if (!dl) return ZYM_ERROR;
    dl->AddCircle(ImVec2((float)optNum(cxV, 0), (float)optNum(cyV, 0)),
                  (float)optNum(rV, 0), optU32(colV, 0xFFFFFFFFu));
    return zym_newNull();
}

ZymValue u_drawCircleFilled(ZymVM* vm, ZymValue, ZymValue cxV, ZymValue cyV,
                            ZymValue rV, ZymValue colV) {
    ImDrawList* dl = curDL(vm, "ui.drawCircleFilled");
    if (!dl) return ZYM_ERROR;
    dl->AddCircleFilled(ImVec2((float)optNum(cxV, 0), (float)optNum(cyV, 0)),
                        (float)optNum(rV, 0), optU32(colV, 0xFFFFFFFFu));
    return zym_newNull();
}

ZymValue u_drawText(ZymVM* vm, ZymValue, ZymValue xV, ZymValue yV,
                    ZymValue colV, ZymValue sV) {
    std::string s;
    if (!reqStr(vm, sV, "ui.drawText(x, y, color, s)", &s)) return ZYM_ERROR;
    ImDrawList* dl = curDL(vm, "ui.drawText");
    if (!dl) return ZYM_ERROR;
    dl->AddText(ImVec2((float)optNum(xV, 0), (float)optNum(yV, 0)),
                optU32(colV, 0xFFFFFFFFu), s.c_str());
    return zym_newNull();
}

ZymValue u_drawTriangle(ZymVM* vm, ZymValue,
                        ZymValue x1V, ZymValue y1V,
                        ZymValue x2V, ZymValue y2V,
                        ZymValue x3V, ZymValue y3V,
                        ZymValue colV) {
    ImDrawList* dl = curDL(vm, "ui.drawTriangle");
    if (!dl) return ZYM_ERROR;
    dl->AddTriangle(ImVec2((float)optNum(x1V, 0), (float)optNum(y1V, 0)),
                    ImVec2((float)optNum(x2V, 0), (float)optNum(y2V, 0)),
                    ImVec2((float)optNum(x3V, 0), (float)optNum(y3V, 0)),
                    optU32(colV, 0xFFFFFFFFu));
    return zym_newNull();
}

ZymValue u_drawTriangleFilled(ZymVM* vm, ZymValue,
                              ZymValue x1V, ZymValue y1V,
                              ZymValue x2V, ZymValue y2V,
                              ZymValue x3V, ZymValue y3V,
                              ZymValue colV) {
    ImDrawList* dl = curDL(vm, "ui.drawTriangleFilled");
    if (!dl) return ZYM_ERROR;
    dl->AddTriangleFilled(ImVec2((float)optNum(x1V, 0), (float)optNum(y1V, 0)),
                          ImVec2((float)optNum(x2V, 0), (float)optNum(y2V, 0)),
                          ImVec2((float)optNum(x3V, 0), (float)optNum(y3V, 0)),
                          optU32(colV, 0xFFFFFFFFu));
    return zym_newNull();
}

// `ui.getCursorPos() -> {x, y}` — current draw cursor in window coords.
ZymValue u_getCursorPos(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.getCursorPos")) return ZYM_ERROR;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ZymValue m = zym_newMap(vm);
    zym_pushRoot(vm, m);
    zym_mapSet(vm, m, "x", zym_newNumber((double)p.x));
    zym_mapSet(vm, m, "y", zym_newNumber((double)p.y));
    zym_popRoot(vm);
    return m;
}

// `ui.getMousePos() -> {x, y}` — ImGui's mouse position (screen coords).
ZymValue u_getMousePos(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.getMousePos")) return ZYM_ERROR;
    ImVec2 p = ImGui::GetMousePos();
    ZymValue m = zym_newMap(vm);
    zym_pushRoot(vm, m);
    zym_mapSet(vm, m, "x", zym_newNumber((double)p.x));
    zym_mapSet(vm, m, "y", zym_newNumber((double)p.y));
    zym_popRoot(vm);
    return m;
}

// `ui.framerate() -> number` — ImGui's smoothed FPS counter.
ZymValue u_framerate(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.framerate")) return ZYM_ERROR;
    return zym_newNumber((double)ImGui::GetIO().Framerate);
}


// ---- PR 2c: style stacks, fonts ----------------------------------------
//
// Scoped style/font wrappers: `withStyleColor(map, body)`,
// `withStyleVar(map, body)`, `withFont(font, body)`. Each pushes the
// requested state, invokes the body, then pops in reverse — matching
// ImGui's stack discipline. The bridge owns the push/pop so scripts
// can never desync.
//
// Fonts: `loadFont(path, sizePx, opts?) -> Font | null` and
// `defaultFont() -> Font`. A Font handle is a Zym map carrying a
// `__font__` native context whose userdata is the underlying
// `ImFont*`. Fonts live in ImGui's shared font atlas (per-context),
// so there's no per-handle teardown — the context's destructor
// already releases them when the owning window closes.
//
// Themes (`UI.themes.dark/light/classic` + `UI.applyTheme`) and
// `UI.raw.*` escape hatches are explicitly deferred from PR 2c.

// --- color slot name table (script string key -> ImGuiCol enum) -----
struct ColSlot { const char* name; ImGuiCol slot; };
const ColSlot kColSlots[] = {
    { "Text",                  ImGuiCol_Text },
    { "TextDisabled",          ImGuiCol_TextDisabled },
    { "WindowBg",              ImGuiCol_WindowBg },
    { "ChildBg",               ImGuiCol_ChildBg },
    { "PopupBg",               ImGuiCol_PopupBg },
    { "Border",                ImGuiCol_Border },
    { "BorderShadow",          ImGuiCol_BorderShadow },
    { "FrameBg",               ImGuiCol_FrameBg },
    { "FrameBgHovered",        ImGuiCol_FrameBgHovered },
    { "FrameBgActive",         ImGuiCol_FrameBgActive },
    { "TitleBg",               ImGuiCol_TitleBg },
    { "TitleBgActive",         ImGuiCol_TitleBgActive },
    { "TitleBgCollapsed",      ImGuiCol_TitleBgCollapsed },
    { "MenuBarBg",             ImGuiCol_MenuBarBg },
    { "ScrollbarBg",           ImGuiCol_ScrollbarBg },
    { "ScrollbarGrab",         ImGuiCol_ScrollbarGrab },
    { "ScrollbarGrabHovered",  ImGuiCol_ScrollbarGrabHovered },
    { "ScrollbarGrabActive",   ImGuiCol_ScrollbarGrabActive },
    { "CheckMark",             ImGuiCol_CheckMark },
    { "CheckboxSelectedBg",    ImGuiCol_CheckboxSelectedBg },
    { "SliderGrab",            ImGuiCol_SliderGrab },
    { "SliderGrabActive",      ImGuiCol_SliderGrabActive },
    { "Button",                ImGuiCol_Button },
    { "ButtonHovered",         ImGuiCol_ButtonHovered },
    { "ButtonActive",          ImGuiCol_ButtonActive },
    { "Header",                ImGuiCol_Header },
    { "HeaderHovered",         ImGuiCol_HeaderHovered },
    { "HeaderActive",          ImGuiCol_HeaderActive },
    { "Separator",             ImGuiCol_Separator },
    { "SeparatorHovered",      ImGuiCol_SeparatorHovered },
    { "SeparatorActive",       ImGuiCol_SeparatorActive },
    { "ResizeGrip",            ImGuiCol_ResizeGrip },
    { "ResizeGripHovered",     ImGuiCol_ResizeGripHovered },
    { "ResizeGripActive",      ImGuiCol_ResizeGripActive },
    { "InputTextCursor",       ImGuiCol_InputTextCursor },
    { "TabHovered",            ImGuiCol_TabHovered },
    { "Tab",                   ImGuiCol_Tab },
    { "TabSelected",           ImGuiCol_TabSelected },
    { "TabSelectedOverline",   ImGuiCol_TabSelectedOverline },
    { "TabDimmed",             ImGuiCol_TabDimmed },
    { "TabDimmedSelected",     ImGuiCol_TabDimmedSelected },
    { "TabDimmedSelectedOverline", ImGuiCol_TabDimmedSelectedOverline },
    { "PlotLines",             ImGuiCol_PlotLines },
    { "PlotLinesHovered",      ImGuiCol_PlotLinesHovered },
    { "PlotHistogram",         ImGuiCol_PlotHistogram },
    { "PlotHistogramHovered",  ImGuiCol_PlotHistogramHovered },
    { "TableHeaderBg",         ImGuiCol_TableHeaderBg },
    { "TableBorderStrong",     ImGuiCol_TableBorderStrong },
    { "TableBorderLight",      ImGuiCol_TableBorderLight },
    { "TableRowBg",            ImGuiCol_TableRowBg },
    { "TableRowBgAlt",         ImGuiCol_TableRowBgAlt },
    { "TextLink",              ImGuiCol_TextLink },
    { "TextSelectedBg",        ImGuiCol_TextSelectedBg },
    { "TreeLines",             ImGuiCol_TreeLines },
    { "DragDropTarget",        ImGuiCol_DragDropTarget },
    { "DragDropTargetBg",      ImGuiCol_DragDropTargetBg },
    { "UnsavedMarker",         ImGuiCol_UnsavedMarker },
    { "NavCursor",             ImGuiCol_NavCursor },
    { "NavWindowingHighlight", ImGuiCol_NavWindowingHighlight },
    { "NavWindowingDimBg",     ImGuiCol_NavWindowingDimBg },
    { "ModalWindowDimBg",      ImGuiCol_ModalWindowDimBg },
};
const size_t kColSlotsCount = sizeof(kColSlots) / sizeof(kColSlots[0]);

bool lookupColSlot(const char* key, ImGuiCol* out) {
    if (!key) return false;
    for (size_t i = 0; i < kColSlotsCount; i++) {
        if (std::strcmp(kColSlots[i].name, key) == 0) {
            *out = kColSlots[i].slot;
            return true;
        }
    }
    return false;
}

// --- style var name table -------------------------------------------
// kind: 1 = float scalar, 2 = ImVec2 pair.
struct VarSlot { const char* name; ImGuiStyleVar var; int kind; };
const VarSlot kVarSlots[] = {
    { "Alpha",                  ImGuiStyleVar_Alpha,                  1 },
    { "DisabledAlpha",          ImGuiStyleVar_DisabledAlpha,          1 },
    { "WindowPadding",          ImGuiStyleVar_WindowPadding,          2 },
    { "WindowRounding",         ImGuiStyleVar_WindowRounding,         1 },
    { "WindowBorderSize",       ImGuiStyleVar_WindowBorderSize,       1 },
    { "WindowMinSize",          ImGuiStyleVar_WindowMinSize,          2 },
    { "WindowTitleAlign",       ImGuiStyleVar_WindowTitleAlign,       2 },
    { "ChildRounding",          ImGuiStyleVar_ChildRounding,          1 },
    { "ChildBorderSize",        ImGuiStyleVar_ChildBorderSize,        1 },
    { "PopupRounding",          ImGuiStyleVar_PopupRounding,          1 },
    { "PopupBorderSize",        ImGuiStyleVar_PopupBorderSize,        1 },
    { "FramePadding",           ImGuiStyleVar_FramePadding,           2 },
    { "FrameRounding",          ImGuiStyleVar_FrameRounding,          1 },
    { "FrameBorderSize",        ImGuiStyleVar_FrameBorderSize,        1 },
    { "ItemSpacing",            ImGuiStyleVar_ItemSpacing,            2 },
    { "ItemInnerSpacing",       ImGuiStyleVar_ItemInnerSpacing,       2 },
    { "IndentSpacing",          ImGuiStyleVar_IndentSpacing,          1 },
    { "CellPadding",            ImGuiStyleVar_CellPadding,            2 },
    { "ScrollbarSize",          ImGuiStyleVar_ScrollbarSize,          1 },
    { "ScrollbarRounding",      ImGuiStyleVar_ScrollbarRounding,      1 },
    { "ScrollbarPadding",       ImGuiStyleVar_ScrollbarPadding,       1 },
    { "GrabMinSize",            ImGuiStyleVar_GrabMinSize,            1 },
    { "GrabRounding",           ImGuiStyleVar_GrabRounding,           1 },
    { "ImageRounding",          ImGuiStyleVar_ImageRounding,          1 },
    { "ImageBorderSize",        ImGuiStyleVar_ImageBorderSize,        1 },
    { "TabRounding",            ImGuiStyleVar_TabRounding,            1 },
    { "TabBorderSize",          ImGuiStyleVar_TabBorderSize,          1 },
    { "TabMinWidthBase",        ImGuiStyleVar_TabMinWidthBase,        1 },
    { "TabMinWidthShrink",      ImGuiStyleVar_TabMinWidthShrink,      1 },
    { "TabBarBorderSize",       ImGuiStyleVar_TabBarBorderSize,       1 },
    { "TabBarOverlineSize",     ImGuiStyleVar_TabBarOverlineSize,     1 },
    { "TableAngledHeadersAngle",ImGuiStyleVar_TableAngledHeadersAngle, 1 },
    { "TableAngledHeadersTextAlign", ImGuiStyleVar_TableAngledHeadersTextAlign, 2 },
    { "TreeLinesSize",          ImGuiStyleVar_TreeLinesSize,          1 },
    { "TreeLinesRounding",      ImGuiStyleVar_TreeLinesRounding,      1 },
    { "DragDropTargetRounding", ImGuiStyleVar_DragDropTargetRounding, 1 },
    { "ButtonTextAlign",        ImGuiStyleVar_ButtonTextAlign,        2 },
    { "SelectableTextAlign",    ImGuiStyleVar_SelectableTextAlign,    2 },
    { "SeparatorSize",          ImGuiStyleVar_SeparatorSize,          1 },
    { "SeparatorTextBorderSize",ImGuiStyleVar_SeparatorTextBorderSize,1 },
    { "SeparatorTextAlign",     ImGuiStyleVar_SeparatorTextAlign,     2 },
    { "SeparatorTextPadding",   ImGuiStyleVar_SeparatorTextPadding,   2 },
};
const size_t kVarSlotsCount = sizeof(kVarSlots) / sizeof(kVarSlots[0]);

bool lookupVarSlot(const char* key, ImGuiStyleVar* out, int* outKind) {
    if (!key) return false;
    for (size_t i = 0; i < kVarSlotsCount; i++) {
        if (std::strcmp(kVarSlots[i].name, key) == 0) {
            *out     = kVarSlots[i].var;
            *outKind = kVarSlots[i].kind;
            return true;
        }
    }
    return false;
}

// --- withStyleColor(map, body) --------------------------------------
//
// `map` maps slot-name string -> color list ([r,g,b] / [r,g,b,a]).
// We collect first, then push all, run body, pop in reverse.

struct StyleColorCollect {
    ZymVM* vm;
    bool   ok;
    std::vector<std::pair<ImGuiCol, ImVec4>> entries;
};

bool styleColorIter(ZymVM* vm, const char* key, ZymValue val, void* ud) {
    auto* c = static_cast<StyleColorCollect*>(ud);
    ImGuiCol slot;
    if (!lookupColSlot(key, &slot)) {
        zym_runtimeError(vm, "ui.withStyleColor: unknown color slot '%s'", key);
        c->ok = false;
        return false;
    }
    float rgba[4] = { 0, 0, 0, 1.0f };
    if (refReadColor(vm, val, "ui.withStyleColor", rgba) == 0) {
        c->ok = false;
        return false;
    }
    c->entries.emplace_back(slot, ImVec4(rgba[0], rgba[1], rgba[2], rgba[3]));
    return true;
}

ZymValue u_withStyleColor(ZymVM* vm, ZymValue, ZymValue mapV, ZymValue bodyV) {
    if (!zym_isMap(mapV)) {
        zym_runtimeError(vm, "ui.withStyleColor(map, body): map must be a map");
        return ZYM_ERROR;
    }
    if (!reqCallable(vm, bodyV, "ui.withStyleColor(map, body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.withStyleColor")) return ZYM_ERROR;

    StyleColorCollect c{ vm, true, {} };
    zym_mapForEach(vm, mapV, styleColorIter, &c);
    if (!c.ok) return ZYM_ERROR;

    for (auto& e : c.entries) ImGui::PushStyleColor(e.first, e.second);
    zym_pushRoot(vm, bodyV);
    ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
    zym_popRoot(vm);
    if (!c.entries.empty()) ImGui::PopStyleColor((int)c.entries.size());
    if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    return zym_newNull();
}

// --- withStyleVar(map, body) ----------------------------------------
//
// `map` maps var-name string -> number (scalar var) or [x, y] list
// (ImVec2 var). Mixing is fine — the table tells us which is which.

struct StyleVarCollect {
    ZymVM* vm;
    bool   ok;
    // Tracks push count for the matching pop after body.
    int    pushCount;
};

bool styleVarIter(ZymVM* vm, const char* key, ZymValue val, void* ud) {
    auto* c = static_cast<StyleVarCollect*>(ud);
    ImGuiStyleVar var; int kind;
    if (!lookupVarSlot(key, &var, &kind)) {
        zym_runtimeError(vm, "ui.withStyleVar: unknown style var '%s'", key);
        c->ok = false;
        return false;
    }
    if (kind == 1) {
        double d;
        if (!reqNum(vm, val, "ui.withStyleVar (scalar)", &d)) {
            c->ok = false;
            return false;
        }
        ImGui::PushStyleVar(var, (float)d);
        c->pushCount++;
    } else {
        if (!zym_isList(val) || zym_listLength(val) < 2) {
            zym_runtimeError(vm,
                "ui.withStyleVar: '%s' expects [x, y] list", key);
            c->ok = false;
            return false;
        }
        ZymValue xV = zym_listGet(vm, val, 0);
        ZymValue yV = zym_listGet(vm, val, 1);
        if (!zym_isNumber(xV) || !zym_isNumber(yV)) {
            zym_runtimeError(vm,
                "ui.withStyleVar: '%s' [x, y] elements must be numbers", key);
            c->ok = false;
            return false;
        }
        ImGui::PushStyleVar(var, ImVec2((float)zym_asNumber(xV),
                                        (float)zym_asNumber(yV)));
        c->pushCount++;
    }
    return true;
}

ZymValue u_withStyleVar(ZymVM* vm, ZymValue, ZymValue mapV, ZymValue bodyV) {
    if (!zym_isMap(mapV)) {
        zym_runtimeError(vm, "ui.withStyleVar(map, body): map must be a map");
        return ZYM_ERROR;
    }
    if (!reqCallable(vm, bodyV, "ui.withStyleVar(map, body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.withStyleVar")) return ZYM_ERROR;

    StyleVarCollect c{ vm, true, 0 };
    zym_mapForEach(vm, mapV, styleVarIter, &c);
    if (!c.ok) {
        // Pop anything we managed to push before failing so the stack
        // stays balanced.
        if (c.pushCount > 0) ImGui::PopStyleVar(c.pushCount);
        return ZYM_ERROR;
    }

    zym_pushRoot(vm, bodyV);
    ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
    zym_popRoot(vm);
    if (c.pushCount > 0) ImGui::PopStyleVar(c.pushCount);
    if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    return zym_newNull();
}

// --- Fonts ----------------------------------------------------------
//
// A Font value is a Zym map carrying a `__font__` native context. The
// userdata is the `ImFont*` pointer; the font itself is owned by the
// ImGui context's font atlas (created lazily on the first
// `UI.frame(...)` for that window). No per-handle finalizer.

ImFont* unwrapFont(ZymVM* vm, ZymValue v) {
    if (!zym_isMap(v)) return nullptr;
    if (!zym_mapHas(v, "__font__")) return nullptr;
    ZymValue ctx = zym_mapGet(vm, v, "__font__");
    return static_cast<ImFont*>(zym_getNativeData(ctx));
}

ZymValue makeFontInstance(ZymVM* vm, ImFont* font) {
    ZymValue ctx = zym_createNativeContext(vm, font, nullptr);
    zym_pushRoot(vm, ctx);
    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__font__", ctx);
    zym_popRoot(vm);
    zym_popRoot(vm);
    return obj;
}

// `readBufferBytes` (declared in natives.hpp) is the Buffer<->bytes
// bridge — it lets `UI.loadFont` accept either a filesystem path
// (string) or an in-memory font Buffer (TTF/OTF bytes loaded from a
// zpk, the network, an embedded asset, etc.) without pulling in the
// Godot `PackedByteArray` header.

// `UI.loadFont(pathOrBuffer, sizePx, opts?) -> Font | null`
//
// Loads a TTF/OTF font into the current ImGui context's atlas at the
// requested size. The first argument is either:
//   - a filesystem path (string): forwarded to AddFontFromFileTTF.
//   - a Buffer of font bytes: forwarded to AddFontFromMemoryTTF after
//     copying the bytes into an ImGui-owned allocation (the atlas
//     takes ownership and frees on shutdown). This lets scripts load
//     fonts that don't live on disk — e.g. unpacked from a `.zpk`,
//     downloaded over the network, or embedded as a string-literal
//     base64 blob decoded to a Buffer.
// Requires an active ImGui context — call from inside `UI.frame(...)`
// so a window has lazily created one, or after any other `UI.*` call
// has done so. On failure returns null and stamps `UI.lastError()`.
// `opts` is currently ignored (reserved for future glyph-range /
// merge / oversampling options).
ZymValue u_loadFont3(ZymVM* vm, ZymValue, ZymValue srcV, ZymValue sizeV,
                    ZymValue /*optsV*/) {
    double size;
    if (!reqNum(vm, sizeV, "ui.loadFont(src, sizePx)", &size)) return ZYM_ERROR;
    if (!ImGui::GetCurrentContext()) {
        setError("ui.loadFont: no active ImGui context (call inside ui.frame)");
        return zym_newNull();
    }
    ImGuiIO& io = ImGui::GetIO();
    if (!io.Fonts) {
        setError("ui.loadFont: no font atlas on this context");
        return zym_newNull();
    }

    ImFont* font = nullptr;
    if (zym_isString(srcV)) {
        const char* path = zym_asCString(srcV);
        font = io.Fonts->AddFontFromFileTTF(path, (float)size);
        if (!font) {
            setError("ui.loadFont: AddFontFromFileTTF failed");
            return zym_newNull();
        }
    } else {
        const char* bytes = nullptr;
        size_t nbytes = 0;
        if (!readBufferBytes(vm, srcV, &bytes, &nbytes)) {
            zym_runtimeError(vm,
                "ui.loadFont(src, sizePx): src must be a string path or a Buffer");
            return ZYM_ERROR;
        }
        if (!bytes || nbytes == 0) {
            setError("ui.loadFont: empty font Buffer");
            return zym_newNull();
        }
        // ImGui's AddFontFromMemoryTTF takes ownership of `font_data`
        // by default (frees via IM_FREE on atlas destruction). Copy
        // into an ImGui-owned allocation so the caller's Buffer stays
        // independently mutable / collectable.
        void* copy = IM_ALLOC(nbytes);
        memcpy(copy, bytes, nbytes);
        font = io.Fonts->AddFontFromMemoryTTF(copy, (int)nbytes, (float)size);
        if (!font) {
            // Atlas didn't accept it — free our copy and report.
            IM_FREE(copy);
            setError("ui.loadFont: AddFontFromMemoryTTF failed");
            return zym_newNull();
        }
    }
    return makeFontInstance(vm, font);
}

ZymValue u_loadFont2(ZymVM* vm, ZymValue self, ZymValue srcV, ZymValue sizeV) {
    return u_loadFont3(vm, self, srcV, sizeV, zym_newNull());
}

// `UI.defaultFont() -> Font` — wraps the ImGui context's first/default
// font (ProggyClean unless something else was loaded earlier).
ZymValue u_defaultFont(ZymVM* vm, ZymValue) {
    if (!ImGui::GetCurrentContext()) {
        zym_runtimeError(vm,
            "ui.defaultFont: no active ImGui context (call inside ui.frame)");
        return ZYM_ERROR;
    }
    ImGuiIO& io = ImGui::GetIO();
    ImFont* font = (io.Fonts && !io.Fonts->Fonts.empty())
        ? io.Fonts->Fonts[0] : ImGui::GetFont();
    if (!font) {
        zym_runtimeError(vm, "ui.defaultFont: no default font available");
        return ZYM_ERROR;
    }
    return makeFontInstance(vm, font);
}

// `UI.withFont(font, body)` — scoped PushFont/PopFont. Uses the font's
// LegacySize (the size passed to `UI.loadFont`) so scripts get the
// size they asked for. To layer a different size on the same font,
// pass `defaultFont()` and use the per-call ImGui sizing APIs in a
// later slice.
ZymValue u_withFont(ZymVM* vm, ZymValue, ZymValue fontV, ZymValue bodyV) {
    ImFont* font = unwrapFont(vm, fontV);
    if (!font) {
        zym_runtimeError(vm, "ui.withFont(font, body): invalid font handle");
        return ZYM_ERROR;
    }
    if (!reqCallable(vm, bodyV, "ui.withFont(font, body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.withFont")) return ZYM_ERROR;

    // PushFont(font, font->LegacySize) preserves the user-requested
    // size for fonts loaded via `UI.loadFont`. For the default font,
    // LegacySize matches its build size as well.
    ImGui::PushFont(font, font->LegacySize);
    zym_pushRoot(vm, bodyV);
    ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
    zym_popRoot(vm);
    ImGui::PopFont();
    if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    return zym_newNull();
}


// ---- PR 3: DrawList handle (full ImGui drawing capability) -------------
//
// A DrawList value is a Zym map carrying a `__drawlist__` native context
// whose userdata is an `ImDrawList*`. The draw list itself is owned by
// the ImGui context (per-window foreground / viewport background /
// foreground), so no per-handle finalizer is needed. Each instance map
// is populated with method closures that take the handle's ctx as
// `self` and forward to the underlying `ImDrawList*`.
//
// Image-touching APIs (AddImage, AddImageQuad, AddImageRounded,
// PathImageRect, PushTextureID) are intentionally NOT exposed — they
// will land alongside the SDL image / texture layer.

ImDrawList* unwrapDrawList(ZymValue ctx) {
    return static_cast<ImDrawList*>(zym_getNativeData(ctx));
}

bool reqDL(ZymVM* vm, ZymValue ctx, const char* where, ImDrawList** out) {
    auto* dl = unwrapDrawList(ctx);
    if (!dl) { zym_runtimeError(vm, "%s: invalid DrawList handle", where); return false; }
    *out = dl;
    return true;
}

// Read a list of points: `[[x1,y1],[x2,y2],...]` or flat `[x1,y1,x2,y2,...]`.
// Returns the number of points written into `out`, or -1 on type error.
int readPointList(ZymVM* vm, ZymValue v, std::vector<ImVec2>* out) {
    out->clear();
    if (!zym_isList(v)) return -1;
    int n = zym_listLength(v);
    if (n == 0) return 0;
    ZymValue first = zym_listGet(vm, v, 0);
    if (zym_isList(first)) {
        // nested form: [[x,y], ...]
        out->reserve(n);
        for (int i = 0; i < n; ++i) {
            ZymValue p = zym_listGet(vm, v, i);
            if (!zym_isList(p) || zym_listLength(p) < 2) return -1;
            ZymValue xv = zym_listGet(vm, p, 0);
            ZymValue yv = zym_listGet(vm, p, 1);
            if (!zym_isNumber(xv) || !zym_isNumber(yv)) return -1;
            out->push_back(ImVec2((float)zym_asNumber(xv), (float)zym_asNumber(yv)));
        }
        return (int)out->size();
    }
    // flat form: [x,y,x,y,...]
    if (n % 2 != 0) return -1;
    out->reserve(n / 2);
    for (int i = 0; i < n; i += 2) {
        ZymValue xv = zym_listGet(vm, v, i);
        ZymValue yv = zym_listGet(vm, v, i + 1);
        if (!zym_isNumber(xv) || !zym_isNumber(yv)) return -1;
        out->push_back(ImVec2((float)zym_asNumber(xv), (float)zym_asNumber(yv)));
    }
    return (int)out->size();
}

// ---- PR 2d: widget parity (broader ImGui surface) ----------------------
//
// Added in this batch:
//   * TabBar / TabItem scoped helpers
//   * ListBox (scoped + flat-items form)
//   * comboScope (BeginCombo/EndCombo, preview-string form)
//   * Vector slider/drag/input variants (Float2/3/4, Int2/3/4)
//   * SliderAngle, VSliderFloat, VSliderInt
//   * SeparatorText(label)
//   * TextLink / TextLinkOpenURL
//   * CheckboxFlags
//   * Scrolling helpers (getScrollX/Y, setScrollX/Y, getScrollMaxX/Y,
//     setScrollHereX/Y, setScrollFromPosX/Y)
//   * Window state queries (isWindowAppearing/Collapsed,
//     getWindowPos/Size/Width/Height)
//   * Item queries (isItemVisible/Edited/Activated/Deactivated/
//     DeactivatedAfterEdit/Toggled, getItemRectMin/Max/Size,
//     isAnyItemHovered/Active/Focused)
//   * Mouse queries (isMouseDown/Clicked/DoubleClicked/Released/Dragging,
//     getMouseDragDelta, resetMouseDragDelta, getMouseClickedCount)
//   * Keyboard queries (isKeyDown/Pressed/Released, getKeyPressedAmount,
//     setNextFrameWantCaptureKeyboard/Mouse)
//   * Clipboard helpers (getClipboardText / setClipboardText)
//   * Context popup helpers (popupContextItem / popupContextWindow)
//   * SetNextItem* helpers + PushItemWidth/PopItemWidth + CalcTextSize
//   * SetNextWindow* helpers (Focus, BgAlpha, ContentSize, Collapsed, Scroll)
//   * Style getters (getStyleColorVec4, getColorU32, getFontSize)

// Pack a 2-element ImVec2 / 3-element / 4-element float vector into a
// ZymValue list. Helper for returning rects, sizes, deltas.
static ZymValue makeXY(ZymVM* vm, float x, float y) {
    ZymValue m = zym_newMap(vm);
    zym_pushRoot(vm, m);
    zym_mapSet(vm, m, "x", zym_newNumber((double)x));
    zym_mapSet(vm, m, "y", zym_newNumber((double)y));
    zym_popRoot(vm);
    return m;
}

// Read N floats from a list reference into out[N]. Returns false on
// length mismatch / non-number entries (sets a runtime error).
static bool refReadFloatN(ZymVM* vm, ZymValue ref, int n, const char* where,
                          float out[]) {
    if (!zym_isList(ref) || zym_listLength(ref) != n) {
        zym_runtimeError(vm, "%s: ref must be a list of %d numbers", where, n);
        return false;
    }
    for (int i = 0; i < n; i++) {
        ZymValue v = zym_listGet(vm, ref, i);
        if (!zym_isNumber(v)) {
            zym_runtimeError(vm, "%s: ref[%d] is not a number", where, i);
            return false;
        }
        out[i] = (float)zym_asNumber(v);
    }
    return true;
}

// Read N ints from a list reference into out[N].
static bool refReadIntN(ZymVM* vm, ZymValue ref, int n, const char* where,
                        int out[]) {
    if (!zym_isList(ref) || zym_listLength(ref) != n) {
        zym_runtimeError(vm, "%s: ref must be a list of %d ints", where, n);
        return false;
    }
    for (int i = 0; i < n; i++) {
        ZymValue v = zym_listGet(vm, ref, i);
        if (!zym_isNumber(v)) {
            zym_runtimeError(vm, "%s: ref[%d] is not a number", where, i);
            return false;
        }
        out[i] = (int)zym_asNumber(v);
    }
    return true;
}

// Write N floats back to a list reference.
static void refWriteFloatN(ZymVM* vm, ZymValue ref, int n, const float v[]) {
    for (int i = 0; i < n; i++) {
        zym_listSet(vm, ref, i, zym_newNumber((double)v[i]));
    }
}

// Write N ints back to a list reference.
static void refWriteIntN(ZymVM* vm, ZymValue ref, int n, const int v[]) {
    for (int i = 0; i < n; i++) {
        zym_listSet(vm, ref, i, zym_newNumber((double)v[i]));
    }
}

// Invoke a callable body once. Used by every scoped helper added in
// this batch. Returns true on success, false if the body raised.
static bool invokeBody(ZymVM* vm, ZymValue bodyV) {
    zym_pushRoot(vm, bodyV);
    ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
    zym_popRoot(vm);
    return st == ZYM_STATUS_OK;
}

// ---- 2d.a: scoped tab bar / tab item ----
ZymValue u_tabBar(ZymVM* vm, ZymValue, ZymValue idV, ZymValue bodyV) {
    std::string id;
    if (!reqStr(vm, idV, "ui.tabBar(id, body)", &id)) return ZYM_ERROR;
    if (!reqCallable(vm, bodyV, "ui.tabBar(id, body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.tabBar")) return ZYM_ERROR;
    bool open = ImGui::BeginTabBar(id.c_str());
    if (open) {
        bool ok = invokeBody(vm, bodyV);
        ImGui::EndTabBar();
        if (!ok) return ZYM_ERROR;
    }
    return zym_newBool(open);
}

ZymValue u_tabItem(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue bodyV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.tabItem(label, body)", &label)) return ZYM_ERROR;
    if (!reqCallable(vm, bodyV, "ui.tabItem(label, body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.tabItem")) return ZYM_ERROR;
    bool open = ImGui::BeginTabItem(label.c_str(), nullptr, 0);
    if (open) {
        bool ok = invokeBody(vm, bodyV);
        ImGui::EndTabItem();
        if (!ok) return ZYM_ERROR;
    }
    return zym_newBool(open);
}

ZymValue u_tabItemButton(ZymVM* vm, ZymValue, ZymValue labelV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.tabItemButton(label)", &label)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.tabItemButton")) return ZYM_ERROR;
    return zym_newBool(ImGui::TabItemButton(label.c_str(), 0));
}

// ---- 2d.b: list box ----
ZymValue u_listBox(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue bodyV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.listBox(label, body)", &label)) return ZYM_ERROR;
    if (!reqCallable(vm, bodyV, "ui.listBox(label, body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.listBox")) return ZYM_ERROR;
    bool open = ImGui::BeginListBox(label.c_str());
    if (open) {
        bool ok = invokeBody(vm, bodyV);
        ImGui::EndListBox();
        if (!ok) return ZYM_ERROR;
    }
    return zym_newBool(open);
}

// flat listBox(label, idxRef, items)
ZymValue u_listBoxFlat(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue idxRefV,
                       ZymValue itemsV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.listBox", &label)) return ZYM_ERROR;
    int cur;
    if (!refReadInt(vm, idxRefV, "ui.listBox", &cur)) return ZYM_ERROR;
    if (!zym_isList(itemsV)) {
        zym_runtimeError(vm, "ui.listBox: items must be a list of strings");
        return ZYM_ERROR;
    }
    if (!requireFrame(vm, "ui.listBox")) return ZYM_ERROR;
    int n = zym_listLength(itemsV);
    std::vector<std::string> store; store.reserve(n);
    std::vector<const char*> ptrs;  ptrs.reserve(n);
    for (int i = 0; i < n; i++) {
        ZymValue iv = zym_listGet(vm, itemsV, i);
        if (!zym_isString(iv)) {
            zym_runtimeError(vm, "ui.listBox: item %d is not a string", i);
            return ZYM_ERROR;
        }
        store.emplace_back(zym_asCString(iv));
        ptrs.push_back(store.back().c_str());
    }
    bool changed = ImGui::ListBox(label.c_str(), &cur,
                                  ptrs.empty() ? nullptr : ptrs.data(), n, -1);
    if (changed) refWriteInt(vm, idxRefV, cur);
    return zym_newBool(changed);
}

// ---- 2d.c: scoped combo (BeginCombo/EndCombo) ----
ZymValue u_comboScope(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue previewV,
                     ZymValue bodyV) {
    std::string label, preview;
    if (!reqStr(vm, labelV,  "ui.comboScope(label, preview, body)", &label))   return ZYM_ERROR;
    if (!reqStr(vm, previewV,"ui.comboScope(label, preview, body)", &preview)) return ZYM_ERROR;
    if (!reqCallable(vm, bodyV,"ui.comboScope(label, preview, body)"))         return ZYM_ERROR;
    if (!requireFrame(vm, "ui.comboScope")) return ZYM_ERROR;
    bool open = ImGui::BeginCombo(label.c_str(), preview.c_str(), 0);
    if (open) {
        bool ok = invokeBody(vm, bodyV);
        ImGui::EndCombo();
        if (!ok) return ZYM_ERROR;
    }
    return zym_newBool(open);
}

// ---- 2d.d: separatorText / textLink ----
ZymValue u_separatorText(ZymVM* vm, ZymValue, ZymValue sV) {
    std::string s; if (!reqStr(vm, sV, "ui.separatorText(s)", &s)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.separatorText")) return ZYM_ERROR;
    ImGui::SeparatorText(s.c_str());
    return zym_newNull();
}

ZymValue u_textLink(ZymVM* vm, ZymValue, ZymValue sV) {
    std::string s; if (!reqStr(vm, sV, "ui.textLink(s)", &s)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.textLink")) return ZYM_ERROR;
    return zym_newBool(ImGui::TextLink(s.c_str()));
}

ZymValue u_textLinkOpenURL(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue urlV) {
    std::string label, url;
    if (!reqStr(vm, labelV, "ui.textLinkOpenURL(label, url)", &label)) return ZYM_ERROR;
    if (!reqStr(vm, urlV,   "ui.textLinkOpenURL(label, url)", &url))   return ZYM_ERROR;
    if (!requireFrame(vm, "ui.textLinkOpenURL")) return ZYM_ERROR;
    ImGui::TextLinkOpenURL(label.c_str(), url.c_str());
    return zym_newNull();
}

// ---- 2d.e: checkboxFlags(label, ref, flagsValue) ----
ZymValue u_checkboxFlags(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue refV,
                         ZymValue flagsV) {
    std::string label; if (!reqStr(vm, labelV, "ui.checkboxFlags", &label)) return ZYM_ERROR;
    int v;             if (!refReadInt(vm, refV, "ui.checkboxFlags", &v))   return ZYM_ERROR;
    int flagsBit;      if (!reqInt(vm, flagsV, "ui.checkboxFlags", &flagsBit)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.checkboxFlags")) return ZYM_ERROR;
    bool changed = ImGui::CheckboxFlags(label.c_str(), &v, flagsBit);
    if (changed) refWriteInt(vm, refV, v);
    return zym_newBool(changed);
}

// ---- 2d.f: vector slider / drag / input ----
#define VEC_RW_FLOAT(NAME, N, IMCALL)                                                  \
ZymValue u_##NAME(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue refV,                 \
                  ZymValue minV, ZymValue maxV) {                                      \
    std::string label; if (!reqStr(vm, labelV, "ui." #NAME, &label)) return ZYM_ERROR; \
    float v[N];        if (!refReadFloatN(vm, refV, N, "ui." #NAME, v)) return ZYM_ERROR; \
    double mn = optNum(minV, 0.0), mx = optNum(maxV, 1.0);                             \
    if (!requireFrame(vm, "ui." #NAME)) return ZYM_ERROR;                              \
    bool changed = ImGui::IMCALL(label.c_str(), v, (float)mn, (float)mx, "%.3f", 0);   \
    if (changed) refWriteFloatN(vm, refV, N, v);                                       \
    return zym_newBool(changed);                                                       \
}

VEC_RW_FLOAT(sliderFloat2, 2, SliderFloat2)
VEC_RW_FLOAT(sliderFloat3, 3, SliderFloat3)
VEC_RW_FLOAT(sliderFloat4, 4, SliderFloat4)

#define VEC_RW_INT(NAME, N, IMCALL)                                                    \
ZymValue u_##NAME(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue refV,                 \
                  ZymValue minV, ZymValue maxV) {                                      \
    std::string label; if (!reqStr(vm, labelV, "ui." #NAME, &label)) return ZYM_ERROR; \
    int v[N];          if (!refReadIntN(vm, refV, N, "ui." #NAME, v)) return ZYM_ERROR; \
    int mn = optInt(minV, 0), mx = optInt(maxV, 100);                                  \
    if (!requireFrame(vm, "ui." #NAME)) return ZYM_ERROR;                              \
    bool changed = ImGui::IMCALL(label.c_str(), v, mn, mx, "%d", 0);                   \
    if (changed) refWriteIntN(vm, refV, N, v);                                         \
    return zym_newBool(changed);                                                       \
}

VEC_RW_INT(sliderInt2, 2, SliderInt2)
VEC_RW_INT(sliderInt3, 3, SliderInt3)
VEC_RW_INT(sliderInt4, 4, SliderInt4)

#define VEC_DRAG_FLOAT(NAME, N, IMCALL)                                                \
ZymValue u_##NAME(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue refV,                 \
                  ZymValue speedV, ZymValue minV, ZymValue maxV) {                     \
    std::string label; if (!reqStr(vm, labelV, "ui." #NAME, &label)) return ZYM_ERROR; \
    float v[N];        if (!refReadFloatN(vm, refV, N, "ui." #NAME, v)) return ZYM_ERROR; \
    double sp = optNum(speedV, 1.0), mn = optNum(minV, 0.0), mx = optNum(maxV, 0.0);   \
    if (!requireFrame(vm, "ui." #NAME)) return ZYM_ERROR;                              \
    bool changed = ImGui::IMCALL(label.c_str(), v, (float)sp, (float)mn, (float)mx,    \
                                 "%.3f", 0);                                           \
    if (changed) refWriteFloatN(vm, refV, N, v);                                       \
    return zym_newBool(changed);                                                       \
}

VEC_DRAG_FLOAT(dragFloat2, 2, DragFloat2)
VEC_DRAG_FLOAT(dragFloat3, 3, DragFloat3)
VEC_DRAG_FLOAT(dragFloat4, 4, DragFloat4)

#define VEC_DRAG_INT(NAME, N, IMCALL)                                                  \
ZymValue u_##NAME(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue refV,                 \
                  ZymValue speedV, ZymValue minV, ZymValue maxV) {                     \
    std::string label; if (!reqStr(vm, labelV, "ui." #NAME, &label)) return ZYM_ERROR; \
    int v[N];          if (!refReadIntN(vm, refV, N, "ui." #NAME, v)) return ZYM_ERROR; \
    double sp = optNum(speedV, 1.0);                                                   \
    int mn = optInt(minV, 0), mx = optInt(maxV, 0);                                    \
    if (!requireFrame(vm, "ui." #NAME)) return ZYM_ERROR;                              \
    bool changed = ImGui::IMCALL(label.c_str(), v, (float)sp, mn, mx, "%d", 0);        \
    if (changed) refWriteIntN(vm, refV, N, v);                                         \
    return zym_newBool(changed);                                                       \
}

VEC_DRAG_INT(dragInt2, 2, DragInt2)
VEC_DRAG_INT(dragInt3, 3, DragInt3)
VEC_DRAG_INT(dragInt4, 4, DragInt4)

#define VEC_INPUT_FLOAT(NAME, N, IMCALL)                                               \
ZymValue u_##NAME(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue refV) {               \
    std::string label; if (!reqStr(vm, labelV, "ui." #NAME, &label)) return ZYM_ERROR; \
    float v[N];        if (!refReadFloatN(vm, refV, N, "ui." #NAME, v)) return ZYM_ERROR; \
    if (!requireFrame(vm, "ui." #NAME)) return ZYM_ERROR;                              \
    bool changed = ImGui::IMCALL(label.c_str(), v, "%.3f", 0);                         \
    if (changed) refWriteFloatN(vm, refV, N, v);                                       \
    return zym_newBool(changed);                                                       \
}

VEC_INPUT_FLOAT(inputFloat2, 2, InputFloat2)
VEC_INPUT_FLOAT(inputFloat3, 3, InputFloat3)
VEC_INPUT_FLOAT(inputFloat4, 4, InputFloat4)

#define VEC_INPUT_INT(NAME, N, IMCALL)                                                 \
ZymValue u_##NAME(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue refV) {               \
    std::string label; if (!reqStr(vm, labelV, "ui." #NAME, &label)) return ZYM_ERROR; \
    int v[N];          if (!refReadIntN(vm, refV, N, "ui." #NAME, v)) return ZYM_ERROR; \
    if (!requireFrame(vm, "ui." #NAME)) return ZYM_ERROR;                              \
    bool changed = ImGui::IMCALL(label.c_str(), v, 0);                                 \
    if (changed) refWriteIntN(vm, refV, N, v);                                         \
    return zym_newBool(changed);                                                       \
}

VEC_INPUT_INT(inputInt2, 2, InputInt2)
VEC_INPUT_INT(inputInt3, 3, InputInt3)
VEC_INPUT_INT(inputInt4, 4, InputInt4)

// ---- 2d.g: sliderAngle / vsliders ----
ZymValue u_sliderAngle(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue refV,
                       ZymValue degMinV, ZymValue degMaxV) {
    std::string label; if (!reqStr(vm, labelV, "ui.sliderAngle", &label)) return ZYM_ERROR;
    float v;           if (!refReadFloat(vm, refV, "ui.sliderAngle", &v)) return ZYM_ERROR;
    double mn = optNum(degMinV, -360.0), mx = optNum(degMaxV, 360.0);
    if (!requireFrame(vm, "ui.sliderAngle")) return ZYM_ERROR;
    bool changed = ImGui::SliderAngle(label.c_str(), &v, (float)mn, (float)mx,
                                      "%.0f deg", 0);
    if (changed) refWriteFloat(vm, refV, v);
    return zym_newBool(changed);
}

ZymValue u_vSliderFloat(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue wV,
                        ZymValue hV, ZymValue refV, ZymValue minV, ZymValue maxV) {
    std::string label; if (!reqStr(vm, labelV, "ui.vSliderFloat", &label)) return ZYM_ERROR;
    float v;           if (!refReadFloat(vm, refV, "ui.vSliderFloat", &v)) return ZYM_ERROR;
    double w = optNum(wV, 18.0), h = optNum(hV, 160.0);
    double mn = optNum(minV, 0.0), mx = optNum(maxV, 1.0);
    if (!requireFrame(vm, "ui.vSliderFloat")) return ZYM_ERROR;
    bool changed = ImGui::VSliderFloat(label.c_str(), ImVec2((float)w, (float)h),
                                       &v, (float)mn, (float)mx, "%.3f", 0);
    if (changed) refWriteFloat(vm, refV, v);
    return zym_newBool(changed);
}

ZymValue u_vSliderInt(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue wV,
                      ZymValue hV, ZymValue refV, ZymValue minV, ZymValue maxV) {
    std::string label; if (!reqStr(vm, labelV, "ui.vSliderInt", &label)) return ZYM_ERROR;
    int v;             if (!refReadInt(vm, refV, "ui.vSliderInt", &v))   return ZYM_ERROR;
    double w = optNum(wV, 18.0), h = optNum(hV, 160.0);
    int mn = optInt(minV, 0), mx = optInt(maxV, 100);
    if (!requireFrame(vm, "ui.vSliderInt")) return ZYM_ERROR;
    bool changed = ImGui::VSliderInt(label.c_str(), ImVec2((float)w, (float)h),
                                     &v, mn, mx, "%d", 0);
    if (changed) refWriteInt(vm, refV, v);
    return zym_newBool(changed);
}

// ---- 2d.h: scrolling helpers ----
ZymValue u_getScrollX(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.getScrollX")) return ZYM_ERROR;
    return zym_newNumber((double)ImGui::GetScrollX());
}
ZymValue u_getScrollY(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.getScrollY")) return ZYM_ERROR;
    return zym_newNumber((double)ImGui::GetScrollY());
}
ZymValue u_getScrollMaxX(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.getScrollMaxX")) return ZYM_ERROR;
    return zym_newNumber((double)ImGui::GetScrollMaxX());
}
ZymValue u_getScrollMaxY(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.getScrollMaxY")) return ZYM_ERROR;
    return zym_newNumber((double)ImGui::GetScrollMaxY());
}
ZymValue u_setScrollX(ZymVM* vm, ZymValue, ZymValue xV) {
    if (!requireFrame(vm, "ui.setScrollX")) return ZYM_ERROR;
    ImGui::SetScrollX((float)optNum(xV, 0)); return zym_newNull();
}
ZymValue u_setScrollY(ZymVM* vm, ZymValue, ZymValue yV) {
    if (!requireFrame(vm, "ui.setScrollY")) return ZYM_ERROR;
    ImGui::SetScrollY((float)optNum(yV, 0)); return zym_newNull();
}
ZymValue u_setScrollHereX(ZymVM* vm, ZymValue, ZymValue cV) {
    if (!requireFrame(vm, "ui.setScrollHereX")) return ZYM_ERROR;
    ImGui::SetScrollHereX((float)optNum(cV, 0.5)); return zym_newNull();
}
ZymValue u_setScrollHereY(ZymVM* vm, ZymValue, ZymValue cV) {
    if (!requireFrame(vm, "ui.setScrollHereY")) return ZYM_ERROR;
    ImGui::SetScrollHereY((float)optNum(cV, 0.5)); return zym_newNull();
}
ZymValue u_setScrollFromPosX(ZymVM* vm, ZymValue, ZymValue pV, ZymValue cV) {
    if (!requireFrame(vm, "ui.setScrollFromPosX")) return ZYM_ERROR;
    ImGui::SetScrollFromPosX((float)optNum(pV, 0), (float)optNum(cV, 0.5));
    return zym_newNull();
}
ZymValue u_setScrollFromPosY(ZymVM* vm, ZymValue, ZymValue pV, ZymValue cV) {
    if (!requireFrame(vm, "ui.setScrollFromPosY")) return ZYM_ERROR;
    ImGui::SetScrollFromPosY((float)optNum(pV, 0), (float)optNum(cV, 0.5));
    return zym_newNull();
}

// ---- 2d.i: window state queries ----
ZymValue u_isWindowAppearing(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.isWindowAppearing")) return ZYM_ERROR;
    return zym_newBool(ImGui::IsWindowAppearing());
}
ZymValue u_isWindowCollapsed(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.isWindowCollapsed")) return ZYM_ERROR;
    return zym_newBool(ImGui::IsWindowCollapsed());
}
ZymValue u_getWindowPos(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.getWindowPos")) return ZYM_ERROR;
    ImVec2 p = ImGui::GetWindowPos(); return makeXY(vm, p.x, p.y);
}
ZymValue u_getWindowSize(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.getWindowSize")) return ZYM_ERROR;
    ImVec2 s = ImGui::GetWindowSize(); return makeXY(vm, s.x, s.y);
}
ZymValue u_getWindowWidth(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.getWindowWidth")) return ZYM_ERROR;
    return zym_newNumber((double)ImGui::GetWindowWidth());
}
ZymValue u_getWindowHeight(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.getWindowHeight")) return ZYM_ERROR;
    return zym_newNumber((double)ImGui::GetWindowHeight());
}

// ---- 2d.j: setNextWindow* ----
ZymValue u_setNextWindowFocus(ZymVM*, ZymValue) {
    ImGui::SetNextWindowFocus(); return zym_newNull();
}
ZymValue u_setNextWindowBgAlpha(ZymVM*, ZymValue, ZymValue aV) {
    ImGui::SetNextWindowBgAlpha((float)optNum(aV, 1.0)); return zym_newNull();
}
ZymValue u_setNextWindowContentSize(ZymVM*, ZymValue, ZymValue wV, ZymValue hV) {
    ImGui::SetNextWindowContentSize(ImVec2((float)optNum(wV, 0), (float)optNum(hV, 0)));
    return zym_newNull();
}
ZymValue u_setNextWindowCollapsed(ZymVM*, ZymValue, ZymValue cV) {
    ImGui::SetNextWindowCollapsed(optBool(cV, false), 0); return zym_newNull();
}
ZymValue u_setNextWindowScroll(ZymVM*, ZymValue, ZymValue xV, ZymValue yV) {
    ImGui::SetNextWindowScroll(ImVec2((float)optNum(xV, -1), (float)optNum(yV, -1)));
    return zym_newNull();
}

// ---- 2d.k: extra item queries ----
ZymValue u_isItemVisible(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.isItemVisible")) return ZYM_ERROR;
    return zym_newBool(ImGui::IsItemVisible());
}
ZymValue u_isItemEdited(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.isItemEdited")) return ZYM_ERROR;
    return zym_newBool(ImGui::IsItemEdited());
}
ZymValue u_isItemActivated(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.isItemActivated")) return ZYM_ERROR;
    return zym_newBool(ImGui::IsItemActivated());
}
ZymValue u_isItemDeactivated(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.isItemDeactivated")) return ZYM_ERROR;
    return zym_newBool(ImGui::IsItemDeactivated());
}
ZymValue u_isItemDeactivatedAfterEdit(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.isItemDeactivatedAfterEdit")) return ZYM_ERROR;
    return zym_newBool(ImGui::IsItemDeactivatedAfterEdit());
}
ZymValue u_isItemToggledOpen(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.isItemToggledOpen")) return ZYM_ERROR;
    return zym_newBool(ImGui::IsItemToggledOpen());
}
ZymValue u_isAnyItemHovered(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.isAnyItemHovered")) return ZYM_ERROR;
    return zym_newBool(ImGui::IsAnyItemHovered());
}
ZymValue u_isAnyItemActive(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.isAnyItemActive")) return ZYM_ERROR;
    return zym_newBool(ImGui::IsAnyItemActive());
}
ZymValue u_isAnyItemFocused(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.isAnyItemFocused")) return ZYM_ERROR;
    return zym_newBool(ImGui::IsAnyItemFocused());
}
ZymValue u_getItemRectMin(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.getItemRectMin")) return ZYM_ERROR;
    ImVec2 p = ImGui::GetItemRectMin(); return makeXY(vm, p.x, p.y);
}
ZymValue u_getItemRectMax(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.getItemRectMax")) return ZYM_ERROR;
    ImVec2 p = ImGui::GetItemRectMax(); return makeXY(vm, p.x, p.y);
}
ZymValue u_getItemRectSize(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.getItemRectSize")) return ZYM_ERROR;
    ImVec2 p = ImGui::GetItemRectSize(); return makeXY(vm, p.x, p.y);
}

// ---- 2d.l: mouse queries ----
ZymValue u_isMouseDown(ZymVM*, ZymValue, ZymValue bV) {
    return zym_newBool(ImGui::IsMouseDown(optInt(bV, 0)));
}
ZymValue u_isMouseClicked(ZymVM*, ZymValue, ZymValue bV) {
    return zym_newBool(ImGui::IsMouseClicked(optInt(bV, 0), false));
}
ZymValue u_isMouseDoubleClicked(ZymVM*, ZymValue, ZymValue bV) {
    return zym_newBool(ImGui::IsMouseDoubleClicked(optInt(bV, 0)));
}
ZymValue u_isMouseReleased(ZymVM*, ZymValue, ZymValue bV) {
    return zym_newBool(ImGui::IsMouseReleased(optInt(bV, 0)));
}
ZymValue u_isMouseDragging(ZymVM*, ZymValue, ZymValue bV, ZymValue thV) {
    return zym_newBool(ImGui::IsMouseDragging(optInt(bV, 0), (float)optNum(thV, -1)));
}
ZymValue u_getMouseDragDelta(ZymVM* vm, ZymValue, ZymValue bV) {
    ImVec2 d = ImGui::GetMouseDragDelta(optInt(bV, 0), -1);
    return makeXY(vm, d.x, d.y);
}
ZymValue u_resetMouseDragDelta(ZymVM*, ZymValue, ZymValue bV) {
    ImGui::ResetMouseDragDelta(optInt(bV, 0)); return zym_newNull();
}
ZymValue u_getMouseClickedCount(ZymVM*, ZymValue, ZymValue bV) {
    return zym_newNumber((double)ImGui::GetMouseClickedCount(optInt(bV, 0)));
}

// ---- 2d.m: keyboard queries ----
ZymValue u_isKeyDown(ZymVM*, ZymValue, ZymValue kV) {
    return zym_newBool(ImGui::IsKeyDown((ImGuiKey)optInt(kV, 0)));
}
ZymValue u_isKeyPressed(ZymVM*, ZymValue, ZymValue kV, ZymValue repV) {
    return zym_newBool(ImGui::IsKeyPressed((ImGuiKey)optInt(kV, 0), optBool(repV, true)));
}
ZymValue u_isKeyReleased(ZymVM*, ZymValue, ZymValue kV) {
    return zym_newBool(ImGui::IsKeyReleased((ImGuiKey)optInt(kV, 0)));
}
ZymValue u_getKeyPressedAmount(ZymVM*, ZymValue, ZymValue kV, ZymValue rdV, ZymValue rrV) {
    return zym_newNumber((double)ImGui::GetKeyPressedAmount(
        (ImGuiKey)optInt(kV, 0), (float)optNum(rdV, 0), (float)optNum(rrV, 0)));
}
ZymValue u_setNextFrameWantCaptureKeyboard(ZymVM*, ZymValue, ZymValue bV) {
    ImGui::SetNextFrameWantCaptureKeyboard(optBool(bV, true)); return zym_newNull();
}
ZymValue u_setNextFrameWantCaptureMouse(ZymVM*, ZymValue, ZymValue bV) {
    ImGui::SetNextFrameWantCaptureMouse(optBool(bV, true)); return zym_newNull();
}

// ---- 2d.n: clipboard ----
ZymValue u_getClipboardText(ZymVM* vm, ZymValue) {
    const char* s = ImGui::GetClipboardText();
    return zym_newString(vm, s ? s : "");
}
ZymValue u_setClipboardText(ZymVM* vm, ZymValue, ZymValue sV) {
    std::string s; if (!reqStr(vm, sV, "ui.setClipboardText", &s)) return ZYM_ERROR;
    ImGui::SetClipboardText(s.c_str()); return zym_newNull();
}

// ---- 2d.o: popupContextItem / popupContextWindow (scoped) ----
ZymValue u_popupContextItem(ZymVM* vm, ZymValue, ZymValue idV, ZymValue bodyV) {
    std::string id;
    if (!reqStr(vm, idV, "ui.popupContextItem(id, body)", &id)) return ZYM_ERROR;
    if (!reqCallable(vm, bodyV, "ui.popupContextItem(id, body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.popupContextItem")) return ZYM_ERROR;
    bool open = ImGui::BeginPopupContextItem(id.c_str(), ImGuiPopupFlags_MouseButtonRight);
    if (open) {
        bool ok = invokeBody(vm, bodyV);
        ImGui::EndPopup();
        if (!ok) return ZYM_ERROR;
    }
    return zym_newBool(open);
}

ZymValue u_popupContextWindow(ZymVM* vm, ZymValue, ZymValue idV, ZymValue bodyV) {
    std::string id;
    if (!reqStr(vm, idV, "ui.popupContextWindow(id, body)", &id)) return ZYM_ERROR;
    if (!reqCallable(vm, bodyV, "ui.popupContextWindow(id, body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.popupContextWindow")) return ZYM_ERROR;
    bool open = ImGui::BeginPopupContextWindow(id.c_str(), ImGuiPopupFlags_MouseButtonRight);
    if (open) {
        bool ok = invokeBody(vm, bodyV);
        ImGui::EndPopup();
        if (!ok) return ZYM_ERROR;
    }
    return zym_newBool(open);
}

// ---- 2d.p: setNextItem*, push/pop ItemWidth, calcTextSize, style getters ----
ZymValue u_setNextItemWidth(ZymVM* vm, ZymValue, ZymValue wV) {
    if (!requireFrame(vm, "ui.setNextItemWidth")) return ZYM_ERROR;
    ImGui::SetNextItemWidth((float)optNum(wV, -1)); return zym_newNull();
}
ZymValue u_setNextItemOpen(ZymVM* vm, ZymValue, ZymValue bV) {
    if (!requireFrame(vm, "ui.setNextItemOpen")) return ZYM_ERROR;
    ImGui::SetNextItemOpen(optBool(bV, false), 0); return zym_newNull();
}
ZymValue u_setNextItemAllowOverlap(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.setNextItemAllowOverlap")) return ZYM_ERROR;
    ImGui::SetNextItemAllowOverlap(); return zym_newNull();
}
ZymValue u_pushItemWidth(ZymVM* vm, ZymValue, ZymValue wV) {
    if (!requireFrame(vm, "ui.pushItemWidth")) return ZYM_ERROR;
    ImGui::PushItemWidth((float)optNum(wV, -1)); return zym_newNull();
}
ZymValue u_popItemWidth(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.popItemWidth")) return ZYM_ERROR;
    ImGui::PopItemWidth(); return zym_newNull();
}
ZymValue u_setKeyboardFocusHere(ZymVM* vm, ZymValue, ZymValue oV) {
    if (!requireFrame(vm, "ui.setKeyboardFocusHere")) return ZYM_ERROR;
    ImGui::SetKeyboardFocusHere(optInt(oV, 0)); return zym_newNull();
}
ZymValue u_setItemDefaultFocus(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.setItemDefaultFocus")) return ZYM_ERROR;
    ImGui::SetItemDefaultFocus(); return zym_newNull();
}
ZymValue u_calcTextSize(ZymVM* vm, ZymValue, ZymValue sV) {
    std::string s; if (!reqStr(vm, sV, "ui.calcTextSize", &s)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.calcTextSize")) return ZYM_ERROR;
    ImVec2 p = ImGui::CalcTextSize(s.c_str()); return makeXY(vm, p.x, p.y);
}
ZymValue u_getStyleColorVec4(ZymVM* vm, ZymValue, ZymValue nameV) {
    std::string name; if (!reqStr(vm, nameV, "ui.getStyleColorVec4", &name)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.getStyleColorVec4")) return ZYM_ERROR;
    int slot = -1;
    for (auto& s : kColSlots) if (name == s.name) { slot = s.slot; break; }
    if (slot < 0) {
        zym_runtimeError(vm, "ui.getStyleColorVec4: unknown color slot '%s'", name.c_str());
        return ZYM_ERROR;
    }
    const ImVec4& c = ImGui::GetStyleColorVec4(slot);
    ZymValue list = zym_newList(vm);
    zym_pushRoot(vm, list);
    zym_listAppend(vm, list, zym_newNumber((double)c.x));
    zym_listAppend(vm, list, zym_newNumber((double)c.y));
    zym_listAppend(vm, list, zym_newNumber((double)c.z));
    zym_listAppend(vm, list, zym_newNumber((double)c.w));
    zym_popRoot(vm);
    return list;
}
ZymValue u_getFontSize(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.getFontSize")) return ZYM_ERROR;
    return zym_newNumber((double)ImGui::GetFontSize());
}
ZymValue u_getTextLineHeight(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.getTextLineHeight")) return ZYM_ERROR;
    return zym_newNumber((double)ImGui::GetTextLineHeight());
}
ZymValue u_getTextLineHeightWithSpacing(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.getTextLineHeightWithSpacing")) return ZYM_ERROR;
    return zym_newNumber((double)ImGui::GetTextLineHeightWithSpacing());
}
ZymValue u_getFrameHeight(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.getFrameHeight")) return ZYM_ERROR;
    return zym_newNumber((double)ImGui::GetFrameHeight());
}
ZymValue u_getFrameHeightWithSpacing(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.getFrameHeightWithSpacing")) return ZYM_ERROR;
    return zym_newNumber((double)ImGui::GetFrameHeightWithSpacing());
}
ZymValue u_getContentRegionAvail(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.getContentRegionAvail")) return ZYM_ERROR;
    ImVec2 p = ImGui::GetContentRegionAvail(); return makeXY(vm, p.x, p.y);
}
ZymValue u_setCursorPos(ZymVM* vm, ZymValue, ZymValue xV, ZymValue yV) {
    if (!requireFrame(vm, "ui.setCursorPos")) return ZYM_ERROR;
    ImGui::SetCursorPos(ImVec2((float)optNum(xV, 0), (float)optNum(yV, 0)));
    return zym_newNull();
}
ZymValue u_setCursorScreenPos(ZymVM* vm, ZymValue, ZymValue xV, ZymValue yV) {
    if (!requireFrame(vm, "ui.setCursorScreenPos")) return ZYM_ERROR;
    ImGui::SetCursorScreenPos(ImVec2((float)optNum(xV, 0), (float)optNum(yV, 0)));
    return zym_newNull();
}

// ---- PR 2e: drag-and-drop, table sort specs, font atlas internals -------
//
// Drag and drop uses string-typed payloads only. The script passes a
// payload string + a user "type" tag (max 32 chars per ImGui). On the
// target side, the script asks for the same type tag and gets the
// string back. Internally we store the payload bytes in a static
// std::string (ImGui already copies them, so the static buffer is
// just for the round-trip out to the script when accepting).

static thread_local std::string g_ui_dndAcceptBuf;

ZymValue u_beginDragDropSource(ZymVM* vm, ZymValue, ZymValue flagsV) {
    if (!requireFrame(vm, "ui.beginDragDropSource")) return ZYM_ERROR;
    int flags = (int)optNum(flagsV, 0);
    return zym_newBool(ImGui::BeginDragDropSource((ImGuiDragDropFlags)flags));
}

ZymValue u_endDragDropSource(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.endDragDropSource")) return ZYM_ERROR;
    ImGui::EndDragDropSource();
    return zym_newNull();
}

ZymValue u_setDragDropPayload(ZymVM* vm, ZymValue, ZymValue typeV, ZymValue dataV) {
    std::string type;
    if (!reqStr(vm, typeV, "ui.setDragDropPayload(type, data)", &type)) return ZYM_ERROR;
    std::string data;
    if (!reqStr(vm, dataV, "ui.setDragDropPayload(type, data)", &data)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.setDragDropPayload")) return ZYM_ERROR;
    return zym_newBool(ImGui::SetDragDropPayload(type.c_str(),
                                                 data.data(),
                                                 data.size()));
}

ZymValue u_beginDragDropTarget(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.beginDragDropTarget")) return ZYM_ERROR;
    return zym_newBool(ImGui::BeginDragDropTarget());
}

ZymValue u_endDragDropTarget(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.endDragDropTarget")) return ZYM_ERROR;
    ImGui::EndDragDropTarget();
    return zym_newNull();
}

ZymValue u_acceptDragDropPayload(ZymVM* vm, ZymValue, ZymValue typeV, ZymValue flagsV) {
    std::string type;
    if (!reqStr(vm, typeV, "ui.acceptDragDropPayload(type, flags)", &type)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.acceptDragDropPayload")) return ZYM_ERROR;
    int flags = (int)optNum(flagsV, 0);
    const ImGuiPayload* p = ImGui::AcceptDragDropPayload(type.c_str(),
                                                        (ImGuiDragDropFlags)flags);
    if (!p || !p->Data || p->DataSize <= 0) return zym_newNull();
    g_ui_dndAcceptBuf.assign(static_cast<const char*>(p->Data), p->DataSize);
    return zym_newStringN(vm, g_ui_dndAcceptBuf.data(),
                          (int)g_ui_dndAcceptBuf.size());
}

ZymValue u_getDragDropPayload(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.getDragDropPayload")) return ZYM_ERROR;
    const ImGuiPayload* p = ImGui::GetDragDropPayload();
    if (!p || !p->Data || p->DataSize <= 0) return zym_newNull();
    ZymValue m = zym_newMap(vm);
    zym_pushRoot(vm, m);
    zym_mapSet(vm, m, "type",
               zym_newStringN(vm, p->DataType, (int)strlen(p->DataType)));
    zym_mapSet(vm, m, "data",
               zym_newStringN(vm, static_cast<const char*>(p->Data),
                              p->DataSize));
    zym_mapSet(vm, m, "preview",  zym_newBool(p->Preview));
    zym_mapSet(vm, m, "delivery", zym_newBool(p->Delivery));
    zym_popRoot(vm);
    return m;
}

// `UI.tableGetSortSpecs() -> list | null`
//
// Returns the current sort specs for the active table as a list of
// `{ column, direction, order }` maps, or `null` when the table is
// not sorted (or has nothing dirty). `direction` is one of the
// strings "asc" / "desc" / "none" to keep callers from having to
// memorize integer constants. Pointer returned by ImGui has frame
// lifetime — we copy everything out before returning.
ZymValue u_tableGetSortSpecs(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.tableGetSortSpecs")) return ZYM_ERROR;
    ImGuiTableSortSpecs* s = ImGui::TableGetSortSpecs();
    if (!s) return zym_newNull();
    ZymValue list = zym_newList(vm);
    zym_pushRoot(vm, list);
    for (int i = 0; i < s->SpecsCount; i++) {
        const ImGuiTableColumnSortSpecs& c = s->Specs[i];
        ZymValue m = zym_newMap(vm);
        zym_pushRoot(vm, m);
        zym_mapSet(vm, m, "column",       zym_newNumber((double)c.ColumnIndex));
        zym_mapSet(vm, m, "userId",       zym_newNumber((double)c.ColumnUserID));
        zym_mapSet(vm, m, "order",        zym_newNumber((double)c.SortOrder));
        const char* dir = "none";
        if (c.SortDirection == ImGuiSortDirection_Ascending)  dir = "asc";
        if (c.SortDirection == ImGuiSortDirection_Descending) dir = "desc";
        zym_mapSet(vm, m, "direction", zym_newStringN(vm, dir, (int)strlen(dir)));
        zym_listAppend(vm, list, m);
        zym_popRoot(vm);
    }
    // Caller can choose to clear the dirty flag after handling.
    s->SpecsDirty = false;
    zym_popRoot(vm);
    return list;
}

// ---- font atlas internals --------------------------------------------
//
// All atlas operations require an active ImGui context (so callers
// must be inside `UI.frame(...)` or have one already created). They
// affect the *current* context's atlas, same as every other font
// API in this file. Note that adding/removing fonts during a frame
// is risky — prefer doing it right after the first `UI.frame` tick
// (when the context has been lazily created) and before any text
// has been drawn this frame.

ZymValue u_addFontDefault(ZymVM* vm, ZymValue) {
    if (!ImGui::GetCurrentContext()) {
        setError("ui.addFontDefault: no active ImGui context (call inside ui.frame)");
        return zym_newNull();
    }
    ImGuiIO& io = ImGui::GetIO();
    if (!io.Fonts) {
        setError("ui.addFontDefault: no font atlas on this context");
        return zym_newNull();
    }
    ImFont* font = io.Fonts->AddFontDefault();
    if (!font) {
        setError("ui.addFontDefault: AddFontDefault returned null");
        return zym_newNull();
    }
    return makeFontInstance(vm, font);
}

ZymValue u_clearFonts(ZymVM* vm, ZymValue) {
    if (!ImGui::GetCurrentContext()) {
        zym_runtimeError(vm, "ui.clearFonts: no active ImGui context");
        return ZYM_ERROR;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts) io.Fonts->Clear();
    return zym_newNull();
}

ZymValue u_getFontTexSize(ZymVM* vm, ZymValue) {
    if (!ImGui::GetCurrentContext()) {
        zym_runtimeError(vm, "ui.getFontTexSize: no active ImGui context");
        return ZYM_ERROR;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (!io.Fonts || !io.Fonts->TexData) {
        return zym_newNull();
    }
    ZymValue m = zym_newMap(vm);
    zym_pushRoot(vm, m);
    zym_mapSet(vm, m, "w", zym_newNumber((double)io.Fonts->TexData->Width));
    zym_mapSet(vm, m, "h", zym_newNumber((double)io.Fonts->TexData->Height));
    zym_popRoot(vm);
    return m;
}

ZymValue u_getFontAtlasFlags(ZymVM* vm, ZymValue) {
    if (!ImGui::GetCurrentContext()) {
        zym_runtimeError(vm, "ui.getFontAtlasFlags: no active ImGui context");
        return ZYM_ERROR;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (!io.Fonts) return zym_newNumber(0);
    return zym_newNumber((double)io.Fonts->Flags);
}

ZymValue u_setFontAtlasFlags(ZymVM* vm, ZymValue, ZymValue fV) {
    if (!ImGui::GetCurrentContext()) {
        zym_runtimeError(vm, "ui.setFontAtlasFlags: no active ImGui context");
        return ZYM_ERROR;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts) io.Fonts->Flags = (ImFontAtlasFlags)(int)optNum(fV, 0);
    return zym_newNull();
}

ZymValue u_getFontCount(ZymVM* vm, ZymValue) {
    if (!ImGui::GetCurrentContext()) {
        zym_runtimeError(vm, "ui.getFontCount: no active ImGui context");
        return ZYM_ERROR;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (!io.Fonts) return zym_newNumber(0);
    return zym_newNumber((double)io.Fonts->Fonts.Size);
}

ZymValue u_getFontAt(ZymVM* vm, ZymValue, ZymValue iV) {
    if (!ImGui::GetCurrentContext()) {
        zym_runtimeError(vm, "ui.getFontAt: no active ImGui context");
        return ZYM_ERROR;
    }
    int i = (int)optNum(iV, 0);
    ImGuiIO& io = ImGui::GetIO();
    if (!io.Fonts || i < 0 || i >= io.Fonts->Fonts.Size) return zym_newNull();
    return makeFontInstance(vm, io.Fonts->Fonts[i]);
}

ZymValue u_lastError(ZymVM* vm, ZymValue /*self*/) {
    return zym_newStringN(vm, g_ui_lastError.c_str(),
                          (int)g_ui_lastError.size());
}

// ---- ImGui recoverable-error diagnostics toggles -------------------------
// ImGui prints messages like "[imgui-error] In window '...': Code uses
// SetCursorPos() ..." through its recoverable-error path. The output is
// gated on two ImGuiIO flags (ConfigErrorRecoveryEnableDebugLog for the
// stderr/printf line and ConfigErrorRecoveryEnableTooltip for the in-app
// tooltip). Both default to true. Scripts can silence them per-frame via
// `UI.silent(true)` (kills both) or surgically via setErrorLogging /
// setErrorTooltip. We do not touch ConfigErrorRecovery itself — disabling
// recovery turns the same conditions into hard asserts, which is the
// opposite of what scripts asking to "silence warnings" want.
ZymValue u_setErrorLogging(ZymVM* vm, ZymValue, ZymValue onV) {
    if (!ImGui::GetCurrentContext()) {
        zym_runtimeError(vm, "ui.setErrorLogging: no active ImGui context");
        return ZYM_ERROR;
    }
    bool on; if (!reqBool(vm, onV, "ui.setErrorLogging", &on)) return ZYM_ERROR;
    ImGui::GetIO().ConfigErrorRecoveryEnableDebugLog = on;
    return zym_newNull();
}

ZymValue u_setErrorTooltip(ZymVM* vm, ZymValue, ZymValue onV) {
    if (!ImGui::GetCurrentContext()) {
        zym_runtimeError(vm, "ui.setErrorTooltip: no active ImGui context");
        return ZYM_ERROR;
    }
    bool on; if (!reqBool(vm, onV, "ui.setErrorTooltip", &on)) return ZYM_ERROR;
    ImGui::GetIO().ConfigErrorRecoveryEnableTooltip = on;
    return zym_newNull();
}

ZymValue u_silent(ZymVM* vm, ZymValue, ZymValue onV) {
    if (!ImGui::GetCurrentContext()) {
        zym_runtimeError(vm, "ui.silent: no active ImGui context");
        return ZYM_ERROR;
    }
    bool on; if (!reqBool(vm, onV, "ui.silent", &on)) return ZYM_ERROR;
    ImGuiIO& io = ImGui::GetIO();
    // `on == true` => silence: turn both diagnostics OFF.
    io.ConfigErrorRecoveryEnableDebugLog = !on;
    io.ConfigErrorRecoveryEnableTooltip  = !on;
    return zym_newNull();
}

// ===== PR 3: DrawList method closures =====================================
// All `dl_*` closures receive the DrawList's native context as `self`.

#define DL_PROLOGUE(WHERE) \
    ImDrawList* dl; if (!reqDL(vm, self, WHERE, &dl)) return ZYM_ERROR

// --- primitives -----------------------------------------------------------

ZymValue dl_addLine(ZymVM* vm, ZymValue self, ZymValue x1V, ZymValue y1V,
                    ZymValue x2V, ZymValue y2V, ZymValue colV, ZymValue thV) {
    DL_PROLOGUE("DrawList.addLine");
    dl->AddLine(ImVec2((float)optNum(x1V,0), (float)optNum(y1V,0)),
                ImVec2((float)optNum(x2V,0), (float)optNum(y2V,0)),
                optU32(colV, 0xFFFFFFFFu), (float)optNum(thV, 1.0));
    return zym_newNull();
}

ZymValue dl_addRect(ZymVM* vm, ZymValue self, ZymValue xV, ZymValue yV,
                    ZymValue wV, ZymValue hV, ZymValue colV,
                    ZymValue rV, ZymValue thV, ZymValue flV) {
    DL_PROLOGUE("DrawList.addRect");
    float x = (float)optNum(xV,0), y = (float)optNum(yV,0);
    float w = (float)optNum(wV,0), h = (float)optNum(hV,0);
    dl->AddRect(ImVec2(x,y), ImVec2(x+w,y+h), optU32(colV, 0xFFFFFFFFu),
                (float)optNum(rV,0), (float)optNum(thV,1.0),
                (ImDrawFlags)optU32(flV, 0));
    return zym_newNull();
}

ZymValue dl_addRectFilled(ZymVM* vm, ZymValue self, ZymValue xV, ZymValue yV,
                          ZymValue wV, ZymValue hV, ZymValue colV,
                          ZymValue rV, ZymValue flV) {
    DL_PROLOGUE("DrawList.addRectFilled");
    float x = (float)optNum(xV,0), y = (float)optNum(yV,0);
    float w = (float)optNum(wV,0), h = (float)optNum(hV,0);
    dl->AddRectFilled(ImVec2(x,y), ImVec2(x+w,y+h), optU32(colV, 0xFFFFFFFFu),
                      (float)optNum(rV,0), (ImDrawFlags)optU32(flV, 0));
    return zym_newNull();
}

ZymValue dl_addRectFilledMultiColor(ZymVM* vm, ZymValue self,
        ZymValue xV, ZymValue yV, ZymValue wV, ZymValue hV,
        ZymValue cTL, ZymValue cTR, ZymValue cBR, ZymValue cBL) {
    DL_PROLOGUE("DrawList.addRectFilledMultiColor");
    float x = (float)optNum(xV,0), y = (float)optNum(yV,0);
    float w = (float)optNum(wV,0), h = (float)optNum(hV,0);
    dl->AddRectFilledMultiColor(ImVec2(x,y), ImVec2(x+w,y+h),
                                optU32(cTL,0xFFFFFFFFu), optU32(cTR,0xFFFFFFFFu),
                                optU32(cBR,0xFFFFFFFFu), optU32(cBL,0xFFFFFFFFu));
    return zym_newNull();
}

ZymValue dl_addQuad(ZymVM* vm, ZymValue self,
        ZymValue x1, ZymValue y1, ZymValue x2, ZymValue y2,
        ZymValue x3, ZymValue y3, ZymValue x4, ZymValue y4,
        ZymValue colV, ZymValue thV) {
    DL_PROLOGUE("DrawList.addQuad");
    dl->AddQuad(ImVec2((float)optNum(x1,0),(float)optNum(y1,0)),
                ImVec2((float)optNum(x2,0),(float)optNum(y2,0)),
                ImVec2((float)optNum(x3,0),(float)optNum(y3,0)),
                ImVec2((float)optNum(x4,0),(float)optNum(y4,0)),
                optU32(colV,0xFFFFFFFFu), (float)optNum(thV,1.0));
    return zym_newNull();
}

ZymValue dl_addQuadFilled(ZymVM* vm, ZymValue self,
        ZymValue x1, ZymValue y1, ZymValue x2, ZymValue y2,
        ZymValue x3, ZymValue y3, ZymValue x4, ZymValue y4,
        ZymValue colV) {
    DL_PROLOGUE("DrawList.addQuadFilled");
    dl->AddQuadFilled(ImVec2((float)optNum(x1,0),(float)optNum(y1,0)),
                      ImVec2((float)optNum(x2,0),(float)optNum(y2,0)),
                      ImVec2((float)optNum(x3,0),(float)optNum(y3,0)),
                      ImVec2((float)optNum(x4,0),(float)optNum(y4,0)),
                      optU32(colV,0xFFFFFFFFu));
    return zym_newNull();
}

ZymValue dl_addTriangle(ZymVM* vm, ZymValue self,
        ZymValue x1, ZymValue y1, ZymValue x2, ZymValue y2,
        ZymValue x3, ZymValue y3, ZymValue colV, ZymValue thV) {
    DL_PROLOGUE("DrawList.addTriangle");
    dl->AddTriangle(ImVec2((float)optNum(x1,0),(float)optNum(y1,0)),
                    ImVec2((float)optNum(x2,0),(float)optNum(y2,0)),
                    ImVec2((float)optNum(x3,0),(float)optNum(y3,0)),
                    optU32(colV,0xFFFFFFFFu), (float)optNum(thV,1.0));
    return zym_newNull();
}

ZymValue dl_addTriangleFilled(ZymVM* vm, ZymValue self,
        ZymValue x1, ZymValue y1, ZymValue x2, ZymValue y2,
        ZymValue x3, ZymValue y3, ZymValue colV) {
    DL_PROLOGUE("DrawList.addTriangleFilled");
    dl->AddTriangleFilled(ImVec2((float)optNum(x1,0),(float)optNum(y1,0)),
                          ImVec2((float)optNum(x2,0),(float)optNum(y2,0)),
                          ImVec2((float)optNum(x3,0),(float)optNum(y3,0)),
                          optU32(colV,0xFFFFFFFFu));
    return zym_newNull();
}

ZymValue dl_addCircle(ZymVM* vm, ZymValue self, ZymValue cxV, ZymValue cyV,
                      ZymValue rV, ZymValue colV, ZymValue segV, ZymValue thV) {
    DL_PROLOGUE("DrawList.addCircle");
    dl->AddCircle(ImVec2((float)optNum(cxV,0),(float)optNum(cyV,0)),
                  (float)optNum(rV,0), optU32(colV,0xFFFFFFFFu),
                  (int)optNum(segV,0), (float)optNum(thV,1.0));
    return zym_newNull();
}

ZymValue dl_addCircleFilled(ZymVM* vm, ZymValue self, ZymValue cxV, ZymValue cyV,
                            ZymValue rV, ZymValue colV, ZymValue segV) {
    DL_PROLOGUE("DrawList.addCircleFilled");
    dl->AddCircleFilled(ImVec2((float)optNum(cxV,0),(float)optNum(cyV,0)),
                        (float)optNum(rV,0), optU32(colV,0xFFFFFFFFu),
                        (int)optNum(segV,0));
    return zym_newNull();
}

ZymValue dl_addNgon(ZymVM* vm, ZymValue self, ZymValue cxV, ZymValue cyV,
                    ZymValue rV, ZymValue colV, ZymValue segV, ZymValue thV) {
    DL_PROLOGUE("DrawList.addNgon");
    dl->AddNgon(ImVec2((float)optNum(cxV,0),(float)optNum(cyV,0)),
                (float)optNum(rV,0), optU32(colV,0xFFFFFFFFu),
                (int)optNum(segV,0), (float)optNum(thV,1.0));
    return zym_newNull();
}

ZymValue dl_addNgonFilled(ZymVM* vm, ZymValue self, ZymValue cxV, ZymValue cyV,
                          ZymValue rV, ZymValue colV, ZymValue segV) {
    DL_PROLOGUE("DrawList.addNgonFilled");
    dl->AddNgonFilled(ImVec2((float)optNum(cxV,0),(float)optNum(cyV,0)),
                      (float)optNum(rV,0), optU32(colV,0xFFFFFFFFu),
                      (int)optNum(segV,0));
    return zym_newNull();
}

ZymValue dl_addEllipse(ZymVM* vm, ZymValue self, ZymValue cxV, ZymValue cyV,
                       ZymValue rxV, ZymValue ryV, ZymValue colV,
                       ZymValue rotV, ZymValue segV, ZymValue thV) {
    DL_PROLOGUE("DrawList.addEllipse");
    dl->AddEllipse(ImVec2((float)optNum(cxV,0),(float)optNum(cyV,0)),
                   ImVec2((float)optNum(rxV,0),(float)optNum(ryV,0)),
                   optU32(colV,0xFFFFFFFFu), (float)optNum(rotV,0),
                   (int)optNum(segV,0), (float)optNum(thV,1.0));
    return zym_newNull();
}

ZymValue dl_addEllipseFilled(ZymVM* vm, ZymValue self, ZymValue cxV, ZymValue cyV,
                             ZymValue rxV, ZymValue ryV, ZymValue colV,
                             ZymValue rotV, ZymValue segV) {
    DL_PROLOGUE("DrawList.addEllipseFilled");
    dl->AddEllipseFilled(ImVec2((float)optNum(cxV,0),(float)optNum(cyV,0)),
                         ImVec2((float)optNum(rxV,0),(float)optNum(ryV,0)),
                         optU32(colV,0xFFFFFFFFu), (float)optNum(rotV,0),
                         (int)optNum(segV,0));
    return zym_newNull();
}

ZymValue dl_addText(ZymVM* vm, ZymValue self, ZymValue xV, ZymValue yV,
                    ZymValue colV, ZymValue sV) {
    std::string s;
    if (!reqStr(vm, sV, "DrawList.addText(x, y, color, s)", &s)) return ZYM_ERROR;
    DL_PROLOGUE("DrawList.addText");
    dl->AddText(ImVec2((float)optNum(xV,0),(float)optNum(yV,0)),
                optU32(colV,0xFFFFFFFFu), s.c_str());
    return zym_newNull();
}

// addBezierCubic takes 11 native args (self + 10 named), one past the
// fixed-arity native dispatcher cap. Registered as a variadic closure
// so it parses cleanly and accepts the full positional arg list; the
// body unpacks them by index. Required args: x1,y1,x2,y2,x3,y3,x4,y4,
// color, thickness. Optional 11th: segments (0 = auto).
ZymValue dl_addBezierCubic(ZymVM* vm, ZymValue self,
                           ZymValue* vargs, int vargc) {
    DL_PROLOGUE("DrawList.addBezierCubic");
    if (vargc < 10) {
        zym_runtimeError(vm, "DrawList.addBezierCubic expects 10 or 11 args, got %d", vargc);
        return ZYM_ERROR;
    }
    int seg = (vargc >= 11) ? (int)optNum(vargs[10], 0) : 0;
    dl->AddBezierCubic(ImVec2((float)optNum(vargs[0],0),(float)optNum(vargs[1],0)),
                       ImVec2((float)optNum(vargs[2],0),(float)optNum(vargs[3],0)),
                       ImVec2((float)optNum(vargs[4],0),(float)optNum(vargs[5],0)),
                       ImVec2((float)optNum(vargs[6],0),(float)optNum(vargs[7],0)),
                       optU32(vargs[8],0xFFFFFFFFu),
                       (float)optNum(vargs[9],1.0), seg);
    return zym_newNull();
}

ZymValue dl_addBezierQuadratic(ZymVM* vm, ZymValue self,
        ZymValue x1, ZymValue y1, ZymValue x2, ZymValue y2,
        ZymValue x3, ZymValue y3,
        ZymValue colV, ZymValue thV, ZymValue segV) {
    DL_PROLOGUE("DrawList.addBezierQuadratic");
    dl->AddBezierQuadratic(ImVec2((float)optNum(x1,0),(float)optNum(y1,0)),
                           ImVec2((float)optNum(x2,0),(float)optNum(y2,0)),
                           ImVec2((float)optNum(x3,0),(float)optNum(y3,0)),
                           optU32(colV,0xFFFFFFFFu), (float)optNum(thV,1.0),
                           (int)optNum(segV,0));
    return zym_newNull();
}

ZymValue dl_addPolyline(ZymVM* vm, ZymValue self, ZymValue ptsV, ZymValue colV,
                        ZymValue closedV, ZymValue thV, ZymValue flV) {
    DL_PROLOGUE("DrawList.addPolyline");
    std::vector<ImVec2> pts;
    int n = readPointList(vm, ptsV, &pts);
    if (n < 0) { zym_runtimeError(vm, "DrawList.addPolyline: invalid point list"); return ZYM_ERROR; }
    if (n < 2) return zym_newNull();
    ImDrawFlags flags = (ImDrawFlags)optU32(flV, 0);
    bool closed = false;
    if (zym_isBool(closedV)) closed = zym_asBool(closedV);
    if (closed) flags |= ImDrawFlags_Closed;
    dl->AddPolyline(pts.data(), n, optU32(colV,0xFFFFFFFFu),
                    (float)optNum(thV,1.0), flags);
    return zym_newNull();
}

ZymValue dl_addConvexPolyFilled(ZymVM* vm, ZymValue self, ZymValue ptsV, ZymValue colV) {
    DL_PROLOGUE("DrawList.addConvexPolyFilled");
    std::vector<ImVec2> pts;
    int n = readPointList(vm, ptsV, &pts);
    if (n < 0) { zym_runtimeError(vm, "DrawList.addConvexPolyFilled: invalid point list"); return ZYM_ERROR; }
    if (n < 3) return zym_newNull();
    dl->AddConvexPolyFilled(pts.data(), n, optU32(colV,0xFFFFFFFFu));
    return zym_newNull();
}

ZymValue dl_addConcavePolyFilled(ZymVM* vm, ZymValue self, ZymValue ptsV, ZymValue colV) {
    DL_PROLOGUE("DrawList.addConcavePolyFilled");
    std::vector<ImVec2> pts;
    int n = readPointList(vm, ptsV, &pts);
    if (n < 0) { zym_runtimeError(vm, "DrawList.addConcavePolyFilled: invalid point list"); return ZYM_ERROR; }
    if (n < 3) return zym_newNull();
    dl->AddConcavePolyFilled(pts.data(), n, optU32(colV,0xFFFFFFFFu));
    return zym_newNull();
}

// --- path API -------------------------------------------------------------

ZymValue dl_pathClear(ZymVM* vm, ZymValue self) {
    DL_PROLOGUE("DrawList.pathClear");
    dl->PathClear(); return zym_newNull();
}

ZymValue dl_pathLineTo(ZymVM* vm, ZymValue self, ZymValue xV, ZymValue yV) {
    DL_PROLOGUE("DrawList.pathLineTo");
    dl->PathLineTo(ImVec2((float)optNum(xV,0),(float)optNum(yV,0)));
    return zym_newNull();
}

ZymValue dl_pathLineToMergeDuplicate(ZymVM* vm, ZymValue self, ZymValue xV, ZymValue yV) {
    DL_PROLOGUE("DrawList.pathLineToMergeDuplicate");
    dl->PathLineToMergeDuplicate(ImVec2((float)optNum(xV,0),(float)optNum(yV,0)));
    return zym_newNull();
}

ZymValue dl_pathFillConvex(ZymVM* vm, ZymValue self, ZymValue colV) {
    DL_PROLOGUE("DrawList.pathFillConvex");
    dl->PathFillConvex(optU32(colV,0xFFFFFFFFu));
    return zym_newNull();
}

ZymValue dl_pathFillConcave(ZymVM* vm, ZymValue self, ZymValue colV) {
    DL_PROLOGUE("DrawList.pathFillConcave");
    dl->PathFillConcave(optU32(colV,0xFFFFFFFFu));
    return zym_newNull();
}

ZymValue dl_pathStroke(ZymVM* vm, ZymValue self, ZymValue colV,
                       ZymValue closedV, ZymValue thV, ZymValue flV) {
    DL_PROLOGUE("DrawList.pathStroke");
    ImDrawFlags flags = (ImDrawFlags)optU32(flV, 0);
    if (zym_isBool(closedV) && zym_asBool(closedV)) flags |= ImDrawFlags_Closed;
    dl->PathStroke(optU32(colV,0xFFFFFFFFu), flags, (float)optNum(thV,1.0));
    return zym_newNull();
}

ZymValue dl_pathArcTo(ZymVM* vm, ZymValue self, ZymValue cxV, ZymValue cyV,
                      ZymValue rV, ZymValue aMinV, ZymValue aMaxV, ZymValue segV) {
    DL_PROLOGUE("DrawList.pathArcTo");
    dl->PathArcTo(ImVec2((float)optNum(cxV,0),(float)optNum(cyV,0)),
                  (float)optNum(rV,0), (float)optNum(aMinV,0),
                  (float)optNum(aMaxV,0), (int)optNum(segV,0));
    return zym_newNull();
}

ZymValue dl_pathArcToFast(ZymVM* vm, ZymValue self, ZymValue cxV, ZymValue cyV,
                          ZymValue rV, ZymValue aMinV, ZymValue aMaxV) {
    DL_PROLOGUE("DrawList.pathArcToFast");
    dl->PathArcToFast(ImVec2((float)optNum(cxV,0),(float)optNum(cyV,0)),
                      (float)optNum(rV,0), (int)optNum(aMinV,0),
                      (int)optNum(aMaxV,12));
    return zym_newNull();
}

ZymValue dl_pathEllipticalArcTo(ZymVM* vm, ZymValue self,
        ZymValue cxV, ZymValue cyV, ZymValue rxV, ZymValue ryV,
        ZymValue rotV, ZymValue aMinV, ZymValue aMaxV, ZymValue segV) {
    DL_PROLOGUE("DrawList.pathEllipticalArcTo");
    dl->PathEllipticalArcTo(ImVec2((float)optNum(cxV,0),(float)optNum(cyV,0)),
                            ImVec2((float)optNum(rxV,0),(float)optNum(ryV,0)),
                            (float)optNum(rotV,0), (float)optNum(aMinV,0),
                            (float)optNum(aMaxV,0), (int)optNum(segV,0));
    return zym_newNull();
}

ZymValue dl_pathBezierCubicCurveTo(ZymVM* vm, ZymValue self,
        ZymValue x2, ZymValue y2, ZymValue x3, ZymValue y3,
        ZymValue x4, ZymValue y4, ZymValue segV) {
    DL_PROLOGUE("DrawList.pathBezierCubicCurveTo");
    dl->PathBezierCubicCurveTo(ImVec2((float)optNum(x2,0),(float)optNum(y2,0)),
                               ImVec2((float)optNum(x3,0),(float)optNum(y3,0)),
                               ImVec2((float)optNum(x4,0),(float)optNum(y4,0)),
                               (int)optNum(segV,0));
    return zym_newNull();
}

ZymValue dl_pathBezierQuadraticCurveTo(ZymVM* vm, ZymValue self,
        ZymValue x2, ZymValue y2, ZymValue x3, ZymValue y3, ZymValue segV) {
    DL_PROLOGUE("DrawList.pathBezierQuadraticCurveTo");
    dl->PathBezierQuadraticCurveTo(ImVec2((float)optNum(x2,0),(float)optNum(y2,0)),
                                   ImVec2((float)optNum(x3,0),(float)optNum(y3,0)),
                                   (int)optNum(segV,0));
    return zym_newNull();
}

ZymValue dl_pathRect(ZymVM* vm, ZymValue self, ZymValue xV, ZymValue yV,
                     ZymValue wV, ZymValue hV, ZymValue rV, ZymValue flV) {
    DL_PROLOGUE("DrawList.pathRect");
    float x = (float)optNum(xV,0), y = (float)optNum(yV,0);
    float w = (float)optNum(wV,0), h = (float)optNum(hV,0);
    dl->PathRect(ImVec2(x,y), ImVec2(x+w,y+h),
                 (float)optNum(rV,0), (ImDrawFlags)optU32(flV,0));
    return zym_newNull();
}

// --- clip rect ------------------------------------------------------------

ZymValue dl_pushClipRect(ZymVM* vm, ZymValue self, ZymValue xV, ZymValue yV,
                         ZymValue wV, ZymValue hV, ZymValue intersectV) {
    DL_PROLOGUE("DrawList.pushClipRect");
    float x = (float)optNum(xV,0), y = (float)optNum(yV,0);
    float w = (float)optNum(wV,0), h = (float)optNum(hV,0);
    bool intersect = zym_isBool(intersectV) ? zym_asBool(intersectV) : false;
    dl->PushClipRect(ImVec2(x,y), ImVec2(x+w,y+h), intersect);
    return zym_newNull();
}

ZymValue dl_pushClipRectFullScreen(ZymVM* vm, ZymValue self) {
    DL_PROLOGUE("DrawList.pushClipRectFullScreen");
    dl->PushClipRectFullScreen(); return zym_newNull();
}

ZymValue dl_popClipRect(ZymVM* vm, ZymValue self) {
    DL_PROLOGUE("DrawList.popClipRect");
    dl->PopClipRect(); return zym_newNull();
}

ZymValue dl_getClipRectMin(ZymVM* vm, ZymValue self) {
    DL_PROLOGUE("DrawList.getClipRectMin");
    ImVec2 p = dl->GetClipRectMin();
    ZymValue m = zym_newMap(vm); zym_pushRoot(vm, m);
    zym_mapSet(vm, m, "x", zym_newNumber(p.x));
    zym_mapSet(vm, m, "y", zym_newNumber(p.y));
    zym_popRoot(vm); return m;
}

ZymValue dl_getClipRectMax(ZymVM* vm, ZymValue self) {
    DL_PROLOGUE("DrawList.getClipRectMax");
    ImVec2 p = dl->GetClipRectMax();
    ZymValue m = zym_newMap(vm); zym_pushRoot(vm, m);
    zym_mapSet(vm, m, "x", zym_newNumber(p.x));
    zym_mapSet(vm, m, "y", zym_newNumber(p.y));
    zym_popRoot(vm); return m;
}

// --- channel splitter -----------------------------------------------------

ZymValue dl_channelsSplit(ZymVM* vm, ZymValue self, ZymValue nV) {
    DL_PROLOGUE("DrawList.channelsSplit");
    dl->ChannelsSplit((int)optNum(nV, 2)); return zym_newNull();
}

ZymValue dl_channelsMerge(ZymVM* vm, ZymValue self) {
    DL_PROLOGUE("DrawList.channelsMerge");
    dl->ChannelsMerge(); return zym_newNull();
}

ZymValue dl_channelsSetCurrent(ZymVM* vm, ZymValue self, ZymValue nV) {
    DL_PROLOGUE("DrawList.channelsSetCurrent");
    dl->ChannelsSetCurrent((int)optNum(nV, 0)); return zym_newNull();
}

#undef DL_PROLOGUE

// --- DrawList instance factory --------------------------------------------

ZymValue makeDrawListInstance(ZymVM* vm, ImDrawList* dl) {
    ZymValue ctx = zym_createNativeContext(vm, dl, nullptr);
    zym_pushRoot(vm, ctx);
    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__drawlist__", ctx);

#define DLM(name, sig, fn) do { \
    ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctx); \
    zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
} while (0)

    DLM("addLine",                 "addLine(x1,y1,x2,y2,color,thickness)",                dl_addLine);
    DLM("addRect",                 "addRect(x,y,w,h,color,rounding,thickness,flags)",      dl_addRect);
    DLM("addRectFilled",           "addRectFilled(x,y,w,h,color,rounding,flags)",          dl_addRectFilled);
    DLM("addRectFilledMultiColor", "addRectFilledMultiColor(x,y,w,h,cTL,cTR,cBR,cBL)",     dl_addRectFilledMultiColor);
    DLM("addQuad",                 "addQuad(x1,y1,x2,y2,x3,y3,x4,y4,color,thickness)",     dl_addQuad);
    DLM("addQuadFilled",           "addQuadFilled(x1,y1,x2,y2,x3,y3,x4,y4,color)",         dl_addQuadFilled);
    DLM("addTriangle",             "addTriangle(x1,y1,x2,y2,x3,y3,color,thickness)",       dl_addTriangle);
    DLM("addTriangleFilled",       "addTriangleFilled(x1,y1,x2,y2,x3,y3,color)",           dl_addTriangleFilled);
    DLM("addCircle",               "addCircle(cx,cy,r,color,segments,thickness)",          dl_addCircle);
    DLM("addCircleFilled",         "addCircleFilled(cx,cy,r,color,segments)",              dl_addCircleFilled);
    DLM("addNgon",                 "addNgon(cx,cy,r,color,sides,thickness)",               dl_addNgon);
    DLM("addNgonFilled",           "addNgonFilled(cx,cy,r,color,sides)",                   dl_addNgonFilled);
    DLM("addEllipse",              "addEllipse(cx,cy,rx,ry,color,rot,segments,thickness)", dl_addEllipse);
    DLM("addEllipseFilled",        "addEllipseFilled(cx,cy,rx,ry,color,rot,segments)",     dl_addEllipseFilled);
    DLM("addText",                 "addText(x,y,color,s)",                                 dl_addText);
    // addBezierCubic takes 10 required args + optional `segments` (total
    // 11), one past the fixed-arity native dispatcher cap. Register it as
    // a variadic native closure so the full set fits — the wrapper itself
    // unpacks args by index. Signature uses `...args` per the Zym native
    // convention (see src/natives/Zym/zym_native.cpp variadic closures).
    {
        ZymValue cl = zym_createNativeClosureVariadic(
            vm,
            "addBezierCubic(...args)",
            (void*)dl_addBezierCubic,
            ctx);
        zym_pushRoot(vm, cl);
        zym_mapSet(vm, obj, "addBezierCubic", cl);
        zym_popRoot(vm);
    }
    DLM("addBezierQuadratic",      "addBezierQuadratic(x1,y1,x2,y2,x3,y3,color,thickness,segments)",  dl_addBezierQuadratic);
    DLM("addPolyline",             "addPolyline(points,color,closed,thickness,flags)",     dl_addPolyline);
    DLM("addConvexPolyFilled",     "addConvexPolyFilled(points,color)",                    dl_addConvexPolyFilled);
    DLM("addConcavePolyFilled",    "addConcavePolyFilled(points,color)",                   dl_addConcavePolyFilled);

    DLM("pathClear",                  "pathClear()",                                       dl_pathClear);
    DLM("pathLineTo",                 "pathLineTo(x,y)",                                   dl_pathLineTo);
    DLM("pathLineToMergeDuplicate",   "pathLineToMergeDuplicate(x,y)",                     dl_pathLineToMergeDuplicate);
    DLM("pathFillConvex",             "pathFillConvex(color)",                             dl_pathFillConvex);
    DLM("pathFillConcave",            "pathFillConcave(color)",                            dl_pathFillConcave);
    DLM("pathStroke",                 "pathStroke(color,closed,thickness,flags)",          dl_pathStroke);
    DLM("pathArcTo",                  "pathArcTo(cx,cy,r,aMin,aMax,segments)",             dl_pathArcTo);
    DLM("pathArcToFast",              "pathArcToFast(cx,cy,r,aMinOf12,aMaxOf12)",          dl_pathArcToFast);
    DLM("pathEllipticalArcTo",        "pathEllipticalArcTo(cx,cy,rx,ry,rot,aMin,aMax,segments)", dl_pathEllipticalArcTo);
    DLM("pathBezierCubicCurveTo",     "pathBezierCubicCurveTo(x2,y2,x3,y3,x4,y4,segments)",      dl_pathBezierCubicCurveTo);
    DLM("pathBezierQuadraticCurveTo", "pathBezierQuadraticCurveTo(x2,y2,x3,y3,segments)",        dl_pathBezierQuadraticCurveTo);
    DLM("pathRect",                   "pathRect(x,y,w,h,rounding,flags)",                  dl_pathRect);

    DLM("pushClipRect",           "pushClipRect(x,y,w,h,intersect)",                        dl_pushClipRect);
    DLM("pushClipRectFullScreen", "pushClipRectFullScreen()",                               dl_pushClipRectFullScreen);
    DLM("popClipRect",            "popClipRect()",                                          dl_popClipRect);
    DLM("getClipRectMin",         "getClipRectMin()",                                       dl_getClipRectMin);
    DLM("getClipRectMax",         "getClipRectMax()",                                       dl_getClipRectMax);

    DLM("channelsSplit",      "channelsSplit(count)",          dl_channelsSplit);
    DLM("channelsMerge",      "channelsMerge()",               dl_channelsMerge);
    DLM("channelsSetCurrent", "channelsSetCurrent(n)",         dl_channelsSetCurrent);

#undef DLM

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}

// --- module-level DrawList factories --------------------------------------

ZymValue u_drawListHandle(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.drawList")) return ZYM_ERROR;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!dl) { zym_runtimeError(vm, "ui.drawList: no current draw list"); return ZYM_ERROR; }
    return makeDrawListInstance(vm, dl);
}

ZymValue u_backgroundDrawList(ZymVM* vm, ZymValue) {
    if (!ImGui::GetCurrentContext()) {
        zym_runtimeError(vm, "ui.backgroundDrawList: no active ImGui context (call inside ui.frame)");
        return ZYM_ERROR;
    }
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!dl) return zym_newNull();
    return makeDrawListInstance(vm, dl);
}

ZymValue u_foregroundDrawList(ZymVM* vm, ZymValue) {
    if (!ImGui::GetCurrentContext()) {
        zym_runtimeError(vm, "ui.foregroundDrawList: no active ImGui context (call inside ui.frame)");
        return ZYM_ERROR;
    }
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return zym_newNull();
    return makeDrawListInstance(vm, dl);
}

} // namespace

// ---- module assembly -----------------------------------------------------

ZymValue nativeUi_create(ZymVM* vm) {
    // Wire up the sdl <-> ui hooks. Safe to do at every `nativeUi_create`
    // call (idempotent — they're just function-pointer assignments).
    g_sdl_uiContextDestructor = destroyUiContext;
    g_sdl_uiEventForwarder    = forwardEvent;

    ZymValue context = zym_createNativeContext(vm, nullptr, nullptr);
    zym_pushRoot(vm, context);

#define MOD(name, sig, fn) \
    ZymValue name = zym_createNativeClosure(vm, sig, (void*)fn, context); \
    zym_pushRoot(vm, name);

    MOD(frame,     "frame(win, body)",  u_frame)
    MOD(lastError, "lastError()",       u_lastError)
    MOD(silent,           "silent(on)",           u_silent)
    MOD(setErrorLogging,  "setErrorLogging(on)",  u_setErrorLogging)
    MOD(setErrorTooltip,  "setErrorTooltip(on)",  u_setErrorTooltip)

    // window: 2-arg (name, body) or 3-arg (name, flags, body) via dispatcher
    ZymValue window2 = zym_createNativeClosure(vm, "window(name, body)",        (void*)u_window,      context);
    zym_pushRoot(vm, window2);
    ZymValue window3 = zym_createNativeClosure(vm, "window(name, flags, body)", (void*)u_windowFlags, context);
    zym_pushRoot(vm, window3);
    ZymValue window = zym_createDispatcher(vm);
    zym_pushRoot(vm, window);
    zym_addOverload(vm, window, window2);
    zym_addOverload(vm, window, window3);

    MOD(setNextWindowPos,  "setNextWindowPos(x, y)",  u_setNextWindowPos)
    MOD(setNextWindowSize, "setNextWindowSize(w, h)", u_setNextWindowSize)

    // text family
    MOD(text,         "text(s)",                u_text)
    MOD(textColored,  "textColored(color, s)",  u_textColored)
    MOD(textWrapped,  "textWrapped(s)",         u_textWrapped)
    MOD(textDisabled, "textDisabled(s)",        u_textDisabled)
    MOD(labelText,    "labelText(label, value)", u_labelText)
    MOD(bulletText,   "bulletText(s)",          u_bulletText)
    MOD(bullet,       "bullet()",               u_bullet)

    // buttons (with dispatcher for button arity overload)
    ZymValue button1 = zym_createNativeClosure(vm, "button(label)", (void*)u_button, context);
    zym_pushRoot(vm, button1);
    ZymValue button3 = zym_createNativeClosure(vm, "button(label, w, h)", (void*)u_buttonSized, context);
    zym_pushRoot(vm, button3);
    ZymValue button = zym_createDispatcher(vm);
    zym_pushRoot(vm, button);
    zym_addOverload(vm, button, button1);
    zym_addOverload(vm, button, button3);

    MOD(smallButton,    "smallButton(label)",          u_smallButton)
    MOD(invisibleButton,"invisibleButton(id, w, h)",   u_invisibleButton)
    MOD(arrowButton,    "arrowButton(id, dir)",        u_arrowButton)

    // toggles
    MOD(checkbox,      "checkbox(label, ref)",                u_checkbox)
    MOD(radioButton,   "radioButton(label, active)",          u_radioButton)
    ZymValue selectable2 = zym_createNativeClosure(vm, "selectable(label, sel)", (void*)u_selectable, context);
    zym_pushRoot(vm, selectable2);
    ZymValue selectable3 = zym_createNativeClosure(vm, "selectable(label, sel, flags)", (void*)u_selectable, context);
    zym_pushRoot(vm, selectable3);
    ZymValue selectable = zym_createDispatcher(vm);
    zym_pushRoot(vm, selectable);
    zym_addOverload(vm, selectable, selectable2);
    zym_addOverload(vm, selectable, selectable3);

    // layout
    ZymValue sameLine0 = zym_createNativeClosure(vm, "sameLine()", (void*)u_sameLine, context);
    zym_pushRoot(vm, sameLine0);
    ZymValue sameLine1 = zym_createNativeClosure(vm, "sameLine(offset)", (void*)u_sameLine, context);
    zym_pushRoot(vm, sameLine1);
    ZymValue sameLine2 = zym_createNativeClosure(vm, "sameLine(offset, spacing)", (void*)u_sameLine, context);
    zym_pushRoot(vm, sameLine2);
    ZymValue sameLine = zym_createDispatcher(vm);
    zym_pushRoot(vm, sameLine);
    zym_addOverload(vm, sameLine, sameLine0);
    zym_addOverload(vm, sameLine, sameLine1);
    zym_addOverload(vm, sameLine, sameLine2);

    MOD(newLine,   "newLine()",   u_newLine)
    MOD(separator, "separator()", u_separator)
    MOD(spacing,   "spacing()",   u_spacing)
    MOD(dummy,     "dummy(w, h)", u_dummy)

    ZymValue indent0 = zym_createNativeClosure(vm, "indent()", (void*)u_indent, context);
    zym_pushRoot(vm, indent0);
    ZymValue indent1 = zym_createNativeClosure(vm, "indent(px)", (void*)u_indent, context);
    zym_pushRoot(vm, indent1);
    ZymValue indent = zym_createDispatcher(vm);
    zym_pushRoot(vm, indent);
    zym_addOverload(vm, indent, indent0);
    zym_addOverload(vm, indent, indent1);

    ZymValue unindent0 = zym_createNativeClosure(vm, "unindent()", (void*)u_unindent, context);
    zym_pushRoot(vm, unindent0);
    ZymValue unindent1 = zym_createNativeClosure(vm, "unindent(px)", (void*)u_unindent, context);
    zym_pushRoot(vm, unindent1);
    ZymValue unindent = zym_createDispatcher(vm);
    zym_pushRoot(vm, unindent);
    zym_addOverload(vm, unindent, unindent0);
    zym_addOverload(vm, unindent, unindent1);

    // state queries
    MOD(isItemHovered,      "isItemHovered()",      u_isItemHovered)
    MOD(isItemClicked,      "isItemClicked()",      u_isItemClicked)
    MOD(isItemActive,       "isItemActive()",       u_isItemActive)
    MOD(isItemFocused,      "isItemFocused()",      u_isItemFocused)
    MOD(isWindowHovered,    "isWindowHovered()",    u_isWindowHovered)
    MOD(isWindowFocused,    "isWindowFocused()",    u_isWindowFocused)
    MOD(wantCaptureMouse,   "wantCaptureMouse()",   u_wantCaptureMouse)
    MOD(wantCaptureKeyboard,"wantCaptureKeyboard()",u_wantCaptureKeyboard)

    // misc
    MOD(tooltip, "tooltip(s)", u_tooltip)

    ZymValue progressBar1 = zym_createNativeClosure(vm, "progressBar(frac)",                       (void*)u_progressBar, context);
    zym_pushRoot(vm, progressBar1);
    ZymValue progressBar3 = zym_createNativeClosure(vm, "progressBar(frac, w, h)",                 (void*)u_progressBar, context);
    zym_pushRoot(vm, progressBar3);
    ZymValue progressBar4 = zym_createNativeClosure(vm, "progressBar(frac, w, h, overlay)",        (void*)u_progressBar, context);
    zym_pushRoot(vm, progressBar4);
    ZymValue progressBar = zym_createDispatcher(vm);
    zym_pushRoot(vm, progressBar);
    zym_addOverload(vm, progressBar, progressBar1);
    zym_addOverload(vm, progressBar, progressBar3);
    zym_addOverload(vm, progressBar, progressBar4);

    ZymValue collapsingHeader1 = zym_createNativeClosure(vm, "collapsingHeader(label)",        (void*)u_collapsingHeader, context);
    zym_pushRoot(vm, collapsingHeader1);
    ZymValue collapsingHeader2 = zym_createNativeClosure(vm, "collapsingHeader(label, flags)", (void*)u_collapsingHeader, context);
    zym_pushRoot(vm, collapsingHeader2);
    ZymValue collapsingHeader = zym_createDispatcher(vm);
    zym_pushRoot(vm, collapsingHeader);
    zym_addOverload(vm, collapsingHeader, collapsingHeader1);
    zym_addOverload(vm, collapsingHeader, collapsingHeader2);

    // ----- batch 2: inputs / sliders / drags / combo -----

    // inputInt(label, ref) / inputInt(label, ref, step)
    ZymValue inputInt2 = zym_createNativeClosure(vm, "inputInt(label, ref)",        (void*)u_inputInt, context);
    zym_pushRoot(vm, inputInt2);
    ZymValue inputInt3 = zym_createNativeClosure(vm, "inputInt(label, ref, step)",  (void*)u_inputInt, context);
    zym_pushRoot(vm, inputInt3);
    ZymValue inputInt  = zym_createDispatcher(vm);
    zym_pushRoot(vm, inputInt);
    zym_addOverload(vm, inputInt, inputInt2);
    zym_addOverload(vm, inputInt, inputInt3);

    // inputFloat(label, ref) / (label, ref, step) / (label, ref, step, fmt)
    ZymValue inputFloat2 = zym_createNativeClosure(vm, "inputFloat(label, ref)",            (void*)u_inputFloat, context);
    zym_pushRoot(vm, inputFloat2);
    ZymValue inputFloat3 = zym_createNativeClosure(vm, "inputFloat(label, ref, step)",      (void*)u_inputFloat, context);
    zym_pushRoot(vm, inputFloat3);
    ZymValue inputFloat4 = zym_createNativeClosure(vm, "inputFloat(label, ref, step, fmt)", (void*)u_inputFloat, context);
    zym_pushRoot(vm, inputFloat4);
    ZymValue inputFloat  = zym_createDispatcher(vm);
    zym_pushRoot(vm, inputFloat);
    zym_addOverload(vm, inputFloat, inputFloat2);
    zym_addOverload(vm, inputFloat, inputFloat3);
    zym_addOverload(vm, inputFloat, inputFloat4);

    // sliderInt(label, ref, min, max) / + fmt
    ZymValue sliderInt4 = zym_createNativeClosure(vm, "sliderInt(label, ref, min, max)",      (void*)u_sliderInt, context);
    zym_pushRoot(vm, sliderInt4);
    ZymValue sliderInt5 = zym_createNativeClosure(vm, "sliderInt(label, ref, min, max, fmt)", (void*)u_sliderInt, context);
    zym_pushRoot(vm, sliderInt5);
    ZymValue sliderInt  = zym_createDispatcher(vm);
    zym_pushRoot(vm, sliderInt);
    zym_addOverload(vm, sliderInt, sliderInt4);
    zym_addOverload(vm, sliderInt, sliderInt5);

    // sliderFloat(label, ref, min, max) / + fmt
    ZymValue sliderFloat4 = zym_createNativeClosure(vm, "sliderFloat(label, ref, min, max)",      (void*)u_sliderFloat, context);
    zym_pushRoot(vm, sliderFloat4);
    ZymValue sliderFloat5 = zym_createNativeClosure(vm, "sliderFloat(label, ref, min, max, fmt)", (void*)u_sliderFloat, context);
    zym_pushRoot(vm, sliderFloat5);
    ZymValue sliderFloat  = zym_createDispatcher(vm);
    zym_pushRoot(vm, sliderFloat);
    zym_addOverload(vm, sliderFloat, sliderFloat4);
    zym_addOverload(vm, sliderFloat, sliderFloat5);

    // dragInt(label, ref) / (label, ref, speed) / (label, ref, speed, min, max)
    ZymValue dragInt2 = zym_createNativeClosure(vm, "dragInt(label, ref)",                  (void*)u_dragInt, context);
    zym_pushRoot(vm, dragInt2);
    ZymValue dragInt3 = zym_createNativeClosure(vm, "dragInt(label, ref, speed)",           (void*)u_dragInt, context);
    zym_pushRoot(vm, dragInt3);
    ZymValue dragInt5 = zym_createNativeClosure(vm, "dragInt(label, ref, speed, min, max)", (void*)u_dragInt, context);
    zym_pushRoot(vm, dragInt5);
    ZymValue dragInt  = zym_createDispatcher(vm);
    zym_pushRoot(vm, dragInt);
    zym_addOverload(vm, dragInt, dragInt2);
    zym_addOverload(vm, dragInt, dragInt3);
    zym_addOverload(vm, dragInt, dragInt5);

    // dragFloat(label, ref) / (label, ref, speed) / (label, ref, speed, min, max)
    ZymValue dragFloat2 = zym_createNativeClosure(vm, "dragFloat(label, ref)",                  (void*)u_dragFloat, context);
    zym_pushRoot(vm, dragFloat2);
    ZymValue dragFloat3 = zym_createNativeClosure(vm, "dragFloat(label, ref, speed)",           (void*)u_dragFloat, context);
    zym_pushRoot(vm, dragFloat3);
    ZymValue dragFloat5 = zym_createNativeClosure(vm, "dragFloat(label, ref, speed, min, max)", (void*)u_dragFloat, context);
    zym_pushRoot(vm, dragFloat5);
    ZymValue dragFloat  = zym_createDispatcher(vm);
    zym_pushRoot(vm, dragFloat);
    zym_addOverload(vm, dragFloat, dragFloat2);
    zym_addOverload(vm, dragFloat, dragFloat3);
    zym_addOverload(vm, dragFloat, dragFloat5);

    // inputText(label, buf) / (label, buf, flags)
    ZymValue inputText2 = zym_createNativeClosure(vm, "inputText(label, buf)",        (void*)u_inputText, context);
    zym_pushRoot(vm, inputText2);
    ZymValue inputText3 = zym_createNativeClosure(vm, "inputText(label, buf, flags)", (void*)u_inputText, context);
    zym_pushRoot(vm, inputText3);
    ZymValue inputText  = zym_createDispatcher(vm);
    zym_pushRoot(vm, inputText);
    zym_addOverload(vm, inputText, inputText2);
    zym_addOverload(vm, inputText, inputText3);

    // inputTextMultiline(label, buf, w, h) / (label, buf, w, h, flags)
    ZymValue inputTextMultiline4 = zym_createNativeClosure(vm, "inputTextMultiline(label, buf, w, h)",        (void*)u_inputTextMultiline, context);
    zym_pushRoot(vm, inputTextMultiline4);
    ZymValue inputTextMultiline5 = zym_createNativeClosure(vm, "inputTextMultiline(label, buf, w, h, flags)", (void*)u_inputTextMultiline, context);
    zym_pushRoot(vm, inputTextMultiline5);
    ZymValue inputTextMultiline  = zym_createDispatcher(vm);
    zym_pushRoot(vm, inputTextMultiline);
    zym_addOverload(vm, inputTextMultiline, inputTextMultiline4);
    zym_addOverload(vm, inputTextMultiline, inputTextMultiline5);

    // combo(label, idxRef, items) — flat 3-arg
    MOD(combo, "combo(label, idxRef, items)", u_combo)

    // ----- batch 3: scoped containers -----

    // child(id, body) / child(id, opts, body) / child(id, w, h, border, body)
    ZymValue child2v = zym_createNativeClosure(vm, "child(id, body)",                 (void*)u_child2, context);
    zym_pushRoot(vm, child2v);
    ZymValue child3v = zym_createNativeClosure(vm, "child(id, opts, body)",           (void*)u_child3, context);
    zym_pushRoot(vm, child3v);
    ZymValue child5v = zym_createNativeClosure(vm, "child(id, w, h, border, body)",   (void*)u_child5, context);
    zym_pushRoot(vm, child5v);
    ZymValue child   = zym_createDispatcher(vm);
    zym_pushRoot(vm, child);
    zym_addOverload(vm, child, child2v);
    zym_addOverload(vm, child, child3v);
    zym_addOverload(vm, child, child5v);

    MOD(group,        "group(body)",                 u_group)
    MOD(treeNode,     "treeNode(label, body)",       u_treeNode)
    MOD(disabled,     "disabled(cond, body)",        u_disabled)
    MOD(id,           "id(idValue, body)",           u_id)
    MOD(clip,         "clip(x, y, w, h, body)",      u_clip)
    MOD(tooltipScope, "tooltipScope(body)",          u_tooltipScope)

    // ----- batch 4: tables -----

    // table(id, columns, body) / table(id, columns, flags, body)
    ZymValue table3v = zym_createNativeClosure(vm, "table(id, columns, body)",        (void*)u_table3, context);
    zym_pushRoot(vm, table3v);
    ZymValue table4v = zym_createNativeClosure(vm, "table(id, columns, flags, body)", (void*)u_table4, context);
    zym_pushRoot(vm, table4v);
    ZymValue table   = zym_createDispatcher(vm);
    zym_pushRoot(vm, table);
    zym_addOverload(vm, table, table3v);
    zym_addOverload(vm, table, table4v);

    // tableNextRow() / tableNextRow(minHeight)
    ZymValue tableNextRow0v = zym_createNativeClosure(vm, "tableNextRow()",          (void*)u_tableNextRow0, context);
    zym_pushRoot(vm, tableNextRow0v);
    ZymValue tableNextRow1v = zym_createNativeClosure(vm, "tableNextRow(minHeight)", (void*)u_tableNextRow1, context);
    zym_pushRoot(vm, tableNextRow1v);
    ZymValue tableNextRow   = zym_createDispatcher(vm);
    zym_pushRoot(vm, tableNextRow);
    zym_addOverload(vm, tableNextRow, tableNextRow0v);
    zym_addOverload(vm, tableNextRow, tableNextRow1v);

    MOD(tableNextColumn,    "tableNextColumn()",          u_tableNextColumn)
    MOD(tableSetColumnIndex,"tableSetColumnIndex(idx)",   u_tableSetColumnIndex)

    // tableSetupColumn(label) / (label, flags) / (label, flags, width)
    ZymValue tableSetupColumn1v = zym_createNativeClosure(vm, "tableSetupColumn(label)",               (void*)u_tableSetupColumn1, context);
    zym_pushRoot(vm, tableSetupColumn1v);
    ZymValue tableSetupColumn2v = zym_createNativeClosure(vm, "tableSetupColumn(label, flags)",        (void*)u_tableSetupColumn2, context);
    zym_pushRoot(vm, tableSetupColumn2v);
    ZymValue tableSetupColumn3v = zym_createNativeClosure(vm, "tableSetupColumn(label, flags, width)", (void*)u_tableSetupColumn3, context);
    zym_pushRoot(vm, tableSetupColumn3v);
    ZymValue tableSetupColumn   = zym_createDispatcher(vm);
    zym_pushRoot(vm, tableSetupColumn);
    zym_addOverload(vm, tableSetupColumn, tableSetupColumn1v);
    zym_addOverload(vm, tableSetupColumn, tableSetupColumn2v);
    zym_addOverload(vm, tableSetupColumn, tableSetupColumn3v);

    MOD(tableSetupScrollFreeze,"tableSetupScrollFreeze(cols, rows)", u_tableSetupScrollFreeze)
    MOD(tableHeadersRow,       "tableHeadersRow()",                  u_tableHeadersRow)
    MOD(tableHeader,           "tableHeader(label)",                 u_tableHeader)
    MOD(tableGetRowIndex,      "tableGetRowIndex()",                 u_tableGetRowIndex)
    MOD(tableGetColumnIndex,   "tableGetColumnIndex()",              u_tableGetColumnIndex)
    MOD(tableGetColumnCount,   "tableGetColumnCount()",              u_tableGetColumnCount)

    // columns(count) / (count, id) / (count, id, border)
    ZymValue columns1v = zym_createNativeClosure(vm, "columns(count)",               (void*)u_columns1, context);
    zym_pushRoot(vm, columns1v);
    ZymValue columns2v = zym_createNativeClosure(vm, "columns(count, id)",           (void*)u_columns2, context);
    zym_pushRoot(vm, columns2v);
    ZymValue columns3v = zym_createNativeClosure(vm, "columns(count, id, border)",   (void*)u_columns3, context);
    zym_pushRoot(vm, columns3v);
    ZymValue columns   = zym_createDispatcher(vm);
    zym_pushRoot(vm, columns);
    zym_addOverload(vm, columns, columns1v);
    zym_addOverload(vm, columns, columns2v);
    zym_addOverload(vm, columns, columns3v);

    MOD(nextColumn, "nextColumn()", u_nextColumn)

    // ----- batch 5: popups / menus -----

    // popup(id, body) / popup(id, flags, body)
    ZymValue popup2v = zym_createNativeClosure(vm, "popup(id, body)",        (void*)u_popup2, context);
    zym_pushRoot(vm, popup2v);
    ZymValue popup3v = zym_createNativeClosure(vm, "popup(id, flags, body)", (void*)u_popup3, context);
    zym_pushRoot(vm, popup3v);
    ZymValue popup   = zym_createDispatcher(vm);
    zym_pushRoot(vm, popup);
    zym_addOverload(vm, popup, popup2v);
    zym_addOverload(vm, popup, popup3v);

    // popupModal(name, body) / popupModal(name, flags, body)
    ZymValue popupModal2v = zym_createNativeClosure(vm, "popupModal(name, body)",        (void*)u_popupModal2, context);
    zym_pushRoot(vm, popupModal2v);
    ZymValue popupModal3v = zym_createNativeClosure(vm, "popupModal(name, flags, body)", (void*)u_popupModal3, context);
    zym_pushRoot(vm, popupModal3v);
    ZymValue popupModal   = zym_createDispatcher(vm);
    zym_pushRoot(vm, popupModal);
    zym_addOverload(vm, popupModal, popupModal2v);
    zym_addOverload(vm, popupModal, popupModal3v);

    // openPopup(id) / openPopup(id, flags)
    ZymValue openPopup1v = zym_createNativeClosure(vm, "openPopup(id)",        (void*)u_openPopup1, context);
    zym_pushRoot(vm, openPopup1v);
    ZymValue openPopup2v = zym_createNativeClosure(vm, "openPopup(id, flags)", (void*)u_openPopup2, context);
    zym_pushRoot(vm, openPopup2v);
    ZymValue openPopup   = zym_createDispatcher(vm);
    zym_pushRoot(vm, openPopup);
    zym_addOverload(vm, openPopup, openPopup1v);
    zym_addOverload(vm, openPopup, openPopup2v);

    MOD(closeCurrentPopup, "closeCurrentPopup()", u_closeCurrentPopup)
    MOD(menuBar,           "menuBar(body)",       u_menuBar)
    MOD(mainMenuBar,       "mainMenuBar(body)",   u_mainMenuBar)

    // menu(label, body) / menu(label, enabled, body)
    ZymValue menu2v = zym_createNativeClosure(vm, "menu(label, body)",          (void*)u_menu2, context);
    zym_pushRoot(vm, menu2v);
    ZymValue menu3v = zym_createNativeClosure(vm, "menu(label, enabled, body)", (void*)u_menu3, context);
    zym_pushRoot(vm, menu3v);
    ZymValue menu   = zym_createDispatcher(vm);
    zym_pushRoot(vm, menu);
    zym_addOverload(vm, menu, menu2v);
    zym_addOverload(vm, menu, menu3v);

    // menuItem(label) / (label, shortcut) / (label, shortcut, selected)
    //   / (label, shortcut, selected, enabled)
    ZymValue menuItem1v = zym_createNativeClosure(vm, "menuItem(label)",                              (void*)u_menuItem1, context);
    zym_pushRoot(vm, menuItem1v);
    ZymValue menuItem2v = zym_createNativeClosure(vm, "menuItem(label, shortcut)",                    (void*)u_menuItem2, context);
    zym_pushRoot(vm, menuItem2v);
    ZymValue menuItem3v = zym_createNativeClosure(vm, "menuItem(label, shortcut, selected)",          (void*)u_menuItem3, context);
    zym_pushRoot(vm, menuItem3v);
    ZymValue menuItem4v = zym_createNativeClosure(vm, "menuItem(label, shortcut, selected, enabled)", (void*)u_menuItem4, context);
    zym_pushRoot(vm, menuItem4v);
    ZymValue menuItem   = zym_createDispatcher(vm);
    zym_pushRoot(vm, menuItem);
    zym_addOverload(vm, menuItem, menuItem1v);
    zym_addOverload(vm, menuItem, menuItem2v);
    zym_addOverload(vm, menuItem, menuItem3v);
    zym_addOverload(vm, menuItem, menuItem4v);

    // ----- batch 6: plots / color / drawList / demo -----

    // plotLines / plotHistogram dispatchers (2/3/5-arg)
    ZymValue plotLines2v = zym_createNativeClosure(vm, "plotLines(label, values)",                       (void*)u_plotLines2, context);
    zym_pushRoot(vm, plotLines2v);
    ZymValue plotLines3v = zym_createNativeClosure(vm, "plotLines(label, values, overlay)",              (void*)u_plotLines3, context);
    zym_pushRoot(vm, plotLines3v);
    ZymValue plotLines5v = zym_createNativeClosure(vm, "plotLines(label, values, overlay, min, max)",    (void*)u_plotLines5, context);
    zym_pushRoot(vm, plotLines5v);
    ZymValue plotLines   = zym_createDispatcher(vm);
    zym_pushRoot(vm, plotLines);
    zym_addOverload(vm, plotLines, plotLines2v);
    zym_addOverload(vm, plotLines, plotLines3v);
    zym_addOverload(vm, plotLines, plotLines5v);

    ZymValue plotHist2v = zym_createNativeClosure(vm, "plotHistogram(label, values)",                    (void*)u_plotHist2, context);
    zym_pushRoot(vm, plotHist2v);
    ZymValue plotHist3v = zym_createNativeClosure(vm, "plotHistogram(label, values, overlay)",           (void*)u_plotHist3, context);
    zym_pushRoot(vm, plotHist3v);
    ZymValue plotHist5v = zym_createNativeClosure(vm, "plotHistogram(label, values, overlay, min, max)", (void*)u_plotHist5, context);
    zym_pushRoot(vm, plotHist5v);
    ZymValue plotHistogram = zym_createDispatcher(vm);
    zym_pushRoot(vm, plotHistogram);
    zym_addOverload(vm, plotHistogram, plotHist2v);
    zym_addOverload(vm, plotHistogram, plotHist3v);
    zym_addOverload(vm, plotHistogram, plotHist5v);

    // color widgets
    MOD(colorEdit,   "colorEdit(label, ref)",   u_colorEdit)
    MOD(colorPicker, "colorPicker(label, ref)", u_colorPicker)
    MOD(colorButton, "colorButton(id, color)",  u_colorButton)

    // color packer
    MOD(color, "color(r, g, b, a)", u_color)

    // drawList primitives — flat, operate on the current window's draw list
    MOD(drawLine,           "drawLine(x1, y1, x2, y2, color)",           u_drawLine)
    MOD(drawRect,           "drawRect(x, y, w, h, color)",               u_drawRect)
    MOD(drawRectFilled,     "drawRectFilled(x, y, w, h, color)",         u_drawRectFilled)
    MOD(drawCircle,         "drawCircle(cx, cy, r, color)",              u_drawCircle)
    MOD(drawCircleFilled,   "drawCircleFilled(cx, cy, r, color)",        u_drawCircleFilled)
    MOD(drawText,           "drawText(x, y, color, s)",                  u_drawText)
    MOD(drawTriangle,       "drawTriangle(x1,y1,x2,y2,x3,y3, color)",    u_drawTriangle)
    MOD(drawTriangleFilled, "drawTriangleFilled(x1,y1,x2,y2,x3,y3, c)",  u_drawTriangleFilled)

    // PR 3: DrawList handle factories — return a DrawList map handle
    // bound to the window draw list / viewport background / foreground.
    MOD(drawList,           "drawList()",            u_drawListHandle)
    MOD(backgroundDrawList, "backgroundDrawList()",  u_backgroundDrawList)
    MOD(foregroundDrawList, "foregroundDrawList()",  u_foregroundDrawList)

    // position / state helpers
    MOD(getCursorPos, "getCursorPos()", u_getCursorPos)
    MOD(getMousePos,  "getMousePos()",  u_getMousePos)
    MOD(framerate,    "framerate()",    u_framerate)

    // ----- PR 2c: style stacks + fonts -----
    MOD(withStyleColor, "withStyleColor(map, body)", u_withStyleColor)
    MOD(withStyleVar,   "withStyleVar(map, body)",   u_withStyleVar)
    MOD(withFont,       "withFont(font, body)",      u_withFont)

    // loadFont: 2-arg (src, sizePx) or 3-arg (src, sizePx, opts). `src`
    // is either a filesystem path (string) or a Buffer of TTF/OTF bytes.
    ZymValue loadFont2v = zym_createNativeClosure(vm, "loadFont(src, sizePx)",       (void*)u_loadFont2, context);
    zym_pushRoot(vm, loadFont2v);
    ZymValue loadFont3v = zym_createNativeClosure(vm, "loadFont(src, sizePx, opts)", (void*)u_loadFont3, context);
    zym_pushRoot(vm, loadFont3v);
    ZymValue loadFont   = zym_createDispatcher(vm);
    zym_pushRoot(vm, loadFont);
    zym_addOverload(vm, loadFont, loadFont2v);
    zym_addOverload(vm, loadFont, loadFont3v);

    MOD(defaultFont,    "defaultFont()",             u_defaultFont)

    // ----- PR 2d: widget parity batch -----
    MOD(tabBar,          "tabBar(id, body)",            u_tabBar)
    MOD(tabItem,         "tabItem(label, body)",        u_tabItem)
    MOD(tabItemButton,   "tabItemButton(label)",        u_tabItemButton)

    // listBox: scoped (label, body) and flat (label, idxRef, items)
    ZymValue listBox2v = zym_createNativeClosure(vm, "listBox(label, body)",          (void*)u_listBox,     context);
    zym_pushRoot(vm, listBox2v);
    ZymValue listBox3v = zym_createNativeClosure(vm, "listBox(label, idxRef, items)", (void*)u_listBoxFlat, context);
    zym_pushRoot(vm, listBox3v);
    ZymValue listBox   = zym_createDispatcher(vm);
    zym_pushRoot(vm, listBox);
    zym_addOverload(vm, listBox, listBox2v);
    zym_addOverload(vm, listBox, listBox3v);

    MOD(comboScope,      "comboScope(label, preview, body)", u_comboScope)
    MOD(separatorText,   "separatorText(s)",                u_separatorText)
    MOD(textLink,        "textLink(s)",                     u_textLink)
    MOD(textLinkOpenURL, "textLinkOpenURL(label, url)",     u_textLinkOpenURL)
    MOD(checkboxFlags,   "checkboxFlags(label, ref, flag)", u_checkboxFlags)

    // vector slider / drag / input
    MOD(sliderFloat2v, "sliderFloat2(label, ref, min, max)", u_sliderFloat2)
    MOD(sliderFloat3v, "sliderFloat3(label, ref, min, max)", u_sliderFloat3)
    MOD(sliderFloat4v, "sliderFloat4(label, ref, min, max)", u_sliderFloat4)
    MOD(sliderInt2v,   "sliderInt2(label, ref, min, max)",   u_sliderInt2)
    MOD(sliderInt3v,   "sliderInt3(label, ref, min, max)",   u_sliderInt3)
    MOD(sliderInt4v,   "sliderInt4(label, ref, min, max)",   u_sliderInt4)
    MOD(dragFloat2v,   "dragFloat2(label, ref, speed, min, max)", u_dragFloat2)
    MOD(dragFloat3v,   "dragFloat3(label, ref, speed, min, max)", u_dragFloat3)
    MOD(dragFloat4v,   "dragFloat4(label, ref, speed, min, max)", u_dragFloat4)
    MOD(dragInt2v,     "dragInt2(label, ref, speed, min, max)",   u_dragInt2)
    MOD(dragInt3v,     "dragInt3(label, ref, speed, min, max)",   u_dragInt3)
    MOD(dragInt4v,     "dragInt4(label, ref, speed, min, max)",   u_dragInt4)
    MOD(inputFloat2v,  "inputFloat2(label, ref)",           u_inputFloat2)
    MOD(inputFloat3v,  "inputFloat3(label, ref)",           u_inputFloat3)
    MOD(inputFloat4v,  "inputFloat4(label, ref)",           u_inputFloat4)
    MOD(inputInt2v,    "inputInt2(label, ref)",             u_inputInt2)
    MOD(inputInt3v,    "inputInt3(label, ref)",             u_inputInt3)
    MOD(inputInt4v,    "inputInt4(label, ref)",             u_inputInt4)

    MOD(sliderAngle,  "sliderAngle(label, ref, degMin, degMax)",     u_sliderAngle)
    MOD(vSliderFloat, "vSliderFloat(label, w, h, ref, min, max)",    u_vSliderFloat)
    MOD(vSliderInt,   "vSliderInt(label, w, h, ref, min, max)",      u_vSliderInt)

    // scrolling
    MOD(getScrollX,        "getScrollX()",                 u_getScrollX)
    MOD(getScrollY,        "getScrollY()",                 u_getScrollY)
    MOD(getScrollMaxX,     "getScrollMaxX()",              u_getScrollMaxX)
    MOD(getScrollMaxY,     "getScrollMaxY()",              u_getScrollMaxY)
    MOD(setScrollX,        "setScrollX(x)",                u_setScrollX)
    MOD(setScrollY,        "setScrollY(y)",                u_setScrollY)
    MOD(setScrollHereX,    "setScrollHereX(centerRatio)",  u_setScrollHereX)
    MOD(setScrollHereY,    "setScrollHereY(centerRatio)",  u_setScrollHereY)
    MOD(setScrollFromPosX, "setScrollFromPosX(localX, centerRatio)", u_setScrollFromPosX)
    MOD(setScrollFromPosY, "setScrollFromPosY(localY, centerRatio)", u_setScrollFromPosY)

    // window state
    MOD(isWindowAppearing, "isWindowAppearing()", u_isWindowAppearing)
    MOD(isWindowCollapsed, "isWindowCollapsed()", u_isWindowCollapsed)
    MOD(getWindowPos,      "getWindowPos()",      u_getWindowPos)
    MOD(getWindowSize,     "getWindowSize()",     u_getWindowSize)
    MOD(getWindowWidth,    "getWindowWidth()",    u_getWindowWidth)
    MOD(getWindowHeight,   "getWindowHeight()",   u_getWindowHeight)

    // setNextWindow*
    MOD(setNextWindowFocus,       "setNextWindowFocus()",            u_setNextWindowFocus)
    MOD(setNextWindowBgAlpha,     "setNextWindowBgAlpha(alpha)",     u_setNextWindowBgAlpha)
    MOD(setNextWindowContentSize, "setNextWindowContentSize(w, h)",  u_setNextWindowContentSize)
    MOD(setNextWindowCollapsed,   "setNextWindowCollapsed(c)",       u_setNextWindowCollapsed)
    MOD(setNextWindowScroll,      "setNextWindowScroll(x, y)",       u_setNextWindowScroll)

    // item queries
    MOD(isItemVisible,              "isItemVisible()",              u_isItemVisible)
    MOD(isItemEdited,               "isItemEdited()",               u_isItemEdited)
    MOD(isItemActivated,            "isItemActivated()",            u_isItemActivated)
    MOD(isItemDeactivated,          "isItemDeactivated()",          u_isItemDeactivated)
    MOD(isItemDeactivatedAfterEdit, "isItemDeactivatedAfterEdit()", u_isItemDeactivatedAfterEdit)
    MOD(isItemToggledOpen,          "isItemToggledOpen()",          u_isItemToggledOpen)
    MOD(isAnyItemHovered,           "isAnyItemHovered()",           u_isAnyItemHovered)
    MOD(isAnyItemActive,            "isAnyItemActive()",            u_isAnyItemActive)
    MOD(isAnyItemFocused,           "isAnyItemFocused()",           u_isAnyItemFocused)
    MOD(getItemRectMin,             "getItemRectMin()",             u_getItemRectMin)
    MOD(getItemRectMax,             "getItemRectMax()",             u_getItemRectMax)
    MOD(getItemRectSize,            "getItemRectSize()",            u_getItemRectSize)

    // mouse queries
    MOD(isMouseDown,          "isMouseDown(button)",          u_isMouseDown)
    MOD(isMouseClicked,       "isMouseClicked(button)",       u_isMouseClicked)
    MOD(isMouseDoubleClicked, "isMouseDoubleClicked(button)", u_isMouseDoubleClicked)
    MOD(isMouseReleased,      "isMouseReleased(button)",      u_isMouseReleased)
    MOD(isMouseDragging,      "isMouseDragging(button, threshold)", u_isMouseDragging)
    MOD(getMouseDragDelta,    "getMouseDragDelta(button)",    u_getMouseDragDelta)
    MOD(resetMouseDragDelta,  "resetMouseDragDelta(button)",  u_resetMouseDragDelta)
    MOD(getMouseClickedCount, "getMouseClickedCount(button)", u_getMouseClickedCount)

    // keyboard queries
    MOD(isKeyDown,                       "isKeyDown(key)",                       u_isKeyDown)
    MOD(isKeyPressed,                    "isKeyPressed(key, repeat)",            u_isKeyPressed)
    MOD(isKeyReleased,                   "isKeyReleased(key)",                   u_isKeyReleased)
    MOD(getKeyPressedAmount,             "getKeyPressedAmount(key, rd, rr)",     u_getKeyPressedAmount)
    MOD(setNextFrameWantCaptureKeyboard, "setNextFrameWantCaptureKeyboard(b)",   u_setNextFrameWantCaptureKeyboard)
    MOD(setNextFrameWantCaptureMouse,    "setNextFrameWantCaptureMouse(b)",      u_setNextFrameWantCaptureMouse)

    // clipboard
    MOD(getClipboardText, "getClipboardText()",  u_getClipboardText)
    MOD(setClipboardText, "setClipboardText(s)", u_setClipboardText)

    // popup context
    MOD(popupContextItem,   "popupContextItem(id, body)",   u_popupContextItem)
    MOD(popupContextWindow, "popupContextWindow(id, body)", u_popupContextWindow)

    // setNextItem / pushItemWidth / calcTextSize / style getters
    MOD(setNextItemWidth,        "setNextItemWidth(w)",        u_setNextItemWidth)
    MOD(setNextItemOpen,         "setNextItemOpen(open)",      u_setNextItemOpen)
    MOD(setNextItemAllowOverlap, "setNextItemAllowOverlap()",  u_setNextItemAllowOverlap)
    MOD(pushItemWidth,           "pushItemWidth(w)",           u_pushItemWidth)
    MOD(popItemWidth,            "popItemWidth()",             u_popItemWidth)
    MOD(setKeyboardFocusHere,    "setKeyboardFocusHere(offset)", u_setKeyboardFocusHere)
    MOD(setItemDefaultFocus,     "setItemDefaultFocus()",      u_setItemDefaultFocus)
    MOD(calcTextSize,            "calcTextSize(s)",            u_calcTextSize)
    MOD(getStyleColorVec4,       "getStyleColorVec4(name)",    u_getStyleColorVec4)
    MOD(getFontSize,             "getFontSize()",              u_getFontSize)
    MOD(getTextLineHeight,       "getTextLineHeight()",        u_getTextLineHeight)
    MOD(getTextLineHeightWithSpacing, "getTextLineHeightWithSpacing()", u_getTextLineHeightWithSpacing)
    MOD(getFrameHeight,          "getFrameHeight()",           u_getFrameHeight)
    MOD(getFrameHeightWithSpacing,"getFrameHeightWithSpacing()", u_getFrameHeightWithSpacing)
    MOD(getContentRegionAvail,   "getContentRegionAvail()",    u_getContentRegionAvail)
    MOD(setCursorPos,            "setCursorPos(x, y)",         u_setCursorPos)
    MOD(setCursorScreenPos,      "setCursorScreenPos(x, y)",   u_setCursorScreenPos)

    // PR 2e — drag-and-drop
    ZymValue beginDragDropSource0 = zym_createNativeClosure(vm, "beginDragDropSource()", (void*)u_beginDragDropSource, context);
    zym_pushRoot(vm, beginDragDropSource0);
    ZymValue beginDragDropSource1 = zym_createNativeClosure(vm, "beginDragDropSource(flags)", (void*)u_beginDragDropSource, context);
    zym_pushRoot(vm, beginDragDropSource1);
    ZymValue beginDragDropSource = zym_createDispatcher(vm);
    zym_pushRoot(vm, beginDragDropSource);
    zym_addOverload(vm, beginDragDropSource, beginDragDropSource0);
    zym_addOverload(vm, beginDragDropSource, beginDragDropSource1);

    MOD(endDragDropSource,    "endDragDropSource()",                 u_endDragDropSource)
    MOD(setDragDropPayload,   "setDragDropPayload(type, data)",      u_setDragDropPayload)
    MOD(beginDragDropTarget,  "beginDragDropTarget()",               u_beginDragDropTarget)
    MOD(endDragDropTarget,    "endDragDropTarget()",                 u_endDragDropTarget)

    ZymValue acceptDragDropPayload1 = zym_createNativeClosure(vm, "acceptDragDropPayload(type)",         (void*)u_acceptDragDropPayload, context);
    zym_pushRoot(vm, acceptDragDropPayload1);
    ZymValue acceptDragDropPayload2 = zym_createNativeClosure(vm, "acceptDragDropPayload(type, flags)",  (void*)u_acceptDragDropPayload, context);
    zym_pushRoot(vm, acceptDragDropPayload2);
    ZymValue acceptDragDropPayload = zym_createDispatcher(vm);
    zym_pushRoot(vm, acceptDragDropPayload);
    zym_addOverload(vm, acceptDragDropPayload, acceptDragDropPayload1);
    zym_addOverload(vm, acceptDragDropPayload, acceptDragDropPayload2);

    MOD(getDragDropPayload,   "getDragDropPayload()",                u_getDragDropPayload)

    // PR 2e — advanced table sort
    MOD(tableGetSortSpecs,    "tableGetSortSpecs()",                 u_tableGetSortSpecs)

    // PR 2e — font atlas internals
    MOD(addFontDefault,       "addFontDefault()",                    u_addFontDefault)
    MOD(clearFonts,           "clearFonts()",                        u_clearFonts)
    MOD(getFontTexSize,       "getFontTexSize()",                    u_getFontTexSize)
    MOD(getFontAtlasFlags,    "getFontAtlasFlags()",                 u_getFontAtlasFlags)
    MOD(setFontAtlasFlags,    "setFontAtlasFlags(flags)",            u_setFontAtlasFlags)
    MOD(getFontCount,         "getFontCount()",                      u_getFontCount)
    MOD(getFontAt,            "getFontAt(i)",                        u_getFontAt)

#undef MOD

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

    zym_mapSet(vm, obj, "frame",             frame);
    zym_mapSet(vm, obj, "window",            window);
    zym_mapSet(vm, obj, "setNextWindowPos",  setNextWindowPos);
    zym_mapSet(vm, obj, "setNextWindowSize", setNextWindowSize);
    zym_mapSet(vm, obj, "lastError",         lastError);
    zym_mapSet(vm, obj, "silent",            silent);
    zym_mapSet(vm, obj, "setErrorLogging",   setErrorLogging);
    zym_mapSet(vm, obj, "setErrorTooltip",   setErrorTooltip);

    // ImGuiWindowFlags constants — exposed as plain integers so scripts
    // can OR them together for `UI.window(name, flags, body)`. Subset
    // most useful for full-pane / chromeless apps.
    zym_mapSet(vm, obj, "WINDOW_NONE",                  zym_newNumber(0));
    zym_mapSet(vm, obj, "WINDOW_NO_TITLE_BAR",          zym_newNumber(1 << 0));
    zym_mapSet(vm, obj, "WINDOW_NO_RESIZE",             zym_newNumber(1 << 1));
    zym_mapSet(vm, obj, "WINDOW_NO_MOVE",               zym_newNumber(1 << 2));
    zym_mapSet(vm, obj, "WINDOW_NO_SCROLLBAR",          zym_newNumber(1 << 3));
    zym_mapSet(vm, obj, "WINDOW_NO_SCROLL_WITH_MOUSE",  zym_newNumber(1 << 4));
    zym_mapSet(vm, obj, "WINDOW_NO_COLLAPSE",           zym_newNumber(1 << 5));
    zym_mapSet(vm, obj, "WINDOW_ALWAYS_AUTO_RESIZE",    zym_newNumber(1 << 6));
    zym_mapSet(vm, obj, "WINDOW_NO_BACKGROUND",         zym_newNumber(1 << 7));
    zym_mapSet(vm, obj, "WINDOW_NO_SAVED_SETTINGS",     zym_newNumber(1 << 8));
    zym_mapSet(vm, obj, "WINDOW_NO_MOUSE_INPUTS",       zym_newNumber(1 << 9));
    zym_mapSet(vm, obj, "WINDOW_MENU_BAR",              zym_newNumber(1 << 10));
    zym_mapSet(vm, obj, "WINDOW_HORIZONTAL_SCROLLBAR",  zym_newNumber(1 << 11));
    zym_mapSet(vm, obj, "WINDOW_NO_FOCUS_ON_APPEARING", zym_newNumber(1 << 12));
    zym_mapSet(vm, obj, "WINDOW_NO_BRING_TO_FRONT_ON_FOCUS", zym_newNumber(1 << 13));
    zym_mapSet(vm, obj, "WINDOW_NO_DECORATION",
        zym_newNumber((1<<0)|(1<<1)|(1<<3)|(1<<5))); // NoTitleBar|NoResize|NoScrollbar|NoCollapse
    zym_mapSet(vm, obj, "WINDOW_NO_INPUTS",
        zym_newNumber((1<<9)|(1<<16)|(1<<17))); // NoMouseInputs|NoNavInputs|NoNavFocus (best-effort)

    zym_mapSet(vm, obj, "text",         text);
    zym_mapSet(vm, obj, "textColored",  textColored);
    zym_mapSet(vm, obj, "textWrapped",  textWrapped);
    zym_mapSet(vm, obj, "textDisabled", textDisabled);
    zym_mapSet(vm, obj, "labelText",    labelText);
    zym_mapSet(vm, obj, "bulletText",   bulletText);
    zym_mapSet(vm, obj, "bullet",       bullet);

    zym_mapSet(vm, obj, "button",          button);
    zym_mapSet(vm, obj, "smallButton",     smallButton);
    zym_mapSet(vm, obj, "invisibleButton", invisibleButton);
    zym_mapSet(vm, obj, "arrowButton",     arrowButton);

    zym_mapSet(vm, obj, "checkbox",    checkbox);
    zym_mapSet(vm, obj, "radioButton", radioButton);
    zym_mapSet(vm, obj, "selectable",  selectable);

    zym_mapSet(vm, obj, "sameLine",  sameLine);
    zym_mapSet(vm, obj, "newLine",   newLine);
    zym_mapSet(vm, obj, "separator", separator);
    zym_mapSet(vm, obj, "spacing",   spacing);
    zym_mapSet(vm, obj, "dummy",     dummy);
    zym_mapSet(vm, obj, "indent",    indent);
    zym_mapSet(vm, obj, "unindent",  unindent);

    zym_mapSet(vm, obj, "isItemHovered",       isItemHovered);
    zym_mapSet(vm, obj, "isItemClicked",       isItemClicked);
    zym_mapSet(vm, obj, "isItemActive",        isItemActive);
    zym_mapSet(vm, obj, "isItemFocused",       isItemFocused);
    zym_mapSet(vm, obj, "isWindowHovered",     isWindowHovered);
    zym_mapSet(vm, obj, "isWindowFocused",     isWindowFocused);
    zym_mapSet(vm, obj, "wantCaptureMouse",    wantCaptureMouse);
    zym_mapSet(vm, obj, "wantCaptureKeyboard", wantCaptureKeyboard);

    zym_mapSet(vm, obj, "tooltip",          tooltip);
    zym_mapSet(vm, obj, "progressBar",      progressBar);
    zym_mapSet(vm, obj, "collapsingHeader", collapsingHeader);

    // batch 2
    zym_mapSet(vm, obj, "inputInt",            inputInt);
    zym_mapSet(vm, obj, "inputFloat",          inputFloat);
    zym_mapSet(vm, obj, "sliderInt",           sliderInt);
    zym_mapSet(vm, obj, "sliderFloat",         sliderFloat);
    zym_mapSet(vm, obj, "dragInt",             dragInt);
    zym_mapSet(vm, obj, "dragFloat",           dragFloat);
    zym_mapSet(vm, obj, "inputText",           inputText);
    zym_mapSet(vm, obj, "inputTextMultiline",  inputTextMultiline);
    zym_mapSet(vm, obj, "combo",               combo);

    // batch 3
    zym_mapSet(vm, obj, "child",        child);
    zym_mapSet(vm, obj, "group",        group);
    zym_mapSet(vm, obj, "treeNode",     treeNode);
    zym_mapSet(vm, obj, "disabled",     disabled);
    zym_mapSet(vm, obj, "id",           id);
    zym_mapSet(vm, obj, "clip",         clip);
    zym_mapSet(vm, obj, "tooltipScope", tooltipScope);

    // batch 4
    zym_mapSet(vm, obj, "table",                  table);
    zym_mapSet(vm, obj, "tableNextRow",           tableNextRow);
    zym_mapSet(vm, obj, "tableNextColumn",        tableNextColumn);
    zym_mapSet(vm, obj, "tableSetColumnIndex",    tableSetColumnIndex);
    zym_mapSet(vm, obj, "tableSetupColumn",       tableSetupColumn);
    zym_mapSet(vm, obj, "tableSetupScrollFreeze", tableSetupScrollFreeze);
    zym_mapSet(vm, obj, "tableHeadersRow",        tableHeadersRow);
    zym_mapSet(vm, obj, "tableHeader",            tableHeader);
    zym_mapSet(vm, obj, "tableGetRowIndex",       tableGetRowIndex);
    zym_mapSet(vm, obj, "tableGetColumnIndex",    tableGetColumnIndex);
    zym_mapSet(vm, obj, "tableGetColumnCount",    tableGetColumnCount);
    zym_mapSet(vm, obj, "columns",                columns);
    zym_mapSet(vm, obj, "nextColumn",             nextColumn);

    // batch 5
    zym_mapSet(vm, obj, "popup",             popup);
    zym_mapSet(vm, obj, "popupModal",        popupModal);
    zym_mapSet(vm, obj, "openPopup",         openPopup);
    zym_mapSet(vm, obj, "closeCurrentPopup", closeCurrentPopup);
    zym_mapSet(vm, obj, "menuBar",           menuBar);
    zym_mapSet(vm, obj, "mainMenuBar",       mainMenuBar);
    zym_mapSet(vm, obj, "menu",              menu);
    zym_mapSet(vm, obj, "menuItem",          menuItem);

    // batch 6
    zym_mapSet(vm, obj, "plotLines",         plotLines);
    zym_mapSet(vm, obj, "plotHistogram",     plotHistogram);
    zym_mapSet(vm, obj, "colorEdit",         colorEdit);
    zym_mapSet(vm, obj, "colorPicker",       colorPicker);
    zym_mapSet(vm, obj, "colorButton",       colorButton);
    zym_mapSet(vm, obj, "color",             color);
    zym_mapSet(vm, obj, "drawLine",          drawLine);
    zym_mapSet(vm, obj, "drawRect",          drawRect);
    zym_mapSet(vm, obj, "drawRectFilled",    drawRectFilled);
    zym_mapSet(vm, obj, "drawCircle",        drawCircle);
    zym_mapSet(vm, obj, "drawCircleFilled",  drawCircleFilled);
    zym_mapSet(vm, obj, "drawText",          drawText);
    zym_mapSet(vm, obj, "drawTriangle",      drawTriangle);
    zym_mapSet(vm, obj, "drawTriangleFilled",drawTriangleFilled);
    zym_mapSet(vm, obj, "drawList",           drawList);
    zym_mapSet(vm, obj, "backgroundDrawList", backgroundDrawList);
    zym_mapSet(vm, obj, "foregroundDrawList", foregroundDrawList);

    // ImDrawFlags_* constants for AddRect / AddRectFilled / AddPolyline /
    // PathStroke / PathRect rounding-corner and closed-shape selection.
    zym_mapSet(vm, obj, "DRAW_NONE",                       zym_newNumber(0));
    zym_mapSet(vm, obj, "DRAW_ROUND_CORNERS_TOP_LEFT",     zym_newNumber(1 << 4));
    zym_mapSet(vm, obj, "DRAW_ROUND_CORNERS_TOP_RIGHT",    zym_newNumber(1 << 5));
    zym_mapSet(vm, obj, "DRAW_ROUND_CORNERS_BOTTOM_LEFT",  zym_newNumber(1 << 6));
    zym_mapSet(vm, obj, "DRAW_ROUND_CORNERS_BOTTOM_RIGHT", zym_newNumber(1 << 7));
    zym_mapSet(vm, obj, "DRAW_ROUND_CORNERS_NONE",         zym_newNumber(1 << 8));
    zym_mapSet(vm, obj, "DRAW_ROUND_CORNERS_TOP",          zym_newNumber((1 << 4) | (1 << 5)));
    zym_mapSet(vm, obj, "DRAW_ROUND_CORNERS_BOTTOM",       zym_newNumber((1 << 6) | (1 << 7)));
    zym_mapSet(vm, obj, "DRAW_ROUND_CORNERS_LEFT",         zym_newNumber((1 << 4) | (1 << 6)));
    zym_mapSet(vm, obj, "DRAW_ROUND_CORNERS_RIGHT",        zym_newNumber((1 << 5) | (1 << 7)));
    zym_mapSet(vm, obj, "DRAW_ROUND_CORNERS_ALL",          zym_newNumber((1 << 4) | (1 << 5) | (1 << 6) | (1 << 7)));
    zym_mapSet(vm, obj, "DRAW_CLOSED",                     zym_newNumber(1 << 9));
    zym_mapSet(vm, obj, "getCursorPos",      getCursorPos);
    zym_mapSet(vm, obj, "getMousePos",       getMousePos);
    zym_mapSet(vm, obj, "framerate",         framerate);

    // PR 2c: style stacks + fonts
    zym_mapSet(vm, obj, "withStyleColor",    withStyleColor);
    zym_mapSet(vm, obj, "withStyleVar",      withStyleVar);
    zym_mapSet(vm, obj, "withFont",          withFont);
    zym_mapSet(vm, obj, "loadFont",          loadFont);
    zym_mapSet(vm, obj, "defaultFont",       defaultFont);

    // PR 2d: widget parity batch
    zym_mapSet(vm, obj, "tabBar",          tabBar);
    zym_mapSet(vm, obj, "tabItem",         tabItem);
    zym_mapSet(vm, obj, "tabItemButton",   tabItemButton);
    zym_mapSet(vm, obj, "listBox",         listBox);
    zym_mapSet(vm, obj, "comboScope",      comboScope);
    zym_mapSet(vm, obj, "separatorText",   separatorText);
    zym_mapSet(vm, obj, "textLink",        textLink);
    zym_mapSet(vm, obj, "textLinkOpenURL", textLinkOpenURL);
    zym_mapSet(vm, obj, "checkboxFlags",   checkboxFlags);
    zym_mapSet(vm, obj, "sliderFloat2",    sliderFloat2v);
    zym_mapSet(vm, obj, "sliderFloat3",    sliderFloat3v);
    zym_mapSet(vm, obj, "sliderFloat4",    sliderFloat4v);
    zym_mapSet(vm, obj, "sliderInt2",      sliderInt2v);
    zym_mapSet(vm, obj, "sliderInt3",      sliderInt3v);
    zym_mapSet(vm, obj, "sliderInt4",      sliderInt4v);
    zym_mapSet(vm, obj, "dragFloat2",      dragFloat2v);
    zym_mapSet(vm, obj, "dragFloat3",      dragFloat3v);
    zym_mapSet(vm, obj, "dragFloat4",      dragFloat4v);
    zym_mapSet(vm, obj, "dragInt2",        dragInt2v);
    zym_mapSet(vm, obj, "dragInt3",        dragInt3v);
    zym_mapSet(vm, obj, "dragInt4",        dragInt4v);
    zym_mapSet(vm, obj, "inputFloat2",     inputFloat2v);
    zym_mapSet(vm, obj, "inputFloat3",     inputFloat3v);
    zym_mapSet(vm, obj, "inputFloat4",     inputFloat4v);
    zym_mapSet(vm, obj, "inputInt2",       inputInt2v);
    zym_mapSet(vm, obj, "inputInt3",       inputInt3v);
    zym_mapSet(vm, obj, "inputInt4",       inputInt4v);
    zym_mapSet(vm, obj, "sliderAngle",     sliderAngle);
    zym_mapSet(vm, obj, "vSliderFloat",    vSliderFloat);
    zym_mapSet(vm, obj, "vSliderInt",      vSliderInt);
    zym_mapSet(vm, obj, "getScrollX",      getScrollX);
    zym_mapSet(vm, obj, "getScrollY",      getScrollY);
    zym_mapSet(vm, obj, "getScrollMaxX",   getScrollMaxX);
    zym_mapSet(vm, obj, "getScrollMaxY",   getScrollMaxY);
    zym_mapSet(vm, obj, "setScrollX",      setScrollX);
    zym_mapSet(vm, obj, "setScrollY",      setScrollY);
    zym_mapSet(vm, obj, "setScrollHereX",  setScrollHereX);
    zym_mapSet(vm, obj, "setScrollHereY",  setScrollHereY);
    zym_mapSet(vm, obj, "setScrollFromPosX", setScrollFromPosX);
    zym_mapSet(vm, obj, "setScrollFromPosY", setScrollFromPosY);
    zym_mapSet(vm, obj, "isWindowAppearing", isWindowAppearing);
    zym_mapSet(vm, obj, "isWindowCollapsed", isWindowCollapsed);
    zym_mapSet(vm, obj, "getWindowPos",      getWindowPos);
    zym_mapSet(vm, obj, "getWindowSize",     getWindowSize);
    zym_mapSet(vm, obj, "getWindowWidth",    getWindowWidth);
    zym_mapSet(vm, obj, "getWindowHeight",   getWindowHeight);
    zym_mapSet(vm, obj, "setNextWindowFocus",       setNextWindowFocus);
    zym_mapSet(vm, obj, "setNextWindowBgAlpha",     setNextWindowBgAlpha);
    zym_mapSet(vm, obj, "setNextWindowContentSize", setNextWindowContentSize);
    zym_mapSet(vm, obj, "setNextWindowCollapsed",   setNextWindowCollapsed);
    zym_mapSet(vm, obj, "setNextWindowScroll",      setNextWindowScroll);
    zym_mapSet(vm, obj, "isItemVisible",              isItemVisible);
    zym_mapSet(vm, obj, "isItemEdited",               isItemEdited);
    zym_mapSet(vm, obj, "isItemActivated",            isItemActivated);
    zym_mapSet(vm, obj, "isItemDeactivated",          isItemDeactivated);
    zym_mapSet(vm, obj, "isItemDeactivatedAfterEdit", isItemDeactivatedAfterEdit);
    zym_mapSet(vm, obj, "isItemToggledOpen",          isItemToggledOpen);
    zym_mapSet(vm, obj, "isAnyItemHovered",           isAnyItemHovered);
    zym_mapSet(vm, obj, "isAnyItemActive",            isAnyItemActive);
    zym_mapSet(vm, obj, "isAnyItemFocused",           isAnyItemFocused);
    zym_mapSet(vm, obj, "getItemRectMin",             getItemRectMin);
    zym_mapSet(vm, obj, "getItemRectMax",             getItemRectMax);
    zym_mapSet(vm, obj, "getItemRectSize",            getItemRectSize);
    zym_mapSet(vm, obj, "isMouseDown",          isMouseDown);
    zym_mapSet(vm, obj, "isMouseClicked",       isMouseClicked);
    zym_mapSet(vm, obj, "isMouseDoubleClicked", isMouseDoubleClicked);
    zym_mapSet(vm, obj, "isMouseReleased",      isMouseReleased);
    zym_mapSet(vm, obj, "isMouseDragging",      isMouseDragging);
    zym_mapSet(vm, obj, "getMouseDragDelta",    getMouseDragDelta);
    zym_mapSet(vm, obj, "resetMouseDragDelta",  resetMouseDragDelta);
    zym_mapSet(vm, obj, "getMouseClickedCount", getMouseClickedCount);
    zym_mapSet(vm, obj, "isKeyDown",                       isKeyDown);
    zym_mapSet(vm, obj, "isKeyPressed",                    isKeyPressed);
    zym_mapSet(vm, obj, "isKeyReleased",                   isKeyReleased);
    zym_mapSet(vm, obj, "getKeyPressedAmount",             getKeyPressedAmount);
    zym_mapSet(vm, obj, "setNextFrameWantCaptureKeyboard", setNextFrameWantCaptureKeyboard);
    zym_mapSet(vm, obj, "setNextFrameWantCaptureMouse",    setNextFrameWantCaptureMouse);
    zym_mapSet(vm, obj, "getClipboardText",   getClipboardText);
    zym_mapSet(vm, obj, "setClipboardText",   setClipboardText);
    zym_mapSet(vm, obj, "popupContextItem",   popupContextItem);
    zym_mapSet(vm, obj, "popupContextWindow", popupContextWindow);
    zym_mapSet(vm, obj, "setNextItemWidth",        setNextItemWidth);
    zym_mapSet(vm, obj, "setNextItemOpen",         setNextItemOpen);
    zym_mapSet(vm, obj, "setNextItemAllowOverlap", setNextItemAllowOverlap);
    zym_mapSet(vm, obj, "pushItemWidth",           pushItemWidth);
    zym_mapSet(vm, obj, "popItemWidth",            popItemWidth);
    zym_mapSet(vm, obj, "setKeyboardFocusHere",    setKeyboardFocusHere);
    zym_mapSet(vm, obj, "setItemDefaultFocus",     setItemDefaultFocus);
    zym_mapSet(vm, obj, "calcTextSize",            calcTextSize);
    zym_mapSet(vm, obj, "getStyleColorVec4",       getStyleColorVec4);
    zym_mapSet(vm, obj, "getFontSize",             getFontSize);
    zym_mapSet(vm, obj, "getTextLineHeight",       getTextLineHeight);
    zym_mapSet(vm, obj, "getTextLineHeightWithSpacing", getTextLineHeightWithSpacing);
    zym_mapSet(vm, obj, "getFrameHeight",          getFrameHeight);
    zym_mapSet(vm, obj, "getFrameHeightWithSpacing", getFrameHeightWithSpacing);
    zym_mapSet(vm, obj, "getContentRegionAvail",   getContentRegionAvail);
    zym_mapSet(vm, obj, "setCursorPos",            setCursorPos);
    zym_mapSet(vm, obj, "setCursorScreenPos",      setCursorScreenPos);

    // PR 2e — drag-and-drop
    zym_mapSet(vm, obj, "beginDragDropSource",  beginDragDropSource);
    zym_mapSet(vm, obj, "endDragDropSource",    endDragDropSource);
    zym_mapSet(vm, obj, "setDragDropPayload",   setDragDropPayload);
    zym_mapSet(vm, obj, "beginDragDropTarget",  beginDragDropTarget);
    zym_mapSet(vm, obj, "endDragDropTarget",    endDragDropTarget);
    zym_mapSet(vm, obj, "acceptDragDropPayload",acceptDragDropPayload);
    zym_mapSet(vm, obj, "getDragDropPayload",   getDragDropPayload);
    // ImGuiDragDropFlags constants — most useful subset.
    zym_mapSet(vm, obj, "DND_SRC_NO_PREVIEW_TOOLTIP",     zym_newNumber(1 << 0));
    zym_mapSet(vm, obj, "DND_SRC_NO_DISABLE_HOVER",       zym_newNumber(1 << 1));
    zym_mapSet(vm, obj, "DND_SRC_NO_HOLD_TO_OPEN_OTHERS", zym_newNumber(1 << 2));
    zym_mapSet(vm, obj, "DND_SRC_ALLOW_NULL_ID",          zym_newNumber(1 << 3));
    zym_mapSet(vm, obj, "DND_SRC_EXTERN",                 zym_newNumber(1 << 4));
    zym_mapSet(vm, obj, "DND_SRC_PAYLOAD_AUTO_EXPIRE",    zym_newNumber(1 << 5));
    zym_mapSet(vm, obj, "DND_ACCEPT_BEFORE_DELIVERY",     zym_newNumber(1 << 10));
    zym_mapSet(vm, obj, "DND_ACCEPT_NO_DRAW_DEFAULT_RECT",zym_newNumber(1 << 11));
    zym_mapSet(vm, obj, "DND_ACCEPT_NO_PREVIEW_TOOLTIP",  zym_newNumber(1 << 12));

    // PR 2e — table sort
    zym_mapSet(vm, obj, "tableGetSortSpecs",    tableGetSortSpecs);

    // PR 2e — font atlas internals
    zym_mapSet(vm, obj, "addFontDefault",    addFontDefault);
    zym_mapSet(vm, obj, "clearFonts",        clearFonts);
    zym_mapSet(vm, obj, "getFontTexSize",    getFontTexSize);
    zym_mapSet(vm, obj, "getFontAtlasFlags", getFontAtlasFlags);
    zym_mapSet(vm, obj, "setFontAtlasFlags", setFontAtlasFlags);
    zym_mapSet(vm, obj, "getFontCount",      getFontCount);
    zym_mapSet(vm, obj, "getFontAt",         getFontAt);
    zym_mapSet(vm, obj, "FONT_ATLAS_NONE",                 zym_newNumber(0));
    zym_mapSet(vm, obj, "FONT_ATLAS_NO_POWER_OF_TWO_HEIGHT", zym_newNumber(1 << 0));
    zym_mapSet(vm, obj, "FONT_ATLAS_NO_MOUSE_CURSORS",     zym_newNumber(1 << 1));
    zym_mapSet(vm, obj, "FONT_ATLAS_NO_BAKED_LINES",       zym_newNumber(1 << 2));

    // ImGuiTableFlags_* — the practical subset for sortable, themed
    // dashboards.  Pass these (bit-OR'd) as the `flags` arg to the
    // 4-arg form of `UI.table(id, columns, flags, body)`.
    zym_mapSet(vm, obj, "TABLE_NONE",            zym_newNumber(0));
    zym_mapSet(vm, obj, "TABLE_RESIZABLE",       zym_newNumber(1 << 0));
    zym_mapSet(vm, obj, "TABLE_REORDERABLE",     zym_newNumber(1 << 1));
    zym_mapSet(vm, obj, "TABLE_HIDEABLE",        zym_newNumber(1 << 2));
    zym_mapSet(vm, obj, "TABLE_SORTABLE",        zym_newNumber(1 << 3));
    zym_mapSet(vm, obj, "TABLE_NO_SAVED_SETTINGS", zym_newNumber(1 << 4));
    zym_mapSet(vm, obj, "TABLE_ROW_BG",          zym_newNumber(1 << 6));
    zym_mapSet(vm, obj, "TABLE_BORDERS_INNER_H", zym_newNumber(1 << 7));
    zym_mapSet(vm, obj, "TABLE_BORDERS_OUTER_H", zym_newNumber(1 << 8));
    zym_mapSet(vm, obj, "TABLE_BORDERS_INNER_V", zym_newNumber(1 << 9));
    zym_mapSet(vm, obj, "TABLE_BORDERS_OUTER_V", zym_newNumber(1 << 10));
    zym_mapSet(vm, obj, "TABLE_BORDERS_H",       zym_newNumber((1 << 7) | (1 << 8)));
    zym_mapSet(vm, obj, "TABLE_BORDERS_V",       zym_newNumber((1 << 9) | (1 << 10)));
    zym_mapSet(vm, obj, "TABLE_BORDERS_INNER",   zym_newNumber((1 << 7) | (1 << 9)));
    zym_mapSet(vm, obj, "TABLE_BORDERS_OUTER",   zym_newNumber((1 << 8) | (1 << 10)));
    zym_mapSet(vm, obj, "TABLE_BORDERS",         zym_newNumber((1 << 7) | (1 << 8) | (1 << 9) | (1 << 10)));
    zym_mapSet(vm, obj, "TABLE_SIZING_FIXED_FIT",     zym_newNumber(1 << 13));
    zym_mapSet(vm, obj, "TABLE_SIZING_FIXED_SAME",    zym_newNumber(2 << 13));
    zym_mapSet(vm, obj, "TABLE_SIZING_STRETCH_PROP",  zym_newNumber(3 << 13));
    zym_mapSet(vm, obj, "TABLE_SIZING_STRETCH_SAME",  zym_newNumber(4 << 13));
    zym_mapSet(vm, obj, "TABLE_SCROLL_X",         zym_newNumber(1 << 24));
    zym_mapSet(vm, obj, "TABLE_SCROLL_Y",         zym_newNumber(1 << 25));
    zym_mapSet(vm, obj, "TABLE_SORT_MULTI",       zym_newNumber(1 << 26));
    zym_mapSet(vm, obj, "TABLE_SORT_TRISTATE",    zym_newNumber(1 << 27));

    // Roots are popped in reverse-push order; everything we've pushed
    // is reachable from `obj`, which is itself rooted. Pop in groups
    // matching pushes (newest first).
    zym_popRoot(vm); // obj

    // PR 2e (popped newest-first, exact reverse of push order)
    zym_popRoot(vm); // getFontAt
    zym_popRoot(vm); // getFontCount
    zym_popRoot(vm); // setFontAtlasFlags
    zym_popRoot(vm); // getFontAtlasFlags
    zym_popRoot(vm); // getFontTexSize
    zym_popRoot(vm); // clearFonts
    zym_popRoot(vm); // addFontDefault
    zym_popRoot(vm); // tableGetSortSpecs
    zym_popRoot(vm); // getDragDropPayload
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); // acceptDragDropPayload{disp,2,1}
    zym_popRoot(vm); // endDragDropTarget
    zym_popRoot(vm); // beginDragDropTarget
    zym_popRoot(vm); // setDragDropPayload
    zym_popRoot(vm); // endDragDropSource
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); // beginDragDropSource{disp,1,0}

    // PR 2d: widget parity batch (popped newest-first, exact reverse of push order)
    zym_popRoot(vm); // setCursorScreenPos
    zym_popRoot(vm); // setCursorPos
    zym_popRoot(vm); // getContentRegionAvail
    zym_popRoot(vm); // getFrameHeightWithSpacing
    zym_popRoot(vm); // getFrameHeight
    zym_popRoot(vm); // getTextLineHeightWithSpacing
    zym_popRoot(vm); // getTextLineHeight
    zym_popRoot(vm); // getFontSize
    zym_popRoot(vm); // getStyleColorVec4
    zym_popRoot(vm); // calcTextSize
    zym_popRoot(vm); // setItemDefaultFocus
    zym_popRoot(vm); // setKeyboardFocusHere
    zym_popRoot(vm); // popItemWidth
    zym_popRoot(vm); // pushItemWidth
    zym_popRoot(vm); // setNextItemAllowOverlap
    zym_popRoot(vm); // setNextItemOpen
    zym_popRoot(vm); // setNextItemWidth
    zym_popRoot(vm); // popupContextWindow
    zym_popRoot(vm); // popupContextItem
    zym_popRoot(vm); // setClipboardText
    zym_popRoot(vm); // getClipboardText
    zym_popRoot(vm); // setNextFrameWantCaptureMouse
    zym_popRoot(vm); // setNextFrameWantCaptureKeyboard
    zym_popRoot(vm); // getKeyPressedAmount
    zym_popRoot(vm); // isKeyReleased
    zym_popRoot(vm); // isKeyPressed
    zym_popRoot(vm); // isKeyDown
    zym_popRoot(vm); // getMouseClickedCount
    zym_popRoot(vm); // resetMouseDragDelta
    zym_popRoot(vm); // getMouseDragDelta
    zym_popRoot(vm); // isMouseDragging
    zym_popRoot(vm); // isMouseReleased
    zym_popRoot(vm); // isMouseDoubleClicked
    zym_popRoot(vm); // isMouseClicked
    zym_popRoot(vm); // isMouseDown
    zym_popRoot(vm); // getItemRectSize
    zym_popRoot(vm); // getItemRectMax
    zym_popRoot(vm); // getItemRectMin
    zym_popRoot(vm); // isAnyItemFocused
    zym_popRoot(vm); // isAnyItemActive
    zym_popRoot(vm); // isAnyItemHovered
    zym_popRoot(vm); // isItemToggledOpen
    zym_popRoot(vm); // isItemDeactivatedAfterEdit
    zym_popRoot(vm); // isItemDeactivated
    zym_popRoot(vm); // isItemActivated
    zym_popRoot(vm); // isItemEdited
    zym_popRoot(vm); // isItemVisible
    zym_popRoot(vm); // setNextWindowScroll
    zym_popRoot(vm); // setNextWindowCollapsed
    zym_popRoot(vm); // setNextWindowContentSize
    zym_popRoot(vm); // setNextWindowBgAlpha
    zym_popRoot(vm); // setNextWindowFocus
    zym_popRoot(vm); // getWindowHeight
    zym_popRoot(vm); // getWindowWidth
    zym_popRoot(vm); // getWindowSize
    zym_popRoot(vm); // getWindowPos
    zym_popRoot(vm); // isWindowCollapsed
    zym_popRoot(vm); // isWindowAppearing
    zym_popRoot(vm); // setScrollFromPosY
    zym_popRoot(vm); // setScrollFromPosX
    zym_popRoot(vm); // setScrollHereY
    zym_popRoot(vm); // setScrollHereX
    zym_popRoot(vm); // setScrollY
    zym_popRoot(vm); // setScrollX
    zym_popRoot(vm); // getScrollMaxY
    zym_popRoot(vm); // getScrollMaxX
    zym_popRoot(vm); // getScrollY
    zym_popRoot(vm); // getScrollX
    zym_popRoot(vm); // vSliderInt
    zym_popRoot(vm); // vSliderFloat
    zym_popRoot(vm); // sliderAngle
    zym_popRoot(vm); // inputInt4
    zym_popRoot(vm); // inputInt3
    zym_popRoot(vm); // inputInt2
    zym_popRoot(vm); // inputFloat4
    zym_popRoot(vm); // inputFloat3
    zym_popRoot(vm); // inputFloat2
    zym_popRoot(vm); // dragInt4
    zym_popRoot(vm); // dragInt3
    zym_popRoot(vm); // dragInt2
    zym_popRoot(vm); // dragFloat4
    zym_popRoot(vm); // dragFloat3
    zym_popRoot(vm); // dragFloat2
    zym_popRoot(vm); // sliderInt4
    zym_popRoot(vm); // sliderInt3
    zym_popRoot(vm); // sliderInt2
    zym_popRoot(vm); // sliderFloat4
    zym_popRoot(vm); // sliderFloat3
    zym_popRoot(vm); // sliderFloat2
    zym_popRoot(vm); // checkboxFlags
    zym_popRoot(vm); // textLinkOpenURL
    zym_popRoot(vm); // textLink
    zym_popRoot(vm); // separatorText
    zym_popRoot(vm); // comboScope
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); // listBox{,2v,3v}
    zym_popRoot(vm); // tabItemButton
    zym_popRoot(vm); // tabItem
    zym_popRoot(vm); // tabBar

    // PR 2c (popped newest-first)
    zym_popRoot(vm); // defaultFont
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); // loadFont{,2v,3v}
    zym_popRoot(vm); // withFont
    zym_popRoot(vm); // withStyleVar
    zym_popRoot(vm); // withStyleColor

    // batch 6 (popped newest-first)
    zym_popRoot(vm); // framerate
    zym_popRoot(vm); // getMousePos
    zym_popRoot(vm); // getCursorPos
    // PR 3 DrawList factories (pushed between drawTriangleFilled and getCursorPos)
    zym_popRoot(vm); // foregroundDrawList
    zym_popRoot(vm); // backgroundDrawList
    zym_popRoot(vm); // drawList
    zym_popRoot(vm); // drawTriangleFilled
    zym_popRoot(vm); // drawTriangle
    zym_popRoot(vm); // drawText
    zym_popRoot(vm); // drawCircleFilled
    zym_popRoot(vm); // drawCircle
    zym_popRoot(vm); // drawRectFilled
    zym_popRoot(vm); // drawRect
    zym_popRoot(vm); // drawLine
    zym_popRoot(vm); // color
    zym_popRoot(vm); // colorButton
    zym_popRoot(vm); // colorPicker
    zym_popRoot(vm); // colorEdit
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); // plotHistogram{,2v,3v,5v}
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); // plotLines{,2v,3v,5v}

    // batch 5 (popped newest-first)
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); // menuItem{,1v,2v,3v,4v}
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); // menu{,2v,3v}
    zym_popRoot(vm); // mainMenuBar
    zym_popRoot(vm); // menuBar
    zym_popRoot(vm); // closeCurrentPopup
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); // openPopup{,1v,2v}
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); // popupModal{,2v,3v}
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); // popup{,2v,3v}

    // batch 4 (popped newest-first)
    zym_popRoot(vm); // nextColumn
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); // columns{,1v,2v,3v}
    zym_popRoot(vm); // tableGetColumnCount
    zym_popRoot(vm); // tableGetColumnIndex
    zym_popRoot(vm); // tableGetRowIndex
    zym_popRoot(vm); // tableHeader
    zym_popRoot(vm); // tableHeadersRow
    zym_popRoot(vm); // tableSetupScrollFreeze
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); // tableSetupColumn{,1v,2v,3v}
    zym_popRoot(vm); // tableSetColumnIndex
    zym_popRoot(vm); // tableNextColumn
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); // tableNextRow{,0v,1v}
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); // table{,3v,4v}

    // batch 3 (popped newest-first)
    zym_popRoot(vm); // tooltipScope
    zym_popRoot(vm); // clip
    zym_popRoot(vm); // id
    zym_popRoot(vm); // disabled
    zym_popRoot(vm); // treeNode
    zym_popRoot(vm); // group
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); // child{,2v,3v,5v}

    // batch 2 (popped newest-first)
    zym_popRoot(vm); // combo
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); // inputTextMultiline{,4,5}
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); // inputText{,2,3}
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); // dragFloat{,2,3,5}
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); // dragInt{,2,3,5}
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); // sliderFloat{,4,5}
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); // sliderInt{,4,5}
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); // inputFloat{,2,3,4}
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); // inputInt{,2,3}

    // misc dispatchers
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); // collapsingHeader{,1,2}
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); // progressBar{,1,3,4}
    // tooltip
    zym_popRoot(vm);
    // state queries (8)
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm);
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm);
    // unindent dispatcher (3)
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm);
    // indent dispatcher (3)
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm);
    // dummy, spacing, separator, newLine
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm);
    // sameLine dispatcher (4)
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm);
    // selectable dispatcher (3)
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm);
    // radioButton, checkbox
    zym_popRoot(vm); zym_popRoot(vm);
    // arrowButton, invisibleButton, smallButton
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm);
    // button dispatcher (3)
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm);
    // text family (7: bullet, bulletText, labelText, textDisabled, textWrapped, textColored, text)
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm);
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm);
    // setNextWindowSize, setNextWindowPos, window dispatcher, window3, window2,
    // setErrorTooltip, setErrorLogging, silent, lastError, frame
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm);
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm);
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm);
    // context
    zym_popRoot(vm);
    return obj;
}
