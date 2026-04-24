// Godot-backed Dir native. Static helpers + handle-based directory access.
#include "core/io/dir_access.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"

#include "natives.hpp"

// ---- helpers ----

struct DirHandle {
    Ref<DirAccess> d;
};

static void daFinalizer(ZymVM*, void* data) {
    delete static_cast<DirHandle*>(data);
}

static DirHandle* unwrapDA(ZymValue ctx) {
    return static_cast<DirHandle*>(zym_getNativeData(ctx));
}

static ZymValue strZ(ZymVM* vm, const String& s) {
    CharString u = s.utf8();
    return zym_newStringN(vm, u.get_data(), u.length());
}

static bool reqStr(ZymVM* vm, ZymValue v, const char* where, String* out) {
    if (!zym_isString(v)) { zym_runtimeError(vm, "%s expects a string", where); return false; }
    *out = String::utf8(zym_asCString(v)); return true;
}
static bool reqInt(ZymVM* vm, ZymValue v, const char* where, int64_t* out) {
    if (!zym_isNumber(v)) { zym_runtimeError(vm, "%s expects a number", where); return false; }
    *out = (int64_t)zym_asNumber(v); return true;
}
static bool reqBool(ZymVM* vm, ZymValue v, const char* where, bool* out) {
    if (!zym_isBool(v)) { zym_runtimeError(vm, "%s expects a bool", where); return false; }
    *out = zym_asBool(v); return true;
}

// Build a Zym list from a PackedStringArray.
static ZymValue pakToList(ZymVM* vm, const PackedStringArray& src) {
    ZymValue list = zym_newList(vm);
    zym_pushRoot(vm, list);
    for (int i = 0; i < src.size(); i++) {
        ZymValue s = strZ(vm, src[i]);
        zym_pushRoot(vm, s);
        zym_listAppend(vm, list, s);
        zym_popRoot(vm);
    }
    zym_popRoot(vm);
    return list;
}

// ---- handle requirement ----

static bool reqOpen(ZymVM* vm, ZymValue ctx, const char* where, Ref<DirAccess>* out) {
    DirHandle* h = unwrapDA(ctx);
    if (h == nullptr || h->d.is_null()) {
        zym_runtimeError(vm, "%s: dir is not open", where);
        return false;
    }
    *out = h->d;
    return true;
}

// ---- instance methods ----

static ZymValue i_path(ZymVM* vm, ZymValue ctx) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.path()", &d)) return ZYM_ERROR;
    return strZ(vm, d->get_current_dir(true));
}

static ZymValue i_changeDir(ZymVM* vm, ZymValue ctx, ZymValue pathV) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.changeDir(path)", &d)) return ZYM_ERROR;
    String path; if (!reqStr(vm, pathV, "Dir.changeDir(path)", &path)) return ZYM_ERROR;
    return zym_newBool(d->change_dir(path) == OK);
}

static ZymValue i_fileExists(ZymVM* vm, ZymValue ctx, ZymValue nameV) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.fileExists(name)", &d)) return ZYM_ERROR;
    String name; if (!reqStr(vm, nameV, "Dir.fileExists(name)", &name)) return ZYM_ERROR;
    return zym_newBool(d->file_exists(name));
}

static ZymValue i_dirExists(ZymVM* vm, ZymValue ctx, ZymValue nameV) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.dirExists(name)", &d)) return ZYM_ERROR;
    String name; if (!reqStr(vm, nameV, "Dir.dirExists(name)", &name)) return ZYM_ERROR;
    return zym_newBool(d->dir_exists(name));
}

static ZymValue i_isReadable(ZymVM* vm, ZymValue ctx, ZymValue nameV) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.isReadable(name)", &d)) return ZYM_ERROR;
    String name; if (!reqStr(vm, nameV, "Dir.isReadable(name)", &name)) return ZYM_ERROR;
    return zym_newBool(d->is_readable(name));
}

static ZymValue i_isWritable(ZymVM* vm, ZymValue ctx, ZymValue nameV) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.isWritable(name)", &d)) return ZYM_ERROR;
    String name; if (!reqStr(vm, nameV, "Dir.isWritable(name)", &name)) return ZYM_ERROR;
    return zym_newBool(d->is_writable(name));
}

static ZymValue i_spaceLeft(ZymVM* vm, ZymValue ctx) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.spaceLeft()", &d)) return ZYM_ERROR;
    return zym_newNumber((double)d->get_space_left());
}

static ZymValue i_makeDir(ZymVM* vm, ZymValue ctx, ZymValue pathV) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.makeDir(path)", &d)) return ZYM_ERROR;
    String path; if (!reqStr(vm, pathV, "Dir.makeDir(path)", &path)) return ZYM_ERROR;
    return zym_newBool(d->make_dir(path) == OK);
}

static ZymValue i_makeDirRecursive(ZymVM* vm, ZymValue ctx, ZymValue pathV) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.makeDirRecursive(path)", &d)) return ZYM_ERROR;
    String path; if (!reqStr(vm, pathV, "Dir.makeDirRecursive(path)", &path)) return ZYM_ERROR;
    return zym_newBool(d->make_dir_recursive(path) == OK);
}

static ZymValue i_copy(ZymVM* vm, ZymValue ctx, ZymValue srcV, ZymValue dstV) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.copy(src, dst)", &d)) return ZYM_ERROR;
    String src; if (!reqStr(vm, srcV, "Dir.copy(src, dst)", &src)) return ZYM_ERROR;
    String dst; if (!reqStr(vm, dstV, "Dir.copy(src, dst)", &dst)) return ZYM_ERROR;
    return zym_newBool(d->copy(src, dst) == OK);
}

static ZymValue i_rename(ZymVM* vm, ZymValue ctx, ZymValue srcV, ZymValue dstV) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.rename(src, dst)", &d)) return ZYM_ERROR;
    String src; if (!reqStr(vm, srcV, "Dir.rename(src, dst)", &src)) return ZYM_ERROR;
    String dst; if (!reqStr(vm, dstV, "Dir.rename(src, dst)", &dst)) return ZYM_ERROR;
    return zym_newBool(d->rename(src, dst) == OK);
}

static ZymValue i_remove(ZymVM* vm, ZymValue ctx, ZymValue nameV) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.remove(name)", &d)) return ZYM_ERROR;
    String name; if (!reqStr(vm, nameV, "Dir.remove(name)", &name)) return ZYM_ERROR;
    return zym_newBool(d->remove(name) == OK);
}

static ZymValue i_eraseContentsRecursive(ZymVM* vm, ZymValue ctx) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.eraseContentsRecursive()", &d)) return ZYM_ERROR;
    return zym_newBool(d->erase_contents_recursive() == OK);
}

static ZymValue i_files(ZymVM* vm, ZymValue ctx) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.files()", &d)) return ZYM_ERROR;
    return pakToList(vm, d->get_files());
}

static ZymValue i_directories(ZymVM* vm, ZymValue ctx) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.directories()", &d)) return ZYM_ERROR;
    return pakToList(vm, d->get_directories());
}

static ZymValue i_listBegin(ZymVM* vm, ZymValue ctx) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.listBegin()", &d)) return ZYM_ERROR;
    return zym_newBool(d->list_dir_begin() == OK);
}

static ZymValue i_listNext(ZymVM* vm, ZymValue ctx) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.listNext()", &d)) return ZYM_ERROR;
    String n = d->get_next();
    if (n.is_empty()) return zym_newNull();
    return strZ(vm, n);
}

static ZymValue i_listCurrentIsDir(ZymVM* vm, ZymValue ctx) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.listCurrentIsDir()", &d)) return ZYM_ERROR;
    return zym_newBool(d->current_is_dir());
}

static ZymValue i_listCurrentIsHidden(ZymVM* vm, ZymValue ctx) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.listCurrentIsHidden()", &d)) return ZYM_ERROR;
    return zym_newBool(d->current_is_hidden());
}

static ZymValue i_listEnd(ZymVM* vm, ZymValue ctx) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.listEnd()", &d)) return ZYM_ERROR;
    d->list_dir_end();
    return zym_newNull();
}

static ZymValue i_isLink(ZymVM* vm, ZymValue ctx, ZymValue nameV) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.isLink(name)", &d)) return ZYM_ERROR;
    String name; if (!reqStr(vm, nameV, "Dir.isLink(name)", &name)) return ZYM_ERROR;
    return zym_newBool(d->is_link(name));
}

static ZymValue i_readLink(ZymVM* vm, ZymValue ctx, ZymValue nameV) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.readLink(name)", &d)) return ZYM_ERROR;
    String name; if (!reqStr(vm, nameV, "Dir.readLink(name)", &name)) return ZYM_ERROR;
    return strZ(vm, d->read_link(name));
}

static ZymValue i_createLink(ZymVM* vm, ZymValue ctx, ZymValue srcV, ZymValue dstV) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.createLink(src, dst)", &d)) return ZYM_ERROR;
    String src; if (!reqStr(vm, srcV, "Dir.createLink(src, dst)", &src)) return ZYM_ERROR;
    String dst; if (!reqStr(vm, dstV, "Dir.createLink(src, dst)", &dst)) return ZYM_ERROR;
    return zym_newBool(d->create_link(src, dst) == OK);
}

static ZymValue i_setIncludeNavigational(ZymVM* vm, ZymValue ctx, ZymValue v) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.setIncludeNavigational(b)", &d)) return ZYM_ERROR;
    bool b; if (!reqBool(vm, v, "Dir.setIncludeNavigational(b)", &b)) return ZYM_ERROR;
    d->set_include_navigational(b);
    return zym_newNull();
}

static ZymValue i_setIncludeHidden(ZymVM* vm, ZymValue ctx, ZymValue v) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.setIncludeHidden(b)", &d)) return ZYM_ERROR;
    bool b; if (!reqBool(vm, v, "Dir.setIncludeHidden(b)", &b)) return ZYM_ERROR;
    d->set_include_hidden(b);
    return zym_newNull();
}

static ZymValue i_includeNavigational(ZymVM* vm, ZymValue ctx) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.includeNavigational()", &d)) return ZYM_ERROR;
    return zym_newBool(d->get_include_navigational());
}

static ZymValue i_includeHidden(ZymVM* vm, ZymValue ctx) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.includeHidden()", &d)) return ZYM_ERROR;
    return zym_newBool(d->get_include_hidden());
}

static ZymValue i_isCaseSensitive(ZymVM* vm, ZymValue ctx, ZymValue pathV) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.isCaseSensitive(path)", &d)) return ZYM_ERROR;
    String path; if (!reqStr(vm, pathV, "Dir.isCaseSensitive(path)", &path)) return ZYM_ERROR;
    return zym_newBool(d->is_case_sensitive(path));
}

static ZymValue i_filesystemType(ZymVM* vm, ZymValue ctx) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.filesystemType()", &d)) return ZYM_ERROR;
    return strZ(vm, d->get_filesystem_type());
}

static ZymValue i_driveCount(ZymVM* vm, ZymValue ctx) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.driveCount()", &d)) return ZYM_ERROR;
    return zym_newNumber((double)d->get_drive_count());
}

static ZymValue i_drive(ZymVM* vm, ZymValue ctx, ZymValue idxV) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.drive(idx)", &d)) return ZYM_ERROR;
    int64_t idx; if (!reqInt(vm, idxV, "Dir.drive(idx)", &idx)) return ZYM_ERROR;
    return strZ(vm, d->get_drive((int)idx));
}

static ZymValue i_currentDrive(ZymVM* vm, ZymValue ctx) {
    Ref<DirAccess> d; if (!reqOpen(vm, ctx, "Dir.currentDrive()", &d)) return ZYM_ERROR;
    return zym_newNumber((double)d->get_current_drive());
}

// ---- instance assembly ----

static ZymValue makeDirInstance(ZymVM* vm, Ref<DirAccess> da) {
    auto* data = new DirHandle{ da };
    ZymValue ctx = zym_createNativeContext(vm, data, daFinalizer);
    zym_pushRoot(vm, ctx);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__da__", ctx);

#define M(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctx); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)

    M("path",                    "path()",                      i_path);
    M("changeDir",               "changeDir(path)",             i_changeDir);

    M("fileExists",              "fileExists(name)",            i_fileExists);
    M("dirExists",               "dirExists(name)",             i_dirExists);
    M("isReadable",              "isReadable(name)",            i_isReadable);
    M("isWritable",              "isWritable(name)",            i_isWritable);
    M("spaceLeft",               "spaceLeft()",                 i_spaceLeft);

    M("makeDir",                 "makeDir(path)",               i_makeDir);
    M("makeDirRecursive",        "makeDirRecursive(path)",      i_makeDirRecursive);
    M("copy",                    "copy(src, dst)",              i_copy);
    M("rename",                  "rename(src, dst)",            i_rename);
    M("remove",                  "remove(name)",                i_remove);
    M("eraseContentsRecursive",  "eraseContentsRecursive()",    i_eraseContentsRecursive);

    M("files",                   "files()",                     i_files);
    M("directories",             "directories()",               i_directories);

    M("listBegin",               "listBegin()",                 i_listBegin);
    M("listNext",                "listNext()",                  i_listNext);
    M("listCurrentIsDir",        "listCurrentIsDir()",          i_listCurrentIsDir);
    M("listCurrentIsHidden",     "listCurrentIsHidden()",       i_listCurrentIsHidden);
    M("listEnd",                 "listEnd()",                   i_listEnd);

    M("isLink",                  "isLink(name)",                i_isLink);
    M("readLink",                "readLink(name)",              i_readLink);
    M("createLink",              "createLink(src, dst)",        i_createLink);

    M("setIncludeNavigational",  "setIncludeNavigational(b)",   i_setIncludeNavigational);
    M("setIncludeHidden",        "setIncludeHidden(b)",         i_setIncludeHidden);
    M("includeNavigational",     "includeNavigational()",       i_includeNavigational);
    M("includeHidden",           "includeHidden()",             i_includeHidden);

    M("isCaseSensitive",         "isCaseSensitive(path)",       i_isCaseSensitive);
    M("filesystemType",          "filesystemType()",            i_filesystemType);

    M("driveCount",              "driveCount()",                i_driveCount);
    M("drive",                   "drive(idx)",                  i_drive);
    M("currentDrive",            "currentDrive()",              i_currentDrive);

#undef M

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}

// ---- Dir global (statics) ----

static ZymValue d_open(ZymVM* vm, ZymValue, ZymValue pathV) {
    String path; if (!reqStr(vm, pathV, "Dir.open(path)", &path)) return ZYM_ERROR;
    Error err = OK;
    Ref<DirAccess> da = DirAccess::open(path, &err);
    if (da.is_null() || err != OK) return zym_newNull();
    return makeDirInstance(vm, da);
}

static ZymValue d_openTemp(ZymVM* vm, ZymValue, ZymValue prefixV, ZymValue keepV) {
    String prefix; if (!reqStr(vm, prefixV, "Dir.openTemp(prefix, keep)", &prefix)) return ZYM_ERROR;
    bool keep; if (!reqBool(vm, keepV, "Dir.openTemp(prefix, keep)", &keep)) return ZYM_ERROR;
    Error err = OK;
    Ref<DirAccess> da = DirAccess::create_temp(prefix, keep, &err);
    if (da.is_null() || err != OK) return zym_newNull();
    return makeDirInstance(vm, da);
}

static ZymValue d_exists(ZymVM* vm, ZymValue, ZymValue pathV) {
    String path; if (!reqStr(vm, pathV, "Dir.exists(path)", &path)) return ZYM_ERROR;
    return zym_newBool(DirAccess::dir_exists_absolute(path));
}

static ZymValue d_makeDir(ZymVM* vm, ZymValue, ZymValue pathV) {
    String path; if (!reqStr(vm, pathV, "Dir.makeDir(path)", &path)) return ZYM_ERROR;
    return zym_newBool(DirAccess::make_dir_absolute(path) == OK);
}

static ZymValue d_makeDirRecursive(ZymVM* vm, ZymValue, ZymValue pathV) {
    String path; if (!reqStr(vm, pathV, "Dir.makeDirRecursive(path)", &path)) return ZYM_ERROR;
    return zym_newBool(DirAccess::make_dir_recursive_absolute(path) == OK);
}

static ZymValue d_copy(ZymVM* vm, ZymValue, ZymValue srcV, ZymValue dstV) {
    String src; if (!reqStr(vm, srcV, "Dir.copy(src, dst)", &src)) return ZYM_ERROR;
    String dst; if (!reqStr(vm, dstV, "Dir.copy(src, dst)", &dst)) return ZYM_ERROR;
    return zym_newBool(DirAccess::copy_absolute(src, dst) == OK);
}

static ZymValue d_rename(ZymVM* vm, ZymValue, ZymValue srcV, ZymValue dstV) {
    String src; if (!reqStr(vm, srcV, "Dir.rename(src, dst)", &src)) return ZYM_ERROR;
    String dst; if (!reqStr(vm, dstV, "Dir.rename(src, dst)", &dst)) return ZYM_ERROR;
    return zym_newBool(DirAccess::rename_absolute(src, dst) == OK);
}

static ZymValue d_remove(ZymVM* vm, ZymValue, ZymValue pathV) {
    String path; if (!reqStr(vm, pathV, "Dir.remove(path)", &path)) return ZYM_ERROR;
    return zym_newBool(DirAccess::remove_absolute(path) == OK);
}

static ZymValue d_files(ZymVM* vm, ZymValue, ZymValue pathV) {
    String path; if (!reqStr(vm, pathV, "Dir.files(path)", &path)) return ZYM_ERROR;
    return pakToList(vm, DirAccess::get_files_at(path));
}

static ZymValue d_directories(ZymVM* vm, ZymValue, ZymValue pathV) {
    String path; if (!reqStr(vm, pathV, "Dir.directories(path)", &path)) return ZYM_ERROR;
    return pakToList(vm, DirAccess::get_directories_at(path));
}

static ZymValue d_driveCount(ZymVM* vm, ZymValue) {
    return zym_newNumber((double)DirAccess::_get_drive_count());
}

static ZymValue d_driveName(ZymVM* vm, ZymValue, ZymValue idxV) {
    int64_t idx; if (!reqInt(vm, idxV, "Dir.driveName(idx)", &idx)) return ZYM_ERROR;
    return strZ(vm, DirAccess::get_drive_name((int)idx));
}

// ---- factory ----

ZymValue nativeDir_create(ZymVM* vm) {
    ZymValue ctx = zym_createNativeContext(vm, nullptr, nullptr);
    zym_pushRoot(vm, ctx);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

#define F(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctx); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)

    F("open",             "open(path)",             d_open);
    F("openTemp",         "openTemp(prefix, keep)", d_openTemp);

    F("exists",           "exists(path)",           d_exists);
    F("makeDir",          "makeDir(path)",          d_makeDir);
    F("makeDirRecursive", "makeDirRecursive(path)", d_makeDirRecursive);
    F("copy",             "copy(src, dst)",         d_copy);
    F("rename",           "rename(src, dst)",       d_rename);
    F("remove",           "remove(path)",           d_remove);

    F("files",            "files(path)",            d_files);
    F("directories",      "directories(path)",      d_directories);

    F("driveCount",       "driveCount()",           d_driveCount);
    F("driveName",        "driveName(idx)",         d_driveName);

#undef F

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}
