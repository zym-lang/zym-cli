# `SDL`

Thin SDL3 substrate exposed to scripts as the global identifier `SDL`.
Its job in the current build is to give the `UI` native (Dear ImGui) a
window and an event pump to talk to; scripts can also use it directly
to open a window, drive an event loop, and query input state. The
global is a map-shaped namespace; per-window calls live on a `Window`
instance returned by `SDL.createWindow`.

---

## Conventions

- **Numbers.** Window sizes, positions, scancodes, button indices, and
  timeouts are passed and returned as Zym numbers. Coordinates are in
  window pixels (HiDPI is opt-in via the `hidpi` window flag).
- **Booleans.** Required where the API takes a flag (`Window.setVSync`,
  `Window.setFullscreen`, etc.). Passing a non-bool raises a runtime
  error.
- **Strings.** Window titles and text-input event payloads are Zym
  strings.
- **Maps as event payloads.** `SDL.pollEvent` / `SDL.waitEvent` return
  a Zym map whose `type` key is a dot-namespaced string (e.g.
  `"key.down"`, `"mouse.motion"`) and whose other keys are per-type
  fields. `null` means "no event available right now".
- **Lifetime.** SDL initialises lazily on first use and refcounts
  across nested VMs. Calling `SDL.quit()` from a nested VM is a no-op;
  real teardown happens at process exit.
- **Threading.** All `SDL.*` and `Window.*` calls must happen on the
  VM's main thread.
- **Errors.** Factory calls that fail (`SDL.createWindow`) return
  `null`; the underlying SDL message is available via
  `SDL.lastError()`. Bad argument types raise a Zym runtime error of
  the form `SDL.method(args) expects a <type>` or
  `Window.method(args) expects a <type>`.

---

## Module-level methods

### Initialisation

| Method | Returns | Notes |
| --- | --- | --- |
| `SDL.init()` | bool | Idempotent. Initialises `video` + `events` subsystems. Called automatically on first `SDL.createWindow`. |
| `SDL.quit()` | null | Decrements the refcount. Process-wide teardown only happens at exit; calling from a nested VM is a no-op. |
| `SDL.lastError()` | string | Most recent SDL error message, or `""` if none. Mirrors `SDL_GetError()`. |
| `SDL.version()` | map | `{ major, minor, patch }` of the linked SDL3. |

### Windows

| Method | Returns | Notes |
| --- | --- | --- |
| `SDL.createWindow(title, w, h, opts?)` | `Window` \| `null` | Creates a window with a default-driver renderer attached. `opts` is an optional map (see below). Returns `null` on failure. |

`opts` keys (all optional):

| Key | Type | Effect |
| --- | --- | --- |
| `resizable` | bool | User can resize the window. |
| `borderless` | bool | No window border / title bar at the OS level. |
| `hidden` | bool | Window is created hidden; call `.show()` to display it. |
| `alwaysOnTop` | bool | Floats above other windows. |
| `hidpi` | bool | Request a high-DPI pixel-density window. |
| `posX`, `posY` | number | Initial window position; otherwise centered. |

### Events

| Method | Returns | Notes |
| --- | --- | --- |
| `SDL.pollEvent()` | event \| `null` | Returns the next queued event, or `null` if the queue is empty. Drives the close-flag and forwards each event to ImGui (when `UI` is loaded). |
| `SDL.waitEvent(timeoutMs?)` | event \| `null` | Blocks until an event arrives or the timeout expires. Pass `null` to wait indefinitely. Returns `null` on timeout. |
| `SDL.pushEvent(event)` | bool | Pushes a no-op user event onto the queue so a blocked `SDL.waitEvent` wakes up. The `event` argument is currently unused; payload-mapping back into `SDL_Event` is not exposed yet. |

### Input state queries

In addition to events, scripts can poll current input state directly.

| Method | Returns | Notes |
| --- | --- | --- |
| `SDL.keyDown(scancode)` | bool | `true` if the SDL3 scancode is currently held. |
| `SDL.mousePos()` | map | `{ x, y }` in window-local coordinates. |
| `SDL.mouseButtons()` | map | `{ left, right, middle }` booleans. |
| `SDL.modifiers()` | map | `{ shift, ctrl, alt, super }` booleans for current modifier state. |

### Image I/O

PNG + JPG are decoded / encoded via SDL_image (vendored — see
`future/gui.md` §2.1). BMP routes through SDL3 core's
`SDL_LoadBMP` / `SDL_SaveBMP` directly. All four entry points return a
`Surface` (CPU-side image) — never a `Texture` — so the "edit before
showing" workflow is encoded into the type system.

| Method | Returns | Notes |
| --- | --- | --- |
| `SDL.loadImage(path)` | `Surface` \| `null` | Decode the file at `path`. Format is sniffed from the extension; falls back to magic-byte sniff inside SDL_image. Returns `null` on failure (see `SDL.lastError`). |
| `SDL.loadImageFromBuffer(buf, fmt)` | `Surface` \| `null` | Decode the bytes in the `Buffer`. `fmt` is a hint (`"png"` / `"jpg"` / `"bmp"`) — pass `null` to let SDL_image sniff. |
| `SDL.saveImage(surface, path, fmt)` | bool | Encode `surface` to `path`. `fmt` defaults to the path extension (`"png"` if none); pass `null` to use the default. PNG/JPG go through SDL_image; BMP through SDL3 core. JPEG quality is fixed at 90. |
| `SDL.saveImageToBuffer(surface, fmt)` | `Buffer` \| `null` | Same as `saveImage` but returns the encoded bytes as an in-memory `Buffer` instead of writing to disk. Useful for sending encoded images over the network, embedding into a packed app, or computing hashes without touching the filesystem. |

> All arguments are required — pass `null` for any optional you want to
> skip (the bridge takes `null` to mean "use the default"). This mirrors
> the rest of the SDL surface; named-argument syntax is not part of the
> language today.

### `SDL.Surface` factories

Surfaces can also be created directly without decoding an image:

| Method | Returns | Notes |
| --- | --- | --- |
| `SDL.Surface.new(w, h, fmt)` | `Surface` \| `null` | Allocate a fresh surface, zero-filled. `fmt` is a string like `"RGBA32"` (default), `"ARGB32"`, `"RGB24"`, `"INDEX8"` — pass `null` for `"RGBA32"`. |
| `SDL.Surface.fromBuffer(buf, w, h, pitch, fmt)` | `Surface` \| `null` | Allocate a fresh `w`×`h` surface in `fmt` (default `"RGBA32"` when `fmt` is `null`) and **copy** the pixel bytes from `buf` row-by-row using `pitch` as the source row stride. The buffer is consumed at call time — mutating it afterwards does not affect the surface. |

---

## Events

Each event returned by `SDL.pollEvent` / `SDL.waitEvent` is a map with
a `type` string and per-type fields. The full list of types in the
current build:

| `type` | Fields |
| --- | --- |
| `"quit"` | — |
| `"key.down"` | `scancode`, `key`, `mod`, `repeat`, `window` |
| `"key.up"` | `scancode`, `key`, `mod`, `repeat`, `window` |
| `"mouse.down"` | `button`, `x`, `y`, `clicks`, `window` |
| `"mouse.up"` | `button`, `x`, `y`, `clicks`, `window` |
| `"mouse.motion"` | `x`, `y`, `dx`, `dy`, `window` |
| `"mouse.wheel"` | `x`, `y`, `window` |
| `"text.input"` | `text`, `window` |
| `"window.resize"` | `w`, `h`, `window` |
| `"window.focus"` | `window` |
| `"window.blur"` | `window` |
| `"window.close"` | `window` |
| `"other"` | `raw` (the raw SDL3 event-type integer) |

`window` is the SDL3 windowID of the source window. `scancode` /
`key` / `mod` are raw SDL3 enum values.

---

## `Window`

Returned by `SDL.createWindow`. Methods are invoked as
`win.method(...)`.

### State

| Method | Returns | Notes |
| --- | --- | --- |
| `win.shouldClose()` | bool | `true` once the OS has requested the window be closed (the close flag is set automatically by `pollEvent` / `waitEvent` when a `"window.close"` event arrives). Sticky — stays `true` after the first close request. |
| `win.getTitle()` | string | Current OS window title. |
| `win.setTitle(s)` | null | Sets the OS window title. |
| `win.getSize()` | map | `{ w, h }` of the current window size. |
| `win.setSize(w, h)` | null | Resizes the window. |
| `win.getPosition()` | map | `{ x, y }` of the window's top-left corner. |
| `win.setPosition(x, y)` | null | Moves the window. |

### Visibility / mode

| Method | Returns | Notes |
| --- | --- | --- |
| `win.show()` | null | Maps the window. |
| `win.hide()` | null | Unmaps the window. |
| `win.minimize()` | null | Iconifies the window. |
| `win.maximize()` | null | Maximises the window. |
| `win.restore()` | null | Restores a minimised / maximised window to its previous size. |
| `win.setFullscreen(b)` | bool | Toggle fullscreen; returns the success flag from SDL. |
| `win.setVSync(b)` | bool | Enable / disable VSync on the attached renderer. Returns `false` if no renderer is attached. |

### Texture factories

Textures are bound to the window's renderer; this is why these
factories live on `Window` rather than on `SDL` itself. A texture
created from one window cannot be drawn through another window's
renderer.

| Method | Returns | Notes |
| --- | --- | --- |
| `win.createTexture(w, h, opts)` | `Texture` \| `null` | Allocate a fresh `w`×`h` texture. `opts` is a map of `{ access, format, linkSurface }`. `access` ∈ `"static"` / `"streaming"` (default) / `"target"`; `format` is a pixel-format name like `"RGBA32"` (default); `linkSurface` is an optional `Surface` value to record as the texture's source so `tex.refresh()` works. Pass `null` for `opts` to take all defaults. |
| `win.textureFromSurface(surface, opts)` | `Texture` \| `null` | Upload the pixels of `surface` to a new texture sized to match. `opts.link` (bool, default `false`) records `surface` as the texture's source so `tex.refresh()` and `tex.source()` work. Pass `null` for `opts` to default to "no link". |

### Lifetime

| Method | Returns | Notes |
| --- | --- | --- |
| `win.free()` | null | Tears down the ImGui context (if any), then the renderer, then the OS window. Safe to call more than once. |

`Window` handles are also finalised automatically when the VM garbage-
collects them; an explicit `.free()` is the deterministic option.

---

## `Surface`

Returned by `SDL.loadImage*` / `SDL.Surface.new` / `SDL.Surface.fromBuffer`
/ `surf.clone` / `surf.convert`. A `Surface` is a CPU-side, mutable
pixel buffer — the "unit of authorship" in the image pipeline. Edits
(blits, fills, masks, per-pixel writes) all happen on a `Surface`;
displaying it via the UI native is a separate publish step against a
`Texture` (deferred to a later slice).

### Queries

| Method | Returns | Notes |
| --- | --- | --- |
| `surf.size()` | map | `{ w, h }` of the surface in pixels. |
| `surf.format()` | string | Short pixel-format name (e.g. `"RGBA32"`, `"ABGR8888"`, `"RGB24"`). |
| `surf.pitch()` | number | Bytes per row of pixels — `w × bpp` rounded up to SDL's alignment. |

### Whole-surface ops

| Method | Returns | Notes |
| --- | --- | --- |
| `surf.clone()` | `Surface` \| `null` | Deep copy of the surface (pixels + format). |
| `surf.convert(fmt)` | `Surface` \| `null` | Returns a fresh surface with the same pixels in `fmt`. Errors if `fmt` is not recognised. |
| `surf.fill(color, rect)` | bool | Fill `rect` (or the whole surface when `rect` is `null`) with `color`. Color is either a 3/4-element list (floats `0..1` or ints `0..255`) or a packed `0xAARRGGBB` number. |
| `surf.clear(color)` | bool | Shorthand for filling the whole surface; pass `null` for `[0,0,0,0]` (fully transparent). |

### Pixel access

These are *slow* at script speed — they cross the Zym↔C boundary for
every pixel. Favour bulk ops (`fill`, `blit`, `applyMask`) when
possible. A future revision will add a `lock(body)` / `pixels()`
`Buffer` escape hatch.

| Method | Returns | Notes |
| --- | --- | --- |
| `surf.getPixel(x, y)` | list \| `null` | `[r, g, b, a]` in floats `0..1`. Returns `null` if `(x, y)` is out of bounds. |
| `surf.setPixel(x, y, color)` | bool | Write a single pixel. Same color shape as `fill`. |

### Blits / composition

| Method | Returns | Notes |
| --- | --- | --- |
| `surf.blit(src, srcRect, dstRect)` | bool | Copy from `src` into this surface. Pass `null` for either rect to mean "whole thing". |
| `surf.blitScaled(src, srcRect, dstRect, mode)` | bool | Like `blit` but rescales. `mode` ∈ `"nearest"` / `"linear"`; pass `null` for `"linear"`. |
| `surf.setBlendMode(mode)` | bool | `mode` ∈ `"none"` / `"blend"` / `"add"` / `"mod"` / `"mul"`. |
| `surf.getBlendMode()` | string | Current blend mode name (or `"custom"` for non-standard). |
| `surf.setAlphaMod(a)` | bool | Per-blit alpha multiplier `0..255`. |
| `surf.setColorMod(r, g, b)` | bool | Per-blit colour multiplier `0..255`. |
| `surf.setColorKey(color)` | bool | Treat pixels matching `color` as fully transparent during blits. Pass `null` to clear the key. |
| `surf.getClipRect()` | map | `{ x, y, w, h }` of the current clip rectangle. |
| `surf.setClipRect(rect)` | bool | Restrict subsequent draws to `rect`. Pass `null` to clear. |

### Masking

`applyMask` is the headline composability verb — there is no
equivalent in SDL3 core, so we implement it on top of the per-pixel
read/write API.

| Method | Returns | Notes |
| --- | --- | --- |
| `surf.applyMask(mask, mode)` | bool | Combine the alpha channel of `mask` into `surf`. `mode` ∈ `"alpha"` (default, `dst.a *= mask.a / 255`), `"luminance"` (`dst.a *= luma(mask.rgb) / 255` using BT.601), `"key"` (`dst.a := 0` where `mask.a == 0`, else unchanged). Pass `null` for the default. The mask is read pixel-by-pixel; it can be any format SDL understands. |

### Lifetime

| Method | Returns | Notes |
| --- | --- | --- |
| `surf.free()` | null | Releases the pixel buffer immediately. Safe to call more than once. |

`Surface` handles are also finalised automatically by the GC; an
explicit `.free()` is the deterministic option, useful when working
with large images.

---

## `Texture`

Returned by `win.createTexture` / `win.textureFromSurface`. A
`Texture` is a GPU-side image bound to a specific `Window`'s renderer
— the "unit of display". Pixel composition happens upstream on a
`Surface`; the texture is the thin "publish" wrapper that the
renderer (and, in a later slice, ImGui via `UI.image`) samples.

The texture handle is **stable for the lifetime of the texture** —
`update` / `updateRect` / `refresh` mutate the GPU contents behind
the handle, they never invalidate it. Any consumer that already has
the handle will sample the updated pixels on its next draw without
the script re-issuing the call. This is the property that makes the
"hand a texture to UI once, then keep editing the source surface"
pattern work in future slices.

### Queries

| Method | Returns | Notes |
| --- | --- | --- |
| `tex.size()` | map | `{ w, h }` of the texture in pixels. |
| `tex.source()` | `Surface` \| `null` | The linked source surface (only if the texture was created with `link: true` / `linkSurface: ...`); otherwise `null`. |

### Publishing pixels

`update` / `updateRect` are the explicit "publish" calls — they
re-upload pixels from a `Surface` into the texture. The source
surface does not need to be the one (if any) the texture was linked
to; any surface will do, and the bridge converts pixel formats
in-flight if they don't match.

`refresh` is the ergonomic shorthand for the linked-surface workflow:
"re-upload from my source surface, no need to pass it again". It
requires the texture was created with `link: true` (or
`linkSurface: ...`) and raises a runtime error otherwise.

| Method | Returns | Notes |
| --- | --- | --- |
| `tex.update(surface, dstRect)` | bool | Full or partial re-upload. Pass `null` for `dstRect` to upload the whole surface. |
| `tex.updateRect(surface, x, y, w, h)` | bool | Partial re-upload using an explicit dst rectangle. |
| `tex.refresh(dirtyRect)` | bool | Re-upload from the linked source surface. Pass `null` for `dirtyRect` to refresh the whole texture. Errors if the texture was not created with `link`. |

### Modes / mods

| Method | Returns | Notes |
| --- | --- | --- |
| `tex.setBlendMode(mode)` | bool | `mode` ∈ `"none"` / `"blend"` / `"add"` / `"mod"` / `"mul"`. |
| `tex.setScaleMode(mode)` | bool | `mode` ∈ `"nearest"` / `"linear"` (default). |
| `tex.setAlphaMod(a)` | bool | Per-draw alpha multiplier `0..255`. |
| `tex.setColorMod(r, g, b)` | bool | Per-draw colour multiplier `0..255`. |

### Lifetime

| Method | Returns | Notes |
| --- | --- | --- |
| `tex.free()` | null | Releases the GPU texture immediately. Safe to call more than once. |

A `Texture` is also finalised automatically by the GC. When the
owning `Window` (or its renderer) is destroyed before the texture's
finaliser runs, the texture is considered "already torn down" and
the finaliser is a no-op — using such a texture afterwards raises a
clean Zym runtime error ("owning Window/renderer has been
destroyed"), never UAF.

### Note on `texture.window()`

`texture.window()` is **not exposed in the current build.** The
texture knows which renderer it was created against (and rejects
draws after the owning window has been freed), but does not hand the
script back the user-facing Window value. Scripts that need to
correlate textures with windows should keep their own
`{ window, texture }` records. This is the only Slice-2 §2.2 method
intentionally omitted from the PR-6 surface.

---

## Example

```zym
print("sdl version: %v", SDL.version())

var win = SDL.createWindow("zym window", 640, 360, {
    resizable: true,
})
if (win == null) {
    print("createWindow failed: %s", SDL.lastError())
    return
}

while (!win.shouldClose()) {
    var e = SDL.pollEvent()
    while (e != null) {
        if (e.type == "quit") {
            print("got quit")
        } else if (e.type == "key.down") {
            print("scancode=%v key=%v", e.scancode, e.key)
        }
        e = SDL.pollEvent()
    }
    // Sleep until the next event so we don't spin the CPU.
    SDL.waitEvent(50)
}

win.free()
```

---

## Notes

- `SDL.createWindow` attaches a default-driver renderer to the window
  automatically; that renderer is what `UI` (ImGui) draws into. Scripts
  do not currently get direct access to the renderer surface — raw 2D
  drawing outside of an ImGui window is not part of the current build.
- The `"other"` event type catches anything that isn't one of the
  handled types above; `raw` holds the SDL3 event-type integer if you
  need to dispatch on it.
- The close-flag stamped by `pollEvent` / `waitEvent` is per-window and
  is matched by SDL3 windowID. Two windows can be closed independently.
