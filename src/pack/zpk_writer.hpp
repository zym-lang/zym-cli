// ZPK writer — emits a .zpk bundle to a file path.
//
// See docs/formats/zpk.md for the format. This header is the public
// surface used by `src/full_executor.cpp` for `-o <out>` packing.

#ifndef ZYM_PACK_ZPK_WRITER_HPP
#define ZYM_PACK_ZPK_WRITER_HPP

#include <stdint.h>
#include <stddef.h>

// One entry to emit. Names are referenced by (offset, length) pairs in
// the on-disk manifest; the writer is responsible for laying out the
// string table and patching offsets. `name` may be null for an unnamed
// entry (length will be set to 0).
//
// `data` / `data_size` are the bytes to write into the data region.
// In v1 they are stored uncompressed (see docs/formats/zpk.md).
typedef struct {
    const char* name;         // optional; UTF-8; not NUL-terminated requirement
    size_t      name_length;  // bytes in `name`; ignored if `name == nullptr`
    uint8_t     kind;         // ZpkKind
    uint16_t    flags;        // ZPK_ENTRY_FLAG_*
    uint32_t    custom;       // free per kind
    const void* data;
    size_t      data_size;
} ZpkEntryInput;

// Write a bundle to `out_path`.
//
// - `stub_data`/`stub_size`: optional CLI stub bytes prepended at offset 0.
//   Pass nullptr/0 to produce a headless `.zpk`.
// - `entries`/`entry_count`: list of entries to emit, in the order the
//   writer should lay them out in the data region.
// - `entry_index`: index into `entries` of the program entry point. The
//   referenced entry must have `kind == ZPK_KIND_ENTRY_BYTECODE`.
//
// Returns 1 on success, 0 on failure (with diagnostics on stderr).
int zpk_write_bundle(const char* out_path,
                     const void* stub_data, size_t stub_size,
                     const ZpkEntryInput* entries, size_t entry_count,
                     uint32_t entry_index);

#endif // ZYM_PACK_ZPK_WRITER_HPP
