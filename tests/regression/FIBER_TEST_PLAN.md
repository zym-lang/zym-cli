# Fiber-era test plan — transcribed from the 0.3.x continuation/preemption corpus

> Written 2026-08-20, immediately before the continuation + script-preemption
> removal for 0.4.0. The 96 test FILES of the old corpus are archived; the
> INVARIANTS they encode are transcribed here so the fiber implementation
> inherits them as its acceptance suite. Derived by exhaustive read of every
> file in tests/regression/ at the point of removal.

**Corpus census: 53 cont, 20 preempt, 20 mixed, 3 core (96 total).**

The corpus was heavily layered: many files were print-based diagnostic probes
later superseded by asserted `cN_*.zym` consolidations (c1 supersedes
t2_capture/t3_matrix/t4_resume/t5_shield; c2/c2b supersede the s1-s6 and
t1/t2_control family; c5 supersedes the a_*/t*_tail/t6-t9 TCO matrix; c4a/c4b
supersede the capital-letter A-K/REPRO family; c3 plus the m/r/s/t300
generated families are one invariant). The fiber plan needs roughly 15
asserted tests, not 96 files.

## Invariant classes that MUST survive into the fiber suite

1. **Parked-stack integrity** (was c3, m/r/s series, t300, t_gc, t_type,
   t_uaf, t_selfstomp): a parked fiber's stack — including its spill region —
   is preserved and GC-rooted. Park a fiber with >252 locals (real spill, no
   env knob, per r5) holding numbers, strings, and lists; run clobber frames
   of swept sizes and sentinel values on other fibers; force GC.cycle twice;
   resume and verify every value AND its typeof. Include the resumed-fiber-
   calls-large-frame case (t_selfstomp) and the ASAN build run (t_uaf).
2. **Upvalue cell identity across park/resume** (was c8/c8b): a closure over
   a parked fiber's local aliases the SAME cell after resume; run the escaped
   closure WHILE parked, assert two-way mutation coherence after resume, and
   again after the fiber completes and the upvalue closes.
3. **Return-value delivery + zero stack growth** (was c6 A4/A5, bug2 pair):
   resume delivers its argument as yield's result; a completing fiber
   delivers its return value to the resumer (never an internal artifact);
   hundreds of full park/resume-to-completion rounds show zero stack growth
   on either side. Flagship: host guard auto-parks a running fiber, a
   scheduler resumes until dead, loop counters and checksums exactly intact,
   parks == resumes.
4. **Per-fiber resource leak loops** (was G/H/I/J loops, c4a): create /
   suspend / discard / abnormally-terminate 200+ fibers in loops; no slot or
   stack leak; flat memory.
5. **Guard re-arm across park and frame-pop paths** (was c1, c5, the TCO
   matrix): the host guard must fire repeatedly across park/resume cycles and
   across every frame-pop path — RET, native tail, native-closure tail —
   under @tco aggressive, safe, and off. Its re-arm state is VM-level and
   self-healing.
6. **Resumer-state isolation on switch** (was c2b): resuming a fiber never
   clobbers the RESUMER's own guard/accounting state; per-fiber state
   switches with the stack switch, adjusted not assigned.
7. **Dead-fiber errors** (was C/D/F ghost files): resuming a dead fiber, or
   yielding outside any fiber, raises a clean runtime error from any call
   depth without unwinding or corrupting the live stack (verify with heavy
   allocation afterward).
8. **Released-region hygiene** (was c7 half b): when a fiber dies or unwinds,
   its released stack region is cleared and un-rooted; kill fibers, drive
   allocation hard under ASAN, sentinels held only in live registers survive.

## Invariant classes with NO fiber counterpart (deliberately dropped)

- Prompt-tag identity/ghost semantics, pushPrompt workarounds (A-K files,
  REPRO, J/K_workaround), shift/handler semantics, resume-boundary splice
  mechanics (c6 A1-A3, c7 half a).
- All script-facing shield-depth arithmetic (b_shield, t6/t7 family,
  s3/s4/s5, t4_impact) — UNLESS the fiber design later adds an
  uninterruptible-region primitive, in which case port: parking from inside
  it releases the region's mask, and the guard never observes a mid-invariant
  state across a park/resume cycle.
- Multi-entry script preemption interference (t5_two_entries).

## Notes

- Two tests were load-bearing for the shipping config specifically: r5.zym
  and c3_continuation_spill_snapshot.zym were the only spill tests needing no
  env knob. Their fiber equivalents must likewise run under stock settings.
- Survivors kept live in tests/regression/: c9_local_struct_enum_in_nested_fn
  (compiler pre-pass), fmt.zym (print/str formatting), t_uaf_control.zym
  (GC/stack baseline control — keep as the healthy-baseline pin).
- The full per-file transcription table (96 rows) is in
  CORPUS_TRANSCRIPTION.md alongside this file; this file keeps the distilled
  plan.
