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

// --- the self bundle (the one this process was launched from) ---
Pack.hasSelf()             -> bool
Pack.entryName()           -> string | null
Pack.list()                -> [entryInfo, ...] | null
Pack.has(name)             -> bool
Pack.open(arg)             -> Buffer | null    // arg: name string or numeric index
Pack.info(arg)             -> entryInfo | null // arg: name string or numeric index
Pack.closeSelf()           -> bool

// --- arbitrary bundles ---
var bundle = Pack.openFile(path)  // -> bundle | null
bundle.list()              -> [entryInfo, ...] | null
bundle.entryName()         -> string | null
bundle.has(name)           -> bool
bundle.open(arg)           -> Buffer | null    // arg: name string or numeric index
bundle.info(arg)           -> entryInfo | null // arg: name string or numeric index
bundle.close()             -> bool
```

`Pack.build` describes the whole bundle in one call; the writer it
sits on (`zpk_write_bundle`) is itself batch-shaped, so layering a
streaming builder on top would only re-buffer the same data on the
script side.

The read API is split into two surfaces:

- **`Pack.*` self-bundle methods** read from the running executable
  (or the `.zpk` passed to `zym run`). The reader is opened lazily on
  the first method call and cached until `Pack.closeSelf()` releases
  it; the next method call after a close re-opens it on demand. When
  the running process has no embedded payload, every self-method
  returns `null` / `false` cheaply (no open is attempted).
- **`Pack.openFile(path)`** opens an arbitrary `.zpk` (or stub-wrapped
  binary) and returns a bundle handle. The handle caches the parsed
  reader for its lifetime and frees it on `bundle.close()`. After
  close, every method on the handle returns `null` / `false`. A GC
  finalizer also closes the reader as a safety net if the script
  forgets to call `close()`.

### `spec` map

| Key          | Type     | Required | Default | Notes                                                                                              |
| ---          | ---      | ---      | ---     | ---                                                                                                |
| `output`     | string   | yes      | —       | Destination path. When it ends in `.zpk` a **headless** bundle is produced and `stub` is ignored.  |
| `entries`    | list     | yes      | —       | Non-empty list of entry maps (see below).                                                          |
| `entryIndex` | number   | no       | `0`     | Index into `entries` of the program entry point. Must reference an entry whose `kind` is `entry_bytecode`. |
| `stub`       | string   | no       | none    | Path to a CLI runtime binary to prepend as the executable stub. Ignored when `output` ends in `.zpk`. |
| `compression`| bool     | no       | `false` | Bundle-wide compression default (zstd). When `true`, every entry is compressed unless it sets `compression: false`. When `false` (or omitted) entries default to uncompressed and opt in with `compression: true`. |
| `level`      | number   | no       | `3`     | Default zstd level (`1..22`). Per-entry `level` overrides this. Ignored on entries that resolve to uncompressed. `3` matches zstd's own default; `19+` is the "release-build" sweet spot. |

### Entry map

Each element of `entries` is a map:

| Key      | Type     | Required | Default | Notes                                                                                |
| ---      | ---      | ---      | ---     | ---                                                                                  |
| `name`   | string   | no       | unnamed | Logical name stored in the bundle's string table (e.g. `"main.zbc"`).                |
| `kind`   | string   | yes      | —       | One of the kind strings below.                                                       |
| `flags`  | number   | no       | `0`     | Per-entry flag bits (forwarded to the on-disk `flags` field).                        |
| `custom` | number   | no       | `0`     | Free per-kind 32-bit field, forwarded verbatim.                                      |
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
| `"entry_bytecode"`  | The program entry point's compiled bytecode.         |
| `"module_bytecode"` | A non-entry bytecode module.                         |
| `"source_map"`      | A source map for one of the bytecode entries.        |
| `"asset"`           | A binary asset blob.                                 |
| `"asset_blob"`      | Alias of `"asset"`.                                  |
| `"asset_text"`      | A text asset.                                        |

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
  - `entryIndex` out of range or referencing a non-`entry_bytecode`
    entry
- **Recoverable failures** (file not openable, short read, short
  write, out-of-memory while assembling) return `false`.

## Examples

### Headless `.zpk` with two modules

```
var bytecodeMain = File.readAllBytes("build/main.zbc");
var bytecodeUtil = File.readAllBytes("build/util.zbc");

var ok = Pack.build({
    output: "dist/app.zpk",
    entries: [
        { name: "main.zbc", kind: "entry_bytecode",  data: bytecodeMain },
        { name: "util.zbc", kind: "module_bytecode", data: bytecodeUtil }
    ]
});
if (!ok) {
    print("packing failed");
}
```

### Stub-wrapped executable, asset streamed from disk

The asset never enters script memory; the writer reads it directly.

```
var bytecode = File.readAllBytes("build/main.zbc");

var ok = Pack.build({
    output: "dist/app",                    // not .zpk → wrapped exe
    stub:   "vendor/zym-runtime",          // CLI runtime stub
    entryIndex: 0,
    entries: [
        { name: "main.zbc", kind: "entry_bytecode", data: bytecode },
        { name: "level1.bin", kind: "asset",  path: "assets/level1.bin" },
        { name: "credits.txt", kind: "asset_text", path: "assets/credits.txt" }
    ]
});
```

## Reading bundles

### `entryInfo` map

`Pack.list` / `Pack.info` (and the same methods on a `Pack.openFile`
handle) return per-entry maps with **every** field of the underlying
on-disk entry. Fields unused in v1 (compression byte, reserved slots)
are still surfaced verbatim so scripts can introspect bundles authored
by future writers without an API churn.

| Key                | Type     | Notes                                                                                  |
| ---                | ---      | ---                                                                                    |
| `index`            | number   | Position in the manifest (0-based).                                                    |
| `name`             | string   | Logical name; empty string when the entry was unnamed.                                 |
| `kind`             | string   | One of the [kind strings](#kind-vocabulary), or `"reserved:0xNN"` / `"user:0xNN"` for bytes outside the documented set. |
| `kindByte`         | number   | Raw kind byte (0–255).                                                                 |
| `compression`      | string   | `"none"` (v1), or `"zstd"` / `"deflate"` / `"unknown"` for future formats.             |
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

`Pack.open`, `Pack.info`, `bundle.open`, and `bundle.info` all accept
either a **string** entry name or a **numeric** manifest index:

- `open("main.zbc")` / `info("main.zbc")` — looks up the first entry
  whose name matches. Returns `null` if no entry has that name.
- `open(0)` / `info(0)` — looks up the entry at that 0-based manifest
  position. Returns `null` if the index is out of range.

Bundles may legally contain multiple entries that share the same
name (each manifest slot is independent). When that happens the
string form resolves to the first match only — use the numeric index
to address any subsequent entry. `Pack.list()` (and `bundle.list()`)
return entries in manifest order, so a typical pattern is to walk
`list()` to find duplicates and then call `open(index)` / `info(index)`
on the specific entries you care about. The single-arg `verify(arg)`
on both surfaces follows the same string/number dispatch.

### Self-bundle vs. `openFile` lifecycle

- The self-bundle methods (`Pack.hasSelf`, `Pack.entryName`, `Pack.list`,
  `Pack.has`, `Pack.open`, `Pack.info`) lazily open
  the running executable's reader on the first call and keep it
  cached. `Pack.closeSelf()` returns `true` if a cached reader was
  released (and `false` if there was nothing to close). The very next
  self-method call after a close will re-open the reader on demand.
- `Pack.openFile(path)` returns a bundle handle on success and `null`
  if the file is not a valid bundle. Each handle owns its own reader,
  cached until `bundle.close()` is called. After close, every method
  on the handle returns `null` / `false`. Forgetting to call `close()`
  is not a leak — a GC finalizer closes the reader when the handle is
  collected — but the explicit `close()` is the recommended pattern
  because it bounds memory use the moment the script is done.

### Reading the self bundle

```
if (Pack.hasSelf()) {
    var entryName = Pack.entryName();
    var list      = Pack.list();
    if (Pack.has("config.json")) {
        var bytes = Pack.open("config.json");
        // bytes is a Buffer; bytes.toString() decodes as UTF-8.
    }
    Pack.closeSelf();
}
```

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
`Pack.openFile` returns `null` and `Pack.hasSelf()` returns `false`.
The manifest CRC and per-entry data CRCs are not enforced at open
time — they're surfaced through `verify()` so scripts can decide what
to do on mismatch.

Both `Pack` (the self bundle) and a `Pack.openFile` handle expose the
same two-arity `verify`:

### `Pack.verify()` / `bundle.verify() -> map | null`

Runs all three CRC checks and returns a structured report. Returns
`null` when there is no self bundle / when the handle has been closed.

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

### `Pack.verify(arg)` / `bundle.verify(arg) -> bool`

Quick per-entry CRC check. `arg` is either a string entry name or a
numeric manifest index. Returns:

- `true` when the entry exists and its on-disk bytes hash to the
  recorded CRC.
- `false` when the entry doesn't exist, the index is out of range,
  the bytes are bounds-busted, the CRC doesn't match, or there is no
  self bundle / the handle has been closed.

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

## See also

- [`docs/formats/zpk.md`](../formats/zpk.md) — on-disk format reference.
