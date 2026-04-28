#pragma once
// =============================================================================
// scripts/zym_shim/zym_variant_strip.h
// -----------------------------------------------------------------------------
// Force-included variant/utility/constructor-registration neutering shim.
// Companions: scripts/zym_shim/core/object/class_db.h (GDREGISTER macros)
// and scripts/zym_shim/core/object/method_bind.h (per-method MethodBind
// instantiations). This shim handles the *file-local-static-template*
// registrars that live inside individual .cpps under godot/core/variant/
// (and so cannot be reached from any -I header shim, because each .cpp
// uses a same-directory relative include for its own header).
//
// Targeted symbols and their owning TUs:
//   1. register_op                       godot/core/variant/variant_op.cpp
//   2. register_builtin_method           godot/core/variant/variant_call.cpp
//   3. register_builtin_compat_method    godot/core/variant/variant_call.cpp
//   4. register_utility_function         godot/core/variant/variant_utility.cpp
//   5. add_constructor                   godot/core/variant/variant_construct.cpp
//
// scripts/build_godot_linux_x86_64.sh appends
//     -include ${ZYM_SHIM_DIR}/zym_variant_strip.h
// to the C++ flags. SCons forwards that to every C++ TU in the godot tree,
// so this header is implicitly the first thing each .cpp sees -- before
// any engine header.
//
// Why a force-include and not the `-I` + `#include_next` lever used for
// class_db.h / method_bind.h: every targeted .cpp does
//     #include "variant_op.h" / "variant.h" / etc.
// (a quoted, same-directory relative include). GCC always resolves that
// to the neighbour engine header before consulting any `-I` path, so a
// header-shim placed at scripts/zym_shim/core/variant/... would never be
// reached. And the registrars themselves are file-local *static* templates
// defined inside the .cpp body -- not in any header we could intercept.
// A force-include is the only knob that touches those TUs without
// patching godot/.
//
// What we strip (and why it's safe).
// Each registrar follows the same pattern:
//
//     template <class T>
//     static void register_X(...) {
//         // ... reference T::call / T::validated_call / T::ptrcall /
//         //     T::get_return_type / T::is_const / T::is_static / ... ,
//         //     pinning every Method_X / Func_X / VariantConstructor_X /
//         //     OperatorEvaluator_X struct's static method bodies into
//         //     the dispatch tables (and therefore into the binary).
//     }
//
//     // Hundreds of call sites below funnel through the same helper:
//     register_X<Method_<type>_<name>>(arg_names, default_args);
//     register_X<Func_<name>>("name", arg_names);
//     register_X<VariantConstructor<R, Args...>>(sarray("from", ...));
//     register_X<OperatorEvaluator<R, A, B>>(Variant::OP_ADD, ...);
//
// zym is a self-hosted C VM that talks to godot only through direct C++
// symbols (FileAccess::open, print_line, PackedByteArray::*, ...). It
// never invokes Variant::call, Variant::evaluate,
// Variant::Utility::call_utility_function, Variant::construct(),
// Variant::get_type_constructor() etc., so the dispatch tables those
// registrars populate are dead at runtime. Removing the per-T template
// instantiations (their `call`/`validated_call`/`ptrcall`/...
// static-method bodies plus the transitive PtrToArg<>/VariantInternalAccessor<>
// /GetTypeInfo<> chains) is pure dead-code removal.
//
// Mechanism (preprocessor / template trick).
// The two relevant syntactic forms of every targeted name in its owning
// .cpp:
//
//     definition site:
//         template <class T>
//         static void register_X(arg1_type a, arg2_type b, ...) { body }
//
//     call sites (every one is template-instantiated):
//         register_X<SomeT>(a, b, ...);
//
// The C/C++ preprocessor only fires a function-like macro when the macro
// name is followed *immediately* by `(`. At the definition site
// `register_X` is followed by `(`. At every call site `register_X` is
// followed by `<` (template arguments). So a function-like macro
//     #define register_X(...) zym_register_X_real(__VA_ARGS__)
// rewrites *only* the definition, renaming the engine's local template
// to `zym_register_X_real` while leaving every `register_X<T>(...)`
// call site textually unchanged. (Variadic `...` is used to accommodate
// the differing arg counts -- 1 to 3 -- across the five registrars
// uniformly.)
//
// We then provide our own `register_X` (a regular C++ template, declared
// before the macro so the macro doesn't catch its own declaration) that
// the call sites bind to. Its body is empty, so `T::call` / `T::ptrcall`
// / `T::get_return_type` etc. are never ODR-used, and the Method_<...> /
// Func_<...> / VariantConstructor_<...> / OperatorEvaluator_<...>
// struct templates -- still declared, never *defined* by name in our
// wrapper -- have their static-method bodies pruned by `-flto` together
// with `-Wl,--gc-sections`.
//
// The renamed engine `zym_register_X_real<T>` template body is itself
// uninstantiated (no caller after the macro rewrite) so it is GC'd
// alongside.
//
// Argument types.
// Each wrapper's parameter types are kept fully generic (template
// parameters) because this header is force-included before any godot
// type is declared -- `Vector`, `String`, `Variant`, `Variant::Operator`
// etc. are not yet visible at the very top of every TU. They get
// deduced from the call-site arguments (`sarray(...)`,
// `Variant::OP_ADD`, ...) at template-argument-substitution time, which
// happens inside the owning .cpp where the engine types are fully
// visible.
//
// Out of scope (header shim cannot reach):
//   - Variant::_register_variant_operators() -- a member function of
//     Variant defined in variant_op.cpp. We do NOT need to noop it
//     directly: its body is one giant sequence of `register_op<...>(...)`
//     calls, all of which now bind to our empty wrapper, so the function
//     becomes inert at runtime and its instantiation footprint
//     collapses to almost nothing.
//   - _register_variant_builtin_methods_string / _math / _array / _misc
//     -- non-template plain functions in variant_call.cpp. Same story:
//     their bodies are forests of register_builtin_method<...>(...)
//     macro expansions, all now noops, so they too are inert.
//   - register_global_constants -- plain non-template free function in
//     core_constants.cpp called from register_core_types.cpp. Header
//     shims cannot intercept it because any rename macro fires
//     symmetrically on definition + call (function-like macro on
//     `register_global_constants(` would syntactically corrupt the
//     definition `void register_global_constants() { ... }` to
//     `void <expansion> { ... }`, and object-like renames are
//     definitionally symmetric). The only viable lever is link-time
//     `-Wl,--wrap=register_global_constants` plus a `__wrap_*` stub --
//     deliberately *not* attempted here.
//
// Disabling. Drop the `-include …/zym_variant_strip.h` flag from
// scripts/build_godot_linux_x86_64.sh; the build is then byte-identical
// to a stock Godot static-library build (with the class_db /
// method_bind shims still in place).
//
// C TUs are unaffected: the entire body is wrapped in `__cplusplus`.
// =============================================================================

#ifdef __cplusplus

// -----------------------------------------------------------------------------
// 1. register_op  (godot/core/variant/variant_op.cpp)
// Engine signature:
//     template <class T>
//     static void register_op(Variant::Operator p_op,
//                             Variant::Type p_type_a,
//                             Variant::Type p_type_b);
// -----------------------------------------------------------------------------

template <typename T, typename Op, typename TA, typename TB>
void zym_register_op_real(Op, TA, TB);

template <typename T, typename Op, typename TA, typename TB>
inline void register_op(Op, TA, TB) {}

#define register_op(a, b, c) zym_register_op_real(a, b, c)

// -----------------------------------------------------------------------------
// 2. register_builtin_method  (godot/core/variant/variant_call.cpp)
// Engine signature:
//     template <class T>
//     static void register_builtin_method(const Vector<String> &p_argnames,
//                                         const Vector<Variant> &p_def_args);
// -----------------------------------------------------------------------------

template <typename T, typename A1, typename A2>
void zym_register_builtin_method_real(const A1 &, const A2 &);

template <typename T, typename A1, typename A2>
inline void register_builtin_method(const A1 &, const A2 &) {}

#define register_builtin_method(a, b) zym_register_builtin_method_real(a, b)

// -----------------------------------------------------------------------------
// 3. register_builtin_compat_method  (godot/core/variant/variant_call.cpp,
//    inside `#ifndef DISABLE_DEPRECATED`)
// Engine signature: identical to register_builtin_method.
// -----------------------------------------------------------------------------

template <typename T, typename A1, typename A2>
void zym_register_builtin_compat_method_real(const A1 &, const A2 &);

template <typename T, typename A1, typename A2>
inline void register_builtin_compat_method(const A1 &, const A2 &) {}

#define register_builtin_compat_method(a, b) zym_register_builtin_compat_method_real(a, b)

// -----------------------------------------------------------------------------
// 4. register_utility_function  (godot/core/variant/variant_utility.cpp)
// Engine signature:
//     template <class T>
//     static void register_utility_function(const String &p_name,
//                                           const Vector<String> &argnames);
// -----------------------------------------------------------------------------

template <typename T, typename A1, typename A2>
void zym_register_utility_function_real(const A1 &, const A2 &);

template <typename T, typename A1, typename A2>
inline void register_utility_function(const A1 &, const A2 &) {}

#define register_utility_function(a, b) zym_register_utility_function_real(a, b)

// -----------------------------------------------------------------------------
// 5. add_constructor  (godot/core/variant/variant_construct.cpp)
// Engine signature:
//     template <class T>
//     static void add_constructor(const Vector<String> &arg_names);
// -----------------------------------------------------------------------------

template <typename T, typename A1>
void zym_add_constructor_real(const A1 &);

template <typename T, typename A1>
inline void add_constructor(const A1 &) {}

#define add_constructor(a) zym_add_constructor_real(a)

#endif // __cplusplus
