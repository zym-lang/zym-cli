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

### Lifetime

| Method | Returns | Notes |
| --- | --- | --- |
| `win.free()` | null | Tears down the ImGui context (if any), then the renderer, then the OS window. Safe to call more than once. |

`Window` handles are also finalised automatically when the VM garbage-
collects them; an explicit `.free()` is the deterministic option.

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
