// imanim.cpp — every `u_anim*` ImAnim wrapper + the
// `registerImAnimBindings` function called from `ui.cpp`'s
// `nativeUi_create`.
//
// Compiled only when ZYM_UI_ENABLED is defined; the CMake target
// `zym_imanim` (which compiles the upstream `im_anim.cpp` TU) is
// linked in unconditionally with the rest of the UI native, so we
// don't need a second flag here.
//
// ImAnim is a pure ImGui addon — it stores per-channel state inside
// ImGui's current context via ImPool + ImGuiStorage keyed by ImGuiID,
// so there is no separate ImAnim context to create / set per window.
// The only frame-bookkeeping call is `iam_update_begin_frame()`,
// which `ui.cpp::u_frame` invokes once after `ImGui::NewFrame()` —
// scripts don't need to (and shouldn't) call it manually.
//
// Section split (mirrors `implot.cpp`'s SECTION banners):
//
//   * SECTION 1 — Frame / global tuning + easing eval + constants.
//   * SECTION 2 — Easing descriptor helpers + per-axis + anchor + blend.
//   * SECTION 3 — Tween API (float / vec2 / vec4 / int / color + rel +
//                  per-axis + rebase + scroll).
//   * SECTION 4 — Oscillate / shake / wiggle.
//   * SECTION 5 — Drag feedback only. Profiler + debug UI
//                 (`animProfiler*`, `animShowUnifiedInspector`,
//                 `animShowDebugTimeline`) are intentionally NOT
//                 bound: Zym drives ImGui/ImPlot/ImAnim purely from
//                 code, so the upstream interactive debug surfaces
//                 are not part of the binding set.
//   * SECTION 6 — Motion paths (curves + fluent builder).
//   * SECTION 7 — Path morphing + text-along-path + quad transforms +
//                  text stagger.
//   * SECTION 8 — Noise + style / gradient / transform interpolation +
//                  repeat-with-variation.
//   * SECTION 9 — Clips + instances + timeline + layered blending +
//                  resolver-callback tweens (deferred from §3).
//
// =============================================================================
// TODO — Deferred callback surface (revisit if/when needed)
// =============================================================================
//
// During the §8/§9 planning round we decided to ship the binding without
// any of ImAnim's callback-shaped APIs, because every "expressive" use
// case those callbacks enable can also be expressed with the data-only
// modes that ARE bound (see `iam_variation_mode` — 7 of 8 modes are
// pure-data: none / increment / decrement / multiply / random /
// random_abs / pingpong, plus per-axis decomposition, color-space-
// aware blending, min/max clamps, and deterministic `seed`). The
// callback hatch is genuinely niche and the binding stays feature-
// complete-for-realistic-use without it. This TODO records what the
// callback surface would look like so a future revisit doesn't have to
// re-do the inventory.
//
// Five callback families exist in upstream (`im_anim.h`):
//
//   1. Variation callbacks (`iam_var_callback` mode). One per type:
//        typedef float    (*iam_variation_float_fn)(int index, void* user);
//        typedef int      (*iam_variation_int_fn)  (int index, void* user);
//        typedef ImVec2   (*iam_variation_vec2_fn) (int index, void* user);
//        typedef ImVec4   (*iam_variation_vec4_fn) (int index, void* user);
//        typedef ImVec4   (*iam_variation_color_fn)(int index, void* user);
//      All take a `user` pointer → straightforward trampoline pattern
//      (slot table of `{ZymVM*, ZymValue closure}`, slot pointer
//      handed to ImAnim as `user`). Closure GC-rooted while the
//      variation descriptor is referenced by a live clip/instance.
//      ~80 lines for all five families + slot lifetime mgmt.
//      Surface to expose:
//        animVarFloatFn(closure)  -> variation desc with fn wired
//        animVarIntFn(closure)    -> ditto
//        animVarVec2Fn(closure)
//        animVarVec4Fn(closure)
//        animVarColorFn(closure)
//
//   2. Clip lifecycle callbacks:
//        typedef void (*iam_clip_callback)(ImGuiID inst_id, void* user);
//      Three slots per clip: on_begin / on_update / on_complete.
//      Each registered via `iam_clip::set_on_begin(...)` etc. (or the
//      flat `iam_clip_set_on_begin(clip_id, fn, user)` C wrappers).
//      Same trampoline pattern as variation callbacks; per-clip
//      storage keyed by clip ImGuiID so closures are released when
//      the clip is destroyed. ~40 lines total.
//      Surface to expose:
//        animClipOnBegin(clipId, closure)
//        animClipOnUpdate(clipId, closure)
//        animClipOnComplete(clipId, closure)
//
//   3. Marker callbacks (per-clip):
//        typedef void (*iam_marker_callback)(ImGuiID inst_id,
//                                            ImGuiID marker_id,
//                                            float   marker_time,
//                                            void*   user);
//      Multiple markers per clip; each call adds one. Same trampoline
//      shape as the lifecycle callbacks but with 3 args. Storage keyed
//      by (clip_id, marker_id). ~30 lines.
//      Surface to expose:
//        animClipMarker(clipId, markerId, time, closure)
//
//   4. Resolver-callback tweens (deferred from §3):
//        typedef float  (*iam_resolver_float_fn) (float t, void* user);
//        typedef int    (*iam_resolver_int_fn)   (float t, void* user);
//        typedef ImVec2 (*iam_resolver_vec2_fn)  (float t, void* user);
//        typedef ImVec4 (*iam_resolver_vec4_fn)  (float t, void* user);
//        typedef ImVec4 (*iam_resolver_color_fn) (float t, void* user);
//        typedef float  (*iam_resolver_angle_fn) (float t, void* user);
//      Consumed by `iam_tween_*_resolved(...)` (6 functions). Lets
//      scripts recompute the tween target each frame from `t`. All take
//      `user` → trampoline pattern. Closure GC-rooted while the tween
//      channel is alive; released when ImAnim GCs the channel or when
//      a fresh `animRebase*` call replaces the target on that channel.
//      ~120 lines including the per-channel rooted-closure registry.
//      Surface to expose:
//        animTweenFloatResolved(id, channelId, closure, dur, ez, policy, dt)
//        animTweenIntResolved(...)
//        animTweenVec2Resolved(...)
//        animTweenVec4Resolved(...)
//        animTweenColorResolved(...)
//        animTweenPathAngleResolved(...)
//
//   5. Custom-ease registration (the awkward one):
//        typedef float (*iam_ease_fn)(float t);   // NO user pointer
//        void iam_register_custom_ease(int slot, iam_ease_fn fn);  // slot 0..15
//      The only callback in ImAnim without a `user` pointer. Solution:
//      generate 16 pre-compiled C trampoline forwarders, one per slot,
//      each dispatching to a slot-indexed `{ZymVM*, ZymValue closure}`
//      table. Bounded count (upstream caps at 16) → static-array
//      storage, no dynamic alloc. ~40 lines for all 16 forwarders +
//      slot table + register/unregister API.
//      Surface to expose:
//        animRegisterCustomEase(slot, closure)
//        animUnregisterCustomEase(slot)
//      Note: `animEaseCustom(slot)` is already bound in §2 — it builds
//      the ease descriptor pointing at a slot. The descriptor evaluates
//      to a no-op until a function is registered for that slot.
//
// Aggregate cost if added later: ~310 lines, no new third-party deps,
// all using the existing `zym_callClosurev` + `zym_pushRoot` /
// `zym_popRoot` mechanics already used by every other UI callback in
// `imgui.cpp` / `implot.cpp` / `ui.cpp`. Frame-update wiring already
// supports the manual-driver mode via `animSetAutoFrameUpdate(false)`
// (§9), so resolver tweens can be deterministically scrubbed if needed.
// =============================================================================

#include "ui_internal.hpp"

#include "im_anim.h"
#include "imgui_internal.h"  // for ImHashStr (used by parseImGuiId in SECTION 3)

#include <unordered_map>     // gradient cache (§8)

namespace {

// ==== SECTION 1: Frame / global tuning + easing eval + constants ==========

// --- Frame bookkeeping ---------------------------------------------------
//
// `iam_update_begin_frame` is called automatically by `ui.cpp::u_frame`
// once per ImGui frame; the script-facing binding is therefore a no-op
// helper that scripts can use for re-entrancy (e.g. multi-pass frames),
// but does not need to be called under normal usage.
ZymValue u_anim_update_begin_frame(ZymVM* vm, ZymValue /*self*/) {
    if (!requireFrame(vm, "ui.animUpdateBeginFrame")) return ZYM_ERROR;
    iam_update_begin_frame();
    return zym_newNull();
}

// `ui.animGc(maxAgeFrames)` — drop stale tween entries older than
// `maxAgeFrames`. Default upstream value (when called without an
// argument) is 600 frames; we expose the explicit-arg form so scripts
// can tune retention without binding a separate default overload.
ZymValue u_anim_gc(ZymVM* vm, ZymValue /*self*/, ZymValue ageV) {
    int age;
    if (!reqInt(vm, ageV, "ui.animGc(maxAgeFrames)", &age)) return ZYM_ERROR;
    if (age < 0) age = 0;
    iam_gc((unsigned int)age);
    return zym_newNull();
}

// `ui.animReserve(float, vec2, vec4, int, color)` — pre-allocate pool
// capacity for each channel kind. Zero/negative values are ignored
// upstream (no-op per channel).
ZymValue u_anim_reserve(ZymVM* vm, ZymValue /*self*/,
                        ZymValue fV, ZymValue v2V, ZymValue v4V,
                        ZymValue iV, ZymValue cV) {
    int cf, cv2, cv4, ci, cc;
    if (!reqInt(vm, fV,  "ui.animReserve(float, vec2, vec4, int, color)", &cf))  return ZYM_ERROR;
    if (!reqInt(vm, v2V, "ui.animReserve(float, vec2, vec4, int, color)", &cv2)) return ZYM_ERROR;
    if (!reqInt(vm, v4V, "ui.animReserve(float, vec2, vec4, int, color)", &cv4)) return ZYM_ERROR;
    if (!reqInt(vm, iV,  "ui.animReserve(float, vec2, vec4, int, color)", &ci))  return ZYM_ERROR;
    if (!reqInt(vm, cV,  "ui.animReserve(float, vec2, vec4, int, color)", &cc))  return ZYM_ERROR;
    iam_reserve(cf, cv2, cv4, ci, cc);
    return zym_newNull();
}

// `ui.animSetEaseLutSamples(count)` — LUT resolution for parametric
// easings (upstream clamps to >=9; default 256).
ZymValue u_anim_set_ease_lut_samples(ZymVM* vm, ZymValue /*self*/, ZymValue countV) {
    int count;
    if (!reqInt(vm, countV, "ui.animSetEaseLutSamples(count)", &count)) return ZYM_ERROR;
    iam_set_ease_lut_samples(count);
    return zym_newNull();
}

// --- Global time scale ----------------------------------------------------

ZymValue u_anim_set_global_time_scale(ZymVM* vm, ZymValue /*self*/, ZymValue scaleV) {
    double scale;
    if (!reqNum(vm, scaleV, "ui.animSetGlobalTimeScale(scale)", &scale)) return ZYM_ERROR;
    iam_set_global_time_scale((float)scale);
    return zym_newNull();
}

ZymValue u_anim_get_global_time_scale(ZymVM* /*vm*/, ZymValue /*self*/) {
    return zym_newNumber((double)iam_get_global_time_scale());
}

// --- Lazy initialisation --------------------------------------------------

ZymValue u_anim_set_lazy_init(ZymVM* vm, ZymValue /*self*/, ZymValue enableV) {
    bool enable;
    if (!reqBool(vm, enableV, "ui.animSetLazyInit(enable)", &enable)) return ZYM_ERROR;
    iam_set_lazy_init(enable);
    return zym_newNull();
}

ZymValue u_anim_is_lazy_init_enabled(ZymVM* /*vm*/, ZymValue /*self*/) {
    return zym_newBool(iam_is_lazy_init_enabled());
}

// --- Easing evaluation ----------------------------------------------------
//
// `ui.animEvalPreset(type, t) -> number` — sample a preset easing at
// `t` in [0,1]. Parametric easings (bezier / spring / steps / custom)
// are NOT presets and must be evaluated through the tween API; this is
// just the cheap stateless preset path.
ZymValue u_anim_eval_preset(ZymVM* vm, ZymValue /*self*/,
                            ZymValue typeV, ZymValue tV) {
    int type;
    double t;
    if (!reqInt(vm, typeV, "ui.animEvalPreset(type, t)", &type)) return ZYM_ERROR;
    if (!reqNum(vm, tV,    "ui.animEvalPreset(type, t)", &t))    return ZYM_ERROR;
    return zym_newNumber((double)iam_eval_preset(type, (float)t));
}

// ==== SECTION 2: Ease descriptor helpers + per-axis + anchor + blend =====
//
// Bindings for the parametric easing descriptors plus stateless helpers
// that don't touch the tween pool. The descriptors are surfaced as plain
// Zym lists `[type, p0, p1, p2, p3]` rather than opaque handles — that
// keeps them trivially composable from script, lets users build them
// inline, and avoids the GC-rooting cost of native userdata for what is
// just five floats. Every later tween binding (sessions 3+) accepts
// either a plain integer preset OR a 5-list parametric desc through the
// `parseEaseDesc()` helper below; the per-axis pack accepts either a
// 2-list or 4-list of those.
//
// Color helper `iam_get_blended_color` is stateless too — it's a pure
// function of (a, b, t, color_space), so it lives here next to the rest
// of the eval-only surface.

// --- Pack helpers (mirror implot.cpp's packVec2 convention) -------------

static ZymValue packVec2(ZymVM* vm, ImVec2 v) {
    ZymValue l = zym_newList(vm);
    zym_listAppend(vm, l, zym_newNumber((double)v.x));
    zym_listAppend(vm, l, zym_newNumber((double)v.y));
    return l;
}

static ZymValue packVec4(ZymVM* vm, ImVec4 v) {
    ZymValue l = zym_newList(vm);
    zym_listAppend(vm, l, zym_newNumber((double)v.x));
    zym_listAppend(vm, l, zym_newNumber((double)v.y));
    zym_listAppend(vm, l, zym_newNumber((double)v.z));
    zym_listAppend(vm, l, zym_newNumber((double)v.w));
    return l;
}

// Read a [x, y] list into an ImVec2. Used by oscillate/shake/wiggle/path
// bindings in later sessions; lives here next to the other helpers.
static bool reqVec2(ZymVM* vm, ZymValue v, const char* where, ImVec2* out) {
    if (!zym_isList(v) || zym_listLength(v) != 2) {
        zym_runtimeError(vm, "%s expects [x, y]", where);
        return false;
    }
    ZymValue xv = zym_listGet(vm, v, 0);
    ZymValue yv = zym_listGet(vm, v, 1);
    if (!zym_isNumber(xv) || !zym_isNumber(yv)) {
        zym_runtimeError(vm, "%s [x, y] elements must be numbers", where);
        return false;
    }
    *out = ImVec2((float)zym_asNumber(xv), (float)zym_asNumber(yv));
    return true;
}

// Read a [x, y, z, w] list into an ImVec4. Used by vec4/color tweens.
static bool reqVec4(ZymVM* vm, ZymValue v, const char* where, ImVec4* out) {
    if (!zym_isList(v) || zym_listLength(v) != 4) {
        zym_runtimeError(vm, "%s expects [x, y, z, w]", where);
        return false;
    }
    float c[4];
    for (int i = 0; i < 4; ++i) {
        ZymValue e = zym_listGet(vm, v, i);
        if (!zym_isNumber(e)) {
            zym_runtimeError(vm, "%s [x, y, z, w] element %d must be a number", where, i);
            return false;
        }
        c[i] = (float)zym_asNumber(e);
    }
    *out = ImVec4(c[0], c[1], c[2], c[3]);
    return true;
}

// --- Ease descriptor parsing --------------------------------------------
//
// Accepts either:
//   * a plain integer  →  treated as a preset (type, p0..p3 = 0).
//   * a 5-element list `[type, p0, p1, p2, p3]` produced by one of the
//     `animEase*` constructors below.
//
// This is the canonical input form for every tween binding from Session
// 3 onward; keep the signature stable.
static bool parseEaseDesc(ZymVM* vm, ZymValue v, const char* where,
                          iam_ease_desc* out) {
    if (zym_isNumber(v)) {
        out->type = (int)zym_asNumber(v);
        out->p0 = out->p1 = out->p2 = out->p3 = 0.0f;
        return true;
    }
    if (zym_isList(v) && zym_listLength(v) == 5) {
        ZymValue t  = zym_listGet(vm, v, 0);
        ZymValue p0 = zym_listGet(vm, v, 1);
        ZymValue p1 = zym_listGet(vm, v, 2);
        ZymValue p2 = zym_listGet(vm, v, 3);
        ZymValue p3 = zym_listGet(vm, v, 4);
        if (!zym_isNumber(t) || !zym_isNumber(p0) || !zym_isNumber(p1) ||
            !zym_isNumber(p2) || !zym_isNumber(p3)) {
            zym_runtimeError(vm,
                "%s ease descriptor list elements must all be numbers", where);
            return false;
        }
        out->type = (int)zym_asNumber(t);
        out->p0   = (float)zym_asNumber(p0);
        out->p1   = (float)zym_asNumber(p1);
        out->p2   = (float)zym_asNumber(p2);
        out->p3   = (float)zym_asNumber(p3);
        return true;
    }
    zym_runtimeError(vm,
        "%s ease arg must be a preset int or a 5-element list "
        "[type, p0, p1, p2, p3]", where);
    return false;
}

// Per-axis ease: 2-list for vec2, 4-list for vec4/color. Each element is
// itself a valid `parseEaseDesc()` input (preset int OR 5-list).
//
// `expected` is the number of axes the caller wants (2 or 4); the list
// length must match. Defaults of `iam_ease_linear` for the unused slots
// are handled by the caller (vec2 path doesn't read .z/.w).
static bool parseEasePerAxis(ZymVM* vm, ZymValue v, const char* where,
                             int expected, iam_ease_per_axis* out) {
    if (!zym_isList(v) || zym_listLength(v) != expected) {
        zym_runtimeError(vm,
            "%s per-axis ease expects a %d-element list of ease descs",
            where, expected);
        return false;
    }
    iam_ease_desc desc[4];
    for (int i = 0; i < expected; ++i) {
        if (!parseEaseDesc(vm, zym_listGet(vm, v, i), where, &desc[i]))
            return false;
    }
    if (expected == 2) {
        // pad z/w with linear so the struct is well-defined regardless
        // of which tween call ends up reading it.
        iam_ease_desc lin{iam_ease_linear, 0, 0, 0, 0};
        desc[2] = lin;
        desc[3] = lin;
    }
    *out = iam_ease_per_axis(desc[0], desc[1], desc[2], desc[3]);
    return true;
}


// --- Ease descriptor constructors ---------------------------------------
//
// Each returns a 5-element `[type, p0, p1, p2, p3]` list ready to feed
// any tween binding. Constructors live on the script side as pure value
// builders — no native state, no GC roots, just float packing.

// Helper: pack a single ease desc into the canonical 5-list.
static ZymValue packEaseDesc(ZymVM* vm, int type, float p0, float p1,
                             float p2, float p3) {
    ZymValue l = zym_newList(vm);
    zym_listAppend(vm, l, zym_newNumber((double)type));
    zym_listAppend(vm, l, zym_newNumber((double)p0));
    zym_listAppend(vm, l, zym_newNumber((double)p1));
    zym_listAppend(vm, l, zym_newNumber((double)p2));
    zym_listAppend(vm, l, zym_newNumber((double)p3));
    return l;
}

// `ui.animEasePreset(type) -> [type,0,0,0,0]` — wraps a plain preset
// enum in the 5-list shape. Useful when you want a uniform call shape
// across preset / parametric easings in scripts.
ZymValue u_anim_ease_preset(ZymVM* vm, ZymValue /*self*/, ZymValue typeV) {
    int type;
    if (!reqInt(vm, typeV, "ui.animEasePreset(type)", &type)) return ZYM_ERROR;
    return packEaseDesc(vm, type, 0, 0, 0, 0);
}

// `ui.animEaseBezier(x1, y1, x2, y2) -> [..]`
//   Cubic bezier easing — control points in CSS-style ordering (P0 and
//   P3 are implicit (0,0) and (1,1)).
ZymValue u_anim_ease_bezier(ZymVM* vm, ZymValue /*self*/,
                            ZymValue x1V, ZymValue y1V,
                            ZymValue x2V, ZymValue y2V) {
    double x1, y1, x2, y2;
    if (!reqNum(vm, x1V, "ui.animEaseBezier(x1, y1, x2, y2)", &x1)) return ZYM_ERROR;
    if (!reqNum(vm, y1V, "ui.animEaseBezier(x1, y1, x2, y2)", &y1)) return ZYM_ERROR;
    if (!reqNum(vm, x2V, "ui.animEaseBezier(x1, y1, x2, y2)", &x2)) return ZYM_ERROR;
    if (!reqNum(vm, y2V, "ui.animEaseBezier(x1, y1, x2, y2)", &y2)) return ZYM_ERROR;
    return packEaseDesc(vm, iam_ease_cubic_bezier,
                        (float)x1, (float)y1, (float)x2, (float)y2);
}

// `ui.animEaseSteps(steps, jumpMode) -> [..]`
//   `jumpMode`: 0 = jump-end (default), 1 = jump-start, 2 = jump-both.
ZymValue u_anim_ease_steps(ZymVM* vm, ZymValue /*self*/,
                           ZymValue stepsV, ZymValue jumpV) {
    int steps, jump;
    if (!reqInt(vm, stepsV, "ui.animEaseSteps(steps, jumpMode)", &steps)) return ZYM_ERROR;
    if (!reqInt(vm, jumpV,  "ui.animEaseSteps(steps, jumpMode)", &jump))  return ZYM_ERROR;
    if (steps < 1) steps = 1;
    return packEaseDesc(vm, iam_ease_steps, (float)steps, (float)jump, 0, 0);
}

// `ui.animEaseBack(overshoot, dir) -> [..]`
//   `dir`: 0 = in, 1 = out, 2 = inOut. Overshoot follows the standard
//   `c1` convention (1.70158 ≈ default 10%).
ZymValue u_anim_ease_back(ZymVM* vm, ZymValue /*self*/,
                          ZymValue overV, ZymValue dirV) {
    double over;
    int dir;
    if (!reqNum(vm, overV, "ui.animEaseBack(overshoot, dir)", &over)) return ZYM_ERROR;
    if (!reqInt(vm, dirV,  "ui.animEaseBack(overshoot, dir)", &dir))  return ZYM_ERROR;
    int type = (dir == 0) ? iam_ease_in_back
             : (dir == 1) ? iam_ease_out_back
             :              iam_ease_in_out_back;
    return packEaseDesc(vm, type, (float)over, 0, 0, 0);
}

// `ui.animEaseElastic(amplitude, period, dir) -> [..]`
//   `dir`: 0 = in, 1 = out, 2 = inOut.
ZymValue u_anim_ease_elastic(ZymVM* vm, ZymValue /*self*/,
                             ZymValue ampV, ZymValue periodV, ZymValue dirV) {
    double amp, period;
    int dir;
    if (!reqNum(vm, ampV,    "ui.animEaseElastic(amplitude, period, dir)", &amp))    return ZYM_ERROR;
    if (!reqNum(vm, periodV, "ui.animEaseElastic(amplitude, period, dir)", &period)) return ZYM_ERROR;
    if (!reqInt(vm, dirV,    "ui.animEaseElastic(amplitude, period, dir)", &dir))    return ZYM_ERROR;
    int type = (dir == 0) ? iam_ease_in_elastic
             : (dir == 1) ? iam_ease_out_elastic
             :              iam_ease_in_out_elastic;
    return packEaseDesc(vm, type, (float)amp, (float)period, 0, 0);
}

// `ui.animEaseSpring(mass, stiffness, damping, v0) -> [..]`
//   Critically-damped feel ≈ damping² ≈ 4*mass*stiffness. `v0` is the
//   initial velocity (usually 0).
ZymValue u_anim_ease_spring(ZymVM* vm, ZymValue /*self*/,
                            ZymValue mV, ZymValue kV, ZymValue dV, ZymValue v0V) {
    double m, k, d, v0;
    if (!reqNum(vm, mV,  "ui.animEaseSpring(mass, stiffness, damping, v0)", &m))  return ZYM_ERROR;
    if (!reqNum(vm, kV,  "ui.animEaseSpring(mass, stiffness, damping, v0)", &k))  return ZYM_ERROR;
    if (!reqNum(vm, dV,  "ui.animEaseSpring(mass, stiffness, damping, v0)", &d))  return ZYM_ERROR;
    if (!reqNum(vm, v0V, "ui.animEaseSpring(mass, stiffness, damping, v0)", &v0)) return ZYM_ERROR;
    return packEaseDesc(vm, iam_ease_spring,
                        (float)m, (float)k, (float)d, (float)v0);
}

// `ui.animEaseCustom(slot) -> [..]`
//   References a function previously registered via the upstream
//   `iam_register_custom_ease(slot, fn)`. Slot is 0..15. NB: there is no
//   Zym-side binding to register the function yet — that's a Session 9
//   concern (it requires a closure trampoline like resolver tweens);
//   for now scripts can construct the descriptor but evaluating it will
//   no-op until a native registers a function for that slot.
ZymValue u_anim_ease_custom(ZymVM* vm, ZymValue /*self*/, ZymValue slotV) {
    int slot;
    if (!reqInt(vm, slotV, "ui.animEaseCustom(slot)", &slot)) return ZYM_ERROR;
    return packEaseDesc(vm, iam_ease_custom, (float)slot, 0, 0, 0);
}

// --- Anchor size --------------------------------------------------------
//
// `ui.animAnchorSize(space) -> [w, h]`. Requires an active ImGui
// frame because `window_content` / `window` / `last_item` all consult
// the current window state.
ZymValue u_anim_anchor_size(ZymVM* vm, ZymValue /*self*/, ZymValue spaceV) {
    int space;
    if (!reqInt(vm, spaceV, "ui.animAnchorSize(space)", &space)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.animAnchorSize")) return ZYM_ERROR;
    return packVec2(vm, iam_anchor_size(space));
}

// --- Color blending -----------------------------------------------------
//
// `ui.animGetBlendedColor(a, b, t, space) -> [r, g, b, a]`
//   Stateless blend of two sRGB colors at parameter `t` in the chosen
//   color space. Useful for one-shot color sampling without paying for
//   a tween channel entry.
ZymValue u_anim_get_blended_color(ZymVM* vm, ZymValue /*self*/,
                                  ZymValue aV, ZymValue bV,
                                  ZymValue tV, ZymValue spaceV) {
    ImVec4 a, b;
    double t;
    int space;
    if (!reqVec4(vm, aV, "ui.animGetBlendedColor(a, b, t, space) a", &a)) return ZYM_ERROR;
    if (!reqVec4(vm, bV, "ui.animGetBlendedColor(a, b, t, space) b", &b)) return ZYM_ERROR;
    if (!reqNum(vm,  tV, "ui.animGetBlendedColor(a, b, t, space)",   &t)) return ZYM_ERROR;
    if (!reqInt(vm,  spaceV, "ui.animGetBlendedColor(a, b, t, space)", &space)) return ZYM_ERROR;
    return packVec4(vm, iam_get_blended_color(a, b, (float)t, space));
}

// ==== SECTION 3: Core tween API ==========================================
//
// Static-target tweens (float / vec2 / vec4 / int / color), the `_rel`
// percent-of-anchor variants, the per-axis variants (vec2 / vec4 /
// color), the in-flight `rebase_*` redirectors, and the four window
// scroll helpers.
//
// Deferred to §9: `iam_tween_*_resolved` (closure-trampoline tweens).
// Locked decision from the planning round — they need a GC-rooted
// trampoline shared with `iam_register_custom_ease` and are handled
// as a single batch there. Static-target tweens are sufficient to
// drive every existing demo / showcase pattern.
//
// --- ID parsing ----------------------------------------------------------
//
// ImAnim keys all tween state by two `ImGuiID`s — a per-entity id and
// a per-channel id within that entity. Scripts pass them either as:
//
//   * a string  → hashed with `ImHashStr` (the canonical ImGui idiom,
//                  same hash ImGui itself uses internally for
//                  `ImGui::PushID`).
//   * a number  → cast to `ImGuiID` directly (lets advanced scripts
//                  reuse ids returned by other native bindings).
//
// Hashing on every call is cheap (FNV-1a over a short string, ~20 ns)
// and keeps Zym scripts from having to manage opaque numeric ids.

static bool parseImGuiId(ZymVM* vm, ZymValue v, const char* where, ImGuiID* out) {
    if (zym_isString(v)) {
        *out = ImHashStr(zym_asCString(v));
        return true;
    }
    if (zym_isNumber(v)) {
        // ImGuiID is uint32_t — values outside that range get truncated,
        // which is acceptable since the caller already opted into raw
        // numeric ids.
        *out = (ImGuiID)(unsigned long long)zym_asNumber(v);
        return true;
    }
    zym_runtimeError(vm,
        "%s id arg must be a string (hashed) or a number (raw ImGuiID)",
        where);
    return false;
}

// --- Common tween-arg blob ----------------------------------------------
//
// Every tween call has the same trailing shape:
//
//   (id, channelId, target, duration, ease, policy, dt)
//
// or for `_rel` / `_color` / per-axis variants, with one or two extra
// slots (anchor_space / color_space / axis / px_bias). Extracting the
// shared prefix keeps the per-binding bodies short and the error
// messages consistent. We don't use a helper for the whole blob because
// `target` is typed differently per binding; the helper covers the
// pieces that are always identical.
struct TweenCommon {
    ImGuiID id;
    ImGuiID channel;
    double  dur;
    iam_ease_desc ez;
    int     policy;
    double  dt;
};

static bool parseTweenCommon(ZymVM* vm, const char* where,
                             ZymValue idV, ZymValue chV,
                             ZymValue durV, ZymValue ezV,
                             ZymValue policyV, ZymValue dtV,
                             TweenCommon* out) {
    if (!parseImGuiId(vm, idV, where, &out->id))         return false;
    if (!parseImGuiId(vm, chV, where, &out->channel))    return false;
    if (!reqNum(vm, durV, where, &out->dur))             return false;
    if (!parseEaseDesc(vm, ezV, where, &out->ez))        return false;
    if (!reqInt(vm, policyV, where, &out->policy))       return false;
    if (!reqNum(vm, dtV, where, &out->dt))               return false;
    return true;
}

// --- Float / int / vec2 / vec4 / color tweens ---------------------------

ZymValue u_anim_tween_float(ZymVM* vm, ZymValue /*self*/,
                            ZymValue idV, ZymValue chV, ZymValue tgtV,
                            ZymValue durV, ZymValue ezV, ZymValue policyV,
                            ZymValue dtV) {
    const char* W = "ui.animTweenFloat(id, channelId, target, dur, ease, policy, dt)";
    TweenCommon c;
    double target;
    if (!parseTweenCommon(vm, W, idV, chV, durV, ezV, policyV, dtV, &c)) return ZYM_ERROR;
    if (!reqNum(vm, tgtV, W, &target)) return ZYM_ERROR;
    return zym_newNumber((double)iam_tween_float(
        c.id, c.channel, (float)target, (float)c.dur, c.ez, c.policy, (float)c.dt));
}

ZymValue u_anim_tween_int(ZymVM* vm, ZymValue /*self*/,
                          ZymValue idV, ZymValue chV, ZymValue tgtV,
                          ZymValue durV, ZymValue ezV, ZymValue policyV,
                          ZymValue dtV) {
    const char* W = "ui.animTweenInt(id, channelId, target, dur, ease, policy, dt)";
    TweenCommon c;
    int target;
    if (!parseTweenCommon(vm, W, idV, chV, durV, ezV, policyV, dtV, &c)) return ZYM_ERROR;
    if (!reqInt(vm, tgtV, W, &target)) return ZYM_ERROR;
    return zym_newNumber((double)iam_tween_int(
        c.id, c.channel, target, (float)c.dur, c.ez, c.policy, (float)c.dt));
}

ZymValue u_anim_tween_vec2(ZymVM* vm, ZymValue /*self*/,
                           ZymValue idV, ZymValue chV, ZymValue tgtV,
                           ZymValue durV, ZymValue ezV, ZymValue policyV,
                           ZymValue dtV) {
    const char* W = "ui.animTweenVec2(id, channelId, target, dur, ease, policy, dt)";
    TweenCommon c;
    ImVec2 target;
    if (!parseTweenCommon(vm, W, idV, chV, durV, ezV, policyV, dtV, &c)) return ZYM_ERROR;
    if (!reqVec2(vm, tgtV, W, &target)) return ZYM_ERROR;
    return packVec2(vm, iam_tween_vec2(
        c.id, c.channel, target, (float)c.dur, c.ez, c.policy, (float)c.dt));
}

ZymValue u_anim_tween_vec4(ZymVM* vm, ZymValue /*self*/,
                           ZymValue idV, ZymValue chV, ZymValue tgtV,
                           ZymValue durV, ZymValue ezV, ZymValue policyV,
                           ZymValue dtV) {
    const char* W = "ui.animTweenVec4(id, channelId, target, dur, ease, policy, dt)";
    TweenCommon c;
    ImVec4 target;
    if (!parseTweenCommon(vm, W, idV, chV, durV, ezV, policyV, dtV, &c)) return ZYM_ERROR;
    if (!reqVec4(vm, tgtV, W, &target)) return ZYM_ERROR;
    return packVec4(vm, iam_tween_vec4(
        c.id, c.channel, target, (float)c.dur, c.ez, c.policy, (float)c.dt));
}

// Color tween takes an extra `colorSpace` slot — placed after policy /
// before dt to match the upstream signature ordering.
ZymValue u_anim_tween_color(ZymVM* vm, ZymValue /*self*/,
                            ZymValue idV, ZymValue chV, ZymValue tgtV,
                            ZymValue durV, ZymValue ezV, ZymValue policyV,
                            ZymValue spaceV, ZymValue dtV) {
    const char* W = "ui.animTweenColor(id, channelId, target, dur, ease, policy, colorSpace, dt)";
    TweenCommon c;
    ImVec4 target;
    int space;
    if (!parseTweenCommon(vm, W, idV, chV, durV, ezV, policyV, dtV, &c)) return ZYM_ERROR;
    if (!reqVec4(vm, tgtV, W, &target)) return ZYM_ERROR;
    if (!reqInt(vm, spaceV, W, &space)) return ZYM_ERROR;
    return packVec4(vm, iam_tween_color(
        c.id, c.channel, target, (float)c.dur, c.ez, c.policy, space, (float)c.dt));
}

// --- Relative tweens (percent-of-anchor + pixel bias) -------------------

ZymValue u_anim_tween_float_rel(ZymVM* vm, ZymValue /*self*/,
                                ZymValue idV, ZymValue chV,
                                ZymValue percentV, ZymValue biasV,
                                ZymValue durV, ZymValue ezV, ZymValue policyV,
                                ZymValue anchorV, ZymValue axisV,
                                ZymValue dtV) {
    const char* W = "ui.animTweenFloatRel(id, channelId, percent, pxBias, dur, ease, policy, anchorSpace, axis, dt)";
    TweenCommon c;
    double percent, bias;
    int anchor, axis;
    if (!parseTweenCommon(vm, W, idV, chV, durV, ezV, policyV, dtV, &c)) return ZYM_ERROR;
    if (!reqNum(vm, percentV, W, &percent)) return ZYM_ERROR;
    if (!reqNum(vm, biasV,    W, &bias))    return ZYM_ERROR;
    if (!reqInt(vm, anchorV,  W, &anchor))  return ZYM_ERROR;
    if (!reqInt(vm, axisV,    W, &axis))    return ZYM_ERROR;
    return zym_newNumber((double)iam_tween_float_rel(
        c.id, c.channel, (float)percent, (float)bias, (float)c.dur,
        c.ez, c.policy, anchor, axis, (float)c.dt));
}

ZymValue u_anim_tween_vec2_rel(ZymVM* vm, ZymValue /*self*/,
                               ZymValue idV, ZymValue chV,
                               ZymValue percentV, ZymValue biasV,
                               ZymValue durV, ZymValue ezV, ZymValue policyV,
                               ZymValue anchorV, ZymValue dtV) {
    const char* W = "ui.animTweenVec2Rel(id, channelId, percent, pxBias, dur, ease, policy, anchorSpace, dt)";
    TweenCommon c;
    ImVec2 percent, bias;
    int anchor;
    if (!parseTweenCommon(vm, W, idV, chV, durV, ezV, policyV, dtV, &c)) return ZYM_ERROR;
    if (!reqVec2(vm, percentV, W, &percent)) return ZYM_ERROR;
    if (!reqVec2(vm, biasV,    W, &bias))    return ZYM_ERROR;
    if (!reqInt(vm, anchorV,   W, &anchor))  return ZYM_ERROR;
    return packVec2(vm, iam_tween_vec2_rel(
        c.id, c.channel, percent, bias, (float)c.dur,
        c.ez, c.policy, anchor, (float)c.dt));
}

ZymValue u_anim_tween_vec4_rel(ZymVM* vm, ZymValue /*self*/,
                               ZymValue idV, ZymValue chV,
                               ZymValue percentV, ZymValue biasV,
                               ZymValue durV, ZymValue ezV, ZymValue policyV,
                               ZymValue anchorV, ZymValue dtV) {
    const char* W = "ui.animTweenVec4Rel(id, channelId, percent, pxBias, dur, ease, policy, anchorSpace, dt)";
    TweenCommon c;
    ImVec4 percent, bias;
    int anchor;
    if (!parseTweenCommon(vm, W, idV, chV, durV, ezV, policyV, dtV, &c)) return ZYM_ERROR;
    if (!reqVec4(vm, percentV, W, &percent)) return ZYM_ERROR;
    if (!reqVec4(vm, biasV,    W, &bias))    return ZYM_ERROR;
    if (!reqInt(vm, anchorV,   W, &anchor))  return ZYM_ERROR;
    return packVec4(vm, iam_tween_vec4_rel(
        c.id, c.channel, percent, bias, (float)c.dur,
        c.ez, c.policy, anchor, (float)c.dt));
}

ZymValue u_anim_tween_color_rel(ZymVM* vm, ZymValue /*self*/,
                                ZymValue idV, ZymValue chV,
                                ZymValue percentV, ZymValue biasV,
                                ZymValue durV, ZymValue ezV, ZymValue policyV,
                                ZymValue colSpaceV, ZymValue anchorV,
                                ZymValue dtV) {
    const char* W = "ui.animTweenColorRel(id, channelId, percent, pxBias, dur, ease, policy, colorSpace, anchorSpace, dt)";
    TweenCommon c;
    ImVec4 percent, bias;
    int colSpace, anchor;
    if (!parseTweenCommon(vm, W, idV, chV, durV, ezV, policyV, dtV, &c)) return ZYM_ERROR;
    if (!reqVec4(vm, percentV,  W, &percent))  return ZYM_ERROR;
    if (!reqVec4(vm, biasV,     W, &bias))     return ZYM_ERROR;
    if (!reqInt(vm, colSpaceV,  W, &colSpace)) return ZYM_ERROR;
    if (!reqInt(vm, anchorV,    W, &anchor))   return ZYM_ERROR;
    return packVec4(vm, iam_tween_color_rel(
        c.id, c.channel, percent, bias, (float)c.dur,
        c.ez, c.policy, colSpace, anchor, (float)c.dt));
}

// --- Per-axis tweens ----------------------------------------------------

ZymValue u_anim_tween_vec2_per_axis(ZymVM* vm, ZymValue /*self*/,
                                    ZymValue idV, ZymValue chV, ZymValue tgtV,
                                    ZymValue durV, ZymValue ezV, ZymValue policyV,
                                    ZymValue dtV) {
    const char* W = "ui.animTweenVec2PerAxis(id, channelId, target, dur, easeXY, policy, dt)";
    ImGuiID id, channel;
    ImVec2 target;
    double dur, dt;
    int policy;
    iam_ease_per_axis ez;
    if (!parseImGuiId(vm, idV, W, &id))           return ZYM_ERROR;
    if (!parseImGuiId(vm, chV, W, &channel))      return ZYM_ERROR;
    if (!reqVec2(vm, tgtV, W, &target))           return ZYM_ERROR;
    if (!reqNum(vm, durV, W, &dur))               return ZYM_ERROR;
    if (!parseEasePerAxis(vm, ezV, W, 2, &ez))    return ZYM_ERROR;
    if (!reqInt(vm, policyV, W, &policy))         return ZYM_ERROR;
    if (!reqNum(vm, dtV, W, &dt))                 return ZYM_ERROR;
    return packVec2(vm, iam_tween_vec2_per_axis(
        id, channel, target, (float)dur, ez, policy, (float)dt));
}

ZymValue u_anim_tween_vec4_per_axis(ZymVM* vm, ZymValue /*self*/,
                                    ZymValue idV, ZymValue chV, ZymValue tgtV,
                                    ZymValue durV, ZymValue ezV, ZymValue policyV,
                                    ZymValue dtV) {
    const char* W = "ui.animTweenVec4PerAxis(id, channelId, target, dur, easeXYZW, policy, dt)";
    ImGuiID id, channel;
    ImVec4 target;
    double dur, dt;
    int policy;
    iam_ease_per_axis ez;
    if (!parseImGuiId(vm, idV, W, &id))           return ZYM_ERROR;
    if (!parseImGuiId(vm, chV, W, &channel))      return ZYM_ERROR;
    if (!reqVec4(vm, tgtV, W, &target))           return ZYM_ERROR;
    if (!reqNum(vm, durV, W, &dur))               return ZYM_ERROR;
    if (!parseEasePerAxis(vm, ezV, W, 4, &ez))    return ZYM_ERROR;
    if (!reqInt(vm, policyV, W, &policy))         return ZYM_ERROR;
    if (!reqNum(vm, dtV, W, &dt))                 return ZYM_ERROR;
    return packVec4(vm, iam_tween_vec4_per_axis(
        id, channel, target, (float)dur, ez, policy, (float)dt));
}

ZymValue u_anim_tween_color_per_axis(ZymVM* vm, ZymValue /*self*/,
                                     ZymValue idV, ZymValue chV, ZymValue tgtV,
                                     ZymValue durV, ZymValue ezV, ZymValue policyV,
                                     ZymValue spaceV, ZymValue dtV) {
    const char* W = "ui.animTweenColorPerAxis(id, channelId, target, dur, easeRGBA, policy, colorSpace, dt)";
    ImGuiID id, channel;
    ImVec4 target;
    double dur, dt;
    int policy, space;
    iam_ease_per_axis ez;
    if (!parseImGuiId(vm, idV, W, &id))           return ZYM_ERROR;
    if (!parseImGuiId(vm, chV, W, &channel))      return ZYM_ERROR;
    if (!reqVec4(vm, tgtV, W, &target))           return ZYM_ERROR;
    if (!reqNum(vm, durV, W, &dur))               return ZYM_ERROR;
    if (!parseEasePerAxis(vm, ezV, W, 4, &ez))    return ZYM_ERROR;
    if (!reqInt(vm, policyV, W, &policy))         return ZYM_ERROR;
    if (!reqInt(vm, spaceV, W, &space))           return ZYM_ERROR;
    if (!reqNum(vm, dtV, W, &dt))                 return ZYM_ERROR;
    return packVec4(vm, iam_tween_color_per_axis(
        id, channel, target, (float)dur, ez, policy, space, (float)dt));
}

// --- Rebase (smooth retarget of an in-flight tween) ---------------------
//
// All `_rebase` calls share the same shape: (id, channelId, newTarget,
// dt). The target type varies; helper-light inline body each.

ZymValue u_anim_rebase_float(ZymVM* vm, ZymValue /*self*/,
                             ZymValue idV, ZymValue chV, ZymValue tgtV,
                             ZymValue dtV) {
    const char* W = "ui.animRebaseFloat(id, channelId, newTarget, dt)";
    ImGuiID id, channel;
    double target, dt;
    if (!parseImGuiId(vm, idV, W, &id))       return ZYM_ERROR;
    if (!parseImGuiId(vm, chV, W, &channel))  return ZYM_ERROR;
    if (!reqNum(vm, tgtV, W, &target))        return ZYM_ERROR;
    if (!reqNum(vm, dtV, W, &dt))             return ZYM_ERROR;
    iam_rebase_float(id, channel, (float)target, (float)dt);
    return zym_newNull();
}

ZymValue u_anim_rebase_int(ZymVM* vm, ZymValue /*self*/,
                           ZymValue idV, ZymValue chV, ZymValue tgtV,
                           ZymValue dtV) {
    const char* W = "ui.animRebaseInt(id, channelId, newTarget, dt)";
    ImGuiID id, channel;
    int target;
    double dt;
    if (!parseImGuiId(vm, idV, W, &id))       return ZYM_ERROR;
    if (!parseImGuiId(vm, chV, W, &channel))  return ZYM_ERROR;
    if (!reqInt(vm, tgtV, W, &target))        return ZYM_ERROR;
    if (!reqNum(vm, dtV, W, &dt))             return ZYM_ERROR;
    iam_rebase_int(id, channel, target, (float)dt);
    return zym_newNull();
}

ZymValue u_anim_rebase_vec2(ZymVM* vm, ZymValue /*self*/,
                            ZymValue idV, ZymValue chV, ZymValue tgtV,
                            ZymValue dtV) {
    const char* W = "ui.animRebaseVec2(id, channelId, newTarget, dt)";
    ImGuiID id, channel;
    ImVec2 target;
    double dt;
    if (!parseImGuiId(vm, idV, W, &id))       return ZYM_ERROR;
    if (!parseImGuiId(vm, chV, W, &channel))  return ZYM_ERROR;
    if (!reqVec2(vm, tgtV, W, &target))       return ZYM_ERROR;
    if (!reqNum(vm, dtV, W, &dt))             return ZYM_ERROR;
    iam_rebase_vec2(id, channel, target, (float)dt);
    return zym_newNull();
}

ZymValue u_anim_rebase_vec4(ZymVM* vm, ZymValue /*self*/,
                            ZymValue idV, ZymValue chV, ZymValue tgtV,
                            ZymValue dtV) {
    const char* W = "ui.animRebaseVec4(id, channelId, newTarget, dt)";
    ImGuiID id, channel;
    ImVec4 target;
    double dt;
    if (!parseImGuiId(vm, idV, W, &id))       return ZYM_ERROR;
    if (!parseImGuiId(vm, chV, W, &channel))  return ZYM_ERROR;
    if (!reqVec4(vm, tgtV, W, &target))       return ZYM_ERROR;
    if (!reqNum(vm, dtV, W, &dt))             return ZYM_ERROR;
    iam_rebase_vec4(id, channel, target, (float)dt);
    return zym_newNull();
}

ZymValue u_anim_rebase_color(ZymVM* vm, ZymValue /*self*/,
                             ZymValue idV, ZymValue chV, ZymValue tgtV,
                             ZymValue dtV) {
    const char* W = "ui.animRebaseColor(id, channelId, newTarget, dt)";
    ImGuiID id, channel;
    ImVec4 target;
    double dt;
    if (!parseImGuiId(vm, idV, W, &id))       return ZYM_ERROR;
    if (!parseImGuiId(vm, chV, W, &channel))  return ZYM_ERROR;
    if (!reqVec4(vm, tgtV, W, &target))       return ZYM_ERROR;
    if (!reqNum(vm, dtV, W, &dt))             return ZYM_ERROR;
    iam_rebase_color(id, channel, target, (float)dt);
    return zym_newNull();
}

// --- Scroll helpers -----------------------------------------------------
//
// All four operate on the current ImGui window; an active frame is
// required. The `ease` argument is mandatory on the Zym side (no C++
// default-arg trick), passed as either a preset int or a 5-list.

ZymValue u_anim_scroll_to_y(ZymVM* vm, ZymValue /*self*/,
                            ZymValue yV, ZymValue durV, ZymValue ezV) {
    const char* W = "ui.animScrollToY(targetY, dur, ease)";
    double y, dur;
    iam_ease_desc ez;
    if (!requireFrame(vm, "ui.animScrollToY"))    return ZYM_ERROR;
    if (!reqNum(vm, yV, W, &y))                   return ZYM_ERROR;
    if (!reqNum(vm, durV, W, &dur))               return ZYM_ERROR;
    if (!parseEaseDesc(vm, ezV, W, &ez))          return ZYM_ERROR;
    iam_scroll_to_y((float)y, (float)dur, ez);
    return zym_newNull();
}

ZymValue u_anim_scroll_to_x(ZymVM* vm, ZymValue /*self*/,
                            ZymValue xV, ZymValue durV, ZymValue ezV) {
    const char* W = "ui.animScrollToX(targetX, dur, ease)";
    double x, dur;
    iam_ease_desc ez;
    if (!requireFrame(vm, "ui.animScrollToX"))    return ZYM_ERROR;
    if (!reqNum(vm, xV, W, &x))                   return ZYM_ERROR;
    if (!reqNum(vm, durV, W, &dur))               return ZYM_ERROR;
    if (!parseEaseDesc(vm, ezV, W, &ez))          return ZYM_ERROR;
    iam_scroll_to_x((float)x, (float)dur, ez);
    return zym_newNull();
}

ZymValue u_anim_scroll_to_top(ZymVM* vm, ZymValue /*self*/,
                              ZymValue durV, ZymValue ezV) {
    const char* W = "ui.animScrollToTop(dur, ease)";
    double dur;
    iam_ease_desc ez;
    if (!requireFrame(vm, "ui.animScrollToTop")) return ZYM_ERROR;
    if (!reqNum(vm, durV, W, &dur))              return ZYM_ERROR;
    if (!parseEaseDesc(vm, ezV, W, &ez))         return ZYM_ERROR;
    iam_scroll_to_top((float)dur, ez);
    return zym_newNull();
}

ZymValue u_anim_scroll_to_bottom(ZymVM* vm, ZymValue /*self*/,
                                 ZymValue durV, ZymValue ezV) {
    const char* W = "ui.animScrollToBottom(dur, ease)";
    double dur;
    iam_ease_desc ez;
    if (!requireFrame(vm, "ui.animScrollToBottom")) return ZYM_ERROR;
    if (!reqNum(vm, durV, W, &dur))                 return ZYM_ERROR;
    if (!parseEaseDesc(vm, ezV, W, &ez))            return ZYM_ERROR;
    iam_scroll_to_bottom((float)dur, ez);
    return zym_newNull();
}

// ==== SECTION 4: Oscillate / shake / wiggle =============================
//
// Stateful per-channel generators driven by `ImGuiID`. Each call returns
// the current sample for the given (id, dt) pair. Unlike tweens, these
// don't have a target — they run forever (oscillate, wiggle) or until
// the configured `decayTime` elapses (shake). `animTriggerShake` resets
// the shake clock so the same id can re-fire on demand.
//
// Color variants use the same `colorSpace` enum as tween/color helpers
// (`UI.ANIM_COL_*`); wave-type variants use `UI.ANIM_WAVE_*`.

// --- Oscillate -----------------------------------------------------------

ZymValue u_anim_oscillate(ZymVM* vm, ZymValue /*self*/,
                          ZymValue idV, ZymValue ampV, ZymValue freqV,
                          ZymValue waveV, ZymValue phaseV, ZymValue dtV) {
    const char* W = "ui.animOscillate(id, amplitude, frequency, waveType, phase, dt)";
    ImGuiID id;
    double amp, freq, phase, dt;
    int wave;
    if (!parseImGuiId(vm, idV, W, &id))  return ZYM_ERROR;
    if (!reqNum(vm, ampV,   W, &amp))    return ZYM_ERROR;
    if (!reqNum(vm, freqV,  W, &freq))   return ZYM_ERROR;
    if (!reqInt(vm, waveV,  W, &wave))   return ZYM_ERROR;
    if (!reqNum(vm, phaseV, W, &phase))  return ZYM_ERROR;
    if (!reqNum(vm, dtV,    W, &dt))     return ZYM_ERROR;
    return zym_newNumber((double)iam_oscillate(
        id, (float)amp, (float)freq, wave, (float)phase, (float)dt));
}

ZymValue u_anim_oscillate_int(ZymVM* vm, ZymValue /*self*/,
                              ZymValue idV, ZymValue ampV, ZymValue freqV,
                              ZymValue waveV, ZymValue phaseV, ZymValue dtV) {
    const char* W = "ui.animOscillateInt(id, amplitude, frequency, waveType, phase, dt)";
    ImGuiID id;
    int amp, wave;
    double freq, phase, dt;
    if (!parseImGuiId(vm, idV, W, &id))  return ZYM_ERROR;
    if (!reqInt(vm, ampV,   W, &amp))    return ZYM_ERROR;
    if (!reqNum(vm, freqV,  W, &freq))   return ZYM_ERROR;
    if (!reqInt(vm, waveV,  W, &wave))   return ZYM_ERROR;
    if (!reqNum(vm, phaseV, W, &phase))  return ZYM_ERROR;
    if (!reqNum(vm, dtV,    W, &dt))     return ZYM_ERROR;
    return zym_newNumber((double)iam_oscillate_int(
        id, amp, (float)freq, wave, (float)phase, (float)dt));
}

ZymValue u_anim_oscillate_vec2(ZymVM* vm, ZymValue /*self*/,
                               ZymValue idV, ZymValue ampV, ZymValue freqV,
                               ZymValue waveV, ZymValue phaseV, ZymValue dtV) {
    const char* W = "ui.animOscillateVec2(id, amplitude, frequency, waveType, phase, dt)";
    ImGuiID id;
    ImVec2 amp, freq, phase;
    int wave;
    double dt;
    if (!parseImGuiId(vm, idV, W, &id))      return ZYM_ERROR;
    if (!reqVec2(vm, ampV,   W, &amp))       return ZYM_ERROR;
    if (!reqVec2(vm, freqV,  W, &freq))      return ZYM_ERROR;
    if (!reqInt(vm, waveV,   W, &wave))      return ZYM_ERROR;
    if (!reqVec2(vm, phaseV, W, &phase))     return ZYM_ERROR;
    if (!reqNum(vm, dtV,     W, &dt))        return ZYM_ERROR;
    return packVec2(vm, iam_oscillate_vec2(
        id, amp, freq, wave, phase, (float)dt));
}

ZymValue u_anim_oscillate_vec4(ZymVM* vm, ZymValue /*self*/,
                               ZymValue idV, ZymValue ampV, ZymValue freqV,
                               ZymValue waveV, ZymValue phaseV, ZymValue dtV) {
    const char* W = "ui.animOscillateVec4(id, amplitude, frequency, waveType, phase, dt)";
    ImGuiID id;
    ImVec4 amp, freq, phase;
    int wave;
    double dt;
    if (!parseImGuiId(vm, idV, W, &id))      return ZYM_ERROR;
    if (!reqVec4(vm, ampV,   W, &amp))       return ZYM_ERROR;
    if (!reqVec4(vm, freqV,  W, &freq))      return ZYM_ERROR;
    if (!reqInt(vm, waveV,   W, &wave))      return ZYM_ERROR;
    if (!reqVec4(vm, phaseV, W, &phase))     return ZYM_ERROR;
    if (!reqNum(vm, dtV,     W, &dt))        return ZYM_ERROR;
    return packVec4(vm, iam_oscillate_vec4(
        id, amp, freq, wave, phase, (float)dt));
}

ZymValue u_anim_oscillate_color(ZymVM* vm, ZymValue /*self*/,
                                ZymValue idV, ZymValue baseV, ZymValue ampV,
                                ZymValue freqV, ZymValue waveV, ZymValue phaseV,
                                ZymValue spaceV, ZymValue dtV) {
    const char* W = "ui.animOscillateColor(id, baseColor, amplitude, frequency, waveType, phase, colorSpace, dt)";
    ImGuiID id;
    ImVec4 base, amp;
    double freq, phase, dt;
    int wave, space;
    if (!parseImGuiId(vm, idV, W, &id))  return ZYM_ERROR;
    if (!reqVec4(vm, baseV,  W, &base))  return ZYM_ERROR;
    if (!reqVec4(vm, ampV,   W, &amp))   return ZYM_ERROR;
    if (!reqNum(vm, freqV,   W, &freq))  return ZYM_ERROR;
    if (!reqInt(vm, waveV,   W, &wave))  return ZYM_ERROR;
    if (!reqNum(vm, phaseV,  W, &phase)) return ZYM_ERROR;
    if (!reqInt(vm, spaceV,  W, &space)) return ZYM_ERROR;
    if (!reqNum(vm, dtV,     W, &dt))    return ZYM_ERROR;
    return packVec4(vm, iam_oscillate_color(
        id, base, amp, (float)freq, wave, (float)phase, space, (float)dt));
}

// --- Shake (decaying random) --------------------------------------------

ZymValue u_anim_shake(ZymVM* vm, ZymValue /*self*/,
                      ZymValue idV, ZymValue intV, ZymValue freqV,
                      ZymValue decayV, ZymValue dtV) {
    const char* W = "ui.animShake(id, intensity, frequency, decayTime, dt)";
    ImGuiID id;
    double intensity, freq, decay, dt;
    if (!parseImGuiId(vm, idV, W, &id))      return ZYM_ERROR;
    if (!reqNum(vm, intV,   W, &intensity))  return ZYM_ERROR;
    if (!reqNum(vm, freqV,  W, &freq))       return ZYM_ERROR;
    if (!reqNum(vm, decayV, W, &decay))      return ZYM_ERROR;
    if (!reqNum(vm, dtV,    W, &dt))         return ZYM_ERROR;
    return zym_newNumber((double)iam_shake(
        id, (float)intensity, (float)freq, (float)decay, (float)dt));
}

ZymValue u_anim_shake_int(ZymVM* vm, ZymValue /*self*/,
                          ZymValue idV, ZymValue intV, ZymValue freqV,
                          ZymValue decayV, ZymValue dtV) {
    const char* W = "ui.animShakeInt(id, intensity, frequency, decayTime, dt)";
    ImGuiID id;
    int intensity;
    double freq, decay, dt;
    if (!parseImGuiId(vm, idV, W, &id))      return ZYM_ERROR;
    if (!reqInt(vm, intV,   W, &intensity))  return ZYM_ERROR;
    if (!reqNum(vm, freqV,  W, &freq))       return ZYM_ERROR;
    if (!reqNum(vm, decayV, W, &decay))      return ZYM_ERROR;
    if (!reqNum(vm, dtV,    W, &dt))         return ZYM_ERROR;
    return zym_newNumber((double)iam_shake_int(
        id, intensity, (float)freq, (float)decay, (float)dt));
}

ZymValue u_anim_shake_vec2(ZymVM* vm, ZymValue /*self*/,
                           ZymValue idV, ZymValue intV, ZymValue freqV,
                           ZymValue decayV, ZymValue dtV) {
    const char* W = "ui.animShakeVec2(id, intensity, frequency, decayTime, dt)";
    ImGuiID id;
    ImVec2 intensity;
    double freq, decay, dt;
    if (!parseImGuiId(vm, idV, W, &id))      return ZYM_ERROR;
    if (!reqVec2(vm, intV,   W, &intensity)) return ZYM_ERROR;
    if (!reqNum(vm, freqV,  W, &freq))       return ZYM_ERROR;
    if (!reqNum(vm, decayV, W, &decay))      return ZYM_ERROR;
    if (!reqNum(vm, dtV,    W, &dt))         return ZYM_ERROR;
    return packVec2(vm, iam_shake_vec2(
        id, intensity, (float)freq, (float)decay, (float)dt));
}

ZymValue u_anim_shake_vec4(ZymVM* vm, ZymValue /*self*/,
                           ZymValue idV, ZymValue intV, ZymValue freqV,
                           ZymValue decayV, ZymValue dtV) {
    const char* W = "ui.animShakeVec4(id, intensity, frequency, decayTime, dt)";
    ImGuiID id;
    ImVec4 intensity;
    double freq, decay, dt;
    if (!parseImGuiId(vm, idV, W, &id))      return ZYM_ERROR;
    if (!reqVec4(vm, intV,   W, &intensity)) return ZYM_ERROR;
    if (!reqNum(vm, freqV,  W, &freq))       return ZYM_ERROR;
    if (!reqNum(vm, decayV, W, &decay))      return ZYM_ERROR;
    if (!reqNum(vm, dtV,    W, &dt))         return ZYM_ERROR;
    return packVec4(vm, iam_shake_vec4(
        id, intensity, (float)freq, (float)decay, (float)dt));
}

ZymValue u_anim_shake_color(ZymVM* vm, ZymValue /*self*/,
                            ZymValue idV, ZymValue baseV, ZymValue intV,
                            ZymValue freqV, ZymValue decayV, ZymValue spaceV,
                            ZymValue dtV) {
    const char* W = "ui.animShakeColor(id, baseColor, intensity, frequency, decayTime, colorSpace, dt)";
    ImGuiID id;
    ImVec4 base, intensity;
    double freq, decay, dt;
    int space;
    if (!parseImGuiId(vm, idV, W, &id))      return ZYM_ERROR;
    if (!reqVec4(vm, baseV,  W, &base))      return ZYM_ERROR;
    if (!reqVec4(vm, intV,   W, &intensity)) return ZYM_ERROR;
    if (!reqNum(vm, freqV,   W, &freq))      return ZYM_ERROR;
    if (!reqNum(vm, decayV,  W, &decay))     return ZYM_ERROR;
    if (!reqInt(vm, spaceV,  W, &space))     return ZYM_ERROR;
    if (!reqNum(vm, dtV,     W, &dt))        return ZYM_ERROR;
    return packVec4(vm, iam_shake_color(
        id, base, intensity, (float)freq, (float)decay, space, (float)dt));
}

// --- Wiggle (continuous smooth noise) -----------------------------------

ZymValue u_anim_wiggle(ZymVM* vm, ZymValue /*self*/,
                       ZymValue idV, ZymValue ampV, ZymValue freqV,
                       ZymValue dtV) {
    const char* W = "ui.animWiggle(id, amplitude, frequency, dt)";
    ImGuiID id;
    double amp, freq, dt;
    if (!parseImGuiId(vm, idV, W, &id))  return ZYM_ERROR;
    if (!reqNum(vm, ampV,   W, &amp))    return ZYM_ERROR;
    if (!reqNum(vm, freqV,  W, &freq))   return ZYM_ERROR;
    if (!reqNum(vm, dtV,    W, &dt))     return ZYM_ERROR;
    return zym_newNumber((double)iam_wiggle(
        id, (float)amp, (float)freq, (float)dt));
}

ZymValue u_anim_wiggle_int(ZymVM* vm, ZymValue /*self*/,
                           ZymValue idV, ZymValue ampV, ZymValue freqV,
                           ZymValue dtV) {
    const char* W = "ui.animWiggleInt(id, amplitude, frequency, dt)";
    ImGuiID id;
    int amp;
    double freq, dt;
    if (!parseImGuiId(vm, idV, W, &id))  return ZYM_ERROR;
    if (!reqInt(vm, ampV,   W, &amp))    return ZYM_ERROR;
    if (!reqNum(vm, freqV,  W, &freq))   return ZYM_ERROR;
    if (!reqNum(vm, dtV,    W, &dt))     return ZYM_ERROR;
    return zym_newNumber((double)iam_wiggle_int(
        id, amp, (float)freq, (float)dt));
}

ZymValue u_anim_wiggle_vec2(ZymVM* vm, ZymValue /*self*/,
                            ZymValue idV, ZymValue ampV, ZymValue freqV,
                            ZymValue dtV) {
    const char* W = "ui.animWiggleVec2(id, amplitude, frequency, dt)";
    ImGuiID id;
    ImVec2 amp;
    double freq, dt;
    if (!parseImGuiId(vm, idV, W, &id))  return ZYM_ERROR;
    if (!reqVec2(vm, ampV,  W, &amp))    return ZYM_ERROR;
    if (!reqNum(vm, freqV,  W, &freq))   return ZYM_ERROR;
    if (!reqNum(vm, dtV,    W, &dt))     return ZYM_ERROR;
    return packVec2(vm, iam_wiggle_vec2(
        id, amp, (float)freq, (float)dt));
}

ZymValue u_anim_wiggle_vec4(ZymVM* vm, ZymValue /*self*/,
                            ZymValue idV, ZymValue ampV, ZymValue freqV,
                            ZymValue dtV) {
    const char* W = "ui.animWiggleVec4(id, amplitude, frequency, dt)";
    ImGuiID id;
    ImVec4 amp;
    double freq, dt;
    if (!parseImGuiId(vm, idV, W, &id))  return ZYM_ERROR;
    if (!reqVec4(vm, ampV,  W, &amp))    return ZYM_ERROR;
    if (!reqNum(vm, freqV,  W, &freq))   return ZYM_ERROR;
    if (!reqNum(vm, dtV,    W, &dt))     return ZYM_ERROR;
    return packVec4(vm, iam_wiggle_vec4(
        id, amp, (float)freq, (float)dt));
}

ZymValue u_anim_wiggle_color(ZymVM* vm, ZymValue /*self*/,
                             ZymValue idV, ZymValue baseV, ZymValue ampV,
                             ZymValue freqV, ZymValue spaceV, ZymValue dtV) {
    const char* W = "ui.animWiggleColor(id, baseColor, amplitude, frequency, colorSpace, dt)";
    ImGuiID id;
    ImVec4 base, amp;
    double freq, dt;
    int space;
    if (!parseImGuiId(vm, idV, W, &id))  return ZYM_ERROR;
    if (!reqVec4(vm, baseV,  W, &base))  return ZYM_ERROR;
    if (!reqVec4(vm, ampV,   W, &amp))   return ZYM_ERROR;
    if (!reqNum(vm, freqV,   W, &freq))  return ZYM_ERROR;
    if (!reqInt(vm, spaceV,  W, &space)) return ZYM_ERROR;
    if (!reqNum(vm, dtV,     W, &dt))    return ZYM_ERROR;
    return packVec4(vm, iam_wiggle_color(
        id, base, amp, (float)freq, space, (float)dt));
}

// --- Shake trigger -------------------------------------------------------
//
// Resets the per-id shake clock so the next `animShake*` call on the
// same id starts a fresh decay cycle. Call once on the frame the impact
// occurs.
ZymValue u_anim_trigger_shake(ZymVM* vm, ZymValue /*self*/, ZymValue idV) {
    ImGuiID id;
    if (!parseImGuiId(vm, idV, "ui.animTriggerShake(id)", &id)) return ZYM_ERROR;
    iam_trigger_shake(id);
    return zym_newNull();
}

// ==== SECTION 5: Drag feedback ==========================================
//
// Bindings for `iam_drag_begin/update/release/cancel` only. Per the
// user directive, the profiler (`iam_profiler_*`) and interactive debug
// UI (`iam_show_unified_inspector`, `iam_show_debug_timeline`) are NOT
// bound — Zym drives ImGui/ImPlot/ImAnim purely from code, so those
// upstream interactive debug surfaces have no role here.
//
// The `iam_drag_feedback` struct is packed as a 6-element list:
//
//   [ [posX, posY],
//     [offX, offY],
//     [velX, velY],
//     isDragging,
//     isSnapping,
//     snapProgress ]
//
// `animDragRelease` flattens the `iam_drag_opts` struct into positional
// args (no transient builder needed for what's a one-shot call). The
// `snapPoints` arg accepts either null/empty (no custom snap points) or
// a list of `[x, y]` lists; a transient `ImVec2[]` is materialised on
// the C stack for the duration of the upstream call.

// Pack an `iam_drag_feedback` into the canonical 6-list shape.
static ZymValue packDragFeedback(ZymVM* vm, iam_drag_feedback const& f) {
    ZymValue l = zym_newList(vm);
    zym_listAppend(vm, l, packVec2(vm, f.position));
    zym_listAppend(vm, l, packVec2(vm, f.offset));
    zym_listAppend(vm, l, packVec2(vm, f.velocity));
    zym_listAppend(vm, l, zym_newBool(f.is_dragging));
    zym_listAppend(vm, l, zym_newBool(f.is_snapping));
    zym_listAppend(vm, l, zym_newNumber((double)f.snap_progress));
    return l;
}

// `ui.animDragBegin(id, pos) -> feedback`
//   Starts tracking a drag at the given screen position. The returned
//   feedback's `isDragging` will be true.
ZymValue u_anim_drag_begin(ZymVM* vm, ZymValue /*self*/,
                           ZymValue idV, ZymValue posV) {
    const char* W = "ui.animDragBegin(id, pos)";
    ImGuiID id;
    ImVec2 pos;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    if (!reqVec2(vm, posV, W, &pos))    return ZYM_ERROR;
    return packDragFeedback(vm, iam_drag_begin(id, pos));
}

// `ui.animDragUpdate(id, pos, dt) -> feedback`
//   Updates the drag position during an active drag. Velocity is
//   estimated from successive positions using `dt`.
ZymValue u_anim_drag_update(ZymVM* vm, ZymValue /*self*/,
                            ZymValue idV, ZymValue posV, ZymValue dtV) {
    const char* W = "ui.animDragUpdate(id, pos, dt)";
    ImGuiID id;
    ImVec2 pos;
    double dt;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    if (!reqVec2(vm, posV, W, &pos))    return ZYM_ERROR;
    if (!reqNum(vm, dtV,  W, &dt))      return ZYM_ERROR;
    return packDragFeedback(vm, iam_drag_update(id, pos, (float)dt));
}

// `ui.animDragRelease(id, pos, snapGrid, snapPoints, snapDuration,
//                      overshoot, easeType, dt) -> feedback`
//
//   `snapGrid`     : [w, h] grid cell size, or [0, 0] for no grid.
//   `snapPoints`   : null or a list of [x, y] custom snap points.
//                    Materialised onto a transient stack buffer for
//                    the duration of the call — no GC roots needed
//                    since `iam_drag_release` consumes them eagerly.
//   `snapDuration` : seconds for the snap-back animation.
//   `overshoot`    : 0 = none, 1 = normal (multiplier on snap motion).
//   `easeType`     : `UI.ANIM_EASE_*` preset for the snap animation.
//   `dt`           : frame delta seconds.
ZymValue u_anim_drag_release(ZymVM* vm, ZymValue /*self*/,
                             ZymValue idV, ZymValue posV,
                             ZymValue gridV, ZymValue pointsV,
                             ZymValue durV, ZymValue overV,
                             ZymValue easeV, ZymValue dtV) {
    const char* W = "ui.animDragRelease(id, pos, snapGrid, snapPoints, snapDuration, overshoot, easeType, dt)";
    ImGuiID id;
    ImVec2 pos, grid;
    double dur, over, dt;
    int ease;
    if (!parseImGuiId(vm, idV, W, &id))   return ZYM_ERROR;
    if (!reqVec2(vm, posV,  W, &pos))     return ZYM_ERROR;
    if (!reqVec2(vm, gridV, W, &grid))    return ZYM_ERROR;
    if (!reqNum(vm, durV,   W, &dur))     return ZYM_ERROR;
    if (!reqNum(vm, overV,  W, &over))    return ZYM_ERROR;
    if (!reqInt(vm, easeV,  W, &ease))    return ZYM_ERROR;
    if (!reqNum(vm, dtV,    W, &dt))      return ZYM_ERROR;

    // Materialise optional custom snap points onto a transient buffer.
    // Upstream copies / consumes them within the call, so a stack-like
    // ImVector is fine — no need to root anything.
    ImVector<ImVec2> snapBuf;
    if (!zym_isNull(pointsV)) {
        if (!zym_isList(pointsV)) {
            zym_runtimeError(vm,
                "%s snapPoints must be null or a list of [x, y] pairs", W);
            return ZYM_ERROR;
        }
        int n = (int)zym_listLength(pointsV);
        snapBuf.reserve(n);
        for (int i = 0; i < n; ++i) {
            ImVec2 p;
            if (!reqVec2(vm, zym_listGet(vm, pointsV, i), W, &p)) return ZYM_ERROR;
            snapBuf.push_back(p);
        }
    }

    iam_drag_opts opts;
    opts.snap_grid         = grid;
    opts.snap_points       = snapBuf.empty() ? nullptr : snapBuf.Data;
    opts.snap_points_count = snapBuf.Size;
    opts.snap_duration     = (float)dur;
    opts.overshoot         = (float)over;
    opts.ease_type         = ease;

    return packDragFeedback(vm, iam_drag_release(id, pos, opts, (float)dt));
}

// `ui.animDragCancel(id)` — drop drag-tracking state for `id` without
// firing a snap animation.
ZymValue u_anim_drag_cancel(ZymVM* vm, ZymValue /*self*/, ZymValue idV) {
    ImGuiID id;
    if (!parseImGuiId(vm, idV, "ui.animDragCancel(id)", &id)) return ZYM_ERROR;
    iam_drag_cancel(id);
    return zym_newNull();
}

// ==== SECTION 6: Motion paths ============================================
//
// Static-curve evaluators (quadratic/cubic bezier + catmull-rom and
// their derivatives), the fluent `iam_path` builder reshaped into a
// procedural surface keyed by the path's `ImGuiID`, path-query helpers,
// the two along-path tweens, and the arc-length parameterization API.
//
// `iam_path` upstream is a thin RAII wrapper over file-local builder
// state (`g_building_path` / `g_current_point`) — there's nothing to
// hold onto on the Zym side besides the ImGuiID that identifies the
// path in the global pool. So the binding looks like:
//
//   local pid = UI.animPathBegin("zoom", [0,0])    -- returns ImGuiID
//   UI.animPathLineTo([100, 0])
//   UI.animPathCubicTo([120, 50], [160, 50], [200, 0])
//   UI.animPathClose()
//   UI.animPathEnd()
//
// Builder calls after `Begin` and before `End` mutate the upstream
// in-progress builder; we don't take a handle on the Zym side because
// the upstream API itself relies on a singleton in-progress state.

// --- Stateless curve evaluators ------------------------------------------

ZymValue u_anim_bezier_quadratic(ZymVM* vm, ZymValue /*self*/,
                                 ZymValue p0V, ZymValue p1V, ZymValue p2V,
                                 ZymValue tV) {
    const char* W = "ui.animBezierQuadratic(p0, p1, p2, t)";
    ImVec2 p0, p1, p2; double t;
    if (!reqVec2(vm, p0V, W, &p0)) return ZYM_ERROR;
    if (!reqVec2(vm, p1V, W, &p1)) return ZYM_ERROR;
    if (!reqVec2(vm, p2V, W, &p2)) return ZYM_ERROR;
    if (!reqNum(vm, tV,  W, &t))   return ZYM_ERROR;
    return packVec2(vm, iam_bezier_quadratic(p0, p1, p2, (float)t));
}

ZymValue u_anim_bezier_cubic(ZymVM* vm, ZymValue /*self*/,
                             ZymValue p0V, ZymValue p1V,
                             ZymValue p2V, ZymValue p3V, ZymValue tV) {
    const char* W = "ui.animBezierCubic(p0, p1, p2, p3, t)";
    ImVec2 p0, p1, p2, p3; double t;
    if (!reqVec2(vm, p0V, W, &p0)) return ZYM_ERROR;
    if (!reqVec2(vm, p1V, W, &p1)) return ZYM_ERROR;
    if (!reqVec2(vm, p2V, W, &p2)) return ZYM_ERROR;
    if (!reqVec2(vm, p3V, W, &p3)) return ZYM_ERROR;
    if (!reqNum(vm, tV,  W, &t))   return ZYM_ERROR;
    return packVec2(vm, iam_bezier_cubic(p0, p1, p2, p3, (float)t));
}

ZymValue u_anim_catmull_rom(ZymVM* vm, ZymValue /*self*/,
                            ZymValue p0V, ZymValue p1V,
                            ZymValue p2V, ZymValue p3V,
                            ZymValue tV, ZymValue tensionV) {
    const char* W = "ui.animCatmullRom(p0, p1, p2, p3, t, tension)";
    ImVec2 p0, p1, p2, p3; double t, tension;
    if (!reqVec2(vm, p0V, W, &p0)) return ZYM_ERROR;
    if (!reqVec2(vm, p1V, W, &p1)) return ZYM_ERROR;
    if (!reqVec2(vm, p2V, W, &p2)) return ZYM_ERROR;
    if (!reqVec2(vm, p3V, W, &p3)) return ZYM_ERROR;
    if (!reqNum(vm, tV,       W, &t))       return ZYM_ERROR;
    if (!reqNum(vm, tensionV, W, &tension)) return ZYM_ERROR;
    return packVec2(vm, iam_catmull_rom(p0, p1, p2, p3, (float)t, (float)tension));
}

ZymValue u_anim_bezier_quadratic_deriv(ZymVM* vm, ZymValue /*self*/,
                                       ZymValue p0V, ZymValue p1V, ZymValue p2V,
                                       ZymValue tV) {
    const char* W = "ui.animBezierQuadraticDeriv(p0, p1, p2, t)";
    ImVec2 p0, p1, p2; double t;
    if (!reqVec2(vm, p0V, W, &p0)) return ZYM_ERROR;
    if (!reqVec2(vm, p1V, W, &p1)) return ZYM_ERROR;
    if (!reqVec2(vm, p2V, W, &p2)) return ZYM_ERROR;
    if (!reqNum(vm, tV,  W, &t))   return ZYM_ERROR;
    return packVec2(vm, iam_bezier_quadratic_deriv(p0, p1, p2, (float)t));
}

ZymValue u_anim_bezier_cubic_deriv(ZymVM* vm, ZymValue /*self*/,
                                   ZymValue p0V, ZymValue p1V,
                                   ZymValue p2V, ZymValue p3V, ZymValue tV) {
    const char* W = "ui.animBezierCubicDeriv(p0, p1, p2, p3, t)";
    ImVec2 p0, p1, p2, p3; double t;
    if (!reqVec2(vm, p0V, W, &p0)) return ZYM_ERROR;
    if (!reqVec2(vm, p1V, W, &p1)) return ZYM_ERROR;
    if (!reqVec2(vm, p2V, W, &p2)) return ZYM_ERROR;
    if (!reqVec2(vm, p3V, W, &p3)) return ZYM_ERROR;
    if (!reqNum(vm, tV,  W, &t))   return ZYM_ERROR;
    return packVec2(vm, iam_bezier_cubic_deriv(p0, p1, p2, p3, (float)t));
}

ZymValue u_anim_catmull_rom_deriv(ZymVM* vm, ZymValue /*self*/,
                                  ZymValue p0V, ZymValue p1V,
                                  ZymValue p2V, ZymValue p3V,
                                  ZymValue tV, ZymValue tensionV) {
    const char* W = "ui.animCatmullRomDeriv(p0, p1, p2, p3, t, tension)";
    ImVec2 p0, p1, p2, p3; double t, tension;
    if (!reqVec2(vm, p0V, W, &p0)) return ZYM_ERROR;
    if (!reqVec2(vm, p1V, W, &p1)) return ZYM_ERROR;
    if (!reqVec2(vm, p2V, W, &p2)) return ZYM_ERROR;
    if (!reqVec2(vm, p3V, W, &p3)) return ZYM_ERROR;
    if (!reqNum(vm, tV,       W, &t))       return ZYM_ERROR;
    if (!reqNum(vm, tensionV, W, &tension)) return ZYM_ERROR;
    return packVec2(vm, iam_catmull_rom_deriv(p0, p1, p2, p3, (float)t, (float)tension));
}

// --- Path builder (file-local in-progress state upstream) ---------------
//
// Upstream `iam_path::begin` returns a value-type wrapper carrying the
// path id; subsequent `line_to`/`quadratic_to`/... calls operate on the
// singleton in-progress builder rather than the returned object. We
// store the active builder in a static so script-side builder calls can
// dispatch without holding a handle, and return the path's ImGuiID from
// `animPathBegin` so scripts can reference the path later.

static iam_path* g_active_path_builder = nullptr;

// `ui.animPathBegin(pathId, start) -> ImGuiID` — start building. Returns
// the numeric path id so scripts can store it and pass it to evaluate /
// tween calls later (also accepts string ids upstream of the call).
ZymValue u_anim_path_begin(ZymVM* vm, ZymValue /*self*/,
                           ZymValue idV, ZymValue startV) {
    const char* W = "ui.animPathBegin(pathId, start)";
    ImGuiID id; ImVec2 start;
    if (!parseImGuiId(vm, idV, W, &id))    return ZYM_ERROR;
    if (!reqVec2(vm, startV, W, &start))   return ZYM_ERROR;
    if (g_active_path_builder) {
        // Drop a previous unfinished builder rather than leak — happens
        // when a script forgets `animPathEnd`. Upstream begin() already
        // resets the file-local state, so we just delete our wrapper.
        delete g_active_path_builder;
        g_active_path_builder = nullptr;
    }
    g_active_path_builder = new iam_path(iam_path::begin(id, start));
    return zym_newNumber((double)id);
}

ZymValue u_anim_path_line_to(ZymVM* vm, ZymValue /*self*/, ZymValue endV) {
    const char* W = "ui.animPathLineTo(end)";
    if (!g_active_path_builder) {
        zym_runtimeError(vm, "%s called without an active animPathBegin", W);
        return ZYM_ERROR;
    }
    ImVec2 end;
    if (!reqVec2(vm, endV, W, &end)) return ZYM_ERROR;
    g_active_path_builder->line_to(end);
    return zym_newNull();
}

ZymValue u_anim_path_quadratic_to(ZymVM* vm, ZymValue /*self*/,
                                  ZymValue ctrlV, ZymValue endV) {
    const char* W = "ui.animPathQuadraticTo(ctrl, end)";
    if (!g_active_path_builder) {
        zym_runtimeError(vm, "%s called without an active animPathBegin", W);
        return ZYM_ERROR;
    }
    ImVec2 ctrl, end;
    if (!reqVec2(vm, ctrlV, W, &ctrl)) return ZYM_ERROR;
    if (!reqVec2(vm, endV,  W, &end))  return ZYM_ERROR;
    g_active_path_builder->quadratic_to(ctrl, end);
    return zym_newNull();
}

ZymValue u_anim_path_cubic_to(ZymVM* vm, ZymValue /*self*/,
                              ZymValue c1V, ZymValue c2V, ZymValue endV) {
    const char* W = "ui.animPathCubicTo(ctrl1, ctrl2, end)";
    if (!g_active_path_builder) {
        zym_runtimeError(vm, "%s called without an active animPathBegin", W);
        return ZYM_ERROR;
    }
    ImVec2 c1, c2, end;
    if (!reqVec2(vm, c1V,  W, &c1))  return ZYM_ERROR;
    if (!reqVec2(vm, c2V,  W, &c2))  return ZYM_ERROR;
    if (!reqVec2(vm, endV, W, &end)) return ZYM_ERROR;
    g_active_path_builder->cubic_to(c1, c2, end);
    return zym_newNull();
}

ZymValue u_anim_path_catmull_to(ZymVM* vm, ZymValue /*self*/,
                                ZymValue endV, ZymValue tensionV) {
    const char* W = "ui.animPathCatmullTo(end, tension)";
    if (!g_active_path_builder) {
        zym_runtimeError(vm, "%s called without an active animPathBegin", W);
        return ZYM_ERROR;
    }
    ImVec2 end; double tension;
    if (!reqVec2(vm, endV,     W, &end))     return ZYM_ERROR;
    if (!reqNum(vm,  tensionV, W, &tension)) return ZYM_ERROR;
    g_active_path_builder->catmull_to(end, (float)tension);
    return zym_newNull();
}

ZymValue u_anim_path_close(ZymVM* vm, ZymValue /*self*/) {
    const char* W = "ui.animPathClose()";
    if (!g_active_path_builder) {
        zym_runtimeError(vm, "%s called without an active animPathBegin", W);
        return ZYM_ERROR;
    }
    g_active_path_builder->close();
    return zym_newNull();
}

// `ui.animPathEnd()` — finalize and register the in-progress path.
ZymValue u_anim_path_end(ZymVM* vm, ZymValue /*self*/) {
    const char* W = "ui.animPathEnd()";
    if (!g_active_path_builder) {
        zym_runtimeError(vm, "%s called without an active animPathBegin", W);
        return ZYM_ERROR;
    }
    g_active_path_builder->end();
    delete g_active_path_builder;
    g_active_path_builder = nullptr;
    return zym_newNull();
}

// --- Path queries -------------------------------------------------------

ZymValue u_anim_path_exists(ZymVM* vm, ZymValue /*self*/, ZymValue idV) {
    ImGuiID id;
    if (!parseImGuiId(vm, idV, "ui.animPathExists(pathId)", &id)) return ZYM_ERROR;
    return zym_newBool(iam_path_exists(id));
}

ZymValue u_anim_path_length(ZymVM* vm, ZymValue /*self*/, ZymValue idV) {
    ImGuiID id;
    if (!parseImGuiId(vm, idV, "ui.animPathLength(pathId)", &id)) return ZYM_ERROR;
    return zym_newNumber((double)iam_path_length(id));
}

ZymValue u_anim_path_evaluate(ZymVM* vm, ZymValue /*self*/,
                              ZymValue idV, ZymValue tV) {
    const char* W = "ui.animPathEvaluate(pathId, t)";
    ImGuiID id; double t;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    if (!reqNum(vm, tV,        W, &t))  return ZYM_ERROR;
    return packVec2(vm, iam_path_evaluate(id, (float)t));
}

ZymValue u_anim_path_tangent(ZymVM* vm, ZymValue /*self*/,
                             ZymValue idV, ZymValue tV) {
    const char* W = "ui.animPathTangent(pathId, t)";
    ImGuiID id; double t;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    if (!reqNum(vm, tV,        W, &t))  return ZYM_ERROR;
    return packVec2(vm, iam_path_tangent(id, (float)t));
}

ZymValue u_anim_path_angle(ZymVM* vm, ZymValue /*self*/,
                           ZymValue idV, ZymValue tV) {
    const char* W = "ui.animPathAngle(pathId, t)";
    ImGuiID id; double t;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    if (!reqNum(vm, tV,        W, &t))  return ZYM_ERROR;
    return zym_newNumber((double)iam_path_angle(id, (float)t));
}

// --- Along-path tweens --------------------------------------------------

ZymValue u_anim_tween_path(ZymVM* vm, ZymValue /*self*/,
                           ZymValue idV, ZymValue chV, ZymValue pathV,
                           ZymValue durV, ZymValue ezV, ZymValue policyV,
                           ZymValue dtV) {
    const char* W = "ui.animTweenPath(id, channelId, pathId, dur, ease, policy, dt)";
    ImGuiID id, ch, pid;
    double dur, dt;
    iam_ease_desc ez;
    int policy;
    if (!parseImGuiId(vm, idV,   W, &id))   return ZYM_ERROR;
    if (!parseImGuiId(vm, chV,   W, &ch))   return ZYM_ERROR;
    if (!parseImGuiId(vm, pathV, W, &pid))  return ZYM_ERROR;
    if (!reqNum(vm, durV,        W, &dur))  return ZYM_ERROR;
    if (!parseEaseDesc(vm, ezV,  W, &ez))   return ZYM_ERROR;
    if (!reqInt(vm, policyV,     W, &policy)) return ZYM_ERROR;
    if (!reqNum(vm, dtV,         W, &dt))   return ZYM_ERROR;
    return packVec2(vm, iam_tween_path(id, ch, pid, (float)dur, ez, policy, (float)dt));
}

ZymValue u_anim_tween_path_angle(ZymVM* vm, ZymValue /*self*/,
                                 ZymValue idV, ZymValue chV, ZymValue pathV,
                                 ZymValue durV, ZymValue ezV, ZymValue policyV,
                                 ZymValue dtV) {
    const char* W = "ui.animTweenPathAngle(id, channelId, pathId, dur, ease, policy, dt)";
    ImGuiID id, ch, pid;
    double dur, dt;
    iam_ease_desc ez;
    int policy;
    if (!parseImGuiId(vm, idV,   W, &id))   return ZYM_ERROR;
    if (!parseImGuiId(vm, chV,   W, &ch))   return ZYM_ERROR;
    if (!parseImGuiId(vm, pathV, W, &pid))  return ZYM_ERROR;
    if (!reqNum(vm, durV,        W, &dur))  return ZYM_ERROR;
    if (!parseEaseDesc(vm, ezV,  W, &ez))   return ZYM_ERROR;
    if (!reqInt(vm, policyV,     W, &policy)) return ZYM_ERROR;
    if (!reqNum(vm, dtV,         W, &dt))   return ZYM_ERROR;
    return zym_newNumber((double)iam_tween_path_angle(id, ch, pid, (float)dur, ez, policy, (float)dt));
}

// --- Arc-length parameterization ----------------------------------------

ZymValue u_anim_path_build_arc_lut(ZymVM* vm, ZymValue /*self*/,
                                   ZymValue idV, ZymValue subV) {
    const char* W = "ui.animPathBuildArcLut(pathId, subdivisions)";
    ImGuiID id; int sub;
    if (!parseImGuiId(vm, idV, W, &id))  return ZYM_ERROR;
    if (!reqInt(vm, subV,      W, &sub)) return ZYM_ERROR;
    iam_path_build_arc_lut(id, sub);
    return zym_newNull();
}

ZymValue u_anim_path_has_arc_lut(ZymVM* vm, ZymValue /*self*/, ZymValue idV) {
    ImGuiID id;
    if (!parseImGuiId(vm, idV, "ui.animPathHasArcLut(pathId)", &id)) return ZYM_ERROR;
    return zym_newBool(iam_path_has_arc_lut(id));
}

ZymValue u_anim_path_distance_to_t(ZymVM* vm, ZymValue /*self*/,
                                   ZymValue idV, ZymValue distV) {
    const char* W = "ui.animPathDistanceToT(pathId, distance)";
    ImGuiID id; double dist;
    if (!parseImGuiId(vm, idV, W, &id))   return ZYM_ERROR;
    if (!reqNum(vm, distV,     W, &dist)) return ZYM_ERROR;
    return zym_newNumber((double)iam_path_distance_to_t(id, (float)dist));
}

ZymValue u_anim_path_evaluate_at_distance(ZymVM* vm, ZymValue /*self*/,
                                          ZymValue idV, ZymValue distV) {
    const char* W = "ui.animPathEvaluateAtDistance(pathId, distance)";
    ImGuiID id; double dist;
    if (!parseImGuiId(vm, idV, W, &id))   return ZYM_ERROR;
    if (!reqNum(vm, distV,     W, &dist)) return ZYM_ERROR;
    return packVec2(vm, iam_path_evaluate_at_distance(id, (float)dist));
}

ZymValue u_anim_path_angle_at_distance(ZymVM* vm, ZymValue /*self*/,
                                       ZymValue idV, ZymValue distV) {
    const char* W = "ui.animPathAngleAtDistance(pathId, distance)";
    ImGuiID id; double dist;
    if (!parseImGuiId(vm, idV, W, &id))   return ZYM_ERROR;
    if (!reqNum(vm, distV,     W, &dist)) return ZYM_ERROR;
    return zym_newNumber((double)iam_path_angle_at_distance(id, (float)dist));
}

ZymValue u_anim_path_tangent_at_distance(ZymVM* vm, ZymValue /*self*/,
                                         ZymValue idV, ZymValue distV) {
    const char* W = "ui.animPathTangentAtDistance(pathId, distance)";
    ImGuiID id; double dist;
    if (!parseImGuiId(vm, idV, W, &id))   return ZYM_ERROR;
    if (!reqNum(vm, distV,     W, &dist)) return ZYM_ERROR;
    return packVec2(vm, iam_path_tangent_at_distance(id, (float)dist));
}

// ==== SECTION 7: Path morphing + text-along-path + quad xforms + stagger ==
//
// Options structs (`iam_morph_opts`, `iam_text_path_opts`,
// `iam_text_stagger_opts`) are flattened to positional Zym args. Upstream
// supplies sensible defaults via in-class constructors; we mirror those
// directly so script call-sites remain readable. The `ImFont*` slot of
// the text APIs is intentionally NOT exposed — Zym does not manage
// `ImFont*` handles, and "nullptr = current font" covers every existing
// upstream demo. If a script needs a non-current font it should `PushFont`
// around the call via the existing `UI.*` font surface.
//
// Color args are taken as ImVec4 (matching the rest of the binding) and
// converted to ImU32 with `ImGui::ColorConvertFloat4ToU32` so scripts
// don't have to reason about packed integer color.

// --- Path morphing ------------------------------------------------------

// Parse [samples, matchEndpoints, useArcLength] into an iam_morph_opts.
// Accepts null for "use upstream defaults".
static bool parseMorphOpts(ZymVM* vm, ZymValue v, const char* where,
                           iam_morph_opts* out) {
    *out = iam_morph_opts();  // defaults
    if (zym_isNull(v)) return true;
    if (!zym_isList(v) || zym_listLength(v) != 3) {
        zym_runtimeError(vm,
            "%s morphOpts must be null or [samples, matchEndpoints, useArcLength]",
            where);
        return false;
    }
    ZymValue sV  = zym_listGet(vm, v, 0);
    ZymValue meV = zym_listGet(vm, v, 1);
    ZymValue alV = zym_listGet(vm, v, 2);
    if (!zym_isNumber(sV) || !zym_isBool(meV) || !zym_isBool(alV)) {
        zym_runtimeError(vm,
            "%s morphOpts elements must be [number, bool, bool]", where);
        return false;
    }
    out->samples         = (int)zym_asNumber(sV);
    out->match_endpoints = zym_asBool(meV);
    out->use_arc_length  = zym_asBool(alV);
    return true;
}

ZymValue u_anim_path_morph(ZymVM* vm, ZymValue /*self*/,
                           ZymValue aV, ZymValue bV, ZymValue tV,
                           ZymValue blendV, ZymValue optsV) {
    const char* W = "ui.animPathMorph(pathA, pathB, t, blend, opts)";
    ImGuiID a, b; double t, blend; iam_morph_opts opts;
    if (!parseImGuiId(vm, aV, W, &a))      return ZYM_ERROR;
    if (!parseImGuiId(vm, bV, W, &b))      return ZYM_ERROR;
    if (!reqNum(vm, tV,     W, &t))        return ZYM_ERROR;
    if (!reqNum(vm, blendV, W, &blend))    return ZYM_ERROR;
    if (!parseMorphOpts(vm, optsV, W, &opts)) return ZYM_ERROR;
    return packVec2(vm, iam_path_morph(a, b, (float)t, (float)blend, opts));
}

ZymValue u_anim_path_morph_tangent(ZymVM* vm, ZymValue /*self*/,
                                   ZymValue aV, ZymValue bV, ZymValue tV,
                                   ZymValue blendV, ZymValue optsV) {
    const char* W = "ui.animPathMorphTangent(pathA, pathB, t, blend, opts)";
    ImGuiID a, b; double t, blend; iam_morph_opts opts;
    if (!parseImGuiId(vm, aV, W, &a))      return ZYM_ERROR;
    if (!parseImGuiId(vm, bV, W, &b))      return ZYM_ERROR;
    if (!reqNum(vm, tV,     W, &t))        return ZYM_ERROR;
    if (!reqNum(vm, blendV, W, &blend))    return ZYM_ERROR;
    if (!parseMorphOpts(vm, optsV, W, &opts)) return ZYM_ERROR;
    return packVec2(vm, iam_path_morph_tangent(a, b, (float)t, (float)blend, opts));
}

ZymValue u_anim_path_morph_angle(ZymVM* vm, ZymValue /*self*/,
                                 ZymValue aV, ZymValue bV, ZymValue tV,
                                 ZymValue blendV, ZymValue optsV) {
    const char* W = "ui.animPathMorphAngle(pathA, pathB, t, blend, opts)";
    ImGuiID a, b; double t, blend; iam_morph_opts opts;
    if (!parseImGuiId(vm, aV, W, &a))      return ZYM_ERROR;
    if (!parseImGuiId(vm, bV, W, &b))      return ZYM_ERROR;
    if (!reqNum(vm, tV,     W, &t))        return ZYM_ERROR;
    if (!reqNum(vm, blendV, W, &blend))    return ZYM_ERROR;
    if (!parseMorphOpts(vm, optsV, W, &opts)) return ZYM_ERROR;
    return zym_newNumber((double)iam_path_morph_angle(a, b, (float)t, (float)blend, opts));
}

// `ui.animTweenPathMorph(id, channelId, pathA, pathB, targetBlend, dur,
//                         pathEase, morphEase, policy, dt, opts) -> [x,y]`
//   Animates both the t-along-path AND the blend factor between the
//   two paths in a single tween. `opts` may be null.
// NOTE: bound as variadic — the closure signature parser caps at 10
// tokens total and `...` counts as one, leaving room for at most 9 fixed
// params. We keep (id, channelId, pathA, pathB, targetBlend, dur,
// pathEase, morphEase, policy) fixed and push the trailing `dt` + `opts`
// through vargs: vargs[0] => dt (required), vargs[1] => opts (null/list).
ZymValue u_anim_tween_path_morph(ZymVM* vm, ZymValue /*self*/,
                                 ZymValue idV, ZymValue chV,
                                 ZymValue aV, ZymValue bV,
                                 ZymValue targetBlendV, ZymValue durV,
                                 ZymValue pathEzV, ZymValue morphEzV,
                                 ZymValue policyV,
                                 ZymValue* vargs, int vargc) {
    const char* W = "ui.animTweenPathMorph(id, channelId, pathA, pathB, "
                    "targetBlend, dur, pathEase, morphEase, policy, ...)";
    ImGuiID id, ch, a, b;
    double targetBlend, dur, dt;
    iam_ease_desc pathEz, morphEz;
    int policy;
    iam_morph_opts opts;
    if (vargc < 1) {
        zym_runtimeError(vm, "%s expects `dt` as the 10th arg", W);
        return ZYM_ERROR;
    }
    ZymValue dtV   = vargs[0];
    ZymValue optsV = (vargc > 1) ? vargs[1] : zym_newNull();
    if (!parseImGuiId(vm, idV, W, &id))             return ZYM_ERROR;
    if (!parseImGuiId(vm, chV, W, &ch))             return ZYM_ERROR;
    if (!parseImGuiId(vm, aV,  W, &a))              return ZYM_ERROR;
    if (!parseImGuiId(vm, bV,  W, &b))              return ZYM_ERROR;
    if (!reqNum(vm, targetBlendV,    W, &targetBlend)) return ZYM_ERROR;
    if (!reqNum(vm, durV,            W, &dur))         return ZYM_ERROR;
    if (!parseEaseDesc(vm, pathEzV,  W, &pathEz))      return ZYM_ERROR;
    if (!parseEaseDesc(vm, morphEzV, W, &morphEz))     return ZYM_ERROR;
    if (!reqInt(vm, policyV,         W, &policy))      return ZYM_ERROR;
    if (!reqNum(vm, dtV,             W, &dt))          return ZYM_ERROR;
    if (!parseMorphOpts(vm, optsV,   W, &opts))        return ZYM_ERROR;
    return packVec2(vm, iam_tween_path_morph(id, ch, a, b,
                                             (float)targetBlend, (float)dur,
                                             pathEz, morphEz, policy,
                                             (float)dt, opts));
}

ZymValue u_anim_get_morph_blend(ZymVM* vm, ZymValue /*self*/,
                                ZymValue idV, ZymValue chV) {
    const char* W = "ui.animGetMorphBlend(id, channelId)";
    ImGuiID id, ch;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    if (!parseImGuiId(vm, chV, W, &ch)) return ZYM_ERROR;
    return zym_newNumber((double)iam_get_morph_blend(id, ch));
}

// --- Text-along-path ----------------------------------------------------

// Parse a flat positional options list:
//   [origin, offset, letterSpacing, align, flipY, color]
// or null for defaults. `color` is a [r,g,b,a] ImVec4. Font is omitted —
// always uses current ImGui font (see SECTION 7 banner).
static bool parseTextPathOpts(ZymVM* vm, ZymValue v, const char* where,
                              iam_text_path_opts* out) {
    *out = iam_text_path_opts();  // defaults
    if (zym_isNull(v)) return true;
    if (!zym_isList(v) || zym_listLength(v) != 6) {
        zym_runtimeError(vm,
            "%s textPathOpts must be null or "
            "[origin, offset, letterSpacing, align, flipY, color]", where);
        return false;
    }
    ImVec2 origin; double offset, spacing; int align; bool flipY; ImVec4 color;
    if (!reqVec2(vm, zym_listGet(vm, v, 0), where, &origin))      return false;
    ZymValue offV = zym_listGet(vm, v, 1);
    ZymValue spV  = zym_listGet(vm, v, 2);
    ZymValue alV  = zym_listGet(vm, v, 3);
    ZymValue fyV  = zym_listGet(vm, v, 4);
    if (!zym_isNumber(offV) || !zym_isNumber(spV) || !zym_isNumber(alV) ||
        !zym_isBool(fyV)) {
        zym_runtimeError(vm,
            "%s textPathOpts [offset, letterSpacing, align, flipY] type mismatch",
            where);
        return false;
    }
    offset  = zym_asNumber(offV);
    spacing = zym_asNumber(spV);
    align   = (int)zym_asNumber(alV);
    flipY   = zym_asBool(fyV);
    if (!reqVec4(vm, zym_listGet(vm, v, 5), where, &color))       return false;

    out->origin         = origin;
    out->offset         = (float)offset;
    out->letter_spacing = (float)spacing;
    out->align          = align;
    out->flip_y         = flipY;
    out->color          = ImGui::ColorConvertFloat4ToU32(color);
    // font / font_scale left at defaults (nullptr / 1.0).
    return true;
}

ZymValue u_anim_text_path(ZymVM* vm, ZymValue /*self*/,
                          ZymValue idV, ZymValue textV, ZymValue optsV) {
    const char* W = "ui.animTextPath(pathId, text, opts)";
    if (!requireFrame(vm, "ui.animTextPath")) return ZYM_ERROR;
    ImGuiID id; std::string text; iam_text_path_opts opts;
    if (!parseImGuiId(vm, idV, W, &id))               return ZYM_ERROR;
    if (!reqStr(vm, textV, W, &text))                 return ZYM_ERROR;
    if (!parseTextPathOpts(vm, optsV, W, &opts))      return ZYM_ERROR;
    iam_text_path(id, text.c_str(), opts);
    return zym_newNull();
}

ZymValue u_anim_text_path_animated(ZymVM* vm, ZymValue /*self*/,
                                   ZymValue idV, ZymValue textV,
                                   ZymValue progV, ZymValue optsV) {
    const char* W = "ui.animTextPathAnimated(pathId, text, progress, opts)";
    if (!requireFrame(vm, "ui.animTextPathAnimated")) return ZYM_ERROR;
    ImGuiID id; std::string text; double prog; iam_text_path_opts opts;
    if (!parseImGuiId(vm, idV, W, &id))               return ZYM_ERROR;
    if (!reqStr(vm, textV,  W, &text))                return ZYM_ERROR;
    if (!reqNum(vm, progV,  W, &prog))                return ZYM_ERROR;
    if (!parseTextPathOpts(vm, optsV, W, &opts))      return ZYM_ERROR;
    iam_text_path_animated(id, text.c_str(), (float)prog, opts);
    return zym_newNull();
}

ZymValue u_anim_text_path_width(ZymVM* vm, ZymValue /*self*/,
                                ZymValue textV, ZymValue optsV) {
    const char* W = "ui.animTextPathWidth(text, opts)";
    std::string text; iam_text_path_opts opts;
    if (!reqStr(vm, textV,  W, &text))                return ZYM_ERROR;
    if (!parseTextPathOpts(vm, optsV, W, &opts))      return ZYM_ERROR;
    return zym_newNumber((double)iam_text_path_width(text.c_str(), opts));
}

// --- Quad transform helpers ---------------------------------------------
//
// Both take/return a 4-vertex quad as a list of four [x,y] pairs.

static bool reqQuad(ZymVM* vm, ZymValue v, const char* where, ImVec2 out[4]) {
    if (!zym_isList(v) || zym_listLength(v) != 4) {
        zym_runtimeError(vm, "%s quad must be a 4-element list of [x,y]", where);
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        if (!reqVec2(vm, zym_listGet(vm, v, i), where, &out[i])) return false;
    }
    return true;
}

static ZymValue packQuad(ZymVM* vm, const ImVec2 q[4]) {
    ZymValue l = zym_newList(vm);
    for (int i = 0; i < 4; ++i) zym_listAppend(vm, l, packVec2(vm, q[i]));
    return l;
}

ZymValue u_anim_transform_quad(ZymVM* vm, ZymValue /*self*/,
                               ZymValue quadV, ZymValue centerV,
                               ZymValue angleV, ZymValue transV) {
    const char* W = "ui.animTransformQuad(quad, center, angleRad, translation)";
    ImVec2 quad[4], center, trans; double angle;
    if (!reqQuad(vm, quadV,    W, quad))   return ZYM_ERROR;
    if (!reqVec2(vm, centerV,  W, &center)) return ZYM_ERROR;
    if (!reqNum(vm, angleV,    W, &angle))  return ZYM_ERROR;
    if (!reqVec2(vm, transV,   W, &trans))  return ZYM_ERROR;
    iam_transform_quad(quad, center, (float)angle, trans);
    return packQuad(vm, quad);
}

ZymValue u_anim_make_glyph_quad(ZymVM* vm, ZymValue /*self*/,
                                ZymValue posV, ZymValue angleV,
                                ZymValue gwV, ZymValue ghV,
                                ZymValue baselineV) {
    const char* W = "ui.animMakeGlyphQuad(pos, angleRad, glyphW, glyphH, baselineOffset)";
    ImVec2 pos; double angle, gw, gh, baseline;
    if (!reqVec2(vm, posV,     W, &pos))      return ZYM_ERROR;
    if (!reqNum(vm, angleV,    W, &angle))    return ZYM_ERROR;
    if (!reqNum(vm, gwV,       W, &gw))       return ZYM_ERROR;
    if (!reqNum(vm, ghV,       W, &gh))       return ZYM_ERROR;
    if (!reqNum(vm, baselineV, W, &baseline)) return ZYM_ERROR;
    ImVec2 quad[4];
    iam_make_glyph_quad(quad, pos, (float)angle, (float)gw, (float)gh, (float)baseline);
    return packQuad(vm, quad);
}

// --- Text stagger -------------------------------------------------------

// Parse:
//   [pos, effect, charDelay, charDuration, effectIntensity, ease,
//    color, fontScale, letterSpacing]
// or null for defaults. Font omitted (see banner).
static bool parseTextStaggerOpts(ZymVM* vm, ZymValue v, const char* where,
                                 iam_text_stagger_opts* out) {
    *out = iam_text_stagger_opts();
    if (zym_isNull(v)) return true;
    if (!zym_isList(v) || zym_listLength(v) != 9) {
        zym_runtimeError(vm,
            "%s textStaggerOpts must be null or "
            "[pos, effect, charDelay, charDuration, effectIntensity, ease, "
            "color, fontScale, letterSpacing]", where);
        return false;
    }
    ImVec2 pos; int effect; double cd, cdur, intensity, fontScale, spacing;
    iam_ease_desc ease; ImVec4 color;
    if (!reqVec2(vm, zym_listGet(vm, v, 0), where, &pos))         return false;
    ZymValue eV = zym_listGet(vm, v, 1);
    if (!zym_isNumber(eV)) {
        zym_runtimeError(vm, "%s textStaggerOpts.effect must be a number", where);
        return false;
    }
    effect = (int)zym_asNumber(eV);
    ZymValue cdV   = zym_listGet(vm, v, 2);
    ZymValue cdurV = zym_listGet(vm, v, 3);
    ZymValue inV   = zym_listGet(vm, v, 4);
    if (!zym_isNumber(cdV) || !zym_isNumber(cdurV) || !zym_isNumber(inV)) {
        zym_runtimeError(vm,
            "%s textStaggerOpts [charDelay, charDuration, effectIntensity] "
            "must all be numbers", where);
        return false;
    }
    cd = zym_asNumber(cdV); cdur = zym_asNumber(cdurV); intensity = zym_asNumber(inV);
    if (!parseEaseDesc(vm, zym_listGet(vm, v, 5), where, &ease))  return false;
    if (!reqVec4(vm,       zym_listGet(vm, v, 6), where, &color)) return false;
    ZymValue fsV = zym_listGet(vm, v, 7);
    ZymValue lsV = zym_listGet(vm, v, 8);
    if (!zym_isNumber(fsV) || !zym_isNumber(lsV)) {
        zym_runtimeError(vm,
            "%s textStaggerOpts [fontScale, letterSpacing] must be numbers", where);
        return false;
    }
    fontScale = zym_asNumber(fsV); spacing = zym_asNumber(lsV);

    out->pos              = pos;
    out->effect           = effect;
    out->char_delay       = (float)cd;
    out->char_duration    = (float)cdur;
    out->effect_intensity = (float)intensity;
    out->ease             = ease;
    out->color            = ImGui::ColorConvertFloat4ToU32(color);
    out->font_scale       = (float)fontScale;
    out->letter_spacing   = (float)spacing;
    // font left at default nullptr.
    return true;
}

ZymValue u_anim_text_stagger(ZymVM* vm, ZymValue /*self*/,
                             ZymValue idV, ZymValue textV,
                             ZymValue progV, ZymValue optsV) {
    const char* W = "ui.animTextStagger(id, text, progress, opts)";
    if (!requireFrame(vm, "ui.animTextStagger")) return ZYM_ERROR;
    ImGuiID id; std::string text; double prog; iam_text_stagger_opts opts;
    if (!parseImGuiId(vm, idV, W, &id))                  return ZYM_ERROR;
    if (!reqStr(vm, textV,  W, &text))                   return ZYM_ERROR;
    if (!reqNum(vm, progV,  W, &prog))                   return ZYM_ERROR;
    if (!parseTextStaggerOpts(vm, optsV, W, &opts))      return ZYM_ERROR;
    iam_text_stagger(id, text.c_str(), (float)prog, opts);
    return zym_newNull();
}

ZymValue u_anim_text_stagger_width(ZymVM* vm, ZymValue /*self*/,
                                   ZymValue textV, ZymValue optsV) {
    const char* W = "ui.animTextStaggerWidth(text, opts)";
    std::string text; iam_text_stagger_opts opts;
    if (!reqStr(vm, textV,  W, &text))                   return ZYM_ERROR;
    if (!parseTextStaggerOpts(vm, optsV, W, &opts))      return ZYM_ERROR;
    return zym_newNumber((double)iam_text_stagger_width(text.c_str(), opts));
}

ZymValue u_anim_text_stagger_duration(ZymVM* vm, ZymValue /*self*/,
                                      ZymValue textV, ZymValue optsV) {
    const char* W = "ui.animTextStaggerDuration(text, opts)";
    std::string text; iam_text_stagger_opts opts;
    if (!reqStr(vm, textV,  W, &text))                   return ZYM_ERROR;
    if (!parseTextStaggerOpts(vm, optsV, W, &opts))      return ZYM_ERROR;
    return zym_newNumber((double)iam_text_stagger_duration(text.c_str(), opts));
}

// ==== SECTION 8: Noise + Style/Gradient/Transform interp + Repeat-with-var =
//
// Scope notes:
//   * `iam_gradient` is a C++ struct with internal ImVector storage. We
//     expose it via an ImGuiID-keyed cache of script-built gradients
//     (begin/addStop/end/clear) — mirrors the §6 path-builder pattern.
//   * Style snapshots are stored upstream keyed by ImGuiID; we just
//     surface register/blend/tween/exists/unregister. `style_blend_to`
//     (which writes into a caller-owned `ImGuiStyle*`) is NOT exposed —
//     Zym does not bind `ImGuiStyle*` and the apply-to-current path
//     (`iam_style_blend`) is the canonical use-case.
//   * `iam_transform` is packed as a flat list `[px,py,sx,sy,rotRad]`.
//   * `iam_variation_*` structs are constructor-only at the Zym level
//     in this session — they are only consumed by clip `key_*_var` /
//     `set_*_var` calls bound in §9. We provide the data-builder
//     helpers (`animVar*` returning a flat list shape) so script code
//     can pre-build variation descriptors that §9 will accept.
//   * Callback-mode variations (`iam_var_callback`) require closure
//     trampolines and are deferred to §9's resolver-tween batch.

// --- Noise --------------------------------------------------------------

// Parse [type, octaves, persistence, lacunarity, seed] or null for defaults.
static bool parseNoiseOpts(ZymVM* vm, ZymValue v, const char* where,
                           iam_noise_opts* out) {
    *out = iam_noise_opts();
    if (zym_isNull(v)) return true;
    if (!zym_isList(v) || zym_listLength(v) != 5) {
        zym_runtimeError(vm,
            "%s noiseOpts must be null or "
            "[type, octaves, persistence, lacunarity, seed]", where);
        return false;
    }
    ZymValue tV = zym_listGet(vm, v, 0);
    ZymValue oV = zym_listGet(vm, v, 1);
    ZymValue pV = zym_listGet(vm, v, 2);
    ZymValue lV = zym_listGet(vm, v, 3);
    ZymValue sV = zym_listGet(vm, v, 4);
    if (!zym_isNumber(tV) || !zym_isNumber(oV) || !zym_isNumber(pV) ||
        !zym_isNumber(lV) || !zym_isNumber(sV)) {
        zym_runtimeError(vm, "%s noiseOpts elements must all be numbers", where);
        return false;
    }
    out->type        = (int)zym_asNumber(tV);
    out->octaves     = (int)zym_asNumber(oV);
    out->persistence = (float)zym_asNumber(pV);
    out->lacunarity  = (float)zym_asNumber(lV);
    out->seed        = (int)zym_asNumber(sV);
    return true;
}

ZymValue u_anim_noise_2d(ZymVM* vm, ZymValue /*self*/,
                         ZymValue xV, ZymValue yV, ZymValue optsV) {
    const char* W = "ui.animNoise2d(x, y, opts)";
    double x, y; iam_noise_opts opts;
    if (!reqNum(vm, xV, W, &x)) return ZYM_ERROR;
    if (!reqNum(vm, yV, W, &y)) return ZYM_ERROR;
    if (!parseNoiseOpts(vm, optsV, W, &opts)) return ZYM_ERROR;
    return zym_newNumber((double)iam_noise_2d((float)x, (float)y, opts));
}

ZymValue u_anim_noise_3d(ZymVM* vm, ZymValue /*self*/,
                         ZymValue xV, ZymValue yV, ZymValue zV, ZymValue optsV) {
    const char* W = "ui.animNoise3d(x, y, z, opts)";
    double x, y, z; iam_noise_opts opts;
    if (!reqNum(vm, xV, W, &x)) return ZYM_ERROR;
    if (!reqNum(vm, yV, W, &y)) return ZYM_ERROR;
    if (!reqNum(vm, zV, W, &z)) return ZYM_ERROR;
    if (!parseNoiseOpts(vm, optsV, W, &opts)) return ZYM_ERROR;
    return zym_newNumber((double)iam_noise_3d((float)x, (float)y, (float)z, opts));
}

ZymValue u_anim_noise_channel_float(ZymVM* vm, ZymValue /*self*/,
                                    ZymValue idV, ZymValue freqV, ZymValue ampV,
                                    ZymValue optsV, ZymValue dtV) {
    const char* W = "ui.animNoiseChannelFloat(id, freq, amp, opts, dt)";
    ImGuiID id; double freq, amp, dt; iam_noise_opts opts;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    if (!reqNum(vm, freqV, W, &freq))   return ZYM_ERROR;
    if (!reqNum(vm, ampV,  W, &amp))    return ZYM_ERROR;
    if (!parseNoiseOpts(vm, optsV, W, &opts)) return ZYM_ERROR;
    if (!reqNum(vm, dtV,   W, &dt))     return ZYM_ERROR;
    return zym_newNumber((double)iam_noise_channel_float(id, (float)freq, (float)amp, opts, (float)dt));
}

ZymValue u_anim_noise_channel_vec2(ZymVM* vm, ZymValue /*self*/,
                                   ZymValue idV, ZymValue freqV, ZymValue ampV,
                                   ZymValue optsV, ZymValue dtV) {
    const char* W = "ui.animNoiseChannelVec2(id, freq, amp, opts, dt)";
    ImGuiID id; ImVec2 freq, amp; double dt; iam_noise_opts opts;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    if (!reqVec2(vm, freqV, W, &freq))  return ZYM_ERROR;
    if (!reqVec2(vm, ampV,  W, &amp))   return ZYM_ERROR;
    if (!parseNoiseOpts(vm, optsV, W, &opts)) return ZYM_ERROR;
    if (!reqNum(vm, dtV, W, &dt))       return ZYM_ERROR;
    return packVec2(vm, iam_noise_channel_vec2(id, freq, amp, opts, (float)dt));
}

ZymValue u_anim_noise_channel_vec4(ZymVM* vm, ZymValue /*self*/,
                                   ZymValue idV, ZymValue freqV, ZymValue ampV,
                                   ZymValue optsV, ZymValue dtV) {
    const char* W = "ui.animNoiseChannelVec4(id, freq, amp, opts, dt)";
    ImGuiID id; ImVec4 freq, amp; double dt; iam_noise_opts opts;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    if (!reqVec4(vm, freqV, W, &freq))  return ZYM_ERROR;
    if (!reqVec4(vm, ampV,  W, &amp))   return ZYM_ERROR;
    if (!parseNoiseOpts(vm, optsV, W, &opts)) return ZYM_ERROR;
    if (!reqNum(vm, dtV, W, &dt))       return ZYM_ERROR;
    return packVec4(vm, iam_noise_channel_vec4(id, freq, amp, opts, (float)dt));
}

ZymValue u_anim_noise_channel_color(ZymVM* vm, ZymValue /*self*/,
                                    ZymValue idV, ZymValue baseV, ZymValue ampV,
                                    ZymValue freqV, ZymValue optsV,
                                    ZymValue spaceV, ZymValue dtV) {
    const char* W = "ui.animNoiseChannelColor(id, baseColor, amp, freq, opts, space, dt)";
    ImGuiID id; ImVec4 base, amp; double freq, dt; int space; iam_noise_opts opts;
    if (!parseImGuiId(vm, idV, W, &id))   return ZYM_ERROR;
    if (!reqVec4(vm, baseV, W, &base))    return ZYM_ERROR;
    if (!reqVec4(vm, ampV,  W, &amp))     return ZYM_ERROR;
    if (!reqNum(vm, freqV,  W, &freq))    return ZYM_ERROR;
    if (!parseNoiseOpts(vm, optsV, W, &opts)) return ZYM_ERROR;
    if (!reqInt(vm, spaceV, W, &space))   return ZYM_ERROR;
    if (!reqNum(vm, dtV,    W, &dt))      return ZYM_ERROR;
    return packVec4(vm, iam_noise_channel_color(id, base, amp, (float)freq, opts, space, (float)dt));
}

ZymValue u_anim_smooth_noise_float(ZymVM* vm, ZymValue /*self*/,
                                   ZymValue idV, ZymValue ampV,
                                   ZymValue speedV, ZymValue dtV) {
    const char* W = "ui.animSmoothNoiseFloat(id, amp, speed, dt)";
    ImGuiID id; double amp, speed, dt;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    if (!reqNum(vm, ampV,   W, &amp))   return ZYM_ERROR;
    if (!reqNum(vm, speedV, W, &speed)) return ZYM_ERROR;
    if (!reqNum(vm, dtV,    W, &dt))    return ZYM_ERROR;
    return zym_newNumber((double)iam_smooth_noise_float(id, (float)amp, (float)speed, (float)dt));
}

ZymValue u_anim_smooth_noise_vec2(ZymVM* vm, ZymValue /*self*/,
                                  ZymValue idV, ZymValue ampV,
                                  ZymValue speedV, ZymValue dtV) {
    const char* W = "ui.animSmoothNoiseVec2(id, amp, speed, dt)";
    ImGuiID id; ImVec2 amp; double speed, dt;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    if (!reqVec2(vm, ampV, W, &amp))    return ZYM_ERROR;
    if (!reqNum(vm, speedV, W, &speed)) return ZYM_ERROR;
    if (!reqNum(vm, dtV,    W, &dt))    return ZYM_ERROR;
    return packVec2(vm, iam_smooth_noise_vec2(id, amp, (float)speed, (float)dt));
}

ZymValue u_anim_smooth_noise_vec4(ZymVM* vm, ZymValue /*self*/,
                                  ZymValue idV, ZymValue ampV,
                                  ZymValue speedV, ZymValue dtV) {
    const char* W = "ui.animSmoothNoiseVec4(id, amp, speed, dt)";
    ImGuiID id; ImVec4 amp; double speed, dt;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    if (!reqVec4(vm, ampV, W, &amp))    return ZYM_ERROR;
    if (!reqNum(vm, speedV, W, &speed)) return ZYM_ERROR;
    if (!reqNum(vm, dtV,    W, &dt))    return ZYM_ERROR;
    return packVec4(vm, iam_smooth_noise_vec4(id, amp, (float)speed, (float)dt));
}

ZymValue u_anim_smooth_noise_color(ZymVM* vm, ZymValue /*self*/,
                                   ZymValue idV, ZymValue baseV, ZymValue ampV,
                                   ZymValue speedV, ZymValue spaceV, ZymValue dtV) {
    const char* W = "ui.animSmoothNoiseColor(id, baseColor, amp, speed, space, dt)";
    ImGuiID id; ImVec4 base, amp; double speed, dt; int space;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    if (!reqVec4(vm, baseV, W, &base))  return ZYM_ERROR;
    if (!reqVec4(vm, ampV,  W, &amp))   return ZYM_ERROR;
    if (!reqNum(vm, speedV, W, &speed)) return ZYM_ERROR;
    if (!reqInt(vm, spaceV, W, &space)) return ZYM_ERROR;
    if (!reqNum(vm, dtV,    W, &dt))    return ZYM_ERROR;
    return packVec4(vm, iam_smooth_noise_color(id, base, amp, (float)speed, space, (float)dt));
}

// --- Style interpolation ------------------------------------------------
//
// Snapshots live upstream keyed by ImGuiID. `iam_style_register` takes
// the snapshot from the caller-supplied `ImGuiStyle&`, but Zym does not
// model `ImGuiStyle` — we therefore only expose the "register the
// current style" path (the canonical capture point). Blend / tween
// operate on already-registered ids.

ZymValue u_anim_style_register_current(ZymVM* vm, ZymValue /*self*/, ZymValue idV) {
    const char* W = "ui.animStyleRegisterCurrent(styleId)";
    ImGuiID id;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    iam_style_register_current(id);
    return zym_newNull();
}

ZymValue u_anim_style_blend(ZymVM* vm, ZymValue /*self*/,
                            ZymValue aV, ZymValue bV,
                            ZymValue tV, ZymValue spaceV) {
    const char* W = "ui.animStyleBlend(styleA, styleB, t, colorSpace)";
    ImGuiID a, b; double t; int space;
    if (!parseImGuiId(vm, aV, W, &a))     return ZYM_ERROR;
    if (!parseImGuiId(vm, bV, W, &b))     return ZYM_ERROR;
    if (!reqNum(vm, tV, W, &t))           return ZYM_ERROR;
    if (!reqInt(vm, spaceV, W, &space))   return ZYM_ERROR;
    iam_style_blend(a, b, (float)t, space);
    return zym_newNull();
}

ZymValue u_anim_style_tween(ZymVM* vm, ZymValue /*self*/,
                            ZymValue idV, ZymValue targetV, ZymValue durV,
                            ZymValue ezV, ZymValue spaceV, ZymValue dtV) {
    const char* W = "ui.animStyleTween(id, targetStyle, dur, ease, colorSpace, dt)";
    ImGuiID id, target; double dur, dt; iam_ease_desc ez; int space;
    if (!parseImGuiId(vm, idV, W, &id))         return ZYM_ERROR;
    if (!parseImGuiId(vm, targetV, W, &target)) return ZYM_ERROR;
    if (!reqNum(vm, durV, W, &dur))             return ZYM_ERROR;
    if (!parseEaseDesc(vm, ezV, W, &ez))        return ZYM_ERROR;
    if (!reqInt(vm, spaceV, W, &space))         return ZYM_ERROR;
    if (!reqNum(vm, dtV, W, &dt))               return ZYM_ERROR;
    iam_style_tween(id, target, (float)dur, ez, space, (float)dt);
    return zym_newNull();
}

ZymValue u_anim_style_exists(ZymVM* vm, ZymValue /*self*/, ZymValue idV) {
    const char* W = "ui.animStyleExists(styleId)";
    ImGuiID id;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    return zym_newBool(iam_style_exists(id));
}

ZymValue u_anim_style_unregister(ZymVM* vm, ZymValue /*self*/, ZymValue idV) {
    const char* W = "ui.animStyleUnregister(styleId)";
    ImGuiID id;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    iam_style_unregister(id);
    return zym_newNull();
}

// --- Gradient interpolation ---------------------------------------------
//
// `iam_gradient` is a value struct with ImVector storage; we keep a
// file-local ImGuiID-keyed cache of gradients built script-side via a
// `begin/addStop/end` pattern (same approach as the §6 path builder).
// `iam_tween_gradient` returns a new gradient each call — upstream
// expects callers to manage that snapshot themselves; we store the
// tween result back into the cache under the tween's `id` so scripts
// can sample it the same way as a hand-built gradient.

static std::unordered_map<ImGuiID, iam_gradient> g_gradients;
static iam_gradient* g_active_gradient_builder = nullptr;
static ImGuiID       g_active_gradient_id      = 0;

ZymValue u_anim_gradient_begin(ZymVM* vm, ZymValue /*self*/, ZymValue idV) {
    const char* W = "ui.animGradientBegin(gradientId)";
    ImGuiID id;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    // Drop any unfinished previous builder (mirrors path-builder policy).
    if (g_active_gradient_builder) {
        delete g_active_gradient_builder;
        g_active_gradient_builder = nullptr;
    }
    g_active_gradient_builder = new iam_gradient();
    g_active_gradient_id = id;
    return zym_newNumber((double)id);
}

ZymValue u_anim_gradient_add_stop(ZymVM* vm, ZymValue /*self*/,
                                  ZymValue posV, ZymValue colorV) {
    const char* W = "ui.animGradientAddStop(position, color)";
    if (!g_active_gradient_builder) {
        zym_runtimeError(vm, "%s called without an active animGradientBegin", W);
        return ZYM_ERROR;
    }
    double pos; ImVec4 color;
    if (!reqNum(vm, posV, W, &pos))      return ZYM_ERROR;
    if (!reqVec4(vm, colorV, W, &color)) return ZYM_ERROR;
    g_active_gradient_builder->add((float)pos, color);
    return zym_newNull();
}

ZymValue u_anim_gradient_end(ZymVM* vm, ZymValue /*self*/) {
    const char* W = "ui.animGradientEnd()";
    if (!g_active_gradient_builder) {
        zym_runtimeError(vm, "%s called without an active animGradientBegin", W);
        return ZYM_ERROR;
    }
    g_gradients[g_active_gradient_id] = *g_active_gradient_builder;
    delete g_active_gradient_builder;
    g_active_gradient_builder = nullptr;
    ImGuiID id = g_active_gradient_id;
    g_active_gradient_id = 0;
    return zym_newNumber((double)id);
}

ZymValue u_anim_gradient_exists(ZymVM* vm, ZymValue /*self*/, ZymValue idV) {
    const char* W = "ui.animGradientExists(gradientId)";
    ImGuiID id;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    return zym_newBool(g_gradients.find(id) != g_gradients.end());
}

ZymValue u_anim_gradient_stop_count(ZymVM* vm, ZymValue /*self*/, ZymValue idV) {
    const char* W = "ui.animGradientStopCount(gradientId)";
    ImGuiID id;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    auto it = g_gradients.find(id);
    if (it == g_gradients.end()) return zym_newNumber(0);
    return zym_newNumber((double)it->second.stop_count());
}

ZymValue u_anim_gradient_sample(ZymVM* vm, ZymValue /*self*/,
                                ZymValue idV, ZymValue tV, ZymValue spaceV) {
    const char* W = "ui.animGradientSample(gradientId, t, colorSpace)";
    ImGuiID id; double t; int space;
    if (!parseImGuiId(vm, idV, W, &id))   return ZYM_ERROR;
    if (!reqNum(vm, tV, W, &t))           return ZYM_ERROR;
    if (!reqInt(vm, spaceV, W, &space))   return ZYM_ERROR;
    auto it = g_gradients.find(id);
    if (it == g_gradients.end()) {
        zym_runtimeError(vm, "%s gradient id not found (call animGradientBegin/End first)", W);
        return ZYM_ERROR;
    }
    return packVec4(vm, it->second.sample((float)t, space));
}

ZymValue u_anim_gradient_lerp(ZymVM* vm, ZymValue /*self*/,
                              ZymValue aV, ZymValue bV, ZymValue tV,
                              ZymValue spaceV, ZymValue outV) {
    const char* W = "ui.animGradientLerp(gradientA, gradientB, t, colorSpace, outGradientId)";
    ImGuiID a, b, out; double t; int space;
    if (!parseImGuiId(vm, aV, W, &a))     return ZYM_ERROR;
    if (!parseImGuiId(vm, bV, W, &b))     return ZYM_ERROR;
    if (!reqNum(vm, tV, W, &t))           return ZYM_ERROR;
    if (!reqInt(vm, spaceV, W, &space))   return ZYM_ERROR;
    if (!parseImGuiId(vm, outV, W, &out)) return ZYM_ERROR;
    auto ita = g_gradients.find(a);
    auto itb = g_gradients.find(b);
    if (ita == g_gradients.end() || itb == g_gradients.end()) {
        zym_runtimeError(vm, "%s gradient id not found", W);
        return ZYM_ERROR;
    }
    g_gradients[out] = iam_gradient_lerp(ita->second, itb->second, (float)t, space);
    return zym_newNumber((double)out);
}

ZymValue u_anim_tween_gradient(ZymVM* vm, ZymValue /*self*/,
                               ZymValue idV, ZymValue chV, ZymValue targetV,
                               ZymValue durV, ZymValue ezV, ZymValue policyV,
                               ZymValue spaceV, ZymValue dtV, ZymValue outV) {
    const char* W = "ui.animTweenGradient(id, channelId, targetGradientId, dur, ease, policy, colorSpace, dt, outGradientId)";
    ImGuiID id, ch, target, out; double dur, dt; iam_ease_desc ez; int policy, space;
    if (!parseImGuiId(vm, idV, W, &id))         return ZYM_ERROR;
    if (!parseImGuiId(vm, chV, W, &ch))         return ZYM_ERROR;
    if (!parseImGuiId(vm, targetV, W, &target)) return ZYM_ERROR;
    if (!reqNum(vm, durV, W, &dur))             return ZYM_ERROR;
    if (!parseEaseDesc(vm, ezV, W, &ez))        return ZYM_ERROR;
    if (!reqInt(vm, policyV, W, &policy))       return ZYM_ERROR;
    if (!reqInt(vm, spaceV, W, &space))         return ZYM_ERROR;
    if (!reqNum(vm, dtV, W, &dt))               return ZYM_ERROR;
    if (!parseImGuiId(vm, outV, W, &out))       return ZYM_ERROR;
    auto it = g_gradients.find(target);
    if (it == g_gradients.end()) {
        zym_runtimeError(vm, "%s target gradient id not found", W);
        return ZYM_ERROR;
    }
    g_gradients[out] = iam_tween_gradient(id, ch, it->second, (float)dur,
                                           ez, policy, space, (float)dt);
    return zym_newNumber((double)out);
}

// --- Transform interpolation --------------------------------------------
//
// Transform packed as `[posX, posY, scaleX, scaleY, rotationRad]`.

static bool reqTransform(ZymVM* vm, ZymValue v, const char* where, iam_transform* out) {
    if (!zym_isList(v) || zym_listLength(v) != 5) {
        zym_runtimeError(vm, "%s transform must be [posX, posY, scaleX, scaleY, rotationRad]", where);
        return false;
    }
    float c[5];
    for (int i = 0; i < 5; ++i) {
        ZymValue e = zym_listGet(vm, v, i);
        if (!zym_isNumber(e)) {
            zym_runtimeError(vm, "%s transform element %d must be a number", where, i);
            return false;
        }
        c[i] = (float)zym_asNumber(e);
    }
    out->position = ImVec2(c[0], c[1]);
    out->scale    = ImVec2(c[2], c[3]);
    out->rotation = c[4];
    return true;
}

static ZymValue packTransform(ZymVM* vm, iam_transform const& t) {
    ZymValue l = zym_newList(vm);
    zym_listAppend(vm, l, zym_newNumber((double)t.position.x));
    zym_listAppend(vm, l, zym_newNumber((double)t.position.y));
    zym_listAppend(vm, l, zym_newNumber((double)t.scale.x));
    zym_listAppend(vm, l, zym_newNumber((double)t.scale.y));
    zym_listAppend(vm, l, zym_newNumber((double)t.rotation));
    return l;
}

ZymValue u_anim_transform_lerp(ZymVM* vm, ZymValue /*self*/,
                               ZymValue aV, ZymValue bV,
                               ZymValue tV, ZymValue modeV) {
    const char* W = "ui.animTransformLerp(a, b, t, rotationMode)";
    iam_transform a, b; double t; int mode;
    if (!reqTransform(vm, aV, W, &a))   return ZYM_ERROR;
    if (!reqTransform(vm, bV, W, &b))   return ZYM_ERROR;
    if (!reqNum(vm, tV, W, &t))         return ZYM_ERROR;
    if (!reqInt(vm, modeV, W, &mode))   return ZYM_ERROR;
    return packTransform(vm, iam_transform_lerp(a, b, (float)t, mode));
}

ZymValue u_anim_tween_transform(ZymVM* vm, ZymValue /*self*/,
                                ZymValue idV, ZymValue chV, ZymValue targetV,
                                ZymValue durV, ZymValue ezV, ZymValue policyV,
                                ZymValue modeV, ZymValue dtV) {
    const char* W = "ui.animTweenTransform(id, channelId, target, dur, ease, policy, rotationMode, dt)";
    ImGuiID id, ch; iam_transform target; double dur, dt; iam_ease_desc ez;
    int policy, mode;
    if (!parseImGuiId(vm, idV, W, &id))   return ZYM_ERROR;
    if (!parseImGuiId(vm, chV, W, &ch))   return ZYM_ERROR;
    if (!reqTransform(vm, targetV, W, &target)) return ZYM_ERROR;
    if (!reqNum(vm, durV, W, &dur))       return ZYM_ERROR;
    if (!parseEaseDesc(vm, ezV, W, &ez))  return ZYM_ERROR;
    if (!reqInt(vm, policyV, W, &policy)) return ZYM_ERROR;
    if (!reqInt(vm, modeV, W, &mode))     return ZYM_ERROR;
    if (!reqNum(vm, dtV, W, &dt))         return ZYM_ERROR;
    return packTransform(vm, iam_tween_transform(id, ch, target, (float)dur,
                                                  ez, policy, mode, (float)dt));
}

ZymValue u_anim_transform_apply(ZymVM* vm, ZymValue /*self*/,
                                ZymValue xfV, ZymValue pointV) {
    const char* W = "ui.animTransformApply(transform, point)";
    iam_transform xf; ImVec2 point;
    if (!reqTransform(vm, xfV, W, &xf))    return ZYM_ERROR;
    if (!reqVec2(vm, pointV, W, &point))   return ZYM_ERROR;
    return packVec2(vm, xf.apply(point));
}

ZymValue u_anim_transform_inverse(ZymVM* vm, ZymValue /*self*/, ZymValue xfV) {
    const char* W = "ui.animTransformInverse(transform)";
    iam_transform xf;
    if (!reqTransform(vm, xfV, W, &xf)) return ZYM_ERROR;
    return packTransform(vm, xf.inverse());
}

ZymValue u_anim_transform_compose(ZymVM* vm, ZymValue /*self*/,
                                  ZymValue aV, ZymValue bV) {
    const char* W = "ui.animTransformCompose(a, b)";
    iam_transform a, b;
    if (!reqTransform(vm, aV, W, &a)) return ZYM_ERROR;
    if (!reqTransform(vm, bV, W, &b)) return ZYM_ERROR;
    return packTransform(vm, a * b);
}

ZymValue u_anim_transform_from_matrix(ZymVM* vm, ZymValue /*self*/,
                                      ZymValue m00V, ZymValue m01V,
                                      ZymValue m10V, ZymValue m11V,
                                      ZymValue txV,  ZymValue tyV) {
    const char* W = "ui.animTransformFromMatrix(m00, m01, m10, m11, tx, ty)";
    double m00, m01, m10, m11, tx, ty;
    if (!reqNum(vm, m00V, W, &m00)) return ZYM_ERROR;
    if (!reqNum(vm, m01V, W, &m01)) return ZYM_ERROR;
    if (!reqNum(vm, m10V, W, &m10)) return ZYM_ERROR;
    if (!reqNum(vm, m11V, W, &m11)) return ZYM_ERROR;
    if (!reqNum(vm, txV,  W, &tx))  return ZYM_ERROR;
    if (!reqNum(vm, tyV,  W, &ty))  return ZYM_ERROR;
    return packTransform(vm, iam_transform_from_matrix(
        (float)m00, (float)m01, (float)m10, (float)m11, (float)tx, (float)ty));
}

ZymValue u_anim_transform_to_matrix(ZymVM* vm, ZymValue /*self*/, ZymValue xfV) {
    const char* W = "ui.animTransformToMatrix(transform) -> [m00,m01,tx,m10,m11,ty]";
    iam_transform xf;
    if (!reqTransform(vm, xfV, W, &xf)) return ZYM_ERROR;
    float m[6];
    iam_transform_to_matrix(xf, m);
    ZymValue l = zym_newList(vm);
    for (int i = 0; i < 6; ++i) zym_listAppend(vm, l, zym_newNumber((double)m[i]));
    return l;
}

// ==== SECTION 9 (frame-update opt-in flag) ================================
//
// Single flag shared by both the auto driver in `ui.cpp::u_frame` and the
// script-side opt-in toggle. Default `true` — auto driving is on. Scripts
// flip via `UI.animSetAutoFrameUpdate(false)` and then drive both
// `UI.animUpdateBeginFrame()` and `UI.animClipUpdate(dt)` themselves.

} // namespace
static bool g_anim_auto_frame_update = true;
bool isAnimAutoFrameUpdateEnabled() { return g_anim_auto_frame_update; }
namespace {

ZymValue u_anim_set_auto_frame_update(ZymVM* vm, ZymValue /*self*/, ZymValue enableV) {
    bool enable;
    if (!reqBool(vm, enableV, "ui.animSetAutoFrameUpdate(enable)", &enable)) return ZYM_ERROR;
    g_anim_auto_frame_update = enable;
    return zym_newNull();
}

ZymValue u_anim_is_auto_frame_update_enabled(ZymVM* /*vm*/, ZymValue /*self*/) {
    return zym_newBool(g_anim_auto_frame_update);
}

ZymValue u_anim_clip_update(ZymVM* vm, ZymValue /*self*/, ZymValue dtV) {
    double dt;
    if (!reqNum(vm, dtV, "ui.animClipUpdate(dt)", &dt)) return ZYM_ERROR;
    iam_clip_update((float)dt);
    return zym_newNull();
}

// ==== SECTION 9: Clips + Instances + Timeline + Layering + Trampolines ===
//
// Final session of the binding plan. Surface covered:
//   * Variation descriptors (`iam_variation_{float,int,vec2,vec4,color}`)
//     surfaced as plain Zym lists tagged by kind so clip key_*_var calls
//     can take them without opaque handles.
//   * Clip authoring — `iam_clip` fluent class flattened to a procedural
//     surface keyed by an "active clip" singleton (same pattern as the
//     §6 path builder and §8 gradient builder).
//   * Instance ops — `iam_instance` methods exposed as functions taking
//     an instance ID (which is what upstream already uses as the public
//     handle via `iam_play`'s second arg).
//   * Clip-system globals: init/shutdown, update, gc, play, query,
//     stagger helpers, layering, save/load.
//   * Resolver-tween trampolines (deferred from §3): closures invoked
//     synchronously inside `iam_tween_*_resolved`, so a transient
//     `zym_pushRoot` around the call is sufficient — no persistent
//     slot table needed.
//   * Custom-ease registration (deferred from §2): 16 fixed slots,
//     each with a dedicated C trampoline that dispatches to a rooted
//     Zym closure. Closure persists until re-registered (no
//     unregister upstream).
//
// Out of scope (documented):
//   * Variation-callback mode (`iam_var_callback`) — preset variation
//     modes cover every demo path; callback mode would need 4 closure
//     trampolines per variation type with no `user`-routable slot
//     scheme that maps cleanly to a stateless Zym closure lifetime.
//   * Clip lifecycle callbacks (`on_begin`/`on_update`/`on_complete`)
//     and marker callbacks — same reasoning: they fire from inside the
//     clip update loop without a per-callback `user` pointer Zym can
//     route through. Scripts can poll `animInstanceTime` /
//     `animInstanceIsPlaying` per frame to achieve equivalent effects.

// --- Variation descriptor tagging ---------------------------------------
//
// Each variation kind is packed as a list whose first element is a tag
// (`int` constant below) so `key_*_var` can validate the kind without
// reading the rest of the payload. Payload layout:
//
//   float:  [0, mode, amount, minClamp, maxClamp, seed]
//   int:    [1, mode, amount, minClamp, maxClamp, seed]
//   vec2:   [2, mode, ax, ay, mnx, mny, mxx, mxy, seed]
//   vec4:   [3, mode, ax, ay, az, aw, mnx, mny, mnz, mnw, mxx, mxy, mxz, mxw, seed]
//   color:  [4, mode, ar, ag, ab, aa, mnr, mng, mnb, mna, mxr, mxg, mxb, mxa, colorSpace, seed]
//
// Per-axis (`xy*`) sub-variation packing is intentionally not exposed —
// the global-mode payloads above cover every preset variation behaviour.

enum {
    kVarTagFloat = 0,
    kVarTagInt   = 1,
    kVarTagVec2  = 2,
    kVarTagVec4  = 3,
    kVarTagColor = 4,
};

static ZymValue packVarFloat(ZymVM* vm, int mode, double amt,
                             double mn, double mx, double seed) {
    ZymValue l = zym_newList(vm);
    zym_listAppend(vm, l, zym_newNumber(kVarTagFloat));
    zym_listAppend(vm, l, zym_newNumber(mode));
    zym_listAppend(vm, l, zym_newNumber(amt));
    zym_listAppend(vm, l, zym_newNumber(mn));
    zym_listAppend(vm, l, zym_newNumber(mx));
    zym_listAppend(vm, l, zym_newNumber(seed));
    return l;
}

ZymValue u_anim_var_float(ZymVM* vm, ZymValue /*self*/,
                          ZymValue modeV, ZymValue amtV,
                          ZymValue mnV, ZymValue mxV, ZymValue seedV) {
    const char* W = "ui.animVarFloat(mode, amount, minClamp, maxClamp, seed)";
    int mode; double amt, mn, mx; int seed;
    if (!reqInt(vm, modeV, W, &mode)) return ZYM_ERROR;
    if (!reqNum(vm, amtV,  W, &amt))  return ZYM_ERROR;
    if (!reqNum(vm, mnV,   W, &mn))   return ZYM_ERROR;
    if (!reqNum(vm, mxV,   W, &mx))   return ZYM_ERROR;
    if (!reqInt(vm, seedV, W, &seed)) return ZYM_ERROR;
    return packVarFloat(vm, mode, amt, mn, mx, (double)(unsigned int)seed);
}

ZymValue u_anim_var_int(ZymVM* vm, ZymValue /*self*/,
                        ZymValue modeV, ZymValue amtV,
                        ZymValue mnV, ZymValue mxV, ZymValue seedV) {
    const char* W = "ui.animVarInt(mode, amount, minClamp, maxClamp, seed)";
    int mode, amt, mn, mx, seed;
    if (!reqInt(vm, modeV, W, &mode)) return ZYM_ERROR;
    if (!reqInt(vm, amtV,  W, &amt))  return ZYM_ERROR;
    if (!reqInt(vm, mnV,   W, &mn))   return ZYM_ERROR;
    if (!reqInt(vm, mxV,   W, &mx))   return ZYM_ERROR;
    if (!reqInt(vm, seedV, W, &seed)) return ZYM_ERROR;
    ZymValue l = zym_newList(vm);
    zym_listAppend(vm, l, zym_newNumber(kVarTagInt));
    zym_listAppend(vm, l, zym_newNumber(mode));
    zym_listAppend(vm, l, zym_newNumber(amt));
    zym_listAppend(vm, l, zym_newNumber(mn));
    zym_listAppend(vm, l, zym_newNumber(mx));
    zym_listAppend(vm, l, zym_newNumber((unsigned int)seed));
    return l;
}

ZymValue u_anim_var_vec2(ZymVM* vm, ZymValue /*self*/,
                         ZymValue modeV, ZymValue amtV,
                         ZymValue mnV, ZymValue mxV, ZymValue seedV) {
    const char* W = "ui.animVarVec2(mode, amount, minClamp, maxClamp, seed)";
    int mode; ImVec2 amt, mn, mx; int seed;
    if (!reqInt(vm, modeV, W, &mode))    return ZYM_ERROR;
    if (!reqVec2(vm, amtV, W, &amt))     return ZYM_ERROR;
    if (!reqVec2(vm, mnV,  W, &mn))      return ZYM_ERROR;
    if (!reqVec2(vm, mxV,  W, &mx))      return ZYM_ERROR;
    if (!reqInt(vm, seedV, W, &seed))    return ZYM_ERROR;
    ZymValue l = zym_newList(vm);
    zym_listAppend(vm, l, zym_newNumber(kVarTagVec2));
    zym_listAppend(vm, l, zym_newNumber(mode));
    zym_listAppend(vm, l, zym_newNumber(amt.x));
    zym_listAppend(vm, l, zym_newNumber(amt.y));
    zym_listAppend(vm, l, zym_newNumber(mn.x));
    zym_listAppend(vm, l, zym_newNumber(mn.y));
    zym_listAppend(vm, l, zym_newNumber(mx.x));
    zym_listAppend(vm, l, zym_newNumber(mx.y));
    zym_listAppend(vm, l, zym_newNumber((unsigned int)seed));
    return l;
}

ZymValue u_anim_var_vec4(ZymVM* vm, ZymValue /*self*/,
                         ZymValue modeV, ZymValue amtV,
                         ZymValue mnV, ZymValue mxV, ZymValue seedV) {
    const char* W = "ui.animVarVec4(mode, amount, minClamp, maxClamp, seed)";
    int mode; ImVec4 amt, mn, mx; int seed;
    if (!reqInt(vm, modeV, W, &mode))    return ZYM_ERROR;
    if (!reqVec4(vm, amtV, W, &amt))     return ZYM_ERROR;
    if (!reqVec4(vm, mnV,  W, &mn))      return ZYM_ERROR;
    if (!reqVec4(vm, mxV,  W, &mx))      return ZYM_ERROR;
    if (!reqInt(vm, seedV, W, &seed))    return ZYM_ERROR;
    ZymValue l = zym_newList(vm);
    zym_listAppend(vm, l, zym_newNumber(kVarTagVec4));
    zym_listAppend(vm, l, zym_newNumber(mode));
    for (int i = 0; i < 4; ++i) zym_listAppend(vm, l, zym_newNumber(((float*)&amt)[i]));
    for (int i = 0; i < 4; ++i) zym_listAppend(vm, l, zym_newNumber(((float*)&mn)[i]));
    for (int i = 0; i < 4; ++i) zym_listAppend(vm, l, zym_newNumber(((float*)&mx)[i]));
    zym_listAppend(vm, l, zym_newNumber((unsigned int)seed));
    return l;
}

ZymValue u_anim_var_color(ZymVM* vm, ZymValue /*self*/,
                          ZymValue modeV, ZymValue amtV,
                          ZymValue mnV, ZymValue mxV,
                          ZymValue spaceV, ZymValue seedV) {
    const char* W = "ui.animVarColor(mode, amount, minClamp, maxClamp, colorSpace, seed)";
    int mode; ImVec4 amt, mn, mx; int space, seed;
    if (!reqInt(vm, modeV, W, &mode))    return ZYM_ERROR;
    if (!reqVec4(vm, amtV, W, &amt))     return ZYM_ERROR;
    if (!reqVec4(vm, mnV,  W, &mn))      return ZYM_ERROR;
    if (!reqVec4(vm, mxV,  W, &mx))      return ZYM_ERROR;
    if (!reqInt(vm, spaceV, W, &space))  return ZYM_ERROR;
    if (!reqInt(vm, seedV,  W, &seed))   return ZYM_ERROR;
    ZymValue l = zym_newList(vm);
    zym_listAppend(vm, l, zym_newNumber(kVarTagColor));
    zym_listAppend(vm, l, zym_newNumber(mode));
    for (int i = 0; i < 4; ++i) zym_listAppend(vm, l, zym_newNumber(((float*)&amt)[i]));
    for (int i = 0; i < 4; ++i) zym_listAppend(vm, l, zym_newNumber(((float*)&mn)[i]));
    for (int i = 0; i < 4; ++i) zym_listAppend(vm, l, zym_newNumber(((float*)&mx)[i]));
    zym_listAppend(vm, l, zym_newNumber(space));
    zym_listAppend(vm, l, zym_newNumber((unsigned int)seed));
    return l;
}

// --- Variation unpackers (used by clip key_*_var bindings below) --------

static bool parseVarFloat(ZymVM* vm, ZymValue v, const char* where,
                          iam_variation_float* out) {
    if (!zym_isList(v) || zym_listLength(v) != 6 ||
        !zym_isNumber(zym_listGet(vm, v, 0)) ||
        (int)zym_asNumber(zym_listGet(vm, v, 0)) != kVarTagFloat) {
        zym_runtimeError(vm, "%s expects an animVarFloat(...) descriptor", where);
        return false;
    }
    out->mode      = (int)zym_asNumber(zym_listGet(vm, v, 1));
    out->amount    = (float)zym_asNumber(zym_listGet(vm, v, 2));
    out->min_clamp = (float)zym_asNumber(zym_listGet(vm, v, 3));
    out->max_clamp = (float)zym_asNumber(zym_listGet(vm, v, 4));
    out->seed      = (unsigned int)zym_asNumber(zym_listGet(vm, v, 5));
    out->callback  = nullptr;
    out->user      = nullptr;
    return true;
}

static bool parseVarInt(ZymVM* vm, ZymValue v, const char* where,
                        iam_variation_int* out) {
    if (!zym_isList(v) || zym_listLength(v) != 6 ||
        (int)zym_asNumber(zym_listGet(vm, v, 0)) != kVarTagInt) {
        zym_runtimeError(vm, "%s expects an animVarInt(...) descriptor", where);
        return false;
    }
    out->mode      = (int)zym_asNumber(zym_listGet(vm, v, 1));
    out->amount    = (int)zym_asNumber(zym_listGet(vm, v, 2));
    out->min_clamp = (int)zym_asNumber(zym_listGet(vm, v, 3));
    out->max_clamp = (int)zym_asNumber(zym_listGet(vm, v, 4));
    out->seed      = (unsigned int)zym_asNumber(zym_listGet(vm, v, 5));
    out->callback  = nullptr;
    out->user      = nullptr;
    return true;
}

static bool parseVarVec2(ZymVM* vm, ZymValue v, const char* where,
                         iam_variation_vec2* out) {
    if (!zym_isList(v) || zym_listLength(v) != 9 ||
        (int)zym_asNumber(zym_listGet(vm, v, 0)) != kVarTagVec2) {
        zym_runtimeError(vm, "%s expects an animVarVec2(...) descriptor", where);
        return false;
    }
    *out = iam_varv2_none();
    out->mode      = (int)zym_asNumber(zym_listGet(vm, v, 1));
    out->amount    = ImVec2((float)zym_asNumber(zym_listGet(vm, v, 2)),
                            (float)zym_asNumber(zym_listGet(vm, v, 3)));
    out->min_clamp = ImVec2((float)zym_asNumber(zym_listGet(vm, v, 4)),
                            (float)zym_asNumber(zym_listGet(vm, v, 5)));
    out->max_clamp = ImVec2((float)zym_asNumber(zym_listGet(vm, v, 6)),
                            (float)zym_asNumber(zym_listGet(vm, v, 7)));
    out->seed      = (unsigned int)zym_asNumber(zym_listGet(vm, v, 8));
    return true;
}

static bool parseVarVec4(ZymVM* vm, ZymValue v, const char* where,
                         iam_variation_vec4* out) {
    if (!zym_isList(v) || zym_listLength(v) != 15 ||
        (int)zym_asNumber(zym_listGet(vm, v, 0)) != kVarTagVec4) {
        zym_runtimeError(vm, "%s expects an animVarVec4(...) descriptor", where);
        return false;
    }
    *out = iam_varv4_none();
    out->mode      = (int)zym_asNumber(zym_listGet(vm, v, 1));
    float a[12];
    for (int i = 0; i < 12; ++i) a[i] = (float)zym_asNumber(zym_listGet(vm, v, 2 + i));
    out->amount    = ImVec4(a[0], a[1], a[2], a[3]);
    out->min_clamp = ImVec4(a[4], a[5], a[6], a[7]);
    out->max_clamp = ImVec4(a[8], a[9], a[10], a[11]);
    out->seed      = (unsigned int)zym_asNumber(zym_listGet(vm, v, 14));
    return true;
}

static bool parseVarColor(ZymVM* vm, ZymValue v, const char* where,
                          iam_variation_color* out) {
    if (!zym_isList(v) || zym_listLength(v) != 16 ||
        (int)zym_asNumber(zym_listGet(vm, v, 0)) != kVarTagColor) {
        zym_runtimeError(vm, "%s expects an animVarColor(...) descriptor", where);
        return false;
    }
    *out = iam_varc_none();
    out->mode      = (int)zym_asNumber(zym_listGet(vm, v, 1));
    float a[12];
    for (int i = 0; i < 12; ++i) a[i] = (float)zym_asNumber(zym_listGet(vm, v, 2 + i));
    out->amount    = ImVec4(a[0], a[1], a[2], a[3]);
    out->min_clamp = ImVec4(a[4], a[5], a[6], a[7]);
    out->max_clamp = ImVec4(a[8], a[9], a[10], a[11]);
    out->color_space = (int)zym_asNumber(zym_listGet(vm, v, 14));
    out->seed        = (unsigned int)zym_asNumber(zym_listGet(vm, v, 15));
    return true;
}

// --- Clip builder (active-clip singleton, mirrors §6 path builder) ------

static iam_clip* g_active_clip = nullptr;

static iam_clip* requireActiveClip(ZymVM* vm, const char* where) {
    if (!g_active_clip) {
        zym_runtimeError(vm, "%s called without an active animClipBegin", where);
        return nullptr;
    }
    return g_active_clip;
}

// Optional bezier-control parser used by every key_* with custom bezier.
// `bezierV` is either null (linear/preset only) or a 4-list [x1,y1,x2,y2].
static bool parseOptBezier(ZymVM* vm, ZymValue v, const char* where,
                           float (*out)[4], bool* hasBezier) {
    *hasBezier = false;
    if (zym_isNull(v)) return true;
    if (!zym_isList(v) || zym_listLength(v) != 4) {
        zym_runtimeError(vm, "%s bezier4 must be null or [x1,y1,x2,y2]", where);
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        ZymValue e = zym_listGet(vm, v, i);
        if (!zym_isNumber(e)) {
            zym_runtimeError(vm, "%s bezier4 element %d must be number", where, i);
            return false;
        }
        (*out)[i] = (float)zym_asNumber(e);
    }
    *hasBezier = true;
    return true;
}

ZymValue u_anim_clip_begin(ZymVM* vm, ZymValue /*self*/, ZymValue idV) {
    const char* W = "ui.animClipBegin(clipId)";
    ImGuiID id;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    if (g_active_clip) { delete g_active_clip; g_active_clip = nullptr; }
    g_active_clip = new iam_clip(iam_clip::begin(id));
    return zym_newNumber((double)id);
}

ZymValue u_anim_clip_end(ZymVM* vm, ZymValue /*self*/) {
    iam_clip* c = requireActiveClip(vm, "ui.animClipEnd()");
    if (!c) return ZYM_ERROR;
    ImGuiID id = c->id();
    c->end();
    delete g_active_clip;
    g_active_clip = nullptr;
    return zym_newNumber((double)id);
}

ZymValue u_anim_clip_key_float(ZymVM* vm, ZymValue /*self*/,
                               ZymValue chV, ZymValue tV, ZymValue valV,
                               ZymValue easeV, ZymValue bezierV) {
    const char* W = "ui.animClipKeyFloat(channelId, time, value, easeType, bezier4)";
    iam_clip* c = requireActiveClip(vm, W); if (!c) return ZYM_ERROR;
    ImGuiID ch; double t, val; int ease; float bz[4]; bool hasBz;
    if (!parseImGuiId(vm, chV, W, &ch)) return ZYM_ERROR;
    if (!reqNum(vm, tV, W, &t))         return ZYM_ERROR;
    if (!reqNum(vm, valV, W, &val))     return ZYM_ERROR;
    if (!reqInt(vm, easeV, W, &ease))   return ZYM_ERROR;
    if (!parseOptBezier(vm, bezierV, W, &bz, &hasBz)) return ZYM_ERROR;
    c->key_float(ch, (float)t, (float)val, ease, hasBz ? bz : nullptr);
    return zym_newNull();
}

ZymValue u_anim_clip_key_int(ZymVM* vm, ZymValue /*self*/,
                             ZymValue chV, ZymValue tV, ZymValue valV,
                             ZymValue easeV) {
    const char* W = "ui.animClipKeyInt(channelId, time, value, easeType)";
    iam_clip* c = requireActiveClip(vm, W); if (!c) return ZYM_ERROR;
    ImGuiID ch; double t; int val, ease;
    if (!parseImGuiId(vm, chV, W, &ch)) return ZYM_ERROR;
    if (!reqNum(vm, tV, W, &t))         return ZYM_ERROR;
    if (!reqInt(vm, valV, W, &val))     return ZYM_ERROR;
    if (!reqInt(vm, easeV, W, &ease))   return ZYM_ERROR;
    c->key_int(ch, (float)t, val, ease);
    return zym_newNull();
}

ZymValue u_anim_clip_key_vec2(ZymVM* vm, ZymValue /*self*/,
                              ZymValue chV, ZymValue tV, ZymValue valV,
                              ZymValue easeV, ZymValue bezierV) {
    const char* W = "ui.animClipKeyVec2(channelId, time, value, easeType, bezier4)";
    iam_clip* c = requireActiveClip(vm, W); if (!c) return ZYM_ERROR;
    ImGuiID ch; double t; ImVec2 val; int ease; float bz[4]; bool hasBz;
    if (!parseImGuiId(vm, chV, W, &ch)) return ZYM_ERROR;
    if (!reqNum(vm, tV, W, &t))         return ZYM_ERROR;
    if (!reqVec2(vm, valV, W, &val))    return ZYM_ERROR;
    if (!reqInt(vm, easeV, W, &ease))   return ZYM_ERROR;
    if (!parseOptBezier(vm, bezierV, W, &bz, &hasBz)) return ZYM_ERROR;
    c->key_vec2(ch, (float)t, val, ease, hasBz ? bz : nullptr);
    return zym_newNull();
}

ZymValue u_anim_clip_key_vec4(ZymVM* vm, ZymValue /*self*/,
                              ZymValue chV, ZymValue tV, ZymValue valV,
                              ZymValue easeV, ZymValue bezierV) {
    const char* W = "ui.animClipKeyVec4(channelId, time, value, easeType, bezier4)";
    iam_clip* c = requireActiveClip(vm, W); if (!c) return ZYM_ERROR;
    ImGuiID ch; double t; ImVec4 val; int ease; float bz[4]; bool hasBz;
    if (!parseImGuiId(vm, chV, W, &ch)) return ZYM_ERROR;
    if (!reqNum(vm, tV, W, &t))         return ZYM_ERROR;
    if (!reqVec4(vm, valV, W, &val))    return ZYM_ERROR;
    if (!reqInt(vm, easeV, W, &ease))   return ZYM_ERROR;
    if (!parseOptBezier(vm, bezierV, W, &bz, &hasBz)) return ZYM_ERROR;
    c->key_vec4(ch, (float)t, val, ease, hasBz ? bz : nullptr);
    return zym_newNull();
}

ZymValue u_anim_clip_key_color(ZymVM* vm, ZymValue /*self*/,
                               ZymValue chV, ZymValue tV, ZymValue valV,
                               ZymValue spaceV, ZymValue easeV, ZymValue bezierV) {
    const char* W = "ui.animClipKeyColor(channelId, time, color, colorSpace, easeType, bezier4)";
    iam_clip* c = requireActiveClip(vm, W); if (!c) return ZYM_ERROR;
    ImGuiID ch; double t; ImVec4 val; int space, ease; float bz[4]; bool hasBz;
    if (!parseImGuiId(vm, chV, W, &ch)) return ZYM_ERROR;
    if (!reqNum(vm, tV, W, &t))         return ZYM_ERROR;
    if (!reqVec4(vm, valV, W, &val))    return ZYM_ERROR;
    if (!reqInt(vm, spaceV, W, &space)) return ZYM_ERROR;
    if (!reqInt(vm, easeV, W, &ease))   return ZYM_ERROR;
    if (!parseOptBezier(vm, bezierV, W, &bz, &hasBz)) return ZYM_ERROR;
    c->key_color(ch, (float)t, val, space, ease, hasBz ? bz : nullptr);
    return zym_newNull();
}

ZymValue u_anim_clip_key_float_spring(ZymVM* vm, ZymValue /*self*/,
                                      ZymValue chV, ZymValue tV, ZymValue targetV,
                                      ZymValue mV, ZymValue kV, ZymValue dV, ZymValue v0V) {
    const char* W = "ui.animClipKeyFloatSpring(channelId, time, target, mass, stiffness, damping, initialVelocity)";
    iam_clip* c = requireActiveClip(vm, W); if (!c) return ZYM_ERROR;
    ImGuiID ch; double t, target, m, k, d, v0;
    if (!parseImGuiId(vm, chV, W, &ch))    return ZYM_ERROR;
    if (!reqNum(vm, tV, W, &t))            return ZYM_ERROR;
    if (!reqNum(vm, targetV, W, &target))  return ZYM_ERROR;
    if (!reqNum(vm, mV, W, &m))            return ZYM_ERROR;
    if (!reqNum(vm, kV, W, &k))            return ZYM_ERROR;
    if (!reqNum(vm, dV, W, &d))            return ZYM_ERROR;
    if (!reqNum(vm, v0V, W, &v0))          return ZYM_ERROR;
    iam_spring_params sp{(float)m, (float)k, (float)d, (float)v0};
    c->key_float_spring(ch, (float)t, (float)target, sp);
    return zym_newNull();
}

// --- Clip key_*_var (variation keyframes) -------------------------------

ZymValue u_anim_clip_key_float_var(ZymVM* vm, ZymValue /*self*/,
                                   ZymValue chV, ZymValue tV, ZymValue valV,
                                   ZymValue varV, ZymValue easeV, ZymValue bezierV) {
    const char* W = "ui.animClipKeyFloatVar(channelId, time, value, varFloat, easeType, bezier4)";
    iam_clip* c = requireActiveClip(vm, W); if (!c) return ZYM_ERROR;
    ImGuiID ch; double t, val; int ease;
    iam_variation_float var; float bz[4]; bool hasBz;
    if (!parseImGuiId(vm, chV, W, &ch))       return ZYM_ERROR;
    if (!reqNum(vm, tV, W, &t))               return ZYM_ERROR;
    if (!reqNum(vm, valV, W, &val))           return ZYM_ERROR;
    if (!parseVarFloat(vm, varV, W, &var))    return ZYM_ERROR;
    if (!reqInt(vm, easeV, W, &ease))         return ZYM_ERROR;
    if (!parseOptBezier(vm, bezierV, W, &bz, &hasBz)) return ZYM_ERROR;
    c->key_float_var(ch, (float)t, (float)val, var, ease, hasBz ? bz : nullptr);
    return zym_newNull();
}

ZymValue u_anim_clip_key_int_var(ZymVM* vm, ZymValue /*self*/,
                                 ZymValue chV, ZymValue tV, ZymValue valV,
                                 ZymValue varV, ZymValue easeV) {
    const char* W = "ui.animClipKeyIntVar(channelId, time, value, varInt, easeType)";
    iam_clip* c = requireActiveClip(vm, W); if (!c) return ZYM_ERROR;
    ImGuiID ch; double t; int val, ease; iam_variation_int var;
    if (!parseImGuiId(vm, chV, W, &ch))     return ZYM_ERROR;
    if (!reqNum(vm, tV, W, &t))             return ZYM_ERROR;
    if (!reqInt(vm, valV, W, &val))         return ZYM_ERROR;
    if (!parseVarInt(vm, varV, W, &var))    return ZYM_ERROR;
    if (!reqInt(vm, easeV, W, &ease))       return ZYM_ERROR;
    c->key_int_var(ch, (float)t, val, var, ease);
    return zym_newNull();
}

ZymValue u_anim_clip_key_vec2_var(ZymVM* vm, ZymValue /*self*/,
                                  ZymValue chV, ZymValue tV, ZymValue valV,
                                  ZymValue varV, ZymValue easeV, ZymValue bezierV) {
    const char* W = "ui.animClipKeyVec2Var(channelId, time, value, varVec2, easeType, bezier4)";
    iam_clip* c = requireActiveClip(vm, W); if (!c) return ZYM_ERROR;
    ImGuiID ch; double t; ImVec2 val; int ease;
    iam_variation_vec2 var; float bz[4]; bool hasBz;
    if (!parseImGuiId(vm, chV, W, &ch))       return ZYM_ERROR;
    if (!reqNum(vm, tV, W, &t))               return ZYM_ERROR;
    if (!reqVec2(vm, valV, W, &val))          return ZYM_ERROR;
    if (!parseVarVec2(vm, varV, W, &var))     return ZYM_ERROR;
    if (!reqInt(vm, easeV, W, &ease))         return ZYM_ERROR;
    if (!parseOptBezier(vm, bezierV, W, &bz, &hasBz)) return ZYM_ERROR;
    c->key_vec2_var(ch, (float)t, val, var, ease, hasBz ? bz : nullptr);
    return zym_newNull();
}

ZymValue u_anim_clip_key_vec4_var(ZymVM* vm, ZymValue /*self*/,
                                  ZymValue chV, ZymValue tV, ZymValue valV,
                                  ZymValue varV, ZymValue easeV, ZymValue bezierV) {
    const char* W = "ui.animClipKeyVec4Var(channelId, time, value, varVec4, easeType, bezier4)";
    iam_clip* c = requireActiveClip(vm, W); if (!c) return ZYM_ERROR;
    ImGuiID ch; double t; ImVec4 val; int ease;
    iam_variation_vec4 var; float bz[4]; bool hasBz;
    if (!parseImGuiId(vm, chV, W, &ch))       return ZYM_ERROR;
    if (!reqNum(vm, tV, W, &t))               return ZYM_ERROR;
    if (!reqVec4(vm, valV, W, &val))          return ZYM_ERROR;
    if (!parseVarVec4(vm, varV, W, &var))     return ZYM_ERROR;
    if (!reqInt(vm, easeV, W, &ease))         return ZYM_ERROR;
    if (!parseOptBezier(vm, bezierV, W, &bz, &hasBz)) return ZYM_ERROR;
    c->key_vec4_var(ch, (float)t, val, var, ease, hasBz ? bz : nullptr);
    return zym_newNull();
}

ZymValue u_anim_clip_key_color_var(ZymVM* vm, ZymValue /*self*/,
                                   ZymValue chV, ZymValue tV, ZymValue valV,
                                   ZymValue varV, ZymValue spaceV,
                                   ZymValue easeV, ZymValue bezierV) {
    const char* W = "ui.animClipKeyColorVar(channelId, time, color, varColor, colorSpace, easeType, bezier4)";
    iam_clip* c = requireActiveClip(vm, W); if (!c) return ZYM_ERROR;
    ImGuiID ch; double t; ImVec4 val; int space, ease;
    iam_variation_color var; float bz[4]; bool hasBz;
    if (!parseImGuiId(vm, chV, W, &ch))       return ZYM_ERROR;
    if (!reqNum(vm, tV, W, &t))               return ZYM_ERROR;
    if (!reqVec4(vm, valV, W, &val))          return ZYM_ERROR;
    if (!parseVarColor(vm, varV, W, &var))    return ZYM_ERROR;
    if (!reqInt(vm, spaceV, W, &space))       return ZYM_ERROR;
    if (!reqInt(vm, easeV, W, &ease))         return ZYM_ERROR;
    if (!parseOptBezier(vm, bezierV, W, &bz, &hasBz)) return ZYM_ERROR;
    c->key_color_var(ch, (float)t, val, var, space, ease, hasBz ? bz : nullptr);
    return zym_newNull();
}

// --- Clip key_*_rel (anchor-relative keyframes) -------------------------

ZymValue u_anim_clip_key_float_rel(ZymVM* vm, ZymValue /*self*/,
                                   ZymValue chV, ZymValue tV, ZymValue pctV,
                                   ZymValue biasV, ZymValue anchorV, ZymValue axisV,
                                   ZymValue easeV, ZymValue bezierV) {
    const char* W = "ui.animClipKeyFloatRel(channelId, time, percent, pxBias, anchorSpace, axis, easeType, bezier4)";
    iam_clip* c = requireActiveClip(vm, W); if (!c) return ZYM_ERROR;
    ImGuiID ch; double t, pct, bias; int anchor, axis, ease;
    float bz[4]; bool hasBz;
    if (!parseImGuiId(vm, chV, W, &ch)) return ZYM_ERROR;
    if (!reqNum(vm, tV, W, &t))         return ZYM_ERROR;
    if (!reqNum(vm, pctV, W, &pct))     return ZYM_ERROR;
    if (!reqNum(vm, biasV, W, &bias))   return ZYM_ERROR;
    if (!reqInt(vm, anchorV, W, &anchor)) return ZYM_ERROR;
    if (!reqInt(vm, axisV, W, &axis))   return ZYM_ERROR;
    if (!reqInt(vm, easeV, W, &ease))   return ZYM_ERROR;
    if (!parseOptBezier(vm, bezierV, W, &bz, &hasBz)) return ZYM_ERROR;
    c->key_float_rel(ch, (float)t, (float)pct, (float)bias, anchor, axis, ease, hasBz ? bz : nullptr);
    return zym_newNull();
}

ZymValue u_anim_clip_key_vec2_rel(ZymVM* vm, ZymValue /*self*/,
                                  ZymValue chV, ZymValue tV, ZymValue pctV,
                                  ZymValue biasV, ZymValue anchorV,
                                  ZymValue easeV, ZymValue bezierV) {
    const char* W = "ui.animClipKeyVec2Rel(channelId, time, percent, pxBias, anchorSpace, easeType, bezier4)";
    iam_clip* c = requireActiveClip(vm, W); if (!c) return ZYM_ERROR;
    ImGuiID ch; double t; ImVec2 pct, bias; int anchor, ease;
    float bz[4]; bool hasBz;
    if (!parseImGuiId(vm, chV, W, &ch)) return ZYM_ERROR;
    if (!reqNum(vm, tV, W, &t))         return ZYM_ERROR;
    if (!reqVec2(vm, pctV, W, &pct))    return ZYM_ERROR;
    if (!reqVec2(vm, biasV, W, &bias))  return ZYM_ERROR;
    if (!reqInt(vm, anchorV, W, &anchor)) return ZYM_ERROR;
    if (!reqInt(vm, easeV, W, &ease))   return ZYM_ERROR;
    if (!parseOptBezier(vm, bezierV, W, &bz, &hasBz)) return ZYM_ERROR;
    c->key_vec2_rel(ch, (float)t, pct, bias, anchor, ease, hasBz ? bz : nullptr);
    return zym_newNull();
}

ZymValue u_anim_clip_key_vec4_rel(ZymVM* vm, ZymValue /*self*/,
                                  ZymValue chV, ZymValue tV, ZymValue pctV,
                                  ZymValue biasV, ZymValue anchorV,
                                  ZymValue easeV, ZymValue bezierV) {
    const char* W = "ui.animClipKeyVec4Rel(channelId, time, percent, pxBias, anchorSpace, easeType, bezier4)";
    iam_clip* c = requireActiveClip(vm, W); if (!c) return ZYM_ERROR;
    ImGuiID ch; double t; ImVec4 pct, bias; int anchor, ease;
    float bz[4]; bool hasBz;
    if (!parseImGuiId(vm, chV, W, &ch)) return ZYM_ERROR;
    if (!reqNum(vm, tV, W, &t))         return ZYM_ERROR;
    if (!reqVec4(vm, pctV, W, &pct))    return ZYM_ERROR;
    if (!reqVec4(vm, biasV, W, &bias))  return ZYM_ERROR;
    if (!reqInt(vm, anchorV, W, &anchor)) return ZYM_ERROR;
    if (!reqInt(vm, easeV, W, &ease))   return ZYM_ERROR;
    if (!parseOptBezier(vm, bezierV, W, &bz, &hasBz)) return ZYM_ERROR;
    c->key_vec4_rel(ch, (float)t, pct, bias, anchor, ease, hasBz ? bz : nullptr);
    return zym_newNull();
}

ZymValue u_anim_clip_key_color_rel(ZymVM* vm, ZymValue /*self*/,
                                   ZymValue chV, ZymValue tV, ZymValue pctV,
                                   ZymValue biasV, ZymValue spaceV, ZymValue anchorV,
                                   ZymValue easeV, ZymValue bezierV) {
    const char* W = "ui.animClipKeyColorRel(channelId, time, percent, pxBias, colorSpace, anchorSpace, easeType, bezier4)";
    iam_clip* c = requireActiveClip(vm, W); if (!c) return ZYM_ERROR;
    ImGuiID ch; double t; ImVec4 pct, bias; int space, anchor, ease;
    float bz[4]; bool hasBz;
    if (!parseImGuiId(vm, chV, W, &ch))   return ZYM_ERROR;
    if (!reqNum(vm, tV, W, &t))           return ZYM_ERROR;
    if (!reqVec4(vm, pctV, W, &pct))      return ZYM_ERROR;
    if (!reqVec4(vm, biasV, W, &bias))    return ZYM_ERROR;
    if (!reqInt(vm, spaceV, W, &space))   return ZYM_ERROR;
    if (!reqInt(vm, anchorV, W, &anchor)) return ZYM_ERROR;
    if (!reqInt(vm, easeV, W, &ease))     return ZYM_ERROR;
    if (!parseOptBezier(vm, bezierV, W, &bz, &hasBz)) return ZYM_ERROR;
    c->key_color_rel(ch, (float)t, pct, bias, space, anchor, ease, hasBz ? bz : nullptr);
    return zym_newNull();
}

// --- Clip timeline grouping + options -----------------------------------

#define CLIP_VOID_FN0(NAME, METHOD, WHERE)                              \
    ZymValue NAME(ZymVM* vm, ZymValue /*self*/) {                       \
        iam_clip* c = requireActiveClip(vm, WHERE); if (!c) return ZYM_ERROR; \
        c->METHOD();                                                    \
        return zym_newNull();                                           \
    }
CLIP_VOID_FN0(u_anim_clip_seq_begin, seq_begin, "ui.animClipSeqBegin()")
CLIP_VOID_FN0(u_anim_clip_seq_end,   seq_end,   "ui.animClipSeqEnd()")
CLIP_VOID_FN0(u_anim_clip_par_begin, par_begin, "ui.animClipParBegin()")
CLIP_VOID_FN0(u_anim_clip_par_end,   par_end,   "ui.animClipParEnd()")
#undef CLIP_VOID_FN0

ZymValue u_anim_clip_set_loop(ZymVM* vm, ZymValue /*self*/,
                              ZymValue loopV, ZymValue dirV, ZymValue countV) {
    const char* W = "ui.animClipSetLoop(loop, direction, loopCount)";
    iam_clip* c = requireActiveClip(vm, W); if (!c) return ZYM_ERROR;
    bool loop; int dir, count;
    if (!reqBool(vm, loopV, W, &loop)) return ZYM_ERROR;
    if (!reqInt(vm, dirV, W, &dir))    return ZYM_ERROR;
    if (!reqInt(vm, countV, W, &count)) return ZYM_ERROR;
    c->set_loop(loop, dir, count);
    return zym_newNull();
}

ZymValue u_anim_clip_set_delay(ZymVM* vm, ZymValue /*self*/, ZymValue delayV) {
    const char* W = "ui.animClipSetDelay(delaySeconds)";
    iam_clip* c = requireActiveClip(vm, W); if (!c) return ZYM_ERROR;
    double d;
    if (!reqNum(vm, delayV, W, &d)) return ZYM_ERROR;
    c->set_delay((float)d);
    return zym_newNull();
}

ZymValue u_anim_clip_set_stagger(ZymVM* vm, ZymValue /*self*/,
                                 ZymValue cntV, ZymValue eachV, ZymValue biasV) {
    const char* W = "ui.animClipSetStagger(count, eachDelay, fromCenterBias)";
    iam_clip* c = requireActiveClip(vm, W); if (!c) return ZYM_ERROR;
    int cnt; double each, bias;
    if (!reqInt(vm, cntV, W, &cnt))   return ZYM_ERROR;
    if (!reqNum(vm, eachV, W, &each)) return ZYM_ERROR;
    if (!reqNum(vm, biasV, W, &bias)) return ZYM_ERROR;
    c->set_stagger(cnt, (float)each, (float)bias);
    return zym_newNull();
}

ZymValue u_anim_clip_set_duration_var(ZymVM* vm, ZymValue /*self*/, ZymValue varV) {
    const char* W = "ui.animClipSetDurationVar(varFloat)";
    iam_clip* c = requireActiveClip(vm, W); if (!c) return ZYM_ERROR;
    iam_variation_float var;
    if (!parseVarFloat(vm, varV, W, &var)) return ZYM_ERROR;
    c->set_duration_var(var);
    return zym_newNull();
}

ZymValue u_anim_clip_set_delay_var(ZymVM* vm, ZymValue /*self*/, ZymValue varV) {
    const char* W = "ui.animClipSetDelayVar(varFloat)";
    iam_clip* c = requireActiveClip(vm, W); if (!c) return ZYM_ERROR;
    iam_variation_float var;
    if (!parseVarFloat(vm, varV, W, &var)) return ZYM_ERROR;
    c->set_delay_var(var);
    return zym_newNull();
}

ZymValue u_anim_clip_set_timescale_var(ZymVM* vm, ZymValue /*self*/, ZymValue varV) {
    const char* W = "ui.animClipSetTimescaleVar(varFloat)";
    iam_clip* c = requireActiveClip(vm, W); if (!c) return ZYM_ERROR;
    iam_variation_float var;
    if (!parseVarFloat(vm, varV, W, &var)) return ZYM_ERROR;
    c->set_timescale_var(var);
    return zym_newNull();
}

// --- Clip-system globals ------------------------------------------------

ZymValue u_anim_clip_init(ZymVM* vm, ZymValue /*self*/,
                          ZymValue clipCapV, ZymValue instCapV) {
    const char* W = "ui.animClipInit(initialClipCap, initialInstCap)";
    int clipCap, instCap;
    if (!reqInt(vm, clipCapV, W, &clipCap)) return ZYM_ERROR;
    if (!reqInt(vm, instCapV, W, &instCap)) return ZYM_ERROR;
    iam_clip_init(clipCap, instCap);
    return zym_newNull();
}

ZymValue u_anim_clip_shutdown(ZymVM* /*vm*/, ZymValue /*self*/) {
    if (g_active_clip) { delete g_active_clip; g_active_clip = nullptr; }
    iam_clip_shutdown();
    return zym_newNull();
}

ZymValue u_anim_clip_gc(ZymVM* vm, ZymValue /*self*/, ZymValue ageV) {
    int age;
    if (!reqInt(vm, ageV, "ui.animClipGc(maxAgeFrames)", &age)) return ZYM_ERROR;
    if (age < 0) age = 0;
    iam_clip_gc((unsigned int)age);
    return zym_newNull();
}

ZymValue u_anim_play(ZymVM* vm, ZymValue /*self*/,
                    ZymValue clipIdV, ZymValue instIdV) {
    const char* W = "ui.animPlay(clipId, instanceId) -> instanceId";
    ImGuiID clipId, instId;
    if (!parseImGuiId(vm, clipIdV, W, &clipId)) return ZYM_ERROR;
    if (!parseImGuiId(vm, instIdV, W, &instId)) return ZYM_ERROR;
    iam_instance inst = iam_play(clipId, instId);
    return zym_newNumber((double)inst.id());
}

ZymValue u_anim_get_instance(ZymVM* vm, ZymValue /*self*/, ZymValue instIdV) {
    const char* W = "ui.animGetInstance(instanceId) -> instanceId or 0";
    ImGuiID instId;
    if (!parseImGuiId(vm, instIdV, W, &instId)) return ZYM_ERROR;
    iam_instance inst = iam_get_instance(instId);
    return zym_newNumber((double)(inst.valid() ? inst.id() : 0u));
}

ZymValue u_anim_clip_duration(ZymVM* vm, ZymValue /*self*/, ZymValue clipIdV) {
    ImGuiID clipId;
    if (!parseImGuiId(vm, clipIdV, "ui.animClipDuration(clipId)", &clipId)) return ZYM_ERROR;
    return zym_newNumber((double)iam_clip_duration(clipId));
}

ZymValue u_anim_clip_exists(ZymVM* vm, ZymValue /*self*/, ZymValue clipIdV) {
    ImGuiID clipId;
    if (!parseImGuiId(vm, clipIdV, "ui.animClipExists(clipId)", &clipId)) return ZYM_ERROR;
    return zym_newBool(iam_clip_exists(clipId));
}

ZymValue u_anim_stagger_delay(ZymVM* vm, ZymValue /*self*/,
                              ZymValue clipIdV, ZymValue idxV) {
    const char* W = "ui.animStaggerDelay(clipId, index)";
    ImGuiID clipId; int idx;
    if (!parseImGuiId(vm, clipIdV, W, &clipId)) return ZYM_ERROR;
    if (!reqInt(vm, idxV, W, &idx)) return ZYM_ERROR;
    return zym_newNumber((double)iam_stagger_delay(clipId, idx));
}

ZymValue u_anim_play_stagger(ZymVM* vm, ZymValue /*self*/,
                             ZymValue clipIdV, ZymValue instIdV, ZymValue idxV) {
    const char* W = "ui.animPlayStagger(clipId, instanceId, index) -> instanceId";
    ImGuiID clipId, instId; int idx;
    if (!parseImGuiId(vm, clipIdV, W, &clipId)) return ZYM_ERROR;
    if (!parseImGuiId(vm, instIdV, W, &instId)) return ZYM_ERROR;
    if (!reqInt(vm, idxV, W, &idx)) return ZYM_ERROR;
    iam_instance inst = iam_play_stagger(clipId, instId, idx);
    return zym_newNumber((double)inst.id());
}

// --- Instance ops -------------------------------------------------------
//
// Instances are addressed by `ImGuiID` from the script side; each op
// reconstructs an `iam_instance` wrapper and dispatches. Ops on a
// non-existent instance silently no-op (upstream behaviour).

ZymValue u_anim_instance_pause(ZymVM* vm, ZymValue /*self*/, ZymValue idV) {
    ImGuiID id;
    if (!parseImGuiId(vm, idV, "ui.animInstancePause(id)", &id)) return ZYM_ERROR;
    iam_instance(id).pause();
    return zym_newNull();
}

ZymValue u_anim_instance_resume(ZymVM* vm, ZymValue /*self*/, ZymValue idV) {
    ImGuiID id;
    if (!parseImGuiId(vm, idV, "ui.animInstanceResume(id)", &id)) return ZYM_ERROR;
    iam_instance(id).resume();
    return zym_newNull();
}

ZymValue u_anim_instance_stop(ZymVM* vm, ZymValue /*self*/, ZymValue idV) {
    ImGuiID id;
    if (!parseImGuiId(vm, idV, "ui.animInstanceStop(id)", &id)) return ZYM_ERROR;
    iam_instance(id).stop();
    return zym_newNull();
}

ZymValue u_anim_instance_destroy(ZymVM* vm, ZymValue /*self*/, ZymValue idV) {
    ImGuiID id;
    if (!parseImGuiId(vm, idV, "ui.animInstanceDestroy(id)", &id)) return ZYM_ERROR;
    iam_instance(id).destroy();
    return zym_newNull();
}

ZymValue u_anim_instance_seek(ZymVM* vm, ZymValue /*self*/,
                              ZymValue idV, ZymValue tV) {
    const char* W = "ui.animInstanceSeek(id, time)";
    ImGuiID id; double t;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    if (!reqNum(vm, tV, W, &t)) return ZYM_ERROR;
    iam_instance(id).seek((float)t);
    return zym_newNull();
}

ZymValue u_anim_instance_set_time_scale(ZymVM* vm, ZymValue /*self*/,
                                        ZymValue idV, ZymValue sV) {
    const char* W = "ui.animInstanceSetTimeScale(id, scale)";
    ImGuiID id; double s;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    if (!reqNum(vm, sV, W, &s)) return ZYM_ERROR;
    iam_instance(id).set_time_scale((float)s);
    return zym_newNull();
}

ZymValue u_anim_instance_set_weight(ZymVM* vm, ZymValue /*self*/,
                                    ZymValue idV, ZymValue wV) {
    const char* W = "ui.animInstanceSetWeight(id, weight)";
    ImGuiID id; double w;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    if (!reqNum(vm, wV, W, &w)) return ZYM_ERROR;
    iam_instance(id).set_weight((float)w);
    return zym_newNull();
}

ZymValue u_anim_instance_then(ZymVM* vm, ZymValue /*self*/,
                              ZymValue idV, ZymValue nextClipV, ZymValue nextInstV) {
    const char* W = "ui.animInstanceThen(id, nextClipId, nextInstanceId)";
    ImGuiID id, nc, ni;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    if (!parseImGuiId(vm, nextClipV, W, &nc)) return ZYM_ERROR;
    // If `nextInstanceId` is null, upstream auto-generates one via the
    // single-arg `then(clip)` overload; otherwise route to the two-arg form.
    if (zym_isNull(nextInstV)) {
        iam_instance(id).then(nc);
    } else {
        if (!parseImGuiId(vm, nextInstV, W, &ni)) return ZYM_ERROR;
        iam_instance(id).then(nc, ni);
    }
    return zym_newNull();
}

ZymValue u_anim_instance_then_delay(ZymVM* vm, ZymValue /*self*/,
                                    ZymValue idV, ZymValue dV) {
    const char* W = "ui.animInstanceThenDelay(id, delay)";
    ImGuiID id; double d;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    if (!reqNum(vm, dV, W, &d)) return ZYM_ERROR;
    iam_instance(id).then_delay((float)d);
    return zym_newNull();
}

ZymValue u_anim_instance_time(ZymVM* vm, ZymValue /*self*/, ZymValue idV) {
    ImGuiID id;
    if (!parseImGuiId(vm, idV, "ui.animInstanceTime(id)", &id)) return ZYM_ERROR;
    return zym_newNumber((double)iam_instance(id).time());
}

ZymValue u_anim_instance_duration(ZymVM* vm, ZymValue /*self*/, ZymValue idV) {
    ImGuiID id;
    if (!parseImGuiId(vm, idV, "ui.animInstanceDuration(id)", &id)) return ZYM_ERROR;
    return zym_newNumber((double)iam_instance(id).duration());
}

ZymValue u_anim_instance_is_playing(ZymVM* vm, ZymValue /*self*/, ZymValue idV) {
    ImGuiID id;
    if (!parseImGuiId(vm, idV, "ui.animInstanceIsPlaying(id)", &id)) return ZYM_ERROR;
    return zym_newBool(iam_instance(id).is_playing());
}

ZymValue u_anim_instance_is_paused(ZymVM* vm, ZymValue /*self*/, ZymValue idV) {
    ImGuiID id;
    if (!parseImGuiId(vm, idV, "ui.animInstanceIsPaused(id)", &id)) return ZYM_ERROR;
    return zym_newBool(iam_instance(id).is_paused());
}

ZymValue u_anim_instance_valid(ZymVM* vm, ZymValue /*self*/, ZymValue idV) {
    ImGuiID id;
    if (!parseImGuiId(vm, idV, "ui.animInstanceValid(id)", &id)) return ZYM_ERROR;
    return zym_newBool(iam_instance(id).valid());
}

// Channel-value queries. Each returns the sampled value on success or
// `null` if the instance/channel doesn't exist (mirrors upstream's
// bool return convention).
ZymValue u_anim_instance_get_float(ZymVM* vm, ZymValue /*self*/,
                                   ZymValue idV, ZymValue chV) {
    const char* W = "ui.animInstanceGetFloat(id, channelId) -> number or null";
    ImGuiID id, ch;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    if (!parseImGuiId(vm, chV, W, &ch)) return ZYM_ERROR;
    float out = 0.0f;
    if (!iam_instance(id).get_float(ch, &out)) return zym_newNull();
    return zym_newNumber((double)out);
}

ZymValue u_anim_instance_get_vec2(ZymVM* vm, ZymValue /*self*/,
                                  ZymValue idV, ZymValue chV) {
    const char* W = "ui.animInstanceGetVec2(id, channelId) -> [x,y] or null";
    ImGuiID id, ch;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    if (!parseImGuiId(vm, chV, W, &ch)) return ZYM_ERROR;
    ImVec2 out(0, 0);
    if (!iam_instance(id).get_vec2(ch, &out)) return zym_newNull();
    return packVec2(vm, out);
}

ZymValue u_anim_instance_get_vec4(ZymVM* vm, ZymValue /*self*/,
                                  ZymValue idV, ZymValue chV) {
    const char* W = "ui.animInstanceGetVec4(id, channelId) -> [x,y,z,w] or null";
    ImGuiID id, ch;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    if (!parseImGuiId(vm, chV, W, &ch)) return ZYM_ERROR;
    ImVec4 out(0, 0, 0, 0);
    if (!iam_instance(id).get_vec4(ch, &out)) return zym_newNull();
    return packVec4(vm, out);
}

ZymValue u_anim_instance_get_int(ZymVM* vm, ZymValue /*self*/,
                                 ZymValue idV, ZymValue chV) {
    const char* W = "ui.animInstanceGetInt(id, channelId) -> int or null";
    ImGuiID id, ch;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    if (!parseImGuiId(vm, chV, W, &ch)) return ZYM_ERROR;
    int out = 0;
    if (!iam_instance(id).get_int(ch, &out)) return zym_newNull();
    return zym_newNumber((double)out);
}

ZymValue u_anim_instance_get_color(ZymVM* vm, ZymValue /*self*/,
                                   ZymValue idV, ZymValue chV, ZymValue spaceV) {
    const char* W = "ui.animInstanceGetColor(id, channelId, colorSpace) -> [r,g,b,a] or null";
    ImGuiID id, ch; int space;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    if (!parseImGuiId(vm, chV, W, &ch)) return ZYM_ERROR;
    if (!reqInt(vm, spaceV, W, &space)) return ZYM_ERROR;
    ImVec4 out(0, 0, 0, 0);
    if (!iam_instance(id).get_color(ch, &out, space)) return zym_newNull();
    return packVec4(vm, out);
}

// --- Layering -----------------------------------------------------------
//
// Procedural form of upstream's `iam_layer_begin / iam_layer_add /
// iam_layer_end`. `iam_layer_add` takes an `iam_instance` value
// in C++; we reconstruct one from the script-supplied ID.

ZymValue u_anim_layer_begin(ZymVM* vm, ZymValue /*self*/, ZymValue idV) {
    ImGuiID id;
    if (!parseImGuiId(vm, idV, "ui.animLayerBegin(targetInstanceId)", &id)) return ZYM_ERROR;
    iam_layer_begin(id);
    return zym_newNull();
}

ZymValue u_anim_layer_add(ZymVM* vm, ZymValue /*self*/,
                          ZymValue srcIdV, ZymValue wV) {
    const char* W = "ui.animLayerAdd(srcInstanceId, weight)";
    ImGuiID srcId; double w;
    if (!parseImGuiId(vm, srcIdV, W, &srcId)) return ZYM_ERROR;
    if (!reqNum(vm, wV, W, &w)) return ZYM_ERROR;
    iam_layer_add(iam_instance(srcId), (float)w);
    return zym_newNull();
}

ZymValue u_anim_layer_end(ZymVM* vm, ZymValue /*self*/, ZymValue idV) {
    ImGuiID id;
    if (!parseImGuiId(vm, idV, "ui.animLayerEnd(targetInstanceId)", &id)) return ZYM_ERROR;
    iam_layer_end(id);
    return zym_newNull();
}

ZymValue u_anim_get_blended_float(ZymVM* vm, ZymValue /*self*/,
                                  ZymValue idV, ZymValue chV) {
    const char* W = "ui.animGetBlendedFloat(instanceId, channelId) -> number or null";
    ImGuiID id, ch;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    if (!parseImGuiId(vm, chV, W, &ch)) return ZYM_ERROR;
    float out = 0.0f;
    if (!iam_get_blended_float(id, ch, &out)) return zym_newNull();
    return zym_newNumber((double)out);
}

ZymValue u_anim_get_blended_vec2(ZymVM* vm, ZymValue /*self*/,
                                 ZymValue idV, ZymValue chV) {
    const char* W = "ui.animGetBlendedVec2(instanceId, channelId) -> [x,y] or null";
    ImGuiID id, ch;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    if (!parseImGuiId(vm, chV, W, &ch)) return ZYM_ERROR;
    ImVec2 out(0, 0);
    if (!iam_get_blended_vec2(id, ch, &out)) return zym_newNull();
    return packVec2(vm, out);
}

ZymValue u_anim_get_blended_vec4(ZymVM* vm, ZymValue /*self*/,
                                 ZymValue idV, ZymValue chV) {
    const char* W = "ui.animGetBlendedVec4(instanceId, channelId) -> [x,y,z,w] or null";
    ImGuiID id, ch;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    if (!parseImGuiId(vm, chV, W, &ch)) return ZYM_ERROR;
    ImVec4 out(0, 0, 0, 0);
    if (!iam_get_blended_vec4(id, ch, &out)) return zym_newNull();
    return packVec4(vm, out);
}

ZymValue u_anim_get_blended_int(ZymVM* vm, ZymValue /*self*/,
                                ZymValue idV, ZymValue chV) {
    const char* W = "ui.animGetBlendedInt(instanceId, channelId) -> int or null";
    ImGuiID id, ch;
    if (!parseImGuiId(vm, idV, W, &id)) return ZYM_ERROR;
    if (!parseImGuiId(vm, chV, W, &ch)) return ZYM_ERROR;
    int out = 0;
    if (!iam_get_blended_int(id, ch, &out)) return zym_newNull();
    return zym_newNumber((double)out);
}

// --- Persistence --------------------------------------------------------
//
// Returns the upstream `iam_result` enum directly (0 = ok). Scripts can
// compare against `iam_ok` (0); upstream doesn't expose the enum as
// individually named constants in a way that maps cleanly, so we keep
// the raw int return for simplicity.

ZymValue u_anim_clip_save(ZymVM* vm, ZymValue /*self*/,
                          ZymValue clipIdV, ZymValue pathV) {
    const char* W = "ui.animClipSave(clipId, path) -> resultCode";
    ImGuiID clipId; std::string path;
    if (!parseImGuiId(vm, clipIdV, W, &clipId)) return ZYM_ERROR;
    if (!reqStr(vm, pathV, W, &path)) return ZYM_ERROR;
    iam_result r = iam_clip_save(clipId, path.c_str());
    return zym_newNumber((double)(int)r);
}

ZymValue u_anim_clip_load(ZymVM* vm, ZymValue /*self*/, ZymValue pathV) {
    const char* W = "ui.animClipLoad(path) -> [resultCode, clipIdOrZero]";
    std::string path;
    if (!reqStr(vm, pathV, W, &path)) return ZYM_ERROR;
    ImGuiID outId = 0;
    iam_result r = iam_clip_load(path.c_str(), &outId);
    ZymValue l = zym_newList(vm);
    zym_listAppend(vm, l, zym_newNumber((double)(int)r));
    zym_listAppend(vm, l, zym_newNumber((double)outId));
    return l;
}

} // namespace

// ---- registration --------------------------------------------------------

void registerImAnimBindings(ZymVM* vm, ZymValue obj, ZymValue context, RootScope& roots) {
#define MOD(name, sig, fn) \
    ZymValue name = roots.push(zym_createNativeClosure(vm, sig, (void*)fn, context));

    // SECTION 1 — frame bookkeeping + globals + easing eval.
    MOD(animUpdateBeginFrame,   "animUpdateBeginFrame()",                       u_anim_update_begin_frame)
    MOD(animGc,                 "animGc(maxAgeFrames)",                         u_anim_gc)
    MOD(animReserve,            "animReserve(float, vec2, vec4, int, color)",   u_anim_reserve)
    MOD(animSetEaseLutSamples,  "animSetEaseLutSamples(count)",                 u_anim_set_ease_lut_samples)
    MOD(animSetGlobalTimeScale, "animSetGlobalTimeScale(scale)",                u_anim_set_global_time_scale)
    MOD(animGetGlobalTimeScale, "animGetGlobalTimeScale()",                     u_anim_get_global_time_scale)
    MOD(animSetLazyInit,        "animSetLazyInit(enable)",                      u_anim_set_lazy_init)
    MOD(animIsLazyInitEnabled,  "animIsLazyInitEnabled()",                      u_anim_is_lazy_init_enabled)
    MOD(animEvalPreset,         "animEvalPreset(type, t)",                      u_anim_eval_preset)

    // SECTION 2 — ease descriptor constructors + anchor size + blend.
    MOD(animEasePreset,         "animEasePreset(type)",                         u_anim_ease_preset)
    MOD(animEaseBezier,         "animEaseBezier(x1, y1, x2, y2)",               u_anim_ease_bezier)
    MOD(animEaseSteps,          "animEaseSteps(steps, jumpMode)",               u_anim_ease_steps)
    MOD(animEaseBack,           "animEaseBack(overshoot, dir)",                 u_anim_ease_back)
    MOD(animEaseElastic,        "animEaseElastic(amplitude, period, dir)",      u_anim_ease_elastic)
    MOD(animEaseSpring,         "animEaseSpring(mass, stiffness, damping, v0)", u_anim_ease_spring)
    MOD(animEaseCustom,         "animEaseCustom(slot)",                         u_anim_ease_custom)
    MOD(animAnchorSize,         "animAnchorSize(space)",                        u_anim_anchor_size)
    MOD(animGetBlendedColor,    "animGetBlendedColor(a, b, t, space)",          u_anim_get_blended_color)

    // SECTION 3 — core tween API + rel + per-axis + rebase + scroll.
    MOD(animTweenFloat,        "animTweenFloat(id, channelId, target, dur, ease, policy, dt)",                                          u_anim_tween_float)
    MOD(animTweenInt,          "animTweenInt(id, channelId, target, dur, ease, policy, dt)",                                            u_anim_tween_int)
    MOD(animTweenVec2,         "animTweenVec2(id, channelId, target, dur, ease, policy, dt)",                                           u_anim_tween_vec2)
    MOD(animTweenVec4,         "animTweenVec4(id, channelId, target, dur, ease, policy, dt)",                                           u_anim_tween_vec4)
    MOD(animTweenColor,        "animTweenColor(id, channelId, target, dur, ease, policy, colorSpace, dt)",                              u_anim_tween_color)
    MOD(animTweenFloatRel,     "animTweenFloatRel(id, channelId, percent, pxBias, dur, ease, policy, anchorSpace, axis, dt)",           u_anim_tween_float_rel)
    MOD(animTweenVec2Rel,      "animTweenVec2Rel(id, channelId, percent, pxBias, dur, ease, policy, anchorSpace, dt)",                  u_anim_tween_vec2_rel)
    MOD(animTweenVec4Rel,      "animTweenVec4Rel(id, channelId, percent, pxBias, dur, ease, policy, anchorSpace, dt)",                  u_anim_tween_vec4_rel)
    MOD(animTweenColorRel,     "animTweenColorRel(id, channelId, percent, pxBias, dur, ease, policy, colorSpace, anchorSpace, dt)",     u_anim_tween_color_rel)
    MOD(animTweenVec2PerAxis,  "animTweenVec2PerAxis(id, channelId, target, dur, easeXY, policy, dt)",                                  u_anim_tween_vec2_per_axis)
    MOD(animTweenVec4PerAxis,  "animTweenVec4PerAxis(id, channelId, target, dur, easeXYZW, policy, dt)",                                u_anim_tween_vec4_per_axis)
    MOD(animTweenColorPerAxis, "animTweenColorPerAxis(id, channelId, target, dur, easeRGBA, policy, colorSpace, dt)",                   u_anim_tween_color_per_axis)
    MOD(animRebaseFloat,       "animRebaseFloat(id, channelId, newTarget, dt)",                                                         u_anim_rebase_float)
    MOD(animRebaseInt,         "animRebaseInt(id, channelId, newTarget, dt)",                                                           u_anim_rebase_int)
    MOD(animRebaseVec2,        "animRebaseVec2(id, channelId, newTarget, dt)",                                                          u_anim_rebase_vec2)
    MOD(animRebaseVec4,        "animRebaseVec4(id, channelId, newTarget, dt)",                                                          u_anim_rebase_vec4)
    MOD(animRebaseColor,       "animRebaseColor(id, channelId, newTarget, dt)",                                                         u_anim_rebase_color)
    MOD(animScrollToY,         "animScrollToY(targetY, dur, ease)",                                                                     u_anim_scroll_to_y)
    MOD(animScrollToX,         "animScrollToX(targetX, dur, ease)",                                                                     u_anim_scroll_to_x)
    MOD(animScrollToTop,       "animScrollToTop(dur, ease)",                                                                            u_anim_scroll_to_top)
    MOD(animScrollToBottom,    "animScrollToBottom(dur, ease)",                                                                         u_anim_scroll_to_bottom)

    // SECTION 4 — oscillate / shake / wiggle + trigger.
    MOD(animOscillate,       "animOscillate(id, amplitude, frequency, waveType, phase, dt)",                                            u_anim_oscillate)
    MOD(animOscillateInt,    "animOscillateInt(id, amplitude, frequency, waveType, phase, dt)",                                         u_anim_oscillate_int)
    MOD(animOscillateVec2,   "animOscillateVec2(id, amplitude, frequency, waveType, phase, dt)",                                        u_anim_oscillate_vec2)
    MOD(animOscillateVec4,   "animOscillateVec4(id, amplitude, frequency, waveType, phase, dt)",                                        u_anim_oscillate_vec4)
    MOD(animOscillateColor,  "animOscillateColor(id, baseColor, amplitude, frequency, waveType, phase, colorSpace, dt)",                u_anim_oscillate_color)
    MOD(animShake,           "animShake(id, intensity, frequency, decayTime, dt)",                                                      u_anim_shake)
    MOD(animShakeInt,        "animShakeInt(id, intensity, frequency, decayTime, dt)",                                                   u_anim_shake_int)
    MOD(animShakeVec2,       "animShakeVec2(id, intensity, frequency, decayTime, dt)",                                                  u_anim_shake_vec2)
    MOD(animShakeVec4,       "animShakeVec4(id, intensity, frequency, decayTime, dt)",                                                  u_anim_shake_vec4)
    MOD(animShakeColor,      "animShakeColor(id, baseColor, intensity, frequency, decayTime, colorSpace, dt)",                          u_anim_shake_color)
    MOD(animWiggle,          "animWiggle(id, amplitude, frequency, dt)",                                                                u_anim_wiggle)
    MOD(animWiggleInt,       "animWiggleInt(id, amplitude, frequency, dt)",                                                             u_anim_wiggle_int)
    MOD(animWiggleVec2,      "animWiggleVec2(id, amplitude, frequency, dt)",                                                            u_anim_wiggle_vec2)
    MOD(animWiggleVec4,      "animWiggleVec4(id, amplitude, frequency, dt)",                                                            u_anim_wiggle_vec4)
    MOD(animWiggleColor,     "animWiggleColor(id, baseColor, amplitude, frequency, colorSpace, dt)",                                    u_anim_wiggle_color)
    MOD(animTriggerShake,    "animTriggerShake(id)",                                                                                    u_anim_trigger_shake)

    // SECTION 5 — drag feedback (profiler + debug UI excluded per directive).
    MOD(animDragBegin,   "animDragBegin(id, pos)",                                                                                       u_anim_drag_begin)
    MOD(animDragUpdate,  "animDragUpdate(id, pos, dt)",                                                                                  u_anim_drag_update)
    MOD(animDragRelease, "animDragRelease(id, pos, snapGrid, snapPoints, snapDuration, overshoot, easeType, dt)",                        u_anim_drag_release)
    MOD(animDragCancel,  "animDragCancel(id)",                                                                                           u_anim_drag_cancel)

    // SECTION 6 — motion paths (curve evaluators, builder, queries,
    //              along-path tweens, arc-length parameterization).
    MOD(animBezierQuadratic,        "animBezierQuadratic(p0, p1, p2, t)",                                                                u_anim_bezier_quadratic)
    MOD(animBezierCubic,            "animBezierCubic(p0, p1, p2, p3, t)",                                                                u_anim_bezier_cubic)
    MOD(animCatmullRom,             "animCatmullRom(p0, p1, p2, p3, t, tension)",                                                        u_anim_catmull_rom)
    MOD(animBezierQuadraticDeriv,   "animBezierQuadraticDeriv(p0, p1, p2, t)",                                                           u_anim_bezier_quadratic_deriv)
    MOD(animBezierCubicDeriv,       "animBezierCubicDeriv(p0, p1, p2, p3, t)",                                                           u_anim_bezier_cubic_deriv)
    MOD(animCatmullRomDeriv,        "animCatmullRomDeriv(p0, p1, p2, p3, t, tension)",                                                   u_anim_catmull_rom_deriv)
    MOD(animPathBegin,              "animPathBegin(pathId, start)",                                                                      u_anim_path_begin)
    MOD(animPathLineTo,             "animPathLineTo(end)",                                                                               u_anim_path_line_to)
    MOD(animPathQuadraticTo,        "animPathQuadraticTo(ctrl, end)",                                                                    u_anim_path_quadratic_to)
    MOD(animPathCubicTo,            "animPathCubicTo(ctrl1, ctrl2, end)",                                                                u_anim_path_cubic_to)
    MOD(animPathCatmullTo,          "animPathCatmullTo(end, tension)",                                                                   u_anim_path_catmull_to)
    MOD(animPathClose,              "animPathClose()",                                                                                   u_anim_path_close)
    MOD(animPathEnd,                "animPathEnd()",                                                                                     u_anim_path_end)
    MOD(animPathExists,             "animPathExists(pathId)",                                                                            u_anim_path_exists)
    MOD(animPathLength,             "animPathLength(pathId)",                                                                            u_anim_path_length)
    MOD(animPathEvaluate,           "animPathEvaluate(pathId, t)",                                                                       u_anim_path_evaluate)
    MOD(animPathTangent,            "animPathTangent(pathId, t)",                                                                        u_anim_path_tangent)
    MOD(animPathAngle,              "animPathAngle(pathId, t)",                                                                          u_anim_path_angle)
    MOD(animTweenPath,              "animTweenPath(id, channelId, pathId, dur, ease, policy, dt)",                                       u_anim_tween_path)
    MOD(animTweenPathAngle,         "animTweenPathAngle(id, channelId, pathId, dur, ease, policy, dt)",                                  u_anim_tween_path_angle)
    MOD(animPathBuildArcLut,        "animPathBuildArcLut(pathId, subdivisions)",                                                         u_anim_path_build_arc_lut)
    MOD(animPathHasArcLut,          "animPathHasArcLut(pathId)",                                                                         u_anim_path_has_arc_lut)
    MOD(animPathDistanceToT,        "animPathDistanceToT(pathId, distance)",                                                             u_anim_path_distance_to_t)
    MOD(animPathEvaluateAtDistance, "animPathEvaluateAtDistance(pathId, distance)",                                                      u_anim_path_evaluate_at_distance)
    MOD(animPathAngleAtDistance,    "animPathAngleAtDistance(pathId, distance)",                                                         u_anim_path_angle_at_distance)
    MOD(animPathTangentAtDistance,  "animPathTangentAtDistance(pathId, distance)",                                                       u_anim_path_tangent_at_distance)

    // SECTION 7 — path morphing + text-along-path + quad xforms + stagger.
    MOD(animPathMorph,           "animPathMorph(pathA, pathB, t, blend, opts)",                                                          u_anim_path_morph)
    MOD(animPathMorphTangent,    "animPathMorphTangent(pathA, pathB, t, blend, opts)",                                                   u_anim_path_morph_tangent)
    MOD(animPathMorphAngle,      "animPathMorphAngle(pathA, pathB, t, blend, opts)",                                                     u_anim_path_morph_angle)
    // animTweenPathMorph — 11 conceptual params; signature parser caps at
    // 10 tokens incl. `...`, so we keep 9 fixed and pass `dt` + `opts`
    // through the variadic tail (vargs[0]=dt, vargs[1]=opts).
    ZymValue animTweenPathMorph = roots.push(zym_createNativeClosureVariadic(
        vm, "animTweenPathMorph(id, channelId, pathA, pathB, targetBlend, dur, pathEase, morphEase, policy, ...)",
        (void*)u_anim_tween_path_morph, context));
    MOD(animGetMorphBlend,       "animGetMorphBlend(id, channelId)",                                                                     u_anim_get_morph_blend)
    MOD(animTextPath,            "animTextPath(pathId, text, opts)",                                                                     u_anim_text_path)
    MOD(animTextPathAnimated,    "animTextPathAnimated(pathId, text, progress, opts)",                                                   u_anim_text_path_animated)
    MOD(animTextPathWidth,       "animTextPathWidth(text, opts)",                                                                        u_anim_text_path_width)
    MOD(animTransformQuad,       "animTransformQuad(quad, center, angleRad, translation)",                                               u_anim_transform_quad)
    MOD(animMakeGlyphQuad,       "animMakeGlyphQuad(pos, angleRad, glyphW, glyphH, baselineOffset)",                                     u_anim_make_glyph_quad)
    MOD(animTextStagger,         "animTextStagger(id, text, progress, opts)",                                                            u_anim_text_stagger)
    MOD(animTextStaggerWidth,    "animTextStaggerWidth(text, opts)",                                                                     u_anim_text_stagger_width)
    MOD(animTextStaggerDuration, "animTextStaggerDuration(text, opts)",                                                                  u_anim_text_stagger_duration)

    // SECTION 8 — noise + style/gradient/transform interp.
    MOD(animNoise2d,             "animNoise2d(x, y, opts)",                                                                              u_anim_noise_2d)
    MOD(animNoise3d,             "animNoise3d(x, y, z, opts)",                                                                           u_anim_noise_3d)
    MOD(animNoiseChannelFloat,   "animNoiseChannelFloat(id, freq, amp, opts, dt)",                                                       u_anim_noise_channel_float)
    MOD(animNoiseChannelVec2,    "animNoiseChannelVec2(id, freq, amp, opts, dt)",                                                        u_anim_noise_channel_vec2)
    MOD(animNoiseChannelVec4,    "animNoiseChannelVec4(id, freq, amp, opts, dt)",                                                        u_anim_noise_channel_vec4)
    MOD(animNoiseChannelColor,   "animNoiseChannelColor(id, baseColor, amp, freq, opts, space, dt)",                                     u_anim_noise_channel_color)
    MOD(animSmoothNoiseFloat,    "animSmoothNoiseFloat(id, amp, speed, dt)",                                                             u_anim_smooth_noise_float)
    MOD(animSmoothNoiseVec2,     "animSmoothNoiseVec2(id, amp, speed, dt)",                                                              u_anim_smooth_noise_vec2)
    MOD(animSmoothNoiseVec4,     "animSmoothNoiseVec4(id, amp, speed, dt)",                                                              u_anim_smooth_noise_vec4)
    MOD(animSmoothNoiseColor,    "animSmoothNoiseColor(id, baseColor, amp, speed, space, dt)",                                           u_anim_smooth_noise_color)
    MOD(animStyleRegisterCurrent,"animStyleRegisterCurrent(styleId)",                                                                    u_anim_style_register_current)
    MOD(animStyleBlend,          "animStyleBlend(styleA, styleB, t, colorSpace)",                                                        u_anim_style_blend)
    MOD(animStyleTween,          "animStyleTween(id, targetStyle, dur, ease, colorSpace, dt)",                                           u_anim_style_tween)
    MOD(animStyleExists,         "animStyleExists(styleId)",                                                                             u_anim_style_exists)
    MOD(animStyleUnregister,     "animStyleUnregister(styleId)",                                                                         u_anim_style_unregister)
    MOD(animGradientBegin,       "animGradientBegin(gradientId)",                                                                        u_anim_gradient_begin)
    MOD(animGradientAddStop,     "animGradientAddStop(position, color)",                                                                 u_anim_gradient_add_stop)
    MOD(animGradientEnd,         "animGradientEnd()",                                                                                    u_anim_gradient_end)
    MOD(animGradientExists,      "animGradientExists(gradientId)",                                                                       u_anim_gradient_exists)
    MOD(animGradientStopCount,   "animGradientStopCount(gradientId)",                                                                    u_anim_gradient_stop_count)
    MOD(animGradientSample,      "animGradientSample(gradientId, t, colorSpace)",                                                        u_anim_gradient_sample)
    MOD(animGradientLerp,        "animGradientLerp(gradientA, gradientB, t, colorSpace, outGradientId)",                                 u_anim_gradient_lerp)
    MOD(animTweenGradient,       "animTweenGradient(id, channelId, targetGradientId, dur, ease, policy, colorSpace, dt, outGradientId)", u_anim_tween_gradient)
    MOD(animTransformLerp,       "animTransformLerp(a, b, t, rotationMode)",                                                             u_anim_transform_lerp)
    MOD(animTweenTransform,      "animTweenTransform(id, channelId, target, dur, ease, policy, rotationMode, dt)",                       u_anim_tween_transform)
    MOD(animTransformApply,      "animTransformApply(transform, point)",                                                                 u_anim_transform_apply)
    MOD(animTransformInverse,    "animTransformInverse(transform)",                                                                      u_anim_transform_inverse)
    MOD(animTransformCompose,    "animTransformCompose(a, b)",                                                                           u_anim_transform_compose)
    MOD(animTransformFromMatrix, "animTransformFromMatrix(m00, m01, m10, m11, tx, ty)",                                                  u_anim_transform_from_matrix)
    MOD(animTransformToMatrix,   "animTransformToMatrix(transform)",                                                                     u_anim_transform_to_matrix)

    zym_mapSet(vm, obj, "animUpdateBeginFrame",  animUpdateBeginFrame);
    zym_mapSet(vm, obj, "animGc",                animGc);
    zym_mapSet(vm, obj, "animReserve",           animReserve);
    zym_mapSet(vm, obj, "animSetEaseLutSamples", animSetEaseLutSamples);
    zym_mapSet(vm, obj, "animSetGlobalTimeScale",animSetGlobalTimeScale);
    zym_mapSet(vm, obj, "animGetGlobalTimeScale",animGetGlobalTimeScale);
    zym_mapSet(vm, obj, "animSetLazyInit",       animSetLazyInit);
    zym_mapSet(vm, obj, "animIsLazyInitEnabled", animIsLazyInitEnabled);
    zym_mapSet(vm, obj, "animEvalPreset",        animEvalPreset);

    zym_mapSet(vm, obj, "animEasePreset",      animEasePreset);
    zym_mapSet(vm, obj, "animEaseBezier",      animEaseBezier);
    zym_mapSet(vm, obj, "animEaseSteps",       animEaseSteps);
    zym_mapSet(vm, obj, "animEaseBack",        animEaseBack);
    zym_mapSet(vm, obj, "animEaseElastic",     animEaseElastic);
    zym_mapSet(vm, obj, "animEaseSpring",      animEaseSpring);
    zym_mapSet(vm, obj, "animEaseCustom",      animEaseCustom);
    zym_mapSet(vm, obj, "animAnchorSize",      animAnchorSize);
    zym_mapSet(vm, obj, "animGetBlendedColor", animGetBlendedColor);

    zym_mapSet(vm, obj, "animTweenFloat",        animTweenFloat);
    zym_mapSet(vm, obj, "animTweenInt",          animTweenInt);
    zym_mapSet(vm, obj, "animTweenVec2",         animTweenVec2);
    zym_mapSet(vm, obj, "animTweenVec4",         animTweenVec4);
    zym_mapSet(vm, obj, "animTweenColor",        animTweenColor);
    zym_mapSet(vm, obj, "animTweenFloatRel",     animTweenFloatRel);
    zym_mapSet(vm, obj, "animTweenVec2Rel",      animTweenVec2Rel);
    zym_mapSet(vm, obj, "animTweenVec4Rel",      animTweenVec4Rel);
    zym_mapSet(vm, obj, "animTweenColorRel",     animTweenColorRel);
    zym_mapSet(vm, obj, "animTweenVec2PerAxis",  animTweenVec2PerAxis);
    zym_mapSet(vm, obj, "animTweenVec4PerAxis",  animTweenVec4PerAxis);
    zym_mapSet(vm, obj, "animTweenColorPerAxis", animTweenColorPerAxis);
    zym_mapSet(vm, obj, "animRebaseFloat",       animRebaseFloat);
    zym_mapSet(vm, obj, "animRebaseInt",         animRebaseInt);
    zym_mapSet(vm, obj, "animRebaseVec2",        animRebaseVec2);
    zym_mapSet(vm, obj, "animRebaseVec4",        animRebaseVec4);
    zym_mapSet(vm, obj, "animRebaseColor",       animRebaseColor);
    zym_mapSet(vm, obj, "animScrollToY",         animScrollToY);
    zym_mapSet(vm, obj, "animScrollToX",         animScrollToX);
    zym_mapSet(vm, obj, "animScrollToTop",       animScrollToTop);
    zym_mapSet(vm, obj, "animScrollToBottom",    animScrollToBottom);

    zym_mapSet(vm, obj, "animOscillate",      animOscillate);
    zym_mapSet(vm, obj, "animOscillateInt",   animOscillateInt);
    zym_mapSet(vm, obj, "animOscillateVec2",  animOscillateVec2);
    zym_mapSet(vm, obj, "animOscillateVec4",  animOscillateVec4);
    zym_mapSet(vm, obj, "animOscillateColor", animOscillateColor);
    zym_mapSet(vm, obj, "animShake",          animShake);
    zym_mapSet(vm, obj, "animShakeInt",       animShakeInt);
    zym_mapSet(vm, obj, "animShakeVec2",      animShakeVec2);
    zym_mapSet(vm, obj, "animShakeVec4",      animShakeVec4);
    zym_mapSet(vm, obj, "animShakeColor",     animShakeColor);
    zym_mapSet(vm, obj, "animWiggle",         animWiggle);
    zym_mapSet(vm, obj, "animWiggleInt",      animWiggleInt);
    zym_mapSet(vm, obj, "animWiggleVec2",     animWiggleVec2);
    zym_mapSet(vm, obj, "animWiggleVec4",     animWiggleVec4);
    zym_mapSet(vm, obj, "animWiggleColor",    animWiggleColor);
    zym_mapSet(vm, obj, "animTriggerShake",   animTriggerShake);

    zym_mapSet(vm, obj, "animDragBegin",   animDragBegin);
    zym_mapSet(vm, obj, "animDragUpdate",  animDragUpdate);
    zym_mapSet(vm, obj, "animDragRelease", animDragRelease);
    zym_mapSet(vm, obj, "animDragCancel",  animDragCancel);

    zym_mapSet(vm, obj, "animBezierQuadratic",        animBezierQuadratic);
    zym_mapSet(vm, obj, "animBezierCubic",            animBezierCubic);
    zym_mapSet(vm, obj, "animCatmullRom",             animCatmullRom);
    zym_mapSet(vm, obj, "animBezierQuadraticDeriv",   animBezierQuadraticDeriv);
    zym_mapSet(vm, obj, "animBezierCubicDeriv",       animBezierCubicDeriv);
    zym_mapSet(vm, obj, "animCatmullRomDeriv",        animCatmullRomDeriv);
    zym_mapSet(vm, obj, "animPathBegin",              animPathBegin);
    zym_mapSet(vm, obj, "animPathLineTo",             animPathLineTo);
    zym_mapSet(vm, obj, "animPathQuadraticTo",        animPathQuadraticTo);
    zym_mapSet(vm, obj, "animPathCubicTo",            animPathCubicTo);
    zym_mapSet(vm, obj, "animPathCatmullTo",          animPathCatmullTo);
    zym_mapSet(vm, obj, "animPathClose",              animPathClose);
    zym_mapSet(vm, obj, "animPathEnd",                animPathEnd);
    zym_mapSet(vm, obj, "animPathExists",             animPathExists);
    zym_mapSet(vm, obj, "animPathLength",             animPathLength);
    zym_mapSet(vm, obj, "animPathEvaluate",           animPathEvaluate);
    zym_mapSet(vm, obj, "animPathTangent",            animPathTangent);
    zym_mapSet(vm, obj, "animPathAngle",              animPathAngle);
    zym_mapSet(vm, obj, "animTweenPath",              animTweenPath);
    zym_mapSet(vm, obj, "animTweenPathAngle",         animTweenPathAngle);
    zym_mapSet(vm, obj, "animPathBuildArcLut",        animPathBuildArcLut);
    zym_mapSet(vm, obj, "animPathHasArcLut",          animPathHasArcLut);
    zym_mapSet(vm, obj, "animPathDistanceToT",        animPathDistanceToT);
    zym_mapSet(vm, obj, "animPathEvaluateAtDistance", animPathEvaluateAtDistance);
    zym_mapSet(vm, obj, "animPathAngleAtDistance",    animPathAngleAtDistance);
    zym_mapSet(vm, obj, "animPathTangentAtDistance",  animPathTangentAtDistance);

    zym_mapSet(vm, obj, "animPathMorph",           animPathMorph);
    zym_mapSet(vm, obj, "animPathMorphTangent",    animPathMorphTangent);
    zym_mapSet(vm, obj, "animPathMorphAngle",      animPathMorphAngle);
    zym_mapSet(vm, obj, "animTweenPathMorph",      animTweenPathMorph);
    zym_mapSet(vm, obj, "animGetMorphBlend",       animGetMorphBlend);
    zym_mapSet(vm, obj, "animTextPath",            animTextPath);
    zym_mapSet(vm, obj, "animTextPathAnimated",    animTextPathAnimated);
    zym_mapSet(vm, obj, "animTextPathWidth",       animTextPathWidth);
    zym_mapSet(vm, obj, "animTransformQuad",       animTransformQuad);
    zym_mapSet(vm, obj, "animMakeGlyphQuad",       animMakeGlyphQuad);
    zym_mapSet(vm, obj, "animTextStagger",         animTextStagger);
    zym_mapSet(vm, obj, "animTextStaggerWidth",    animTextStaggerWidth);
    zym_mapSet(vm, obj, "animTextStaggerDuration", animTextStaggerDuration);

    zym_mapSet(vm, obj, "animNoise2d",             animNoise2d);
    zym_mapSet(vm, obj, "animNoise3d",             animNoise3d);
    zym_mapSet(vm, obj, "animNoiseChannelFloat",   animNoiseChannelFloat);
    zym_mapSet(vm, obj, "animNoiseChannelVec2",    animNoiseChannelVec2);
    zym_mapSet(vm, obj, "animNoiseChannelVec4",    animNoiseChannelVec4);
    zym_mapSet(vm, obj, "animNoiseChannelColor",   animNoiseChannelColor);
    zym_mapSet(vm, obj, "animSmoothNoiseFloat",    animSmoothNoiseFloat);
    zym_mapSet(vm, obj, "animSmoothNoiseVec2",     animSmoothNoiseVec2);
    zym_mapSet(vm, obj, "animSmoothNoiseVec4",     animSmoothNoiseVec4);
    zym_mapSet(vm, obj, "animSmoothNoiseColor",    animSmoothNoiseColor);
    zym_mapSet(vm, obj, "animStyleRegisterCurrent",animStyleRegisterCurrent);
    zym_mapSet(vm, obj, "animStyleBlend",          animStyleBlend);
    zym_mapSet(vm, obj, "animStyleTween",          animStyleTween);
    zym_mapSet(vm, obj, "animStyleExists",         animStyleExists);
    zym_mapSet(vm, obj, "animStyleUnregister",     animStyleUnregister);
    zym_mapSet(vm, obj, "animGradientBegin",       animGradientBegin);
    zym_mapSet(vm, obj, "animGradientAddStop",     animGradientAddStop);
    zym_mapSet(vm, obj, "animGradientEnd",         animGradientEnd);
    zym_mapSet(vm, obj, "animGradientExists",      animGradientExists);
    zym_mapSet(vm, obj, "animGradientStopCount",   animGradientStopCount);
    zym_mapSet(vm, obj, "animGradientSample",      animGradientSample);
    zym_mapSet(vm, obj, "animGradientLerp",        animGradientLerp);
    zym_mapSet(vm, obj, "animTweenGradient",       animTweenGradient);
    zym_mapSet(vm, obj, "animTransformLerp",       animTransformLerp);
    zym_mapSet(vm, obj, "animTweenTransform",      animTweenTransform);
    zym_mapSet(vm, obj, "animTransformApply",      animTransformApply);
    zym_mapSet(vm, obj, "animTransformInverse",    animTransformInverse);
    zym_mapSet(vm, obj, "animTransformCompose",    animTransformCompose);
    zym_mapSet(vm, obj, "animTransformFromMatrix", animTransformFromMatrix);
    zym_mapSet(vm, obj, "animTransformToMatrix",   animTransformToMatrix);

    // ---- Enum constants ----
    //
    // Exposed as plain integers (mirrors `UI.PLOT_*` convention). All
    // ImAnim enums get a flat `UI.ANIM_*` prefix so scripts pass them
    // directly to the tween API without nested lookups.

    // --- iam_ease_type (`UI.ANIM_EASE_*`)
    zym_mapSet(vm, obj, "ANIM_EASE_LINEAR",         zym_newNumber(iam_ease_linear));
    zym_mapSet(vm, obj, "ANIM_EASE_IN_QUAD",        zym_newNumber(iam_ease_in_quad));
    zym_mapSet(vm, obj, "ANIM_EASE_OUT_QUAD",       zym_newNumber(iam_ease_out_quad));
    zym_mapSet(vm, obj, "ANIM_EASE_IN_OUT_QUAD",    zym_newNumber(iam_ease_in_out_quad));
    zym_mapSet(vm, obj, "ANIM_EASE_IN_CUBIC",       zym_newNumber(iam_ease_in_cubic));
    zym_mapSet(vm, obj, "ANIM_EASE_OUT_CUBIC",      zym_newNumber(iam_ease_out_cubic));
    zym_mapSet(vm, obj, "ANIM_EASE_IN_OUT_CUBIC",   zym_newNumber(iam_ease_in_out_cubic));
    zym_mapSet(vm, obj, "ANIM_EASE_IN_QUART",       zym_newNumber(iam_ease_in_quart));
    zym_mapSet(vm, obj, "ANIM_EASE_OUT_QUART",      zym_newNumber(iam_ease_out_quart));
    zym_mapSet(vm, obj, "ANIM_EASE_IN_OUT_QUART",   zym_newNumber(iam_ease_in_out_quart));
    zym_mapSet(vm, obj, "ANIM_EASE_IN_QUINT",       zym_newNumber(iam_ease_in_quint));
    zym_mapSet(vm, obj, "ANIM_EASE_OUT_QUINT",      zym_newNumber(iam_ease_out_quint));
    zym_mapSet(vm, obj, "ANIM_EASE_IN_OUT_QUINT",   zym_newNumber(iam_ease_in_out_quint));
    zym_mapSet(vm, obj, "ANIM_EASE_IN_SINE",        zym_newNumber(iam_ease_in_sine));
    zym_mapSet(vm, obj, "ANIM_EASE_OUT_SINE",       zym_newNumber(iam_ease_out_sine));
    zym_mapSet(vm, obj, "ANIM_EASE_IN_OUT_SINE",    zym_newNumber(iam_ease_in_out_sine));
    zym_mapSet(vm, obj, "ANIM_EASE_IN_EXPO",        zym_newNumber(iam_ease_in_expo));
    zym_mapSet(vm, obj, "ANIM_EASE_OUT_EXPO",       zym_newNumber(iam_ease_out_expo));
    zym_mapSet(vm, obj, "ANIM_EASE_IN_OUT_EXPO",    zym_newNumber(iam_ease_in_out_expo));
    zym_mapSet(vm, obj, "ANIM_EASE_IN_CIRC",        zym_newNumber(iam_ease_in_circ));
    zym_mapSet(vm, obj, "ANIM_EASE_OUT_CIRC",       zym_newNumber(iam_ease_out_circ));
    zym_mapSet(vm, obj, "ANIM_EASE_IN_OUT_CIRC",    zym_newNumber(iam_ease_in_out_circ));
    zym_mapSet(vm, obj, "ANIM_EASE_IN_BACK",        zym_newNumber(iam_ease_in_back));
    zym_mapSet(vm, obj, "ANIM_EASE_OUT_BACK",       zym_newNumber(iam_ease_out_back));
    zym_mapSet(vm, obj, "ANIM_EASE_IN_OUT_BACK",    zym_newNumber(iam_ease_in_out_back));
    zym_mapSet(vm, obj, "ANIM_EASE_IN_ELASTIC",     zym_newNumber(iam_ease_in_elastic));
    zym_mapSet(vm, obj, "ANIM_EASE_OUT_ELASTIC",    zym_newNumber(iam_ease_out_elastic));
    zym_mapSet(vm, obj, "ANIM_EASE_IN_OUT_ELASTIC", zym_newNumber(iam_ease_in_out_elastic));
    zym_mapSet(vm, obj, "ANIM_EASE_IN_BOUNCE",      zym_newNumber(iam_ease_in_bounce));
    zym_mapSet(vm, obj, "ANIM_EASE_OUT_BOUNCE",     zym_newNumber(iam_ease_out_bounce));
    zym_mapSet(vm, obj, "ANIM_EASE_IN_OUT_BOUNCE",  zym_newNumber(iam_ease_in_out_bounce));
    zym_mapSet(vm, obj, "ANIM_EASE_STEPS",          zym_newNumber(iam_ease_steps));
    zym_mapSet(vm, obj, "ANIM_EASE_CUBIC_BEZIER",   zym_newNumber(iam_ease_cubic_bezier));
    zym_mapSet(vm, obj, "ANIM_EASE_SPRING",         zym_newNumber(iam_ease_spring));
    zym_mapSet(vm, obj, "ANIM_EASE_CUSTOM",         zym_newNumber(iam_ease_custom));

    // --- iam_policy (`UI.ANIM_POLICY_*`) — crossfade/cut/queue handling
    //     when a new tween target arrives mid-animation.
    zym_mapSet(vm, obj, "ANIM_POLICY_CROSSFADE", zym_newNumber(iam_policy_crossfade));
    zym_mapSet(vm, obj, "ANIM_POLICY_CUT",       zym_newNumber(iam_policy_cut));
    zym_mapSet(vm, obj, "ANIM_POLICY_QUEUE",     zym_newNumber(iam_policy_queue));

    // --- iam_color_space (`UI.ANIM_COL_*`) — color blending space.
    zym_mapSet(vm, obj, "ANIM_COL_SRGB",        zym_newNumber(iam_col_srgb));
    zym_mapSet(vm, obj, "ANIM_COL_SRGB_LINEAR", zym_newNumber(iam_col_srgb_linear));
    zym_mapSet(vm, obj, "ANIM_COL_HSV",         zym_newNumber(iam_col_hsv));
    zym_mapSet(vm, obj, "ANIM_COL_OKLAB",       zym_newNumber(iam_col_oklab));
    zym_mapSet(vm, obj, "ANIM_COL_OKLCH",       zym_newNumber(iam_col_oklch));

    // --- iam_anchor_space (`UI.ANIM_ANCHOR_*`) — reference frame for
    //     percent-relative tween targets.
    zym_mapSet(vm, obj, "ANIM_ANCHOR_WINDOW_CONTENT", zym_newNumber(iam_anchor_window_content));
    zym_mapSet(vm, obj, "ANIM_ANCHOR_WINDOW",         zym_newNumber(iam_anchor_window));
    zym_mapSet(vm, obj, "ANIM_ANCHOR_VIEWPORT",       zym_newNumber(iam_anchor_viewport));
    zym_mapSet(vm, obj, "ANIM_ANCHOR_LAST_ITEM",      zym_newNumber(iam_anchor_last_item));

    // --- iam_wave_type (`UI.ANIM_WAVE_*`) — oscillator waveforms (§4).
    zym_mapSet(vm, obj, "ANIM_WAVE_SINE",     zym_newNumber(iam_wave_sine));
    zym_mapSet(vm, obj, "ANIM_WAVE_TRIANGLE", zym_newNumber(iam_wave_triangle));
    zym_mapSet(vm, obj, "ANIM_WAVE_SAWTOOTH", zym_newNumber(iam_wave_sawtooth));
    zym_mapSet(vm, obj, "ANIM_WAVE_SQUARE",   zym_newNumber(iam_wave_square));

    // --- iam_path_segment_type (`UI.ANIM_SEG_*`) — motion-path
    //     segment kinds (§6 — bound in a later session, but the
    //     constants ride along now so scripts referencing them
    //     compile-link in the meantime).
    zym_mapSet(vm, obj, "ANIM_SEG_LINE",             zym_newNumber(iam_seg_line));
    zym_mapSet(vm, obj, "ANIM_SEG_QUADRATIC_BEZIER", zym_newNumber(iam_seg_quadratic_bezier));
    zym_mapSet(vm, obj, "ANIM_SEG_CUBIC_BEZIER",     zym_newNumber(iam_seg_cubic_bezier));
    zym_mapSet(vm, obj, "ANIM_SEG_CATMULL_ROM",      zym_newNumber(iam_seg_catmull_rom));

    // --- iam_text_path_align (`UI.ANIM_TEXT_ALIGN_*`) — §7 text-along-path.
    zym_mapSet(vm, obj, "ANIM_TEXT_ALIGN_START",  zym_newNumber(iam_text_align_start));
    zym_mapSet(vm, obj, "ANIM_TEXT_ALIGN_CENTER", zym_newNumber(iam_text_align_center));
    zym_mapSet(vm, obj, "ANIM_TEXT_ALIGN_END",    zym_newNumber(iam_text_align_end));

    // --- iam_text_stagger_effect (`UI.ANIM_TEXT_FX_*`) — §7 per-char effects.
    zym_mapSet(vm, obj, "ANIM_TEXT_FX_NONE",        zym_newNumber(iam_text_fx_none));
    zym_mapSet(vm, obj, "ANIM_TEXT_FX_FADE",        zym_newNumber(iam_text_fx_fade));
    zym_mapSet(vm, obj, "ANIM_TEXT_FX_SCALE",       zym_newNumber(iam_text_fx_scale));
    zym_mapSet(vm, obj, "ANIM_TEXT_FX_SLIDE_UP",    zym_newNumber(iam_text_fx_slide_up));
    zym_mapSet(vm, obj, "ANIM_TEXT_FX_SLIDE_DOWN",  zym_newNumber(iam_text_fx_slide_down));
    zym_mapSet(vm, obj, "ANIM_TEXT_FX_SLIDE_LEFT",  zym_newNumber(iam_text_fx_slide_left));
    zym_mapSet(vm, obj, "ANIM_TEXT_FX_SLIDE_RIGHT", zym_newNumber(iam_text_fx_slide_right));
    zym_mapSet(vm, obj, "ANIM_TEXT_FX_ROTATE",      zym_newNumber(iam_text_fx_rotate));
    zym_mapSet(vm, obj, "ANIM_TEXT_FX_BOUNCE",      zym_newNumber(iam_text_fx_bounce));
    zym_mapSet(vm, obj, "ANIM_TEXT_FX_WAVE",        zym_newNumber(iam_text_fx_wave));
    zym_mapSet(vm, obj, "ANIM_TEXT_FX_TYPEWRITER",  zym_newNumber(iam_text_fx_typewriter));

    // --- iam_noise_type (`UI.ANIM_NOISE_*`) — §8 noise channel types.
    zym_mapSet(vm, obj, "ANIM_NOISE_PERLIN",  zym_newNumber(iam_noise_perlin));
    zym_mapSet(vm, obj, "ANIM_NOISE_SIMPLEX", zym_newNumber(iam_noise_simplex));
    zym_mapSet(vm, obj, "ANIM_NOISE_VALUE",   zym_newNumber(iam_noise_value));
    zym_mapSet(vm, obj, "ANIM_NOISE_WORLEY",  zym_newNumber(iam_noise_worley));

    // --- iam_rotation_mode (`UI.ANIM_ROTATION_*`) — §8 transform rotation.
    zym_mapSet(vm, obj, "ANIM_ROTATION_SHORTEST", zym_newNumber(iam_rotation_shortest));
    zym_mapSet(vm, obj, "ANIM_ROTATION_LONGEST",  zym_newNumber(iam_rotation_longest));
    zym_mapSet(vm, obj, "ANIM_ROTATION_CW",       zym_newNumber(iam_rotation_cw));
    zym_mapSet(vm, obj, "ANIM_ROTATION_CCW",      zym_newNumber(iam_rotation_ccw));
    zym_mapSet(vm, obj, "ANIM_ROTATION_DIRECT",   zym_newNumber(iam_rotation_direct));

    // ====================================================================
    // SECTION 9 — clip authoring + instance ops + clip-system globals
    //              + layering + persistence + frame-update opt-in flag.
    // ====================================================================

    // Frame-update opt-in: scripts can disable the auto-driver in
    // `ui.cpp::u_frame` and call these themselves for scrub / pause /
    // multi-pass control.
    MOD(animSetAutoFrameUpdate,       "animSetAutoFrameUpdate(enable)",       u_anim_set_auto_frame_update)
    MOD(animIsAutoFrameUpdateEnabled, "animIsAutoFrameUpdateEnabled()",       u_anim_is_auto_frame_update_enabled)
    MOD(animClipUpdate,               "animClipUpdate(dt)",                   u_anim_clip_update)

    // Variation descriptor constructors (data-only modes; callback mode
    // deferred — see TODO at top of file).
    MOD(animVarFloat, "animVarFloat(mode, amount, minClamp, maxClamp, seed)",                                      u_anim_var_float)
    MOD(animVarInt,   "animVarInt(mode, amount, minClamp, maxClamp, seed)",                                        u_anim_var_int)
    MOD(animVarVec2,  "animVarVec2(mode, amount, minClamp, maxClamp, seed)",                                       u_anim_var_vec2)
    MOD(animVarVec4,  "animVarVec4(mode, amount, minClamp, maxClamp, seed)",                                       u_anim_var_vec4)
    MOD(animVarColor, "animVarColor(mode, amount, minClamp, maxClamp, colorSpace, seed)",                          u_anim_var_color)

    // Clip authoring (active-clip singleton, mirrors §6 path builder).
    MOD(animClipBegin,            "animClipBegin(clipId)",                                                                                                       u_anim_clip_begin)
    MOD(animClipEnd,              "animClipEnd()",                                                                                                               u_anim_clip_end)
    MOD(animClipKeyFloat,         "animClipKeyFloat(channelId, time, value, easeType, bezier4)",                                                                 u_anim_clip_key_float)
    MOD(animClipKeyInt,           "animClipKeyInt(channelId, time, value, easeType)",                                                                            u_anim_clip_key_int)
    MOD(animClipKeyVec2,          "animClipKeyVec2(channelId, time, value, easeType, bezier4)",                                                                  u_anim_clip_key_vec2)
    MOD(animClipKeyVec4,          "animClipKeyVec4(channelId, time, value, easeType, bezier4)",                                                                  u_anim_clip_key_vec4)
    MOD(animClipKeyColor,         "animClipKeyColor(channelId, time, value, colorSpace, easeType, bezier4)",                                                     u_anim_clip_key_color)
    MOD(animClipKeyFloatSpring,   "animClipKeyFloatSpring(channelId, time, target, mass, stiffness, damping, v0)",                                               u_anim_clip_key_float_spring)
    MOD(animClipKeyFloatVar,      "animClipKeyFloatVar(channelId, time, value, varFloat, easeType, bezier4)",                                                    u_anim_clip_key_float_var)
    MOD(animClipKeyIntVar,        "animClipKeyIntVar(channelId, time, value, varInt, easeType)",                                                                 u_anim_clip_key_int_var)
    MOD(animClipKeyVec2Var,       "animClipKeyVec2Var(channelId, time, value, varVec2, easeType, bezier4)",                                                      u_anim_clip_key_vec2_var)
    MOD(animClipKeyVec4Var,       "animClipKeyVec4Var(channelId, time, value, varVec4, easeType, bezier4)",                                                      u_anim_clip_key_vec4_var)
    MOD(animClipKeyColorVar,      "animClipKeyColorVar(channelId, time, value, varColor, colorSpace, easeType, bezier4)",                                        u_anim_clip_key_color_var)
    MOD(animClipKeyFloatRel,      "animClipKeyFloatRel(channelId, time, percent, pxBias, anchorSpace, axis, easeType, bezier4)",                                 u_anim_clip_key_float_rel)
    MOD(animClipKeyVec2Rel,       "animClipKeyVec2Rel(channelId, time, percent, pxBias, anchorSpace, easeType, bezier4)",                                        u_anim_clip_key_vec2_rel)
    MOD(animClipKeyVec4Rel,       "animClipKeyVec4Rel(channelId, time, percent, pxBias, anchorSpace, easeType, bezier4)",                                        u_anim_clip_key_vec4_rel)
    MOD(animClipKeyColorRel,      "animClipKeyColorRel(channelId, time, percent, pxBias, colorSpace, anchorSpace, easeType, bezier4)",                           u_anim_clip_key_color_rel)
    MOD(animClipSeqBegin,         "animClipSeqBegin()",                                                                                                          u_anim_clip_seq_begin)
    MOD(animClipSeqEnd,           "animClipSeqEnd()",                                                                                                            u_anim_clip_seq_end)
    MOD(animClipParBegin,         "animClipParBegin()",                                                                                                          u_anim_clip_par_begin)
    MOD(animClipParEnd,           "animClipParEnd()",                                                                                                            u_anim_clip_par_end)
    MOD(animClipSetLoop,          "animClipSetLoop(loop, direction, loopCount)",                                                                                 u_anim_clip_set_loop)
    MOD(animClipSetDelay,         "animClipSetDelay(delaySeconds)",                                                                                              u_anim_clip_set_delay)
    MOD(animClipSetStagger,       "animClipSetStagger(count, eachDelay, fromCenterBias)",                                                                        u_anim_clip_set_stagger)
    MOD(animClipSetDurationVar,   "animClipSetDurationVar(varFloat)",                                                                                            u_anim_clip_set_duration_var)
    MOD(animClipSetDelayVar,      "animClipSetDelayVar(varFloat)",                                                                                               u_anim_clip_set_delay_var)
    MOD(animClipSetTimescaleVar,  "animClipSetTimescaleVar(varFloat)",                                                                                           u_anim_clip_set_timescale_var)

    // Clip-system globals.
    MOD(animClipInit,        "animClipInit(initialClipCap, initialInstCap)",                                                                                     u_anim_clip_init)
    MOD(animClipShutdown,    "animClipShutdown()",                                                                                                               u_anim_clip_shutdown)
    MOD(animClipGc,          "animClipGc(maxAgeFrames)",                                                                                                         u_anim_clip_gc)
    MOD(animPlay,            "animPlay(clipId, instanceId)",                                                                                                     u_anim_play)
    MOD(animGetInstance,     "animGetInstance(instanceId)",                                                                                                      u_anim_get_instance)
    MOD(animClipDuration,    "animClipDuration(clipId)",                                                                                                         u_anim_clip_duration)
    MOD(animClipExists,      "animClipExists(clipId)",                                                                                                           u_anim_clip_exists)
    MOD(animStaggerDelay,    "animStaggerDelay(clipId, index)",                                                                                                  u_anim_stagger_delay)
    MOD(animPlayStagger,     "animPlayStagger(clipId, instanceId, index)",                                                                                       u_anim_play_stagger)

    // Instance ops.
    MOD(animInstancePause,        "animInstancePause(id)",                                                                                                       u_anim_instance_pause)
    MOD(animInstanceResume,       "animInstanceResume(id)",                                                                                                      u_anim_instance_resume)
    MOD(animInstanceStop,         "animInstanceStop(id)",                                                                                                        u_anim_instance_stop)
    MOD(animInstanceDestroy,      "animInstanceDestroy(id)",                                                                                                     u_anim_instance_destroy)
    MOD(animInstanceSeek,         "animInstanceSeek(id, time)",                                                                                                  u_anim_instance_seek)
    MOD(animInstanceSetTimeScale, "animInstanceSetTimeScale(id, scale)",                                                                                         u_anim_instance_set_time_scale)
    MOD(animInstanceSetWeight,    "animInstanceSetWeight(id, weight)",                                                                                           u_anim_instance_set_weight)
    MOD(animInstanceThen,         "animInstanceThen(id, nextClipId, nextInstanceId)",                                                                            u_anim_instance_then)
    MOD(animInstanceThenDelay,    "animInstanceThenDelay(id, delay)",                                                                                            u_anim_instance_then_delay)
    MOD(animInstanceTime,         "animInstanceTime(id)",                                                                                                        u_anim_instance_time)
    MOD(animInstanceDuration,     "animInstanceDuration(id)",                                                                                                    u_anim_instance_duration)
    MOD(animInstanceIsPlaying,    "animInstanceIsPlaying(id)",                                                                                                   u_anim_instance_is_playing)
    MOD(animInstanceIsPaused,     "animInstanceIsPaused(id)",                                                                                                    u_anim_instance_is_paused)
    MOD(animInstanceValid,        "animInstanceValid(id)",                                                                                                       u_anim_instance_valid)
    MOD(animInstanceGetFloat,     "animInstanceGetFloat(id, channelId)",                                                                                         u_anim_instance_get_float)
    MOD(animInstanceGetVec2,      "animInstanceGetVec2(id, channelId)",                                                                                          u_anim_instance_get_vec2)
    MOD(animInstanceGetVec4,      "animInstanceGetVec4(id, channelId)",                                                                                          u_anim_instance_get_vec4)
    MOD(animInstanceGetInt,       "animInstanceGetInt(id, channelId)",                                                                                           u_anim_instance_get_int)
    MOD(animInstanceGetColor,     "animInstanceGetColor(id, channelId, colorSpace)",                                                                             u_anim_instance_get_color)

    // Layering.
    MOD(animLayerBegin,        "animLayerBegin(targetInstanceId)",                                                                                               u_anim_layer_begin)
    MOD(animLayerAdd,          "animLayerAdd(srcInstanceId, weight)",                                                                                            u_anim_layer_add)
    MOD(animLayerEnd,          "animLayerEnd(targetInstanceId)",                                                                                                 u_anim_layer_end)
    MOD(animGetBlendedFloat,   "animGetBlendedFloat(instanceId, channelId)",                                                                                     u_anim_get_blended_float)
    MOD(animGetBlendedVec2,    "animGetBlendedVec2(instanceId, channelId)",                                                                                      u_anim_get_blended_vec2)
    MOD(animGetBlendedVec4,    "animGetBlendedVec4(instanceId, channelId)",                                                                                      u_anim_get_blended_vec4)
    MOD(animGetBlendedInt,     "animGetBlendedInt(instanceId, channelId)",                                                                                       u_anim_get_blended_int)

    // Persistence.
    MOD(animClipSave, "animClipSave(clipId, path)",   u_anim_clip_save)
    MOD(animClipLoad, "animClipLoad(path)",           u_anim_clip_load)

#undef MOD

    // ---- §9 zym_mapSet block ----
    zym_mapSet(vm, obj, "animSetAutoFrameUpdate",       animSetAutoFrameUpdate);
    zym_mapSet(vm, obj, "animIsAutoFrameUpdateEnabled", animIsAutoFrameUpdateEnabled);
    zym_mapSet(vm, obj, "animClipUpdate",               animClipUpdate);

    zym_mapSet(vm, obj, "animVarFloat", animVarFloat);
    zym_mapSet(vm, obj, "animVarInt",   animVarInt);
    zym_mapSet(vm, obj, "animVarVec2",  animVarVec2);
    zym_mapSet(vm, obj, "animVarVec4",  animVarVec4);
    zym_mapSet(vm, obj, "animVarColor", animVarColor);

    zym_mapSet(vm, obj, "animClipBegin",           animClipBegin);
    zym_mapSet(vm, obj, "animClipEnd",             animClipEnd);
    zym_mapSet(vm, obj, "animClipKeyFloat",        animClipKeyFloat);
    zym_mapSet(vm, obj, "animClipKeyInt",          animClipKeyInt);
    zym_mapSet(vm, obj, "animClipKeyVec2",         animClipKeyVec2);
    zym_mapSet(vm, obj, "animClipKeyVec4",         animClipKeyVec4);
    zym_mapSet(vm, obj, "animClipKeyColor",        animClipKeyColor);
    zym_mapSet(vm, obj, "animClipKeyFloatSpring",  animClipKeyFloatSpring);
    zym_mapSet(vm, obj, "animClipKeyFloatVar",     animClipKeyFloatVar);
    zym_mapSet(vm, obj, "animClipKeyIntVar",       animClipKeyIntVar);
    zym_mapSet(vm, obj, "animClipKeyVec2Var",      animClipKeyVec2Var);
    zym_mapSet(vm, obj, "animClipKeyVec4Var",      animClipKeyVec4Var);
    zym_mapSet(vm, obj, "animClipKeyColorVar",     animClipKeyColorVar);
    zym_mapSet(vm, obj, "animClipKeyFloatRel",     animClipKeyFloatRel);
    zym_mapSet(vm, obj, "animClipKeyVec2Rel",      animClipKeyVec2Rel);
    zym_mapSet(vm, obj, "animClipKeyVec4Rel",      animClipKeyVec4Rel);
    zym_mapSet(vm, obj, "animClipKeyColorRel",     animClipKeyColorRel);
    zym_mapSet(vm, obj, "animClipSeqBegin",        animClipSeqBegin);
    zym_mapSet(vm, obj, "animClipSeqEnd",          animClipSeqEnd);
    zym_mapSet(vm, obj, "animClipParBegin",        animClipParBegin);
    zym_mapSet(vm, obj, "animClipParEnd",          animClipParEnd);
    zym_mapSet(vm, obj, "animClipSetLoop",         animClipSetLoop);
    zym_mapSet(vm, obj, "animClipSetDelay",        animClipSetDelay);
    zym_mapSet(vm, obj, "animClipSetStagger",      animClipSetStagger);
    zym_mapSet(vm, obj, "animClipSetDurationVar",  animClipSetDurationVar);
    zym_mapSet(vm, obj, "animClipSetDelayVar",     animClipSetDelayVar);
    zym_mapSet(vm, obj, "animClipSetTimescaleVar", animClipSetTimescaleVar);

    zym_mapSet(vm, obj, "animClipInit",     animClipInit);
    zym_mapSet(vm, obj, "animClipShutdown", animClipShutdown);
    zym_mapSet(vm, obj, "animClipGc",       animClipGc);
    zym_mapSet(vm, obj, "animPlay",         animPlay);
    zym_mapSet(vm, obj, "animGetInstance",  animGetInstance);
    zym_mapSet(vm, obj, "animClipDuration", animClipDuration);
    zym_mapSet(vm, obj, "animClipExists",   animClipExists);
    zym_mapSet(vm, obj, "animStaggerDelay", animStaggerDelay);
    zym_mapSet(vm, obj, "animPlayStagger",  animPlayStagger);

    zym_mapSet(vm, obj, "animInstancePause",        animInstancePause);
    zym_mapSet(vm, obj, "animInstanceResume",       animInstanceResume);
    zym_mapSet(vm, obj, "animInstanceStop",         animInstanceStop);
    zym_mapSet(vm, obj, "animInstanceDestroy",      animInstanceDestroy);
    zym_mapSet(vm, obj, "animInstanceSeek",         animInstanceSeek);
    zym_mapSet(vm, obj, "animInstanceSetTimeScale", animInstanceSetTimeScale);
    zym_mapSet(vm, obj, "animInstanceSetWeight",    animInstanceSetWeight);
    zym_mapSet(vm, obj, "animInstanceThen",         animInstanceThen);
    zym_mapSet(vm, obj, "animInstanceThenDelay",    animInstanceThenDelay);
    zym_mapSet(vm, obj, "animInstanceTime",         animInstanceTime);
    zym_mapSet(vm, obj, "animInstanceDuration",     animInstanceDuration);
    zym_mapSet(vm, obj, "animInstanceIsPlaying",    animInstanceIsPlaying);
    zym_mapSet(vm, obj, "animInstanceIsPaused",     animInstanceIsPaused);
    zym_mapSet(vm, obj, "animInstanceValid",        animInstanceValid);
    zym_mapSet(vm, obj, "animInstanceGetFloat",     animInstanceGetFloat);
    zym_mapSet(vm, obj, "animInstanceGetVec2",      animInstanceGetVec2);
    zym_mapSet(vm, obj, "animInstanceGetVec4",      animInstanceGetVec4);
    zym_mapSet(vm, obj, "animInstanceGetInt",       animInstanceGetInt);
    zym_mapSet(vm, obj, "animInstanceGetColor",     animInstanceGetColor);

    zym_mapSet(vm, obj, "animLayerBegin",      animLayerBegin);
    zym_mapSet(vm, obj, "animLayerAdd",        animLayerAdd);
    zym_mapSet(vm, obj, "animLayerEnd",        animLayerEnd);
    zym_mapSet(vm, obj, "animGetBlendedFloat", animGetBlendedFloat);
    zym_mapSet(vm, obj, "animGetBlendedVec2",  animGetBlendedVec2);
    zym_mapSet(vm, obj, "animGetBlendedVec4",  animGetBlendedVec4);
    zym_mapSet(vm, obj, "animGetBlendedInt",   animGetBlendedInt);

    zym_mapSet(vm, obj, "animClipSave", animClipSave);
    zym_mapSet(vm, obj, "animClipLoad", animClipLoad);

    // ---- §9 enum constants ----
    // iam_variation_mode — pure-data variation modes for `animVar*`.
    zym_mapSet(vm, obj, "ANIM_VAR_NONE",       zym_newNumber(iam_var_none));
    zym_mapSet(vm, obj, "ANIM_VAR_INCREMENT",  zym_newNumber(iam_var_increment));
    zym_mapSet(vm, obj, "ANIM_VAR_DECREMENT",  zym_newNumber(iam_var_decrement));
    zym_mapSet(vm, obj, "ANIM_VAR_MULTIPLY",   zym_newNumber(iam_var_multiply));
    zym_mapSet(vm, obj, "ANIM_VAR_RANDOM",     zym_newNumber(iam_var_random));
    zym_mapSet(vm, obj, "ANIM_VAR_RANDOM_ABS", zym_newNumber(iam_var_random_abs));
    zym_mapSet(vm, obj, "ANIM_VAR_PINGPONG",   zym_newNumber(iam_var_pingpong));
    zym_mapSet(vm, obj, "ANIM_VAR_CALLBACK",   zym_newNumber(iam_var_callback));

    // iam_dir — playback direction for `animClipSetLoop`.
    zym_mapSet(vm, obj, "ANIM_DIR_NORMAL",   zym_newNumber(iam_dir_normal));
    zym_mapSet(vm, obj, "ANIM_DIR_REVERSE",  zym_newNumber(iam_dir_reverse));
    zym_mapSet(vm, obj, "ANIM_DIR_ALTERNATE", zym_newNumber(iam_dir_alternate));

    // iam_result — return codes from `animClipSave` / `animClipLoad`.
    zym_mapSet(vm, obj, "ANIM_OK",            zym_newNumber(iam_ok));
    zym_mapSet(vm, obj, "ANIM_ERR_NOT_FOUND", zym_newNumber(iam_err_not_found));
    zym_mapSet(vm, obj, "ANIM_ERR_BAD_ARG",   zym_newNumber(iam_err_bad_arg));
    zym_mapSet(vm, obj, "ANIM_ERR_NO_MEM",    zym_newNumber(iam_err_no_mem));
}
