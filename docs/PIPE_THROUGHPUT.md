# Pipe throughput (2026-10-16–17)

`yes | dd of=/dev/null status=progress` exercises pipe transfers, scheduler
wakeups and two system calls per output block. GNU dd's default output block
size is 512 bytes. It is not a disk or memory-bandwidth benchmark.

## Changes

This section and the first two comparisons record the initial kernel-only
experiment. The subsequent 500 MB/s experiment is recorded separately below.

The x86-64 kernel's generic `memcpy` and `memset` now use `rep movsb` and
`rep stosb`. Previously they ran byte loops, including unoptimized loops in
debug builds. The new routines copy exactly the requested length, handle
unaligned addresses, return the original destination, clear DF and use no
SIMD state. `memcpy` still requires non-overlapping regions.

User-buffer validation no longer walks an already accessible page a second
time. Lazy-fault and COW handlers still require a successful permission
recheck. Output validation still checks the entire readable range before
resolving any COW pages. Pointer bounds, zero-length handling, pipe capacity,
blocking behavior and signal handling are unchanged.

## Debug kernel comparison

Both kernels use `PROFILE=debug` (`-O0 -g`) and the same Kconfig selection.
The baseline is ReliefNT `50f9f77af4d99094d61025dd3035bea24e487667`; the
candidate adds the two changes above. No dd block-size override was used.

Each boot runs three approximately 10-second rounds of the exact command.
The controller sends SIGINT to the pipeline's process group and records dd's
final byte count and elapsed time. The table uses decimal MB/s medians.

| Guest vCPUs | Baseline MB/s | Candidate MB/s | Change |
| ---: | ---: | ---: | ---: |
| 1 | 44.63 | 71.73 | +60.73% |
| 2 | 38.99 | 63.94 | +63.98% |

Environment: QEMU/KVM q35, `host,+invtsc`, 4096 MiB RAM, BusyBox `yes`, GNU
coreutils dd 9.12, identical prepared guest root filesystem. The benchmark
service runs before the desktop session. These results do not measure an
active desktop or native VMware.

Both ESP kernel paths were replaced in private image copies and read back
before boot. Snapshot mode preserved those private base disks; the source
image hash was unchanged. Evidence is in the local ignored directory
`build/yes-dd-optimization/{baseline-run,candidate-run,comparison}`.

Kernel SHA-256:

- Baseline: `d7f8e18c128aed21ee7905ca0a60ebee6ba911fe10eefd7862b3f2485be2d034`
- Candidate: `f618dee2098db0f8ac5c512d0992fb94f75dac4cb44a65a6bf60652c4787a61a`

## Release kernel comparison

The same experiment was repeated with `PROFILE=release` (`-O3`). The
baseline was built from an independent archive of the same baseline commit,
including its pinned kconfig-frontends submodule and locked font input.
The baseline/candidate configurations compare byte-for-byte equal to the
debug configuration. The active kernel checkout was not reverted for this
control build.

| Guest vCPUs | Baseline MB/s | Candidate MB/s | Change |
| ---: | ---: | ---: | ---: |
| 1 | 93.58 | 98.08 | +4.81% |
| 2 | 82.97 | 92.00 | +10.88% |

These are observed medians from three rounds in one boot per configuration,
not a confidence interval. The smaller gain is consistent with the baseline
release compiler already unrolling the byte-copy loop. The release/debug
difference also includes optimization of the rest of the kernel; it must not
be attributed solely to the two source changes. Two vCPUs still do not speed
up this workload: read/write paths share the kernel execution lock, and
512-byte blocks retain substantial per-system-call work.

Evidence: `build/yes-dd-optimization/{baseline-release-run,release-run,release-comparison}`.

- Baseline SHA-256: `3825de40802b82ef899b931ecee2b904e0f822c037b038b06c5b2a939197b0d9`
- Candidate SHA-256: `f90724a8cdcc174efd237bc92e9d194437bd27aa902660f632760a02ba988842`

## Reaching 500 MB/s

The exact command remains `yes | dd of=/dev/null status=progress`; dd still
uses its default 512-byte blocks. These results use a release (`-O3`) kernel,
QEMU/KVM q35, `host,+invtsc`, 4096 MiB RAM and the same three-round,
approximately ten-second controller. They precede desktop startup.

| Configuration | 1 vCPU MB/s | 2 vCPUs MB/s |
| --- | ---: | ---: |
| Initial memory/usercopy candidate, BusyBox yes | 98.08 | 92.00 |
| Correct scheduler accounting + pipe write thresholds, BusyBox yes | 66.33 | 80.38 |
| Plus poll transition filtering and nonblocking pipe fix, BusyBox yes | 66.60 | 84.08 |
| Plus direct interruptible wake, GNU yes with 64 KiB fallback | 482.24 | 361.43 |
| Plus pinned read/write dispatch shortcut | 500.15 | 373.90 |
| Plus RDPID CPU identification | **722.77** | **549.88** |
| Final kernel with the original BusyBox yes (producer control) | 85.13 | 93.60 |

Final samples were 722.766, 721.300 and 727.618 MB/s with one vCPU, and
548.889, 549.875 and 552.995 MB/s with two. Medians are calculated from dd's
final byte counts and elapsed times, rather than its rounded progress display.
This is three rounds in one boot per CPU configuration, not a confidence
interval or a VMware prediction. Adjacent rows change the stated component;
the GNU yes row also changes the producer and must not be presented as a
kernel-only gain.

The final BusyBox control confirms that changing the default producer is
essential to this result. Against the initial memory/usercopy candidate it
is slower on one vCPU and approximately unchanged on two. These changes
do not make explicit `busybox yes` a 500 MB/s producer.

The scheduler change fixes an accounting bug: full frame capture stopped the
current fair-accounting interval and marked the task READY, but the previous
early return did not restart either. The new helper preserves RUNNING and
the accounting interval when the same task can return safely. Rescheduling,
affinity changes, image replacement and deliverable task/process signals
still require the full path. Correct accounting initially reduced this
particular BusyBox pipeline's throughput; retaining the old accounting bug
would not be a valid optimization.

Blocked pipe writers now record the free-space threshold required for their
retry. Reads wake all writers whose threshold is met, including eligible
writers behind an ineligible queue head. Waking all eligible writers avoids
stranding space if one awakened writer exits before retrying. Unconditional
close/EOF/EPIPE wakeups remain. Wakeups use the scheduler's interruptible
operation while the queue registration still pins the task; stopped tasks
remain stopped. Read-side global poll notification occurs at full-to-nonfull
transitions. Writer thresholds are independent of poll readiness.

Nonblocking writes of at most PIPE_BUF (4096) remain atomic; larger
nonblocking writes can now return the available partial count instead of
incorrectly returning EAGAIN whenever the entire request does not fit.
Blocking writes retain the existing full-request wait policy, and pipe
capacity is unchanged. These choices follow the applicable
[POSIX write rules](https://pubs.opengroup.org/onlinepubs/9799919799/functions/write.html).
The poll transition approach was checked against the
[Linux pipe implementation](https://github.com/torvalds/linux/blob/master/fs/pipe.c).

Pinned read/write calls bypass unrelated implicit-stdio materialization and
the subsystem ownership scan, then use the original common backend. Buffer
validation, access modes, descriptor pin lifetime, blocking retries, signal
handling and the syscall epilogue remain in that backend and its callers.

CPU identification prefers CPUID-advertised RDPID, reading the existing
IA32_TSC_AUX CPU index without requesting a serializing timestamp. RDTSCP and
early-boot APIC lookup remain fallbacks. Linux uses the same instruction for
[CPU identification](https://github.com/torvalds/linux/blob/master/arch/x86/include/asm/segment.h).
The 500 MB/s results above depend on the virtual CPU exposing RDPID; they
must not be promised on a hypervisor that hides it.

### Producer change

The default `yes` is now GNU coreutils yes 9.12, from the already pinned
coreutils package used for dd. The package publishes only dd and yes;
the BusyBox provider composer excludes their competing links. Explicit
`busybox yes` remains available. GNU yes prebuilds the repeated output
pattern. Its normal splice path remains available, and the portable
full_write fallback now uses at least 64 KiB instead of musl's 1024-byte
BUFSIZ. This is the producer's batching size, not a dd block-size override.
The current kernel uses the ordinary write fallback.

As a separate host diagnosis, the locally built BusyBox yes piped to GNU dd
on Linux achieved only 136.20, 135.76 and 135.55 MB/s (three two-second
rounds). The host and guest BusyBox build hashes differ, so this diagnoses
the producer implementation rather than providing a bit-identical control.
It explains why the end-to-end target cannot be attributed solely to kernel
work. GNU yes still writes and dd still reads every byte; no `/dev/null`
shortcut, synthesized result or dropped pipe copy was added.

### Evidence and artifacts

- Final benchmark: `build/yes-dd-optimization/gnu500-rdpid-run/results.json`.
- Intermediate runs: `kernel500-busybox-run`, `kernel500-final-busybox-run`,
  `gnu500-release-run` and `gnu500-fastdispatch-run` below
  `build/yes-dd-optimization/`.
- Final unchanged-producer control:
  `build/yes-dd-optimization/final500-busybox-run/results.json`.
- Final release kernel SHA-256:
  `d245feced835f0797fab6e7496bf8174cc9a4d11d5d7ce12292aee5e54a34c33`.
- GNU yes SHA-256:
  `b52939badaa29690e897e10a21ce49f1ee5a616e67159bcb65f341ead983ccc6`.
- Prepared GNU yes disk SHA-256:
  `275bb96ada49050110fbf590867ad67164d6d92def65132d38be8d24edbcf774`.

The prepared GNU disk changes `/usr/bin/yes` and records its version. The
controller, dd binary, BusyBox binary, EFI and partition layout are preserved.
Both kernel ESP paths are injected and verified in a private copy for each
run. The prepared disk and private base copies remain unchanged by snapshot
boots. Hashes and exact QEMU commands are in each result JSON.

## Initial validation

- Debug and release kernel compilation: passed.
- `python3 tools/test_kernel_memory.py`: passed with GCC/cc and Clang,
  production routines compiled at both `-O0` and `-O3`, ASan/UBSan enabled.
  Each configuration covers 73,728 alignment/length combinations, lengths
  0 through 512, 2,048 deterministic random cases, 90 guard-page cases and
  a 4,194,341-byte copy/fill. All string symbols are renamed in the fixture
  to avoid overriding host libc.
- `python3 -m unittest tools.test_gpu.GpuAbiTests.test_syscall_and_copyout`:
  passed, including lazy faults and COW handlers that report success without
  establishing access, read-only/PROT_NONE pages, cross-page bounds and
  validation before COW side effects.
- `python3 tools/test_icewm_interactions_qemu.py --kernel ... --output ...`:
  passed with the candidate debug kernel injected into a private disk:
  title/Alt drag, resize, maximize/restore, minimize/restore, pointer release
  and terminal input. Evidence: `build/yes-dd-optimization/icewm-regression`.
- Additional `tools.test_linux_memory` suite: **not passing**. Four existing
  fixtures fail compilation/linking: `physical_pages` uses the old `mm_init`
  signature, `elf_file_page` lacks a `console_log` stub,
  `mprotect_vma_growth` lacks SHM/audio VMA stubs, and `brk_collision` uses the
  old `task_shm_map` signature. These fixtures and their production sources
  were not changed here. Details: `out/tmp/yesdd-memory-regressions.log`.

## Final validation (2026-10-17)

- Release and debug kernel compilation: passed. The final debug kernel is
  `71357c9cbdd7ed2260759e772665844e3522ae321fa98148f6a62a1a54416888`;
  the throughput figures above apply to the release kernel.
- The real pinned coreutils package build and BusyBox link-provider test:
  passed. `tools/test_yes.py` validates repeated output across multiple
  64 KiB boundaries, multiple/empty operands, a 70,003-character operand,
  EPIPE exit and real 64 KiB fallback write requests with vmsplice forced
  to ENOSYS. The original small-buffer binary fails the batching check.
- `tools/test_pipe_wake.py`: passed with ASan/UBSan against production pipe
  and wait code. Covers 16 successive 512-byte reads before an 8192-byte
  writer wakes, queue ordering, competing writers and requeue, ring wrap,
  payload integrity, nonblocking PIPE_BUF atomicity and larger partial
  writes, close/EOF/SIGPIPE, poll transitions and stopped-task preservation.
  Its `--verify-red` mode reproduces premature wakeups with the old IPC code.
- `tools/test_sched_resume.py`: passed with ASan/UBSan against the actual
  scheduler. Covers RUNNING/accounting preservation, exact accumulated CPU
  charge on fallback, rescheduling, affinity/frame/CPU-ownership guards and
  task/process pending signals, including blocked-signal preservation.
  Existing EEVDF policy/scheduler/red-black-tree tests also passed.
- `tools/test_smp_cpu_id.py`: passed with ASan/UBSan against actual smp.c
  on two fixed host CPUs. Covers CPUID probing/cache, forced APIC fallback,
  real RDTSCP/RDPID and observed zero/nonzero TSC_AUX mappings. Arbitrary
  TSC_AUX boundary values are not injected from unprivileged host code.
- Guest CPU identity probes: passed with two vCPUs both normally and with
  RDPID hidden (`host,-rdpid,+invtsc`). Each verifies 128 raw getcpu samples
  per CPU, 1024 cross-CPU migrations, TLS preservation and 188,416 checked
  pipe bytes. Evidence: `build/yes-dd-500/cpu-identity-{rdpid,rdtscp}/`.
- The final release kernel completed all 36 decomposition cases on each
  of one and two vCPUs. Device cases account for actual successful short
  I/O, and pipe cases verify 64 MiB of contents per repetition. Evidence:
  `build/yes-dd-500/evidence/final-rdpid/`. Fixed syscall costs improved:
  one-vCPU getppid fell from 478 to 262 ns, and a 512-byte `/dev/null`
  write from about 524 to 274 ns. This is not a universal pipe speedup:
  single-process 512-byte read/refill changed from 424 to 334 MB/s on one
  vCPU and from 276 to 705 MB/s on two; two-process 512-byte transfer
  changed from 303 to 285 and from 239 to 390 MB/s respectively.
- Memory/guard-page and GPU usercopy validation listed above passed.
  Poll/AF_UNIX wakeup and descriptor lifetime/SCM_RIGHTS tests passed.
  The ioctl descriptor-table ASan/UBSan fixture passed, together with
  38/38 host Linux reference checks; its guest ISO was not run here.
  Test-fixture maintenance adds the missing SMP/console stubs, includes
  the actual audio timer implementation in the ioctl test link, and
  updates the dispatch-definition lookup for the new internal signature.
- Final release-kernel IceWM/QEMU interaction regression: passed title/Alt
  drag, resize, maximize/restore, minimize/restore, pointer release and
  terminal input. Both injected ESP kernels were read back and matched the
  final release hash; source and snapshot base images were unchanged.
  Evidence: `build/yes-dd-500/icewm-final-release/`. This verifies completed
  operations, not frame rate or responsiveness during a parallel build.

CPU instruction selection, as in the previous globally cached RDTSCP path,
assumes a homogeneous instruction feature set across online CPUs. A machine
with different RDPID/RDTSCP support on different CPUs has not been validated.

VMware 验证未完成。Real hardware has not been tested.

Generated kernels are available locally at
`out/xorg-vmware-debug/generated/system/kernel.sys` (debug) and
`out/yesdd-release/generated/system/kernel.sys` (release). Production disk
images and installer media were not repackaged by this experiment.

## Further kernel optimization (2026-10-17)

This experiment starts from parent `4b41b6ab232b9ad4d4d5e3bf0ae6b61f68f957f9`
and ReliefNT `a24aa43271a3a9b0cabf9ae84c937f3cb4e80143`. It keeps the prepared
GNU yes disk, yes/dd binaries, default 512-byte dd blocks and controller from
the preceding experiment. All throughput comparisons in this section use
`PROFILE=release` (`-O3`), KVM, q35, `host,+invtsc` and 4096 MiB RAM.
Compilation and host tests are stopped before each timed guest run.

### Wait queues and output validation

Wait queues now publish an atomic count/minimum-threshold hint under their
existing lock. Empty removal/wake operations and threshold wakeups that cannot
satisfy any waiter can return without taking the lock. Adding or updating a
waiter first publishes a conservative nonempty, zero-threshold hint; actual
task entries and thresholds remain authoritative under the queue lock.
Wakeups still retain the registration while the scheduler uses the task.
Close/EOF/EPIPE wakeups and stopped-task behavior are preserved.
The hint does not by itself prevent a producer from reading an old empty
state before registration begins. Syscall callers retain execution-transaction
serialization; concurrent audio IRQ paths retain their epoch check after
registration/sleep and explicitly wake/retry when it changes.

The global poll notifier still increments its generation and takes the queue
lock unconditionally. Registration holds that lock through the BLOCKED state
transition. An empty hint is deliberately not used to bypass this handshake.
This follows the ordering concerns described in the Linux
[wait-queue documentation](https://docs.kernel.org/next/driver-api/basics.html);
the hint is not a replacement for condition-to-sleep synchronization.

`user_range_writable()` checks an already mapped single output page once.
The production PTE query requires PRESENT, USER and WRITABLE, which implies
readability on x86. Bounds and overflow checks precede the shortcut. Lazy,
COW and cross-page ranges use the original full readable-range validation
before changing COW mappings. No bytes, faults or permission checks are
discarded to improve a benchmark result.

| Guest vCPUs | Baseline MB/s | First candidate MB/s | Repeat MB/s | Change (repeat) |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 729.584 | 759.218 | 759.497 | +4.10% |
| 2 | 555.725 | 577.544 | 577.822 | +3.98% |

Each entry is the median of three approximately ten-second rounds, computed
from dd's final byte count and elapsed time. These are observations from one
boot per entry, not confidence intervals. The repeat uses the same retained
candidate binary in a fresh boot of each CPU configuration. A subsequent existing
image kernel control gave 728.814/550.584 MB/s. Despite its local path containing
`debug`, that control has the same optimized instruction sizes as the release
baseline; it is not an `-O0` comparison. The explicit debug build uses `-O0 -g`
and is recorded separately as a compilation check.

Evidence: `build/yes-dd-optimization/next5g-{baseline,candidate,final,debug-baseline}/results.json`.
The last directory name is historical and denotes the optimized existing-image
control above. Both ESP paths were read back and matched the selected kernel;
the source disk and snapshot base disks remained unchanged.
The repeat comparison is `build/yes-dd-optimization/next5g-final-comparison/comparison.md`.

The 5 GB/s target was not reached. The default command still performs roughly
three million read/write calls per second at the observed one-vCPU throughput.
Its small-block fixed costs remain significant, and kernel pipe operations
still share the execution transaction. The lower two-vCPU throughput is
consistent with serialization and cross-CPU handoff costs; these data do not
measure a memory-bandwidth limit or establish a per-function cost breakdown.

- Baseline kernel SHA-256:
  `74bbb165f9249f8822d56451063306bea51fbdc8dea16ecc48ec6955f9f032df`.
- Wait/usercopy candidate SHA-256:
  `f6a8193fdae96a097820e4eecd052d43d5cf0ed036be581d5433c4976afe73b7`.

### Rejected execution-lock trial

A separate trial replaced the execution lock owner's serving-counter increment
with a relaxed load and release store, retaining all SC ticket allocation and
reader admission operations. Linux's
[generic ticket spinlock](https://github.com/torvalds/linux/blob/master/include/asm-generic/ticket_spinlock.h)
also uses release-store owner handoff; its implementation is a reference for
that operation, not a proof of this kernel's separate reader-admission protocol.

The trial passed the concurrency and driver-phase tests, but measured only
751.842/577.736 MB/s on one/two vCPUs, compared with 759.218/577.544 MB/s for
the wait/usercopy candidate. This does not establish an additional speedup.
The lock change was therefore removed, restoring its original SC increment;
the useful suspend/resume concurrency regression remains. Trial evidence is
`build/yes-dd-optimization/next5g-candidate2/results.json`, kernel SHA-256
`dc6cf0fea477037fbbfae545b5263e101e92b85e302ada661650185d33e0474b`.

SYSRET was reviewed but not implemented here. The existing entry/frame and
exception-stack handling need a separate design before replacing IRET. In
particular, the final return frame must be checked for canonical addresses,
selectors, RCX/R11 consistency and flag restrictions. Linux documents the
required conservative fallback in its
[x86 return code](https://github.com/torvalds/linux/blob/v6.12/arch/x86/entry/common.c)
and [entry assembly](https://github.com/torvalds/linux/blob/v6.12/arch/x86/entry/entry_64.S).

### Validation of this experiment

- Release and explicit debug kernel compilation passed for the wait/usercopy
  candidate and final retained source. The final release hash is identical to
  the first wait/usercopy candidate. The explicit debug kernel SHA-256 is
  `5d6f77b7fa351982af7b1b15c4a5791545c71efef5e0805c4bd9e6e882e0a2e7`.
- ASan/UBSan host checks passed: pipe wake/PIPE_BUF behavior, usercopy/COW,
  poll wakeups, AF_UNIX IPC, descriptor pin/SCM_RIGHTS lifetime, scheduler
  resume/accounting and kernel memory/guard pages.
- The new pthread wait-queue fixture passed. It uses real acquire/release
  locks and deterministic contention to exercise poll notifications on both
  sides of the BLOCKED transition, stale generations, stopped tasks,
  threshold boundaries and concurrent wake/removal. Fast-path operation
  counts are also checked by the pipe fixture, including `UINT32_MAX`.
- Execution-lock host tests passed with ASan/UBSan, including readers,
  recursive writers, exclusion/progress and IRQ restoration. The new
  suspend/writer/resume test passed with both the old and new lock: resume
  waits behind an active writer and observes its protected payload. All seven
  existing driver execution-phase checks also passed.
- Final release-kernel IceWM/QEMU regression passed title/Alt drag, resize,
  maximize/restore, minimize/restore, mouse release and terminal input.
  Both ESP readbacks matched the final kernel; source and snapshot base disks
  were unchanged. Evidence:
  `build/yes-dd-optimization/next5g-icewm-final-retry/results.json`.
  The first attempt passed dragging and resizing but failed at maximize
  because the test still sent Alt+F10/F9. The shipped preferences use
  Super+Z/M; only the test's shortcut input was synchronized before rerunning.
  The initial failure is preserved in `next5g-icewm-final/results.json`.
- The final release kernel completed all 36 decomposition measurements
  (including three repetitions per case) on each of one and two vCPUs.
  Device tests account for successful short I/O; pipe tests verify 64 MiB of
  payload per repetition, including the two-process wakeup path. Both ESP
  readbacks matched the selected kernel and source/snapshot base disks stayed
  unchanged. Evidence: `build/yes-dd-500/evidence/next5g-final/results.json`.
  These are separate diagnostic cases, not replacements for the exact GNU
  yes/dd command in the throughput table.
- The full kernel `make ... test` was attempted and failed at the existing
  audio/driver UAPI ABI golden drift. Header export (81 self-contained C/C++
  headers) and test-tools passed. The golden file was not regenerated.

Logs: `out/tmp/kernel-5g-*-tests.log`, `kernel-5g-kernel-suite.log`,
`kernel-5g-final-build.log` and `kernel-5g-final-debug-build.log`.

Final standalone kernels are copied to `build/kernel-throughput-5g/final/`.
The `out/xorg-vmware-debug/generated/system/kernel.sys` output is explicitly
an `-O0` debug build from this experiment; the directory name alone does not
select the profile (the parent Makefile defaults to release). Existing formal
RAW/VMDK/ISO media were not repackaged by this experiment. Runtime checks inject
the release kernel into private image copies instead.

VMware 验证未完成。The IceWM test uses QEMU's VMware SVGA emulation, not the
VMware hypervisor. It checks completed interactions, not desktop frame rate,
parallel-build responsiveness or real-hardware performance.
