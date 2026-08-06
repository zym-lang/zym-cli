# Preemption (embedding side)

The C API for interrupting, bounding, and stopping a VM you are hosting. Declared in `zym/zym.h`.

A host embedding a VM needs one guarantee above all others: **it can always take control back**. Whatever the script does — infinite loop, deep recursion, a critical section it declares itself, allocation without end — the host decides when it stops. This surface is how that guarantee is expressed.

For the script-visible surface, which is deliberately weaker, see `docs/core/preemption.md`.

---

## Conventions

- **Slices are instruction counts.** Deterministic and machine-independent. This is not a timer; a host that needs wall-clock deadlines drives `zym_preemptTrigger` from its own clock.
- **`ZymPreemptId`** is a `uint32_t`. `0` is never valid and is what a full table returns.
- **Ownership.** Everything registered through this API is *host-owned*. Script cannot cancel, retune, or trigger it. The reverse is not true: the host can address script-owned entries by id.
- **Maskability is opt-in.** Host entries are non-maskable by default, so `Preempt.shield(...)` in the script does not suppress them. Pass `ZYM_PREEMPT_MASKABLE` if you want an entry a script may defer.
- **Capacity is a build-time constant**, shared with script. It defaults to **8** (the MCU figure) and is overridden with `-DZYM_PREEMPT_MAX_ENTRIES=N`, the same way `FRAMES_MAX` and `STACK_MAX` are; the `zym` CLI builds with 32. Each slot costs 24 bytes of VM struct. Always read `zym_preemptCapacity()` rather than hard-coding a number — a library compiled against one value and a host assuming another is the failure this avoids.
- **Slices below 1 are clamped to 1**, so an entry always makes forward progress.

---

## Interrupting

```c
typedef uint32_t ZymPreemptId;

#define ZYM_PREEMPT_MASKABLE  (1u << 0)
#define ZYM_PREEMPT_ONESHOT   (1u << 1)

ZymPreemptId zym_preemptRegister(ZymVM* vm, int slice,
                                 ZymValue callback, uint32_t flags);
bool zym_preemptUnregister(ZymVM* vm, ZymPreemptId id);
bool zym_preemptSetSlice(ZymVM* vm, ZymPreemptId id, int slice);
int  zym_preemptRemaining(ZymVM* vm, ZymPreemptId id);
bool zym_preemptTrigger(ZymVM* vm, ZymPreemptId id);
int  zym_preemptCapacity(void);
```

| Function | Returns | Notes |
| --- | --- | --- |
| `zym_preemptRegister(vm, slice, callback, flags)` | `ZymPreemptId` | Registers a host-owned entry. `0` when the table is full, or when `callback` is a closure taking arguments — check it. Pass `zym_newNull()` as `callback` for a watchdog (see below). |
| `zym_preemptUnregister(vm, id)` | `bool` | Removes an entry. `false` if the id is unknown. A host may unregister a script-owned entry. |
| `zym_preemptSetSlice(vm, id, slice)` | `bool` | Sets the interval and **restarts the countdown**: the entry next fires `slice` instructions from now. This is how you give an exhausted entry more budget after an abort. |
| `zym_preemptRemaining(vm, id)` | `int` | Instructions until this entry fires; `-1` if unknown. |
| `zym_preemptTrigger(vm, id)` | `bool` | Fires the entry at the next instruction boundary regardless of its countdown. The hook for wall-clock deadlines, signals, and UI cancel buttons. |
| `zym_preemptCapacity()` | `int` | Total entries per VM. |

### Reserving slots from script

The table is shared, so a script that registers greedily can leave you unable to arm a watchdog or a deadline later. A reserve holds slots back.

```c
bool zym_setHostPreemptReserve(ZymVM* vm, int slots);
int  zym_getHostPreemptReserve(const ZymVM* vm);

int  zym_preemptCount(const ZymVM* vm, bool script_owned_only);
int  zym_preemptScriptCapacity(const ZymVM* vm);   // capacity - reserve
int  zym_preemptScriptAvailable(const ZymVM* vm);
int  zym_preemptIds(const ZymVM* vm, ZymPreemptId* out, int max);
```

Script's ceiling becomes `capacity - reserve`; you stay free to use any slot beyond your reserve if script has not taken it. A reserve is a floor for you and a ceiling for script.

It is expressed as a reserve rather than a script quota deliberately. A quota of 24 is right on a 32-slot build, wrong on an 8-slot MCU build, and stale on a 64-slot one. A reserve of 8 is right on all three without recomputing anything, and "keep me some room" is what you actually meant.

**Settable only before the VM has executed anything.** It returns `false` afterwards. That is not a limitation to work around — it is what lets a script treat its budget as fixed: whatever `Preempt.capacity()` reads at the start of a run is still bindable at the end. A budget that could shrink mid-run for reasons script cannot observe would make every registration a lottery. It also means the reserve can never fail to be satisfied, since at bring-up script holds nothing.

The payoff is late binding without idle cost. Without a reserve, a host that might need a slot later has to register one up front — and a live entry is not free: it joins every rearm calculation and every expiry scan, and a rearming entry fires on its own schedule whether you want it yet or not. With a reserve you bind only when you actually need to, and the slot is guaranteed to be there.

```c
ZymVM* vm = zym_newVM(NULL);
zym_setHostPreemptReserve(vm, 8);      // before anything runs
// ... grant natives, define globals, compile ...
// script may now hold at most capacity - 8, and 8 remain yours
```

### Watchdogs: a NULL callback means abort

Passing `zym_newNull()` as the callback registers a **watchdog**. On expiry the VM does not run anything — it returns `ZYM_STATUS_SUSPENDED` to you with `zym_vmCause()` reading `ZYM_CAUSE_PREEMPT`.

That is the shape to reach for when supervising code you do not trust. A callback-based entry runs *script*, which is something the script can subvert: it could loop inside your callback, throw from it, or arrange for it never to complete. A watchdog gives it nothing to work with.

```c
// Non-maskable so a script shield cannot defer it; rearming so each
// zym_resume grants another slice.
ZymPreemptId wd = zym_preemptRegister(vm, 1000000, zym_newNull(), 0);
if (wd == 0) { /* table full */ }
```

---

## Stopping

```c
void zym_requestStop(ZymVM* vm);
void zym_clearStop(ZymVM* vm);
bool zym_stopRequested(const ZymVM* vm);
bool zym_isAborting(const ZymVM* vm);
```

| Function | Notes |
| --- | --- |
| `zym_requestStop(vm)` | Stops the VM at its next instruction. Unmaskable and sticky. |
| `zym_clearStop(vm)` | Clears it. Required before the VM can run again. |
| `zym_stopRequested(vm)` | Whether a stop is pending. |
| `zym_isAborting(vm)` | Same condition, read from inside a native that wants to bail out early. |

A stop outranks everything. It is checked before any masking, so a shield, an in-flight preempt callback, or an empty preemption table cannot suppress it, and the VM never clears it on your behalf.

`stop_requested` is declared for cross-context writes, so `zym_requestStop` is safe to call from a **signal handler, an ISR, or another thread** while the VM runs. The rest of this API is not: register, unregister, and retune from the thread that owns the VM.

---

## Bounding memory

```c
void   zym_setMemoryLimit(ZymVM* vm, size_t bytes);
size_t zym_getMemoryLimit(const ZymVM* vm);
size_t zym_memoryUsed(const ZymVM* vm);
bool   zym_oomPending(const ZymVM* vm);
void   zym_clearOom(ZymVM* vm);
```

A watchdog bounds how long a script runs; this bounds how much it allocates. `0` means unlimited, which is the default.

Crossing the ceiling does **not** fail the allocation. The request is satisfied — the allocator still has memory — and the VM is then suspended at the next instruction boundary with `ZYM_STATUS_SUSPENDED` and cause `ZYM_CAUSE_MEMORY_LIMIT`. Failing it instead would strand every caller inside the VM that assumes allocation succeeds, and would leave you nothing to recover. Overshoot is therefore bounded by one allocation rather than zero.

A collection runs before the ceiling is declared crossed, so a script that merely produces garbage is never charged for it. Only what it retains counts.

The condition is sticky, like a stop. `zym_setMemoryLimit` above current usage retires it automatically, so raising the limit is a single call; use `zym_clearOom` when you want to release the VM without granting more room.

This bounds the script. The allocator genuinely running out is a different condition, handled separately below.

### When the allocator itself fails

A ceiling breach is recoverable because memory *is* available and you merely declined to hand more over: the allocation succeeds and the VM suspends. A genuine allocation failure cannot work that way, because `reallocate` has to return usable memory to callers that assume success.

So it unwinds instead. The VM leaves the operation entirely and the call returns `ZYM_STATUS_RUNTIME_ERROR` (or `ZYM_STATUS_COMPILE_ERROR` from `zym_compile`) with `zym_vmCause()` reading `ZYM_CAUSE_OUT_OF_MEMORY`, in state `ZYM_STATE_FAILED` rather than `SUSPENDED`. That distinction is deliberate: the frames were abandoned mid-operation, so the VM is not continuable. Free it.

The unwind lands at the **nearest** API boundary, so a native that re-entered the VM gets a status back and returns normally rather than being jumped over.

One case remains fatal: an allocation failure with no boundary armed, which means outside any VM operation, such as during `zym_newVM` itself. There is nowhere to unwind to, and a host that cannot construct a VM has nothing to recover into.

A compile has no instruction boundary to suspend at, so the memory ceiling reaches it through the frontend's cancellation poll instead: crossing the ceiling mid-compile stops it at the next statement boundary with a compile error, rather than letting it run away unbounded.

---

## Observing and resuming

A suspension does not unwind. Frames, stack, and instruction pointer are all intact, which is what makes `zym_resume` meaningful.

There is one suspended status, not one per reason, because there is one VM state. `zym_vmCause()` says which of `ZYM_CAUSE_PREEMPT`, `ZYM_CAUSE_HOST_STOP`, `ZYM_CAUSE_MEMORY_LIMIT`, or `ZYM_CAUSE_PREEMPT_BLOCKED` put it there, and those call for different responses.

```c
ZymStatus zym_resume(ZymVM* vm);

// Prefer these over a hand-rolled loop.
ZymStatus zym_runToCompletion(ZymVM* vm, ZymChunk* chunk);
ZymStatus zym_callToCompletion(ZymVM* vm, const char* fn, int argc, ZymValue* argv);
```

**Never write `while (s == ZYM_STATUS_SUSPENDED) s = zym_resume(vm);`.** That disarms every watchdog on the VM: it grants a fresh slice forever and the supervision never reaches you. The completion helpers exist so the policy lives in one place — they continue only past `ZYM_CAUSE_PREEMPT_BLOCKED`, where a preempt callback could not be pushed because the call stack was exhausted and the entry has already been rearmed, and hand everything else back.

| Suspended by | To continue |
| --- | --- |
| a rearming watchdog | `zym_resume` — each call grants one fresh slice |
| a watchdog you are finished with | `zym_preemptUnregister` first |
| a watchdog needing a different budget | `zym_preemptSetSlice`, which restarts the countdown |
| `zym_requestStop` | `zym_clearStop` first; sticky by design |
| the memory ceiling | `zym_setMemoryLimit` higher, or `zym_clearOom` |

If more than one condition is pending, all must be cleared. Resuming a VM that is not suspended returns `ZYM_STATUS_RUNTIME_ERROR` rather than executing from a stale position.

`ZYM_STATUS_SUSPENDED` is **not** a runtime error. No diagnostic is pushed and no script-visible handler runs, so the script cannot observe, intercept, or loop inside its own termination. Test it separately from `ZYM_STATUS_RUNTIME_ERROR`: the two mean "I stopped it" and "it failed", and conflating them loses the distinction you most need when reporting back to whoever supplied the code.

**A native that re-enters the VM must propagate `ZYM_STATUS_SUSPENDED`** rather than treating it as an ordinary failure. Swallowing it defeats the stop — the script continues running inside your native's error path.

---

## Example

### Supervising untrusted code, with recovery

```c
ZymVM* vm = zym_newVM(NULL);

ZymChunk* chunk = zym_newChunk(vm);
if (zym_compile(vm, src, chunk, NULL, "untrusted.zym",
                (ZymCompilerConfig){ .include_line_info = 1 }, NULL)
        != ZYM_STATUS_OK) {
    /* compile diagnostics */
}

// Both halves of the resource budget.
ZymPreemptId wd = zym_preemptRegister(vm, 500000, zym_newNull(), 0);
zym_setMemoryLimit(vm, zym_memoryUsed(vm) + (4u << 20));   // +4 MiB

ZymStatus st = zym_runChunk(vm, chunk);

int slices = 0;
while (st == ZYM_STATUS_SUSPENDED) {
    if (zym_oomPending(vm)) {
        fprintf(stderr, "script exceeded its memory budget\n");
        break;                       // no more room: this one is done
    }
    if (++slices > 20) {
        fprintf(stderr, "script exceeded its total instruction budget\n");
        break;
    }
    /* host work between slices */
    st = zym_resume(vm);             // rearming watchdog grants another slice
}

zym_preemptUnregister(vm, wd);
zym_freeChunk(vm, chunk);
zym_freeVM(vm);
```

The `slices > 20` bound is the point of the loop. A rearming watchdog on its own grants budget forever, one slice at a time; the counter is what converts it into a hard total. Without it you have written an unbounded VM the slow way.

Testing the cause inside the loop is what separates "needs more time" from "needs more memory" — every suspension arrives as `ZYM_STATUS_SUSPENDED`, and they call for different decisions. `zym_vmCause()` answers it directly; `zym_oomPending()` is the narrower check used above.

---

### A wall-clock deadline

Preemption counts instructions, so wall-clock bounds are built by driving `zym_preemptTrigger` from your own clock. Register a maskable no-op entry as the vehicle and trigger it when your deadline passes:

```c
// From a timer thread, a signal handler, or a UI cancel button:
zym_requestStop(vm);        // safe cross-context; unmaskable and sticky
```

For a soft deadline the script may defer briefly, register a maskable entry and `zym_preemptTrigger` it instead. For a hard one, use `zym_requestStop` — it is the only path that is safe to call from another context and that nothing in the script can defer.

---

## Notes

- **Authority is the whole design.** Host entries are non-maskable and host-owned by default; script entries are always maskable and script-owned. A script cannot disarm what is supervising it, and a shield it raises never suppresses a host watchdog.
- **Host entries are invisible to script, not just untouchable.** `Preempt.remaining` returns `-1` for an entry script does not own and `Preempt.ids()` lists only its own, so a script cannot map your supervision by probing the id space. Expose a deadline through your own native if you want it seen.
- **One callback per expiry.** When several entries come due on the same instruction, the first by registration order runs; the rest keep refreshed deadlines for a later pass. A callback-less entry always wins over a callback, so a watchdog is honoured before any script runs.
- **An entry is masked while its own callback runs**, so it cannot re-enter itself. Other entries still fire.
- **The dispatch cost is one decrement and one predicted branch**, whether or not any entries exist. All table work happens on expiry, in a cold path.
- **Callbacks must have arity 0.** `zym_preemptRegister` returns `0` for a callback that takes arguments, the same way it reports a full table — the VM has no way to invoke one, so it will not hand back an id for an entry that could never fire.
- **Stop is per-VM.** Stopping a child VM leaves its parent, and every sibling, running.
