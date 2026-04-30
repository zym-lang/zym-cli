# `Path`

A namespace of pure-string path utilities for portably manipulating
filesystem paths across operating systems. Registered at VM startup as
the global identifier `Path`; all methods are invoked as
`Path.method(...)`.

`Path` does **not** access the filesystem. Every method is a string-in /
string-or-bool-out transformation. For real filesystem queries (does
this exist? is it a file or a directory? how big is it?) use the `File`
and `Dir` natives.

For OS-specific information that feeds these utilities (user home,
data/config/cache directories, executable path, env vars) see
`docs/system.md`. `Path.join(System.dataDir(), ...)` is the
recommended pattern for building portable per-user output paths.

---

## Conventions

- **Strings only.** Every input is a string, every result is a string
  or boolean. Bad argument types raise a Zym runtime error of the form
  `Path.method(args) expects a string`.
- **No filesystem I/O.** `Path.normalize` does *not* resolve symlinks
  or check existence; it is purely textual `..` / `.` / multi-slash
  collapse. To follow symlinks or canonicalize against the live FS,
  combine `Path.normalize` with `Dir.exists` / `File.exists` checks.
- **POSIX-style names.** "basename" means different things in
  different ecosystems, so this API exposes both forms under explicit
  names: `stem` for *trailing component minus extension* and
  `basename` for *whole path minus extension*. `dirname` / `filename`
  / `extension` follow POSIX `basename`, Python `os.path`, and Node
  `path` conventions.
- **Cross-platform separator.** Methods that build paths use the host
  platform's native separator (`/` on Linux/macOS, `\` on Windows);
  literal `/` in your script is accepted as a separator on every
  platform for path input, but `Path.separator()` is the source of
  truth for the *output* separator on the current host.

---

## Query

| Method | Returns | Notes |
| --- | --- | --- |
| `Path.isAbsolute(p)` | bool | `true` if `p` starts at a root (Unix `/foo`, Windows `C:\foo`). |
| `Path.isRelative(p)` | bool | Inverse of `isAbsolute`. Empty string returns `true` (it is relative). |
| `Path.isNetworkShare(p)` | bool | `true` for UNC paths like `//server/share` or `\\server\share`. |

## Split

| Method | Returns | Notes |
| --- | --- | --- |
| `Path.dirname(p)` | string | Everything up to (but not including) the trailing component. `dirname("/a/b/c.txt")` → `"/a/b"`; `dirname("/")` → `"/"`. |
| `Path.filename(p)` | string | The trailing component. `filename("/a/b/c.txt")` → `"c.txt"`; `filename("/a/b/")` → `""`. |
| `Path.extension(p)` | string | Rightmost extension, **without** the leading dot. `"/a/b.tar.gz"` → `"gz"`. Empty string if there is no extension. |
| `Path.stem(p)` | string | Trailing component minus the rightmost extension. `"/a/b/c.txt"` → `"c"`; `"/a/b.tar.gz"` → `"b.tar"`. |
| `Path.basename(p)` | string | Whole path minus the rightmost extension. `"/a/b/c.txt"` → `"/a/b/c"`. Use `stem` if you want only the trailing component without extension. |

## Build

| Method | Returns | Notes |
| --- | --- | --- |
| `Path.join(...)` | string | Variadic join. Empty segments are skipped. If a later segment is itself absolute, it rebases the result (matches POSIX `os.path.join`). `join()` returns `""`. |
| `Path.normalize(p)` | string | Textually resolves `.` and `..`, collapses doubled separators, trims a trailing separator. **Does not touch the filesystem.** |
| `Path.relative(from, to)` | string | Returns the relative path from `from` to `to`. `from` is treated as a *file*: its parent directory is the basis. `relative("/a/b", "/a/c")` → `"../c"`. |
| `Path.withExtension(p, ext)` | string | Replaces (or appends) the rightmost extension. `ext` accepts `"md"` and `".md"`; `""` drops the extension. |
| `Path.expandUser(p)` | string | Expands a leading `~` or `~/...` to the current user's home directory (`$HOME` on Unix, `%USERPROFILE%` or `%HOMEDRIVE%%HOMEPATH%` on Windows). The `~user` form is **not** expanded; non-tilde paths pass through unchanged. |

## Misc

| Method | Returns | Notes |
| --- | --- | --- |
| `Path.separator()` | string | The host platform's path separator: `"/"` on Linux/macOS, `"\\"` on Windows. Rarely needed in scripts that use `Path.join`. |

---

## Examples

### Build a per-user output path

```zym
var dir = Path.join(System.dataDir(), "my-tool")
Dir.makeRecursive(dir)
File.writeAllText(Path.join(dir, "config.json"), "{}")
```

### Inspect a path

```zym
var p = "/var/log/zym/run.2024-01-15.log"
print("dir : %s", Path.dirname(p))         // /var/log/zym
print("file: %s", Path.filename(p))        // run.2024-01-15.log
print("stem: %s", Path.stem(p))            // run.2024-01-15
print("ext : %s", Path.extension(p))       // log
```

### Replace an extension

```zym
var src = "/tmp/data.json"
var out = Path.withExtension(src, "min.json.gz")
// out -> "/tmp/data.min.json.gz"
```

### Resolve a tilde-rooted path

```zym
var cfg = Path.normalize(Path.expandUser("~/.config/my-tool"))
// cfg -> "/home/me/.config/my-tool" on Linux
```

### Compute a relative path

```zym
var rel = Path.relative("/srv/app/web/index.html", "/srv/app/static/logo.svg")
// rel -> "../static/logo.svg"
```

---

## Notes

- **`stem` vs. `basename`.** Other ecosystems disagree on which means
  what; this API uses `stem` for "filename without extension" and
  `basename` for "path without extension" so both forms are
  unambiguously available under explicit names.
- **`extension` returns the *rightmost* extension only.** Multi-suffix
  files like `archive.tar.gz` report `"gz"`, with `stem` =
  `"archive.tar"`. This matches POSIX and Python conventions.
- **`relative` treats `from` as a file.** If you want directory-to-
  directory relative paths, call `Path.relative(Path.join(fromDir,
  ".sentinel"), to)` or pass an explicit trailing component.
- **`normalize` is purely textual.** It does not resolve symbolic
  links, dereference `~`, or look at filesystem state. Compose with
  `Path.expandUser` and `Dir.exists` / `File.exists` for richer
  behavior.
- **`expandUser` only handles the current user.** The `~username` form
  is intentionally unsupported (the host has no portable way to
  resolve another user's home from script-side without shelling out).
  If you need it, drive `getent passwd <user>` via `Process.exec`.
- **No filesystem methods here.** Anything that asks "does it exist?"
  / "is it readable?" lives in `File` (`File.exists`, `File.size`,
  `File.modificationTime`) and `Dir` (`Dir.exists`, `Dir.list`,
  `Dir.makeRecursive`). `Path` is exclusively a string library.
