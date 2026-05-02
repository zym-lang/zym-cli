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

ZymValue nativeZym_create(ZymVM* vm);
