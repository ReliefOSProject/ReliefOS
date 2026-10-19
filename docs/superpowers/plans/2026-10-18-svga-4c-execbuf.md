# Task 4C: validated vmwgfx EXECBUF and fences

**Goal:** accept the pinned Linux vmwgfx v1 EXECBUF ABI for the bounded VGPU9 path and submit validated, relocated SVGA commands with real fence and resource lifetimes. Publish 3D only when the complete required path is available.

**Authority:** `.superpowers/sdd/2026-10-18-svga-xorg/task-4c-brief.md` and focused contracts under `.superpowers/sdd/2026-10-18-svga-xorg/`. The brief controls when details differ.

## Global constraints

- Implement on the explicitly assigned accepted 4C0 fix1 baseline; no commits, branches, pushes, child agents, or VM work.
- Keep temporary paths beneath `/home/xiaobai/Projects/Projects/ReliefOS_Xorg/out/tmp`.
- Use pinned Linux 6.12 and Mesa 26.1.6 behavior. Do not patch user-space, claim unsupported capabilities, or change unrelated KMS/PRIME/packaging paths.
- Every production behavior starts with a targeted failing test. Use bounded heap/task-owned state; never retain stack state across scheduling or destruct in IRQ context.
- Root owns native VMware probes and runtime-fix feedback. Send it stable target images/probes when available.

## Task 1: parser contract and immutable snapshot

Add production-backed sanitizer tests for v1 short/full arguments, unknown/short requests, zero-length commands, bounded command snapshots, checked packet walking, late malformed packets, and untouched state on rejection. Implement bounded VGPU9 packet parsing over the immutable copied stream; reject unsupported command families. Expected: ASan/UBSan test shows RED before implementation and GREEN after; no command is submitted on any parse error.

## Task 2: typed resource validation and staged state

Test wrong-file/stale/wrong-type resource IDs, checked DMA ranges, subresource topology, four render targets, shader scoping, multi-vector constants, and define/bind/destroy order including late failure. Implement trusted relocations and transaction-local logical overlays using accepted 4B and 4C0 APIs. Expected: only authorized retained resource IDs enter translated words, and logical state publishes only after atomic batch submit succeeds.

## Task 3: BO reservations, jobs, and atomic submission

Test GPU/CPU exclusion, ALLOW_CS semantics, retained BOs through handle close, empty fence-only submits, FIFO EBUSY retry without replay, copy-reply failure after commit, and aggregate resource/job bounds across close/reopen. Implement reservation interfaces, borrowed GMR attachment lifecycle, bounded committed jobs, cancellation-safe cleanup, and minimal fence-only backend support. Expected: job resources retire only from actual progress; no unbounded waits under FIFO/global locks.

## Task 4: fence ABI, waits, and task continuations

Test real fence sequence/generation, zero reply mask and fd=-1, signaled flag echo, timeout cookies, signal/SA_RESTART, UNREF only on success, exit/exec cleanup, final close/reuse, and SYNCCPU interrupted-to-EBUSY mapping. Implement per-file handles plus independent job refs, task-owned continuation snapshots and paced bounded progress. Expected: committed prefixes are never replayed and fresh post-signal syscalls resolve the then-current fd.

## Task 5: truthful caps and verification

Add focused DRM/SVGA/OFD tests, enable GET_PARAM(3D) and only caps proven by the complete implementation, run the full debug kernel build, and provide root the stable target probe. Expected: all focused tests and build pass; report exact source, build, and probe hashes, commands, exits, and remaining unsupported behavior. Native VMware green remains root's acceptance gate.
