#include "zpk_writer.hpp"
#include "zpk_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// CRC-32 (IEEE 802.3, polynomial 0xEDB88320). Single shared
// implementation used by both writer and reader. The table is built
// lazily once.
extern "C" uint32_t zpk_crc32(uint32_t seed, const void* data, size_t length) {
    static uint32_t table[256];
    static bool initialized = false;
    if (!initialized) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        initialized = true;
    }

    uint32_t crc = seed ^ 0xFFFFFFFFu;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < length; i++) {
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

namespace {

// Append `len` bytes from `src` to the buffer at *cursor*; advance the
// cursor. Caller has guaranteed sufficient capacity.
inline void put(uint8_t*& cursor, const void* src, size_t len) {
    if (len == 0) return;
    memcpy(cursor, src, len);
    cursor += len;
}

} // namespace

int zpk_write_bundle(const char* out_path,
                     const void* stub_data, size_t stub_size,
                     const ZpkEntryInput* entries, size_t entry_count,
                     uint32_t entry_index)
{
    if (!out_path) {
        fprintf(stderr, "zpk_write_bundle: out_path is null.\n");
        return 0;
    }
    if (entry_count == 0 || !entries) {
        fprintf(stderr, "zpk_write_bundle: at least one entry is required.\n");
        return 0;
    }
    if (entry_index >= entry_count) {
        fprintf(stderr, "zpk_write_bundle: entry_index %u out of range (entry_count=%zu).\n",
                entry_index, entry_count);
        return 0;
    }
    if (entries[entry_index].kind != ZPK_KIND_ENTRY_BYTECODE) {
        fprintf(stderr, "zpk_write_bundle: entry at index %u must have kind ENTRY_BYTECODE.\n",
                entry_index);
        return 0;
    }

    // ----- Plan the layout in memory before writing anything. ------------
    //
    // [stub][data region][string table][manifest entries][footer]
    //
    // We accumulate offsets first, then assemble the final buffer. This
    // keeps the writer simple and deterministic: same inputs -> same
    // bytes, no streaming dependency on file position.

    const uint64_t data_region_offset = static_cast<uint64_t>(stub_size);

    // Per-entry offsets within the data region.
    uint64_t* data_offsets = static_cast<uint64_t*>(malloc(sizeof(uint64_t) * entry_count));
    uint32_t* data_crcs    = static_cast<uint32_t*>(malloc(sizeof(uint32_t) * entry_count));
    if (!data_offsets || !data_crcs) {
        fprintf(stderr, "zpk_write_bundle: out of memory.\n");
        free(data_offsets);
        free(data_crcs);
        return 0;
    }

    uint64_t cursor_off = data_region_offset;
    for (size_t i = 0; i < entry_count; i++) {
        data_offsets[i] = cursor_off;
        data_crcs[i] = zpk_crc32(0, entries[i].data, entries[i].data_size);
        cursor_off += entries[i].data_size;
    }

    // Build the string table (concatenation of names, no separators or
    // NULs required by the format). We don't deduplicate; future
    // versions can without changing the on-disk shape.
    size_t strtab_capacity = 0;
    for (size_t i = 0; i < entry_count; i++) {
        if (entries[i].name && entries[i].name_length > 0) {
            strtab_capacity += entries[i].name_length;
        }
    }

    uint8_t* strtab = nullptr;
    if (strtab_capacity > 0) {
        strtab = static_cast<uint8_t*>(malloc(strtab_capacity));
        if (!strtab) {
            fprintf(stderr, "zpk_write_bundle: out of memory (strtab).\n");
            free(data_offsets);
            free(data_crcs);
            return 0;
        }
    }

    uint32_t* name_offsets = static_cast<uint32_t*>(malloc(sizeof(uint32_t) * entry_count));
    uint32_t* name_lengths = static_cast<uint32_t*>(malloc(sizeof(uint32_t) * entry_count));
    if (!name_offsets || !name_lengths) {
        fprintf(stderr, "zpk_write_bundle: out of memory (name tables).\n");
        free(data_offsets);
        free(data_crcs);
        free(strtab);
        free(name_offsets);
        free(name_lengths);
        return 0;
    }

    {
        size_t st_cur = 0;
        for (size_t i = 0; i < entry_count; i++) {
            if (entries[i].name && entries[i].name_length > 0) {
                name_offsets[i] = static_cast<uint32_t>(st_cur);
                name_lengths[i] = static_cast<uint32_t>(entries[i].name_length);
                memcpy(strtab + st_cur, entries[i].name, entries[i].name_length);
                st_cur += entries[i].name_length;
            } else {
                name_offsets[i] = 0;
                name_lengths[i] = 0;
            }
        }
    }

    const uint64_t strtab_offset   = cursor_off;
    const uint64_t strtab_size     = static_cast<uint64_t>(strtab_capacity);
    const uint64_t manifest_offset = strtab_offset + strtab_size;
    const uint64_t manifest_size   = static_cast<uint64_t>(entry_count) * ZPK_ENTRY_SIZE;
    const uint64_t footer_offset   = manifest_offset + manifest_size;
    const uint64_t total_size      = footer_offset + ZPK_FOOTER_SIZE;

    // ----- Build the manifest entries. -----------------------------------
    ZpkEntry* manifest = static_cast<ZpkEntry*>(calloc(entry_count, sizeof(ZpkEntry)));
    if (!manifest) {
        fprintf(stderr, "zpk_write_bundle: out of memory (manifest).\n");
        free(data_offsets);
        free(data_crcs);
        free(strtab);
        free(name_offsets);
        free(name_lengths);
        return 0;
    }

    for (size_t i = 0; i < entry_count; i++) {
        ZpkEntry& e = manifest[i];
        e.kind              = entries[i].kind;
        e.compression       = ZPK_COMPRESSION_NONE;
        e.flags             = entries[i].flags;
        e.name_offset       = name_offsets[i];
        e.name_length       = name_lengths[i];
        e.reserved          = 0;
        e.data_offset       = data_offsets[i];
        e.data_size         = static_cast<uint64_t>(entries[i].data_size);
        e.uncompressed_size = static_cast<uint64_t>(entries[i].data_size);
        e.data_crc32        = data_crcs[i];
        e.custom            = entries[i].custom;
    }

    // ----- Compute manifest CRC over (manifest entries || strtab). -------
    uint32_t manifest_crc = zpk_crc32(0, manifest, static_cast<size_t>(manifest_size));
    if (strtab_size > 0) {
        manifest_crc = zpk_crc32(manifest_crc, strtab, static_cast<size_t>(strtab_size));
    }

    // ----- Build the footer. ---------------------------------------------
    ZpkFooter footer;
    memset(&footer, 0, sizeof(footer));
    static const uint8_t magic_bytes[ZPK_MAGIC_SIZE] = ZPK_MAGIC_BYTES;
    memcpy(footer.magic, magic_bytes, ZPK_MAGIC_SIZE);
    footer.format_version  = ZPK_FORMAT_VERSION;
    footer.footer_size     = ZPK_FOOTER_SIZE;
    footer.flags           = 0;
    footer.manifest_offset = manifest_offset;
    footer.entry_count     = static_cast<uint32_t>(entry_count);
    footer.entry_index     = entry_index;
    footer.strtab_offset   = strtab_offset;
    footer.strtab_size     = strtab_size;
    footer.manifest_crc32  = manifest_crc;
    footer.footer_crc32    = 0; // computed below

    // CRC of the footer with `footer_crc32` zeroed (already zero).
    footer.footer_crc32 = zpk_crc32(0, &footer, sizeof(footer));

    // ----- Assemble and write. -------------------------------------------
    uint8_t* out_buf = static_cast<uint8_t*>(malloc(static_cast<size_t>(total_size)));
    if (!out_buf) {
        fprintf(stderr, "zpk_write_bundle: out of memory (output buffer, %zu bytes).\n",
                static_cast<size_t>(total_size));
        free(data_offsets);
        free(data_crcs);
        free(strtab);
        free(name_offsets);
        free(name_lengths);
        free(manifest);
        return 0;
    }

    uint8_t* cur = out_buf;
    if (stub_size > 0) put(cur, stub_data, stub_size);
    for (size_t i = 0; i < entry_count; i++) {
        if (entries[i].data_size > 0) {
            put(cur, entries[i].data, entries[i].data_size);
        }
    }
    if (strtab_size > 0) put(cur, strtab, static_cast<size_t>(strtab_size));
    put(cur, manifest, static_cast<size_t>(manifest_size));
    put(cur, &footer, sizeof(footer));

    free(data_offsets);
    free(data_crcs);
    free(strtab);
    free(name_offsets);
    free(name_lengths);
    free(manifest);

    FILE* f = fopen(out_path, "wb");
    if (!f) {
        fprintf(stderr, "zpk_write_bundle: could not create file \"%s\".\n", out_path);
        free(out_buf);
        return 0;
    }
    size_t written = fwrite(out_buf, 1, static_cast<size_t>(total_size), f);
    fclose(f);
    free(out_buf);

    if (written != total_size) {
        fprintf(stderr, "zpk_write_bundle: short write to \"%s\".\n", out_path);
        return 0;
    }
    return 1;
}
