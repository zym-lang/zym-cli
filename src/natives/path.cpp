// Path namespace -- thin wrapper over String's path methods plus a couple
// of zym-flavor helpers (variadic join, expandUser, withExtension). All
// methods are pure string/string -> string|bool transformations and do
// not touch the filesystem. See docs/path.md for full surface.
#include "core/os/os.h"
#include "core/string/ustring.h"
#include "core/variant/variant.h"

#include "natives.hpp"

static ZymValue stringToZym(ZymVM* vm, const String& s) {
    CharString utf8 = s.utf8();
    return zym_newStringN(vm, utf8.get_data(), utf8.length());
}

static bool reqString(ZymVM* vm, ZymValue v, const char* where, String* out) {
    if (!zym_isString(v)) {
        zym_runtimeError(vm, "%s expects a string", where);
        return false;
    }
    *out = String::utf8(zym_asCString(v));
    return true;
}

// ---- query ----

static ZymValue p_isAbsolute(ZymVM* vm, ZymValue, ZymValue pV) {
    String p; if (!reqString(vm, pV, "Path.isAbsolute(p)", &p)) return ZYM_ERROR;
    return zym_newBool(p.is_absolute_path());
}

static ZymValue p_isRelative(ZymVM* vm, ZymValue, ZymValue pV) {
    String p; if (!reqString(vm, pV, "Path.isRelative(p)", &p)) return ZYM_ERROR;
    return zym_newBool(p.is_relative_path());
}

static ZymValue p_isNetworkShare(ZymVM* vm, ZymValue, ZymValue pV) {
    String p; if (!reqString(vm, pV, "Path.isNetworkShare(p)", &p)) return ZYM_ERROR;
    return zym_newBool(p.is_network_share_path());
}

// ---- split ----

static ZymValue p_dirname(ZymVM* vm, ZymValue, ZymValue pV) {
    String p; if (!reqString(vm, pV, "Path.dirname(p)", &p)) return ZYM_ERROR;
    return stringToZym(vm, p.get_base_dir());
}

static ZymValue p_filename(ZymVM* vm, ZymValue, ZymValue pV) {
    String p; if (!reqString(vm, pV, "Path.filename(p)", &p)) return ZYM_ERROR;
    return stringToZym(vm, p.get_file());
}

static ZymValue p_extension(ZymVM* vm, ZymValue, ZymValue pV) {
    String p; if (!reqString(vm, pV, "Path.extension(p)", &p)) return ZYM_ERROR;
    return stringToZym(vm, p.get_extension());
}

// stem = trailing component minus extension. Godot's `get_basename`
// returns the *whole path* without extension, which is not what most
// scripts mean by "stem"; we apply get_file first to get just the
// trailing component, then strip the extension.
static ZymValue p_stem(ZymVM* vm, ZymValue, ZymValue pV) {
    String p; if (!reqString(vm, pV, "Path.stem(p)", &p)) return ZYM_ERROR;
    return stringToZym(vm, p.get_file().get_basename());
}

// basename = whole path minus extension (matches Godot's `get_basename`).
// We expose this under the explicit name so users who want the "path
// without extension" form have it without the standard-vocabulary clash.
static ZymValue p_basename(ZymVM* vm, ZymValue, ZymValue pV) {
    String p; if (!reqString(vm, pV, "Path.basename(p)", &p)) return ZYM_ERROR;
    return stringToZym(vm, p.get_basename());
}

// ---- build ----

static ZymValue p_join(ZymVM* vm, ZymValue, ZymValue* vargs, int vargc) {
    if (vargc == 0) return stringToZym(vm, String());
    String acc;
    if (!reqString(vm, vargs[0], "Path.join(...)", &acc)) return ZYM_ERROR;
    for (int i = 1; i < vargc; i++) {
        String next;
        if (!reqString(vm, vargs[i], "Path.join(...)", &next)) return ZYM_ERROR;
        if (next.is_empty()) continue;
        if (next.is_absolute_path()) {
            // path_join would still smash the leading separator; if a
            // later segment is itself absolute, rebase on it (matches
            // posix `os.path.join` / `Path` semantics).
            acc = next;
            continue;
        }
        acc = acc.is_empty() ? next : acc.path_join(next);
    }
    return stringToZym(vm, acc);
}

static ZymValue p_normalize(ZymVM* vm, ZymValue, ZymValue pV) {
    String p; if (!reqString(vm, pV, "Path.normalize(p)", &p)) return ZYM_ERROR;
    return stringToZym(vm, p.simplify_path());
}

static ZymValue p_relative(ZymVM* vm, ZymValue, ZymValue fromV, ZymValue toV) {
    String from, to;
    if (!reqString(vm, fromV, "Path.relative(from, to)", &from)) return ZYM_ERROR;
    if (!reqString(vm, toV,   "Path.relative(from, to)", &to))   return ZYM_ERROR;
    // Godot's `path_to` produces a relative path between two directory
    // paths; `path_to_file` does the same but treats `to` as a file
    // (so its trailing component is preserved). For a generic
    // "relative path between two locations" we use `path_to_file`,
    // which is the form scripts almost always want.
    return stringToZym(vm, from.path_to_file(to));
}

static ZymValue p_expandUser(ZymVM* vm, ZymValue, ZymValue pV) {
    String p; if (!reqString(vm, pV, "Path.expandUser(p)", &p)) return ZYM_ERROR;
    if (p.is_empty() || !p.begins_with("~")) return stringToZym(vm, p);

    // Only `~` and `~/...` are expanded; `~user/...` is not (Godot has
    // no equivalent and we don't want to shell out to getpwnam).
    if (p.length() > 1 && p[1] != '/' && p[1] != '\\') {
        return stringToZym(vm, p);
    }

    String home = OS::get_singleton()->get_environment("HOME");
    if (home.is_empty()) {
        // Windows fallback: USERPROFILE, then HOMEDRIVE+HOMEPATH.
        home = OS::get_singleton()->get_environment("USERPROFILE");
        if (home.is_empty()) {
            String drive = OS::get_singleton()->get_environment("HOMEDRIVE");
            String hpath = OS::get_singleton()->get_environment("HOMEPATH");
            if (!drive.is_empty() || !hpath.is_empty()) home = drive + hpath;
        }
    }
    if (home.is_empty()) return stringToZym(vm, p);

    if (p.length() == 1) return stringToZym(vm, home);
    // `p[1]` is '/' or '\\'; skip it so path_join doesn't double-up.
    String tail = p.substr(2, p.length() - 2);
    return stringToZym(vm, tail.is_empty() ? home : home.path_join(tail));
}

// withExtension(p, ext): replace (or append) the trailing extension.
// `ext` may be given with or without a leading dot; both are accepted.
// An empty string drops the extension entirely.
static ZymValue p_withExtension(ZymVM* vm, ZymValue, ZymValue pV, ZymValue eV) {
    String p, ext;
    if (!reqString(vm, pV, "Path.withExtension(p, ext)", &p))   return ZYM_ERROR;
    if (!reqString(vm, eV, "Path.withExtension(p, ext)", &ext)) return ZYM_ERROR;
    String stem = p.get_basename();
    if (ext.is_empty()) return stringToZym(vm, stem);
    if (ext.begins_with(".")) ext = ext.substr(1, ext.length() - 1);
    return stringToZym(vm, stem + "." + ext);
}

// separator(): "/" on posix, "\\" on Windows. Implemented at compile
// time so it follows whichever host the binary was built for.
static ZymValue p_separator(ZymVM* vm, ZymValue) {
#if defined(WINDOWS_ENABLED)
    return stringToZym(vm, String("\\"));
#else
    return stringToZym(vm, String("/"));
#endif
}

// ---- assembly ----

ZymValue nativePath_create(ZymVM* vm) {
    ZymValue context = zym_createNativeContext(vm, nullptr, nullptr);
    zym_pushRoot(vm, context);

#define M(name, sig, fn) \
    ZymValue name = zym_createNativeClosure(vm, sig, (void*)fn, context); \
    zym_pushRoot(vm, name);
#define MV(name, sig, fn) \
    ZymValue name = zym_createNativeClosureVariadic(vm, sig, (void*)fn, context); \
    zym_pushRoot(vm, name);

    M (isAbsolute,     "isAbsolute(p)",        p_isAbsolute)
    M (isRelative,     "isRelative(p)",        p_isRelative)
    M (isNetworkShare, "isNetworkShare(p)",    p_isNetworkShare)
    M (dirname,        "dirname(p)",           p_dirname)
    M (filename,       "filename(p)",          p_filename)
    M (extension,      "extension(p)",         p_extension)
    M (stem,           "stem(p)",              p_stem)
    M (basename,       "basename(p)",          p_basename)
    MV(join,           "join(...)",            p_join)
    M (normalize,      "normalize(p)",         p_normalize)
    M (relative,       "relative(from, to)",   p_relative)
    M (expandUser,     "expandUser(p)",        p_expandUser)
    M (withExtension,  "withExtension(p, ext)",p_withExtension)
    M (separator,      "separator()",          p_separator)

#undef M
#undef MV

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

    zym_mapSet(vm, obj, "isAbsolute",     isAbsolute);
    zym_mapSet(vm, obj, "isRelative",     isRelative);
    zym_mapSet(vm, obj, "isNetworkShare", isNetworkShare);
    zym_mapSet(vm, obj, "dirname",        dirname);
    zym_mapSet(vm, obj, "filename",       filename);
    zym_mapSet(vm, obj, "extension",      extension);
    zym_mapSet(vm, obj, "stem",           stem);
    zym_mapSet(vm, obj, "basename",       basename);
    zym_mapSet(vm, obj, "join",           join);
    zym_mapSet(vm, obj, "normalize",      normalize);
    zym_mapSet(vm, obj, "relative",       relative);
    zym_mapSet(vm, obj, "expandUser",     expandUser);
    zym_mapSet(vm, obj, "withExtension",  withExtension);
    zym_mapSet(vm, obj, "separator",      separator);

    // context + 14 methods + obj = 16
    for (int i = 0; i < 16; i++) zym_popRoot(vm);
    return obj;
}
