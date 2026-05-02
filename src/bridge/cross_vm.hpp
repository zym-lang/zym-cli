// Cross-VM value bridge for the Zym CLI.
//
// Provides:
//   - Recursive value marshalling between two ZymVM*s (null/bool/number/string/
//     list/map/struct/enum + Buffer byte-copy + closures wrapped as cross-VM
//     callables).
//   - Registration of a parent closure as a child native (`registerNative`).
//   - Wrapping of an arbitrary callable from one VM as a native closure on
//     another (used when a callable value crosses the boundary inside a
//     marshalled graph).
//
// All cross-VM callables become variadic on the destination side (the source
// callable's arity is not introspectable through the public C API), so the
// caller is responsible for invoking them with the right number of arguments.
//
// See future/zym_roadmap.md §3 PR 4 and docs/cli/zym.md for the scripting
// model.
#pragma once

#include "zym/zym.h"

namespace zym_bridge {

// Recursive value marshaller. Copies `v` from `srcVm` to `dstVm` in place,
// preserving identity for repeated non-leaf values within a single call
// (lists, maps, and structs that appear more than once become a single
// shared destination value). Cycles are handled the same way.
//
// Buffer values cross by **byte-copy** — source and destination Buffers are
// independent; mutating one does not affect the other.
//
// Closure / function values cross by **wrapping**: the destination value is
// a fresh variadic native closure on `dstVm` whose context owns a callable
// reference back into `srcVm`.
//
// On any failure (bad input, allocation, etc.) raises a runtime error on
// `srcVm` and returns false. On success writes the destination value to
// `*out` and returns true.
bool marshal(ZymVM* srcVm, ZymVM* dstVm, ZymValue v, ZymValue* out);

// Register a parent closure as a child native, with the given Zym-style
// signature (e.g. `"hostLog(msg)"` or `"sum(...nums)"`). Parses the
// signature once for fixed arity + variadic flag, picks the correct
// trampoline, and binds the resulting closure as a child global named
// after the prefix of the signature.
//
// Setup-phase use only; the child must not yet have entered execution
// phase (the caller is expected to gate on that).
//
// Returns true on success; on failure raises a runtime error on
// `parentVm` and returns false.
bool register_cross_native(ZymVM* parentVm,
                           ZymVM* childVm,
                           const char* signature,
                           ZymValue parentClosure);

// Wrap an arbitrary callable from `srcVm` as a fresh variadic native
// closure on `dstVm`. The destination closure, when invoked, marshals its
// arguments to `srcVm`, calls the source callable via
// `zym_callClosurev`, and marshals the result back.
//
// Returns ZYM_ERROR on failure with a runtime error already raised on
// `dstVm`.
ZymValue wrap_callable(ZymVM* srcVm, ZymVM* dstVm, ZymValue srcCallable);

}  // namespace zym_bridge
