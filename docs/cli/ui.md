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

- **Refs (color).** A color ref is a 4-element list of normalised
  floats `[r, g, b, a]` (each `0.0 .. 1.0`). `UI.colorEdit` /
  `UI.colorPicker` write the edited components back into the list.
  Packed colors (32-bit `0xAABBGGRR` for `DrawList` primitives) are
  separate; build them with `UI.color(r, g, b, a)` or pass an
  already-packed integer.
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
| `UI.textColored(color, s)` | null | Text in the given 4-elem color ref (`[r, g, b, a]`). |
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
| `UI.colorEdit(label, ref)` | Color editor; `ref` is a 3- or 4-element float list. |
| `UI.colorPicker(label, ref)` | Full picker widget. |
| `UI.colorButton(id, color)` | Just the swatch; returns `true` on click. |
| `UI.color(r, g, b, a)` | Packs four `0.0 .. 1.0` floats into a 32-bit color suitable for `UI.drawRect` / `UI.drawText` / etc. |

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

These draw into the current ImGui window's foreground draw list, so
they must be called inside a `UI.window` (or `UI.child`) body. All
colors are packed 32-bit values — build them with `UI.color(r, g, b, a)`.

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
| `UI.loadFont(path, sizePx)` | Font \| null | Load a TTF/OTF at the given pixel size into the active context's atlas. Returns `null` and stamps `UI.lastError()` on failure (missing file, bad font, no active context). |
| `UI.loadFont(path, sizePx, opts)` | Font \| null | Same, with an options map. `opts` is currently reserved for future glyph-range / merge options and is ignored. |
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
