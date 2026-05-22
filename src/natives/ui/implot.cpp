// implot.cpp — every `u_plot*` ImPlot wrapper + the
// `registerImPlotBindings` function called from `ui.cpp`'s
// `nativeUi_create`.
//
// Compiled only when ZYM_UI_ENABLED is defined.
//
// Section progress (each section is its own self-contained landing,
// build-verified before the next one lands):
//   [x] 1. Enums + Context + BeginPlot/EndPlot scaffolding
//   [x] 2. Setup* (axes, axis limits, axis ticks, axis format)
//   [x] 3. Plot items
//        [x] 3a. Line/Scatter/Stairs/Shaded/Bars/BarGroups/Stems/InfLines/Dummy
//        [x] 3b. Histogram/Histogram2D/PieChart/Heatmap/ErrorBars/Digital
//        [x] 3c. Image/Text  <-- this turn
//   [ ] 4. Tools (DragPoint/DragLine/DragRect/Annotation/Tag)
//   [ ] 5. Style stacks + Colormaps + Legend
//   [ ] 6. Subplots + AlignedPlots
//   [ ] 7. Queries (IsPlotHovered, GetPlotMousePos, GetPlotLimits, ...)
//
// The ImPlot per-window context lifecycle is wired in `ui.cpp`
// (ensureWindowContext / destroyUiContext / u_frame's SetCurrentContext),
// so the scaffolding here only needs the script-facing surface.

#include "ui_internal.hpp"

#include "implot.h"

namespace {

// ==== SECTION 1: Context + BeginPlot/EndPlot scope ========================

// Frame-context guard for plot-only calls. Every `u_plot*` native that
// must be inside `BeginPlot/EndPlot` calls `requirePlot()`; calls that
// only need an active ImGui frame use the existing `requireFrame`.
//
// `BeginPlot` itself doesn't need this — it only needs an ImGui frame —
// but everything that draws into a plot does. ImPlot exposes
// `ImPlot::GetCurrentContext()` for the context-level check; the
// plot-currently-open check requires peeking at the internal state,
// which we do via `ImPlot::GetPlotPos()` returning a sentinel only when
// inside `BeginPlot/EndPlot`. Simpler: we route through a single
// per-frame flag managed by `u_plot`.
//
// NOTE: kept here (and not in ui_internal.hpp) because only the ImPlot
// wrappers need it. ImGui wrappers don't.
thread_local bool g_inPlot = false;

inline bool requirePlot(ZymVM* vm, const char* where) {
    if (!requireFrame(vm, where)) return false;
    if (!g_inPlot) {
        zym_runtimeError(vm, "%s: called outside ui.plot(...)", where);
        return false;
    }
    return true;
}

// `ui.plot(title, body) -> bool` — wraps `BeginPlot/EndPlot` with the
// default size (-1, 0) and no flags. Mirrors `ui.window`'s contract:
// the bool matches `BeginPlot`'s return (plot opened); `body` runs only
// when the bool is true; `EndPlot` is still called in either case (per
// ImPlot's API contract that `EndPlot` must be called when `BeginPlot`
// returns true — but unlike ImGui, it must NOT be called when it
// returns false).
ZymValue u_plot(ZymVM* vm, ZymValue /*self*/, ZymValue titleV, ZymValue bodyV) {
    std::string title;
    if (!reqStr(vm, titleV, "ui.plot(title, body)", &title)) return ZYM_ERROR;
    if (!reqCallable(vm, bodyV, "ui.plot(title, body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.plot")) return ZYM_ERROR;

    bool open = ImPlot::BeginPlot(title.c_str(), ImVec2(-1, 0), 0);
    if (open) {
        zym_pushRoot(vm, bodyV);
        bool prev = g_inPlot;
        g_inPlot = true;
        ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
        g_inPlot = prev;
        zym_popRoot(vm);
        ImPlot::EndPlot();
        if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    }
    return zym_newBool(open);
}

// `ui.plot(title, w, h, body) -> bool` — explicit size overload.
ZymValue u_plotSized(ZymVM* vm, ZymValue /*self*/, ZymValue titleV,
                    ZymValue wV, ZymValue hV, ZymValue bodyV) {
    std::string title;
    if (!reqStr(vm, titleV, "ui.plot(title, w, h, body)", &title)) return ZYM_ERROR;
    double w, h;
    if (!reqNum(vm, wV, "ui.plot(title, w, h, body)", &w)) return ZYM_ERROR;
    if (!reqNum(vm, hV, "ui.plot(title, w, h, body)", &h)) return ZYM_ERROR;
    if (!reqCallable(vm, bodyV, "ui.plot(title, w, h, body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.plot")) return ZYM_ERROR;

    bool open = ImPlot::BeginPlot(title.c_str(), ImVec2((float)w, (float)h), 0);
    if (open) {
        zym_pushRoot(vm, bodyV);
        bool prev = g_inPlot;
        g_inPlot = true;
        ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
        g_inPlot = prev;
        zym_popRoot(vm);
        ImPlot::EndPlot();
        if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    }
    return zym_newBool(open);
}

// `ui.plot(title, w, h, flags, body) -> bool` — full overload with
// ImPlotFlags bitmask (use the `UI.PLOT_*` constants).
ZymValue u_plotFlags(ZymVM* vm, ZymValue /*self*/, ZymValue titleV,
                     ZymValue wV, ZymValue hV, ZymValue flagsV,
                     ZymValue bodyV) {
    std::string title;
    if (!reqStr(vm, titleV, "ui.plot(title, w, h, flags, body)", &title)) return ZYM_ERROR;
    double w, h;
    if (!reqNum(vm, wV, "ui.plot(title, w, h, flags, body)", &w)) return ZYM_ERROR;
    if (!reqNum(vm, hV, "ui.plot(title, w, h, flags, body)", &h)) return ZYM_ERROR;
    int flags;
    if (!reqInt(vm, flagsV, "ui.plot(title, w, h, flags, body)", &flags)) return ZYM_ERROR;
    if (!reqCallable(vm, bodyV, "ui.plot(title, w, h, flags, body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.plot")) return ZYM_ERROR;

    bool open = ImPlot::BeginPlot(title.c_str(),
                                  ImVec2((float)w, (float)h),
                                  (ImPlotFlags)flags);
    if (open) {
        zym_pushRoot(vm, bodyV);
        bool prev = g_inPlot;
        g_inPlot = true;
        ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
        g_inPlot = prev;
        zym_popRoot(vm);
        ImPlot::EndPlot();
        if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    }
    return zym_newBool(open);
}

// `ui.isInPlot() -> bool` — script-side query mirroring `g_inPlot`.
// Useful for libraries that want to no-op when called from outside a
// plot scope without erroring out.
ZymValue u_isInPlot(ZymVM* /*vm*/, ZymValue /*self*/) {
    return zym_newBool(g_inPlot);
}

// ==== SECTION 2: Setup* + Set(Next)Axis*/Set(Next)Axes* ===================
//
// Every Setup* call must be issued INSIDE a `ui.plot(...)` body (between
// BeginPlot and the first plot-item call). The SetAxis/SetAxes calls
// likewise require an active plot; SetNextAxis*/SetNextAxes* are issued
// OUTSIDE the plot body (before the next ui.plot call) per ImPlot's
// contract, but still need an active ImGui frame.

// --- SetupAxis(axis, label?, flags?) -----
ZymValue u_plotSetupAxis(ZymVM* vm, ZymValue, ZymValue axisV, ZymValue labelV, ZymValue flagsV) {
    int axis; if (!reqInt(vm, axisV, "ui.plotSetupAxis(axis, label?, flags?)", &axis)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotSetupAxis")) return ZYM_ERROR;
    const char* label = optStr(labelV, nullptr);
    int flags = optInt(flagsV, 0);
    ImPlot::SetupAxis((ImAxis)axis, label, (ImPlotAxisFlags)flags);
    return zym_newNull();
}

// --- SetupAxes(xLabel, yLabel, xFlags?, yFlags?) -----
ZymValue u_plotSetupAxes(ZymVM* vm, ZymValue, ZymValue xLabelV, ZymValue yLabelV,
                         ZymValue xFlagsV, ZymValue yFlagsV) {
    if (!requirePlot(vm, "ui.plotSetupAxes")) return ZYM_ERROR;
    const char* xL = optStr(xLabelV, nullptr);
    const char* yL = optStr(yLabelV, nullptr);
    int xf = optInt(xFlagsV, 0);
    int yf = optInt(yFlagsV, 0);
    ImPlot::SetupAxes(xL, yL, (ImPlotAxisFlags)xf, (ImPlotAxisFlags)yf);
    return zym_newNull();
}

// --- SetupAxisLimits(axis, vMin, vMax, cond?) -----
ZymValue u_plotSetupAxisLimits(ZymVM* vm, ZymValue, ZymValue axisV, ZymValue vMinV,
                               ZymValue vMaxV, ZymValue condV) {
    int axis; if (!reqInt(vm, axisV, "ui.plotSetupAxisLimits", &axis)) return ZYM_ERROR;
    double vMin, vMax;
    if (!reqNum(vm, vMinV, "ui.plotSetupAxisLimits", &vMin)) return ZYM_ERROR;
    if (!reqNum(vm, vMaxV, "ui.plotSetupAxisLimits", &vMax)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotSetupAxisLimits")) return ZYM_ERROR;
    int cond = optInt(condV, ImPlotCond_Once);
    ImPlot::SetupAxisLimits((ImAxis)axis, vMin, vMax, (ImPlotCond)cond);
    return zym_newNull();
}

// --- SetupAxesLimits(xMin, xMax, yMin, yMax, cond?) -----
ZymValue u_plotSetupAxesLimits(ZymVM* vm, ZymValue, ZymValue xMinV, ZymValue xMaxV,
                               ZymValue yMinV, ZymValue yMaxV, ZymValue condV) {
    double xMin, xMax, yMin, yMax;
    if (!reqNum(vm, xMinV, "ui.plotSetupAxesLimits", &xMin)) return ZYM_ERROR;
    if (!reqNum(vm, xMaxV, "ui.plotSetupAxesLimits", &xMax)) return ZYM_ERROR;
    if (!reqNum(vm, yMinV, "ui.plotSetupAxesLimits", &yMin)) return ZYM_ERROR;
    if (!reqNum(vm, yMaxV, "ui.plotSetupAxesLimits", &yMax)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotSetupAxesLimits")) return ZYM_ERROR;
    int cond = optInt(condV, ImPlotCond_Once);
    ImPlot::SetupAxesLimits(xMin, xMax, yMin, yMax, (ImPlotCond)cond);
    return zym_newNull();
}

// --- SetupAxisLinks(axis, ref) -----
// `ref` is a 2-element list [min, max] of numbers. ImPlot writes through
// the pointers on pan/zoom; we read the list into a per-call pair of
// doubles, hand the addresses to ImPlot, then write back at the end of
// the call. NOTE: this means the link only fires for the duration of
// this call — for true persistent linking, the script must call
// SetupAxisLinks every frame (matches how `ref`-cell widgets work).
ZymValue u_plotSetupAxisLinks(ZymVM* vm, ZymValue, ZymValue axisV, ZymValue refV) {
    int axis; if (!reqInt(vm, axisV, "ui.plotSetupAxisLinks(axis, [min,max])", &axis)) return ZYM_ERROR;
    if (!zym_isList(refV) || zym_listLength(refV) < 2) {
        zym_runtimeError(vm, "ui.plotSetupAxisLinks: ref must be a 2-element list [min, max]");
        return ZYM_ERROR;
    }
    if (!requirePlot(vm, "ui.plotSetupAxisLinks")) return ZYM_ERROR;
    ZymValue v0 = zym_listGet(vm, refV, 0);
    ZymValue v1 = zym_listGet(vm, refV, 1);
    if (!zym_isNumber(v0) || !zym_isNumber(v1)) {
        zym_runtimeError(vm, "ui.plotSetupAxisLinks: ref elements must be numbers");
        return ZYM_ERROR;
    }
    double lo = zym_asNumber(v0);
    double hi = zym_asNumber(v1);
    ImPlot::SetupAxisLinks((ImAxis)axis, &lo, &hi);
    // ImPlot may have mutated lo/hi during Setup (axis already had links
    // committed earlier this frame); write back so the script sees them.
    zym_listSet(vm, refV, 0, zym_newNumber(lo));
    zym_listSet(vm, refV, 1, zym_newNumber(hi));
    return zym_newNull();
}

// --- SetupAxisFormat(axis, fmt) — string-form only (printf-style) -----
ZymValue u_plotSetupAxisFormat(ZymVM* vm, ZymValue, ZymValue axisV, ZymValue fmtV) {
    int axis; if (!reqInt(vm, axisV, "ui.plotSetupAxisFormat(axis, fmt)", &axis)) return ZYM_ERROR;
    std::string fmt;
    if (!reqStr(vm, fmtV, "ui.plotSetupAxisFormat(axis, fmt)", &fmt)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotSetupAxisFormat")) return ZYM_ERROR;
    ImPlot::SetupAxisFormat((ImAxis)axis, fmt.c_str());
    return zym_newNull();
}

// --- SetupAxisTicks helpers ---
//
// Reads a list-of-numbers into a `std::vector<double>`. Returns false
// (and raises) on type mismatch.
static bool readNumList(ZymVM* vm, ZymValue v, const char* where, std::vector<double>* out) {
    if (!zym_isList(v)) {
        zym_runtimeError(vm, "%s expects a list of numbers", where);
        return false;
    }
    int n = zym_listLength(v);
    out->resize((size_t)n);
    for (int i = 0; i < n; i++) {
        ZymValue e = zym_listGet(vm, v, i);
        if (!zym_isNumber(e)) {
            zym_runtimeError(vm, "%s: element %d is not a number", where, i);
            return false;
        }
        (*out)[i] = zym_asNumber(e);
    }
    return true;
}

// Reads a list-of-strings into a vector of C-string pointers (whose
// storage lives in `backing`). Backing must outlive the returned ptrs.
static bool readStrList(ZymVM* vm, ZymValue v, const char* where,
                        std::vector<std::string>* backing,
                        std::vector<const char*>* outPtrs) {
    if (!zym_isList(v)) {
        zym_runtimeError(vm, "%s labels must be a list of strings", where);
        return false;
    }
    int n = zym_listLength(v);
    backing->resize((size_t)n);
    outPtrs->resize((size_t)n);
    for (int i = 0; i < n; i++) {
        ZymValue e = zym_listGet(vm, v, i);
        if (!zym_isString(e)) {
            zym_runtimeError(vm, "%s: label %d is not a string", where, i);
            return false;
        }
        (*backing)[i] = zym_asCString(e);
        (*outPtrs)[i] = (*backing)[i].c_str();
    }
    return true;
}

// `ui.plotSetupAxisTicks(axis, values)`
ZymValue u_plotSetupAxisTicks_v(ZymVM* vm, ZymValue, ZymValue axisV, ZymValue valuesV) {
    int axis; if (!reqInt(vm, axisV, "ui.plotSetupAxisTicks(axis, values)", &axis)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotSetupAxisTicks")) return ZYM_ERROR;
    std::vector<double> values;
    if (!readNumList(vm, valuesV, "ui.plotSetupAxisTicks(axis, values)", &values)) return ZYM_ERROR;
    ImPlot::SetupAxisTicks((ImAxis)axis, values.data(), (int)values.size(), nullptr, false);
    return zym_newNull();
}

// `ui.plotSetupAxisTicks(axis, values, labels, keepDefault)`
ZymValue u_plotSetupAxisTicks_vl(ZymVM* vm, ZymValue, ZymValue axisV, ZymValue valuesV,
                                 ZymValue labelsV, ZymValue keepV) {
    int axis; if (!reqInt(vm, axisV, "ui.plotSetupAxisTicks(axis, values, labels, keepDefault)", &axis)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotSetupAxisTicks")) return ZYM_ERROR;
    std::vector<double> values;
    if (!readNumList(vm, valuesV, "ui.plotSetupAxisTicks values", &values)) return ZYM_ERROR;
    std::vector<std::string> backing;
    std::vector<const char*> ptrs;
    if (!readStrList(vm, labelsV, "ui.plotSetupAxisTicks", &backing, &ptrs)) return ZYM_ERROR;
    if ((int)ptrs.size() != (int)values.size()) {
        zym_runtimeError(vm, "ui.plotSetupAxisTicks: labels length (%d) must match values length (%d)",
                         (int)ptrs.size(), (int)values.size());
        return ZYM_ERROR;
    }
    bool keep = optBool(keepV, false);
    ImPlot::SetupAxisTicks((ImAxis)axis, values.data(), (int)values.size(), ptrs.data(), keep);
    return zym_newNull();
}

// `ui.plotSetupAxisTicks(axis, vMin, vMax, nTicks, labels?, keepDefault?)`
ZymValue u_plotSetupAxisTicks_range(ZymVM* vm, ZymValue, ZymValue axisV, ZymValue vMinV,
                                    ZymValue vMaxV, ZymValue nTicksV, ZymValue labelsV,
                                    ZymValue keepV) {
    int axis; if (!reqInt(vm, axisV, "ui.plotSetupAxisTicks(axis, vMin, vMax, nTicks, labels?, keep?)", &axis)) return ZYM_ERROR;
    double vMin, vMax;
    if (!reqNum(vm, vMinV, "ui.plotSetupAxisTicks vMin", &vMin)) return ZYM_ERROR;
    if (!reqNum(vm, vMaxV, "ui.plotSetupAxisTicks vMax", &vMax)) return ZYM_ERROR;
    int nTicks; if (!reqInt(vm, nTicksV, "ui.plotSetupAxisTicks nTicks", &nTicks)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotSetupAxisTicks")) return ZYM_ERROR;
    std::vector<std::string> backing;
    std::vector<const char*> ptrs;
    const char* const* labelPtrs = nullptr;
    if (zym_isList(labelsV)) {
        if (!readStrList(vm, labelsV, "ui.plotSetupAxisTicks", &backing, &ptrs)) return ZYM_ERROR;
        if ((int)ptrs.size() != nTicks) {
            zym_runtimeError(vm, "ui.plotSetupAxisTicks: labels length (%d) must match nTicks (%d)",
                             (int)ptrs.size(), nTicks);
            return ZYM_ERROR;
        }
        labelPtrs = ptrs.data();
    }
    bool keep = optBool(keepV, false);
    ImPlot::SetupAxisTicks((ImAxis)axis, vMin, vMax, nTicks, labelPtrs, keep);
    return zym_newNull();
}

// --- SetupAxisScale(axis, scale) — enum form only. (Custom-transform
// overload deferred; needs a ImPlotTransform callback bridge that's
// not worth the complexity until somebody asks for it.)
ZymValue u_plotSetupAxisScale(ZymVM* vm, ZymValue, ZymValue axisV, ZymValue scaleV) {
    int axis; if (!reqInt(vm, axisV, "ui.plotSetupAxisScale", &axis)) return ZYM_ERROR;
    int scale; if (!reqInt(vm, scaleV, "ui.plotSetupAxisScale", &scale)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotSetupAxisScale")) return ZYM_ERROR;
    ImPlot::SetupAxisScale((ImAxis)axis, (ImPlotScale)scale);
    return zym_newNull();
}

// --- SetupAxisLimitsConstraints(axis, vMin, vMax) -----
ZymValue u_plotSetupAxisLimitsConstraints(ZymVM* vm, ZymValue, ZymValue axisV,
                                          ZymValue vMinV, ZymValue vMaxV) {
    int axis; if (!reqInt(vm, axisV, "ui.plotSetupAxisLimitsConstraints", &axis)) return ZYM_ERROR;
    double vMin, vMax;
    if (!reqNum(vm, vMinV, "ui.plotSetupAxisLimitsConstraints", &vMin)) return ZYM_ERROR;
    if (!reqNum(vm, vMaxV, "ui.plotSetupAxisLimitsConstraints", &vMax)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotSetupAxisLimitsConstraints")) return ZYM_ERROR;
    ImPlot::SetupAxisLimitsConstraints((ImAxis)axis, vMin, vMax);
    return zym_newNull();
}

// --- SetupAxisZoomConstraints(axis, zMin, zMax) -----
ZymValue u_plotSetupAxisZoomConstraints(ZymVM* vm, ZymValue, ZymValue axisV,
                                        ZymValue zMinV, ZymValue zMaxV) {
    int axis; if (!reqInt(vm, axisV, "ui.plotSetupAxisZoomConstraints", &axis)) return ZYM_ERROR;
    double zMin, zMax;
    if (!reqNum(vm, zMinV, "ui.plotSetupAxisZoomConstraints", &zMin)) return ZYM_ERROR;
    if (!reqNum(vm, zMaxV, "ui.plotSetupAxisZoomConstraints", &zMax)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotSetupAxisZoomConstraints")) return ZYM_ERROR;
    ImPlot::SetupAxisZoomConstraints((ImAxis)axis, zMin, zMax);
    return zym_newNull();
}

// --- SetupLegend(location, flags?) -----
ZymValue u_plotSetupLegend(ZymVM* vm, ZymValue, ZymValue locV, ZymValue flagsV) {
    int loc; if (!reqInt(vm, locV, "ui.plotSetupLegend(location, flags?)", &loc)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotSetupLegend")) return ZYM_ERROR;
    int flags = optInt(flagsV, 0);
    ImPlot::SetupLegend((ImPlotLocation)loc, (ImPlotLegendFlags)flags);
    return zym_newNull();
}

// --- SetupMouseText(location, flags?) -----
ZymValue u_plotSetupMouseText(ZymVM* vm, ZymValue, ZymValue locV, ZymValue flagsV) {
    int loc; if (!reqInt(vm, locV, "ui.plotSetupMouseText(location, flags?)", &loc)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotSetupMouseText")) return ZYM_ERROR;
    int flags = optInt(flagsV, 0);
    ImPlot::SetupMouseText((ImPlotLocation)loc, (ImPlotMouseTextFlags)flags);
    return zym_newNull();
}

// --- SetupFinish() -----
ZymValue u_plotSetupFinish(ZymVM* vm, ZymValue) {
    if (!requirePlot(vm, "ui.plotSetupFinish")) return ZYM_ERROR;
    ImPlot::SetupFinish();
    return zym_newNull();
}

// --- SetAxis(axis) — switch current y-axis (legacy 1-arg form: x stays) -----
ZymValue u_plotSetAxis(ZymVM* vm, ZymValue, ZymValue axisV) {
    int axis; if (!reqInt(vm, axisV, "ui.plotSetAxis(axis)", &axis)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotSetAxis")) return ZYM_ERROR;
    ImPlot::SetAxis((ImAxis)axis);
    return zym_newNull();
}

// --- SetAxes(xAxis, yAxis) -----
ZymValue u_plotSetAxes(ZymVM* vm, ZymValue, ZymValue xV, ZymValue yV) {
    int x, y;
    if (!reqInt(vm, xV, "ui.plotSetAxes(xAxis, yAxis)", &x)) return ZYM_ERROR;
    if (!reqInt(vm, yV, "ui.plotSetAxes(xAxis, yAxis)", &y)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotSetAxes")) return ZYM_ERROR;
    ImPlot::SetAxes((ImAxis)x, (ImAxis)y);
    return zym_newNull();
}

// --- SetNextAxisLimits(axis, vMin, vMax, cond?) — called OUTSIDE a plot -----
ZymValue u_plotSetNextAxisLimits(ZymVM* vm, ZymValue, ZymValue axisV, ZymValue vMinV,
                                 ZymValue vMaxV, ZymValue condV) {
    int axis; if (!reqInt(vm, axisV, "ui.plotSetNextAxisLimits", &axis)) return ZYM_ERROR;
    double vMin, vMax;
    if (!reqNum(vm, vMinV, "ui.plotSetNextAxisLimits", &vMin)) return ZYM_ERROR;
    if (!reqNum(vm, vMaxV, "ui.plotSetNextAxisLimits", &vMax)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.plotSetNextAxisLimits")) return ZYM_ERROR;
    int cond = optInt(condV, ImPlotCond_Once);
    ImPlot::SetNextAxisLimits((ImAxis)axis, vMin, vMax, (ImPlotCond)cond);
    return zym_newNull();
}

// --- SetNextAxisLinks(axis, ref) — same caveat as SetupAxisLinks -----
ZymValue u_plotSetNextAxisLinks(ZymVM* vm, ZymValue, ZymValue axisV, ZymValue refV) {
    int axis; if (!reqInt(vm, axisV, "ui.plotSetNextAxisLinks(axis, [min,max])", &axis)) return ZYM_ERROR;
    if (!zym_isList(refV) || zym_listLength(refV) < 2) {
        zym_runtimeError(vm, "ui.plotSetNextAxisLinks: ref must be a 2-element list [min, max]");
        return ZYM_ERROR;
    }
    if (!requireFrame(vm, "ui.plotSetNextAxisLinks")) return ZYM_ERROR;
    ZymValue v0 = zym_listGet(vm, refV, 0);
    ZymValue v1 = zym_listGet(vm, refV, 1);
    if (!zym_isNumber(v0) || !zym_isNumber(v1)) {
        zym_runtimeError(vm, "ui.plotSetNextAxisLinks: ref elements must be numbers");
        return ZYM_ERROR;
    }
    double lo = zym_asNumber(v0);
    double hi = zym_asNumber(v1);
    ImPlot::SetNextAxisLinks((ImAxis)axis, &lo, &hi);
    zym_listSet(vm, refV, 0, zym_newNumber(lo));
    zym_listSet(vm, refV, 1, zym_newNumber(hi));
    return zym_newNull();
}

// --- SetNextAxisToFit(axis) -----
ZymValue u_plotSetNextAxisToFit(ZymVM* vm, ZymValue, ZymValue axisV) {
    int axis; if (!reqInt(vm, axisV, "ui.plotSetNextAxisToFit", &axis)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.plotSetNextAxisToFit")) return ZYM_ERROR;
    ImPlot::SetNextAxisToFit((ImAxis)axis);
    return zym_newNull();
}

// --- SetNextAxesLimits(xMin, xMax, yMin, yMax, cond?) -----
ZymValue u_plotSetNextAxesLimits(ZymVM* vm, ZymValue, ZymValue xMinV, ZymValue xMaxV,
                                 ZymValue yMinV, ZymValue yMaxV, ZymValue condV) {
    double xMin, xMax, yMin, yMax;
    if (!reqNum(vm, xMinV, "ui.plotSetNextAxesLimits", &xMin)) return ZYM_ERROR;
    if (!reqNum(vm, xMaxV, "ui.plotSetNextAxesLimits", &xMax)) return ZYM_ERROR;
    if (!reqNum(vm, yMinV, "ui.plotSetNextAxesLimits", &yMin)) return ZYM_ERROR;
    if (!reqNum(vm, yMaxV, "ui.plotSetNextAxesLimits", &yMax)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.plotSetNextAxesLimits")) return ZYM_ERROR;
    int cond = optInt(condV, ImPlotCond_Once);
    ImPlot::SetNextAxesLimits(xMin, xMax, yMin, yMax, (ImPlotCond)cond);
    return zym_newNull();
}

// --- SetNextAxesToFit() -----
ZymValue u_plotSetNextAxesToFit(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.plotSetNextAxesToFit")) return ZYM_ERROR;
    ImPlot::SetNextAxesToFit();
    return zym_newNull();
}

// ==== SECTION 3a: Plot items (line family) ================================
//
// Line, Scatter, Stairs, Shaded, Bars, BarGroups, Stems, InfLines, Dummy.
//
// Data input shape: every plot-item native accepts EITHER a Zym list of
// numbers OR a Zym Buffer of packed `double` values (8 bytes per element).
// The `readDoubles` helper auto-detects and yields a zero-copy view into
// the buffer or an owned copy of the list. See `DoubleView`.

struct DoubleView {
    const double*       data  = nullptr;
    int                 count = 0;
    std::vector<double> owned;          // empty on the buffer path
};

// Reads a list-of-numbers OR a Buffer-of-packed-doubles into a contiguous
// `double*` view. Returns false (and raises) on a type mismatch.
//
// Buffer path: zero-copy reinterpret of the buffer bytes as `const double*`.
// The buffer's byte length must be a multiple of sizeof(double); count is
// bytes / 8.
//
// List path: copies each element into `owned`.
static bool readDoubles(ZymVM* vm, ZymValue v, const char* where, DoubleView* out) {
    if (zym_isList(v)) {
        int n = zym_listLength(v);
        out->owned.resize((size_t)n);
        for (int i = 0; i < n; i++) {
            ZymValue e = zym_listGet(vm, v, i);
            if (!zym_isNumber(e)) {
                zym_runtimeError(vm, "%s: list element %d is not a number", where, i);
                return false;
            }
            out->owned[(size_t)i] = zym_asNumber(e);
        }
        out->data  = out->owned.data();
        out->count = n;
        return true;
    }
    const char* bytes = nullptr;
    size_t      nbytes = 0;
    if (readBufferBytes(vm, v, &bytes, &nbytes)) {
        if ((nbytes % sizeof(double)) != 0) {
            zym_runtimeError(vm,
                "%s: Buffer length %zu is not a multiple of sizeof(double) (8)",
                where, nbytes);
            return false;
        }
        out->data  = reinterpret_cast<const double*>(bytes);
        out->count = (int)(nbytes / sizeof(double));
        return true;
    }
    zym_runtimeError(vm, "%s: argument must be a list of numbers or a Buffer of f64", where);
    return false;
}

// Convenience: build an ImPlotSpec carrying only Flags. Section 3a doesn't
// expose the rest of the spec (style stacks in Section 5 cover that).
static inline ImPlotSpec specWithFlags(int flags) {
    ImPlotSpec s;
    s.Flags = (ImPlotItemFlags)flags;
    return s;
}

// ---- PlotLine ----
// (label, ys)
ZymValue u_plotLine_y(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue ysV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotLine(label, ys)", &label)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotLine")) return ZYM_ERROR;
    DoubleView ys;
    if (!readDoubles(vm, ysV, "ui.plotLine(label, ys)", &ys)) return ZYM_ERROR;
    ImPlot::PlotLine(label.c_str(), ys.data, ys.count);
    return zym_newNull();
}
// (label, xs, ys)
ZymValue u_plotLine_xy(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue xsV, ZymValue ysV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotLine(label, xs, ys)", &label)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotLine")) return ZYM_ERROR;
    DoubleView xs, ys;
    if (!readDoubles(vm, xsV, "ui.plotLine xs", &xs)) return ZYM_ERROR;
    if (!readDoubles(vm, ysV, "ui.plotLine ys", &ys)) return ZYM_ERROR;
    int n = xs.count < ys.count ? xs.count : ys.count;
    ImPlot::PlotLine(label.c_str(), xs.data, ys.data, n);
    return zym_newNull();
}
// (label, xs, ys, flags)
ZymValue u_plotLine_xyf(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue xsV,
                        ZymValue ysV, ZymValue flagsV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotLine(label, xs, ys, flags)", &label)) return ZYM_ERROR;
    int flags; if (!reqInt(vm, flagsV, "ui.plotLine flags", &flags)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotLine")) return ZYM_ERROR;
    DoubleView xs, ys;
    if (!readDoubles(vm, xsV, "ui.plotLine xs", &xs)) return ZYM_ERROR;
    if (!readDoubles(vm, ysV, "ui.plotLine ys", &ys)) return ZYM_ERROR;
    int n = xs.count < ys.count ? xs.count : ys.count;
    ImPlot::PlotLine(label.c_str(), xs.data, ys.data, n, specWithFlags(flags));
    return zym_newNull();
}

// ---- PlotScatter ----
ZymValue u_plotScatter_y(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue ysV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotScatter(label, ys)", &label)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotScatter")) return ZYM_ERROR;
    DoubleView ys;
    if (!readDoubles(vm, ysV, "ui.plotScatter ys", &ys)) return ZYM_ERROR;
    ImPlot::PlotScatter(label.c_str(), ys.data, ys.count);
    return zym_newNull();
}
ZymValue u_plotScatter_xy(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue xsV, ZymValue ysV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotScatter(label, xs, ys)", &label)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotScatter")) return ZYM_ERROR;
    DoubleView xs, ys;
    if (!readDoubles(vm, xsV, "ui.plotScatter xs", &xs)) return ZYM_ERROR;
    if (!readDoubles(vm, ysV, "ui.plotScatter ys", &ys)) return ZYM_ERROR;
    int n = xs.count < ys.count ? xs.count : ys.count;
    ImPlot::PlotScatter(label.c_str(), xs.data, ys.data, n);
    return zym_newNull();
}
ZymValue u_plotScatter_xyf(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue xsV,
                           ZymValue ysV, ZymValue flagsV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotScatter(label, xs, ys, flags)", &label)) return ZYM_ERROR;
    int flags; if (!reqInt(vm, flagsV, "ui.plotScatter flags", &flags)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotScatter")) return ZYM_ERROR;
    DoubleView xs, ys;
    if (!readDoubles(vm, xsV, "ui.plotScatter xs", &xs)) return ZYM_ERROR;
    if (!readDoubles(vm, ysV, "ui.plotScatter ys", &ys)) return ZYM_ERROR;
    int n = xs.count < ys.count ? xs.count : ys.count;
    ImPlot::PlotScatter(label.c_str(), xs.data, ys.data, n, specWithFlags(flags));
    return zym_newNull();
}

// ---- PlotStairs ----
ZymValue u_plotStairs_y(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue ysV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotStairs(label, ys)", &label)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotStairs")) return ZYM_ERROR;
    DoubleView ys;
    if (!readDoubles(vm, ysV, "ui.plotStairs ys", &ys)) return ZYM_ERROR;
    ImPlot::PlotStairs(label.c_str(), ys.data, ys.count);
    return zym_newNull();
}
ZymValue u_plotStairs_xy(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue xsV, ZymValue ysV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotStairs(label, xs, ys)", &label)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotStairs")) return ZYM_ERROR;
    DoubleView xs, ys;
    if (!readDoubles(vm, xsV, "ui.plotStairs xs", &xs)) return ZYM_ERROR;
    if (!readDoubles(vm, ysV, "ui.plotStairs ys", &ys)) return ZYM_ERROR;
    int n = xs.count < ys.count ? xs.count : ys.count;
    ImPlot::PlotStairs(label.c_str(), xs.data, ys.data, n);
    return zym_newNull();
}
ZymValue u_plotStairs_xyf(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue xsV,
                          ZymValue ysV, ZymValue flagsV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotStairs(label, xs, ys, flags)", &label)) return ZYM_ERROR;
    int flags; if (!reqInt(vm, flagsV, "ui.plotStairs flags", &flags)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotStairs")) return ZYM_ERROR;
    DoubleView xs, ys;
    if (!readDoubles(vm, xsV, "ui.plotStairs xs", &xs)) return ZYM_ERROR;
    if (!readDoubles(vm, ysV, "ui.plotStairs ys", &ys)) return ZYM_ERROR;
    int n = xs.count < ys.count ? xs.count : ys.count;
    ImPlot::PlotStairs(label.c_str(), xs.data, ys.data, n, specWithFlags(flags));
    return zym_newNull();
}

// ---- PlotShaded ----
// (label, ys, yref?)
ZymValue u_plotShaded_y(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue ysV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotShaded(label, ys)", &label)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotShaded")) return ZYM_ERROR;
    DoubleView ys;
    if (!readDoubles(vm, ysV, "ui.plotShaded ys", &ys)) return ZYM_ERROR;
    ImPlot::PlotShaded(label.c_str(), ys.data, ys.count);
    return zym_newNull();
}
ZymValue u_plotShaded_xy(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue xsV, ZymValue ysV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotShaded(label, xs, ys)", &label)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotShaded")) return ZYM_ERROR;
    DoubleView xs, ys;
    if (!readDoubles(vm, xsV, "ui.plotShaded xs", &xs)) return ZYM_ERROR;
    if (!readDoubles(vm, ysV, "ui.plotShaded ys", &ys)) return ZYM_ERROR;
    int n = xs.count < ys.count ? xs.count : ys.count;
    ImPlot::PlotShaded(label.c_str(), xs.data, ys.data, n);
    return zym_newNull();
}
// (label, xs, ys, yref)
ZymValue u_plotShaded_xyref(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue xsV,
                            ZymValue ysV, ZymValue yrefV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotShaded(label, xs, ys, yref)", &label)) return ZYM_ERROR;
    double yref;
    if (!reqNum(vm, yrefV, "ui.plotShaded yref", &yref)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotShaded")) return ZYM_ERROR;
    DoubleView xs, ys;
    if (!readDoubles(vm, xsV, "ui.plotShaded xs", &xs)) return ZYM_ERROR;
    if (!readDoubles(vm, ysV, "ui.plotShaded ys", &ys)) return ZYM_ERROR;
    int n = xs.count < ys.count ? xs.count : ys.count;
    ImPlot::PlotShaded(label.c_str(), xs.data, ys.data, n, yref);
    return zym_newNull();
}
// (label, xs, ys1, ys2) — band between two lines
ZymValue u_plotShaded_band(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue xsV,
                           ZymValue ys1V, ZymValue ys2V, ZymValue flagsV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotShaded(label, xs, ys1, ys2, flags?)", &label)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotShaded")) return ZYM_ERROR;
    DoubleView xs, y1, y2;
    if (!readDoubles(vm, xsV,  "ui.plotShaded xs",  &xs)) return ZYM_ERROR;
    if (!readDoubles(vm, ys1V, "ui.plotShaded ys1", &y1)) return ZYM_ERROR;
    if (!readDoubles(vm, ys2V, "ui.plotShaded ys2", &y2)) return ZYM_ERROR;
    int n = xs.count;
    if (y1.count < n) n = y1.count;
    if (y2.count < n) n = y2.count;
    int flags = optInt(flagsV, 0);
    ImPlot::PlotShaded(label.c_str(), xs.data, y1.data, y2.data, n, specWithFlags(flags));
    return zym_newNull();
}

// ---- PlotBars ----
// (label, values)
ZymValue u_plotBars_y(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue ysV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotBars(label, values)", &label)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotBars")) return ZYM_ERROR;
    DoubleView ys;
    if (!readDoubles(vm, ysV, "ui.plotBars values", &ys)) return ZYM_ERROR;
    ImPlot::PlotBars(label.c_str(), ys.data, ys.count);
    return zym_newNull();
}
// (label, values, barSize)
ZymValue u_plotBars_ys(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue ysV, ZymValue szV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotBars(label, values, barSize)", &label)) return ZYM_ERROR;
    double sz;
    if (!reqNum(vm, szV, "ui.plotBars barSize", &sz)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotBars")) return ZYM_ERROR;
    DoubleView ys;
    if (!readDoubles(vm, ysV, "ui.plotBars values", &ys)) return ZYM_ERROR;
    ImPlot::PlotBars(label.c_str(), ys.data, ys.count, sz);
    return zym_newNull();
}
// (label, xs, ys, barSize, flags?)
ZymValue u_plotBars_xys(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue xsV,
                        ZymValue ysV, ZymValue szV, ZymValue flagsV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotBars(label, xs, ys, barSize, flags?)", &label)) return ZYM_ERROR;
    double sz;
    if (!reqNum(vm, szV, "ui.plotBars barSize", &sz)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotBars")) return ZYM_ERROR;
    DoubleView xs, ys;
    if (!readDoubles(vm, xsV, "ui.plotBars xs", &xs)) return ZYM_ERROR;
    if (!readDoubles(vm, ysV, "ui.plotBars ys", &ys)) return ZYM_ERROR;
    int n = xs.count < ys.count ? xs.count : ys.count;
    int flags = optInt(flagsV, 0);
    ImPlot::PlotBars(label.c_str(), xs.data, ys.data, n, sz, specWithFlags(flags));
    return zym_newNull();
}

// ---- PlotBarGroups ----
// (labelIds, values, itemCount, groupCount, groupSize?, shift?, flags?)
//
// `values` is a row-major flat list/buffer of length itemCount*groupCount,
// matching ImPlot's matrix layout.
ZymValue u_plotBarGroups(ZymVM* vm, ZymValue, ZymValue labelIdsV, ZymValue valuesV,
                         ZymValue itemCV, ZymValue groupCV, ZymValue groupSizeV,
                         ZymValue shiftV, ZymValue flagsV) {
    int itemC, groupC;
    if (!reqInt(vm, itemCV,  "ui.plotBarGroups itemCount",  &itemC))  return ZYM_ERROR;
    if (!reqInt(vm, groupCV, "ui.plotBarGroups groupCount", &groupC)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotBarGroups")) return ZYM_ERROR;
    std::vector<std::string> backing;
    std::vector<const char*> ptrs;
    if (!readStrList(vm, labelIdsV, "ui.plotBarGroups labels", &backing, &ptrs)) return ZYM_ERROR;
    if ((int)ptrs.size() != itemC) {
        zym_runtimeError(vm,
            "ui.plotBarGroups: labels length (%d) must match itemCount (%d)",
            (int)ptrs.size(), itemC);
        return ZYM_ERROR;
    }
    DoubleView values;
    if (!readDoubles(vm, valuesV, "ui.plotBarGroups values", &values)) return ZYM_ERROR;
    int need = itemC * groupC;
    if (values.count < need) {
        zym_runtimeError(vm,
            "ui.plotBarGroups: values length (%d) is smaller than itemCount*groupCount (%d)",
            values.count, need);
        return ZYM_ERROR;
    }
    double groupSize = optNum(groupSizeV, 0.67);
    double shift     = optNum(shiftV,     0.0);
    int    flags     = optInt(flagsV,     0);
    ImPlot::PlotBarGroups(ptrs.data(), values.data, itemC, groupC, groupSize, shift, specWithFlags(flags));
    return zym_newNull();
}

// ---- PlotStems ----
ZymValue u_plotStems_y(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue ysV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotStems(label, values)", &label)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotStems")) return ZYM_ERROR;
    DoubleView ys;
    if (!readDoubles(vm, ysV, "ui.plotStems values", &ys)) return ZYM_ERROR;
    ImPlot::PlotStems(label.c_str(), ys.data, ys.count);
    return zym_newNull();
}
ZymValue u_plotStems_xy(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue xsV, ZymValue ysV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotStems(label, xs, ys)", &label)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotStems")) return ZYM_ERROR;
    DoubleView xs, ys;
    if (!readDoubles(vm, xsV, "ui.plotStems xs", &xs)) return ZYM_ERROR;
    if (!readDoubles(vm, ysV, "ui.plotStems ys", &ys)) return ZYM_ERROR;
    int n = xs.count < ys.count ? xs.count : ys.count;
    ImPlot::PlotStems(label.c_str(), xs.data, ys.data, n);
    return zym_newNull();
}
ZymValue u_plotStems_xyref(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue xsV,
                           ZymValue ysV, ZymValue refV, ZymValue flagsV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotStems(label, xs, ys, ref, flags?)", &label)) return ZYM_ERROR;
    double ref;
    if (!reqNum(vm, refV, "ui.plotStems ref", &ref)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotStems")) return ZYM_ERROR;
    DoubleView xs, ys;
    if (!readDoubles(vm, xsV, "ui.plotStems xs", &xs)) return ZYM_ERROR;
    if (!readDoubles(vm, ysV, "ui.plotStems ys", &ys)) return ZYM_ERROR;
    int n = xs.count < ys.count ? xs.count : ys.count;
    int flags = optInt(flagsV, 0);
    ImPlot::PlotStems(label.c_str(), xs.data, ys.data, n, ref, specWithFlags(flags));
    return zym_newNull();
}

// ---- PlotInfLines ----
ZymValue u_plotInfLines(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue valuesV, ZymValue flagsV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotInfLines(label, values, flags?)", &label)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotInfLines")) return ZYM_ERROR;
    DoubleView values;
    if (!readDoubles(vm, valuesV, "ui.plotInfLines values", &values)) return ZYM_ERROR;
    int flags = optInt(flagsV, 0);
    ImPlot::PlotInfLines(label.c_str(), values.data, values.count, specWithFlags(flags));
    return zym_newNull();
}

// ---- PlotDummy ----
ZymValue u_plotDummy(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue flagsV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotDummy(label, flags?)", &label)) return ZYM_ERROR;
    int flags = optInt(flagsV, 0);
    if (!requirePlot(vm, "ui.plotDummy")) return ZYM_ERROR;
    ImPlot::PlotDummy(label.c_str(), specWithFlags(flags));
    return zym_newNull();
}

// ==== SECTION 3b: Plot items (statistical family) =========================
//
// Histogram, Histogram2D, PieChart, Heatmap, ErrorBars, Digital.
//
// Two of these (PlotHistogram, PlotHistogram2D) RETURN a value (the max
// observed bin count); we surface that as a Number from the native.
// PlotPieChart and PlotHeatmap take a `label_fmt` printf-style string.
// PlotHeatmap consumes a row-major (or column-major via flags) matrix
// of length rows*cols.

// ---- PlotErrorBars ----
// (label, xs, ys, err)  — symmetric
ZymValue u_plotErrorBars_sym(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue xsV,
                             ZymValue ysV, ZymValue errV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotErrorBars(label, xs, ys, err)", &label)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotErrorBars")) return ZYM_ERROR;
    DoubleView xs, ys, err;
    if (!readDoubles(vm, xsV,  "ui.plotErrorBars xs",  &xs))  return ZYM_ERROR;
    if (!readDoubles(vm, ysV,  "ui.plotErrorBars ys",  &ys))  return ZYM_ERROR;
    if (!readDoubles(vm, errV, "ui.plotErrorBars err", &err)) return ZYM_ERROR;
    int n = xs.count;
    if (ys.count  < n) n = ys.count;
    if (err.count < n) n = err.count;
    ImPlot::PlotErrorBars(label.c_str(), xs.data, ys.data, err.data, n);
    return zym_newNull();
}
// (label, xs, ys, err, flags)
ZymValue u_plotErrorBars_symF(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue xsV,
                              ZymValue ysV, ZymValue errV, ZymValue flagsV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotErrorBars(label, xs, ys, err, flags)", &label)) return ZYM_ERROR;
    int flags; if (!reqInt(vm, flagsV, "ui.plotErrorBars flags", &flags)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotErrorBars")) return ZYM_ERROR;
    DoubleView xs, ys, err;
    if (!readDoubles(vm, xsV,  "ui.plotErrorBars xs",  &xs))  return ZYM_ERROR;
    if (!readDoubles(vm, ysV,  "ui.plotErrorBars ys",  &ys))  return ZYM_ERROR;
    if (!readDoubles(vm, errV, "ui.plotErrorBars err", &err)) return ZYM_ERROR;
    int n = xs.count;
    if (ys.count  < n) n = ys.count;
    if (err.count < n) n = err.count;
    ImPlot::PlotErrorBars(label.c_str(), xs.data, ys.data, err.data, n, specWithFlags(flags));
    return zym_newNull();
}
// (label, xs, ys, neg, pos, flags?)  — asymmetric
ZymValue u_plotErrorBars_asym(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue xsV,
                              ZymValue ysV, ZymValue negV, ZymValue posV, ZymValue flagsV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotErrorBars(label, xs, ys, neg, pos, flags?)", &label)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotErrorBars")) return ZYM_ERROR;
    DoubleView xs, ys, neg, pos;
    if (!readDoubles(vm, xsV,  "ui.plotErrorBars xs",  &xs))  return ZYM_ERROR;
    if (!readDoubles(vm, ysV,  "ui.plotErrorBars ys",  &ys))  return ZYM_ERROR;
    if (!readDoubles(vm, negV, "ui.plotErrorBars neg", &neg)) return ZYM_ERROR;
    if (!readDoubles(vm, posV, "ui.plotErrorBars pos", &pos)) return ZYM_ERROR;
    int n = xs.count;
    if (ys.count  < n) n = ys.count;
    if (neg.count < n) n = neg.count;
    if (pos.count < n) n = pos.count;
    int flags = optInt(flagsV, 0);
    ImPlot::PlotErrorBars(label.c_str(), xs.data, ys.data, neg.data, pos.data, n, specWithFlags(flags));
    return zym_newNull();
}

// ---- PlotPieChart ----
// (labels, values, x, y, radius, labelFmt?, angle0?, flags?)
ZymValue u_plotPieChart(ZymVM* vm, ZymValue, ZymValue labelsV, ZymValue valuesV,
                        ZymValue xV, ZymValue yV, ZymValue radiusV,
                        ZymValue labelFmtV, ZymValue angle0V, ZymValue flagsV) {
    double x, y, radius;
    if (!reqNum(vm, xV,      "ui.plotPieChart x",      &x))      return ZYM_ERROR;
    if (!reqNum(vm, yV,      "ui.plotPieChart y",      &y))      return ZYM_ERROR;
    if (!reqNum(vm, radiusV, "ui.plotPieChart radius", &radius)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotPieChart")) return ZYM_ERROR;
    std::vector<std::string> backing;
    std::vector<const char*> ptrs;
    if (!readStrList(vm, labelsV, "ui.plotPieChart labels", &backing, &ptrs)) return ZYM_ERROR;
    DoubleView values;
    if (!readDoubles(vm, valuesV, "ui.plotPieChart values", &values)) return ZYM_ERROR;
    int n = (int)ptrs.size();
    if (values.count < n) n = values.count;
    const char* labelFmt = optStr(labelFmtV, "%.1f");
    double angle0 = optNum(angle0V, 90.0);
    int    flags  = optInt(flagsV,  0);
    ImPlot::PlotPieChart(ptrs.data(), values.data, n, x, y, radius, labelFmt, angle0, specWithFlags(flags));
    return zym_newNull();
}

// ---- PlotHeatmap ----
// (label, values, rows, cols, scaleMin?, scaleMax?, labelFmt?, xMin?, yMin?, xMax?, yMax?, flags?)
//
// `values` is a row-major (default) or column-major (flags) flat matrix
// of length rows*cols. Bounds default to (0,0)-(1,1) matching ImPlot.
ZymValue u_plotHeatmap(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue valuesV,
                       ZymValue rowsV, ZymValue colsV, ZymValue scaleMinV,
                       ZymValue scaleMaxV, ZymValue labelFmtV,
                       ZymValue xMinV, ZymValue yMinV, ZymValue xMaxV, ZymValue yMaxV,
                       ZymValue flagsV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotHeatmap(label, values, rows, cols, ...)", &label)) return ZYM_ERROR;
    int rows, cols;
    if (!reqInt(vm, rowsV, "ui.plotHeatmap rows", &rows)) return ZYM_ERROR;
    if (!reqInt(vm, colsV, "ui.plotHeatmap cols", &cols)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotHeatmap")) return ZYM_ERROR;
    DoubleView values;
    if (!readDoubles(vm, valuesV, "ui.plotHeatmap values", &values)) return ZYM_ERROR;
    int need = rows * cols;
    if (values.count < need) {
        zym_runtimeError(vm,
            "ui.plotHeatmap: values length (%d) is smaller than rows*cols (%d)",
            values.count, need);
        return ZYM_ERROR;
    }
    double scaleMin = optNum(scaleMinV, 0.0);
    double scaleMax = optNum(scaleMaxV, 0.0);
    const char* labelFmt = optStr(labelFmtV, "%.1f");
    double xMin = optNum(xMinV, 0.0);
    double yMin = optNum(yMinV, 0.0);
    double xMax = optNum(xMaxV, 1.0);
    double yMax = optNum(yMaxV, 1.0);
    int    flags = optInt(flagsV, 0);
    ImPlot::PlotHeatmap(label.c_str(), values.data, rows, cols, scaleMin, scaleMax,
                        labelFmt, ImPlotPoint(xMin, yMin), ImPlotPoint(xMax, yMax),
                        specWithFlags(flags));
    return zym_newNull();
}

// ---- PlotHistogram ----
// (label, values, bins?, barScale?, rangeMin?, rangeMax?, flags?) -> Number
//
// Returns the max bin count (ImPlot returns double). `bins` accepts
// either a positive int (explicit bin count) or one of the
// `UI.PLOT_BIN_*` sentinels (negative values). The range is optional:
// if either rangeMin or rangeMax is omitted, ImPlotRange() (auto) is used.
ZymValue u_plotHistogram(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue valuesV,
                         ZymValue binsV, ZymValue barScaleV,
                         ZymValue rangeMinV, ZymValue rangeMaxV, ZymValue flagsV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotHistogram(label, values, ...)", &label)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotHistogram")) return ZYM_ERROR;
    DoubleView values;
    if (!readDoubles(vm, valuesV, "ui.plotHistogram values", &values)) return ZYM_ERROR;
    int    bins     = optInt(binsV,      ImPlotBin_Sturges);
    double barScale = optNum(barScaleV,  1.0);
    int    flags    = optInt(flagsV,     0);
    ImPlotRange range;
    if (zym_isNumber(rangeMinV) && zym_isNumber(rangeMaxV)) {
        range = ImPlotRange(zym_asNumber(rangeMinV), zym_asNumber(rangeMaxV));
    }
    double maxBin = ImPlot::PlotHistogram(label.c_str(), values.data, values.count,
                                          bins, barScale, range, specWithFlags(flags));
    return zym_newNumber(maxBin);
}

// ---- PlotHistogram2D ----
// (label, xs, ys, xBins?, yBins?, xMin?, xMax?, yMin?, yMax?, flags?) -> Number
ZymValue u_plotHistogram2D(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue xsV, ZymValue ysV,
                           ZymValue xBinsV, ZymValue yBinsV,
                           ZymValue xMinV, ZymValue xMaxV,
                           ZymValue yMinV, ZymValue yMaxV, ZymValue flagsV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotHistogram2D(label, xs, ys, ...)", &label)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotHistogram2D")) return ZYM_ERROR;
    DoubleView xs, ys;
    if (!readDoubles(vm, xsV, "ui.plotHistogram2D xs", &xs)) return ZYM_ERROR;
    if (!readDoubles(vm, ysV, "ui.plotHistogram2D ys", &ys)) return ZYM_ERROR;
    int n = xs.count < ys.count ? xs.count : ys.count;
    int xBins = optInt(xBinsV, ImPlotBin_Sturges);
    int yBins = optInt(yBinsV, ImPlotBin_Sturges);
    int flags = optInt(flagsV, 0);
    ImPlotRect range;
    if (zym_isNumber(xMinV) && zym_isNumber(xMaxV) &&
        zym_isNumber(yMinV) && zym_isNumber(yMaxV)) {
        range = ImPlotRect(zym_asNumber(xMinV), zym_asNumber(xMaxV),
                           zym_asNumber(yMinV), zym_asNumber(yMaxV));
    }
    double maxBin = ImPlot::PlotHistogram2D(label.c_str(), xs.data, ys.data, n,
                                            xBins, yBins, range, specWithFlags(flags));
    return zym_newNumber(maxBin);
}

// ---- PlotDigital ----
// (label, xs, ys, flags?)
ZymValue u_plotDigital(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue xsV,
                       ZymValue ysV, ZymValue flagsV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotDigital(label, xs, ys, flags?)", &label)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotDigital")) return ZYM_ERROR;
    DoubleView xs, ys;
    if (!readDoubles(vm, xsV, "ui.plotDigital xs", &xs)) return ZYM_ERROR;
    if (!readDoubles(vm, ysV, "ui.plotDigital ys", &ys)) return ZYM_ERROR;
    int n = xs.count < ys.count ? xs.count : ys.count;
    int flags = optInt(flagsV, 0);
    ImPlot::PlotDigital(label.c_str(), xs.data, ys.data, n, specWithFlags(flags));
    return zym_newNull();
}

// ==== SECTION 3c: Plot items (visual family) ==============================
//
// PlotImage — draws a textured quad in plot coords. The texture comes
// from the SDL native (`win.createTexture` / `win.textureFromSurface`),
// matching how `UI.image` already consumes them in `imgui.cpp`. We
// don't reuse imgui.cpp's `reqTexture` because it's TU-private there;
// instead we replicate the small handle-lookup via `sdlGetTextureHandle`
// (declared in `sdl_internal.hpp`, transitively included by
// `ui_internal.hpp`). The texture pointer is bit-cast into the
// `ImTextureID` slot the SDLRenderer3 backend reads back at draw time —
// same convention as `UI.image`.
//
// PlotText — draws text at a plot-coord position with an optional
// pixel offset and `ImPlotTextFlags_Vertical` rotation.

// Small TU-local texture-arg helper. Returns nullptr on failure (and
// raises a runtime error). `where` is included in the message for the
// usual caller-locating hint.
inline SDL_Texture* reqPlotTexture(ZymVM* vm, ZymValue v, const char* where) {
    TextureHandle* t = sdlGetTextureHandle(vm, v);
    if (!t || !t->texture) {
        zym_runtimeError(vm, "%s: expected a Texture value (from win.createTexture / win.textureFromSurface)", where);
        return nullptr;
    }
    if (!t->owner || !t->owner->renderer) {
        zym_runtimeError(vm, "%s: owning Window/renderer has been destroyed", where);
        return nullptr;
    }
    return t->texture;
}

// Small TU-local 2-element [x, y] reader. Falls back to `fallback` on
// null. Mirrors imgui.cpp's `optVec2` (TU-private there too).
inline bool plotOptVec2(ZymVM* vm, ZymValue v, ImVec2 fallback, const char* where, ImVec2* out) {
    if (zym_isNull(v)) { *out = fallback; return true; }
    if (!zym_isList(v) || zym_listLength(v) != 2) {
        zym_runtimeError(vm, "%s: expected a 2-element list [x, y]", where);
        return false;
    }
    ZymValue xv = zym_listGet(vm, v, 0);
    ZymValue yv = zym_listGet(vm, v, 1);
    if (!zym_isNumber(xv) || !zym_isNumber(yv)) {
        zym_runtimeError(vm, "%s: [x, y] elements must be numbers", where);
        return false;
    }
    *out = ImVec2((float)zym_asNumber(xv), (float)zym_asNumber(yv));
    return true;
}

// Pack an ImU32 RGBA into ImVec4 (0..1). Same convention as
// imgui.cpp's `u32ToVec4` (TU-private there).
inline ImVec4 plotU32ToVec4(ImU32 c) {
    return ImVec4(
        (float)((c >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f,
        (float)((c >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f,
        (float)((c >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f,
        (float)((c >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f);
}

// ---- PlotImage ----
// (label, tex, xMin, yMin, xMax, yMax, uv0?, uv1?, tint?, flags?)
ZymValue u_plotImage(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue texV,
                     ZymValue xMinV, ZymValue yMinV, ZymValue xMaxV, ZymValue yMaxV,
                     ZymValue uv0V, ZymValue uv1V, ZymValue tintV, ZymValue flagsV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotImage(label, tex, xMin, yMin, xMax, yMax, ...)", &label)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotImage")) return ZYM_ERROR;
    SDL_Texture* tex = reqPlotTexture(vm, texV, "ui.plotImage tex");
    if (!tex) return ZYM_ERROR;
    double xMin, yMin, xMax, yMax;
    if (!reqNum(vm, xMinV, "ui.plotImage xMin", &xMin)) return ZYM_ERROR;
    if (!reqNum(vm, yMinV, "ui.plotImage yMin", &yMin)) return ZYM_ERROR;
    if (!reqNum(vm, xMaxV, "ui.plotImage xMax", &xMax)) return ZYM_ERROR;
    if (!reqNum(vm, yMaxV, "ui.plotImage yMax", &yMax)) return ZYM_ERROR;
    ImVec2 uv0, uv1;
    if (!plotOptVec2(vm, uv0V, ImVec2(0, 0), "ui.plotImage uv0", &uv0)) return ZYM_ERROR;
    if (!plotOptVec2(vm, uv1V, ImVec2(1, 1), "ui.plotImage uv1", &uv1)) return ZYM_ERROR;
    ImVec4 tint = plotU32ToVec4(optU32(tintV, IM_COL32_WHITE));
    int flags = optInt(flagsV, 0);
    ImTextureRef ref((ImTextureID)(intptr_t)tex);
    ImPlot::PlotImage(label.c_str(), ref,
                      ImPlotPoint(xMin, yMin), ImPlotPoint(xMax, yMax),
                      uv0, uv1, tint, specWithFlags(flags));
    return zym_newNull();
}

// ---- PlotText ----
// (text, x, y, pixOffset?, flags?)
ZymValue u_plotText(ZymVM* vm, ZymValue, ZymValue textV, ZymValue xV, ZymValue yV,
                    ZymValue pixOffsetV, ZymValue flagsV) {
    std::string text;
    if (!reqStr(vm, textV, "ui.plotText(text, x, y, pixOffset?, flags?)", &text)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotText")) return ZYM_ERROR;
    double x, y;
    if (!reqNum(vm, xV, "ui.plotText x", &x)) return ZYM_ERROR;
    if (!reqNum(vm, yV, "ui.plotText y", &y)) return ZYM_ERROR;
    ImVec2 pixOffset;
    if (!plotOptVec2(vm, pixOffsetV, ImVec2(0, 0), "ui.plotText pixOffset", &pixOffset)) return ZYM_ERROR;
    int flags = optInt(flagsV, 0);
    ImPlot::PlotText(text.c_str(), x, y, pixOffset, specWithFlags(flags));
    return zym_newNull();
}

} // namespace

// ---- registration --------------------------------------------------------

void registerImPlotBindings(ZymVM* vm, ZymValue obj, ZymValue context, RootScope& roots) {
#define MOD(name, sig, fn) \
    ZymValue name = roots.push(zym_createNativeClosure(vm, sig, (void*)fn, context));

    // plot: 2-arg (title, body), 4-arg (title, w, h, body),
    // 5-arg (title, w, h, flags, body) — dispatcher
    ZymValue plot2 = zym_createNativeClosure(vm, "plot(title, body)",                  (void*)u_plot,       context);
    roots.push(plot2);
    ZymValue plot4 = zym_createNativeClosure(vm, "plot(title, w, h, body)",            (void*)u_plotSized,  context);
    roots.push(plot4);
    ZymValue plot5 = zym_createNativeClosure(vm, "plot(title, w, h, flags, body)",     (void*)u_plotFlags,  context);
    roots.push(plot5);
    ZymValue plot = zym_createDispatcher(vm);
    roots.push(plot);
    zym_addOverload(vm, plot, plot2);
    zym_addOverload(vm, plot, plot4);
    zym_addOverload(vm, plot, plot5);

    MOD(isInPlot, "isInPlot()", u_isInPlot)

    // ---- SECTION 2: Setup* + Set(Next)Axis*/Set(Next)Axes* ----
    MOD(plotSetupAxis,                  "plotSetupAxis(axis, label, flags)",                       u_plotSetupAxis)
    MOD(plotSetupAxes,                  "plotSetupAxes(xLabel, yLabel, xFlags, yFlags)",           u_plotSetupAxes)
    MOD(plotSetupAxisLimits,            "plotSetupAxisLimits(axis, vMin, vMax, cond)",             u_plotSetupAxisLimits)
    MOD(plotSetupAxesLimits,            "plotSetupAxesLimits(xMin, xMax, yMin, yMax, cond)",       u_plotSetupAxesLimits)
    MOD(plotSetupAxisLinks,             "plotSetupAxisLinks(axis, ref)",                           u_plotSetupAxisLinks)
    MOD(plotSetupAxisFormat,            "plotSetupAxisFormat(axis, fmt)",                          u_plotSetupAxisFormat)
    MOD(plotSetupAxisScale,             "plotSetupAxisScale(axis, scale)",                         u_plotSetupAxisScale)
    MOD(plotSetupAxisLimitsConstraints, "plotSetupAxisLimitsConstraints(axis, vMin, vMax)",        u_plotSetupAxisLimitsConstraints)
    MOD(plotSetupAxisZoomConstraints,   "plotSetupAxisZoomConstraints(axis, zMin, zMax)",          u_plotSetupAxisZoomConstraints)
    MOD(plotSetupLegend,                "plotSetupLegend(location, flags)",                        u_plotSetupLegend)
    MOD(plotSetupMouseText,             "plotSetupMouseText(location, flags)",                     u_plotSetupMouseText)
    MOD(plotSetupFinish,                "plotSetupFinish()",                                       u_plotSetupFinish)
    MOD(plotSetAxis,                    "plotSetAxis(axis)",                                       u_plotSetAxis)
    MOD(plotSetAxes,                    "plotSetAxes(xAxis, yAxis)",                               u_plotSetAxes)
    MOD(plotSetNextAxisLimits,          "plotSetNextAxisLimits(axis, vMin, vMax, cond)",           u_plotSetNextAxisLimits)
    MOD(plotSetNextAxisLinks,           "plotSetNextAxisLinks(axis, ref)",                         u_plotSetNextAxisLinks)
    MOD(plotSetNextAxisToFit,           "plotSetNextAxisToFit(axis)",                              u_plotSetNextAxisToFit)
    MOD(plotSetNextAxesLimits,          "plotSetNextAxesLimits(xMin, xMax, yMin, yMax, cond)",     u_plotSetNextAxesLimits)
    MOD(plotSetNextAxesToFit,           "plotSetNextAxesToFit()",                                  u_plotSetNextAxesToFit)

    // plotSetupAxisTicks dispatcher: 2-arg (axis, values),
    // 4-arg (axis, values, labels, keepDefault), 6-arg
    // (axis, vMin, vMax, nTicks, labels, keepDefault).
    ZymValue pSatV = zym_createNativeClosure(vm, "plotSetupAxisTicks(axis, values)",                                            (void*)u_plotSetupAxisTicks_v,     context);
    roots.push(pSatV);
    ZymValue pSatVL = zym_createNativeClosure(vm, "plotSetupAxisTicks(axis, values, labels, keepDefault)",                      (void*)u_plotSetupAxisTicks_vl,    context);
    roots.push(pSatVL);
    ZymValue pSatR = zym_createNativeClosure(vm, "plotSetupAxisTicks(axis, vMin, vMax, nTicks, labels, keepDefault)",           (void*)u_plotSetupAxisTicks_range, context);
    roots.push(pSatR);
    ZymValue plotSetupAxisTicks = zym_createDispatcher(vm);
    roots.push(plotSetupAxisTicks);
    zym_addOverload(vm, plotSetupAxisTicks, pSatV);
    zym_addOverload(vm, plotSetupAxisTicks, pSatVL);
    zym_addOverload(vm, plotSetupAxisTicks, pSatR);

    // ---- SECTION 3a: Plot items (line family) — dispatchers ----
    //
    // Each item exposes the arity-routed shapes documented at the top of
    // Section 3a. The pattern is identical for every item:
    //   create each overload as a native closure -> root it ->
    //   wrap in a dispatcher -> add overloads -> register on `obj`.
    auto mkOv = [&](const char* sig, void* fn) {
        ZymValue c = zym_createNativeClosure(vm, sig, fn, context);
        roots.push(c);
        return c;
    };
    auto mkDisp = [&]() {
        ZymValue d = zym_createDispatcher(vm);
        roots.push(d);
        return d;
    };

    // plotLine
    ZymValue plotLine = mkDisp();
    zym_addOverload(vm, plotLine, mkOv("plotLine(label, ys)",                (void*)u_plotLine_y));
    zym_addOverload(vm, plotLine, mkOv("plotLine(label, xs, ys)",            (void*)u_plotLine_xy));
    zym_addOverload(vm, plotLine, mkOv("plotLine(label, xs, ys, flags)",     (void*)u_plotLine_xyf));

    // plotScatter
    ZymValue plotScatter = mkDisp();
    zym_addOverload(vm, plotScatter, mkOv("plotScatter(label, ys)",            (void*)u_plotScatter_y));
    zym_addOverload(vm, plotScatter, mkOv("plotScatter(label, xs, ys)",        (void*)u_plotScatter_xy));
    zym_addOverload(vm, plotScatter, mkOv("plotScatter(label, xs, ys, flags)", (void*)u_plotScatter_xyf));

    // plotStairs
    ZymValue plotStairs = mkDisp();
    zym_addOverload(vm, plotStairs, mkOv("plotStairs(label, ys)",            (void*)u_plotStairs_y));
    zym_addOverload(vm, plotStairs, mkOv("plotStairs(label, xs, ys)",        (void*)u_plotStairs_xy));
    zym_addOverload(vm, plotStairs, mkOv("plotStairs(label, xs, ys, flags)", (void*)u_plotStairs_xyf));

    // plotShaded
    ZymValue plotShaded = mkDisp();
    zym_addOverload(vm, plotShaded, mkOv("plotShaded(label, ys)",                 (void*)u_plotShaded_y));
    zym_addOverload(vm, plotShaded, mkOv("plotShaded(label, xs, ys)",             (void*)u_plotShaded_xy));
    zym_addOverload(vm, plotShaded, mkOv("plotShaded(label, xs, ys, yref)",       (void*)u_plotShaded_xyref));
    zym_addOverload(vm, plotShaded, mkOv("plotShaded(label, xs, ys1, ys2, flags)",(void*)u_plotShaded_band));

    // plotBars
    ZymValue plotBars = mkDisp();
    zym_addOverload(vm, plotBars, mkOv("plotBars(label, values)",                       (void*)u_plotBars_y));
    zym_addOverload(vm, plotBars, mkOv("plotBars(label, values, barSize)",              (void*)u_plotBars_ys));
    zym_addOverload(vm, plotBars, mkOv("plotBars(label, xs, ys, barSize, flags)",       (void*)u_plotBars_xys));

    // plotBarGroups — single shape (matrix data + label list)
    MOD(plotBarGroups, "plotBarGroups(labels, values, itemCount, groupCount, groupSize, shift, flags)", u_plotBarGroups)

    // plotStems
    ZymValue plotStems = mkDisp();
    zym_addOverload(vm, plotStems, mkOv("plotStems(label, values)",                  (void*)u_plotStems_y));
    zym_addOverload(vm, plotStems, mkOv("plotStems(label, xs, ys)",                  (void*)u_plotStems_xy));
    zym_addOverload(vm, plotStems, mkOv("plotStems(label, xs, ys, ref, flags)",      (void*)u_plotStems_xyref));

    // plotInfLines & plotDummy — single shapes, both take optional flags
    MOD(plotInfLines, "plotInfLines(label, values, flags)", u_plotInfLines)
    MOD(plotDummy,    "plotDummy(label, flags)",            u_plotDummy)

    // ---- SECTION 3b: Plot items (statistical family) — dispatchers ----

    // plotErrorBars — symmetric (xs, ys, err [, flags]) or asymmetric (xs, ys, neg, pos [, flags])
    ZymValue plotErrorBars = mkDisp();
    zym_addOverload(vm, plotErrorBars, mkOv("plotErrorBars(label, xs, ys, err)",            (void*)u_plotErrorBars_sym));
    zym_addOverload(vm, plotErrorBars, mkOv("plotErrorBars(label, xs, ys, err, flags)",     (void*)u_plotErrorBars_symF));
    zym_addOverload(vm, plotErrorBars, mkOv("plotErrorBars(label, xs, ys, neg, pos, flags)",(void*)u_plotErrorBars_asym));

    // plotPieChart / plotHeatmap / plotHistogram / plotHistogram2D / plotDigital — single shapes
    MOD(plotPieChart,    "plotPieChart(labels, values, x, y, radius, labelFmt, angle0, flags)", u_plotPieChart)
    MOD(plotHeatmap,     "plotHeatmap(label, values, rows, cols, scaleMin, scaleMax, labelFmt, xMin, yMin, xMax, yMax, flags)", u_plotHeatmap)
    MOD(plotHistogram,   "plotHistogram(label, values, bins, barScale, rangeMin, rangeMax, flags)", u_plotHistogram)
    MOD(plotHistogram2D, "plotHistogram2D(label, xs, ys, xBins, yBins, xMin, xMax, yMin, yMax, flags)", u_plotHistogram2D)
    MOD(plotDigital,     "plotDigital(label, xs, ys, flags)", u_plotDigital)

    // ---- SECTION 3c: Plot items (visual family) ----
    MOD(plotImage, "plotImage(label, tex, xMin, yMin, xMax, yMax, uv0, uv1, tint, flags)", u_plotImage)
    MOD(plotText,  "plotText(text, x, y, pixOffset, flags)",                              u_plotText)

#undef MOD

    zym_mapSet(vm, obj, "plot",      plot);
    zym_mapSet(vm, obj, "isInPlot",  isInPlot);
    zym_mapSet(vm, obj, "plotSetupAxis",                  plotSetupAxis);
    zym_mapSet(vm, obj, "plotSetupAxes",                  plotSetupAxes);
    zym_mapSet(vm, obj, "plotSetupAxisLimits",            plotSetupAxisLimits);
    zym_mapSet(vm, obj, "plotSetupAxesLimits",            plotSetupAxesLimits);
    zym_mapSet(vm, obj, "plotSetupAxisLinks",             plotSetupAxisLinks);
    zym_mapSet(vm, obj, "plotSetupAxisFormat",            plotSetupAxisFormat);
    zym_mapSet(vm, obj, "plotSetupAxisTicks",             plotSetupAxisTicks);
    zym_mapSet(vm, obj, "plotSetupAxisScale",             plotSetupAxisScale);
    zym_mapSet(vm, obj, "plotSetupAxisLimitsConstraints", plotSetupAxisLimitsConstraints);
    zym_mapSet(vm, obj, "plotSetupAxisZoomConstraints",   plotSetupAxisZoomConstraints);
    zym_mapSet(vm, obj, "plotSetupLegend",                plotSetupLegend);
    zym_mapSet(vm, obj, "plotSetupMouseText",             plotSetupMouseText);
    zym_mapSet(vm, obj, "plotSetupFinish",                plotSetupFinish);
    zym_mapSet(vm, obj, "plotSetAxis",                    plotSetAxis);
    zym_mapSet(vm, obj, "plotSetAxes",                    plotSetAxes);
    zym_mapSet(vm, obj, "plotSetNextAxisLimits",          plotSetNextAxisLimits);
    zym_mapSet(vm, obj, "plotSetNextAxisLinks",           plotSetNextAxisLinks);
    zym_mapSet(vm, obj, "plotSetNextAxisToFit",           plotSetNextAxisToFit);
    zym_mapSet(vm, obj, "plotSetNextAxesLimits",          plotSetNextAxesLimits);
    zym_mapSet(vm, obj, "plotSetNextAxesToFit",           plotSetNextAxesToFit);

    // Section 3a — plot items
    zym_mapSet(vm, obj, "plotLine",      plotLine);
    zym_mapSet(vm, obj, "plotScatter",   plotScatter);
    zym_mapSet(vm, obj, "plotStairs",    plotStairs);
    zym_mapSet(vm, obj, "plotShaded",    plotShaded);
    zym_mapSet(vm, obj, "plotBars",      plotBars);
    zym_mapSet(vm, obj, "plotBarGroups", plotBarGroups);
    zym_mapSet(vm, obj, "plotStems",     plotStems);
    zym_mapSet(vm, obj, "plotInfLines",  plotInfLines);
    zym_mapSet(vm, obj, "plotDummy",     plotDummy);

    // Section 3b — statistical plot items
    zym_mapSet(vm, obj, "plotErrorBars",   plotErrorBars);
    zym_mapSet(vm, obj, "plotPieChart",    plotPieChart);
    zym_mapSet(vm, obj, "plotHeatmap",     plotHeatmap);
    zym_mapSet(vm, obj, "plotHistogram",   plotHistogram);
    zym_mapSet(vm, obj, "plotHistogram2D", plotHistogram2D);
    zym_mapSet(vm, obj, "plotDigital",     plotDigital);

    // Section 3c — visual plot items
    zym_mapSet(vm, obj, "plotImage", plotImage);
    zym_mapSet(vm, obj, "plotText",  plotText);

    // ---- Enum constants ----
    //
    // Exposed as plain integers so scripts can OR / pass them directly.
    // Naming convention mirrors the existing ImGui constants
    // (`WINDOW_NO_TITLE_BAR` etc.): family prefix + UPPER_SNAKE name.

    // --- ImPlotFlags (`UI.PLOT_*`) — passed to ui.plot(title, w, h, flags, body)
    zym_mapSet(vm, obj, "PLOT_NONE",          zym_newNumber(ImPlotFlags_None));
    zym_mapSet(vm, obj, "PLOT_NO_TITLE",      zym_newNumber(ImPlotFlags_NoTitle));
    zym_mapSet(vm, obj, "PLOT_NO_LEGEND",     zym_newNumber(ImPlotFlags_NoLegend));
    zym_mapSet(vm, obj, "PLOT_NO_MOUSE_TEXT", zym_newNumber(ImPlotFlags_NoMouseText));
    zym_mapSet(vm, obj, "PLOT_NO_INPUTS",     zym_newNumber(ImPlotFlags_NoInputs));
    zym_mapSet(vm, obj, "PLOT_NO_MENUS",      zym_newNumber(ImPlotFlags_NoMenus));
    zym_mapSet(vm, obj, "PLOT_NO_BOX_SELECT", zym_newNumber(ImPlotFlags_NoBoxSelect));
    zym_mapSet(vm, obj, "PLOT_NO_FRAME",      zym_newNumber(ImPlotFlags_NoFrame));
    zym_mapSet(vm, obj, "PLOT_EQUAL",         zym_newNumber(ImPlotFlags_Equal));
    zym_mapSet(vm, obj, "PLOT_CROSSHAIRS",    zym_newNumber(ImPlotFlags_Crosshairs));
    zym_mapSet(vm, obj, "PLOT_CANVAS_ONLY",   zym_newNumber(ImPlotFlags_CanvasOnly));

    // --- ImPlotAxisFlags (`UI.PLOT_AXIS_*`) — passed to SetupAxis (Section 2)
    zym_mapSet(vm, obj, "PLOT_AXIS_NONE",            zym_newNumber(ImPlotAxisFlags_None));
    zym_mapSet(vm, obj, "PLOT_AXIS_NO_LABEL",        zym_newNumber(ImPlotAxisFlags_NoLabel));
    zym_mapSet(vm, obj, "PLOT_AXIS_NO_GRID_LINES",   zym_newNumber(ImPlotAxisFlags_NoGridLines));
    zym_mapSet(vm, obj, "PLOT_AXIS_NO_TICK_MARKS",   zym_newNumber(ImPlotAxisFlags_NoTickMarks));
    zym_mapSet(vm, obj, "PLOT_AXIS_NO_TICK_LABELS",  zym_newNumber(ImPlotAxisFlags_NoTickLabels));
    zym_mapSet(vm, obj, "PLOT_AXIS_NO_INITIAL_FIT",  zym_newNumber(ImPlotAxisFlags_NoInitialFit));
    zym_mapSet(vm, obj, "PLOT_AXIS_NO_MENUS",        zym_newNumber(ImPlotAxisFlags_NoMenus));
    zym_mapSet(vm, obj, "PLOT_AXIS_NO_SIDE_SWITCH",  zym_newNumber(ImPlotAxisFlags_NoSideSwitch));
    zym_mapSet(vm, obj, "PLOT_AXIS_NO_HIGHLIGHT",    zym_newNumber(ImPlotAxisFlags_NoHighlight));
    zym_mapSet(vm, obj, "PLOT_AXIS_OPPOSITE",        zym_newNumber(ImPlotAxisFlags_Opposite));
    zym_mapSet(vm, obj, "PLOT_AXIS_FOREGROUND",      zym_newNumber(ImPlotAxisFlags_Foreground));
    zym_mapSet(vm, obj, "PLOT_AXIS_INVERT",          zym_newNumber(ImPlotAxisFlags_Invert));
    zym_mapSet(vm, obj, "PLOT_AXIS_AUTO_FIT",        zym_newNumber(ImPlotAxisFlags_AutoFit));
    zym_mapSet(vm, obj, "PLOT_AXIS_RANGE_FIT",       zym_newNumber(ImPlotAxisFlags_RangeFit));
    zym_mapSet(vm, obj, "PLOT_AXIS_PAN_STRETCH",     zym_newNumber(ImPlotAxisFlags_PanStretch));
    zym_mapSet(vm, obj, "PLOT_AXIS_LOCK_MIN",        zym_newNumber(ImPlotAxisFlags_LockMin));
    zym_mapSet(vm, obj, "PLOT_AXIS_LOCK_MAX",        zym_newNumber(ImPlotAxisFlags_LockMax));
    zym_mapSet(vm, obj, "PLOT_AXIS_LOCK",            zym_newNumber(ImPlotAxisFlags_Lock));
    zym_mapSet(vm, obj, "PLOT_AXIS_NO_DECORATIONS",  zym_newNumber(ImPlotAxisFlags_NoDecorations));
    zym_mapSet(vm, obj, "PLOT_AXIS_AUX_DEFAULT",     zym_newNumber(ImPlotAxisFlags_AuxDefault));

    // --- ImPlotLegendFlags (`UI.PLOT_LEGEND_*`) — passed to SetupLegend
    zym_mapSet(vm, obj, "PLOT_LEGEND_NONE",               zym_newNumber(ImPlotLegendFlags_None));
    zym_mapSet(vm, obj, "PLOT_LEGEND_NO_BUTTONS",         zym_newNumber(ImPlotLegendFlags_NoButtons));
    zym_mapSet(vm, obj, "PLOT_LEGEND_NO_HIGHLIGHT_ITEM",  zym_newNumber(ImPlotLegendFlags_NoHighlightItem));
    zym_mapSet(vm, obj, "PLOT_LEGEND_NO_HIGHLIGHT_AXIS",  zym_newNumber(ImPlotLegendFlags_NoHighlightAxis));
    zym_mapSet(vm, obj, "PLOT_LEGEND_NO_MENUS",           zym_newNumber(ImPlotLegendFlags_NoMenus));
    zym_mapSet(vm, obj, "PLOT_LEGEND_OUTSIDE",            zym_newNumber(ImPlotLegendFlags_Outside));
    zym_mapSet(vm, obj, "PLOT_LEGEND_HORIZONTAL",         zym_newNumber(ImPlotLegendFlags_Horizontal));
    zym_mapSet(vm, obj, "PLOT_LEGEND_SORT",               zym_newNumber(ImPlotLegendFlags_Sort));
    zym_mapSet(vm, obj, "PLOT_LEGEND_REVERSE",            zym_newNumber(ImPlotLegendFlags_Reverse));

    // --- ImPlotSubplotFlags (`UI.PLOT_SUBPLOT_*`) — Section 6
    zym_mapSet(vm, obj, "PLOT_SUBPLOT_NONE",         zym_newNumber(ImPlotSubplotFlags_None));
    zym_mapSet(vm, obj, "PLOT_SUBPLOT_NO_TITLE",     zym_newNumber(ImPlotSubplotFlags_NoTitle));
    zym_mapSet(vm, obj, "PLOT_SUBPLOT_NO_LEGEND",    zym_newNumber(ImPlotSubplotFlags_NoLegend));
    zym_mapSet(vm, obj, "PLOT_SUBPLOT_NO_MENUS",     zym_newNumber(ImPlotSubplotFlags_NoMenus));
    zym_mapSet(vm, obj, "PLOT_SUBPLOT_NO_RESIZE",    zym_newNumber(ImPlotSubplotFlags_NoResize));
    zym_mapSet(vm, obj, "PLOT_SUBPLOT_NO_ALIGN",     zym_newNumber(ImPlotSubplotFlags_NoAlign));
    zym_mapSet(vm, obj, "PLOT_SUBPLOT_SHARE_ITEMS",  zym_newNumber(ImPlotSubplotFlags_ShareItems));
    zym_mapSet(vm, obj, "PLOT_SUBPLOT_LINK_ROWS",    zym_newNumber(ImPlotSubplotFlags_LinkRows));
    zym_mapSet(vm, obj, "PLOT_SUBPLOT_LINK_COLS",    zym_newNumber(ImPlotSubplotFlags_LinkCols));
    zym_mapSet(vm, obj, "PLOT_SUBPLOT_LINK_ALL_X",   zym_newNumber(ImPlotSubplotFlags_LinkAllX));
    zym_mapSet(vm, obj, "PLOT_SUBPLOT_LINK_ALL_Y",   zym_newNumber(ImPlotSubplotFlags_LinkAllY));
    zym_mapSet(vm, obj, "PLOT_SUBPLOT_COL_MAJOR",    zym_newNumber(ImPlotSubplotFlags_ColMajor));

    // --- ImPlotMouseTextFlags (`UI.PLOT_MOUSE_TEXT_*`)
    zym_mapSet(vm, obj, "PLOT_MOUSE_TEXT_NONE",        zym_newNumber(ImPlotMouseTextFlags_None));
    zym_mapSet(vm, obj, "PLOT_MOUSE_TEXT_NO_AUX_AXES", zym_newNumber(ImPlotMouseTextFlags_NoAuxAxes));
    zym_mapSet(vm, obj, "PLOT_MOUSE_TEXT_NO_FORMAT",   zym_newNumber(ImPlotMouseTextFlags_NoFormat));
    zym_mapSet(vm, obj, "PLOT_MOUSE_TEXT_SHOW_ALWAYS", zym_newNumber(ImPlotMouseTextFlags_ShowAlways));

    // --- ImPlotCond (`UI.PLOT_COND_*`)
    zym_mapSet(vm, obj, "PLOT_COND_NONE",   zym_newNumber(ImPlotCond_None));
    zym_mapSet(vm, obj, "PLOT_COND_ALWAYS", zym_newNumber(ImPlotCond_Always));
    zym_mapSet(vm, obj, "PLOT_COND_ONCE",   zym_newNumber(ImPlotCond_Once));

    // --- ImPlotCol (`UI.PLOT_COL_*`)
    zym_mapSet(vm, obj, "PLOT_COL_FRAME_BG",         zym_newNumber(ImPlotCol_FrameBg));
    zym_mapSet(vm, obj, "PLOT_COL_PLOT_BG",          zym_newNumber(ImPlotCol_PlotBg));
    zym_mapSet(vm, obj, "PLOT_COL_PLOT_BORDER",      zym_newNumber(ImPlotCol_PlotBorder));
    zym_mapSet(vm, obj, "PLOT_COL_LEGEND_BG",        zym_newNumber(ImPlotCol_LegendBg));
    zym_mapSet(vm, obj, "PLOT_COL_LEGEND_BORDER",    zym_newNumber(ImPlotCol_LegendBorder));
    zym_mapSet(vm, obj, "PLOT_COL_LEGEND_TEXT",      zym_newNumber(ImPlotCol_LegendText));
    zym_mapSet(vm, obj, "PLOT_COL_TITLE_TEXT",       zym_newNumber(ImPlotCol_TitleText));
    zym_mapSet(vm, obj, "PLOT_COL_INLAY_TEXT",       zym_newNumber(ImPlotCol_InlayText));
    zym_mapSet(vm, obj, "PLOT_COL_AXIS_TEXT",        zym_newNumber(ImPlotCol_AxisText));
    zym_mapSet(vm, obj, "PLOT_COL_AXIS_GRID",        zym_newNumber(ImPlotCol_AxisGrid));
    zym_mapSet(vm, obj, "PLOT_COL_AXIS_TICK",        zym_newNumber(ImPlotCol_AxisTick));
    zym_mapSet(vm, obj, "PLOT_COL_AXIS_BG",          zym_newNumber(ImPlotCol_AxisBg));
    zym_mapSet(vm, obj, "PLOT_COL_AXIS_BG_HOVERED",  zym_newNumber(ImPlotCol_AxisBgHovered));
    zym_mapSet(vm, obj, "PLOT_COL_AXIS_BG_ACTIVE",   zym_newNumber(ImPlotCol_AxisBgActive));
    zym_mapSet(vm, obj, "PLOT_COL_SELECTION",        zym_newNumber(ImPlotCol_Selection));
    zym_mapSet(vm, obj, "PLOT_COL_CROSSHAIRS",       zym_newNumber(ImPlotCol_Crosshairs));

    // --- ImPlotStyleVar (`UI.PLOT_STYLE_*`)
    zym_mapSet(vm, obj, "PLOT_STYLE_PLOT_DEFAULT_SIZE",    zym_newNumber(ImPlotStyleVar_PlotDefaultSize));
    zym_mapSet(vm, obj, "PLOT_STYLE_PLOT_MIN_SIZE",        zym_newNumber(ImPlotStyleVar_PlotMinSize));
    zym_mapSet(vm, obj, "PLOT_STYLE_PLOT_BORDER_SIZE",     zym_newNumber(ImPlotStyleVar_PlotBorderSize));
    zym_mapSet(vm, obj, "PLOT_STYLE_MINOR_ALPHA",          zym_newNumber(ImPlotStyleVar_MinorAlpha));
    zym_mapSet(vm, obj, "PLOT_STYLE_MAJOR_TICK_LEN",       zym_newNumber(ImPlotStyleVar_MajorTickLen));
    zym_mapSet(vm, obj, "PLOT_STYLE_MINOR_TICK_LEN",       zym_newNumber(ImPlotStyleVar_MinorTickLen));
    zym_mapSet(vm, obj, "PLOT_STYLE_MAJOR_TICK_SIZE",      zym_newNumber(ImPlotStyleVar_MajorTickSize));
    zym_mapSet(vm, obj, "PLOT_STYLE_MINOR_TICK_SIZE",      zym_newNumber(ImPlotStyleVar_MinorTickSize));
    zym_mapSet(vm, obj, "PLOT_STYLE_MAJOR_GRID_SIZE",      zym_newNumber(ImPlotStyleVar_MajorGridSize));
    zym_mapSet(vm, obj, "PLOT_STYLE_MINOR_GRID_SIZE",      zym_newNumber(ImPlotStyleVar_MinorGridSize));
    zym_mapSet(vm, obj, "PLOT_STYLE_PLOT_PADDING",         zym_newNumber(ImPlotStyleVar_PlotPadding));
    zym_mapSet(vm, obj, "PLOT_STYLE_LABEL_PADDING",        zym_newNumber(ImPlotStyleVar_LabelPadding));
    zym_mapSet(vm, obj, "PLOT_STYLE_LEGEND_PADDING",       zym_newNumber(ImPlotStyleVar_LegendPadding));
    zym_mapSet(vm, obj, "PLOT_STYLE_LEGEND_INNER_PADDING", zym_newNumber(ImPlotStyleVar_LegendInnerPadding));
    zym_mapSet(vm, obj, "PLOT_STYLE_LEGEND_SPACING",       zym_newNumber(ImPlotStyleVar_LegendSpacing));
    zym_mapSet(vm, obj, "PLOT_STYLE_MOUSE_POS_PADDING",    zym_newNumber(ImPlotStyleVar_MousePosPadding));
    zym_mapSet(vm, obj, "PLOT_STYLE_ANNOTATION_PADDING",   zym_newNumber(ImPlotStyleVar_AnnotationPadding));
    zym_mapSet(vm, obj, "PLOT_STYLE_FIT_PADDING",          zym_newNumber(ImPlotStyleVar_FitPadding));
    zym_mapSet(vm, obj, "PLOT_STYLE_DIGITAL_PADDING",      zym_newNumber(ImPlotStyleVar_DigitalPadding));
    zym_mapSet(vm, obj, "PLOT_STYLE_DIGITAL_SPACING",      zym_newNumber(ImPlotStyleVar_DigitalSpacing));

    // --- ImPlotScale (`UI.PLOT_SCALE_*`)
    zym_mapSet(vm, obj, "PLOT_SCALE_LINEAR", zym_newNumber(ImPlotScale_Linear));
    zym_mapSet(vm, obj, "PLOT_SCALE_TIME",   zym_newNumber(ImPlotScale_Time));
    zym_mapSet(vm, obj, "PLOT_SCALE_LOG10",  zym_newNumber(ImPlotScale_Log10));
    zym_mapSet(vm, obj, "PLOT_SCALE_SYMLOG", zym_newNumber(ImPlotScale_SymLog));

    // --- ImPlotMarker (`UI.PLOT_MARKER_*`)
    zym_mapSet(vm, obj, "PLOT_MARKER_NONE",     zym_newNumber(ImPlotMarker_None));
    zym_mapSet(vm, obj, "PLOT_MARKER_AUTO",     zym_newNumber(ImPlotMarker_Auto));
    zym_mapSet(vm, obj, "PLOT_MARKER_CIRCLE",   zym_newNumber(ImPlotMarker_Circle));
    zym_mapSet(vm, obj, "PLOT_MARKER_SQUARE",   zym_newNumber(ImPlotMarker_Square));
    zym_mapSet(vm, obj, "PLOT_MARKER_DIAMOND",  zym_newNumber(ImPlotMarker_Diamond));
    zym_mapSet(vm, obj, "PLOT_MARKER_UP",       zym_newNumber(ImPlotMarker_Up));
    zym_mapSet(vm, obj, "PLOT_MARKER_DOWN",     zym_newNumber(ImPlotMarker_Down));
    zym_mapSet(vm, obj, "PLOT_MARKER_LEFT",     zym_newNumber(ImPlotMarker_Left));
    zym_mapSet(vm, obj, "PLOT_MARKER_RIGHT",    zym_newNumber(ImPlotMarker_Right));
    zym_mapSet(vm, obj, "PLOT_MARKER_CROSS",    zym_newNumber(ImPlotMarker_Cross));
    zym_mapSet(vm, obj, "PLOT_MARKER_PLUS",     zym_newNumber(ImPlotMarker_Plus));
    zym_mapSet(vm, obj, "PLOT_MARKER_ASTERISK", zym_newNumber(ImPlotMarker_Asterisk));

    // --- ImPlotColormap (`UI.PLOT_COLORMAP_*`)
    zym_mapSet(vm, obj, "PLOT_COLORMAP_DEEP",     zym_newNumber(ImPlotColormap_Deep));
    zym_mapSet(vm, obj, "PLOT_COLORMAP_DARK",     zym_newNumber(ImPlotColormap_Dark));
    zym_mapSet(vm, obj, "PLOT_COLORMAP_PASTEL",   zym_newNumber(ImPlotColormap_Pastel));
    zym_mapSet(vm, obj, "PLOT_COLORMAP_PAIRED",   zym_newNumber(ImPlotColormap_Paired));
    zym_mapSet(vm, obj, "PLOT_COLORMAP_VIRIDIS",  zym_newNumber(ImPlotColormap_Viridis));
    zym_mapSet(vm, obj, "PLOT_COLORMAP_PLASMA",   zym_newNumber(ImPlotColormap_Plasma));
    zym_mapSet(vm, obj, "PLOT_COLORMAP_HOT",      zym_newNumber(ImPlotColormap_Hot));
    zym_mapSet(vm, obj, "PLOT_COLORMAP_COOL",     zym_newNumber(ImPlotColormap_Cool));
    zym_mapSet(vm, obj, "PLOT_COLORMAP_PINK",     zym_newNumber(ImPlotColormap_Pink));
    zym_mapSet(vm, obj, "PLOT_COLORMAP_JET",      zym_newNumber(ImPlotColormap_Jet));
    zym_mapSet(vm, obj, "PLOT_COLORMAP_TWILIGHT", zym_newNumber(ImPlotColormap_Twilight));
    zym_mapSet(vm, obj, "PLOT_COLORMAP_RDBU",     zym_newNumber(ImPlotColormap_RdBu));
    zym_mapSet(vm, obj, "PLOT_COLORMAP_BRBG",     zym_newNumber(ImPlotColormap_BrBG));
    zym_mapSet(vm, obj, "PLOT_COLORMAP_PIYG",     zym_newNumber(ImPlotColormap_PiYG));
    zym_mapSet(vm, obj, "PLOT_COLORMAP_SPECTRAL", zym_newNumber(ImPlotColormap_Spectral));
    zym_mapSet(vm, obj, "PLOT_COLORMAP_GREYS",    zym_newNumber(ImPlotColormap_Greys));

    // --- ImPlotLocation (`UI.PLOT_LOCATION_*`) — used by SetupLegend etc.
    zym_mapSet(vm, obj, "PLOT_LOCATION_CENTER",    zym_newNumber(ImPlotLocation_Center));
    zym_mapSet(vm, obj, "PLOT_LOCATION_NORTH",     zym_newNumber(ImPlotLocation_North));
    zym_mapSet(vm, obj, "PLOT_LOCATION_SOUTH",     zym_newNumber(ImPlotLocation_South));
    zym_mapSet(vm, obj, "PLOT_LOCATION_WEST",      zym_newNumber(ImPlotLocation_West));
    zym_mapSet(vm, obj, "PLOT_LOCATION_EAST",      zym_newNumber(ImPlotLocation_East));
    zym_mapSet(vm, obj, "PLOT_LOCATION_NORTHWEST", zym_newNumber(ImPlotLocation_NorthWest));
    zym_mapSet(vm, obj, "PLOT_LOCATION_NORTHEAST", zym_newNumber(ImPlotLocation_NorthEast));
    zym_mapSet(vm, obj, "PLOT_LOCATION_SOUTHWEST", zym_newNumber(ImPlotLocation_SouthWest));
    zym_mapSet(vm, obj, "PLOT_LOCATION_SOUTHEAST", zym_newNumber(ImPlotLocation_SouthEast));

    // --- ImPlotBin (`UI.PLOT_BIN_*`) — histogram bin-count sentinels
    zym_mapSet(vm, obj, "PLOT_BIN_SQRT",    zym_newNumber(ImPlotBin_Sqrt));
    zym_mapSet(vm, obj, "PLOT_BIN_STURGES", zym_newNumber(ImPlotBin_Sturges));
    zym_mapSet(vm, obj, "PLOT_BIN_RICE",    zym_newNumber(ImPlotBin_Rice));
    zym_mapSet(vm, obj, "PLOT_BIN_SCOTT",   zym_newNumber(ImPlotBin_Scott));

    // --- ImAxis (`UI.AXIS_*`) — axis selectors for Setup*/PlotLine etc.
    zym_mapSet(vm, obj, "AXIS_X1", zym_newNumber(ImAxis_X1));
    zym_mapSet(vm, obj, "AXIS_X2", zym_newNumber(ImAxis_X2));
    zym_mapSet(vm, obj, "AXIS_X3", zym_newNumber(ImAxis_X3));
    zym_mapSet(vm, obj, "AXIS_Y1", zym_newNumber(ImAxis_Y1));
    zym_mapSet(vm, obj, "AXIS_Y2", zym_newNumber(ImAxis_Y2));
    zym_mapSet(vm, obj, "AXIS_Y3", zym_newNumber(ImAxis_Y3));

    // --- ImPlotItemFlags (`UI.PLOT_ITEM_*`) — common to every plot item
    zym_mapSet(vm, obj, "PLOT_ITEM_NONE",      zym_newNumber(ImPlotItemFlags_None));
    zym_mapSet(vm, obj, "PLOT_ITEM_NO_LEGEND", zym_newNumber(ImPlotItemFlags_NoLegend));
    zym_mapSet(vm, obj, "PLOT_ITEM_NO_FIT",    zym_newNumber(ImPlotItemFlags_NoFit));

    // --- ImPlotLineFlags (`UI.PLOT_LINE_*`)
    zym_mapSet(vm, obj, "PLOT_LINE_NONE",     zym_newNumber(ImPlotLineFlags_None));
    zym_mapSet(vm, obj, "PLOT_LINE_SEGMENTS", zym_newNumber(ImPlotLineFlags_Segments));
    zym_mapSet(vm, obj, "PLOT_LINE_LOOP",     zym_newNumber(ImPlotLineFlags_Loop));
    zym_mapSet(vm, obj, "PLOT_LINE_SKIP_NAN", zym_newNumber(ImPlotLineFlags_SkipNaN));
    zym_mapSet(vm, obj, "PLOT_LINE_NO_CLIP",  zym_newNumber(ImPlotLineFlags_NoClip));
    zym_mapSet(vm, obj, "PLOT_LINE_SHADED",   zym_newNumber(ImPlotLineFlags_Shaded));

    // --- ImPlotScatterFlags (`UI.PLOT_SCATTER_*`)
    zym_mapSet(vm, obj, "PLOT_SCATTER_NONE",    zym_newNumber(ImPlotScatterFlags_None));
    zym_mapSet(vm, obj, "PLOT_SCATTER_NO_CLIP", zym_newNumber(ImPlotScatterFlags_NoClip));

    // --- ImPlotStairsFlags (`UI.PLOT_STAIRS_*`)
    zym_mapSet(vm, obj, "PLOT_STAIRS_NONE",     zym_newNumber(ImPlotStairsFlags_None));
    zym_mapSet(vm, obj, "PLOT_STAIRS_PRE_STEP", zym_newNumber(ImPlotStairsFlags_PreStep));
    zym_mapSet(vm, obj, "PLOT_STAIRS_SHADED",   zym_newNumber(ImPlotStairsFlags_Shaded));

    // --- ImPlotShadedFlags (`UI.PLOT_SHADED_*`)
    zym_mapSet(vm, obj, "PLOT_SHADED_NONE", zym_newNumber(ImPlotShadedFlags_None));

    // --- ImPlotBarsFlags (`UI.PLOT_BARS_*`)
    zym_mapSet(vm, obj, "PLOT_BARS_NONE",       zym_newNumber(ImPlotBarsFlags_None));
    zym_mapSet(vm, obj, "PLOT_BARS_HORIZONTAL", zym_newNumber(ImPlotBarsFlags_Horizontal));

    // --- ImPlotBarGroupsFlags (`UI.PLOT_BAR_GROUPS_*`)
    zym_mapSet(vm, obj, "PLOT_BAR_GROUPS_NONE",       zym_newNumber(ImPlotBarGroupsFlags_None));
    zym_mapSet(vm, obj, "PLOT_BAR_GROUPS_HORIZONTAL", zym_newNumber(ImPlotBarGroupsFlags_Horizontal));
    zym_mapSet(vm, obj, "PLOT_BAR_GROUPS_STACKED",    zym_newNumber(ImPlotBarGroupsFlags_Stacked));

    // --- ImPlotStemsFlags (`UI.PLOT_STEMS_*`)
    zym_mapSet(vm, obj, "PLOT_STEMS_NONE",       zym_newNumber(ImPlotStemsFlags_None));
    zym_mapSet(vm, obj, "PLOT_STEMS_HORIZONTAL", zym_newNumber(ImPlotStemsFlags_Horizontal));

    // --- ImPlotInfLinesFlags (`UI.PLOT_INF_LINES_*`)
    zym_mapSet(vm, obj, "PLOT_INF_LINES_NONE",       zym_newNumber(ImPlotInfLinesFlags_None));
    zym_mapSet(vm, obj, "PLOT_INF_LINES_HORIZONTAL", zym_newNumber(ImPlotInfLinesFlags_Horizontal));

    // --- ImPlotDummyFlags (`UI.PLOT_DUMMY_*`)
    zym_mapSet(vm, obj, "PLOT_DUMMY_NONE", zym_newNumber(ImPlotDummyFlags_None));

    // --- ImPlotErrorBarsFlags (`UI.PLOT_ERROR_BARS_*`)
    zym_mapSet(vm, obj, "PLOT_ERROR_BARS_NONE",       zym_newNumber(ImPlotErrorBarsFlags_None));
    zym_mapSet(vm, obj, "PLOT_ERROR_BARS_HORIZONTAL", zym_newNumber(ImPlotErrorBarsFlags_Horizontal));

    // --- ImPlotPieChartFlags (`UI.PLOT_PIE_CHART_*`)
    zym_mapSet(vm, obj, "PLOT_PIE_CHART_NONE",            zym_newNumber(ImPlotPieChartFlags_None));
    zym_mapSet(vm, obj, "PLOT_PIE_CHART_NORMALIZE",       zym_newNumber(ImPlotPieChartFlags_Normalize));
    zym_mapSet(vm, obj, "PLOT_PIE_CHART_IGNORE_HIDDEN",   zym_newNumber(ImPlotPieChartFlags_IgnoreHidden));
    zym_mapSet(vm, obj, "PLOT_PIE_CHART_EXPLODING",       zym_newNumber(ImPlotPieChartFlags_Exploding));
    zym_mapSet(vm, obj, "PLOT_PIE_CHART_NO_SLICE_BORDER", zym_newNumber(ImPlotPieChartFlags_NoSliceBorder));

    // --- ImPlotHeatmapFlags (`UI.PLOT_HEATMAP_*`)
    zym_mapSet(vm, obj, "PLOT_HEATMAP_NONE",      zym_newNumber(ImPlotHeatmapFlags_None));
    zym_mapSet(vm, obj, "PLOT_HEATMAP_COL_MAJOR", zym_newNumber(ImPlotHeatmapFlags_ColMajor));

    // --- ImPlotHistogramFlags (`UI.PLOT_HISTOGRAM_*`)
    zym_mapSet(vm, obj, "PLOT_HISTOGRAM_NONE",         zym_newNumber(ImPlotHistogramFlags_None));
    zym_mapSet(vm, obj, "PLOT_HISTOGRAM_HORIZONTAL",   zym_newNumber(ImPlotHistogramFlags_Horizontal));
    zym_mapSet(vm, obj, "PLOT_HISTOGRAM_CUMULATIVE",   zym_newNumber(ImPlotHistogramFlags_Cumulative));
    zym_mapSet(vm, obj, "PLOT_HISTOGRAM_DENSITY",      zym_newNumber(ImPlotHistogramFlags_Density));
    zym_mapSet(vm, obj, "PLOT_HISTOGRAM_NO_OUTLIERS",  zym_newNumber(ImPlotHistogramFlags_NoOutliers));
    zym_mapSet(vm, obj, "PLOT_HISTOGRAM_COL_MAJOR",    zym_newNumber(ImPlotHistogramFlags_ColMajor));

    // --- ImPlotDigitalFlags (`UI.PLOT_DIGITAL_*`)
    zym_mapSet(vm, obj, "PLOT_DIGITAL_NONE", zym_newNumber(ImPlotDigitalFlags_None));

    // --- ImPlotImageFlags (`UI.PLOT_IMAGE_*`)
    zym_mapSet(vm, obj, "PLOT_IMAGE_NONE", zym_newNumber(ImPlotImageFlags_None));

    // --- ImPlotTextFlags (`UI.PLOT_TEXT_*`)
    zym_mapSet(vm, obj, "PLOT_TEXT_NONE",     zym_newNumber(ImPlotTextFlags_None));
    zym_mapSet(vm, obj, "PLOT_TEXT_VERTICAL", zym_newNumber(ImPlotTextFlags_Vertical));
}
