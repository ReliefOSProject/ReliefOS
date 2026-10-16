# IceWM interaction responsiveness

This investigation covers slow or unsuccessful window operations on the Xorg
desktop, reported on VMware. The fixes belong to shared kernel paths rather
than individual applications or an opaque-move preference workaround.

## Confirmed defects

* `poll`, `select`, and `epoll_wait` previously rescanned only after a scheduler
  tick. Descriptor readiness now wakes a shared, bounded waiter queue. A
  generation sampled before the readiness scan is checked while registering
  and blocking, closing the scan-to-sleep race, including input IRQ publication.
  AF_UNIX sockets, pipes, PTYs, eventfd, epoll ADD/MOD and evdev publish changes.
  The blocked-syscall return avoids a second tick sleep in the syscall epilogue.
  Stopped tasks remain stopped and waiter storage stays pinned during wakeup.
  A one-tick backstop remains for unnotified sources, queue exhaustion, and
  descriptor-free sleeps; existing timeout and signal-interruption paths remain.
* The returning-task fast path used one global reschedule hint. A scheduling
  round on CPU0 could clear CPU1's request, and CPU1's timer did not re-arm it.
  Reschedule requests are now per CPU; both timer paths request a local round,
  and runnable-task publication notifies online CPUs. Signal/signalfd wakeups
  also publish the notification. No task structure layout changed.
* The shared evdev ring silently skipped overwritten records. Losing a button
  release could leave userspace believing the button was held. Readers now get
  `SYN_DROPPED`, then `SYN_REPORT`, with retained history discarded at the recovery
  boundary. Current state remains available through the existing EVIOCG queries,
  following the [Linux evdev recovery contract](https://docs.kernel.org/input/event-codes.html#ev-syn).
  Single-event reads and sequence wrap are covered. Loss accounting is per
  device: another VT or grab reader can receive a conservative generic loss
  notice, while event payloads remain filtered by VT and grab ownership.
* The mouse driver's IRQ12 drain read keyboard bytes from the shared 8042 output
  port before testing their source, then discarded them. It now checks AUX
  before reading, leaving keyboard bytes, including Alt release, for IRQ1.
* fbdev fallback scanout was scheduled at 10 Hz. It is now scheduled at 50 Hz
  on the 100 Hz PIT. Timer refresh attempts the SVGA lock once and defers a full
  FIFO rather than entering a polling loop. A full FIFO gets one doorbell/BUSY
  service attempt and is retried later. Explicit presents and fences retain
  their existing synchronization. FIFO configuration and same-mode requests
  restore supported GFB write tracing. Actual VMware reset behavior was not
  established by the modeled reset test.

## Event-loop diagnosis and measured limits

IceWM 4.0.0 performs an `XSync` round trip for each additional motion removed by
its [motion coalescer](https://github.com/ice-wm/icewm/blob/4.0.0/src/ywindow.cc#L547-L569).
The default drag test accepts either 200 ms elapsed time or a 4 px displacement;
it does not require a minimum hold time. A diagnostic XQueryPointer client
confirmed correct button-down motion during a 200 Hz stream even when the frame
did not move. Holding the button and sending a later motion eventually moved
the frame, supporting a service-backlog diagnosis. With readiness wakeups, the
same 200 Hz interaction sequence completes in QEMU and the native VMware drag
and resize checks pass.

The guest AF_UNIX request/reply probe checks every payload and sequence, bounds
each mode to 15 seconds, and reports timing without a performance assertion.
These are aggregate measurements from QEMU/KVM with two vCPUs, using
`CLOCK_MONOTONIC_RAW`; the timer resolution limits short-run precision.

| Affinity and operation | Before | After |
| --- | ---: | ---: |
| Both endpoints on CPU0, 128 poll round trips | 1280 ms | 20 ms |
| Both endpoints on CPU0, 32 select round trips | 340 ms | 10 ms |
| Both endpoints on CPU0, 32 epoll round trips | 370 ms | 10 ms |
| Endpoints pinned to different CPUs, 128 poll round trips | 2050 ms | 2040 ms |

The split-CPU result remains tick-limited: remote readiness sets the target
CPU's reschedule flag but does not send a reschedule IPI. An idle AP wakes at
its local timer, and a running AP checks the flag at its next preemption point.
The task remains ready; this is a remaining cross-CPU latency limitation, not
evidence of a lost poll wakeup. No new interrupt vector was added in this fix.

## Verification

Host regression entry points:

```sh
export TMPDIR="$PWD/out/tmp"
mkdir -p "$TMPDIR"
python3 tools/test_eevdf.py
python3 tools/test_poll_wakeup.py
python3 tools/test_evdev_overflow.py
python3 tools/test_installer_input.py
python3 tools/test_linux_threads.py
python3 tools/test_xorg_vt_handoff.py
python3 tools/test_xorg_display_refresh.py
python3 tools/test_svga.py
python3 tools/test_runtime_responsiveness.py
python3 tools/test_vdso_time.py
python3 tools/test_vdso_mapping.py
python3 tools/test_header_export.py
python3 tools/test_execution_lock.py
python3 tools/test_unix_ipc.py
python3 tools/test_linux_pty.py
python3 tools/test_linux_socket_batches.py
```

The scheduler regression includes actual scheduler code under ASan/UBSan.
Its guest counterpart pinned a busy task to CPU0 and two competitors to CPU1:
the old kernel failed because a competitor did no work; the corrected kernel
completed with approximately equal CPU1 counters (2.15 million each). Normal
nice-weighted competition remained correct.

The final kernel was built successfully with:

```sh
TMPDIR="$PWD/out/tmp" make -C kernel/reliefnt \
  O="$PWD/out/vdso-time-kernel" -j8 all
```

The frozen-tick poll fixture uses production syscall, AF_UNIX and wait-queue
code under ASan/UBSan. It covers publication before registration, event wake
before the timer, stopped/exited cleanup, finite timeout retention and
descriptor-free fallback. The
signal tests separately cover EINTR and deadline clearing; the host fixtures
do not execute a full interrupt-return transition. The guest round-trip and
graphical tests exercise that transition in the actual kernel.

The display tests include production framebuffer/SVGA code and a host device
model, checking trace restoration, timer cadence, lock contention and both FIFO
paths under backpressure. They establish bounded guest operations, not hardware
presentation latency or measured FPS.
The host model supplies a minimal font fixture so it builds without generated
kernel resources; these scanout/FIFO tests do not cover text rasterization.

The QEMU graphical test uses a private raw copy with `snapshot=on`, checks final
geometry after drag/resize and release, exercises maximize/minimize/restore, and
proves terminal keyboard input with a serial marker. Screenshots must converge;
character-cell resize tolerances are separate from pointer-move tolerances.

```sh
python3 tools/test_icewm_interactions_qemu.py \
  --image out/xorg-vmware-debug/images/reliefos.raw \
  --kernel out/vdso-time-kernel/generated/system/kernel.sys \
  --motion-interval .005 \
  --output build/icewm-interactions-new-run
```

The final QEMU run passed all listed interactions at a requested 200 Hz pointer
sample rate. Source and private base image hashes were unchanged. Native
Workstation 26, two vCPUs, also passed title-bar and Alt drags, reverse dragging,
resize and release checks at the same requested input rate, plus
maximize/restore, minimize/restore and a final terminal serial marker.
The private VMware test VM was stopped successfully after the run.

Local evidence:

* `build/icewm-interactions-readiness-200hz/results.json` and its screenshots:
  complete QEMU interaction sequence, source/base hash checks passed.
* `build/icewm-vmware-final-200hz/results.json` and its screenshots:
  complete native VMware interaction sequence, including terminal input.
* `build/icewm-scheduler-readiness/serial.log`: both CPU1 competitors progress
  while CPU0 is busy; normal nice-weighted competition also passes.
* `build/icewm-poll-readiness/serial.log`: both affinity modes pass payload and
  sequence checks; timings are listed above.
* `out/icewm-fixed/verification.json`: successful build and all 16 host test
  entry points, with their individual log paths.

**VMware 验证未完成**: the user's original VM and its hardware/3D configuration
have not been retested. The local VMware probe uses an isolated two-vCPU VM
with 3D disabled. These tests do not establish measured FPS or physical input
latency.

A clean VMDK containing the final kernel is at
`out/icewm-fixed/images/reliefos.vmdk`. Its kernel readback hashes and image hash
are in `out/icewm-fixed/kernel-manifest.json`. The VMware test VM writes to a
separate copy, and the original `out/xorg-vmware-debug` images were not replaced.

The existing evdev-grab fixture needed missing audio dispatcher dependencies;
the new stubs abort if an evdev scenario accidentally enters an audio branch.
The membarrier host fixture now uses its mocked LAPIC CPU identity instead of
the host Linux TSC_AUX value. These are fixture repairs, not audio or membarrier
product changes. ABI golden files and audio support manifests were not changed.
Isolated input, VT, PTY and socket fixtures also received the new notifier
dependency stubs; actual readiness notification is tested in the poll fixture.
Build and runtime artifacts are kept outside the source commits.
