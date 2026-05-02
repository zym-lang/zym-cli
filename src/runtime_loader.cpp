#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "runtime_loader.hpp"
#include "natives/natives.hpp"
#include "pack/zpk_reader.hpp"
#include "zym/zym.h"

// Discovery and bytecode extraction now live in `src/pack/zpk_reader`.
// The on-disk format is documented in `docs/formats/zpk.md`. This file
// is the runtime stub: resolve self-exe, open the bundled payload,
// pull the entry bytecode out of the manifest, and hand it to the VM.

char* get_executable_path(char* buffer, size_t size) {
#ifdef _WIN32
    DWORD result = GetModuleFileNameA(NULL, buffer, (DWORD)size);
    if (result == 0 || result == size) {
        return nullptr;
    }
    return buffer;
#else
    ssize_t len = readlink("/proc/self/exe", buffer, size - 1);
    if (len == -1) {
        return nullptr;
    }
    buffer[len] = '\0';
    return buffer;
#endif
}

bool has_embedded_bytecode() {
    // Probe the running executable for a valid `.zpk` footer.
    return zpk_reader_self_exe_has_payload() != 0;
}

int runtime_main(int argc, char** argv, ZymAllocator* allocator) {
    ZpkReader reader;
    if (!zpk_reader_open_self_exe(&reader)) {
        return 1;
    }

    size_t bytecode_size = 0;
    char* bytecode = zpk_reader_read_entry(&reader, reader.footer.entry_index, &bytecode_size);
    if (!bytecode) {
        zpk_reader_close(&reader);
        return 1;
    }

    // The reader has copied the entry bytes; the file mapping is no
    // longer needed for startup. Releasing it early frees a few MB
    // back to the OS before the VM starts allocating.
    zpk_reader_close(&reader);

    if (bytecode_size < 5 || memcmp(bytecode, "ZYM\0", 4) != 0) {
        fprintf(stderr, "Error: Invalid bytecode format (missing ZYM header).\n");
        free(bytecode);
        return 1;
    }

    ZymVM* vm = zym_newVM(allocator);
    ZymChunk* chunk = zym_newChunk(vm);

    setupNatives(vm);

    if (zym_deserializeChunk(vm, chunk, bytecode, bytecode_size) != ZYM_STATUS_OK) {
        fprintf(stderr, "Error: Failed to deserialize bytecode.\n");
        free(bytecode);
        zym_freeChunk(vm, chunk);
        zym_freeVM(vm);
        return 1;
    }

    free(bytecode);

    ZymStatus result = zym_runChunk(vm, chunk);
    while (result == ZYM_STATUS_YIELD) {
        result = zym_resume(vm);
    }
    if (result != ZYM_STATUS_OK) {
        fprintf(stderr, "Error: Runtime error occurred.\n");
        zym_freeChunk(vm, chunk);
        zym_freeVM(vm);
        return 1;
    }

    ZymValue argv_list = zym_newList(vm);
    for (int i = 0; i < argc; i++) {
        zym_listAppend(vm, argv_list, zym_newString(vm, argv[i]));
    }

    if (zym_hasFunction(vm, "main", 1)) {
        ZymStatus call_result = zym_call(vm, "main", 1, argv_list);
        while (call_result == ZYM_STATUS_YIELD) {
            call_result = zym_resume(vm);
        }
        if (call_result != ZYM_STATUS_OK) {
            fprintf(stderr, "Error: main(argv) function failed.\n");
            zym_freeChunk(vm, chunk);
            zym_freeVM(vm);
            return 1;
        }
    }

    zym_freeChunk(vm, chunk);
    zym_freeVM(vm);

    return 0;
}
