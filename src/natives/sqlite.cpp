// SQLite native — better-sqlite3-style API over the vendored SQLite
// amalgamation (third_party/sqlite, version 3.50.2).
//
// Surface (matches better-sqlite3 closely; deviations noted inline):
//
//   SQLite.open(path)              -> Database          // file on disk
//   SQLite.open(":memory:")        -> Database          // empty in-memory
//   SQLite.open(buffer)            -> Database          // in-memory, loaded from buffer
//   SQLite.open(arg, opts)         -> Database          // opts: { readonly, fileMustExist }
//
//   Database:
//     db.memory()                  -> bool
//     db.readonly()                -> bool
//     db.name()                    -> string (filename, ":memory:" for memory/buffer DBs)
//     db.open()                    -> bool
//     db.inTransaction()           -> bool
//     db.prepare(sql)              -> Statement
//     db.exec(sql)                 -> db   (chainable; supports multi-statement SQL)
//     db.pragma(name)              -> result
//     db.pragma(name, opts)        -> result   (opts: { simple })
//     db.transaction(fn)           -> wrapped fn (default mode: "deferred")
//     db.transaction(fn, mode)     -> wrapped fn (mode: "deferred"/"immediate"/"exclusive")
//     db.serialize()               -> Buffer
//     db.defaultSafeIntegers()     -> db   (toggle)
//     db.defaultSafeIntegers(b)    -> db
//     db.close()                   -> db
//
//   Statement:
//     stmt.source()                -> string  (the SQL text)
//     stmt.reader()                -> bool    (returns rows?)
//     stmt.readonly()              -> bool    (modifies the DB?)
//     stmt.busy()                  -> bool    (mid-iteration?)
//     stmt.run([params])           -> { changes, lastInsertRowid }
//     stmt.get([params])           -> row | null
//     stmt.all([params])           -> list of rows
//     stmt.iterate([params])       -> iterator with .next() -> row | null  (deviation)
//     stmt.columns()               -> list of { name, column, table, database, type }
//     stmt.pluck()                 -> stmt   (toggle)
//     stmt.pluck(b)                -> stmt
//     stmt.expand()                -> stmt   (toggle)
//     stmt.expand(b)               -> stmt
//     stmt.raw()                   -> stmt   (toggle)
//     stmt.raw(b)                  -> stmt
//     stmt.bind(params)            -> stmt   (permanent bind; once-per-stmt)
//     stmt.safeIntegers()          -> stmt   (toggle)
//     stmt.safeIntegers(b)         -> stmt
//     stmt.finalize()              -> null
//
// Parameter binding:
//   Positional:  stmt.run(1, "foo", buf)
//   Named:       stmt.run({ id: 1, name: "foo" })  — binds @id / :id / $id
//   Mixed not allowed (matches better-sqlite3).
//
// Type mapping:
//   SQLite NULL    <-> zym null
//   SQLite INTEGER <-> zym number (double)            — see safeIntegers
//   SQLite REAL    <-> zym number
//   SQLite TEXT    <-> zym string
//   SQLite BLOB    <-> zym Buffer
//
// safeIntegers mode:
//   Off (default): all INTEGER values pass through `double`. Values whose
//                  magnitude exceeds 2^53 silently lose precision.
//   On:            INTEGER columns whose value > 2^53 (or < -2^53) are
//                  returned as decimal STRINGS. On binding, decimal-string
//                  arguments that fit in int64 are bound as INTEGER. This
//                  is the lossless escape hatch for snowflake-style IDs.
//
// Deviations from better-sqlite3:
//   * `SQLite.open(...)` factory instead of `new Database(...)` (zym
//     has no `new`).
//   * `db.transaction(fn).immediate(...)` — better-sqlite3 attaches
//     `.deferred / .immediate / .exclusive` to the returned wrapped
//     function. zym functions don't carry attached properties; we take
//     the mode as a second argument: `db.transaction(fn, "immediate")`.
//   * `stmt.iterate(...)` returns an object with `.next()` instead of
//     a JS-style iterator, because zym has no `for...of` protocol.
//   * `SQLite.open(buffer)` — an extension over better-sqlite3 enabled
//     by sqlite3_deserialize. Allows loading a DB from a Buffer (e.g.
//     a zpk archive entry) and round-tripping via `db.serialize()`.
//   * `db.function() / db.aggregate() / db.loadExtension() / db.backup()`
//     are not bound in v1. They can be added later without API churn.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <initializer_list>

#include "sqlite3.h"

#include "natives.hpp"

namespace {

// ============================================================================
// Handle structs
// ============================================================================

struct StmtHandle;

struct DbHandle {
    sqlite3* db = nullptr;
    // Path passed to open() ("path/to.db" or ":memory:" for memory and
    // buffer-loaded DBs). Cached because sqlite3_db_filename returns ""
    // for memory DBs, and we want db.name() to be stable.
    char* name = nullptr;
    bool readonly = false;
    bool memory = false;        // ":memory:" or loaded from a Buffer
    bool default_safe_ints = false;
    // Intrusive list of open statements so finalizing a Database
    // also finalizes its statements (sqlite3_close would otherwise
    // return SQLITE_BUSY and leak the handle). Statements remove
    // themselves on close.
    StmtHandle* stmts_head = nullptr;
};

struct StmtHandle {
    DbHandle* owner = nullptr;
    sqlite3_stmt* stmt = nullptr;
    char* sql = nullptr;        // cached source SQL (utf-8, owned)
    bool busy = false;          // mid-iteration via `iterate`
    bool safe_ints = false;
    bool pluck = false;
    bool expand = false;
    bool raw = false;
    // Doubly-linked list inside owner->stmts_head.
    StmtHandle* prev = nullptr;
    StmtHandle* next = nullptr;
};

void stmt_unlink_from_owner(StmtHandle* h) {
    if (!h->owner) return;
    if (h->prev) h->prev->next = h->next;
    else h->owner->stmts_head = h->next;
    if (h->next) h->next->prev = h->prev;
    h->prev = h->next = nullptr;
}

void stmt_link_to_owner(StmtHandle* h, DbHandle* owner) {
    h->owner = owner;
    h->next = owner->stmts_head;
    h->prev = nullptr;
    if (owner->stmts_head) owner->stmts_head->prev = h;
    owner->stmts_head = h;
}

// ============================================================================
// Finalizers
// ============================================================================

void stmtFinalizer(ZymVM*, void* d) {
    auto* h = static_cast<StmtHandle*>(d);
    if (!h) return;
    if (h->stmt) sqlite3_finalize(h->stmt);
    stmt_unlink_from_owner(h);
    free(h->sql);
    delete h;
}

void dbFinalizer(ZymVM*, void* d) {
    auto* h = static_cast<DbHandle*>(d);
    if (!h) return;
    // Finalize every still-live statement first so close doesn't return
    // SQLITE_BUSY. The StmtHandles themselves are still owned by their
    // own GC contexts; we only clear their underlying sqlite3_stmt*.
    while (h->stmts_head) {
        StmtHandle* s = h->stmts_head;
        if (s->stmt) { sqlite3_finalize(s->stmt); s->stmt = nullptr; }
        s->owner = nullptr;
        h->stmts_head = s->next;
        if (h->stmts_head) h->stmts_head->prev = nullptr;
        s->prev = s->next = nullptr;
    }
    if (h->db) sqlite3_close_v2(h->db);
    free(h->name);
    delete h;
}

// ============================================================================
// Small helpers
// ============================================================================

ZymValue strZ(ZymVM* vm, const char* s) {
    if (!s) return zym_newNull();
    return zym_newString(vm, s);
}

ZymValue strZN(ZymVM* vm, const char* s, int n) {
    return zym_newStringN(vm, s, n);
}

DbHandle* unwrapDb(ZymValue ctx) {
    return static_cast<DbHandle*>(zym_getNativeData(ctx));
}

StmtHandle* unwrapStmt(ZymValue ctx) {
    return static_cast<StmtHandle*>(zym_getNativeData(ctx));
}

// Format SQLite's error code + message into a runtime error.
void raise_sqlite(ZymVM* vm, sqlite3* db, const char* where) {
    int code = db ? sqlite3_extended_errcode(db) : SQLITE_ERROR;
    const char* msg = db ? sqlite3_errmsg(db) : sqlite3_errstr(code);
    const char* name = sqlite3_errstr(code);
    zym_runtimeError(vm, "%s: %s (%s, code %d)", where, msg ? msg : "", name ? name : "?", code);
}

bool isBufferValue(ZymVM* vm, ZymValue v) {
    if (!zym_isMap(v)) return false;
    return zym_mapHas(v, "__pba__");
}

// Duplicate a C string with malloc.
char* dupcstr(const char* s) {
    if (!s) return nullptr;
    size_t n = strlen(s);
    char* out = (char*)malloc(n + 1);
    if (!out) return nullptr;
    memcpy(out, s, n + 1);
    return out;
}

// ============================================================================
// Param binding
// ============================================================================

// Bind one ZymValue to slot `idx` (1-based) of `stmt`. Returns true on
// success; on failure, calls zym_runtimeError and returns false.
//
// safe_ints affects how strings that look like big integers are bound:
//   off: strings always bind as TEXT
//   on : strings that parse as int64 *and* whose magnitude exceeds
//        2^53 (i.e. they were emitted by a safeIntegers read) bind as
//        INTEGER. Strings that fit in double bind as TEXT (we have no
//        signal they were meant as ints). This lossy heuristic is
//        avoided by binding pure ints directly.
bool bind_one(ZymVM* vm, sqlite3_stmt* stmt, int idx, ZymValue v, bool safe_ints, const char* where) {
    int rc = SQLITE_OK;
    if (zym_isNull(v)) {
        rc = sqlite3_bind_null(stmt, idx);
    } else if (zym_isBool(v)) {
        rc = sqlite3_bind_int(stmt, idx, zym_asBool(v) ? 1 : 0);
    } else if (zym_isNumber(v)) {
        double d = zym_asNumber(v);
        // Integers that fit exactly in double round-trip as INTEGER
        // in SQLite. Floats with a fractional part go as REAL.
        double r;
        if (d == (double)(int64_t)d && d >= -9007199254740992.0 && d <= 9007199254740992.0 && !((r = d, r != r))) {
            rc = sqlite3_bind_int64(stmt, idx, (sqlite3_int64)d);
        } else {
            rc = sqlite3_bind_double(stmt, idx, d);
        }
    } else if (zym_isString(v)) {
        const char* s = nullptr; int byteLen = 0;
        zym_toStringBytes(v, &s, &byteLen);
        if (safe_ints && byteLen > 0 && byteLen < 21) {
            // Try parsing as int64.
            char tmp[24]; memcpy(tmp, s, byteLen); tmp[byteLen] = 0;
            char* endp = nullptr;
            long long ll = strtoll(tmp, &endp, 10);
            if (endp && *endp == 0 && (ll > 9007199254740992LL || ll < -9007199254740992LL)) {
                rc = sqlite3_bind_int64(stmt, idx, (sqlite3_int64)ll);
            } else {
                rc = sqlite3_bind_text(stmt, idx, s, byteLen, SQLITE_TRANSIENT);
            }
        } else {
            rc = sqlite3_bind_text(stmt, idx, s, byteLen, SQLITE_TRANSIENT);
        }
    } else if (isBufferValue(vm, v)) {
        const char* data = nullptr; size_t size = 0;
        readBufferBytes(vm, v, &data, &size);
        if (size == 0) {
            rc = sqlite3_bind_zeroblob(stmt, idx, 0);
        } else {
            rc = sqlite3_bind_blob64(stmt, idx, data, (sqlite3_uint64)size, SQLITE_TRANSIENT);
        }
    } else {
        zym_runtimeError(vm, "%s: cannot bind value of type %s at param %d", where, zym_typeName(v), idx);
        return false;
    }
    if (rc != SQLITE_OK) {
        zym_runtimeError(vm, "%s: bind failed at param %d: %s", where, idx, sqlite3_errstr(rc));
        return false;
    }
    return true;
}

// Result of binding a single named-map argument: passes parameter name
// through sqlite3_bind_parameter_index so @, :, $ prefixes all resolve.
struct NamedBindUserdata {
    ZymVM* vm;
    sqlite3_stmt* stmt;
    bool safe_ints;
    const char* where;
    bool ok;
};

bool named_bind_iter(ZymVM* vm, const char* key, ZymValue val, void* ud) {
    auto* d = static_cast<NamedBindUserdata*>(ud);
    // Try @, :, $ in order — better-sqlite3 / SQLite docs allow any of
    // these three prefixes; the bind index is keyed by the literal
    // text including the prefix.
    char buf[256];
    int idx = 0;
    for (char prefix : {'@', ':', '$'}) {
        snprintf(buf, sizeof(buf), "%c%s", prefix, key);
        idx = sqlite3_bind_parameter_index(d->stmt, buf);
        if (idx > 0) break;
    }
    if (idx == 0) {
        zym_runtimeError(d->vm, "%s: no parameter named '%s' in SQL", d->where, key);
        d->ok = false;
        return false;
    }
    if (!bind_one(d->vm, d->stmt, idx, val, d->safe_ints, d->where)) {
        d->ok = false;
        return false;
    }
    (void)vm;
    return true;
}

// Bind the args of a `run`/`get`/`all`/`iterate` call. `argv` is the
// argument array starting *after* `ctx`; `argc` is its length. Returns
// true on success.
bool bind_args(ZymVM* vm, sqlite3_stmt* stmt, ZymValue* argv, int argc,
               bool safe_ints, const char* where) {
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    if (argc == 0) return true;

    // Named-binding form: a single map argument.
    if (argc == 1 && zym_isMap(argv[0]) && !isBufferValue(vm, argv[0])) {
        NamedBindUserdata u{vm, stmt, safe_ints, where, true};
        zym_mapForEach(vm, argv[0], named_bind_iter, &u);
        return u.ok;
    }

    // Positional form.
    int expected = sqlite3_bind_parameter_count(stmt);
    if (argc != expected) {
        zym_runtimeError(vm, "%s: expected %d bind parameter(s), got %d", where, expected, argc);
        return false;
    }
    for (int i = 0; i < argc; i++) {
        if (!bind_one(vm, stmt, i + 1, argv[i], safe_ints, where)) return false;
    }
    return true;
}

// ============================================================================
// Column -> ZymValue
// ============================================================================

ZymValue column_to_zym(ZymVM* vm, sqlite3_stmt* stmt, int i, bool safe_ints) {
    int type = sqlite3_column_type(stmt, i);
    switch (type) {
        case SQLITE_NULL:
            return zym_newNull();
        case SQLITE_INTEGER: {
            sqlite3_int64 v = sqlite3_column_int64(stmt, i);
            if (safe_ints && (v > 9007199254740992LL || v < -9007199254740992LL)) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%lld", (long long)v);
                return zym_newString(vm, buf);
            }
            return zym_newNumber((double)v);
        }
        case SQLITE_FLOAT:
            return zym_newNumber(sqlite3_column_double(stmt, i));
        case SQLITE_TEXT: {
            const unsigned char* s = sqlite3_column_text(stmt, i);
            int n = sqlite3_column_bytes(stmt, i);
            return zym_newStringN(vm, (const char*)s, n);
        }
        case SQLITE_BLOB: {
            const void* p = sqlite3_column_blob(stmt, i);
            int n = sqlite3_column_bytes(stmt, i);
            return makeBufferFromBytes(vm, (const char*)p, (size_t)n);
        }
        default:
            return zym_newNull();
    }
}

// Build one row from the current cursor position.
//   raw=true   -> list of values, no names
//   pluck=true -> just column 0's value (raw wins over pluck if both set;
//                 better-sqlite3's stmt.raw() overrides pluck)
//   expand=true-> nested map { tableName: { colName: value, ... }, ... }
//                 columns without an origin table go in a synthetic
//                 "$" key (rare; happens for expressions).
//   default    -> flat map of column name -> value
ZymValue row_value(ZymVM* vm, StmtHandle* h) {
    sqlite3_stmt* stmt = h->stmt;
    int n = sqlite3_column_count(stmt);
    if (h->raw) {
        ZymValue list = zym_newList(vm);
        zym_pushRoot(vm, list);
        for (int i = 0; i < n; i++) {
            zym_listAppend(vm, list, column_to_zym(vm, stmt, i, h->safe_ints));
        }
        zym_popRoot(vm);
        return list;
    }
    if (h->pluck) {
        if (n == 0) return zym_newNull();
        return column_to_zym(vm, stmt, 0, h->safe_ints);
    }
    if (h->expand) {
        ZymValue out = zym_newMap(vm);
        zym_pushRoot(vm, out);
        for (int i = 0; i < n; i++) {
            const char* table = sqlite3_column_table_name(stmt, i);
            const char* col   = sqlite3_column_origin_name(stmt, i);
            if (!col) col = sqlite3_column_name(stmt, i);
            const char* tk = table ? table : "$";
            ZymValue sub = zym_mapGet(vm, out, tk);
            if (sub == ZYM_ERROR || !zym_isMap(sub)) {
                sub = zym_newMap(vm);
                zym_pushRoot(vm, sub);
                zym_mapSet(vm, out, tk, sub);
                zym_popRoot(vm);
            }
            zym_mapSet(vm, sub, col ? col : "", column_to_zym(vm, stmt, i, h->safe_ints));
        }
        zym_popRoot(vm);
        return out;
    }
    // Default: flat map by column alias.
    ZymValue out = zym_newMap(vm);
    zym_pushRoot(vm, out);
    for (int i = 0; i < n; i++) {
        const char* name = sqlite3_column_name(stmt, i);
        zym_mapSet(vm, out, name ? name : "", column_to_zym(vm, stmt, i, h->safe_ints));
    }
    zym_popRoot(vm);
    return out;
}

// ============================================================================
// Statement methods
// ============================================================================

ZymValue stmt_source(ZymVM* vm, ZymValue ctx) {
    StmtHandle* h = unwrapStmt(ctx);
    if (!h || !h->sql) return strZ(vm, "");
    return strZ(vm, h->sql);
}

ZymValue stmt_reader(ZymVM*, ZymValue ctx) {
    StmtHandle* h = unwrapStmt(ctx);
    if (!h || !h->stmt) return zym_newBool(false);
    return zym_newBool(sqlite3_column_count(h->stmt) > 0);
}

ZymValue stmt_readonly(ZymVM*, ZymValue ctx) {
    StmtHandle* h = unwrapStmt(ctx);
    if (!h || !h->stmt) return zym_newBool(false);
    return zym_newBool(sqlite3_stmt_readonly(h->stmt) != 0);
}

ZymValue stmt_busy(ZymVM*, ZymValue ctx) {
    StmtHandle* h = unwrapStmt(ctx);
    if (!h || !h->stmt) return zym_newBool(false);
    return zym_newBool(h->busy);
}

ZymValue stmt_run(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) {
    StmtHandle* h = unwrapStmt(ctx);
    if (!h || !h->stmt || !h->owner || !h->owner->db) {
        zym_runtimeError(vm, "Statement.run: statement is finalized");
        return ZYM_ERROR;
    }
    if (h->busy) {
        zym_runtimeError(vm, "Statement.run: statement is busy (mid-iterate)");
        return ZYM_ERROR;
    }
    if (!bind_args(vm, h->stmt, vargs, vargc, h->safe_ints, "Statement.run")) return ZYM_ERROR;
    int rc = sqlite3_step(h->stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        raise_sqlite(vm, h->owner->db, "Statement.run");
        sqlite3_reset(h->stmt);
        return ZYM_ERROR;
    }
    int changes = sqlite3_changes(h->owner->db);
    sqlite3_int64 last = sqlite3_last_insert_rowid(h->owner->db);
    sqlite3_reset(h->stmt);

    ZymValue out = zym_newMap(vm);
    zym_pushRoot(vm, out);
    zym_mapSet(vm, out, "changes", zym_newNumber((double)changes));
    // lastInsertRowid: if safe_ints and out of double range, return as string.
    if (h->safe_ints && (last > 9007199254740992LL || last < -9007199254740992LL)) {
        char buf[32]; snprintf(buf, sizeof(buf), "%lld", (long long)last);
        zym_mapSet(vm, out, "lastInsertRowid", zym_newString(vm, buf));
    } else {
        zym_mapSet(vm, out, "lastInsertRowid", zym_newNumber((double)last));
    }
    zym_popRoot(vm);
    return out;
}

ZymValue stmt_get(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) {
    StmtHandle* h = unwrapStmt(ctx);
    if (!h || !h->stmt || !h->owner || !h->owner->db) {
        zym_runtimeError(vm, "Statement.get: statement is finalized");
        return ZYM_ERROR;
    }
    if (h->busy) {
        zym_runtimeError(vm, "Statement.get: statement is busy (mid-iterate)");
        return ZYM_ERROR;
    }
    if (!bind_args(vm, h->stmt, vargs, vargc, h->safe_ints, "Statement.get")) return ZYM_ERROR;
    int rc = sqlite3_step(h->stmt);
    if (rc == SQLITE_ROW) {
        ZymValue row = row_value(vm, h);
        sqlite3_reset(h->stmt);
        return row;
    }
    if (rc == SQLITE_DONE) {
        sqlite3_reset(h->stmt);
        return zym_newNull();
    }
    raise_sqlite(vm, h->owner->db, "Statement.get");
    sqlite3_reset(h->stmt);
    return ZYM_ERROR;
}

ZymValue stmt_all(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) {
    StmtHandle* h = unwrapStmt(ctx);
    if (!h || !h->stmt || !h->owner || !h->owner->db) {
        zym_runtimeError(vm, "Statement.all: statement is finalized");
        return ZYM_ERROR;
    }
    if (h->busy) {
        zym_runtimeError(vm, "Statement.all: statement is busy (mid-iterate)");
        return ZYM_ERROR;
    }
    if (!bind_args(vm, h->stmt, vargs, vargc, h->safe_ints, "Statement.all")) return ZYM_ERROR;
    ZymValue list = zym_newList(vm);
    zym_pushRoot(vm, list);
    while (true) {
        int rc = sqlite3_step(h->stmt);
        if (rc == SQLITE_ROW) {
            zym_listAppend(vm, list, row_value(vm, h));
        } else if (rc == SQLITE_DONE) {
            break;
        } else {
            raise_sqlite(vm, h->owner->db, "Statement.all");
            sqlite3_reset(h->stmt);
            zym_popRoot(vm);
            return ZYM_ERROR;
        }
    }
    sqlite3_reset(h->stmt);
    zym_popRoot(vm);
    return list;
}

ZymValue stmt_columns(ZymVM* vm, ZymValue ctx) {
    StmtHandle* h = unwrapStmt(ctx);
    if (!h || !h->stmt) {
        zym_runtimeError(vm, "Statement.columns: statement is finalized");
        return ZYM_ERROR;
    }
    int n = sqlite3_column_count(h->stmt);
    ZymValue list = zym_newList(vm);
    zym_pushRoot(vm, list);
    for (int i = 0; i < n; i++) {
        ZymValue m = zym_newMap(vm);
        zym_pushRoot(vm, m);
        const char* name = sqlite3_column_name(h->stmt, i);
        const char* col  = sqlite3_column_origin_name(h->stmt, i);
        const char* tbl  = sqlite3_column_table_name(h->stmt, i);
        const char* db   = sqlite3_column_database_name(h->stmt, i);
        const char* type = sqlite3_column_decltype(h->stmt, i);
        zym_mapSet(vm, m, "name",     strZ(vm, name ? name : ""));
        zym_mapSet(vm, m, "column",   col  ? strZ(vm, col)  : zym_newNull());
        zym_mapSet(vm, m, "table",    tbl  ? strZ(vm, tbl)  : zym_newNull());
        zym_mapSet(vm, m, "database", db   ? strZ(vm, db)   : zym_newNull());
        zym_mapSet(vm, m, "type",     type ? strZ(vm, type) : zym_newNull());
        zym_listAppend(vm, list, m);
        zym_popRoot(vm);
    }
    zym_popRoot(vm);
    return list;
}

// Toggle helpers. With no argument, flip; with a bool, set explicitly.
bool read_toggle(ZymVM* vm, ZymValue* vargs, int vargc, bool* current, const char* where) {
    if (vargc == 0) { *current = !*current; return true; }
    if (!zym_isBool(vargs[0])) {
        zym_runtimeError(vm, "%s expects a bool or no argument", where);
        return false;
    }
    *current = zym_asBool(vargs[0]);
    return true;
}

ZymValue stmt_pluck(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc);
ZymValue stmt_expand(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc);
ZymValue stmt_raw(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc);
ZymValue stmt_safeIntegers(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc);
ZymValue stmt_finalize(ZymVM* vm, ZymValue ctx);
ZymValue stmt_bind(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc);

// Iterator object — returned by stmt.iterate(). Has .next() and .return().
struct IterHandle {
    StmtHandle* stmt = nullptr;
    bool done = false;
};

void iterFinalizer(ZymVM*, void* d) {
    auto* h = static_cast<IterHandle*>(d);
    if (!h) return;
    if (h->stmt && h->stmt->stmt) {
        sqlite3_reset(h->stmt->stmt);
        h->stmt->busy = false;
    }
    delete h;
}

ZymValue iter_next(ZymVM* vm, ZymValue ctx) {
    auto* h = static_cast<IterHandle*>(zym_getNativeData(ctx));
    if (!h || h->done || !h->stmt || !h->stmt->stmt) return zym_newNull();
    int rc = sqlite3_step(h->stmt->stmt);
    if (rc == SQLITE_ROW) {
        return row_value(vm, h->stmt);
    }
    if (rc == SQLITE_DONE) {
        h->done = true;
        sqlite3_reset(h->stmt->stmt);
        h->stmt->busy = false;
        return zym_newNull();
    }
    raise_sqlite(vm, h->stmt->owner ? h->stmt->owner->db : nullptr, "Iterator.next");
    h->done = true;
    sqlite3_reset(h->stmt->stmt);
    h->stmt->busy = false;
    return ZYM_ERROR;
}

ZymValue iter_return(ZymVM* vm, ZymValue ctx) {
    auto* h = static_cast<IterHandle*>(zym_getNativeData(ctx));
    if (!h) return zym_newNull();
    if (!h->done && h->stmt && h->stmt->stmt) {
        sqlite3_reset(h->stmt->stmt);
        h->stmt->busy = false;
    }
    h->done = true;
    (void)vm;
    return zym_newNull();
}

ZymValue stmt_iterate(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) {
    StmtHandle* h = unwrapStmt(ctx);
    if (!h || !h->stmt || !h->owner || !h->owner->db) {
        zym_runtimeError(vm, "Statement.iterate: statement is finalized");
        return ZYM_ERROR;
    }
    if (h->busy) {
        zym_runtimeError(vm, "Statement.iterate: statement is busy");
        return ZYM_ERROR;
    }
    if (!bind_args(vm, h->stmt, vargs, vargc, h->safe_ints, "Statement.iterate")) return ZYM_ERROR;
    h->busy = true;

    auto* ih = new IterHandle();
    ih->stmt = h;
    ZymValue ictx = zym_createNativeContext(vm, ih, iterFinalizer);
    zym_pushRoot(vm, ictx);
    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__iter__", ictx);
    {
        ZymValue cl = zym_createNativeClosure(vm, "next()", (void*)iter_next, ictx);
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, "next", cl); zym_popRoot(vm);
    }
    {
        ZymValue cl = zym_createNativeClosure(vm, "return()", (void*)iter_return, ictx);
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, "return", cl); zym_popRoot(vm);
    }
    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ictx
    return obj;
}

ZymValue stmt_pluck(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) {
    StmtHandle* h = unwrapStmt(ctx);
    if (!h) { zym_runtimeError(vm, "Statement.pluck: invalid statement"); return ZYM_ERROR; }
    if (!read_toggle(vm, vargs, vargc, &h->pluck, "Statement.pluck")) return ZYM_ERROR;
    // Returning ctx alone wouldn't expose the method map; the wrapper
    // map is held by the script and is what `.pluck()` itself was
    // invoked on, so returning the map would require accessing it.
    // Instead, return null and have the script chain by re-using the
    // bound name. better-sqlite3's `stmt.pluck(true).all(...)` chaining
    // doesn't naturally work in zym because methods don't carry their
    // owning map. Users do `stmt.pluck(true); rows = stmt.all(...)`.
    (void)vm;
    return zym_newNull();
}

ZymValue stmt_expand(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) {
    StmtHandle* h = unwrapStmt(ctx);
    if (!h) { zym_runtimeError(vm, "Statement.expand: invalid statement"); return ZYM_ERROR; }
    if (!read_toggle(vm, vargs, vargc, &h->expand, "Statement.expand")) return ZYM_ERROR;
    return zym_newNull();
}

ZymValue stmt_raw(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) {
    StmtHandle* h = unwrapStmt(ctx);
    if (!h) { zym_runtimeError(vm, "Statement.raw: invalid statement"); return ZYM_ERROR; }
    if (!read_toggle(vm, vargs, vargc, &h->raw, "Statement.raw")) return ZYM_ERROR;
    return zym_newNull();
}

ZymValue stmt_safeIntegers(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) {
    StmtHandle* h = unwrapStmt(ctx);
    if (!h) { zym_runtimeError(vm, "Statement.safeIntegers: invalid statement"); return ZYM_ERROR; }
    if (!read_toggle(vm, vargs, vargc, &h->safe_ints, "Statement.safeIntegers")) return ZYM_ERROR;
    return zym_newNull();
}

ZymValue stmt_finalize(ZymVM* vm, ZymValue ctx) {
    StmtHandle* h = unwrapStmt(ctx);
    if (!h) return zym_newNull();
    if (h->stmt) { sqlite3_finalize(h->stmt); h->stmt = nullptr; }
    stmt_unlink_from_owner(h);
    (void)vm;
    return zym_newNull();
}

ZymValue stmt_bind(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) {
    StmtHandle* h = unwrapStmt(ctx);
    if (!h || !h->stmt) {
        zym_runtimeError(vm, "Statement.bind: statement is finalized");
        return ZYM_ERROR;
    }
    if (!bind_args(vm, h->stmt, vargs, vargc, h->safe_ints, "Statement.bind")) return ZYM_ERROR;
    return zym_newNull();
}

// ============================================================================
// Statement factory
// ============================================================================

ZymValue makeStmtInstance(ZymVM* vm, DbHandle* db, sqlite3_stmt* stmt, const char* sql) {
    auto* h = new StmtHandle();
    h->stmt = stmt;
    h->sql = dupcstr(sql);
    h->safe_ints = db->default_safe_ints;
    stmt_link_to_owner(h, db);

    ZymValue ctx = zym_createNativeContext(vm, h, stmtFinalizer);
    zym_pushRoot(vm, ctx);
    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__stmt__", ctx);

#define M(name, sig, fn) do { \
    ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctx); \
    zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
} while (0)
#define MV(name, sig, fn) do { \
    ZymValue cl = zym_createNativeClosureVariadic(vm, sig, (void*)fn, ctx); \
    zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
} while (0)

    M (    "source",       "source()",         stmt_source);
    M (    "reader",       "reader()",         stmt_reader);
    M (    "readonly",     "readonly()",       stmt_readonly);
    M (    "busy",         "busy()",           stmt_busy);
    MV(    "run",          "run(...)",         stmt_run);
    MV(    "get",          "get(...)",         stmt_get);
    MV(    "all",          "all(...)",         stmt_all);
    MV(    "iterate",      "iterate(...)",     stmt_iterate);
    M (    "columns",      "columns()",        stmt_columns);
    MV(    "pluck",        "pluck(...)",       stmt_pluck);
    MV(    "expand",       "expand(...)",      stmt_expand);
    MV(    "raw",          "raw(...)",         stmt_raw);
    MV(    "safeIntegers", "safeIntegers(...)",stmt_safeIntegers);
    MV(    "bind",         "bind(...)",        stmt_bind);
    M (    "finalize",     "finalize()",       stmt_finalize);

#undef M
#undef MV

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}

// ============================================================================
// Database methods
// ============================================================================

ZymValue db_memory(ZymVM*, ZymValue ctx) {
    DbHandle* h = unwrapDb(ctx);
    return zym_newBool(h ? h->memory : false);
}

ZymValue db_readonly(ZymVM*, ZymValue ctx) {
    DbHandle* h = unwrapDb(ctx);
    return zym_newBool(h ? h->readonly : false);
}

ZymValue db_name(ZymVM* vm, ZymValue ctx) {
    DbHandle* h = unwrapDb(ctx);
    if (!h || !h->name) return strZ(vm, "");
    return strZ(vm, h->name);
}

ZymValue db_open(ZymVM*, ZymValue ctx) {
    DbHandle* h = unwrapDb(ctx);
    return zym_newBool(h && h->db != nullptr);
}

ZymValue db_inTransaction(ZymVM*, ZymValue ctx) {
    DbHandle* h = unwrapDb(ctx);
    if (!h || !h->db) return zym_newBool(false);
    return zym_newBool(sqlite3_get_autocommit(h->db) == 0);
}

ZymValue db_close(ZymVM* vm, ZymValue ctx) {
    DbHandle* h = unwrapDb(ctx);
    if (!h) return zym_newNull();
    // Finalize live statements (mirrors what dbFinalizer would do).
    while (h->stmts_head) {
        StmtHandle* s = h->stmts_head;
        if (s->stmt) { sqlite3_finalize(s->stmt); s->stmt = nullptr; }
        s->owner = nullptr;
        h->stmts_head = s->next;
        if (h->stmts_head) h->stmts_head->prev = nullptr;
        s->prev = s->next = nullptr;
    }
    if (h->db) { sqlite3_close_v2(h->db); h->db = nullptr; }
    (void)vm;
    return zym_newNull();
}

ZymValue db_exec(ZymVM* vm, ZymValue ctx, ZymValue sqlV) {
    DbHandle* h = unwrapDb(ctx);
    if (!h || !h->db) {
        zym_runtimeError(vm, "Database.exec: database is closed");
        return ZYM_ERROR;
    }
    if (!zym_isString(sqlV)) {
        zym_runtimeError(vm, "Database.exec(sql): sql must be a string");
        return ZYM_ERROR;
    }
    const char* sql = zym_asCString(sqlV);
    char* errmsg = nullptr;
    int rc = sqlite3_exec(h->db, sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        zym_runtimeError(vm, "Database.exec: %s", errmsg ? errmsg : sqlite3_errstr(rc));
        if (errmsg) sqlite3_free(errmsg);
        return ZYM_ERROR;
    }
    return zym_newNull();
}

ZymValue db_prepare(ZymVM* vm, ZymValue ctx, ZymValue sqlV) {
    DbHandle* h = unwrapDb(ctx);
    if (!h || !h->db) {
        zym_runtimeError(vm, "Database.prepare: database is closed");
        return ZYM_ERROR;
    }
    if (!zym_isString(sqlV)) {
        zym_runtimeError(vm, "Database.prepare(sql): sql must be a string");
        return ZYM_ERROR;
    }
    const char* sql = zym_asCString(sqlV);
    sqlite3_stmt* stmt = nullptr;
    const char* tail = nullptr;
    int rc = sqlite3_prepare_v3(h->db, sql, -1, SQLITE_PREPARE_PERSISTENT, &stmt, &tail);
    if (rc != SQLITE_OK || !stmt) {
        raise_sqlite(vm, h->db, "Database.prepare");
        if (stmt) sqlite3_finalize(stmt);
        return ZYM_ERROR;
    }
    return makeStmtInstance(vm, h, stmt, sql);
}

// db.pragma(name [, opts]). When `simple` is true, return only the
// scalar value of the first column of the first row (matches
// better-sqlite3's `{ simple: true }`). Otherwise return a list of rows
// (each row is a flat map).
ZymValue db_pragma(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) {
    DbHandle* h = unwrapDb(ctx);
    if (!h || !h->db) {
        zym_runtimeError(vm, "Database.pragma: database is closed");
        return ZYM_ERROR;
    }
    if (vargc < 1 || !zym_isString(vargs[0])) {
        zym_runtimeError(vm, "Database.pragma(name, opts?): name must be a string");
        return ZYM_ERROR;
    }
    bool simple = false;
    if (vargc >= 2 && zym_isMap(vargs[1])) {
        ZymValue v = zym_mapGet(vm, vargs[1], "simple");
        if (v != ZYM_ERROR && zym_isBool(v)) simple = zym_asBool(v);
    }
    char sql[1024];
    snprintf(sql, sizeof(sql), "PRAGMA %s", zym_asCString(vargs[0]));
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(h->db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        raise_sqlite(vm, h->db, "Database.pragma");
        if (stmt) sqlite3_finalize(stmt);
        return ZYM_ERROR;
    }
    StmtHandle tmp{}; tmp.stmt = stmt; tmp.owner = h; tmp.safe_ints = h->default_safe_ints;
    if (simple) {
        int srC = sqlite3_step(stmt);
        ZymValue out = zym_newNull();
        if (srC == SQLITE_ROW && sqlite3_column_count(stmt) > 0) {
            out = column_to_zym(vm, stmt, 0, h->default_safe_ints);
        } else if (srC != SQLITE_ROW && srC != SQLITE_DONE) {
            raise_sqlite(vm, h->db, "Database.pragma");
            sqlite3_finalize(stmt);
            return ZYM_ERROR;
        }
        sqlite3_finalize(stmt);
        return out;
    }
    ZymValue list = zym_newList(vm);
    zym_pushRoot(vm, list);
    while (true) {
        int srC = sqlite3_step(stmt);
        if (srC == SQLITE_ROW) {
            zym_listAppend(vm, list, row_value(vm, &tmp));
        } else if (srC == SQLITE_DONE) {
            break;
        } else {
            raise_sqlite(vm, h->db, "Database.pragma");
            sqlite3_finalize(stmt);
            zym_popRoot(vm);
            return ZYM_ERROR;
        }
    }
    sqlite3_finalize(stmt);
    zym_popRoot(vm);
    return list;
}

ZymValue db_defaultSafeIntegers(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) {
    DbHandle* h = unwrapDb(ctx);
    if (!h) {
        zym_runtimeError(vm, "Database.defaultSafeIntegers: invalid database");
        return ZYM_ERROR;
    }
    if (!read_toggle(vm, vargs, vargc, &h->default_safe_ints, "Database.defaultSafeIntegers")) return ZYM_ERROR;
    return zym_newNull();
}

// db.serialize() -> Buffer with the database image.
ZymValue db_serialize(ZymVM* vm, ZymValue ctx) {
    DbHandle* h = unwrapDb(ctx);
    if (!h || !h->db) {
        zym_runtimeError(vm, "Database.serialize: database is closed");
        return ZYM_ERROR;
    }
    sqlite3_int64 size = 0;
    unsigned char* ptr = sqlite3_serialize(h->db, "main", &size, 0);
    if (!ptr) {
        // serialize() returns NULL for in-memory DBs that have never been
        // written to, or on OOM. Treat as empty buffer (callers can
        // round-trip an empty DB this way).
        if (size == 0) {
            return makeBufferFromBytes(vm, "", 0);
        }
        zym_runtimeError(vm, "Database.serialize: failed");
        return ZYM_ERROR;
    }
    ZymValue buf = makeBufferFromBytes(vm, (const char*)ptr, (size_t)size);
    sqlite3_free(ptr);
    return buf;
}

// Transaction wrapper closure context: the wrapped function value and the
// mode marker (B/I/E for deferred/immediate/exclusive).
struct TxnHandle {
    DbHandle* db = nullptr;
    ZymValue fn = 0;            // closure to invoke
    char mode = 'D';            // 'D' deferred, 'I' immediate, 'E' exclusive
};

void txnFinalizer(ZymVM*, void* d) {
    auto* h = static_cast<TxnHandle*>(d);
    if (!h) return;
    delete h;
}

// The wrapped function: BEGIN, call user fn, COMMIT on success, ROLLBACK on
// runtime error. Nested calls reuse SAVEPOINT (matches better-sqlite3).
ZymValue txn_call(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) {
    auto* h = static_cast<TxnHandle*>(zym_getNativeData(ctx));
    if (!h || !h->db || !h->db->db) {
        zym_runtimeError(vm, "transaction(): database is closed");
        return ZYM_ERROR;
    }
    sqlite3* db = h->db->db;
    bool nested = (sqlite3_get_autocommit(db) == 0);
    char* errmsg = nullptr;
    int rc;
    if (nested) {
        rc = sqlite3_exec(db, "SAVEPOINT _zym_txn", nullptr, nullptr, &errmsg);
    } else {
        const char* begin = "BEGIN";
        if (h->mode == 'I') begin = "BEGIN IMMEDIATE";
        else if (h->mode == 'E') begin = "BEGIN EXCLUSIVE";
        rc = sqlite3_exec(db, begin, nullptr, nullptr, &errmsg);
    }
    if (rc != SQLITE_OK) {
        zym_runtimeError(vm, "transaction(): begin failed: %s", errmsg ? errmsg : sqlite3_errstr(rc));
        if (errmsg) sqlite3_free(errmsg);
        return ZYM_ERROR;
    }

    ZymStatus s = zym_callClosurev(vm, h->fn, vargc, vargs);
    if (s != ZYM_STATUS_OK) {
        // User function raised; roll back.
        if (nested) sqlite3_exec(db, "ROLLBACK TO _zym_txn; RELEASE _zym_txn", nullptr, nullptr, nullptr);
        else        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        return ZYM_ERROR;
    }
    ZymValue ret = zym_getCallResult(vm);
    zym_pushRoot(vm, ret);

    if (nested) rc = sqlite3_exec(db, "RELEASE _zym_txn", nullptr, nullptr, &errmsg);
    else        rc = sqlite3_exec(db, "COMMIT",          nullptr, nullptr, &errmsg);
    zym_popRoot(vm);
    if (rc != SQLITE_OK) {
        zym_runtimeError(vm, "transaction(): commit failed: %s", errmsg ? errmsg : sqlite3_errstr(rc));
        if (errmsg) sqlite3_free(errmsg);
        return ZYM_ERROR;
    }
    return ret;
}

ZymValue db_transaction(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) {
    DbHandle* h = unwrapDb(ctx);
    if (!h || !h->db) {
        zym_runtimeError(vm, "Database.transaction: database is closed");
        return ZYM_ERROR;
    }
    if (vargc < 1 || (!zym_isClosure(vargs[0]) && !zym_isFunction(vargs[0]))) {
        zym_runtimeError(vm, "Database.transaction(fn, mode?): fn must be a function");
        return ZYM_ERROR;
    }
    char mode = 'D';
    if (vargc >= 2 && zym_isString(vargs[1])) {
        const char* m = zym_asCString(vargs[1]);
        if (strcmp(m, "deferred") == 0) mode = 'D';
        else if (strcmp(m, "immediate") == 0) mode = 'I';
        else if (strcmp(m, "exclusive") == 0) mode = 'E';
        else {
            zym_runtimeError(vm, "Database.transaction: mode must be 'deferred', 'immediate', or 'exclusive'");
            return ZYM_ERROR;
        }
    }
    auto* th = new TxnHandle();
    th->db = h;
    th->fn = vargs[0];
    th->mode = mode;
    ZymValue tctx = zym_createNativeContext(vm, th, txnFinalizer);
    return zym_createNativeClosureVariadic(vm, "transaction(...)", (void*)txn_call, tctx);
}

// ============================================================================
// Database factory
// ============================================================================

ZymValue makeDbInstance(ZymVM* vm, DbHandle* h) {
    ZymValue ctx = zym_createNativeContext(vm, h, dbFinalizer);
    zym_pushRoot(vm, ctx);
    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__db__", ctx);

#define M(name, sig, fn) do { \
    ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctx); \
    zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
} while (0)
#define MV(name, sig, fn) do { \
    ZymValue cl = zym_createNativeClosureVariadic(vm, sig, (void*)fn, ctx); \
    zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
} while (0)

    M (    "memory",              "memory()",              db_memory);
    M (    "readonly",            "readonly()",            db_readonly);
    M (    "name",                "name()",                db_name);
    M (    "open",                "open()",                db_open);
    M (    "inTransaction",       "inTransaction()",       db_inTransaction);
    M (    "prepare",             "prepare(sql)",          db_prepare);
    M (    "exec",                "exec(sql)",             db_exec);
    MV(    "pragma",              "pragma(...)",           db_pragma);
    MV(    "transaction",         "transaction(...)",      db_transaction);
    M (    "serialize",           "serialize()",           db_serialize);
    MV(    "defaultSafeIntegers", "defaultSafeIntegers(...)", db_defaultSafeIntegers);
    M (    "close",               "close()",               db_close);

#undef M
#undef MV

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}

// ============================================================================
// SQLite.open
// ============================================================================

ZymValue sqlite_open(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) {
    (void)ctx;
    if (vargc < 1) {
        zym_runtimeError(vm, "SQLite.open(path | ':memory:' | buffer [, opts])");
        return ZYM_ERROR;
    }
    bool readonly = false;
    bool fileMustExist = false;
    if (vargc >= 2 && !zym_isNull(vargs[1])) {
        if (!zym_isMap(vargs[1])) {
            zym_runtimeError(vm, "SQLite.open: opts must be a map or null");
            return ZYM_ERROR;
        }
        ZymValue v;
        v = zym_mapGet(vm, vargs[1], "readonly");
        if (v != ZYM_ERROR && zym_isBool(v)) readonly = zym_asBool(v);
        v = zym_mapGet(vm, vargs[1], "fileMustExist");
        if (v != ZYM_ERROR && zym_isBool(v)) fileMustExist = zym_asBool(v);
    }

    sqlite3* db = nullptr;
    int flags = readonly
        ? SQLITE_OPEN_READONLY
        : (SQLITE_OPEN_READWRITE | (fileMustExist ? 0 : SQLITE_OPEN_CREATE));

    // --- buffer form ---
    if (isBufferValue(vm, vargs[0])) {
        const char* src = nullptr; size_t size = 0;
        readBufferBytes(vm, vargs[0], &src, &size);
        // Open empty memory DB first, then deserialize into it.
        int rc = sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_MEMORY, nullptr);
        if (rc != SQLITE_OK) {
            zym_runtimeError(vm, "SQLite.open(buffer): %s", sqlite3_errstr(rc));
            if (db) sqlite3_close_v2(db);
            return ZYM_ERROR;
        }
        // sqlite3_deserialize wants sqlite3_malloc'd memory if RESIZABLE.
        // Copy into sqlite-owned memory so SQLite can grow it if needed.
        unsigned char* mem = nullptr;
        unsigned int dflags = readonly ? SQLITE_DESERIALIZE_READONLY
                                       : (SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE);
        if (size > 0) {
            mem = (unsigned char*)sqlite3_malloc64(size);
            if (!mem) {
                zym_runtimeError(vm, "SQLite.open(buffer): out of memory");
                sqlite3_close_v2(db);
                return ZYM_ERROR;
            }
            memcpy(mem, src, size);
        }
        rc = sqlite3_deserialize(db, "main", mem, (sqlite3_int64)size, (sqlite3_int64)size, dflags);
        if (rc != SQLITE_OK) {
            zym_runtimeError(vm, "SQLite.open(buffer): deserialize failed: %s", sqlite3_errstr(rc));
            if (mem) sqlite3_free(mem);
            sqlite3_close_v2(db);
            return ZYM_ERROR;
        }
        auto* h = new DbHandle();
        h->db = db;
        h->name = dupcstr(":memory:");
        h->memory = true;
        h->readonly = readonly;
        return makeDbInstance(vm, h);
    }

    // --- string path form ---
    if (!zym_isString(vargs[0])) {
        zym_runtimeError(vm, "SQLite.open: first arg must be a string path, ':memory:', or Buffer");
        return ZYM_ERROR;
    }
    const char* path = zym_asCString(vargs[0]);
    bool isMemory = (strcmp(path, ":memory:") == 0);
    int rc = sqlite3_open_v2(path, &db, flags | (isMemory ? SQLITE_OPEN_MEMORY : 0), nullptr);
    if (rc != SQLITE_OK) {
        zym_runtimeError(vm, "SQLite.open('%s'): %s", path, db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        if (db) sqlite3_close_v2(db);
        return ZYM_ERROR;
    }
    auto* h = new DbHandle();
    h->db = db;
    h->name = dupcstr(path);
    h->memory = isMemory;
    h->readonly = readonly;
    return makeDbInstance(vm, h);
}

} // namespace

// ============================================================================
// Global factory
// ============================================================================

ZymValue nativeSqlite_create(ZymVM* vm) {
    ZymValue ctx = zym_createNativeContext(vm, nullptr, nullptr);
    zym_pushRoot(vm, ctx);
    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

    {
        ZymValue cl = zym_createNativeClosureVariadic(vm, "open(...)", (void*)sqlite_open, ctx);
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, "open", cl); zym_popRoot(vm);
    }
    // Expose the SQLite version as a constant for diagnostic use.
    zym_mapSet(vm, obj, "version", zym_newString(vm, sqlite3_libversion()));

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}
