# `Zym`

Nested in-process VM API. The global identifier `Zym` is a top-level
singleton namespace (no `create(...)` constructor of its own); methods
are invoked as `Zym.method(...)`.

`Zym` lets a script spin up a fresh VM *inside* the current process,
wire it up exactly the way it wants, and bridge values across the
boundary. The parent script plays the role of an embedder for its
child: it decides which native modules the child can see, defines the
child's globals, registers parent-side natives the child can call,
drives compile/run, and reads diagnostics back.

For OS-specific information that feeds these utilities (user home,
data/config/cache directories, executable path, env vars) see
`docs/system.md`. For binary blob handling that crosses VM boundaries
see `docs/buffer.md`.

---

## Conventions

- **Capability-gated.** `Zym` itself is a regular catalog entry: a
  child VM has it only if its parent granted it (via
  `registerCliNative("Zym")` or `"ALL"`). A child that wasn't granted
  `Zym` has no `Zym` global in scope and cannot spawn nested VMs at
  all. The ability to sandbox *is itself* a capability the parent can
  withhold.
- **Calling-VM perspective.** Every `Zym.*` method answers in terms of
  *the VM the script is running in*. `Zym.cliNatives()` returns
  *this* VM's grantable set, never some hidden global list. From
  inside a sandboxed child, names that were not granted are simply
  not observable.
- **`Buffer` is implicit.** `Buffer` is the only always-present native
  — it is part of the language surface (treat it like
  `list` / `map` / `string`) and is **not** returned from
  `cliNatives()`. Every other native — including `Zym` itself — is
  a grantable catalog entry.
- **Setup → execution is one-way.** A fresh child VM begins in
  *setup phase*. The first call to any of `compile`, `runChunk`,
  `call`, or `deserializeChunk` flips it into *execution phase*.
  Setup-only methods (`registerCliNative`, `defineGlobal`,
  `registerNative`) cease to be callable after the flip; re-running
  the same chunk does not reopen setup.
- **Errors.** Bad argument types or arity raise a Zym runtime error
  of the form `Zym.method(args) ...`. *Capability and lifecycle
  failures all collapse to a single* `no such native` *error* —
  unknown name, name withheld by the parent, calling a method on a
  freed VM, or calling a setup method on a VM that has already
  entered the execution phase. A sandboxed child cannot distinguish
  between these cases by error message.
- **Pipeline status codes.** The mutating pipeline calls (`compile`,
  `runChunk`, `call`, `deserializeChunk`) return values from
  `Zym.STATUS`: `OK` (`0`), `COMPILE_ERROR` (`1`),
  `RUNTIME_ERROR` (`2`), `YIELD` (`3`). `runChunk` and `call`
  auto-loop on `YIELD` until the call resolves to `OK` or a non-yield
  error.

---

## Top-level singleton

| Method | Returns | Notes |
| --- | --- | --- |
| `Zym.cliNatives()` | `[string]` | The list of native names the calling VM is allowed to grant to its children, in grant order (catalog declaration order at the root). `Buffer` is omitted (it is universal, not grantable). |
| `Zym.newVM()` | `ChildVM` | Allocates a fresh in-process VM. The returned value is a struct of method closures (see below) bound to the new VM. The child's allocator is inherited from the parent (not script-selectable). The child starts with `Buffer` auto-installed and an empty grantable set; the parent must explicitly grant any other natives via `registerCliNative` *before* the child enters the execution phase. |
| `Zym.STATUS` | `map` | Status code constants returned by pipeline calls: `OK`, `COMPILE_ERROR`, `RUNTIME_ERROR`, `YIELD`. |

---

## ChildVM (returned by `Zym.newVM`)

Every method below is reached through the value returned by
`Zym.newVM()`.

### Setup-phase methods

Only valid before the child enters execution phase. After the flip
they raise `no such native`.

| Method | Returns | Notes |
| --- | --- | --- |
| `cv.registerCliNative(arg)` | `bool` | Grants one or more native modules to the child. `arg` is a single name (`"File"`), a list of names (`["File", "Path"]`), or the literal string `"ALL"`. Names must be in the calling VM's own grantable set; granting a name that is unknown *or* withheld raises `no such native` (same error in both cases). Idempotent: re-granting a name already on the child is a silent no-op. With `"ALL"`, every name from the calling VM's grantable set is granted in declaration order. |
| `cv.defineGlobal(name, value)` | `bool` | Defines a global on the child. The value is marshalled across the VM boundary as a full graph copy (primitives, strings, lists, maps, structs, enums, `Buffer` byte-copy, and closures wrapped as cross-VM callables). Last-write-wins on name collision (matches the underlying C API). |
| `cv.registerNative(signature, fn)` | `bool` | Registers a parent closure as a native on the child. `signature` follows the C-side `"name(arg1, arg2)"` convention and also accepts the script-natural rest form `"name(a, ...rest)"` / `"name(...rest)"` for variadics. The child sees a regular native; calling it from the child marshals args back to the parent, runs `fn`, and marshals the result back to the child. |

### Query

| Method | Returns | Notes |
| --- | --- | --- |
| `cv.cliNatives()` | `[string]` | The names this child has been granted, in grant order. Equivalent to "what this child could grant onward". |
| `cv.hasFunction(name, arity)` | `bool` | True iff the child has a top-level function `name` with the given arity. |
| `cv.diagnostics()` | `[map]` | Drains the child's diagnostic sink. Each entry is `{ severity, file, fileId, line, column, startByte, length, message }`. `severity` is one of `"error"`, `"warning"`, `"info"`, `"hint"`. After this call the child's sink is empty. |

### Pipeline

Mirror `full_executor.cpp` step-for-step.

| Method | Returns | Notes |
| --- | --- | --- |
| `cv.newSourceMap()` | `SourceMap` | Allocates a fresh source map on the child. The returned value is a small struct exposing `free()`. Released automatically when the parent VM tears down. |
| `cv.newChunk()` | `Chunk` | Allocates a fresh chunk on the child. Same lifetime semantics as `SourceMap`. |
| `cv.registerSourceFile(path, source)` | `int` (fileId) | Registers a buffer with the child's file registry. The returned `fileId` is what `preprocess` and diagnostics use to refer to this source. |
| `cv.preprocess(source, sourceMap, fileId)` | `{ source, status }` | Runs the preprocessor. On success, `source` is the expanded buffer and `status == Zym.STATUS.OK`; on failure `source` is `null` and the status is non-`OK` (drain via `diagnostics()`). |
| `cv.compile(source, chunk, sourceMap, entryFile, opts)` | `int` (status) | Compiles `source` into `chunk`. `sourceMap` may be `null` for raw text. `opts.includeLineInfo` (default `true`) controls whether line info is embedded. **Flips the child into execution phase.** |
| `cv.loadModules(source, sourceMap, entryFile, callback, opts)` | `{ status, combinedSource, modulePaths }` or `{ status, error }` | Multi-file compile. The parent `callback(path)` mirrors the C `readAndPreprocessCallback`: it must return `{ source, sourceMap, fileId }` for each imported module, or `null` to signal a missing file. Returns the combined preprocessed source plus the resolved module paths on success. |
| `cv.serializeChunk(chunk, opts)` | `{ status, bytes }` | Serializes a compiled chunk to a `Buffer` of `.zbc` bytes. `opts.includeLineInfo` mirrors compile. |
| `cv.deserializeChunk(chunk, bytes)` | `int` (status) | Loads `.zbc` bytes (a `Buffer`) into a freshly-allocated chunk. **Flips the child into execution phase.** |
| `cv.runChunk(chunk)` | `int` (status) | Runs a compiled or deserialized chunk on the child. Auto-loops on `YIELD`. **Flips the child into execution phase.** |
| `cv.call(name, args)` | `int` (status) | Calls a top-level function on the child by name with positional `args` (a list). Args are marshalled across the VM boundary (full graph copy). Auto-loops on `YIELD`. **Flips the child into execution phase.** |
| `cv.callResult()` | `value` | Returns the marshalled return value of the most recent successful `cv.call(...)`. Lists, maps, structs, enums, Buffers, and closures (wrapped on the parent side) all round-trip back. |

### Lifecycle

| Method | Returns | Notes |
| --- | --- | --- |
| `cv.freeChunk(chunk)` | `bool` | Releases a chunk's resources early. Equivalent to `chunk.free()`. Returns `true` on the first call, `false` if already freed. |
| `cv.free()` | `bool` | Tears the child VM down explicitly. Returns `true` on the first call, `false` if already freed. After `free()`, every other ChildVM method raises `no such native`. The child is also freed automatically when the parent VM tears down its globals — `free()` is just a way to release resources earlier. |

---

## Examples

### Spawn, grant, and inspect

```zym
var sandbox = Zym.newVM()
sandbox.registerCliNative(["File", "Path"])
print(sandbox.cliNatives())          // [File, Path]

// Grant the universe at once.
var open = Zym.newVM()
open.registerCliNative("ALL")

// Idempotent re-grants — no error.
var sb = Zym.newVM()
sb.registerCliNative("Path")
sb.registerCliNative("Path")
sb.registerCliNative(["File", "Path"])
print(sb.cliNatives())               // [Path, File]

// Withhold the ability to nest further: leaf has no Zym in its set,
// so any code running inside it cannot spawn its own children.
var leaf = Zym.newVM()
leaf.registerCliNative(["File", "Dir", "Path"])
```

### Compile and run a script

```zym
var src = "func answer() { return 42 }"

var vm  = Zym.newVM()
var sm  = vm.newSourceMap()
var fid = vm.registerSourceFile("entry.zym", src)
var pre = vm.preprocess(src, sm, fid)
var ch  = vm.newChunk()

if (vm.compile(pre.source, ch, sm, "entry.zym", { includeLineInfo: true }) == Zym.STATUS.OK) {
    vm.runChunk(ch)
    if (vm.hasFunction("answer", 0)) {
        if (vm.call("answer", []) == Zym.STATUS.OK) {
            print(vm.callResult())   // 42
        }
    }
}
```

### Round-trip through `.zbc` bytes

`serializeChunk` returns a `Buffer`; that `Buffer` can cross VM
boundaries (byte-copy) and be fed to `deserializeChunk` on a fresh VM.

```zym
var ser = vm.serializeChunk(ch, { includeLineInfo: true })

var vm2 = Zym.newVM()
var ch2 = vm2.newChunk()
vm2.deserializeChunk(ch2, ser.bytes)
vm2.runChunk(ch2)
vm2.call("answer", [])
print(vm2.callResult())              // 42
```

### Calling regular (fixed-arity) functions

`cv.call` takes a list of positional args. The result of the call —
of any shape — is fetched with `cv.callResult()` after a successful
status.

```zym
var src = "
    func add(a, b) { return a + b }
    func describe(name, n) {
        return { who: name, count: n }
    }
"

var vm  = Zym.newVM()
var sm  = vm.newSourceMap()
var fid = vm.registerSourceFile("m.zym", src)
var pre = vm.preprocess(src, sm, fid)
var ch  = vm.newChunk()
vm.compile(pre.source, ch, sm, "m.zym", { includeLineInfo: true })
vm.runChunk(ch)

vm.call("add", [2, 3])
print(vm.callResult())               // 5

vm.call("describe", ["ada", 10])
var d = vm.callResult()
print(d.who)                         // ada
print(d.count)                       // 10
```

### Calling variadic functions

The child's `func f(a, ...rest) { ... }` is reached the same way as
any other function. `cv.call` flattens the args list onto the
underlying call frame; the child's variadic binding packs the trailing
arguments into `rest` itself.

```zym
var src = "
    func collect(...parts) { return parts }
    func label(name, ...parts) { return [name, parts] }
"

var vm  = Zym.newVM()
var sm  = vm.newSourceMap()
var fid = vm.registerSourceFile("v.zym", src)
var pre = vm.preprocess(src, sm, fid)
var ch  = vm.newChunk()
vm.compile(pre.source, ch, sm, "v.zym", { includeLineInfo: true })
vm.runChunk(ch)

vm.call("collect", [1, 2, 3, 4])
print(vm.callResult())               // [1, 2, 3, 4]

vm.call("label", ["nums", 10, 20, 30])
print(vm.callResult())               // [nums, [10, 20, 30]]
```

### Setting and reading globals

`defineGlobal` sets a global on the child during setup phase. There
is no `getGlobal`; reading a value back from the child is done by
calling a child function that returns it (or by sending a Buffer
back). This mirrors the C API and keeps the bridge a single
direction at any one moment.

```zym
var src = "
    func userInfo() { return { name: USER, limit: LIMIT, debug: DEBUG } }
    func tags()     { return TAGS }
"

var vm = Zym.newVM()

// Primitives, lists, maps, structs all marshal across.
vm.defineGlobal("USER",  "ada")
vm.defineGlobal("LIMIT", 100)
vm.defineGlobal("DEBUG", true)
vm.defineGlobal("TAGS",  ["alpha", "beta", "gamma"])

var sm  = vm.newSourceMap()
var fid = vm.registerSourceFile("g.zym", src)
var pre = vm.preprocess(src, sm, fid)
var ch  = vm.newChunk()
vm.compile(pre.source, ch, sm, "g.zym", { includeLineInfo: true })
vm.runChunk(ch)

vm.call("userInfo", [])
var info = vm.callResult()
print(info.name)                     // ada
print(info.limit)                    // 100
print(info.debug)                    // true

vm.call("tags", [])
print(vm.callResult())               // [alpha, beta, gamma]
```

### Registering a parent closure as a child native (fixed arity)

`registerNative` exposes a parent-side closure to the child as a
regular native. When the child calls it, arguments are marshalled to
the parent, the closure runs, and its return value is marshalled back
into the child.

```zym
var src = "
    func go(n) { return double(n) + 1 }
"

var vm = Zym.newVM()
vm.registerNative("double(x)", func(x) { return x * 2 })

var sm  = vm.newSourceMap()
var fid = vm.registerSourceFile("n.zym", src)
var pre = vm.preprocess(src, sm, fid)
var ch  = vm.newChunk()
vm.compile(pre.source, ch, sm, "n.zym", { includeLineInfo: true })
vm.runChunk(ch)

vm.call("go", [10])
print(vm.callResult())               // 21
```

### Registering a parent closure as a child native (variadic)

The script-natural `...rest` form is accepted directly. Every
trailing argument the child passes is packed into `parts` on the
parent side, exactly as if the closure had been invoked from inside
the same VM.

```zym
var src = "
    func wrap()        { return collect(1, 2, 3, 4) }
    func wrapLabelled() { return label('nums', 10, 20, 30) }
"

var vm = Zym.newVM()
vm.registerNative("collect(...parts)",      func(...parts)        { return parts })
vm.registerNative("label(name, ...parts)",  func(name, ...parts)  { return [name, parts] })

var sm  = vm.newSourceMap()
var fid = vm.registerSourceFile("vn.zym", src)
var pre = vm.preprocess(src, sm, fid)
var ch  = vm.newChunk()
vm.compile(pre.source, ch, sm, "vn.zym", { includeLineInfo: true })
vm.runChunk(ch)

vm.call("wrap", [])
print(vm.callResult())               // [1, 2, 3, 4]

vm.call("wrapLabelled", [])
print(vm.callResult())               // [nums, [10, 20, 30]]
```

### Marshalling bridge: lists, maps, structs, Buffers, closures

Every value crossing the VM boundary is copied. Mutating one side's
copy has no effect on the other.

```zym
var src = "
    func roundtrip(v) { return v }                            // identity test
    func fields(m)    { return [m.name, m.count, m.tags] }
    func bump(buf)    { buf[0] = 99; return buf }              // mutates child copy
"

var vm = Zym.newVM()
var sm = vm.newSourceMap()
var fid = vm.registerSourceFile("b.zym", src)
var pre = vm.preprocess(src, sm, fid)
var ch  = vm.newChunk()
vm.compile(pre.source, ch, sm, "b.zym", { includeLineInfo: true })
vm.runChunk(ch)

// Lists round-trip (deep copy).
vm.call("roundtrip", [[1, 2, 3]])
print(vm.callResult())               // [1, 2, 3]

// Maps round-trip.
vm.call("fields", [{ name: "ada", count: 3, tags: ["a", "b"] }])
print(vm.callResult())               // [ada, 3, [a, b]]

// Buffers cross by byte-copy: the parent's buf is unchanged.
var buf = Buffer.create(4)
buf[0] = 1
vm.call("bump", [buf])
var out = vm.callResult()
print(buf[0])                        // 1   (parent untouched)
print(out[0])                        // 99  (child's mutated copy, marshalled back)
```

Closures cross both directions: a parent closure handed to a child
becomes a callable inside the child, and a closure returned *from* a
child call comes back as a callable on the parent. Calling either
re-enters the originating VM.

```zym
var src = "
    var counter = 0
    func makeCounter() { return func() { counter = counter + 1; return counter } }
"

var vm = Zym.newVM()
var sm = vm.newSourceMap()
var fid = vm.registerSourceFile("c.zym", src)
var pre = vm.preprocess(src, sm, fid)
var ch  = vm.newChunk()
vm.compile(pre.source, ch, sm, "c.zym", { includeLineInfo: true })
vm.runChunk(ch)

vm.call("makeCounter", [])
var bump = vm.callResult()           // a callable bound to the child VM

print(bump())                        // 1
print(bump())                        // 2
print(bump())                        // 3
```

---

## Notes

- **`Zym.cliNatives()` is per-VM, not global.** Two VMs in the same
  process can return different lists; that is the point of the
  capability sandbox. There is no way to query a global catalog from
  script.
- **`Zym` itself appears in the list when granted.** A parent that
  receives `Zym` as part of its own grant set sees `"Zym"` in
  `cliNatives()`; granting `"Zym"` to a grandchild then lets *that*
  grandchild nest further (subject to its own subset grants).
- **Allocator is inherited, never script-selectable.** `Zym.newVM()`
  is zero-arg: the child VM transparently reuses the parent's
  allocator. Memory configuration is not part of the script-visible
  surface.
- **`Buffer` is auto-installed on every child.** It does not need to
  be granted, and granting it is a no-op. It is the recommended way
  to pass bulk data across VM boundaries: Buffers cross by byte-copy,
  so the child's copy and the parent's copy are independent (mutating
  one never affects the other).
- **Capabilities can only shrink.** A child's grantable set is always
  a subset of its parent's. There is no API to broaden a set after
  the fact, and grants are not retroactive: once a child has been
  spawned with a given set, future grants to the parent never reach
  the existing child.
- **No `getGlobal`.** Reading a value back from the child is done by
  calling a child function that returns it. `defineGlobal` is the
  only direction in which globals cross the boundary, and it is
  setup-phase only.
- **Marshalling is a copy, not a share.** Lists, maps, structs,
  enums, and Buffers all cross by recursive deep copy (Buffers by
  byte-copy). Closures cross as opaque cross-VM callables — the
  closure itself stays in its origin VM, and invocations re-enter
  that VM through the bridge.
