// implot.cpp — every `u_plot*` ImPlot wrapper + the
// `registerImPlotBindings` function called from `ui.cpp`'s
// `nativeUi_create`.
//
// Compiled only when ZYM_UI_ENABLED is defined.
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

// ==== SECTION 4: Tools (DragPoint/DragLine/DragRect/Annotation/Tag) =======
//
// All of these draw interactive overlays inside a `BeginPlot/EndPlot`
// scope, so they require `requirePlot()`. The drag tools take live
// `double*` arguments that ImPlot mutates on drag — we use the same
// ref-list convention as `plotSetupAxisLinks`: a single-element
// `[v]` list for single-double tools, `[x, y]` for `DragPoint`, and
// `[x1, y1, x2, y2]` for `DragRect`. Values are read in, the address
// is handed to ImPlot, and the (possibly mutated) values are written
// back into the same list before returning. Persistent live drag
// state therefore "just works" as long as the script reuses the same
// list across frames.
//
// Color is passed as a packed `UI.color(...)` ImU32 (same convention
// as `UI.image` / `plotImage`'s tint) — we decode to `ImVec4` with
// `plotU32ToVec4`. Annotations and Tags share the color convention.
//
// Annotation / TagX / TagY have an optional trailing `text` string
// argument: when present, the formatted-text overload is used; when
// `null` or omitted, the no-text overload (plain colored dot/tag with
// auto-formatted value) runs.

// --- DragPoint(id, ref=[x,y], col, size?, flags?) ---
// Returns `true` while the point is being dragged this frame.
ZymValue u_plotDragPoint(ZymVM* vm, ZymValue, ZymValue idV, ZymValue refV,
                         ZymValue colV, ZymValue sizeV, ZymValue flagsV) {
    int id; if (!reqInt(vm, idV, "ui.plotDragPoint(id, [x,y], col, size?, flags?)", &id)) return ZYM_ERROR;
    if (!zym_isList(refV) || zym_listLength(refV) < 2) {
        zym_runtimeError(vm, "ui.plotDragPoint: ref must be a 2-element list [x, y]");
        return ZYM_ERROR;
    }
    if (!requirePlot(vm, "ui.plotDragPoint")) return ZYM_ERROR;
    ZymValue xv = zym_listGet(vm, refV, 0);
    ZymValue yv = zym_listGet(vm, refV, 1);
    if (!zym_isNumber(xv) || !zym_isNumber(yv)) {
        zym_runtimeError(vm, "ui.plotDragPoint: ref elements must be numbers");
        return ZYM_ERROR;
    }
    double x = zym_asNumber(xv);
    double y = zym_asNumber(yv);
    ImVec4 col = plotU32ToVec4(optU32(colV, IM_COL32_WHITE));
    double size = optNum(sizeV, 4.0);
    int flags = optInt(flagsV, 0);
    bool held = ImPlot::DragPoint(id, &x, &y, col, (float)size, (ImPlotDragToolFlags)flags);
    zym_listSet(vm, refV, 0, zym_newNumber(x));
    zym_listSet(vm, refV, 1, zym_newNumber(y));
    return zym_newBool(held);
}

// --- DragLineX(id, ref=[x], col, thickness?, flags?) ---
ZymValue u_plotDragLineX(ZymVM* vm, ZymValue, ZymValue idV, ZymValue refV,
                         ZymValue colV, ZymValue thickV, ZymValue flagsV) {
    int id; if (!reqInt(vm, idV, "ui.plotDragLineX(id, [x], col, thickness?, flags?)", &id)) return ZYM_ERROR;
    if (!zym_isList(refV) || zym_listLength(refV) < 1) {
        zym_runtimeError(vm, "ui.plotDragLineX: ref must be a 1-element list [x]");
        return ZYM_ERROR;
    }
    if (!requirePlot(vm, "ui.plotDragLineX")) return ZYM_ERROR;
    ZymValue v0 = zym_listGet(vm, refV, 0);
    if (!zym_isNumber(v0)) {
        zym_runtimeError(vm, "ui.plotDragLineX: ref[0] must be a number");
        return ZYM_ERROR;
    }
    double x = zym_asNumber(v0);
    ImVec4 col = plotU32ToVec4(optU32(colV, IM_COL32_WHITE));
    double thickness = optNum(thickV, 1.0);
    int flags = optInt(flagsV, 0);
    bool held = ImPlot::DragLineX(id, &x, col, (float)thickness, (ImPlotDragToolFlags)flags);
    zym_listSet(vm, refV, 0, zym_newNumber(x));
    return zym_newBool(held);
}

// --- DragLineY(id, ref=[y], col, thickness?, flags?) ---
ZymValue u_plotDragLineY(ZymVM* vm, ZymValue, ZymValue idV, ZymValue refV,
                         ZymValue colV, ZymValue thickV, ZymValue flagsV) {
    int id; if (!reqInt(vm, idV, "ui.plotDragLineY(id, [y], col, thickness?, flags?)", &id)) return ZYM_ERROR;
    if (!zym_isList(refV) || zym_listLength(refV) < 1) {
        zym_runtimeError(vm, "ui.plotDragLineY: ref must be a 1-element list [y]");
        return ZYM_ERROR;
    }
    if (!requirePlot(vm, "ui.plotDragLineY")) return ZYM_ERROR;
    ZymValue v0 = zym_listGet(vm, refV, 0);
    if (!zym_isNumber(v0)) {
        zym_runtimeError(vm, "ui.plotDragLineY: ref[0] must be a number");
        return ZYM_ERROR;
    }
    double y = zym_asNumber(v0);
    ImVec4 col = plotU32ToVec4(optU32(colV, IM_COL32_WHITE));
    double thickness = optNum(thickV, 1.0);
    int flags = optInt(flagsV, 0);
    bool held = ImPlot::DragLineY(id, &y, col, (float)thickness, (ImPlotDragToolFlags)flags);
    zym_listSet(vm, refV, 0, zym_newNumber(y));
    return zym_newBool(held);
}

// --- DragRect(id, ref=[x1,y1,x2,y2], col, flags?) ---
ZymValue u_plotDragRect(ZymVM* vm, ZymValue, ZymValue idV, ZymValue refV,
                        ZymValue colV, ZymValue flagsV) {
    int id; if (!reqInt(vm, idV, "ui.plotDragRect(id, [x1,y1,x2,y2], col, flags?)", &id)) return ZYM_ERROR;
    if (!zym_isList(refV) || zym_listLength(refV) < 4) {
        zym_runtimeError(vm, "ui.plotDragRect: ref must be a 4-element list [x1, y1, x2, y2]");
        return ZYM_ERROR;
    }
    if (!requirePlot(vm, "ui.plotDragRect")) return ZYM_ERROR;
    double v[4];
    for (int i = 0; i < 4; i++) {
        ZymValue e = zym_listGet(vm, refV, i);
        if (!zym_isNumber(e)) {
            zym_runtimeError(vm, "ui.plotDragRect: ref[%d] must be a number", i);
            return ZYM_ERROR;
        }
        v[i] = zym_asNumber(e);
    }
    ImVec4 col = plotU32ToVec4(optU32(colV, IM_COL32_WHITE));
    int flags = optInt(flagsV, 0);
    bool held = ImPlot::DragRect(id, &v[0], &v[1], &v[2], &v[3], col, (ImPlotDragToolFlags)flags);
    for (int i = 0; i < 4; i++)
        zym_listSet(vm, refV, i, zym_newNumber(v[i]));
    return zym_newBool(held);
}

// --- Annotation(x, y, col, pixOffset?, clamp?, text?, round?) ---
// `text` is optional: when present, uses the text-formatted overload;
// when null/omitted, uses the round-only no-text overload.
ZymValue u_plotAnnotation(ZymVM* vm, ZymValue, ZymValue xV, ZymValue yV,
                          ZymValue colV, ZymValue pixOffsetV, ZymValue clampV,
                          ZymValue textV, ZymValue roundV) {
    double x, y;
    if (!reqNum(vm, xV, "ui.plotAnnotation x", &x)) return ZYM_ERROR;
    if (!reqNum(vm, yV, "ui.plotAnnotation y", &y)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotAnnotation")) return ZYM_ERROR;
    ImVec4 col = plotU32ToVec4(optU32(colV, IM_COL32_WHITE));
    ImVec2 pixOffset;
    if (!plotOptVec2(vm, pixOffsetV, ImVec2(0, 0), "ui.plotAnnotation pixOffset", &pixOffset)) return ZYM_ERROR;
    bool clamp = optBool(clampV, false);
    if (zym_isNull(textV)) {
        bool round = optBool(roundV, false);
        ImPlot::Annotation(x, y, col, pixOffset, clamp, round);
    } else {
        std::string text;
        if (!reqStr(vm, textV, "ui.plotAnnotation text", &text)) return ZYM_ERROR;
        ImPlot::Annotation(x, y, col, pixOffset, clamp, "%s", text.c_str());
    }
    return zym_newNull();
}

// --- TagX(x, col, text?, round?) ---
ZymValue u_plotTagX(ZymVM* vm, ZymValue, ZymValue xV, ZymValue colV,
                    ZymValue textV, ZymValue roundV) {
    double x;
    if (!reqNum(vm, xV, "ui.plotTagX(x, col, text?, round?)", &x)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotTagX")) return ZYM_ERROR;
    ImVec4 col = plotU32ToVec4(optU32(colV, IM_COL32_WHITE));
    if (zym_isNull(textV)) {
        bool round = optBool(roundV, false);
        ImPlot::TagX(x, col, round);
    } else {
        std::string text;
        if (!reqStr(vm, textV, "ui.plotTagX text", &text)) return ZYM_ERROR;
        ImPlot::TagX(x, col, "%s", text.c_str());
    }
    return zym_newNull();
}

// --- TagY(y, col, text?, round?) ---
ZymValue u_plotTagY(ZymVM* vm, ZymValue, ZymValue yV, ZymValue colV,
                    ZymValue textV, ZymValue roundV) {
    double y;
    if (!reqNum(vm, yV, "ui.plotTagY(y, col, text?, round?)", &y)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotTagY")) return ZYM_ERROR;
    ImVec4 col = plotU32ToVec4(optU32(colV, IM_COL32_WHITE));
    if (zym_isNull(textV)) {
        bool round = optBool(roundV, false);
        ImPlot::TagY(y, col, round);
    } else {
        std::string text;
        if (!reqStr(vm, textV, "ui.plotTagY text", &text)) return ZYM_ERROR;
        ImPlot::TagY(y, col, "%s", text.c_str());
    }
    return zym_newNull();
}

// ==== SECTION 5: Style stacks + Colormaps + Legend ========================
//
// Scoped, body-callback style/colormap wrappers — mirrors the ImGui
// `withStyleColor` / `withStyleVar` / `withFont` idiom in `imgui.cpp`
// rather than exposing raw `PushStyleColor` / `PopStyleColor` /
// `PushStyleVar` / `PopStyleVar` / `PushColormap` / `PopColormap` to
// scripts. Each `with*` call collects the requested state from a map
// (or single value for `withColormap`), pushes everything, runs the
// `body` closure, and unconditionally pops the matching count back off
// the stack — even if the body raises a runtime error.
//
// Legend popup uses the same body-callback shape as `UI.plot` /
// `UI.window`: open is bool, body runs only when true, `EndLegendPopup`
// must NOT be called when Begin returned false (ImPlot contract).

// --- StyleColorsAuto/Classic/Dark/Light (applied to the global style)
ZymValue u_plotStyleColorsAuto(ZymVM*, ZymValue)    { ImPlot::StyleColorsAuto(nullptr);    return zym_newNull(); }
ZymValue u_plotStyleColorsClassic(ZymVM*, ZymValue) { ImPlot::StyleColorsClassic(nullptr); return zym_newNull(); }
ZymValue u_plotStyleColorsDark(ZymVM*, ZymValue)    { ImPlot::StyleColorsDark(nullptr);    return zym_newNull(); }
ZymValue u_plotStyleColorsLight(ZymVM*, ZymValue)   { ImPlot::StyleColorsLight(nullptr);   return zym_newNull(); }

// ---- Slot lookup tables (string name -> ImPlot enum) --------------------

struct PlotColSlot { const char* name; ImPlotCol slot; };
const PlotColSlot kPlotColSlots[] = {
    { "FrameBg",       ImPlotCol_FrameBg },
    { "PlotBg",        ImPlotCol_PlotBg },
    { "PlotBorder",    ImPlotCol_PlotBorder },
    { "LegendBg",      ImPlotCol_LegendBg },
    { "LegendBorder",  ImPlotCol_LegendBorder },
    { "LegendText",    ImPlotCol_LegendText },
    { "TitleText",     ImPlotCol_TitleText },
    { "InlayText",     ImPlotCol_InlayText },
    { "AxisText",      ImPlotCol_AxisText },
    { "AxisGrid",      ImPlotCol_AxisGrid },
    { "AxisTick",      ImPlotCol_AxisTick },
    { "AxisBg",        ImPlotCol_AxisBg },
    { "AxisBgHovered", ImPlotCol_AxisBgHovered },
    { "AxisBgActive",  ImPlotCol_AxisBgActive },
    { "Selection",     ImPlotCol_Selection },
    { "Crosshairs",    ImPlotCol_Crosshairs },
};
const size_t kPlotColSlotsCount = sizeof(kPlotColSlots) / sizeof(kPlotColSlots[0]);

bool lookupPlotColSlot(const char* key, ImPlotCol* out) {
    if (!key) return false;
    for (size_t i = 0; i < kPlotColSlotsCount; i++) {
        if (std::strcmp(kPlotColSlots[i].name, key) == 0) {
            *out = kPlotColSlots[i].slot;
            return true;
        }
    }
    return false;
}

// kind: 1 = float, 2 = ImVec2
struct PlotVarSlot { const char* name; ImPlotStyleVar var; int kind; };
const PlotVarSlot kPlotVarSlots[] = {
    { "PlotDefaultSize",    ImPlotStyleVar_PlotDefaultSize,    2 },
    { "PlotMinSize",        ImPlotStyleVar_PlotMinSize,        2 },
    { "PlotBorderSize",     ImPlotStyleVar_PlotBorderSize,     1 },
    { "MinorAlpha",         ImPlotStyleVar_MinorAlpha,         1 },
    { "MajorTickLen",       ImPlotStyleVar_MajorTickLen,       2 },
    { "MinorTickLen",       ImPlotStyleVar_MinorTickLen,       2 },
    { "MajorTickSize",      ImPlotStyleVar_MajorTickSize,      2 },
    { "MinorTickSize",      ImPlotStyleVar_MinorTickSize,      2 },
    { "MajorGridSize",      ImPlotStyleVar_MajorGridSize,      2 },
    { "MinorGridSize",      ImPlotStyleVar_MinorGridSize,      2 },
    { "PlotPadding",        ImPlotStyleVar_PlotPadding,        2 },
    { "LabelPadding",       ImPlotStyleVar_LabelPadding,       2 },
    { "LegendPadding",      ImPlotStyleVar_LegendPadding,      2 },
    { "LegendInnerPadding", ImPlotStyleVar_LegendInnerPadding, 2 },
    { "LegendSpacing",      ImPlotStyleVar_LegendSpacing,      2 },
    { "MousePosPadding",    ImPlotStyleVar_MousePosPadding,    2 },
    { "AnnotationPadding",  ImPlotStyleVar_AnnotationPadding,  2 },
    { "FitPadding",         ImPlotStyleVar_FitPadding,         2 },
    { "DigitalPadding",     ImPlotStyleVar_DigitalPadding,     1 },
    { "DigitalSpacing",     ImPlotStyleVar_DigitalSpacing,     1 },
};
const size_t kPlotVarSlotsCount = sizeof(kPlotVarSlots) / sizeof(kPlotVarSlots[0]);

bool lookupPlotVarSlot(const char* key, ImPlotStyleVar* out, int* outKind) {
    if (!key) return false;
    for (size_t i = 0; i < kPlotVarSlotsCount; i++) {
        if (std::strcmp(kPlotVarSlots[i].name, key) == 0) {
            *out     = kPlotVarSlots[i].var;
            *outKind = kPlotVarSlots[i].kind;
            return true;
        }
    }
    return false;
}

// --- withStyleColor(map, body) ------------------------------------------
//
// `map` maps slot-name string -> color (packed `UI.color(...)` int OR
// `[r,g,b]` / `[r,g,b,a]` list). We collect first, then push all,
// run body, pop the matching count.

struct PlotStyleColorCollect {
    ZymVM* vm;
    bool   ok;
    std::vector<std::pair<ImPlotCol, ImVec4>> entries;
};

bool plotStyleColorIter(ZymVM* vm, const char* key, ZymValue val, void* ud) {
    auto* c = static_cast<PlotStyleColorCollect*>(ud);
    ImPlotCol slot;
    if (!lookupPlotColSlot(key, &slot)) {
        zym_runtimeError(vm, "ui.plotWithStyleColor: unknown color slot '%s'", key);
        c->ok = false;
        return false;
    }
    float rgba[4] = { 0, 0, 0, 1.0f };
    if (refReadColor(vm, val, "ui.plotWithStyleColor", rgba) == 0) {
        c->ok = false;
        return false;
    }
    c->entries.emplace_back(slot, ImVec4(rgba[0], rgba[1], rgba[2], rgba[3]));
    return true;
}

ZymValue u_plotWithStyleColor(ZymVM* vm, ZymValue, ZymValue mapV, ZymValue bodyV) {
    if (!zym_isMap(mapV)) {
        zym_runtimeError(vm, "ui.plotWithStyleColor(map, body): map must be a map");
        return ZYM_ERROR;
    }
    if (!reqCallable(vm, bodyV, "ui.plotWithStyleColor(map, body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.plotWithStyleColor")) return ZYM_ERROR;

    PlotStyleColorCollect c{ vm, true, {} };
    zym_mapForEach(vm, mapV, plotStyleColorIter, &c);
    if (!c.ok) return ZYM_ERROR;

    for (auto& e : c.entries) ImPlot::PushStyleColor(e.first, e.second);
    zym_pushRoot(vm, bodyV);
    ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
    zym_popRoot(vm);
    if (!c.entries.empty()) ImPlot::PopStyleColor((int)c.entries.size());
    if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    return zym_newNull();
}

// --- withStyleVar(map, body) --------------------------------------------
//
// `map` maps var-name string -> number (scalar var) or [x, y] list
// (ImVec2 var). The slot table tells us which kind is which.

struct PlotStyleVarCollect {
    ZymVM* vm;
    bool   ok;
    int    pushCount;
};

bool plotStyleVarIter(ZymVM* vm, const char* key, ZymValue val, void* ud) {
    auto* c = static_cast<PlotStyleVarCollect*>(ud);
    ImPlotStyleVar var; int kind;
    if (!lookupPlotVarSlot(key, &var, &kind)) {
        zym_runtimeError(vm, "ui.plotWithStyleVar: unknown style var '%s'", key);
        c->ok = false;
        return false;
    }
    if (kind == 1) {
        double d;
        if (!reqNum(vm, val, "ui.plotWithStyleVar (scalar)", &d)) {
            c->ok = false;
            return false;
        }
        ImPlot::PushStyleVar(var, (float)d);
        c->pushCount++;
    } else {
        if (!zym_isList(val) || zym_listLength(val) < 2) {
            zym_runtimeError(vm,
                "ui.plotWithStyleVar: '%s' expects [x, y] list", key);
            c->ok = false;
            return false;
        }
        ZymValue xV = zym_listGet(vm, val, 0);
        ZymValue yV = zym_listGet(vm, val, 1);
        if (!zym_isNumber(xV) || !zym_isNumber(yV)) {
            zym_runtimeError(vm,
                "ui.plotWithStyleVar: '%s' [x, y] elements must be numbers", key);
            c->ok = false;
            return false;
        }
        ImPlot::PushStyleVar(var, ImVec2((float)zym_asNumber(xV),
                                         (float)zym_asNumber(yV)));
        c->pushCount++;
    }
    return true;
}

ZymValue u_plotWithStyleVar(ZymVM* vm, ZymValue, ZymValue mapV, ZymValue bodyV) {
    if (!zym_isMap(mapV)) {
        zym_runtimeError(vm, "ui.plotWithStyleVar(map, body): map must be a map");
        return ZYM_ERROR;
    }
    if (!reqCallable(vm, bodyV, "ui.plotWithStyleVar(map, body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.plotWithStyleVar")) return ZYM_ERROR;

    PlotStyleVarCollect c{ vm, true, 0 };
    zym_mapForEach(vm, mapV, plotStyleVarIter, &c);
    if (!c.ok) {
        if (c.pushCount > 0) ImPlot::PopStyleVar(c.pushCount);
        return ZYM_ERROR;
    }

    zym_pushRoot(vm, bodyV);
    ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
    zym_popRoot(vm);
    if (c.pushCount > 0) ImPlot::PopStyleVar(c.pushCount);
    if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    return zym_newNull();
}

// --- withColormap(cmapOrName, body) -------------------------------------
//
// Scoped `PushColormap` / `PopColormap`. Accepts either an int cmap
// index (one of the `UI.PLOT_COLORMAP_*` constants or a value returned
// by `UI.plotAddColormap`) or a string colormap name.

ZymValue u_plotWithColormap(ZymVM* vm, ZymValue, ZymValue cmapV, ZymValue bodyV) {
    if (!reqCallable(vm, bodyV, "ui.plotWithColormap(cmapOrName, body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.plotWithColormap")) return ZYM_ERROR;

    if (zym_isString(cmapV)) {
        ImPlot::PushColormap(zym_asCString(cmapV));
    } else if (zym_isNumber(cmapV)) {
        ImPlot::PushColormap((ImPlotColormap)(int)zym_asNumber(cmapV));
    } else {
        zym_runtimeError(vm,
            "ui.plotWithColormap(cmapOrName, body): cmapOrName must be an int or string");
        return ZYM_ERROR;
    }

    zym_pushRoot(vm, bodyV);
    ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
    zym_popRoot(vm);
    ImPlot::PopColormap(1);
    if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    return zym_newNull();
}

// --- GetLastItemColor() -> ImU32 (packed UI.color form) ---
ZymValue u_plotGetLastItemColor(ZymVM*, ZymValue) {
    ImVec4 c = ImPlot::GetLastItemColor();
    ImU32 packed = IM_COL32(
        (int)(c.x * 255.0f + 0.5f),
        (int)(c.y * 255.0f + 0.5f),
        (int)(c.z * 255.0f + 0.5f),
        (int)(c.w * 255.0f + 0.5f));
    return zym_newNumber((double)packed);
}

// --- GetStyleColorName(idx) -> string ---
ZymValue u_plotGetStyleColorName(ZymVM* vm, ZymValue, ZymValue idxV) {
    int idx; if (!reqInt(vm, idxV, "ui.plotGetStyleColorName(idx)", &idx)) return ZYM_ERROR;
    const char* s = ImPlot::GetStyleColorName((ImPlotCol)idx);
    return zym_newString(vm, s ? s : "");
}

// --- GetMarkerName(idx) -> string ---
ZymValue u_plotGetMarkerName(ZymVM* vm, ZymValue, ZymValue idxV) {
    int idx; if (!reqInt(vm, idxV, "ui.plotGetMarkerName(idx)", &idx)) return ZYM_ERROR;
    const char* s = ImPlot::GetMarkerName((ImPlotMarker)idx);
    return zym_newString(vm, s ? s : "");
}

// --- NextMarker() -> int (advances the current plot's marker) ---
ZymValue u_plotNextMarker(ZymVM* vm, ZymValue) {
    if (!requirePlot(vm, "ui.plotNextMarker")) return ZYM_ERROR;
    return zym_newNumber((double)ImPlot::NextMarker());
}

// ---- Colormaps -----------------------------------------------------------

// --- AddColormap(name, cols, qual?) — cols is a list of packed UI.color ImU32 ---
ZymValue u_plotAddColormap(ZymVM* vm, ZymValue, ZymValue nameV, ZymValue colsV, ZymValue qualV) {
    std::string name;
    if (!reqStr(vm, nameV, "ui.plotAddColormap(name, cols, qual?)", &name)) return ZYM_ERROR;
    if (!zym_isList(colsV)) {
        zym_runtimeError(vm, "ui.plotAddColormap: cols must be a list of packed UI.color values");
        return ZYM_ERROR;
    }
    int n = zym_listLength(colsV);
    if (n < 2) {
        zym_runtimeError(vm, "ui.plotAddColormap: cols must contain at least 2 colors");
        return ZYM_ERROR;
    }
    std::vector<ImU32> cols((size_t)n);
    for (int i = 0; i < n; i++) {
        ZymValue e = zym_listGet(vm, colsV, i);
        if (!zym_isNumber(e)) {
            zym_runtimeError(vm, "ui.plotAddColormap: cols[%d] is not a number", i);
            return ZYM_ERROR;
        }
        cols[(size_t)i] = (ImU32)zym_asNumber(e);
    }
    bool qual = optBool(qualV, true);
    ImPlotColormap cmap = ImPlot::AddColormap(name.c_str(), cols.data(), n, qual);
    return zym_newNumber((double)cmap);
}

// --- GetColormapCount() -> int ---
ZymValue u_plotGetColormapCount(ZymVM*, ZymValue) {
    return zym_newNumber((double)ImPlot::GetColormapCount());
}

// --- GetColormapName(cmap) -> string ---
ZymValue u_plotGetColormapName(ZymVM* vm, ZymValue, ZymValue cmapV) {
    int cmap; if (!reqInt(vm, cmapV, "ui.plotGetColormapName(cmap)", &cmap)) return ZYM_ERROR;
    const char* s = ImPlot::GetColormapName((ImPlotColormap)cmap);
    return zym_newString(vm, s ? s : "");
}

// --- GetColormapIndex(name) -> int ---
ZymValue u_plotGetColormapIndex(ZymVM* vm, ZymValue, ZymValue nameV) {
    std::string name;
    if (!reqStr(vm, nameV, "ui.plotGetColormapIndex(name)", &name)) return ZYM_ERROR;
    return zym_newNumber((double)ImPlot::GetColormapIndex(name.c_str()));
}

// --- NextColormapColor() -> packed UI.color ImU32 ---
ZymValue u_plotNextColormapColor(ZymVM*, ZymValue) {
    ImVec4 c = ImPlot::NextColormapColor();
    ImU32 packed = IM_COL32(
        (int)(c.x * 255.0f + 0.5f),
        (int)(c.y * 255.0f + 0.5f),
        (int)(c.z * 255.0f + 0.5f),
        (int)(c.w * 255.0f + 0.5f));
    return zym_newNumber((double)packed);
}

// --- GetColormapSize(cmap?) -> int ---
ZymValue u_plotGetColormapSize(ZymVM* vm, ZymValue, ZymValue cmapV) {
    (void)vm;
    int cmap = optInt(cmapV, IMPLOT_AUTO);
    return zym_newNumber((double)ImPlot::GetColormapSize((ImPlotColormap)cmap));
}

// --- GetColormapColor(idx, cmap?) -> packed UI.color ImU32 ---
ZymValue u_plotGetColormapColor(ZymVM* vm, ZymValue, ZymValue idxV, ZymValue cmapV) {
    int idx; if (!reqInt(vm, idxV, "ui.plotGetColormapColor(idx, cmap?)", &idx)) return ZYM_ERROR;
    int cmap = optInt(cmapV, IMPLOT_AUTO);
    ImVec4 c = ImPlot::GetColormapColor(idx, (ImPlotColormap)cmap);
    ImU32 packed = IM_COL32(
        (int)(c.x * 255.0f + 0.5f),
        (int)(c.y * 255.0f + 0.5f),
        (int)(c.z * 255.0f + 0.5f),
        (int)(c.w * 255.0f + 0.5f));
    return zym_newNumber((double)packed);
}

// --- SampleColormap(t, cmap?) -> packed UI.color ImU32 ---
ZymValue u_plotSampleColormap(ZymVM* vm, ZymValue, ZymValue tV, ZymValue cmapV) {
    double t; if (!reqNum(vm, tV, "ui.plotSampleColormap(t, cmap?)", &t)) return ZYM_ERROR;
    int cmap = optInt(cmapV, IMPLOT_AUTO);
    ImVec4 c = ImPlot::SampleColormap((float)t, (ImPlotColormap)cmap);
    ImU32 packed = IM_COL32(
        (int)(c.x * 255.0f + 0.5f),
        (int)(c.y * 255.0f + 0.5f),
        (int)(c.z * 255.0f + 0.5f),
        (int)(c.w * 255.0f + 0.5f));
    return zym_newNumber((double)packed);
}

// --- ColormapScale(label, scaleMin, scaleMax, size?, format?, flags?, cmap?) ---
ZymValue u_plotColormapScale(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue minV,
                             ZymValue maxV, ZymValue sizeV, ZymValue fmtV,
                             ZymValue flagsV, ZymValue cmapV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotColormapScale label", &label)) return ZYM_ERROR;
    double smin, smax;
    if (!reqNum(vm, minV, "ui.plotColormapScale scaleMin", &smin)) return ZYM_ERROR;
    if (!reqNum(vm, maxV, "ui.plotColormapScale scaleMax", &smax)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.plotColormapScale")) return ZYM_ERROR;
    ImVec2 size;
    if (!plotOptVec2(vm, sizeV, ImVec2(0, 0), "ui.plotColormapScale size", &size)) return ZYM_ERROR;
    const char* fmt = optStr(fmtV, "%g");
    int flags = optInt(flagsV, 0);
    int cmap  = optInt(cmapV, IMPLOT_AUTO);
    ImPlot::ColormapScale(label.c_str(), smin, smax, size, fmt,
                          (ImPlotColormapScaleFlags)flags, (ImPlotColormap)cmap);
    return zym_newNull();
}

// --- ColormapSlider(label, ref=[t], format?, cmap?) -> bool ---
// `ref` is a 1-element list [t in 0..1]; writes back the new t.
ZymValue u_plotColormapSlider(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue refV,
                              ZymValue fmtV, ZymValue cmapV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotColormapSlider(label, [t], format?, cmap?)", &label)) return ZYM_ERROR;
    if (!zym_isList(refV) || zym_listLength(refV) < 1) {
        zym_runtimeError(vm, "ui.plotColormapSlider: ref must be a 1-element list [t]");
        return ZYM_ERROR;
    }
    if (!requireFrame(vm, "ui.plotColormapSlider")) return ZYM_ERROR;
    ZymValue v0 = zym_listGet(vm, refV, 0);
    if (!zym_isNumber(v0)) {
        zym_runtimeError(vm, "ui.plotColormapSlider: ref[0] must be a number");
        return ZYM_ERROR;
    }
    float t = (float)zym_asNumber(v0);
    const char* fmt = optStr(fmtV, "");
    int cmap = optInt(cmapV, IMPLOT_AUTO);
    bool changed = ImPlot::ColormapSlider(label.c_str(), &t, nullptr, fmt, (ImPlotColormap)cmap);
    zym_listSet(vm, refV, 0, zym_newNumber((double)t));
    return zym_newBool(changed);
}

// --- ColormapButton(label, size?, cmap?) -> bool ---
ZymValue u_plotColormapButton(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue sizeV, ZymValue cmapV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotColormapButton(label, size?, cmap?)", &label)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.plotColormapButton")) return ZYM_ERROR;
    ImVec2 size;
    if (!plotOptVec2(vm, sizeV, ImVec2(0, 0), "ui.plotColormapButton size", &size)) return ZYM_ERROR;
    int cmap = optInt(cmapV, IMPLOT_AUTO);
    bool clicked = ImPlot::ColormapButton(label.c_str(), size, (ImPlotColormap)cmap);
    return zym_newBool(clicked);
}

// --- BustColorCache(plotTitle?) ---
ZymValue u_plotBustColorCache(ZymVM* vm, ZymValue, ZymValue titleV) {
    (void)vm;
    const char* title = optStr(titleV, nullptr);
    ImPlot::BustColorCache(title);
    return zym_newNull();
}

// ---- Legend utils --------------------------------------------------------

// --- BeginLegendPopup(label, body, mouseButton?) -> bool ---
// Body-callback shape (mirrors `UI.plot`). EndLegendPopup is ONLY called
// when BeginLegendPopup returned true, per ImPlot contract.
ZymValue u_plotLegendPopup(ZymVM* vm, ZymValue, ZymValue labelV, ZymValue bodyV, ZymValue mbV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotLegendPopup(label, body, mouseButton?)", &label)) return ZYM_ERROR;
    if (!reqCallable(vm, bodyV, "ui.plotLegendPopup(label, body, mouseButton?)")) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotLegendPopup")) return ZYM_ERROR;
    int mb = optInt(mbV, 1);
    bool open = ImPlot::BeginLegendPopup(label.c_str(), mb);
    if (open) {
        zym_pushRoot(vm, bodyV);
        ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
        zym_popRoot(vm);
        ImPlot::EndLegendPopup();
        if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    }
    return zym_newBool(open);
}

// --- IsLegendEntryHovered(label) -> bool ---
ZymValue u_plotIsLegendEntryHovered(ZymVM* vm, ZymValue, ZymValue labelV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotIsLegendEntryHovered(label)", &label)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotIsLegendEntryHovered")) return ZYM_ERROR;
    return zym_newBool(ImPlot::IsLegendEntryHovered(label.c_str()));
}

// ==== SECTION 6: Subplots + AlignedPlots ==================================
//
// `BeginSubplots(title, rows, cols, size, flags?, rowRatios?, colRatios?)`
// opens a grid container; the script body then issues up to rows*cols
// `ui.plot(...)` calls — each becomes one cell. `rowRatios` / `colRatios`
// are optional `float*` arrays that ImPlot mutates when the user drags
// the resize splitters; we accept Zym lists of numbers, hand ImPlot a
// per-call `std::vector<float>`, and write the mutated values back into
// the same list at the end so the script can persist them across
// frames (matches the convention used by `SetupAxisLinks` and the
// drag tools).
//
// `BeginAlignedPlots(groupId, vertical?)` is the simpler sibling: every
// `ui.plot(...)` issued inside the body gets its plot area aligned
// against the group. Vertical groups align widths; horizontal groups
// align heights.

// Helper: read a Zym list of numbers into a std::vector<float>. Returns
// true and fills `out` on success; returns false on a non-list / wrong
// element type (and raises). When `v` is null/not a list, leaves `out`
// empty and returns true (caller's "no ratios supplied" path).
static bool readFloatListOptional(ZymVM* vm, ZymValue v, const char* where,
                                  std::vector<float>* out) {
    if (zym_isNull(v)) {
        out->clear();
        return true;
    }
    if (!zym_isList(v)) {
        zym_runtimeError(vm, "%s: expected a list of numbers or null", where);
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
        (*out)[i] = (float)zym_asNumber(e);
    }
    return true;
}

// Helper: write a std::vector<float> back into a Zym list. Used to
// reflect ImPlot's mutations to row/col ratios after the splitters get
// dragged. The list is mutated in place (length preserved).
static void writeFloatListBack(ZymVM* vm, ZymValue listV,
                               const std::vector<float>& src) {
    if (src.empty() || !zym_isList(listV)) return;
    int n = zym_listLength(listV);
    int m = (int)src.size();
    int k = n < m ? n : m;
    for (int i = 0; i < k; i++) {
        zym_listSet(vm, listV, i, zym_newNumber((double)src[i]));
    }
}

// `ui.plotSubplots(title, rows, cols, w, h, flags, rowRatios, colRatios, body) -> bool`
//
// Body-callback shape mirroring `ui.plot`: `body` runs only if
// `BeginSubplots` returned true, and `EndSubplots` is called from the
// true branch (per ImPlot's contract that `EndSubplots` is invoked iff
// `BeginSubplots` returned true).
ZymValue u_plotSubplots(ZymVM* vm, ZymValue, ZymValue titleV,
                        ZymValue rowsV, ZymValue colsV,
                        ZymValue wV, ZymValue hV,
                        ZymValue flagsV,
                        ZymValue rowRatiosV, ZymValue colRatiosV,
                        ZymValue bodyV) {
    std::string title;
    if (!reqStr(vm, titleV, "ui.plotSubplots", &title)) return ZYM_ERROR;
    int rows, cols;
    if (!reqInt(vm, rowsV, "ui.plotSubplots rows", &rows)) return ZYM_ERROR;
    if (!reqInt(vm, colsV, "ui.plotSubplots cols", &cols)) return ZYM_ERROR;
    double w, h;
    if (!reqNum(vm, wV, "ui.plotSubplots w", &w)) return ZYM_ERROR;
    if (!reqNum(vm, hV, "ui.plotSubplots h", &h)) return ZYM_ERROR;
    int flags = optInt(flagsV, 0);
    if (!reqCallable(vm, bodyV, "ui.plotSubplots(..., body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.plotSubplots")) return ZYM_ERROR;

    std::vector<float> rowRatios;
    std::vector<float> colRatios;
    if (!readFloatListOptional(vm, rowRatiosV, "ui.plotSubplots rowRatios", &rowRatios)) return ZYM_ERROR;
    if (!readFloatListOptional(vm, colRatiosV, "ui.plotSubplots colRatios", &colRatios)) return ZYM_ERROR;

    bool open = ImPlot::BeginSubplots(title.c_str(), rows, cols,
                                      ImVec2((float)w, (float)h),
                                      (ImPlotSubplotFlags)flags,
                                      rowRatios.empty() ? nullptr : rowRatios.data(),
                                      colRatios.empty() ? nullptr : colRatios.data());
    if (open) {
        zym_pushRoot(vm, bodyV);
        ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
        zym_popRoot(vm);
        ImPlot::EndSubplots();
        // Reflect any splitter-driven mutations back to the script.
        writeFloatListBack(vm, rowRatiosV, rowRatios);
        writeFloatListBack(vm, colRatiosV, colRatios);
        if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    }
    return zym_newBool(open);
}

// `ui.plotAlignedPlots(groupId, vertical, body) -> bool`
//
// Body-callback shape; `body` runs only when `BeginAlignedPlots`
// returned true, and `EndAlignedPlots` is called from the true branch.
ZymValue u_plotAlignedPlots(ZymVM* vm, ZymValue, ZymValue groupV,
                            ZymValue verticalV, ZymValue bodyV) {
    std::string group;
    if (!reqStr(vm, groupV, "ui.plotAlignedPlots(groupId, vertical, body)", &group)) return ZYM_ERROR;
    bool vertical = optBool(verticalV, true);
    if (!reqCallable(vm, bodyV, "ui.plotAlignedPlots(groupId, vertical, body)")) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.plotAlignedPlots")) return ZYM_ERROR;

    bool open = ImPlot::BeginAlignedPlots(group.c_str(), vertical);
    if (open) {
        zym_pushRoot(vm, bodyV);
        ZymStatus st = zym_callClosurev(vm, bodyV, 0, nullptr);
        zym_popRoot(vm);
        ImPlot::EndAlignedPlots();
        if (st != ZYM_STATUS_OK) return ZYM_ERROR;
    }
    return zym_newBool(open);
}

// ==== SECTION 7: Queries + Misc utilities =================================
//
// Read-only inspection of the current plot's state: hover/select queries,
// coordinate conversion, axis/legend hit-testing, plus a handful of
// standalone "show" helpers (style/colormap selectors, user guide,
// metrics window) that don't need a plot scope but do need a frame.
//
// Conventions:
//   - Points (ImPlotPoint / ImVec2) are returned as a 2-element list
//     `[x, y]`. Rects (ImPlotRect) are returned as 4-element lists
//     `[xMin, xMax, yMin, yMax]` matching ImPlot's struct field order.
//   - Optional `xAxis` / `yAxis` arguments default to `IMPLOT_AUTO`
//     (the script-facing constant is `UI.PLOT_AUTO`).
//   - All in-plot queries route through `requirePlot()`; standalone
//     helpers route through `requireFrame()`.

// Helper: pack an ImVec2 into a 2-element Zym list.
static ZymValue packVec2(ZymVM* vm, ImVec2 v) {
    ZymValue l = zym_newList(vm);
    zym_listAppend(vm, l, zym_newNumber((double)v.x));
    zym_listAppend(vm, l, zym_newNumber((double)v.y));
    return l;
}

// Helper: pack an ImPlotPoint into a 2-element Zym list.
static ZymValue packPlotPoint(ZymVM* vm, ImPlotPoint p) {
    ZymValue l = zym_newList(vm);
    zym_listAppend(vm, l, zym_newNumber(p.x));
    zym_listAppend(vm, l, zym_newNumber(p.y));
    return l;
}

// Helper: pack an ImPlotRect into a 4-element Zym list [xMin, xMax, yMin, yMax].
static ZymValue packPlotRect(ZymVM* vm, ImPlotRect r) {
    ZymValue l = zym_newList(vm);
    zym_listAppend(vm, l, zym_newNumber(r.X.Min));
    zym_listAppend(vm, l, zym_newNumber(r.X.Max));
    zym_listAppend(vm, l, zym_newNumber(r.Y.Min));
    zym_listAppend(vm, l, zym_newNumber(r.Y.Max));
    return l;
}

// --- IsPlotHovered() -> bool -----
ZymValue u_plotIsPlotHovered(ZymVM* vm, ZymValue) {
    if (!requirePlot(vm, "ui.plotIsPlotHovered")) return ZYM_ERROR;
    return zym_newBool(ImPlot::IsPlotHovered());
}

// --- IsAxisHovered(axis) -> bool -----
ZymValue u_plotIsAxisHovered(ZymVM* vm, ZymValue, ZymValue axisV) {
    int axis; if (!reqInt(vm, axisV, "ui.plotIsAxisHovered(axis)", &axis)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotIsAxisHovered")) return ZYM_ERROR;
    return zym_newBool(ImPlot::IsAxisHovered((ImAxis)axis));
}

// --- IsSubplotsHovered() -> bool — call inside a Subplots body -----
ZymValue u_plotIsSubplotsHovered(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.plotIsSubplotsHovered")) return ZYM_ERROR;
    return zym_newBool(ImPlot::IsSubplotsHovered());
}

// --- IsPlotSelected() -> bool -----
ZymValue u_plotIsPlotSelected(ZymVM* vm, ZymValue) {
    if (!requirePlot(vm, "ui.plotIsPlotSelected")) return ZYM_ERROR;
    return zym_newBool(ImPlot::IsPlotSelected());
}

// --- GetPlotSelection(xAxis?, yAxis?) -> [xMin, xMax, yMin, yMax] -----
ZymValue u_plotGetPlotSelection(ZymVM* vm, ZymValue, ZymValue xV, ZymValue yV) {
    if (!requirePlot(vm, "ui.plotGetPlotSelection")) return ZYM_ERROR;
    int xa = optInt(xV, IMPLOT_AUTO);
    int ya = optInt(yV, IMPLOT_AUTO);
    return packPlotRect(vm, ImPlot::GetPlotSelection((ImAxis)xa, (ImAxis)ya));
}

// --- CancelPlotSelection() -----
ZymValue u_plotCancelPlotSelection(ZymVM* vm, ZymValue) {
    if (!requirePlot(vm, "ui.plotCancelPlotSelection")) return ZYM_ERROR;
    ImPlot::CancelPlotSelection();
    return zym_newNull();
}

// --- GetPlotPos() -> [x, y] (pixels) -----
ZymValue u_plotGetPlotPos(ZymVM* vm, ZymValue) {
    if (!requirePlot(vm, "ui.plotGetPlotPos")) return ZYM_ERROR;
    return packVec2(vm, ImPlot::GetPlotPos());
}

// --- GetPlotSize() -> [w, h] (pixels) -----
ZymValue u_plotGetPlotSize(ZymVM* vm, ZymValue) {
    if (!requirePlot(vm, "ui.plotGetPlotSize")) return ZYM_ERROR;
    return packVec2(vm, ImPlot::GetPlotSize());
}

// --- GetPlotMousePos(xAxis?, yAxis?) -> [x, y] (plot coords) -----
ZymValue u_plotGetPlotMousePos(ZymVM* vm, ZymValue, ZymValue xV, ZymValue yV) {
    if (!requirePlot(vm, "ui.plotGetPlotMousePos")) return ZYM_ERROR;
    int xa = optInt(xV, IMPLOT_AUTO);
    int ya = optInt(yV, IMPLOT_AUTO);
    return packPlotPoint(vm, ImPlot::GetPlotMousePos((ImAxis)xa, (ImAxis)ya));
}

// --- GetPlotLimits(xAxis?, yAxis?) -> [xMin, xMax, yMin, yMax] -----
ZymValue u_plotGetPlotLimits(ZymVM* vm, ZymValue, ZymValue xV, ZymValue yV) {
    if (!requirePlot(vm, "ui.plotGetPlotLimits")) return ZYM_ERROR;
    int xa = optInt(xV, IMPLOT_AUTO);
    int ya = optInt(yV, IMPLOT_AUTO);
    return packPlotRect(vm, ImPlot::GetPlotLimits((ImAxis)xa, (ImAxis)ya));
}

// --- PixelsToPlot(x, y, xAxis?, yAxis?) -> [x, y] (plot coords) -----
ZymValue u_plotPixelsToPlot(ZymVM* vm, ZymValue, ZymValue pxV, ZymValue pyV,
                            ZymValue xaV, ZymValue yaV) {
    double px, py;
    if (!reqNum(vm, pxV, "ui.plotPixelsToPlot(x, y, ...)", &px)) return ZYM_ERROR;
    if (!reqNum(vm, pyV, "ui.plotPixelsToPlot(x, y, ...)", &py)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotPixelsToPlot")) return ZYM_ERROR;
    int xa = optInt(xaV, IMPLOT_AUTO);
    int ya = optInt(yaV, IMPLOT_AUTO);
    return packPlotPoint(vm, ImPlot::PixelsToPlot((float)px, (float)py, (ImAxis)xa, (ImAxis)ya));
}

// --- PlotToPixels(x, y, xAxis?, yAxis?) -> [x, y] (pixels) -----
ZymValue u_plotPlotToPixels(ZymVM* vm, ZymValue, ZymValue xV, ZymValue yV,
                            ZymValue xaV, ZymValue yaV) {
    double x, y;
    if (!reqNum(vm, xV, "ui.plotPlotToPixels(x, y, ...)", &x)) return ZYM_ERROR;
    if (!reqNum(vm, yV, "ui.plotPlotToPixels(x, y, ...)", &y)) return ZYM_ERROR;
    if (!requirePlot(vm, "ui.plotPlotToPixels")) return ZYM_ERROR;
    int xa = optInt(xaV, IMPLOT_AUTO);
    int ya = optInt(yaV, IMPLOT_AUTO);
    return packVec2(vm, ImPlot::PlotToPixels(x, y, (ImAxis)xa, (ImAxis)ya));
}

// --- HideNextItem(hidden?, cond?) -----
ZymValue u_plotHideNextItem(ZymVM* vm, ZymValue, ZymValue hiddenV, ZymValue condV) {
    if (!requireFrame(vm, "ui.plotHideNextItem")) return ZYM_ERROR;
    bool hidden = optBool(hiddenV, true);
    int cond = optInt(condV, ImPlotCond_Once);
    ImPlot::HideNextItem(hidden, (ImPlotCond)cond);
    return zym_newNull();
}

// --- PushPlotClipRect(expand?) -----
ZymValue u_plotPushPlotClipRect(ZymVM* vm, ZymValue, ZymValue expandV) {
    if (!requirePlot(vm, "ui.plotPushPlotClipRect")) return ZYM_ERROR;
    double expand = optNum(expandV, 0.0);
    ImPlot::PushPlotClipRect((float)expand);
    return zym_newNull();
}

// --- PopPlotClipRect() -----
ZymValue u_plotPopPlotClipRect(ZymVM* vm, ZymValue) {
    if (!requirePlot(vm, "ui.plotPopPlotClipRect")) return ZYM_ERROR;
    ImPlot::PopPlotClipRect();
    return zym_newNull();
}

// --- ItemIcon(color) — color is packed UI.color(...) ImU32 -----
ZymValue u_plotItemIcon(ZymVM* vm, ZymValue, ZymValue colV) {
    int col; if (!reqInt(vm, colV, "ui.plotItemIcon(color)", &col)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.plotItemIcon")) return ZYM_ERROR;
    ImPlot::ItemIcon((ImU32)col);
    return zym_newNull();
}

// --- ColormapIcon(cmap) -----
ZymValue u_plotColormapIcon(ZymVM* vm, ZymValue, ZymValue cmapV) {
    int cmap; if (!reqInt(vm, cmapV, "ui.plotColormapIcon(cmap)", &cmap)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.plotColormapIcon")) return ZYM_ERROR;
    ImPlot::ColormapIcon((ImPlotColormap)cmap);
    return zym_newNull();
}

// --- ShowStyleSelector(label) -> bool — standalone (no plot needed) -----
ZymValue u_plotShowStyleSelector(ZymVM* vm, ZymValue, ZymValue labelV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotShowStyleSelector(label)", &label)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.plotShowStyleSelector")) return ZYM_ERROR;
    return zym_newBool(ImPlot::ShowStyleSelector(label.c_str()));
}

// --- ShowColormapSelector(label) -> bool -----
ZymValue u_plotShowColormapSelector(ZymVM* vm, ZymValue, ZymValue labelV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotShowColormapSelector(label)", &label)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.plotShowColormapSelector")) return ZYM_ERROR;
    return zym_newBool(ImPlot::ShowColormapSelector(label.c_str()));
}

// --- ShowInputMapSelector(label) -> bool -----
ZymValue u_plotShowInputMapSelector(ZymVM* vm, ZymValue, ZymValue labelV) {
    std::string label;
    if (!reqStr(vm, labelV, "ui.plotShowInputMapSelector(label)", &label)) return ZYM_ERROR;
    if (!requireFrame(vm, "ui.plotShowInputMapSelector")) return ZYM_ERROR;
    return zym_newBool(ImPlot::ShowInputMapSelector(label.c_str()));
}

// --- ShowStyleEditor() — embeds the editor as a block (not a window) -----
ZymValue u_plotShowStyleEditor(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.plotShowStyleEditor")) return ZYM_ERROR;
    ImPlot::ShowStyleEditor(nullptr);
    return zym_newNull();
}

// --- ShowUserGuide() — embedded help block -----
ZymValue u_plotShowUserGuide(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.plotShowUserGuide")) return ZYM_ERROR;
    ImPlot::ShowUserGuide();
    return zym_newNull();
}

// --- ShowMetricsWindow() — its own window; no plot/frame scope needed
//     beyond an active ImGui frame -----
ZymValue u_plotShowMetricsWindow(ZymVM* vm, ZymValue) {
    if (!requireFrame(vm, "ui.plotShowMetricsWindow")) return ZYM_ERROR;
    ImPlot::ShowMetricsWindow(nullptr);
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

    // ---- SECTION 4: Tools (Drag* / Annotation / Tag*) ----
    MOD(plotDragPoint,  "plotDragPoint(id, ref, col, size, flags)",                u_plotDragPoint)
    MOD(plotDragLineX,  "plotDragLineX(id, ref, col, thickness, flags)",           u_plotDragLineX)
    MOD(plotDragLineY,  "plotDragLineY(id, ref, col, thickness, flags)",           u_plotDragLineY)
    MOD(plotDragRect,   "plotDragRect(id, ref, col, flags)",                       u_plotDragRect)
    MOD(plotAnnotation, "plotAnnotation(x, y, col, pixOffset, clamp, text, round)", u_plotAnnotation)
    MOD(plotTagX,       "plotTagX(x, col, text, round)",                           u_plotTagX)
    MOD(plotTagY,       "plotTagY(y, col, text, round)",                           u_plotTagY)

    // ---- SECTION 5: Style stacks + Colormaps + Legend ----
    MOD(plotStyleColorsAuto,    "plotStyleColorsAuto()",    u_plotStyleColorsAuto)
    MOD(plotStyleColorsClassic, "plotStyleColorsClassic()", u_plotStyleColorsClassic)
    MOD(plotStyleColorsDark,    "plotStyleColorsDark()",    u_plotStyleColorsDark)
    MOD(plotStyleColorsLight,   "plotStyleColorsLight()",   u_plotStyleColorsLight)
    MOD(plotWithStyleColor,     "plotWithStyleColor(map, body)", u_plotWithStyleColor)
    MOD(plotWithStyleVar,       "plotWithStyleVar(map, body)",   u_plotWithStyleVar)
    MOD(plotWithColormap,       "plotWithColormap(cmapOrName, body)", u_plotWithColormap)
    MOD(plotGetLastItemColor,   "plotGetLastItemColor()",        u_plotGetLastItemColor)
    MOD(plotGetStyleColorName,  "plotGetStyleColorName(idx)",    u_plotGetStyleColorName)
    MOD(plotGetMarkerName,      "plotGetMarkerName(idx)",        u_plotGetMarkerName)
    MOD(plotNextMarker,         "plotNextMarker()",              u_plotNextMarker)
    MOD(plotAddColormap,        "plotAddColormap(name, cols, qual)",   u_plotAddColormap)
    MOD(plotGetColormapCount,   "plotGetColormapCount()",              u_plotGetColormapCount)
    MOD(plotGetColormapName,    "plotGetColormapName(cmap)",           u_plotGetColormapName)
    MOD(plotGetColormapIndex,   "plotGetColormapIndex(name)",          u_plotGetColormapIndex)
    MOD(plotNextColormapColor,  "plotNextColormapColor()",             u_plotNextColormapColor)
    MOD(plotGetColormapSize,    "plotGetColormapSize(cmap)",           u_plotGetColormapSize)
    MOD(plotGetColormapColor,   "plotGetColormapColor(idx, cmap)",     u_plotGetColormapColor)
    MOD(plotSampleColormap,     "plotSampleColormap(t, cmap)",         u_plotSampleColormap)
    MOD(plotColormapScale,      "plotColormapScale(label, scaleMin, scaleMax, size, format, flags, cmap)", u_plotColormapScale)
    MOD(plotColormapSlider,     "plotColormapSlider(label, ref, format, cmap)", u_plotColormapSlider)
    MOD(plotColormapButton,     "plotColormapButton(label, size, cmap)",        u_plotColormapButton)
    MOD(plotBustColorCache,     "plotBustColorCache(plotTitle)",                u_plotBustColorCache)
    MOD(plotLegendPopup,        "plotLegendPopup(label, body, mouseButton)",    u_plotLegendPopup)
    MOD(plotIsLegendEntryHovered, "plotIsLegendEntryHovered(label)",            u_plotIsLegendEntryHovered)

    // ---- SECTION 6: Subplots + AlignedPlots ----
    MOD(plotSubplots,      "plotSubplots(title, rows, cols, w, h, flags, rowRatios, colRatios, body)", u_plotSubplots)
    MOD(plotAlignedPlots,  "plotAlignedPlots(groupId, vertical, body)",                                u_plotAlignedPlots)

    // ---- SECTION 7: Queries + Misc utilities ----
    MOD(plotIsPlotHovered,         "plotIsPlotHovered()",                           u_plotIsPlotHovered)
    MOD(plotIsAxisHovered,         "plotIsAxisHovered(axis)",                       u_plotIsAxisHovered)
    MOD(plotIsSubplotsHovered,     "plotIsSubplotsHovered()",                       u_plotIsSubplotsHovered)
    MOD(plotIsPlotSelected,        "plotIsPlotSelected()",                          u_plotIsPlotSelected)
    MOD(plotGetPlotSelection,      "plotGetPlotSelection(xAxis, yAxis)",            u_plotGetPlotSelection)
    MOD(plotCancelPlotSelection,   "plotCancelPlotSelection()",                     u_plotCancelPlotSelection)
    MOD(plotGetPlotPos,            "plotGetPlotPos()",                              u_plotGetPlotPos)
    MOD(plotGetPlotSize,           "plotGetPlotSize()",                             u_plotGetPlotSize)
    MOD(plotGetPlotMousePos,       "plotGetPlotMousePos(xAxis, yAxis)",             u_plotGetPlotMousePos)
    MOD(plotGetPlotLimits,         "plotGetPlotLimits(xAxis, yAxis)",               u_plotGetPlotLimits)
    MOD(plotPixelsToPlot,          "plotPixelsToPlot(x, y, xAxis, yAxis)",          u_plotPixelsToPlot)
    MOD(plotPlotToPixels,          "plotPlotToPixels(x, y, xAxis, yAxis)",          u_plotPlotToPixels)
    MOD(plotHideNextItem,          "plotHideNextItem(hidden, cond)",                u_plotHideNextItem)
    MOD(plotPushPlotClipRect,      "plotPushPlotClipRect(expand)",                  u_plotPushPlotClipRect)
    MOD(plotPopPlotClipRect,       "plotPopPlotClipRect()",                         u_plotPopPlotClipRect)
    MOD(plotItemIcon,              "plotItemIcon(color)",                           u_plotItemIcon)
    MOD(plotColormapIcon,          "plotColormapIcon(cmap)",                        u_plotColormapIcon)
    MOD(plotShowStyleSelector,     "plotShowStyleSelector(label)",                  u_plotShowStyleSelector)
    MOD(plotShowColormapSelector,  "plotShowColormapSelector(label)",               u_plotShowColormapSelector)
    MOD(plotShowInputMapSelector,  "plotShowInputMapSelector(label)",               u_plotShowInputMapSelector)
    MOD(plotShowStyleEditor,       "plotShowStyleEditor()",                         u_plotShowStyleEditor)
    MOD(plotShowUserGuide,         "plotShowUserGuide()",                           u_plotShowUserGuide)
    MOD(plotShowMetricsWindow,     "plotShowMetricsWindow()",                       u_plotShowMetricsWindow)

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

    // Section 4 — drag tools + annotations + tags
    zym_mapSet(vm, obj, "plotDragPoint",  plotDragPoint);
    zym_mapSet(vm, obj, "plotDragLineX",  plotDragLineX);
    zym_mapSet(vm, obj, "plotDragLineY",  plotDragLineY);
    zym_mapSet(vm, obj, "plotDragRect",   plotDragRect);
    zym_mapSet(vm, obj, "plotAnnotation", plotAnnotation);
    zym_mapSet(vm, obj, "plotTagX",       plotTagX);
    zym_mapSet(vm, obj, "plotTagY",       plotTagY);

    // Section 5 — style stacks + colormaps + legend
    zym_mapSet(vm, obj, "plotStyleColorsAuto",    plotStyleColorsAuto);
    zym_mapSet(vm, obj, "plotStyleColorsClassic", plotStyleColorsClassic);
    zym_mapSet(vm, obj, "plotStyleColorsDark",    plotStyleColorsDark);
    zym_mapSet(vm, obj, "plotStyleColorsLight",   plotStyleColorsLight);
    zym_mapSet(vm, obj, "plotWithStyleColor",     plotWithStyleColor);
    zym_mapSet(vm, obj, "plotWithStyleVar",       plotWithStyleVar);
    zym_mapSet(vm, obj, "plotWithColormap",       plotWithColormap);
    zym_mapSet(vm, obj, "plotGetLastItemColor",   plotGetLastItemColor);
    zym_mapSet(vm, obj, "plotGetStyleColorName",  plotGetStyleColorName);
    zym_mapSet(vm, obj, "plotGetMarkerName",      plotGetMarkerName);
    zym_mapSet(vm, obj, "plotNextMarker",         plotNextMarker);
    zym_mapSet(vm, obj, "plotAddColormap",        plotAddColormap);
    zym_mapSet(vm, obj, "plotGetColormapCount",   plotGetColormapCount);
    zym_mapSet(vm, obj, "plotGetColormapName",    plotGetColormapName);
    zym_mapSet(vm, obj, "plotGetColormapIndex",   plotGetColormapIndex);
    zym_mapSet(vm, obj, "plotNextColormapColor",  plotNextColormapColor);
    zym_mapSet(vm, obj, "plotGetColormapSize",    plotGetColormapSize);
    zym_mapSet(vm, obj, "plotGetColormapColor",   plotGetColormapColor);
    zym_mapSet(vm, obj, "plotSampleColormap",     plotSampleColormap);
    zym_mapSet(vm, obj, "plotColormapScale",      plotColormapScale);
    zym_mapSet(vm, obj, "plotColormapSlider",     plotColormapSlider);
    zym_mapSet(vm, obj, "plotColormapButton",     plotColormapButton);
    zym_mapSet(vm, obj, "plotBustColorCache",     plotBustColorCache);
    zym_mapSet(vm, obj, "plotLegendPopup",        plotLegendPopup);
    zym_mapSet(vm, obj, "plotIsLegendEntryHovered", plotIsLegendEntryHovered);

    // Section 6 — subplots + aligned plots
    zym_mapSet(vm, obj, "plotSubplots",     plotSubplots);
    zym_mapSet(vm, obj, "plotAlignedPlots", plotAlignedPlots);

    // Section 7 — queries + misc utilities
    zym_mapSet(vm, obj, "plotIsPlotHovered",        plotIsPlotHovered);
    zym_mapSet(vm, obj, "plotIsAxisHovered",        plotIsAxisHovered);
    zym_mapSet(vm, obj, "plotIsSubplotsHovered",    plotIsSubplotsHovered);
    zym_mapSet(vm, obj, "plotIsPlotSelected",       plotIsPlotSelected);
    zym_mapSet(vm, obj, "plotGetPlotSelection",     plotGetPlotSelection);
    zym_mapSet(vm, obj, "plotCancelPlotSelection",  plotCancelPlotSelection);
    zym_mapSet(vm, obj, "plotGetPlotPos",           plotGetPlotPos);
    zym_mapSet(vm, obj, "plotGetPlotSize",          plotGetPlotSize);
    zym_mapSet(vm, obj, "plotGetPlotMousePos",      plotGetPlotMousePos);
    zym_mapSet(vm, obj, "plotGetPlotLimits",        plotGetPlotLimits);
    zym_mapSet(vm, obj, "plotPixelsToPlot",         plotPixelsToPlot);
    zym_mapSet(vm, obj, "plotPlotToPixels",         plotPlotToPixels);
    zym_mapSet(vm, obj, "plotHideNextItem",         plotHideNextItem);
    zym_mapSet(vm, obj, "plotPushPlotClipRect",     plotPushPlotClipRect);
    zym_mapSet(vm, obj, "plotPopPlotClipRect",      plotPopPlotClipRect);
    zym_mapSet(vm, obj, "plotItemIcon",             plotItemIcon);
    zym_mapSet(vm, obj, "plotColormapIcon",         plotColormapIcon);
    zym_mapSet(vm, obj, "plotShowStyleSelector",    plotShowStyleSelector);
    zym_mapSet(vm, obj, "plotShowColormapSelector", plotShowColormapSelector);
    zym_mapSet(vm, obj, "plotShowInputMapSelector", plotShowInputMapSelector);
    zym_mapSet(vm, obj, "plotShowStyleEditor",      plotShowStyleEditor);
    zym_mapSet(vm, obj, "plotShowUserGuide",        plotShowUserGuide);
    zym_mapSet(vm, obj, "plotShowMetricsWindow",    plotShowMetricsWindow);

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

    // --- ImPlotDragToolFlags (`UI.PLOT_DRAG_TOOL_*`) — Section 4 tools
    zym_mapSet(vm, obj, "PLOT_DRAG_TOOL_NONE",       zym_newNumber(ImPlotDragToolFlags_None));
    zym_mapSet(vm, obj, "PLOT_DRAG_TOOL_NO_CURSORS", zym_newNumber(ImPlotDragToolFlags_NoCursors));
    zym_mapSet(vm, obj, "PLOT_DRAG_TOOL_NO_FIT",     zym_newNumber(ImPlotDragToolFlags_NoFit));
    zym_mapSet(vm, obj, "PLOT_DRAG_TOOL_NO_INPUTS",  zym_newNumber(ImPlotDragToolFlags_NoInputs));
    zym_mapSet(vm, obj, "PLOT_DRAG_TOOL_DELAYED",    zym_newNumber(ImPlotDragToolFlags_Delayed));

    // --- ImPlotColormapScaleFlags (`UI.PLOT_COLORMAP_SCALE_*`) — Section 5
    zym_mapSet(vm, obj, "PLOT_COLORMAP_SCALE_NONE",     zym_newNumber(ImPlotColormapScaleFlags_None));
    zym_mapSet(vm, obj, "PLOT_COLORMAP_SCALE_NO_LABEL", zym_newNumber(ImPlotColormapScaleFlags_NoLabel));
    zym_mapSet(vm, obj, "PLOT_COLORMAP_SCALE_OPPOSITE", zym_newNumber(ImPlotColormapScaleFlags_Opposite));
    zym_mapSet(vm, obj, "PLOT_COLORMAP_SCALE_INVERT",   zym_newNumber(ImPlotColormapScaleFlags_Invert));

    // --- IMPLOT_AUTO sentinel — used by colormap & marker overload defaults
    zym_mapSet(vm, obj, "PLOT_AUTO",          zym_newNumber(IMPLOT_AUTO));
}
