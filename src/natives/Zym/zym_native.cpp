// `Zym` native module — see zym_native.hpp for scope.
//
// Lifetime model (mirrors the pre-godot CLI's `VMData`/`zymvm_cleanup`
// pattern in src/natives/ZymVM.c): the per-VM `ZymCliVmCtx*` is owned
// by a single finalizer-bearing `zym_createNativeContext` value
// produced in `nativeZym_create`. Every `Zym.*` method closure shares
// that context, so the ctx pointer travels with the closures. When
// the VM tears down its globals, the closures drop, the shared
// context is GC'd, and the finalizer below frees the ctx (unless the
// ctx is marked `externalOwner` — see `ZymCliVmCtx::externalOwner`,
// used for child VMs whose ctx is owned by the parent's
// `ChildVmHandle`). There is no process-wide bookkeeping.
//
// PR 2 adds the nested-VM surface: `Zym.newVM()` and the setup-phase
// methods on the returned ChildVM struct. The ChildVM closures live
// in the *parent* VM but operate on the *child* VM. Their context
// is a `ChildVmHandle` (parent-allocated) that holds:
//   - the child `ZymVM*` pointer,
//   - the child's `ZymCliVmCtx*` (externalOwner=true; freed by the
//     handle finalizer, not by the child's Zym finalizer),
//   - the parent's `ZymCliVmCtx*` (borrowed; used for capability
//     resolution in registerCliNative / "ALL"),
//   - a `freed` flag so an explicit `vm.free()` and the finalizer
//     don't both call `zym_freeVM`.

#include "zym_native.hpp"

#include "../cli_catalog.hpp"
#include "../natives.hpp"
#include "../../bridge/cross_vm.hpp"
#include "zym/module_loader.h"

#include "zym/diagnostics.h"
#include "zym/sourcemap.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// =============================================================================
// Status code mirror exposed to scripts as `Zym.STATUS` (roadmap §0 → Errors).
// Numeric values mirror the C `ZymStatus` enum so a script can branch on the
// code returned from compile / runChunk / call / deserializeChunk just like
// `full_executor.cpp` does.
// =============================================================================
struct StatusEntry { const char* name; int value; };
const StatusEntry kStatusEntries[] = {
    { "OK",             ZYM_STATUS_OK },
    { "COMPILE_ERROR",  ZYM_STATUS_COMPILE_ERROR },
    { "RUNTIME_ERROR",  ZYM_STATUS_RUNTIME_ERROR },
    { "YIELD",          ZYM_STATUS_YIELD },
};

// =============================================================================
// CTX FINALIZER (shared by parent's Zym module and any child that gets Zym
// installed via `cli_catalog_install_named`)
// =============================================================================

void zym_ctx_finalizer(ZymVM* /*vm*/, void* p) {
    auto* ctx = static_cast<ZymCliVmCtx*>(p);
    if (!ctx) return;
    if (ctx->externalOwner) return;  // owned elsewhere; see ZymCliVmCtx
    delete ctx;
}

// =============================================================================
// ChildVmHandle — parent-side state backing a `ChildVM` script value
// =============================================================================
//
// Allocated by `Zym.newVM()` in the parent VM. Bound as the userdata
// of one finalizer-bearing native context value shared by every
// ChildVM method closure (`registerCliNative`, `defineGlobal`, ...).
// When the parent VM tears down its globals, the ChildVM map drops,
// the shared context is GC'd, the finalizer below fires, and:
//   1. if `freed == false`, calls `zym_freeVM(child)` (which fires
//      any finalizers in the child, including the child-side Zym
//      ctx finalizer if Zym was granted — that one is a no-op because
//      `externalOwner == true`),
//   2. deletes `childCtx`,
//   3. deletes the handle itself.
//
// `parentCtx` is a borrowed pointer into the parent's own ctx; it is
// used only during normal method dispatch (capability resolution),
// never during finalization.

struct ChildVmHandle {
    ZymVM* child = nullptr;
    ZymCliVmCtx* childCtx = nullptr;
    ZymCliVmCtx* parentCtx = nullptr;
    bool freed = false;
    // Refcount for cross-resource ownership. The ChildVM map's native
    // context holds 1 ref; every SourceMap / Chunk wrapper allocated
    // off this VM holds 1 ref; every cached `getFunc` dispatcher holds
    // 1 ref. The handle is destroyed (and the child VM freed if not
    // already) when the count drops to zero.
    int refcount = 0;
    // `getFunc` cache — same name on the same VM returns the same
    // parent-side dispatcher closure every time (identity-stable per
    // user spec). Entries are removed when their dispatcher's
    // finalizer fires (script lost all refs) or when the handle
    // tears down.
    std::unordered_map<std::string, ZymValue> funcCache;
};

void handle_decref(ChildVmHandle* h);

void handle_decref(ChildVmHandle* h) {
    if (!h) return;
    if (--h->refcount > 0) return;
    if (h->child && !h->freed) {
        zym_freeVM(h->child);
        h->freed = true;
    }
    // childCtx is externalOwner=true, so the child's Zym finalizer
    // (if Zym was granted) skipped freeing it. Free it here.
    delete h->childCtx;
    delete h;
}

void child_handle_finalizer(ZymVM* /*parentVm*/, void* p) {
    handle_decref(static_cast<ChildVmHandle*>(p));
}

// =============================================================================
// Resource wrappers — SourceMap and Chunk values exposed to scripts.
//
// Each is a parent-VM map carrying an opaque native context whose userdata
// is one of the structs below. The resource holds a refcount on its owning
// `ChildVmHandle` so it survives `vm` going out of scope script-side, and the
// finalizer releases the underlying `ZymSourceMap*` / `ZymChunk*` if the
// child VM is still alive — otherwise the child VM teardown already collected
// it. `freed` short-circuits double-free when the script calls `.free()`
// explicitly before GC.
// =============================================================================

struct SourceMapRes {
    ChildVmHandle* h = nullptr;
    ZymSourceMap* sm = nullptr;
    bool freed = false;
};

void source_map_finalizer(ZymVM* /*parentVm*/, void* p) {
    auto* r = static_cast<SourceMapRes*>(p);
    if (!r) return;
    if (!r->freed && r->h && !r->h->freed && r->sm) {
        zym_freeSourceMap(r->h->child, r->sm);
    }
    handle_decref(r->h);
    delete r;
}

struct ChunkRes {
    ChildVmHandle* h = nullptr;
    ZymChunk* chunk = nullptr;
    bool freed = false;
};

void chunk_finalizer(ZymVM* /*parentVm*/, void* p) {
    auto* r = static_cast<ChunkRes*>(p);
    if (!r) return;
    if (!r->freed && r->h && !r->h->freed && r->chunk) {
        zym_freeChunk(r->h->child, r->chunk);
    }
    handle_decref(r->h);
    delete r;
}

// =============================================================================
// Helpers
// =============================================================================

// Returns true if `set` contains `name` (string equality).
bool set_has(const std::vector<std::string>& set, const char* name) {
    if (!name) return false;
    for (const auto& s : set) {
        if (s == name) return true;
    }
    return false;
}

// Capability-and-phase guard for ChildVM methods. Returns nullptr
// (after raising a runtime error) if the call is invalid; otherwise
// returns the handle. `setupOnly` enforces `phase == Setup`.
//
// All "the call cannot proceed" cases collapse to a single
// "no such native" runtime error (roadmap §0 → Errors): the script
// must not be able to tell whether the method was called on a freed
// VM, on a post-freeze VM, or just doesn't exist. From the script's
// perspective, the capability is simply absent.
ChildVmHandle* require_child(ZymVM* vm, ZymValue context, bool setupOnly) {
    auto* h = static_cast<ChildVmHandle*>(zym_getNativeData(context));
    if (!h || h->freed || !h->child || !h->childCtx) {
        zym_runtimeError(vm, "no such native");
        return nullptr;
    }
    if (setupOnly && h->childCtx->phase != ZymVmPhase::Setup) {
        zym_runtimeError(vm, "no such native");
        return nullptr;
    }
    return h;
}

// Resolves a single grant name against the *parent's* capability
// set (never the global catalog) and installs it on the child.
// Returns false (after raising) if the name is unknown or withheld.
// Idempotent: re-granting an already-installed name is a silent
// no-op (cli_catalog_install_named handles this internally).
bool grant_one(ZymVM* parentVm, ChildVmHandle* h, const char* name) {
    if (!name) {
        zym_runtimeError(parentVm, "no such native");
        return false;
    }
    if (!set_has(h->parentCtx->available, name)) {
        // Unknown OR withheld: indistinguishable to the script.
        zym_runtimeError(parentVm, "no such native: %s", name);
        return false;
    }
    cli_catalog_install_named(h->child, h->childCtx, name);
    return true;
}

// =============================================================================
// Zym.* methods (top-level singleton, parent-VM closures)
// =============================================================================

ZymValue z_cliNatives(ZymVM* vm, ZymValue context) {
    auto* cli = static_cast<ZymCliVmCtx*>(zym_getNativeData(context));
    ZymValue list = zym_newList(vm);
    if (!cli) return list;
    for (const auto& name : cli->available) {
        zym_listAppend(vm, list, zym_newString(vm, name.c_str()));
    }
    return list;
}

// forward decl: defined below the ChildVM section so it can see the
// methods table.
ZymValue make_child_vm(ZymVM* parentVm, ChildVmHandle* handle);

ZymValue z_newVM(ZymVM* parentVm, ZymValue context) {
    auto* parentCtx = static_cast<ZymCliVmCtx*>(zym_getNativeData(context));
    if (!parentCtx) {
        zym_runtimeError(parentVm, "no such native");
        return ZYM_ERROR;
    }

    // Allocator is inherited (roadmap §0). Scripts cannot select one.
    const ZymAllocator* alloc = zym_getAllocator(parentVm);
    ZymVM* child = zym_newVM(const_cast<ZymAllocator*>(alloc));

    // Buffer is the only auto-installed native (roadmap §0).
    cli_catalog_install_auto(child);

    auto* childCtx = new ZymCliVmCtx();
    childCtx->vm = child;
    childCtx->externalOwner = true;  // owned by handle below

    auto* handle = new ChildVmHandle();
    handle->child = child;
    handle->childCtx = childCtx;
    handle->parentCtx = parentCtx;
    handle->refcount = 1;  // owned by the ChildVM map below

    return make_child_vm(parentVm, handle);
}

// =============================================================================
// Resource constructors (parent-VM maps)
// =============================================================================
//
// `make_source_map` and `make_chunk` mint a parent-VM map with an opaque
// native context wrapping the corresponding child resource. Each one bumps
// the handle's refcount so the resource can outlive the ChildVM script value.

ZymValue make_source_map(ZymVM* parentVm, ChildVmHandle* h, ZymSourceMap* sm);
ZymValue make_chunk     (ZymVM* parentVm, ChildVmHandle* h, ZymChunk* c);

// Forward decl: also defined below.
ChildVmHandle* require_child(ZymVM* vm, ZymValue context, bool setupOnly);

// =============================================================================
// Pipeline-resource accessors
// =============================================================================

SourceMapRes* unwrap_source_map(ZymVM* parentVm, ZymValue v) {
    if (!zym_isMap(v)) return nullptr;
    ZymValue ctx = zym_mapGet(parentVm, v, "__sm__");
    if (ctx == ZYM_ERROR) return nullptr;
    return static_cast<SourceMapRes*>(zym_getNativeData(ctx));
}

ChunkRes* unwrap_chunk(ZymVM* parentVm, ZymValue v) {
    if (!zym_isMap(v)) return nullptr;
    ZymValue ctx = zym_mapGet(parentVm, v, "__chunk__");
    if (ctx == ZYM_ERROR) return nullptr;
    return static_cast<ChunkRes*>(zym_getNativeData(ctx));
}

// Verify a resource is alive AND belongs to this VM. Mismatches collapse
// to "no such native" (roadmap §0 → Errors): the script must not be able
// to tell whether the resource is from a sibling VM, freed, or simply
// the wrong type.
SourceMapRes* require_source_map(ZymVM* parentVm, ChildVmHandle* h, ZymValue v) {
    auto* r = unwrap_source_map(parentVm, v);
    if (!r || r->freed || !r->sm || r->h != h || r->h->freed) {
        zym_runtimeError(parentVm, "no such native");
        return nullptr;
    }
    return r;
}

ChunkRes* require_chunk(ZymVM* parentVm, ChildVmHandle* h, ZymValue v) {
    auto* r = unwrap_chunk(parentVm, v);
    if (!r || r->freed || !r->chunk || r->h != h || r->h->freed) {
        zym_runtimeError(parentVm, "no such native");
        return nullptr;
    }
    return r;
}

// Auto-loop on YIELD (matches `execute_bytecode` in full_executor.cpp).
ZymStatus run_to_completion_chunk(ZymVM* child, ZymChunk* chunk) {
    ZymStatus s = zym_runChunk(child, chunk);
    while (s == ZYM_STATUS_YIELD) s = zym_resume(child);
    return s;
}

ZymStatus call_to_completion(ZymVM* child, const char* name, int argc, ZymValue* argv) {
    ZymStatus s = zym_callv(child, name, argc, argv);
    while (s == ZYM_STATUS_YIELD) s = zym_resume(child);
    return s;
}

// =============================================================================
// Pipeline methods (closures live in parent VM, operate on child VM)
// =============================================================================
// All mutate the child's source registry / chunk / VM state. Methods that
// trigger the setup→execution freeze do so explicitly via `freeze`.

void freeze(ChildVmHandle* h) {
    if (h && h->childCtx && h->childCtx->phase == ZymVmPhase::Setup) {
        h->childCtx->phase = ZymVmPhase::Execution;
    }
}

ZymValue cv_newSourceMap(ZymVM* parentVm, ZymValue context) {
    auto* h = require_child(parentVm, context, /*setupOnly*/ false);
    if (!h) return ZYM_ERROR;
    ZymSourceMap* sm = zym_newSourceMap(h->child);
    if (!sm) { zym_runtimeError(parentVm, "no such native"); return ZYM_ERROR; }
    return make_source_map(parentVm, h, sm);
}

ZymValue cv_newChunk(ZymVM* parentVm, ZymValue context) {
    auto* h = require_child(parentVm, context, /*setupOnly*/ false);
    if (!h) return ZYM_ERROR;
    ZymChunk* c = zym_newChunk(h->child);
    if (!c) { zym_runtimeError(parentVm, "no such native"); return ZYM_ERROR; }
    return make_chunk(parentVm, h, c);
}

ZymValue cv_registerSourceFile(ZymVM* parentVm, ZymValue context,
                               ZymValue pathV, ZymValue srcV) {
    auto* h = require_child(parentVm, context, /*setupOnly*/ false);
    if (!h) return ZYM_ERROR;
    if (!zym_isString(pathV) || !zym_isString(srcV)) {
        zym_runtimeError(parentVm,
            "registerSourceFile(path, source) expects two strings");
        return ZYM_ERROR;
    }
    const char* path = zym_asCString(pathV);
    const char* src  = zym_asCString(srcV);
    ZymFileId id = zym_registerSourceFile(h->child, path, src, std::strlen(src));
    return zym_newNumber((double)id);
}

ZymValue cv_preprocess(ZymVM* parentVm, ZymValue context,
                       ZymValue srcV, ZymValue smV, ZymValue idV) {
    auto* h = require_child(parentVm, context, /*setupOnly*/ false);
    if (!h) return ZYM_ERROR;
    if (!zym_isString(srcV) || !zym_isNumber(idV)) {
        zym_runtimeError(parentVm,
            "preprocess(source, sourceMap, fileId): bad arg types");
        return ZYM_ERROR;
    }
    auto* smr = require_source_map(parentVm, h, smV);
    if (!smr) return ZYM_ERROR;

    const char* src = zym_asCString(srcV);
    ZymFileId fid   = (ZymFileId)zym_asNumber(idV);
    const char* out = nullptr;
    ZymStatus st = zym_preprocess(h->child, src, smr->sm, fid, &out);

    ZymValue result = zym_newMap(parentVm);
    zym_pushRoot(parentVm, result);
    zym_mapSet(parentVm, result, "status", zym_newNumber((double)st));
    if (st == ZYM_STATUS_OK && out) {
        zym_mapSet(parentVm, result, "source", zym_newString(parentVm, out));
        zym_freeProcessedSource(h->child, out);
    } else {
        zym_mapSet(parentVm, result, "source", zym_newNull());
    }
    zym_popRoot(parentVm);
    return result;
}

ZymValue cv_compile(ZymVM* parentVm, ZymValue context,
                    ZymValue srcV, ZymValue chunkV, ZymValue smV,
                    ZymValue entryV, ZymValue optsV) {
    auto* h = require_child(parentVm, context, /*setupOnly*/ false);
    if (!h) return ZYM_ERROR;
    if (!zym_isString(srcV) || !zym_isString(entryV)) {
        zym_runtimeError(parentVm,
            "compile(source, chunk, sourceMap, entryFile, opts): bad arg types");
        return ZYM_ERROR;
    }
    auto* cr = require_chunk(parentVm, h, chunkV);
    if (!cr) return ZYM_ERROR;
    // sourceMap may be null/omitted when compiling raw text.
    ZymSourceMap* sm = nullptr;
    if (!zym_isNull(smV)) {
        auto* smr = require_source_map(parentVm, h, smV);
        if (!smr) return ZYM_ERROR;
        sm = smr->sm;
    }

    ZymCompilerConfig config = { /*include_line_info=*/ true };
    if (zym_isMap(optsV)) {
        ZymValue v = zym_mapGet(parentVm, optsV, "includeLineInfo");
        if (v != ZYM_ERROR && zym_isBool(v)) config.include_line_info = zym_asBool(v);
    }

    const char* src   = zym_asCString(srcV);
    const char* entry = zym_asCString(entryV);
    ZymStatus st = zym_compile(h->child, src, cr->chunk, sm, entry, config, nullptr);
    freeze(h);
    return zym_newNumber((double)st);
}

ZymValue cv_serializeChunk(ZymVM* parentVm, ZymValue context,
                           ZymValue chunkV, ZymValue optsV) {
    auto* h = require_child(parentVm, context, /*setupOnly*/ false);
    if (!h) return ZYM_ERROR;
    auto* cr = require_chunk(parentVm, h, chunkV);
    if (!cr) return ZYM_ERROR;

    ZymCompilerConfig config = { /*include_line_info=*/ true };
    if (zym_isMap(optsV)) {
        ZymValue v = zym_mapGet(parentVm, optsV, "includeLineInfo");
        if (v != ZYM_ERROR && zym_isBool(v)) config.include_line_info = zym_asBool(v);
    }

    char* buf = nullptr;
    size_t size = 0;
    ZymStatus st = zym_serializeChunk(h->child, config, cr->chunk, &buf, &size);
    if (st != ZYM_STATUS_OK || !buf) {
        if (buf) std::free(buf);
        ZymValue result = zym_newMap(parentVm);
        zym_pushRoot(parentVm, result);
        zym_mapSet(parentVm, result, "status", zym_newNumber((double)st));
        zym_mapSet(parentVm, result, "bytes",  zym_newNull());
        zym_popRoot(parentVm);
        return result;
    }
    ZymValue bytes = makeBufferFromBytes(parentVm, buf, size);
    std::free(buf);
    zym_pushRoot(parentVm, bytes);
    ZymValue result = zym_newMap(parentVm);
    zym_pushRoot(parentVm, result);
    zym_mapSet(parentVm, result, "status", zym_newNumber((double)ZYM_STATUS_OK));
    zym_mapSet(parentVm, result, "bytes",  bytes);
    zym_popRoot(parentVm);
    zym_popRoot(parentVm);
    return result;
}

ZymValue cv_deserializeChunk(ZymVM* parentVm, ZymValue context,
                             ZymValue chunkV, ZymValue bufV) {
    auto* h = require_child(parentVm, context, /*setupOnly*/ false);
    if (!h) return ZYM_ERROR;
    auto* cr = require_chunk(parentVm, h, chunkV);
    if (!cr) return ZYM_ERROR;
    const char* data = nullptr;
    size_t size = 0;
    if (!readBufferBytes(parentVm, bufV, &data, &size)) {
        zym_runtimeError(parentVm,
            "deserializeChunk(chunk, bytes) expects a Buffer");
        return ZYM_ERROR;
    }
    ZymStatus st = zym_deserializeChunk(h->child, cr->chunk, data, size);
    freeze(h);
    return zym_newNumber((double)st);
}

ZymValue cv_runChunk(ZymVM* parentVm, ZymValue context, ZymValue chunkV) {
    auto* h = require_child(parentVm, context, /*setupOnly*/ false);
    if (!h) return ZYM_ERROR;
    auto* cr = require_chunk(parentVm, h, chunkV);
    if (!cr) return ZYM_ERROR;
    ZymStatus st = run_to_completion_chunk(h->child, cr->chunk);
    freeze(h);
    return zym_newNumber((double)st);
}

// Forward decls — defined further down (alongside marshal_to_child).
bool marshal_to_parent(ZymVM* child, ZymVM* parentVm, ZymValue v, ZymValue* out);

// -----------------------------------------------------------------------------
// vm.run(srcOrBuffer) and vm.runBytecode(bytecodeBuffer) — convenience helpers
// that compress the standard
//   newSourceMap → registerSourceFile → newChunk → compile → runChunk
// (or newChunk → deserializeChunk → runChunk) sequence into a single call.
// Return shape: { status, result } where `status` is a `Zym.STATUS` code and
// `result` is the value returned by the chunk's top-level (or `null`).
// -----------------------------------------------------------------------------

ZymValue cv_run(ZymVM* parentVm, ZymValue context, ZymValue srcV) {
    auto* h = require_child(parentVm, context, /*setupOnly*/ false);
    if (!h) return ZYM_ERROR;

    // Accept either a string or a Buffer carrying utf-8 source bytes.
    const char* src = nullptr;
    std::string srcStorage;
    if (zym_isString(srcV)) {
        src = zym_asCString(srcV);
    } else {
        const char* data = nullptr;
        size_t size = 0;
        if (!readBufferBytes(parentVm, srcV, &data, &size)) {
            zym_runtimeError(parentVm,
                "run(source) expects a string or a Buffer of source bytes");
            return ZYM_ERROR;
        }
        srcStorage.assign(data, size);
        src = srcStorage.c_str();
    }

    ZymSourceMap* sm = zym_newSourceMap(h->child);
    ZymChunk*     ch = zym_newChunk(h->child);
    if (!sm || !ch) {
        if (sm) zym_freeSourceMap(h->child, sm);
        if (ch) zym_freeChunk(h->child, ch);
        zym_runtimeError(parentVm, "no such native");
        return ZYM_ERROR;
    }
    ZymFileId fid = zym_registerSourceFile(h->child, "<inline>",
                                           src, std::strlen(src));

    // Run the preprocessor first so directives (e.g. imports/macros) are
    // expanded before compilation, matching the manual pipeline.
    const char* processed = nullptr;
    ZymStatus st = zym_preprocess(h->child, src, sm, fid, &processed);
    const char* compileSrc = (st == ZYM_STATUS_OK && processed) ? processed : src;

    ZymCompilerConfig config = { /*include_line_info=*/ true };
    if (st == ZYM_STATUS_OK) {
        st = zym_compile(h->child, compileSrc, ch, sm, "<inline>", config, nullptr);
    }
    freeze(h);

    ZymValue result = zym_newMap(parentVm);
    zym_pushRoot(parentVm, result);
    if (st == ZYM_STATUS_OK) {
        st = run_to_completion_chunk(h->child, ch);
    }
    zym_mapSet(parentVm, result, "status", zym_newNumber((double)st));

    ZymValue marshalled = zym_newNull();
    if (st == ZYM_STATUS_OK) {
        ZymValue r = zym_getCallResult(h->child);
        if (r != ZYM_ERROR) {
            ZymValue tmp;
            if (marshal_to_parent(h->child, parentVm, r, &tmp)) {
                marshalled = tmp;
            }
        }
    }
    zym_mapSet(parentVm, result, "result", marshalled);

    if (processed) zym_freeProcessedSource(h->child, processed);
    zym_freeChunk(h->child, ch);
    zym_freeSourceMap(h->child, sm);
    zym_popRoot(parentVm);
    return result;
}

ZymValue cv_runBytecode(ZymVM* parentVm, ZymValue context, ZymValue bufV) {
    auto* h = require_child(parentVm, context, /*setupOnly*/ false);
    if (!h) return ZYM_ERROR;

    const char* data = nullptr;
    size_t size = 0;
    if (!readBufferBytes(parentVm, bufV, &data, &size)) {
        zym_runtimeError(parentVm,
            "runBytecode(bytes) expects a Buffer of bytecode");
        return ZYM_ERROR;
    }

    ZymChunk* ch = zym_newChunk(h->child);
    if (!ch) {
        zym_runtimeError(parentVm, "no such native");
        return ZYM_ERROR;
    }

    ZymStatus st = zym_deserializeChunk(h->child, ch, data, size);
    freeze(h);

    ZymValue result = zym_newMap(parentVm);
    zym_pushRoot(parentVm, result);
    if (st == ZYM_STATUS_OK) {
        st = run_to_completion_chunk(h->child, ch);
    }
    zym_mapSet(parentVm, result, "status", zym_newNumber((double)st));

    ZymValue marshalled = zym_newNull();
    if (st == ZYM_STATUS_OK) {
        ZymValue r = zym_getCallResult(h->child);
        if (r != ZYM_ERROR) {
            ZymValue tmp;
            if (marshal_to_parent(h->child, parentVm, r, &tmp)) {
                marshalled = tmp;
            }
        }
    }
    zym_mapSet(parentVm, result, "result", marshalled);

    zym_freeChunk(h->child, ch);
    zym_popRoot(parentVm);
    return result;
}

ZymValue cv_hasFunction(ZymVM* parentVm, ZymValue context,
                        ZymValue nameV, ZymValue arityV) {
    auto* h = require_child(parentVm, context, /*setupOnly*/ false);
    if (!h) return ZYM_ERROR;
    if (!zym_isString(nameV) || !zym_isNumber(arityV)) {
        zym_runtimeError(parentVm, "hasFunction(name, arity): bad arg types");
        return ZYM_ERROR;
    }
    return zym_newBool(zym_hasFunction(h->child,
                                       zym_asCString(nameV),
                                       (int)zym_asNumber(arityV)));
}

// hasFunc(name) — true if any callable named `name` exists at any arity
//   (fixed or variadic). Delegates to `zym_hasAnyFunction`.
// hasFunc(name, arity) — true if a callable named `name` is dispatchable
//   with exactly `arity` args. Returns true for either an exact fixed-arity
//   match (`name@arity`) or a variadic acceptance (`name@vF` with
//   `arity >= F`). Delegates to `zym_canCallWith`.
//
// Variadic on the script side so the second arg is optional.
ZymValue cv_hasFunc(ZymVM* parentVm, ZymValue context,
                    ZymValue nameV, ZymValue* vargs, int vargc) {
    auto* h = require_child(parentVm, context, /*setupOnly*/ false);
    if (!h) return ZYM_ERROR;
    if (!zym_isString(nameV)) {
        zym_runtimeError(parentVm, "hasFunc(name [, arity]): name must be a string");
        return ZYM_ERROR;
    }
    if (vargc > 1) {
        zym_runtimeError(parentVm, "hasFunc: too many arguments (expected name [, arity])");
        return ZYM_ERROR;
    }
    const char* name = zym_asCString(nameV);

    if (vargc == 1) {
        if (!zym_isNumber(vargs[0])) {
            zym_runtimeError(parentVm, "hasFunc(name, arity): arity must be a number");
            return ZYM_ERROR;
        }
        return zym_newBool(zym_canCallWith(h->child, name, (int)zym_asNumber(vargs[0])));
    }
    return zym_newBool(zym_hasAnyFunction(h->child, name));
}

// Marshal a single primitive across VMs. PR 3 keeps this primitives-only,
// matching `defineGlobal`. Lists/maps/structs/Buffer/callables land in PR 4.
// Cross-VM value marshaller — full graph copy with identity tracking,
// Buffer byte-copy, and closure trampoline wrapping. Backed by
// `src/bridge/cross_vm.{hpp,cpp}` (mirror of zym-js).
bool marshal_to_child(ZymVM* parentVm, ZymVM* child, ZymValue v, ZymValue* out) {
    return zym_bridge::marshal(parentVm, child, v, out);
}

bool marshal_to_parent(ZymVM* child, ZymVM* parentVm, ZymValue v, ZymValue* out) {
    return zym_bridge::marshal(child, parentVm, v, out);
}

ZymValue cv_call(ZymVM* parentVm, ZymValue context,
                 ZymValue nameV, ZymValue argsV) {
    auto* h = require_child(parentVm, context, /*setupOnly*/ false);
    if (!h) return ZYM_ERROR;
    if (!zym_isString(nameV)) {
        zym_runtimeError(parentVm, "call(name, args): name must be a string");
        return ZYM_ERROR;
    }
    if (!zym_isList(argsV)) {
        zym_runtimeError(parentVm, "call(name, args): args must be a list");
        return ZYM_ERROR;
    }
    int n = zym_listLength(argsV);
    std::vector<ZymValue> argv;
    argv.reserve((size_t)n);
    for (int i = 0; i < n; i++) {
        ZymValue parentArg = zym_listGet(parentVm, argsV, i);
        ZymValue childArg;
        if (!marshal_to_child(parentVm, h->child, parentArg, &childArg)) {
            return ZYM_ERROR;
        }
        argv.push_back(childArg);
    }
    ZymStatus st = call_to_completion(h->child, zym_asCString(nameV), n,
                                      argv.empty() ? nullptr : argv.data());
    freeze(h);
    return zym_newNumber((double)st);
}

// Variadic positional-args sibling to `cv_call`. Mirrors `zym_callv` in
// the C API: `vm.callv(name, a, b, c)` is the same call as
// `vm.call(name, [a, b, c])`, just spelled with positional args at the
// call site rather than an explicit list. Both write to the same
// backing slot, so `vm.callResult()` reads the result of whichever
// was used most recently.
ZymValue cv_callv(ZymVM* parentVm, ZymValue context,
                  ZymValue nameV, ZymValue* vargs, int vargc) {
    auto* h = require_child(parentVm, context, /*setupOnly*/ false);
    if (!h) return ZYM_ERROR;
    if (!zym_isString(nameV)) {
        zym_runtimeError(parentVm, "callv(name, ...): name must be a string");
        return ZYM_ERROR;
    }
    std::vector<ZymValue> argv;
    argv.reserve((size_t)vargc);
    for (int i = 0; i < vargc; i++) {
        ZymValue childArg;
        if (!marshal_to_child(parentVm, h->child, vargs[i], &childArg)) {
            return ZYM_ERROR;
        }
        argv.push_back(childArg);
    }
    ZymStatus st = call_to_completion(h->child, zym_asCString(nameV), vargc,
                                      argv.empty() ? nullptr : argv.data());
    freeze(h);
    return zym_newNumber((double)st);
}

ZymValue cv_callResult(ZymVM* parentVm, ZymValue context) {
    auto* h = require_child(parentVm, context, /*setupOnly*/ false);
    if (!h) return ZYM_ERROR;
    ZymValue childResult = zym_getCallResult(h->child);
    if (childResult == ZYM_ERROR) return zym_newNull();
    ZymValue parentResult;
    if (!marshal_to_parent(h->child, parentVm, childResult, &parentResult)) {
        return ZYM_ERROR;
    }
    return parentResult;
}

// =============================================================================
// getFunc — parent-side dispatcher closure mirroring a child function set
// =============================================================================
//
// `vm.getFunc(name)` returns a parent-side variadic native closure that
// forwards into `zym_callv(child, name, ...)` and marshals the result
// back. The wrapper covers the entire function set for that name on the
// child (every fixed overload + any variadic), with overload resolution
// performed on the child via the normal `zym_call_prepare` walk.
//
// Identity is stable per name on the same VM: the wrapper is cached on
// the handle and the same `ZymValue` is returned every time.

struct GetFuncCtx {
    ChildVmHandle* h = nullptr;
    std::string name;
};

void getfunc_finalizer(ZymVM* /*parentVm*/, void* p) {
    auto* g = static_cast<GetFuncCtx*>(p);
    if (!g) return;
    if (g->h) {
        // Cache key still owns its (now-being-freed) ZymValue entry; the
        // cache map sits inside `funcCache` on the handle. Erase it so
        // a subsequent `getFunc(name)` rebuilds a fresh wrapper.
        g->h->funcCache.erase(g->name);
        handle_decref(g->h);
    }
    delete g;
}

// Variadic native body for the dispatcher closure. Marshals every
// positional arg from parent → child, forwards the call, marshals the
// result back. Triggers the setup→execution freeze (matches `cv_call`).
// Errors during the call (non-OK status, marshalling failure, etc.)
// surface as a parent-side runtime error with the canonical
// "no such native" message — same shape `cv.call` already uses.
ZymValue cv_invoke_dispatcher(ZymVM* parentVm, ZymValue context,
                              ZymValue* vargs, int vargc) {
    auto* g = static_cast<GetFuncCtx*>(zym_getNativeData(context));
    if (!g || !g->h || g->h->freed || !g->h->child) {
        zym_runtimeError(parentVm, "no such native");
        return ZYM_ERROR;
    }
    ChildVmHandle* h = g->h;

    std::vector<ZymValue> argv;
    argv.reserve((size_t)vargc);
    for (int i = 0; i < vargc; i++) {
        ZymValue childArg;
        if (!marshal_to_child(parentVm, h->child, vargs[i], &childArg)) {
            return ZYM_ERROR;
        }
        argv.push_back(childArg);
    }
    ZymStatus st = call_to_completion(h->child, g->name.c_str(), vargc,
                                      argv.empty() ? nullptr : argv.data());
    freeze(h);
    if (st != ZYM_STATUS_OK) {
        zym_runtimeError(parentVm, "no such native");
        return ZYM_ERROR;
    }
    ZymValue childResult = zym_getCallResult(h->child);
    if (childResult == ZYM_ERROR) return zym_newNull();
    ZymValue parentResult;
    if (!marshal_to_parent(h->child, parentVm, childResult, &parentResult)) {
        return ZYM_ERROR;
    }
    return parentResult;
}

ZymValue cv_getFunc(ZymVM* parentVm, ZymValue context, ZymValue nameV) {
    auto* h = require_child(parentVm, context, /*setupOnly*/ false);
    if (!h) return ZYM_ERROR;
    if (!zym_isString(nameV)) {
        zym_runtimeError(parentVm, "getFunc(name): name must be a string");
        return ZYM_ERROR;
    }
    const char* name = zym_asCString(nameV);
    if (!zym_hasAnyFunction(h->child, name)) {
        return zym_newNull();
    }
    // Cached? Return the same value every time for identity stability.
    auto it = h->funcCache.find(name);
    if (it != h->funcCache.end()) {
        return it->second;
    }
    auto* g = new GetFuncCtx{ h, std::string(name) };
    ZymValue ctx = zym_createNativeContext(parentVm, g, getfunc_finalizer);
    zym_pushRoot(parentVm, ctx);
    // Use a generic signature — the child does real arity resolution per
    // call via `zym_call_prepare`. The signature's display name doesn't
    // need to match `name`; what matters is that it's variadic with zero
    // fixed prefix so the wrapper accepts any argc.
    ZymValue closure = zym_createNativeClosureVariadic(
        parentVm, "getFunc(...)", (void*)cv_invoke_dispatcher, ctx);
    zym_popRoot(parentVm);
    h->refcount++;  // one ref held by the cached dispatcher closure
    h->funcCache.emplace(std::string(name), closure);
    return closure;
}

const char* severity_name(ZymDiagSeverity s) {
    switch (s) {
        case ZYM_DIAG_ERROR:   return "error";
        case ZYM_DIAG_WARNING: return "warning";
        case ZYM_DIAG_INFO:    return "info";
        case ZYM_DIAG_HINT:    return "hint";
    }
    return "info";
}

ZymValue cv_diagnostics(ZymVM* parentVm, ZymValue context) {
    auto* h = require_child(parentVm, context, /*setupOnly*/ false);
    if (!h) return ZYM_ERROR;

    size_t count = 0;
    const ZymDiagnostic* diags = zymGetDiagnostics(h->child, &count);

    ZymValue list = zym_newList(parentVm);
    zym_pushRoot(parentVm, list);
    for (size_t i = 0; i < count; i++) {
        const ZymDiagnostic* d = &diags[i];
        ZymValue m = zym_newMap(parentVm);
        zym_pushRoot(parentVm, m);
        zym_mapSet(parentVm, m, "severity",  zym_newString(parentVm, severity_name(d->severity)));
        zym_mapSet(parentVm, m, "fileId",    zym_newNumber((double)d->fileId));
        zym_mapSet(parentVm, m, "startByte", zym_newNumber((double)d->startByte));
        zym_mapSet(parentVm, m, "length",    zym_newNumber((double)d->length));
        zym_mapSet(parentVm, m, "line",      zym_newNumber((double)d->line));
        zym_mapSet(parentVm, m, "column",    zym_newNumber((double)d->column));
        zym_mapSet(parentVm, m, "message",
                   d->message ? zym_newString(parentVm, d->message) : zym_newNull());
        // Resolve the file path/contents from the child registry while the
        // child is still alive, so the parent can render diagnostics without
        // poking into child memory after the fact.
        ZymSourceFileInfo info{};
        if (d->fileId != ZYM_FILE_ID_INVALID &&
            zym_getSourceFile(h->child, d->fileId, &info)) {
            zym_mapSet(parentVm, m, "file",
                       info.path ? zym_newString(parentVm, info.path) : zym_newNull());
        } else {
            zym_mapSet(parentVm, m, "file", zym_newNull());
        }
        zym_listAppend(parentVm, list, m);
        zym_popRoot(parentVm);
    }
    zymClearDiagnostics(h->child);
    zym_popRoot(parentVm);
    return list;
}

// =============================================================================
// Resource methods (sm.free / chunk.free)
// =============================================================================

ZymValue sm_free(ZymVM* /*parentVm*/, ZymValue context) {
    auto* r = static_cast<SourceMapRes*>(zym_getNativeData(context));
    if (!r || r->freed || !r->sm) return zym_newBool(false);
    if (r->h && !r->h->freed) zym_freeSourceMap(r->h->child, r->sm);
    r->freed = true;
    r->sm = nullptr;
    return zym_newBool(true);
}

ZymValue chunk_free(ZymVM* /*parentVm*/, ZymValue context) {
    auto* r = static_cast<ChunkRes*>(zym_getNativeData(context));
    if (!r || r->freed || !r->chunk) return zym_newBool(false);
    if (r->h && !r->h->freed) zym_freeChunk(r->h->child, r->chunk);
    r->freed = true;
    r->chunk = nullptr;
    return zym_newBool(true);
}

ZymValue cv_freeChunk(ZymVM* parentVm, ZymValue /*context*/, ZymValue chunkV) {
    auto* r = unwrap_chunk(parentVm, chunkV);
    if (!r || r->freed || !r->chunk) return zym_newBool(false);
    if (r->h && !r->h->freed) zym_freeChunk(r->h->child, r->chunk);
    r->freed = true;
    r->chunk = nullptr;
    return zym_newBool(true);
}

ZymValue make_source_map(ZymVM* parentVm, ChildVmHandle* h, ZymSourceMap* sm) {
    auto* r = new SourceMapRes{ h, sm, false };
    h->refcount++;
    ZymValue ctx = zym_createNativeContext(parentVm, r, source_map_finalizer);
    zym_pushRoot(parentVm, ctx);
    ZymValue mFree = zym_createNativeClosure(parentVm, "free()", (void*)sm_free, ctx);
    zym_pushRoot(parentVm, mFree);
    ZymValue obj = zym_newMap(parentVm);
    zym_pushRoot(parentVm, obj);
    zym_mapSet(parentVm, obj, "__sm__", ctx);
    zym_mapSet(parentVm, obj, "free",   mFree);
    for (int i = 0; i < 3; i++) zym_popRoot(parentVm);
    return obj;
}

ZymValue make_chunk(ZymVM* parentVm, ChildVmHandle* h, ZymChunk* c) {
    auto* r = new ChunkRes{ h, c, false };
    h->refcount++;
    ZymValue ctx = zym_createNativeContext(parentVm, r, chunk_finalizer);
    zym_pushRoot(parentVm, ctx);
    ZymValue mFree = zym_createNativeClosure(parentVm, "free()", (void*)chunk_free, ctx);
    zym_pushRoot(parentVm, mFree);
    ZymValue obj = zym_newMap(parentVm);
    zym_pushRoot(parentVm, obj);
    zym_mapSet(parentVm, obj, "__chunk__", ctx);
    zym_mapSet(parentVm, obj, "free",      mFree);
    for (int i = 0; i < 3; i++) zym_popRoot(parentVm);
    return obj;
}

// =============================================================================
// ChildVM.* methods (closures live in parent VM, operate on child VM)
// =============================================================================

ZymValue cv_cliNatives(ZymVM* parentVm, ZymValue context) {
    ZymValue list = zym_newList(parentVm);
    auto* h = require_child(parentVm, context, /*setupOnly*/ false);
    if (!h) return list;  // require_child already raised
    for (const auto& n : h->childCtx->available) {
        zym_listAppend(parentVm, list, zym_newString(parentVm, n.c_str()));
    }
    return list;
}

ZymValue cv_registerCliNative(ZymVM* parentVm, ZymValue context, ZymValue arg) {
    auto* h = require_child(parentVm, context, /*setupOnly*/ true);
    if (!h) return ZYM_ERROR;

    if (zym_isString(arg)) {
        const char* s = zym_asCString(arg);
        if (s && std::strcmp(s, "ALL") == 0) {
            // Grant every name the parent itself has. Order matches
            // the parent's `available` (declaration order).
            // Snapshot first because grant_one mutates childCtx, not
            // parentCtx — but defensive copy avoids surprises.
            std::vector<std::string> snapshot = h->parentCtx->available;
            for (const auto& n : snapshot) {
                cli_catalog_install_named(h->child, h->childCtx, n.c_str());
            }
            return zym_newBool(true);
        }
        if (!grant_one(parentVm, h, s)) return ZYM_ERROR;
        return zym_newBool(true);
    }

    if (zym_isList(arg)) {
        int n = zym_listLength(arg);
        for (int i = 0; i < n; i++) {
            ZymValue v = zym_listGet(parentVm, arg, i);
            if (!zym_isString(v)) {
                zym_runtimeError(parentVm,
                    "registerCliNative([...]) expects a list of strings");
                return ZYM_ERROR;
            }
            if (!grant_one(parentVm, h, zym_asCString(v))) return ZYM_ERROR;
        }
        return zym_newBool(true);
    }

    zym_runtimeError(parentVm,
        "registerCliNative(arg) expects a string, list of strings, or \"ALL\"");
    return ZYM_ERROR;
}

ZymValue cv_defineGlobal(ZymVM* parentVm, ZymValue context,
                         ZymValue nameV, ZymValue value) {
    auto* h = require_child(parentVm, context, /*setupOnly*/ true);
    if (!h) return ZYM_ERROR;

    if (!zym_isString(nameV)) {
        zym_runtimeError(parentVm, "defineGlobal(name, value) expects a string name");
        return ZYM_ERROR;
    }
    const char* name = zym_asCString(nameV);

    // Cross-VM marshal: primitives, strings, lists, maps, structs, enums,
    // Buffers (byte-copy), and closures (wrapped as variadic native closures
    // on the child). See `src/bridge/cross_vm.{hpp,cpp}`.
    ZymValue marshalled;
    if (!marshal_to_child(parentVm, h->child, value, &marshalled)) {
        return ZYM_ERROR;
    }

    zym_defineGlobal(h->child, name, marshalled);
    return zym_newBool(true);
}

// =============================================================================
// PR 4: registerNative — bind a parent closure as a child native
// =============================================================================

ZymValue cv_registerNative(ZymVM* parentVm, ZymValue context,
                           ZymValue sigV, ZymValue fnV) {
    auto* h = require_child(parentVm, context, /*setupOnly*/ true);
    if (!h) return ZYM_ERROR;
    if (!zym_isString(sigV)) {
        zym_runtimeError(parentVm,
            "registerNative(signature, fn) expects a string signature");
        return ZYM_ERROR;
    }
    if (!zym_isClosure(fnV) && !zym_isFunction(fnV)) {
        zym_runtimeError(parentVm,
            "registerNative(signature, fn) expects a closure as the second arg");
        return ZYM_ERROR;
    }
    if (!zym_bridge::register_cross_native(
            parentVm, h->child, zym_asCString(sigV), fnV)) {
        return ZYM_ERROR;
    }
    return zym_newBool(true);
}

// =============================================================================
// PR 4: loadModules — multi-file compile with parent-closure read callback
// =============================================================================

struct LoadModulesCtx {
    ZymVM* parentVm;
    ZymVM* child;
    ZymValue parentCallback;  // parent-side closure
};

static ModuleReadResult zym_loadModules_trampoline(const char* path, void* user_data) {
    ModuleReadResult result = { nullptr, nullptr, ZYM_FILE_ID_INVALID };
    auto* ctx = static_cast<LoadModulesCtx*>(user_data);
    if (!ctx || !ctx->parentVm || !ctx->child) return result;

    // Call parent closure with the path string. Parent sees a parent-VM
    // string; we marshal nothing back here — the script callback's job is
    // to return either a map { source, sourceMap, fileId } or null.
    ZymValue pathV = zym_newString(ctx->parentVm, path ? path : "");
    ZymValue argv[1] = { pathV };
    ZymStatus st = zym_callClosurev(ctx->parentVm, ctx->parentCallback, 1, argv);
    if (st != ZYM_STATUS_OK) return result;

    ZymValue ret = zym_getCallResult(ctx->parentVm);
    if (zym_isNull(ret)) return result;
    if (!zym_isMap(ret)) return result;

    // Pull `source` (string), `sourceMap` (parent-side wrapper), `fileId`
    // (number) from the returned map.
    ZymValue srcV = zym_mapGet(ctx->parentVm, ret, "source");
    if (srcV == ZYM_ERROR || zym_isNull(srcV) || !zym_isString(srcV)) return result;
    ZymValue smV  = zym_mapGet(ctx->parentVm, ret, "sourceMap");
    ZymValue fidV = zym_mapGet(ctx->parentVm, ret, "fileId");

    const char* src = zym_asCString(srcV);
    if (!src) return result;
    // loadModules takes ownership of `source` (free'd in freeModuleLoadResult);
    // duplicate into the heap so we don't hand out a VM-managed pointer.
    size_t n = strlen(src);
    char* heap_src = (char*)std::malloc(n + 1);
    if (!heap_src) return result;
    std::memcpy(heap_src, src, n + 1);

    ZymSourceMap* smPtr = nullptr;
    if (smV != ZYM_ERROR && !zym_isNull(smV)) {
        auto* smr = unwrap_source_map(ctx->parentVm, smV);
        if (smr && !smr->freed) smPtr = smr->sm;
    }
    ZymFileId fid = ZYM_FILE_ID_INVALID;
    if (fidV != ZYM_ERROR && zym_isNumber(fidV)) {
        fid = (ZymFileId)(int)zym_asNumber(fidV);
    }

    result.source     = heap_src;
    result.source_map = smPtr;
    result.file_id    = fid;
    return result;
}

ZymValue cv_loadModules(ZymVM* parentVm, ZymValue context,
                        ZymValue srcV, ZymValue smV, ZymValue entryV,
                        ZymValue cbV, ZymValue optsV) {
    auto* h = require_child(parentVm, context, /*setupOnly*/ false);
    if (!h) return ZYM_ERROR;
    if (!zym_isString(srcV) || !zym_isString(entryV)) {
        zym_runtimeError(parentVm,
            "loadModules(source, sourceMap, entryFile, callback, opts): bad arg types");
        return ZYM_ERROR;
    }
    if (!zym_isClosure(cbV) && !zym_isFunction(cbV)) {
        zym_runtimeError(parentVm,
            "loadModules(source, sourceMap, entryFile, callback, opts): callback must be a closure");
        return ZYM_ERROR;
    }
    auto* smr = require_source_map(parentVm, h, smV);
    if (!smr) return ZYM_ERROR;

    bool debug_names = true;
    bool write_debug = false;
    if (zym_isMap(optsV)) {
        ZymValue v;
        v = zym_mapGet(parentVm, optsV, "debugNames");
        if (v != ZYM_ERROR && zym_isBool(v)) debug_names = zym_asBool(v);
        v = zym_mapGet(parentVm, optsV, "writeDebugOutput");
        if (v != ZYM_ERROR && zym_isBool(v)) write_debug = zym_asBool(v);
    }

    LoadModulesCtx ctx { parentVm, h->child, cbV };
    zym_pushRoot(parentVm, cbV);
    ModuleLoadResult* mr = loadModules(
        h->child, zym_asCString(srcV), smr->sm, zym_asCString(entryV),
        zym_loadModules_trampoline, &ctx,
        debug_names, write_debug, nullptr);
    zym_popRoot(parentVm);

    ZymValue result = zym_newMap(parentVm);
    zym_pushRoot(parentVm, result);
    if (!mr || mr->has_error) {
        zym_mapSet(parentVm, result, "status", zym_newNumber((double)ZYM_STATUS_COMPILE_ERROR));
        zym_mapSet(parentVm, result, "error",
            zym_newString(parentVm, (mr && mr->error_message) ? mr->error_message : "loadModules failed"));
    } else {
        zym_mapSet(parentVm, result, "status", zym_newNumber((double)ZYM_STATUS_OK));
        zym_mapSet(parentVm, result, "combinedSource",
            zym_newString(parentVm, mr->combined_source ? mr->combined_source : ""));
        ZymValue paths = zym_newList(parentVm);
        zym_pushRoot(parentVm, paths);
        for (int i = 0; i < mr->module_count; i++) {
            zym_listAppend(parentVm, paths,
                zym_newString(parentVm, mr->module_paths[i] ? mr->module_paths[i] : ""));
        }
        zym_mapSet(parentVm, result, "modulePaths", paths);
        zym_popRoot(parentVm);
    }
    if (mr) freeModuleLoadResult(h->child, mr);
    zym_popRoot(parentVm);
    return result;
}

ZymValue cv_free(ZymVM* /*parentVm*/, ZymValue context) {
    auto* h = static_cast<ChildVmHandle*>(zym_getNativeData(context));
    if (!h) return zym_newBool(false);
    if (h->freed || !h->child) return zym_newBool(false);
    zym_freeVM(h->child);
    h->freed = true;
    h->child = nullptr;
    // Do NOT delete childCtx here — the parent's native context
    // finalizer will do that on parent VM teardown. Subsequent
    // ChildVM method calls hit `require_child` which sees `freed`
    // and raises "no such native".
    return zym_newBool(true);
}

// =============================================================================
// ChildVM struct assembly
// =============================================================================

ZymValue make_child_vm(ZymVM* parentVm, ChildVmHandle* handle) {
    ZymValue ctx = zym_createNativeContext(parentVm, handle, child_handle_finalizer);
    zym_pushRoot(parentVm, ctx);

#define MK_CLOSURE(sig, fn) zym_createNativeClosure(parentVm, sig, (void*)fn, ctx)

    // Setup-phase methods
    ZymValue mRegister = MK_CLOSURE("registerCliNative(arg)", cv_registerCliNative);            zym_pushRoot(parentVm, mRegister);
    ZymValue mDefine   = MK_CLOSURE("defineGlobal(name, value)", cv_defineGlobal);              zym_pushRoot(parentVm, mDefine);
    ZymValue mRegN     = MK_CLOSURE("registerNative(signature, fn)", cv_registerNative);        zym_pushRoot(parentVm, mRegN);

    // Phase-neutral / introspection
    ZymValue mCli      = MK_CLOSURE("cliNatives()", cv_cliNatives);                              zym_pushRoot(parentVm, mCli);
    ZymValue mFree     = MK_CLOSURE("free()", cv_free);                                          zym_pushRoot(parentVm, mFree);

    // Pipeline (allowed in either phase; mutating ones flip phase to Execution).
    ZymValue mNewSm    = MK_CLOSURE("newSourceMap()", cv_newSourceMap);                          zym_pushRoot(parentVm, mNewSm);
    ZymValue mNewCh    = MK_CLOSURE("newChunk()", cv_newChunk);                                  zym_pushRoot(parentVm, mNewCh);
    ZymValue mRegSrc   = MK_CLOSURE("registerSourceFile(path, source)", cv_registerSourceFile);  zym_pushRoot(parentVm, mRegSrc);
    ZymValue mPre      = MK_CLOSURE("preprocess(source, sourceMap, fileId)", cv_preprocess);     zym_pushRoot(parentVm, mPre);
    ZymValue mLoadMod  = MK_CLOSURE("loadModules(source, sourceMap, entryFile, callback, opts)", cv_loadModules); zym_pushRoot(parentVm, mLoadMod);
    ZymValue mComp     = MK_CLOSURE("compile(source, chunk, sourceMap, entryFile, opts)", cv_compile); zym_pushRoot(parentVm, mComp);
    ZymValue mSer      = MK_CLOSURE("serializeChunk(chunk, opts)", cv_serializeChunk);           zym_pushRoot(parentVm, mSer);
    ZymValue mDes      = MK_CLOSURE("deserializeChunk(chunk, bytes)", cv_deserializeChunk);      zym_pushRoot(parentVm, mDes);
    ZymValue mRun      = MK_CLOSURE("runChunk(chunk)", cv_runChunk);                             zym_pushRoot(parentVm, mRun);
    ZymValue mRunSrc   = MK_CLOSURE("run(source)", cv_run);                                      zym_pushRoot(parentVm, mRunSrc);
    ZymValue mRunBc    = MK_CLOSURE("runBytecode(bytes)", cv_runBytecode);                       zym_pushRoot(parentVm, mRunBc);
    ZymValue mCall     = MK_CLOSURE("call(name, args)", cv_call);                                zym_pushRoot(parentVm, mCall);
    ZymValue mCallV    = zym_createNativeClosureVariadic(parentVm,
                            "callv(name, ...)", (void*)cv_callv, ctx);                            zym_pushRoot(parentVm, mCallV);
    ZymValue mCallR    = MK_CLOSURE("callResult()", cv_callResult);                              zym_pushRoot(parentVm, mCallR);
    ZymValue mHas      = MK_CLOSURE("hasFunction(name, arity)", cv_hasFunction);                 zym_pushRoot(parentVm, mHas);
    ZymValue mHasFunc  = zym_createNativeClosureVariadic(parentVm,
                            "hasFunc(name, ...)", (void*)cv_hasFunc, ctx);                       zym_pushRoot(parentVm, mHasFunc);
    ZymValue mGetFunc  = MK_CLOSURE("getFunc(name)", cv_getFunc);                                zym_pushRoot(parentVm, mGetFunc);
    ZymValue mDiag     = MK_CLOSURE("diagnostics()", cv_diagnostics);                            zym_pushRoot(parentVm, mDiag);
    ZymValue mFreeCh   = MK_CLOSURE("freeChunk(chunk)", cv_freeChunk);                           zym_pushRoot(parentVm, mFreeCh);

#undef MK_CLOSURE

    ZymValue obj = zym_newMap(parentVm);
    zym_pushRoot(parentVm, obj);

    zym_mapSet(parentVm, obj, "registerCliNative",  mRegister);
    zym_mapSet(parentVm, obj, "defineGlobal",       mDefine);
    zym_mapSet(parentVm, obj, "registerNative",     mRegN);
    zym_mapSet(parentVm, obj, "cliNatives",         mCli);
    zym_mapSet(parentVm, obj, "free",               mFree);
    zym_mapSet(parentVm, obj, "newSourceMap",       mNewSm);
    zym_mapSet(parentVm, obj, "newChunk",           mNewCh);
    zym_mapSet(parentVm, obj, "registerSourceFile", mRegSrc);
    zym_mapSet(parentVm, obj, "preprocess",         mPre);
    zym_mapSet(parentVm, obj, "loadModules",        mLoadMod);
    zym_mapSet(parentVm, obj, "compile",            mComp);
    zym_mapSet(parentVm, obj, "serializeChunk",     mSer);
    zym_mapSet(parentVm, obj, "deserializeChunk",   mDes);
    zym_mapSet(parentVm, obj, "runChunk",           mRun);
    zym_mapSet(parentVm, obj, "run",                mRunSrc);
    zym_mapSet(parentVm, obj, "runBytecode",        mRunBc);
    zym_mapSet(parentVm, obj, "call",               mCall);
    zym_mapSet(parentVm, obj, "callv",              mCallV);
    zym_mapSet(parentVm, obj, "callResult",         mCallR);
    zym_mapSet(parentVm, obj, "hasFunction",        mHas);
    zym_mapSet(parentVm, obj, "hasFunc",            mHasFunc);
    zym_mapSet(parentVm, obj, "getFunc",            mGetFunc);
    zym_mapSet(parentVm, obj, "diagnostics",        mDiag);
    zym_mapSet(parentVm, obj, "freeChunk",          mFreeCh);

    // ctx + 24 closures + obj = 26
    for (int i = 0; i < 26; i++) zym_popRoot(parentVm);
    return obj;
}

} // namespace

ZymValue nativeZym_create(ZymVM* vm, ZymCliVmCtx* ctx) {
    // The shared context wraps `ctx` with the finalizer that owns
    // its lifetime (unless `ctx->externalOwner` says otherwise — see
    // `zym_ctx_finalizer`). Every Zym.* method closure binds to this
    // exact value, so all methods see the same ctx and the ctx is
    // freed exactly once when all closures are gone.
    ZymValue sharedCtx = zym_createNativeContext(vm, ctx, zym_ctx_finalizer);
    zym_pushRoot(vm, sharedCtx);

    ZymValue mCli = zym_createNativeClosure(
        vm, "cliNatives()", (void*)z_cliNatives, sharedCtx);
    zym_pushRoot(vm, mCli);

    ZymValue mNew = zym_createNativeClosure(
        vm, "newVM()", (void*)z_newVM, sharedCtx);
    zym_pushRoot(vm, mNew);

    // Status code mirror — `Zym.STATUS.OK`, `.COMPILE_ERROR`, etc.
    ZymValue status = zym_newMap(vm);
    zym_pushRoot(vm, status);
    for (const auto& e : kStatusEntries) {
        zym_mapSet(vm, status, e.name, zym_newNumber((double)e.value));
    }

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

    zym_mapSet(vm, obj, "cliNatives", mCli);
    zym_mapSet(vm, obj, "newVM",      mNew);
    zym_mapSet(vm, obj, "STATUS",     status);

    // sharedCtx + cliNatives + newVM + status + obj = 5
    for (int i = 0; i < 5; i++) zym_popRoot(vm);
    return obj;
}
