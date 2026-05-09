// `Pack` native — script-side surface for assembling ZPK bundles.
//
// Catalog status: `Pack` is a regular grantable catalog entry (see
// src/natives/cli_catalog.{hpp,cpp}). The root VM has it because the
// root receives the full catalog; child VMs created via `Zym.newVM`
// only have it when their parent explicitly grants it via
// `registerCliNative("Pack")`. A child without the grant simply has no
// `Pack` global, identical to the policy used by `File`, `Process`,
// `AES`, and every other grantable native.
//
// Surface (statics):
//   Pack.build(spec) -> bool
//
// `spec` is a map describing the whole bundle in one call; the writer
// (`src/pack/zpk_writer`) is itself batch-shaped, so layering a
// streaming builder on top would only re-buffer the same data on the
// script side. See `docs/cli/pack.md` for the spec / entry shape and
// the kind-string vocabulary.
//
// Memory: per-entry bytes may come from an in-memory `Buffer` (`data`)
// OR from an absolute/relative file path (`path`); the writer streams
// `path`-supplied entries from disk, so scripts that pack large assets
// never have to materialize them as a script-side `Buffer` first.

#include "natives.hpp"
#include "../pack/zpk_writer.hpp"
#include "../pack/zpk_reader.hpp"
#include "../pack/zpk_format.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Provided by buffer.cpp — type-clean Buffer reader / builder so we
// don't have to drag Godot's `PackedByteArray` header into this
// translation unit.
extern bool readBufferBytes(ZymVM* vm, ZymValue v, const char** out_data, size_t* out_size);
// (declared in natives.hpp) ZymValue makeBufferFromBytes(ZymVM*, const char*, size_t);

namespace {

// ---- helpers --------------------------------------------------------------

// Read a binary file fully into a malloc'd buffer. Returns nullptr on
// failure (and does not touch *out_size). Mirrors the helper used by
// `full_executor.cpp` for stub loading; duplicated here rather than
// exposed because it's a 20-line utility and the executor's copy is
// `static` for a reason (different error-reporting style).
char* slurp_binary(const char* path, size_t* out_size) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return nullptr;
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return nullptr; }
    long sz = std::ftell(f);
    if (sz < 0) { std::fclose(f); return nullptr; }
    std::rewind(f);
    char* buf = static_cast<char*>(std::malloc(sz > 0 ? static_cast<size_t>(sz) : 1));
    if (!buf) { std::fclose(f); return nullptr; }
    size_t got = sz > 0 ? std::fread(buf, 1, static_cast<size_t>(sz), f) : 0;
    std::fclose(f);
    if (got != static_cast<size_t>(sz)) { std::free(buf); return nullptr; }
    *out_size = static_cast<size_t>(sz);
    return buf;
}

bool ends_with(const char* s, const char* suffix) {
    if (!s || !suffix) return false;
    size_t ls = std::strlen(s);
    size_t lf = std::strlen(suffix);
    if (ls < lf) return false;
    return std::strcmp(s + ls - lf, suffix) == 0;
}

// Map a script-facing kind string to the on-disk `ZpkKind` byte.
// Strings are used (rather than numeric constants) per the
// status-string convention in `docs/cli/conventions.md`.
bool kind_from_string(const char* s, uint8_t* out) {
    if (!s || !*s) return false;
    if (std::strcmp(s, "entry_bytecode")  == 0) { *out = ZPK_KIND_ENTRY_BYTECODE;  return true; }
    if (std::strcmp(s, "entry_source")    == 0) { *out = ZPK_KIND_ENTRY_SOURCE;    return true; }
    if (std::strcmp(s, "module_bytecode") == 0) { *out = ZPK_KIND_MODULE_BYTECODE; return true; }
    if (std::strcmp(s, "source_map")      == 0) { *out = ZPK_KIND_SOURCE_MAP;      return true; }
    if (std::strcmp(s, "asset")           == 0) { *out = ZPK_KIND_ASSET_BLOB;      return true; }
    if (std::strcmp(s, "asset_blob")      == 0) { *out = ZPK_KIND_ASSET_BLOB;      return true; }
    if (std::strcmp(s, "asset_text")      == 0) { *out = ZPK_KIND_ASSET_TEXT;      return true; }
    return false;
}

// Optional-string field: returns nullptr if the key is missing / null /
// not a string. The caller uses presence to decide whether the field
// is set.
const char* opt_string(ZymVM* vm, ZymValue map, const char* key) {
    ZymValue v = zym_mapGet(vm, map, key);
    if (v == ZYM_ERROR) return nullptr;
    if (zym_isNull(v)) return nullptr;
    if (!zym_isString(v)) return nullptr;
    return zym_asCString(v);
}

// Optional-number field: returns `dflt` if the key is missing / null /
// not a number. We don't error on type mismatch here; a separate type
// check is the caller's responsibility for fields where the user
// clearly meant to set them.
double opt_number(ZymVM* vm, ZymValue map, const char* key, double dflt) {
    ZymValue v = zym_mapGet(vm, map, key);
    if (v == ZYM_ERROR) return dflt;
    if (zym_isNull(v)) return dflt;
    if (!zym_isNumber(v)) return dflt;
    return zym_asNumber(v);
}

// ---- the build call -------------------------------------------------------

ZymValue f_build(ZymVM* vm, ZymValue /*self*/, ZymValue specV) {
    if (!zym_isMap(specV)) {
        zym_runtimeError(vm, "Pack.build(spec) expects a map");
        return ZYM_ERROR;
    }

    // ----- output path -----
    ZymValue outputV = zym_mapGet(vm, specV, "output");
    if (outputV == ZYM_ERROR || zym_isNull(outputV) || !zym_isString(outputV)) {
        zym_runtimeError(vm, "Pack.build(spec) expects spec.output to be a string");
        return ZYM_ERROR;
    }
    const char* output_path = zym_asCString(outputV);
    if (!output_path || !*output_path) {
        zym_runtimeError(vm, "Pack.build(spec): spec.output must not be empty");
        return ZYM_ERROR;
    }

    // ----- entries -----
    ZymValue entriesV = zym_mapGet(vm, specV, "entries");
    if (entriesV == ZYM_ERROR || zym_isNull(entriesV) || !zym_isList(entriesV)) {
        zym_runtimeError(vm, "Pack.build(spec) expects spec.entries to be a list");
        return ZYM_ERROR;
    }
    int n = zym_listLength(entriesV);
    if (n <= 0) {
        zym_runtimeError(vm, "Pack.build(spec): spec.entries must contain at least one entry");
        return ZYM_ERROR;
    }

    // ----- entryIndex (optional, default 0) -----
    uint32_t entry_index = 0;
    {
        ZymValue eiv = zym_mapGet(vm, specV, "entryIndex");
        if (eiv != ZYM_ERROR && !zym_isNull(eiv)) {
            if (!zym_isNumber(eiv)) {
                zym_runtimeError(vm, "Pack.build(spec): spec.entryIndex must be a number");
                return ZYM_ERROR;
            }
            double d = zym_asNumber(eiv);
            if (d < 0 || d >= (double)n) {
                zym_runtimeError(vm, "Pack.build(spec): spec.entryIndex out of range");
                return ZYM_ERROR;
            }
            entry_index = (uint32_t)d;
        }
    }

    // ----- compression / level (bundle-wide defaults; per-entry wins) -----
    //
    // Resolution rule: when `spec.compression == true`, every entry
    // defaults to compressed; an entry sets `compression: false` to
    // opt out. When `spec.compression` is omitted or false, every
    // entry defaults to uncompressed; an entry sets
    // `compression: true` to opt in. Per-entry `compression` always
    // wins over the bundle default. `level` mirrors the same
    // shape — entry's `level` overrides the bundle's, which defaults
    // to 3 (zstd's own default, also `Buffer.compress("zstd")`'s).
    bool bundle_compress = false;
    {
        ZymValue cv = zym_mapGet(vm, specV, "compression");
        if (cv != ZYM_ERROR && !zym_isNull(cv)) {
            if (!zym_isBool(cv)) {
                zym_runtimeError(vm, "Pack.build(spec): spec.compression must be a bool");
                return ZYM_ERROR;
            }
            bundle_compress = zym_asBool(cv);
        }
    }
    int bundle_level = 3;
    {
        ZymValue lv = zym_mapGet(vm, specV, "level");
        if (lv != ZYM_ERROR && !zym_isNull(lv)) {
            if (!zym_isNumber(lv)) {
                zym_runtimeError(vm, "Pack.build(spec): spec.level must be a number");
                return ZYM_ERROR;
            }
            double d = zym_asNumber(lv);
            if (d < 1 || d > 22) {
                zym_runtimeError(vm, "Pack.build(spec): spec.level must be in 1..22");
                return ZYM_ERROR;
            }
            bundle_level = (int)d;
        }
    }

    // ----- materialize ZpkEntryInput[] -----
    //
    // We keep the names alive via a side vector of std::string so the
    // `name` pointers we hand to the writer remain valid for the
    // duration of the call. `path` strings are owned by the script's
    // map, which is rooted on the call stack while we're inside
    // `f_build`, so we can safely borrow their pointers without
    // copying.
    std::vector<ZpkEntryInput> infos(n);
    std::vector<std::string>   name_storage(n);
    std::memset(infos.data(), 0, sizeof(ZpkEntryInput) * n);

    for (int i = 0; i < n; i++) {
        ZymValue e = zym_listGet(vm, entriesV, i);
        if (e == ZYM_ERROR || !zym_isMap(e)) {
            zym_runtimeError(vm, "Pack.build(spec): spec.entries[%d] must be a map", i);
            return ZYM_ERROR;
        }

        // kind (required)
        ZymValue kindV = zym_mapGet(vm, e, "kind");
        if (kindV == ZYM_ERROR || zym_isNull(kindV) || !zym_isString(kindV)) {
            zym_runtimeError(vm, "Pack.build(spec): spec.entries[%d].kind must be a string", i);
            return ZYM_ERROR;
        }
        uint8_t kind_byte = 0;
        const char* kind_str = zym_asCString(kindV);
        if (!kind_from_string(kind_str, &kind_byte)) {
            zym_runtimeError(vm,
                "Pack.build(spec): spec.entries[%d].kind '%s' is not a known kind "
                "(expected 'entry_bytecode', 'entry_source', 'module_bytecode', 'source_map', 'asset', or 'asset_text')",
                i, kind_str);
            return ZYM_ERROR;
        }

        // name (optional)
        const char* name = opt_string(vm, e, "name");
        if (name) {
            name_storage[i].assign(name);
            infos[i].name = name_storage[i].data();
            infos[i].name_length = name_storage[i].size();
        }

        // flags / custom (optional)
        infos[i].kind   = kind_byte;
        infos[i].flags  = (uint16_t)opt_number(vm, e, "flags",  0.0);
        infos[i].custom = (uint32_t)opt_number(vm, e, "custom", 0.0);

        // compression (optional bool, overrides bundle default).
        bool entry_compress = bundle_compress;
        {
            ZymValue ecv = zym_mapGet(vm, e, "compression");
            if (ecv != ZYM_ERROR && !zym_isNull(ecv)) {
                if (!zym_isBool(ecv)) {
                    zym_runtimeError(vm,
                        "Pack.build(spec): spec.entries[%d].compression must be a bool", i);
                    return ZYM_ERROR;
                }
                entry_compress = zym_asBool(ecv);
            }
        }
        int entry_level = bundle_level;
        {
            ZymValue elv = zym_mapGet(vm, e, "level");
            if (elv != ZYM_ERROR && !zym_isNull(elv)) {
                if (!zym_isNumber(elv)) {
                    zym_runtimeError(vm,
                        "Pack.build(spec): spec.entries[%d].level must be a number", i);
                    return ZYM_ERROR;
                }
                double d = zym_asNumber(elv);
                if (d < 1 || d > 22) {
                    zym_runtimeError(vm,
                        "Pack.build(spec): spec.entries[%d].level must be in 1..22", i);
                    return ZYM_ERROR;
                }
                entry_level = (int)d;
            }
        }
        infos[i].compression = entry_compress ? ZPK_COMPRESSION_ZSTD : ZPK_COMPRESSION_NONE;
        infos[i].level       = entry_compress ? entry_level : 0;

        // bytes source: exactly one of `data` (Buffer) or `path` (string).
        ZymValue dataV = zym_mapGet(vm, e, "data");
        ZymValue pathV = zym_mapGet(vm, e, "path");
        const bool data_present = (dataV != ZYM_ERROR) && !zym_isNull(dataV);
        const bool path_present = (pathV != ZYM_ERROR) && !zym_isNull(pathV);

        if (data_present && path_present) {
            zym_runtimeError(vm,
                "Pack.build(spec): spec.entries[%d] sets both `data` and `path`; pick exactly one", i);
            return ZYM_ERROR;
        }
        if (!data_present && !path_present) {
            zym_runtimeError(vm,
                "Pack.build(spec): spec.entries[%d] needs exactly one of `data` (Buffer) or `path` (string)", i);
            return ZYM_ERROR;
        }

        if (data_present) {
            const char* bytes = nullptr;
            size_t      sz    = 0;
            if (!readBufferBytes(vm, dataV, &bytes, &sz)) {
                zym_runtimeError(vm,
                    "Pack.build(spec): spec.entries[%d].data must be a Buffer", i);
                return ZYM_ERROR;
            }
            infos[i].data      = bytes;
            infos[i].data_size = sz;
            infos[i].file_path = nullptr;
        } else {
            if (!zym_isString(pathV)) {
                zym_runtimeError(vm,
                    "Pack.build(spec): spec.entries[%d].path must be a string", i);
                return ZYM_ERROR;
            }
            infos[i].data      = nullptr;
            infos[i].data_size = 0;
            infos[i].file_path = zym_asCString(pathV);
        }
    }

    // ----- enforce single entry-kind entry per bundle -----
    //
    // The runtime loader picks the program entry by the footer's
    // `entry_index` and dispatches on its kind. A bundle with more
    // than one entry-kind entry would be ambiguous, and `entryIndex`
    // must point at one of them. Module-bytecode entries are not
    // counted — they're imported, not "the entry".
    {
        int entry_kind_count = 0;
        for (int i = 0; i < n; i++) {
            if (infos[i].kind == ZPK_KIND_ENTRY_BYTECODE ||
                infos[i].kind == ZPK_KIND_ENTRY_SOURCE) {
                entry_kind_count++;
            }
        }
        if (entry_kind_count > 1) {
            zym_runtimeError(vm,
                "Pack.build(spec): bundle has %d entry-kind entries (entry_bytecode/entry_source); "
                "only one is allowed", entry_kind_count);
            return ZYM_ERROR;
        }
        const uint8_t ek = infos[entry_index].kind;
        if (ek != ZPK_KIND_ENTRY_BYTECODE && ek != ZPK_KIND_ENTRY_SOURCE) {
            zym_runtimeError(vm,
                "Pack.build(spec): spec.entries[entryIndex=%u].kind must be "
                "'entry_bytecode' or 'entry_source'", (unsigned)entry_index);
            return ZYM_ERROR;
        }
    }

    // ----- stub (optional; ignored when output ends in .zpk) -----
    char*  stub_data = nullptr;
    size_t stub_size = 0;
    const bool headless = ends_with(output_path, ".zpk");
    if (!headless) {
        const char* stub_path = opt_string(vm, specV, "stub");
        if (stub_path && *stub_path) {
            stub_data = slurp_binary(stub_path, &stub_size);
            if (!stub_data) {
                std::fprintf(stderr,
                    "Pack.build: could not read stub file \"%s\".\n", stub_path);
                return zym_newBool(false);
            }
        }
    }

    int ok = zpk_write_bundle(output_path,
                              stub_data, stub_size,
                              infos.data(), (size_t)n,
                              entry_index);
    if (stub_data) std::free(stub_data);

    return zym_newBool(ok != 0);
}

// ===========================================================================
// READ API
// ===========================================================================
//
// Two surfaces share the same shape:
//
//   * Self-bundle: `Pack.hasSelf/entryName/list/has/open/openIndex/info/
//     closeSelf`. Backed by a process-wide lazily-initialised `ZpkReader`.
//     `Pack.closeSelf()` releases the reader; subsequent self-method calls
//     simply re-open it on demand if the running exe still has a payload.
//
//   * Arbitrary bundles: `Pack.openFile(path) -> bundle | null`. Each
//     handle owns its own `ZpkReader`, cached for the handle's lifetime
//     and freed by `bundle.close()` (or by the GC finalizer as a safety
//     net). After close, every method on the handle returns `null` /
//     `false`.

// ---- kind / compression vocabularies --------------------------------------

const char* kind_to_string(uint8_t k, char* user_buf /*>=16 bytes*/) {
    switch (k) {
        case ZPK_KIND_RESERVED:        return "reserved";
        case ZPK_KIND_ENTRY_BYTECODE:  return "entry_bytecode";
        case ZPK_KIND_MODULE_BYTECODE: return "module_bytecode";
        case ZPK_KIND_SOURCE_MAP:      return "source_map";
        case ZPK_KIND_ASSET_BLOB:      return "asset_blob";
        case ZPK_KIND_ASSET_TEXT:      return "asset_text";
        case ZPK_KIND_NATIVE_LIB:      return "native_lib";
        case ZPK_KIND_MANIFEST_EXT:    return "manifest_ext";
        case ZPK_KIND_SIGNATURE:       return "signature";
        case ZPK_KIND_ENTRY_SOURCE:    return "entry_source";
        default:
            // 0x0A..0x7E reserved; 0x7F..0xFF user range.
            if (k >= ZPK_KIND_USER_MIN) {
                std::snprintf(user_buf, 16, "user:0x%02X", (unsigned)k);
                return user_buf;
            }
            std::snprintf(user_buf, 16, "reserved:0x%02X", (unsigned)k);
            return user_buf;
    }
}

const char* compression_to_string(uint8_t c) {
    switch (c) {
        case ZPK_COMPRESSION_NONE:    return "none";
        case ZPK_COMPRESSION_ZSTD:    return "zstd";
        case ZPK_COMPRESSION_DEFLATE: return "deflate";
        default:                      return "unknown";
    }
}

// Build a script-facing `entryInfo` map from a manifest entry. Carries
// every `ZpkEntry` field — even the ones unused in v1 — so scripts have
// full introspection moving forward.
ZymValue make_entry_info(ZymVM* vm, const ZpkReader* r, uint32_t index) {
    const ZpkEntry& e = r->manifest[index];
    char user_buf[16];

    ZymValue m = zym_newMap(vm);
    zym_pushRoot(vm, m);

    zym_mapSet(vm, m, "index",            zym_newNumber((double)index));

    // name: empty string when the entry has no logical name.
    if (e.name_length > 0 && r->strtab) {
        zym_mapSet(vm, m, "name",
            zym_newStringN(vm, (const char*)(r->strtab + e.name_offset), (int)e.name_length));
    } else {
        zym_mapSet(vm, m, "name", zym_newString(vm, ""));
    }

    zym_mapSet(vm, m, "kind",             zym_newString(vm, kind_to_string(e.kind, user_buf)));
    zym_mapSet(vm, m, "kindByte",         zym_newNumber((double)e.kind));
    zym_mapSet(vm, m, "compression",      zym_newString(vm, compression_to_string(e.compression)));
    zym_mapSet(vm, m, "compressionByte",  zym_newNumber((double)e.compression));
    zym_mapSet(vm, m, "flags",            zym_newNumber((double)e.flags));
    zym_mapSet(vm, m, "required",         zym_newBool((e.flags & ZPK_ENTRY_FLAG_REQUIRED) != 0));
    zym_mapSet(vm, m, "lazy",             zym_newBool((e.flags & ZPK_ENTRY_FLAG_LAZY) != 0));
    zym_mapSet(vm, m, "nameOffset",       zym_newNumber((double)e.name_offset));
    zym_mapSet(vm, m, "nameLength",       zym_newNumber((double)e.name_length));
    zym_mapSet(vm, m, "reserved",         zym_newNumber((double)e.reserved));
    zym_mapSet(vm, m, "dataOffset",       zym_newNumber((double)e.data_offset));
    zym_mapSet(vm, m, "dataSize",         zym_newNumber((double)e.data_size));
    zym_mapSet(vm, m, "uncompressedSize", zym_newNumber((double)e.uncompressed_size));
    zym_mapSet(vm, m, "size",             zym_newNumber((double)e.uncompressed_size));
    zym_mapSet(vm, m, "dataCrc32",        zym_newNumber((double)e.data_crc32));
    zym_mapSet(vm, m, "custom",           zym_newNumber((double)e.custom));
    zym_mapSet(vm, m, "isEntry",          zym_newBool(index == r->footer.entry_index));

    zym_popRoot(vm);
    return m;
}

// Linear-scan lookup by name. `name` is NUL-terminated; manifest names
// are not. We compare on (length, bytes). Returns -1 if not found.
int find_entry_by_name(const ZpkReader* r, const char* name) {
    if (!r || !name) return -1;
    const size_t name_len = std::strlen(name);
    const uint32_t n = r->footer.entry_count;
    for (uint32_t i = 0; i < n; i++) {
        const ZpkEntry& e = r->manifest[i];
        if (e.name_length != name_len) continue;
        if (name_len == 0) return (int)i;
        if (!r->strtab) continue;
        if (std::memcmp(r->strtab + e.name_offset, name, name_len) == 0) return (int)i;
    }
    return -1;
}

// ---- CRC verification ----------------------------------------------------
//
// All three CRCs (footer, manifest+strtab, per-entry data) are
// computed locally using `zpk_crc32` over the in-memory `file_data`
// the reader holds. The reader rejects any open with a bad footer
// CRC (so an opened reader always has `footer.ok == true`); manifest
// and per-entry CRCs are warn-only at open time, which is exactly
// why this surface exists — scripts can decide what to do on
// mismatch instead of relying on the stderr warning.

uint32_t compute_footer_crc(const ZpkReader* r) {
    ZpkFooter tmp = r->footer;
    tmp.footer_crc32 = 0;
    return zpk_crc32(0, &tmp, sizeof(tmp));
}

uint32_t compute_manifest_crc(const ZpkReader* r) {
    const size_t mfs = static_cast<size_t>(r->footer.entry_count) * ZPK_ENTRY_SIZE;
    uint32_t crc = zpk_crc32(0, r->manifest, mfs);
    if (r->footer.strtab_size > 0 && r->strtab) {
        crc = zpk_crc32(crc, r->strtab, static_cast<size_t>(r->footer.strtab_size));
    }
    return crc;
}

// Compute the data CRC for entry `index` over its on-disk bytes.
// Returns false (with `*out_crc` left untouched) if the entry's
// `data_offset` / `data_size` falls outside the loaded `file_data`.
bool compute_entry_crc(const ZpkReader* r, uint32_t index, uint32_t* out_crc) {
    const ZpkEntry& e = r->manifest[index];
    if (e.data_offset > r->file_size) return false;
    if (e.data_offset + e.data_size > r->file_size) return false;
    *out_crc = zpk_crc32(0, r->file_data + e.data_offset, (size_t)e.data_size);
    return true;
}

// Quick per-entry verify used by the bool-returning one-shot form.
bool entry_crc_ok(const ZpkReader* r, uint32_t index) {
    uint32_t computed = 0;
    if (!compute_entry_crc(r, index, &computed)) return false;
    return computed == r->manifest[index].data_crc32;
}

// Build the full structured report. Shape:
//   {
//     ok:       <bool>,
//     footer:   { ok, expected, computed },
//     manifest: { ok, expected, computed },
//     entries:  [ { index, name, ok, expected, computed, readable }, ... ]
//   }
// `readable` is false only when an entry's `data_offset`/`data_size`
// is bounds-busted (a corrupt manifest); in that case `computed` is
// surfaced as 0 and `ok` is false.
ZymValue build_verify_report(ZymVM* vm, const ZpkReader* r) {
    ZymValue report = zym_newMap(vm);
    zym_pushRoot(vm, report);

    // Footer.
    const uint32_t footer_expected = r->footer.footer_crc32;
    const uint32_t footer_computed = compute_footer_crc(r);
    const bool footer_ok = (footer_expected == footer_computed);
    {
        ZymValue m = zym_newMap(vm);
        zym_pushRoot(vm, m);
        zym_mapSet(vm, m, "ok",       zym_newBool(footer_ok));
        zym_mapSet(vm, m, "expected", zym_newNumber((double)footer_expected));
        zym_mapSet(vm, m, "computed", zym_newNumber((double)footer_computed));
        zym_mapSet(vm, report, "footer", m);
        zym_popRoot(vm);
    }

    // Manifest+strtab.
    const uint32_t manifest_expected = r->footer.manifest_crc32;
    const uint32_t manifest_computed = compute_manifest_crc(r);
    const bool manifest_ok = (manifest_expected == manifest_computed);
    {
        ZymValue m = zym_newMap(vm);
        zym_pushRoot(vm, m);
        zym_mapSet(vm, m, "ok",       zym_newBool(manifest_ok));
        zym_mapSet(vm, m, "expected", zym_newNumber((double)manifest_expected));
        zym_mapSet(vm, m, "computed", zym_newNumber((double)manifest_computed));
        zym_mapSet(vm, report, "manifest", m);
        zym_popRoot(vm);
    }

    // Per-entry data CRCs.
    bool all_entries_ok = true;
    ZymValue entries = zym_newList(vm);
    zym_pushRoot(vm, entries);
    for (uint32_t i = 0; i < r->footer.entry_count; i++) {
        const ZpkEntry& e = r->manifest[i];
        uint32_t computed = 0;
        const bool readable = compute_entry_crc(r, i, &computed);
        const bool ok = readable && (computed == e.data_crc32);
        if (!ok) all_entries_ok = false;

        ZymValue m = zym_newMap(vm);
        zym_pushRoot(vm, m);
        zym_mapSet(vm, m, "index",    zym_newNumber((double)i));
        if (e.name_length > 0 && r->strtab) {
            zym_mapSet(vm, m, "name",
                zym_newStringN(vm, (const char*)(r->strtab + e.name_offset), (int)e.name_length));
        } else {
            zym_mapSet(vm, m, "name", zym_newString(vm, ""));
        }
        zym_mapSet(vm, m, "ok",       zym_newBool(ok));
        zym_mapSet(vm, m, "expected", zym_newNumber((double)e.data_crc32));
        zym_mapSet(vm, m, "computed", zym_newNumber((double)computed));
        zym_mapSet(vm, m, "readable", zym_newBool(readable));
        zym_listAppend(vm, entries, m);
        zym_popRoot(vm);
    }
    zym_mapSet(vm, report, "entries", entries);
    zym_popRoot(vm);

    zym_mapSet(vm, report, "ok",
        zym_newBool(footer_ok && manifest_ok && all_entries_ok));

    zym_popRoot(vm);
    return report;
}

// Resolve a one-arg verify(arg) parameter (string name or numeric
// index) to a manifest index. Returns -1 on out-of-range / not-found
// (callers translate that to `false`); raises a runtime error and
// returns -2 on a wrong-type argument.
int resolve_entry_arg(ZymVM* vm, const ZpkReader* r, ZymValue arg, const char* fn_name) {
    if (zym_isNumber(arg)) {
        double d = zym_asNumber(arg);
        if (d < 0 || d >= (double)r->footer.entry_count) return -1;
        return (int)d;
    }
    if (zym_isString(arg)) {
        return find_entry_by_name(r, zym_asCString(arg));
    }
    zym_runtimeError(vm, "%s expects a string entry name or a numeric index", fn_name);
    return -2;
}

// Read entry bytes by index, return as Buffer (or null on failure).
ZymValue read_entry_as_buffer(ZymVM* vm, const ZpkReader* r, uint32_t index) {
    size_t sz = 0;
    char* bytes = zpk_reader_read_entry(r, index, &sz);
    if (!bytes) return zym_newNull();
    ZymValue buf = makeBufferFromBytes(vm, bytes, sz);
    std::free(bytes);
    return buf;
}

// ---- self-bundle reader (lazy, process-wide) ------------------------------

ZpkReader g_self_reader{};
bool      g_self_open = false;
bool      g_self_tried = false;
bool      g_self_has_payload = false;

// Returns a pointer to the cached self reader, or nullptr if the running
// executable has no payload. Opens lazily on first call and after any
// `Pack.closeSelf()`.
const ZpkReader* self_reader_get() {
    if (g_self_open) return &g_self_reader;
    // Cheap probe first; avoids re-opening when we already know there's
    // nothing to read. The probe is re-evaluated after closeSelf, which
    // resets `g_self_tried` so a script that closes-then-reopens still
    // works (the running exe doesn't change underneath us, but the
    // contract is "lazy reopen").
    if (!g_self_tried) {
        g_self_has_payload = (zpk_reader_self_exe_has_payload() != 0);
        g_self_tried = true;
    }
    if (!g_self_has_payload) return nullptr;
    if (zpk_reader_open_self_exe(&g_self_reader) != 1) {
        // Probe lied or open failed; mark as unavailable for this run
        // until closeSelf() resets the cache.
        g_self_has_payload = false;
        return nullptr;
    }
    g_self_open = true;
    return &g_self_reader;
}

void self_reader_close() {
    if (g_self_open) {
        zpk_reader_close(&g_self_reader);
        g_self_open = false;
    }
    g_self_tried = false;        // allow re-probe
    g_self_has_payload = false;
}

// ---- self-bundle method implementations -----------------------------------

ZymValue f_hasSelf(ZymVM* /*vm*/, ZymValue) {
    // Don't open the reader for a yes/no question.
    return zym_newBool(zpk_reader_self_exe_has_payload() != 0);
}

ZymValue f_self_entryName(ZymVM* vm, ZymValue) {
    const ZpkReader* r = self_reader_get();
    if (!r) return zym_newNull();
    const ZpkEntry& e = r->manifest[r->footer.entry_index];
    if (e.name_length == 0 || !r->strtab) return zym_newString(vm, "");
    return zym_newStringN(vm, (const char*)(r->strtab + e.name_offset), (int)e.name_length);
}

ZymValue f_self_list(ZymVM* vm, ZymValue) {
    const ZpkReader* r = self_reader_get();
    if (!r) return zym_newNull();
    ZymValue list = zym_newList(vm);
    zym_pushRoot(vm, list);
    for (uint32_t i = 0; i < r->footer.entry_count; i++) {
        zym_listAppend(vm, list, make_entry_info(vm, r, i));
    }
    zym_popRoot(vm);
    return list;
}

ZymValue f_self_has(ZymVM* vm, ZymValue, ZymValue nameV) {
    if (!zym_isString(nameV)) {
        zym_runtimeError(vm, "Pack.has(name) expects a string");
        return ZYM_ERROR;
    }
    const ZpkReader* r = self_reader_get();
    if (!r) return zym_newBool(false);
    return zym_newBool(find_entry_by_name(r, zym_asCString(nameV)) >= 0);
}

// Pack.open(arg) — string entry name or numeric manifest index.
// Names with multiple matching entries resolve to the first hit
// (use the numeric form to disambiguate). Returns null when there's
// no self bundle, or the name/index doesn't resolve.
ZymValue f_self_open(ZymVM* vm, ZymValue, ZymValue arg) {
    const ZpkReader* r = self_reader_get();
    if (!r) return zym_newNull();
    int idx = resolve_entry_arg(vm, r, arg, "Pack.open(arg)");
    if (idx == -2) return ZYM_ERROR;
    if (idx < 0)   return zym_newNull();
    return read_entry_as_buffer(vm, r, (uint32_t)idx);
}

// Pack.info(arg) — `arg` is a string entry name or a numeric index.
// Names with multiple matching entries resolve to the first hit (use
// the numeric form to disambiguate). Returns null when there's no
// self bundle, or the name/index doesn't resolve.
ZymValue f_self_info(ZymVM* vm, ZymValue, ZymValue arg) {
    const ZpkReader* r = self_reader_get();
    if (!r) return zym_newNull();
    int idx = resolve_entry_arg(vm, r, arg, "Pack.info(arg)");
    if (idx == -2) return ZYM_ERROR;
    if (idx < 0)   return zym_newNull();
    return make_entry_info(vm, r, (uint32_t)idx);
}

// Pack.verify() — full structured CRC report for the self bundle, or
// null when there's no self bundle.
ZymValue f_self_verify0(ZymVM* vm, ZymValue) {
    const ZpkReader* r = self_reader_get();
    if (!r) return zym_newNull();
    return build_verify_report(vm, r);
}

// Pack.verify(arg) — quick per-entry bool check for the self bundle.
// `arg` may be a string entry name or a numeric index. Returns false
// for "no self bundle", "no such entry", or "CRC mismatch"; raises a
// runtime error only on a wrong-type argument.
ZymValue f_self_verify1(ZymVM* vm, ZymValue, ZymValue arg) {
    const ZpkReader* r = self_reader_get();
    if (!r) return zym_newBool(false);
    int idx = resolve_entry_arg(vm, r, arg, "Pack.verify(arg)");
    if (idx == -2) return ZYM_ERROR;
    if (idx < 0)   return zym_newBool(false);
    return zym_newBool(entry_crc_ok(r, (uint32_t)idx));
}

ZymValue f_closeSelf(ZymVM* /*vm*/, ZymValue) {
    bool was_open = g_self_open;
    self_reader_close();
    return zym_newBool(was_open);
}

// ---- arbitrary-bundle handle ----------------------------------------------

struct BundleHandle {
    ZpkReader reader{};
    bool      open = false;     // false after `close()` (or before open)
};

void bundleFinalizer(ZymVM*, void* data) {
    auto* h = static_cast<BundleHandle*>(data);
    if (!h) return;
    if (h->open) {
        zpk_reader_close(&h->reader);
        h->open = false;
    }
    delete h;
}

// `self` is the closure's bound context (the `__bundle__` native
// context created in `make_bundle_instance`), exactly mirroring the
// pattern used in `hash.cpp` / `buffer.cpp` instance methods. The
// underlying `BundleHandle*` is the context's user data.
BundleHandle* unwrap_bundle_with_vm(ZymVM* /*vm*/, ZymValue ctx) {
    return static_cast<BundleHandle*>(zym_getNativeData(ctx));
}

// ---- arbitrary-bundle method implementations ------------------------------

ZymValue b_list(ZymVM* vm, ZymValue self) {
    BundleHandle* h = unwrap_bundle_with_vm(vm, self);
    if (!h || !h->open) return zym_newNull();
    ZymValue list = zym_newList(vm);
    zym_pushRoot(vm, list);
    for (uint32_t i = 0; i < h->reader.footer.entry_count; i++) {
        zym_listAppend(vm, list, make_entry_info(vm, &h->reader, i));
    }
    zym_popRoot(vm);
    return list;
}

ZymValue b_entryName(ZymVM* vm, ZymValue self) {
    BundleHandle* h = unwrap_bundle_with_vm(vm, self);
    if (!h || !h->open) return zym_newNull();
    const ZpkEntry& e = h->reader.manifest[h->reader.footer.entry_index];
    if (e.name_length == 0 || !h->reader.strtab) return zym_newString(vm, "");
    return zym_newStringN(vm, (const char*)(h->reader.strtab + e.name_offset), (int)e.name_length);
}

ZymValue b_has(ZymVM* vm, ZymValue self, ZymValue nameV) {
    if (!zym_isString(nameV)) {
        zym_runtimeError(vm, "bundle.has(name) expects a string");
        return ZYM_ERROR;
    }
    BundleHandle* h = unwrap_bundle_with_vm(vm, self);
    if (!h || !h->open) return zym_newBool(false);
    return zym_newBool(find_entry_by_name(&h->reader, zym_asCString(nameV)) >= 0);
}

// bundle.open(arg) — string name or numeric index, mirroring
// `Pack.open(arg)`. Numeric form is the way to disambiguate when
// multiple entries share a name.
ZymValue b_open(ZymVM* vm, ZymValue self, ZymValue arg) {
    BundleHandle* h = unwrap_bundle_with_vm(vm, self);
    if (!h || !h->open) return zym_newNull();
    int idx = resolve_entry_arg(vm, &h->reader, arg, "bundle.open(arg)");
    if (idx == -2) return ZYM_ERROR;
    if (idx < 0)   return zym_newNull();
    return read_entry_as_buffer(vm, &h->reader, (uint32_t)idx);
}

// bundle.info(arg) — string name or numeric index, mirroring
// `Pack.info(arg)`. Numeric form is the way to disambiguate when
// multiple entries share a name.
ZymValue b_info(ZymVM* vm, ZymValue self, ZymValue arg) {
    BundleHandle* h = unwrap_bundle_with_vm(vm, self);
    if (!h || !h->open) return zym_newNull();
    int idx = resolve_entry_arg(vm, &h->reader, arg, "bundle.info(arg)");
    if (idx == -2) return ZYM_ERROR;
    if (idx < 0)   return zym_newNull();
    return make_entry_info(vm, &h->reader, (uint32_t)idx);
}

// bundle.verify() — full structured CRC report for the open handle,
// or null after the handle has been closed.
ZymValue b_verify0(ZymVM* vm, ZymValue self) {
    BundleHandle* h = unwrap_bundle_with_vm(vm, self);
    if (!h || !h->open) return zym_newNull();
    return build_verify_report(vm, &h->reader);
}

// bundle.verify(arg) — quick per-entry bool check on the handle.
// Same string/number dispatch as `Pack.verify(arg)`.
ZymValue b_verify1(ZymVM* vm, ZymValue self, ZymValue arg) {
    BundleHandle* h = unwrap_bundle_with_vm(vm, self);
    if (!h || !h->open) return zym_newBool(false);
    int idx = resolve_entry_arg(vm, &h->reader, arg, "bundle.verify(arg)");
    if (idx == -2) return ZYM_ERROR;
    if (idx < 0)   return zym_newBool(false);
    return zym_newBool(entry_crc_ok(&h->reader, (uint32_t)idx));
}

ZymValue b_close(ZymVM* vm, ZymValue self) {
    BundleHandle* h = unwrap_bundle_with_vm(vm, self);
    if (!h) return zym_newBool(false);
    if (!h->open) return zym_newBool(false);
    zpk_reader_close(&h->reader);
    h->open = false;
    return zym_newBool(true);
}

// ---- bundle assembly ------------------------------------------------------

ZymValue make_bundle_instance(ZymVM* vm, BundleHandle* handle) {
    ZymValue ctxv = zym_createNativeContext(vm, handle, bundleFinalizer);
    zym_pushRoot(vm, ctxv);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__bundle__", ctxv);

#define M(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctxv); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)

    M("list",      "list()",       b_list);
    M("entryName", "entryName()",  b_entryName);
    M("has",       "has(name)",    b_has);
    M("open",      "open(arg)",    b_open);
    M("info",      "info(arg)",    b_info);
    M("close",     "close()",      b_close);

#undef M

    // `verify` is overloaded by arity:
    //   verify()    -> full structured report (or null after close)
    //   verify(arg) -> bool quick check; arg is a name or numeric index
    {
        ZymValue v0 = zym_createNativeClosure(vm, "verify()",    (void*)b_verify0, ctxv);
        zym_pushRoot(vm, v0);
        ZymValue v1 = zym_createNativeClosure(vm, "verify(arg)", (void*)b_verify1, ctxv);
        zym_pushRoot(vm, v1);
        ZymValue dispatcher = zym_createDispatcher(vm);
        zym_pushRoot(vm, dispatcher);
        zym_addOverload(vm, dispatcher, v0);
        zym_addOverload(vm, dispatcher, v1);
        zym_mapSet(vm, obj, "verify", dispatcher);
        zym_popRoot(vm); // dispatcher
        zym_popRoot(vm); // v1
        zym_popRoot(vm); // v0
    }

    zym_popRoot(vm);
    zym_popRoot(vm);
    return obj;
}

ZymValue f_openFile(ZymVM* vm, ZymValue, ZymValue pathV) {
    if (!zym_isString(pathV)) {
        zym_runtimeError(vm, "Pack.openFile(path) expects a string");
        return ZYM_ERROR;
    }
    const char* path = zym_asCString(pathV);
    auto* h = new BundleHandle();
    if (zpk_reader_open_path(&h->reader, path) != 1) {
        delete h;
        return zym_newNull();
    }
    h->open = true;
    return make_bundle_instance(vm, h);
}

} // namespace

// ---- factory --------------------------------------------------------------

ZymValue nativePack_create(ZymVM* vm) {
    ZymValue ctxv = zym_createNativeContext(vm, nullptr, nullptr);
    zym_pushRoot(vm, ctxv);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

#define F(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctxv); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)

    F("build",      "build(spec)",     f_build);

    // Self-bundle introspection.
    F("hasSelf",    "hasSelf()",       f_hasSelf);
    F("entryName",  "entryName()",     f_self_entryName);
    F("list",       "list()",          f_self_list);
    F("has",        "has(name)",       f_self_has);
    F("open",       "open(arg)",       f_self_open);
    F("info",       "info(arg)",       f_self_info);
    F("closeSelf",  "closeSelf()",     f_closeSelf);

    // Arbitrary bundle.
    F("openFile",   "openFile(path)",  f_openFile);

#undef F

    // `Pack.verify` overloaded by arity (same shape as the bundle handle):
    //   Pack.verify()    -> full report for the self bundle, or null when none
    //   Pack.verify(arg) -> bool quick per-entry check; arg is a name or index
    {
        ZymValue v0 = zym_createNativeClosure(vm, "verify()",    (void*)f_self_verify0, ctxv);
        zym_pushRoot(vm, v0);
        ZymValue v1 = zym_createNativeClosure(vm, "verify(arg)", (void*)f_self_verify1, ctxv);
        zym_pushRoot(vm, v1);
        ZymValue dispatcher = zym_createDispatcher(vm);
        zym_pushRoot(vm, dispatcher);
        zym_addOverload(vm, dispatcher, v0);
        zym_addOverload(vm, dispatcher, v1);
        zym_mapSet(vm, obj, "verify", dispatcher);
        zym_popRoot(vm);
        zym_popRoot(vm);
        zym_popRoot(vm);
    }

    zym_popRoot(vm);
    zym_popRoot(vm);
    return obj;
}
