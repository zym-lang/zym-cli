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

## Quick start

The shortest end-to-end path: spin up a VM, register one parent
native, compile and run a script that uses it, call a function on the
child, then free the VM. `vm.run(src)` collapses
sourceMap + registerSourceFile + newChunk + compile + runChunk into a
single call and returns `{ status, result }`.

```zym
var src = "
    func greet(who) {
        return hostHello(who)
    }
"

var vm = Zym.newVM()
vm.registerNative("hostHello(who)", func(who) { return "hi " + who })

vm.run(src)

vm.call("greet", ["ada"])
print(vm.callResult())            // hi ada

vm.free()
```

That's the whole loop. Everything else in this document is either an
elaboration of one of these steps (capability grants, multi-file
compiles, bytecode round-trips, diagnostics) or a different value
shape crossing the bridge. The full pipeline (`newSourceMap`,
`registerSourceFile`, `newChunk`, `compile`, `runChunk`) remains
available when you need finer control.

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
| `cv.hasFunction(name, arity)` | `bool` | True iff the child has a top-level function `name` with exactly the given fixed arity. Strict slot probe. |
| `cv.hasFunc(name [, arity])` | `bool` | Existence probe with an optional arity. With one argument, returns `true` if **any** callable named `name` exists at any arity (fixed *or* variadic). With two arguments, returns `true` if calling `name` with exactly `arity` args can dispatch — i.e., either an exact fixed-arity match or a variadic with `arity >= fixed-prefix`. Useful for entry-point discovery (`if (vm.hasFunc("main")) ...`) before any `cv.call`. Not intended for hot paths. |
| `cv.diagnostics()` | `[map]` | Drains the child's diagnostic sink. Each entry is `{ severity, file, fileId, line, column, startByte, length, message }`. `severity` is one of `"error"`, `"warning"`, `"info"`, `"hint"`. After this call the child's sink is empty. |
| `cv.moduleLoader.getCaller()` | `string` or `null` | Resolved module path of the **immediate parent** of the module whose `read_callback` is currently running. `null` when the *entry* module is being loaded (it has no caller). **Raises a runtime error if called outside of an active `loadModules` `read_callback` invocation.** See [Module loader context](#module-loader-context). |
| `cv.moduleLoader.getStack()` | `[string]` | The full chain of in-flight `read_callback` invocations on this VM. `stack[0]` is the entry module, `stack[stack.length - 1]` equals the `path` the current callback was invoked with. `getCaller()` is equivalent to `stack[stack.length - 2]` (or `null` at the entry). **Raises a runtime error if called outside of an active `read_callback` invocation.** |

### Pipeline

Mirror `full_executor.cpp` step-for-step.

| Method | Returns | Notes |
| --- | --- | --- |
| `cv.newSourceMap()` | `SourceMap` | Allocates a fresh source map on the child. The returned value is a small struct exposing `free()`. Released automatically when the parent VM tears down. |
| `cv.newChunk()` | `Chunk` | Allocates a fresh chunk on the child. Same lifetime semantics as `SourceMap`. |
| `cv.registerSourceFile(path, source)` | `int` (fileId) | Registers a buffer with the child's file registry. The returned `fileId` is what `preprocess` and diagnostics use to refer to this source. |
| `cv.preprocess(source, sourceMap, fileId)` | `{ source, status }` | Runs the preprocessor. On success, `source` is the expanded buffer and `status == Zym.STATUS.OK`; on failure `source` is `null` and the status is non-`OK` (drain via `diagnostics()`). |
| `cv.compile(source, chunk, sourceMap, entryFile, opts)` | `int` (status) | Compiles `source` into `chunk`. `sourceMap` may be `null` for raw text. `opts.includeLineInfo` (default `true`) controls whether line info is embedded. **Flips the child into execution phase.** |
| `cv.loadModules(source, sourceMap, entryFile, callback, opts)` | `{ status, combinedSource, combinedSourceMap, modulePaths }` or `{ status, error }` | Multi-file compile. The parent `callback(path)` mirrors the C `readAndPreprocessCallback`: it must return `{ source, sourceMap, fileId }` for each imported module (the per-module `sourceMap` is the one produced by `preprocess` for *that module's raw source*), or `null` to signal a missing file. The native trampoline deep-clones the per-module SourceMap into the child VM so the parent wrapper retains ownership safely. On success, returns the combined preprocessed source together with the **combined** `SourceMap` (`combinedSourceMap`) — that's the map that must be passed to `compile`, not the entry-only map. `opts.resolveCallback` (optional closure `func(path) -> string \| null`) installs a resolver hook that runs **before** the loader's cycle detector and module cache: returning a non-null string overrides the canonical key the loader uses for cycle detection, caching, and the subsequent `read_callback(path)` argument; returning `null` (or omitting the option) keeps the loader's default root-relative resolution byte-identical to today. See [Resolve callback](#resolve-callback). |
| `cv.serializeChunk(chunk, opts)` | `{ status, bytes }` | Serializes a compiled chunk to a `Buffer` of `.zbc` bytes. `opts.includeLineInfo` mirrors compile. |
| `cv.deserializeChunk(chunk, bytes)` | `int` (status) | Loads `.zbc` bytes (a `Buffer`) into a freshly-allocated chunk. **Flips the child into execution phase.** |
| `cv.runChunk(chunk)` | `int` (status) | Runs a compiled or deserialized chunk on the child. Auto-loops on `YIELD`. **Flips the child into execution phase.** |
| `cv.run(source)` | `{ status, result }` | One-shot helper: registers a hidden source file, **runs the preprocessor**, compiles, and runs the chunk in a single call. `source` is a `string` or a `Buffer` of utf-8 source bytes (not a `.zbc` Buffer — use `runBytecode` for that). `status` is a `Zym.STATUS` code; `result` is the marshalled top-level return value (or `null` on non-`OK` status). **Flips the child into execution phase.** |
| `cv.runBytecode(bytes)` | `{ status, result }` | One-shot helper for serialized bytecode: deserializes `bytes` (a `Buffer` produced by `serializeChunk`) into a fresh chunk and runs it. Same return shape as `run`. **Flips the child into execution phase.** |
| `cv.call(name, args)` | `int` (status) | Calls a top-level function on the child by name with positional `args` (a list). Args are marshalled across the VM boundary (full graph copy). Auto-loops on `YIELD`. **Flips the child into execution phase.** |
| `cv.callv(name, ...args)` | `int` (status) | Positional-args sibling to `cv.call`. `vm.callv("greet", "ada")` is equivalent to `vm.call("greet", ["ada"])`, just spelled with positional args at the call site rather than an explicit list. Both write to the same backing slot, so `cv.callResult()` reads the result of whichever was used most recently. Mirrors the C `zym_callv` / `zym_call` split. |
| `cv.callResult()` | `value` | Returns the marshalled return value of the most recent successful `cv.call(...)` / `cv.callv(...)`. Lists, maps, structs, enums, Buffers, and closures (wrapped on the parent side) all round-trip back. |
| `cv.getFunc(name)` | `callable` or `null` | Returns a parent-side callable that forwards into the child function set named `name` (every fixed overload + any variadic, with overload resolution performed by the child per call). Invoking it returns the marshalled value directly — no `callResult` step needed. Returns `null` if no such name exists on the child. **Identity-stable:** calling `getFunc(name)` twice on the same VM returns the same callable. |

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

### One-shot `run` / `runBytecode`

When you don't need to keep the source map or chunk around, `vm.run`
collapses the boilerplate into a single call. It accepts either a
source `string` or a `Buffer` carrying utf-8 source bytes, and returns
`{ status, result }` where `result` is the top-level return value of
the script (or `null` on non-`OK` status).

```zym
var vm = Zym.newVM()
var r  = vm.run("func answer() { return 42 } answer()")
print("%v", r)                       // {"status": 0, "result": 42}

// Source from a Buffer (e.g. read from a file or sent across a VM
// boundary) works the same.
var blob = Buffer.fromString("func three() { return 3 } three()")
print("%v", Zym.newVM().run(blob))   // {"status": 0, "result": 3}

// run runs the preprocessor first, so directives expand transparently.
var pp = Zym.newVM().run("#define ANSWER 42\nfunc a() { return ANSWER } a()")
print("%v", pp)                      // {"status": 0, "result": 42}
```

`runBytecode` is the equivalent for serialized `.zbc` bytes — pass it
the `Buffer` returned by `serializeChunk` and it deserializes + runs
in one shot:

```zym
// Compile once, ship the bytes, run somewhere else.
var ser = vm.serializeChunk(ch, { includeLineInfo: true })

var vm2 = Zym.newVM()
print("%v", vm2.runBytecode(ser.bytes))   // {"status": 0, "result": ...}
```

Both helpers flip the child into execution phase, so any
setup-only call (`registerCliNative`, `defineGlobal`,
`registerNative`) must happen *before* `run` / `runBytecode`.

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

### Multi-file compile with `loadModules`

`loadModules` mirrors the C-side `readAndPreprocessCallback` pipeline:
the parent supplies a `callback(path)` that resolves each `import` it
encounters, and `loadModules` returns a single combined preprocessed
source ready for `compile`. The callback runs with the **parent's**
capabilities (it's a parent closure), so a child without `File`
cannot read modules — the closure simply isn't expressible.

The callback must return a map shaped like the C `ModuleReadResult`:

```zym
{ source: <string>, sourceMap: <SourceMap>, fileId: <int> }
```

`sourceMap` is the per-module map produced by `preprocess` for *that
module's raw source* — exactly the same thing the C
`readAndPreprocessCallback` hands back. The native trampoline
deep-clones it into the child VM's allocator before forwarding to
`loadModules`, so there's no cross-allocator hazard: the parent
wrapper retains ownership of the original (and is freed by its own
finalizer / explicit `.free()`), while the clone is owned by
`loadModules` and released through the child's allocator. Returning
the per-module map is what gives diagnostics full sub-line origin
precision (originStartByte / originLength / originLine point at the
exact byte range in the raw module source). Passing `null` is also
accepted: origin attribution falls back to `fileId` for every line of
`source`, which is correct at file/line granularity but loses
sub-line precision after preprocessor expansion.

Returning `null` (instead of a map) from the callback signals
"file not found"; the loader will push a diagnostic and continue.

```zym
var entry = "
    import \"./mathx\"
    import \"./greet\"
    func main() {
        return greet_for(\"ada\", mathx_double(21))
    }
"
var mathx = "func mathx_double(x) { return x * 2 }"
var greet = "
    func greet_for(name, n) {
        return { who: name, count: n }
    }
"

var vm  = Zym.newVM()
vm.registerCliNative(["File", "Path"])    // grant whatever the callback needs

var sm  = vm.newSourceMap()
var fid = vm.registerSourceFile("entry.zym", entry)
var pre = vm.preprocess(entry, sm, fid)

// In real code the callback would consult File / Path; here we serve
// a tiny in-memory module map so the example is self-contained.
var modules = {
    "./mathx": mathx,
    "./greet": greet,
}

var loaded = vm.loadModules(pre.source, sm, "entry.zym",
    func(path) {
        if (!modules[path]) { return null }       // miss → diagnostic
        var raw    = modules[path]
        var sub    = vm.registerSourceFile(path, raw)
        var sub_sm = vm.newSourceMap()             // per-module map
        var pp     = vm.preprocess(raw, sub_sm, sub)
        // Return the per-module SourceMap from preprocess so diagnostics
        // get full sub-line origin precision. The native trampoline
        // deep-clones it into the child VM; the parent wrapper retains
        // ownership of the original.
        return { source: pp.source, sourceMap: sub_sm, fileId: sub }
    },
    { debugNames: true }
)

if (loaded.status == Zym.STATUS.OK) {
    var ch = vm.newChunk()
    // Pass `loaded.combinedSourceMap` (NOT the entry-only `sm`) — it's
    // sized for the combined buffer. Using `sm` here would produce
    // misaligned diagnostics on the post-loader source.
    vm.compile(loaded.combinedSource, ch, loaded.combinedSourceMap, "entry.zym", { includeLineInfo: true })
    vm.runChunk(ch)
    vm.call("main", [])
    var r = vm.callResult()
    print(r.who)                                   // ada
    print(r.count)                                 // 42
    print(loaded.modulePaths)                      // [./mathx, ./greet, entry.zym]
} else {
    // loaded.error is set; full details are in vm.diagnostics()
    for (d in vm.diagnostics()) { print(d.message) }
}
```

Notes:

- The callback closes over whatever parent state it needs (`vm`, `sm`,
  a packed-bytecode index, a virtual filesystem, etc.). Different
  `loadModules` calls can pass different callbacks.
- `loaded.modulePaths` lists the resolved paths in load order — useful
  for diagnostics, caching, and watch-mode reloads.
- `loadModules` does **not** flip the child into execution phase on its
  own; the subsequent `compile` call does.
- `loaded.combinedSourceMap` is owned by the script after this call
  (transferred out of the internal result struct). It will be freed
  automatically when its wrapper is collected, or you can call
  `loaded.combinedSourceMap.free()` explicitly. When `loadModules`
  fails (`status != OK`), `combinedSourceMap` is absent.

### Module loader context

`read_callback(path)` keeps its single-argument shape — that's the 99%
case (resolve `path` against `SCRIPT_PATH`, read the file, return its
preprocessed source) and it's also the shape an MCU / runtime-only build
needs. For the cases where the callback needs to know *who* is asking
— typically because a previously-resolved module lives outside the
entry script's directory and now wants to do sibling imports — the
child VM handle exposes a small query surface:

- `vm.moduleLoader.getCaller()` — resolved path of the module that
  issued the `import("...")` that triggered this callback. `null` for
  the entry module.
- `vm.moduleLoader.getStack()` — `[entry, ..., currently-loading]`.
  Always non-empty inside a callback; the last element equals the
  `path` argument the callback was invoked with.

#### Scope of validity

Both methods are **only meaningful inside an active `read_callback`
invocation** on `vm`. Calling them at any other time — including from
a closure captured during a callback and invoked later, from a
coroutine resumed outside the loader, or from a different VM's
callback — raises a runtime error of the form:

```
vm.moduleLoader.getCaller(): not valid outside of a read_callback
invocation. This method is only meaningful while the module loader is
actively resolving an import.
```

The handle itself is fine to write down (`var ml = vm.moduleLoader`),
but the methods enforce the scope at call time. There is no silent
"return stale data" mode — misuse is always loud.

#### Caching semantics

`getStack()` reflects the chain that triggered the *load* of the
currently-resolving module, not the chain of every subsequent `import`
that resolves to the same module (those are served from the loader's
internal cache and never call back). Don't rely on per-import-site
chains; they are not well-defined when caching is in play, which is
the same constraint Node / esbuild / Deno loader hooks all operate
under.

#### Example: routing bare names to a data directory

A common case: `script.zym` writes `import("pie")` and expects the
loader to look in `System.dataDir()/zym/modules`, but `pie.zym` (which
lives in that data directory) writes `import("something.zym")` and
expects that to resolve relative to **itself**, not relative to
`script.zym`. The path passed to `read_callback` cannot distinguish
those two cases on its own — both come through as `"something.zym"`
keyed to root-relative space. `getCaller()` is what closes the gap.

```zym
var MODULE_PATH = Path.join(System.dataDir(), "zym", "modules")
var dataDirModules = {}            // resolved path -> true
var SCRIPT_PATH = Path.dirname(entryPath)

func readAndPreprocessCallback(path)
{
    var caller = vm.moduleLoader.getCaller()  // null for the entry hop

    var resolved
    if (caller != null && dataDirModules[caller]) {
        // Sibling import inside a data-dir module — resolve relative
        // to MODULE_PATH so `something.zym` next to `pie.zym` is found.
        resolved = Path.normalize(Path.join(MODULE_PATH, path))
        dataDirModules[path] = true
    } else if (Path.extension(path) == "") {
        // Bare name from the entry tree → data dir lookup.
        resolved = Path.normalize(Path.join(MODULE_PATH, path + ".zym"))
        dataDirModules[path] = true
    } else {
        resolved = Path.normalize(Path.join(SCRIPT_PATH, path))
    }

    var source = readFile(resolved)
    if (!source) { return null }

    var sm  = vm.newSourceMap()
    var fid = vm.registerSourceFile(path, source)
    var ps  = vm.preprocess(source, sm, fid)
    return { source: ps.source, sourceMap: sm, fileId: fid }
}
```

`getStack()` is the same information in chain form — use it when you
want diagnostics ("module `a` → `b` → `c` failed to resolve") or
policy decisions ("only `trusted/*` modules may import `user:*`")
without maintaining your own parents map.

### Resolve callback

`getCaller()` / `getStack()` are sufficient for a callback that just
needs to know *who asked* in order to decide *what file to read*. They
are **not** sufficient when two physically-distinct modules would
otherwise collide on the same key in the C loader's cycle detector
and module cache.

Concretely, given `script -> m1 -> (dataDir)m2 -> (dataDir)m1`, the C
loader resolves `("m2", "m1")` to the string `"m1"` — the same key
that's already on its `ImportStack` from the first hop. It then fires
a false-positive `Circular import detected: m1 -> m2 -> m1` *before*
the read callback ever runs, so no amount of script-side bookkeeping
inside `read_callback` can rescue the situation. Symmetrically, two
parallel imports of `"m1"` — one meant to come from the data dir, one
meant to come from a local sibling — silently alias into a single
cache slot because the loader cannot tell them apart.

`opts.resolveCallback` plugs in at exactly the seam where this
matters: the loader calls it on the result of its built-in resolver
**before** the `stack_contains` / `loaded_modules` lookup. The string
the resolver returns (if any) becomes the canonical key for cycle
detection, caching, the `read_callback` `path` argument, and the
`base_path` used when scanning the loaded module's transitive
imports. Returning `null` (or the same string the loader passed in)
means "keep the loader default" — there is zero behavioral or
performance impact for callers that don't install a resolver.

`vm.moduleLoader.getCaller()` and `getStack()` are valid inside the
resolve callback too, with the same semantics: `getCaller()` is the
*requester* of the import currently being resolved, `getStack()[len-1]`
is the requester (since the about-to-be-resolved module has not yet
been pushed onto the loader's stack).

#### Example: namespaced keys for a data-dir module system

This extends the previous example. The read callback alone could
route `script -> m1` and `m1 -> (dataDir)m2` correctly using
`getCaller()` — but the moment a data-dir module imports something
that happens to share a name with anything already on the import
stack (a sibling, an ancestor, even itself transitively), the C-side
cycle detector trips on a key collision the script cannot intervene
in. The fix is to canonicalize data-dir modules under a `"data:"`
prefix *before* the loader commits to a key:

```zym
var MODULE_PATH = Path.join(System.dataDir(), "zym", "modules")
var dataDirModules = {}            // canonical key -> true
var SCRIPT_PATH = Path.dirname(entryPath)

func startsWith(s, p) { return Path.startsWith(s, p) }

// Decide the canonical key *before* the loader does its cycle/cache
// check. Returning null means "use loader default" (root-relative).
func resolveCallback(path)
{
    var caller = vm.moduleLoader.getCaller()  // requester of this import

    // Sibling import inside an already-known data-dir module: keep it
    // in the data-dir namespace so its key cannot collide with a
    // local module of the same name.
    if (caller != null && startsWith(caller, "data:")) {
        return "data:" + path
    }

    // Bare name from the entry tree → route to data dir.
    if (Path.extension(path) == "") {
        return "data:" + path
    }

    return null   // keep loader default for ordinary local imports
}

func readAndPreprocessCallback(path)
{
    // `path` is whatever resolveCallback canonicalized to (or the
    // loader default if resolveCallback returned null).
    var resolved
    if (startsWith(path, "data:")) {
        var rel = Path.slice(path, length("data:"), length(path))
        resolved = Path.normalize(Path.join(MODULE_PATH, rel + ".zym"))
        dataDirModules[path] = true
    } else {
        resolved = Path.normalize(Path.join(SCRIPT_PATH, path))
    }

    var source = readFile(resolved)
    if (!source) { return null }

    var sm  = vm.newSourceMap()
    var fid = vm.registerSourceFile(path, source)
    var ps  = vm.preprocess(source, sm, fid)
    return { source: ps.source, sourceMap: sm, fileId: fid }
}

var loaded = vm.loadModules(
    pre.source, sm, "entry.zym",
    readAndPreprocessCallback,
    {
        debugNames: true,
        resolveCallback: resolveCallback,
    }
)
```

With this in place, `script -> m1 -> (dataDir)m2 -> (dataDir)m1`
appears to the C loader as `["m1", "data:m2", "data:m1"]` — no
collision with the entry-tree `m1`, no false cycle, and a genuine
`data:m1 -> data:m2 -> data:m1` would still trip the cycle detector
correctly. `loaded.modulePaths` and any diagnostics that bubble up
also become self-documenting: the `"data:"` prefix tells you at a
glance which side of the namespace boundary a given module came from.

Notes:

- `resolveCallback` is optional and must be either a closure or
  absent/`null`; anything else raises a runtime error from
  `loadModules`. When absent, the C loader takes its
  byte-identical-to-before path — no resolver trampoline is invoked
  and there is no per-import marshalling cost.
- The string the resolver returns is **borrowed** at the C boundary
  (the loader copies it internally on return). Scripts don't need to
  reason about lifetime; just return the string.
- The resolver is invoked once per import edge — including for
  imports that ultimately hit the module cache. That's the whole
  point: it has to run before the cache lookup in order to influence
  which slot is consulted.

#### MCU / runtime-only builds

Scripts that never read `vm.moduleLoader.*` pay no cost: the
trampoline does not allocate or marshal any extra parent-VM values
per import, and the loader handle itself is a thin proxy. The fields
that back these accessors are zeroed on VM init and only ever
written during `load_module_recursive`, so a binary that never calls
`loadModules` carries the cost of two pointer-sized fields on `VM`
and nothing else.

### Probing for a function before calling it

`cv.hasFunc` answers "is this name callable?" without committing to a
particular arity. It's the recommended pre-check before `cv.call` for
optional entry points (`main`, lifecycle hooks, etc.). Pass an arity
to narrow the question to a specific overload.

```zym
var vm = Zym.newVM()
vm.run("
    func answer() { return 42 }
    func greet(who) { return who }
    func add(a, b) { return a + b }
")

// Existence question — any arity counts.
print(vm.hasFunc("answer"))          // true
print(vm.hasFunc("greet"))           // true
print(vm.hasFunc("missing"))         // false

// Specific overload question — exact arity match.
print(vm.hasFunc("add", 2))          // true
print(vm.hasFunc("add", 3))          // false
print(vm.hasFunc("answer", 0))       // true

// Idiomatic optional entry-point dispatch.
if (vm.hasFunc("main")) {
    vm.call("main", [argv])
}

// Variadics are detected too — both script `func collect(...parts)` and
// natives registered via `registerNative("collect(...parts)", fn)`.
// With an arity, `hasFunc` answers "can I call it with N args?" — true
// for variadics whenever `N >= fixed-prefix`.
var vm2 = Zym.newVM()
vm2.run("
    func collect(...parts)         { return parts }
    func label(name, ...rest)      { return [name, rest] }
")
print(vm2.hasFunc("collect"))        // true
print(vm2.hasFunc("collect", 0))     // true (variadic accepts 0)
print(vm2.hasFunc("collect", 5))     // true
print(vm2.hasFunc("label", 1))       // true (just the fixed prefix)
print(vm2.hasFunc("label", 0))       // false (below fixed prefix)
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

### `callv` — positional args at the call site

When the args are literals, `cv.callv(name, ...)` skips the
list-wrapping ceremony of `cv.call`. The two are equivalent — both
write to the same backing slot, both walk the same arity-resolution
path, both return a `Zym.STATUS` code, and both leave the result
where `cv.callResult()` reads it. Use whichever shape matches the
call site.

```zym
var src = "
    func greet(who) { return \"hi \" + who }
    func add(a, b)  { return a + b }
    func collect(...parts) { return parts }
"

var vm = Zym.newVM()
vm.run(src)

// Equivalent — pick whichever fits the call site:
vm.call("greet", ["ada"])
vm.callv("greet", "ada")             // same result, no list

vm.callv("add", 2, 3)
print(vm.callResult())               // 5

// Variadics resolve naturally: trailing positional args are packed
// into the child's rest parameter.
vm.callv("collect", 1, 2, 3, 4)
print(vm.callResult())               // [1, 2, 3, 4]
```

`callv` is most useful when the args are literals or come from a
small, named set; `call` is the right choice when the args are
already a list (e.g., a forwarded `...rest`, a parsed JSON payload,
or a list built up by `map`/`reduce`).

### `getFunc` — store and call like a native

`cv.call` and `cv.callv` are the right shape when you're driving the
child function-by-function from a known sequence of work. When you
want to *hold onto* a child function and call it many times — for
module/helper patterns, callbacks stored in a parent-side data
structure, or just to keep the call site clean — `cv.getFunc(name)`
returns a parent-side callable that forwards into the child function
set as if it were a native parent closure.

The returned callable covers the **entire function set** under that
name on the child: every fixed overload plus any variadic. Overload
resolution is performed on the child per call (same logic that
`cv.call` / `cv.callv` go through), so a single `getFunc` result
handles every shape the child accepts.

```zym
var src = "
    func greet(who) { return \"hi \" + who }
    func add(a, b)  { return a + b }
    func collect(...parts) { return parts }
"

var vm = Zym.newVM()
vm.run(src)

// Look up once, call as if it were a regular callable.
var greet   = vm.getFunc("greet")
var add     = vm.getFunc("add")
var collect = vm.getFunc("collect")

print(greet("ada"))                  // hi ada
print(add(2, 3))                     // 5
print(collect(1, 2, 3, 4))           // [1, 2, 3, 4]

// Identity-stable: same name, same VM → same callable.
print(vm.getFunc("greet") == greet)  // true

// Absent name → null. Idiomatic guard:
var maybeMain = vm.getFunc("main")
if (maybeMain != null) {
    maybeMain()
}
```

The result is a normal value: store it in a list/map, pass it to a
parent-side higher-order function, hand it to another VM via
`registerNative` — wherever a callable is accepted, the dispatcher
fits in. Args go in the same way as `call` / `callv` (full marshalled
graph, including Buffers and cross-VM closures); the return value
comes back marshalled, so there's no separate `callResult` step.

`getFunc` itself doesn't change phase — it's a read against the
child's compiled global table. The returned callable, when invoked,
flips the child into execution phase if it isn't already (same trigger
as `call` / `callv`). Looking up a name before the child has been
compiled returns `null` because the function genuinely isn't there
yet.

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
