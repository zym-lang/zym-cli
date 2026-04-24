// Godot-backed File native. Static helpers + handle-based file I/O.
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"

#include "natives.hpp"

// ---- helpers ----

struct FileHandle {
    Ref<FileAccess> f;
};

static void faFinalizer(ZymVM*, void* data) {
    delete static_cast<FileHandle*>(data);
}

static FileHandle* unwrapFA(ZymValue ctx) {
    return static_cast<FileHandle*>(zym_getNativeData(ctx));
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

// Resolve a Buffer arg (map with __pba__ context) -> PackedByteArray*.
static bool reqBufferArg(ZymVM* vm, ZymValue v, const char* where, PackedByteArray** out) {
    if (zym_isMap(v)) {
        ZymValue ctx = zym_mapGet(vm, v, "__pba__");
        if (ctx != ZYM_ERROR) {
            void* data = zym_getNativeData(ctx);
            if (data) { *out = static_cast<PackedByteArray*>(data); return true; }
        }
    }
    zym_runtimeError(vm, "%s expects a Buffer", where);
    return false;
}

// Build a fresh Buffer instance from a PackedByteArray (defined in buffer.cpp).
extern ZymValue makeBufferInstance(ZymVM* vm, const PackedByteArray& src);

static int modeFromString(const String& m) {
    if (m == "r")  return FileAccess::READ;
    if (m == "w")  return FileAccess::WRITE;
    if (m == "rw") return FileAccess::READ_WRITE;
    if (m == "wr") return FileAccess::WRITE_READ;
    return -1;
}

static int compressionFromString(const String& a) {
    if (a == "fastlz")  return FileAccess::COMPRESSION_FASTLZ;
    if (a == "deflate") return FileAccess::COMPRESSION_DEFLATE;
    if (a == "zstd")    return FileAccess::COMPRESSION_ZSTD;
    if (a == "gzip")    return FileAccess::COMPRESSION_GZIP;
    if (a == "brotli")  return FileAccess::COMPRESSION_BROTLI;
    return -1;
}

// Read optional endian override from a variadic tail. Returns -1 to mean
// "leave handle's current setting", 0 = little, 1 = big.
static int readEndianOpt(ZymVM* vm, const char* where, ZymValue* vargs, int vargc) {
    if (vargc <= 0) return -1;
    if (!zym_isString(vargs[0])) { zym_runtimeError(vm, "%s: endian must be a string", where); return -2; }
    const char* s = zym_asCString(vargs[0]);
    if (s[0] == 'l' && s[1] == 'e' && s[2] == 0) return 0;
    if (s[0] == 'b' && s[1] == 'e' && s[2] == 0) return 1;
    zym_runtimeError(vm, "%s: endian must be \"le\" or \"be\"", where);
    return -2;
}

// RAII scoped endian override; restores on destruction.
struct ScopedEndian {
    Ref<FileAccess>* fa; bool prev; bool touched;
    ScopedEndian(Ref<FileAccess>* p, int want) : fa(p), prev((*p)->is_big_endian()), touched(false) {
        if (want >= 0) { (*fa)->set_big_endian(want == 1); touched = true; }
    }
    ~ScopedEndian() { if (touched) (*fa)->set_big_endian(prev); }
};

// Forward declare instance builder.
static ZymValue makeFileInstance(ZymVM* vm, Ref<FileAccess> fa);

// ---- handle requirement ----

static bool reqOpen(ZymVM* vm, ZymValue ctx, const char* where, Ref<FileAccess>* out) {
    FileHandle* h = unwrapFA(ctx);
    if (h == nullptr || h->f.is_null() || !h->f->is_open()) {
        zym_runtimeError(vm, "%s: file is not open", where);
        return false;
    }
    *out = h->f;
    return true;
}

// ---- instance methods ----

static ZymValue i_isOpen(ZymVM*, ZymValue ctx) {
    FileHandle* h = unwrapFA(ctx);
    return zym_newBool(h != nullptr && h->f.is_valid() && h->f->is_open());
}

static ZymValue i_close(ZymVM*, ZymValue ctx) {
    FileHandle* h = unwrapFA(ctx);
    if (h && h->f.is_valid() && h->f->is_open()) h->f->close();
    return zym_newNull();
}

static ZymValue i_path(ZymVM* vm, ZymValue ctx) {
    FileHandle* h = unwrapFA(ctx);
    if (!h || h->f.is_null()) return zym_newString(vm, "");
    return strZ(vm, h->f->get_path());
}

static ZymValue i_pathAbsolute(ZymVM* vm, ZymValue ctx) {
    FileHandle* h = unwrapFA(ctx);
    if (!h || h->f.is_null()) return zym_newString(vm, "");
    return strZ(vm, h->f->get_path_absolute());
}

static ZymValue i_length(ZymVM* vm, ZymValue ctx) {
    Ref<FileAccess> f; if (!reqOpen(vm, ctx, "File.length()", &f)) return ZYM_ERROR;
    return zym_newNumber((double)f->get_length());
}

static ZymValue i_position(ZymVM* vm, ZymValue ctx) {
    Ref<FileAccess> f; if (!reqOpen(vm, ctx, "File.position()", &f)) return ZYM_ERROR;
    return zym_newNumber((double)f->get_position());
}

static ZymValue i_seek(ZymVM* vm, ZymValue ctx, ZymValue posV) {
    Ref<FileAccess> f; if (!reqOpen(vm, ctx, "File.seek(pos)", &f)) return ZYM_ERROR;
    int64_t p; if (!reqInt(vm, posV, "File.seek(pos)", &p)) return ZYM_ERROR;
    if (p < 0) { zym_runtimeError(vm, "File.seek(pos): pos must be >= 0"); return ZYM_ERROR; }
    f->seek((uint64_t)p);
    return zym_newNull();
}

static ZymValue i_seekEnd(ZymVM* vm, ZymValue ctx, ZymValue offV) {
    Ref<FileAccess> f; if (!reqOpen(vm, ctx, "File.seekEnd(off)", &f)) return ZYM_ERROR;
    int64_t off; if (!reqInt(vm, offV, "File.seekEnd(off)", &off)) return ZYM_ERROR;
    f->seek_end(off);
    return zym_newNull();
}

static ZymValue i_eof(ZymVM* vm, ZymValue ctx) {
    Ref<FileAccess> f; if (!reqOpen(vm, ctx, "File.eof()", &f)) return ZYM_ERROR;
    return zym_newBool(f->eof_reached());
}

static ZymValue i_flush(ZymVM* vm, ZymValue ctx) {
    Ref<FileAccess> f; if (!reqOpen(vm, ctx, "File.flush()", &f)) return ZYM_ERROR;
    f->flush();
    return zym_newNull();
}

static ZymValue i_resize(ZymVM* vm, ZymValue ctx, ZymValue nv) {
    Ref<FileAccess> f; if (!reqOpen(vm, ctx, "File.resize(n)", &f)) return ZYM_ERROR;
    int64_t n; if (!reqInt(vm, nv, "File.resize(n)", &n)) return ZYM_ERROR;
    if (n < 0) { zym_runtimeError(vm, "File.resize(n): n must be >= 0"); return ZYM_ERROR; }
    return zym_newNumber((double)f->resize(n));
}

static ZymValue i_getError(ZymVM* vm, ZymValue ctx) {
    FileHandle* h = unwrapFA(ctx);
    if (!h || h->f.is_null()) return zym_newNumber(0);
    return zym_newNumber((double)h->f->get_error());
}

static ZymValue i_setBigEndian(ZymVM* vm, ZymValue ctx, ZymValue bv) {
    Ref<FileAccess> f; if (!reqOpen(vm, ctx, "File.setBigEndian(b)", &f)) return ZYM_ERROR;
    bool b; if (!reqBool(vm, bv, "File.setBigEndian(b)", &b)) return ZYM_ERROR;
    f->set_big_endian(b);
    return zym_newNull();
}

static ZymValue i_isBigEndian(ZymVM* vm, ZymValue ctx) {
    Ref<FileAccess> f; if (!reqOpen(vm, ctx, "File.isBigEndian()", &f)) return ZYM_ERROR;
    return zym_newBool(f->is_big_endian());
}

// ---- block I/O ----

static ZymValue i_readBytes(ZymVM* vm, ZymValue ctx, ZymValue nv) {
    Ref<FileAccess> f; if (!reqOpen(vm, ctx, "File.readBytes(n)", &f)) return ZYM_ERROR;
    int64_t n; if (!reqInt(vm, nv, "File.readBytes(n)", &n)) return ZYM_ERROR;
    if (n < 0) { zym_runtimeError(vm, "File.readBytes(n): n must be >= 0"); return ZYM_ERROR; }
    PackedByteArray out = f->get_buffer((int64_t)n);
    return makeBufferInstance(vm, out);
}

static ZymValue i_writeBytes(ZymVM* vm, ZymValue ctx, ZymValue bufV) {
    Ref<FileAccess> f; if (!reqOpen(vm, ctx, "File.writeBytes(buf)", &f)) return ZYM_ERROR;
    PackedByteArray* buf; if (!reqBufferArg(vm, bufV, "File.writeBytes(buf)", &buf)) return ZYM_ERROR;
    return zym_newBool(f->store_buffer(*buf));
}

// ---- text I/O ----

static ZymValue i_readText(ZymVM* vm, ZymValue ctx) {
    Ref<FileAccess> f; if (!reqOpen(vm, ctx, "File.readText()", &f)) return ZYM_ERROR;
    return strZ(vm, f->get_as_text());
}

static ZymValue i_readLine(ZymVM* vm, ZymValue ctx) {
    Ref<FileAccess> f; if (!reqOpen(vm, ctx, "File.readLine()", &f)) return ZYM_ERROR;
    return strZ(vm, f->get_line());
}

static ZymValue i_readCSVLine(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) {
    Ref<FileAccess> f; if (!reqOpen(vm, ctx, "File.readCSVLine(...)", &f)) return ZYM_ERROR;
    String delim = ",";
    if (vargc > 0) {
        if (!zym_isString(vargs[0])) { zym_runtimeError(vm, "File.readCSVLine(...): delim must be a string"); return ZYM_ERROR; }
        delim = String::utf8(zym_asCString(vargs[0]));
    }
    Vector<String> parts = f->get_csv_line(delim);
    ZymValue list = zym_newList(vm);
    zym_pushRoot(vm, list);
    for (int i = 0; i < parts.size(); i++) {
        zym_listAppend(vm, list, strZ(vm, parts[i]));
    }
    zym_popRoot(vm);
    return list;
}

static ZymValue i_writeString(ZymVM* vm, ZymValue ctx, ZymValue sv) {
    Ref<FileAccess> f; if (!reqOpen(vm, ctx, "File.writeString(s)", &f)) return ZYM_ERROR;
    String s; if (!reqStr(vm, sv, "File.writeString(s)", &s)) return ZYM_ERROR;
    return zym_newBool(f->store_string(s));
}

static ZymValue i_writeLine(ZymVM* vm, ZymValue ctx, ZymValue sv) {
    Ref<FileAccess> f; if (!reqOpen(vm, ctx, "File.writeLine(s)", &f)) return ZYM_ERROR;
    String s; if (!reqStr(vm, sv, "File.writeLine(s)", &s)) return ZYM_ERROR;
    return zym_newBool(f->store_line(s));
}

static ZymValue i_writeCSVLine(ZymVM* vm, ZymValue ctx, ZymValue listV, ZymValue* vargs, int vargc) {
    Ref<FileAccess> f; if (!reqOpen(vm, ctx, "File.writeCSVLine(list, ...)", &f)) return ZYM_ERROR;
    if (!zym_isList(listV)) { zym_runtimeError(vm, "File.writeCSVLine(list, ...): expects a list"); return ZYM_ERROR; }
    String delim = ",";
    if (vargc > 0) {
        if (!zym_isString(vargs[0])) { zym_runtimeError(vm, "File.writeCSVLine(list, ...): delim must be a string"); return ZYM_ERROR; }
        delim = String::utf8(zym_asCString(vargs[0]));
    }
    int n = zym_listLength(listV);
    Vector<String> v;
    v.resize(n);
    for (int i = 0; i < n; i++) {
        ZymValue e = zym_listGet(vm, listV, i);
        if (!zym_isString(e)) { zym_runtimeError(vm, "File.writeCSVLine(list, ...): element %d is not a string", i); return ZYM_ERROR; }
        v.write[i] = String::utf8(zym_asCString(e));
    }
    return zym_newBool(f->store_csv_line(v, delim));
}

// ---- typed codec macros ----

#define READ_INT(name, width, getter, castTo) \
static ZymValue i_##name(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) { \
    (void)width; \
    Ref<FileAccess> f; if (!reqOpen(vm, ctx, "File." #name "(...)", &f)) return ZYM_ERROR; \
    int e = readEndianOpt(vm, "File." #name "(...)", vargs, vargc); \
    if (e == -2) return ZYM_ERROR; \
    ScopedEndian _s(&f, e); \
    return zym_newNumber((double)(castTo)f->getter()); \
}

READ_INT(readU8,  1, get_8,  uint8_t)
READ_INT(readI8,  1, get_8,  int8_t)
READ_INT(readU16, 2, get_16, uint16_t)
READ_INT(readI16, 2, get_16, int16_t)
READ_INT(readU32, 4, get_32, uint32_t)
READ_INT(readI32, 4, get_32, int32_t)
READ_INT(readU64, 8, get_64, uint64_t)
READ_INT(readI64, 8, get_64, int64_t)

#undef READ_INT

#define READ_FLOAT(name, getter) \
static ZymValue i_##name(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) { \
    Ref<FileAccess> f; if (!reqOpen(vm, ctx, "File." #name "(...)", &f)) return ZYM_ERROR; \
    int e = readEndianOpt(vm, "File." #name "(...)", vargs, vargc); \
    if (e == -2) return ZYM_ERROR; \
    ScopedEndian _s(&f, e); \
    return zym_newNumber((double)f->getter()); \
}

READ_FLOAT(readHalf,   get_half)
READ_FLOAT(readFloat,  get_float)
READ_FLOAT(readDouble, get_double)

#undef READ_FLOAT

#define WRITE_INT(name, storer, cast) \
static ZymValue i_##name(ZymVM* vm, ZymValue ctx, ZymValue valV, ZymValue* vargs, int vargc) { \
    Ref<FileAccess> f; if (!reqOpen(vm, ctx, "File." #name "(value, ...)", &f)) return ZYM_ERROR; \
    int64_t v; if (!reqInt(vm, valV, "File." #name "(value, ...)", &v)) return ZYM_ERROR; \
    int e = readEndianOpt(vm, "File." #name "(value, ...)", vargs, vargc); \
    if (e == -2) return ZYM_ERROR; \
    ScopedEndian _s(&f, e); \
    return zym_newBool(f->storer((cast)v)); \
}

WRITE_INT(writeU8,  store_8,  uint8_t)
WRITE_INT(writeI8,  store_8,  int8_t)
WRITE_INT(writeU16, store_16, uint16_t)
WRITE_INT(writeI16, store_16, int16_t)
WRITE_INT(writeU32, store_32, uint32_t)
WRITE_INT(writeI32, store_32, int32_t)
WRITE_INT(writeU64, store_64, uint64_t)
WRITE_INT(writeI64, store_64, int64_t)

#undef WRITE_INT

#define WRITE_FLOAT(name, storer, ftype) \
static ZymValue i_##name(ZymVM* vm, ZymValue ctx, ZymValue valV, ZymValue* vargs, int vargc) { \
    Ref<FileAccess> f; if (!reqOpen(vm, ctx, "File." #name "(value, ...)", &f)) return ZYM_ERROR; \
    if (!zym_isNumber(valV)) { zym_runtimeError(vm, "File." #name "(value, ...): value must be a number"); return ZYM_ERROR; } \
    int e = readEndianOpt(vm, "File." #name "(value, ...)", vargs, vargc); \
    if (e == -2) return ZYM_ERROR; \
    ScopedEndian _s(&f, e); \
    return zym_newBool(f->storer((ftype)zym_asNumber(valV))); \
}

WRITE_FLOAT(writeHalf,   store_half,   float)
WRITE_FLOAT(writeFloat,  store_float,  float)
WRITE_FLOAT(writeDouble, store_double, double)

#undef WRITE_FLOAT

// ---- instance assembly ----

static ZymValue makeFileInstance(ZymVM* vm, Ref<FileAccess> fa) {
    auto* data = new FileHandle{ fa };
    ZymValue ctx = zym_createNativeContext(vm, data, faFinalizer);
    zym_pushRoot(vm, ctx);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__fa__", ctx);

#define M(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctx); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)
#define MV(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosureVariadic(vm, sig, (void*)fn, ctx); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)

    M("isOpen",        "isOpen()",         i_isOpen);
    M("close",         "close()",          i_close);
    M("path",          "path()",           i_path);
    M("pathAbsolute",  "pathAbsolute()",   i_pathAbsolute);
    M("length",        "length()",         i_length);
    M("position",      "position()",       i_position);
    M("seek",          "seek(pos)",        i_seek);
    M("seekEnd",       "seekEnd(off)",     i_seekEnd);
    M("eof",           "eof()",            i_eof);
    M("flush",         "flush()",          i_flush);
    M("resize",        "resize(n)",        i_resize);
    M("getError",      "getError()",       i_getError);
    M("setBigEndian",  "setBigEndian(b)",  i_setBigEndian);
    M("isBigEndian",   "isBigEndian()",    i_isBigEndian);

    M("readBytes",     "readBytes(n)",     i_readBytes);
    M("writeBytes",    "writeBytes(buf)",  i_writeBytes);
    M("readText",      "readText()",       i_readText);
    M("readLine",      "readLine()",       i_readLine);
    MV("readCSVLine",  "readCSVLine(...)", i_readCSVLine);
    M("writeString",   "writeString(s)",   i_writeString);
    M("writeLine",     "writeLine(s)",     i_writeLine);
    MV("writeCSVLine", "writeCSVLine(list, ...)", i_writeCSVLine);

    MV("readU8",     "readU8(...)",     i_readU8);
    MV("readI8",     "readI8(...)",     i_readI8);
    MV("readU16",    "readU16(...)",    i_readU16);
    MV("readI16",    "readI16(...)",    i_readI16);
    MV("readU32",    "readU32(...)",    i_readU32);
    MV("readI32",    "readI32(...)",    i_readI32);
    MV("readU64",    "readU64(...)",    i_readU64);
    MV("readI64",    "readI64(...)",    i_readI64);
    MV("readHalf",   "readHalf(...)",   i_readHalf);
    MV("readFloat",  "readFloat(...)",  i_readFloat);
    MV("readDouble", "readDouble(...)", i_readDouble);

    MV("writeU8",     "writeU8(value, ...)",     i_writeU8);
    MV("writeI8",     "writeI8(value, ...)",     i_writeI8);
    MV("writeU16",    "writeU16(value, ...)",    i_writeU16);
    MV("writeI16",    "writeI16(value, ...)",    i_writeI16);
    MV("writeU32",    "writeU32(value, ...)",    i_writeU32);
    MV("writeI32",    "writeI32(value, ...)",    i_writeI32);
    MV("writeU64",    "writeU64(value, ...)",    i_writeU64);
    MV("writeI64",    "writeI64(value, ...)",    i_writeI64);
    MV("writeHalf",   "writeHalf(value, ...)",   i_writeHalf);
    MV("writeFloat",  "writeFloat(value, ...)",  i_writeFloat);
    MV("writeDouble", "writeDouble(value, ...)", i_writeDouble);

#undef M
#undef MV

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}

// ---- File global (statics) ----

static ZymValue f_open(ZymVM* vm, ZymValue, ZymValue pathV, ZymValue modeV) {
    String path; if (!reqStr(vm, pathV, "File.open(path, mode)", &path)) return ZYM_ERROR;
    String mode; if (!reqStr(vm, modeV, "File.open(path, mode)", &mode)) return ZYM_ERROR;
    int m = modeFromString(mode);
    if (m < 0) { zym_runtimeError(vm, "File.open(path, mode): mode must be \"r\", \"w\", \"rw\", or \"wr\""); return ZYM_ERROR; }
    Ref<FileAccess> fa = FileAccess::open(path, m);
    if (fa.is_null()) return zym_newNull();
    return makeFileInstance(vm, fa);
}

static ZymValue f_openCompressed(ZymVM* vm, ZymValue, ZymValue pathV, ZymValue modeV, ZymValue algoV) {
    String path; if (!reqStr(vm, pathV, "File.openCompressed(path, mode, algo)", &path)) return ZYM_ERROR;
    String mode; if (!reqStr(vm, modeV, "File.openCompressed(path, mode, algo)", &mode)) return ZYM_ERROR;
    String algo; if (!reqStr(vm, algoV, "File.openCompressed(path, mode, algo)", &algo)) return ZYM_ERROR;
    int m = modeFromString(mode);
    if (m < 0) { zym_runtimeError(vm, "File.openCompressed: mode must be \"r\", \"w\", \"rw\", or \"wr\""); return ZYM_ERROR; }
    int c = compressionFromString(algo);
    if (c < 0) { zym_runtimeError(vm, "File.openCompressed: algo must be \"fastlz\", \"deflate\", \"zstd\", \"gzip\", or \"brotli\""); return ZYM_ERROR; }
    Ref<FileAccess> fa = FileAccess::open_compressed(path, (FileAccess::ModeFlags)m, (FileAccess::CompressionMode)c);
    if (fa.is_null()) return zym_newNull();
    return makeFileInstance(vm, fa);
}

static ZymValue f_openEncryptedPass(ZymVM* vm, ZymValue, ZymValue pathV, ZymValue modeV, ZymValue passV) {
    String path; if (!reqStr(vm, pathV, "File.openEncryptedPass(path, mode, password)", &path)) return ZYM_ERROR;
    String mode; if (!reqStr(vm, modeV, "File.openEncryptedPass(path, mode, password)", &mode)) return ZYM_ERROR;
    String pass; if (!reqStr(vm, passV, "File.openEncryptedPass(path, mode, password)", &pass)) return ZYM_ERROR;
    int m = modeFromString(mode);
    if (m < 0) { zym_runtimeError(vm, "File.openEncryptedPass: mode must be \"r\", \"w\", \"rw\", or \"wr\""); return ZYM_ERROR; }
    Ref<FileAccess> fa = FileAccess::open_encrypted_pass(path, (FileAccess::ModeFlags)m, pass);
    if (fa.is_null()) return zym_newNull();
    return makeFileInstance(vm, fa);
}

static ZymValue f_exists(ZymVM* vm, ZymValue, ZymValue pathV) {
    String path; if (!reqStr(vm, pathV, "File.exists(path)", &path)) return ZYM_ERROR;
    return zym_newBool(FileAccess::exists(path));
}

static ZymValue f_size(ZymVM* vm, ZymValue, ZymValue pathV) {
    String path; if (!reqStr(vm, pathV, "File.size(path)", &path)) return ZYM_ERROR;
    return zym_newNumber((double)FileAccess::get_size(path));
}

static ZymValue f_modifiedTime(ZymVM* vm, ZymValue, ZymValue pathV) {
    String path; if (!reqStr(vm, pathV, "File.modifiedTime(path)", &path)) return ZYM_ERROR;
    return zym_newNumber((double)FileAccess::get_modified_time(path));
}

static ZymValue f_accessTime(ZymVM* vm, ZymValue, ZymValue pathV) {
    String path; if (!reqStr(vm, pathV, "File.accessTime(path)", &path)) return ZYM_ERROR;
    return zym_newNumber((double)FileAccess::get_access_time(path));
}

static ZymValue f_md5(ZymVM* vm, ZymValue, ZymValue pathV) {
    String path; if (!reqStr(vm, pathV, "File.md5(path)", &path)) return ZYM_ERROR;
    return strZ(vm, FileAccess::get_md5(path));
}

static ZymValue f_sha256(ZymVM* vm, ZymValue, ZymValue pathV) {
    String path; if (!reqStr(vm, pathV, "File.sha256(path)", &path)) return ZYM_ERROR;
    return strZ(vm, FileAccess::get_sha256(path));
}

static ZymValue f_readBytes(ZymVM* vm, ZymValue, ZymValue pathV) {
    String path; if (!reqStr(vm, pathV, "File.readBytes(path)", &path)) return ZYM_ERROR;
    Error err = OK;
    Vector<uint8_t> bytes = FileAccess::get_file_as_bytes(path, &err);
    if (err != OK) { zym_runtimeError(vm, "File.readBytes(path): cannot read (error %d)", (int)err); return ZYM_ERROR; }
    return makeBufferInstance(vm, bytes);
}

static ZymValue f_readText(ZymVM* vm, ZymValue, ZymValue pathV) {
    String path; if (!reqStr(vm, pathV, "File.readText(path)", &path)) return ZYM_ERROR;
    Error err = OK;
    String text = FileAccess::get_file_as_string(path, &err);
    if (err != OK) { zym_runtimeError(vm, "File.readText(path): cannot read (error %d)", (int)err); return ZYM_ERROR; }
    return strZ(vm, text);
}

static bool writeAll(ZymVM* vm, const String& path, int mode, const char* where,
                     const PackedByteArray* buf, const String* s) {
    Ref<FileAccess> fa = FileAccess::open(path, mode);
    if (fa.is_null()) {
        zym_runtimeError(vm, "%s: cannot open (error %d)", where, (int)FileAccess::get_open_error());
        return false;
    }
    bool ok = true;
    if (buf) ok = fa->store_buffer(*buf);
    else if (s) ok = fa->store_string(*s);
    fa->close();
    return ok;
}

static ZymValue f_writeBytes(ZymVM* vm, ZymValue, ZymValue pathV, ZymValue bufV) {
    String path; if (!reqStr(vm, pathV, "File.writeBytes(path, buf)", &path)) return ZYM_ERROR;
    PackedByteArray* buf; if (!reqBufferArg(vm, bufV, "File.writeBytes(path, buf)", &buf)) return ZYM_ERROR;
    bool ok = writeAll(vm, path, FileAccess::WRITE, "File.writeBytes(path, buf)", buf, nullptr);
    return zym_newBool(ok);
}

static ZymValue f_writeText(ZymVM* vm, ZymValue, ZymValue pathV, ZymValue sv) {
    String path; if (!reqStr(vm, pathV, "File.writeText(path, s)", &path)) return ZYM_ERROR;
    String s; if (!reqStr(vm, sv, "File.writeText(path, s)", &s)) return ZYM_ERROR;
    bool ok = writeAll(vm, path, FileAccess::WRITE, "File.writeText(path, s)", nullptr, &s);
    return zym_newBool(ok);
}

static ZymValue f_append(ZymVM* vm, ZymValue, ZymValue pathV, ZymValue dataV) {
    String path; if (!reqStr(vm, pathV, "File.append(path, data)", &path)) return ZYM_ERROR;
    // Open in READ_WRITE to preserve contents, then seek to end.
    Ref<FileAccess> fa = FileAccess::open(path, FileAccess::READ_WRITE);
    if (fa.is_null()) fa = FileAccess::open(path, FileAccess::WRITE);
    if (fa.is_null()) { zym_runtimeError(vm, "File.append(path, data): cannot open (error %d)", (int)FileAccess::get_open_error()); return ZYM_ERROR; }
    fa->seek_end(0);
    bool ok;
    if (zym_isString(dataV)) {
        String s = String::utf8(zym_asCString(dataV));
        ok = fa->store_string(s);
    } else if (zym_isMap(dataV)) {
        PackedByteArray* buf;
        if (!reqBufferArg(vm, dataV, "File.append(path, data)", &buf)) { fa->close(); return ZYM_ERROR; }
        ok = fa->store_buffer(*buf);
    } else {
        fa->close();
        zym_runtimeError(vm, "File.append(path, data): data must be a string or Buffer");
        return ZYM_ERROR;
    }
    fa->close();
    return zym_newBool(ok);
}

static ZymValue f_copy(ZymVM* vm, ZymValue, ZymValue srcV, ZymValue dstV) {
    String src; if (!reqStr(vm, srcV, "File.copy(src, dst)", &src)) return ZYM_ERROR;
    String dst; if (!reqStr(vm, dstV, "File.copy(src, dst)", &dst)) return ZYM_ERROR;
    return zym_newBool(DirAccess::copy_absolute(src, dst) == OK);
}

static ZymValue f_remove(ZymVM* vm, ZymValue, ZymValue pathV) {
    String path; if (!reqStr(vm, pathV, "File.remove(path)", &path)) return ZYM_ERROR;
    return zym_newBool(DirAccess::remove_absolute(path) == OK);
}

static ZymValue f_rename(ZymVM* vm, ZymValue, ZymValue srcV, ZymValue dstV) {
    String src; if (!reqStr(vm, srcV, "File.rename(src, dst)", &src)) return ZYM_ERROR;
    String dst; if (!reqStr(vm, dstV, "File.rename(src, dst)", &dst)) return ZYM_ERROR;
    return zym_newBool(DirAccess::rename_absolute(src, dst) == OK);
}

// ---- factory ----

ZymValue nativeFile_create(ZymVM* vm) {
    ZymValue ctx = zym_createNativeContext(vm, nullptr, nullptr);
    zym_pushRoot(vm, ctx);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

#define F(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctx); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)

    F("open",              "open(path, mode)",                        f_open);
    F("openCompressed",    "openCompressed(path, mode, algo)",        f_openCompressed);
    F("openEncryptedPass", "openEncryptedPass(path, mode, password)", f_openEncryptedPass);

    F("readBytes",    "readBytes(path)",         f_readBytes);
    F("readText",     "readText(path)",          f_readText);
    F("writeBytes",   "writeBytes(path, buf)",   f_writeBytes);
    F("writeText",    "writeText(path, s)",      f_writeText);
    F("append",       "append(path, data)",      f_append);

    F("exists",       "exists(path)",            f_exists);
    F("size",         "size(path)",              f_size);
    F("modifiedTime", "modifiedTime(path)",      f_modifiedTime);
    F("accessTime",   "accessTime(path)",        f_accessTime);
    F("md5",          "md5(path)",               f_md5);
    F("sha256",       "sha256(path)",            f_sha256);

    F("copy",         "copy(src, dst)",          f_copy);
    F("remove",       "remove(path)",            f_remove);
    F("rename",       "rename(src, dst)",        f_rename);

#undef F

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}
