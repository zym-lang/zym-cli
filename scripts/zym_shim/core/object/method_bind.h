#pragma once
// =============================================================================
// scripts/zym_shim/core/object/method_bind.h
// -----------------------------------------------------------------------------
// External (no godot/ source edits) MethodBind-instantiation neutering shim.
// Companion to scripts/zym_shim/core/object/class_db.h: that shim noops the
// `GDREGISTER_*` macros so `ClassDB::register_class<T>()` is never invoked at
// runtime (which means every `T::_bind_methods()` body is dead code at the
// runtime level). This shim attacks the same dead code at the *compile* level
// -- it stops the per-method `MethodBindT<...>` / `MethodBindTC<...>` /
// `MethodBindTR<...>` / `MethodBindTRC<...>` / `MethodBindTS<...>` /
// `MethodBindTRS<...>` / `MethodBindVarArgT<...>` / `MethodBindVarArgTR<...>`
// class templates from ever being instantiated, so their virtual `call`,
// `validated_call`, `ptrcall`, `_gen_argument_type`, `_gen_argument_type_info`
// (and `get_argument_meta` under DEBUG_ENABLED) thunks never reach the object
// files in the first place. Per-method vtables, type-info helpers, and the
// transitive `call_with_*` / `PtrToArg<>` / `GetTypeInfo<>` / `Variant`
// glue chains they pull in disappear with them -- this is where the bulk of
// reflection-driven code-size weight actually lives.
//
// Mechanism. SCons compiles every godot TU with `-I${SCRIPT_DIR}/zym_shim`
// prepended (see scripts/build_godot_linux_x86_64.sh), so any
// `#include "core/object/method_bind.h"` lands here first; only
// `godot/core/object/method_bind.cpp` uses a same-directory relative include
// and bypasses us, but that TU implements the `MethodBind` base class
// itself -- it never defines a `_bind_methods()` body and never calls the
// `create_*_method_bind` factories, so bypassing it is a no-op for the
// strip.
//
// We then:
//   1. Rename the engine's eight `create_*_method_bind` free templates via
//      object-like macros installed *before* `#include_next`. Each macro
//      rewrites the engine's *definition* (e.g. `MethodBind
//      *create_method_bind(...)` becomes `MethodBind
//      *zym_create_method_bind_real(...)`), and the engine's free templates
//      are the only place those identifiers appear inside method_bind.h.
//      No call sites live in this header, so the macros don't hit anything
//      else here.
//   2. `#undef` the macros so the original identifiers are free for our
//      noop overloads to claim.
//   3. Provide eight `inline` noop overloads under the original names that
//      simply `return nullptr` -- one for each engine signature shape:
//        - `void (T::*)(P...)`
//        - `void (T::*)(P...) const`
//        - `R    (T::*)(P...)`
//        - `R    (T::*)(P...) const`
//        - `void (*  )(P...)`
//        - `R    (*  )(P...)`
//        - `void (T::*)(const Variant **, int, Callable::CallError &)` (vararg)
//        - `R    (T::*)(const Variant **, int, Callable::CallError &)` (vararg)
//
//      Because no `MethodBindT<...>` / `MethodBindTC<...>` / ... is *named*
//      inside our noops, the corresponding class templates stay
//      uninstantiated. The engine's renamed `zym_create_*_method_bind_real`
//      templates aren't called from anywhere either (their previous one and
//      only caller -- `ClassDB::bind_method` and friends -- now binds to
//      our noops via overload resolution at instantiation time), so they
//      stay uninstantiated too.
//
// Resolution and lookup. The engine's `ClassDB::bind_method` template body
// in core/object/class_db.h (see lines ~388 / ~402 / ~417 / ~431) calls
// `create_method_bind(p_method)` *unqualified*. That's a dependent call
// (the argument type depends on the template parameter `M`), so name
// lookup at the point of instantiation finds every `create_method_bind`
// in scope. Our noops are the only candidates under that name (the engine
// originals were renamed away by the time class_db.h gets parsed), so they
// always win.
//
// Why we stop here. The user's stretch goal was to also strip
// `ClassDB::bind_method` itself -- skipping the `Variant args[...]`
// construction and `bind_methodfi` extern call inside its template body.
// That isn't doable from a header-only shim: `bind_method` is a *class*
// member of `ClassDB`, and standard C++ has no way to add or replace
// members of a class from outside its definition. Replacing the
// declaration via macro rewrite would also have to replace the call sites
// `ClassDB::bind_method(...)` in user code, but a function-like macro
// expands `bind_method(...)` to a stand-alone expression -- it can't
// produce a token that's valid after the `ClassDB::` qualifier (since the
// qualifier sits *outside* the macro's match window). The
// `ClassDB::bind_method` template body therefore stays on disk; what we
// *do* strip is everything it would otherwise pull in (the
// `MethodBindT<...>` family + its transitive instantiation chains), which
// is the actual size lever. The residual cost is a `Variant args[1]` stack
// init plus an extern `bind_methodfi` call per `_bind_methods()` line, all
// of which is dead code at runtime (GDREGISTER is noop'd, so
// `_bind_methods()` is never invoked).
//
// Disabling. Drop the `-I${SCRIPT_DIR}/zym_shim` flag from
// scripts/build_godot_linux_x86_64.sh; the build is then byte-identical to
// a stock Godot static-library build (alongside the class_db shim).
// =============================================================================

// Pre-include rewrites. Object-like macros, deliberately *not* function-like
// -- the engine's headers reference these identifiers only in their
// definition heads, never in calls (those live in class_db.h's bind_method
// templates and inside our noops below, both of which are processed after
// the `#undef`s further down).
#define create_method_bind        zym_create_method_bind_real
#define create_static_method_bind zym_create_static_method_bind_real
#define create_vararg_method_bind zym_create_vararg_method_bind_real

#include_next "core/object/method_bind.h"

#undef create_method_bind
#undef create_static_method_bind
#undef create_vararg_method_bind

// Single shared MethodBind subclass with empty overrides. Replaces the
// engine's per-method `MethodBindT<R, A, B...>` / `MethodBindTC<...>` /
// `MethodBindTR<...>` / `MethodBindTRC<...>` / `MethodBindTS<...>` /
// `MethodBindTRS<...>` / `MethodBindVarArgT<...>` / `MethodBindVarArgTR<...>`
// instantiations (one vtable, one set of virtual bodies, total -- no
// dependence on the user's method pointer shape, so the entire
// `call_with_*`, `PtrToArg<>`, `GetTypeInfo<>` glue chain that the engine
// templates would otherwise drag in stays out of every godot TU).
//
// Why a real subclass instead of `return nullptr;`. The 8
// `ClassDB::register_custom_instance_class<T>()` calls in
// `core/register_core_types.cpp` (HTTPClient, X509Certificate, CryptoKey,
// HMACContext, Crypto, StreamPeerTLS, PacketPeerDTLS, DTLSServer) bypass
// the GDREGISTER-noop layer entirely, so `T::initialize_class()` --
// hence `T::_bind_methods()` -- *is* invoked at runtime for those eight
// classes. With a nullptr noop, each call inside their `_bind_methods`
// body forwards `nullptr` to `ClassDB::bind_methodfi`, which spams
// `ERR_FAIL_NULL_V(p_bind, nullptr)` at startup. Returning a valid
// (if behaviourally inert) `MethodBind` instance instead silences that
// path while still avoiding every per-template instantiation -- the
// shared `ZymNoopMethodBind` vtable carries the cost once.
//
// Lifetime + identity. Each call to a `create_*_method_bind` overload
// `memnew`s a fresh `ZymNoopMethodBind` (NOT a static singleton) because
// `bind_methodfi` may legitimately `memdelete(p_bind)` along its error
// paths (`type` not registered, or "method already bound" -- neither
// triggers for the 8 properly-registered custom-instance classes whose
// `_bind_methods()` actually executes, but using a singleton would
// nonetheless make those error paths UB if they ever fired). Per-bind
// allocation cost is ~80 bytes * a handful of methods across 8 classes
// = trivial.
//
// `set_instance_class` is mirrored from the engine's own
// `create_method_bind` (member-pointer overloads only -- the static
// ones intentionally don't, matching the engine).

class ZymNoopMethodBind : public ::MethodBind {
protected:
	::Variant::Type _gen_argument_type(int) const override { return ::Variant::NIL; }
	::PropertyInfo _gen_argument_type_info(int) const override { return {}; }

public:
	::Variant call(::Object *, const ::Variant **, int, ::Callable::CallError &) const override { return ::Variant(); }
	void validated_call(::Object *, const ::Variant **, ::Variant *) const override {}
	void ptrcall(::Object *, const void **, void *) const override {}

#ifdef DEBUG_ENABLED
	::GodotTypeInfo::Metadata get_argument_meta(int) const override { return ::GodotTypeInfo::METADATA_NONE; }
#endif
};

// Eight overloads, one per engine signature shape. Each one mirrors the
// engine's `set_instance_class(T::get_class_static())` for member-pointer
// variants so `bind_methodfi`'s `classes.getptr(instance_type)` lookup
// hits the right `ClassInfo`. Static overloads leave `instance_class`
// blank (matching engine behaviour at line ~712 / ~778 of method_bind.h).
// `_FORCE_INLINE_` is deliberately avoided in favour of plain `inline` so
// these are visible to the linker as weak symbols (multiple TUs may emit
// them, none take their address).

template <typename T, typename... P>
inline ::MethodBind *create_method_bind(void (T::*)(P...)) {
	::MethodBind *a = memnew(ZymNoopMethodBind);
	a->set_instance_class(T::get_class_static());
	return a;
}

template <typename T, typename... P>
inline ::MethodBind *create_method_bind(void (T::*)(P...) const) {
	::MethodBind *a = memnew(ZymNoopMethodBind);
	a->set_instance_class(T::get_class_static());
	return a;
}

template <typename T, typename R, typename... P>
inline ::MethodBind *create_method_bind(R (T::*)(P...)) {
	::MethodBind *a = memnew(ZymNoopMethodBind);
	a->set_instance_class(T::get_class_static());
	return a;
}

template <typename T, typename R, typename... P>
inline ::MethodBind *create_method_bind(R (T::*)(P...) const) {
	::MethodBind *a = memnew(ZymNoopMethodBind);
	a->set_instance_class(T::get_class_static());
	return a;
}

template <typename... P>
inline ::MethodBind *create_static_method_bind(void (*)(P...)) {
	return memnew(ZymNoopMethodBind);
}

template <typename R, typename... P>
inline ::MethodBind *create_static_method_bind(R (*)(P...)) {
	return memnew(ZymNoopMethodBind);
}

template <typename T>
inline ::MethodBind *create_vararg_method_bind(
		void (T::*)(const ::Variant **, int, ::Callable::CallError &),
		const ::MethodInfo &, bool) {
	::MethodBind *a = memnew(ZymNoopMethodBind);
	a->set_instance_class(T::get_class_static());
	return a;
}

template <typename T, typename R>
inline ::MethodBind *create_vararg_method_bind(
		R (T::*)(const ::Variant **, int, ::Callable::CallError &),
		const ::MethodInfo &, bool) {
	::MethodBind *a = memnew(ZymNoopMethodBind);
	a->set_instance_class(T::get_class_static());
	return a;
}
