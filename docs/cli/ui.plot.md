# `UI.plot*`

ImPlot plotting surface exposed under the same global `UI` namespace
as the Dear ImGui widgets (see [`ui.md`](ui.md)). ImPlot rides on top
of ImGui, so every `UI.plot*` call must happen inside a `UI.frame(win,
body)` body, and most must additionally happen inside a `UI.plot(...)`
body.

ImPlot is immediate-mode like ImGui: scripts re-describe the plot
every frame. The `UI.plot(title, body)` call wraps `BeginPlot/EndPlot`
the same way `UI.window(name, body)` wraps `Begin/End` — the `body`
callback is only invoked when the plot opened, and the matching
`EndPlot` is taken care of by the bridge.

---

## Conventions

- **Plot scope.** Calls that draw into or configure a plot (`UI.plot*`
  except `UI.plot` itself, `UI.isInPlot`, and the `setNext*` family)
  must be issued from inside a `UI.plot(...)` body. Calls outside that
  scope raise a runtime error of the form
  `ui.plotX: called outside ui.plot(...)`.
- **Frame guard.** All `UI.plot*` calls additionally require an active
  ImGui frame (i.e. they must be reached from inside `UI.frame(win,
  body)`). `UI.isInPlot()` is the single exception and is always safe.
- **Numeric data.** Plot items accept numeric arrays as **either** a
  Zym list of numbers `[1.0, 2.0, 3.0]` **or** a `Buffer` of packed
  `f64` doubles (8 bytes per element). Buffers are read zero-copy;
  lists are copied into a small per-call vector. Mixed-shape inputs
  across arguments of the same call are fine — each argument is
  detected independently.
- **Axes.** Axes are addressed via `UI.AXIS_X1 / X2 / X3 / Y1 / Y2 /
  Y3` integer constants (matching ImPlot's `ImAxis_*`). The default
  pair is `X1` / `Y1`. Calls that take an `axis` argument want one of
  these; `SetAxes` / `SetupAxes` take an `(xAxis, yAxis)` pair.
- **Refs (axis links).** `UI.plotSetupAxisLinks` and
  `UI.plotSetNextAxisLinks` take a 2-element ref list `[min, max]` of
  numbers. The bridge reads the list into a pair of doubles, hands
  their addresses to ImPlot, then writes the (possibly mutated) values
  back into the list before returning. The link only fires for the
  duration of the single call — for persistent linking, call every
  frame (same convention as the `UI.input*` scalar refs).
- **Flag bitmasks.** Every plot item / setup call accepts an optional
  trailing `flags` integer built from the family's `UI.PLOT_*`
  constants. OR them together with `+` (no overlap is guaranteed).
  Pass `0` (or omit the argument where the overload allows) for the
  default behaviour.
- **Scoped callbacks.** `UI.plot(title, body)` mirrors `UI.window` —
  the body is only invoked when `BeginPlot` returned `true`, and
  `EndPlot` is called automatically. The same shape is used by
  `UI.plotLegendPopup(label, body)` (paired with `EndLegendPopup`).
- **Scoped style stacks.** Style colors, style vars, and colormaps
  use `with*` body-callback wrappers (`UI.plotWithStyleColor`,
  `UI.plotWithStyleVar`, `UI.plotWithColormap`) that push the
  requested state, run the body, and unconditionally pop the
  matching count — matching `UI.withStyleColor` / `UI.withStyleVar`
  / `UI.withFont` from the ImGui surface. Pops still happen if the
  body errors, so the stack is never left unbalanced.
- **Colors.** Anywhere a color is taken (drag-tool / tag / annotation
  colors, custom colormap entries), it's a packed `UI.color(r,g,b,a)`
  integer — the same shape used by `UI.image` and friends. Color
  *returns* (e.g. `UI.plotGetLastItemColor`,
  `UI.plotSampleColormap`) are packed the same way, so the value can
  be fed straight back into any color-taking call.
- **Errors.** As with `UI.*`, bad argument types raise a Zym runtime
  error of the form `ui.plotX(...) expects a <type>`. `UI.lastError()`
  also surfaces the most recent ImGui/ImPlot-side message.

---

## Plot lifecycle

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.plot(title, body)` | bool | Opens an ImPlot region with default size `(-1, 0)` and no flags. `body` runs only when `true`. |
| `UI.plot(title, w, h, body)` | bool | Same, with explicit pixel size. `0` for either dimension means "auto". |
| `UI.plot(title, w, h, flags, body)` | bool | Same, with a `UI.PLOT_*` flag bitmask. |
| `UI.isInPlot()` | bool | `true` if the caller is inside a `UI.plot(...)` body. Always safe to call. |

### `UI.PLOT_*` (plot flags — `ImPlotFlags`)

| Constant | Effect |
| --- | --- |
| `PLOT_NONE` | No flags. |
| `PLOT_NO_TITLE` | Hide the title. |
| `PLOT_NO_LEGEND` | Hide the legend. |
| `PLOT_NO_MOUSE_TEXT` | Hide the mouse coordinate overlay. |
| `PLOT_NO_INPUTS` | Disable user pan/zoom interaction. |
| `PLOT_NO_MENUS` | Disable the right-click context menus. |
| `PLOT_NO_BOX_SELECT` | Disable middle-mouse box-select zoom. |
| `PLOT_NO_FRAME` | Don't draw the frame around the plot. |
| `PLOT_EQUAL` | Equal X/Y unit scale. |
| `PLOT_CROSSHAIRS` | Show a crosshair at the mouse position. |
| `PLOT_CANVAS_ONLY` | Aggregate: hide title, legend, mouse text, and menus. |

---

## Axes

All Setup* calls below must be issued **inside** a `UI.plot(...)` body,
**before** the first plot-item call. The SetNext* family is the
companion that's called **outside** the plot body (before the next
`UI.plot`), but still needs an active `UI.frame`.

### Setup

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.plotSetupAxis(axis, label, flags)` | null | Configure a single axis. `label` may be `null`; `flags` is a `UI.PLOT_AXIS_*` bitmask. |
| `UI.plotSetupAxes(xLabel, yLabel, xFlags, yFlags)` | null | Configure the default X/Y pair in one call. Any of the args may be `null` / `0`. |
| `UI.plotSetupAxisLimits(axis, vMin, vMax, cond)` | null | Set axis limits. `cond` defaults to `UI.PLOT_COND_ONCE`. |
| `UI.plotSetupAxesLimits(xMin, xMax, yMin, yMax, cond)` | null | Set both default axes' limits at once. |
| `UI.plotSetupAxisLinks(axis, ref)` | null | `ref` is a 2-element `[min, max]` list of numbers, written back per frame. |
| `UI.plotSetupAxisFormat(axis, fmt)` | null | `fmt` is a printf-style format string (e.g. `"%.2f"`). |
| `UI.plotSetupAxisScale(axis, scale)` | null | Set axis scale to one of the `UI.PLOT_SCALE_*` enum values. |
| `UI.plotSetupAxisLimitsConstraints(axis, vMin, vMax)` | null | Restrict pan to keep limits within `[vMin, vMax]`. |
| `UI.plotSetupAxisZoomConstraints(axis, zMin, zMax)` | null | Restrict zoom range to `[zMin, zMax]`. |
| `UI.plotSetupAxisTicks(axis, values)` | null | Place ticks at the values from a numeric list/Buffer. |
| `UI.plotSetupAxisTicks(axis, values, labels, keepDefault)` | null | Same, with parallel string labels and a flag to keep ImPlot's default ticks too. |
| `UI.plotSetupAxisTicks(axis, vMin, vMax, nTicks, labels, keepDefault)` | null | Place `nTicks` evenly between `vMin` and `vMax`. `labels` and `keepDefault` are optional. |
| `UI.plotSetupLegend(location, flags)` | null | `location` is a `UI.PLOT_LOCATION_*`; `flags` is a `UI.PLOT_LEGEND_*` bitmask. |
| `UI.plotSetupMouseText(location, flags)` | null | Position the mouse-coordinate overlay. |
| `UI.plotSetupFinish()` | null | Force-finishes setup early so subsequent reads (e.g. mouse pos) are valid before any item is plotted. |

### Switch / Set / SetNext

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.plotSetAxis(axis)` | null | Switch the current Y axis for subsequent plot items in this plot. |
| `UI.plotSetAxes(xAxis, yAxis)` | null | Switch both current axes. |
| `UI.plotSetNextAxisLimits(axis, vMin, vMax, cond)` | null | Apply limits to the **next** `UI.plot`'s axis. Called outside the plot body. |
| `UI.plotSetNextAxisLinks(axis, ref)` | null | Link the next plot's axis to a `[min, max]` ref. Outside the plot body. |
| `UI.plotSetNextAxisToFit(axis)` | null | Auto-fit the next plot's axis to its data. |
| `UI.plotSetNextAxesLimits(xMin, xMax, yMin, yMax, cond)` | null | Apply default-pair limits to the next plot. |
| `UI.plotSetNextAxesToFit()` | null | Auto-fit both default axes on the next plot. |

### `UI.AXIS_*` (axis selectors — `ImAxis`)

| Constant | Meaning |
| --- | --- |
| `AXIS_X1` / `AXIS_X2` / `AXIS_X3` | Bottom / top / additional X axis. |
| `AXIS_Y1` / `AXIS_Y2` / `AXIS_Y3` | Left / right / additional Y axis. |

### `UI.PLOT_AXIS_*` (axis flags — `ImPlotAxisFlags`)

| Constant | Effect |
| --- | --- |
| `PLOT_AXIS_NONE` | No flags. |
| `PLOT_AXIS_NO_LABEL` | Hide the axis label. |
| `PLOT_AXIS_NO_GRID_LINES` | Hide the grid lines on this axis. |
| `PLOT_AXIS_NO_TICK_MARKS` | Hide tick marks. |
| `PLOT_AXIS_NO_TICK_LABELS` | Hide tick labels. |
| `PLOT_AXIS_NO_INITIAL_FIT` | Don't auto-fit on first frame. |
| `PLOT_AXIS_NO_MENUS` | Disable right-click axis menu. |
| `PLOT_AXIS_NO_SIDE_SWITCH` | Disable swapping axis to opposite side. |
| `PLOT_AXIS_NO_HIGHLIGHT` | Don't highlight on hover. |
| `PLOT_AXIS_OPPOSITE` | Render this axis on the opposite side. |
| `PLOT_AXIS_FOREGROUND` | Render grid in front of items. |
| `PLOT_AXIS_INVERT` | Invert axis direction. |
| `PLOT_AXIS_AUTO_FIT` | Auto-fit every frame. |
| `PLOT_AXIS_RANGE_FIT` | Fit only data visible on the other axis. |
| `PLOT_AXIS_PAN_STRETCH` | Stretch on pan instead of moving. |
| `PLOT_AXIS_LOCK_MIN` / `PLOT_AXIS_LOCK_MAX` | Lock one end of the range. |
| `PLOT_AXIS_LOCK` | Aggregate of both lock flags. |
| `PLOT_AXIS_NO_DECORATIONS` | Aggregate: hide label, grid, ticks, tick labels. |
| `PLOT_AXIS_AUX_DEFAULT` | Default flags for auxiliary axes. |

### `UI.PLOT_SCALE_*` (axis scales — `ImPlotScale`)

| Constant | Meaning |
| --- | --- |
| `PLOT_SCALE_LINEAR` | Linear (default). |
| `PLOT_SCALE_TIME` | Date/time scale. |
| `PLOT_SCALE_LOG10` | Base-10 logarithmic. |
| `PLOT_SCALE_SYM_LOG` | Symmetric log (handles zero/negative). |

### `UI.PLOT_COND_*` (limit conditions — `ImPlotCond`)

| Constant | Meaning |
| --- | --- |
| `PLOT_COND_NONE` | No condition. |
| `PLOT_COND_ALWAYS` | Apply every frame. |
| `PLOT_COND_ONCE` | Apply once and remember (default for `SetupAxisLimits`). |

### `UI.PLOT_LOCATION_*` (legend / overlay anchor — `ImPlotLocation`)

| Constant | Meaning |
| --- | --- |
| `PLOT_LOCATION_CENTER` | Center. |
| `PLOT_LOCATION_NORTH` / `_SOUTH` / `_WEST` / `_EAST` | Edge centers. |
| `PLOT_LOCATION_NORTH_WEST` / `_NORTH_EAST` / `_SOUTH_WEST` / `_SOUTH_EAST` | Corners. |

### `UI.PLOT_LEGEND_*` (legend flags — `ImPlotLegendFlags`)

| Constant | Effect |
| --- | --- |
| `PLOT_LEGEND_NONE` | No flags. |
| `PLOT_LEGEND_NO_BUTTONS` | Hide hide/show buttons. |
| `PLOT_LEGEND_NO_HIGHLIGHT_ITEM` | Don't highlight items on legend hover. |
| `PLOT_LEGEND_NO_HIGHLIGHT_AXIS` | Don't highlight axis on legend hover. |
| `PLOT_LEGEND_NO_MENUS` | Disable right-click legend menus. |
| `PLOT_LEGEND_OUTSIDE` | Render legend outside the plot. |
| `PLOT_LEGEND_HORIZONTAL` | Lay out entries horizontally. |
| `PLOT_LEGEND_SORT` | Sort entries alphabetically. |

### `UI.PLOT_MOUSE_TEXT_*` (mouse overlay flags — `ImPlotMouseTextFlags`)

| Constant | Effect |
| --- | --- |
| `PLOT_MOUSE_TEXT_NONE` | No flags. |
| `PLOT_MOUSE_TEXT_NO_AUX_AXES` | Only show coordinates for X1/Y1. |
| `PLOT_MOUSE_TEXT_NO_FORMAT` | Use the axis tick format instead of the dedicated one. |
| `PLOT_MOUSE_TEXT_SHOW_ALWAYS` | Always show the overlay even when mouse is outside. |

---

## Plot items

Every plot-item call accepts numeric arrays as **lists of numbers** or
**`Buffer` objects** (packed `f64`). Buffers are zero-copy; lists are
copied per call. All take an optional trailing `flags` integer built
from the item's `UI.PLOT_*` flag family — and additionally from
`UI.PLOT_ITEM_*` (the flags common to every item).

### Line family

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.plotLine(label, ys)` | null | Plot `ys` against an implicit `x = 0, 1, 2, ...`. |
| `UI.plotLine(label, xs, ys)` | null | Standard X/Y line. |
| `UI.plotLine(label, xs, ys, flags)` | null | Same, with `UI.PLOT_LINE_*` flags. |
| `UI.plotScatter(label, ys)` / `(label, xs, ys)` / `(label, xs, ys, flags)` | null | Marker scatter plot. Flags: `UI.PLOT_SCATTER_*`. |
| `UI.plotStairs(label, ys)` / `(label, xs, ys)` / `(label, xs, ys, flags)` | null | Step plot. Flags: `UI.PLOT_STAIRS_*`. |
| `UI.plotShaded(label, ys)` | null | Shaded region between `ys` and `y = 0`. |
| `UI.plotShaded(label, xs, ys)` | null | Same, with explicit X. |
| `UI.plotShaded(label, xs, ys, yref)` | null | Shade between `ys` and the constant `yref`. |
| `UI.plotShaded(label, xs, ys1, ys2, flags)` | null | Shade between two Y series. Flags: `UI.PLOT_SHADED_*`. |
| `UI.plotBars(label, values)` / `(label, values, size)` / `(label, xs, ys, size, flags)` | null | Vertical or horizontal bars. Flags: `UI.PLOT_BARS_*`. |
| `UI.plotBarGroups(labels, values, itemCount, groupCount, groupSize, shift, flags)` | null | Grouped bars from a row-major matrix. `labels` is a string list (one per item / row); `values` is a flat `itemCount * groupCount` matrix. Flags: `UI.PLOT_BAR_GROUPS_*`. |
| `UI.plotStems(label, values)` / `(label, xs, ys)` / `(label, xs, ys, ref, flags)` | null | Stem plot. Flags: `UI.PLOT_STEMS_*`. |
| `UI.plotInfLines(label, values, flags)` | null | Infinite vertical (or horizontal via flags) lines at each value. Flags: `UI.PLOT_INF_LINES_*`. |
| `UI.plotDummy(label, flags)` | null | Submit a no-op entry so it appears in the legend. |

### Statistical family

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.plotErrorBars(label, xs, ys, err)` | null | Symmetric error bars. |
| `UI.plotErrorBars(label, xs, ys, err, flags)` | null | Same, with flags. |
| `UI.plotErrorBars(label, xs, ys, neg, pos, flags)` | null | Asymmetric error bars (`neg` and `pos` magnitudes). Flags: `UI.PLOT_ERROR_BARS_*`. |
| `UI.plotPieChart(labels, values, x, y, radius, labelFmt, angle0, flags)` | null | Pie chart at `(x, y)` with `radius` in plot units. `labelFmt` is a printf format (default `"%.1f"`); `angle0` in degrees (default `90`). Flags: `UI.PLOT_PIE_CHART_*`. |
| `UI.plotHeatmap(label, values, rows, cols, scaleMin, scaleMax, labelFmt, xMin, yMin, xMax, yMax, flags)` | null | Row-major (default) or column-major (via flags) matrix of length `rows*cols`. Pass `scaleMin == scaleMax` for auto-scale. Bounds default to `(0,0)–(1,1)`. Flags: `UI.PLOT_HEATMAP_*`. |
| `UI.plotHistogram(label, values, bins, barScale, rangeMin, rangeMax, flags)` | Number | Returns the max bin count. `bins` is either a positive int or one of the `UI.PLOT_BIN_*` sentinels. Omit / pass `null` for both `rangeMin` and `rangeMax` to auto-range. Flags: `UI.PLOT_HISTOGRAM_*`. |
| `UI.plotHistogram2D(label, xs, ys, xBins, yBins, xMin, xMax, yMin, yMax, flags)` | Number | 2-D histogram, returns max bin count. Pass all four range bounds to set the rect; omit to auto-range. Shares `UI.PLOT_HISTOGRAM_*` flags. |
| `UI.plotDigital(label, xs, ys, flags)` | null | Digital signal (square wave) at the bottom of the plot. Flags: `UI.PLOT_DIGITAL_*`. |

### Visual family

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.plotImage(label, tex, xMin, yMin, xMax, yMax, uv0, uv1, tint, flags)` | null | Draw an SDL texture in plot coords. `tex` is a `Texture` (from `win.createTexture` / `win.textureFromSurface`). `uv0` / `uv1` are `[u, v]` lists (default `[0,0]` / `[1,1]`); `tint` is a packed `UI.color(...)` int (default opaque white). Flags: `UI.PLOT_IMAGE_*`. |
| `UI.plotText(text, x, y, pixOffset, flags)` | null | Draw text at plot coordinates `(x, y)`. `pixOffset` is a `[dx, dy]` pixel-space offset. Use `UI.PLOT_TEXT_VERTICAL` to rotate 90°. |

### `UI.PLOT_ITEM_*` (common item flags — `ImPlotItemFlags`)

| Constant | Effect |
| --- | --- |
| `PLOT_ITEM_NONE` | No flags. |
| `PLOT_ITEM_NO_LEGEND` | Don't register a legend entry. |
| `PLOT_ITEM_NO_FIT` | Don't include this item when auto-fitting axes. |

### Per-item flag families

| Family | Constants | Meaning |
| --- | --- | --- |
| `UI.PLOT_LINE_*` | `_NONE`, `_SEGMENTS`, `_LOOP`, `_SKIP_NAN`, `_NO_CLIP`, `_SHADED` | Line rendering tweaks. |
| `UI.PLOT_SCATTER_*` | `_NONE`, `_NO_CLIP` | Scatter rendering. |
| `UI.PLOT_STAIRS_*` | `_NONE`, `_PRE_STEP`, `_SHADED` | Step orientation / shading. |
| `UI.PLOT_SHADED_*` | `_NONE` | Reserved. |
| `UI.PLOT_BARS_*` | `_NONE`, `_HORIZONTAL` | Bar orientation. |
| `UI.PLOT_BAR_GROUPS_*` | `_NONE`, `_HORIZONTAL`, `_STACKED` | Group layout. |
| `UI.PLOT_STEMS_*` | `_NONE`, `_HORIZONTAL` | Stem orientation. |
| `UI.PLOT_INF_LINES_*` | `_NONE`, `_HORIZONTAL` | Line orientation. |
| `UI.PLOT_DUMMY_*` | `_NONE` | Reserved. |
| `UI.PLOT_ERROR_BARS_*` | `_NONE`, `_HORIZONTAL` | Error-bar orientation. |
| `UI.PLOT_PIE_CHART_*` | `_NONE`, `_NORMALIZE`, `_IGNORE_HIDDEN`, `_EXPLODING`, `_NO_SLICE_BORDER` | Pie chart options. |
| `UI.PLOT_HEATMAP_*` | `_NONE`, `_COL_MAJOR` | Matrix data order. |
| `UI.PLOT_HISTOGRAM_*` | `_NONE`, `_HORIZONTAL`, `_CUMULATIVE`, `_DENSITY`, `_NO_OUTLIERS`, `_COL_MAJOR` | Histogram (1-D & 2-D). |
| `UI.PLOT_DIGITAL_*` | `_NONE` | Reserved. |
| `UI.PLOT_IMAGE_*` | `_NONE` | Reserved. |
| `UI.PLOT_TEXT_*` | `_NONE`, `_VERTICAL` | Rotate text 90°. |

### `UI.PLOT_BIN_*` (histogram bin-count sentinels — `ImPlotBin`)

| Constant | Meaning |
| --- | --- |
| `PLOT_BIN_SQRT` | √N bins. |
| `PLOT_BIN_STURGES` | Sturges' formula (default). |
| `PLOT_BIN_RICE` | Rice rule. |
| `PLOT_BIN_SCOTT` | Scott's normal reference rule. |

Pass any **positive integer** instead to request an explicit bin
count.

---

## Drag tools, annotations, tags

Interactive overlays drawn on top of an open plot. Every drag tool
takes an integer `id` (must be unique within the plot — analogous to
an ImGui id), a ref list that's read AND written back per call, a
packed `UI.color(...)`, and an optional `flags` bitmask drawn from
`UI.PLOT_DRAG_TOOL_*`. All drag tools return `bool` — `true` while
the tool is being held this frame.

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.plotDragPoint(id, [x,y], col, size, flags)` | bool | Drag a point in plot coords. Default `size = 4`. Ref written back on drag. |
| `UI.plotDragLineX(id, [x], col, thickness, flags)` | bool | Drag a vertical line at `x`. Default `thickness = 1`. |
| `UI.plotDragLineY(id, [y], col, thickness, flags)` | bool | Drag a horizontal line at `y`. |
| `UI.plotDragRect(id, [x1,y1,x2,y2], col, flags)` | bool | Drag a rectangle. All four corners written back. |
| `UI.plotAnnotation(x, y, col, pixOffset, clamp, text, round)` | null | Fixed annotation at `(x, y)`. `pixOffset` is `[dx, dy]` (default `[0,0]`); `clamp` keeps the annotation inside the plot; `text` is optional — when `null`, no-text overload; `round` snaps to integers (no-text overload only). |
| `UI.plotTagX(x, col, text, round)` | null | Tag the X axis at value `x`. `text` optional; `round` snaps. |
| `UI.plotTagY(y, col, text, round)` | null | Tag the Y axis at value `y`. |

### `UI.PLOT_DRAG_TOOL_*` (drag tool flags — `ImPlotDragToolFlags`)

| Constant | Effect |
| --- | --- |
| `PLOT_DRAG_TOOL_NONE` | No flags. |
| `PLOT_DRAG_TOOL_NO_CURSORS` | Don't change the mouse cursor on hover. |
| `PLOT_DRAG_TOOL_NO_FIT` | Don't include the tool when auto-fitting axes. |
| `PLOT_DRAG_TOOL_NO_INPUTS` | Tool is rendered but can't be interacted with. |
| `PLOT_DRAG_TOOL_DELAYED` | Apply input/output one frame late (useful in some animation flows). |

---

## Style stacks, colormaps, legend popup

`UI.plot*` style follows the same scoped-callback idiom used by
`UI.withStyleColor` / `UI.withStyleVar` / `UI.withFont` — there is
no raw `plotPushStyleColor` / `plotPopStyleColor`. Instead, push a
batch of state, run a body, and the bridge pops the matching count
on both success and error paths.

### Presets

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.plotStyleColorsAuto()` | null | Use auto-derived colors (default — track the ImGui theme). |
| `UI.plotStyleColorsClassic()` | null | Apply the classic theme to the global ImPlot style. |
| `UI.plotStyleColorsDark()` | null | Apply the dark theme. |
| `UI.plotStyleColorsLight()` | null | Apply the light theme. |

These mutate the global `ImPlotStyle` — they're persistent, not
scoped, and meant to be called once at startup (or whenever the
imgui theme changes).

### Scoped style / colormap

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.plotWithStyleColor(map, body)` | null | Push every entry of `map` (slot-name string → color), run `body`, pop them all. Colors accept packed `UI.color(...)` ints OR `[r,g,b]` / `[r,g,b,a]` lists. See slot table below. |
| `UI.plotWithStyleVar(map, body)` | null | Push every entry of `map` (var-name string → number or `[x, y]`), run `body`, pop them all. The slot table tells you which keys take a scalar and which take an `[x, y]` pair. |
| `UI.plotWithColormap(cmapOrName, body)` | null | Scoped `PushColormap` / `PopColormap`. `cmapOrName` is either an int colormap index (a `UI.PLOT_COLORMAP_*` constant or the value returned from `UI.plotAddColormap`) or a string colormap name. |

#### Style color slot names (keys for `UI.plotWithStyleColor`)

Mirror `ImPlotCol_*` 1:1.

| Key | Slot |
| --- | --- |
| `"FrameBg"` | Plot frame background. |
| `"PlotBg"` | Plot area background. |
| `"PlotBorder"` | Plot border. |
| `"LegendBg"` / `"LegendBorder"` / `"LegendText"` | Legend chrome + text. |
| `"TitleText"` | Plot title text. |
| `"InlayText"` | Text drawn inside the plot area (`UI.plotText`). |
| `"AxisText"` / `"AxisGrid"` / `"AxisTick"` | Axis text, grid lines, tick marks. |
| `"AxisBg"` / `"AxisBgHovered"` / `"AxisBgActive"` | Axis background states. |
| `"Selection"` | Box-select rectangle. |
| `"Crosshairs"` | Crosshair lines (when `PLOT_CROSSHAIRS` is on). |

#### Style var slot names (keys for `UI.plotWithStyleVar`)

Mirror `ImPlotStyleVar_*`. The **kind** column says whether the
value is a single number or an `[x, y]` 2-element list.

| Key | Kind | Meaning |
| --- | --- | --- |
| `"PlotBorderSize"` | number | Border thickness (pixels). |
| `"MinorAlpha"` | number | Alpha of minor grid lines. |
| `"DigitalPadding"` | number | Padding for digital plots. |
| `"DigitalSpacing"` | number | Spacing for digital plots. |
| `"PlotDefaultSize"` | `[w, h]` | Default size used by `UI.plot(title, body)`. |
| `"PlotMinSize"` | `[w, h]` | Minimum size enforced on resize. |
| `"MajorTickLen"` / `"MinorTickLen"` | `[x, y]` | Major / minor tick length (pixels, X / Y). |
| `"MajorTickSize"` / `"MinorTickSize"` | `[x, y]` | Major / minor tick line thickness. |
| `"MajorGridSize"` / `"MinorGridSize"` | `[x, y]` | Major / minor grid-line thickness. |
| `"PlotPadding"` | `[x, y]` | Padding inside the plot frame. |
| `"LabelPadding"` | `[x, y]` | Padding around axis labels. |
| `"LegendPadding"` / `"LegendInnerPadding"` / `"LegendSpacing"` | `[x, y]` | Legend chrome metrics. |
| `"MousePosPadding"` | `[x, y]` | Padding for the mouse coord overlay. |
| `"AnnotationPadding"` | `[x, y]` | Padding inside `UI.plotAnnotation` text boxes. |
| `"FitPadding"` | `[x, y]` | Auto-fit padding (fraction of range). |

Unknown keys raise a runtime error of the form
`ui.plotWithStyleColor: unknown color slot 'X'` /
`ui.plotWithStyleVar: unknown style var 'X'`.

### Colormap management

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.plotAddColormap(name, cols, qual)` | Number | Register a custom colormap. `cols` is a list of packed `UI.color(...)` ints (at least 2). `qual = true` (default) for qualitative (discrete) colormaps, `false` for continuous. Returns the new `ImPlotColormap` index. |
| `UI.plotGetColormapCount()` | Number | Total number of registered colormaps. |
| `UI.plotGetColormapName(cmap)` | String | Name of colormap by index. |
| `UI.plotGetColormapIndex(name)` | Number | Lookup colormap by name (returns `-1` if not found). |
| `UI.plotGetColormapSize(cmap)` | Number | Number of colors in colormap (default `UI.PLOT_AUTO` = current). |
| `UI.plotGetColormapColor(idx, cmap)` | Number | Returns the `idx`-th color of the colormap as a packed `UI.color(...)` int. `cmap` defaults to `UI.PLOT_AUTO`. |
| `UI.plotSampleColormap(t, cmap)` | Number | Sample the colormap at `t` in `[0, 1]`; returns a packed color. |
| `UI.plotNextColormapColor()` | Number | Returns the next color from the current colormap and advances the counter. Useful from inside a `UI.plotWithColormap` body. |
| `UI.plotColormapScale(label, scaleMin, scaleMax, size, format, flags, cmap)` | null | Draw a vertical color-bar widget. `size` is `[w, h]` (default `[0,0]`); `format` defaults to `"%g"`; `flags` is `UI.PLOT_COLORMAP_SCALE_*`; `cmap` defaults to `UI.PLOT_AUTO`. |
| `UI.plotColormapSlider(label, [t], format, cmap)` | bool | Horizontal slider over `[0, 1]`. Ref written back on drag. `true` when the value changed this frame. |
| `UI.plotColormapButton(label, size, cmap)` | bool | Button rendered with the colormap as its background. Returns `true` on click. |
| `UI.plotBustColorCache(plotTitle)` | null | Clear cached colors for `plotTitle` (or all plots when `null`). Required after changing a colormap mid-frame. |

### Markers and helpers

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.plotGetLastItemColor()` | Number | Packed `UI.color(...)` of the most recently submitted plot item — handy for drawing matching overlays via `UI.draw*`. |
| `UI.plotGetStyleColorName(idx)` | String | Name of an `ImPlotCol_*` slot (e.g. `"PlotBg"`). |
| `UI.plotGetMarkerName(idx)` | String | Name of a `UI.PLOT_MARKER_*` value. |
| `UI.plotNextMarker()` | Number | Returns the next marker from the auto-cycle and advances the counter. |

### Legend popup

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.plotLegendPopup(label, body, mouseButton)` | bool | Open a context popup on right-click (default `mouseButton = 1`) of a legend entry. `body` only runs when open; `EndLegendPopup` is called automatically. |
| `UI.plotIsLegendEntryHovered(label)` | bool | `true` if the legend entry for `label` is currently hovered. |

### `UI.PLOT_COLORMAP_SCALE_*` (color-bar flags — `ImPlotColormapScaleFlags`)

| Constant | Effect |
| --- | --- |
| `PLOT_COLORMAP_SCALE_NONE` | No flags. |
| `PLOT_COLORMAP_SCALE_NO_LABEL` | Hide the label. |
| `PLOT_COLORMAP_SCALE_OPPOSITE` | Render labels / ticks on the opposite side. |
| `PLOT_COLORMAP_SCALE_INVERT` | Invert the color range. |

### `UI.PLOT_AUTO`

Sentinel (`-1`) accepted by every `cmap`-taking call meaning "use
the currently-active colormap". Same value as ImPlot's `IMPLOT_AUTO`.

---

## Style enums

These families are integer constants for use with the `ImPlotCol_*` /
`ImPlotStyleVar_*` ID-based APIs (e.g. `UI.plotGetStyleColorName`,
`UI.plotGetMarkerName`). The `with*` wrappers above take **string
keys**, not these integer constants — see the slot tables for those.

### `UI.PLOT_COL_*` (style colors — `ImPlotCol`)

Integers mirroring `ImPlotCol_*` 1:1. Names: `_FRAME_BG`, `_PLOT_BG`,
`_PLOT_BORDER`, `_LEGEND_BG`, `_LEGEND_BORDER`, `_LEGEND_TEXT`,
`_TITLE_TEXT`, `_INLAY_TEXT`, `_AXIS_TEXT`, `_AXIS_GRID`,
`_AXIS_TICK`, `_AXIS_BG`, `_AXIS_BG_HOVERED`, `_AXIS_BG_ACTIVE`,
`_SELECTION`, `_CROSSHAIRS`.

### `UI.PLOT_STYLE_*` (style vars — `ImPlotStyleVar`)

Integers mirroring `ImPlotStyleVar_*`: `_LINE_WEIGHT`, `_MARKER`,
`_MARKER_SIZE`, `_MARKER_WEIGHT`, `_FILL_ALPHA`, `_ERROR_BAR_SIZE`,
`_ERROR_BAR_WEIGHT`, `_DIGITAL_BIT_HEIGHT`, `_DIGITAL_BIT_GAP`,
`_PLOT_BORDER_SIZE`, `_MINOR_ALPHA`, `_MAJOR_TICK_LEN` /
`_MINOR_TICK_LEN`, `_MAJOR_TICK_SIZE` / `_MINOR_TICK_SIZE`,
`_MAJOR_GRID_SIZE` / `_MINOR_GRID_SIZE`, `_PLOT_PADDING`,
`_LABEL_PADDING`, `_LEGEND_PADDING` / `_LEGEND_INNER_PADDING` /
`_LEGEND_SPACING`, `_MOUSE_POS_PADDING`, `_ANNOTATION_PADDING`,
`_FIT_PADDING`, `_PLOT_DEFAULT_SIZE`, `_PLOT_MIN_SIZE`.

### `UI.PLOT_MARKER_*` (markers — `ImPlotMarker`)

`_NONE`, `_CIRCLE`, `_SQUARE`, `_DIAMOND`, `_UP`, `_DOWN`, `_LEFT`,
`_RIGHT`, `_CROSS`, `_PLUS`, `_ASTERISK`.

### `UI.PLOT_COLORMAP_*` (built-in colormaps — `ImPlotColormap`)

`_DEEP`, `_DARK`, `_PASTEL`, `_PAIRED`, `_VIRIDIS`, `_PLASMA`, `_HOT`,
`_COOL`, `_PINK`, `_JET`, `_TWILIGHT`, `_RD_BU`, `_BR_BG`, `_PI_YG`,
`_SPECTRAL`, `_GREYS`.

### `UI.PLOT_SUBPLOT_*` (subplot flags — `ImPlotSubplotFlags`)

Flags for `UI.plotSubplots`. Includes `_NONE`, `_NO_TITLE`,
`_NO_LEGEND`, `_NO_MENUS`, `_NO_RESIZE`, `_NO_ALIGN`, `_SHARE_ITEMS`,
`_LINK_ROWS`, `_LINK_COLS`, `_LINK_ALL_X`, `_LINK_ALL_Y`,
`_COL_MAJOR`.

---

## Subplots and aligned plots

Containers that arrange multiple `UI.plot(...)` calls in a shared
layout. Both use the same body-callback shape as `UI.plot` — the
body runs only when `Begin*` returned `true`, and `End*` is called
automatically. Each call to `UI.plot(...)` issued inside the body
becomes one cell (`plotSubplots`) or one aligned member
(`plotAlignedPlots`).

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.plotSubplots(title, rows, cols, w, h, flags, rowRatios, colRatios, body)` | bool | Open a `rows × cols` grid of plots. `flags` is a bitmask of `UI.PLOT_SUBPLOT_*`. `rowRatios` / `colRatios` are optional lists of numbers (length `rows` / `cols`) that ImPlot mutates in place as the user drags the resize splitters — pass `null` to skip. Issue up to `rows*cols` `UI.plot(...)` calls inside `body`. |
| `UI.plotAlignedPlots(groupId, vertical, body)` | bool | Align the plot areas of every `UI.plot(...)` issued inside `body` against the named group. `vertical = true` aligns widths (default); `false` aligns heights. The same `groupId` can be reused across multiple `plotAlignedPlots` blocks in the same frame to align across windows. |

The row / col ratio lists are **mutable**: when
`PLOT_SUBPLOT_NO_RESIZE` is off, dragging the splitters writes the
new ratios back into the same list, so a persistent script-side
list will track the user's resizes across frames.

---

## Queries and misc utilities

Read-only inspection of the current plot's state: hover/select
queries, coordinate conversion, axis/legend hit-testing, plus a
handful of standalone "show" helpers that just need an active
ImGui frame.

Points (`ImVec2` / `ImPlotPoint`) are returned as a 2-element list
`[x, y]`. Rects (`ImPlotRect`) are returned as a 4-element list
`[xMin, xMax, yMin, yMax]` matching ImPlot's struct field order.
Optional `xAxis` / `yAxis` arguments default to `UI.PLOT_AUTO`
(the script-facing sentinel for `IMPLOT_AUTO = -1`, meaning "the
current axis").

### Hover, selection, geometry

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.plotIsPlotHovered()` | bool | Mouse is over the plot area. Must be inside `UI.plot(...)`. |
| `UI.plotIsAxisHovered(axis)` | bool | Mouse is over the given axis's label/ticks. Must be inside `UI.plot(...)`. |
| `UI.plotIsSubplotsHovered()` | bool | Mouse is over the current subplots region. Call inside a `UI.plotSubplots(...)` body. |
| `UI.plotIsPlotSelected()` | bool | A box-select drag is currently active. |
| `UI.plotGetPlotSelection(xAxis, yAxis)` | `[xMin, xMax, yMin, yMax]` | Current selection rect in plot coords. Empty rect when nothing is selected. |
| `UI.plotCancelPlotSelection()` | null | Cancel the active selection. |
| `UI.plotGetPlotPos()` | `[x, y]` | Top-left of the plot area, in screen pixels. |
| `UI.plotGetPlotSize()` | `[w, h]` | Plot area size in pixels. |

### Coordinate conversion

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.plotGetPlotMousePos(xAxis, yAxis)` | `[x, y]` | Mouse position in plot coords for the given axis pair. |
| `UI.plotGetPlotLimits(xAxis, yAxis)` | `[xMin, xMax, yMin, yMax]` | Current axis limits. |
| `UI.plotPixelsToPlot(x, y, xAxis, yAxis)` | `[x, y]` | Convert screen pixels → plot coords. |
| `UI.plotPlotToPixels(x, y, xAxis, yAxis)` | `[x, y]` | Convert plot coords → screen pixels. |

### Items, clip, icons

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.plotHideNextItem(hidden, cond)` | null | Hide the next plot item. `hidden` defaults to `true`; `cond` is a `UI.PLOT_COND_*` value (defaults to `Once`). Call before issuing the item. |
| `UI.plotPushPlotClipRect(expand)` | null | Push the plot area as the current clip rect, optionally expanded by `expand` pixels. Must be inside `UI.plot(...)`. |
| `UI.plotPopPlotClipRect()` | null | Pop the matching clip rect. |
| `UI.plotItemIcon(color)` | null | Render a small square icon at the current cursor (e.g. as a legend swatch). `color` is a packed `UI.color(...)` int. |
| `UI.plotColormapIcon(cmap)` | null | Render a small icon showing the given colormap. |

### Standalone helpers

These don't need a plot scope — just an active `UI.frame(win, body)`.
They render their UI directly into the current ImGui window.

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.plotShowStyleSelector(label)` | bool | Combo box that swaps the current style preset. Returns `true` if the selection changed this frame. |
| `UI.plotShowColormapSelector(label)` | bool | Combo box that swaps the current colormap. |
| `UI.plotShowInputMapSelector(label)` | bool | Combo box that swaps the current input map. |
| `UI.plotShowStyleEditor()` | null | Embed the full style editor as a block in the current window. |
| `UI.plotShowUserGuide()` | null | Embed ImPlot's built-in user-guide text. |
| `UI.plotShowMetricsWindow()` | null | Show ImPlot's metrics/debug window. |

`UI.PLOT_AUTO` is the script-facing sentinel for `IMPLOT_AUTO`
(`-1`) — pass it as `xAxis` / `yAxis` when you want "the current
axis" rather than naming one explicitly.

---

## Examples

```zym
UI.frame(win, fn() {
    if (UI.plot("Wave", 0, 240, fn() {
        UI.plotSetupAxes("t", "y", 0, 0)
        UI.plotSetupAxisLimits(UI.AXIS_X1, 0, 6.28, UI.PLOT_COND_ALWAYS)
        UI.plotSetupAxisLimits(UI.AXIS_Y1, -1.2, 1.2, UI.PLOT_COND_ALWAYS)

        var xs = []
        var ys = []
        for (var i = 0; i < 256; i = i + 1) {
            var t = i * 6.28 / 256
            xs.push(t)
            ys.push(Math.sin(t))
        }
        UI.plotLine("sin", xs, ys)
    })) {
        // body ran, plot rendered
    }
})
```

Histogram returning its peak bin count:

```zym
var maxBin = UI.plotHistogram(
    "samples", samples,
    UI.PLOT_BIN_STURGES,
    1.0,
    null, null,
    UI.PLOT_HISTOGRAM_DENSITY)
```

A 2×2 subplot grid with persistent row/col ratios:

```zym
var rowRatios = [1.0, 1.0]
var colRatios = [1.0, 1.0]

UI.frame(win, fn() {
    UI.plotSubplots("grid", 2, 2, -1, 400,
        UI.PLOT_SUBPLOT_NONE,
        rowRatios, colRatios,
        fn() {
            if (UI.plot("a", fn() { UI.plotLine("x", xs, ys) })) {}
            if (UI.plot("b", fn() { UI.plotScatter("x", xs, ys) })) {}
            if (UI.plot("c", fn() { UI.plotBars("x", ys) })) {}
            if (UI.plot("d", fn() { UI.plotStairs("x", xs, ys) })) {}
        })
    // rowRatios / colRatios now reflect any splitter drags
})
```

Scoped style + a draggable point:

```zym
var point = [3.0, 4.0]

UI.frame(win, fn() {
    UI.plotWithStyleVar({ "PlotBorderSize": 2.0, "PlotPadding": [8, 8] }, fn() {
        UI.plotWithColormap(UI.PLOT_COLORMAP_VIRIDIS, fn() {
            if (UI.plot("Scoped", fn() {
                UI.plotLine("samples", xs, ys)

                // returns true while the user is dragging
                UI.plotDragPoint(1, point,
                    UI.color(255, 64, 64, 255),
                    6.0, UI.PLOT_DRAG_TOOL_NONE)

                UI.plotTagX(point[0], UI.color(255, 64, 64, 255),
                    "x = " + point[0], false)
            })) {}
        })
    })
})
```

---

## Notes

- `UI.plot*` is gated behind the same `ZYM_UI` build flag as the
  ImGui surface. The ImPlot context is created lazily per window
  alongside the ImGui one; no manual context plumbing is required.
- The full ImPlot v1.0 public surface is now wired (Sections 1–7:
  context + Begin/EndPlot, axis setup, plot items, drag tools,
  scoped style/colormap/legend, subplots + aligned plots, and the
  queries / misc utilities documented above).
- Callback-getter overloads (`PlotLineG` / `PlotScatterG` / `PlotBarsG`
  / `PlotErrorBarsG` / `PlotDigitalG` / `PlotStairsG` / `PlotShadedG`)
  and the custom-transform / custom-formatter callback overloads of
  `SetupAxisScale` / `SetupAxisFormat` / `PlotPieChart` /
  `ColormapScale` are intentionally not exposed — they require a
  C-callback-to-Zym-closure bridge that hasn't been needed yet.
- The legacy flat `plotPushStyleColor` / `plotPopStyleColor` /
  `plotPushStyleVar` / `plotPopStyleVar` / `plotPushColormap` /
  `plotPopColormap` natives were intentionally **not exposed** —
  scoped `UI.plotWithStyleColor` / `UI.plotWithStyleVar` /
  `UI.plotWithColormap` is the only style-stack surface, matching
  the existing `UI.withStyleColor` / `UI.withStyleVar` / `UI.withFont`
  idiom for ImGui.
