#pragma once
//
// root_scope.hpp
//
// Tiny RAII helper to manage `zym_pushRoot` / `zym_popRoot` pairing
// during native module construction.
//
// The native modules (ui.cpp, sdl.cpp, sockets.cpp, ...) all build
// their module map by:
//   1. Creating dozens-to-hundreds of native closures + dispatchers.
//   2. `zym_pushRoot`-ing each one to keep it alive against the GC
//      while later closures are being created (creation itself can
//      trigger collection).
//   3. Stuffing them all into the module map (which becomes their
//      permanent root via the module-registry root).
//   4. `zym_popRoot`-ing each one in reverse order before returning.
//
// Step 4 was historically a long ledger of bare `zym_popRoot(vm);`
// lines tagged with `// name` comments that had to stay in lock-step
// with the pushes. Adding / removing / reordering a binding meant
// hand-editing both ends, and `ui.cpp` had grown a ~200-line trailing
// pop block that was fragile to maintain.
//
// `RootScope` replaces that pattern:
//
//     RootScope roots(vm);
//     ZymValue button = roots.push(zym_createNativeClosure(vm, ...));
//     ZymValue text   = roots.push(zym_createNativeClosure(vm, ...));
//     ...
//     ZymValue obj = zym_newMap(vm);
//     roots.push(obj);
//     zym_mapSet(vm, obj, "button", button);
//     zym_mapSet(vm, obj, "text",   text);
//     ...
//     roots.popAll();   // explicit (matches existing style),
//                       // OR rely on the destructor on early return.
//     return obj;
//
// Semantics:
//   * `push(v)` calls `zym_pushRoot(vm, v)` and increments an internal
//     counter, then returns `v` unchanged so it can be used inline:
//
//         ZymValue x = roots.push(make_something(vm));
//
//   * `popAll()` calls `zym_popRoot(vm)` once per outstanding push and
//     resets the counter to 0. Idempotent (safe to call twice).
//
//   * Destructor calls `popAll()`. This makes early `return` on error
//     paths safe — you don't have to remember to drain manually.
//     Combined with explicit `popAll()` at the end of the happy path,
//     you get both the "visible in code review" property and the
//     "impossible to forget" property.
//
//   * Non-copyable, non-movable: a RootScope's lifetime is tied to a
//     specific stack frame and a specific `ZymVM*`. Copying would
//     double-drain; moving would silently transfer the count and
//     was deemed not worth the API surface.
//
// This is a header-only helper local to the native modules. It does
// NOT change the public `zym/zym.h` API — every `roots.push` / 
// `roots.popAll` ultimately calls the same two engine entry points
// (`zym_pushRoot` / `zym_popRoot`) that hand-written code already
// used. The only difference is the count is tracked in the C++ side
// instead of by-eye against a comment ledger.

#include "zym/zym.h"

namespace zym_native_detail {

class RootScope {
public:
    explicit RootScope(ZymVM* vm) noexcept : m_vm(vm), m_count(0) {}

    // Non-copyable, non-movable. See the rationale in the header
    // comment above.
    RootScope(const RootScope&)            = delete;
    RootScope& operator=(const RootScope&) = delete;
    RootScope(RootScope&&)                 = delete;
    RootScope& operator=(RootScope&&)      = delete;

    ~RootScope() noexcept { popAll(); }

    // Push `v` as a GC root and return it. Returning the value lets
    // callers chain `roots.push(make_x(...))` inline at the
    // assignment site, which is the predominant shape.
    inline ZymValue push(ZymValue v) noexcept {
        zym_pushRoot(m_vm, v);
        ++m_count;
        return v;
    }

    // Drain every outstanding root pushed through this scope, in the
    // reverse order they were pushed (which is what `zym_popRoot`
    // requires — it pops the most-recently-pushed root). Safe to call
    // more than once; subsequent calls are no-ops.
    inline void popAll() noexcept {
        while (m_count > 0) {
            zym_popRoot(m_vm);
            --m_count;
        }
    }

    // Current outstanding root count, exposed for assertions / debug.
    inline int count() const noexcept { return m_count; }

private:
    ZymVM* m_vm;
    int    m_count;
};

} // namespace zym_native_detail

// Pull into the top-level for terse use inside the native TUs that
// already operate in their own anonymous namespace. The header is
// only included by `src/natives/*.cpp`, so this is local pollution
// at worst.
using zym_native_detail::RootScope;
