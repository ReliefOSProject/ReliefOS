"""Storage command ownership shared by build and rootfs staging."""
from pathlib import Path
import shutil

FORMATTER_COMMANDS = tuple(f"usr/sbin/{verb}.{fs}"
                          for fs in ("ext2", "ext4", "fat", "exfat") for verb in ("mkfs", "fsck"))
UTIL_LINUX_COMMANDS = ("usr/sbin/fdisk", "usr/sbin/sfdisk", "usr/sbin/runuser",
                       "usr/sbin/fsck", "usr/sbin/blkid", "bin/mount", "bin/umount", "bin/lsblk")
UTIL_LINUX_LIBRARIES = tuple(f"usr/lib/lib{name}.so.1"
                           for name in ("fdisk", "smartcols", "uuid", "blkid", "mount"))

COMPAT_LINKS = {"sbin/" + Path(path).name: "../" + path
                for path in UTIL_LINUX_COMMANDS if path.startswith("usr/sbin/")}
COMPAT_LINKS.update({"sbin/umount": "../bin/umount",
                     "sbin/leonos-grub-installer": "../usr/sbin/leonos-grub-installer"})
for verb in ("mkfs", "fsck"):
    COMPAT_LINKS[f"sbin/{verb}.exfat"] = f"../usr/sbin/{verb}.exfat"
    for filesystem in ("ext2", "ext3", "ext4", "fat", "vfat", "msdos"):
        name = f"{verb}.{filesystem}"
        COMPAT_LINKS[f"usr/sbin/{name}"] = f"../../sbin/{name}"
    COMPAT_LINKS[f"sbin/{verb}.fat32"] = f"{verb}.fat"
    COMPAT_LINKS[f"usr/sbin/{verb}.fat32"] = f"../../sbin/{verb}.fat32"
for name in ("e2fsck", "mke2fs", "dosfsck", "dosfslabel", "fatlabel", "mkdosfs"):
    COMPAT_LINKS[f"usr/sbin/{name}"] = f"../../sbin/{name}"
for name in ("chattr", "lsattr"):
    COMPAT_LINKS[f"usr/bin/{name}"] = f"../../bin/{name}"


def stage_filesystems(source: Path, destination: Path) -> None:
    """Copy a built package only after every required command is present."""
    for name in FORMATTER_COMMANDS:
        if not (source / name).is_file():
            raise ValueError(f"missing official filesystem tool: {source / name}")
    for path in source.rglob("*"):
        target = destination / path.relative_to(source)
        # Upstream e2fsprogs installs some archives as 0444. Replace owned
        # files instead of opening old read-only files for in-place writes.
        if target.is_symlink() or (not path.is_dir() and target.is_file()):
            target.unlink()
    shutil.copytree(source, destination, symlinks=True, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns(".storage-package.json"))
