# `Dir`

Directory I/O. The global identifier `Dir` is a namespace of static helpers and
opener constructors. Opening a directory returns a **directory handle** whose
instance methods are invoked as `d.method(...)`.

---

## Conventions

- **Paths.** Strings are interpreted as filesystem paths (absolute or relative
  to the current working directory). No virtual-filesystem prefixes are
  applied.
- **Names vs paths.** Instance methods that take a `name` expect a relative
  entry under the handle's current directory. Methods that take a `path`
  accept either an absolute path or a path relative to the current directory.
- **Numbers.** Counts, indices, and byte sizes are Zym numbers. Integer
  arguments are truncated toward zero.
- **Booleans.** Most mutating operations return `true` on success, `false` on
  failure. Query methods return a `bool` directly.
- **Lists.** Methods that enumerate entries (`files`, `directories`) return
  Zym lists of strings. Order is filesystem-dependent and not guaranteed.
- **Open failures.** `open` / `openTemp` return `null` on failure (check with
  `if d == null`).
- **Errors.** Invalid argument types, or instance methods called on a closed /
  invalid handle, produce a Zym runtime error of the form
  `Dir.method(args) ...`.

---

## Opening directories

| Function | Returns |
| --- | --- |
| `Dir.open(path)` | handle, or `null` on failure |
| `Dir.openTemp(prefix, keep)` | handle to a newly-created temporary directory, or `null` |

`openTemp`'s `prefix` is a leaf-name prefix for the generated directory; when
`keep` is `false` the directory is cleaned up on handle release, when `true`
it is left in place.

---

## Static helpers

Convenience wrappers that operate on a path without opening a handle.

| Function | Returns | Notes |
| --- | --- | --- |
| `Dir.exists(path)` | bool | `true` if `path` resolves to an existing directory. |
| `Dir.makeDir(path)` | bool | Creates a single directory; parent must exist. |
| `Dir.makeDirRecursive(path)` | bool | Creates the directory and any missing parents. |
| `Dir.copy(src, dst)` | bool | Copies a single file at `src` to `dst`. |
| `Dir.rename(src, dst)` | bool | Renames / moves `src` to `dst`. |
| `Dir.remove(path)` | bool | Removes a file or an *empty* directory at `path`. |
| `Dir.files(path)` | list of strings | Names of regular files directly under `path`. |
| `Dir.directories(path)` | list of strings | Names of subdirectories directly under `path`. |
| `Dir.driveCount()` | number | Number of logical drives (platform-dependent; 0 on Unix-likes). |
| `Dir.driveName(idx)` | string | Name of drive `idx`; empty string if out of range. |

`Dir.remove` only removes empty directories. To clear a directory tree use a
handle and call `eraseContentsRecursive()`, or pair `Dir.remove` with a
recursive enumeration of `files()` / `directories()`.

---

## Handle state

| Method | Returns | Notes |
| --- | --- | --- |
| `d.path()` | string | Absolute path of the handle's current directory. |
| `d.changeDir(path)` | bool | Navigate to `path`; resolves `.` / `..`. Returns `false` if the target doesn't exist. |

---

## Queries on entries

Entry-relative queries. `name` is resolved against the handle's current
directory.

| Method | Returns | Notes |
| --- | --- | --- |
| `d.fileExists(name)` | bool | `true` if `name` is a regular file. |
| `d.dirExists(name)` | bool | `true` if `name` is a directory. |
| `d.isReadable(name)` | bool | |
| `d.isWritable(name)` | bool | |
| `d.isLink(name)` | bool | `true` if `name` is a symbolic link. |
| `d.readLink(name)` | string | Target path of a symbolic link; empty string if `name` is not a link. |
| `d.spaceLeft()` | number | Free bytes on the filesystem containing the current directory. |

---

## Mutations

| Method | Returns | Notes |
| --- | --- | --- |
| `d.makeDir(path)` | bool | Creates a single directory under the current directory (or at an absolute path). |
| `d.makeDirRecursive(path)` | bool | Creates the directory and any missing parents. |
| `d.copy(src, dst)` | bool | Copies a single file. |
| `d.rename(src, dst)` | bool | Renames / moves an entry. |
| `d.remove(name)` | bool | Removes a file or an empty directory. |
| `d.eraseContentsRecursive()` | bool | Recursively deletes every entry inside the current directory (the directory itself is kept). Dangerous — use with care. |
| `d.createLink(src, dst)` | bool | Creates a symbolic link at `dst` pointing at `src`. |

---

## Enumeration

Two styles are supported. Pick whichever fits the script:

**Snapshot style** — returns the whole listing as a list of names.

| Method | Returns |
| --- | --- |
| `d.files()` | list of strings |
| `d.directories()` | list of strings |

**Iterator style** — walks one entry at a time, exposes per-entry metadata
without allocating a list.

| Method | Returns | Notes |
| --- | --- | --- |
| `d.listBegin()` | bool | Starts iteration. Returns `false` if the directory can't be opened. |
| `d.listNext()` | string or `null` | Next entry name, or `null` when enumeration is finished. |
| `d.listCurrentIsDir()` | bool | `true` if the entry returned by the last `listNext` is a directory. |
| `d.listCurrentIsHidden()` | bool | `true` if the entry is hidden (leading `.` on Unix, hidden attribute on Windows). |
| `d.listEnd()` | null | Releases iterator state. Safe to call even after `listNext` returned `null`. |

Between `listBegin` and `listEnd`, each call to `listNext` advances the
iterator and then `listCurrentIsDir` / `listCurrentIsHidden` reflect the entry
just returned.

---

## Listing filters

Applies to both snapshot and iterator enumeration.

| Method | Returns | Notes |
| --- | --- | --- |
| `d.setIncludeNavigational(b)` | null | If `true`, `.` and `..` are included in listings. Default: `false`. |
| `d.setIncludeHidden(b)` | null | If `true`, hidden entries are included. Default: `false`. |
| `d.includeNavigational()` | bool | Current setting. |
| `d.includeHidden()` | bool | Current setting. |

---

## Filesystem metadata

| Method | Returns | Notes |
| --- | --- | --- |
| `d.isCaseSensitive(path)` | bool | Whether names under `path` are case-sensitive. Platform- and filesystem-dependent. |
| `d.filesystemType()` | string | Platform name of the filesystem hosting the current directory (e.g. `"ext4"`, `"NTFS"`, `"APFS"`). Empty string if unavailable. |

---

## Drives

Drive APIs are primarily meaningful on Windows; on Unix-likes `driveCount()`
is `0` and `drive(idx)` returns an empty string.

| Method | Returns |
| --- | --- |
| `d.driveCount()` | number |
| `d.drive(idx)` | string |
| `d.currentDrive()` | number — index of the drive backing the current directory |

---

## Example

```zym
var d = Dir.open(".")
if (d == null) {
    print("cannot open cwd")
} else {
    d.setIncludeHidden(false)

    var dirs = d.directories()
    for (var i = 0; i < length(dirs); i = i + 1) {
        print("dir : %s", dirs[i])
    }

    var files = d.files()
    for (var i = 0; i < length(files); i = i + 1) {
        print("file: %s", files[i])
    }
}

if (!Dir.exists("build/tmp")) {
    Dir.makeDirRecursive("build/tmp")
}

var t = Dir.openTemp("zym_", false)  // auto-cleaned when handle drops
t.makeDir("cache")
```

---

## Notes & gotchas

- **`remove` on directories requires empty.** `Dir.remove(path)` /
  `d.remove(name)` refuse to delete a non-empty directory. Use
  `eraseContentsRecursive()` first, or walk children manually.
- **`eraseContentsRecursive` is destructive and non-recoverable.** It does
  not move entries to a trash / recycle bin. Confirm `d.path()` before
  calling it.
- **Hidden / navigational entries are filtered by default.** `.` and `..`
  and hidden names do not appear in `files()` / `directories()` /
  `listNext()` unless explicitly enabled via `setIncludeNavigational(true)`
  / `setIncludeHidden(true)`.
- **Symlinks are not followed for `isLink`.** `fileExists` / `dirExists`
  follow symlinks; `isLink` reports the link itself. Use `readLink` to get
  the target.
- **`listBegin` must be paired with `listEnd`.** Leaving an iterator open
  holds a directory handle at the OS level. `listEnd` is safe to call
  multiple times.
- **`createLink` may fail without elevated privileges on Windows.** On
  Unix-likes symlink creation is unprivileged.
- **Handle aliasing.** Assigning a `Dir` handle (`d2 = d1`) aliases the
  underlying directory; operations on either reference affect the same
  position / filter state. Open a second handle with `Dir.open` if you
  need independent iteration.
- **Closing is implicit.** The directory handle is released when the last
  reference is dropped. There is no explicit `close()`; ensure iterators
  are terminated with `listEnd` if you intend to keep the handle alive.
