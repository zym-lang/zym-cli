// Minimal lifecycle shim for the Godot core singletons (OS, Engine,
// ProjectSettings, PackedData, ClassDB) used by zym-cli natives.
// Call init() once before any Godot API, shutdown() once after. Use Scope
// for RAII. zym_core never includes this - only zym-cli main/natives do.

#pragma once

namespace zym::godot_host {

// Bring singletons up. Call once. Returns true on success.
bool init();

// Tear singletons down. Call once, only after a successful init().
void shutdown();

// RAII: init() in ctor, shutdown() in dtor. Drop at top of main().
struct Scope {
    bool ok;
    Scope() : ok(init()) {}
    ~Scope() { if (ok) { shutdown(); } }
    Scope(const Scope&)            = delete;
    Scope& operator=(const Scope&) = delete;
};

} // namespace zym::godot_host
