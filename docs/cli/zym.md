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
  of the form `Zym.method(args) ...`. Capability and phase
  violations (forthcoming) collapse to a single "no such native"-style
  error so a sandboxed child cannot tell the difference between
  *unknown*, *withheld*, and *post-freeze*.

---

## Catalog query

| Method | Returns | Notes |
| --- | --- | --- |
| `Zym.cliNatives()` | `[string]` | The list of native names the calling VM is allowed to grant to its children. Order is stable and matches the order natives were declared in. `Buffer` is omitted (it is universal, not grantable). At a VM that received the full set, this is every grantable native. |

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

## Roadmap

The full nested-VM surface is delivered incrementally. This page is
the source of truth for what is *currently callable*; methods that
have not yet landed are not callable, and calling them raises the
standard "no such method" runtime error (the same error a sandboxed
child sees for any name not in its capability set).

| Status | Adds |
| --- | --- |
| shipped | `Zym.cliNatives()`. |
| upcoming | `Zym.newVM()` returning a child-VM handle; `Zym.STATUS` enum; setup-phase methods on the child (`registerCliNative`, `defineGlobal`, `cliNatives`, `free`); one-way `setup → execution` phase guard. |
| upcoming | Pipeline methods on the child handle: `newSourceMap`, `registerSourceFile`, `preprocess`, `loadModules`, `newChunk`, `compile`, `serializeChunk`, `deserializeChunk`, `runChunk`, `call`, `hasFunction`, `diagnostics`, `freeChunk`. |
| upcoming | Cross-VM bridge: `registerNative(signature, parentClosure)`, recursive value marshaller, callable trampolines (parent ↔ child), `Buffer` byte-copy across boundaries. |

---

## Notes

- **`Zym.cliNatives()` is per-VM, not global.** Two VMs in the same
  process can return different lists; that is the point of the
  capability sandbox. There is no way to query a global catalog from
  script.
- **Catalog order is stable.** `cliNatives()` returns names in a
  fixed declaration order, not alphabetical. Tests can compare
  against fixed slices without sorting.
- **`Zym` itself appears in the list when granted.** A parent that
  receives `Zym` as part of its own grant set sees `"Zym"` in
  `cliNatives()`; granting `"Zym"` to a grandchild then lets *that*
  grandchild nest further (subject to its own subset grants).
- **Allocator is inherited, never script-selectable.** When
  `Zym.newVM()` lands, it will be zero-arg: the child VM transparently
  reuses the parent's allocator. Memory configuration is not part of
  the script-visible surface.
