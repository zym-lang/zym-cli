# Pack

`Pack` assembles ZPK bundles — the on-disk container the CLI uses to
ship runnable scripts (and their assets) as a single file. It is also
how scripts read entries back out of a bundle, whether that bundle is
the one the running process was launched from or an arbitrary `.zpk`
file on disk. The wire format is documented in
[`docs/formats/zpk.md`](../formats/zpk.md).

## Capability / grant model

`Pack` is a **grantable** CLI native, on the same footing as `File`,
`Process`, `AES`, etc.:

- The **root VM** has `Pack` because it receives the full catalog at
  boot.
- A **child VM** created via `Zym.newVM(...)` has `Pack` *only* when
  its parent script explicitly grants it (e.g.
  `registerCliNative("Pack")`). A child that wasn't granted `Pack`
  has no `Pack` global at all and cannot assemble bundles.

This matches the policy used by every other grantable native and by
`Zym` itself; bundling capability is never auto-installed.

## Surface

```
// --- assembling bundles ---
Pack.build(spec) -> bool

// --- diagnostics toggle ---
Pack.setVerboseOutput(verbose) -> bool

// --- arbitrary bundles ---
var bundle = Pack.openFile(path)      // open from a filesystem path
var bundle = Pack.openBuffer(buffer)  // open from an in-memory Buffer
bundle.list()              -> [entryInfo, ...] | null
bundle.entryName()         -> string | null
bundle.has(name)           -> bool
bundle.open(arg)           -> Buffer | null    // arg: name string or numeric index
bundle.info(arg)           -> entryInfo | null // arg: name string or numeric index
bundle.formatVersion()     -> number | null    // null after close()
bundle.close()             -> bool

// --- editing an existing bundle ---
var edit = Pack.editFile(path)        // open from disk; commit = atomic rename
var edit = Pack.editBuffer(buffer)    // open from Buffer; commit mutates in place
edit.list()                -> [entryInfo, ...]    // staged view
edit.info(arg)             -> entryInfo | null    // arg: name string or index
edit.entryIndex()          -> number | null
edit.add(spec)             -> bool
edit.remove(arg)           -> bool
edit.replace(arg, spec)    -> bool                // identity-preserving
edit.rename(arg, newName)  -> bool
edit.move(srcArg, dstIdx)  -> bool
edit.setEntryIndex(arg)    -> bool
edit.setFlags(arg, flags)  -> bool                // flags: number | map
edit.setCustom(arg, u32)   -> bool
edit.setStub(arg)          -> bool                // arg: path | Buffer | null
edit.commit()              -> bool                // single atomic rewrite
edit.close()               -> bool

// --- inspecting and splicing native binaries ---
Pack.inspectBin(path)                            -> { ... } | null
Pack.splice(stubPath, zpkPath, outputPath)       -> bool
```

`Pack.build` describes the whole bundle in one call; the writer it
sits on (`zpk_write_bundle`) is itself batch-shaped, so layering a
streaming builder on top would only re-buffer the same data on the
script side.

The read API is exposed through bundle handles:

- **`Pack.openFile(path)`** opens an arbitrary `.zpk` (or stub-wrapped
  binary) and returns a bundle handle. The handle caches the parsed
  reader for its lifetime and frees it on `bundle.close()`. After
  close, every method on the handle returns `null` / `false`. A GC
  finalizer also closes the reader as a safety net if the script
  forgets to call `close()`.
- **`Pack.openBuffer(buffer)`** opens a `.zpk` whose bytes already
  live in script memory (e.g. fetched over the network, decrypted
  in-process, generated on the fly). The reader takes its own copy of
  the bytes, so the source `Buffer` is independent and may be reused
  or discarded immediately. The returned handle behaves identically
  to one returned by `openFile` — same methods, same caching, same
  `close()` lifecycle, same GC-finalizer safety net.

### Format version

`bundle.formatVersion()` reports the on-disk `format_version` of the
bundle as a number, or `null` when the handle has been closed. Useful
for tooling such as `zym pack info` that wants to print the format
level a bundle was written against.

### Verbose diagnostics toggle

`Pack.setVerboseOutput(verbose) -> bool` controls whether `Pack`'s
**speculative** reader probes (the ones used internally by
`Pack.build`, `Pack.splice`, and `Pack.inspectBin` to sniff whether
an input file already carries a `.zpk` payload) print diagnostics on
stderr.

- Default is **quiet** (`false`). The common case for these probes is
  a fresh native stub with no payload yet, where the reader would
  otherwise print messages like `zpk: no .zpk payload found (footer
  magic missing).` — pure noise, since "no payload" is the expected
  signal these calls are looking for.
- Pass `true` to opt in to chatty output, e.g. when debugging a
  pack/splice that isn't producing what you expect.
- The setting is module-global and persists until changed again.
- The returned bool is the new value (i.e. the argument, echoed back).
- Type/shape mistakes (non-bool argument) raise a runtime error.

This switch only governs the **probe** paths. User-facing opens
(`Pack.openFile`, `Pack.openBuffer`) keep their normal diagnostics
regardless of this flag, because the script explicitly asked to open
that bundle and expects feedback when it isn't a valid `.zpk`.

```
Pack.setVerboseOutput(true);
Pack.build({
    output: "dist/app",
    stub:   "vendor/zym-runtime",
    entries: [ { name: "main.zbc", kind: "entry_bytecode", data: bc } ]
});
Pack.setVerboseOutput(false);
```

### `spec` map

| Key          | Type     | Required | Default | Notes                                                                                              |
| ---          | ---      | ---      | ---     | ---                                                                                                |
| `output`     | string   | yes      | —       | Destination path. When it ends in `.zpk` a **headless** bundle is produced and `stub` is ignored.  |
| `entries`    | list     | yes      | —       | Non-empty list of entry maps (see below).                                                          |
| `entryIndex` | number   | no       | `0`     | Index into `entries` of the program entry point. Must reference an entry whose `kind` is `entry_bytecode` or `entry_source`. A bundle may contain at most **one** entry-kind entry. |
| `stub`       | string   | no       | none    | Path to a CLI runtime binary to prepend as the executable stub. Ignored when `output` ends in `.zpk`. **If the stub file already carries a ZPK payload, only its native portion is taken — the existing payload is dropped and replaced with the new one.** This means a stub-wrapped binary can be re-packed in place without ever stacking multiple ZPK regions; `Pack` enforces "exactly one ZPK per executable" by construction. No `mode` flag is needed: append-vs-swap is decided by what the stub file actually contains. |
| `compression`| bool     | no       | `false` | Bundle-wide compression default (zstd). When `true`, every entry is compressed unless it sets `compression: false`. When `false` (or omitted) entries default to uncompressed and opt in with `compression: true`. |
| `level`      | number   | no       | `3`     | Default zstd level (`1..22`). Per-entry `level` overrides this. Ignored on entries that resolve to uncompressed. `3` matches zstd's own default; `19+` is the "release-build" sweet spot. |
| `setExecutable` | bool  | no       | `false` | When `true`, mark the output file as executable after writing. **POSIX (Linux/macOS):** adds execute bits mirrored from the read bits, masked by the process umask (matches `chmod +x` honoring umask). **Windows:** silent no-op — executability there is decided by file extension (`.exe`, `.bat`, …) and the PE header, neither of which `Pack` touches. A `chmod` failure on POSIX warns to stderr but does **not** fail the build (the bundle bytes were written successfully; the user can retry the chmod themselves). |

### Entry map

Each element of `entries` is a map:

| Key      | Type     | Required | Default | Notes                                                                                |
| ---      | ---      | ---      | ---     | ---                                                                                  |
| `name`   | string   | no       | unnamed | Logical name stored in the bundle's string table (e.g. `"main.zbc"`).                |
| `kind`   | string   | yes      | —       | One of the kind strings below.                                                       |
| `flags`  | number   | no       | `0`     | Per-entry flag bits (forwarded to the on-disk `flags` field). **16-bit unsigned**; valid range `0..65535` (`0x0000..0xFFFF`). Scripts that don't want to do bit math can just pass a plain number in that range; values outside it are truncated to `uint16_t`. |
| `custom` | number   | no       | `0`     | Free per-kind 32-bit field, forwarded verbatim. **32-bit unsigned**; valid range `0..4294967295` (`0x00000000..0xFFFFFFFF`). Treat it as either 32 bitflag/tag slots or a plain numeric tag — `Pack` doesn't interpret it. Values outside the range are truncated to `uint32_t`. |
| `data`   | Buffer   | one of   | —       | In-memory bytes. Use this when the data already lives in script memory.              |
| `path`   | string   | one of   | —       | Absolute or relative file path. The writer streams this file from disk; the bytes never round-trip through a script-side `Buffer`. |
| `compression` | bool | no       | (inherits) | Per-entry override of the bundle-wide `compression`. Always wins over the bundle default. |
| `level`  | number   | no       | (inherits) | Per-entry override of the bundle-wide `level` (`1..22`). Ignored when the entry resolves to uncompressed. |

Every entry must set **exactly one** of `data` or `path`. Setting both,
or neither, raises a runtime error.

### Compression

`Pack` supports **zstd** as the only compression codec. The on-disk
format records compression per entry, so each entry can be compressed
or stored verbatim independently — there is no whole-bundle codec.

Resolution rule (bundle default + per-entry override):

- `spec.compression` omitted or `false` → entries default to
  **uncompressed**; an entry sets `compression: true` to opt in.
- `spec.compression: true` → entries default to **compressed**; an
  entry sets `compression: false` to opt out.
- Per-entry `compression` always wins over the bundle default.
- `level` follows the same shape: bundle-level default (3 if omitted),
  overridden by per-entry `level`. Range is `1..22`, matching
  `Buffer.compress("zstd", level)`.

**Auto-fallback to uncompressed.** If an entry resolved to compressed
but the zstd output isn't strictly smaller than the raw input, the
writer stores the raw bytes instead and records `compression: none`
on disk. Already-compressed assets (PNG, opus, etc.) therefore don't
get a worse-than-passthrough re-encode just because the bundle's
default is `true`.

**Reads are transparent.** `open(arg)` always hands back the
**decompressed** payload as a `Buffer`. Scripts wanting to know what
the on-disk codec actually was can check `info(arg).compression`
(`"zstd"` / `"none"`).

### `kind` vocabulary

The accepted entry kind strings are:

| String              | Meaning                                              |
| ---                 | ---                                                  |
| `"entry_source"`    | The program entry point's raw source (`.zym`). The runtime loader compiles it on boot, then runs. Only one of `entry_source` / `entry_bytecode` is permitted per bundle. |
| `"entry_bytecode"`  | The program entry point's compiled bytecode (`.zbc`). The runtime loader deserializes and runs it directly. |
| `"source_map"`      | A source map for the entry's bytecode (or any other consumer). The pairing is by name; the runtime loader does not consume source maps itself. |
| `"file"`            | A named, path-addressable resource consumed as a coherent unit — analogous to a file in a filesystem. Use this for configuration, templates, scripts loaded by name, text or binary content that a script looks up via a path-shaped name. Names should be normalized paths (forward slashes, no leading `/`, no `..`). |
| `"blob"`            | Opaque, id-addressed bytes whose meaning is determined by the producer/consumer pair. Use this for binary handoff between cooperating scripts, attached signatures, additional `.zbc` modules loaded by a script into an in-process VM, ML weights — anything that's "just bytes by id." Sub-kind discrimination (encoding, MIME, format tag) goes in the per-entry `custom` u32 (32 bitflag/tag slots) or `flags` u16, which the writer forwards verbatim. |

Strings are used (rather than numeric constants) so scripts don't have
to know the on-disk byte values.

## Return value

`Pack.build(spec)` returns:

- `true` on success — the file at `spec.output` exists and is a valid
  ZPK bundle.
- `false` on any I/O / validation failure (could not open the stub,
  could not stream a `path` entry, short write, etc.). The writer's
  human-facing diagnostics are emitted on `stderr`; the bool is the
  programmatic signal.

## Errors raised vs. returned

- **Type / shape mistakes** raise a runtime error of the form
  `Pack.build(spec) ...`. Examples:
  - `Pack.build(42)`
  - `spec.output` missing or not a string
  - `spec.entries` not a list, or empty
  - an entry's `kind` not a recognized string
  - an entry that sets both `data` and `path`, or neither
  - an entry whose `data` is not a `Buffer`
  - `entryIndex` out of range or referencing an entry whose `kind` is
    not `entry_source` or `entry_bytecode`
  - more than one entry has an entry-kind (`entry_source` /
    `entry_bytecode`) in the same bundle
- **Recoverable failures** (file not openable, short read, short
  write, out-of-memory while assembling) return `false`.

### Source vs. bytecode entries

A bundle's program entry can be either compiled bytecode
(`entry_bytecode`) or raw `.zym` source (`entry_source`). Pick one:

- **`entry_bytecode`** — the runtime loader deserializes and runs the
  chunk directly. Use this for shipping production builds. Modules
  are resolved at compile time, so a fully-compiled `entry_bytecode`
  chunk already contains everything its entry script imported — no
  runtime resolution against the bundle is performed (or needed).
- **`entry_source`** — the runtime loader compiles the source on every
  boot, then runs it. Useful for small single-file tools, tweak-and-run
  debugging workflows, and "patch the script, re-launch" iteration.

**Module resolution policy.** Module imports are a **compile-time**
concept; ZPK never resolves modules from inside the bundle at runtime,
regardless of entry kind. When the entry is `entry_source`, the
loader compiles on boot and module imports are resolved **from disk
only** (relative to the running process's working directory). When
the entry is `entry_bytecode`, the chunk was compiled ahead of time
with all of its imports already inlined, so no runtime resolution
happens at all. If you need a self-contained, no-disk-required
bundle, compile to `entry_bytecode`. Additional `.zbc` blobs you
want to read by name from the bundle should be stored as `blob`
entries (read explicitly via `open(arg)`); they are **not**
consulted by any import statement.

A bundle may contain **at most one** entry-kind entry; mixing
`entry_bytecode` and `entry_source` in the same bundle is rejected at
`Pack.build` time.

Syntax errors in an `entry_source` entry surface at **boot time**
(when the loader compiles), not at pack time. That's the intended
debug-iteration behavior, but is worth knowing if you're shipping
source-entry bundles to other users.

## Examples

### Headless `.zpk` with an entry and a named blob

```
var bytecodeMain = File.readAllBytes("build/main.zbc");
var bytecodeUtil = File.readAllBytes("build/util.zbc");

var ok = Pack.build({
    output: "dist/app.zpk",
    entries: [
        { name: "main.zbc", kind: "entry_bytecode", data: bytecodeMain },
        { name: "util.zbc", kind: "blob",           data: bytecodeUtil }
    ]
});
if (!ok) {
    print("packing failed");
}
```

### Stub-wrapped executable, files streamed from disk

The file contents never enter script memory; the writer reads them directly.

```
var bytecode = File.readAllBytes("build/main.zbc");

var ok = Pack.build({
    output: "dist/app",                    // not .zpk → wrapped exe
    stub:   "vendor/zym-runtime",          // CLI runtime stub
    entryIndex: 0,
    entries: [
        { name: "main.zbc", kind: "entry_bytecode", data: bytecode },
        { name: "assets/level1.bin",  kind: "file", path: "assets/level1.bin" },
        { name: "assets/credits.txt", kind: "file", path: "assets/credits.txt" }
    ]
});
```

### Stub-wrapped executable from raw source

The loader compiles `app.zym` on every boot. Module imports in
`app.zym`, if any, are resolved from disk relative to the running
process — **not** from inside the bundle.

```
var ok = Pack.build({
    output: "dist/app",                    // not .zpk → wrapped exe
    stub:   "vendor/zym-runtime",
    entries: [
        { name: "app.zym", kind: "entry_source", path: "src/app.zym" }
    ]
});
```

## Inspecting and splicing native binaries

`Pack` exposes two file-level operations for working with executables
that already carry — or are about to carry — a ZPK payload. They are
cross-platform: ELFs, PE/COFF, Mach-O binaries, and raw blobs are all
treated identically because the operation only looks at the trailing
ZPK footer.

### `Pack.inspectBin(path)`

Read-only geometry probe. Opens the file, validates the trailing
footer, and returns the boundary between the native portion and the
ZPK payload. Cheap; does not iterate entry payloads.

```
{
    fileSize:      <number>,   // total size of the file in bytes
    stubSize:      <number>,   // bytes 0..stubSize are the native stub
    payloadSize:   <number>,   // bytes stubSize..fileSize are the ZPK payload
    formatVersion: <number>,   // ZPK format version recorded in the footer
    entryCount:    <number>,
    entryIndex:    <number>,
    isHeadless:    <bool>,     // stubSize == 0 (a plain `.zpk`)
    hasStub:       <bool>      // !isHeadless
}
```

Returns **`null`** when the file does not contain a valid trailing
ZPK payload (no magic, bad CRC, truncated footer, file unreadable),
so scripts can branch cheaply on "is this binary already packed?":

```
var info = Pack.inspectBin("dist/app");
if (info == null) {
    print("no payload yet — fresh build needed");
} else {
    print("stub: " + str(info.stubSize) + "  payload: " + str(info.payloadSize));
}
```

### `Pack.splice(stubPath, zpkPath, outputPath) -> bool`

Combine an already-built standalone `.zpk` with a native stub binary
and write the result to `outputPath`. The file-level peer of
`Pack.build`: it doesn't decompose the source `.zpk` back through the
writer, so a pre-built bundle can be shipped on top of any stub
without round-tripping the entries through script memory.

- If the stub at `stubPath` already carries a payload, only its
  native portion is taken — the previous payload is dropped,
  preserving the "exactly one ZPK per executable" invariant. Like
  `Pack.build`'s `stub` option, this means `Pack.splice` transparently
  **replaces** an existing ZPK on the stub rather than appending a
  second one; no `mode` flag is needed because append-vs-replace is
  decided by what the stub file actually contains.
- If the source `.zpk` argument is itself a stub-wrapped binary, only
  its payload is taken and grafted onto the new stub.
- On POSIX (Linux/macOS), the output file inherits the **permission
  bits of the source stub** (so splicing an executable stub yields an
  executable result, and splicing a non-executable file yields a
  non-executable result). No `setExecutable` field is needed on
  `Pack.splice`: the mode is mirrored automatically because the
  result *is* whatever the input stub already was. On Windows this
  is a silent no-op (executability is decided by extension / PE
  header). A `chmod` failure warns to stderr but does not fail the
  splice.
- After concatenation, the appended payload's absolute offsets
  (footer's `manifest_offset` / `strtab_offset`, every entry's
  `data_offset`) are rewritten to account for the new stub prefix
  and the manifest + footer CRCs are recomputed. Per-entry
  `dataCrc32` values are preserved unchanged because they hash entry
  bytes (not their position in the file). The output is therefore a
  fully valid bundle, indistinguishable from one written by
  `Pack.build` directly.

Returns `true` on success; `false` (with a stderr line) on I/O
failure or when `zpkPath` is not a valid bundle. Type/shape mistakes
(non-string args) raise a runtime error.

```
// Build a portable .zpk once, ship it on top of platform-specific stubs.
Pack.build({
    output: "dist/app.zpk",
    entries: [...]
});
Pack.splice("vendor/zym-runtime-linux-x86_64", "dist/app.zpk", "dist/app");
Pack.splice("vendor/zym-runtime-windows-x86_64.exe", "dist/app.zpk", "dist/app.exe");
```

## Reading bundles

### `entryInfo` map

`bundle.list` / `bundle.info` (on a handle returned by `Pack.openFile`
or `Pack.openBuffer`) return per-entry maps with **every** field of
the underlying on-disk entry. Fields unused in v1 (compression byte,
reserved slots) are still surfaced verbatim so scripts can introspect
bundles authored by future writers without an API churn.

| Key                | Type     | Notes                                                                                  |
| ---                | ---      | ---                                                                                    |
| `index`            | number   | Position in the manifest (0-based).                                                    |
| `name`             | string   | Logical name; empty string when the entry was unnamed.                                 |
| `kind`             | string   | One of the [kind strings](#kind-vocabulary), or `"reserved:0xNN"` / `"user:0xNN"` for bytes outside the documented set. |
| `kindByte`         | number   | Raw kind byte (0–255).                                                                 |
| `compression`      | string   | `"none"` or `"zstd"`. (`"unknown"` is reported for any other on-disk byte read from a forward-compatible bundle.)        |
| `compressionByte`  | number   | Raw compression byte.                                                                  |
| `flags`            | number   | Raw 16-bit flag bits.                                                                  |
| `required`         | bool     | Convenience: the "required" flag bit is set.                                           |
| `lazy`             | bool     | Convenience: the "lazy" flag bit is set.                                               |
| `nameOffset`       | number   | Offset into the bundle's string table.                                                 |
| `nameLength`       | number   | Bytes in the name; `0` for unnamed entries.                                            |
| `reserved`         | number   | The manifest entry's reserved 32-bit field (must be `0` in v1; surfaced for future use). |
| `dataOffset`       | number   | Absolute offset of the entry's bytes in the file.                                      |
| `dataSize`         | number   | On-disk size (post-compression). Equal to `uncompressedSize` in v1.                    |
| `uncompressedSize` | number   | Logical (decompressed) size.                                                           |
| `size`             | number   | Alias of `uncompressedSize` for convenience.                                           |
| `dataCrc32`        | number   | CRC-32 of the on-disk bytes.                                                           |
| `custom`           | number   | Free per-kind 32-bit field, surfaced verbatim.                                         |
| `isEntry`          | bool     | `true` when this entry is the program entry point.                                     |

### `open(arg)` / `info(arg)` — name or numeric index

`bundle.open` and `bundle.info` both accept either a **string** entry
name or a **numeric** manifest index:

- `open("main.zbc")` / `info("main.zbc")` — looks up the first entry
  whose name matches. Returns `null` if no entry has that name.
- `open(0)` / `info(0)` — looks up the entry at that 0-based manifest
  position. Returns `null` if the index is out of range.

Bundles may legally contain multiple entries that share the same
name (each manifest slot is independent). When that happens the
string form resolves to the first match only — use the numeric index
to address any subsequent entry. `bundle.list()` returns entries in
manifest order, so a typical pattern is to walk `list()` to find
duplicates and then call `open(index)` / `info(index)` on the
specific entries you care about. The single-arg `verify(arg)` on the
handle follows the same string/number dispatch.

### Handle lifecycle

`Pack.openFile(path)` (and `Pack.openBuffer(buffer)`) returns a bundle
handle on success and `null` if the input is not a valid bundle. Each
handle owns its own reader, cached until `bundle.close()` is called.
After close, every method on the handle returns `null` / `false`.
Forgetting to call `close()` is not a leak — a GC finalizer closes
the reader when the handle is collected — but the explicit `close()`
is the recommended pattern because it bounds memory use the moment
the script is done.

### Reading an arbitrary bundle

```
var b = Pack.openFile("dist/app.zpk");
if (b == null) {
    print("not a valid bundle");
} else {
    print("entry: " + b.entryName());
    var info = b.info("main.zbc");
    if (info != null) {
        // info.dataSize, info.dataCrc32, info.kind, ...
    }
    var first = b.open(0);         // Buffer of the first entry's bytes
    b.close();                     // free cache
}
```

## Verifying CRCs

Every bundle stores three independent CRC-32s — one over the footer,
one over the manifest table (entries plus the string table), and one
per entry over its on-disk bytes. The footer CRC is enforced when a
bundle is opened: a bundle with a bad footer CRC is rejected, so
`Pack.openFile` (and `Pack.openBuffer`) returns `null`. The manifest
CRC and per-entry data CRCs are not enforced at open time — they're
surfaced through `verify()` on the bundle handle so scripts can decide
what to do on mismatch.

A `Pack.openFile` / `Pack.openBuffer` handle exposes a two-arity
`verify`:

### `bundle.verify() -> map | null`

Runs all three CRC checks and returns a structured report. Returns
`null` when the handle has been closed.

```
{
    ok:       <bool>,                    // true iff every CRC matches
    footer:   { ok, expected, computed },
    manifest: { ok, expected, computed },
    entries:  [
        { index, name, ok, expected, computed, readable },
        ...                              // one per manifest entry
    ]
}
```

- `ok` (top-level) is the AND of `footer.ok`, `manifest.ok`, and every
  `entries[i].ok`.
- `expected` is the value stored in the bundle; `computed` is the
  value computed locally. Both are surfaced as numbers so scripts
  can log / compare them on mismatch.
- `readable` (per entry) is `false` only when the entry's
  `dataOffset` / `dataSize` falls outside the file (a corrupt
  manifest); in that case `computed` is reported as `0` and `ok` is
  `false`.

### `bundle.verify(arg) -> bool`

Quick per-entry CRC check. `arg` is either a string entry name or a
numeric manifest index. Returns:

- `true` when the entry exists and its on-disk bytes hash to the
  recorded CRC.
- `false` when the entry doesn't exist, the index is out of range,
  the bytes are bounds-busted, the CRC doesn't match, or the handle
  has been closed.

Use `verify()` when you want a full report; use `verify(arg)` when
you just want a one-shot bool for a single entry. The full
`verify()` already contains the per-entry detail, so the one-arg
form is purely a convenience for the common case.

```
var b = Pack.openFile("dist/app.zpk");
if (b != null) {
    var rep = b.verify();
    if (!rep.ok) {
        // rep.entries[i] tells you exactly which entry tripped.
    }
    if (b.verify("main.zbc")) {
        // ready to load.
    }
    b.close();
}
```

### What the CRCs cover (and what they don't)

All three CRCs hash **bundle content only** — bytes that live inside
the ZPK region of the file. They are independent of anything the
operating system tracks about the file:

- **Filename / path** — not covered. Renaming `app` to `myapp`, or
  moving the file to a different directory, does not invalidate any
  CRC. The reader locates the footer at `fileSize - footerSize` and
  validates from there; the path is just how you got to the bytes.
- **Filesystem mode bits** — not covered. Toggling the executable bit
  (`chmod +x` / `chmod -x`), changing ownership, or altering ACLs has
  no effect on the CRCs. (`chmod -x` will of course stop the OS from
  running a stub-wrapped bundle, but `Pack.openFile` / `bundle.verify`
  on the same file will still succeed.)
- **Modification timestamps, extended attributes, etc.** — not
  covered. Same reason.
- **The native stub portion of a wrapped executable** — not covered.
  Replacing or modifying the stub (e.g. via `Pack.splice`, or by
  building from a newer runtime binary) does not invalidate the
  payload's CRCs because the stub lives outside the hashed region.

What **is** covered:

- `footer_crc32` — every byte of the footer struct (with the CRC
  field itself zeroed during the hash).
- `manifest_crc32` — the manifest entries concatenated with the
  string table.
- `dataCrc32` (per entry) — that entry's on-disk bytes (i.e. the
  zstd frame for compressed entries, the raw bytes for uncompressed).

In practice this means a freshly built bundle can be renamed,
`chmod`'d, copied between filesystems, or have its stub replaced via
`Pack.splice`, and `bundle.verify().ok` will still return `true` as
long as the bundle bytes themselves were not corrupted in transit.

## Editing an existing bundle

`Pack.build` is the from-scratch path: it writes a whole bundle from
a script-built description. Once a bundle exists, the **edit
transaction** API lets a script open it, stage an arbitrary number of
mutations against an in-memory op log, and `commit()` them as a
**single atomic rewrite**. Use this when you want random-access
authoring — add an entry, swap one out, rename a few, reorder some,
point `entry_index` somewhere else — without re-running the whole
`Pack.build` pipeline.

The shape is a handle with a `commit()` step, on purpose: every
mutation in a `.zpk` re-emits the strtab + manifest + footer at
minimum (offsets are chained), so a per-call mutator API would force
one rewrite per call. The edit handle stages everything in memory and
commits **once**, no matter how many ops were queued.

> ⚠️  **Concurrent edits are last-writer-wins with no detection.**
> Zym is single-process, so cross-process races are not on the table,
> but **two same-process editors of the same path or the same Buffer
> will race**: whichever calls `commit()` last overwrites the work of
> any prior commit, silently. There is no advisory lock, no
> conflict marker, no "you're stale" error. If your script has any
> chance of running two edits against the same target, serialize them
> yourself.

### Construction

```
var edit = Pack.editFile("dist/app.zpk");  // EditHandle | null
var edit = Pack.editBuffer(buf);           // EditHandle | null
```

- **`Pack.editFile(path)`** opens an existing `.zpk` (headless or
  stub-wrapped) from disk. `commit()` writes the new bundle to a
  sibling temp file, `fsync`s it, mirrors the source's mode bits
  (so `+x` survives), then atomically renames it over the source
  path. On failure the temp file is removed and the source is
  untouched.
- **`Pack.editBuffer(buffer)`** opens a `.zpk` whose bytes already
  live in a script `Buffer`. `commit()` builds the new bundle in
  memory, then **resizes and overwrites the borrowed `Buffer` in
  place** — every live script reference to the same Buffer (including
  the caller's local in a function that received it as a parameter)
  observes the new contents on return.

Both constructors return `null` if the input isn't a valid `.zpk`
(footer magic missing, footer CRC mismatch, etc.); they don't raise.

### Lifecycle

```
open → stage many ops → commit() → (optionally stage more) → close()
```

- Successful `commit()` clears the staged op log and re-opens the
  reader against the freshly-committed bytes, so further ops chain
  off the new state.
- Failed `commit()` (e.g. invalid `entry_index` after staged ops,
  unreadable stub path, write error) leaves the handle's staged ops
  intact so the script can fix the problem and retry.
- `close()` drops staged state without writing. It's idempotent;
  calling it on a committed handle is fine. A GC finalizer closes
  the handle if the script forgets.
- `edit.list()` / `edit.info(arg)` / `edit.entryIndex()` read the
  **staged view**, not the source — they reflect every queued op. An
  entry that was staged for `add` or `replace` has `fromSource: false`
  in its `entryInfo`; unchanged source entries have `fromSource: true`.

### Determinism

`Pack.editFile(p).commit()` with **zero ops** produces a
byte-identical file. (This falls out of the writer's existing
determinism guarantee and the zero-copy "borrow from open reader"
path used for unchanged entries.) Useful when you want a "verify
and rewrite" pass without ops, e.g. for future format-version
migrations.

### Entry-argument dispatch

Every op that targets an existing entry (`remove`, `replace`,
`rename`, `move` source side, `setEntryIndex`, `setFlags`,
`setCustom`) accepts either:

- a **string** — first-hit match against `view[i].name`, or
- a **number** — manifest index into the staged view (`0..view.size())`.

Same dispatch as `bundle.open(arg)` / `bundle.info(arg)` /
`bundle.verify(arg)`. Mixed-type arrays of args work fine.

### Op reference

#### `edit.add(spec) -> bool`

Append (or insert at `spec.index`) a new entry. `spec` mirrors a
`Pack.build` entry spec exactly:

```
{
    name:        "<entry name>",
    kind:        "file" | "blob" | "entry_source" | "entry_bytecode" | "source_map",
    data:        <Buffer>,                  // OR
    path:        "<path on disk>",
    flags:       <number> | { required, lazy, mask },   // optional
    compression: "none" | "zstd",                       // optional
    level:       <number>,                              // optional
    custom:      <u32>,                                 // optional
    index:       <number>                               // optional; insertion point
}
```

`kind` and exactly one of `data` / `path` are required; other fields
are optional. `index` out of range (negative or > current view size)
is rejected at stage time.

#### `edit.remove(arg) -> bool`

Removes the entry resolved by `arg`. If the removed slot was
`entry_index`, the staged `entry_index` becomes `null` until either
another `add` shifts in or `setEntryIndex` is staged. The view is
rebuilt before commit; an `entry_index` of `null` at commit time is
an error.

#### `edit.replace(arg, spec) -> bool`

Identity-preserving update: the manifest slot index stays the same,
so a `entry_index` pointing here is preserved. `spec` is partial —
every field is optional; omitted fields keep their source values.
Same `spec` shape as `add` (except `index` is ignored).

#### `edit.rename(arg, newName) -> bool`

Pure strtab-side edit. `newName` must be a string; emptiness /
path-shape validation is the same as the writer's at commit time.

#### `edit.move(srcArg, dstIndex) -> bool`

Reorders the manifest: the entry at `srcArg` moves to `dstIndex`,
shifting others as needed. `dstIndex` must be in `[0..view.size())`.
The staged `entry_index` is auto-updated so it keeps pointing at
the same logical entry.

#### `edit.setEntryIndex(arg) -> bool`

Re-targets the program entry pointer. The kind check (must resolve
to `ENTRY_SOURCE` or `ENTRY_BYTECODE`) runs at commit time against
the final staged state, so it's fine to stage `setEntryIndex`
before the `add` that creates the target.

#### `edit.setFlags(arg, flags) -> bool`

Set the per-entry flags word. `flags` is either a raw `number` (used
directly as the u16 mask) or a map mirroring the reverse-shape of
`entryInfo`:

```
edit.setFlags("main.zbc", { required: true, lazy: false });
edit.setFlags("main.zbc", { mask: 0x01 });   // raw bits
```

#### `edit.setCustom(arg, u32) -> bool`

Set the per-entry `custom` field. Truncated to `uint32_t`.

#### `edit.setStub(arg) -> bool`

Replace, attach, or strip the native stub on commit.

- `arg = "<path>"`   — load stub bytes from a file.
- `arg = <Buffer>`   — use the Buffer's bytes directly. The bytes are
                       copied into the staged op, so the source Buffer
                       can be reused or discarded immediately.
- `arg = null`       — strip the stub; the committed bundle is
                       headless.

If multiple `setStub` ops are staged, the **last one wins**. With no
`setStub` op queued, the source bundle's existing stub is preserved
verbatim.

> **Note.** `Pack.splice(stubPath, zpkPath, outputPath)` and
> `edit.setStub` overlap in functionality but are separate APIs.
> `Pack.splice` is a one-shot fast path that swaps the stub on a
> standalone `.zpk` and writes the result to a new path without
> opening a transaction. Prefer `Pack.splice` for the pure stub-swap
> case; use `editFile(...).setStub(...).commit()` when you also want
> to combine the stub change with other mutations in the same
> transaction.

#### `edit.commit() -> bool`

Materialize the staged ops as a single rewrite.

- For `editFile`: writes to `<path>.zym-edit.tmp.<pid>`, `fsync`s
  on POSIX, mirrors source mode bits via `chmod` on POSIX, then
  atomically renames over the source path. After a successful
  rename the reader is re-opened from the new file. On any failure
  before the rename the temp file is removed and the source is
  untouched.
- For `editBuffer`: builds the new bundle in memory, then resizes
  and overwrites the borrowed `PackedByteArray` underlying the
  source `Buffer`. After write-back, the reader is re-opened
  against the buffer's new bytes.

On success, the staged op log is cleared and further ops chain off
the freshly-committed state.

> ⚠️  **Buffer snapshot caveat.** If a script took a sub-view of the
> source Buffer, or copied bytes out of it into a separate Buffer,
> **before** `commit()`, that snapshot is independent and will *not*
> auto-update when the source Buffer is mutated by the commit.
> Standard mutator caveat; same as any other in-place Buffer write.

#### `edit.close() -> bool`

Drop staged state and close the reader. Idempotent; safe to call on
a committed handle. A GC finalizer is a safety net but explicit
`close()` is recommended.

### Examples

#### Add a file and re-target the entry point

```
var e = Pack.editFile("dist/app.zpk");
e.add({
    name: "main.zbc",
    kind: "entry_bytecode",
    data: newBc
});
e.setEntryIndex("main.zbc");
e.commit();
e.close();
```

#### Replace a config in a stub-wrapped binary, preserving `+x`

```
var e = Pack.editFile("dist/app");                  // stub-wrapped
e.replace("config.json", { data: newCfgBytes });
e.commit();                                         // atomic rename; +x preserved
e.close();
```

#### Mutate a Buffer through a function (out-param pattern)

```
function patchConfig(bundleBuf, newCfg) {
    var e = Pack.editBuffer(bundleBuf);
    e.replace("config.json", { data: newCfg });
    e.commit();                                     // mutates bundleBuf in place
    e.close();
}

var buf = File.readAllBytes("dist/app.zpk");
patchConfig(buf, newCfg);
// buf now contains the rewritten bundle.
File.writeAllBytes("dist/app.zpk", buf);
```

#### Strip a stub, write headless

```
var e = Pack.editFile("dist/app");                  // stub-wrapped
e.setStub(null);
e.commit();                                         // result is now headless
e.close();
```

## See also

- [`docs/formats/zpk.md`](../formats/zpk.md) — on-disk format reference.
