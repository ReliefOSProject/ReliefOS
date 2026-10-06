# Syscalls

ReliefOS targets the native Linux v6.12 x86-64 user ABI. The current full
status is recorded in `LINUX_ABI_SYSCALLS_2026-09-07.csv` and
`LINUX_ABI_PROGRESS_2026-09-08.md`. The tables below are an extension reference
and historical subset, not a complete compatibility claim.

## Login system information

The dynamic MOTD uses `uname`, `time`/`localtime_r`, Linux `sysinfo`,
`statvfs`, and the Linux `SIOCGIFCONF`/`SIOCGIFFLAGS` network ioctls.
Terminal width comes from `TIOCGWINSZ`, including `PAM_TTY` when PAM redirects
stdout to a pipe. No private syscall is used or added.

`sysinfo.loads` now reports real 1/5/15-minute exponentially weighted runnable
load averages with Linux's Q16 scaling. The BSP samples non-idle RUNNING/READY
tasks every five seconds under the scheduler lock. Current BLOCKED tasks are
sleepers/waiters; the scheduler does not have a separate uninterruptible I/O
state. Memory percentage is allocated physical RAM (`totalram - freeram`);
root usage excludes blocks reserved from ordinary users, like `df`.

`/etc/motd` and `/etc/motd.zh_CN` hold the short, editable link footer.
`/usr/lib/reliefos/motd` honors the PAM user's `.hushlogin` before running the
standard-interface helper `motd-status`. It selects one language using
`LC_ALL`, `LC_MESSAGES`, then `LANG`. Versions, times and state are read at
login; unavailable measurements are labelled rather than invented. UTF-8
cell widths determine aligned 3/2/1-column layouts and wrapping.

## Entry Convention

musl enters the kernel with the native `syscall` instruction. The ReliefOS
extension assembly helpers in
`userland/runtime/src/syscall.S` translate C call arguments into the syscall ABI:

- `rax`: syscall number.
- `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9`: arguments 0 through 5.
- `rax`: return value.

Return values follow the kernel convention:

- `>= 0`: success or byte/count/id result.
- `< 0`: negative errno value.
- Unknown syscall numbers return `-ENOSYS`.

The public userland numbers and wrappers are in:

- `userland/runtime/include/reliefos/syscall.h` (the old `leonos/` header forwards to it)
- `userland/runtime/src/libc.c`

The kernel-side numbers and errno constants are in:

- `kernel/reliefnt/kernel/reliefnt/include/reliefnt/syscall.h`

## Implemented Syscall Table

| Number | Name | libc wrapper | Notes |
| ---: | --- | --- | --- |
| 0 | `read` | `read` | Reads files, directories, stdin PTY input, and directory entries. |
| 1 | `write` | `write` | Writes files, stdout/stderr console output, and PTY output. |
| 2 | `open` | `open` | Opens files, directories, and synthetic `/dev/*` device nodes. |
| 3 | `close` | `close` | Closes task file descriptors above the stdio/device range. |
| 4 | `stat` | `stat` | Stats a path into `struct leonos_stat`. |
| 5 | `fstat` | `fstat` | Stats an open file descriptor. |
| 8 | `lseek` | `lseek` | Supports files and directory cursors. |
| 9 | `mmap` | `mmap` | Supports private anonymous/file mappings and writable `/dev/fb0` framebuffer mappings. |
| 11 | `munmap` | `munmap` | Supports whole and partial unmapping of existing VMAs. |
| 16 | `ioctl` | `ioctl` | Device-ABI extension point: Linux fbdev/block/evdev/OSS/termios requests, `LEONOS_FBIOBLIT`, GPU extensions on `/dev/gpu`, net control on an AF_INET fd, and driver control on `/dev/driverctl`. |
| 24 | `sched_yield` | `sched_yield` | Yields the current task if another task can run. |
| 29 | `shmget` | `shmget` | Native keyed/private SysV segment creation and lookup; finite limits via IPC_INFO. |
| 30 | `shmat` | `shmat` | Native shared PTE references; RDONLY/RND/REMAP and readonly EXEC; W^X policy applies. |
| 31 | `shmctl` | `shmctl` | LP64 STAT/SET/RMID, INFO/STAT_ANY, owner/capability and memlock checks; native guest probe passes 83 checks. |
| 35 | `nanosleep` | `sleep_ms` | libc passes milliseconds; kernel also accepts a Linux-like timespec pointer. |
| 39 | `getpid` | `getpid` | Returns the current scheduler PID. |
| 57 | `fork` | `fork` | Creates a copy-on-write child; the child receives zero. |
| 58 | `vfork` | `vfork` | Currently has the same copy-on-write behavior as `fork`. |
| 59 | `execve` | `execve` | Replaces the current process image with an ELF program. |
| 60 | `exit` | `exit` | Releases process-owned files, windows, PTYs, and exits with a code. |
| 61 | `wait4` | `wait4` | Waits for a child and writes a Linux-style shifted status. |
| 67 | `shmdt` | `shmdt` | Detaches remaining split pieces by original attach identity; final RMID release returns backing pages. |
| 79 | `getcwd` | `getcwd` | Copies the task current directory. |
| 80 | `chdir` | `chdir` | Changes the task current directory after path lookup. |
| 82 | `rename` | `rename` | Renames exFAT, FAT32, ext4, or legacy ext2 files/directories within one filesystem. |
| 83 | `mkdir` | `mkdir` | Creates an exFAT, FAT32, ext4, or legacy ext2 directory. |
| 84 | `rmdir` | `rmdir` | Removes an empty exFAT, FAT32, ext4, or legacy ext2 directory. |
| 87 | `unlink` | `unlink` | Removes an exFAT, FAT32, ext4, or legacy ext2 file. |

## GPU Calls

GPU extensions use syscall 16 (`ioctl`) on a descriptor opened for
`/dev/gpu` (legacy fd 3 calls are translated to that node by libc). The
commands in `include/leonos/gpu.h` are `LEONOS_IOCTL_GPU_INFO`
(`0x4c475001`), `CREATE` (`0x4c475002`), `RENDER` (`0x4c475003`), `DESTROY`
(`0x4c475004`) and `DIAGNOSTICS` (`0x4c475005`). DIAGNOSTICS returns the latest
device render-failure snapshot only to its owner. Requests carry exact structure
size and ABI version 1; pointers are 64-bit user addresses. Invalid ranges return
`-EFAULT`, invalid versions or counts return `-EINVAL`, and unavailable hardware
returns a negative SVGA status.
Only the creator can use a render handle. See [SVGA3D.md](SVGA3D.md) for details.

## File and Directory Calls

Paths use Unix syntax such as `/usr/lib/reliefos/apps/desktop/desktop.elf`. Relative
paths are resolved against the task current directory by
`fs_permissions_resolve()` in the kernel, which also checks directory search
permission on every component it walks. Inputs containing `:`
are rejected.

Open flags are defined in `include/leonos/fs.h`:

- `LEONOS_O_RDONLY`
- `LEONOS_O_WRONLY`
- `LEONOS_O_RDWR`
- `LEONOS_O_CREAT`
- `LEONOS_O_TRUNC`
- `LEONOS_O_APPEND`

Seek modes are:

- `LEONOS_SEEK_SET`
- `LEONOS_SEEK_CUR`
- `LEONOS_SEEK_END`

Directory reads return one `struct leonos_dir_entry` per successful `read`.
The higher-level `leonos_list_dir()` libc helper can also list a directory into a
caller-provided array.

## Memory Calls

`mmap(addr, len, prot, flags, fd, offset)` records a task VMA and maps pages
according to the mapping type.

Supported protection bits:

- `LEONOS_PROT_READ`
- `LEONOS_PROT_WRITE`
- `LEONOS_PROT_EXEC`

Supported mapping flags:

- `LEONOS_MAP_PRIVATE`
- `LEONOS_MAP_FIXED`
- `LEONOS_MAP_ANONYMOUS`

Anonymous mappings require:

- `LEONOS_MAP_PRIVATE | LEONOS_MAP_ANONYMOUS`
- `fd == -1`
- `offset == 0`

File mappings currently require:

- `LEONOS_MAP_PRIVATE`
- no `LEONOS_MAP_ANONYMOUS`
- page-aligned `offset`
- readable file descriptor
- `prot == LEONOS_PROT_READ`

File mappings are lazy. The page fault handler maps file-backed pages on first
read. Writable file-backed mappings are still `-ENOSYS`.

If a page fault cannot be recovered by the lazy mapping path, kernel-mode faults
and the desktop window-server fault path still go to bugcheck. Ordinary Ring-3
applications are terminated instead: the kernel releases their open file state,
destroys their GUI/PTY ownership, records exit code `0x8000000e`, schedules the
next runnable user task, and posts an `Application Page Fault` desktop window
with PID, path, user, fault address, error flags, RIP/RSP/RBP, general registers,
CS, RFLAGS, and tick details.

`munmap(addr, len)` requires a page-aligned start address inside user space and
an existing VMA that fully covers the requested range. It can trim the front or
back of a VMA, remove a whole VMA, or split a VMA in the middle.

The libc heap allocator uses anonymous private `mmap` arenas and may release
large page-aligned free blocks with `munmap`.

## Process and Scheduler Calls

`fork()` creates a copy-on-write child with inherited file descriptors. The
parent receives the child PID and the child receives zero. `execve(path, argv,
envp)` validates and copies the user argument vectors, then replaces the
current process image while preserving its PID, working directory, identity,
PTY and descriptors not marked `FD_CLOEXEC`. Use `fork()` followed by
`execve()` to start a child; GUI applications can use `leonos_spawn_argv()`.

`wait4(pid, status, options, rusage)` waits for a child process. `options` and
`rusage` are accepted for ABI shape but are not a full Linux wait
implementation.

`nanosleep` has two call shapes:

- libc `sleep_ms(ms)` calls syscall 35 with milliseconds in argument 0.
- If argument 0 points to a readable 16-byte object, the kernel treats it as
  seconds and nanoseconds and converts it to scheduler ticks.

## Ioctl Groups

`ioctl(fd, request, arg)` is the extension point for device ABIs. The
private ReliefOS ioctl multiplexers (auth, GUI windows, appearance, system info,
device list, audio, network sockets, text layout, PTY, signal) have been
removed; those services now use Linux UAPI device ioctls, standard syscalls,
or AF_UNIX service protocols. What the kernel still accepts, keyed by the
`/dev` node the descriptor was opened on:

- `/dev/fb0`: Linux fbdev `FBIOGET_VSCREENINFO`, `FBIOGET_FSCREENINFO`,
  `FBIOPUT_VSCREENINFO`, `FBIOPAN_DISPLAY`, plus the ReliefOS presentation
  extensions `LEONOS_FBIOGET_CAPABILITIES`, `LEONOS_FBIOUPDATE_REGION`, and
  `LEONOS_FBIOBLIT` (the blit request windowd uses to compose; it is
  restricted to the active graphical VT owner).
- `/dev/gpu`: the GPU ABI above (`LEONOS_IOCTL_GPU_*`).
- `/dev/driverctl`: `LEONOS_DRIVER_CONTROL_IOCTL` with a fixed-size
  `struct leonos_driver_control`; administrator tasks only. Ordinary clients
  go through devmand instead (`system_driver_list()`/
  `system_driver_control()`).
- `/dev/sdX`, `/dev/sdXN`, and `/dev/nvmeXnY[pZ]` nodes: Linux `BLKGETSIZE64`, `BLKGETSIZE`,
  `BLKSSZGET`, `BLKROGET`, `BLKROSET`, `BLKRRPART`.
- `/dev/input/event0` and `event1`: Linux `struct input_event` records via
  `read(2)`; the kernel implements `EVIOCGVERSION`, `EVIOCGID`, `EVIOCGNAME`,
  `EVIOCGPHYS`, `EVIOCGBIT`, `EVIOCGKEY`, and no-op `EVIOCGRAB`, plus
  `O_NONBLOCK` and `poll(POLLIN)`. Text-input methods are the imd daemon's
  AF_UNIX service at `/run/leonos/input-method.sock`, not a device node.
- `/dev/snd/controlC<N>` and `pcmC<N>D<M>p/c`: registered-card Linux v6.14
  ALSA control/PCM requests, read/write/poll and shared PCM mmap. Nodes and
  negotiation reflect supported hardware directions and capabilities. The
  implemented request list, unsupported operations and deferred guest/upstream
  runtime validation are documented in the Audio ABI section of `docs/ABI.md`.
  The same public headers are exercised by the A6 standard-libc probe; its
  fixed ioctl/access/errno/mmap matrix is maintained in
  `docs/audio-abi-support.json`.
- `/dev/dsp` (aliased `/dev/audio`): OSS format/rate/channel negotiation,
  `GETFMTS`, `GETCAPS`, `GETBLKSIZE`, `SETFRAGMENT`, `GETISPACE/GETOSPACE`,
  `GETIPTR/GETOPTR`, `GETODELAY`, `GETTRIGGER/SETTRIGGER`, `SETDUPLEX`,
  `NONBLOCK`, `RESET` and blocking `SYNC`. Registered-card read/write/poll use
  real PCM state; `/dev/mixer` exposes the same controls as ALSA. `/dev/audio`
  retains its PCM compatibility alias rather than a mu-law default.
  The ABI-v1 AC97/ES1371 backend is stereo S16_LE playback only, with an
  exclusive generation-bound OFD. It advertises no DSP capabilities and does
  not implement capture, RESET, hardware pointer queries or output disabling.
- `/dev/ptmx`, `/dev/pts/<id>`, `/dev/tty`: standard Linux termios ioctls
  (`TCGETS`/`TCSETS`, `TIOCGWINSZ`, `TIOCSCTTY`, `TIOCGPGRP`, `TIOCSPGRP`,
  `TIOCGSID`, `TIOCNOTTY`, `TIOCSPTLCK`, `TIOCGPTLCK`); PTY users call
  `posix_openpt/openpty/forkpty`.
- AF_INET socket fds: `LEONOS_NET_CONTROL_IOCTL` with a versioned
  `struct leonos_net_control` for read-only network status queries; see
  Networking below.

The legacy fd 3 control channel is translated by libc to the matching
`/dev` node for older binaries. System inventory (`leonos_device_list()`),
auth (`include/leonos/auth.h` over `/etc/passwd` + PAM), signals
(`rt_sigaction`/`rt_sigprocmask`), and appearance (windowd messages) are
service/libc APIs without any ioctl request codes.

## Networking

The network ABI is defined in `include/leonos/net.h`. Legacy configuration and
diagnostic helpers are wrapped by libc as:

- `leonos_net_config`
- `leonos_net_dhcp_renew`
- `leonos_net_dns_resolve`
- `leonos_net_http_get`
- `leonos_net_ping`

The TCP client socket ABI is wrapped as:

- `leonos_socket_tcp`
- `leonos_socket_connect`
- `leonos_socket_send`
- `leonos_socket_recv`
- `leonos_socket_close`
- `leonos_net_connections`

IPv4 values are host-order packed addresses. For example, `10.0.2.2` is
`0x0a000202`.

The kernel supports a polling Intel e1000 MMIO driver, ARP, IPv4,
ICMP Echo, UDP transmit/receive for DNS, DNS A record lookups, a small ARP
cache, and active-open TCP client sockets over standard socket syscalls. At
boot the kernel brings the interface up with no address; IPv4 configuration
comes exclusively from the userspace `reliefos-dhcp` OpenRC service (BusyBox
`udhcpc -f -i eth0`). Its hook applies the address with `ifconfig`/`route`
(the kernel records it through the standard `SIOCSIF*` ioctls) and publishes
the lease atomically at `/run/reliefos/dhcp-lease`; libc merges that file into
`leonos_net_config()` results as the DHCP source. If DHCP never succeeds the
interface simply stays unconfigured. QEMU user-network guests normally
receive `10.0.2.15/24` with gateway `10.0.2.2` from the built-in lease
server.

`netctl.elf` can still request a renew after the desktop is
running; `leonos_net_dhcp_renew()` restarts the `reliefos-dhcp` service through
`rcctl.elf`, which the libc elevates via the sudo path for non-root callers.
Non-admin users may read network status and use DNS/HTTP/socket APIs. The
kernel-side `LEONOS_NET_CONTROL_DHCP` operation returns `EOPNOTSUPP` (the
in-kernel DHCP client was removed); lease renewal is purely the OpenRC
service.
`netctl.elf` also queries `leonos_net_connections` and displays TCP client
sockets in `SYN_SENT`, `ESTABLISHED`, `TIME_WAIT`, or `CLOSED`. Administrators
and trusted service tasks see the full socket table; normal users see only
connections owned by their uid.

Background DHCP is handled by the OpenRC service `reliefos-dhcp` (root-owned
udhcpc); `leonos_net_dhcp_renew()` restarts it via `rcctl`-style
`reliefos_openrc_run("reliefos-dhcp", "restart")` and then reads the lease the
hook publishes at `/run/reliefos/dhcp-lease`. The legacy `serviced.elf`
background retry loop and `/run/leonos/services.state` are gone; the desktop
`servicemgr` surfaces service state instead.

`leonos_socket_tcp()` is a thin wrapper over `socket(AF_INET, SOCK_STREAM, 0)`
and returns a real socket fd. `leonos_socket_connect` accepts a host name or
IPv4 literal, resolves DNS A records when needed, tries each returned address,
and records the selected remote IP and local port. `leonos_socket_send` and
`leonos_socket_recv` adapt the socket fd to the legacy status-record shape
with per-call timeouts. Closing uses standard `close(2)` on the fd.

`include/leonos/http.h` provides the higher-level userland HTTP client:
`leonos_http_get`, `leonos_http_request`, and `leonos_http_resolve_url`.
The client uses the socket wrappers, follows bounded redirects, decodes chunked
transfer responses, exposes response headers, content type, body length, final
URL, redirect count, and truncation flags. It sends plain `HTTP/1.1` for
`http://`, and uses Mbed TLS 3.6.7 for TLS 1.2, CA-chain, hostname, and clock
validation of `https://`. `httpget.elf` and `browser.elf` use this library for
both schemes. The older fixed-buffer `leonos_net_http_get` helper remains as a
compatibility wrapper for small diagnostic callers.

`downloadmgr.elf` also uses the HTTP client. It is currently fixed-buffer and
reports oversized responses through the truncation flag instead of streaming
large files incrementally.

The `LEONOS_NET_CONTROL_IOCTL` requests return `0` when the control structure
was processed. Per operation results are reported in the structure `status`
field. Timeouts are bounded by the kernel even if a larger value is requested;
socket, HTTP, DNS, and DHCP paths currently cap requested waits at 10000 ms.

## Authentication and Authorization

The authentication ABI is defined in `include/leonos/auth.h`. libc exposes:

- `leonos_auth_status`
- `leonos_auth_current`
- `leonos_auth_list_users`
- `leonos_auth_users_alloc`
- `leonos_auth_logout`
- `leonos_auth_create_user`
- `leonos_auth_update_user`

Interactive login runs through the standard PAM stack in `login.elf`
(`pam_leonos_password` verifies `/etc/shadow`), not through a libc
`leonos_auth_login` wrapper.
Successful login updates the current desktop session identity in the scheduler:
`uid`, `role`, `session_id`, `username`, and `home` are attached to the desktop
task and inherited by child applications. Logout clears the session identity and
kills ordinary user tasks in the session, then desktop returns to `login.elf`.

The kernel makes every file, task-kill, user-management, and installer-storage
decision itself in `kernel/reliefnt/fs/permissions.c`, against the permissions the
storage layer reports: the `LEONACL.SYS` sidecar on exFAT and FAT32, native
    inode fields on ext4, legacy ext2, and tmpfs, and fixed modes for PTY and device nodes. The
mapping is:

- `stat`, directory reads, and file reads: Read/List.
- `open` create/truncate, `write`, and `mkdir`: Write/Create.
- `execve` and path traversal: Execute/Traverse.
- `unlink`, `rmdir`, and the source side of `rename`: Delete.
- ACL set operations: Manage Permissions.

Normal users can access their own home through Owner permissions and shared
temporary files through the `/tmp` default ACL. Bundled help files under
`/docs` are treated as a system tree: normal users receive read/execute access
by default, while administrators retain full control. Administrators can manage
users and can take ownership or repair corrupt ACL metadata. Shutdown and reboot
remain available to any logged-in user.

## Current behavior and limitations

Standard C/POSIX wrappers, pthread and signals use musl. The kernel supports
native signal frames, a clone/futex thread subset and Unix STREAM/DGRAM/
SEQPACKET sockets including SCM_RIGHTS. Tested behavior and outstanding flags,
errors, lifecycle and concurrency cases remain itemized in the ABI ledger.

- True parent-suspending vfork, complete wait4 options/rusage and full clone
  semantics remain incomplete. SIGCHLD group-exit notification is implemented;
  complete siginfo, automatic reaping and reparenting remain outstanding.
- Raw getpriority returns `20 - priority`; libc performs the API conversion.
- NOFILE/AS soft/hard limits and prlimit64 share state across pthreads and copy
  at fork; other resources and complete enforcement remain incomplete.
- File access enforces owner/group/other Unix DAC with mode, UID and GID.
  chmod/chown work for the verified subset. ext4 and ext2 store native metadata;
  FAT/exFAT use ReliefOS metadata records. Full inode lifetime and special-bit
  behavior remain outstanding.
- File-backed mappings, INET servers/UDP/IPv6, PTY lock/hangup, event APIs and
  all remaining audit rows are still in scope. AP user scheduling is disabled;
  BSP preemption tests do not establish SMP compatibility.
