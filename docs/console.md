# Console

`Console` is a singleton that exposes terminal output, styling, cursor and screen control, and basic input. It is auto-registered in every script as the global `Console`.

```zym
Console.setColor("green")
Console.writeLine("hello, console")
Console.reset()
```

## Conventions

- **Singleton.** Always called as `Console.method(...)`. There is no constructor; you do not create a `Console`.
- **Return value.** Methods return `null`. They do not chain — call them as separate statements.
- **Coordinates.** Cursor positions are 1-based (`moveCursor(1, 1)` is the top-left). `getWidth()` / `getHeight()` return the current terminal size in cells.
- **Streams.** Plain methods (`write`, `writeLine`, `writeBuffer`, `flush`) target standard output. The `*Err` variants (`writeErr`, `writeLineErr`, `writeBufferErr`, `isTTYErr`) target standard error. ANSI escapes from styling and cursor methods are written to standard output.
- **Buffers.** `writeBuffer` / `writeBufferErr` accept a `Buffer` (see `docs/buffer.md`); raw bytes are emitted as-is, no encoding conversion.
- **Booleans.** Methods that take a flag (`setBold(true)`, `setRawMode(false)`) accept a real boolean.
- **Restore on exit.** When the process ends or the `Console` is finalized, terminal state is restored: SGR is reset, the cursor is shown again, the alternate screen is exited, and raw-mode termios / Windows console mode are restored.
- **Errors.** Bad arguments raise a runtime error in the form `Console.method(...): <reason>` (e.g. `color must be 0..15`).

## Capabilities

| Method | Returns | Notes |
| --- | --- | --- |
| `Console.getWidth()` | number | Current terminal width in columns. Falls back to `80` if the size can't be queried. |
| `Console.getHeight()` | number | Current terminal height in rows. Falls back to `24` if the size can't be queried. |
| `Console.isTTY()` | bool | `true` if standard output is a terminal. |
| `Console.isTTYErr()` | bool | `true` if standard error is a terminal. |

## Writing

| Method | Notes |
| --- | --- |
| `Console.write(s)` | Writes `s` to stdout, no newline. |
| `Console.writeLine(s)` | Writes `s` to stdout followed by `\n`. |
| `Console.writeErr(s)` | Writes `s` to stderr, no newline. |
| `Console.writeLineErr(s)` | Writes `s` to stderr followed by `\n`. |
| `Console.writeBuffer(buf)` | Writes the raw bytes of a `Buffer` to stdout. |
| `Console.writeBufferErr(buf)` | Writes the raw bytes of a `Buffer` to stderr. |
| `Console.flush()` | Flushes pending stdout. Useful before reading input or before sleeping. |

## Colors and styles

Colors can be specified two ways on every color setter:

- **By index** — a number from `0` to `15` (the standard 16 ANSI colors).
- **By name** — a string. Recognized names: `black`, `red`, `green`, `yellow`, `blue`, `magenta`, `cyan`, `white`, and their bright variants `brightBlack` (alias `gray`), `brightRed`, `brightGreen`, `brightYellow`, `brightBlue`, `brightMagenta`, `brightCyan`, `brightWhite`. The legacy snake_case forms (`bright_red`, etc.) are also accepted for compatibility.

| Method | Notes |
| --- | --- |
| `Console.setColor(c)` | Sets the foreground color (number 0–15 or name string). |
| `Console.setBackgroundColor(c)` | Sets the background color (number 0–15 or name string). |
| `Console.setColorRGB(r, g, b)` | Sets the foreground to a 24-bit truecolor. Each component is `0..255`. |
| `Console.setBackgroundColorRGB(r, g, b)` | Sets the background to a 24-bit truecolor. Each component is `0..255`. |
| `Console.reset()` | Clears all color and style attributes (back to terminal default). |
| `Console.setBold(on)` | Enables/disables bold. |
| `Console.setDim(on)` | Enables/disables dim. |
| `Console.setItalic(on)` | Enables/disables italic. |
| `Console.setUnderline(on)` | Enables/disables underline. |
| `Console.setReverse(on)` | Enables/disables reverse video. |
| `Console.setStrikethrough(on)` | Enables/disables strikethrough. |

Truecolor and several styles depend on the terminal. On terminals that don't support them, the escape is still sent and is silently ignored or downgraded by the host.

## Cursor

| Method | Notes |
| --- | --- |
| `Console.moveCursor(row, col)` | Absolute move to `(row, col)`, 1-based. |
| `Console.moveCursorUp(n)` | Move up. `n` defaults to `1`. |
| `Console.moveCursorDown(n)` | Move down. `n` defaults to `1`. |
| `Console.moveCursorLeft(n)` | Move left. `n` defaults to `1`. |
| `Console.moveCursorRight(n)` | Move right. `n` defaults to `1`. |
| `Console.hideCursor()` | Hides the cursor. Restored automatically at finalization. |
| `Console.showCursor()` | Shows the cursor. |
| `Console.saveCursorPos()` | Saves the current cursor position (DECSC). |
| `Console.restoreCursorPos()` | Restores a previously saved position (DECRC). |

## Clearing and scrolling

| Method | Notes |
| --- | --- |
| `Console.clear()` | Clears the entire screen and homes the cursor. |
| `Console.clearLine()` | Clears the current line. |
| `Console.clearToEndOfLine()` | Clears from the cursor to the end of the line. |
| `Console.clearToStartOfLine()` | Clears from the start of the line to the cursor. |
| `Console.scrollUp(n)` | Scrolls the screen up `n` lines. `n` defaults to `1`. |
| `Console.scrollDown(n)` | Scrolls the screen down `n` lines. `n` defaults to `1`. |

## Screen modes

| Method | Notes |
| --- | --- |
| `Console.useAltScreen()` | Switches to the alternate screen buffer (full-screen TUI). The previous screen is preserved by the terminal. |
| `Console.useMainScreen()` | Returns to the main screen. Also done automatically at finalization if alt was active. |
| `Console.setTitle(s)` | Sets the terminal window title. |
| `Console.beep()` | Emits an audible/visual bell (`\a`). |

## Input

| Method | Returns | Notes |
| --- | --- | --- |
| `Console.readLine()` | string \| null | Reads one line from stdin. Returns `null` on EOF. The trailing newline is stripped. |
| `Console.readChar()` | string \| null | Reads a single character. In line-buffered mode this still waits for Enter; combine with `setRawMode(true)` to read keystrokes immediately. Returns `null` on EOF. |
| `Console.hasInput()` | bool | `true` if at least one byte is available on stdin without blocking. |
| `Console.setRawMode(on)` | null | Enables/disables raw mode. In raw mode echo is off and input is delivered byte-by-byte. State is restored at finalization regardless. |

## Example

```zym
Console.setColor("green")
Console.writeLine("hello, console")
Console.reset()

print("size = %nx%n", Console.getWidth(), Console.getHeight())
print("isTTY = %v", Console.isTTY())

Console.setColorRGB(255, 128, 0)
Console.write("orange ")
Console.setBold(true)
Console.writeLine("and bold")
Console.reset()

Console.setBackgroundColor("blue")
Console.write(" bg ")
Console.reset()
Console.writeLine("")

var b = Buffer.fromString("via buffer\n")
Console.writeBuffer(b)
Console.flush()
```

## Notes & gotchas

- **Methods do not chain.** Each call returns `null`. Write `Console.setColor("red")` and `Console.writeLine("...")` as separate statements; `Console.setColor("red").writeLine(...)` is a runtime error.
- **Restore is best-effort.** Terminal state (SGR, cursor visibility, alt screen, raw mode) is restored when the process ends normally or the `Console` is garbage-collected. A hard kill (`SIGKILL`) bypasses this; if your script enters raw mode or the alt screen and crashes that way, run `reset` from your shell to recover.
- **Non-TTY output.** If stdout is redirected to a file or pipe, color and cursor escapes are still written. Pipe through a tool that strips ANSI, or guard styling with `if (Console.isTTY()) { ... }`.
- **Truecolor and styles.** `setColorRGB` and styles like `setStrikethrough` rely on terminal support. Unsupported escapes are silently dropped by the terminal — output remains correct, just unstyled.
- **Bell.** `beep()` writes `\a`. Many terminals visualize this as a flash; some do nothing.
- **Reading after writing.** Call `Console.flush()` before `readLine()` if you wrote a prompt without a trailing newline, otherwise the prompt may not appear before the read blocks.
- **Raw mode + input.** `readChar()` returns one byte. Multi-byte UTF-8 characters and escape sequences (arrow keys, function keys, mouse events) arrive as several bytes — read repeatedly and parse at the script level if you need them.
- **`hasInput()` only checks stdin.** It does not check whether stdout is ready to write.
- **Windows.** On Windows the console is switched to UTF-8 and virtual-terminal processing is enabled at first use; older terminals (legacy `cmd.exe` without VT) will display escape sequences literally.
