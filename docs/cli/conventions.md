# Native API Conventions

This document collects the cross-cutting conventions shared by every
native module (`File`, `Dir`, `Process`, `Buffer`, `RegEx`, `JSON`,
`Crypto`, `Random`, `Hash`, `System`, `Path`, `Time`, `Console`, and the
networking surface to come). It exists so per-native docs can stay
focused on what each module *does* and refer here for *how it returns
values, signals failure, and exchanges bytes*.

If a native diverges from anything below, it documents the divergence
in its own page.

---

## Identifiers and globals

- All natives are registered as **uppercase global identifiers** at VM
  startup: `File`, `Dir`, `Process`, `Buffer`, `RegEx`, `JSON`,
  `Crypto`, `Random`, `Hash`, `System`, `Path`, `Time`, `Console`,
  plus the standalone `print(...)` function.
- Static (singleton) methods are reached as `Module.method(...)`.
- Instance methods (where the module has a `create(...)`) are reached
  as `inst.method(...)` after `var inst = Module.create(...)`.

---

## Return shapes

zym natives use a small, deliberate vocabulary for return values.
Pick the rightmost row in the table that fits a method's purpose.

| Shape                       | When to use                                                                | Example                                                |
| --- | --- | --- |
| **Plain value**             | The method always succeeds for valid arguments.                            | `Path.dirname(p) -> string`, `Time.ticksMsec() -> number` |
| **`bool`**                  | A yes/no question, or "did the operation succeed?" when there is no further detail to report. | `Path.isAbsolute(p)`, `Crypto.verify(...)`, `Dir.exists(p)` |
| **Value or `null`**         | The operation may fail and the only useful information on failure is *that* it failed. `null` is the documented failure indicator; success returns the typed value. | `File.open(...) -> file \| null`, `JSON.parse(text) -> value \| null`, `RegEx.create(pat) -> regex \| null` |
| **Status string**           | The operation has more than two outcomes that the caller is expected to branch on. Returns a short lowercase string from a documented vocabulary. | `Process.spawn(...) -> { status: "ok" \| "spawn_failed" \| ..., ... }` |
| **Result map**              | Multiple values that travel together (data + metadata, e.g. an exit code plus stdout). | `Process.exec(...) -> { exitCode, stdout, stderr }`    |
| **Runtime error**           | Argument types are wrong, or the call is structurally invalid (e.g. `Path.dirname(42)`). | All natives raise typed runtime errors of the form `Module.method(args) expects a <type>`. |

### Status-string vocabulary

When a native returns a string status — used today by `Process.spawn`,
and reserved for the upcoming networking natives — values are drawn
from this single shared vocabulary so scripts can match against
consistent strings:

| String        | Meaning                                                                                  |
| --- | --- |
| `"ok"`        | The operation completed successfully.                                                    |
| `"busy"`      | The operation cannot make progress right now; retry is appropriate (non-blocking I/O).   |
| `"timeout"`   | A bounded wait elapsed before the operation could complete.                              |
| `"eof"`       | A reader observed end-of-stream (peer closed cleanly, file finished).                    |
| `"closed"`    | The handle is no longer valid for the requested operation.                               |
| `"error"`     | Something went wrong that doesn't fit the categories above and cannot be retried.        |
| `"not_found"` | The named target does not exist (lookup miss, missing key/file/host).                    |
| `"denied"`    | The operation is refused for a permissions / authorization reason.                       |

A native that returns one of these strings documents which subset it
actually produces. Scripts should `switch` on string equality and
treat unknown values defensively.

### Errors raised vs. returned

- **Programming bugs** (wrong type, missing argument, calling on the
  wrong kind of value) raise a Zym **runtime error** with a message of
  the form `Module.method(args) ...`. They unwind the stack and stop
  the script unless caught.
- **Recoverable / expected failures** (file not found, parse error,
  network busy, peer closed) are returned via one of `null`, a status
  string, or a result map. They never raise.
- This split means a script that handles `null`/status-string returns
  doesn't need exception handling around every native call; only
  programmer mistakes propagate as errors.

---

## Bytes and string interop

`Buffer` is the byte-interop currency between every native that talks
about raw bytes (`File`, `Process`, `Crypto`, `Hash`, `Buffer` itself,
and the networking natives to come).

| Direction            | Pattern                                                       |
| --- | --- |
| Bytes → script       | A native that produces bytes returns a `Buffer`. It does **not** return a hex string, base64, or array of integers unless the method is explicitly named that way (e.g. a hypothetical `*Hex()`/`*Base64()` helper). |
| Script → native      | A method that consumes bytes accepts a `Buffer`. Strings are accepted only when the docs say so; in those cases they are encoded as **UTF-8** before being handed to the underlying call. |
| Mixed text/bytes     | Methods named `readAllText` / `writeAllText` work in `string`; methods named `readAllBytes` / `writeAllBytes` / `read` / `write` work in `Buffer`. |
| Decoding             | `Buffer.toString()` decodes the buffer as UTF-8. Invalid sequences are replaced rather than raising. |

Concretely:

- `File.read(n)` returns a `Buffer`, not a string.
- `Process.exec(...)` returns `stdout`/`stderr` as `Buffer` (so binary
  child output round-trips losslessly); `Process.exec(...).stdout`
  is the buffer, and the script does `.toString()` if it wants text.
- `Crypto.sign(...)` / `Hash.digest(...)` return `Buffer`; the script
  encodes them to hex / base64 itself if it needs an over-the-wire
  representation.
- `Buffer.compress(algo, level?)` and `Buffer.decompress(algo, max)`
  work `Buffer -> Buffer`.
- The upcoming networking natives consume and produce `Buffer` for
  every payload; status / metadata travel as separate fields in the
  result map.

---

## Numbers

- Zym numbers are double-precision floats. Native methods that
  conceptually return integers (counts, timestamps, lengths,
  exit codes, file sizes) still return them as `number`. Values up to
  `2^53` round-trip exactly; beyond that the usual IEEE-754 caveats
  apply (e.g. nanosecond timestamps near 2106 lose low bits).
- Where a native exposes an underlying 64-bit state that **cannot**
  round-trip through a double (e.g. PCG full state, full PCRE2 group
  pointers), the native documents the precision caveat in its own
  page (`Random` does this).

---

## Strings

- All string-returning methods produce **UTF-8**. Where the underlying
  platform call returns a wide-character string (Windows path APIs,
  Win32 console), the native transcodes before returning.
- All string-accepting methods are interpreted as UTF-8. ASCII inputs
  are a strict subset and always work.
- Methods that compare strings do so byte-for-byte unless the doc
  explicitly says otherwise (`RegEx`'s `(?i)` flag,
  `System.systemDir`'s case-insensitive `name` lookup).

---

## Lifecycle and aliasing

- Natives that own an external resource (`File`, `Process`, `Dir`
  iterators, `RegEx` instances, `Crypto*` instances, network
  sockets) expose a `create` / `open` / `compile` factory and a
  `close` (or implicit GC finalizer) for cleanup.
- Multiple zym variables binding to the same instance share the same
  underlying handle. Calling `close()` on one variable invalidates the
  other; subsequent calls on either return the documented "closed"
  shape (`null` for value-returning methods, `"closed"` for
  status-string returns).
- A native that does **not** own external resources (`Path`, `JSON`,
  `Hash` digest one-shots, `Time` static methods, `System` static
  methods) is purely functional and has no lifecycle.

---

## Errors from the underlying engine

When a native delegates to engine code that itself prints to stderr on
internal failure, those messages can appear in the form
`ERROR: <text>\n   at: <function> (<file>:<line>)`. Whether they
appear is controlled by the build-time `USE_ENGINE_ERRORS` toggle
(default `ON`). Building with `USE_ENGINE_ERRORS=OFF` silences them
globally; the native's own return value (`null`, status string, etc.)
is unchanged either way.

---

## What is intentionally NOT a convention

- **No exceptions.** Native code never raises a typed exception; it
  either returns a value or runs to completion, or — for programmer
  mistakes only — raises a runtime error that unwinds the script.
- **No callbacks from native into script.** No native takes a zym
  closure as a "called when done" argument. zym is single-threaded
  and natives complete synchronously; for long-running operations
  that need progress, the native exposes a polling / iteration
  interface (e.g. `Dir.list` enumerator, future socket `poll()`).
- **No promises / async values.** Where blocking would be unwise,
  natives expose timeouts directly (`System.sleep(ms)`, future
  `socket.read(n, timeoutMs)`).
- **No global mutable state on the native side.** Every native is
  either purely functional or owns its state behind a handle. The
  shared exceptions are `System.setEnv` / `Process.setEnv` (which
  mutate the *process* environment by definition) and the global
  `print(...)` function (which writes to stdout).

---

## Quick reference for new natives

When adding a new native, follow these rules and you'll match the
existing surface:

1. Register the global as a single uppercase identifier.
2. Use plain values where the call always succeeds.
3. Use `null` to signal "expected failure where the only useful
   information is *that* it failed".
4. Use a status string from the table above when the script must
   distinguish more than two outcomes.
5. Use a result map for multi-value returns.
6. Raise a runtime error only for argument-shape problems.
7. Speak `Buffer` for bytes, UTF-8 strings for text.
8. Document any deviation in the native's own page.
