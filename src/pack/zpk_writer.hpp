// ZPK writer — emits a .zpk bundle to a file path.
//
// See docs/formats/zpk.md for the format. This header is the public
// surface used by `src/full_executor.cpp` for `-o <out>` packing.

#ifndef ZYM_PACK_ZPK_WRITER_HPP
#define ZYM_PACK_ZPK_WRITER_HPP

#include <stdint.h>
#include <stddef.h>

// The writer can borrow already-on-disk-shaped entry bytes directly
// from an open reader, skipping its compression pass. See
// `ZpkEntryInput::source_reader` below.
#include "zpk_reader.hpp"

// One entry to emit. Names are referenced by (offset, length) pairs in
// the on-disk manifest; the writer is responsible for laying out the
// string table and patching offsets. `name` may be null for an unnamed
// entry (length will be set to 0).
//
// `data` / `data_size` are the *raw* (uncompressed) bytes to store
// in the data region. When `compression` is non-zero the writer
// compresses them in-place before laying out the bundle; the on-disk
// `data_size` then reflects the compressed size and `uncompressed_size`
// reflects the original. With `compression == ZPK_COMPRESSION_NONE`
// (the default) the bytes are written verbatim.
//
// Bytes source: exactly one of three paths supplies the entry's bytes:
//   1. In-memory: set `data` / `data_size`, leave `file_path` null and
//      `source_reader` null.
//   2. Streamed from disk: set `file_path` (NUL-terminated), leave
//      `data` null and `data_size` 0, `source_reader` null. The writer
//      opens the file, reads it, and inserts its contents at this
//      entry's slot. This avoids requiring callers (e.g. the `Pack`
//      script-facing native) to first materialize large assets as
//      in-memory `Buffer`s.
//   3. Borrowed from an open reader: set `source_reader` to a live
//      `ZpkReader*` and `source_index` to the entry index within that
//      reader. The writer `memcpy`s the on-disk slice straight from
//      the reader's `file_data` into the output and inherits the
//      reader's `compression`, `uncompressed_size`, and `data_crc32`
//      verbatim — no recompression, no CRC recomputation. The
//      `data`/`data_size`/`file_path` fields are ignored when
//      `source_reader` is set, and so is this struct's `compression`
//      (the source entry's compression byte wins). The caller must
//      keep the reader alive for the duration of the write.
//
// Setting more than one bytes-source, or none, is a programmer error
// and the writer fails the call.
//
// Compression: `compression` is a `ZpkCompression` byte (0 = none,
// 1 = zstd). When zstd is requested, `level` selects the zstd level
// in 1..22; pass 0 to use the writer's safe default (3). If the
// compressed output isn't strictly smaller than the raw input the
// writer auto-falls back to storing the entry uncompressed — scripts
// never get a "compression made it bigger" surprise.
typedef struct {
    const char* name;         // optional; UTF-8; not NUL-terminated requirement
    size_t      name_length;  // bytes in `name`; ignored if `name == nullptr`
    uint8_t     kind;         // ZpkKind
    uint8_t     compression;  // ZpkCompression; 0 = none (default)
    int         level;        // codec level; 0 = codec default
    uint16_t    flags;        // ZPK_ENTRY_FLAG_*
    uint32_t    custom;       // free per kind
    const void* data;
    size_t      data_size;
    const char* file_path;        // optional; when non-null, writer streams from this path
    const ZpkReader* source_reader; // optional; when non-null, writer borrows the slice from this reader
    uint32_t    source_index;     // entry index within `source_reader` (ignored if `source_reader == nullptr`)
} ZpkEntryInput;

// Write a bundle to `out_path`.
//
// - `stub_data`/`stub_size`: optional CLI stub bytes prepended at offset 0.
//   Pass nullptr/0 to produce a headless `.zpk`.
// - `entries`/`entry_count`: list of entries to emit, in the order the
//   writer should lay them out in the data region.
// - `entry_index`: index into `entries` of the program entry point. The
//   referenced entry must have `kind == ZPK_KIND_ENTRY_BYTECODE` or
//   `ZPK_KIND_ENTRY_SOURCE`.
//
// Returns 1 on success, 0 on failure (with diagnostics on stderr).
int zpk_write_bundle(const char* out_path,
                     const void* stub_data, size_t stub_size,
                     const ZpkEntryInput* entries, size_t entry_count,
                     uint32_t entry_index);

// Same as `zpk_write_bundle`, but emits to a caller-owned malloc'd
// buffer rather than to a path. On success the function `malloc`s
// `*out_buf` (the caller must `free` it) and stores its size in
// `*out_size`. On failure both out-params are zeroed and 0 is
// returned.
//
// Used by the `Pack.editBuffer` commit path so the writer doesn't
// have to round-trip through a temp file when the target is a
// script-side `Buffer`.
int zpk_write_bundle_to_memory(uint8_t** out_buf, size_t* out_size,
                               const void* stub_data, size_t stub_size,
                               const ZpkEntryInput* entries, size_t entry_count,
                               uint32_t entry_index);

#endif // ZYM_PACK_ZPK_WRITER_HPP
