// implot.cpp — every `u_plot*` ImPlot wrapper + the
// `registerImPlotBindings` function called from `ui.cpp`'s
// `nativeUi_create`.
//
// PLACEHOLDER (Phase B carve-out, March 2026): this file exists so the
// build architecture is in place; the actual 1:1 ImPlot v1.0 bindings
// land in a follow-up (Phase A — see ui.cpp's header comment for the
// split rationale).
//
// Until then `registerImPlotBindings` is a no-op: ImPlot's per-window
// context is still created/destroyed in `ui.cpp`'s `ensureWindowContext`
// / `destroyUiContext`, and `ImPlot::SetCurrentContext` is still
// called inside `u_frame`, so any C++ code that directly references
// `ImPlot::*` will work — there's just no script-facing surface yet.
//
// Phase A populates this file with:
//   - readPlotData(list-or-Buffer -> std::vector<double>) helper
//   - requirePlot / requireSubplot guards (paired with `requireFrame`)
//   - every public ImPlot:: function as a `u_plot*` native
//   - every ImPlot enum exported as a `UI.PLOT_*` integer constant
//
// Compiled only when ZYM_UI_ENABLED is defined.

#include "ui_internal.hpp"

#include "implot.h"

void registerImPlotBindings(ZymVM* /*vm*/, ZymValue /*obj*/,
                            ZymValue /*context*/, RootScope& /*roots*/) {
    // Placeholder — Phase A populates this with the full ImPlot v1.0
    // surface (see file header comment). Until then there's nothing
    // to register; the ImPlot context lifecycle is still wired through
    // `ui.cpp` so scripts that don't call any `UI.plot*` natives are
    // unaffected.
}
