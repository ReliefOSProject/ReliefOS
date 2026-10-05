# ReliefOS ABI

This guide retains frozen Linux/ReliefOS ABI identifiers and legacy `leonos_*`
names where existing binaries or callers still use them. Canonical ReliefOS
names and the forwarding/alias rules are listed in
[`branding-compatibility.md`](branding-compatibility.md).

## Linux x86_64 syscall convention

- `rax`: syscall number.
- `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9`: arguments 0 through 5.
- `rax`: return value.
- Negative return values are `-errno`.

Unimplemented syscalls return `-ENOSYS`.

## GPU Rendering ABI

`include/leonos/gpu.h` defines the versioned, process-owned offscreen rendering
API used by glxgears. `leonos_gpu_create`, `leonos_gpu_render` and
`leonos_gpu_destroy` manage bounded SVGA3D triangle batches; `leonos_gpu_info`
reports capabilities, resource counters and estimated busy time.
`leonos_gpu_diagnostics` retrieves the latest device render-failure
snapshot only for its owning process, without advancing the hardware FIFO.
Existing rendering request layouts and the CPU/memory performance structure
are unchanged. See [SVGA3D.md](SVGA3D.md)
for layout semantics, limits, synchronization, software fallback and validation.

## Dynamic Library ABI

Dynamic PIE executables use `/lib/ld-musl-x86_64.so.1`. musl supplies the
loader and standard C/POSIX ABI; mimalloc supplies public allocation functions.
ReliefOS extensions live in `/usr/lib/reliefos/libreliefos.so.2`; the compatible
`libleonos.so.2` object name is retained during migration. The build driver writes
`/usr/lib/reliefos:/usr/lib/leonos:/lib:/usr/lib` as the application search path. A private ReliefOS ABI note
is not required for Linux ELF programs. Static binaries use musl CRT and the
same allocator policy.

The old `libleonos.so.1` alias and native loader are not distributed. Native
binaries and API packages using the retired root layout must be rebuilt for
the current paths; there is no automatic migration of stored paths.

## Syscall subset

ReliefOS keeps Linux-compatible syscall numbers for the current user ABI:

- File I/O: `read`, `write`, `open`, `close`, `stat`, `fstat`, `lseek`
- Filesystem mutation: `mkdir`, `unlink`, `rmdir`, `rename`
- Process and scheduler: `execve`, `wait4`, `exit`, `getpid`, `sched_yield`,
  `nanosleep`
- Working directory: `getcwd`, `chdir`
- Memory: `mmap`, `munmap`
- Device and system extensions: `ioctl`

Standard libc wrappers come from `third_party/musl`. ReliefOS extensions live in
`userland/runtime`. Canonical kernel wire definitions are in `kernel/reliefnt/include/uapi`; the
complete per-call status is in `LINUX_ABI_SYSCALLS_2026-09-07.csv`. `mmap`
supports anonymous private mappings and private file mappings; `munmap`
supports whole or partial unmapping.

## Shared POSIX porting surface

Native x86-64 SysV shared memory uses `shmget`/`shmat`/`shmdt`/`shmctl`
and the Linux v6.14 LP64 wire records. Key lookup, generation IDs, DAC,
IPC_SET/STAT/RMID, IPC_INFO, SHM_INFO/STAT/STAT_ANY and LOCK/UNLOCK have
production host-fixture and build evidence. Registry pages and each mapped PTE
own separate references; RMID hides the key but retains an attached ID until
the final VMA is released. Split VMAs count in `shm_nattch`, matching the native
Linux reference; `shmdt` retires the selected original attachment's remaining
pieces. Fork obtains independent shared references; CLONE_VM shares its MM.

The current registry reports finite limits through IPC_INFO: 4096 segments,
256 MiB per segment and 65536 total pages. Pages are eagerly zeroed/resident;
SHM_NORESERVE does not add demand paging. SHM_HUGETLB returns ENOSYS, as in
Linux without CONFIG_HUGETLBFS. The existing W^X policy permits readonly
SHM_EXEC mappings and rejects writable executable attachment with EACCES.
SHM_LOCK enforces owner/CAP_IPC_LOCK and the process-wide RLIMIT_MEMLOCK
(initial soft/hard defaults 8 MiB); residency does not imply a swap subsystem.
Namespaces, IPC sysctl and `/proc/sysvipc/shm` are outside this implementation.
The same musl probe has run on Linux only; ReliefOS guest and upstream
dmix/dsnoop acceptance remain H6/U1/U6 gates.

`libreliefos.so.2` provides the ANSI curses subset used by `sl`; the same
implementation is also exported by the compatible `libleonos.so.2`.
Applications include `<curses.h>` or `<ncurses.h>` from the SDK. This is a
ReliefOS terminal API, not binary compatibility with host ncurses.

Standard file, process, pthread, signal and socket APIs come from musl without
private POSIX adapters. Raw syscalls return negative errno; musl converts
errors according to each libc function's contract. For example raw getcwd
returns a byte count, while libc getcwd returns a pointer.

The kernel supports native signal frames, red-zone preservation, alternate
stacks and FP/SIMD restoration for the tested configurations. `forkpty` uses
Linux ioctl encodings, setsid and TIOCSCTTY. The native regression suite covers
pthread contention/cancellation and Unix descriptor passing. This does not
certify complete signal, PTY, wait, clone, socket or SMP behavior; consult
[the implementation ledger](LINUX_ABI_PROGRESS_2026-09-08.md).

`stat`, `fstat` and `lstat` use Linux x86-64 layouts, including the 144-byte raw
stat record. Compact ReliefOS metadata is available through explicitly named
`leonos_stat_legacy` and `leonos_fstat_legacy` extension functions only.

See [Syscalls](SYSCALLS.md) for extension interfaces and the audit CSV for the
full native Linux v6.12 syscall scope.

## Time Synchronization ABI

Time synchronization is a userland service, not a kernel ioctl. The libc
helper `leonos_time_ntp_sync()` in `include/leonos/system.h` restarts the
root-owned OpenRC service `reliefos-ntp`, then polls the fresh notification
file `/run/reliefos/ntp-state` (mode-checked, root-owned, ≤120 s old) and
confirms an active PLL via `adjtimex(2)` before reporting `valid=1`. The
result returns the selected server, resolved IPv4 address, network status,
and validated Unix seconds from `CLOCK_REALTIME`. Ordinary applications
change system time only through the standard `settimeofday`/`adjtimex`
syscalls subject to the kernel's permission checks, not through a ReliefOS
private control.

## Device model

The runtime exposes a synthetic devfs namespace. Common nodes are
`/dev/null`, `/dev/zero`, `/dev/full`, `/dev/random`, `/dev/urandom`,
`/dev/tty` (`tty1`–`tty6`), `/dev/console`, `/dev/kmsg`,
`/dev/ptmx` and `/dev/pts/<id>`, `/dev/fb0`, `/dev/gpu`, `/dev/dsp` (aliased
`/dev/audio`), `/dev/serial0` (aliased `/dev/ttyS0`), `/dev/ethernet0`,
`/dev/rtc`, `/dev/driverctl`, `/dev/shm0`, Linux-style storage nodes such as
`/dev/sda` and `/dev/nvme0n1`, and `/dev/input/event0`/`event1`.
`/dev/stdin`, `/dev/stdout`, and `/dev/stderr` alias the current process
streams. `/dev/input`, `/dev/disk`, `/dev/pts`, and `/dev/shm` are
directories and are enumerated through normal directory syscalls. Device
nodes are synthetic and are not stored in the filesystem image.

Framebuffer, audio, input, disk, and PTY libc helpers open their
corresponding `/dev` node and issue the device ABI (Linux fbdev, evdev,
block, and OSS ioctls, plus the ReliefOS `LEONOS_FBIOBLIT` and
`LEONOS_IOCTL_GPU_*` extensions). PTY users can use the
standard `posix_openpt/openpty/forkpty` functions; master/slave data flows
through normal `read/write/poll` descriptors. Calls made by old
applications with the historical descriptor 3 are translated to the matching
node by libc, so existing binaries continue to work while using the devfs
namespace. System applications query the complete hardware inventory through
`leonos_device_list()`, which talks to the devmand service over its AF_UNIX
protocol (`LEONOS_DEVMAND_MSG_DEVICE_LIST`); there is no device-list ioctl.

PCM interfaces and their current verification limits are described in the Audio
ABI section below. `/dev/audio` remains a compatibility alias for `/dev/dsp`;
it does not select the traditional OSS mu-law default.

## Driver Module ABI

Loadable Ring 0 driver modules use the public definitions in
`include/leonos/driver.h`. Ordinary applications list and control drivers
through `system_driver_list()`/`system_driver_control()`
(`include/leonos/devmgr_service.h`), which send versioned requests to the
devmand service over its AF_UNIX socket; devmand in turn issues the kernel's
`LEONOS_DRIVER_CONTROL_IOCTL` on `/dev/driverctl`, and the kernel permits
control actions only for administrator tasks. There are no legacy
`LEONOS_IOCTL_DRIVER_*` ioctls.

The kernel loads unsigned ELF64 `ET_REL` files from `/drivers` after the
root filesystem is mounted. The complete binary format, restricted kernel API,
and persistent `/etc/reliefos/drivers.conf` policy are documented in
[Drivers](DRIVERS.md).

## Kernel Debug Module ABI

`/usr/lib/reliefos/kerneldebug.sys` is a built-in-only x86_64 little-endian `ET_REL`
module. New builds contain a `.note.reliefos.kerneldebug` ELF note; the legacy
`.note.leonos.kerneldebug` section remains accepted. Both notes are owned by
`LEONKDBG`, type `0x4c4b4447`, ABI `1`, and the fixed entry-name hash. The
loader accepts only PIC-free kernel sections and the `NONE`, `64`, `32`,
`32S`, `PC32`, and `PLT32` relocations; dynamic segments, TLS, IFUNC,
undefined symbols, W+X sections, and unknown sections are rejected.

The module receives the fixed `reliefos_kernel_debug_api` table declared in
`kernel/reliefnt/kernel/reliefnt/include/reliefnt/kernel_debug.h`. It provides ostui output/input,
TSC timing, controlled syscall/ioctl benchmark callbacks, and explicit
continue, reboot, and shutdown operations. A valid module owns the diagnostic
session; the kernel's minimal menu is only a recovery path for a missing or
rejected module.

The one-shot marker is `/boot/reliefos/state/kerneldebug.next`; the loader also
accepts `/boot/leonos/state/kerneldebug.next` for old installations. The loader consumes
and deletes it before validating its contents, preventing repeated entry after
an interrupted or malformed debug boot. The persistent activation flag is
`/var/lib/reliefos/kerneldebug.enabled`.

## Appearance ABI

The runtime UI appearance is a Desktop-owned state. `Metro` is the default
(`LEONOS_UI_THEME_METRO`); `LEONOS_UI_THEME_WIN95` restores the legacy Win95
palette and bevelled controls. The global `/etc/reliefos/display.conf`
`theme=` key remains the boot/default style used before a user session is
available, including early framebuffer output and bugcheck rendering.

Per-user personalization is saved separately in
`/home/<name>/appearance.conf`. `struct leonos_appearance_state` and
`struct leonos_appearance_request` carry the active theme, independent Metro
and Win95 basic color scheme IDs, a wallpaper display mode, and a wallpaper BMP
path. Wallpaper BMP decoding is bounded to 1280 x 720 and accepts
uncompressed 24-bit or 32-bit BMP files.

The independent color scheme IDs are `Blue`, `Teal`, `Green`, `Purple`,
`Red`, `Graphite`, and `Kawaii Pink`. The `Kawaii Pink` scheme is available
for both Metro and Win95; each theme keeps its own palette and persisted
`metro.color` / `win95.color` value. Its configuration value remains `pink`
for compatibility.

Appearance state travels over the windowd AF_UNIX protocol, not ioctls:
clients call the libc helpers in `userland/runtime/src/wind.c`, which exchange
`LEONOS_WIN_MSG_APPEARANCE_STATE` and `LEONOS_WIN_MSG_APPEARANCE_REQUEST`
messages with the window server. Logged-in user tasks may submit an
appearance request; the window server polls and publishes the updated state
through the paired messages, writes the current user's `appearance.conf`,
reloads the wallpaper, then sends `LEONOS_GUI_APP_EVENT_THEME_CHANGED` to active
application windows. The event carries the theme in `x`, the Metro color
scheme in `y`, and the Win95 color scheme in `dx`.

## Authentication ABI

Multi-user state is exposed through `include/leonos/auth.h` and libc wrappers
in `userland/runtime/src/auth_accounts.c`, which read the standard
`/etc/passwd` and `/etc/shadow` files via `getpwuid`/musl (rooted at the
target install tree for offline tools).

User-facing roles are:

- `LEONOS_AUTH_ROLE_ADMIN`
- `LEONOS_AUTH_ROLE_USER`

Roles are derived from the account UID (`uid == 0` is admin). Usernames are
lowercase letters, digits, and `_`; passwords must be non-empty. Login and
password changes go through the standard PAM stack (`linux-pam` with the
`pam_leonos_password` module); there are no `LEONOS_AUTH_IOCTL_*` requests —
the private auth ioctl family was removed. The legacy
`/var/lib/leonos/users.db` v2 record is only probed during account migration.

Task snapshots now include `uid`, `role`, `session_id`, and `username`.
Children inherit identity and current directory from the parent task.
File access decisions are made in the kernel: `kernel/reliefnt/fs/permissions.c`
compares a task's filesystem UID/GID and role against the permission value the
storage layer reports for the path. Protected service work is gated by kernel
task flags (`TASK_FLAG_SERVICE`, `TASK_FLAG_WINDOW_SERVER`) and by
`LEONOS_AUTH_ROLE_ADMIN`. There is no authorization opcode set or policy
callback in the ABI; the former `LEONOS_AUTHZ_*` request record was an internal
RPC envelope and is not published.

## Filesystem permission ABI

POSIX permissions reach userland through the Linux ABI only: `stat`, `chmod`,
`chown` and their `*at` variants. `struct leonos_fs_acl` and the
`leonos_fs_acl_*` helpers in `include/leonos/fs.h` are a libc-level convenience
API implemented on top of those same calls (see `userland/runtime/src/libc.c`);
they are not ioctls and the kernel exposes no ACL ioctl. On FAT32 and exFAT the
kernel stores the resulting mode, owner and group in the hidden `LEONACL.SYS`
sidecar owned by `kernel/reliefnt/drivers/bootstrap/storage/storage_sidecar.c`; on ext2 it uses
the native inode fields. See
[KERNEL_USERSPACE_BOUNDARIES.md](KERNEL_USERSPACE_BOUNDARIES.md).

`struct leonos_fs_acl` contains the owner uid and up to
`LEONOS_FS_ACL_MAX_ACE` ACE rows. Supported principals are Owner, System,
Administrators, Users, and Everyone. Supported permission bits are Read/List,
Write/Create, Execute/Traverse, Delete, and Manage Permissions. ACL rows only
grant allowed permissions; an unchecked permission bit means no grant.

## Block Storage ABI

Disk tools operate on Linux-style `/dev/sdX`, `/dev/sdXN`, `/dev/nvmeXnY`, and
`/dev/nvmeXnYpZ` nodes. Legacy `/dev/diskN` and `/dev/diskNpN` nodes are not
created.
They obtain capacity
and sector geometry through `<linux/fs.h>` `BLKGETSIZE64` and `BLKSSZGET`,
update GPT metadata with aligned raw I/O followed by `BLKRRPART`, and mount
FAT32, exFAT, ext2 compatibility, or ext4 volumes with `<sys/mount.h>`
`mount(2)` and `umount2(2)`. New installations use ext4; ext4-specific
formatting and checking are delegated to upstream `mkfs.ext4`/`fsck.ext4`.
The SDK no longer defines ReliefOS-specific disk-management ioctl records.

## Boot handoff ABI

`include/leonos/boot_handoff.h` describes exactly one thing: the record the EFI
loader hands to `kernel.sys`. `struct leonos_boot_handoff` carries the Multiboot2
and framebuffer facts, the memory maps, and the `loader`, `kernel` and
`installer_root` module ranges. `LEONOS_BOOT_HANDOFF_VERSION` is `7`.

Version 7 removed the second boot module and every table that used to cross it:
the middlelayer module range and API pointer, the kernel service table, the
mount-policy record, the VFS resolve record, and the raw-device catalog query.
Those services are now ordinary kernel-internal calls compiled into
`kernel.sys`, so no wire format is needed for them. A kernel that sees any other
handoff version rejects it in `boot_handoff_is_current()` rather than reading a
layout it does not understand.

Kernel code owns hardware probing, interrupts, page tables, physical memory,
scheduling, user pointer validation, storage block I/O, exFAT/FAT32/ext2
mutation, path resolution and the final DAC decision. The storage layer in
`kernel/reliefnt/drivers/bootstrap/storage/` owns on-disk metadata, including `LEONACL.SYS`.
The full ownership map is in
[KERNEL_USERSPACE_BOUNDARIES.md](KERNEL_USERSPACE_BOUNDARIES.md).

## Path resolution

Path normalization lives in the kernel: `fs_permissions_resolve()` and
`fs_permissions_resolve_flags()` in `kernel/reliefnt/fs/permissions.c` combine the
task's current directory with the input, walk components and symlinks, and check
directory search permission on the way. Whether the final symlink is followed is
decided by the syscall the caller made (for example `O_NOFOLLOW`); more than 40
traversals returns `ELOOP`. There is no resolution ABI and no separate
resolution service.

## Device catalog

`userland/apps/device-agent` is the producer: it reads `/dev` through
`leonos_readdir()`, classifies each node into `struct leonos_device_info`
(display, input, storage, audio, serial, network, system), and answers clients
over the devmand IPC that `userland/runtime/src/devmand_client.c` wraps as
`leonos_device_list()`. Driver records come from `/proc/leonos-drivers`.

## Machine Identity ABI

`leonos_machine_identity()` (`include/leonos/system.h`, implemented in
`userland/runtime/src/procsys.c`) fills `struct leonos_machine_identity` from
procfs: it reads the SMBIOS system UUID exported at
`/sys/class/dmi/id/product_uuid` and marks
`LEONOS_MACHINE_IDENTITY_FLAG_PLATFORM_UUID` when a valid 36-character UUID
is present. There is no `LEONOS_IOCTL_MACHINE_IDENTITY`; the private ioctl
was removed in favor of the sysfs view. License code uses the platform UUID
as the primary machine binding with the boot GPT GUID pair as fallback when
firmware does not provide a valid UUID. Network adapter MAC addresses are
deliberately excluded from the license machine ID so hot-adding or removing
e1000 hardware does not invalidate an existing activation.

## Network ABI

Sockets use the standard Linux socket syscalls: the kernel dispatches
`socket`, `connect`, `accept`/`accept4`, `bind`, `listen`, `send`/`recv`,
`sendto`/`recvfrom`, `sendmsg`/`recvmsg`, `shutdown`, and option queries to
the AF_UNIX and AF_INET backends (`syscall_socket_dispatch` in
`kernel/reliefnt/kernel/reliefnt/syscall.c`). `include/leonos/net.h` keeps a versioned
compatibility surface — `leonos_net_config()`, `leonos_net_dhcp_renew()`,
DNS, ping, TCP helpers — but its libc implementations in
`userland/runtime/src/netsock.c` are ordinary socket-fd clients:
`leonos_socket_tcp()` is `socket(AF_INET, SOCK_STREAM, 0)` and the
read-only queries issue `LEONOS_NET_CONTROL_IOCTL` on an `AF_INET` datagram
fd; there are no `LEONOS_IOCTL_NET_*` request codes.

Network lifecycle is an OpenRC service: DHCP renew restarts `reliefos-ntp`'s
companion `reliefos-dhcp` (root-owned udhcpc), whose hook publishes the lease
atomically at `/run/reliefos/dhcp-lease`; clients verify the file's ownership
and mode before trusting it. Connection inventory uses the control ioctl and
is filtered by identity: administrators and trusted service tasks see all
sockets, while normal users see only sockets owned by their uid.

`include/leonos/http.h` adds a libc HTTP client on top of those sockets:
`leonos_http_get`, `leonos_http_request`, and `leonos_http_resolve_url`.
It follows bounded redirects, reports final URL/status/content type, copies
response headers, decodes chunked transfer bodies, and returns truncation flags
for callers with small buffers. `httpget.elf`, `browser.elf`, and
`downloadmgr.elf` use this library for `http://` and `https://` traffic;
lower-level tools such as `ping.elf` and `netctl.elf` continue to use
ICMP/DHCP/DNS/socket status APIs directly. HTTPS uses a TLS 1.2 Mbed TLS client
profile, a bundled CA store, hostname validation, and a valid system clock. TCP
server/listener sockets, UDP sockets, cookies, cache, and full TCP window
management are still out of scope for this ABI version.

## Audio ABI

The registered-card audio path implements the Linux v6.14 native x86-64 sound
UAPI exported through the SDK. `/dev/snd/controlC<N>` and
`pcmC<N>D<M>p/c` are generated from live card capabilities (N=0..31, M=0..7);
unsupported devices or directions are absent. Character major 116 uses control
minor `N*32`, playback `N*32+16+M`, and capture `N*32+24+M`. Device access uses
the existing owner/group/other DAC rules. A sound open owns its PCM/control
state through an open-file description; dup, fork, SCM_RIGHTS and in-flight I/O
retain it until the final release.

PCM supports interleaved read/write and shared data/status/control mmap,
HW_REFINE/HW_PARAMS/HW_FREE, SW_PARAMS, INFO/CHANNEL_INFO, STATUS/DELAY,
protocol/timestamp requests, PREPARE/START/DROP/DRAIN, RESET/XRUN,
SYNC_PTR/HWSYNC, REWIND/FORWARD and direct-fd LINK/UNLINK. Formats, rates,
channels and geometry come from hardware caps; noninterleaved access, PAUSE
and synchronized start are not advertised. Mapped data prevents destructive
HW_PARAMS/HW_FREE. Status pages are read-only; control updates are validated
against the actual ring boundary. Unknown PCM/control commands return ENOTTY
before examining their encoded argument. Known requests retain their documented
state and user-memory errors.

Control supports card/PCM enumeration, fixed ELEM_LIST/INFO/READ/WRITE,
OFD-owned LOCK/UNLOCK, SUBSCRIBE_EVENTS and DB_SCALE TLV_READ. Each subscribed
description retains a merged pending mask for every fixed element in notification
order. User-defined ELEM_ADD/REPLACE/REMOVE return EOPNOTSUPP; TLV_WRITE/COMMAND
return ENXIO. Raw MIDI and hwdep are outside this implemented interface.
`/dev/mixer` reads/writes the same registered controls through OSS masks and
stereo percentages. `/dev/dsp` provides negotiated S16_LE/S8/U8 where supported,
capture/playback and hardware-backed queue/fragment/pointer/trigger operations,
RESET, blocking SYNC and NONBLOCK. The adapter preserves partial-frame byte tails,
uses DROP when disabling a trigger, and retains hardware leases until OFD close.
It does not advertise OSS mmap or software format/rate converters.

When `/dev/dsp` binds to an ABI-v1 AC97/ES1371 backend, it holds an exclusive
backend-generation lease through OFD close. This path provides stereo S16_LE
playback, real rate negotiation, short writes, queue queries and SYNC; GETCAPS
returns zero. Capture, RESET, hardware pointer queries and disabling output
trigger are unsupported. Loading a replacement module does not redirect an
already-open description, and v1 registration creates no ALSA card.

These paths have source, production host-fixture and root kernel/runtime/SDK build
evidence. Standard-libc HDA guest playback, capture, mapped lifetime and control
restoration have also passed in isolated QEMU runs. This covers those probe
modes; the complete guest contract, unmodified upstream ALSA applications and
physical audio remain H6/U1/U6 gates. See
`docs/superpowers/progress/2026-10-02-intel-hda-audio-status.md` for the audit.

The A6 standard-libc probe is built for the host and the relocatable musl SDK
with `python3 tools/test_linux_audio.py --build-guest`. It prints ABI layouts
without DMA addresses, uses a global ten-second deadline, preserves short
byte/frame transfers across `EINTR`/`EAGAIN`, rejects zero progress, and logs
control restoration attempts on normal and error exits. The fixed support and
unsupported-operation matrix is in `docs/audio-abi-support.json`; it does not
turn a host build into guest or physical audio evidence.

The private `LEONOS_IOCTL_AUDIO_*` ioctls were removed and the
`include/leonos/audio.h` wrapper declarations no longer have libc
implementations. Existing autoloaded driver modules include `ac97.drv`, which
supports QEMU's Intel ICH AC'97 controller, while `es1371.drv` supports
VMware's Ensoniq AudioPCI ES1371 controller. Both accept 16-bit,
stereo PCM at 8000–48000 Hz selected with `SNDCTL_DSP_SETFMT`,
`SNDCTL_DSP_CHANNELS`, and `SNDCTL_DSP_SPEED`. The AC'97 backend keeps a
persistent DMA ring and reports non-blocking short writes with
`EAGAIN`/`LEONOS_AUDIO_STATUS_WOULD_BLOCK` semantics; callers should retain
and retry the unwritten tail. `doom.elf` and the built-in `wavplay.elf`
test tone use native 48000 Hz output to avoid emulator-side resampling.
`wavplay.elf` also opens matching PCM WAV files and is the default `.wav`
handler.

## Application Services

## PortableGL Rendering ABI

`/usr/lib/libportablegl.so.1` provides the PortableGL 0.101 API with the
ReliefOS ABI-v1 window wrapper declared by `leonos/pgl.h`. The wrapper manages a
GUI window, an ABGR32 color buffer and a D24S8 depth/stencil buffer, and submits
frames through `leonos_gui_present_window`. Contexts are single-process and
single-current; callers must handle resize events through
`leonos_pgl_process_event` before drawing the next frame. The SDK includes the
matching `portablegl.h`, `leonos/pgl.h`, shared library and static archive. The
system build limits one draw call to 50,000 output vertices, leaving the
renderer usable within the current user address-space budget.

The launcher library in `leonos/launch.h` owns user-facing file launch policy.
It supports `.lnk` shortcuts, built-in program aliases, and persistent extension
associations stored in `/etc/reliefos/fileassoc.cfg`. Settings can edit the common
associations for `.txt`, `.md`, `.html`, `.htm`, `.bmp`, `.wav`, and `.hlp`.
The default `.hlp` handler is `/usr/lib/reliefos/apps/oshlp/oshlp.elf`; it accepts
`oshlp.elf <file.hlp> [doc.id]` and opens a Markdown page inside a ReliefOS help
container.

Current companion applications:

- `downloadmgr.elf`: uses the libc HTTP client and saves HTTP/HTTPS downloads to
  the current user's `/home/<name>/downloads` directory.
- `imageview.elf`: opens uncompressed 24/32-bit BMP/DIB files and PNG files,
  supports Fit/1x/2x zoom, and can move to previous/next supported images in the
  same directory.
- `wavplay.elf`: plays 16-bit stereo PCM WAV files through the active audio
  driver, or a built-in test melody when started without a file.
- `oshlp.elf`: opens ReliefOS `.hlp` help containers from `/usr/share/doc/reliefos` or any path
  passed by another app. The help viewer uses the current system language as its
  default but language changes inside the window are local to that process.
- `servicemgr.elf`: service manager UI. It lists the OpenRC services
  (`reliefos-windowd`, `reliefos-dhcp`, `reliefos-session`, `reliefos-device`,
  `reliefos-ntp`), reads enabled state from `/etc/runlevels/default/`, and
  starts/stops/restarts/enables them by running `rcctl.elf` (elevated through
  the sudo path for non-root callers). The retired `serviced.elf` runtime,
  `/run/leonos/services.state`, `/run/leonos/services.cmd`, and
  `/etc/leonos/services.cfg` no longer exist.

Service behaviour is now OpenRC policy: `reliefos-dhcp` runs BusyBox `udhcpc`
with the publishing hook, and `reliefos-ntp` runs BusyBox `ntpd` against
`pool.ntp.org`; its hook publishes `/run/reliefos/ntp-state` and `ntpd` steers
the kernel clock through the standard `adjtimex` PLL. The desktop taskbar
reads the live network/clock state instead of the old `network_icon` and
`rtc_clock` toggles.
## POSIX/Linux ABI migration status

The public `stat`, `fstat`, and `lstat` symbols use the musl/Linux x86-64
`struct stat` layout. ReliefOS first-party code that needs the compact metadata
record must call the explicitly named `leonos_stat_legacy` or
`leonos_fstat_legacy` functions. Linux fbdev applications can open `/dev/fb0`,
map its framebuffer, and use the `FBIOGET_VSCREENINFO`, `FBIOGET_FSCREENINFO`,
and `FBIOPUT_VSCREENINFO` requests from `<linux/fb.h>`.

### Fixed virtual consoles and atomic presentation

See [Console, VT and sessions](TTY_VT.md) for tty1–tty6, VT_GETSTATE,
VT_ACTIVATE, VT_WAITACTIVE, KDGETMODE/KDSETMODE, controlling-terminal permissions,
and the bounded text history. `<leonos/fb.h>` adds LEONOS_FBIOBLIT (0x46f2),
using the fixed 32-byte leonos_fb_present record. Inactive graphical callers
receive EAGAIN. `<leonos/device.h>` adds LEONOS_EVIOCSVT (0x400445f0), a uint32
graphical-origin filter shared by an evdev open file description; zero keeps
the raw stream. LEONOS_VT_GETGENERATION (0x800856f0) returns a uint64 display
generation so a compositor can detect switches that happened while it was
paused. These three requests are ReliefOS extensions, not Linux ioctl ABI.

The userspace IPC library retains at most one partially transmitted frame per
nonblocking connection. A successful send means the frame is accepted; event
loops must call `leonos_ipc_flush(fd)` until it succeeds. EAGAIN from a new send
rejects that new frame while preserving the earlier tail. Later sends and
receives also attempt to drain it. Close with `leonos_ipc_close` to release the
pending state. Framing and SCM_RIGHTS association remain unchanged.
