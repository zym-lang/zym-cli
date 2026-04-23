#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "runtime_loader.hpp"
#include "full_executor.hpp"
#include "godot_host.hpp"
#include "zym/zym.h"

// ---- Custom Allocator Example ------------------------------------------------
// This demonstrates how to define and use a custom allocator with the Zym API.
// In practice, you could replace these with a pool allocator, arena allocator,
// or any other memory management strategy. Here we simply wrap the standard
// C library functions to show the wiring.

static void* cli_alloc(void* ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}

static void* cli_calloc(void* ctx, size_t count, size_t size) {
    (void)ctx;
    return calloc(count, size);
}

static void* cli_realloc(void* ctx, void* ptr, size_t old_size, size_t new_size) {
    (void)ctx;
    (void)old_size;
    return realloc(ptr, new_size);
}

static void cli_free(void* ctx, void* ptr, size_t size) {
    (void)ctx;
    (void)size;
    free(ptr);
}

int main(int argc, char** argv)
{
    // Godot core up before Zym, down on scope exit (covers all return paths).
    zym::godot_host::Scope godot_scope;

    // Create a custom allocator that wraps standard malloc/free.
    // A real application could point these at a custom pool, arena, etc.
    ZymAllocator allocator = {
        cli_alloc,
        cli_calloc,
        cli_realloc,
        cli_free,
        nullptr
    };

    if (has_embedded_bytecode()) {
        return runtime_main(argc, argv, &allocator);
    }
    return full_main(argc, argv, &allocator);
}
