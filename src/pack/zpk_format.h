// ZPK — Zym Packed Bundle Format (v1)
//
// On-disk specification lives at docs/formats/zpk.md. That document is
// the source of truth; if any field, offset, value, or rule here
// disagrees with it, the doc wins and this header is wrong.
//
// Layout (lowest-to-highest offset):
//   [optional CLI stub][data region][string table][manifest entries][footer 64B]
//
// All multi-byte integers are little-endian. Structs are packed: no
// implicit compiler padding. All offsets are absolute from byte 0.

#ifndef ZYM_PACK_ZPK_FORMAT_H
#define ZYM_PACK_ZPK_FORMAT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 8-byte magic that identifies a .zpk payload. Frozen for the lifetime
// of the format; variants are expressed via `format_version`.
#define ZPK_MAGIC_BYTES { 'Z', 'Y', 'M', 'P', 'K', 0, 0, 0 }
#define ZPK_MAGIC_SIZE 8

// On-disk sizes. Exposed as macros so other code can rely on them in
// constant expressions / static asserts.
#define ZPK_FOOTER_SIZE 64
#define ZPK_ENTRY_SIZE  48

// Current writer's format version. Readers accept this value.
#define ZPK_FORMAT_VERSION 1

// Entry kinds. See docs/formats/zpk.md "Entry kinds".
//
// `kind` is descriptive: it tells consumers what an entry *is*. The
// program entry point is whichever entry the footer's `entry_index`
// points at, regardless of kind (though it must be ENTRY_BYTECODE).
typedef enum {
    ZPK_KIND_RESERVED        = 0x00, // invalid; rejected
    ZPK_KIND_ENTRY_BYTECODE  = 0x01, // .zbc, used as program entry
    ZPK_KIND_MODULE_BYTECODE = 0x02, // .zbc, importable module
    ZPK_KIND_SOURCE_MAP      = 0x03, // optional debug pairing
    ZPK_KIND_ASSET_BLOB      = 0x04, // arbitrary bytes by name
    ZPK_KIND_ASSET_TEXT      = 0x05, // UTF-8 text (hint only)
    ZPK_KIND_NATIVE_LIB      = 0x06, // reserved
    ZPK_KIND_MANIFEST_EXT    = 0x07, // reserved (TOML/JSON sidecar)
    ZPK_KIND_SIGNATURE       = 0x08, // reserved
    // 0x09..0x7E reserved for future Zym use
    ZPK_KIND_USER_MIN        = 0x7F,
    ZPK_KIND_USER_MAX        = 0xFF
} ZpkKind;

// Compression algorithm byte. Only `none` is implemented in v1.
typedef enum {
    ZPK_COMPRESSION_NONE    = 0,
    ZPK_COMPRESSION_ZSTD    = 1, // reserved
    ZPK_COMPRESSION_DEFLATE = 2  // reserved
} ZpkCompression;

// Per-entry flag bits.
#define ZPK_ENTRY_FLAG_REQUIRED (1u << 0)
#define ZPK_ENTRY_FLAG_LAZY     (1u << 1)

// Footer-level flag bits. Reserved in v1.
#define ZPK_FOOTER_FLAG_SIGNED              (1u << 0)
#define ZPK_FOOTER_FLAG_MANIFEST_COMPRESSED (1u << 1)

#pragma pack(push, 1)

// 48-byte manifest entry. See docs/formats/zpk.md "Manifest entry".
typedef struct {
    uint8_t  kind;              // ZpkKind
    uint8_t  compression;       // ZpkCompression; v1 must be 0
    uint16_t flags;             // ZPK_ENTRY_FLAG_*
    uint32_t name_offset;       // into string table
    uint32_t name_length;       // bytes; may be 0
    uint32_t reserved;          // must be 0
    uint64_t data_offset;       // absolute
    uint64_t data_size;         // on-disk size (post-compression)
    uint64_t uncompressed_size; // logical size; == data_size when uncompressed
    uint32_t data_crc32;        // CRC-32 of on-disk bytes
    uint32_t custom;            // free per kind
} ZpkEntry;

// 64-byte footer, ending exactly at EOF. See docs/formats/zpk.md "Footer".
typedef struct {
    uint8_t  magic[ZPK_MAGIC_SIZE]; // ZPK_MAGIC_BYTES
    uint16_t format_version;        // ZPK_FORMAT_VERSION
    uint16_t footer_size;           // ZPK_FOOTER_SIZE
    uint32_t flags;                 // ZPK_FOOTER_FLAG_*
    uint64_t manifest_offset;       // absolute
    uint32_t entry_count;
    uint32_t entry_index;           // < entry_count; entry must be ENTRY_BYTECODE
    uint64_t strtab_offset;         // absolute
    uint64_t strtab_size;
    uint32_t manifest_crc32;        // CRC32 over (manifest entries || strtab)
    uint32_t footer_crc32;          // CRC32 over this struct with this field zeroed
    uint8_t  reserved[8];           // must be 0
} ZpkFooter;

#pragma pack(pop)

#ifdef __cplusplus
} // extern "C"

// Compile-time guards: layout must match the spec exactly.
static_assert(sizeof(ZpkEntry)  == ZPK_ENTRY_SIZE,  "ZpkEntry must be 48 bytes");
static_assert(sizeof(ZpkFooter) == ZPK_FOOTER_SIZE, "ZpkFooter must be 64 bytes");
#endif

// CRC-32 (IEEE 802.3 polynomial 0xEDB88320), seedable so callers can
// chain multiple regions (manifest entries followed by string table).
// Pass `0` as the initial seed; the result is the final CRC.
#ifdef __cplusplus
extern "C" {
#endif

uint32_t zpk_crc32(uint32_t seed, const void* data, size_t length);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // ZYM_PACK_ZPK_FORMAT_H
