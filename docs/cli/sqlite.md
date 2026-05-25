# `SQLite`

A SQLite binding modeled closely on Node's
[`better-sqlite3`](https://github.com/WiseLibs/better-sqlite3). The
global identifier `SQLite` is a small namespace; the real work happens
on the `Database` and `Statement` handles returned from `SQLite.open`
and `db.prepare`.

The native ships with the vendored amalgamation (SQLite 3.50.2) built
single-threaded. There are no external runtime dependencies and no
`libsqlite3` to install on the host.

---

## Conventions

- **Three open forms.** Databases can be opened from a filesystem path,
  from the literal `":memory:"`, or from a `Buffer` containing a
  serialized DB image. The buffer form runs SQLite's
  `sqlite3_deserialize` under the hood, so a database can travel
  through ZPK archives or over a socket without ever touching disk.
- **Type mapping.**

  | SQLite       | Zym                                  |
  | ---          | ---                                  |
  | `NULL`       | `null`                               |
  | `INTEGER`    | `number` (double; see *safeIntegers*) |
  | `REAL`       | `number`                             |
  | `TEXT`       | `string`                             |
  | `BLOB`       | `Buffer`                             |

  Booleans bind as `INTEGER 0/1` (matching better-sqlite3) and come
  back as numbers — SQLite has no boolean column type.
- **Integer precision.** Zym numbers are doubles, so SQLite integers
  whose magnitude exceeds 2^53 silently lose precision by default. Opt
  in to `safeIntegers` on a statement (or set
  `db.defaultSafeIntegers(true)`) to receive out-of-range integers as
  decimal strings instead. See [Big integers](#big-integers).
- **Parameter binding.** Each `run` / `get` / `all` / `iterate` /
  `bind` call accepts either positional arguments (one per `?`
  placeholder) or a single map argument keyed by the parameter name
  (binds to `@name`, `:name`, or `$name` in the SQL). Mixing
  positional and named forms in one call is rejected.
- **Lifetimes.** `Database` and `Statement` handles each carry a
  finalizer; closing the parent `Database` also finalizes every
  prepared statement attached to it. You rarely need to call
  `stmt.finalize()` or `db.close()` explicitly — they exist for
  scripts that want deterministic resource release.
- **Errors.** SQLite errors raise Zym runtime errors of the form
  `Database.method: <sqlite message> (<error name>, code <n>)`. There
  is no per-call success boolean; if the call returns, it succeeded.
- **Capability / grant model.** `SQLite` is a grantable CLI native
  alongside `File`, `Process`, `Pack`, etc. The root VM gets it by
  default; a child VM created via `Zym.newVM(...)` receives it only
  if its parent grants it (e.g. `registerCliNative("SQLite")`).

---

## Statics

| Method | Returns | Notes |
| --- | --- | --- |
| `SQLite.open(path)` | Database | Opens a database file at `path`. Created if missing unless `fileMustExist` is set. |
| `SQLite.open(":memory:")` | Database | Opens a fresh in-memory database. |
| `SQLite.open(buffer)` | Database | Opens an in-memory database loaded from a `Buffer` containing a serialized SQLite image (see `db.serialize()`). |
| `SQLite.open(arg, opts)` | Database | Any of the three forms, plus an options map. Recognised keys: `readonly` (bool), `fileMustExist` (bool — file form only). |
| `SQLite.version` | string | The linked SQLite library version, e.g. `"3.50.2"`. Not a method. |

---

## `Database` instance

Returned by `SQLite.open(...)`.

### Properties (getters)

| Method | Returns | Notes |
| --- | --- | --- |
| `db.memory()` | bool | `true` for `:memory:` and buffer-loaded databases. |
| `db.readonly()` | bool | `true` when opened with `{ readonly: true }`. |
| `db.name()` | string | The path or `":memory:"` token passed to `open`. Stable for the life of the handle. |
| `db.open()` | bool | `false` after `db.close()`. |
| `db.inTransaction()` | bool | `true` if any transaction (top-level or savepoint) is currently active. |

### Statements

| Method | Returns | Notes |
| --- | --- | --- |
| `db.prepare(sql)` | Statement | Compiles `sql` into a reusable prepared statement. Prepared statements survive the call that created them and may be reused across many `run` / `get` / `all` invocations. |
| `db.exec(sql)` | null | Executes `sql` as one or more semicolon-separated statements with no parameters and no result rows. Use this for DDL and bulk script execution. |

### Pragmas

| Method | Returns | Notes |
| --- | --- | --- |
| `db.pragma(name)` | list of rows | Runs `PRAGMA <name>` and returns the result as a list of maps, one per row. |
| `db.pragma(name, opts)` | scalar or list | When `opts.simple` is `true`, returns just the first column of the first row (or `null` for empty results). |

### Transactions

| Method | Returns | Notes |
| --- | --- | --- |
| `db.transaction(fn)` | function | Wraps `fn` so that calling the returned function runs `fn` inside a `BEGIN` / `COMMIT`. Nested calls open a `SAVEPOINT` instead. If `fn` raises a runtime error, the wrapper rolls back and re-raises. |
| `db.transaction(fn, mode)` | function | Same as above but uses `BEGIN <mode>` for the outermost transaction. `mode` is one of `"deferred"` (default), `"immediate"`, or `"exclusive"`. *(Deviation from better-sqlite3, which attaches `.deferred` / `.immediate` / `.exclusive` to the returned function — zym functions don't carry attached properties.)* |

### Buffer serialization

| Method | Returns | Notes |
| --- | --- | --- |
| `db.serialize()` | Buffer | Returns the full database image as a fresh `Buffer`. The buffer can be stored in a ZPK entry, sent over the network, or reopened with `SQLite.open(buffer)`. |

### Big integers

| Method | Returns | Notes |
| --- | --- | --- |
| `db.defaultSafeIntegers()` | null | Toggles the default `safeIntegers` mode used by every statement subsequently prepared on this database. |
| `db.defaultSafeIntegers(b)` | null | Sets the default explicitly to `b`. |

### Lifecycle

| Method | Returns | Notes |
| --- | --- | --- |
| `db.close()` | null | Closes the database and finalizes every prepared statement attached to it. After close, every method on the handle raises. GC will also close the database as a safety net. |

---

## `Statement` instance

Returned by `db.prepare(sql)`. Prepared statements are reusable; each
`run` / `get` / `all` / `iterate` call resets the statement and rebinds
its parameters.

### Properties (getters)

| Method | Returns | Notes |
| --- | --- | --- |
| `stmt.source()` | string | The original SQL text. |
| `stmt.reader()` | bool | `true` if the statement produces result rows (i.e. `SELECT` / `PRAGMA` / `RETURNING ...`). |
| `stmt.readonly()` | bool | `true` if the statement does not modify the database. |
| `stmt.busy()` | bool | `true` while a `stmt.iterate(...)` cursor is still being consumed. |

### Execution

Each method below accepts either zero or more positional arguments
(one per `?`) **or** a single map argument (named bindings). Mixed
calls are rejected.

| Method | Returns | Notes |
| --- | --- | --- |
| `stmt.run(params?)` | `{ changes, lastInsertRowid }` | Executes the statement and discards any result rows. `changes` is the number of rows affected; `lastInsertRowid` is the rowid of the most recent successful `INSERT` on the parent database. |
| `stmt.get(params?)` | row \| `null` | Executes the statement, returns the first row, and resets. Returns `null` if no rows. |
| `stmt.all(params?)` | list of rows | Executes the statement and returns every row as a list. |
| `stmt.iterate(params?)` | iterator | Returns an iterator object with `.next()` and `.return()` methods. Call `.next()` until it returns `null`. The statement is marked `busy` until iteration completes or `.return()` is called. *(Deviation from better-sqlite3, which uses the JS iterator protocol — zym has no `for...of`.)* |
| `stmt.bind(params)` | null | Permanently binds parameters so subsequent execution methods don't need them. Calling `bind` a second time is an error in better-sqlite3 and is also allowed here (it rebinds). |

### Row shape toggles

Toggles are sticky on the statement; the call returns `null`, not the
statement itself.

| Method | Effect |
| --- | --- |
| `stmt.pluck(b?)` | When on, single-column rows are returned as bare values instead of `{ column: value }` maps. |
| `stmt.expand(b?)` | When on, rows are returned as nested maps keyed by source table: `row.users.id`, `row.posts.title`. Columns that don't trace to a source table (expressions, constants) go under the synthetic `$` key. |
| `stmt.raw(b?)` | When on, rows are returned as lists of values in column order (no names). `raw` wins over `pluck` / `expand` when more than one is set. |

### Reflection

| Method | Returns | Notes |
| --- | --- | --- |
| `stmt.columns()` | list | One entry per result column: `{ name, column, table, database, type }`. `name` is the SELECT alias; `column` is the underlying column name from the source table (or `null` for expressions); `table` and `database` identify the source. `type` is the declared SQL type or `null`. |

### Big integers

| Method | Effect |
| --- | --- |
| `stmt.safeIntegers(b?)` | Toggle. When on, `INTEGER` columns whose value exceeds 2^53 in magnitude are returned as decimal strings instead of numbers. On the bind side, decimal-string arguments that parse to an integer outside the double range are bound as `INTEGER`. |

### Lifecycle

| Method | Returns | Notes |
| --- | --- | --- |
| `stmt.finalize()` | null | Releases the underlying prepared statement. Optional — GC and `db.close()` will both finalize the statement on your behalf. |

---

## Examples

### Quick start

```zym
var db = SQLite.open(":memory:")

db.exec("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER)")

var insert = db.prepare("INSERT INTO users (name, age) VALUES (?, ?)")
insert.run("Alice", 30)
insert.run("Bob", 25)

var users = db.prepare("SELECT * FROM users ORDER BY age").all()
print(users)   // [{"id":2,"name":"Bob","age":25}, {"id":1,"name":"Alice","age":30}]
```

### Named bindings

```zym
var stmt = db.prepare("INSERT INTO users (name, age) VALUES (@name, @age)")
stmt.run({ name: "Carol", age: 42 })
```

Any of `@name`, `:name`, or `$name` resolves to the same map key.

### Transactions

```zym
var insertOne = db.prepare("INSERT INTO users (name, age) VALUES (?, ?)")

var insertMany = db.transaction(func(rows) {
    var i = 0
    while (i < length(rows)) {
        insertOne.run(rows[i][0], rows[i][1])
        i = i + 1
    }
})

insertMany([["Dave", 18], ["Eve", 35], ["Frank", 50]])
```

If the callback raises a runtime error, the wrapper issues a
`ROLLBACK` and re-raises. For locking-sensitive bulk inserts, request
an immediate lock up front:

```zym
var insertMany = db.transaction(func(rows) { /* ... */ }, "immediate")
```

### Streaming with `iterate`

```zym
var iter = db.prepare("SELECT id, name FROM users ORDER BY id").iterate()
var row = iter.next()
while (row != null) {
    print("%v: %v\n", row.id, row.name)
    row = iter.next()
}
```

Calling `iter.return()` before exhaustion is safe and releases the
underlying cursor early.

### Row shape toggles

```zym
// Pluck a single column
var names = db.prepare("SELECT name FROM users ORDER BY id")
names.pluck(true)
print(names.all())   // ["Alice", "Bob", "Carol", ...]

// Raw rows as lists
var s = db.prepare("SELECT id, name, age FROM users")
s.raw(true)
print(s.all())       // [[1, "Alice", 30], [2, "Bob", 25], ...]

// Expand by source table (useful with JOINs)
var j = db.prepare("SELECT u.id, u.name, p.title FROM users u JOIN posts p ON p.author_id = u.id")
j.expand(true)
var row = j.get()
print(row.users.name)
print(row.posts.title)
```

### Buffer round-trip

Open a database from a `Buffer` (e.g. the bytes of a `.sqlite` file
embedded in a ZPK), mutate it, and write the result back into a fresh
`Buffer`:

```zym
// Load a config DB stored inside a ZPK archive.
var bytes = bundle.open("config.sqlite")     // returns a Buffer
var db    = SQLite.open(bytes)

db.prepare("UPDATE settings SET value = ? WHERE key = ?").run(newValue, "theme")

// Hand back a fresh buffer to whatever wants to persist the DB.
var updated = db.serialize()
db.close()

// `updated` can now be written into a ZPK entry, sent over a socket,
// or held in memory for later.
```

This path never touches the filesystem, so it's well suited to
configuration storage, save files, and shipping small databases over
the network.

### Pragmas

```zym
print(db.pragma("journal_mode", { simple: true }))   // "memory"
print(db.pragma("foreign_keys", { simple: true }))   // 0

db.pragma("journal_mode = WAL")                       // returns the new mode as a row
db.pragma("foreign_keys = ON")
```

### Big integers

```zym
var stmt = db.prepare("SELECT 9223372036854775807 AS big")

print(stmt.get().big)                // 9.2233720368548e+18 — precision lost
stmt.safeIntegers(true)
print(stmt.get().big)                // "9223372036854775807" — exact, as a string

// To bind a string back as an int64, use safeIntegers on the binding
// side too:
var q = db.prepare("SELECT * FROM events WHERE id = ?")
q.safeIntegers(true)
q.get("9223372036854775807")
```

Without `safeIntegers`, the integer value is squashed to the nearest
double on the way in and out. Switching it on costs nothing for values
that already fit in 53 bits.

### Reflection

```zym
var stmt = db.prepare("SELECT id, name AS who FROM users")
print(stmt.columns())
// [{"name":"id","column":"id","table":"users","database":"main","type":"INTEGER"},
//  {"name":"who","column":"name","table":"users","database":"main","type":"TEXT"}]
print(stmt.reader())     // true
print(stmt.readonly())   // true
```

---

## Deviations from `better-sqlite3`

For consistency with the rest of zym, the binding makes three small
departures from the upstream JavaScript API:

1. `SQLite.open(...)` is a factory function, since zym has no `new`.
2. `db.transaction(fn, mode)` takes the transaction mode as a second
   argument instead of attaching `.deferred` / `.immediate` /
   `.exclusive` to the returned function. zym functions don't carry
   attached properties.
3. `stmt.iterate(...)` returns an explicit `{ next(), return() }`
   object instead of an iterator protocol object; zym has no
   `for...of` construct, so iteration is always `while (r != null)`.

A few less-frequently-used pieces of the upstream surface
(`db.function`, `db.aggregate`, `db.backup`, `db.loadExtension`,
`db.table`) are not yet bound. They can be added without breaking the
existing API.

---

## Capability extension over `better-sqlite3`

`SQLite.open(buffer)` and `db.serialize()` together let scripts load
and store databases as in-memory byte buffers. This isn't part of the
better-sqlite3 surface but falls out cleanly from SQLite's own
`sqlite3_serialize` / `sqlite3_deserialize` C API. The intended use
cases are:

- Loading a config or save-state database out of a ZPK archive entry,
  mutating it, and writing it back into the archive.
- Streaming a small database over a socket without staging it on disk.
- Snapshotting an in-memory database for later restoration.

The buffer is copied into SQLite-owned memory on `open`, so the
original `Buffer` can be freed (or overwritten) immediately afterward.
