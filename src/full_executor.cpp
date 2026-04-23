#define DEBUG_SHOW 0

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifndef _WIN32
    #include <unistd.h>
    #define _fileno fileno
    #define _dup    dup
    #define _dup2   dup2
    #define _close  close
    #define _isatty isatty
    #define _access access
    #define F_OK 0
#else
    #include <io.h>
#endif
#include <fcntl.h>

#include "full_executor.hpp"
#include "runtime_loader.hpp"
#include "natives/natives.hpp"
#include "zym/zym.h"
#include "zym/module_loader.h"
#include "zym/debug.h"

#define FOOTER_MAGIC "ZYMBCODE"

// ---------------------------------------------------------------------------
// Phase 1.7 — rich diagnostic rendering.
//
// Consumes the structured fields that `ZymDiagnostic` has carried since
// Phase 1.3/1.6 (fileId / startByte / length / startLine / startColumn /
// endLine / endColumn / severity / code / hint) to emit rustc-style output:
//
//     error: Expect expression.
//      --> tests/recovery/missing_expression.zym:3:9
//       |
//     3 | var x = ;
//       |         ^
//
// Diagnostics are sorted by (fileId, startByte) for deterministic order,
// grouped by file so the `-->` header is informative, and colored when
// stderr is a TTY. A final summary ("N errors, M warnings") is printed
// when more than one diagnostic was produced.
// ---------------------------------------------------------------------------

typedef struct {
    int diag_index;
    int file_id;
    int start_byte;
    int line;
} DiagSortKey;

static int diag_sort_cmp(const void* pa, const void* pb) {
    const DiagSortKey* a = (const DiagSortKey*)pa;
    const DiagSortKey* b = (const DiagSortKey*)pb;
    if (a->file_id != b->file_id) return (a->file_id < b->file_id) ? -1 : 1;
    if (a->start_byte != b->start_byte) {
        if (a->start_byte < 0 && b->start_byte < 0) {
            if (a->line != b->line) return (a->line < b->line) ? -1 : 1;
            return 0;
        }
        if (a->start_byte < 0) return 1;
        if (b->start_byte < 0) return -1;
        return (a->start_byte < b->start_byte) ? -1 : 1;
    }
    return 0;
}

static int severity_is_error(ZymDiagSeverity s) {
    return s == ZYM_DIAG_ERROR;
}

static int severity_is_warning(ZymDiagSeverity s) {
    return s == ZYM_DIAG_WARNING;
}

static const char* severity_label(ZymDiagSeverity s) {
    switch (s) {
        case ZYM_DIAG_WARNING: return "warning";
        case ZYM_DIAG_INFO:    return "info";
        case ZYM_DIAG_HINT:    return "hint";
        case ZYM_DIAG_ERROR:
        default:               return "error";
    }
}

// ANSI colors when stderr is a TTY.
static const char* severity_color(ZymDiagSeverity s, int use_color) {
    if (!use_color) return "";
    switch (s) {
        case ZYM_DIAG_WARNING: return "\x1b[1;33m"; // bold yellow
        case ZYM_DIAG_INFO:    return "\x1b[1;34m"; // bold blue
        case ZYM_DIAG_HINT:    return "\x1b[1;36m"; // bold cyan
        case ZYM_DIAG_ERROR:
        default:               return "\x1b[1;31m"; // bold red
    }
}
static const char* color_reset(int use_color)  { return use_color ? "\x1b[0m"  : ""; }
static const char* color_bold(int use_color)   { return use_color ? "\x1b[1m"  : ""; }
static const char* color_dim(int use_color)    { return use_color ? "\x1b[2m"  : ""; }

// Count decimal digits for gutter alignment (min 1).
static int count_digits(int n) {
    if (n <= 0) return 1;
    int d = 0;
    while (n > 0) { d++; n /= 10; }
    return d;
}

// Find the byte range of the line containing `offset` inside bytes[0..len).
// Sets *out_start, *out_end (exclusive of the trailing '\n').
static void line_range_at(const char* bytes, int len, int offset,
                          int* out_start, int* out_end) {
    if (offset < 0) offset = 0;
    if (offset > len) offset = len;
    int s = offset;
    while (s > 0 && bytes[s - 1] != '\n') s--;
    int e = offset;
    while (e < len && bytes[e] != '\n') e++;
    if (out_start) *out_start = s;
    if (out_end)   *out_end   = e;
}

// Render the 1-based line `line_no` from `sf` with a caret under the
// (column, length) range. column is 1-based bytes; length may be 0.
static void render_source_line(FILE* out,
                               const ZymSourceFileInfo* sf,
                               int line_no, int column, int length,
                               int gutter_w,
                               ZymDiagSeverity sev, int use_color) {
    if (sf == NULL || sf->bytes == NULL || line_no <= 0) return;

    // Walk forward to the nth newline.
    int remaining = line_no - 1;
    int p = 0;
    int total = (int)sf->length;
    while (remaining > 0 && p < total) {
        if (sf->bytes[p] == '\n') remaining--;
        p++;
    }
    if (remaining > 0) return; // line_no past EOF

    int line_start = p;
    int line_end;
    line_range_at(sf->bytes, total, p, &line_start, &line_end);
    int line_len = line_end - line_start;
    if (line_len < 0) line_len = 0;

    // Empty-gutter separator line.
    fprintf(out, "%s%*s |%s\n",
            color_dim(use_color), gutter_w, "", color_reset(use_color));

    // Source line.
    fprintf(out, "%s%*d |%s %.*s\n",
            color_dim(use_color), gutter_w, line_no, color_reset(use_color),
            line_len, sf->bytes + line_start);

    // Caret line.
    if (column > 0) {
        int col_zero = column - 1;
        if (col_zero > line_len) col_zero = line_len;
        int span = length;
        if (span <= 0) span = 1;
        if (col_zero + span > line_len + 1) span = (line_len + 1) - col_zero;
        if (span < 1) span = 1;

        fprintf(out, "%s%*s |%s ", color_dim(use_color), gutter_w, "",
                color_reset(use_color));
        // Preserve tabs in leading run for correct visual alignment.
        for (int i = 0; i < col_zero; i++) {
            char c = sf->bytes[line_start + i];
            fputc(c == '\t' ? '\t' : ' ', out);
        }
        fputs(severity_color(sev, use_color), out);
        for (int i = 0; i < span; i++) fputc('^', out);
        fputs(color_reset(use_color), out);
        fputc('\n', out);
    }
}

static void render_diagnostic(FILE* out,
                              ZymVM* vm,
                              const ZymDiagnostic* d,
                              ZymFileId* last_file,
                              int gutter_w,
                              int use_color) {
    const char* label = severity_label(d->severity);
    const char* color = severity_color(d->severity, use_color);
    const char* reset = color_reset(use_color);
    const char* bold  = color_bold(use_color);

    // Header: `error: <message>`   (or `error[E0001]: <message>` when coded)
#if ZYM_HAS_DIAGNOSTIC_CODES
    if (d->code && d->code[0]) {
        fprintf(out, "%s%s[%s]%s%s: %s\n",
                color, label, d->code, reset, bold,
                d->message ? d->message : "");
        fputs(reset, out);
    } else
#endif
    {
        fprintf(out, "%s%s%s%s: %s\n",
                color, label, reset, bold, d->message ? d->message : "");
        fputs(reset, out);
    }

    // --> path:line:col   (only when we have a file; reprint per diag so
    //                       the line is self-contained and greppable)
    ZymSourceFileInfo sf = {0};
    int have_sf = zym_getSourceFile(vm, d->fileId, &sf);
    const char* path = (have_sf && sf.path) ? sf.path : NULL;

    if (path || d->line > 0) {
        fprintf(out, "%s%*s -->%s ", color_dim(use_color), gutter_w, "", reset);
        if (path) fputs(path, out);
        else      fputs("<source>", out);
        if (d->line > 0) {
            fprintf(out, ":%d", d->line);
            if (d->column > 0) fprintf(out, ":%d", d->column);
        }
        fputc('\n', out);
    }

    // Caret line (only if we have a span and source bytes).
    if (have_sf && sf.bytes && d->line > 0) {
        render_source_line(out, &sf, d->line,
                           d->column > 0 ? d->column : 1,
                           d->length,
                           gutter_w,
                           d->severity, use_color);
    }

#if ZYM_HAS_DIAGNOSTIC_CODES
    if (d->hint && d->hint[0]) {
        fprintf(out, "%s%*s =%s %shint:%s %s\n",
                color_dim(use_color), gutter_w, "", reset,
                color_bold(use_color), reset, d->hint);
    }
#endif

    (void)last_file;
}

static void drain_and_print_diagnostics(ZymVM* vm) {
    size_t count = 0;
    const ZymDiagnostic* diags = zymGetDiagnostics(vm, &count);
    if (count == 0) return;

    int use_color = 0;
#ifndef _WIN32
    use_color = isatty(fileno(stderr));
#endif

    // Stable-ish sort by (fileId, startByte); keep original index as the
    // tiebreaker so diagnostics on the same token still print in arrival order.
    DiagSortKey* order = (DiagSortKey*)malloc(count * sizeof(DiagSortKey));
    if (order == NULL) {
        for (size_t i = 0; i < count; i++) {
            fprintf(stderr, "%s: %s\n", severity_label(diags[i].severity),
                    diags[i].message ? diags[i].message : "");
        }
        zymClearDiagnostics(vm);
        return;
    }
    int max_line = 0;
    for (size_t i = 0; i < count; i++) {
        order[i].diag_index = (int)i;
        order[i].file_id    = diags[i].fileId;
        order[i].start_byte = diags[i].startByte;
        order[i].line       = diags[i].line;
        int ln = diags[i].line;
        if (ln > max_line) max_line = ln;
    }
    qsort(order, count, sizeof(DiagSortKey), diag_sort_cmp);

    // Gutter width is governed by the largest line number seen.
    int gutter_w = count_digits(max_line > 0 ? max_line : 1);
    if (gutter_w < 2) gutter_w = 2;

    ZymFileId last_file = ZYM_FILE_ID_INVALID;
    size_t errors = 0, warnings = 0;
    for (size_t i = 0; i < count; i++) {
        const ZymDiagnostic* d = &diags[order[i].diag_index];
        if (severity_is_error(d->severity))   errors++;
        if (severity_is_warning(d->severity)) warnings++;
        render_diagnostic(stderr, vm, d, &last_file, gutter_w, use_color);
        if (i + 1 < count) fputc('\n', stderr);
    }

    if (count > 1) {
        fprintf(stderr, "\n%s=%s %zu diagnostic%s (%zu error%s, %zu warning%s)\n",
                color_dim(use_color), color_reset(use_color),
                count, count == 1 ? "" : "s",
                errors, errors == 1 ? "" : "s",
                warnings, warnings == 1 ? "" : "s");
    }

    free(order);
    zymClearDiagnostics(vm);
}

static void print_banner(void) {
    printf("\n");
    printf("  =====================================================================\n");
    printf("  |                                                                   |\n");
    printf("  |    ########  ##    ##  ##     ##          v0.3.0                  |\n");
    printf("  |       ##      ##  ##   ###   ###                                  |\n");
    printf("  |      ##        ####    #### ####     Programming Language         |\n");
    printf("  |     ##          ##     ## ### ##                                  |\n");
    printf("  |    ########     ##     ##     ##     Fast. Simple. Powerful.      |\n");
    printf("  |                                                                   |\n");
    printf("  =====================================================================\n");
    printf("\n");
    printf("  = USAGE =============================================================\n");
    printf("  |                                                                   |\n");
    printf("  |  Basic Commands:                                                  |\n");
    printf("  |    zym                           Show this help information       |\n");
    printf("  |    zym <file.zym>                Compile and run source file      |\n");
    printf("  |    zym <file.zbc>                Run precompiled bytecode         |\n");
    printf("  |                                                                   |\n");
    printf("  |  Compilation:                                                     |\n");
    printf("  |    zym <file.zym> -o <out.zbc>   Compile to bytecode              |\n");
    printf("  |    zym <file.zym> -o <out.exe>   Compile to standalone exe        |\n");
    printf("  |    zym <file.zbc> -o <out.exe>   Pack bytecode into exe           |\n");
    printf("  |                                                                   |\n");
    printf("  |  Cross-Platform Packing:                                          |\n");
    printf("  |    zym <file> -o <out> -r <runtime>  Use explicit runtime binary  |\n");
    printf("  |                                                                   |\n");
    printf("  |  Development Tools:                                               |\n");
    printf("  |    zym <file> --dump              Disassemble to console          |\n");
    printf("  |    zym <file> --dump <out.txt>    Disassemble to file             |\n");
    printf("  |    zym <file> --strip             Strip debug info (smaller)      |\n");
    printf("  |    zym <file.zym> --preprocess    Show preprocessed source        |\n");
    printf("  |    zym <file.zym> --combined      Show combined source+modules    |\n");
    printf("  |                                                                   |\n");
    printf("  |  Output to File:                                                  |\n");
    printf("  |    zym <file.zym> --preprocess <out.zym>                          |\n");
    printf("  |    zym <file.zym> --combined <out.zym>                            |\n");
    printf("  |                                                                   |\n");
    printf("  |  Combine Operations:                                              |\n");
    printf("  |    zym <file.zym> --strip -o <out.zbc>                            |\n");
    printf("  |    zym <file.zym> --dump <out.txt> -o <out.exe>                   |\n");
    printf("  |                                                                   |\n");
    printf("  =====================================================================\n");
    printf("\n");
}

static char* read_file(const char* path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "Error: Could not open file \"%s\".\n", path);
        return NULL;
    }
    fseek(file, 0L, SEEK_END);
    size_t fileSize = ftell(file);
    rewind(file);
    char *buffer = (char*)malloc(fileSize + 1);
    if (buffer == NULL) {
        fprintf(stderr, "Error: Not enough memory to read \"%s\".\n", path);
        fclose(file);
        return NULL;
    }
    size_t bytesRead = fread(buffer, sizeof(char), fileSize, file);
    if (bytesRead < fileSize) {
        fprintf(stderr, "Error: Could not read file \"%s\".\n", path);
        free(buffer);
        fclose(file);
        return NULL;
    }
    buffer[bytesRead] = '\0';
    fclose(file);
    return buffer;
}

static char* read_binary_file(const char* path, size_t* out_size) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "Error: Could not open file \"%s\".\n", path);
        return NULL;
    }
    fseek(file, 0L, SEEK_END);
    size_t fileSize = ftell(file);
    rewind(file);
    char *buffer = (char*)malloc(fileSize);
    if (buffer == NULL) {
        fprintf(stderr, "Error: Not enough memory to read \"%s\".\n", path);
        fclose(file);
        return NULL;
    }
    size_t bytesRead = fread(buffer, sizeof(char), fileSize, file);
    if (bytesRead < fileSize) {
        fprintf(stderr, "Error: Could not read file \"%s\".\n", path);
        free(buffer);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *out_size = fileSize;
    return buffer;
}

static int write_binary_file(const char* path, const char* data, size_t size) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "Error: Could not create file \"%s\".\n", path);
        return 0;
    }
    size_t written = fwrite(data, sizeof(char), size, file);
    fclose(file);
    if (written < size) {
        fprintf(stderr, "Error: Could not write complete data to \"%s\".\n", path);
        return 0;
    }
    return 1;
}

static int validate_bytecode_magic(const char* data, size_t size) {
    if (size < 5) return 0;
    return memcmp(data, "ZYM\0", 4) == 0;
}

static int has_extension(const char* path, const char* ext) {
    size_t path_len = strlen(path);
    size_t ext_len = strlen(ext);
    if (path_len < ext_len) return 0;
    return strcmp(path + path_len - ext_len, ext) == 0;
}

static int is_exe_output(const char* path) {
#ifdef _WIN32
    return has_extension(path, ".exe");
#else
    // On Linux/macOS, an executable output is anything that's NOT .zbc
    return !has_extension(path, ".zbc");
#endif
}

static int pack_bytecode_into_exe(const char* bytecode, size_t bytecode_size, const char* output_path, const char* runtime_path) {
    // Determine which runtime binary to use
    char exe_path[4096];
    const char* stub_path;

    if (runtime_path) {
        // Explicit runtime provided via -r
        stub_path = runtime_path;
    } else {
        // Use the current running executable as the runtime
        if (!get_executable_path(exe_path, sizeof(exe_path))) {
            fprintf(stderr, "Error: Could not determine executable path.\n");
            return 0;
        }
        stub_path = exe_path;
    }

    size_t stub_size = 0;
    char* stub_data = read_binary_file(stub_path, &stub_size);
    if (!stub_data) {
        fprintf(stderr, "Error: Could not read runtime binary: %s\n", stub_path);
        return 0;
    }

    // Build output: [runtime][bytecode][size][magic]
    size_t footer_size = 12; // 4 bytes size + 8 bytes magic
    size_t total_size = stub_size + bytecode_size + footer_size;

    char* output = (char*)malloc(total_size);
    if (!output) {
        fprintf(stderr, "Error: Could not allocate memory for packed executable.\n");
        free(stub_data);
        return 0;
    }

    // Copy runtime binary
    memcpy(output, stub_data, stub_size);
    free(stub_data);

    // Copy bytecode
    memcpy(output + stub_size, bytecode, bytecode_size);

    // Write footer: 4-byte size (little-endian) + 8-byte magic
    uint32_t size_le = (uint32_t)bytecode_size;
    unsigned char* footer = (unsigned char*)(output + stub_size + bytecode_size);
    footer[0] = (size_le) & 0xFF;
    footer[1] = (size_le >> 8) & 0xFF;
    footer[2] = (size_le >> 16) & 0xFF;
    footer[3] = (size_le >> 24) & 0xFF;
    memcpy(footer + 4, FOOTER_MAGIC, 8);

    int success = write_binary_file(output_path, output, total_size);
    free(output);

    if (!success) return 0;

    printf("Packed executable created: %s\n", output_path);
    printf("  Runtime:       %s (%zu bytes)\n", stub_path, stub_size);
    printf("  Bytecode size: %zu bytes\n", bytecode_size);
    printf("  Total size:    %zu bytes\n", total_size);

    return 1;
}

static ModuleReadResult readAndPreprocessCallback(const char* path, void* user_data) {
    ModuleReadResult result = { .source = NULL, .source_map = NULL, .file_id = ZYM_FILE_ID_INVALID };
    ZymVM* vm = (ZymVM*)user_data;
    char* raw_source = read_file(path);
    if (!raw_source) return result;

    // Register each imported module with the VM's source-file registry so
    // scanner tokens carry origin fileIds through the preprocess pass,
    // then produce its per-module SourceMap so module_loader can thread
    // per-line origin info into the combined buffer.
    ZymFileId file_id = zym_registerSourceFile(vm, path, raw_source, strlen(raw_source));
    ZymSourceMap* source_map = zym_newSourceMap(vm);
    const char* preprocessed = NULL;
    ZymStatus status = zym_preprocess(vm, raw_source, source_map, file_id, &preprocessed);

    free(raw_source);

    if (status != ZYM_STATUS_OK) {
        zym_freeSourceMap(vm, source_map);
        return result;
    }

    result.source = (char*)preprocessed;
    result.source_map = source_map;
    result.file_id = file_id;
    return result;
}

static int preprocess_source(const char* source_file, char** out_preprocessed_source, ZymAllocator* allocator) {
    char* pre_source = read_file(source_file);
    if (!pre_source) return 0;

    const char* processed_source = NULL;

    ZymVM* vm = zym_newVM(allocator);
    ZymSourceMap* source_map = zym_newSourceMap(vm);

    setupNatives(vm);

    ZymFileId entry_id = zym_registerSourceFile(vm, source_file, pre_source, strlen(pre_source));
    if (zym_preprocess(vm, pre_source, source_map, entry_id, &processed_source) != ZYM_STATUS_OK) {
        drain_and_print_diagnostics(vm);
        fprintf(stderr, "Error: Preprocessing failed.\n");
        free(pre_source);
        zym_freeSourceMap(vm, source_map);
        zym_freeVM(vm);
        return 0;
    }

    size_t processed_len = strlen(processed_source);
    *out_preprocessed_source = (char*)malloc(processed_len + 1);
    if (!*out_preprocessed_source) {
        fprintf(stderr, "Error: Could not allocate memory for preprocessed source.\n");
        zym_freeProcessedSource(vm, processed_source);
        free(pre_source);
        zym_freeSourceMap(vm, source_map);
        zym_freeVM(vm);
        return 0;
    }
    strcpy(*out_preprocessed_source, processed_source);

    zym_freeProcessedSource(vm, processed_source);
    free(pre_source);
    zym_freeSourceMap(vm, source_map);
    zym_freeVM(vm);

    return 1;
}

static int generate_combined_source(const char* source_file, char** out_combined_source, int use_debug_names, ZymAllocator* allocator) {
    char* pre_source = read_file(source_file);
    if (!pre_source) return 0;

    const char* processed_source = NULL;

    ZymVM* compile_vm = zym_newVM(allocator);
    ZymSourceMap* source_map = zym_newSourceMap(compile_vm);

    setupNatives(compile_vm);

#if DEBUG_SHOW
    printf("Preprocessing source...\n");
#endif
    ZymFileId entry_id = zym_registerSourceFile(compile_vm, source_file, pre_source, strlen(pre_source));
    if (zym_preprocess(compile_vm, pre_source, source_map, entry_id, &processed_source) != ZYM_STATUS_OK) {
        drain_and_print_diagnostics(compile_vm);
        fprintf(stderr, "Error: Preprocessing failed.\n");
        free(pre_source);
        zym_freeSourceMap(compile_vm, source_map);
        zym_freeVM(compile_vm);
        return 0;
    }

#if DEBUG_SHOW
    printf("Loading modules...\n");
#endif
    ModuleLoadResult* module_result = loadModules(compile_vm, processed_source, source_map, source_file, readAndPreprocessCallback, compile_vm, use_debug_names, false, NULL);

    if (module_result->has_error) {
        fprintf(stderr, "Error: Module loading failed: %s\n", module_result->error_message);
        zym_freeProcessedSource(compile_vm, processed_source);
        free(pre_source);
        freeModuleLoadResult(compile_vm, module_result);
        zym_freeSourceMap(compile_vm, source_map);
        zym_freeVM(compile_vm);
        return 0;
    }

    size_t combined_len = strlen(module_result->combined_source);
    *out_combined_source = (char*)malloc(combined_len + 1);
    if (!*out_combined_source) {
        fprintf(stderr, "Error: Could not allocate memory for combined source.\n");
        zym_freeProcessedSource(compile_vm, processed_source);
        free(pre_source);
        freeModuleLoadResult(compile_vm, module_result);
        zym_freeSourceMap(compile_vm, source_map);
        zym_freeVM(compile_vm);
        return 0;
    }
    strcpy(*out_combined_source, module_result->combined_source);

    zym_freeProcessedSource(compile_vm, processed_source);
    free(pre_source);
    freeModuleLoadResult(compile_vm, module_result);
    zym_freeSourceMap(compile_vm, source_map);
    zym_freeVM(compile_vm);

    return 1;
}

static int compile_source_to_bytecode(const char* source_file, char** out_bytecode, size_t* out_size, int include_line_info, ZymAllocator* allocator) {
    char* pre_source = read_file(source_file);
    if (!pre_source) return 0;

    const char* processed_source = NULL;

    ZymVM* compile_vm = zym_newVM(allocator);
    ZymSourceMap* source_map = zym_newSourceMap(compile_vm);
    ZymChunk* compiled_chunk = zym_newChunk(compile_vm);

    setupNatives(compile_vm);

#if DEBUG_SHOW
    printf("Preprocessing source...\n");
#endif
    ZymFileId entry_id = zym_registerSourceFile(compile_vm, source_file, pre_source, strlen(pre_source));
    if (zym_preprocess(compile_vm, pre_source, source_map, entry_id, &processed_source) != ZYM_STATUS_OK) {
        fprintf(stderr, "Error: Preprocessing failed.\n");
        free(pre_source);
        zym_freeChunk(compile_vm, compiled_chunk);
        zym_freeSourceMap(compile_vm, source_map);
        zym_freeVM(compile_vm);
        return 0;
    }

#if DEBUG_SHOW
    printf("Loading modules...\n");
#endif
    // Use debug names based on whether line info is included (!strip mode)
    ModuleLoadResult* module_result = loadModules(compile_vm, processed_source, source_map, source_file, readAndPreprocessCallback, compile_vm, include_line_info, false, NULL);

    if (module_result->has_error) {
        fprintf(stderr, "Error: Module loading failed: %s\n", module_result->error_message);
        zym_freeProcessedSource(compile_vm, processed_source);
        free(pre_source);
        freeModuleLoadResult(compile_vm, module_result);
        zym_freeChunk(compile_vm, compiled_chunk);
        zym_freeSourceMap(compile_vm, source_map);
        zym_freeVM(compile_vm);
        return 0;
    }

#if DEBUG_SHOW
    printf("Compiling...\n");
#endif

    ZymCompilerConfig config = { .include_line_info = (bool)include_line_info };
    const char* entry_file_path = module_result->module_count > 0 ? module_result->module_paths[0] : source_file;

    // In strip mode, use path hash instead of actual path
    char* entry_file_to_use = NULL;
    char hash_buffer[16];
    if (!include_line_info) {
        unsigned int hash = 0;
        const char* c = entry_file_path;
        while (*c) {
            hash = ((hash << 5) + hash) + *c;
            c++;
        }
        snprintf(hash_buffer, sizeof(hash_buffer), "%x", hash);
        entry_file_to_use = hash_buffer;
    } else {
        entry_file_to_use = (char*)entry_file_path;
    }

    if (zym_compile(compile_vm, module_result->combined_source, compiled_chunk, module_result->source_map, entry_file_to_use, config, NULL) != ZYM_STATUS_OK) {
        drain_and_print_diagnostics(compile_vm);
        fprintf(stderr, "Error: Compilation failed.\n");
        zym_freeProcessedSource(compile_vm, processed_source);
        free(pre_source);
        freeModuleLoadResult(compile_vm, module_result);
        zym_freeChunk(compile_vm, compiled_chunk);
        zym_freeSourceMap(compile_vm, source_map);
        zym_freeVM(compile_vm);
        return 0;
    }
    zym_freeProcessedSource(compile_vm, processed_source);
    free(pre_source);
    freeModuleLoadResult(compile_vm, module_result);

#if DEBUG_SHOW
    printf("Serializing bytecode...\n");
#endif
    if (zym_serializeChunk(compile_vm, config, compiled_chunk, out_bytecode, out_size) != ZYM_STATUS_OK) {
        fprintf(stderr, "Error: Serialization failed.\n");
        zym_freeChunk(compile_vm, compiled_chunk);
        zym_freeSourceMap(compile_vm, source_map);
        zym_freeVM(compile_vm);
        return 0;
    }

    zym_freeChunk(compile_vm, compiled_chunk);
    zym_freeSourceMap(compile_vm, source_map);
    zym_freeVM(compile_vm);

    return 1;
}

static int dump_chunk_to_file(ZymChunk* chunk, const char* output_file) {
    int stdout_fd = _dup(_fileno(stdout));
    if (stdout_fd == -1) {
        fprintf(stderr, "Error: Could not duplicate stdout.\n");
        return 0;
    }

    FILE* file = freopen(output_file, "w", stdout);
    if (file == NULL) {
        fprintf(stderr, "Error: Could not redirect output to \"%s\".\n", output_file);
        _close(stdout_fd);
        return 0;
    }

    disassembleChunk(chunk, "chunk");

    fflush(stdout);
    _dup2(stdout_fd, _fileno(stdout));
    _close(stdout_fd);
    clearerr(stdout);

    return 1;
}

static int dump_bytecode(char* bytecode, size_t bytecode_size, const char* output_file, ZymAllocator* allocator) {
    ZymVM* vm = zym_newVM(allocator);
    ZymChunk* chunk = zym_newChunk(vm);

    setupNatives(vm);

    if (zym_deserializeChunk(vm, chunk, bytecode, bytecode_size) != ZYM_STATUS_OK) {
        fprintf(stderr, "Error: Deserialization failed.\n");
        zym_freeChunk(vm, chunk);
        zym_freeVM(vm);
        return 1;
    }

    if (output_file) {
        if (!dump_chunk_to_file(chunk, output_file)) {
            zym_freeChunk(vm, chunk);
            zym_freeVM(vm);
            return 1;
        }
        printf("Disassembly written to: %s\n", output_file);
    } else {
        disassembleChunk(chunk, "chunk");
    }

    zym_freeChunk(vm, chunk);
    zym_freeVM(vm);
    return 0;
}

static int execute_bytecode(char* bytecode, size_t bytecode_size, int script_argc, char** script_argv, const char* program_name, ZymAllocator* allocator) {
    ZymVM* run_vm = zym_newVM(allocator);
    ZymChunk* loaded_chunk = zym_newChunk(run_vm);

    setupNatives(run_vm);

#if DEBUG_SHOW
    printf("Deserializing bytecode...\n");
#endif
    if (zym_deserializeChunk(run_vm, loaded_chunk, bytecode, bytecode_size) != ZYM_STATUS_OK) {
        fprintf(stderr, "Error: Deserialization failed.\n");
        zym_freeChunk(run_vm, loaded_chunk);
        zym_freeVM(run_vm);
        return 1;
    }

#if DEBUG_SHOW
    printf("Executing bytecode...\n");
#endif
    ZymStatus result = zym_runChunk(run_vm, loaded_chunk);
    while (result == ZYM_STATUS_YIELD) {
        result = zym_resume(run_vm);
    }

    if (result != ZYM_STATUS_OK) {
        fprintf(stderr, "Error: Runtime error occurred.\n");
        zym_freeChunk(run_vm, loaded_chunk);
        zym_freeVM(run_vm);
        return 1;
    }

#if DEBUG_SHOW
    printf("Calling main function...\n");
#endif
    ZymValue argv_list = zym_newList(run_vm);
    zym_listAppend(run_vm, argv_list, zym_newString(run_vm, program_name));
    for (int i = 0; i < script_argc; i++) {
        zym_listAppend(run_vm, argv_list, zym_newString(run_vm, script_argv[i]));
    }

    if (zym_hasFunction(run_vm, "main", 1)) {
        ZymStatus call_result = zym_call(run_vm, "main", 1, argv_list);
        while (call_result == ZYM_STATUS_YIELD) {
            call_result = zym_resume(run_vm);
        }
        if (call_result != ZYM_STATUS_OK) {
            fprintf(stderr, "Error: main(argv) function failed.\n");
            zym_freeChunk(run_vm, loaded_chunk);
            zym_freeVM(run_vm);
            return 1;
        }
    }

    zym_freeChunk(run_vm, loaded_chunk);
    zym_freeVM(run_vm);
    return 0;
}

int full_main(int argc, char** argv, ZymAllocator* allocator) {
    if (argc == 1 || (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0))) {
        print_banner();
        return 0;
    }

    if (argc == 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0)) {
        printf("0.3.0\n");
        return 0;
    }

    // Find the "--" delimiter that separates zym flags from script args
    int delimiter_index = -1;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            delimiter_index = i;
            break;
        }
    }

    // Calculate script args (everything after "--")
    int script_argc = 0;
    char** script_argv = NULL;
    if (delimiter_index != -1) {
        script_argc = argc - delimiter_index - 1;
        script_argv = &argv[delimiter_index + 1];
    }

    if (argc == 2 || (argc == 3 && strcmp(argv[2], "--strip") == 0) || (delimiter_index != -1 && delimiter_index <= 3)) {
        const char* input_file = argv[1];
        int strip_mode = 0;

        for (int i = 2; i < argc && (delimiter_index == -1 || i < delimiter_index); i++) {
            if (strcmp(argv[i], "--strip") == 0) {
                strip_mode = 1;
                break;
            }
        }

        if (has_extension(input_file, ".zbc")) {
            printf("Running precompiled bytecode: %s\n", input_file);
            size_t bytecode_size = 0;
            char* bytecode = read_binary_file(input_file, &bytecode_size);
            if (!bytecode) return 1;

            if (!validate_bytecode_magic(bytecode, bytecode_size)) {
                fprintf(stderr, "Error: Invalid bytecode file (bad magic header).\n");
                free(bytecode);
                return 1;
            }

            int result = execute_bytecode(bytecode, bytecode_size, script_argc, script_argv, argv[0], allocator);
            free(bytecode);
            return result;
        }

        if (has_extension(input_file, ".zym")) {
            char* bytecode = NULL;
            size_t bytecode_size = 0;

            int include_line_info = strip_mode ? 0 : 1;
            if (!compile_source_to_bytecode(input_file, &bytecode, &bytecode_size, include_line_info, allocator)) {
                return 1;
            }

            //printf("Compilation successful. Bytecode size: %zu bytes\n", bytecode_size);
            int result = execute_bytecode(bytecode, bytecode_size, script_argc, script_argv, argv[0], allocator);
            free(bytecode);
            return result;
        }

        fprintf(stderr, "Error: File must have .zym or .zbc extension.\n");
        return 1;
    }

    const char* input_file = argv[1];
    const char* dump_output = NULL;
    const char* compile_output = NULL;
    const char* preprocess_output = NULL;
    const char* combined_output = NULL;
    const char* runtime_path = NULL;
    int has_dump = 0;
    int has_compile_output = 0;
    int has_strip = 0;
    int has_preprocess = 0;
    int has_combined = 0;

    int parse_end = (delimiter_index != -1) ? delimiter_index : argc;
    for (int i = 2; i < parse_end; i++) {
        if (strcmp(argv[i], "--dump") == 0) {
            has_dump = 1;
            if (i + 1 < parse_end && argv[i + 1][0] != '-') {
                dump_output = argv[i + 1];
                i++;
            }
        } else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= parse_end) {
                fprintf(stderr, "Error: -o requires an output file path.\n");
                return 1;
            }
            compile_output = argv[i + 1];
            has_compile_output = 1;
            i++;
        } else if (strcmp(argv[i], "--strip") == 0) {
            has_strip = 1;
        } else if (strcmp(argv[i], "--preprocess") == 0) {
            has_preprocess = 1;
            if (i + 1 < parse_end && argv[i + 1][0] != '-') {
                preprocess_output = argv[i + 1];
                i++;
            }
        } else if (strcmp(argv[i], "--combined") == 0) {
            has_combined = 1;
            if (i + 1 < parse_end && argv[i + 1][0] != '-') {
                combined_output = argv[i + 1];
                i++;
            }
        } else if (strcmp(argv[i], "-r") == 0) {
            if (i + 1 >= parse_end) {
                fprintf(stderr, "Error: -r requires a runtime binary path.\n");
                return 1;
            }
            runtime_path = argv[i + 1];
            i++;
        }
    }

#if DEBUG_SHOW
    printf("DEBUG: has_dump=%d, dump_output=%s\n", has_dump, dump_output ? dump_output : "NULL");
    printf("DEBUG: has_compile_output=%d, compile_output=%s\n", has_compile_output, compile_output ? compile_output : "NULL");
    printf("DEBUG: has_strip=%d\n", has_strip);
    printf("DEBUG: has_preprocess=%d, preprocess_output=%s\n", has_preprocess, preprocess_output ? preprocess_output : "NULL");
    printf("DEBUG: has_combined=%d, combined_output=%s\n", has_combined, combined_output ? combined_output : "NULL");
#endif

    int input_is_zym = has_extension(input_file, ".zym");
    int input_is_zbc = has_extension(input_file, ".zbc");

    if (!input_is_zym && !input_is_zbc) {
        fprintf(stderr, "Error: File must have .zym or .zbc extension.\n");
        return 1;
    }

    if (has_preprocess) {
        if (!input_is_zym) {
            fprintf(stderr, "Error: --preprocess only works with .zym input files.\n");
            return 1;
        }

        char* preprocessed_source = NULL;
        if (!preprocess_source(input_file, &preprocessed_source, allocator)) {
            return 1;
        }

        if (preprocess_output) {
            if (!has_extension(preprocess_output, ".zym")) {
                fprintf(stderr, "Error: --preprocess output must have .zym extension.\n");
                free(preprocessed_source);
                return 1;
            }

            FILE* out_file = fopen(preprocess_output, "w");
            if (!out_file) {
                fprintf(stderr, "Error: Could not create file \"%s\".\n", preprocess_output);
                free(preprocessed_source);
                return 1;
            }
            fprintf(out_file, "%s", preprocessed_source);
            fclose(out_file);
            free(preprocessed_source);
            printf("Preprocessed source written to: %s\n", preprocess_output);
        } else {
            printf("%s", preprocessed_source);
            free(preprocessed_source);
        }

        return 0;
    }

    if (has_combined) {
        if (!input_is_zym) {
            fprintf(stderr, "Error: --combined only works with .zym input files.\n");
            return 1;
        }

        char* combined_source = NULL;
        int use_debug_names = !has_strip;
        if (!generate_combined_source(input_file, &combined_source, use_debug_names, allocator)) {
            return 1;
        }

        if (combined_output) {
            if (!has_extension(combined_output, ".zym")) {
                fprintf(stderr, "Error: --combined output must have .zym extension.\n");
                free(combined_source);
                return 1;
            }

            FILE* out_file = fopen(combined_output, "w");
            if (!out_file) {
                fprintf(stderr, "Error: Could not create file \"%s\".\n", combined_output);
                free(combined_source);
                return 1;
            }
            fprintf(out_file, "%s", combined_source);
            fclose(out_file);
            free(combined_source);
            printf("Combined source written to: %s\n", combined_output);
        } else {
            printf("%s", combined_source);
            free(combined_source);
        }

        return 0;
    }

    char* bytecode = NULL;
    size_t bytecode_size = 0;
    int bytecode_allocated = 0;

    if (input_is_zbc) {
        bytecode = read_binary_file(input_file, &bytecode_size);
        if (!bytecode) return 1;

        if (!validate_bytecode_magic(bytecode, bytecode_size)) {
            fprintf(stderr, "Error: Invalid bytecode file (bad magic header).\n");
            free(bytecode);
            return 1;
        }
        bytecode_allocated = 1;
    } else if (input_is_zym) {
        int include_line_info = has_strip ? 0 : 1;
        if (!compile_source_to_bytecode(input_file, &bytecode, &bytecode_size, include_line_info, allocator)) {
            return 1;
        }
        printf("Compilation successful. Bytecode size: %zu bytes\n", bytecode_size);
        bytecode_allocated = 1;
    }

    int final_result = 0;

    if (has_dump) {
        int dump_result = dump_bytecode(bytecode, bytecode_size, dump_output, allocator);
        if (dump_result != 0) {
            final_result = dump_result;
        }
    }

    if (has_compile_output) {
        int output_is_exe = is_exe_output(compile_output);
        int output_is_zbc = has_extension(compile_output, ".zbc");

        if (output_is_exe) {
            printf("Packing bytecode into %s\n", compile_output);
            if (!pack_bytecode_into_exe(bytecode, bytecode_size, compile_output, runtime_path)) {
                if (bytecode_allocated) free(bytecode);
                return 1;
            }
        } else if (output_is_zbc) {
            printf("Writing bytecode to %s\n", compile_output);
            if (!write_binary_file(compile_output, bytecode, bytecode_size)) {
                if (bytecode_allocated) free(bytecode);
                return 1;
            }
            printf("Bytecode written to: %s\n", compile_output);
        } else {
            fprintf(stderr, "Error: Output file must have .exe or .zbc extension.\n");
            if (bytecode_allocated) free(bytecode);
            return 1;
        }
    }

    if (bytecode_allocated) {
        free(bytecode);
    }

    return final_result;
}
