# `File`

File I/O. The global identifier `File` is a namespace of static helpers and
opener constructors. Opening a file returns a **file handle** whose instance
methods are invoked as `f.method(...)`.

---

## Conventions

- **Paths.** Strings are interpreted as filesystem paths (absolute or relative
  to the current working directory). No virtual-filesystem prefixes are
  applied.
- **Modes.** `open*` functions take a mode string:
  - `"r"`  — read-only; file must exist.
  - `"w"`  — write; truncates or creates.
  - `"rw"` — read + write; file must exist, position starts at 0.
  - `"wr"` — write + read; truncates or creates.
- **Numbers.** Sizes, offsets, positions, byte values, and timestamps are
  Zym numbers. Integer methods truncate toward zero.
- **Buffers.** Byte I/O uses `Buffer` instances (see [buffer.md](buffer.md)).
- **Endianness.** Typed multi-byte reads/writes accept an optional trailing
  `"le"` (default) or `"be"` string. See [Endianness](#endianness).
- **Open failures.** `open`, `openCompressed`, and `openEncryptedPass` return
  `null` on failure (check with `if f == null`). Use `lastError()` / the
  `getError()` method on a handle for a numeric status. Whole-file helpers
  (`File.readText` / `File.readBytes`) instead raise a runtime error.
- **Errors.** Invalid argument types, bad modes, negative sizes, or operations
  on a closed handle produce a Zym runtime error of the form
  `File.method(args) ...`.

---

## Opening files

| Function | Returns |
| --- | --- |
| `File.open(path, mode)` | handle, or `null` on failure |
| `File.openCompressed(path, mode, algo)` | handle over a compressed stream, or `null` |
| `File.openEncryptedPass(path, mode, password)` | handle over an encrypted stream, or `null` |

`algo` accepts `"fastlz"`, `"deflate"`, `"zstd"`, `"gzip"`, or `"brotli"`.
Availability of each algorithm depends on the build; unsupported algorithms
cause the open call to fail (`null`).

> **Brotli is decompress-only.** The underlying engine ships a brotli
> decoder but no encoder, so `"brotli"` cannot be used with
> `File.openCompressed` for round-trip I/O. Opening for write (`"w"` /
> `"wr"`) succeeds, but the close step fails with
> `"Only brotli decompression is supported."` and produces an empty file.
> Opening for read (`"r"` / `"rw"`) only accepts streams in the engine's
> own framed compressed-file container, which the engine itself cannot
> produce — so there is currently no supported way to read a brotli
> stream through this API. Use one of the other algorithms for
> round-trip compression.

---

## Whole-file helpers

Convenience wrappers that open, transfer, and close in one call.

| Function | Returns | Notes |
| --- | --- | --- |
| `File.readBytes(path)` | buffer | Reads entire file into a new `Buffer`. Raises on failure. |
| `File.readText(path)` | string | Reads entire file as UTF-8 text. Raises on failure. |
| `File.writeBytes(path, buf)` | bool | Truncates / creates and writes the buffer. |
| `File.writeText(path, s)` | bool | Truncates / creates and writes the string. |
| `File.append(path, data)` | bool | Appends to an existing file (or creates one). `data` may be a string or a `Buffer`. |

---

## Metadata

| Function | Returns | Notes |
| --- | --- | --- |
| `File.exists(path)` | bool | `true` if a regular file exists at `path`. |
| `File.size(path)` | number | Size in bytes, or `0` if missing. |
| `File.modifiedTime(path)` | number | Unix timestamp (seconds) of last modification. |
| `File.accessTime(path)` | number | Unix timestamp (seconds) of last access. |
| `File.md5(path)` | string | MD5 of the file contents as lower-case hex. |
| `File.sha256(path)` | string | SHA-256 of the file contents as lower-case hex. |

---

## Filesystem operations

| Function | Returns | Notes |
| --- | --- | --- |
| `File.copy(src, dst)` | bool | Copies a file; `true` on success. |
| `File.remove(path)` | bool | Deletes a file; `true` on success. |
| `File.rename(src, dst)` | bool | Renames / moves a file; `true` on success. |

---

## Handle — state & positioning

| Method | Returns | Notes |
| --- | --- | --- |
| `f.isOpen()` | bool | `true` while the handle holds an open file. |
| `f.close()` | null | Flushes and closes. Safe to call more than once. |
| `f.path()` | string | Path as originally passed to the opener. |
| `f.pathAbsolute()` | string | Absolute path. |
| `f.length()` | number | Current file size in bytes. |
| `f.position()` | number | Current cursor offset. |
| `f.seek(pos)` | null | Moves the cursor to absolute byte offset `pos` (`>= 0`). |
| `f.seekEnd(off)` | null | Moves the cursor to `length() + off`. Use `0` for end-of-file. |
| `f.eof()` | bool | `true` after a read has passed the last byte. |
| `f.flush()` | null | Flushes pending writes to disk. |
| `f.resize(n)` | number | Truncates or extends the file to `n` bytes. Returns a status code (`0` on success). |
| `f.getError()` | number | Last error code observed on the handle (`0` = ok). |
| `f.setBigEndian(b)` | null | Sets the handle-wide endianness default for typed I/O. |
| `f.isBigEndian()` | bool | Current handle-wide endianness setting. |

---

## Handle — block I/O

| Method | Returns | Notes |
| --- | --- | --- |
| `f.readBytes(n)` | buffer | Reads up to `n` bytes from the cursor. The returned buffer may be shorter near EOF. |
| `f.writeBytes(buf)` | bool | Writes the entire contents of `buf` at the cursor. |

---

## Handle — text I/O

| Method | Returns | Notes |
| --- | --- | --- |
| `f.readText()` | string | Reads the remaining bytes as UTF-8. |
| `f.readLine()` | string | Reads one line (up to and excluding the newline). |
| `f.readCSVLine([delim])` | list of strings | Parses one CSV row. `delim` defaults to `","`. |
| `f.writeString(s)` | bool | Writes the raw UTF-8 bytes of `s` (no newline). |
| `f.writeLine(s)` | bool | Writes `s` followed by a newline. |
| `f.writeCSVLine(list[, delim])` | bool | Writes `list` as a CSV row. All elements must be strings. |

---

## Handle — integer decode/encode

Signed variants sign-extend; unsigned variants zero-extend. All multi-byte
methods accept an optional trailing endian override.

> **Precision gotcha.** Zym numbers are IEEE 754 doubles, so only integers in
> `[-2^53, 2^53]` (±9,007,199,254,740,992) are represented exactly. `U64` and
> `I64` values beyond that range silently round on decode and lose low-order
> bits on encode. If you need exact 64-bit values, split them into two 32-bit
> halves with `writeU32` / `readU32`.

| Width | Read (unsigned) | Read (signed) | Write (unsigned) | Write (signed) |
| --- | --- | --- | --- | --- |
| 1 byte  | `f.readU8([e])`  | `f.readI8([e])`  | `f.writeU8(v[, e])`  | `f.writeI8(v[, e])`  |
| 2 bytes | `f.readU16([e])` | `f.readI16([e])` | `f.writeU16(v[, e])` | `f.writeI16(v[, e])` |
| 4 bytes | `f.readU32([e])` | `f.readI32([e])` | `f.writeU32(v[, e])` | `f.writeI32(v[, e])` |
| 8 bytes | `f.readU64([e])` | `f.readI64([e])` | `f.writeU64(v[, e])` | `f.writeI64(v[, e])` |

`e` is the optional endian string `"le"` or `"be"`. When omitted, the
handle's current endian setting (see `setBigEndian`) is used. 1-byte methods
accept `e` for API symmetry but ignore it.

---

## Handle — float decode/encode

| Width | Read | Write |
| --- | --- | --- |
| 2 bytes (IEEE 754 half)   | `f.readHalf([e])`   | `f.writeHalf(v[, e])`   |
| 4 bytes (IEEE 754 single) | `f.readFloat([e])`  | `f.writeFloat(v[, e])`  |
| 8 bytes (IEEE 754 double) | `f.readDouble([e])` | `f.writeDouble(v[, e])` |

`writeHalf` / `writeFloat` narrow a Zym number (double) to the target
precision; round-tripping through them is lossy for most values.

---

## Endianness

Typed multi-byte reads/writes on a file handle consult, in order:

1. An explicit trailing `"le"` or `"be"` argument at the call site, if
   provided. The handle's setting is not modified.
2. Otherwise, the handle-wide setting from `setBigEndian(true|false)`
   (default: little-endian).

Any string other than `"le"` / `"be"`, or a non-string value in the endian
slot, raises a runtime error.

```zym
var f = File.open("out.bin", "w")
f.writeU32(0x11223344, "be")   // per-call override
f.setBigEndian(true)
f.writeU32(0x55667788)         // uses handle default (BE)
f.close()
```

---

## Example

```zym
// Whole-file helpers
File.writeText("hello.txt", "hello\n")
print("%s", File.readText("hello.txt"))
print("size=%n exists=%b", File.size("hello.txt"), File.exists("hello.txt"))
print("sha=%s", File.sha256("hello.txt"))

// Streaming read
var f = File.open("hello.txt", "r")
if (f == null) {
    print("open failed")
} else {
    while (!f.eof()) {
        var line = f.readLine()
        if (line != "") {
            print("> %s", line)
        }
    }
    f.close()
}

// Binary write + seek
var g = File.open("out.bin", "w")
g.writeU32(0xCAFEBABE, "be")
g.writeFloat(3.14)
g.close()

File.remove("hello.txt")
File.remove("out.bin")
```

---

## Notes & gotchas

- **Open can return `null`.** `open`, `openCompressed`, and
  `openEncryptedPass` do not raise on failure; they return `null`. Scripts
  should check for `null` before using the handle.
- **Closed-handle calls raise.** Calling any method other than `isOpen()`,
  `close()`, `path()`, `pathAbsolute()`, or `getError()` after `close()`
  raises a runtime error.
- **`readBytes(n)` may return fewer than `n` bytes** near end-of-file. Use
  `buf.size()` on the result rather than assuming the full count.
- **`resize(n)` on a file handle** truncates or extends the underlying file.
  Newly exposed bytes on growth are not zero-filled on all platforms; write
  explicit zeros if you need a known state.
- **`append(path, data)`** opens the file in read/write mode (falling back to
  write mode for new files) and seeks to end before writing; it closes the
  handle for you.
- **`modifiedTime` / `accessTime` / `size`** return `0` rather than raising
  when the path does not exist. Pair them with `exists` when you need to
  distinguish missing files from empty ones.
- **`md5` / `sha256`** stream the whole file; they can be slow on large
  inputs.
- **Compression / encryption availability** depends on the build.
  `openCompressed` with an unsupported algorithm simply returns `null`.
- **Handle-wide endianness is stateful.** Mixing `setBigEndian(true)` with
  per-call `"le"` overrides is supported, but the per-call override does not
  change the handle's setting. Be explicit when interleaving.
