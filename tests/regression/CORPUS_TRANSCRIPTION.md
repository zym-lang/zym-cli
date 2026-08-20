# Per-file transcription of the 0.3.x regression corpus (96 files)

> Companion to FIBER_TEST_PLAN.md. Generated 2026-08-20 by exhaustive read of
> every .zym file immediately before the continuation/preemption removal.
> Columns: file | invariant it guarded | fiber-era equivalent.

**Census: 53 cont, 20 preempt, 20 mixed, 3 core.**

The m-series and s0/s1000/s2000/s3000 are machine-generated bisection variants
of one shape (N=300 locals spilling past the 252-register window, capture
mid-body, clobber of M locals with sentinel SENT while parked, resume must
yield 44850); they collapse into a single parameterized fiber test.

## cont (53)

| file | invariant | fiber-era equivalent |
|---|---|---|
| A_control.zym | A single non-nested withPrompt whose body aborts must leave the prompt stack exactly empty afterward; a subsequent popPrompt must error. | Obsolete as prompt bookkeeping; class survives as: a finished/aborted fiber leaves no VM-level registration behind. |
| B_nested.zym | An abort to an OUTER prompt from inside a nested INNER prompt must pop both entries; prompt count returns to zero. | Obsolete; covered by "nested fiber completes/aborts and both fibers' resources are fully reclaimed". |
| C_which_tag.zym | After a nested transfer, the INNER tag must be dead (abort to it errors), pinning WHICH entry was popped. | Obsolete; nearest analog: resuming a dead fiber must raise a defined error. |
| D_ghost_outer.zym | Aborting to a tag whose dynamic extent ended must be a runtime error, not silently accepted by a stale ghost entry. | Resuming a completed/dead fiber (or yielding outside any fiber) must raise a clean error. |
| D_outer_ghost.zym | Same ghost probe fired three frames deep: the ghost must not transfer control or discard live frames. | Invalid yield/resume raised deep in a call chain errors without unwinding/corrupting the stack. |
| E0_resume_ok.zym | Capture through an inner prompt, then resume; resume value arrives at suspension point, extent completes. | Baseline fiber test: create, yield, resume with a value; yield returns it, fiber completes. |
| E_half2.zym | An inner prompt live inside the captured extent must be restored on resume — frames AND delimiter. | Nested fiber parked mid-resume must find its whole context operable after resume. |
| E_inner_lost.zym | Repro twin of E_half2: capture(INNER) after resume must not die "prompt tag not found". | Same as E_half2; one nested-fiber park/resume test covers both. |
| F2_deep_ghost.zym | A ghost entry with frame_index > frame_count must not unwind with a stale stack_base; VM sane under later allocation. | Erroring on a dead fiber whose recorded stack was deeper than current leaves stack_top/frames untouched. |
| F_deep_ghost.zym | Same as F2 with arithmetic padding. | Same as F2_deep_ghost. |
| F_severity.zym | A leaked entry from an earlier nested capture must not contaminate later unrelated prompt use. | Cross-fiber isolation: one fiber's suspension state invisible to unrelated fibers. |
| G_leak.zym | 200 nested aborts (> MAX_PROMPTS=64) must not leak one prompt slot per transfer. | Create/abort 200+ fibers in a loop; no slot or stack leak; flat memory. |
| H_capture_shift_leak.zym | The per-transfer leak must not exist on the capture or shift paths either. | Obsolete per-primitive split; covered by the fiber leak-loop test. |
| H_control_loop.zym | Control: 100 single-prompt captures leak nothing. | Control: 100 create/yield/discard fiber cycles complete. |
| I_abort_loop.zym | 100 nested aborts complete without slot exhaustion. | 100 abnormally-terminated fibers in a loop; completes. |
| I_shift_leak.zym | Shift through a nested prompt must pop both entries. | Obsolete (shift-specific). |
| J_shift_loop.zym | 100 nested shifts with a handler leak nothing. | Obsolete (shift-specific). |
| J_workaround.zym | Documents (in)validity of "caller re-pushes INNER before resume" workaround. | Obsolete; defect class disappears with per-fiber stacks. |
| K_control_capture.zym | Control for capture-path leak: one non-nested capture pops its own entry. | Obsolete; subsumed by fiber leak-loop control. |
| K_workaround.zym | A manually re-pushed prompt at the resume site delimits the WRONG extent; workaround proven unsound. | Obsolete; no analog. |
| REPRO.zym | Combined original repro: ghost acceptance of dead tag + inner prompt not restored on resume. | Superseded by D/E-series equivalents. |
| bug2_control_plain_capture.zym | Preemption-free capture/resume: resume(k, 42) delivers 42 at suspension point; completing resume returns the body's value. | Core fiber semantics: resume delivers its argument as yield's result; completing fiber delivers its return value to the resumer. |
| c3_continuation_spill_snapshot.zym | Captured continuation carries the parallel spill stack: spilled locals snapshotted, re-based on resume, survive clobbering, GC roots while suspended (5 cases incl. nested + shift). | Parked fiber's stack incl. spill region preserved and GC-rooted by construction; park with >252 locals, GC + large frames on other fibers, resume, verify all. |
| c3_continuation_spill_snapshot_forced.zym | Same under ZYM_FORCE_SPILL_AT=8/6. | Same fiber test under the forced-spill knob, or merged into the real-spill test. |
| c4a_tagged_capture_ghost_prompt.zym | A tagged transfer through an inner prompt pops EVERY entry at/above the match: no slot leak over 200 iterations for capture/abort/shift; no stale entry shadows a live prompt with the same tag. | Leak half: 200+ fiber suspend/discard cycles leak nothing. Shadowing half obsolete: transfer always reaches the live resumer, never a dead record. |
| c4b_inner_prompt_lost_on_resume.zym | A prompt live inside the captured extent is restored on resume and popped exactly once on normal return (no mirror ghost), looped past MAX_PROMPTS. | Nested fiber parked/resumed has its inner construct operable after resume and torn down exactly once; loop to catch residue. |
| c7_shift_drain_and_stack_release.zym | (a) shift's unwind drains invalidated resume boundaries; (b) every stack_top-lowering site closes upvalues into and CLEARS the released region (UAF/ASAN target). | (a) obsolete. (b) survives: dead fiber's released stack cleared and un-rooted; kill fibers, churn allocation under ASAN. |
| c8_upvalue_alias_resume.zym | A closure over a local inside the continuation's extent aliases the SAME cell after resume (42/142/142/142). | Closure over a parked fiber's local aliases the same cell after resume; two-way mutation coherence across yield/resume. |
| c8b_upvalue_cell_mutation_while_parked.zym | The escaped closure runs WHILE parked, writing through the sealed cell; resume treats the cell as authoritative; alias stays two-way after, and after final close. | Run the closure while the fiber is parked; write visible inside fiber after resume; binding unified after completion. |
| m10.zym | Bisection M=10: parked 300-local spilling frame resumes to 44850 after a 10-local sentinel-1000 clobber. | All nine m-files collapse into one parameterized clobber-size-sweep fiber test. |
| m50.zym | Same, M=50. | Collapsed into the sweep. |
| m150.zym | Same, M=150. | Collapsed into the sweep. |
| m190.zym | Same, M=190 (brackets the register-window/spill boundary). | Collapsed into the sweep. |
| m191.zym | Same, M=191. | Collapsed into the sweep. |
| m192.zym | Same, M=192. | Collapsed into the sweep. |
| m200.zym | Same, M=200. | Collapsed into the sweep. |
| m250.zym | Same, M=250. | Collapsed into the sweep. |
| m300.zym | Same, M=300 (clobber as large as the victim). | Collapsed into the sweep. |
| r1.zym | Under ZYM_FORCE_SPILL_AT, captured spilled locals survive an intervening spilling clobber call; resume to 78. | Parked-fiber spill survival under the forced-spill knob. |
| r2.zym | Same with identical sentinel clobber values + no-clobber control; any leak shows as an exact sentinel multiple. | Keep the sentinel trick: it fingerprints where a bad value came from. |
| r3.zym | Resuming from a DEEPER frame than the capture site: restored frames get correct spill bases, not stale ones. | Resume a parked fiber from deep inside another fiber's chain; parked state independent of resumer depth. |
| r4.zym | Spilled locals holding heap objects remain GC-reachable while suspended; resume after churn yields intact values. | Parked fiber's spill region is a GC root; churn, resume, verify. |
| r5.zym | Real-spill, NO env knob: 300 locals spill under stock settings, survive sentinel clobber while parked, sum 300. | The stock-settings parked-fiber spill test (must run in the shipping config). |
| s0.zym | Sentinel sweep SENT=0: corruption tracks the clobber's written value. | Collapsed into the sentinel-fingerprint variant. |
| s1000.zym | Sentinel sweep SENT=1000. | Collapsed. |
| s2000.zym | Sentinel sweep SENT=2000. | Collapsed. |
| s3000.zym | Sentinel sweep SENT=3000. | Collapsed. |
| smoke.zym | Minimal capture/resume smoke; isContinuation true; resume returns 3. | Minimal fiber smoke: create, yield, status check, resume returns body's value. |
| t300.zym | Full-size parked frame with in-file controls: plain 44850, capture+clobber+resume 44850, capture+immediate-resume 44850. | Canonical parked-stack isolation test with both controls kept. |
| t_gc.zym | 300 list-valued locals captured; two GC.cycle() while suspended; spot-checked intact after resume. | Force GC cycles while a parked fiber holds only-referenced-from-stack objects; intact on resume. |
| t_selfstomp.zym | After resume, the restored frame calls a large-frame helper; callee must not stomp the resumed frame's spilled locals. | Resumed fiber immediately calls a large-frame function; no overlap (holds by construction; pin anyway). |
| t_type.zym | Clobber writes STRINGS while a number-holding frame is parked; resume returns the number 299, distinguishing slot reuse from value corruption. | String-clobber variant; assert value AND typeof after resume. |
| t_uaf.zym | Heap-object locals captured, then GC + churn + GC while parked; resume finds live objects (ASAN target). | GC+churn+GC while parked fiber holds heap objects; deep-verify under the ASAN build. |

## preempt (20)

| file | invariant | fiber-era equivalent |
|---|---|---|
| a_mechanism.zym | Fire-once failure is per-ENTRY state (stuck in_flight), not the closure. | Mostly obsolete; surviving: guard re-arm state is VM-level and self-healing, verified by repeated trips. |
| a_native_tail.zym | Under @tco aggressive, a recurring callback that TAIL-returns a native keeps firing (~100 hits / 10k iters). | Guard keeps tripping across native tail-call fast paths; bookkeeping released on every frame-pop path. |
| a_native_tail_off.zym | Control: same under @tco off. | Control leg, TCO off. |
| a_script_tail.zym | Control: script-tail callback re-arms fine under aggressive TCO. | Control leg: guard trips across script tail calls. |
| b_shield.zym | Shield bodies tail-returning natives must not leak shield depth. | Obsolete (shields removed); port only if a masking primitive returns. |
| c5_native_tail_frame_cleanup.zym | Consolidated: NATIVE and native-closure tail-return fast paths perform RET's frame cleanup for both PREEMPT and DISABLE_PREEMPT flags. | Guard: every frame-pop path (RET, native tail, native-closure tail) releases guard bookkeeping identically, under @tco aggressive. Shield half obsolete. |
| s0_baseline.zym | Unshielded entry fires during a spin; shield masks; depth rests at 0. | Guard fires during a plain busy loop under the compile flag — the host-guard smoke test. Shield half obsolete. |
| t0.zym | Earlier form of s0_baseline. | Same; obsolete as separate file. |
| t10_default.zym | Default TCO mode: native-tail callback keeps firing; depth 0 after native-tail shield body. | Guard trip count sane under default TCO. |
| t1_baseline.zym | Recurring entry fires many times over 200k iters; remaining()/ids() coherent. | Guard fires proportionally to instruction count; host introspection coherent. |
| t1_native_tail.zym | Bisection: aggressive + native-tail = the failing cell pre-C5. | Merged into the c5-equivalent matrix. |
| t2_native_nontail.zym | Bisection: aggressive + NON-tail native re-arms fine. | Matrix control leg. |
| t3_script_tail.zym | Bisection: aggressive + script-tail re-arms fine. | Matrix control leg. |
| t4_tco_off.zym | Bisection: @tco off + native-tail re-arms fine. | Matrix control leg. |
| t5_two_entries.zym | One entry's masked state must not starve another; request() works. | Obsolete: multiple script entries no longer exist. |
| t6_shield.zym | Shield-depth leak accumulation across body kinds; leaked depth suppresses a live entry. | Obsolete. |
| t6_shield_off.zym | Control under @tco off. | Obsolete. |
| t7_shield_ran.zym | The native-tail shield body actually RAN and depth released. | Obsolete. |
| t8_native_closure.zym | A NATIVE CLOSURE tail-called from callback and shield body: re-arm + depth release. | Matrix leg: guard trips across bound-native-closure tail calls. |
| t9_safe.zym | @tco safe: native-tail callback re-arms; shield releases. | Matrix leg: @tco safe control. |

## mixed (20)

| file | invariant | fiber-era equivalent |
|---|---|---|
| bug2_completing_step.zym | Sliced workload resumed step-by-step produces the body's VALUE on the completing step (bounded steps), never a continuation artifact. | Guard parks the fiber; resume loop drives it; completing resume returns the workload's value, bounded step count. |
| bug2_external_scheduler.zym | Preempt.every slices ordinaryWork(50000); resume-until-done: iterations==50000, resumes==captures, non-continuation result. | FLAGSHIP: guard auto-parks a running fiber; scheduler resumes until dead; counters and checksums exactly intact; parks==resumes. |
| c1_unwind_no_rearm.zym | A non-local exit out of a preempt callback re-arms the shared countdown exactly as RET does (per-primitive, with control). | Guard counter re-arms when its firing parks the fiber; repeated trips across park/resume cycles. |
| c2_capture_negative_shield_depth.zym | Capture truncated at a preempt frame must not miscount stored shield depth; resume never installs negative depth. | Largely obsolete; surviving: per-fiber guard accounting saved at park restores exactly on resume — never negative, counted by frame kind. |
| c2b_resume_clobbers_live_shield.zym | Resume ADJUSTS, not assigns, shield depth: resuming a depth-0 continuation inside a live shield keeps the resumer's masking. | Resume never clobbers the RESUMER's own guard state; per-fiber accounting switches with the stack switch. |
| c6_resume_boundary_snapshot.zym | (A1-A3) capture spanning live resume splices preserves return routing through 3 splices; (A4) completing resume reclaims its stack slice (400 rounds); (A5) external-scheduler shape. | Splices obsolete by construction; survives: return value reaches resumer after arbitrarily many park/resume cycles; hundreds of rounds, zero stack growth. |
| s1_repro.zym | Original C2 repro (print-based). | Superseded by c2/c2b equivalents. |
| s2_control_nopreempt.zym | Control A: same shape without preempt frame keeps depth 0. | Control leg: park without guard involvement. |
| s3_control_shielded.zym | Control B: capture-in-callback with live shield BELOW the prompt; masking holds after resume. | Obsolete (shield-specific). |
| s4_nested.zym | Two NESTED preempt callbacks with capture from the inner: depth sane after resume (no double subtraction). | If the host guard can nest, pin re-entrant-fire-around-park; else drop. |
| s5_overwrite.zym | Assignment-vs-adjustment defect in isolation. | Superseded by c2b equivalent. |
| s6_final.zym | One long-lived entry: shield masks before AND after a capture/resume round trip; depth balanced. | End-to-end: guard behaves identically before and after a park/resume cycle. |
| t1.zym | Early print-based s6_final. | Superseded. |
| t2_capture.zym | After a capture escapes the prompt, the entry keeps firing during later work; request() still forces a fire. | Guard keeps tripping after a guard-triggered park escaped a fiber; force-trip hook still works. |
| t2_control.zym | Control: capture NOT from a preempt frame leaves depth 0; later shield masks. | Control: plain park/resume leaves guard accounting untouched. |
| t3_matrix.zym | Four-cell diagnosis matrix pinning cause to the preempt-frame unwind. | Collapses to the c1-equivalent with a plain-park control. |
| t3_nested.zym | Two DISTINCT entries, B fires inside A's callback and captures; depth sane; both shields mask after. | Folded into the s4 nesting case or dropped. |
| t4_impact.zym | A shielded critical section (a+b==100) never observed torn, before and after a capture/resume round trip. | Obsolete unless an uninterruptible-region primitive exists; then: guard never observes mid-invariant state across park/resume. |
| t4_resume.zym | The "full fiber shape": capture in callback, resume to completion, entry keeps firing, setSlice() re-arms. | Direct predecessor of the core fiber test; slice reconfig is host-side now. |
| t5_shield.zym | Capture out of the MIDDLE of a shield body releases the frame's depth; entry keeps firing. | Obsolete; port only with an uninterruptible-region primitive. |

## core (3) — keep running as-is

| file | invariant |
|---|---|
| c9_local_struct_enum_in_nested_fn.zym | Struct/enum declared in a function visible from nested functions in either source order (schema pre-pass). |
| fmt.zym | print/str format specifiers %n/%s/%v; string + toString(number) concatenation. |
| t_uaf_control.zym | No-continuation control: a 300-local frame holding heap objects survives GC + churn + GC in a plain call — the healthy GC/stack baseline pin. |
