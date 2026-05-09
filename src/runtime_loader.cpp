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
#include "pack/zpk_format.h"
#include "zym/zym.h"
#include "zym/module_loader.h"

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

// Disk-backed module read callback used when the program entry is a
// source-kind entry. Module resolution is intentionally disk-only:
// imports inside an `entry_source` script are looked up on disk
// relative to the script's working directory, not inside the bundle.
// This is documented in docs/cli/pack.md.
static ModuleReadResult disk_module_read_cb(const char* path, void* user_data) {
    ModuleReadResult result = { .source = NULL, .source_map = NULL, .file_id = ZYM_FILE_ID_INVALID };
    ZymVM* vm = (ZymVM*)user_data;

    FILE* f = fopen(path, "rb");
    if (!f) return result;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return result; }
    rewind(f);
    char* raw = (char*)malloc((size_t)sz + 1);
    if (!raw) { fclose(f); return result; }
    size_t got = fread(raw, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(raw); return result; }
    raw[sz] = '\0';

    ZymFileId file_id = zym_registerSourceFile(vm, path, raw, (size_t)sz);
    ZymSourceMap* source_map = zym_newSourceMap(vm);
    const char* preprocessed = NULL;
    ZymStatus status = zym_preprocess(vm, raw, source_map, file_id, &preprocessed);
    free(raw);

    if (status != ZYM_STATUS_OK) {
        zym_freeSourceMap(vm, source_map);
        return result;
    }
    result.source = (char*)preprocessed;
    result.source_map = source_map;
    result.file_id = file_id;
    return result;
}

// Run a source-kind entry: register it as a source file, preprocess,
// load on-disk modules, compile, and run. Mirrors the compile pipeline
// in `src/full_executor.cpp::compile_source_to_bytecode` but executes
// directly without the de/serialize verification round-trip.
//
// `entry_name` is the bundle entry's name (used as the source-file
// label for stack traces); a NULL or empty name falls back to
// "<entry>".
static int run_source_entry(const char* source, size_t source_size,
                            const char* entry_name,
                            int argc, char** argv,
                            ZymAllocator* allocator) {
    const char* source_label = (entry_name && *entry_name) ? entry_name : "<entry>";

    // `zym_registerSourceFile` / preprocess expect a NUL-terminated C
    // string. The reader hands us raw bytes sized to the entry, so we
    // copy + terminate here.
    char* src = (char*)malloc(source_size + 1);
    if (!src) {
        fprintf(stderr, "Error: Out of memory for entry source.\n");
        return 1;
    }
    memcpy(src, source, source_size);
    src[source_size] = '\0';

    ZymVM* vm = zym_newVM(allocator);
    ZymSourceMap* source_map = zym_newSourceMap(vm);
    ZymChunk* chunk = zym_newChunk(vm);

    setupNatives(vm);

    ZymFileId entry_id = zym_registerSourceFile(vm, source_label, src, source_size);
    const char* processed = NULL;
    if (zym_preprocess(vm, src, source_map, entry_id, &processed) != ZYM_STATUS_OK) {
        fprintf(stderr, "Error: Preprocessing failed.\n");
        free(src);
        zym_freeChunk(vm, chunk);
        zym_freeSourceMap(vm, source_map);
        zym_freeVM(vm);
        return 1;
    }

    // Modules are resolved from disk only — modules are a compile-time
    // concept and are not resolved from inside the bundle at runtime.
    // Ship a fully-compiled `entry_bytecode` if you need a self-contained
    // bundle.
    ModuleLoadResult* mr = loadModules(vm, processed, source_map, source_label,
                                       disk_module_read_cb, vm,
                                       /*use_debug_names=*/true,
                                       /*hash_module_names=*/false,
                                       NULL);
    if (mr->has_error) {
        fprintf(stderr, "Error: Module loading failed: %s\n", mr->error_message);
        zym_freeProcessedSource(vm, processed);
        free(src);
        freeModuleLoadResult(vm, mr);
        zym_freeChunk(vm, chunk);
        zym_freeSourceMap(vm, source_map);
        zym_freeVM(vm);
        return 1;
    }

    ZymCompilerConfig cfg = { .include_line_info = true };
    const char* entry_path = mr->module_count > 0 ? mr->module_paths[0] : source_label;
    if (zym_compile(vm, mr->combined_source, chunk, mr->source_map,
                    (char*)entry_path, cfg, NULL) != ZYM_STATUS_OK) {
        fprintf(stderr, "Error: Compilation failed.\n");
        zym_freeProcessedSource(vm, processed);
        free(src);
        freeModuleLoadResult(vm, mr);
        zym_freeChunk(vm, chunk);
        zym_freeSourceMap(vm, source_map);
        zym_freeVM(vm);
        return 1;
    }

    zym_freeProcessedSource(vm, processed);
    free(src);
    freeModuleLoadResult(vm, mr);

    ZymStatus result = zym_runChunk(vm, chunk);
    while (result == ZYM_STATUS_YIELD) {
        result = zym_resume(vm);
    }
    if (result != ZYM_STATUS_OK) {
        fprintf(stderr, "Error: Runtime error occurred.\n");
        zym_freeChunk(vm, chunk);
        zym_freeSourceMap(vm, source_map);
        zym_freeVM(vm);
        return 1;
    }

    char exe_path_buf[4096];
    const char* argv0 = get_executable_path(exe_path_buf, sizeof(exe_path_buf));
    if (!argv0) argv0 = (argc > 0 ? argv[0] : "");

    ZymValue argv_list = zym_newList(vm);
    zym_listAppend(vm, argv_list, zym_newString(vm, argv0));
    for (int i = 1; i < argc; i++) {
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
            zym_freeSourceMap(vm, source_map);
            zym_freeVM(vm);
            return 1;
        }
    }

    zym_freeChunk(vm, chunk);
    zym_freeSourceMap(vm, source_map);
    zym_freeVM(vm);
    return 0;
}

int runtime_main(int argc, char** argv, ZymAllocator* allocator) {
    ZpkReader reader;
    if (!zpk_reader_open_self_exe(&reader)) {
        return 1;
    }

    const uint32_t idx = reader.footer.entry_index;
    if (idx >= reader.footer.entry_count) {
        fprintf(stderr, "Error: Bundle entry_index out of range.\n");
        zpk_reader_close(&reader);
        return 1;
    }
    const uint8_t entry_kind = reader.manifest[idx].kind;

    // Capture the entry's name for source-kind dispatch (used as the
    // source-file label for stack traces).
    char entry_name_buf[512];
    entry_name_buf[0] = '\0';
    {
        const ZpkEntry& e = reader.manifest[idx];
        if (e.name_length > 0 && reader.strtab) {
            size_t n = (size_t)e.name_length;
            if (n >= sizeof(entry_name_buf)) n = sizeof(entry_name_buf) - 1;
            memcpy(entry_name_buf, reader.strtab + e.name_offset, n);
            entry_name_buf[n] = '\0';
        }
    }

    size_t bytecode_size = 0;
    char* bytecode = zpk_reader_read_entry(&reader, idx, &bytecode_size);
    if (!bytecode) {
        zpk_reader_close(&reader);
        return 1;
    }

    // The reader has copied the entry bytes; the file mapping is no
    // longer needed for startup. Releasing it early frees a few MB
    // back to the OS before the VM starts allocating.
    zpk_reader_close(&reader);

    // ----- entry_source: compile-on-load + run -----
    if (entry_kind == ZPK_KIND_ENTRY_SOURCE) {
        int rc = run_source_entry(bytecode, bytecode_size, entry_name_buf,
                                  argc, argv, allocator);
        free(bytecode);
        return rc;
    }

    // ----- entry_bytecode: existing fast path -----
    if (entry_kind != ZPK_KIND_ENTRY_BYTECODE) {
        fprintf(stderr, "Error: Bundle entry kind 0x%02X is not runnable.\n",
                (unsigned)entry_kind);
        free(bytecode);
        return 1;
    }

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

    char exe_path_buf[4096];
    const char* argv0 = get_executable_path(exe_path_buf, sizeof(exe_path_buf));
    if (!argv0) argv0 = (argc > 0 ? argv[0] : "");

    ZymValue argv_list = zym_newList(vm);
    zym_listAppend(vm, argv_list, zym_newString(vm, argv0));
    for (int i = 1; i < argc; i++) {
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
