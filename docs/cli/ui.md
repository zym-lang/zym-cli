# `UI`

Dear ImGui widget surface exposed to scripts as the global identifier
`UI`. The native rides on top of the `SDL` substrate: every `UI.*`
call must happen inside an `SDL` window's frame, opened with
`UI.frame(win, body)`. The global is a map-shaped namespace.

ImGui is immediate-mode: scripts re-describe the UI every frame, and
widgets return their interaction result (`true` if a button was
clicked this frame, the new value of a slider, etc.). Stateful inputs
take a **ref** — a single-element list or a `Buffer` — so the bridge
can mutate the script-visible value in place.

---

## Conventions

- **Numbers.** Coordinates, sizes, slider/drag bounds, plot data, and
  flag bitmasks are Zym numbers. Floats are passed and returned as
  regular numbers; ImGui handles the `int` / `float` distinction
  internally based on the widget.
- **Booleans.** Required where the API takes a flag (`UI.checkbox`'s
  ref slot, `UI.menu`'s `enabled` arg). Passing a non-bool raises a
  runtime error.
- **Strings.** Labels, formatted overlays, text content, and IDs are
  Zym strings.
- **Refs (scalar).** Stateful inputs receive a `[value]` single-element
  list. The bridge reads `list[0]` to initialise the widget and writes
  `list[0]` back when the user edits the value:

  ```zym
  var count   = [0]
  var enabled = [false]
  UI.sliderInt("count", count, 0, 100)   // count[0] is updated in place
  UI.checkbox("enabled", enabled)
  ```

- **Refs (text).** `UI.inputText` / `UI.inputTextMultiline` take a
  `Buffer` so the underlying byte array can grow across frames. The
  Buffer's full allocated size is the edit capacity; the edited string
  is written back with NUL padding to preserve that capacity. Use
  `buf.toUtf8()` to read the current contents as a Zym string.

  ```zym
  var nameBuf = Buffer.new(128)
  UI.inputText("name", nameBuf)
  var name = nameBuf.toUtf8()
  ```

- **Refs (color).** A color ref is a 3- or 4-element list of
  normalised floats `[r, g, b]` / `[r, g, b, a]` (each `0.0 .. 1.0`).
  `UI.colorEdit` / `UI.colorPicker` / `UI.colorButton` /
  `UI.textColored` all take a color ref in this shape and (for
  `colorEdit` / `colorPicker`) write the edited components back into
  the list in place. The native picks the 3- vs 4-channel ImGui call
  based on the list length.
- **Packed colors (`ImU32`).** `DrawList` primitives and the legacy
  `UI.draw*` flat helpers take a packed 32-bit color in `IM_COL32`
  (ABGR) order. Build them with `UI.color(r, g, b, a)` where each
  component is an integer in `[0, 255]` (alpha defaults to `255` if
  omitted), or pass an already-packed integer literal. Color refs and
  packed colors are **not** interchangeable — `colorEdit` wants the
  ref list, `drawRect` wants the packed int.
- **Scoped callbacks.** Every ImGui call shaped like `Begin*` /
  `End*` is exposed as a single Zym function taking a `body` callback
  as its last argument. The bridge calls `Begin*` before invoking the
  body and the matching `End*` after it returns. The wrapper's return
  value is the `Begin*` bool; the body is **only invoked when that
  bool is true**, so scripts never have to guard manually. Examples:
  `UI.window`, `UI.child`, `UI.table`, `UI.group`, `UI.treeNode`,
  `UI.popup`, `UI.menu`, `UI.disabled`, `UI.id`, `UI.clip`,
  `UI.tooltipScope`, and `UI.frame` itself.
- **Frame guard.** Any `UI.*` call that touches an ImGui frame raises
  a runtime error if called outside `UI.frame(win, body)`. The single
  exception is `UI.lastError()`, which is always safe.
- **Errors.** Bad argument types raise a Zym runtime error of the form
  `UI.method(args) expects a <type>`. The most recent ImGui-side error
  message is also available via `UI.lastError()`.

---

## Frame lifecycle

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.frame(win, body)` | null | Begins a new ImGui frame for `win`, invokes `body`, then renders and presents. Single-frame call — drive the outer loop yourself with `while (!win.shouldClose()) { UI.frame(...) }`. |
| `UI.lastError()` | string | Most recent UI-side error message, or `""` if none. |
| `UI.silent(on)` | null | Convenience switch for ImGui's recoverable-error diagnostics. `UI.silent(true)` suppresses **both** the stderr `[imgui-error]` log lines (the `"In window 'X': Code uses SetCursorPos() ..."` style messages) and the on-screen error tooltip. `UI.silent(false)` restores both. Equivalent to calling `setErrorLogging(!on)` and `setErrorTooltip(!on)` together. Requires an active ImGui context — call from inside `UI.frame(...)`. The setting persists across frames until changed again. |
| `UI.setErrorLogging(on)` | null | Toggle the stderr `[imgui-error]` log lines alone (`io.ConfigErrorRecoveryEnableDebugLog`). `on=true` enables logging (default), `false` silences it. |
| `UI.setErrorTooltip(on)` | null | Toggle the in-app red error tooltip alone (`io.ConfigErrorRecoveryEnableTooltip`). `on=true` enables the tooltip (default), `false` hides it. |

> **Note.** These switches only affect ImGui's *reporting* of recoverable
> errors — the underlying error-recovery path itself stays enabled, so
> the warned-about condition is still handled safely. They are the
> right knob for noisy diagnostics like the `SetCursorPos()` "submit a
> Dummy() afterwards" message when you've intentionally drawn a child
> via the `DrawList` without a sizing item. They do **not** silence
> Zym-side runtime errors raised by the `UI` native itself (those are
> always reported and can't be muted).

---

## Windows

ImGui sub-windows inside the SDL window. Pin the position and size
ahead of `UI.window` with `setNextWindow*` if you want a chromeless
full-pane root.

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.window(name, body)` | bool | Floating panel with default chrome (title bar, resize, move, collapse). |
| `UI.window(name, flags, body)` | bool | Same, but with a `UI.WINDOW_*` bitmask. OR the constants together with `+`. |
| `UI.setNextWindowPos(x, y)` | null | Sets the position of the next `UI.window` call. |
| `UI.setNextWindowSize(w, h)` | null | Sets the size of the next `UI.window` call. |

### `UI.WINDOW_*` flags

| Constant | Effect |
| --- | --- |
| `WINDOW_NONE` | No flags. |
| `WINDOW_NO_TITLE_BAR` | Hide the title bar. |
| `WINDOW_NO_RESIZE` | Disable user resize gripper. |
| `WINDOW_NO_MOVE` | Disable user move. |
| `WINDOW_NO_SCROLLBAR` | Hide scrollbars. |
| `WINDOW_NO_SCROLL_WITH_MOUSE` | Disable mouse-wheel scrolling. |
| `WINDOW_NO_COLLAPSE` | Hide the collapse triangle. |
| `WINDOW_ALWAYS_AUTO_RESIZE` | Resize every frame to fit content. |
| `WINDOW_NO_BACKGROUND` | Don't draw the window background. |
| `WINDOW_NO_SAVED_SETTINGS` | Don't persist position/size across runs. |
| `WINDOW_NO_MOUSE_INPUTS` | Ignore mouse input inside the window. |
| `WINDOW_MENU_BAR` | Reserve room for `UI.menuBar`. |
| `WINDOW_HORIZONTAL_SCROLLBAR` | Allow horizontal scrolling. |
| `WINDOW_NO_FOCUS_ON_APPEARING` | Don't auto-focus on first appearance. |
| `WINDOW_NO_BRING_TO_FRONT_ON_FOCUS` | Stay underneath other windows when focused. |
| `WINDOW_NO_DECORATION` | Aggregate: no title bar, resize, scrollbar, or collapse. |
| `WINDOW_NO_INPUTS` | Aggregate: ignore mouse / nav input. |

---

## Layout

### Scoped containers

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.child(id, body)` | bool | Nested scrollable region. |
| `UI.child(id, opts, body)` | bool | Same, with an options map. Recognised keys: `w` (number, default `0`), `h` (number, default `0`), `border` (bool, default `false`). Unknown keys are ignored. |
| `UI.child(id, w, h, border, body)` | bool | Same, with explicit size and an optional border. |
| `UI.group(body)` | null | Treats the body's widgets as a single layout item. |
| `UI.disabled(cond, body)` | null | Greys out and disables interaction within `body` when `cond` is `true`. |
| `UI.id(idValue, body)` | null | Pushes a unique ID onto ImGui's ID stack for `body`. Accepts string or int IDs — use when generating widgets in a loop. |
| `UI.clip(x, y, w, h, body)` | null | Pushes a clip rectangle (intersected with the current one) for `body`. |

### Flat layout helpers

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.sameLine()` | null | Lay the next widget on the same line as the previous one. |
| `UI.sameLine(offset)` | null | Same, with an explicit horizontal offset. |
| `UI.sameLine(offset, spacing)` | null | Same, with offset and inter-item spacing. |
| `UI.newLine()` | null | Force a line break. |
| `UI.separator()` | null | Horizontal separator. |
| `UI.spacing()` | null | Small vertical gap. |
| `UI.dummy(w, h)` | null | Reserve a `w × h` invisible item — for layout padding. |
| `UI.indent()` / `UI.indent(px)` | null | Indent subsequent widgets. |
| `UI.unindent()` / `UI.unindent(px)` | null | Reverse the indent. |

---

## Text

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.text(s)` | null | Plain text. |
| `UI.textColored(color, s)` | null | Text in the given color ref — a 3- or 4-element float list (`[r, g, b]` / `[r, g, b, a]`, each `0.0 .. 1.0`). Not a packed `UI.color` int. |
| `UI.textWrapped(s)` | null | Word-wrapped to the current content region. |
| `UI.textDisabled(s)` | null | Greyed-out text. |
| `UI.labelText(label, value)` | null | `label = value` style row. |
| `UI.bulletText(s)` | null | Bulleted line of text. |
| `UI.bullet()` | null | Lone bullet glyph (for use with `sameLine`). |

---

## Buttons and toggles

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.button(label)` | bool | `true` on the frame the button is clicked. |
| `UI.button(label, w, h)` | bool | Same, with explicit size. |
| `UI.smallButton(label)` | bool | Compact button (no frame padding). |
| `UI.invisibleButton(id, w, h)` | bool | A clickable region with no visual. |
| `UI.arrowButton(id, dir)` | bool | Arrow button; `dir` is an integer (0=left, 1=right, 2=up, 3=down). |
| `UI.checkbox(label, ref)` | bool | `ref` is a `[bool]` single-element list. Returns `true` on edit. |
| `UI.radioButton(label, active)` | bool | Renders as selected when `active` is `true`; returns `true` on click. |
| `UI.selectable(label, sel)` | bool | List-row style toggle; `sel` is the current selected state. |
| `UI.selectable(label, sel, flags)` | bool | Same, with a `ImGuiSelectableFlags_*` bitmask. |

---

## Inputs

All scalar inputs take a single-element list ref and return `true` on
edit. `_` arguments are optional in the dispatched form.

### Numbers

| Method | Notes |
| --- | --- |
| `UI.inputInt(label, ref)` | Plain int input. |
| `UI.inputInt(label, ref, step)` | Int input with step buttons. |
| `UI.inputFloat(label, ref)` | Plain float input. |
| `UI.inputFloat(label, ref, step)` | Float input with step buttons. |
| `UI.inputFloat(label, ref, step, fmt)` | Same, with a `printf`-style format. |
| `UI.sliderInt(label, ref, min, max)` | Int slider. |
| `UI.sliderInt(label, ref, min, max, fmt)` | Same, with format. |
| `UI.sliderFloat(label, ref, min, max)` | Float slider. |
| `UI.sliderFloat(label, ref, min, max, fmt)` | Same, with format. |
| `UI.dragInt(label, ref)` | Click-and-drag int. |
| `UI.dragInt(label, ref, speed)` | Same, with sensitivity. |
| `UI.dragInt(label, ref, speed, min, max)` | Same, with clamped range. |
| `UI.dragFloat(label, ref)` | Click-and-drag float. |
| `UI.dragFloat(label, ref, speed)` | Same, with sensitivity. |
| `UI.dragFloat(label, ref, speed, min, max)` | Same, with clamped range. |

### Text

| Method | Notes |
| --- | --- |
| `UI.inputText(label, buf)` | `buf` is a `Buffer.new(N)`. The full `N` bytes are the edit capacity. |
| `UI.inputText(label, buf, flags)` | Same, with an `ImGuiInputTextFlags_*` bitmask. |
| `UI.inputTextMultiline(label, buf, w, h)` | Multi-line variant with explicit box size. |
| `UI.inputTextMultiline(label, buf, w, h, flags)` | Same, with flags. |

### Combo

| Method | Notes |
| --- | --- |
| `UI.combo(label, idxRef, items)` | Drop-down. `idxRef` is `[int]` (the current item index); `items` is a list of strings. |

---

## Colors

| Method | Notes |
| --- | --- |
| `UI.colorEdit(label, ref)` | Color editor; `ref` is a 3- or 4-element float list (`[r, g, b]` / `[r, g, b, a]`, each `0.0 .. 1.0`). Returns `true` on edit; writes the new components back into `ref`. |
| `UI.colorPicker(label, ref)` | Full picker widget. Same ref shape as `colorEdit`. |
| `UI.colorButton(id, ref)` | Just the swatch; takes the same 3- or 4-element color **ref list** as `colorEdit`. Returns `true` on click. Does **not** accept a packed `UI.color` integer. |
| `UI.color(r, g, b)` | Pack `[0, 255]` integer components into an `IM_COL32` packed color. Alpha defaults to `255`. |
| `UI.color(r, g, b, a)` | Same, with explicit alpha. Output is suitable for `UI.drawRect` / `UI.drawText` / all `DrawList` methods. Component out-of-range values are clamped. |

---

## Trees and collapsing headers

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.treeNode(label, body)` | bool | Scoped: `body` runs when the node is open. |
| `UI.collapsingHeader(label)` | bool | Flat: returns `true` while the header is open. |
| `UI.collapsingHeader(label, flags)` | bool | Same, with `ImGuiTreeNodeFlags_*` bitmask. |

---

## Tables

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.table(id, columns, body)` | bool | Scoped table. |
| `UI.table(id, columns, flags, body)` | bool | Same, with `ImGuiTableFlags_*` bitmask. |
| `UI.tableNextRow()` | null | Start a new row. |
| `UI.tableNextRow(minHeight)` | null | Same, with a minimum row height. |
| `UI.tableNextColumn()` | bool | Step to the next column; `false` if it's not visible (clipped). |
| `UI.tableSetColumnIndex(idx)` | bool | Jump to column `idx`. |
| `UI.tableSetupColumn(label)` | null | Declare a column. Call between `UI.table(...)` opening and the first row. |
| `UI.tableSetupColumn(label, flags)` | null | Same, with `ImGuiTableColumnFlags_*` bitmask. |
| `UI.tableSetupColumn(label, flags, width)` | null | Same, with initial width. |
| `UI.tableSetupScrollFreeze(cols, rows)` | null | Freeze leading rows/columns from scrolling. |
| `UI.tableHeadersRow()` | null | Auto-emit a header row from the setup columns. |
| `UI.tableHeader(label)` | null | Manually emit a single header cell. |
| `UI.tableGetRowIndex()` | number | Current row index. |
| `UI.tableGetColumnIndex()` | number | Current column index. |
| `UI.tableGetColumnCount()` | number | Total declared columns. |

### `UI.TABLE_*` flags

Bit-or these and pass to the 4-arg form of `UI.table(id, columns, flags, body)`.

| Constant | Meaning |
|----------|---------|
| `UI.TABLE_NONE` | No flags. |
| `UI.TABLE_RESIZABLE` | Allow the user to resize columns by dragging the header borders. |
| `UI.TABLE_REORDERABLE` | Allow the user to reorder columns by dragging their headers. |
| `UI.TABLE_HIDEABLE` | Right-click on a header to hide/show columns. |
| `UI.TABLE_SORTABLE` | Enable click-to-sort headers; query the order via `UI.tableGetSortSpecs()`. |
| `UI.TABLE_NO_SAVED_SETTINGS` | Don't persist column state in the imgui ini file. |
| `UI.TABLE_ROW_BG` | Alternate `TableRowBg` / `TableRowBgAlt` per row automatically. |
| `UI.TABLE_BORDERS_INNER_H` / `UI.TABLE_BORDERS_OUTER_H` | Draw inner / outer horizontal borders. |
| `UI.TABLE_BORDERS_INNER_V` / `UI.TABLE_BORDERS_OUTER_V` | Draw inner / outer vertical borders. |
| `UI.TABLE_BORDERS_H` / `UI.TABLE_BORDERS_V` | Both inner and outer of one axis. |
| `UI.TABLE_BORDERS_INNER` / `UI.TABLE_BORDERS_OUTER` | All inner / all outer borders. |
| `UI.TABLE_BORDERS` | All inner + all outer borders. |
| `UI.TABLE_SIZING_FIXED_FIT` | Columns default to fixed width fitting their content. |
| `UI.TABLE_SIZING_FIXED_SAME` | Fixed width, all columns sized to the widest. |
| `UI.TABLE_SIZING_STRETCH_PROP` | Stretch with weights proportional to content width. |
| `UI.TABLE_SIZING_STRETCH_SAME` | Stretch with equal weights. |
| `UI.TABLE_SCROLL_X` / `UI.TABLE_SCROLL_Y` | Make the table scrollable along the given axis (the table itself needs a sized outer container). |
| `UI.TABLE_SORT_MULTI` | Hold shift while clicking headers to add a secondary sort key (`tableGetSortSpecs` may return multiple entries). |
| `UI.TABLE_SORT_TRISTATE` | Allow "no sort" as a third header state. |

### Legacy columns (pre-`table` API)

| Method | Notes |
| --- | --- |
| `UI.columns(count)` | Begin a column region. |
| `UI.columns(count, id)` | Same, with an ID. |
| `UI.columns(count, id, border)` | Same, with a column-border toggle. |
| `UI.nextColumn()` | Advance to the next column. |

---

## Popups and menus

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.popup(id, body)` | bool | Scoped popup. Open it with `UI.openPopup(id)`. |
| `UI.popup(id, flags, body)` | bool | Same, with `ImGuiWindowFlags_*` bitmask. |
| `UI.popupModal(name, body)` | bool | Modal popup. |
| `UI.popupModal(name, flags, body)` | bool | Same, with flags. |
| `UI.openPopup(id)` | null | Request a popup be shown on the next frame. |
| `UI.openPopup(id, flags)` | null | Same, with `ImGuiPopupFlags_*` bitmask. |
| `UI.closeCurrentPopup()` | null | Close the currently-open popup. |
| `UI.menuBar(body)` | bool | Per-window menu bar (the window must have `WINDOW_MENU_BAR`). |
| `UI.mainMenuBar(body)` | bool | Top-of-viewport menu bar. |
| `UI.menu(label, body)` | bool | Scoped menu inside a menu bar. |
| `UI.menu(label, enabled, body)` | bool | Same, with an enabled flag. |
| `UI.menuItem(label)` | bool | Plain menu item; `true` on click. |
| `UI.menuItem(label, shortcut)` | bool | Same, with a display-only shortcut string. |
| `UI.menuItem(label, shortcut, selected)` | bool | `selected` may be a plain bool (display only) or a `[bool]` ref (toggled on click). |
| `UI.menuItem(label, shortcut, selected, enabled)` | bool | Same, with enabled flag. |

---

## Tooltips, progress, plots

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.tooltip(s)` | null | Sets a one-line tooltip for the just-rendered item. |
| `UI.tooltipScope(body)` | null | Scoped multi-line tooltip; runs `body` inside the tooltip popup. |
| `UI.progressBar(frac)` | null | Progress bar; `frac` in `[0.0, 1.0]`. |
| `UI.progressBar(frac, w, h)` | null | Same, with explicit size. |
| `UI.progressBar(frac, w, h, overlay)` | null | Same, with overlay text. |
| `UI.plotLines(label, values)` | null | Line plot; `values` is a list of numbers. |
| `UI.plotLines(label, values, overlay)` | null | Same, with overlay text. |
| `UI.plotLines(label, values, overlay, min, max)` | null | Same, with explicit Y-scale. |
| `UI.plotHistogram(label, values)` | null | Histogram. |
| `UI.plotHistogram(label, values, overlay)` | null | Same, with overlay text. |
| `UI.plotHistogram(label, values, overlay, min, max)` | null | Same, with explicit Y-scale. |

---

## Custom 2D drawing

`UI` exposes the full ImGui `ImDrawList` surface, including the image
methods that bind an `SDL.Texture` for sampling (see *Images* below
for the widget-level `UI.image` / `UI.imageButton` and the
`dl.addImage*` DrawList methods). Two flavours are available:

1. **Flat helpers** — one-shot calls on the current window's draw list,
   for short scripts and ad-hoc overlays. Convenient but limited: no
   paths, no clip stack, no channel splitter.
2. **`DrawList` handle** — a script-side mirror of the C++ API, with
   `add*` / `path*` / `pushClipRect` / `channelsSplit` methods. Use this
   when you need composed shapes, layered draws, or to target the
   viewport background / foreground instead of the current window.

All colors are packed 32-bit `ImU32` values — build them with
`UI.color(r, g, b, a)` (four `0.0 .. 1.0` floats).

### Flat helpers (current-window draw list)

| Method | Returns |
| --- | --- |
| `UI.drawLine(x1, y1, x2, y2, color)` | null |
| `UI.drawRect(x, y, w, h, color)` | null |
| `UI.drawRectFilled(x, y, w, h, color)` | null |
| `UI.drawCircle(cx, cy, r, color)` | null |
| `UI.drawCircleFilled(cx, cy, r, color)` | null |
| `UI.drawText(x, y, color, s)` | null |
| `UI.drawTriangle(x1, y1, x2, y2, x3, y3, color)` | null |
| `UI.drawTriangleFilled(x1, y1, x2, y2, x3, y3, color)` | null |

Must be called inside a `UI.window` (or `UI.child`) body — they bind to
whichever ImGui window is current.

### `DrawList` handle

Three factory functions return a `DrawList` handle bound to a specific
`ImDrawList`. The handle is a plain Zym map whose methods take no
explicit receiver — call them as `dl.addLine(...)` etc.

| Factory | Returns | Notes |
| --- | --- | --- |
| `UI.drawList()` | DrawList | The current ImGui window's draw list. Must be called inside a `UI.window` / `UI.child` body (otherwise raises). |
| `UI.backgroundDrawList()` | DrawList \| null | The viewport background draw list — anything drawn here renders **under** all ImGui windows. Must be called inside `UI.frame(...)`. |
| `UI.foregroundDrawList()` | DrawList \| null | The viewport foreground draw list — anything drawn here renders **over** all ImGui windows (useful for HUD-style overlays). Must be called inside `UI.frame(...)`. |

DrawList handles are atlas-owned and have no per-handle finalizer; the
draw list itself lives as long as its owning ImGui context. Hold one
across frames at your own risk — re-fetch each frame to be safe.

> **Argument arity.** Each `DrawList` method is registered as a
> **fixed-arity** native closure for the exact arg count shown in the
> tables below — every argument (including the ones marked `?`) must
> be passed explicitly. The `?` mark records *which* arg corresponds
> to an ImGui default: pass `0` for `segments`, pass `0.0` for
> `rotation` / `thickness=1.0` (1.0 is also the bridge default for
> stroke methods), and pass `UI.DRAW_NONE` for `flags`. Native
> dispatcher cap is 10 args; the single method that exceeds it is
> `dl.addBezierCubic` (11 args), which is registered as a **variadic**
> native closure — its trailing `segments` arg may be omitted, in
> which case ImGui auto-picks the segment count.

#### Primitives — `Add*`

| Method | Notes |
| --- | --- |
| `dl.addLine(x1, y1, x2, y2, color, thickness?)` | `thickness` defaults to `1.0`. |
| `dl.addRect(x, y, w, h, color, rounding?, thickness?, flags?)` | `flags` is a bitmask of `UI.DRAW_ROUND_CORNERS_*` / `UI.DRAW_*` constants. |
| `dl.addRectFilled(x, y, w, h, color, rounding?, flags?)` | Filled variant. |
| `dl.addRectFilledMultiColor(x, y, w, h, cTL, cTR, cBR, cBL)` | Four-corner gradient. |
| `dl.addQuad(x1, y1, x2, y2, x3, y3, x4, y4, color, thickness?)` | Stroked quad. |
| `dl.addQuadFilled(x1, y1, x2, y2, x3, y3, x4, y4, color)` | Filled quad. |
| `dl.addTriangle(x1, y1, x2, y2, x3, y3, color, thickness?)` | Stroked triangle. |
| `dl.addTriangleFilled(x1, y1, x2, y2, x3, y3, color)` | Filled triangle. |
| `dl.addCircle(cx, cy, r, color, segments?, thickness?)` | `segments=0` lets ImGui pick adaptively from `CircleTessellationMaxError`. |
| `dl.addCircleFilled(cx, cy, r, color, segments?)` | Filled circle. |
| `dl.addNgon(cx, cy, r, color, sides, thickness?)` | Fixed-sided polygon. |
| `dl.addNgonFilled(cx, cy, r, color, sides)` | Filled n-gon. |
| `dl.addEllipse(cx, cy, rx, ry, color, rot?, segments?, thickness?)` | `rot` is in radians. |
| `dl.addEllipseFilled(cx, cy, rx, ry, color, rot?, segments?)` | Filled ellipse. |
| `dl.addText(x, y, color, s)` | Uses the currently pushed font (or `UI.withFont`). |
| `dl.addBezierCubic(x1, y1, x2, y2, x3, y3, x4, y4, color, thickness?, segments?)` | Cubic Bezier (4 control points). |
| `dl.addBezierQuadratic(x1, y1, x2, y2, x3, y3, color, thickness?, segments?)` | Quadratic Bezier (3 control points). |
| `dl.addPolyline(points, color, closed?, thickness?, flags?)` | `points` is a list — see *Point lists* below. |
| `dl.addConvexPolyFilled(points, color)` | Fast convex fill. |
| `dl.addConcavePolyFilled(points, color)` | Generic concave fill (slower; correct for any simple polygon). |

#### Stateful path API — `Path*`

Build up a path with point-by-point calls, then close it with
`pathStroke` (line) or `pathFillConvex` / `pathFillConcave` (fill). The
fill / stroke calls reset the path automatically, so reusing the same
`dl` for back-to-back shapes is fine.

| Method | Notes |
| --- | --- |
| `dl.pathClear()` | Drop all currently buffered points. |
| `dl.pathLineTo(x, y)` | Append a single point. |
| `dl.pathLineToMergeDuplicate(x, y)` | Append, but skip if equal to the last point. |
| `dl.pathFillConvex(color)` | Submit as a filled convex polygon and reset the path. |
| `dl.pathFillConcave(color)` | Submit as a filled concave polygon and reset the path. |
| `dl.pathStroke(color, closed?, thickness?, flags?)` | Submit as a stroked polyline and reset. `flags` accepts `UI.DRAW_*`; `closed` is sugar for `UI.DRAW_CLOSED`. |
| `dl.pathArcTo(cx, cy, r, aMin, aMax, segments?)` | Append a circular arc. Angles in radians, must satisfy `aMin <= aMax`. |
| `dl.pathArcToFast(cx, cy, r, aMinOf12, aMaxOf12)` | Same as `pathArcTo` but with precomputed 12-step angles (`0..12`). |
| `dl.pathEllipticalArcTo(cx, cy, rx, ry, rot, aMin, aMax, segments?)` | Append an elliptical arc. `rot` is the ellipse rotation in radians. |
| `dl.pathBezierCubicCurveTo(x2, y2, x3, y3, x4, y4, segments?)` | Cubic Bezier from current point through 3 controls. |
| `dl.pathBezierQuadraticCurveTo(x2, y2, x3, y3, segments?)` | Quadratic Bezier from current point through 2 controls. |
| `dl.pathRect(x, y, w, h, rounding?, flags?)` | Append a rectangle (optionally rounded). |

#### Clip rect

`pushClipRect` / `popClipRect` on a `DrawList` are **render-only**
scissoring — they clip geometry submitted to that draw list but do
**not** affect ImGui's hit-testing or widget culling. For widget-level
clipping that also affects hover/click, use `UI.clip(x, y, w, h, body)`.

| Method | Notes |
| --- | --- |
| `dl.pushClipRect(x, y, w, h, intersect?)` | `intersect=true` intersects with the current clip rect instead of replacing it. |
| `dl.pushClipRectFullScreen()` | Push a clip rect that covers the whole viewport. |
| `dl.popClipRect()` | Pop the most recent push. |
| `dl.getClipRectMin()` | `{ x, y }` of the current clip rect's top-left. |
| `dl.getClipRectMax()` | `{ x, y }` of the current clip rect's bottom-right. |

#### Channel splitter

The channel splitter lets you submit shapes out of order and have them
rendered in a fixed z-stacking — e.g. draw a background fill *after*
text is laid out, while still having the fill appear *under* the text.

| Method | Notes |
| --- | --- |
| `dl.channelsSplit(count)` | Begin splitting into `count` channels (≥2). Subsequent draws go to channel 0 by default. |
| `dl.channelsSetCurrent(n)` | Switch the active channel for subsequent draws. |
| `dl.channelsMerge()` | Merge all channels back, preserving submission order *within* each channel and stacking channels low-to-high. |

#### Point lists

Methods that take a `points` argument (`addPolyline`,
`addConvexPolyFilled`, `addConcavePolyFilled`) accept either form:

```zym
// Nested:
dl.addPolyline([[10, 10], [50, 30], [90, 10]], color, true, 2.0)

// Flat:
dl.addPolyline([10, 10, 50, 30, 90, 10], color, true, 2.0)
```

The flat form requires an even number of entries; the nested form
requires each inner list to have at least 2 numbers.

#### `UI.DRAW_*` flags

For `addRect` / `addRectFilled` / `pathRect`, the rounding-corner
flags select which corners get rounded when `rounding > 0`. For
`pathStroke` / `addPolyline`, `UI.DRAW_CLOSED` joins the last and
first point.

| Flag | Notes |
| --- | --- |
| `UI.DRAW_NONE` | No flags. |
| `UI.DRAW_ROUND_CORNERS_TOP_LEFT` | Round only the top-left corner. |
| `UI.DRAW_ROUND_CORNERS_TOP_RIGHT` | Round only the top-right corner. |
| `UI.DRAW_ROUND_CORNERS_BOTTOM_LEFT` | Round only the bottom-left corner. |
| `UI.DRAW_ROUND_CORNERS_BOTTOM_RIGHT` | Round only the bottom-right corner. |
| `UI.DRAW_ROUND_CORNERS_TOP` | `TOP_LEFT \| TOP_RIGHT`. |
| `UI.DRAW_ROUND_CORNERS_BOTTOM` | `BOTTOM_LEFT \| BOTTOM_RIGHT`. |
| `UI.DRAW_ROUND_CORNERS_LEFT` | `TOP_LEFT \| BOTTOM_LEFT`. |
| `UI.DRAW_ROUND_CORNERS_RIGHT` | `TOP_RIGHT \| BOTTOM_RIGHT`. |
| `UI.DRAW_ROUND_CORNERS_ALL` | All four corners. |
| `UI.DRAW_ROUND_CORNERS_NONE` | Explicitly disables rounding (NOT the same as `0`; required to override a non-zero `rounding` value when you want square corners). |
| `UI.DRAW_CLOSED` | For `pathStroke` / `addPolyline`: connect last point to first. |

#### Example — composed shape with the path API

```zym
UI.frame(win, func() {
  UI.window("Path demo", func() {
    var dl = UI.drawList()
    // UI.color takes 0..255 integer components, alpha defaults to 255.
    var c = UI.color(230, 80, 50)

    // A teardrop: arc + a line back to the start, then convex-filled.
    dl.pathArcTo(200, 150, 60, 0, 3.14159 * 1.5, 24)
    dl.pathLineTo(200, 150)
    dl.pathFillConvex(c)

    // Stroked rounded box on top.
    dl.addRect(120, 80, 160, 140, UI.color(255, 255, 255, 255),
               8.0, 2.0, UI.DRAW_ROUND_CORNERS_ALL)
  })
})
```

#### Image methods — `addImage*`

These DrawList methods sample pixels from an `SDL.Texture` (built via
`win.createTexture` / `win.textureFromSurface` — see `sdl.md`). The
texture handle is **stable** for its lifetime: editing the upstream
`SDL.Surface` and calling `tex.update(...)` / `tex.refresh()` mutates
the GPU contents behind the same handle, so the very same
`dl.addImage(tex, ...)` call sees the updated pixels on the next
frame — no rebinding required.

| Method | Notes |
| --- | --- |
| `dl.addImage(tex, x, y, w, h, uv0?, uv1?, color?)` | Axis-aligned image quad. `uv0` / `uv1` default to `[0,0]` / `[1,1]` (full texture); pass 2-element lists to crop. `color` is a packed `ImU32` tint (defaults to opaque white). |
| `dl.addImageQuad(tex, x1, y1, x2, y2, x3, y3, x4, y4, uv1?, uv2?, uv3?, uv4?, color?)` | Free-form four-corner quad. UVs default to a CCW square (`[0,0]`, `[1,0]`, `[1,1]`, `[0,1]`). Variadic — pass between 9 and 14 args. |
| `dl.addImageRounded(tex, x, y, w, h, color, rounding, flags?, uv0?, uv1?)` | Rounded-rect image, useful for avatars and rounded thumbnails. `flags` accepts `UI.DRAW_ROUND_CORNERS_*`. Variadic — pass between 7 and 10 args. |

`uv0` / `uv1` / `uv*` are 2-element `[u, v]` lists in normalised texture
coordinates (`0..1`). Out-of-range values sample with the texture's
configured scale mode (set via `tex.setScaleMode("nearest" | "linear")`).

---

## Images

`UI.image` and `UI.imageButton` are widget-level image bindings that
participate in ImGui's layout flow (cursor advances by the image size,
the image becomes the "last item" for `UI.isItemHovered()` etc.). They
consume the same `SDL.Texture` values as the `dl.addImage*` methods
above and inherit the same handle-stability guarantee — pass the
texture into `UI.image` once per frame body, edit the upstream
surface, call `tex.update(...)` / `tex.refresh()`, and the next frame
renders with the new pixels automatically.

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.image(tex, w, h, uv0?, uv1?, tint?, border?)` | null | Render the texture inline at `w × h` pixels. `uv0` / `uv1` are 2-element `[u, v]` lists (default `[0,0]` / `[1,1]`). `tint` is a packed `ImU32` (defaults to opaque white). `border` is accepted for API stability but ignored on modern ImGui (the border colour was promoted to a style slot — push `ImGuiCol_ImageBorder` via `UI.withStyleColor` instead). |
| `UI.imageButton(id, tex, w, h, uv0?, uv1?, bgColor?, tint?)` | bool | Render a clickable image button with the given string `id`. Returns `true` on the frame the button is clicked. `bgColor` defaults to transparent; `tint` to opaque white. |

> **`null` for "use the default".** Every optional argument (`uv0`,
> `uv1`, `tint`, `border`, `bgColor`) accepts `null` to mean "use the
> ImGui default for this slot." Pass an explicit value to override.

> **Frame context.** Both calls must happen inside a `UI.window` /
> `UI.child` body (and therefore inside `UI.frame(win, body)`) — same
> rule as every other widget. Calling them outside a frame raises a
> Zym runtime error.

### Example — live editable image

```zym
var src = SDL.Surface.new(64, 64, null)
src.fill([0.10, 0.40, 0.85, 1.0], null)

// `link: true` lets us push surface edits via tex.refresh() without
// having to keep a separate reference to the surface around.
var tex = win.textureFromSurface(src, { link: true })

while (!win.shouldClose()) {
  UI.frame(win, func() {
    UI.window("Badge", func() {
      if (UI.button("Recolour")) {
        src.fill([0.85, 0.20, 0.20, 1.0], null)
        tex.refresh(null)        // explicit publish
      }
      UI.image(tex, 128, 128, null, null, null, null)
    })
  })
}
```

The `UI.image(tex, ...)` call runs every frame (that's just how
immediate-mode UI works), but it does **not** re-upload pixels —
ImGui samples whatever the texture currently contains. The upload
only happens inside `tex.refresh(null)`, which only runs when the
button is clicked.

---

## State queries

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.isItemHovered()` | bool | Hover state of the just-rendered item. |
| `UI.isItemClicked()` | bool | Click state of the just-rendered item. |
| `UI.isItemActive()` | bool | `true` while the item is being interacted with. |
| `UI.isItemFocused()` | bool | Focus state of the just-rendered item. |
| `UI.isWindowHovered()` | bool | Hover state of the current ImGui window. |
| `UI.isWindowFocused()` | bool | Focus state of the current ImGui window. |
| `UI.wantCaptureMouse()` | bool | `true` if ImGui is consuming mouse input this frame — scripts should gate their own mouse logic on this. |
| `UI.wantCaptureKeyboard()` | bool | Same, for keyboard. |
| `UI.getCursorPos()` | map | `{ x, y }` of the current layout cursor position. |
| `UI.getMousePos()` | map | `{ x, y }` of the mouse in window-local coordinates. |
| `UI.framerate()` | number | ImGui's smoothed framerate estimate (`io.Framerate`). |

---

## Theming

Theming in `UI` is a thin layer over ImGui's two style stacks
(`PushStyleColor` / `PushStyleVar`) plus the font atlas. Everything is
**scoped**: each helper takes a `body` callback, pushes the requested
state before invoking it, and pops back on return — even if the body
mutates state or raises. The same panel can be rendered three different
ways simply by wrapping it in different scopes.

### Style stacks

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.withStyleColor(map, body)` | null | Push one or more color slots for the duration of `body`. Keys are slot names (see below); values are 3- or 4-element `[r, g, b]` / `[r, g, b, a]` lists of floats in `0.0 .. 1.0`. |
| `UI.withStyleVar(map, body)` | null | Push one or more style variables for the duration of `body`. Keys are var names (see below); scalar vars take a number, `ImVec2` vars take a `[x, y]` list. |
| `UI.withFont(font, body)` | null | Push a font handle for the duration of `body`. `font` must come from `UI.loadFont` or `UI.defaultFont`. |

Unknown slot/var names raise a runtime error, so typos surface at the
first frame. Nest the scopes freely — colors and vars compose, and the
unwind order is balanced regardless of how the body returns.

```zym
var accent = {
    Button:        [0.20, 0.55, 0.85, 1.0],
    ButtonHovered: [0.30, 0.70, 0.95, 1.0],
    ButtonActive:  [0.10, 0.40, 0.75, 1.0],
    CheckMark:     [0.95, 0.80, 0.20, 1.0],
}
var spacing = {
    FrameRounding: 6.0,
    FramePadding:  [8, 6],
    ItemSpacing:   [10, 8],
}

UI.withStyleColor(accent, func() {
    UI.withStyleVar(spacing, func() {
        UI.button("Save")
        UI.sameLine()
        UI.button("Cancel")
    })
})
```

### Color slot names (`UI.withStyleColor` keys)

These are the `ImGuiCol_*` enumerators with the `ImGuiCol_` prefix
dropped:

`Text`, `TextDisabled`, `WindowBg`, `ChildBg`, `PopupBg`, `Border`,
`BorderShadow`, `FrameBg`, `FrameBgHovered`, `FrameBgActive`, `TitleBg`,
`TitleBgActive`, `TitleBgCollapsed`, `MenuBarBg`, `ScrollbarBg`,
`ScrollbarGrab`, `ScrollbarGrabHovered`, `ScrollbarGrabActive`,
`CheckMark`, `CheckboxSelectedBg`, `SliderGrab`, `SliderGrabActive`,
`Button`, `ButtonHovered`, `ButtonActive`, `Header`, `HeaderHovered`,
`HeaderActive`, `Separator`, `SeparatorHovered`, `SeparatorActive`,
`ResizeGrip`, `ResizeGripHovered`, `ResizeGripActive`, `InputTextCursor`,
`TabHovered`, `Tab`, `TabSelected`, `TabSelectedOverline`, `TabDimmed`,
`TabDimmedSelected`, `TabDimmedSelectedOverline`, `PlotLines`,
`PlotLinesHovered`, `PlotHistogram`, `PlotHistogramHovered`,
`TableHeaderBg`, `TableBorderStrong`, `TableBorderLight`, `TableRowBg`,
`TableRowBgAlt`, `TextLink`, `TextSelectedBg`, `TreeLines`,
`DragDropTarget`, `DragDropTargetBg`, `UnsavedMarker`, `NavCursor`,
`NavWindowingHighlight`, `NavWindowingDimBg`, `ModalWindowDimBg`.

### Style var names (`UI.withStyleVar` keys)

These are the `ImGuiStyleVar_*` enumerators with the `ImGuiStyleVar_`
prefix dropped. The **Kind** column indicates the expected value shape:
`scalar` = a single number, `vec2` = a `[x, y]` list of numbers.

| Name | Kind |
| --- | --- |
| `Alpha` | scalar |
| `DisabledAlpha` | scalar |
| `WindowPadding` | vec2 |
| `WindowRounding` | scalar |
| `WindowBorderSize` | scalar |
| `WindowMinSize` | vec2 |
| `WindowTitleAlign` | vec2 |
| `ChildRounding` | scalar |
| `ChildBorderSize` | scalar |
| `PopupRounding` | scalar |
| `PopupBorderSize` | scalar |
| `FramePadding` | vec2 |
| `FrameRounding` | scalar |
| `FrameBorderSize` | scalar |
| `ItemSpacing` | vec2 |
| `ItemInnerSpacing` | vec2 |
| `IndentSpacing` | scalar |
| `CellPadding` | vec2 |
| `ScrollbarSize` | scalar |
| `ScrollbarRounding` | scalar |
| `ScrollbarPadding` | scalar |
| `GrabMinSize` | scalar |
| `GrabRounding` | scalar |
| `ImageRounding` | scalar |
| `ImageBorderSize` | scalar |
| `TabRounding` | scalar |
| `TabBorderSize` | scalar |
| `TabMinWidthBase` | scalar |
| `TabMinWidthShrink` | scalar |
| `TabBarBorderSize` | scalar |
| `TabBarOverlineSize` | scalar |
| `TableAngledHeadersAngle` | scalar |
| `TableAngledHeadersTextAlign` | vec2 |
| `TreeLinesSize` | scalar |
| `TreeLinesRounding` | scalar |
| `DragDropTargetRounding` | scalar |
| `ButtonTextAlign` | vec2 |
| `SelectableTextAlign` | vec2 |
| `SeparatorSize` | scalar |
| `SeparatorTextBorderSize` | scalar |
| `SeparatorTextAlign` | vec2 |
| `SeparatorTextPadding` | vec2 |

### Fonts

A **Font** is an opaque handle wrapping an `ImFont*` owned by the
current ImGui context's font atlas. The atlas lives as long as the
context, so font handles do not need to be freed and remain valid for
the lifetime of the window that owns the context.

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.loadFont(src, sizePx)` | Font \| null | Load a TTF/OTF at the given pixel size into the active context's atlas. `src` is either a filesystem path (string) **or** a `Buffer` holding the raw font bytes — see "Loading from a Buffer" below. Returns `null` and stamps `UI.lastError()` on failure (missing file, bad font, empty buffer, no active context). Passing anything other than a string or Buffer raises a Zym runtime error. |
| `UI.loadFont(src, sizePx, opts)` | Font \| null | Same, with an options map. `opts` keys are **currently ignored** — the map slot is reserved for future glyph-range / merge / oversampling options. Passing `{}` or `null` is the same as the 2-arg form. |
| `UI.defaultFont()` | Font | Handle to the context's default font (ProggyClean unless something else was loaded earlier). |
| `UI.withFont(font, body)` | null | Push `font` for the duration of `body`. Uses the size the font was loaded at. |

Both `UI.loadFont` and `UI.defaultFont` must be called inside a
`UI.frame(...)` body — they need the lazily-created per-window ImGui
context. Calling `UI.loadFont` outside a frame returns `null`;
`UI.defaultFont` raises a runtime error.

```zym
var bigFont = null

UI.frame(win, func() {
    if (bigFont == null) {
        bigFont = UI.loadFont("assets/Inter.ttf", 24)
        if (bigFont == null) { bigFont = UI.defaultFont() }
    }
    UI.withFont(bigFont, func() {
        UI.text("Headline")
    })
    UI.text("Body text in the surrounding font.")
})
```

#### Loading from a Buffer

The string-path form forwards to `AddFontFromFileTTF` and is hardcoded
to the filesystem. To load fonts that don't live on disk — bytes
unpacked from a `.zpk`, downloaded over the network, or embedded as a
decoded base64 blob — pass a `Buffer` of TTF/OTF bytes instead. The
bridge forwards to `AddFontFromMemoryTTF` and copies the bytes into
an ImGui-owned allocation, so:

- The atlas owns its own copy of the font data and frees it on
  context shutdown.
- The script's `Buffer` is independent — it can be mutated, freed, or
  go out of scope after `UI.loadFont` returns without affecting the
  loaded font.

```zym
var titleFont = null

UI.frame(win, func() {
    if (titleFont == null) {
        // e.g. read TTF bytes out of a zpk into a Buffer:
        var bytes = Pack.read("fonts/Inter.ttf")   // returns a Buffer
        titleFont = UI.loadFont(bytes, 28)
        if (titleFont == null) { titleFont = UI.defaultFont() }
    }
    UI.withFont(titleFont, func() { UI.text("Headline") })
})
```

An empty or zero-byte Buffer returns `null` and stamps
`UI.lastError()` with `"ui.loadFont: empty font Buffer"`. A non-string,
non-Buffer first argument raises a Zym runtime error rather than a
quiet `null`, because that's a script-side type bug — there's no
fallback the native could reasonably try.

---

## Example

```zym
var win = SDL.createWindow("zym ui", 800, 600, { resizable: true })
if (win == null) { print("createWindow: %s", SDL.lastError()); return }

var count   = [0]
var nameBuf = Buffer.new(64)
var color   = [0.4, 0.7, 0.9, 1.0]

while (!win.shouldClose()) {
    var e = SDL.pollEvent()
    while (e != null) { e = SDL.pollEvent() }

    UI.frame(win, func() {
        var ws = win.getSize()
        UI.setNextWindowPos(0, 0)
        UI.setNextWindowSize(ws.w + 0.0, ws.h + 0.0)
        var flags = UI.WINDOW_NO_TITLE_BAR + UI.WINDOW_NO_RESIZE
        flags = flags + UI.WINDOW_NO_MOVE + UI.WINDOW_NO_COLLAPSE
        UI.window("root", flags, func() {
            UI.text("hello from zym + imgui")
            UI.sliderInt("count", count, 0, 100)
            UI.inputText("name", nameBuf)
            UI.colorEdit("tint", color)
            if (UI.button("click me")) {
                print("clicked! count=%v name=%s", count[0], nameBuf.toUtf8())
            }
        })
    })
}

win.free()
```

---

## Widget parity (extended surface)

The functions below round out ZYM's coverage of ImGui's public widget
API. Everything in this section follows the same conventions as the
rest of the doc: scalar refs are single-element lists, vector refs are
N-element lists, callable bodies are invoked inside the begin/end pair,
and every entry requires being inside `UI.frame(...)`.

### Tab bar / tab item

| Call | Notes |
|------|-------|
| `UI.tabBar(id, body)` | `BeginTabBar` / `EndTabBar`. Returns `true` if the bar opened. Body should call `UI.tabItem` / `UI.tabItemButton` entries. |
| `UI.tabItem(label, body)` | `BeginTabItem` / `EndTabItem`. Returns `true` if the tab is currently selected. |
| `UI.tabItemButton(label)` | Trailing pseudo-tab button. Returns `true` on click. |

### List box

| Call | Notes |
|------|-------|
| `UI.listBox(label, body)` | Scoped `BeginListBox` / `EndListBox`. Body emits selectables / items. Returns `true` if visible. |
| `UI.listBox(label, idxRef, items)` | Flat one-shot — `items` is a list of strings, `idxRef` is `[i]`. Returns `true` on change. |

### Combo (scoped)

| Call | Notes |
|------|-------|
| `UI.comboScope(label, preview, body)` | `BeginCombo` / `EndCombo`. Use when items aren't a flat list (mix selectables, separators, headers). |

### Vector slider / drag / input

The 2/3/4 suffix takes a list ref of that length and updates it in place.

| Call | Notes |
|------|-------|
| `UI.sliderFloat2/3/4(label, ref, min, max)` | Vector slider. |
| `UI.sliderInt2/3/4(label, ref, min, max)` | Int vector slider. |
| `UI.dragFloat2/3/4(label, ref, speed, min, max)` | Vector drag. `min == max` means unbounded. |
| `UI.dragInt2/3/4(label, ref, speed, min, max)` | Int vector drag. |
| `UI.inputFloat2/3/4(label, ref)` | Direct vector entry. |
| `UI.inputInt2/3/4(label, ref)` | Int vector entry. |

### Specialty sliders

| Call | Notes |
|------|-------|
| `UI.sliderAngle(label, ref, degMin, degMax)` | `ref[0]` is radians, slider displays degrees. |
| `UI.vSliderFloat(label, w, h, ref, min, max)` | Vertical float slider. |
| `UI.vSliderInt(label, w, h, ref, min, max)` | Vertical int slider. |

### Text / separator extras

| Call | Notes |
|------|-------|
| `UI.separatorText(s)` | A separator with a centered label. |
| `UI.textLink(s)` | Clickable underlined text. Returns `true` on click. |
| `UI.textLinkOpenURL(label, url)` | Same, but ImGui handles opening the URL via platform IO. |

### Toggles

| Call | Notes |
|------|-------|
| `UI.checkboxFlags(label, ref, flag)` | Flips bit `flag` on `ref[0]` (int). Returns `true` on change. |

### Scrolling

| Call | Notes |
|------|-------|
| `UI.getScrollX()` / `UI.getScrollY()` | Current scroll for the current window. |
| `UI.getScrollMaxX()` / `UI.getScrollMaxY()` | Maximum scroll. |
| `UI.setScrollX(x)` / `UI.setScrollY(y)` | Set scroll. |
| `UI.setScrollHereX(centerRatio)` / `UI.setScrollHereY(centerRatio)` | Center current cursor in view. |
| `UI.setScrollFromPosX(localX, centerRatio)` / `UI.setScrollFromPosY(localY, centerRatio)` | Scroll to a specific local position. |

### Window state queries

| Call | Notes |
|------|-------|
| `UI.isWindowAppearing()` | First frame the window is visible. |
| `UI.isWindowCollapsed()` | True if collapsed. |
| `UI.getWindowPos()` / `UI.getWindowSize()` | `{x, y}` map. |
| `UI.getWindowWidth()` / `UI.getWindowHeight()` | Scalar. |

### `UI.setNextWindow*`

| Call | Notes |
|------|-------|
| `UI.setNextWindowFocus()` | Focus on next `UI.window`. |
| `UI.setNextWindowBgAlpha(alpha)` | Override window bg alpha for the next window only. |
| `UI.setNextWindowContentSize(w, h)` | Force content size (for `Always*` scrollbars). |
| `UI.setNextWindowCollapsed(c)` | Start collapsed/expanded. |
| `UI.setNextWindowScroll(x, y)` | Initial scroll (`-1` = leave as-is). |

### Item / any-item queries

| Call | Notes |
|------|-------|
| `UI.isItemVisible()`, `UI.isItemEdited()` | Standard ImGui item-state checks. |
| `UI.isItemActivated()`, `UI.isItemDeactivated()`, `UI.isItemDeactivatedAfterEdit()` | Edge-triggered. |
| `UI.isItemToggledOpen()` | For tree nodes / collapsing headers. |
| `UI.isAnyItemHovered()`, `UI.isAnyItemActive()`, `UI.isAnyItemFocused()` | Wide window-scoped queries. |
| `UI.getItemRectMin()` / `Max()` / `Size()` | `{x, y}` map for the last submitted item. |

### Mouse queries

| Call | Notes |
|------|-------|
| `UI.isMouseDown(button)`, `UI.isMouseClicked(button)`, `UI.isMouseDoubleClicked(button)`, `UI.isMouseReleased(button)` | Buttons: `0=L`, `1=R`, `2=M`. |
| `UI.isMouseDragging(button, threshold)` | `threshold == -1` uses default. |
| `UI.getMouseDragDelta(button)` | `{x, y}` map of drag delta. |
| `UI.resetMouseDragDelta(button)` | Reset accumulator. |
| `UI.getMouseClickedCount(button)` | Multi-click count. |

### Keyboard queries

| Call | Notes |
|------|-------|
| `UI.isKeyDown(key)`, `UI.isKeyPressed(key, repeat)`, `UI.isKeyReleased(key)` | `key` is an `ImGuiKey` integer (use ImGui enum value). |
| `UI.getKeyPressedAmount(key, rd, rr)` | Repeat-aware press count for a frame. |
| `UI.setNextFrameWantCaptureKeyboard(b)` / `UI.setNextFrameWantCaptureMouse(b)` | Override IO routing for next frame. |

### Clipboard

| Call | Notes |
|------|-------|
| `UI.getClipboardText()` | Returns a string. |
| `UI.setClipboardText(s)` | Sets the platform clipboard. |

### Context popups

| Call | Notes |
|------|-------|
| `UI.popupContextItem(id, body)` | Right-click on previous item opens this popup (scoped). |
| `UI.popupContextWindow(id, body)` | Right-click anywhere in the current window opens it (scoped). |

### Item width, focus, layout

| Call | Notes |
|------|-------|
| `UI.setNextItemWidth(w)` | Width of next widget; negative = right-aligned from edge. |
| `UI.setNextItemOpen(open)` | Pre-set the open state of the next tree node / collapsing header. |
| `UI.setNextItemAllowOverlap()` | Allow next item to be overlapped by following items. |
| `UI.pushItemWidth(w)` / `UI.popItemWidth()` | Stack-based width override. |
| `UI.setKeyboardFocusHere(offset)` | Focus the previous or next widget. |
| `UI.setItemDefaultFocus()` | Default-focus the last submitted item. |
| `UI.calcTextSize(s)` | `{x, y}` for a string at the current font. |

### Cursor / metrics getters

| Call | Notes |
|------|-------|
| `UI.getContentRegionAvail()` | `{x, y}` of remaining space. |
| `UI.setCursorPos(x, y)` / `UI.setCursorScreenPos(x, y)` | Local / screen-space cursor placement. |
| `UI.getFontSize()` | Current font height in pixels. |
| `UI.getTextLineHeight()` / `UI.getTextLineHeightWithSpacing()` | Layout metrics. |
| `UI.getFrameHeight()` / `UI.getFrameHeightWithSpacing()` | Standard widget row heights. |
| `UI.getStyleColorVec4(name)` | Returns the live `[r, g, b, a]` for any `ImGuiCol_*` slot name. |

### Drag and drop

Drag-and-drop is exposed in its low-level Begin/End form so the same
shape used in the ImGui C++ API is available to scripts. Payloads
are **string-typed only** — `data` is a ZYM string, sent and received
verbatim (encode any structured data with `JSON.encode` or similar
yourself).

| Call | Notes |
|------|-------|
| `UI.beginDragDropSource()` / `UI.beginDragDropSource(flags)` | Call after submitting an item. If `true`, call `setDragDropPayload(...)` then `endDragDropSource()`. |
| `UI.setDragDropPayload(type, data)` | `type` is a user tag (max 32 chars; strings starting with `_` are reserved by ImGui). `data` is a ZYM string. Returns `true` once the payload is accepted. |
| `UI.endDragDropSource()` | Only call when `beginDragDropSource()` returned `true`. |
| `UI.beginDragDropTarget()` | Call after submitting an item that may receive a payload. |
| `UI.acceptDragDropPayload(type)` / `UI.acceptDragDropPayload(type, flags)` | Returns the payload string on delivery, or `null` if no matching payload was delivered this frame. |
| `UI.endDragDropTarget()` | Only call when `beginDragDropTarget()` returned `true`. |
| `UI.getDragDropPayload()` | Peek the current in-flight payload from anywhere as `{type, data, preview, delivery}` or `null`. |

Flag constants (use as bitmask in source/accept flags):

| Constant | Meaning |
|----------|---------|
| `UI.DND_SRC_NO_PREVIEW_TOOLTIP` | Disable the source tooltip preview. |
| `UI.DND_SRC_NO_DISABLE_HOVER` | By default, while dragging, no hover on the source. This flag re-enables it. |
| `UI.DND_SRC_NO_HOLD_TO_OPEN_OTHERS` | Don't allow holding the source to open tree nodes / collapsing headers. |
| `UI.DND_SRC_ALLOW_NULL_ID` | Allow drag-source on items with no ID. |
| `UI.DND_SRC_EXTERN` | Source is external to ImGui (e.g. OS drag). |
| `UI.DND_SRC_PAYLOAD_AUTO_EXPIRE` | Payload auto-expires if not re-set every frame. |
| `UI.DND_ACCEPT_BEFORE_DELIVERY` | `acceptDragDropPayload` returns the string while hovering, before the button is released; check `getDragDropPayload().delivery` to know if it was actually dropped. |
| `UI.DND_ACCEPT_NO_DRAW_DEFAULT_RECT` | Don't draw the default highlight rectangle on the target. |
| `UI.DND_ACCEPT_NO_PREVIEW_TOOLTIP` | Hide the source's preview tooltip from the target site. |

Minimal example:

```zym
UI.selectable("drag me", false)
if (UI.beginDragDropSource()) {
    UI.setDragDropPayload("ITEM", "row-7")
    UI.text("dragging row-7")
    UI.endDragDropSource()
}
UI.button("drop here")
if (UI.beginDragDropTarget()) {
    var data = UI.acceptDragDropPayload("ITEM")
    if (data != null) { print("dropped: %v", data) }
    UI.endDragDropTarget()
}
```

### Advanced table sort

For tables created with `ImGuiTableFlags_Sortable` (and friends),
`UI.tableGetSortSpecs()` returns the current sort instructions. It
must be called between `UI.table(...)`'s begin and end (i.e. from
inside the table body, the same constraint ImGui has for
`TableGetSortSpecs()`).

| Call | Returns |
|------|---------|
| `UI.tableGetSortSpecs()` | `list` of `{column, userId, direction, order}` maps, or `null` if no sort is active. |

Each entry:

- `column` — integer column index this sort applies to.
- `userId` — the `userId` you (optionally) passed to
  `UI.tableSetupColumn` (or `0` if none).
- `direction` — `"asc"`, `"desc"`, or `"none"`.
- `order` — sort priority (`0` is primary; only nonzero with
  `ImGuiTableFlags_SortMulti`).

Calling this also clears ImGui's internal `SpecsDirty` flag, so the
next frame won't re-report the same sort unless the user clicks a
header again.

### Font atlas internals

All atlas calls require an active ImGui context (i.e. they must be
made from inside a `UI.frame(win, ...)` body, or any other call site
that already has one). They affect the *current* context's atlas, so
each window/context has its own atlas state.

> Avoid adding or clearing fonts mid-frame — call these right after
> the first `UI.frame` tick for that window (when the context is
> lazily created) and before any text has been drawn for the frame.

| Call | Notes |
|------|-------|
| `UI.addFontDefault() -> Font \| null` | Push the built-in ProggyClean font onto the atlas and return its handle. |
| `UI.clearFonts()` | Clear all fonts from the atlas (next text submission will use defaults). |
| `UI.getFontCount() -> int` | Number of fonts currently in the atlas. |
| `UI.getFontAt(i) -> Font \| null` | Wrap the i-th font as a `Font` handle (same shape as `UI.defaultFont()`). |
| `UI.getFontTexSize() -> {w, h} \| null` | Backing texture dimensions (`null` before the atlas has been built). |
| `UI.getFontAtlasFlags() -> int` | Current atlas build flags (`ImFontAtlasFlags_*`). |
| `UI.setFontAtlasFlags(flags)` | Replace the atlas flags. |

Flag constants:

| Constant | Meaning |
|----------|---------|
| `UI.FONT_ATLAS_NONE` | No special flags. |
| `UI.FONT_ATLAS_NO_POWER_OF_TWO_HEIGHT` | Don't round the atlas height to the next power of two. |
| `UI.FONT_ATLAS_NO_MOUSE_CURSORS` | Don't bake software mouse cursors into the atlas. |
| `UI.FONT_ATLAS_NO_BAKED_LINES` | Don't bake thick line textures (saves memory; antialiased lines fall back to polygons). |

---

## Notes

- `UI` is gated behind the `ZYM_UI` build flag (default `ON` for the
  CLI, `OFF` for `RUNTIME_ONLY` builds). It has a soft dependency on
  the `SDL` native; trying to use `UI` without `SDL` in scope raises a
  runtime error at first call.
- ImGui contexts are created lazily per-window on the first `UI.frame`
  for that window. Two windows have independent ImGui state — no
  manual `SetCurrentContext` plumbing required.
- Stateful inputs follow the **ref convention** uniformly: single-
  element lists for scalars and colors, `Buffer` for text. Mixing
  forms (e.g. passing a plain `0` to `UI.sliderInt`) raises a runtime
  error.
