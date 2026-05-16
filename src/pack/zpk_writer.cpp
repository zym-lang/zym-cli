#include "zpk_writer.hpp"
#include "zpk_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Godot's compression facade — already linked into the binary for the
// `Buffer.compress` native, so adopting it here costs no new dependency.
#include "core/io/compression.h"

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

int zpk_write_bundle_to_memory(uint8_t** out_buf_p, size_t* out_size_p,
                               const void* stub_data, size_t stub_size,
                               const ZpkEntryInput* entries, size_t entry_count,
                               uint32_t entry_index)
{
    if (!out_buf_p || !out_size_p) {
        fprintf(stderr, "zpk_write_bundle_to_memory: out_buf/out_size is null.\n");
        return 0;
    }
    *out_buf_p = nullptr;
    *out_size_p = 0;
    if (entry_count == 0 || !entries) {
        fprintf(stderr, "zpk_write_bundle: at least one entry is required.\n");
        return 0;
    }
    // `entry_index` is either an in-range manifest index pointing at an
    // ENTRY_SOURCE/ENTRY_BYTECODE entry (runnable bundle) or the
    // `ZPK_NO_ENTRY` sentinel meaning "general archive, no program
    // entry point". At most one entry-kind entry is allowed per bundle:
    // the runtime loader dispatches on a single index, so a second one
    // would be ambiguous. Module-bytecode entries don't count — they're
    // imported, not "the entry". A bundle with zero entry-kind entries
    // is valid as a general archive and must carry the sentinel.
    if (entry_index != ZPK_NO_ENTRY && entry_index >= entry_count) {
        fprintf(stderr, "zpk_write_bundle: entry_index %u out of range (entry_count=%zu).\n",
                entry_index, entry_count);
        return 0;
    }
    {
        size_t entry_kind_count = 0;
        for (size_t i = 0; i < entry_count; i++) {
            if (entries[i].kind == ZPK_KIND_ENTRY_BYTECODE ||
                entries[i].kind == ZPK_KIND_ENTRY_SOURCE) {
                entry_kind_count++;
            }
        }
        if (entry_kind_count > 1) {
            fprintf(stderr, "zpk_write_bundle: bundle has %zu entry-kind entries; only one allowed.\n",
                    entry_kind_count);
            return 0;
        }
        if (entry_index != ZPK_NO_ENTRY) {
            const uint8_t k = entries[entry_index].kind;
            if (k != ZPK_KIND_ENTRY_BYTECODE && k != ZPK_KIND_ENTRY_SOURCE) {
                fprintf(stderr, "zpk_write_bundle: entry at index %u must have kind ENTRY_BYTECODE or ENTRY_SOURCE (or pass ZPK_NO_ENTRY for a general archive).\n",
                        entry_index);
                return 0;
            }
        }
    }

    // ----- Resolve file-backed entries by reading from disk. -------------
    //
    // Entries whose bytes live on disk (`file_path` set) are slurped
    // here, once, into per-entry buffers owned by `loaded_buffers`.
    // From that point on the rest of the writer works against the
    // in-memory pointer/size pair just like for `data`-supplied entries
    // — the on-disk format and CRC pass do not need to know whether a
    // given entry came from a script-side `Buffer` or from a file.
    //
    // Mutually exclusive: exactly one of `data` / `file_path` must be
    // set per entry. Both-set or neither-set is a hard error.
    ZpkEntryInput* effective = static_cast<ZpkEntryInput*>(malloc(sizeof(ZpkEntryInput) * entry_count));
    void** loaded_buffers = static_cast<void**>(calloc(entry_count, sizeof(void*)));
    if (!effective || !loaded_buffers) {
        fprintf(stderr, "zpk_write_bundle: out of memory.\n");
        free(effective);
        free(loaded_buffers);
        return 0;
    }
    for (size_t i = 0; i < entry_count; i++) {
        effective[i] = entries[i];
        const bool has_data   = entries[i].data != nullptr || entries[i].data_size > 0;
        const bool has_path   = entries[i].file_path != nullptr;
        const bool has_borrow = entries[i].source_reader != nullptr;
        // At most one of the three bytes-sources may be set.
        const int source_count = (has_data ? 1 : 0) + (has_path ? 1 : 0) + (has_borrow ? 1 : 0);
        if (source_count > 1) {
            fprintf(stderr, "zpk_write_bundle: entry %zu has more than one bytes-source set (data/file_path/source_reader).\n", i);
            for (size_t k = 0; k < entry_count; k++) free(loaded_buffers[k]);
            free(loaded_buffers);
            free(effective);
            return 0;
        }
        if (has_borrow) {
            // Borrow the on-disk slice straight from an open reader.
            // Validation: index in range; the slice's compression byte
            // and CRC become the new entry's; we do not recompress.
            const ZpkReader* src = entries[i].source_reader;
            const uint32_t   si  = entries[i].source_index;
            if (!src || si >= src->footer.entry_count) {
                fprintf(stderr, "zpk_write_bundle: entry %zu source_index %u out of range.\n", i, si);
                for (size_t k = 0; k < entry_count; k++) free(loaded_buffers[k]);
                free(loaded_buffers);
                free(effective);
                return 0;
            }
            const ZpkEntry& se = src->manifest[si];
            effective[i].data        = src->file_data + se.data_offset;
            effective[i].data_size   = static_cast<size_t>(se.data_size);
            effective[i].compression = se.compression; // override: source's on-disk byte wins
            effective[i].file_path   = nullptr;
            // `effective[i].source_reader` stays set as the sentinel
            // for the compression / CRC passes below.
            continue;
        }
        if (source_count == 0) {
            // Treat as a legitimate empty in-memory entry: data=null,
            // data_size=0. No file to load.
            effective[i].data = nullptr;
            effective[i].data_size = 0;
            continue;
        }
        if (has_path) {
            FILE* f = fopen(entries[i].file_path, "rb");
            if (!f) {
                fprintf(stderr, "zpk_write_bundle: could not open entry source \"%s\".\n",
                        entries[i].file_path);
                for (size_t k = 0; k < entry_count; k++) free(loaded_buffers[k]);
                free(loaded_buffers);
                free(effective);
                return 0;
            }
            if (fseek(f, 0, SEEK_END) != 0) {
                fprintf(stderr, "zpk_write_bundle: seek failed on \"%s\".\n", entries[i].file_path);
                fclose(f);
                for (size_t k = 0; k < entry_count; k++) free(loaded_buffers[k]);
                free(loaded_buffers);
                free(effective);
                return 0;
            }
            long sz = ftell(f);
            if (sz < 0) {
                fprintf(stderr, "zpk_write_bundle: tell failed on \"%s\".\n", entries[i].file_path);
                fclose(f);
                for (size_t k = 0; k < entry_count; k++) free(loaded_buffers[k]);
                free(loaded_buffers);
                free(effective);
                return 0;
            }
            rewind(f);
            void* buf = nullptr;
            if (sz > 0) {
                buf = malloc(static_cast<size_t>(sz));
                if (!buf) {
                    fprintf(stderr, "zpk_write_bundle: out of memory loading \"%s\".\n", entries[i].file_path);
                    fclose(f);
                    for (size_t k = 0; k < entry_count; k++) free(loaded_buffers[k]);
                    free(loaded_buffers);
                    free(effective);
                    return 0;
                }
                size_t got = fread(buf, 1, static_cast<size_t>(sz), f);
                if (got != static_cast<size_t>(sz)) {
                    fprintf(stderr, "zpk_write_bundle: short read from \"%s\".\n", entries[i].file_path);
                    free(buf);
                    fclose(f);
                    for (size_t k = 0; k < entry_count; k++) free(loaded_buffers[k]);
                    free(loaded_buffers);
                    free(effective);
                    return 0;
                }
            }
            fclose(f);
            loaded_buffers[i] = buf;
            effective[i].data = buf;
            effective[i].data_size = static_cast<size_t>(sz);
            effective[i].file_path = nullptr;
        }
    }
    entries = effective;

    // ----- Compression pass. ---------------------------------------------
    //
    // For every entry whose `compression` byte requests a codec, run
    // the codec on the (now in-memory) raw bytes. The original raw
    // size is captured into `uncompressed_sizes[i]` before we touch
    // `effective[i].data_size`, so the manifest's `uncompressed_size`
    // field can be filled in unconditionally further down.
    //
    // Auto-fallback policy: if the compressed output is not strictly
    // smaller than the raw input we revert this entry to
    // `ZPK_COMPRESSION_NONE` and keep the raw bytes. This matches the
    // documented `Pack.build` contract (the script never gets a bigger
    // entry just because it asked for compression).
    //
    // The compressed buffer (when used) takes over the
    // `loaded_buffers[i]` slot so it is freed alongside the
    // file-loaded buffers via the existing cleanup paths. Any prior
    // `loaded_buffers[i]` from the file-loading pass is freed first.
    uint64_t* uncompressed_sizes = static_cast<uint64_t*>(malloc(sizeof(uint64_t) * entry_count));
    if (!uncompressed_sizes) {
        fprintf(stderr, "zpk_write_bundle: out of memory.\n");
        for (size_t k = 0; k < entry_count; k++) free(loaded_buffers[k]);
        free(loaded_buffers);
        free(effective);
        return 0;
    }
    for (size_t i = 0; i < entry_count; i++) {
        // Borrowed entries arrive already in final on-disk shape; the
        // source manifest holds the authoritative uncompressed_size,
        // which we must preserve verbatim (the bytes are not going
        // through our compression pass).
        if (effective[i].source_reader != nullptr) {
            const ZpkEntry& se = effective[i].source_reader->manifest[effective[i].source_index];
            uncompressed_sizes[i] = se.uncompressed_size;
            continue;
        }

        uncompressed_sizes[i] = static_cast<uint64_t>(effective[i].data_size);

        if (effective[i].compression == ZPK_COMPRESSION_NONE) continue;
        if (effective[i].compression != ZPK_COMPRESSION_ZSTD) {
            fprintf(stderr, "zpk_write_bundle: entry %zu requests unsupported compression %u.\n",
                    i, (unsigned)effective[i].compression);
            free(uncompressed_sizes);
            for (size_t k = 0; k < entry_count; k++) free(loaded_buffers[k]);
            free(loaded_buffers);
            free(effective);
            return 0;
        }
        // Empty entry → nothing to compress; record as `none`.
        if (effective[i].data_size == 0) {
            effective[i].compression = ZPK_COMPRESSION_NONE;
            continue;
        }

        const int saved_zstd_level = Compression::zstd_level;
        if (effective[i].level > 0) {
            Compression::zstd_level = effective[i].level;
        }

        const int64_t bound = Compression::get_max_compressed_buffer_size(
            static_cast<int64_t>(effective[i].data_size), Compression::MODE_ZSTD);
        if (bound <= 0) {
            Compression::zstd_level = saved_zstd_level;
            fprintf(stderr, "zpk_write_bundle: entry %zu compression bound failed.\n", i);
            free(uncompressed_sizes);
            for (size_t k = 0; k < entry_count; k++) free(loaded_buffers[k]);
            free(loaded_buffers);
            free(effective);
            return 0;
        }
        void* cbuf = malloc(static_cast<size_t>(bound));
        if (!cbuf) {
            Compression::zstd_level = saved_zstd_level;
            fprintf(stderr, "zpk_write_bundle: out of memory compressing entry %zu.\n", i);
            free(uncompressed_sizes);
            for (size_t k = 0; k < entry_count; k++) free(loaded_buffers[k]);
            free(loaded_buffers);
            free(effective);
            return 0;
        }
        const int64_t got = Compression::compress(
            static_cast<uint8_t*>(cbuf),
            static_cast<const uint8_t*>(effective[i].data),
            static_cast<int64_t>(effective[i].data_size),
            Compression::MODE_ZSTD);
        Compression::zstd_level = saved_zstd_level;

        if (got <= 0 || static_cast<size_t>(got) >= effective[i].data_size) {
            // Either the codec failed or it didn't actually shrink the
            // entry. Drop the compressed buffer and store as `none`.
            free(cbuf);
            effective[i].compression = ZPK_COMPRESSION_NONE;
            continue;
        }

        // Success: swap in the compressed buffer. Free any prior
        // file-loaded buffer for this slot (compressed bytes
        // supersede them on disk).
        if (loaded_buffers[i]) free(loaded_buffers[i]);
        loaded_buffers[i] = cbuf;
        effective[i].data = cbuf;
        effective[i].data_size = static_cast<size_t>(got);
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
        free(uncompressed_sizes);
        for (size_t k = 0; k < entry_count; k++) free(loaded_buffers[k]);
        free(loaded_buffers);
        free(effective);
        return 0;
    }

    uint64_t cursor_off = data_region_offset;
    for (size_t i = 0; i < entry_count; i++) {
        data_offsets[i] = cursor_off;
        if (entries[i].source_reader != nullptr) {
            // Borrowed: the bytes are bit-identical to the source's
            // on-disk slice, so inherit the source's `data_crc32`
            // verbatim. (Recomputing would give the same value but
            // wastes CPU on large slices.)
            data_crcs[i] = entries[i].source_reader->manifest[entries[i].source_index].data_crc32;
        } else {
            data_crcs[i] = zpk_crc32(0, entries[i].data, entries[i].data_size);
        }
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
            free(uncompressed_sizes);
            for (size_t k = 0; k < entry_count; k++) free(loaded_buffers[k]);
            free(loaded_buffers);
            free(effective);
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
        free(uncompressed_sizes);
        for (size_t k = 0; k < entry_count; k++) free(loaded_buffers[k]);
        free(loaded_buffers);
        free(effective);
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
        free(uncompressed_sizes);
        for (size_t k = 0; k < entry_count; k++) free(loaded_buffers[k]);
        free(loaded_buffers);
        free(effective);
        return 0;
    }

    for (size_t i = 0; i < entry_count; i++) {
        ZpkEntry& e = manifest[i];
        e.kind              = entries[i].kind;
        e.compression       = entries[i].compression;
        e.flags             = entries[i].flags;
        e.name_offset       = name_offsets[i];
        e.name_length       = name_lengths[i];
        e.reserved          = 0;
        e.data_offset       = data_offsets[i];
        e.data_size         = static_cast<uint64_t>(entries[i].data_size);
        e.uncompressed_size = uncompressed_sizes[i];
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
        free(uncompressed_sizes);
        for (size_t k = 0; k < entry_count; k++) free(loaded_buffers[k]);
        free(loaded_buffers);
        free(effective);
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
    free(uncompressed_sizes);
    for (size_t k = 0; k < entry_count; k++) free(loaded_buffers[k]);
    free(loaded_buffers);
    free(effective);

    // Hand the assembled buffer to the caller. They free it.
    *out_buf_p  = out_buf;
    *out_size_p = static_cast<size_t>(total_size);
    return 1;
}

int zpk_write_bundle(const char* out_path,
                     const void* stub_data, size_t stub_size,
                     const ZpkEntryInput* entries, size_t entry_count,
                     uint32_t entry_index)
{
    if (!out_path) {
        fprintf(stderr, "zpk_write_bundle: out_path is null.\n");
        return 0;
    }
    uint8_t* out_buf = nullptr;
    size_t   out_size = 0;
    if (!zpk_write_bundle_to_memory(&out_buf, &out_size,
                                    stub_data, stub_size,
                                    entries, entry_count, entry_index)) {
        return 0;
    }
    FILE* f = fopen(out_path, "wb");
    if (!f) {
        fprintf(stderr, "zpk_write_bundle: could not create file \"%s\".\n", out_path);
        free(out_buf);
        return 0;
    }
    size_t written = fwrite(out_buf, 1, out_size, f);
    fclose(f);
    free(out_buf);

    if (written != out_size) {
        fprintf(stderr, "zpk_write_bundle: short write to \"%s\".\n", out_path);
        return 0;
    }
    return 1;
}
