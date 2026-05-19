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
ZymValue u_inputText(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue bufV, ZymValue flagsV) {
    std::string label; if (!reqStr(vm, labelV, "ui.inputText", &label)) return ZYM_ERROR;
    const char* bytes = nullptr; size_t size = 0;
    if (!readBufferBytes(vm, bufV, &bytes, &size)) {
        zym_runtimeError(vm, "ui.inputText expects a Buffer");
        return ZYM_ERROR;
    }
    int flags = optInt(flagsV, 0);
    if (!requireFrame(vm, "ui.inputText")) return ZYM_ERROR;
    // Stage into a local growable buffer ImGui can edit in place. Cap
    // ImGui-edited size to the Buffer's allocated size; scripts size
    // their Buffer with Buffer.alloc(N) up front.
    if (size == 0) size = 1;
    std::vector<char> tmp(size);
    if (bytes) memcpy(tmp.data(), bytes, size - 1);
    tmp[size - 1] = 0; // ensure NUL terminator
    // ImGui needs the full capacity to grow into.
    bool changed = ImGui::InputText(label.c_str(), tmp.data(), tmp.size(), flags);
    if (changed) {
        size_t newLen = strnlen(tmp.data(), tmp.size());
        writeBufferBytes(vm, bufV, tmp.data(), newLen);
    }
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
    if (bytes) memcpy(tmp.data(), bytes, size - 1);
    tmp[size - 1] = 0;
    bool changed = ImGui::InputTextMultiline(label.c_str(), tmp.data(), tmp.size(),
                                             ImVec2(w, h), flags);
    if (changed) {
        size_t newLen = strnlen(tmp.data(), tmp.size());
        writeBufferBytes(vm, bufV, tmp.data(), newLen);
    }
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

ZymValue u_lastError(ZymVM* vm, ZymValue /*self*/) {
    return zym_newStringN(vm, g_ui_lastError.c_str(),
                          (int)g_ui_lastError.size());
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
    MOD(window,    "window(name, body)", u_window)
    MOD(lastError, "lastError()",       u_lastError)

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

#undef MOD

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

    zym_mapSet(vm, obj, "frame",     frame);
    zym_mapSet(vm, obj, "window",    window);
    zym_mapSet(vm, obj, "lastError", lastError);

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

    // Roots are popped in reverse-push order; everything we've pushed
    // is reachable from `obj`, which is itself rooted. Pop in groups
    // matching pushes (newest first).
    zym_popRoot(vm); // obj

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
    // lastError, window, frame
    zym_popRoot(vm); zym_popRoot(vm); zym_popRoot(vm);
    // context
    zym_popRoot(vm);
    return obj;
}
