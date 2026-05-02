#pragma once

// `Zym` native module — script-side surface for spinning up nested
// in-process VMs and bridging values between them.
//
// Catalog entry: `Zym` is a regular grantable catalog entry (see
// src/natives/cli_catalog.{hpp,cpp} and future/zym_roadmap.md). The
// root VM has it because it has the full catalog; children only have
// it if their parent explicitly granted `Zym` via `registerCliNative`.
// A child without `Zym` cannot spawn nested VMs at all — `Zym` is
// simply not in scope.
//
// PR 1 (this PR): scaffolding only. `nativeZym_create` registers the
// `Zym` global with a single working method, `Zym.cliNatives()`, that
// returns the calling VM's `available` set in declaration order. This
// exercises the per-VM `ZymCliVmCtx` plumbing end-to-end without yet
// adding any pipeline / VM-creation surface. The remaining methods
// (`newVM`, `STATUS`, etc.) land in PR 2 onward; see roadmap §3.

#include "zym/zym.h"

struct ZymCliVmCtx;

// Creates the `Zym` native module map and binds every method's
// closure context to the supplied `ctx`. Ownership of `ctx`
// transfers here: a finalizer-bearing native context wrapping `ctx`
// is created and shared by every method closure, so VM teardown
// (which drops the closures) fires the finalizer and frees `ctx`.
// Callers must not delete `ctx` themselves after this returns.
ZymValue nativeZym_create(ZymVM* vm, ZymCliVmCtx* ctx);
