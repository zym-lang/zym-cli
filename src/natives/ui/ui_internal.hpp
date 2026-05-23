#pragma once
//
// ui_internal.hpp
//
// Cross-TU helpers shared between the `ui/*` translation units that
// make up the `ui` native:
//
//   * ui.cpp     — thin FFI binder: includes, per-window context
//                  lifecycle (ImGui + ImPlot), `u_frame`, and the
//                  `nativeUi_create` shell.
//   * imgui.cpp  — every `u_*` ImGui widget wrapper + the
//                  `registerImGuiBindings` registration body.
//   * implot.cpp — every `u_plot*` ImPlot wrapper + the
//                  `registerImPlotBindings` registration body
//                  (placeholder; Phase A populates it).
//
// All of these need the same tiny arg-helper surface (`reqStr`,
// `optNum`, `requireFrame`, ref read/write helpers, ...) and the same
// shared `g_ui_lastError`. Keeping the helpers `inline` here avoids
// the alternative of either:
//   (a) duplicating their definitions across the three TUs, or
//   (b) introducing a fourth `ui_helpers.cpp` TU for what is largely
//       a dozen one-liners.
//
// `g_ui_lastError` is defined exactly once (in ui.cpp) and only
// declared here, so the linker still sees a single instance.
//
// Compiled only when ZYM_UI_ENABLED is defined.

#include "../natives.hpp"
#include "../root_scope.hpp"
#include "sdl_internal.hpp"

#include "imgui.h"

#include <cfloat>
#include <cstring>
#include <string>
#include <vector>

namespace zym_ui_internal {

// Last error stamped by any UI-stack call. Defined in ui.cpp; read
// by `ui.lastError()` (also lives in ui.cpp).
extern std::string g_ui_lastError;

inline void setError(const char* msg) {
    g_ui_lastError = msg ? msg : "";
}


inline bool reqStr(ZymVM* vm, ZymValue v, const char* where, std::string* out) {
    if (!zym_isString(v)) {
        zym_runtimeError(vm, "%s expects a string", where);
        return false;
    }
    *out = zym_asCString(v);
    return true;
}

inline bool reqCallable(ZymVM* vm, ZymValue v, const char* where) {
    if (zym_isClosure(v) || zym_isFunction(v)) return true;
    zym_runtimeError(vm, "%s expects a callback function", where);
    return false;
}

inline bool reqNum(ZymVM* vm, ZymValue v, const char* where, double* out) {
    if (!zym_isNumber(v)) { zym_runtimeError(vm, "%s expects a number", where); return false; }
    *out = zym_asNumber(v); return true;
}
inline bool reqInt(ZymVM* vm, ZymValue v, const char* where, int* out) {
    double d; if (!reqNum(vm, v, where, &d)) return false;
    *out = (int)d; return true;
}
inline bool reqBool(ZymVM* vm, ZymValue v, const char* where, bool* out) {
    if (!zym_isBool(v)) { zym_runtimeError(vm, "%s expects a bool", where); return false; }
    *out = zym_asBool(v); return true;
}

// Optional arg helpers — used by widgets with default values.
inline double optNum(ZymValue v, double fallback) {
    if (zym_isNumber(v)) return zym_asNumber(v);
    return fallback;
}
inline int optInt(ZymValue v, int fallback) {
    if (zym_isNumber(v)) return (int)zym_asNumber(v);
    return fallback;
}
// Unsigned-32 variant — used for packed colors (IM_COL32) which routinely
// exceed INT_MAX. Casting a double > INT_MAX directly to `int` is UB and
// in practice produces 0/INT_MIN on GCC, which makes drawList colors look
// frozen regardless of what the script passes. Round through uint32_t.
inline uint32_t optU32(ZymValue v, uint32_t fallback) {
    if (zym_isNumber(v)) {
        double d = zym_asNumber(v);
        if (d < 0.0) d = 0.0;
        if (d > 4294967295.0) d = 4294967295.0;
        return (uint32_t)d;
    }
    return fallback;
}
inline bool optBool(ZymValue v, bool fallback) {
    if (zym_isBool(v)) return zym_asBool(v);
    return fallback;
}
inline const char* optStr(ZymValue v, const char* fallback) {
    if (zym_isString(v)) return zym_asCString(v);
    return fallback;
}

// Single-element list "ref" helpers — script passes `[0]` / `[false]` /
// `[0.0]` / `["text"]` and the bridge reads/writes index 0 in place.
inline bool refReadInt(ZymVM* vm, ZymValue ref, const char* where, int* out) {
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
inline bool refReadFloat(ZymVM* vm, ZymValue ref, const char* where, float* out) {
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
inline bool refReadBool(ZymVM* vm, ZymValue ref, const char* where, bool* out) {
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
inline bool refWriteInt(ZymVM* vm, ZymValue ref, int val) {
    return zym_listSet(vm, ref, 0, zym_newNumber((double)val));
}
inline bool refWriteFloat(ZymVM* vm, ZymValue ref, float val) {
    return zym_listSet(vm, ref, 0, zym_newNumber((double)val));
}
inline bool refWriteBool(ZymVM* vm, ZymValue ref, bool val) {
    return zym_listSet(vm, ref, 0, zym_newBool(val));
}

// Color ref — list of 3 or 4 floats in [r, g, b] / [r, g, b, a] form.
// Returns the count actually read (3 or 4).
inline int refReadColor(ZymVM* vm, ZymValue ref, const char* where, float out[4]) {
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
inline void refWriteColor(ZymVM* vm, ZymValue ref, const float c[4], int count) {
    for (int i = 0; i < count; i++) {
        zym_listSet(vm, ref, i, zym_newNumber((double)c[i]));
    }
}

// Frame-context guard used by every widget — they all need an active
// ImGui frame, which `ui.frame(win, body)` is responsible for opening.
inline bool requireFrame(ZymVM* vm, const char* where) {
    if (!ImGui::GetCurrentContext()) {
        zym_runtimeError(vm, "%s: called outside ui.frame(...)", where);
        return false;
    }
    return true;
}

} // namespace zym_ui_internal

// Pull the helpers into the file scope of every TU that includes this
// header. Anonymous-namespace conflicts are not a concern: this header
// is only included by `ui/*.cpp` files which themselves operate inside
// `namespace { ... }` blocks.
using zym_ui_internal::g_ui_lastError;
using zym_ui_internal::setError;
using zym_ui_internal::reqStr;
using zym_ui_internal::reqCallable;
using zym_ui_internal::reqNum;
using zym_ui_internal::reqInt;
using zym_ui_internal::reqBool;
using zym_ui_internal::optNum;
using zym_ui_internal::optInt;
using zym_ui_internal::optU32;
using zym_ui_internal::optBool;
using zym_ui_internal::optStr;
using zym_ui_internal::refReadInt;
using zym_ui_internal::refReadFloat;
using zym_ui_internal::refReadBool;
using zym_ui_internal::refWriteInt;
using zym_ui_internal::refWriteFloat;
using zym_ui_internal::refWriteBool;
using zym_ui_internal::refReadColor;
using zym_ui_internal::refWriteColor;
using zym_ui_internal::requireFrame;

// ---- per-wrapper-file registration entry points ------------------------
//
// Each wrapper TU exposes a single registration function called from
// `ui.cpp`'s `nativeUi_create`. They take:
//   * vm    — the VM doing the registration
//   * obj   — the `ui` module map being built (already a GC root)
//   * ctx   — a shared NativeContext that every wrapped function uses
//             (so each closure doesn't need its own context userdata)
//   * roots — the RAII scope guarding all the closures + dispatchers
//             created during registration; the wrapper file calls
//             `roots.push(...)` on every new closure so the trailing
//             `roots.popAll()` in `nativeUi_create` drains the lot.

void registerImGuiBindings(ZymVM* vm, ZymValue obj, ZymValue ctx, RootScope& roots);
void registerImPlotBindings(ZymVM* vm, ZymValue obj, ZymValue ctx, RootScope& roots);
void registerImAnimBindings(ZymVM* vm, ZymValue obj, ZymValue ctx, RootScope& roots);

// ---- Cross-TU accessor: auto-frame-update flag --------------------------
//
// Defined in `imanim.cpp`. `ui.cpp::u_frame` queries it once per frame
// to decide whether to drive `iam_update_begin_frame()` + `iam_clip_update(dt)`
// automatically. Scripts can flip it via `UI.animSetAutoFrameUpdate(bool)`
// to take over the driving manually (via `UI.animUpdateBeginFrame()` and
// `UI.animClipUpdate(dt)`) for scrub / pause / multi-pass scenarios.
bool isAnimAutoFrameUpdateEnabled();

