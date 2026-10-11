# Build and Installer

> **历史验证记录。** 本文保留旧版本的来宾回归命令和结果，文中 `build.py`、
> `build/` 输出目录及旧配置生成器不再是维护入口。当前构建只使用根
> `Makefile`，请先阅读 [GNU Make + C 构建系统](BUILDSYSTEM.md)，再将这里的
> 回归场景映射到对应的 `make` 目标；不要复制本文旧命令到生产构建链。

## Accounts and optional components

Fresh installations create a fixed `root` account (UID/GID 0) and a chosen
ordinary account (UID/GID 1000) in the installer. Both passwords require
1-32 UTF-8 characters without whitespace; confirmations must match. Python
and GCC/binutils can be selected independently. OOBE is no longer shipped.
See [Installer Accounts and Components](INSTALLER_ACCOUNTS.md) for the current
GUI/TTY installation and login regression commands. Older QEMU scripts below
that assume OOBE or omit account setup need that workflow updated before use.

## Installer and window regressions

After `python3 build.py run release`, run:

```sh
python3 tools/test_installer_input.py
python3 tools/test_linux_pty.py
python3 tools/test_runtime_responsiveness.py
python3 tools/test_terminal.py
python3 tools/test_oobe.py
python3 tools/test_installer_window_qemu.py installer
python3 tools/test_installer_window_qemu.py desktop
python3 tools/test_installer_window_qemu.py tty --install
python3 tools/test_installer_window_qemu.py installed --disk build/ui-regressions/tty/scratch.raw
```

The QEMU checks boot the distribution ISOs using KVM, OVMF, 4 GiB RAM and two
CPUs. Screenshots and serial logs go under `build/ui-regressions`. The TTY check
attaches only its disposable `tty/scratch.raw` hard disk; `--install` confirms
formatting and copying to that disk, with a 15-minute copy timeout for the full
toolchain/runtime payload. Without it, the test cancels at confirmation. The
`installed` case boots that disk with no ISO attached and repeats the desktop check.
The desktop check maximizes Terminal, runs Linux Vim, restores the window, and
maximizes File Manager and Task Manager. The installer check verifies its real
sidebar and footer coordinates, without compensating for a cropped surface.
These checks do not establish VMware validation.

Window resizing retains the public `leonos_gui_*` API. The shared window client
now submits a fully painted replacement segment to windowd before releasing its
old segment, and the compositor invalidates its old mapping on replacement.
Normal, installer, and SDK builds ship matching client/server code. Applications
that statically embedded an older window client need relinking to gain this fix;
the previous CREATE/PRESENT wire messages remain accepted for fixed-size frames.

TTY reads now wait for a canonical line instead of reporting EOF on an empty
live input queue. Block-device SDK helpers translate POSIX `-1`/`errno` into
their documented negative-error return values, including missing disk probes.

## Power control regressions

```sh
python3 build.py run test-power
python3 tools/test_power_qemu.py desktop-reboot
python3 tools/test_power_qemu.py desktop-shutdown
python3 tools/test_power_qemu.py installer-reboot
```

The host checks execute the actual reboot dispatcher with hardware operations
intercepted, plus desktop confirmation/error handling and the historical authd
power policy. These older consumer tests need migration to the new sudo path.
The QEMU checks click the real controls and require guest-originated RESET or
SHUTDOWN events. A reboot must reach a second kernel boot; shutdown must exit
QEMU. The installer case performs a full GUI install on a new 2 GiB test disk,
ejects the ISO before clicking Restart, and checks the installed ext4 system
reaches OOBE. It refuses to overwrite an existing test disk; use a fresh
`--output` directory for repeat installations. Logs, screenshots and event
records are saved under `build/power-regressions`.

The kernel accepts the Linux raw `reboot(magic1, magic2, cmd, arg)` argument
order, all four Linux magic2 values, and 32-bit argument widths. Obsolete
one-argument raw calls are rejected. The bundled unmodified musl `reboot(int)`
wrapper therefore works without an application-specific ABI workaround.

The desktop remains a privileged session launcher; its children apply the PAM
session and drop to the authenticated identity before exec. Power helpers call
musl reboot for a real root process; ordinary processes execute reboot/poweroff
through official sudo and require an explicit sudoers grant. There is no authd
power endpoint. New integration behavior has not been rerun after the user's
2026-09-12 test pause.
The installer runs as root and uses musl reboot directly after sync. Errors
remain visible in the desktop, GUI installer and TTY installer.

This is a targeted power-control repair, not full reboot(2) certification:
the existing kernel UID gate and remaining capability, namespace, HALT,
CAD/RESTART2 and other command semantics remain in the ABI audit backlog.
QEMU results do not establish VMware validation.

Build 4.6.2-3490 validation: host power, OOBE, UAPI and security checks passed;
QEMU Start-menu reboot reached a second kernel boot, and shutdown produced a
guest-shutdown event and normal QEMU exit. Both ISOs and GCC payload integrity
checks passed. The full GUI installation/restart test was stopped at the user's
request before completion; its completion-page restart is not yet guest-verified.

## Source of truth

GNU Make is the maintained build entry point. Component selection comes from
`configs/components.toml` and Kconfig. Generated files are not edited by hand.

Important generated files include:

- `<O>/include/generated/autoconf.h`
- `<O>/include/generated/autoconf-installer.h`
- `<O>/include/generated/build_info.h`

The default output directory is `out/x86_64/release`; source-tree version headers
are not build inputs.

## Configuration

`Kconfig` currently exposes two groups:

- `Build`
- `System Configuration`

The build group controls the source macro that is compiled into binaries for
the standalone VMDK image and the source macro compiled into binaries installed
from the installer ISO. The default for both license gates is enabled.

The system group controls the license platform URL that is compiled into
userland binaries. The default is `http://127.0.0.1:30301`.

The Make/Kconfig front end normalizes `.config`, writes `autoconf.h` with the
VMDK `RELIEFOS_LICENSE_REQUIRE` policy, writes `autoconf-installer.h` with the
installer-installed-system policy, and writes `CONFIG_LICENSE_SERVER_URL` into
both headers. License binaries read that compiled macro directly; there is no
runtime `/etc/reliefos/license.conf` server override.

## Main outputs

Common build outputs (under `$(O)`, default `out/x86_64/release`):

- `images/reliefos.vmdk` / `images/reliefos.raw`
- `images/reliefos-live.iso`
- `images/reliefos-installer.iso`
- `images/installer-root.ext4` (historically `install/root.fat`)
- `images/root.ext4` (Live / disk root; ext4 preserves the current real symlinks and inode metadata)
- `stage/esp` (ESP staging directory; esp.fat is generated from it during ISO/VMDK assembly)

The common system staging tree is:

- `stage/esp`

It contains the Alpine-shaped root tree (`bin/`, `sbin/`, `lib/`, `usr/`,
`etc/reliefos/`, `var/lib/reliefos/`, `opt/`) plus the ESP-only loader and kernel
under `reliefos/`. Legacy `leonos` paths remain available for migration and
compatibility. Help documents live in `usr/share/doc/reliefos/`; application
packages live in `usr/lib/reliefos/apps/`.
Vim and ncurses are enabled by default. Vim now arrives as the unmodified
Alpine `vim` APK (pinned in `configs/dependencies.lock.json`), and ncurses is
built from the pinned submodule via `make upstream-ncurses`. Both `image-vmdk` and
`installer` package Vim's runtime and the
ncurses terminfo database. The ncurses tools also embed fallback descriptions
for ReliefOS terminal types (`xterm`, `xterm-256color`, `linux`, `vt100`, `ansi`,
`screen`, and `screen-256color`), so `clear`, `tput`, and Vim remain usable if
the external database cannot be read. `musl-desktop-vim` remains a compatible
target name for the standalone GRUB live desktop ISO, now using this same normal
payload. There is no dependency on `build/musl/vim-src` or other experimental
downloads. `python3 build.py run test-terminal-packages` checks the actual Linux
executables with both the packaged database and a deliberately unavailable
terminfo path; it does not certify the kernel's whole Linux ABI.
The normal application set includes `login.elf` and `oshlp.elf`. Standalone
`image-vmdk` and ordinary `image-iso` automatically provision these public test
accounts and require PAM login:

| User | Password | Identity / sudo |
| --- | --- | --- |
| test | test | UID/GID 1000, wheel member; sudo authenticates with `test` |
| root | root | UID/GID 0 |

Both use the default `%wheel ALL=(ALL:ALL) ALL` policy, without NOPASSWD.
The image-only seed is `system/test-accounts`; `/home/test` is mode 0700 and
owned by 1000:1000 inside ext4. Shared staging stays unchanged, so the installer
runtime and `/install/root` retain locked account seeds until the installer
provisions the chosen passwords. No authd binary or AUS2 seed is packaged.
`python3 tools/test_image_accounts.py` checks both tree-generation paths,
yescrypt password matching and actual ext4 ownership in small temporary images;
it is not a QEMU login or sudo execution test.

## Installer packaging

The installer has two related payload groups:

- Top-level ISO boot payload: kernel and installer root.
- Installed-system root payload: a copy of `build/esp` without `EFI/`,
  `grub/`, `reliefos/`, or `leonos/`, stored under `install/root` inside
  `build/install/root.fat`.
- Installed-system FAT32 ESP payload: the UEFI/GRUB and early boot files
  stored under `install/esp` inside `build/install/root.fat`.

`tools/make_installer_root.py` creates `build/install/root.fat`, copies the
normal staging tree into `/install/root` and its boot subset into
`/install/esp`, removes stale license override files from the root payload,
and overlays
policy-sensitive binaries built with `autoconf-installer.h`.

`tools/make_installer_iso.py` creates `build/images/reliefos-installer.iso` and
stages:

- `reliefos/kernel.sys`
- `install/root.fat`

This keeps installer boot and installed-system boot on the same matched
component set.

`install/root.fat` is one Multiboot module. It is mapped directly by
the kernel and is not copied into a second RAM buffer. The installer ISO mounts
this ext4 image (the historical filename is retained) as a writable live
environment: file and directory changes are
kept in the mapped RAM image for the session and discarded on reboot; they never
modify the ISO. The guest nevertheless
needs enough physical memory for GRUB to load the complete module before the
kernel begins, and the kernel only manages pages above the 768 MiB user window.
In particular, a 400 MiB installer root requires at least 2 GiB
of VM RAM; a 1 GiB VM can omit the module before the loader receives control.
If GRUB places that module over the kernel's fixed ELF load
range, the Loader relocates it into EFI LoaderData before loading the image;
the relocated range is reserved and mounted directly by the kernel.

## Updating an already mounted target

The update wizard mounts `/target` and `/target/boot` while scanning the existing
installation, then prepares them again after confirmation. A VMware report from
build 3531 showed the first mount succeeding and the second preparation failing
with `Mount failed ret=-22`, before partition enumeration.

The failing operation was `mkdir("/target", 0755)`. Storage routing translated
the existing mountpoint to the ext4 backend's `/`; splitting that root into a
parent and filename returned `EINVAL`. `storage_mkdir` now checks the global
namespace first and returns `EEXIST` for existing objects, including mount roots,
while preserving lookup failures. This follows Linux v6.12 `filename_create`
and `do_mkdirat` in `fs/namei.c`. Installer mountpoint creation/stat failures now
include their stage, path and error in the serial log.

The earlier AHCI `ret=-11` messages in that report are pending asynchronous I/O:
the syscall dispatcher waits and retries them, and the first mount completes.
They do not explain the deterministic second-preparation `EINVAL`.

Targeted reproduction and regression commands:

```sh
python3 build.py test storage-mkdir-mount
python3 build.py run installer
python3 tools/test_installer_update_qemu.py \
  --disk build/installer-accounts-final-gui/scratch.raw \
  --output build/installer-update-fixed
```

The host test executes the real storage VFS and ext4 code on isolated RAM-backed
filesystem images. The QEMU script copies the supplied installed test disk and
only modifies the copy. Use a new output directory per run. Optical boot gets
an explicit firmware priority so the installed disk cannot bypass the installer.

Build 3532 validation on 2026-09-11:

- The host test failed before the fix with mounted `/target` returning `-22`,
  then passed with `-17` under ASan/UBSan. Nested mountpoints, existing files and
  symlinks, missing parents, volume isolation, and pending I/O are covered.
- The unchanged build 3531 installer reproduced `Mount failed ret=-22` in QEMU
  after program scanning (`build/installer-update-before-2/update/serial.log`).
- The installer build completed with zero errors. The same GUI update sequence
  on a copy of an installed build 3518 disk completed on build 3532, including
  root payload and EFI/GRUB replacement. Log and screenshot:
  `build/installer-update-fixed/update/serial.log` and `updated.png`.
- Booting the updated disk without an ISO reached build 3532. Existing root and
  ordinary passwords worked, UID/GID remained 0 and 1000 respectively, ordinary
  access to `/root` and `/etc/passwd` was denied, and disabled Python/GCC stayed
  absent. Reproduce this fixture's login checks with
  `python3 tools/test_installer_accounts_qemu.py --output build/installer-update-fixed --login-only --components none`.
- Ext2 rename/symlink and e2fsck checks, rootfs/permission regressions, and AHCI
  completion tests passed. QEMU configured two CPUs; the kernel enabled one.
  VMware supplied the failure evidence, but the repaired ISO is not yet
  VMware-verified. TTY update and fresh installation were not rerun this time.

## Ext2 copy performance

Ext2 keeps 128 clean filesystem blocks in a bounded LRU cache (512 KiB of
payload, 516 KiB including metadata), allocated on first use outside the fixed
kernel image. Allocation failure falls back to uncached I/O. Writes remain
synchronous: only successfully written data enters the cache. Device writes
invalidate overlapping entries before submission, including raw-device aliases
and failed writes; remounting a volume invalidates its previous entries.
Storage's existing transaction lock serializes cache access.

Block and inode allocation start from a per-volume hint, skip exhausted groups
and full bitmap bytes, and wrap to reuse free space below the hint. Cached
bitmaps, inode tables, indirect blocks and superblock counters eliminate
repeated metadata reads. Installer copying uses `sched_yield()` between short
reads instead of `sleep_ms(1)`: the 4 KiB syscall read slice otherwise incurs a
timer tick on each iteration. GUI progress remains throttled to 50 ms. Per-file
ownership, mode, fsync, temporary-file replacement and error handling remain
in place.

Targeted host checks:

```sh
python3 build.py run test-ext2-cache
python3 build.py run test-ext2-performance
python3 tools/test_ext2_performance.py --output build/ext2-performance-after.json \
  --baseline build/ext2-performance-before.json
python3 tools/test_storage_rename.py
python3 tools/test_storage_mkdir_mount.py
```

The baseline JSON is captured by the same workload before the optimization;
it is a local measurement artifact, not a required checkout file. Host tests
compile the actual ext2 implementation with ASan/UBSan. Cache tests cover
1/2/4 KiB blocks, allocation failure, volume isolation, raw aliases, failed
writes, remounting and eviction. The performance workload checks an 8 MiB
file round-trip across indirect-block boundaries and creates 256 small files;
`e2fsck -fn` independently verifies the resulting filesystem.

Measured block-I/O command counts on 2026-09-11:

| Workload | Reads before | Reads after | Writes before/after |
| --- | ---: | ---: | ---: |
| Create an 8 MiB file | 13,288 | 1 | 12,545 |
| Read back the 8 MiB file | 5,608 | 2,051 | 0 |
| Create 256 files of 192 bytes | 8,480 | 12 | 2,832 |

Build 3534 QEMU/KVM GUI update used the same build 3518 installed-disk input
and q35/AHCI configuration as build 3532. Both copied 5,387 rootfs files and
selected four application updates. From the confirmed update's second
mount through completion, elapsed guest time fell from 445.986 s to 180.011 s
(2.48x). The ext2 core-files stage, ending at the first ESP file, fell from
390.411 s to 121.314 s (3.22x). Evidence is in
`build/installer-update-fixed/update/serial.log` and
`build/ext2-update-optimized/update/serial.log`; the disk input is copied, never
updated in place. This is an end-to-end workload comparison, not a SATA
bandwidth benchmark. FAT32 ESP copying still took about 56 s. Ext2 metadata
write amplification remains; this change adds no delayed writeback and does
not certify power-loss recovery. VMware performance has not been measured.

Post-update root/ordinary login, file writes, credential and access checks
passed. A fresh GUI install with both Python and GCC enabled also completed,
followed by root/ordinary login and desktop Terminal validation. Its copy stage
took 596.246 s including ESP; this larger payload has no matched pre-change
fresh-install timing. Reproduce with:

```sh
python3 tools/test_installer_accounts_qemu.py \
  --output build/ext2-install-optimized --components all --desktop
```

After QEMU exited, both disks' ext2 partitions were extracted and checked with
host `e2fsck -fn`: zero errors (6,005 files on the updated disk, 18,915 on the
fresh install). `debugfs` extraction and `cmp` verified that the installed
113,335,392-byte Python and 29,917,624-byte GCC `cc1` matched the staged
payload byte for byte, with matching SHA-256 values. Logs and desktop capture:
`build/ext2-install-optimized/install/serial.log`,
`build/ext2-install-optimized/e2fsck.log`,
`build/ext2-install-optimized/desktop/terminal-identity.png`.

The tested installer is `build/images/leonos4-installer.iso`, build 3534,
SHA-256 `598dcdc356f76c64c39e6fe52e8bb44dcfd099ec9162cbf8d14cd165419439fd`.

### Bounded write batching (build 3535)

The next optimization follows the allocation principles in the local Linux
v6.12 sources: `fs/ext2/inode.c:399` (`ext2_alloc_blocks`), `:479`
(`ext2_alloc_branch`), `:624` (`ext2_get_blocks`),
`fs/ext2/balloc.c:1208` (`ext2_new_blocks`), and
`fs/buffer.c:2086` (`__block_write_begin_int`). Linux can allocate multiple
adjacent blocks, stop mappings at indirect-leaf boundaries, and zero only the
unwritten portions of new buffers in memory. LeonOS applies those ideas within
its existing synchronous storage transactions; it does not implement Linux's
page-cache writeback machinery.

`storage_ext2_write.c` initializes up to 32 KiB of consecutive new blocks in one
data write. It updates the bitmap, free counts and pointer leaf once per run,
and publishes pointers only after initialized data has been written. Runs stop
at occupied physical blocks, occupied logical mappings, group boundaries and
the end of the pointer leaf. New indirect branches and existing mappings retain
the prior path. Failed writes restore this run's metadata, including partially
written sectors. A rollback failure is reported and retains allocations that
may still be referenced; persistent device failure still requires a filesystem
check. This is not a journal or a claim of power-loss atomicity.

The installer collects short reads into 32 KiB buffers, yielding between them,
and the block layer passes up to 64 sectors to AHCI. Per-sector fallback remains
available after failed commands. ESP writes retain the 4 KiB installer slice.
The ownership/mode/fsync/rename publication sequence remains unchanged.

Compared with build 3534, the same 8 MiB creation workload dropped from 12,545
to 1,597 write requests and from 45,083,648 to 13,028,352 written bytes. Creating
256 small files dropped from 2,832 to 2,576 writes. Read counts did not increase.
The measured requests here are at the ext2/block boundary; the separate block
test verifies that an accepted 32 KiB AHCI transfer is one transport request.

```sh
python3 build.py run test-ext2-write-batch
python3 build.py run test-storage-write-batch
python3 build.py run test-installer-copy
python3 tools/test_ext2_performance.py --output build/ext2-performance-round2-after.json \
  --write-baseline build/ext2-performance-round2-before.json
```

The ASan/UBSan tests exercise 1/2/4 KiB filesystems and six write-failure
positions, including a partial first sector; direct/single/double-indirect
data runs; holes and overwrites; physical fragmentation; group boundaries;
reuse of blocks containing old data; zero-length writes; and remounting.
All 21 images pass `e2fsck -fn`. The block test checks successful batching,
partial-command fallback, and error propagation. The installer read test
checks short reads, EINTR, partial EOF and read failure before publication.

QEMU/KVM GUI update on the same copied build 3518 disk still copied 5,387 rootfs
files and selected four application packages. The confirmed update fell from
180.011 s (3534) to 150.733 s (3535); the ext2 stage fell from 121.314 s to
93.600 s. FAT32 ESP took another 54.222 s. Evidence:
`build/ext2-update-batched/update/serial.log`. These are single-run guest-clock
measurements with the same VM configuration; VMware performance remains
unmeasured.

The matching fresh GUI install enabled Python and GCC on both builds and
copied 17,866 rootfs files. From the first copy-stage status through completion,
elapsed time fell from 596.246 s to 400.007 s (32.9% less). Before the ESP stage,
time fell from 549.071 s to 351.985 s. As individual examples, GCC `cc1` took
8.732 s then 2.028 s, and Python `python3.14` took 31.567 s then 6.623 s;
these use consecutive copy-start timestamps. Logs:
`build/ext2-install-optimized/install/serial.log` and
`build/ext2-install-batched/install/serial.log`.

The installer build completed with zero errors. Updated-system root and
ordinary-user login, file writes and permission isolation passed, and its
extracted ext2 partition passed `e2fsck -fn`. Both update and fresh-install
tests use q35/AHCI, 4 GiB RAM and KVM; QEMU exposes two CPUs but the kernel
enables one. These results are not multicore storage stress certification.

The fresh install also passed ordinary/root login, permission isolation,
Python/GCC presence and desktop Terminal checks. Its extracted ext2 partition
passed `e2fsck -fn` (18,915 files). `debugfs` extraction followed by `cmp` and
SHA-256 comparison verified the 113 MB Python and 30 MB GCC `cc1` against the
installation source, byte for byte. Evidence:
`build/ext2-install-batched/e2fsck.log`, `readback-sha256.log`,
`build/ext2-install-batched/desktop/terminal-identity.png`,
and `build/ext2-install-batched.log`.

Tested ISO: `build/images/leonos4-installer.iso`, build 3535,
SHA-256 `75ad1453ecceb14fe3bdf6351146e3143a40f3a9c21ac7cb4d70191b29cbafc0`.

For VirtualBox installation tests, attach the target VDI through a SATA/AHCI,
NVMe, or legacy PIIX4 IDE controller. NVMe namespaces use synchronous polling
queues and require 512-byte logical sectors. IDE/PATA disks use synchronous
PIO with LBA48 (and LBA28 fallback); IDE/ATAPI optical media is read-only and
can provide the Installer ISO.

For QEMU, the normal `make run` command keeps the AHCI topology. Set
`RELIEFOS_QEMU_IDE=1` when running `make run-iso` or a QMP smoke test to use
a PIIX3 IDE controller with the disk on primary master and the ISO on the
secondary channel.
Set `RELIEFOS_QEMU_NVME=1` to attach the same VMDK through a QEMU NVMe controller;
this is intentionally opt-in so normal QEMU runs retain the AHCI regression path.

The installer ISO GRUB menu provides the default graphical installer and one
`Install ReliefOS (TTY mode)` entry. TTY mode starts an interactive BusyBox Ash
shell in the installer root. Run `installer` in that shell to start the TTY
version of `installer.elf`; otherwise the same environment can be used for a
manual installation with upstream util-linux
`fdisk`/`sfdisk`/`blkid`/`lsblk`/`mount`/`umount`, upstream filesystem
`mkfs.*`/`fsck.*` programs, BusyBox file tools, and `leonos-grub-installer`.
`fdisk` creates an empty GPT directly with `g`; FAT32 formatting uses the
upstream `mkfs.fat -F 32` syntax. Read-only checks require each checker's `-n`
option. The complete current procedure is installed as
`/root/ADVANCED_INSTALL.txt` in the installer runtime and maintained in
`docs/ADVANCED_INSTALL.md`. The shell does not perform automatic partitioning.

The installer runtime includes `desktop.elf`, `installer.elf`, and
`/bin/busybox`. The installed-system root payload under
`/install/root/usr/lib/reliefos/apps` contains the normal app set, including
`login.elf`. A fresh install boots directly into login using accounts created
on the installer's Accounts page. Copying the root payload manually does not
provision these accounts or the installed-system marker.

The policy-sensitive installed-system binaries are currently `desktop.elf`
and `settings.elf`. They link a libc build that uses
`autoconf-installer.h`, so disk files cannot turn off validation or redirect
the license server. To build an image without license validation, change the
corresponding source macro through Kconfig and regenerate/rebuild so the
generated binaries contain `RELIEFOS_LICENSE_REQUIRE 0`.

Installer update mode refreshes FAT32 ESP boot files from `/install/esp`, ext4
system files from `/install/root`, selected changed or missing application
packages under `/usr/lib/reliefos/apps`, and bundled docs from
`/install/root/usr/share/doc/reliefos`. The core update refreshes the real
`/bin`, `/sbin`, `/lib`, `/usr/bin`, `/usr/sbin`, `/usr/lib`, `/etc/reliefos`,
`/opt` and `/var/lib/reliefos` trees without deleting unrelated target files.
Third-party shared libraries such as `libmagic.so.1`, `liblua.so.5`, and
`sqlite.so.3` live in `/usr/lib`; the canonical `libreliefos.so.2` lives in
`/usr/lib/reliefos`, while `libleonos.so.2` remains in `/usr/lib/leonos` for old
ELF dependencies. It also copies `/usr/lib/reliefos/kerneldebug.sys` from the
payload.
This keeps the dynamic loader, shared runtime, applications, and the built-in
kernel debugging module on one release version. Missing runtime directories or
the debug module on an older target are created during the update; the
installer payload itself must contain all of these files or the update is
rejected as incomplete.
Selected program packages are compared recursively and only shipped files that
are missing or changed are copied. This includes package metadata, licenses,
icons, headers, examples, and private runtime data; files added locally to an
installed package are left untouched.
Docs are merged: matching bundled `.hlp` files are overwritten under
`/target/usr/share/doc/reliefos`, but extra third-party help files already
present there are kept. Update mode does not replace `/target/etc/reliefos` or
`/target/var/lib/reliefos`, so local machine state such as
`license.dat` and the installed marker is preserved across an
installer-driven update. The machine ID is derived from detected machine
identity at runtime instead of being stored in `/etc/install.id`. The stable
identity source is SMBIOS System UUID when firmware provides it, otherwise the
boot GPT disk and ESP partition GUIDs. A fresh install copies the staged
`etc/reliefos` and `var/lib/reliefos` trees, then writes the chosen accounts before
publishing the ESP boot payload. Update supports an existing non-usr-merge ext4 layout; legacy
ext2 targets remain readable for compatibility, while old
pre-FHS installations need a fresh install. There is no migration/archive
stage. Populated legacy private account databases are rejected before overlay;
standard account and policy files are preserved. See
`docs/ROOTFS_LAYOUT_AND_MIGRATION.md` and `docs/SUDO_AND_ELEVATION.md`.

All ext4 image producers require `fakeroot` and `e2fsprogs`; legacy ext2 image
producers use the same tools. File ownership is
normalized to root:root inside fakeroot before mke2fs imports the payload;
this does not change host workspace ownership.

## WSL validation commands

For this checkout, validate Linux-targeted build and QEMU behavior through WSL.
Native Windows build-tool output is not the authoritative proof path.

Build the normal disk image and installer ISO:

```sh
wsl -e bash -lc 'cd "/mnt/d/Projects/C/LeonOS 4" && make image-vmdk installer'
```

For docs-only changes, a full image rebuild is normally unnecessary. At minimum
check whitespace and patch hygiene:

```sh
wsl -e bash -lc 'cd "/mnt/d/Projects/C/LeonOS 4" && git diff --check -- docs'
```
