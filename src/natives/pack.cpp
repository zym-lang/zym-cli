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

#ifndef _WIN32
#  include <sys/stat.h>
#  include <errno.h>
#  include <unistd.h>
#endif
#ifdef _WIN32
#  include <process.h>  // _getpid
#endif

// Provided by buffer.cpp — type-clean Buffer reader / builder so we
// don't have to drag Godot's `PackedByteArray` header into this
// translation unit.
extern bool readBufferBytes(ZymVM* vm, ZymValue v, const char** out_data, size_t* out_size);
// (declared in natives.hpp) ZymValue makeBufferFromBytes(ZymVM*, const char*, size_t);

namespace {

// Module-wide verbosity flag for `Pack`'s speculative reader probes
// (currently `sniff_payload_geometry`, used by `Pack.build` /
// `Pack.splice` / `Pack.inspectBin`). Defaults to quiet because the
// common case for these probes is a clean stub with no payload — the
// reader's "no .zpk payload found" diagnostic is noise on that path.
// Scripts opt in via `Pack.setVerboseOutput(true)` when they want
// the reader chatter for debugging.
//
// This only governs the *probe* paths. User-facing opens
// (`Pack.openFile`, `Pack.openBuffer`) keep their normal diagnostics
// regardless, since the user explicitly asked to open the bundle and
// expects feedback when it's not a valid `.zpk`.
bool g_pack_verbose = false;

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

// Apply executable permission bits to a file. POSIX-only; on Windows
// (where "executable" is determined by extension and the PE header,
// neither of which we touch) this is a silent no-op returning true.
//
// On POSIX, the file's current mode is read via `stat` and the
// execute bits are mirrored onto each user/group/world class that
// already has the corresponding read bit set, masked by the process
// umask. This matches the behavior of `install -m +x` and `chmod +x`
// honoring umask, which is the least-surprising default. Returns
// `true` on success, `false` (with a stderr line) on stat/chmod
// failure — the caller is expected to treat failure as "warn but
// the bundle was written successfully".
bool apply_executable_bit(const char* path, const char* origin) {
#ifdef _WIN32
    (void)path;
    (void)origin;
    return true;
#else
    struct stat st;
    if (::stat(path, &st) != 0) {
        std::fprintf(stderr,
            "%s: stat(\"%s\") failed: %s; bundle written but executable bit not set.\n",
            origin, path, std::strerror(errno));
        return false;
    }
    mode_t m = st.st_mode;
    // Mirror read bits to execute bits, then mask by umask.
    mode_t add = 0;
    if (m & S_IRUSR) add |= S_IXUSR;
    if (m & S_IRGRP) add |= S_IXGRP;
    if (m & S_IROTH) add |= S_IXOTH;
    mode_t cur = ::umask(0); ::umask(cur);
    add &= ~cur;
    mode_t target = (m | add) & 07777;
    if (target == (m & 07777)) return true; // already executable
    if (::chmod(path, target) != 0) {
        std::fprintf(stderr,
            "%s: chmod(\"%s\") failed: %s; bundle written but executable bit not set.\n",
            origin, path, std::strerror(errno));
        return false;
    }
    return true;
#endif
}

// Mirror the source file's permission bits onto `dst`. POSIX-only;
// silent no-op + true on Windows. Used by `Pack.splice` so the
// rebuilt binary inherits the source stub's executable-ness without
// needing an explicit toggle. Failure warns but does not fail the
// operation.
bool mirror_mode_bits(const char* src, const char* dst, const char* origin) {
#ifdef _WIN32
    (void)src;
    (void)dst;
    (void)origin;
    return true;
#else
    struct stat st;
    if (::stat(src, &st) != 0) {
        std::fprintf(stderr,
            "%s: stat(\"%s\") failed: %s; output mode bits not mirrored.\n",
            origin, src, std::strerror(errno));
        return false;
    }
    if (::chmod(dst, st.st_mode & 07777) != 0) {
        std::fprintf(stderr,
            "%s: chmod(\"%s\") failed: %s; output mode bits not mirrored.\n",
            origin, dst, std::strerror(errno));
        return false;
    }
    return true;
#endif
}

bool ends_with(const char* s, const char* suffix) {
    if (!s || !suffix) return false;
    size_t ls = std::strlen(s);
    size_t lf = std::strlen(suffix);
    if (ls < lf) return false;
    return std::strcmp(s + ls - lf, suffix) == 0;
}

// Geometry of an existing payload inside an executable / bundle file.
// Populated by `sniff_payload_geometry()`.
struct PayloadGeometry {
    uint64_t file_size;
    uint64_t stub_size;     // bytes 0..stub_size are the native stub (0 for headless)
    uint64_t payload_size;  // bytes stub_size..file_size are the ZPK payload
    uint16_t format_version;
    uint32_t entry_count;
    uint32_t entry_index;
};

// Probe a path for a trailing ZPK payload, computing the boundary
// between the native stub (if any) and the ZPK region. Returns true
// on success and fills `*out`. Returns false (without printing) when
// the file is not a valid ZPK or could not be read; this is by design
// — callers (`Pack.inspectBin`, `Pack.build` in stub-replace mode)
// want to branch cheaply on the result.
//
// Stub end = lowest data_offset across all manifest entries (the data
// region starts immediately after the stub). When the bundle has no
// data-bearing entries, the data region is empty and the strtab /
// manifest follow the stub directly, so `strtab_offset` (or
// `manifest_offset` if strtab is empty) is the correct boundary.
bool sniff_payload_geometry(const char* path, PayloadGeometry* out) {
    ZpkReader r{};
    if (zpk_reader_open_path_verbose(&r, path, g_pack_verbose ? 1 : 0) != 1) return false;

    uint64_t stub_end = r.footer.strtab_offset;
    if (r.footer.strtab_size == 0) {
        // strtab is zero-length; manifest comes right after the data
        // region in that case (manifest_offset == strtab_offset, but
        // be defensive in case a future writer reorders).
        if (r.footer.manifest_offset < stub_end) stub_end = r.footer.manifest_offset;
    }
    for (uint32_t i = 0; i < r.footer.entry_count; i++) {
        const ZpkEntry& e = r.manifest[i];
        if (e.data_size == 0) continue;
        if (e.data_offset < stub_end) stub_end = e.data_offset;
    }

    out->file_size      = r.file_size;
    out->stub_size      = stub_end;
    out->payload_size   = r.file_size - stub_end;
    out->format_version = r.footer.format_version;
    out->entry_count    = r.footer.entry_count;
    out->entry_index    = r.footer.entry_index;

    zpk_reader_close(&r);
    return true;
}

// Map a script-facing kind string to the on-disk `ZpkKind` byte.
// Strings are used (rather than numeric constants) per the
// status-string convention in `docs/cli/conventions.md`.
bool kind_from_string(const char* s, uint8_t* out) {
    if (!s || !*s) return false;
    if (std::strcmp(s, "entry_source")    == 0) { *out = ZPK_KIND_ENTRY_SOURCE;    return true; }
    if (std::strcmp(s, "entry_bytecode")  == 0) { *out = ZPK_KIND_ENTRY_BYTECODE;  return true; }
    if (std::strcmp(s, "source_map")      == 0) { *out = ZPK_KIND_SOURCE_MAP;      return true; }
    if (std::strcmp(s, "file")            == 0) { *out = ZPK_KIND_FILE;            return true; }
    if (std::strcmp(s, "blob")            == 0) { *out = ZPK_KIND_BLOB;            return true; }
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

// ---- shared entry-spec parsing --------------------------------------------
//
// Parses a single entry-spec map (the shape used by `Pack.build`'s
// `spec.entries[i]` and the upcoming `EditHandle.add` / `replace`
// specs) into a `ZpkEntryInput` plus a side `std::string` for the
// name.
//
// `where`: human-readable prefix for runtime error messages (e.g.
//   "Pack.build(spec): spec.entries[3]" or "edit.replace(arg, spec)").
// `require_all`:
//   - true  (build / add)    : `kind` is required, and exactly one of
//                              `data` / `path` must be set.
//   - false (replace)        : every field is optional; at most one of
//                              `data` / `path` may be set; on absence
//                              the corresponding `out_info` field is
//                              left zero-initialised so the caller can
//                              detect "field not set in this spec".
// `out_set_*` flags (when non-null) are written to true for any field
// the spec actually mentioned, letting `replace` distinguish "field
// omitted" from "field explicitly set to its zero value".
//
// `bundle_compress` / `bundle_level` are the bundle-wide defaults
// (`f_build` reads them from the outer spec; `add`/`replace` pass
// `false` / `3` since there is no bundle scope at edit time).
//
// On success returns true and fills `*out_info` + `*out_name_storage`
// (the latter's `.data()` is plugged into `out_info->name`).
// On failure raises a runtime error on `vm` and returns false.
bool parse_entry_spec(ZymVM* vm, ZymValue e, const char* where,
                      bool require_all,
                      bool bundle_compress, int bundle_level,
                      ZpkEntryInput* out_info,
                      std::string*   out_name_storage,
                      bool* out_set_kind = nullptr,
                      bool* out_set_name = nullptr,
                      bool* out_set_flags = nullptr,
                      bool* out_set_custom = nullptr,
                      bool* out_set_compression = nullptr,
                      bool* out_set_bytes = nullptr)
{
    if (!zym_isMap(e)) {
        zym_runtimeError(vm, "%s must be a map", where);
        return false;
    }
    std::memset(out_info, 0, sizeof(*out_info));

    // kind
    ZymValue kindV = zym_mapGet(vm, e, "kind");
    const bool kind_present = (kindV != ZYM_ERROR) && !zym_isNull(kindV);
    if (kind_present) {
        if (!zym_isString(kindV)) {
            zym_runtimeError(vm, "%s.kind must be a string", where);
            return false;
        }
        uint8_t kind_byte = 0;
        const char* kind_str = zym_asCString(kindV);
        if (!kind_from_string(kind_str, &kind_byte)) {
            zym_runtimeError(vm,
                "%s.kind '%s' is not a known kind "
                "(expected 'entry_source', 'entry_bytecode', 'source_map', 'file', or 'blob')",
                where, kind_str);
            return false;
        }
        out_info->kind = kind_byte;
        if (out_set_kind) *out_set_kind = true;
    } else if (require_all) {
        zym_runtimeError(vm, "%s.kind must be a string", where);
        return false;
    }

    // name (optional in both modes)
    const char* name = opt_string(vm, e, "name");
    if (name) {
        out_name_storage->assign(name);
        out_info->name = out_name_storage->data();
        out_info->name_length = out_name_storage->size();
        if (out_set_name) *out_set_name = true;
    }

    // flags / custom (optional in both modes)
    {
        ZymValue fv = zym_mapGet(vm, e, "flags");
        if (fv != ZYM_ERROR && !zym_isNull(fv)) {
            if (!zym_isNumber(fv)) {
                zym_runtimeError(vm, "%s.flags must be a number", where);
                return false;
            }
            out_info->flags = (uint16_t)zym_asNumber(fv);
            if (out_set_flags) *out_set_flags = true;
        }
    }
    {
        ZymValue cv = zym_mapGet(vm, e, "custom");
        if (cv != ZYM_ERROR && !zym_isNull(cv)) {
            if (!zym_isNumber(cv)) {
                zym_runtimeError(vm, "%s.custom must be a number", where);
                return false;
            }
            out_info->custom = (uint32_t)zym_asNumber(cv);
            if (out_set_custom) *out_set_custom = true;
        }
    }

    // compression / level (optional; entry overrides bundle default)
    bool entry_compress = bundle_compress;
    bool comp_present = false;
    {
        ZymValue ecv = zym_mapGet(vm, e, "compression");
        if (ecv != ZYM_ERROR && !zym_isNull(ecv)) {
            if (!zym_isBool(ecv)) {
                zym_runtimeError(vm, "%s.compression must be a bool", where);
                return false;
            }
            entry_compress = zym_asBool(ecv);
            comp_present = true;
        }
    }
    int entry_level = bundle_level;
    {
        ZymValue elv = zym_mapGet(vm, e, "level");
        if (elv != ZYM_ERROR && !zym_isNull(elv)) {
            if (!zym_isNumber(elv)) {
                zym_runtimeError(vm, "%s.level must be a number", where);
                return false;
            }
            double d = zym_asNumber(elv);
            if (d < 1 || d > 22) {
                zym_runtimeError(vm, "%s.level must be in 1..22", where);
                return false;
            }
            entry_level = (int)d;
            comp_present = true;
        }
    }
    if (comp_present || require_all) {
        out_info->compression = entry_compress ? ZPK_COMPRESSION_ZSTD : ZPK_COMPRESSION_NONE;
        out_info->level       = entry_compress ? entry_level : 0;
        if (comp_present && out_set_compression) *out_set_compression = true;
    }

    // bytes source: in `require_all` mode exactly one of `data` /
    // `path` must be set; in partial mode at most one may be set
    // (and either being absent leaves the bytes-source unset).
    ZymValue dataV = zym_mapGet(vm, e, "data");
    ZymValue pathV = zym_mapGet(vm, e, "path");
    const bool data_present = (dataV != ZYM_ERROR) && !zym_isNull(dataV);
    const bool path_present = (pathV != ZYM_ERROR) && !zym_isNull(pathV);

    if (data_present && path_present) {
        zym_runtimeError(vm, "%s sets both `data` and `path`; pick exactly one", where);
        return false;
    }
    if (require_all && !data_present && !path_present) {
        zym_runtimeError(vm,
            "%s needs exactly one of `data` (Buffer) or `path` (string)", where);
        return false;
    }
    if (data_present) {
        const char* bytes = nullptr;
        size_t      sz    = 0;
        if (!readBufferBytes(vm, dataV, &bytes, &sz)) {
            zym_runtimeError(vm, "%s.data must be a Buffer", where);
            return false;
        }
        out_info->data      = bytes;
        out_info->data_size = sz;
        out_info->file_path = nullptr;
        if (out_set_bytes) *out_set_bytes = true;
    } else if (path_present) {
        if (!zym_isString(pathV)) {
            zym_runtimeError(vm, "%s.path must be a string", where);
            return false;
        }
        out_info->data      = nullptr;
        out_info->data_size = 0;
        out_info->file_path = zym_asCString(pathV);
        if (out_set_bytes) *out_set_bytes = true;
    }

    return true;
}

// ---- the build call -------------------------------------------------------

ZymValue f_setVerboseOutput(ZymVM* vm, ZymValue /*self*/, ZymValue vV) {
    if (!zym_isBool(vV)) {
        zym_runtimeError(vm, "Pack.setVerboseOutput(verbose) expects a bool");
        return ZYM_ERROR;
    }
    g_pack_verbose = zym_asBool(vV);
    return zym_newBool(g_pack_verbose);
}

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
        char where[64];
        std::snprintf(where, sizeof(where), "Pack.build(spec): spec.entries[%d]", i);
        if (!parse_entry_spec(vm, e, where,
                              /*require_all=*/true,
                              bundle_compress, bundle_level,
                              &infos[i], &name_storage[i])) {
            return ZYM_ERROR;
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
    //
    // If the stub file already carries a ZPK payload (i.e. it was
    // itself produced by an earlier `Pack.build` / `Pack.splice`),
    // we trim the existing payload off and rebuild on top of just
    // the native portion. This means a single stub binary can be
    // re-packed in place without ever stacking multiple ZPK regions
    // — `Pack` enforces "exactly one ZPK per executable" by
    // construction. No `mode` flag is needed: append-vs-swap is
    // decided by what the stub file actually contains.
    char*  stub_data = nullptr;
    size_t stub_size = 0;
    const bool headless = ends_with(output_path, ".zpk");
    if (!headless) {
        const char* stub_path = opt_string(vm, specV, "stub");
        if (stub_path && *stub_path) {
            PayloadGeometry geom;
            const bool has_payload = sniff_payload_geometry(stub_path, &geom);

            stub_data = slurp_binary(stub_path, &stub_size);
            if (!stub_data) {
                std::fprintf(stderr,
                    "Pack.build: could not read stub file \"%s\".\n", stub_path);
                return zym_newBool(false);
            }
            if (has_payload && geom.stub_size < stub_size) {
                // Trim the existing payload; we only keep the native
                // portion. (Truncate the in-memory buffer; the
                // underlying allocation stays — `free` still works.)
                stub_size = static_cast<size_t>(geom.stub_size);
            }
        }
    }

    // ----- setExecutable (optional, default false) -----
    //
    // After the bundle is written, optionally mark the output file
    // as executable. POSIX adds the execute bits (mirrored from the
    // read bits, masked by umask); Windows is a silent no-op since
    // executability there is decided by extension / PE header. A
    // chmod failure warns to stderr but does not fail the build —
    // the bundle bytes themselves were written successfully.
    bool set_exec = false;
    {
        ZymValue sv = zym_mapGet(vm, specV, "setExecutable");
        if (sv != ZYM_ERROR && !zym_isNull(sv)) {
            if (!zym_isBool(sv)) {
                zym_runtimeError(vm,
                    "Pack.build(spec): spec.setExecutable must be a bool");
                return ZYM_ERROR;
            }
            set_exec = zym_asBool(sv);
        }
    }

    int ok = zpk_write_bundle(output_path,
                              stub_data, stub_size,
                              infos.data(), (size_t)n,
                              entry_index);
    if (stub_data) std::free(stub_data);

    if (ok != 0 && set_exec) {
        // Failure here is intentionally non-fatal: the bundle is
        // valid, just not chmod'd. The user can retry the chmod
        // themselves. We still warn so the failure is visible.
        (void)apply_executable_bit(output_path, "Pack.build");
    }

    return zym_newBool(ok != 0);
}

// ===========================================================================
// READ API
// ===========================================================================
//
// Bundles are accessed exclusively via handles returned by
// `Pack.openFile(path)` / `Pack.openBuffer(buffer)`. Each handle owns
// its own `ZpkReader`, cached for the handle's lifetime and freed by
// `bundle.close()` (or by the GC finalizer as a safety net). After
// close, every method on the handle returns `null` / `false`.
//
// `openFile` works uniformly on headless `.zpk` files and on
// stub-wrapped executables (the footer-from-EOF probe is
// stub-agnostic), so a single API covers "open a loose bundle",
// "introspect another exe", and "open the running exe" (via
// whatever path the host hands the script).

// ---- kind / compression vocabularies --------------------------------------

const char* kind_to_string(uint8_t k, char* user_buf /*>=16 bytes*/) {
    switch (k) {
        case ZPK_KIND_RESERVED:        return "reserved";
        case ZPK_KIND_ENTRY_SOURCE:    return "entry_source";
        case ZPK_KIND_ENTRY_BYTECODE:  return "entry_bytecode";
        case ZPK_KIND_SOURCE_MAP:      return "source_map";
        case ZPK_KIND_FILE:            return "file";
        case ZPK_KIND_BLOB:            return "blob";
        default:
            // 0x06..0x7E reserved; 0x7F..0xFF user range.
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

// bundle.entryIndex() -> number | null
//   The manifest index of the program entry-point entry (the one
//   `entryName()` resolves to). Returns `null` after the handle has been
//   closed. Mirrors `edit.entryIndex()` on the edit-handle side.
ZymValue b_entryIndex(ZymVM* vm, ZymValue self) {
    BundleHandle* h = unwrap_bundle_with_vm(vm, self);
    if (!h || !h->open) return zym_newNull();
    return zym_newNumber((double)h->reader.footer.entry_index);
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

// bundle.formatVersion() -> number | null
//   The on-disk `format_version` recorded in the bundle's footer.
//   Returns `null` after the handle has been closed.
ZymValue b_formatVersion(ZymVM* vm, ZymValue self) {
    BundleHandle* h = unwrap_bundle_with_vm(vm, self);
    if (!h || !h->open) return zym_newNull();
    return zym_newNumber((double)h->reader.footer.format_version);
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

    M("list",          "list()",          b_list);
    M("entryName",     "entryName()",     b_entryName);
    M("entryIndex",    "entryIndex()",    b_entryIndex);
    M("has",           "has(name)",       b_has);
    M("open",          "open(arg)",       b_open);
    M("info",          "info(arg)",       b_info);
    M("close",         "close()",         b_close);
    M("formatVersion", "formatVersion()", b_formatVersion);

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

// Pack.openBuffer(buffer) -> bundle | null
//   Opens a `.zpk` from an in-memory `Buffer`. The reader takes its own
//   copy of the bytes (so the caller's `Buffer` is independent and may
//   be reused or freed immediately afterwards). Useful when the bundle
//   is fetched over the network, decrypted in-process, or otherwise
//   produced without ever touching the filesystem.
ZymValue f_openBuffer(ZymVM* vm, ZymValue, ZymValue bufV) {
    const char* bytes = nullptr;
    size_t size = 0;
    if (!readBufferBytes(vm, bufV, &bytes, &size)) {
        zym_runtimeError(vm, "Pack.openBuffer(buffer) expects a Buffer");
        return ZYM_ERROR;
    }
    auto* h = new BundleHandle();
    if (zpk_reader_open_memory(&h->reader, bytes, size) != 1) {
        delete h;
        return zym_newNull();
    }
    h->open = true;
    return make_bundle_instance(vm, h);
}

// Pack.inspectBin(path) -> { fileSize, stubSize, payloadSize,
//                            formatVersion, entryCount, entryIndex,
//                            isHeadless, hasStub } | null
//
// Cross-platform geometry probe. Returns `null` when the file does
// not contain a valid trailing ZPK payload (no magic, bad CRC,
// truncated footer, file unreadable, etc.) so scripts can branch
// cheaply on "is this binary already packed?". Does **not** raise on
// I/O failures — those are the expected `null` path.
//
// The boundary between the native stub and the ZPK region is
// computed from the manifest (lowest `data_offset`, falling back to
// `strtab_offset` when the bundle has no data-bearing entries).
//
// Named `inspectBin` rather than `inspectExe` because the operation
// is platform-agnostic — ELFs, PE/COFF, Mach-O, raw blobs, anything
// with a ZPK trailer all work the same way.
ZymValue f_inspectBin(ZymVM* vm, ZymValue, ZymValue pathV) {
    if (!zym_isString(pathV)) {
        zym_runtimeError(vm, "Pack.inspectBin(path) expects a string");
        return ZYM_ERROR;
    }
    const char* path = zym_asCString(pathV);
    PayloadGeometry geom;
    if (!sniff_payload_geometry(path, &geom)) {
        return zym_newNull();
    }

    ZymValue m = zym_newMap(vm);
    zym_pushRoot(vm, m);
    zym_mapSet(vm, m, "fileSize",      zym_newNumber((double)geom.file_size));
    zym_mapSet(vm, m, "stubSize",      zym_newNumber((double)geom.stub_size));
    zym_mapSet(vm, m, "payloadSize",   zym_newNumber((double)geom.payload_size));
    zym_mapSet(vm, m, "formatVersion", zym_newNumber((double)geom.format_version));
    zym_mapSet(vm, m, "entryCount",    zym_newNumber((double)geom.entry_count));
    zym_mapSet(vm, m, "entryIndex",    zym_newNumber((double)geom.entry_index));
    zym_mapSet(vm, m, "isHeadless",    zym_newBool(geom.stub_size == 0));
    zym_mapSet(vm, m, "hasStub",       zym_newBool(geom.stub_size != 0));
    zym_popRoot(vm);
    return m;
}

// Pack.splice(stubPath, zpkPath, outputPath) -> bool
//
// Combine an already-built standalone `.zpk` with a native stub
// binary and write the result to `outputPath`. If the stub at
// `stubPath` already carries a ZPK payload, only its native portion
// (bytes `0..stub_size`) is taken — the previous payload is dropped,
// preserving the "exactly one ZPK per executable" invariant. The
// `.zpk` is then appended after the stub, with all of its absolute
// offsets (footer's `manifest_offset` / `strtab_offset`, every
// entry's `data_offset`) rewritten to account for the new stub
// prefix, and the manifest + footer CRCs recomputed.
//
// This is the file-level peer of `Pack.build`: it doesn't decompose
// the source `.zpk` back through the writer pipeline, so a pre-built
// bundle can be shipped on top of any stub without round-tripping
// through script memory. Per-entry `data_crc32` values are computed
// over each entry's bytes (not its position in the file), so they
// remain valid after the offset shift.
ZymValue f_splice(ZymVM* vm, ZymValue, ZymValue stubPathV, ZymValue zpkPathV, ZymValue outPathV) {
    if (!zym_isString(stubPathV) || !zym_isString(zpkPathV) || !zym_isString(outPathV)) {
        zym_runtimeError(vm,
            "Pack.splice(stubPath, zpkPath, outputPath) expects three strings");
        return ZYM_ERROR;
    }
    const char* stub_path = zym_asCString(stubPathV);
    const char* zpk_path  = zym_asCString(zpkPathV);
    const char* out_path  = zym_asCString(outPathV);

    // Slurp the stub. If it already has a payload, trim to the
    // native portion before writing.
    size_t stub_size = 0;
    char* stub_data = slurp_binary(stub_path, &stub_size);
    if (!stub_data) {
        std::fprintf(stderr,
            "Pack.splice: could not read stub file \"%s\".\n", stub_path);
        return zym_newBool(false);
    }
    {
        PayloadGeometry geom;
        if (sniff_payload_geometry(stub_path, &geom) && geom.stub_size < stub_size) {
            stub_size = static_cast<size_t>(geom.stub_size);
        }
    }

    // Slurp the source `.zpk` and verify it actually is one. We need
    // both the geometry (for the source's stub_size, to detect a
    // stub-wrapped input that we'll re-base from its own data
    // region) and the raw bytes (to rewrite the footer / manifest).
    PayloadGeometry zpk_geom;
    if (!sniff_payload_geometry(zpk_path, &zpk_geom)) {
        std::fprintf(stderr,
            "Pack.splice: \"%s\" is not a valid .zpk bundle.\n", zpk_path);
        std::free(stub_data);
        return zym_newBool(false);
    }
    size_t zpk_total = 0;
    char* zpk_full = slurp_binary(zpk_path, &zpk_total);
    if (!zpk_full) {
        std::fprintf(stderr,
            "Pack.splice: could not read zpk file \"%s\".\n", zpk_path);
        std::free(stub_data);
        return zym_newBool(false);
    }

    // Source ZPK region: bytes `[zpk_geom.stub_size .. zpk_total)`.
    // (When the source is itself stub-wrapped — i.e. someone passed
    // an exe-with-payload as the `zpk` argument — we splice only its
    // payload onto the new stub.)
    const size_t src_payload_off  = static_cast<size_t>(zpk_geom.stub_size);
    const size_t src_payload_size = zpk_total - src_payload_off;

    // Allocate the rewritten payload (same size as source payload —
    // we only mutate offsets, not the byte count).
    uint8_t* payload = static_cast<uint8_t*>(std::malloc(src_payload_size));
    if (!payload) {
        std::fprintf(stderr, "Pack.splice: out of memory.\n");
        std::free(stub_data);
        std::free(zpk_full);
        return zym_newBool(false);
    }
    std::memcpy(payload, zpk_full + src_payload_off, src_payload_size);
    std::free(zpk_full);

    // Compute the per-section delta: every absolute offset in the
    // source payload assumed `src_payload_off` as the data-region
    // base; we want it to be `stub_size` instead.
    const int64_t delta = static_cast<int64_t>(stub_size) - static_cast<int64_t>(src_payload_off);

    // Locate the footer (last ZPK_FOOTER_SIZE bytes of the payload).
    if (src_payload_size < ZPK_FOOTER_SIZE) {
        std::fprintf(stderr, "Pack.splice: source payload too small for a footer.\n");
        std::free(stub_data);
        std::free(payload);
        return zym_newBool(false);
    }
    ZpkFooter* footer = reinterpret_cast<ZpkFooter*>(payload + src_payload_size - ZPK_FOOTER_SIZE);

    // Rebase footer's absolute offsets.
    footer->manifest_offset = static_cast<uint64_t>(static_cast<int64_t>(footer->manifest_offset) + delta);
    footer->strtab_offset   = static_cast<uint64_t>(static_cast<int64_t>(footer->strtab_offset)   + delta);

    // Rebase every manifest entry's data_offset. The manifest sits
    // at the (rebased) `manifest_offset`, but inside the in-memory
    // `payload` buffer it's at `manifest_offset - stub_size` (the
    // payload base inside the final file).
    const size_t manifest_local = static_cast<size_t>(footer->manifest_offset) - stub_size;
    if (manifest_local + (size_t)footer->entry_count * ZPK_ENTRY_SIZE > src_payload_size - ZPK_FOOTER_SIZE) {
        std::fprintf(stderr, "Pack.splice: manifest extends past payload.\n");
        std::free(stub_data);
        std::free(payload);
        return zym_newBool(false);
    }
    ZpkEntry* manifest = reinterpret_cast<ZpkEntry*>(payload + manifest_local);
    for (uint32_t i = 0; i < footer->entry_count; i++) {
        manifest[i].data_offset = static_cast<uint64_t>(static_cast<int64_t>(manifest[i].data_offset) + delta);
    }

    // Recompute manifest CRC over (manifest entries || strtab).
    const size_t manifest_size = (size_t)footer->entry_count * ZPK_ENTRY_SIZE;
    uint32_t manifest_crc = zpk_crc32(0, manifest, manifest_size);
    if (footer->strtab_size > 0) {
        const size_t strtab_local = static_cast<size_t>(footer->strtab_offset) - stub_size;
        manifest_crc = zpk_crc32(manifest_crc, payload + strtab_local, static_cast<size_t>(footer->strtab_size));
    }
    footer->manifest_crc32 = manifest_crc;

    // Recompute footer CRC (with `footer_crc32` zeroed during hash).
    footer->footer_crc32 = 0;
    footer->footer_crc32 = zpk_crc32(0, footer, sizeof(ZpkFooter));

    // Write stub + rebased payload to output.
    FILE* f = std::fopen(out_path, "wb");
    if (!f) {
        std::fprintf(stderr,
            "Pack.splice: could not create output file \"%s\".\n", out_path);
        std::free(stub_data);
        std::free(payload);
        return zym_newBool(false);
    }
    bool ok = true;
    if (stub_size > 0) {
        if (std::fwrite(stub_data, 1, stub_size, f) != stub_size) ok = false;
    }
    if (ok && src_payload_size > 0) {
        if (std::fwrite(payload, 1, src_payload_size, f) != src_payload_size) ok = false;
    }
    std::fclose(f);
    std::free(stub_data);
    std::free(payload);
    if (!ok) {
        std::fprintf(stderr,
            "Pack.splice: short write to \"%s\".\n", out_path);
        return zym_newBool(false);
    }

    // Mirror the source stub's permission bits onto the output so a
    // splice of an executable stub yields an executable result, and
    // a splice of a non-executable file yields a non-executable
    // result. POSIX-only; no-op on Windows. Failure warns but does
    // not fail the operation — the splice itself succeeded.
    (void)mirror_mode_bits(stub_path, out_path, "Pack.splice");

    return zym_newBool(true);
}

// ---- edit-transaction handle (Stages 5–6: skeleton + staging ops) --------
//
// Per `future/pack_updates_roadmap.md`, `Pack.editFile` / `Pack.editBuffer`
// open an existing bundle, let the script stage an arbitrary number of
// mutations against an in-memory op log, and `commit()` them in one
// atomic rewrite.
//
// Stage 5 landed the handle struct, the tagged `StagedOp` shape, the
// two constructors, and the read-side methods (`list` / `info` /
// `entryIndex` / `close`) reading directly from the source reader.
//
// Stage 6 adds the *mutating ops* (staging only — `commit` lands in
// Stage 7/8) and projects the staged op log onto the read-side
// methods so `list` / `info` / `entryIndex` reflect the post-edit
// view, not the source view. The projection is recomputed on demand
// (`rebuild_view`) rather than maintained incrementally — simpler,
// no incremental-correctness bugs, and edit transactions are not
// hot-path.

enum StagedOpKind : uint8_t {
    OP_ADD = 1,
    OP_REMOVE,
    OP_REPLACE,
    OP_RENAME,
    OP_MOVE,
    OP_SET_ENTRY_INDEX,
    OP_SET_FLAGS,
    OP_SET_CUSTOM,
    OP_SET_STUB,
};

// Stub source for `setStub`. Resolved at commit time.
enum StubSourceKind : uint8_t {
    STUB_SRC_NONE = 0,    // sentinel; setStub not invoked on this op slot
    STUB_SRC_PATH,        // path string
    STUB_SRC_BUFFER,      // owned bytes copied from a Buffer
    STUB_SRC_NULL,        // explicit null → strip stub on commit
};

struct StagedOp {
    StagedOpKind kind;

    // Most ops carry a "target" entry identifier resolved against the
    // *current staged view* at commit time. `target_is_index=true`
    // means `target_index` is authoritative; otherwise `target_name`
    // is the first-hit name match. `add` uses neither (it inserts).
    bool         target_is_index = false;
    int          target_index    = -1;
    std::string  target_name;

    // op-specific payloads (only the relevant union member is populated)
    // ADD / REPLACE: a parsed entry-spec (validated at stage time)
    ZpkEntryInput spec{};
    std::string   spec_name;     // backing storage for spec.name
    std::vector<char> spec_bytes; // owned copy of `data` bytes (so the script-side Buffer can move on)
    std::string   spec_path;      // owned copy of `file_path` string
    bool          spec_set_kind = false, spec_set_name = false;
    bool          spec_set_flags = false, spec_set_custom = false;
    bool          spec_set_compression = false, spec_set_bytes = false;
    int           add_insert_index = -1; // ADD only; -1 = append

    // RENAME
    std::string  new_name;

    // MOVE
    int          dst_index = -1;

    // SET_ENTRY_INDEX is encoded via target_*; no extra payload.

    // SET_FLAGS
    uint16_t     flags_value = 0;

    // SET_CUSTOM
    uint32_t     custom_value = 0;

    // SET_STUB
    StubSourceKind   stub_source = STUB_SRC_NONE;
    std::string      stub_path;     // STUB_SRC_PATH
    std::vector<char> stub_bytes;   // STUB_SRC_BUFFER
};

// Staged-view entry. Represents one row of the manifest as it would
// be after applying all staged ops to the source. Used by `list` /
// `info` / `entryIndex` (read-side) and by every mutating op for
// validating arg dispatch (string-name first-hit / index bounds)
// against the post-staging shape.
struct StagedEntry {
    // Origin: either an unchanged source row (with optional metadata
    // overrides), or a brand-new added row (or a source row whose
    // bytes were replaced).
    bool        from_source = true;
    uint32_t    source_index = 0;       // valid iff from_source

    // For non-source rows (added or bytes-replaced), `bytes_op_index`
    // points at the StagedOp in `EditHandle::ops` that supplied the
    // bytes (its `spec_bytes` / `spec_path` / `spec.compression`
    // / `spec.level` / `spec.flags`+`custom` if set). -1 means
    // "bytes come from the source reader at `source_index`".
    int         bytes_op_index = -1;

    // Identity / metadata. For source rows these start as a copy of
    // the source manifest entry; ops mutate them in place.
    std::string name;
    uint8_t     kind = 0;
    uint16_t    flags = 0;
    uint32_t    custom = 0;
    uint8_t     compression = 0;
    uint64_t    uncompressed_size = 0;
    uint64_t    data_size = 0;
    uint32_t    data_crc32 = 0;
};

struct EditHandle {
    ZpkReader              src{};
    bool                   open = false;
    bool                   from_buffer = false;  // true → editBuffer; false → editFile
    std::string            source_path;          // editFile: original path (for commit's atomic rename)
    ZymValue               buffer_ref = ZYM_ERROR; // editBuffer: bound Buffer value (for write-back)
    std::vector<StagedOp>  ops;

    // Staged view: rebuilt on demand via `rebuild_view`. Index is the
    // post-staging manifest position; `entry_index` is the
    // post-staging program-entry pointer (UINT32_MAX = none).
    std::vector<StagedEntry> view;
    uint32_t                 view_entry_index = UINT32_MAX;
    bool                     view_dirty = true;
};

void editFinalizer(ZymVM*, void* data) {
    auto* h = static_cast<EditHandle*>(data);
    if (!h) return;
    if (h->open) {
        zpk_reader_close(&h->src);
        h->open = false;
    }
    delete h;
}

EditHandle* unwrap_edit(ZymVM* /*vm*/, ZymValue ctx) {
    return static_cast<EditHandle*>(zym_getNativeData(ctx));
}

// Read a source manifest entry's name into a std::string. Source
// names are not NUL-terminated, so we have to length-copy.
void source_entry_name(const ZpkReader* r, uint32_t i, std::string* out) {
    const ZpkEntry& e = r->manifest[i];
    if (e.name_length > 0 && r->strtab) {
        out->assign((const char*)(r->strtab + e.name_offset), (size_t)e.name_length);
    } else {
        out->clear();
    }
}

// Re-project the staged view from `src.manifest` plus `ops` in order.
// Cheap by construction: O(entries + ops). Called lazily before any
// op or read that needs the post-staging shape.
void rebuild_view(EditHandle* h) {
    if (!h->view_dirty) return;
    h->view.clear();
    // Seed from source.
    const uint32_t n = h->src.footer.entry_count;
    h->view.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        const ZpkEntry& e = h->src.manifest[i];
        StagedEntry s;
        s.from_source = true;
        s.source_index = i;
        source_entry_name(&h->src, i, &s.name);
        s.kind = e.kind;
        s.flags = e.flags;
        s.custom = e.custom;
        s.compression = e.compression;
        s.uncompressed_size = e.uncompressed_size;
        s.data_size = e.data_size;
        s.data_crc32 = e.data_crc32;
        h->view.push_back(std::move(s));
    }
    h->view_entry_index = h->src.footer.entry_index;
    if (h->view_entry_index >= h->view.size()) h->view_entry_index = UINT32_MAX;

    // Replay ops in order. We use lambdas for arg resolution against
    // the *current* (mid-replay) view, since some op chains depend
    // on prior ops (e.g. add then setFlags on the added entry).
    auto resolve = [&](const StagedOp& op) -> int {
        if (op.target_is_index) {
            int idx = op.target_index;
            if (idx < 0 || idx >= (int)h->view.size()) return -1;
            return idx;
        }
        // first-hit name
        for (size_t i = 0; i < h->view.size(); i++) {
            if (h->view[i].name == op.target_name) return (int)i;
        }
        return -1;
    };

    for (size_t op_i = 0; op_i < h->ops.size(); op_i++) {
        const StagedOp& op = h->ops[op_i];
        switch (op.kind) {
            case OP_ADD: {
                StagedEntry s;
                s.from_source = false;
                s.bytes_op_index = (int)op_i;
                s.name = op.spec_name;
                s.kind = op.spec.kind;
                s.flags = op.spec.flags;
                s.custom = op.spec.custom;
                s.compression = op.spec.compression;
                // For added entries we don't know on-disk sizes /
                // CRCs until commit-time. Report logical size from
                // staged bytes; commit recomputes everything.
                s.uncompressed_size = op.spec.data_size;
                s.data_size = op.spec.data_size;
                s.data_crc32 = 0;
                int ins = op.add_insert_index;
                if (ins < 0 || ins > (int)h->view.size()) ins = (int)h->view.size();
                h->view.insert(h->view.begin() + ins, std::move(s));
                if (h->view_entry_index != UINT32_MAX &&
                    (int)h->view_entry_index >= ins) {
                    h->view_entry_index++;
                }
                break;
            }
            case OP_REMOVE: {
                int idx = resolve(op);
                if (idx < 0) break; // commit will re-validate; skip in view replay
                h->view.erase(h->view.begin() + idx);
                if (h->view_entry_index != UINT32_MAX) {
                    if ((int)h->view_entry_index == idx) {
                        h->view_entry_index = UINT32_MAX;
                    } else if ((int)h->view_entry_index > idx) {
                        h->view_entry_index--;
                    }
                }
                break;
            }
            case OP_REPLACE: {
                int idx = resolve(op);
                if (idx < 0) break;
                StagedEntry& s = h->view[idx];
                if (op.spec_set_kind)   s.kind = op.spec.kind;
                if (op.spec_set_flags)  s.flags = op.spec.flags;
                if (op.spec_set_custom) s.custom = op.spec.custom;
                if (op.spec_set_compression) s.compression = op.spec.compression;
                if (op.spec_set_name)   s.name = op.spec_name;
                if (op.spec_set_bytes) {
                    s.from_source = false; // body replaced; commit re-emits fresh
                    s.bytes_op_index = (int)op_i;
                    s.uncompressed_size = op.spec.data_size;
                    s.data_size = op.spec.data_size;
                    s.data_crc32 = 0;
                }
                break;
            }
            case OP_RENAME: {
                int idx = resolve(op);
                if (idx < 0) break;
                h->view[idx].name = op.new_name;
                break;
            }
            case OP_MOVE: {
                int idx = resolve(op);
                if (idx < 0) break;
                int dst = op.dst_index;
                if (dst < 0) dst = 0;
                if (dst >= (int)h->view.size()) dst = (int)h->view.size() - 1;
                if (dst == idx) break;
                StagedEntry s = std::move(h->view[idx]);
                h->view.erase(h->view.begin() + idx);
                h->view.insert(h->view.begin() + dst, std::move(s));
                // Update entry_index pointer.
                if (h->view_entry_index != UINT32_MAX) {
                    uint32_t ei = h->view_entry_index;
                    if ((int)ei == idx) {
                        h->view_entry_index = (uint32_t)dst;
                    } else {
                        // moved row vacated idx, occupies dst
                        int e = (int)ei;
                        if (idx < dst) {
                            if (e > idx && e <= dst) e--;
                        } else {
                            if (e >= dst && e < idx) e++;
                        }
                        h->view_entry_index = (uint32_t)e;
                    }
                }
                break;
            }
            case OP_SET_ENTRY_INDEX: {
                int idx = resolve(op);
                if (idx < 0) break;
                h->view_entry_index = (uint32_t)idx;
                break;
            }
            case OP_SET_FLAGS: {
                int idx = resolve(op);
                if (idx < 0) break;
                h->view[idx].flags = op.flags_value;
                break;
            }
            case OP_SET_CUSTOM: {
                int idx = resolve(op);
                if (idx < 0) break;
                h->view[idx].custom = op.custom_value;
                break;
            }
            case OP_SET_STUB:
                // Affects commit only; no view change.
                break;
        }
    }

    h->view_dirty = false;
}

// Resolve a script-supplied arg (string name | numeric index) against
// the current staged view. Returns:
//   >= 0 : view index
//   -1   : not found / out of range (caller turns into `false`)
//   -2   : wrong-type argument (runtime error already raised)
int resolve_view_arg(ZymVM* vm, EditHandle* h, ZymValue arg, const char* fn_name) {
    rebuild_view(h);
    if (zym_isNumber(arg)) {
        double d = zym_asNumber(arg);
        if (d < 0 || d >= (double)h->view.size()) return -1;
        return (int)d;
    }
    if (zym_isString(arg)) {
        const char* s = zym_asCString(arg);
        for (size_t i = 0; i < h->view.size(); i++) {
            if (h->view[i].name == s) return (int)i;
        }
        return -1;
    }
    zym_runtimeError(vm, "%s expects a string entry name or a numeric index", fn_name);
    return -2;
}

// Populate a StagedOp's `target_*` from a name-or-index arg without
// resolving (resolution happens at view-rebuild / commit time so the
// op can refer to a future entry created by an earlier op). Returns
// false on wrong-type (runtime error raised).
bool stage_target(ZymVM* vm, StagedOp* op, ZymValue arg, const char* fn_name) {
    if (zym_isNumber(arg)) {
        op->target_is_index = true;
        op->target_index = (int)zym_asNumber(arg);
        return true;
    }
    if (zym_isString(arg)) {
        op->target_is_index = false;
        op->target_name.assign(zym_asCString(arg));
        return true;
    }
    zym_runtimeError(vm, "%s expects a string entry name or a numeric index", fn_name);
    return false;
}

// Build an `entryInfo` map from a `StagedEntry` (post-staging view).
// Mirrors the shape of `make_entry_info` but reports `dataOffset` as
// 0 / `dataSize` from the staged-size estimate since the on-disk
// layout is unknown until commit.
ZymValue make_staged_entry_info(ZymVM* vm, EditHandle* h, uint32_t idx) {
    const StagedEntry& s = h->view[idx];
    char user_buf[16];
    ZymValue m = zym_newMap(vm);
    zym_pushRoot(vm, m);
    zym_mapSet(vm, m, "index",            zym_newNumber((double)idx));
    zym_mapSet(vm, m, "name",             zym_newStringN(vm, s.name.data(), (int)s.name.size()));
    zym_mapSet(vm, m, "kind",             zym_newString(vm, kind_to_string(s.kind, user_buf)));
    zym_mapSet(vm, m, "kindByte",         zym_newNumber((double)s.kind));
    zym_mapSet(vm, m, "compression",      zym_newString(vm, compression_to_string(s.compression)));
    zym_mapSet(vm, m, "compressionByte",  zym_newNumber((double)s.compression));
    zym_mapSet(vm, m, "flags",            zym_newNumber((double)s.flags));
    zym_mapSet(vm, m, "required",         zym_newBool((s.flags & ZPK_ENTRY_FLAG_REQUIRED) != 0));
    zym_mapSet(vm, m, "lazy",             zym_newBool((s.flags & ZPK_ENTRY_FLAG_LAZY) != 0));
    zym_mapSet(vm, m, "dataOffset",       zym_newNumber(0.0));
    zym_mapSet(vm, m, "dataSize",         zym_newNumber((double)s.data_size));
    zym_mapSet(vm, m, "uncompressedSize", zym_newNumber((double)s.uncompressed_size));
    zym_mapSet(vm, m, "size",             zym_newNumber((double)s.uncompressed_size));
    zym_mapSet(vm, m, "dataCrc32",        zym_newNumber((double)s.data_crc32));
    zym_mapSet(vm, m, "custom",           zym_newNumber((double)s.custom));
    zym_mapSet(vm, m, "isEntry",          zym_newBool(h->view_entry_index == idx));
    zym_mapSet(vm, m, "fromSource",       zym_newBool(s.from_source));
    zym_popRoot(vm);
    return m;
}

// ---- Stage 6: read-side methods (projected from staged view) -------------

ZymValue e_list(ZymVM* vm, ZymValue self) {
    EditHandle* h = unwrap_edit(vm, self);
    if (!h || !h->open) return zym_newNull();
    rebuild_view(h);
    ZymValue list = zym_newList(vm);
    zym_pushRoot(vm, list);
    for (uint32_t i = 0; i < h->view.size(); i++) {
        zym_listAppend(vm, list, make_staged_entry_info(vm, h, i));
    }
    zym_popRoot(vm);
    return list;
}

ZymValue e_info(ZymVM* vm, ZymValue self, ZymValue arg) {
    EditHandle* h = unwrap_edit(vm, self);
    if (!h || !h->open) return zym_newNull();
    int idx = resolve_view_arg(vm, h, arg, "edit.info(arg)");
    if (idx == -2) return ZYM_ERROR;
    if (idx < 0)   return zym_newNull();
    return make_staged_entry_info(vm, h, (uint32_t)idx);
}

ZymValue e_entryIndex(ZymVM* vm, ZymValue self) {
    EditHandle* h = unwrap_edit(vm, self);
    if (!h || !h->open) return zym_newNull();
    rebuild_view(h);
    if (h->view_entry_index == UINT32_MAX) return zym_newNull();
    return zym_newNumber((double)h->view_entry_index);
}

ZymValue e_close(ZymVM* vm, ZymValue self) {
    EditHandle* h = unwrap_edit(vm, self);
    if (!h) return zym_newBool(false);
    if (!h->open) return zym_newBool(false);
    zpk_reader_close(&h->src);
    h->open = false;
    h->ops.clear();
    h->view.clear();
    h->view_dirty = true;
    return zym_newBool(true);
}

// ---- Stage 6: mutating ops (staging only — commit lands Stage 7/8) -------

// Mark the view stale and push the staged op. Centralised so any
// future bookkeeping (op count limits, debug logging, …) has one
// place to hook.
void push_op(EditHandle* h, StagedOp&& op) {
    h->ops.push_back(std::move(op));
    h->view_dirty = true;
}

ZymValue e_add(ZymVM* vm, ZymValue self, ZymValue specV) {
    EditHandle* h = unwrap_edit(vm, self);
    if (!h || !h->open) return zym_newBool(false);

    // Optional `index:` insertion point; default = append.
    int insert_index = -1;
    ZymValue ixV = zym_mapGet(vm, specV, "index");
    if (ixV != ZYM_ERROR && !zym_isNull(ixV)) {
        if (!zym_isNumber(ixV)) {
            zym_runtimeError(vm, "edit.add(spec).index must be a number");
            return ZYM_ERROR;
        }
        rebuild_view(h);
        double d = zym_asNumber(ixV);
        if (d < 0 || d > (double)h->view.size()) {
            zym_runtimeError(vm,
                "edit.add(spec).index %.0f out of range [0..%zu]",
                d, h->view.size());
            return ZYM_ERROR;
        }
        insert_index = (int)d;
    }

    StagedOp op;
    op.kind = OP_ADD;
    op.add_insert_index = insert_index;
    if (!parse_entry_spec(vm, specV, "edit.add(spec)",
                          /*require_all=*/true,
                          /*bundle_compress=*/false, /*bundle_level=*/3,
                          &op.spec, &op.spec_name,
                          &op.spec_set_kind, &op.spec_set_name,
                          &op.spec_set_flags, &op.spec_set_custom,
                          &op.spec_set_compression, &op.spec_set_bytes)) {
        return ZYM_ERROR;
    }
    // Own any borrowed bytes / path so the script-side Buffer / string
    // can be reused or GC'd before commit.
    if (op.spec.data && op.spec.data_size > 0) {
        const char* p = static_cast<const char*>(op.spec.data);
        op.spec_bytes.assign(p, p + op.spec.data_size);
        op.spec.data = op.spec_bytes.data();
    }
    if (op.spec.file_path) {
        op.spec_path.assign(op.spec.file_path);
        op.spec.file_path = op.spec_path.c_str();
    }
    // Re-bind name pointer to the owned storage (parse_entry_spec
    // set spec.name to spec_name->data(), but after `op` was moved
    // into push_op below the pointer would dangle without re-binding
    // post-move; defer that to commit-time helpers since we read
    // from spec_name directly in rebuild_view).
    push_op(h, std::move(op));
    return zym_newBool(true);
}

ZymValue e_remove(ZymVM* vm, ZymValue self, ZymValue arg) {
    EditHandle* h = unwrap_edit(vm, self);
    if (!h || !h->open) return zym_newBool(false);
    StagedOp op;
    op.kind = OP_REMOVE;
    if (!stage_target(vm, &op, arg, "edit.remove(arg)")) return ZYM_ERROR;
    push_op(h, std::move(op));
    return zym_newBool(true);
}

ZymValue e_replace(ZymVM* vm, ZymValue self, ZymValue arg, ZymValue specV) {
    EditHandle* h = unwrap_edit(vm, self);
    if (!h || !h->open) return zym_newBool(false);
    StagedOp op;
    op.kind = OP_REPLACE;
    if (!stage_target(vm, &op, arg, "edit.replace(arg, spec)")) return ZYM_ERROR;
    if (!parse_entry_spec(vm, specV, "edit.replace(arg, spec)",
                          /*require_all=*/false,
                          /*bundle_compress=*/false, /*bundle_level=*/3,
                          &op.spec, &op.spec_name,
                          &op.spec_set_kind, &op.spec_set_name,
                          &op.spec_set_flags, &op.spec_set_custom,
                          &op.spec_set_compression, &op.spec_set_bytes)) {
        return ZYM_ERROR;
    }
    if (op.spec_set_bytes && op.spec.data && op.spec.data_size > 0) {
        const char* p = static_cast<const char*>(op.spec.data);
        op.spec_bytes.assign(p, p + op.spec.data_size);
        op.spec.data = op.spec_bytes.data();
    }
    if (op.spec_set_bytes && op.spec.file_path) {
        op.spec_path.assign(op.spec.file_path);
        op.spec.file_path = op.spec_path.c_str();
    }
    push_op(h, std::move(op));
    return zym_newBool(true);
}

ZymValue e_rename(ZymVM* vm, ZymValue self, ZymValue arg, ZymValue newNameV) {
    EditHandle* h = unwrap_edit(vm, self);
    if (!h || !h->open) return zym_newBool(false);
    if (!zym_isString(newNameV)) {
        zym_runtimeError(vm, "edit.rename(arg, newName): newName must be a string");
        return ZYM_ERROR;
    }
    StagedOp op;
    op.kind = OP_RENAME;
    if (!stage_target(vm, &op, arg, "edit.rename(arg, newName)")) return ZYM_ERROR;
    op.new_name.assign(zym_asCString(newNameV));
    push_op(h, std::move(op));
    return zym_newBool(true);
}

ZymValue e_move(ZymVM* vm, ZymValue self, ZymValue srcArg, ZymValue dstV) {
    EditHandle* h = unwrap_edit(vm, self);
    if (!h || !h->open) return zym_newBool(false);
    if (!zym_isNumber(dstV)) {
        zym_runtimeError(vm, "edit.move(srcArg, dstIndex): dstIndex must be a number");
        return ZYM_ERROR;
    }
    StagedOp op;
    op.kind = OP_MOVE;
    if (!stage_target(vm, &op, srcArg, "edit.move(srcArg, dstIndex)")) return ZYM_ERROR;
    op.dst_index = (int)zym_asNumber(dstV);
    if (op.dst_index < 0) {
        zym_runtimeError(vm, "edit.move(srcArg, dstIndex): dstIndex must be >= 0");
        return ZYM_ERROR;
    }
    push_op(h, std::move(op));
    return zym_newBool(true);
}

ZymValue e_setEntryIndex(ZymVM* vm, ZymValue self, ZymValue arg) {
    EditHandle* h = unwrap_edit(vm, self);
    if (!h || !h->open) return zym_newBool(false);
    StagedOp op;
    op.kind = OP_SET_ENTRY_INDEX;
    if (!stage_target(vm, &op, arg, "edit.setEntryIndex(arg)")) return ZYM_ERROR;
    push_op(h, std::move(op));
    return zym_newBool(true);
}

ZymValue e_setFlags(ZymVM* vm, ZymValue self, ZymValue arg, ZymValue flagsV) {
    EditHandle* h = unwrap_edit(vm, self);
    if (!h || !h->open) return zym_newBool(false);
    StagedOp op;
    op.kind = OP_SET_FLAGS;
    if (!stage_target(vm, &op, arg, "edit.setFlags(arg, flags)")) return ZYM_ERROR;

    // `flags` accepts either a numeric mask or a map of {required,
    // lazy} bools (matching the `make_entry_info` reverse shape).
    if (zym_isNumber(flagsV)) {
        op.flags_value = (uint16_t)zym_asNumber(flagsV);
    } else if (zym_isMap(flagsV)) {
        uint16_t f = 0;
        ZymValue rv = zym_mapGet(vm, flagsV, "required");
        if (rv != ZYM_ERROR && !zym_isNull(rv)) {
            if (!zym_isBool(rv)) {
                zym_runtimeError(vm, "edit.setFlags: flags.required must be a bool");
                return ZYM_ERROR;
            }
            if (zym_asBool(rv)) f |= ZPK_ENTRY_FLAG_REQUIRED;
        }
        ZymValue lv = zym_mapGet(vm, flagsV, "lazy");
        if (lv != ZYM_ERROR && !zym_isNull(lv)) {
            if (!zym_isBool(lv)) {
                zym_runtimeError(vm, "edit.setFlags: flags.lazy must be a bool");
                return ZYM_ERROR;
            }
            if (zym_asBool(lv)) f |= ZPK_ENTRY_FLAG_LAZY;
        }
        ZymValue mv = zym_mapGet(vm, flagsV, "mask");
        if (mv != ZYM_ERROR && !zym_isNull(mv)) {
            if (!zym_isNumber(mv)) {
                zym_runtimeError(vm, "edit.setFlags: flags.mask must be a number");
                return ZYM_ERROR;
            }
            f |= (uint16_t)zym_asNumber(mv);
        }
        op.flags_value = f;
    } else {
        zym_runtimeError(vm,
            "edit.setFlags(arg, flags): flags must be a number mask or a map");
        return ZYM_ERROR;
    }
    push_op(h, std::move(op));
    return zym_newBool(true);
}

ZymValue e_setCustom(ZymVM* vm, ZymValue self, ZymValue arg, ZymValue customV) {
    EditHandle* h = unwrap_edit(vm, self);
    if (!h || !h->open) return zym_newBool(false);
    if (!zym_isNumber(customV)) {
        zym_runtimeError(vm, "edit.setCustom(arg, custom): custom must be a number");
        return ZYM_ERROR;
    }
    StagedOp op;
    op.kind = OP_SET_CUSTOM;
    if (!stage_target(vm, &op, arg, "edit.setCustom(arg, custom)")) return ZYM_ERROR;
    op.custom_value = (uint32_t)zym_asNumber(customV);
    push_op(h, std::move(op));
    return zym_newBool(true);
}

ZymValue e_setStub(ZymVM* vm, ZymValue self, ZymValue arg) {
    EditHandle* h = unwrap_edit(vm, self);
    if (!h || !h->open) return zym_newBool(false);
    StagedOp op;
    op.kind = OP_SET_STUB;
    if (zym_isNull(arg)) {
        op.stub_source = STUB_SRC_NULL;
    } else if (zym_isString(arg)) {
        op.stub_source = STUB_SRC_PATH;
        op.stub_path.assign(zym_asCString(arg));
    } else {
        const char* bytes = nullptr;
        size_t size = 0;
        if (!readBufferBytes(vm, arg, &bytes, &size)) {
            zym_runtimeError(vm,
                "edit.setStub(arg): arg must be a path string, a Buffer, or null");
            return ZYM_ERROR;
        }
        op.stub_source = STUB_SRC_BUFFER;
        if (size > 0) op.stub_bytes.assign(bytes, bytes + size);
    }
    push_op(h, std::move(op));
    return zym_newBool(true);
}

// ---- Stage 7: commit() for editFile -------------------------------------
//
// Translates the staged view into a `ZpkEntryInput[]` array (using the
// Stage-1 borrow variant for unchanged source rows and the staged op's
// owned bytes / path for added or bytes-replaced rows), resolves the
// stub source (default = preserved verbatim from the source bundle;
// the most recent `OP_SET_STUB` op overrides), runs final-state
// invariant checks (`entry_index` -> ENTRY_SOURCE / ENTRY_BYTECODE),
// writes to `<path>.zym-edit.tmp.<pid>`, `fsync`s, `chmod`s to mirror
// the source's mode bits (preserves `+x`), then `rename`s atomically.
//
// On failure: returns `false`, unlinks the temp, and leaves the source
// untouched. The handle remains usable with its staged ops preserved
// so the script can fix and retry.
//
// On success: returns `true`. The reader is closed and re-opened from
// the new file so any further ops chain off the freshly-written
// bundle; `h->ops` is cleared and `view_dirty` is set so the next
// read-side method reflects the committed state.

// Compute the size of the leading CLI stub (bytes before the ZPK
// payload) in an open reader's owned file_data. The payload regions
// in the footer (manifest_offset, strtab_offset) and the first entry
// in the manifest are all post-stub, so the smallest of those is the
// stub boundary. For a bundle with zero entries, `strtab_offset` is
// authoritative (it is the first non-stub offset).
size_t compute_source_stub_size(const ZpkReader* r) {
    uint64_t lo = r->footer.strtab_offset;
    if (r->footer.manifest_offset < lo) lo = r->footer.manifest_offset;
    for (uint32_t i = 0; i < r->footer.entry_count; i++) {
        if (r->manifest[i].data_size > 0 && r->manifest[i].data_offset < lo) {
            lo = r->manifest[i].data_offset;
        }
    }
    if (lo > r->file_size) lo = r->file_size; // paranoia
    return (size_t)lo;
}

// Pre-flight invariant checks against the post-staging view. Returns
// true if OK; on failure raises a runtime error and returns false.
bool validate_commit_view(ZymVM* vm, EditHandle* h, const char* fn_name) {
    rebuild_view(h);
    if (h->view.size() > UINT32_MAX) {
        zym_runtimeError(vm, "%s: too many entries (%zu)", fn_name, h->view.size());
        return false;
    }
    if (h->view_entry_index != UINT32_MAX) {
        if ((size_t)h->view_entry_index >= h->view.size()) {
            zym_runtimeError(vm,
                "%s: staged entry_index %u out of range [0..%zu)",
                fn_name, h->view_entry_index, h->view.size());
            return false;
        }
        uint8_t k = h->view[h->view_entry_index].kind;
        if (k != ZPK_KIND_ENTRY_SOURCE && k != ZPK_KIND_ENTRY_BYTECODE) {
            zym_runtimeError(vm,
                "%s: staged entry_index %u points at kind 0x%02x; must be "
                "ENTRY_SOURCE (0x01) or ENTRY_BYTECODE (0x02)",
                fn_name, h->view_entry_index, (unsigned)k);
            return false;
        }
    }
    return true;
}

// Locate the latest staged OP_SET_STUB op (if any). Returns its index
// in `h->ops` or -1 if none.
int find_latest_setstub(const EditHandle* h) {
    for (int i = (int)h->ops.size() - 1; i >= 0; i--) {
        if (h->ops[i].kind == OP_SET_STUB) return i;
    }
    return -1;
}

ZymValue e_commit(ZymVM* vm, ZymValue self) {
    EditHandle* h = unwrap_edit(vm, self);
    if (!h || !h->open) {
        zym_runtimeError(vm, "edit.commit(): handle is closed");
        return ZYM_ERROR;
    }

    if (!validate_commit_view(vm, h, "edit.commit()")) return ZYM_ERROR;

    // ---- Resolve the stub source ----
    // Default = preserve source verbatim. Latest OP_SET_STUB overrides.
    const void* stub_data  = nullptr;
    size_t      stub_size  = 0;
    // Heap buffer if we loaded a stub from disk; freed at function exit.
    char*       owned_stub = nullptr;

    int set_stub_idx = find_latest_setstub(h);
    if (set_stub_idx >= 0) {
        const StagedOp& s = h->ops[set_stub_idx];
        switch (s.stub_source) {
            case STUB_SRC_NULL:
                // headless on commit
                break;
            case STUB_SRC_PATH: {
                owned_stub = slurp_binary(s.stub_path.c_str(), &stub_size);
                if (!owned_stub) {
                    zym_runtimeError(vm,
                        "edit.commit(): failed to read stub from path '%s'",
                        s.stub_path.c_str());
                    return ZYM_ERROR;
                }
                stub_data = owned_stub;
                break;
            }
            case STUB_SRC_BUFFER:
                stub_data = s.stub_bytes.empty() ? nullptr : s.stub_bytes.data();
                stub_size = s.stub_bytes.size();
                break;
            case STUB_SRC_NONE:
                break;
        }
    } else {
        // No setStub; borrow the source bundle's stub verbatim.
        size_t src_stub = compute_source_stub_size(&h->src);
        if (src_stub > 0) {
            stub_data = h->src.file_data;
            stub_size = src_stub;
        }
    }

    // ---- Build ZpkEntryInput[] from the staged view ----
    std::vector<ZpkEntryInput> inputs(h->view.size());
    uint32_t entry_index = h->view_entry_index == UINT32_MAX
                               ? 0u : h->view_entry_index;
    // If view_entry_index is UINT32_MAX, the writer still needs *some*
    // entry index; conventional choice = 0. But a v1 bundle with zero
    // entries can't carry a meaningful entry_index either way; the
    // writer enforces it points at a valid kind, so a no-entry edit
    // would only be legal if the source already had zero entries AND
    // entry_index was zero. We don't try to enable headless-edit-zero
    // here — error out if there are no entries.
    if (h->view.empty()) {
        if (owned_stub) std::free(owned_stub);
        zym_runtimeError(vm,
            "edit.commit(): cannot commit a bundle with zero entries");
        return ZYM_ERROR;
    }

    for (size_t i = 0; i < h->view.size(); i++) {
        const StagedEntry& s = h->view[i];
        ZpkEntryInput& in = inputs[i];
        // Zero-initialize.
        in = ZpkEntryInput{};
        in.name        = s.name.empty() ? nullptr : s.name.data();
        in.name_length = s.name.size();
        in.kind        = s.kind;
        in.flags       = s.flags;
        in.custom      = s.custom;

        if (s.from_source) {
            // Borrow the on-disk slice from the open source reader.
            in.source_reader = &h->src;
            in.source_index  = s.source_index;
            // compression/data/file_path are ignored when source_reader
            // is set (per zpk_writer.hpp contract).
        } else if (s.bytes_op_index >= 0 &&
                   s.bytes_op_index < (int)h->ops.size()) {
            const StagedOp& op = h->ops[s.bytes_op_index];
            in.compression = op.spec.compression;
            in.level       = op.spec.level;
            if (!op.spec_bytes.empty() || op.spec.data) {
                in.data      = op.spec_bytes.empty() ? op.spec.data
                                                     : (const void*)op.spec_bytes.data();
                in.data_size = op.spec_bytes.empty() ? op.spec.data_size
                                                     : op.spec_bytes.size();
            } else if (!op.spec_path.empty()) {
                in.file_path = op.spec_path.c_str();
            } else if (op.spec.file_path) {
                in.file_path = op.spec.file_path;
            }
        } else {
            // Should be unreachable: a non-source row without a
            // bytes-providing op means rebuild_view dropped state.
            if (owned_stub) std::free(owned_stub);
            zym_runtimeError(vm,
                "edit.commit(): internal: staged entry %zu has no bytes source", i);
            return ZYM_ERROR;
        }
    }

    // ---- editBuffer path (Stage 8) ----
    // Build the new bundle entirely in memory, write it back into the
    // borrowed PackedByteArray* via writeBufferBytes (so every live
    // script reference to the source Buffer sees the new contents),
    // then re-open the reader against the freshly-mutated buffer so
    // any further staged ops chain off the committed state.
    if (h->from_buffer) {
        uint8_t* out_buf = nullptr;
        size_t   out_size = 0;
        int ok = zpk_write_bundle_to_memory(&out_buf, &out_size,
                                            stub_data, stub_size,
                                            inputs.data(), inputs.size(),
                                            entry_index);
        if (!ok) {
            if (owned_stub) std::free(owned_stub);
            zym_runtimeError(vm,
                "edit.commit(): failed to build new bundle in memory");
            return ZYM_ERROR;
        }
        // Write the new bytes into the borrowed PackedByteArray*.
        // Done while the source reader still holds its private copy
        // of the *old* bytes (zpk_reader_open_memory copies), so the
        // borrow sources used by zpk_write_bundle_to_memory above
        // remain valid through the call. After this point the
        // Buffer's contents are the new bundle.
        if (!writeBufferBytes(vm, h->buffer_ref, out_buf, out_size)) {
            std::free(out_buf);
            if (owned_stub) std::free(owned_stub);
            zym_runtimeError(vm,
                "edit.commit(): failed to write back into Buffer");
            return ZYM_ERROR;
        }
        std::free(out_buf);
        if (owned_stub) std::free(owned_stub);

        // Re-open the reader against the buffer's new contents. We
        // re-fetch the bytes via readBufferBytes to get a current
        // pointer (the resize above may have reallocated).
        zpk_reader_close(&h->src);
        const char* new_bytes = nullptr;
        size_t      new_size  = 0;
        if (!readBufferBytes(vm, h->buffer_ref, &new_bytes, &new_size) ||
            zpk_reader_open_memory(&h->src, new_bytes, new_size) != 1) {
            h->open = false;
            zym_runtimeError(vm,
                "edit.commit(): commit succeeded but reopening the Buffer "
                "failed; handle is now closed");
            return ZYM_ERROR;
        }
        h->ops.clear();
        h->view.clear();
        h->view_dirty = true;
        return zym_newBool(true);
    }

    // ---- editFile path (Stage 7) ----
    // Materialize the temp file path.
    long pid_l =
#ifdef _WIN32
        (long)::_getpid();
#else
        (long)::getpid();
#endif
    char pid_buf[32];
    std::snprintf(pid_buf, sizeof(pid_buf), ".zym-edit.tmp.%ld", pid_l);
    std::string temp_path = h->source_path + pid_buf;

    // ---- Write the new bundle to the temp file ----
    int ok = zpk_write_bundle(temp_path.c_str(),
                              stub_data, stub_size,
                              inputs.data(), inputs.size(),
                              entry_index);
    if (!ok) {
        std::remove(temp_path.c_str());
        if (owned_stub) std::free(owned_stub);
        zym_runtimeError(vm,
            "edit.commit(): failed to write temp file '%s'", temp_path.c_str());
        return ZYM_ERROR;
    }

    // ---- fsync the temp file (POSIX) ----
#ifndef _WIN32
    {
        FILE* f = std::fopen(temp_path.c_str(), "rb");
        if (f) {
            int fd = ::fileno(f);
            if (fd >= 0) ::fsync(fd);
            std::fclose(f);
        }
    }
#endif

    // ---- chmod the temp to mirror the source's mode bits ----
#ifndef _WIN32
    {
        struct stat st;
        if (::stat(h->source_path.c_str(), &st) == 0) {
            ::chmod(temp_path.c_str(), st.st_mode & 07777);
        }
    }
#endif

    // ---- Atomic rename ----
    // The source reader owns an mmap-style file_data buffer that is a
    // private copy of the source bytes (zpk_reader copies on open), so
    // renaming over the source path is safe with the reader open.
    // However, we'll re-open the reader from the new file post-rename
    // so subsequent ops chain off the fresh state.
    if (std::rename(temp_path.c_str(), h->source_path.c_str()) != 0) {
        std::remove(temp_path.c_str());
        if (owned_stub) std::free(owned_stub);
        zym_runtimeError(vm,
            "edit.commit(): atomic rename failed (temp='%s', dest='%s')",
            temp_path.c_str(), h->source_path.c_str());
        return ZYM_ERROR;
    }

    if (owned_stub) std::free(owned_stub);

    // ---- Re-open the reader from the freshly-committed file ----
    // We must close *after* the writer is done (it used h->src as a
    // borrow source for unchanged entries), which is the case here.
    zpk_reader_close(&h->src);
    if (zpk_reader_open_path(&h->src, h->source_path.c_str()) != 1) {
        // The commit succeeded on-disk; only the in-memory handle is
        // now stale. Mark the handle closed so further ops fail loudly
        // rather than reading dangling state.
        h->open = false;
        zym_runtimeError(vm,
            "edit.commit(): commit succeeded but reopening '%s' failed; "
            "handle is now closed",
            h->source_path.c_str());
        return ZYM_ERROR;
    }
    h->ops.clear();
    h->view.clear();
    h->view_dirty = true;
    return zym_newBool(true);
}

ZymValue make_edit_instance(ZymVM* vm, EditHandle* handle) {
    ZymValue ctxv = zym_createNativeContext(vm, handle, editFinalizer);
    zym_pushRoot(vm, ctxv);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__edit__", ctxv);

#define EM(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctxv); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)

    // Read-side (project the staged op log on top of the source).
    EM("list",          "list()",                       e_list);
    EM("info",          "info(arg)",                    e_info);
    EM("entryIndex",    "entryIndex()",                 e_entryIndex);
    EM("close",         "close()",                      e_close);

    // Mutating ops — Stage 6: staging only; `commit` (Stages 7/8)
    // is what actually re-emits the bundle.
    EM("add",           "add(spec)",                    e_add);
    EM("remove",        "remove(arg)",                  e_remove);
    EM("replace",       "replace(arg, spec)",           e_replace);
    EM("rename",        "rename(arg, newName)",         e_rename);
    EM("move",          "move(srcArg, dstIndex)",       e_move);
    EM("setEntryIndex", "setEntryIndex(arg)",           e_setEntryIndex);
    EM("setFlags",      "setFlags(arg, flags)",         e_setFlags);
    EM("setCustom",     "setCustom(arg, custom)",       e_setCustom);
    EM("setStub",       "setStub(arg)",                 e_setStub);

    // Finalisers — Stage 7 lands `commit` for editFile; editBuffer
    // commit (Stage 8) currently returns an error at the top of
    // `e_commit`.
    EM("commit",        "commit()",                     e_commit);

#undef EM

    zym_popRoot(vm);
    zym_popRoot(vm);
    return obj;
}

ZymValue f_editFile(ZymVM* vm, ZymValue, ZymValue pathV) {
    if (!zym_isString(pathV)) {
        zym_runtimeError(vm, "Pack.editFile(path) expects a string");
        return ZYM_ERROR;
    }
    const char* path = zym_asCString(pathV);
    auto* h = new EditHandle();
    if (zpk_reader_open_path(&h->src, path) != 1) {
        delete h;
        return zym_newNull();
    }
    h->open = true;
    h->from_buffer = false;
    h->source_path.assign(path);
    return make_edit_instance(vm, h);
}

ZymValue f_editBuffer(ZymVM* vm, ZymValue, ZymValue bufV) {
    const char* bytes = nullptr;
    size_t size = 0;
    if (!readBufferBytes(vm, bufV, &bytes, &size)) {
        zym_runtimeError(vm, "Pack.editBuffer(buffer) expects a Buffer");
        return ZYM_ERROR;
    }
    auto* h = new EditHandle();
    if (zpk_reader_open_memory(&h->src, bytes, size) != 1) {
        delete h;
        return zym_newNull();
    }
    h->open = true;
    h->from_buffer = true;
    h->buffer_ref = bufV;
    return make_edit_instance(vm, h);
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

    F("build",         "build(spec)",       f_build);

    // Toggle whether the reader's speculative probes (used by
    // `Pack.build` / `Pack.splice` / `Pack.inspectBin` to sniff an
    // existing payload) emit diagnostics on stderr. Defaults to
    // quiet; returns the new value.
    F("setVerboseOutput", "setVerboseOutput(verbose)", f_setVerboseOutput);

    // Bundle handles. `openFile` works uniformly on headless `.zpk`
    // files and on stub-wrapped executables; the host (CLI launcher
    // or stub) is responsible for passing the desired path in.
    F("openFile",      "openFile(path)",    f_openFile);
    F("openBuffer",    "openBuffer(buffer)",f_openBuffer);

    // Binary inspection / splice (cross-platform; works on any file
    // with a trailing ZPK footer, not just ELFs).
    F("inspectBin",    "inspectBin(path)",                            f_inspectBin);
    F("splice",        "splice(stubPath, zpkPath, outputPath)",       f_splice);

    // Edit transaction (Stage 5: constructors + read-side only;
    // mutating ops and commit land in Stages 6–8).
    F("editFile",      "editFile(path)",     f_editFile);
    F("editBuffer",    "editBuffer(buffer)", f_editBuffer);

#undef F

    zym_popRoot(vm);
    zym_popRoot(vm);
    return obj;
}
