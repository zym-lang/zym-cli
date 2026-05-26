// ui.cpp — thin FFI binder for the `ui` native.
//
// Phase B carve-out (March 2026): the `ui` native used to live in a
// single 4800-line `src/natives/ui.cpp`. It's now split across
// `src/natives/ui/`:
//
//   * ui.cpp     — this file. Holds the bits that have to be unique
//                  per process / per native module:
//                    - shared `g_ui_lastError` storage (read by the
//                      `ui.lastError` native and stamped by anything
//                      in the stack that fails),
//                    - the per-window context lifecycle for ImGui +
//                      ImPlot (createContext / destroyContext / event
//                      forwarding, all wired into `sdl.cpp` via the
//                      `g_sdl_uiContext*` hook globals),
//                    - the `ui.frame(win, body)` native (it owns the
//                      ImGui Begin/EndFrame pair so scripts can't
//                      desync it),
//                    - the `nativeUi_create` entry point itself,
//                      which just creates the module map and delegates
//                      to `registerImGuiBindings` + `registerImPlotBindings`.
//   * imgui.cpp  — every `u_*` ImGui widget wrapper + the
//                  `registerImGuiBindings` function that mounts them.
//   * implot.cpp — every `u_plot*` ImPlot wrapper + the
//                  `registerImPlotBindings` function (placeholder right
//                  now; Phase A populates it with the full ImPlot v1.0
//                  surface).
//
// All three TUs share helpers via `ui_internal.hpp` (`reqStr`,
// `optNum`, `requireFrame`, ref helpers, ...).
//
// Compiled only when ZYM_UI_ENABLED is defined (see CMakeLists.txt).
// When disabled this TU is excluded and `cli_catalog.cpp` omits the
// `ui` row.

#include "ui_internal.hpp"

#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "implot.h"
#include "im_anim.h"

#include <SDL3/SDL.h>

// Shared error-string storage — declared in ui_internal.hpp as
// `extern std::string g_ui_lastError;` so imgui.cpp / implot.cpp can
// stamp it via `setError(...)` and read it via `g_ui_lastError`.
namespace zym_ui_internal {
    std::string g_ui_lastError;
}

namespace {

// ---- per-window ImGui+ImPlot context lifecycle --------------------------
//
// Each `WindowHandle` (owned by `sdl.cpp`) gets its own ImGui context
// (and a paired ImPlot context) the first time `ui.frame(win, ...)`
// targets it. The context owns the SDL3 + SDLRenderer3 backend state
// — both need init when the context is created and shutdown before
// `SDL_DestroyRenderer` / `SDL_DestroyWindow` run, otherwise the
// backend dereferences freed pointers.
//
// Teardown order matters: ImPlot's context holds an internal pointer
// to the owning ImGui context and dereferences it during destroy, so
// we must DestroyContext ImPlot BEFORE shutting down the ImGui
// backends.

// Destroy the ImGui+ImPlot context attached to a WindowHandle, if any.
// Called from sdl.cpp's window finalizer / Window.free() via the
// `g_sdl_uiContextDestructor` hook so teardown happens BEFORE the
// SDL renderer/window go away.
void destroyUiContext(WindowHandle* w) {
    if (!w || !w->imguiContext) return;
    auto* ctx = static_cast<ImGuiContext*>(w->imguiContext);
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(ctx);
    // Tear down ImPlot BEFORE ImGui — ImPlot's context holds a pointer
    // to the owning ImGui context and dereferences it on destroy.
    if (w->implotContext) {
        auto* pctx = static_cast<ImPlotContext*>(w->implotContext);
        ImPlot::SetCurrentContext(pctx);
        ImPlot::DestroyContext(pctx);
        w->implotContext = nullptr;
        ImPlot::SetCurrentContext(nullptr);
    }
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    // ImAnim's clip + tween registries are PROCESS-global (live in
    // `iam_clip_detail::g_clip_sys` and `iam_detail::g_*` inside ImAnim),
    // not per-context. Drain them here so ASan doesn't flag the owned
    // `ImVector<iam_track>` / `ImVector<keyframe>` / instance color-entry
    // vectors / `ImGuiStorage` pairs as leaks at process exit. Safe to
    // call multiple times — `iam_clip_shutdown` resets the `initialized`
    // flag, and `iam_gc(0)` is a no-op if there are no tween entries.
    // No-op for processes that never touched the anim system (both
    // functions early-out when their state is empty).
    iam_clip_shutdown();
    iam_gc(0);
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
    // The previous "broadcast to current context as a fallback" path
    // was unsafe — see the pre-Phase-B comment in the git history for
    // the long-form rationale; the short version is that
    // ImGui_ImplSDL3_ProcessEvent dereferences the SDL3 backend data
    // without a runtime guard in Release, so calling it against a
    // context whose backend isn't initialised for THIS event's window
    // segfaults.
    if (!w) return;
    auto props = SDL_GetWindowProperties(w);
    if (!props) return;
    WindowHandle* wh = static_cast<WindowHandle*>(
        SDL_GetPointerProperty(props, "zym.handle", nullptr));
    if (!wh || !wh->imguiContext) return;
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(wh->imguiContext));
    ImGui_ImplSDL3_ProcessEvent(e);
    if (prev) ImGui::SetCurrentContext(prev);
}

// Lazily attach an ImGui (and paired ImPlot) context to a window.
// Returns the ImGui context or nullptr on failure (with
// `g_ui_lastError` set).
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
    // Spin up a paired ImPlot context. ImPlot calls ImGui internally
    // (it draws via ImDrawList) so the ImGui context MUST be current
    // when ImPlot::CreateContext is called; we already did that above.
    ImPlotContext* pctx = ImPlot::CreateContext();
    if (!pctx) {
        // Don't fail the whole frame for an ImPlot allocation failure;
        // plotting calls will surface their own errors via a
        // `requirePlot()` guard in `implot.cpp`. Just leave
        // implotContext null.
        setError("ui: ImPlot::CreateContext failed");
    } else {
        w->implotContext = pctx;
    }
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
    if (w->implotContext) {
        ImPlot::SetCurrentContext(static_cast<ImPlotContext*>(w->implotContext));
    }

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    // ImAnim has no per-context state of its own — it stores channels
    // inside ImGui's current context via ImPool + ImGuiStorage — so a
    // single per-frame begin call is all the bookkeeping it needs.
    // Both `iam_update_begin_frame()` (tween bookkeeping) and
    // `iam_clip_update(dt)` (clip-system advance) are driven here so
    // scripts don't have to remember the paired init calls. The single
    // `isAnimAutoFrameUpdateEnabled()` flag (toggled from script via
    // `UI.animSetAutoFrameUpdate(bool)`) gates BOTH — there's no
    // realistic scenario where you'd want one auto and the other manual,
    // since the clip system feeds off the tween system.
    if (isAnimAutoFrameUpdateEnabled()) {
        iam_update_begin_frame();
        iam_clip_update(ImGui::GetIO().DeltaTime);
    }

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

} // namespace

// ---- module assembly -----------------------------------------------------
//
// `nativeUi_create` is now a thin shell: it wires the sdl<->ui
// callback hooks, sets up a `RootScope` to guard all the closures
// the two register functions are about to create, creates the
// module map + shared NativeContext, and delegates the heavy lifting
// to `registerImGuiBindings` (in imgui.cpp) and `registerImPlotBindings`
// (in implot.cpp). The `frame` native is registered directly here
// because `u_frame` lives in this TU.

ZymValue nativeUi_create(ZymVM* vm) {
    // Wire up the sdl <-> ui hooks. Safe to do at every `nativeUi_create`
    // call (idempotent — they're just function-pointer assignments).
    g_sdl_uiContextDestructor = destroyUiContext;
    g_sdl_uiEventForwarder    = forwardEvent;

    // RAII GC-root scope spanning both register* calls. The destructor
    // / explicit `popAll` drains every closure + dispatcher created
    // during registration in reverse-push order. See root_scope.hpp.
    RootScope roots(vm);

    ZymValue context = roots.push(zym_createNativeContext(vm, nullptr, nullptr));
    ZymValue obj     = roots.push(zym_newMap(vm));

    // `frame` is the only widget native whose backing C++ function
    // (`u_frame`) lives in this TU rather than imgui.cpp; register it
    // here so the closure factory call has a static linkage target.
    ZymValue frame = roots.push(
        zym_createNativeClosure(vm, "frame(win, body)", (void*)u_frame, context));
    zym_mapSet(vm, obj, "frame", frame);

    // Delegate the bulk of the registration to the per-wrapper TUs.
    registerImGuiBindings(vm, obj, context, roots);
    registerImPlotBindings(vm, obj, context, roots);
    registerImAnimBindings(vm, obj, context, roots);

    // Drain every GC root we staged through `roots` above. Everything
    // that needs long-term survival is now reachable from `obj`, which
    // the module-registry will root permanently once we return.
    roots.popAll();

    return obj;
}
