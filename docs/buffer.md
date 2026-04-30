# `Buffer`

Mutable byte array. The global identifier `Buffer` is a constructor namespace;
calling one of its constructors returns a buffer instance whose methods are
invoked as `b.method(...)`.

---

## Conventions

- **Numbers.** Sizes, indices, byte values, offsets, and encoded integers are
  passed and returned as Zym numbers. Integer methods truncate toward zero;
  byte values are masked to 8 bits.
- **Assignment aliases.** Plain assignment (`b2 = b1`) makes `b2` refer to the
  *same* buffer as `b1`; subsequent mutations through either name are visible
  through the other. To get an independent copy use `b.duplicate()` or
  `Buffer.fromBytes(b)`. `slice` and `concat` always return independent
  buffers.
- **Indices.** Valid indices are in `0 .. size() - 1`. Out-of-range `get` /
  `set` / decode / encode calls raise a runtime error.
- **Byte range.** Values passed to `set`, `append`, `fill`, `insert`, `has`,
  `find`, etc. are coerced to a `uint8` (`0..255`).
- **Decode/encode offsets.** An offset `o` is valid when
  `0 <= o <= size() - width`, where `width` is `1/2/4/8` for the corresponding
  integer/float size.
- **Endianness.** Every multi-byte decode/encode takes an optional trailing
  string `"le"` (default) or `"be"`. See [Endianness](#endianness).
- **Errors.** Bad argument types or out-of-range indices produce a Zym runtime
  error of the form `Buffer.method(args) ...`.

---

## Construction

| Method | Returns |
| --- | --- |
| `Buffer.new(size)` | buffer of `size` zero bytes |
| `Buffer.fromBytes(buf)` | independent copy of another buffer |
| `Buffer.fromHex(s)` | buffer from an even-length hex string (`0-9a-fA-F`) |
| `Buffer.fromString(s)` | buffer containing the raw bytes of a Zym string |
| `Buffer.fromList(list)` | buffer from a list of numbers (each coerced to a byte) |

`fromHex` rejects odd-length input and any non-hex character.
`fromList` rejects elements that are not numbers.

---

## Size & state

| Method | Returns | Notes |
| --- | --- | --- |
| `b.size()` | number | Length in bytes. |
| `b.isEmpty()` | bool | `true` when `size() == 0`. |
| `b.clear()` | null | Drops all bytes. |
| `b.resize(n)` | number | Grows/truncates to `n` bytes. New bytes on growth are **not** zero-initialized; their contents are unspecified. Call `fill(0)` (or `set` each new byte) if you need a clean region. Returns a status code (`0` on success). |
| `b.fill(v)` | null | Sets every byte to `v & 0xFF`. |
| `b.duplicate()` | buffer | Independent copy. |

---

## Element access

| Method | Returns | Notes |
| --- | --- | --- |
| `b.get(i)` | number | Byte at index `i`. |
| `b.set(i, v)` | null | Writes `v & 0xFF` at index `i`. |
| `b.append(v)` | bool | Appends a byte; `true` on success. |
| `b.pushBack(v)` | bool | Alias for `append`. |
| `b.insert(i, v)` | number | Inserts `v` before index `i`; returns a status code. |
| `b.removeAt(i)` | null | Removes the byte at `i`. |
| `b.erase(v)` | bool | Removes the first byte equal to `v`; `true` if found. |

---

## Ordering & search

| Method | Returns | Notes |
| --- | --- | --- |
| `b.reverse()` | null | Reverses in place. |
| `b.sort()` | null | Ascending in-place sort. |
| `b.has(v)` | bool | `true` if any byte equals `v`. |
| `b.find(v, from)` | number | Index of the first `v` at/after `from`, or `-1`. |
| `b.rfind(v, from)` | number | Index of the last `v` at/before `from` (use `-1` for end), or `-1`. |
| `b.count(v)` | number | Count of bytes equal to `v`. |
| `b.bsearch(v, before)` | number | Binary-search insertion index; assumes sorted input. `before=true` returns the lower bound, `false` the upper bound. |

---

## Slicing & composition

| Method | Returns | Notes |
| --- | --- | --- |
| `b.slice(begin, end)` | buffer | Copy of the range `[begin, end)`; negative values count from the end. |
| `b.equals(other)` | bool | Byte-wise equality. |
| `b.concat(other)` | buffer | New buffer containing `b` followed by `other`. |

---

## Text conversion

| Method | Returns | Notes |
| --- | --- | --- |
| `b.hex()` | string | Lower-case hex encoding. |
| `b.toUtf8()` | string | Decodes the bytes as UTF-8. Invalid sequences are replaced with `�`. |
| `b.toAscii()` | string | Decodes the bytes as ASCII (bytes ≥ 0x80 are treated as Latin-1). |

---

## Integer decode/encode

All integer decoders and encoders use little-endian layout. Decoders return
numbers; encoders return `null`. Signed variants sign-extend; unsigned
variants zero-extend.

> **Precision gotcha.** Zym numbers are IEEE 754 doubles, so only integers in
> `[-2^53, 2^53]` (±9,007,199,254,740,992) are represented exactly. `U64` and
> `I64` values beyond that range silently round on decode and lose low-order
> bits on encode. If you need exact 64-bit values, split them into two 32-bit
> halves with `encodeU32` / `decodeU32`.

| Width | Decode (unsigned) | Decode (signed) | Encode (unsigned) | Encode (signed) |
| --- | --- | --- | --- | --- |
| 1 byte  | `b.decodeU8(offset)`       | `b.decodeI8(offset)`       | `b.encodeU8(offset, v)`       | `b.encodeI8(offset, v)`       |
| 2 bytes | `b.decodeU16(offset[, e])` | `b.decodeI16(offset[, e])` | `b.encodeU16(offset, v[, e])` | `b.encodeI16(offset, v[, e])` |
| 4 bytes | `b.decodeU32(offset[, e])` | `b.decodeI32(offset[, e])` | `b.encodeU32(offset, v[, e])` | `b.encodeI32(offset, v[, e])` |
| 8 bytes | `b.decodeU64(offset[, e])` | `b.decodeI64(offset[, e])` | `b.encodeU64(offset, v[, e])` | `b.encodeI64(offset, v[, e])` |

`e` is the optional endian string `"le"` (default) or `"be"`. 1-byte methods
accept `e` for API symmetry but ignore it.

---

## Float decode/encode

| Width | Decode | Encode |
| --- | --- | --- |
| 2 bytes (IEEE 754 half)   | `b.decodeHalf(offset[, e])`   | `b.encodeHalf(offset, v[, e])`   |
| 4 bytes (IEEE 754 single) | `b.decodeFloat(offset[, e])`  | `b.encodeFloat(offset, v[, e])`  |
| 8 bytes (IEEE 754 double) | `b.decodeDouble(offset[, e])` | `b.encodeDouble(offset, v[, e])` |

`e` is the optional endian string `"le"` (default) or `"be"`. `encodeHalf` /
`encodeFloat` narrow a Zym number (double) to the target precision.

---

## Compression

Buffers can be compressed and decompressed in-process. Output is always a new,
independent buffer; the source is not modified.

| Method | Returns | Notes |
| --- | --- | --- |
| `b.compress(algo)` | buffer or `null` | Compresses the buffer using `algo` at the default level. Returns `null` on failure. |
| `b.compress(algo, level)` | buffer or `null` | Same, with an explicit compression level (see table below). |
| `b.decompress(algo, maxOutputSize)` | buffer or `null` | Decompresses the buffer using `algo`, capping the output to `maxOutputSize` bytes. Returns `null` if the data is malformed, the wrong algorithm, or exceeds the cap. |
| `b.decompressDynamic(algo)` | buffer or `null` | Decompresses without a known output size — the destination buffer grows automatically as data is produced. Slower than `decompress` (it may resize the output multiple times) but useful when the decompressed length is unknown. Only `"gzip"`, `"deflate"`, and `"brotli"` are supported; passing any other `algo` raises a runtime error. |
| `b.decompressDynamic(algo, maxOutputSize)` | buffer or `null` | Same, with a hard cap on the output size. Returns `null` if the cap is exceeded mid-stream. `maxOutputSize` must be `>= 0` (passing `0` yields an empty buffer). |

Algorithms (`algo` is matched case-insensitively):

| `algo` | Compress | Decompress | Level range | Default level | Notes |
| --- | --- | --- | --- | --- | --- |
| `"fastlz"`  | yes | yes | *(no level)*    | n/a   | LZ77-family, very fast, modest ratio. Limited to 2 GiB input. |
| `"deflate"` | yes | yes | `1..9` (zlib)   | `6`   | Raw DEFLATE stream (no header). |
| `"gzip"`    | yes | yes | `1..9` (zlib)   | `6`   | DEFLATE wrapped in a gzip header (`.gz`-compatible). |
| `"zstd"`    | yes | yes | `1..22`         | `3`   | Modern algorithm; level has the largest visible effect on ratio vs. speed. Levels above ~19 are "ultra" (much slower for small extra gain). |
| `"brotli"`  | **no** | yes | *(no level)*    | n/a   | Decompress-only — brotli writes are not supported. `b.compress("brotli")` raises a runtime error. |

Notes:

- **Level out of range** raises a runtime error of the form
  `Buffer.compress(algo, level?): level N out of range for "<algo>" (lo..hi)`.
- **Passing a level to `"fastlz"` or `"brotli"`** raises a runtime error
  (`"<algo>" does not accept a level`).
- **`maxOutputSize` is a hard cap** — if the decompressed data would exceed it,
  `decompress` returns `null` rather than truncating, *with one exception*:
  `"fastlz"` carries no framed size, so a too-small `maxOutputSize` silently
  truncates the output instead of erroring. Always size `maxOutputSize` to the
  known/expected uncompressed length when using `"fastlz"`. The other four
  algorithms detect and reject under-sized buffers.
- **Levels are applied per call.** Other compression paths (e.g.
  `File.openCompressed`) are unaffected; defaults are restored before
  `compress` returns.
- **`decompressDynamic` vs `decompress`.** Use `decompress` when you know (or
  have a tight upper bound on) the decompressed size — it allocates the output
  once and is the faster path. Use `decompressDynamic` when the decompressed
  size is unknown (e.g. arbitrary HTTP response bodies); it grows the output
  as needed at the cost of repeated reallocations and copies. Only the
  algorithms whose decoder is genuinely streaming are accepted
  (`"gzip"`, `"deflate"`, `"brotli"`); `"fastlz"` and `"zstd"` raise a
  runtime error and must go through `decompress`.
- **Empty input.** `compress` on an empty buffer produces a valid empty/header
  stream; `decompress` of empty input returns an empty buffer for
  gzip/deflate/brotli (zero bytes cannot be inflated), and `null` for
  fastlz/zstd (empty input is treated as malformed).
- **Cross-tool compatibility.** `gzip` output is readable by the standard
  `gzip` / `gunzip` tool; `zstd` output is readable by the `zstd` CLI;
  `deflate` output is *raw* DEFLATE without a header and is **not** the same
  as a `.gz` file; `fastlz` has no widely-used external tool.
  `brotli` decompression accepts standard `.br` streams from `curl --compressed`,
  the `brotli` CLI, etc.

```zym
// Build a compressible payload by repeating a short phrase.
var phrase = "the quick brown fox jumps over the lazy dog. "
var text = ""
for (var i = 0; i < 64; i = i + 1) { text = text + phrase }

var orig     = Buffer.fromString(text)
var packed   = orig.compress("zstd", 19)
var unpacked = packed.decompress("zstd", orig.size())
print("orig=%n  packed=%n  match=%v", orig.size(), packed.size(), orig.equals(unpacked))
```

---

## Endianness

Every multi-byte `decode*` / `encode*` method accepts an optional trailing
endian string:

- `"le"` — little-endian (default when the argument is omitted).
- `"be"` — big-endian.

Any other string, or a non-string value, raises a runtime error. 1-byte
methods (`decodeU8` / `decodeI8` / `encodeU8` / `encodeI8`) accept the
argument for API symmetry but ignore it.

```zym
var b = Buffer.new(4)
b.encodeU32(0, 0x11223344, "be")       // bytes: 11 22 33 44
print("%n", b.decodeU32(0, "be"))      // 287454020 (0x11223344)
print("%n", b.decodeU32(0))            // 1144201745 (0x44332211) - read as LE
```

Choose the endianness at every call site; there is no buffer-wide mode.

---

## Example

```zym
var b = Buffer.new(8)
b.encodeU32(0, 0xCAFEBABE)
b.encodeI32(4, 42)

print("size: %n", b.size())
print("hex:  %s", b.hex())

print("magic: %n", b.decodeU32(0))
print("value: %n", b.decodeI32(4))

var c = Buffer.fromString("hi")
print("utf8: %s", c.toUtf8())
print("hex:  %s", c.hex())

var d = b.concat(c)
print("combined size: %n", d.size())
```

---

## Notes & gotchas

- **Assignment aliases, it does not copy.** `b2 = b1` makes both names refer
  to the same buffer; a `set` / `append` / `resize` through either is visible
  through the other. Use `b.duplicate()` or `Buffer.fromBytes(b)` for an
  independent copy. `slice`, `concat`, and the `Buffer.from*` constructors
  already return independent buffers.
- **`resize(n)` does not zero new bytes on growth.** The contents of the new
  tail are unspecified. Follow `resize` with `fill(0)` (or explicit `set`s) if
  you need a known state.
- **U64 / I64 precision.** Zym numbers are doubles. Values outside
  `[-2^53, 2^53]` lose precision in both directions. Use paired `U32`
  reads/writes when exactness matters.
- **`encodeHalf` / `encodeFloat` narrow** a Zym number (double) to the target
  precision; round-tripping through them is lossy for most values.
- **`find` / `rfind` `from` argument.** `find` defaults to `0`; `rfind`
  expects `-1` (or `size() - 1`) to search from the end. Passing `0` to
  `rfind` only searches the first byte.
- **`bsearch` requires sorted input.** Behavior on unsorted buffers is
  unspecified.
- **Hex / UTF-8 / ASCII decoders are strict / lossy as documented.**
  `fromHex` rejects odd-length or non-hex input; `toUtf8` replaces invalid
  sequences with `\uFFFD`; `toAscii` treats bytes ≥ 0x80 as Latin-1 rather
  than erroring.
- **Typed views** (packed `i32` / `f32` arrays sharing storage) are not
  exposed yet.
