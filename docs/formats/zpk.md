# ZPK — Zym Packed Bundle Format

`.zpk` is the container format used by the Zym CLI to bundle one or more
`.zbc` bytecode modules — and, eventually, arbitrary assets — into a
single artifact that the runtime can load and execute. It supersedes the
earlier 12‑byte `ZYMBCODE` footer used by the first iteration of
`runtime_loader` and `full_executor`.

A `.zpk` is a **packed bundle as a feature of the CLI**, not of the
core language. `zym_core` knows nothing about this format; only the CLI
(`src/runtime_loader.cpp` for reading, `src/full_executor.cpp` for
writing) and the `src/pack/` library do.

This document is the on‑disk specification. It is the source of truth
for both the writer and the reader; if any field, offset, or value here
disagrees with the C++ structs in `src/pack/zpk_format.h`, this
document wins and the header is wrong.

---

## At a glance

A `.zpk` exists in two **physical forms** that share **one logical
layout**:

- **Headless bundle** (`app.zpk`) — the file is *only* the packed
  payload, executable via `zym run app.zpk`.
- **Stub‑wrapped bundle** (`app`, `app.exe`) — the same payload is
  appended to a native CLI executable that auto‑loads it on launch.

The reader does not need to distinguish between the two. All offsets
inside a `.zpk` are absolute from byte 0 of the file, and the format
metadata lives at the very end, so the only difference between forms is
whether byte 0 is your payload or the stub's ELF/PE/Mach‑O header.

```
+---------------------------------------------------------+ offset 0
| (optional) CLI stub — untouched native executable       |
+---------------------------------------------------------+
| Data region                                             |
|   - entry bytecode                                      |
|   - additional bytecode modules                         |
|   - assets (text, blobs, ...)                           |
|   - (reserved) signature blob                           |
+---------------------------------------------------------+
| String table (UTF-8, no NUL terminators required)       |
+---------------------------------------------------------+
| Manifest entries (fixed-size records, packed array)     |
+---------------------------------------------------------+
| Footer (fixed 64 bytes, ends exactly at EOF)            |
+---------------------------------------------------------+ EOF
```

Three rules govern the layout:

1. **All offsets are absolute** from byte 0 of the file. The stub size
   varies per OS/arch and must not leak into the payload.
2. **The footer is fixed‑size and ends at EOF.** Readers do
   `seek(end - 64); read(64)`. There is no scanning for magic.
3. **Manifest entries are fixed‑size.** Names live in a separate
   string table. Random access is O(1); the manifest can be `mmap`ed
   and treated as a C array.

---

## Endianness, alignment, and encoding

- All multi‑byte integers are **little‑endian**.
- Structs are **packed**: no implicit compiler padding. Field offsets
  match this document exactly.
- The footer is aligned to its EOF anchor; the manifest is aligned to
  8 bytes (entries are 48 bytes, divisible by 8); the data region has
  no alignment requirement on individual entries beyond what the writer
  chooses to emit.
- All names in the string table are **UTF‑8**. NUL bytes are permitted
  inside names but discouraged. Names are referenced by `(offset,
  length)`, so no terminator is required.

---

## Footer

The footer is the **64‑byte block ending at EOF**. It is the entry
point for every reader.

| Offset | Size | Field              | Notes |
| --- | --- | --- | --- |
| 0  | 8 | `magic[8]`            | ASCII `"ZYMPK\0\0\0"`. Identifies a `.zpk` payload. Never changes between versions. |
| 8  | 2 | `format_version`      | Starts at `1`. Bumped only on incompatible changes. |
| 10 | 2 | `footer_size`         | Size of this footer in bytes. `64` for v1. Allows future versions to grow the footer; older readers see `footer_size > 64` and refuse with a clear "newer format" error. |
| 12 | 4 | `flags`               | Bit field. See *Footer flags* below. Unknown bits must be ignored unless documented as `MUST_UNDERSTAND`. |
| 16 | 8 | `manifest_offset`     | Absolute offset of the first `ZpkEntry`. |
| 24 | 4 | `entry_count`         | Number of `ZpkEntry` records in the manifest. |
| 28 | 4 | `entry_index`         | Index of the manifest entry that the runtime should execute as the program entry point. Must be `< entry_count` and must point to an entry of kind `ENTRY_BYTECODE`. |
| 32 | 8 | `strtab_offset`       | Absolute offset of the string table. |
| 40 | 8 | `strtab_size`         | Size of the string table in bytes. |
| 48 | 4 | `manifest_crc32`      | CRC‑32 (IEEE 802.3 polynomial) computed over the manifest entries followed by the string table. |
| 52 | 4 | `footer_crc32`        | CRC‑32 of this 64‑byte footer with `footer_crc32` itself treated as zero during the calculation. |
| 56 | 8 | _reserved_            | Must be written as zero in v1. Readers must not interpret. |

Total: **64 bytes**.

### Footer flags

| Bit | Name                  | Meaning |
| --- | --- | --- |
| 0   | `SIGNED`              | (Reserved.) A `SIGNATURE` entry exists and covers the rest of the file. Not validated in v1. |
| 1   | `MANIFEST_COMPRESSED` | (Reserved.) The manifest entries and/or string table are compressed. Not used in v1; manifests are always stored raw. |
| 2…31 | _reserved_           | Must be written as zero. |

### Validation rules

A reader must, in order:

1. Confirm the file is at least `footer_size` bytes long.
2. Read the last `64` bytes and verify `magic == "ZYMPK\0\0\0"`.
3. Verify `footer_size >= 64` and reject with a versioning error if
   `footer_size > 64` and the reader does not understand the extension.
4. Verify `format_version` is supported.
5. Recompute and verify `footer_crc32`.
6. Verify `manifest_offset + entry_count * 48 <= file_size - footer_size`.
7. Verify `strtab_offset + strtab_size <= file_size - footer_size`.
8. Recompute and verify `manifest_crc32`.
9. Verify `entry_index < entry_count` and that the indexed entry has
   `kind == ENTRY_BYTECODE`.

CRC mismatches in v1 may be reported as a warning by the loader (so a
half‑broken bundle still attempts to run) but will be promoted to a hard
error in a later version once the writer is trusted. New writers must
always emit correct CRCs.

---

## Manifest entry

Each manifest entry is **48 bytes**, fixed. The manifest is `entry_count`
of these laid out contiguously starting at `manifest_offset`.

| Offset | Size | Field               | Notes |
| --- | --- | --- | --- |
| 0  | 1 | `kind`                 | See *Entry kinds* below. |
| 1  | 1 | `compression`          | See *Compression* below. `0` (none) in v1. |
| 2  | 2 | `flags`                | Per‑entry bit field. See *Entry flags*. |
| 4  | 4 | `name_offset`          | Byte offset into the string table. |
| 8  | 4 | `name_length`          | Length in bytes. May be `0` for unnamed entries. |
| 12 | 4 | _reserved_             | Must be zero. Pads `data_offset` to 8‑byte alignment. |
| 16 | 8 | `data_offset`          | Absolute offset of the entry's bytes. |
| 24 | 8 | `data_size`            | Size of the on‑disk bytes (post‑compression, post‑any‑transform). |
| 32 | 8 | `uncompressed_size`    | Logical byte size after decompression. Equal to `data_size` when `compression == 0`. |
| 40 | 4 | `data_crc32`           | CRC‑32 of the on‑disk bytes (`data_offset .. data_offset + data_size`). |
| 44 | 4 | `custom`               | Free per‑kind. Defined by the kind that uses it. Otherwise zero. |

Total: **48 bytes**.

### Entry kinds

The `kind` byte is descriptive — it tells consumers what an entry
*is* — but it is **not** what determines the program entry point. The
program entry point is whichever entry the footer's `entry_index`
points at. `kind` and `entry_index` are orthogonal.

| Value | Name              | Meaning |
| --- | --- | --- |
| `0x00` | `RESERVED`       | Invalid. Catches all‑zero corruption. Readers must reject. |
| `0x01` | `ENTRY_BYTECODE` | A `.zbc` bytecode module suitable for use as the program entry point. Must be the kind of the entry indexed by `entry_index`. |
| `0x02` | `MODULE_BYTECODE`| A `.zbc` bytecode module imported by name via the loader's module resolver. |
| `0x03` | `SOURCE_MAP`     | Optional debug information paired with a bytecode module. The pairing is by name (e.g. module `"foo"` → source map `"foo.map"`); the loader is free to ignore source maps it does not consume. |
| `0x04` | `ASSET_BLOB`     | Arbitrary bytes addressable by name. |
| `0x05` | `ASSET_TEXT`     | UTF‑8 text. Identical to `ASSET_BLOB` on disk; the kind exists only as a convenience hint to consumers. |
| `0x06` | `NATIVE_LIB`     | (Reserved.) Bundled native shared library. Not loaded by v1. |
| `0x07` | `MANIFEST_EXT`   | (Reserved.) Structured metadata (e.g. TOML/JSON) consumed by the loader for build info, target ABI, copyright, etc. Not consumed by v1. |
| `0x08` | `SIGNATURE`      | (Reserved.) Detached signature blob covering the file outside this entry's data range. Not validated by v1. |
| `0x09 .. 0x7E` | _reserved_ | Reserved for future Zym use. |
| `0x7F .. 0xFF` | `USER_*`   | Free for user/plugin use. The runtime ignores entries with user kinds; scripts may consume them via the `pack.*` API. |

A reader **must not** treat unknown `kind` values as errors — it must
ignore them, unless the entry's `flags` bit `REQUIRED` is set, in which
case the reader must refuse to load the bundle. This is the format's
forward‑compatibility hinge.

### Entry flags

| Bit | Name        | Meaning |
| --- | --- | --- |
| 0   | `REQUIRED`  | If the reader does not understand this entry's `kind`, it must fail loading the bundle. Used to mark entries that future versions may add and that older runtimes cannot safely ignore. |
| 1   | `LAZY`      | Hint: the loader should not eagerly read or decompress this entry. Purely advisory; readers may ignore. |
| 2…15 | _reserved_ | Must be zero. |

### Compression

| Value | Algorithm | Status |
| --- | --- | --- |
| `0` | none      | Required. Always supported. |
| `1` | zstd      | Reserved. Not produced by v1 writer; readers may decline. |
| `2` | deflate   | Reserved. Not produced by v1 writer; readers may decline. |
| other | _reserved_ | Reader rejects unless it understands the value. |

In v1 the writer must emit `compression == 0` for every entry, and
`uncompressed_size == data_size`. The byte exists in the layout from day
one so that compression can be added later without a format bump.

---

## String table

A contiguous blob of UTF‑8 bytes at `(strtab_offset, strtab_size)`.
Names are referenced by `(name_offset, name_length)` pairs in entries;
no terminator byte is required, and overlapping or shared substrings
are permitted (writers may, but need not, deduplicate). Empty names
(`name_length == 0`) are valid and indicate an unnamed entry; the
program entry point typically has a conventional name (`"main.zbc"`)
but this is not required by the format.

A name must be valid UTF‑8 and must not start with `/` or contain `..`
path components if the loader exposes it through a path‑like API
(`pack.open`, module resolver). Writers should normalize paths to
forward slashes.

---

## Discovery

The runtime locates a `.zpk` payload using this fixed precedence:

1. If invoked as `zym run <path>`, the file at `<path>` is opened and
   parsed as a `.zpk`.
2. Otherwise, the runtime resolves its own executable path
   (`/proc/self/exe` on Linux, `GetModuleFileNameW` on Windows,
   `_NSGetExecutablePath` on macOS — never raw `argv[0]`) and attempts
   to read a footer at EOF.
3. If neither yields a valid footer, the runtime falls back to its
   non‑bundled behavior (CLI mode).

This is identical in spirit to the discovery used by the original
`ZYMBCODE` footer; only the validation step changes.

---

## Writer algorithm

The reference writer (`src/pack/zpk_writer`) emits a bundle as follows.
Inputs: an optional stub byte slice, a list of entry inputs
`(name, kind, flags, bytes)`, and an `entry_index` selecting which
entry is the program entry point.

1. If a stub is provided, write it first. Record `stub_end` as the
   current file position. Otherwise `stub_end == 0`.
2. For each entry input, write its bytes to the file, recording the
   absolute `data_offset` and the actual `data_size`. In v1
   `uncompressed_size == data_size`. Compute `data_crc32` over the
   bytes as written.
3. Write the string table, recording `strtab_offset` and `strtab_size`.
4. Compute and write the manifest entries, recording
   `manifest_offset`. `name_offset`/`name_length` reference the string
   table written in step 3.
5. Compute `manifest_crc32` over the manifest bytes followed by the
   string table bytes.
6. Construct the footer with `magic`, `format_version = 1`,
   `footer_size = 64`, `flags = 0`, `manifest_offset`, `entry_count`,
   `entry_index`, `strtab_offset`, `strtab_size`, `manifest_crc32`,
   and `footer_crc32 = 0`.
7. Compute `footer_crc32` over the 64‑byte footer with the
   `footer_crc32` field zeroed; patch it into the footer.
8. Write the footer.

Determinism rules for reproducible builds:

- The writer must not embed timestamps in the footer or manifest
  unless the caller explicitly opts in via a `MANIFEST_EXT` entry.
- For a given input list and `entry_index`, the writer must emit a
  byte‑identical file across runs. The string table layout is
  permitted but not required to deduplicate; whichever choice the
  writer makes must be deterministic.

## Reader algorithm

The reference reader (`src/pack/zpk_reader`) opens a path or the
running executable, validates, and exposes random access to entries.

1. Open the file read‑only. Prefer `mmap` so entry reads are
   zero‑copy when uncompressed.
2. If the file is shorter than 64 bytes, fail.
3. Read the last 64 bytes. Verify `magic`, `format_version`,
   `footer_size`, and `footer_crc32` per *Validation rules* above.
4. Read the manifest (`entry_count * 48` bytes at `manifest_offset`)
   and the string table.
5. Verify `manifest_crc32`.
6. Build a name → entry index map for `find(name)`.
7. For `read(index)`:
   - Bounds‑check `index < entry_count`.
   - If `compression == 0`: return a borrowed slice
     `[data_offset, data_offset + data_size)`.
   - Otherwise: decompress to an owned buffer of size
     `uncompressed_size`. (Not implemented in v1.)
   - Optionally verify `data_crc32`.
8. The "entry bytecode" used by `runtime_loader` is simply
   `read(footer.entry_index)`.

---

## Relationship to the legacy `ZYMBCODE` footer

The earlier format was:

```
[bytecode][u32 size little-endian][8B "ZYMBCODE"]
```

with a fixed 12‑byte footer carrying only the bytecode size. `.zpk`
replaces it. The two formats are intentionally distinguished by their
magic strings (`"ZYMBCODE"` vs `"ZYMPK\0\0\0"`), so a reader can
detect a legacy bundle and emit a clear error rather than misparsing
it. There is no in‑place upgrade path; legacy bundles must be
re‑packed with the current `zym` CLI.

---

## Compatibility policy

- `magic` is **frozen for the lifetime of the format**. Variants are
  expressed via `format_version`, not new magic.
- `format_version` increments only on **incompatible** changes.
  Additive changes (new `kind` values, new `flags` bits without
  `REQUIRED`, larger `footer_size`) do not bump the version.
- A reader for version `N` must accept bundles produced by writers
  for version `N`, ignoring unknown kinds (without `REQUIRED`),
  unknown non‑`MUST_UNDERSTAND` flag bits, and trailing footer bytes
  beyond the 64 it understands.
- A reader for version `N` may reject bundles produced for version
  `M > N` if it cannot safely interpret them. The error must mention
  both versions.

---

## Reserved and unused fields

The following are reserved in v1 and must be written as zero. Future
versions may give them meaning; readers must not interpret them.

- Footer: bytes `56..63` (the trailing 8‑byte reserved region).
- Footer flag bits `2..31`.
- Manifest entry: bytes `12..15` (`reserved`), `flags` bits `2..15`.
- Compression values other than `0` (until activated by a later
  version of this document).
- Entry kinds `0x09..0x7E`.
