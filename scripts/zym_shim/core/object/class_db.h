#pragma once
// =============================================================================
// scripts/zym_shim/core/object/class_db.h
// -----------------------------------------------------------------------------
// External (no godot/ source edits) ClassDB-registration neutering shim.
//
// scripts/build_godot_linux_x86_64.sh prepends `-I${SCRIPT_DIR}/zym_shim`
// to the C++ flags. SCons emits `-I.` for the godot tree *after* user
// CXXFLAGS, so this shim sits ahead of `godot/core/object/class_db.h` on
// the search path. Any TU's `#include "core/object/class_db.h"` lands
// here first.
//
// We then:
//   1. Use `#include_next` to delegate to the real engine header. This is
//      important — we must NOT pre-include class_db.h into every TU (some
//      TUs, e.g. core/variant/array.cpp, intentionally compile against an
//      incomplete `Array`/`Dictionary`/`Object`/`String` and assert that
//      via STATIC_ASSERT_INCOMPLETE_TYPE; force-injecting class_db.h there
//      pulls those types in transitively and breaks the assertion).
//   2. After the engine header has had its say, `#undef` and redefine the
//      `GDREGISTER_*` macros to no-ops. Every later use site of those
//      macros in the same TU expands to `((void)0)`, so all `_bind_methods`
//      bodies, MethodBind thunks, property/signal/enum descriptors, and
//      method-name string interns disappear from the object file.
//
// Safe only because zym never resolves classes/methods/properties by name
// through ClassDB (no GDExtension, no embedded scripting, no
// `Object::call("name", ...)`, no `ClassDB::instantiate("Name")`). All
// engine access is via direct C++ symbols (`FileAccess::open`,
// `DirAccess::open`, `print_line`, `PackedByteArray`, etc.), which are
// unaffected by what is or isn't registered with ClassDB.
//
// To disable this lever, drop the `-I${SCRIPT_DIR}/zym_shim` flag from
// build_godot_linux_x86_64.sh; the build is then byte-identical to a stock
// Godot static-library build.
// =============================================================================

#include_next "core/object/class_db.h"

#ifdef GDREGISTER_CLASS
#undef GDREGISTER_CLASS
#endif
#ifdef GDREGISTER_VIRTUAL_CLASS
#undef GDREGISTER_VIRTUAL_CLASS
#endif
#ifdef GDREGISTER_ABSTRACT_CLASS
#undef GDREGISTER_ABSTRACT_CLASS
#endif
#ifdef GDREGISTER_INTERNAL_CLASS
#undef GDREGISTER_INTERNAL_CLASS
#endif
#ifdef GDREGISTER_RUNTIME_CLASS
#undef GDREGISTER_RUNTIME_CLASS
#endif
#ifdef GDREGISTER_NATIVE_STRUCT
#undef GDREGISTER_NATIVE_STRUCT
#endif

#define GDREGISTER_CLASS(m_class)                 ((void)0)
#define GDREGISTER_VIRTUAL_CLASS(m_class)         ((void)0)
#define GDREGISTER_ABSTRACT_CLASS(m_class)        ((void)0)
#define GDREGISTER_INTERNAL_CLASS(m_class)        ((void)0)
#define GDREGISTER_RUNTIME_CLASS(m_class)         ((void)0)
#define GDREGISTER_NATIVE_STRUCT(m_class, m_code) ((void)0)
