# Robustness tests

Tests in this folder answer one question: **can malformed or hostile input
make the host process crash, corrupt memory, or die?**

They are not correctness tests — `tests/core` and `tests/cli` cover what the
language and natives *do*. These cover what the runtime must *never* do, no
matter what bytes it is handed: no segfault, no wild pointer write, no
`exit()` from inside a library call, no unbounded allocation or recursion.

Unlike the other suites these are C programs, not `.zym` scripts, because
they drive the embedding API (`zym_deserializeChunk`, `zym_runChunk`) with
inputs the compiler would never emit. `runAll.sh` compiles and runs them.

## Running

```
tests/robustness/runAll.sh
```

It needs a built `libzym_core.a`. By default it looks in
`cmake-build-linux/zym_core/`; override with `$ZYM_CORE_LIB`, and pass a
higher iteration count as the first argument for a longer soak:

```
tests/robustness/runAll.sh 50000
```

A test fails if it exits non-zero — which includes dying on a signal, so a
segfault is reported as a failure rather than silently passing.

## What is covered

| Test | Guards against |
| --- | --- |
| `regress_enum_forged_pointer.c` | An enum constant with an out-of-range `type_id` sign-extending into `SIGN_BIT`, so a NaN-boxed value reads as an object pointer and the GC writes through an address the file chose. |
| `fuzz_bytecode_loader.c` | Loader-level failures generally: random byte mutation, hostile 32-bit values at every aligned offset (count and index fields), and every truncation length. |

## Scope boundary — read this before trusting it

These tests prove the **loader** is safe. They do **not** prove it is safe to
*execute* untrusted bytecode.

A mutated chunk can be structurally valid — every count, length, and pool
index in range — while its instruction stream is nonsense. The loader
correctly accepts it, and the interpreter then crashes, because opcodes,
register indices, constant indices, and jump targets are never validated
against the chunk they belong to. Closing that requires a bytecode verifier
(the JVM-style pass that proves an instruction stream well-formed before
running it), which does not exist yet.

So today:

- Attempting to **load** untrusted bytecode is safe; it is rejected or loads
  harmlessly.
- **Running** untrusted bytecode is not safe.

`fuzz_bytecode_loader.c` deliberately stops at the loader boundary and says
so in its output. If a verifier is added later, extend the fuzzer to execute
survivors and the gap closes.
