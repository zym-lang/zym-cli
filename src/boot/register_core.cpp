// =============================================================================
// src/boot/register_core.cpp
// -----------------------------------------------------------------------------
// zym's hand-rolled replacement for Godot's `::register_core_types()` /
// `::unregister_core_types()` (godot/core/register_core_types.cpp at
// submodule SHA 8f0647b00ea750be863626ed6641f7bb799e0024).
//
// Strategy (additive bottom-up):
//   1. Bodies start EMPTY.
//   2. Run tests/cli + tests/core. Each failure pinpoints exactly one
//      engine-side init that zym genuinely depends on.
//   3. Re-add only the proven-needed lines. Every register line gets a
//      paired unregister line in reverse order, in the SAME commit.
//   4. The engine's ~218-line body lives in src/boot/ENGINE_REFERENCE.md
//      as a verbatim snapshot. When the godot submodule is bumped, diff
//      the new engine source against ENGINE_REFERENCE.md to spot any new
//      zym-relevant init lines (and update the snapshot + SHA).
//
// What stays in godot_host.cpp (out of scope for this replacement):
//   - `register_core_settings()`           (engine-side, kept verbatim)
//   - `register_early_core_singletons()`   (engine-side, kept verbatim)
//   - `register_core_singletons()`         (engine-side, kept verbatim)
//   - OS_LinuxBSD / Engine / PackedData / ProjectSettings lifecycle
//
// Confirmed permanent skips (zym never touches these, so they will not
// come back regardless of test failures):
//   - register_global_constants()                    -- reflection only
//   - 8 ClassDB::register_custom_instance_class<T>() -- HTTPClient,
//     X509Certificate, CryptoKey, HMACContext, Crypto, StreamPeerTLS,
//     PacketPeerDTLS, DTLSServer
//   - resource_format_saver_crypto / resource_saver_json /
//     resource_loader_gdextension / core_bind resource loader|saver
//   - GDExtensionManager + ResourceUID singletons
//   - IP::create()
//   - Every GDREGISTER_* expansion (already noop'd by the class_db.h shim
//     -- removing the call site here lets `--gc-sections` evict the
//     entire register_core_types.cpp.o + transitively unreferenced
//     ClassInfo / get_class_static() chains)
// =============================================================================

#include "register_core.hpp"

// Access widening. The three foundational entrypoints we need
// (`ObjectDB::setup` / `ObjectDB::cleanup` and `StringName::setup` /
// `StringName::cleanup`) are declared `private` on their owning classes
// and intentionally not part of the engine's public API -- they are
// only meant to be invoked from godot/core/register_core_types.cpp.
//
// We can't modify godot/, can't `friend` ourselves into the classes,
// and `#define private public` isn't reliable here (GCC accepts it for
// `protected` -- as already used in src/godot_host.cpp -- but does NOT
// honour it for `private` on these particular headers; keyword macro
// replacement is implementation-defined per [macro.replace]).
//
// Cleanest fix that doesn't touch godot/: compile this single TU with
// `-fno-access-control` (GCC/Clang flag that disables access checks for
// the whole TU). Wired in CMakeLists.txt via:
//
//   set_source_files_properties(src/boot/register_core.cpp
//       PROPERTIES COMPILE_OPTIONS "-fno-access-control")
//
// Confined to this TU so the access-widening can't leak ODR-wise
// elsewhere in the build.
#include "core/core_string_names.h"
#include "core/object/object.h"
#include "core/string/string_name.h"

namespace zym::boot {

void register_core_types() {
	// --- Additive batch #1: foundational state ---------------------------
	// Discovered by: 18/18 core tests SEGV'd in StringName ctor with
	// "Condition !configured is true" propagating into ProjectSettings
	// ctor's first Object::set call. The engine's register_core_types()
	// brings these up at lines 133-136 before anything else, and they
	// have no zym-side substitute -- ObjectDB owns the global Object
	// instance map, StringName owns the global interned-string table,
	// and CoreStringNames seeds the well-known name pool that core
	// macros (PROPERTY_HINT_*, signal names, ...) reference.
	ObjectDB::setup();
	StringName::setup();
	CoreStringNames::create();
}

void unregister_core_types() {
	// Reverse order of register_core_types() above. Teardown is symmetric
	// because register_*_core_singletons() / register_core_extensions()
	// are intentionally not called from godot_host.cpp (see the long
	// comment there) -- so the only state we need to tear down is the
	// foundational state we set up above.
	CoreStringNames::free();
	StringName::cleanup();
	ObjectDB::cleanup();
}

} // namespace zym::boot
