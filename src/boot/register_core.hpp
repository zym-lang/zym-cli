// =============================================================================
// src/boot/register_core.hpp
// -----------------------------------------------------------------------------
// zym's hand-rolled replacement for Godot's `::register_core_types()` /
// `::unregister_core_types()` (godot/core/register_core_types.cpp). See
// register_core.cpp for the rationale and the running ENGINE_REFERENCE diff.
//
// Strategy: additive. Bodies start empty. Lines are added one at a time
// only when a real test failure proves the engine's behaviour was depended
// on. Every additive line in register_core_types() must be paired with a
// symmetric teardown line in unregister_core_types() in the same commit,
// in reverse order.
//
// Called from src/godot_host.cpp instead of `::register_core_types()` /
// `::unregister_core_types()`. The `godot_host` boot sequence still calls
// the engine's `register_core_settings()` / `register_early_core_singletons()`
// / `register_core_singletons()` for now -- those are out of scope for
// this replacement.
// =============================================================================
#pragma once

namespace zym::boot {

void register_core_types();
void unregister_core_types();

// Lazily load the system CA chain (system trust store) on first use. Safe
// to call repeatedly and from any thread; the underlying
// `Crypto::load_default_certificates(String())` is invoked exactly once
// via `std::call_once`. See register_core.cpp for the rationale (the
// Windows `OS::get_system_ca_certificates()` walk costs ~0.5-2s and was
// previously running unconditionally at boot from `register_core_types`).
void ensure_default_certificates_loaded();

} // namespace zym::boot
