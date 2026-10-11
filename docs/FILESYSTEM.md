# Filesystems

ReliefOS uses a multi-filesystem storage layer. The normal installed system is
not a FAT32 root filesystem: it has a small FAT32 EFI System Partition (ESP)
and a separate writable ext4 root partition. Existing ext2 and exFAT installations
remain readable legacy volumes, but the current Alpine-shaped root layout
requires real symlinks (for example `/bin/sh` and `/var/run`); FAT32/exFAT
cannot represent them, so current images do not create an exFAT root and the
installer rejects that combination instead of materializing directory copies.

## Installed Disk Layout

| GPT partition | Type | Contents | Runtime mount |
| --- | --- | --- | --- |
| 1 | EFI System Partition / FAT32 | `EFI/`, `grub/`, `reliefos/kernel.sys`, `leonos/kernel.sys` (rollback copy) | `/boot` normally, `/target/boot` while Installer is running |
| 2 | Linux filesystem / ext4 | normal rootfs: `bin/`, `sbin/`, `lib/`, `usr/`, `etc/`, `opt/`, `var/`, `home/`, `tmp/`, `run/` | `/` in a normal session, `/target` while Installer is running |

UEFI GRUB reads partition 1. Once the kernel is running,
the storage layer selects partition 2 as `/`. FAT32 and exFAT data-volume
backends remain available; the current userspace boot contract requires ext4
and the new root skeleton. Old images require a fresh installation.

The installer itself uses a writable ext4 image as its ramdisk root because it must start
before any target disk is trusted. Its payload is deliberately split:

- `/install/root` is copied to the ext4 target root `/target` (or to an
  existing target with the same layout during update mode).
- `/install/esp` is copied to the FAT32 target ESP `/target/boot`.

Fresh installation creates both GPT partitions. Update requires both a valid
root and ESP; old FAT32-only installations should use a fresh install. The
installer does not migrate former `/system`, `/programs` or `/users` trees.
See `docs/ROOTFS_LAYOUT_AND_MIGRATION.md` for the current layout and boundaries.

## Device Namespace

`/dev` is a kernel-provided synthetic filesystem. It is present on every
runtime root. Mount-point directories are stored in the image, but device
nodes are synthesized. `/dev/shm` is a real root-backed directory, mode 1777,
cleared before userspace on each boot; it is not a tmpfs mount. Directory reads enumerate
the complete set of device nodes, including `/dev/null`, `/dev/zero`,
`/dev/tty`, `/dev/console`, `/dev/fb0`, `/dev/input/event0`,
`/dev/input/event1`, `/dev/dsp`, `/dev/serial0`, `/dev/ethernet0`, `/dev/gpu`,
`/dev/rtc`, `/dev/driverctl`, `/dev/kmsg`, `/dev/ptmx`, and block-device
Linux-style block nodes are exposed as `/dev/sda`, `/dev/sdb`, and so on for
ATA/SATA disks, and `/dev/nvme0n1`, `/dev/nvme1n1`, and so on for NVMe
namespaces. Partitions use `/dev/sda1` and `/dev/nvme0n1p1` spelling.
The corresponding libc APIs open these paths before issuing device ioctls.

Whole disks and GPT partitions are exposed using those Linux block names. They
support sector-aligned `read`/`write`, and
the Linux UAPI requests `BLKGETSIZE64`, `BLKGETSIZE`, `BLKSSZGET`, `BLKROGET`
and `BLKRRPART` from `<linux/fs.h>`. Raw reads use the device's Linux open-time
DAC permissions, including readable descriptors inherited across a UID change.
Raw writes require an administrator (or the installer ISO's uid-0 shell);
writes and partition-table rereads are rejected for booted or mounted disks.
Disk-backed filesystem `stat.st_dev` and `/proc/self/mountinfo` use the same
device numbers as `/dev` and sysfs. The util-linux build uses direct blkid
probing, including filesystem versions, so root's `lsblk -f` reports real
types, labels, UUIDs and mounted filesystem usage. Without a udev cache,
ordinary users need `sudo lsblk -f` for fields requiring device reads.

`/dev/input/event0` is the Linux evdev keyboard stream and
`/dev/input/event1` is the mouse stream. Each opened descriptor has its own
event cursor, so readers do not consume Desktop input. The text-input method
service no longer has a device node; the retired `/dev/input-method` endpoint
was replaced by the imd daemon's AF_UNIX socket at
`/run/reliefos/input-method.sock`. The runtime also exposes the old socket path
through its compatibility link.

`/dev/dsp` is the Linux OSS-compatible PCM playback node. It accepts
16-bit little-endian stereo samples through normal `write` calls, supports
nonblocking mode and `poll(POLLOUT)`, and exposes the supported OSS setup
ioctls from `<linux/soundcard.h>`. `/dev/audio` is a compatibility alias for
legacy binaries only.

## Supported Formats

### ext4

Generated and fresh-installed roots use an explicit ext4 profile with extents,
metadata checksums, 64-bit block groups, flex_bg, a journal, dir_index,
large_file, huge_file, and extra_isize. Linux ext4 images are readable and
writable through the native backend. Unknown incompat features are rejected;
advanced bigalloc, inline-data, encrypted, verity, and casefold profiles remain
outside the basic interoperability contract.

### ext2 compatibility

The kernel implements the classic, unjournaled ext2 subset used by legacy
compatibility images and pre-ext4 targets:

- 1 KiB, 2 KiB, and 4 KiB blocks; generated ReliefOS images use 4 KiB.
- 128-byte-or-larger classic inodes.
- Direct, singly indirect, and doubly indirect file blocks.
- File read/write/truncate, directory enumeration, create, `mkdir`, `unlink`,
  `rmdir`, and same-filesystem `rename`.
- Standard Linux filesystem-data GPT type GUID.

It intentionally rejects extents, journals, metadata checksums, 64-bit block
numbers, encryption, and unsupported incompatible feature bits. Build images
with `mke2fs -t ext2`; `tools/make_image.py` disables unsupported modern
extensions explicitly.

### ext4 feature policy

The ext4 backend described by `docs/superpowers/specs/2026-09-28-reliefos-ext4-design.md`
is implemented for the basic interoperability scope. The feature policy is
defined by the current source tree and the in-tree Linux reference (`linux/`,
v7.3-rc5, feature masks in `linux/fs/ext4/ext4.h`). The machine-readable matrix is
`tools/tests/ext4_feature_matrix.py`; run
`python3 tools/tests/ext4_feature_matrix.py --check-schema` to verify every
mask against the reference tree. Status values are `rw` (read-write supported
by the plan), `reject` (feature bit present means the mount is refused with
`-EOPNOTSUPP` and the volume is left unmodified), and `api-eopnotsupp`
(accepted at mount; the related API call returns `-EOPNOTSUPP`).

| Feature | Class | Mask | Scope | Status | Trigger error |
| --- | --- | --- | --- | --- | --- |
| extents | incompat | 0x0040 | extent | rw | |
| 64bit | incompat | 0x0080 | group-descriptor | rw | |
| flex_bg | incompat | 0x0200 | group-descriptor | rw | |
| sparse_super | ro_compat | 0x0001 | superblock | rw | |
| sparse_super2 | compat | 0x0200 | superblock | rw | |
| uninit_bg | ro_compat | 0x0010 | group-descriptor | rw | alias of gdt_csum |
| metadata_csum | ro_compat | 0x0400 | metadata | rw | |
| gdt_csum | ro_compat | 0x0010 | group-descriptor | rw | |
| large_file | ro_compat | 0x0002 | inode | rw | |
| huge_file | ro_compat | 0x0008 | inode | rw | |
| extra_isize | ro_compat | 0x0040 | inode | rw | |
| dir_nlink | ro_compat | 0x0020 | directory | rw | |
| dir_index | compat | 0x0020 | directory | rw | |
| has_journal | compat | 0x0004 | journal | rw | |
| bigalloc | ro_compat | 0x0200 | allocation | reject | mount: -EOPNOTSUPP |
| inline_data | incompat | 0x8000 | inode | reject | mount: -EOPNOTSUPP |
| casefold | incompat | 0x20000 | directory | reject | mount: -EOPNOTSUPP |
| encrypt | incompat | 0x10000 | file-data | reject | mount: -EOPNOTSUPP; FS_IOC_SET_ENCRYPTION_POLICY: -EOPNOTSUPP |
| verity | ro_compat | 0x8000 | file-data | reject | mount: -EOPNOTSUPP; FS_IOC_ENABLE_VERITY: -EOPNOTSUPP |
| quota | ro_compat | 0x0100 | allocation | reject | mount: -EOPNOTSUPP; quotactl: -EOPNOTSUPP |
| project | ro_compat | 0x2000 | allocation | reject | mount: -EOPNOTSUPP; FS_IOC_FSSETXATTR: -EOPNOTSUPP |
| fast_commit | compat | 0x0400 | journal | reject | mount: -EOPNOTSUPP |
| mmp | incompat | 0x0100 | superblock | reject | mount: -EOPNOTSUPP |
| dax | none | 0 | vfs-api | api-eopnotsupp | mount -o dax and FS_IOC_FSSETXATTR: -EOPNOTSUPP |

`uninit_bg` is the e2fsprogs name of the same RO_COMPAT bit as `gdt_csum`
(0x0010); both names map to that one mask. `dax` has no ext4 feature bit
(mount option / `FS_XFLAG_DAX`), so its mask is 0 and it is refused per call.
Feature bits outside this table are not part of the plan.

### exFAT

New images and fresh installations use the standard single-FAT exFAT subset:

- 512-byte logical sectors with a validated primary and backup boot region.
- Allocation bitmap, compressed Microsoft upcase table, and directory
  entry-set checksums.
- UTF-8/UTF-16 names (up to 255 UTF-16 units) with case-insensitive lookup.
- Contiguous `NoFatChain` and ordinary FAT-chain files; reads, writes,
  truncation, directory creation/removal, deletion, enumeration, and same-
  volume rename.
- Microsoft Basic Data GPT type is required for the runtime root. The
  recommended GPT partition name is `LEONOS4_ROOT`; boot discovery also
  accepts a valid exFAT signature when that label is missing or was changed.
  The exFAT volume-label limit means the on-volume label is `LEONOS4ROOT`.

TexFAT, multiple FATs, non-512-byte sectors, POSIX inode metadata, and native
permissions are rejected. `LEONACL.SYS` remains the hidden ReliefOS ACL
sidecar. Sudden-power-loss repair is expected to use the host `fsck.exfat`.

### FAT32

FAT32 remains supported for the ESP, installer ramdisk, legacy installed
images, removable media, and compatibility data volumes. Long file names,
directory traversal, reads, writes, creation, deletion, and renaming remain
available. FAT32 remains the only filesystem the UEFI/GRUB boot path relies
on.

### ISO 9660

Optical media is discovered through AHCI ATAPI and automatically mounted at
`/media/cdrom<N>`. ISO 9660 volumes are read-only. This is independent
of whether the boot/root disk uses exFAT, ext4, ext2, or FAT32.

## Runtime Data Mounts

Disk Manager can mount a supported, unprotected FAT32, exFAT, or ext4 GPT data
partition for the current boot. The kernel mounts it at the stable path
`/mnt/disk<N>p<M>`, where `N` is the disk ID and `M` is the GPT entry index plus
one. The mount path is shown in the partition status and appears in File
Manager's sidebar without restarting File Manager. BusyBox `mount` exposes
the same operation and accepts an explicit absolute target path.

Runtime data mounts are deliberately non-persistent in this first version:
they are removed on reboot and no automatic mounting policy is stored on disk.
Mounting the same partition again is idempotent and returns its existing mount
path.
FAT32, exFAT, ext4, and the classic ext2 subsets documented above are accepted; an
unknown or unsupported on-disk filesystem is rejected rather than mounted
according to its GPT type alone.

The installer reserves `/target` for the target root and `/target/boot` for its ESP while a
target is mounted. Formatting or mounting an installer target is rejected with
busy if those paths contain an optical, data, or other runtime volume; this
prevents an installer operation from silently replacing a live mount.

Unmounting is administrator-gated. The kernel refuses it while any live task
has that volume as its working directory, owns an open file on it, is executing
an image from it, or retains a file-backed mapping from it. Format and delete
are likewise refused while the partition is mounted. This avoids stale file
nodes and mappings after a volume is removed.

## Disk Manager Partitions

Disk Manager presents the GPT entries of every detected AHCI, IDE/PATA, or NVMe disk, including
their name, LBA range, filesystem probe, capacity, GPT role, protection state,
and mount path when mounted. It can create a 1 MiB-aligned FAT32, exFAT, or ext4
data partition in free space, format an existing data partition as either
filesystem, delete an existing data partition's GPT entry, and mount or
unmount a supported data partition. New partition creation includes formatting
as part of the operation.

Formatting reuses the installer-grade FAT32, exFAT, and ext4 formatters but
applies only to the selected partition extent. FAT32 and exFAT data partitions
use the Microsoft Basic Data GPT type; ext4 and ext2 data partitions use the
Linux filesystem type.
Deleting a partition removes its GPT metadata only, so it is not a secure-wipe
operation.

Disk Manager never allows partition changes or runtime mounts against the
current boot disk or a target disk currently mounted by the installer. This
restriction is enforced by the kernel, not merely disabled buttons. The
initial partition-management ABI supports the common 128-entry, 128-byte GPT
table format generated by ReliefOS and rejects a malformed, overlapping,
out-of-range, or CRC-invalid table before making a modification.

## VFS and Paths

ReliefOS paths use Unix absolute syntax, for example
`/usr/lib/reliefos/apps/desktop/desktop.elf`. The kernel resolves `.` and `..`
and the storage layer dispatches the result through the longest matching mount
path. Paths containing `:` are rejected; legacy disk prefixes are not
supported. Filesystem names are case-insensitive at the
ReliefOS path layer for compatibility with historical FAT32 behavior; avoid
creating names that differ only by case on ext2.

The device directory is synthesized at `/dev`; it contains standard device
nodes and `input`/`pts` subdirectories even when the backing filesystem has no
physical directory entry. `LEONACL.SYS` is internal
ACL sidecar metadata: it remains readable by the authorization service but is
hidden from normal exFAT, FAT32, and ext2 directory enumeration.

## API Behavior

Current file syscalls support all writable exFAT, ext4, legacy ext2, and FAT32 roots:

- File reads and short, bounded writes.
- Create and overwrite through `open` flags.
- `mkdir`, `unlink`, `rmdir`, and `rename` within one mounted filesystem.
- Directory listing, `stat`, seek, and ACL service integration.

Cross-mount rename is rejected. ISO 9660 returns a read-only error for all
mutation operations. exFAT allocation updates its bitmap and FAT chains; ext4
allocation updates extents, bitmaps, and journal metadata; legacy ext2 allocation
updates its inode/block bitmaps and free counts; FAT32 retains its cluster-chain
allocator and LFN handling.

## Safety and Recovery

ext4 journaling protects normal metadata updates; legacy ext2 has no journal and
can still require offline repair after sudden power loss. The installer writes a complete target layout before
copying payload files and treats a failed copy as an installation failure. The
normal image generator and installer formatter reserve distinct boot and root
partitions so a large runtime file cannot consume ESP space.

Use `tools/analyze_boot_log.py` when boot diagnostics show a root mount failure.
The log identifies whether exFAT, ext4, legacy ext2, or the legacy FAT32 fallback was used.
