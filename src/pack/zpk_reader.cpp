#include "zpk_reader.hpp"
#include "zpk_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

bool resolve_self_exe(char* buffer, size_t size) {
#ifdef _WIN32
    DWORD result = GetModuleFileNameA(NULL, buffer, (DWORD)size);
    return result != 0 && result != size;
#else
    ssize_t len = readlink("/proc/self/exe", buffer, size - 1);
    if (len <= 0) return false;
    buffer[len] = '\0';
    return true;
#endif
}

// Slurp the whole file into a fresh buffer. On failure returns nullptr.
uint8_t* slurp(const char* path, size_t* out_size) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return nullptr; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return nullptr; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return nullptr; }

    uint8_t* buf = static_cast<uint8_t*>(malloc(static_cast<size_t>(n)));
    if (!buf) { fclose(f); return nullptr; }

    size_t read_count = fread(buf, 1, static_cast<size_t>(n), f);
    fclose(f);
    if (read_count != static_cast<size_t>(n)) {
        free(buf);
        return nullptr;
    }
    *out_size = static_cast<size_t>(n);
    return buf;
}

// Validate the on-disk footer in `file_data`. On success populates
// `*out_footer` and returns 1. On failure logs a diagnostic if
// `verbose` is set and returns 0.
int validate_footer(const uint8_t* file_data, size_t file_size,
                    ZpkFooter* out_footer, bool verbose) {
    if (file_size < ZPK_FOOTER_SIZE) {
        if (verbose) fprintf(stderr, "zpk: file too small to contain a footer (%zu bytes).\n", file_size);
        return 0;
    }

    ZpkFooter f;
    memcpy(&f, file_data + file_size - ZPK_FOOTER_SIZE, ZPK_FOOTER_SIZE);

    static const uint8_t expected_magic[ZPK_MAGIC_SIZE] = ZPK_MAGIC_BYTES;
    if (memcmp(f.magic, expected_magic, ZPK_MAGIC_SIZE) != 0) {
        if (verbose) {
            // Distinguish the legacy ZYMBCODE bundle from "no payload at all"
            // so users get a useful error message during the migration.
            if (file_size >= 12 &&
                memcmp(file_data + file_size - 8, "ZYMBCODE", 8) == 0) {
                fprintf(stderr,
                        "zpk: legacy ZYMBCODE footer detected; this binary was "
                        "produced by an older zym CLI. Re-pack with the current "
                        "version to upgrade to the .zpk format.\n");
            } else {
                fprintf(stderr, "zpk: no .zpk payload found (footer magic missing).\n");
            }
        }
        return 0;
    }

    if (f.footer_size != ZPK_FOOTER_SIZE) {
        if (verbose) fprintf(stderr, "zpk: unsupported footer_size %u (this build understands %u).\n",
                             (unsigned)f.footer_size, (unsigned)ZPK_FOOTER_SIZE);
        return 0;
    }

    if (f.format_version != ZPK_FORMAT_VERSION) {
        if (verbose) fprintf(stderr, "zpk: unsupported format_version %u (this build understands %u).\n",
                             (unsigned)f.format_version, (unsigned)ZPK_FORMAT_VERSION);
        return 0;
    }

    // Verify footer CRC: hash a copy of the footer with footer_crc32 zeroed.
    {
        ZpkFooter tmp = f;
        uint32_t expected = tmp.footer_crc32;
        tmp.footer_crc32 = 0;
        uint32_t actual = zpk_crc32(0, &tmp, sizeof(tmp));
        if (actual != expected) {
            if (verbose) fprintf(stderr, "zpk: footer CRC mismatch (file=%08x computed=%08x).\n",
                                 expected, actual);
            return 0;
        }
    }

    // Bounds checks for the regions the footer points at.
    const uint64_t payload_end = static_cast<uint64_t>(file_size) - ZPK_FOOTER_SIZE;
    const uint64_t manifest_end =
        f.manifest_offset + static_cast<uint64_t>(f.entry_count) * ZPK_ENTRY_SIZE;
    if (manifest_end > payload_end) {
        if (verbose) fprintf(stderr, "zpk: manifest extends beyond footer.\n");
        return 0;
    }
    if (f.strtab_offset + f.strtab_size > payload_end) {
        if (verbose) fprintf(stderr, "zpk: string table extends beyond footer.\n");
        return 0;
    }
    if (f.entry_count > 0 && f.entry_index >= f.entry_count) {
        if (verbose) fprintf(stderr, "zpk: entry_index %u out of range (entry_count=%u).\n",
                             f.entry_index, f.entry_count);
        return 0;
    }

    *out_footer = f;
    return 1;
}

} // namespace

int zpk_reader_open_path(ZpkReader* out, const char* path) {
    if (!out || !path) return 0;
    memset(out, 0, sizeof(*out));

    size_t file_size = 0;
    uint8_t* file_data = slurp(path, &file_size);
    if (!file_data) {
        fprintf(stderr, "zpk: could not read \"%s\".\n", path);
        return 0;
    }

    ZpkFooter footer;
    if (!validate_footer(file_data, file_size, &footer, /*verbose=*/true)) {
        free(file_data);
        return 0;
    }

    const ZpkEntry* manifest = reinterpret_cast<const ZpkEntry*>(
        file_data + footer.manifest_offset);
    const uint8_t* strtab = (footer.strtab_size > 0)
        ? (file_data + footer.strtab_offset)
        : nullptr;

    // Verify manifest CRC over (entries || strtab).
    {
        const size_t mfs = static_cast<size_t>(footer.entry_count) * ZPK_ENTRY_SIZE;
        uint32_t crc = zpk_crc32(0, manifest, mfs);
        if (footer.strtab_size > 0) {
            crc = zpk_crc32(crc, strtab, static_cast<size_t>(footer.strtab_size));
        }
        if (crc != footer.manifest_crc32) {
            // Per docs/formats/zpk.md: warn-only in v1.
            fprintf(stderr, "zpk: warning: manifest CRC mismatch in \"%s\" "
                            "(file=%08x computed=%08x). Continuing.\n",
                    path, footer.manifest_crc32, crc);
        }
    }

    // Validate the entry-point entry up front so callers can rely on it.
    if (footer.entry_count == 0 ||
        manifest[footer.entry_index].kind != ZPK_KIND_ENTRY_BYTECODE) {
        fprintf(stderr, "zpk: entry-point entry must have kind ENTRY_BYTECODE.\n");
        free(file_data);
        return 0;
    }

    out->file_data = file_data;
    out->file_size = file_size;
    out->footer    = footer;
    out->manifest  = manifest;
    out->strtab    = strtab;
    return 1;
}

int zpk_reader_open_self_exe(ZpkReader* out) {
    char exe_path[4096];
    if (!resolve_self_exe(exe_path, sizeof(exe_path))) {
        fprintf(stderr, "zpk: could not determine executable path.\n");
        return 0;
    }
    return zpk_reader_open_path(out, exe_path);
}

void zpk_reader_close(ZpkReader* r) {
    if (!r) return;
    if (r->file_data) free(r->file_data);
    memset(r, 0, sizeof(*r));
}

int zpk_reader_path_has_payload(const char* path) {
    if (!path) return 0;
    size_t file_size = 0;
    uint8_t* file_data = slurp(path, &file_size);
    if (!file_data) return 0;
    ZpkFooter footer;
    int ok = validate_footer(file_data, file_size, &footer, /*verbose=*/false);
    free(file_data);
    return ok;
}

int zpk_reader_self_exe_has_payload() {
    char exe_path[4096];
    if (!resolve_self_exe(exe_path, sizeof(exe_path))) return 0;
    return zpk_reader_path_has_payload(exe_path);
}

char* zpk_reader_read_entry(const ZpkReader* r, uint32_t index, size_t* out_size) {
    if (!r || !r->file_data || !out_size) return nullptr;
    if (index >= r->footer.entry_count) {
        fprintf(stderr, "zpk: entry index %u out of range (entry_count=%u).\n",
                index, r->footer.entry_count);
        return nullptr;
    }

    const ZpkEntry& e = r->manifest[index];

    if (e.compression != ZPK_COMPRESSION_NONE) {
        fprintf(stderr, "zpk: entry %u uses compression %u which this build does not support.\n",
                index, (unsigned)e.compression);
        return nullptr;
    }

    // Bounds-check the slice.
    const uint64_t payload_end = static_cast<uint64_t>(r->file_size) - ZPK_FOOTER_SIZE;
    if (e.data_offset + e.data_size > payload_end) {
        fprintf(stderr, "zpk: entry %u data extends beyond footer.\n", index);
        return nullptr;
    }
    if (e.data_size != e.uncompressed_size) {
        fprintf(stderr, "zpk: entry %u has data_size != uncompressed_size with no compression.\n",
                index);
        return nullptr;
    }

    // CRC verify. Per spec v1 this is warn-only.
    {
        uint32_t crc = zpk_crc32(0, r->file_data + e.data_offset, static_cast<size_t>(e.data_size));
        if (crc != e.data_crc32) {
            fprintf(stderr, "zpk: warning: entry %u CRC mismatch (file=%08x computed=%08x).\n",
                    index, e.data_crc32, crc);
        }
    }

    char* buf = static_cast<char*>(malloc(static_cast<size_t>(e.data_size)));
    if (!buf) {
        fprintf(stderr, "zpk: out of memory reading entry %u (%llu bytes).\n",
                index, (unsigned long long)e.data_size);
        return nullptr;
    }
    memcpy(buf, r->file_data + e.data_offset, static_cast<size_t>(e.data_size));
    *out_size = static_cast<size_t>(e.data_size);
    return buf;
}
