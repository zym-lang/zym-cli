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
#include "core/core_globals.h"
#include "core/core_string_names.h"
#include "core/io/ip.h"
#include "core/object/object.h"
#include "core/os/memory.h"
#include "core/string/string_name.h"

// mbedtls module owns the `_create` factories for `Crypto`, `CryptoKey`,
// `X509Certificate`, and `HMACContext`. We deliberately do NOT call the
// stock `initialize_mbedtls_module(...)` wrapper here -- that wrapper's
// first line is `GLOBAL_DEF("network/tls/enable_tls_v1.3", true)`, which
// dereferences `ProjectSettings::get_singleton()` and segfaults in zym
// (where no `ProjectSettings` exists). Instead we replicate just the
// pieces of the wrapper that don't depend on ProjectSettings/OS:
// `psa_crypto_init` (mbedtls 3+) and the four
// `CryptoMbedTLS / StreamPeerMbedTLS / PacketPeerMbedDTLS / DTLSServerMbedTLS::initialize_*`
// calls that install the static factory pointers `Crypto::_create` /
// `CryptoKey::_create` / `X509Certificate::_create` / `HMACContext::_create`
// (plus the TLS / DTLS server factories for symmetry).
#include "modules/mbedtls/crypto_mbedtls.h"
#include "modules/mbedtls/dtls_server_mbedtls.h"
#include "modules/mbedtls/packet_peer_mbed_dtls.h"
#include "modules/mbedtls/stream_peer_mbedtls.h"

#if defined(MBEDTLS_VERSION_MAJOR) && MBEDTLS_VERSION_MAJOR >= 3
#include <psa/crypto.h>
#endif

// Godot's mbedtls thirdparty config defines `MBEDTLS_THREADING_ALT` when
// `THREADS_ENABLED` is set (which it is in the zym build), so mbedtls
// will dispatch every internal mutex through the four `godot_mbedtls_*`
// callbacks defined in `godot/modules/mbedtls/register_types.cpp`. If
// `mbedtls_threading_set_alt(...)` is never called, those callbacks stay
// nullptr and the very first `mbedtls_entropy_func` call (e.g. from
// `CryptoMbedTLS::CryptoMbedTLS()` -> `mbedtls_ctr_drbg_seed`) hits a
// null mutex op and returns `MBEDTLS_ERR_ENTROPY_NO_STRONG_SOURCE` (-52).
#if defined(MBEDTLS_THREADING_ALT)
#include <mbedtls/threading.h>
extern "C" {
void godot_mbedtls_mutex_init(mbedtls_threading_mutex_t *p_mutex);
void godot_mbedtls_mutex_free(mbedtls_threading_mutex_t *p_mutex);
int  godot_mbedtls_mutex_lock(mbedtls_threading_mutex_t *p_mutex);
int  godot_mbedtls_mutex_unlock(mbedtls_threading_mutex_t *p_mutex);
}
#endif

namespace zym::boot {

// Owned by `register_core_types()` / freed in `unregister_core_types()`.
static IP *ip = nullptr;

void register_core_types() {
	// --- Build-time toggle: engine error/warning output ------------------
	// Driven by the CMake option `USE_ENGINE_ERRORS` -> `ZYM_ENGINE_ERRORS`.
	// When 0, suppress Godot's `ERROR: ... at: <fn> (file.cpp:N)` lines on
	// stderr (all `ERR_FAIL_*` / `ERR_PRINT` macros short-circuit through
	// `CoreGlobals::print_error_enabled`). zym's own runtime errors are
	// plain interpreter strings and are unaffected.
#if defined(ZYM_ENGINE_ERRORS) && ZYM_ENGINE_ERRORS == 0
	CoreGlobals::print_error_enabled = false;
#endif

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

	// --- Additive batch #2: mbedtls-backed crypto factories --------------
	// Replicates the body of `initialize_mbedtls_module(MODULE_INITIALIZATION_LEVEL_CORE)`
	// minus its `GLOBAL_DEF(...)` and verbose-stdout `OS::get_singleton()`
	// dependencies (we have neither `ProjectSettings` nor a fully wired
	// `OS` singleton at this point). After these calls, the static
	// `_create` slots for `Crypto`, `CryptoKey`, `X509Certificate`, and
	// `HMACContext` point at the mbedtls implementations.
#if defined(MBEDTLS_THREADING_ALT)
	::mbedtls_threading_set_alt(
			::godot_mbedtls_mutex_init,
			::godot_mbedtls_mutex_free,
			::godot_mbedtls_mutex_lock,
			::godot_mbedtls_mutex_unlock);
#endif
#if defined(MBEDTLS_VERSION_MAJOR) && MBEDTLS_VERSION_MAJOR >= 3
	(void)::psa_crypto_init();
#endif
	CryptoMbedTLS::initialize_crypto();
	StreamPeerMbedTLS::initialize_tls();
	PacketPeerMbedDTLS::initialize_dtls();
	DTLSServerMbedTLS::initialize();

	// Load default CA chain (system trust store) so `TLS.connect(...)`
	// against real-world hosts (https://example.com etc.) can verify the
	// server cert without the script needing to pass `trustedRoots`.
	// `Crypto::load_default_certificates("")` falls back to
	// `OS::get_system_ca_certificates()` (or the builtin bundle if the
	// engine was compiled with `BUILTIN_CERTS_ENABLED`). Calls into
	// `mbedtls_x509_crt_parse` and is safe to invoke at startup; passing
	// an empty path tells it "use system roots, not a project-defined
	// path". Skipping this leaves `default_certs` null, which in turn
	// makes `TLSContextMbedTLS::init_client` return `ERR_UNCONFIGURED`
	// for any non-`unsafe` client.
	Crypto::load_default_certificates(String());

	// --- Additive batch #3: IP singleton --------------------------------
	// `IP` is the engine's DNS/local-interface namespace and is the
	// foundation for every networking native (TCP/UDP/TLS). On Linux
	// `IP::create()` returns a fresh `IPUnix` (via the platform-side
	// `_create` factory installed when `ip_unix.cpp` is linked into
	// libcore). Without this, `IP::get_singleton()` is null and the
	// IP native's `resolve` / `resolveAll` / `localAddresses` would
	// crash on the very first call.
	ip = IP::create();
}

void unregister_core_types() {
	// Reverse order of register_core_types() above. Teardown is symmetric
	// because register_*_core_singletons() / register_core_extensions()
	// are intentionally not called from godot_host.cpp (see the long
	// comment there) -- so the only state we need to tear down is the
	// foundational state we set up above.
	if (ip) {
		memdelete(ip);
		ip = nullptr;
	}
	DTLSServerMbedTLS::finalize();
	PacketPeerMbedDTLS::finalize_dtls();
	StreamPeerMbedTLS::finalize_tls();
	CryptoMbedTLS::finalize_crypto();
#if defined(MBEDTLS_VERSION_MAJOR) && MBEDTLS_VERSION_MAJOR >= 3
	::mbedtls_psa_crypto_free();
#endif
	CoreStringNames::free();
	StringName::cleanup();
	ObjectDB::cleanup();
}

} // namespace zym::boot
