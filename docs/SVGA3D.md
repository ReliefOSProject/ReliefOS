# VMware SVGA-II 3D

ReliefOS initializes the SVGA-II 3D path after the physical page allocator is
ready. The driver first checks the device capability register, extended FIFO,
fence support and GMR support. Legacy hosts use FIFO hardware versions and
capability records; GB Objects hosts use indexed device capability registers,
guest object tables and memory objects (MOBs). Failed capability checks retain
the existing 2D framebuffer and return a negative status from `svga3d_init()`.
If a host stalls while it still references guest memory, the driver preserves
those pages and the FIFO until a later initialization/shutdown retry can finish
retirement. FIFO waits remain bounded.

The public ring-0 API is declared in `kernel/reliefnt/kernel/reliefnt/include/reliefnt/svga.h`.
The implementation lives in `kernel/reliefnt/drivers/bootstrap/svga/` and uses the vendored
Linux VMware protocol headers in `kernel/reliefnt/drivers/bootstrap/svga/protocol/`, with
the required GB wire extensions in `kernel/reliefnt/drivers/bootstrap/svga/gb_protocol.h`.

## Xorg, Mesa and the DRM device

The desktop configuration uses unmodified Alpine Xorg 21.1.25 with its
`modesetting` driver and glamor, plus the signed Mesa Gallium DRI subpackage
26.1.6-r0. The `vmwgfx_dri.so` entry point is the upstream Gallium SVGA driver.
The profile leaves the Xorg Device driver and KMS path to standard
auto-configuration, requests `SWcursor`, and sets `PageFlip` false. That option
prevents modesetting page flips; it does not disable DRI3, glamor or Present,
whose copy path remains available. Xorg 21.1.25 registers DRI3 and Present as
non-disableable extensions, so `-extension DRI3` is not a supported way to
turn off DRI3.

Mesa's VGPU9 path provides the GL 2.1 / GLSL 1.20 compatibility level that
meets glamor's source-level desktop GL minimum. This source compatibility does
not establish that a particular guest boot obtained a VMware context or that
all desktop rendering and interaction are stable. The independent
`glxgears` SVGA3D ioctl renderer described above is application-specific and
does not prove that Xorg's GLX/DRI or glamor path works.

The primary KMS node `/dev/dri/card0` and render node `/dev/dri/renderD128`
are exposed with mode `0660`, group `video` (GID 44). Mesa and Xorg use the
same vmwgfx DRM device for the screen pixmap, glamor and KMS scanout. DRI3
clients must use that same device and the authenticated primary-node route.
The vmwgfx PRIME path exchanges opaque shareable surface references for this
same-device use; it is not a promise of cross-device DMA-BUF attachment,
mapping or import. A PRIME capability bit alone does not prove a surface can
be exported and imported.

The fixed profile does not claim atomic modesetting, page-flip presentation,
hardware vblank, hardware cursor support, cross-device DMA-BUF import, physical
host GPU utilization, or installer-ISO acceptance.

## Product validation status (2026-10-19)

All required final runtime gates passed for the bounded VGPU9 profile. The
source-frozen canonical image uses kernel
`e9cf25df756dfcb78eccae03d1e890a608019686d022086e88ead91c9b30743f`. Native
VMware GPU/PRIME/KMS checks completed with status 0, including Mesa pixel and
feature checks, same-device PRIME, and visible scanout; the red triangle and
blue pixels were root-viewed and all four owned-resource baselines were
restored. The default-user accelerated session used unmodified Xorg and the
upstream `vmwgfx` glamor path, reported GL 2.1, and passed the original DRI3
surface checks in both directions. The session covered visible gears and
Chinese File Manager, window move/resize/cover, VT2 isolation and return,
renderer kill and recovery, normal closes/logout, and restored tty1 text
login. The upstream DRI query remains `accelerated=no` and `video_memory=1
MiB`; these query values do not identify the renderer that produced a frame or
measure physical-host GPU use. See
`out/svga-final-console-gpu-vmware/runtime-result.json` and
`out/svga-final-console-session-vmware/runtime-result.json`.

The unchanged repository lifecycle oracle passed on both QEMU Standard VGA
(531.68 seconds) and VMware VGA 2D (618.91 seconds) with status 0 on the same
canonical source image. Fresh tty1 numeric-prompt checks were separate OUT-only
strict observations, not part of the unchanged oracle: Standard VGA displayed
`37192972` and VMware VGA 2D displayed `83680142`; each token was erased without
pressing Enter. Their control records and root-reviewed captures are in
`out/svq-std-e9cf-n/` and `out/svq-vmw-e9cf-n/`. The VMware VGA 2D log retains
the SMP APIC 1 startup timeout followed by `online=2/2`, also present in the
earlier controls; its cause remains uninvestigated and is not claimed fixed or
warning-free.

The native VMware session with `mks.enable3d=FALSE` also passed the bounded
software-fallback gate. 3D was unavailable (`-95`), the render node was absent,
and the default user obtained `llvmpipe (LLVM 22.1.3)`; Xorg declined glamor for
that software renderer and initialized DRISWRAST. Chinese File Manager,
window move/resize, VT2 numeric-marker clear and return, normal client/session
exit, a fresh greeter, and tty1 numeric prompt entry/erase without Enter were
observed. This is a working software-rendered fallback, not accelerated
glamor. The runner completed with status 0 in 623.17 seconds, including a
recovered host-space pause before guest kernel execution. The same attempt
paused with VMware `noSpaceBasic/noSpaceCritical` and zero kernel serial bytes;
root moved only a stopped, read-only private GPU RAW between owned filesystems,
verified the copy before replacing its former path with a symlink, and resumed
the same VM without changing guest configuration or discarding artifacts. See
`out/svga-final-console-fallback-vmware/` and
`out/svga-storage-relief-20261019/migration-proof.json`. This run was not an
uninterrupted cold boot. Independently prepared private RAW image hashes differ;
the canonical kernel, four injected payloads and 14 product-file readbacks
were verified separately, so the full private images are not claimed byte-
identical. Existing Xorg/font and XDM reset warnings remain in the runtime
logs.

Build and runtime results are separate. The frozen source set contains 2,737
identities; the canonical kernel and product image builds completed, both ESP
kernel readbacks match, 14 product-file readbacks match staging, and the RAW
and VMDK content comparison passed. The full `make test` target remains failed
(exit 2) because `out/audio-hda/system/lib/libreliefos-audio.so.1` is missing;
the repository-wide suite is not green. Targeted source fixtures and package
checks do not replace that result.

These results cover the final pinned VGPU9 profile only. They do not establish
physical host GPU utilization, full Linux/POSIX certification, installer-ISO
acceptance, atomic modesetting, page-flip presentation, hardware vblank,
hardware cursor support, or cross-device DMA-BUF import. The inherited
`preadv2` flag/error-order concern and unknown upstream recursive-exit caller
remain deferred; console replay's allocation fallback preserves text and
cursor semantics but can retain the earlier device-memory replay latency.
## glxgears and Task Manager

Start glxgears from its existing desktop entry, or from a GUI terminal:

    /usr/lib/reliefos/apps/glxgears/glxgears.elf

No boot parameter is required. The app prefers SVGA3D when 3D is available and
prints `glxgears: renderer=VMware SVGA3D hardware`. Otherwise, or if hardware
rendering fails, it logs the failure and continues with PortableGL software.
Both applications are already enabled in `configs/default.conf`.

The hardware backend reuses the original gear geometry and prepares diffuse
vertex colors in userland. SVGA3D performs object-to-clip transforms, clipping,
depth testing, interpolation and rasterization. The result is read back through
SurfaceDMA and submitted to the existing GUI window service, so overlapping
windows and desktop controls continue to compose normally. This backend is not
a general OpenGL/GLX driver; PortableGL remains the software OpenGL library.

Each GPU context owns two color/depth/vertex/readback slots. A render ioctl
queues frame N on the next slot, then waits for the previously queued slot and
copies that readback to userland. After startup, the GPU therefore has a full
frame interval to finish before its fence is needed instead of stalling the
caller on every submission. The first two calls return the first rendered
frame while the pipeline fills.

Task Manager's Performance tab includes a GPU (estimated) percentage and history.
It measures elapsed device-clock ticks spent inside each render call, including
any synchronous Fence wait. Idle time between frames is not counted. The value
includes virtual host scheduling and DMA, and is not the physical host GPU
engine utilization. Device absence, counter resets and the first sample display
N/A. CPU/memory ABI is unchanged.

High guest CPU usage does not by itself indicate software rasterization. The
current hardware path still computes vertex lighting on the CPU, copies each
returned frame through the GPU ioctl and GUI window service, and uses the CPU
desktop compositor. Fence waits are bounded and poll while holding the device
lock, but normal frame-to-frame waits now target the previous slot and are
usually already complete. A short submission-to-Fence interval can therefore
coexist with high guest CPU usage. These counters cannot establish which
physical rendering backend VMware selected on the host.

The GPU ioctl snapshots vertex/draw inputs and validates every writable output
page, including COW resolution. Under the kernel execution lock, the driver
copies a successful fenced readback directly into that output. It does not
allocate and clear another frame-sized kernel buffer or copy that buffer back
to userland. Failed renders leave output unchanged. Readback and CPU desktop
composition remain CPU copies; double buffering removes the per-frame wait from
the critical path.

Every five seconds, hardware glxgears also logs `SVGA3D device totals` alongside
FPS: generation, submitted/completed/failed frames, and submitted triangles.
These are cumulative device-wide counters, not per-process FPS or GPU engine
usage; compare successive samples within one generation. Completed frames count
readbacks delivered to the caller. When diagnosing load, compare guest CPU usage
with glxgears running and closed, and include these logs and whether the gears
rotate.

Normal 2D/resource packets still publish with an explicit SYNC. An ordinary
framebuffer present (`SVGA_CMD_UPDATE`) is asynchronous: the packet is written
and the SYNC doorbell is rung, and the device lock is released without reading
SVGA_REG_BUSY, so a 2D present never serializes guest CPU behind host FIFO
progress. The bounded BUSY wait survives only where progress must be proven:
explicit fence waits, mode transitions and shutdown. A glxgears
frame is deferred as one batch: its vertex upload, state, draw and readback
packets are written before a single SYNC at the frame Fence. This avoids one
SYNC port write per packet while preserving the earlier explicit-notification
fallback for every non-batched path. Fence waits first poll the FIFO Fence
register at MMIO speed with paced PAUSE delays; only a slow frame falls back to
the bounded SVGA_REG_BUSY loop. The slow fallback preserves the previous
synchronous progress guarantee and the final allowed BUSY-read boundary check.

For VMware acceptance, run glxgears alongside Task Manager, exercise rotation,
polygon modes, window resizing, overlapping windows, close and force termination.
Confirm the hardware renderer log and actual gears, then repeat with VM 3D disabled
to check software fallback and the unavailable GPU state. Host tests cannot prove
that VMware renders the scene correctly.

### Diagnosing Software Fallback

On a device-side render failure, glxgears prints the saved failure stage, FIFO
MIN/MAX/NEXT/STOP/BUSY, Fence passed/issued values and frame counters before
releasing the context. This snapshot is taken before cleanup issues more
commands, and querying it does not read I/O ports or process queued work.
`prepare-fence` identifies synchronization while reclaiming a reused slot;
`frame-fence` identifies a timed-out wait for either the first frame or the
previously queued slot. Other stages identify the submission or copyout
operation that returned an error. A Fence failure does not identify which
earlier command the host is processing. Include all three diagnostic lines
when reporting `SVGA3D render failed`.

glxgears logs `GPU_INFO failed, error=...` when the kernel query fails. A
successful query prints its flags and generation; if 3D is unavailable the app
returns `-19` (ENODEV). Older builds incorrectly used `-38` (ENOSYS) for this
case too, so that older error alone does not identify the failing layer.

To see the driver initialization log, reboot and select
`ReliefOS (with boot log)` in GRUB. Alternatively edit the normal GRUB entry
with `e`, append `bootlog=1 bootlog-pause=1` to its
`multiboot2 /reliefos/kernel.sys ...` line, and boot with Ctrl+X. In updated kernels,
`bootlog-pause=1` repeats the SVGA3D initialization summary and waits for Enter
before starting desktop or TTY processes. Capture all the `[svga3d]` lines
above `Boot log paused`, then press Enter to continue. The installer ISO's
`Install ReliefOS (Enable boot log screen)` entry also pauses, so the new kernel
can be diagnosed from the ISO without installing it. Older kernels ignore the
pause parameter. A VMware serial port redirected to a host file preserves the serial boot log
when the selected entry includes `log=screen,serial`.
The current `/dev/kmsg` implements writes only and cannot read the boot log.

The `probe=... status=...` line records the precise rejection stage. The
`probe-fifo`, `raw-hw` and `revised` fields retain the extended FIFO data from
before 2D rollback; `fifo=0` in the current 2D state does not mean the host
originally advertised no FIFO features. Capability-record corruption retains
its EIO status instead of being replaced by ENOTSUP.

For devices advertising `SVGA_CAP_GBOBJECTS` (0x08000000), the diagnostic also
reads `SVGA_REG_DEV_CAP` (52) with index `SVGA3D_DEVCAP_3D` (0). This is the
register interface used by upstream
[vmwgfx_devcaps.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/vmwgfx/vmwgfx_devcaps.c).
`gb=1 devcap3d=1` can coexist with zero legacy FIFO hardware versions. Linux
checks guest-backed resource support in this case, as shown in
[vmw_supports_3d](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/vmwgfx/vmwgfx_cmd.c).
ReliefOS now initializes the GB backend for these devices. A successful probe
reports `init=0 available=1` and `probe=ready-gb status=0`; the raw FIFO versions
can still be zero. The reported `host=0x20001` describes legacy command
compatibility, matching vmwgfx's GB compatibility interface, rather than a
nonzero value read from the legacy hardware-version fields. Initialization
does not succeed until the object-table setup Fence completes.

`probe=mob-limits` means the advertised MOB budget or maximum object size is
insufficient. `probe=gb-tables` reports allocation, command submission or Fence
failure while installing object tables. These stages retain the original error.
No additional defconfig option or boot parameter enables GB support.

## GB Resource Lifetime

The driver installs four pre-DX object tables (MOB, surface, context, shader).
It leaves the Screen Target table unconfigured: setting this table switches
away from the legacy screen interface, so framebuffer UPDATE and v9 Present
must not be paired with it. This is also how vmwgfx used GB resources before
implementing a complete Screen Target display backend; see the upstream
[Screen Target implementation](https://lists.freedesktop.org/archives/dri-devel/2015-March/078905.html).
Tables use PPN32 page tables of depth zero or one; ordinary MOBs
also support depth two for surfaces larger than 4 MiB. Allocations must fit the
advertised MOB size, guest memory budget and 32-bit page-number range.
Context backing is 16 KiB and is bound with `validContents=0`. Color, depth and
vertex surfaces receive dedicated pinned MOB backing. Existing fixed-pipeline
draw commands, GMR SurfaceDMA and Present use these bound GB resources.

Retirement invalidates discarded surface contents, unbinds and destroys the
resource, destroys its MOB, and waits for a Fence before freeing data or page
tables. Each submitted step is recorded so retries cannot double-destroy an
object. Raw ring-0 render-target/texture bindings update reference tracking.
Partially created or retiring resources cannot render. A failed ring-0 create
can return an owned cleanup handle; the process GPU API retains it internally.
MOB backing and object-table pages are included in reported guest memory usage.

The GB backend implements the fixed pipeline used by the triangle and glxgears.
The ring-0 raw interface rejects shader/query commands that would require GB
translation; it does not expose a general shader or DX context implementation.
Existing rendering request layouts remain unchanged.

## User ABI

`include/reliefos/gpu.h` (also shipped in the SDK) defines version-1 GPU ioctls:
INFO, CREATE, RENDER, DESTROY and DIAGNOSTICS. The libc exports matching
`reliefos_gpu_*` wrappers; `leonos_gpu_*` names remain as source-compatible aliases.
Raw requests must initialize `size` and `version`; wrappers supply these fields.
Return values are zero or a negative errno. Context handles belong to the creating
process, are not inherited by fork, and are retired on exit/exec. Mode resets
invalidate old contexts; the application falls back if it receives an error.

DIAGNOSTICS (`0x4c475005`) returns the device's most recent render-failure snapshot
only to its owning process. `status=0` means no snapshot for this caller; a later
failure replaces it, and owner exit or device reset invalidates it. The snapshot
contains no physical addresses. Existing version-1 requests retain their sizes
and semantics; glxgears continues software fallback if a kernel rejects the
diagnostic query.

Frames contain bounded triangle lists: at most 32768 FLOAT3/color vertices and
16 draw ranges, with finite column-major object-to-clip matrices in D3D depth
range [0,w]. Point, line and solid fill are supported. There is no raw hardware
command submission, BAR mapping or kernel pointer exposure in this ABI.
The kernel copies and validates input, verifies writable output including COW,
and reads back tightly packed, top-down 00RRGGBB pixels. `pixel_capacity` must
equal the context's width times height. Render failures leave output untouched.
Timed-out host resources remain tracked until synchronization permits cleanup;
after the host resumes, a GPU statistics query also retries their retirement.

## Boot Self-test

To run the fixed-pipeline triangle self-test, add this kernel command-line
token:

    svga3d-triangle=1

The test creates a context, color surface, depth-stencil surface, vertex
surface, and GMR buffer; uploads three colored vertices with SurfaceDMA;
clears the render targets; submits fixed-pipeline state and DrawPrimitives;
then submits Present and destroys every resource. The result is logged as
`[svga3d] triangle-test=0` on success.

For VMware validation, enable virtual 3D acceleration for the VM and run the
same image once with acceleration disabled. The disabled case must show the
normal 2D desktop and a nonzero 3D initialization status without a hang. The
maintainer should also repeat mode changes and the triangle test several times
to exercise FIFO reset and resource teardown.

The host regression test does not need the complete kernel dependency graph:

    python3 build.py test svga

It compiles the actual FIFO, capability negotiation, fence, GMR, MOB/OTable,
resource, DMA, Present, and triangle command code against a simulated SVGA
device with AddressSanitizer and UndefinedBehaviorSanitizer enabled. The model
checks legacy and GB rendering submission, page-table walks, no free before
Fence completion, allocation/submission failures, timeout recovery, mode reset,
disabled 3D, process ownership, and GPU statistics. The display regression models
the Screen Target table's effect on legacy UPDATE/Present and checks that 3D
initialization, reinitialization, and mode changes preserve those display commands.
It also compiles/links all
driver files with the kernel's GPR-only ABI at O0 and O2. VMware rendering is
still a separate acceptance test performed by the maintainer.
