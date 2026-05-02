# `Zym`

Nested in-process VM API. The global identifier `Zym` is a top-level
singleton namespace (no `create(...)` constructor of its own); methods
are invoked as `Zym.method(...)`.

`Zym` lets a script spin up a fresh VM *inside* the current process,
wire it up exactly the way it wants, and bridge values across the
boundary. The parent script plays the role of an embedder for its
child: it decides which native modules the child can see, defines the
child's globals, drives compile/run, and reads diagnostics back.

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
- **Errors.** Bad argument types or arity raise a Zym runtime error
  of the form `Zym.method(args) ...`. *Capability and lifecycle
  failures all collapse to a single* `no such native` *error* —
  unknown name, name withheld by the parent, calling a method on a
  freed VM, or (forthcoming) calling a setup method on a VM that has
  already entered the execution phase. A sandboxed child cannot
  distinguish between these cases by error message.

---

## Top-level singleton

| Method | Returns | Notes |
| --- | --- | --- |
| `Zym.cliNatives()` | `[string]` | The list of native names the calling VM is allowed to grant to its children. Order is the order they were granted to *this* VM (matches the catalog declaration order at the root). `Buffer` is omitted (it is universal, not grantable). |
| `Zym.newVM()` | `ChildVM` | Allocates a fresh in-process VM. The returned value is a struct of method closures (see below) bound to the new VM. The child's allocator is inherited from the parent (not script-selectable). The child starts with `Buffer` auto-installed and an empty grantable set; the parent must explicitly grant any other natives via `registerCliNative` *before* the child enters the execution phase. |

### Example

```zym
print(Zym.cliNatives())
// [print, Time, File, Dir, Console, Process, RegEx, JSON, Crypto,
//  Random, Hash, System, Path, IP, TCP, UDP, TLS, DTLS, ENet, AES,
//  Sockets, Zym]
```

A child given only `[File, Path]` would instead see:

```zym
print(Zym.cliNatives())     // [File, Path]
```

---

## ChildVM (returned by `Zym.newVM`)

Every method below is reached through the value returned by
`Zym.newVM()`. The child VM is initially in **setup phase**: methods
that mutate the child's environment (`registerCliNative`,
`defineGlobal`) are only valid in this phase. Setup phase will become
one-way frozen by the first call into the execution-phase pipeline
(landing in a future release).

### Setup-phase methods

| Method | Returns | Notes |
| --- | --- | --- |
| `cv.registerCliNative(arg)` | `bool` | Grants one or more native modules to the child. `arg` is a single name (`"File"`), a list of names (`["File", "Path"]`), or the literal string `"ALL"`. Names must be in the calling VM's own grantable set; granting a name that is unknown *or* withheld raises `no such native` (same error in both cases). Idempotent: re-granting a name already on the child is a silent no-op. With `"ALL"`, every name from the calling VM's grantable set is granted in declaration order. |
| `cv.defineGlobal(name, value)` | `bool` | Defines a global on the child. PR-2 release marshalls only string, number, and bool primitives across the VM boundary. List/map/Buffer/closure marshalling lands with the cross-VM bridge. Last-write-wins on name collision (matches the underlying C API). |
| `cv.cliNatives()` | `[string]` | The names this child has been granted, in grant order. Equivalent to "what this child could grant onward". |
| `cv.free()` | `bool` | Tears the child VM down explicitly. Returns `true` on the first call, `false` if already freed. After `free()`, every other ChildVM method raises `no such native`. The child is also freed automatically when the parent VM tears down its globals — `free()` is just a way to release resources earlier. |

### Examples

```zym
// Grant a small surface and inspect it.
var sandbox = Zym.newVM()
sandbox.registerCliNative(["File", "Path"])
print(sandbox.cliNatives())          // [File, Path]

// Grant the universe at once.
var open = Zym.newVM()
open.registerCliNative("ALL")
print(length(open.cliNatives()))     // matches Zym.cliNatives()

// Idempotent re-grants.
var sb = Zym.newVM()
sb.registerCliNative("Path")
sb.registerCliNative("Path")         // no-op
sb.registerCliNative(["File", "Path"])
print(sb.cliNatives())               // [Path, File]

// Withholding the ability to nest further.
var leaf = Zym.newVM()
leaf.registerCliNative(["File", "Dir", "Path"])
// leaf has no Zym in its grantable set, so any code running inside
// leaf has no `Zym` global at all and cannot call Zym.newVM().

// Primitive globals.
var c = Zym.newVM()
c.defineGlobal("USER",   "ada")
c.defineGlobal("LIMIT",  100)
c.defineGlobal("DEBUG",  true)

// Explicit teardown.
var t = Zym.newVM()
t.free()
t.free()                              // already freed: returns false
```

---

## Roadmap

The full nested-VM surface is delivered incrementally. This page is
the source of truth for what is *currently callable*; methods that
have not yet landed are not callable, and calling them raises the
standard "no such method" runtime error (the same error a sandboxed
child sees for any name not in its capability set).

| Status | Adds |
| --- | --- |
| shipped | `Zym.cliNatives()`. |
| shipped | `Zym.newVM()`; ChildVM setup-phase methods (`registerCliNative`, `defineGlobal`, `cliNatives`, `free`); capability sandbox; `Buffer` auto-install on every child. |
| upcoming | Pipeline methods on the child handle: `newSourceMap`, `registerSourceFile`, `preprocess`, `loadModules`, `newChunk`, `compile`, `serializeChunk`, `deserializeChunk`, `runChunk`, `call`, `hasFunction`, `diagnostics`, `freeChunk`. The first such call freezes the child into execution phase. |
| upcoming | Cross-VM bridge: `registerNative(signature, parentClosure)`, recursive value marshaller (lists, maps, structs, enums, callables), `Buffer` byte-copy across boundaries. |

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
  to pass bulk data across VM boundaries (in a future release —
  current PR ships `defineGlobal` with primitives only).
- **Capabilities can only shrink.** A child's grantable set is always
  a subset of its parent's. There is no API to broaden a set after
  the fact, and grants are not retroactive: once a child has been
  spawned with a given set, future grants to the parent never reach
  the existing child.
