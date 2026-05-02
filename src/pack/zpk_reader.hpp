// ZPK reader — opens a .zpk bundle (headless or stub-wrapped) and
// exposes random access to manifest entries.
//
// See docs/formats/zpk.md for the format. This header is the public
// surface used by `src/runtime_loader.cpp` to find and load the entry
// bytecode at process startup.

#ifndef ZYM_PACK_ZPK_READER_HPP
#define ZYM_PACK_ZPK_READER_HPP

#include <stdint.h>
#include <stddef.h>

#include "zpk_format.h"

// Opaque-ish handle. Fields are exposed for read-only inspection by
// callers that want to peek at the footer / manifest directly; users
// should still go through `zpk_reader_read_entry` for entry data.
typedef struct {
    // Whole-file bytes loaded into memory. Owned by the reader.
    uint8_t*   file_data;
    size_t     file_size;

    // Decoded footer (copy; the on-disk footer at the tail of
    // `file_data` is identical).
    ZpkFooter  footer;

    // Pointers into `file_data`. Valid until `zpk_reader_close`.
    const ZpkEntry* manifest;     // entry_count entries
    const uint8_t*  strtab;       // strtab_size bytes; may be null if strtab_size == 0
} ZpkReader;

// Open a `.zpk` from a filesystem path. Returns 1 on success, 0 on
// failure (with diagnostics on stderr). On success, the caller owns
// the reader and must call `zpk_reader_close` to release it.
int zpk_reader_open_path(ZpkReader* out, const char* path);

// Open the running executable (resolved via /proc/self/exe on Linux,
// GetModuleFileNameA on Windows) and parse it as a `.zpk`. Returns 1
// on success, 0 on failure.
int zpk_reader_open_self_exe(ZpkReader* out);

// Release any resources owned by the reader and zero its fields.
void zpk_reader_close(ZpkReader* r);

// Probe whether a path looks like a valid `.zpk` (magic + footer_size
// + version) without keeping it open. Returns 1 if valid, 0 otherwise.
// Useful for the legacy "has_embedded_bytecode" check in
// runtime_loader.
int zpk_reader_path_has_payload(const char* path);

// Same probe against the running executable.
int zpk_reader_self_exe_has_payload();

// Copy the bytes of the entry at `index` into a freshly malloc'd
// buffer. The caller takes ownership and must `free()` it. Returns
// nullptr on error (and writes diagnostics to stderr).
//
// For uncompressed entries (the only kind v1 produces), this is a
// straight slice copy. The CRC is verified; v1 treats a CRC mismatch
// as a warning, not an error, per docs/formats/zpk.md.
//
// On success, `*out_size` is set to the entry's logical (uncompressed)
// size in bytes.
char* zpk_reader_read_entry(const ZpkReader* r, uint32_t index, size_t* out_size);

#endif // ZYM_PACK_ZPK_READER_HPP
