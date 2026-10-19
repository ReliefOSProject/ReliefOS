# SVGA Task 4C0 Transaction Implementation Plan

> **For agentic workers:** Implement task-by-task with the test-driven development workflow. This task has one product/test writer and no branch, commit, push, or VM work.

**Goal:** Add an all-or-nothing trusted SVGA batch path for legacy shader/query work with realistic Mesa metadata bounds, truthful FIFO capacity errors, real fence progress, and bounded shader retirement.

**Architecture:** Keep caller-owned translated words, and add a heap-owned per-batch sidecar for ordered state events, deduplicated typed references, staged shader versions, and query matching. Reserve shader contexts/slots while staging; publish the FIFO body and one real fence as the only commit point; move dead shader backing to a task-context reaper queue. Keep the existing batch initializer and immediate shader helpers source-compatible, adding a sized initializer and explicit abort for new transactional callers.

**Tech Stack:** C, trusted ReliefNT SVGA backend, production-backed host fixtures under ASan/UBSan, x86_64 debug kernel build.

**Spec:** `.superpowers/sdd/2026-10-18-svga-xorg/task-4c0-brief.md`

## Global Constraints

- Input snapshot is at most 65,536 bytes; translated output is at most 262,144 bytes with checked arithmetic.
- Preserve existing 4A/4B APIs and unrelated work; no EXECBUF parser/UAPI, PRIME, VM, branch, commit, or push work.
- A failed or aborted transaction emits no FIFO words, changes no committed shader/query state, and advances no fence.
- FIFO-full before commit returns retryable `SVGA_EBUSY`; statically impossible ring fit returns `SVGA_ENOMEM`.
- Shader backing is retained through its actual covering fence; generation changes and timeouts do not prove retirement.
- Per-operation metadata follows the 64 KiB source bound; deduplicated references follow capacities 64 contexts, 256 surfaces, and 128 GMRs.
- No large per-operation kernel-stack arrays, quadratic query matching, or long spin under the SVGA lock.
- Use `TMPDIR="$PWD/out/tmp"`; root owns native VMware acceptance.

## Review Focus

- Abort after earlier staged shader/query work must leave old maps, bindings, pending results, FIFO cursor, and issued fence intact; cover with a production fixture.
- FIFO pressure after staging must preserve the whole batch for retry or explicit abort; cover exact retry and page accounting.
- More than 32 binds and query operations must remain ordered and complete; cover a 128-operation stream and the 256 unmatched-result ceiling.
- Output plus fence and guard must fit the physical ring statically; cover a >64 KiB output body, one-DWORD-over boundary, and transient free-space pressure.
- Fence snapshots must report raw FIFO progress and reject unissued/stale generations across wrap; cover ordinary and near-wrap sequences.

### Task 1: Production fixture coverage for transactional behavior

**Files:**
- Modify: `tools/tests/svga_test.c`
- Modify: `tools/test_svga.py` only if new linked test support is required

**Interfaces:**
- Consume the current public batch, shader, query, and fence APIs.
- Expected new API names are `svga_batch_init_sized`, `svga_batch_shader_define`, `svga_batch_shader_destroy`, `svga_batch_abort`, and `svga_fence_progress_snapshot`.

- [ ] Add real fixture cases for ordered define/bind/destroy/redefine, abort rollback, >32 shader/query operations, >64 KiB translated output, FIFO feasibility/backpressure, fence wrap, and post-fence retirement.
- [ ] Run each new case against the current implementation and confirm the expected missing behavior fails before implementation.

### Task 2: Heap-backed transaction metadata and shader preparation ownership

**Files:**
- Modify: `kernel/reliefnt/kernel/reliefnt/include/reliefnt/svga.h`
- Modify: `kernel/reliefnt/drivers/video/svga/device.h`
- Modify: `kernel/reliefnt/drivers/video/svga/gb.c`
- Modify: `kernel/reliefnt/drivers/video/svga/shader.c`
- Modify: `tools/tests/svga_test.c` for host heap stubs as needed

**Interfaces:**
- Keep `svga_batch_init(batch, generation, words, capacity)` compatible.
- Add `svga_batch_init_sized(batch, generation, words, output_capacity, source_bytes)` and `svga_batch_abort(batch)`.
- Allocate transaction/event/reference/query metadata from the kernel heap outside `svga_lock`; copy the page allocator callbacks into staged backing ownership.
- Reserve shader context mutation and host slot state under `svga_lock`; perform page allocation and code copy after unlocking.

- [ ] Add a focused failing test for oversized source/output and sidecar allocation failure cleanup.
- [ ] Add the sized initializer, dynamic sidecar, source-derived event capacity, allocator snapshots, and reservation accounting.
- [ ] Run `PYTHONPATH=tools python3 tools/test_svga.py` and confirm all existing batch callers remain source-compatible.

### Task 3: Ordered transactional shader staging, commit, abort, and retirement

**Files:**
- Modify: `kernel/reliefnt/kernel/reliefnt/include/reliefnt/svga.h`
- Modify: `kernel/reliefnt/drivers/video/svga/device.h`
- Modify: `kernel/reliefnt/drivers/video/svga/shader.c`
- Modify: `kernel/reliefnt/drivers/video/svga/fifo.c`
- Modify: `kernel/reliefnt/drivers/video/svga/gb.c`
- Modify: `kernel/reliefnt/drivers/video/svga/svga3d.c` only for teardown coordination

**Interfaces:**
- Add batch-scoped shader define/destroy operations; bind resolves against the staged key overlay.
- `svga_batch_submit` is the only publication point; `SVGA_EBUSY` leaves the transaction retryable.
- `svga_backend_reap_retired(budget, reaped)` detaches only records whose covering fence actually passed and frees backing outside the lock.

- [ ] Add tests for same-stream shader ordering, old-state preservation before submit, explicit abort, FIFO-pressure retry, host-slot exhaustion/reuse, and retirement after raw fence progress.
- [ ] Implement ordered GB packet emission and final key/binding publication with no fallible allocation after FIFO append.
- [ ] Run focused sanitizer-backed SVGA tests; verify page and slot counts return to the original values after abort and retirement.

### Task 4: Bounded query metadata and incremental query validation

**Files:**
- Modify: `kernel/reliefnt/kernel/reliefnt/include/reliefnt/svga.h`
- Modify: `kernel/reliefnt/drivers/video/svga/device.h`
- Modify: `kernel/reliefnt/drivers/video/svga/query.c`
- Modify: `kernel/reliefnt/drivers/video/svga/fifo.c`
- Modify: `tools/tests/svga_test.c`

**Interfaces:**
- Query operation storage shares the sidecar's source-derived event limit.
- Validation and commit use transaction-local context state and a bounded result-key index; persistent unmatched END records remain capped at 256.

- [ ] Add tests for 128 BEGIN/END/WAIT cycles, same-batch END/WAIT without persistent-slot use, and 257 unmatched ENDs rejected before FIFO commit.
- [ ] Replace nested query event rescans and stack arrays with bounded incremental/indexed state.
- [ ] Run focused sanitizer-backed SVGA tests and verify no state mutation on query-validation failure.

### Task 5: Atomic FIFO output bounds and real fence progress snapshots

**Files:**
- Modify: `kernel/reliefnt/kernel/reliefnt/include/reliefnt/svga.h`
- Modify: `kernel/reliefnt/drivers/video/svga/fifo.c`
- Modify: `tools/tests/svga_test.c`

**Interfaces:**
- Define separate 65,536-byte input and 262,144-byte output limits; legacy `SVGA_BATCH_MAX_BYTES` remains compatible.
- Add a try-lock progress snapshot returning `{generation, issued_seq, passed_seq}` from the device and raw FIFO fence register.

- [ ] Add tests for >64 KiB output, static `body + 8-byte fence + 4-byte guard` feasibility, transient free-space `SVGA_EBUSY`, raw progress snapshots, unissued tokens, generation changes, and sequence wrap.
- [ ] Extend FIFO and batch bounds with overflow-safe arithmetic; return `SVGA_ENOMEM` for impossible static fit without changing `NEXT_CMD` or issued sequence.
- [ ] Run focused sanitizer-backed SVGA tests and the full debug kernel build.

### Task 6: Bounded task-context reaper integration and completion report

**Files:**
- Modify: `kernel/reliefnt/drivers/video/drm/drm_vmwgfx_resource.c`
- Modify: `tools/tests/drm_core_test.c` only for backend reaper stubs
- Create: `.superpowers/sdd/2026-10-18-svga-xorg/task-4c0-report.md`

**Interfaces:**
- The existing `drm_vmwgfx_process_retirements(budget)` hook remains the only integration point; it calls the backend reaper under existing syscall execution ownership.
- No new DRM UAPI or resource lookup API is added.

- [ ] Add a production-core fixture that proves the hook advances bounded shader backing retirement.
- [ ] Run ASan/UBSan DRM, SVGA, and GPU suites, then the full debug kernel build with `TMPDIR="$PWD/out/tmp"`.
- [ ] Report exact changed files, API ownership and commit/abort points, test/build commands and exits, log/artifact hashes, and remaining limits.
