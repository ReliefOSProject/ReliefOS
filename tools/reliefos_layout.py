#!/usr/bin/env python3
"""Canonical ReliefOS names for host-side rootfs staging helpers.

Directory creation and symbolic-link topology comes from the shared C
rootfs contract table. The RELIEFOS-named constants below use the canonical
guest directories from ``include/reliefos/layout.h``. Separate published
filenames and protocol values that still contain ``leonos`` remain explicit
compatibility values in that header.

The installed root follows the Alpine Linux FHS shape: /bin, /sbin, /lib,
/usr/bin, /usr/sbin and /usr/lib are real directories.  There is no usr-merge
and no /lib64; the musl ELF PT_INTERP path stays /lib/ld-musl-x86_64.so.1.
Standard runtime links include:

    /var/run -> ../run
    /var/lock -> ../run/lock

ReliefOS-owned state and resources get explicitly named subdirectories so they
do not collide with third-party Linux software:

    /etc/reliefos                 persistent configuration
    /var/lib/reliefos             persistent mutable state
    /var/cache/reliefos           cache data
    /run/reliefos                 volatile per-boot IPC and session state
    /usr/lib/reliefos             private libraries, drivers and loader payload
    /usr/lib/reliefos/apps        application packages (manifest + executable)
    /usr/lib/reliefos/drivers     ReliefOS Ring-0 driver modules
    /usr/lib/reliefos/tests       diagnostic guest probes
    /usr/share/reliefos           desktop resources that are not icon-theme data
    /usr/share/fonts/reliefos     ReliefOS UI fonts
    /usr/share/doc/reliefos       bundled help and vendor notices

The C header include/reliefos/layout.h exposes the corresponding guest paths.
Consumers should use its macros or this module instead of adding new path
literals.
"""
from __future__ import annotations

import os
import re
from pathlib import Path, PurePosixPath

# ---------------------------------------------------------------------------
# Guest-relative directory contract (no leading slash).
# ---------------------------------------------------------------------------
BIN = "bin"
SBIN = "sbin"
LIB = "lib"
BOOT = "boot"
HOME = "home"
SRV = "srv"
USR = "usr"
USR_BIN = "usr/bin"
USR_LIB = "usr/lib"
USR_SBIN = "usr/sbin"
USR_SHARE = "usr/share"

ETC = "etc"
ETC_RELIEFOS = "etc/reliefos"
ETC_SSL_CERTS = "etc/ssl/certs"

VAR = "var"
VAR_LIB = "var/lib"
VAR_LIB_RELIEFOS = "var/lib/reliefos"
VAR_CACHE_RELIEFOS = "var/cache/reliefos"
VAR_LOG = "var/log"
VAR_TMP = "var/tmp"

RUN_RELIEFOS = "run/reliefos"

RELIEFOS_LIB = "usr/lib/reliefos"
RELIEFOS_APPS = "usr/lib/reliefos/apps"
RELIEFOS_DRIVERS = "usr/lib/reliefos/drivers"
RELIEFOS_TESTS = "usr/lib/reliefos/tests"
RELIEFOS_SHARE = "usr/share/reliefos"
RELIEFOS_RESOURCES = "usr/share/reliefos/resources"
RELIEFOS_FONTS = "usr/share/fonts/reliefos"
RELIEFOS_DOC = "usr/share/doc/reliefos"
LICENSES = "usr/share/licenses"
MISC = "usr/share/misc"
TERMINFO = "usr/share/terminfo"
LOCALE = "usr/share/locale"
EXAMPLES = "usr/share/examples"

OPT = "opt"
OPT_DYNE = "opt/dyne"
OPT_LUA = "opt/lua"
OPT_PYTHON = "opt/python"
OPT_TCC = "opt/tcc"

# One directory/mode/link table for all images, the installer and early boot.
ROOTFS_CONTRACT = Path(__file__).resolve().parents[1] / "kernel/reliefnt/include/uapi/reliefos/rootfs.h"
_contract = ROOTFS_CONTRACT.read_text(encoding="ascii")
ROOT_DIRECTORIES = {path.lstrip("/"): int(mode, 8) for path, mode in
                    re.findall(r'X\("(/[^"\n]+)", (0[0-7]+)\)', _contract)}
ROOT_SYMLINKS = dict((path.lstrip("/"), target) for path, target in
                    re.findall(r'X\("(/[^"\n]+)", "([^"\n]+)"\)', _contract))
if not ROOT_DIRECTORIES or not ROOT_SYMLINKS:
    raise ValueError("invalid rootfs contract: missing directory or link table")


def layout_directories(root: Path) -> None:
    """Create only real directories; do not traverse staging symlinks."""
    if root.is_symlink():
        raise ValueError(f"rootfs root is a symlink: {root}")
    root.mkdir(parents=True, exist_ok=True)
    root.chmod(0o755)
    for directory, mode in ROOT_DIRECTORIES.items():
        path = root
        for component in PurePosixPath(directory).parts:
            path /= component
            if path.is_symlink():
                raise ValueError(f"rootfs directory is a symlink: {path}")
            path.mkdir(exist_ok=True)
        path.chmod(mode)


def apply_root_symlinks(root: Path) -> None:
    """Publish the standard links, refusing conflicting files/directories."""
    for link, target in ROOT_SYMLINKS.items():
        path = root / link
        if path.is_symlink() and os.readlink(path) == target:
            continue
        if path.exists() or path.is_symlink():
            raise ValueError(f"conflicting rootfs link: {path}")
        path.symlink_to(target)


# Absolute runtime paths used by C code and generated configuration.
P_BIN = "/bin"
P_SBIN = "/sbin"
P_LIB = "/lib"
P_USR_BIN = "/usr/bin"
P_USR_LIB = "/usr/lib"
P_ETC = "/etc"
P_ETC_RELIEFOS = "/etc/reliefos"
P_ETC_SSL_CERTS = "/etc/ssl/certs"
P_VAR_LIB_RELIEFOS = "/var/lib/reliefos"
P_VAR_CACHE_RELIEFOS = "/var/cache/reliefos"
P_RUN_RELIEFOS = "/run/reliefos"
P_RELIEFOS_LIB = "/usr/lib/reliefos"
P_RELIEFOS_APPS = "/usr/lib/reliefos/apps"
P_RELIEFOS_DRIVERS = "/usr/lib/reliefos/drivers"
P_RELIEFOS_TESTS = "/usr/lib/reliefos/tests"
P_RELIEFOS_SHARE = "/usr/share/reliefos"
P_RELIEFOS_RESOURCES = "/usr/share/reliefos/resources"
P_RELIEFOS_FONTS = "/usr/share/fonts/reliefos"
P_RELIEFOS_DOC = "/usr/share/doc/reliefos"
P_LICENSES = "/usr/share/licenses"
P_MISC = "/usr/share/misc"
P_TERMINFO = "/usr/share/terminfo"
P_LOCALE = "/usr/share/locale"

P_MUSL_INTERP = "/lib/ld-musl-x86_64.so.1"
P_CACERT = "/etc/ssl/certs/ca-certificates.crt"
P_LIBRELIEFOS = "/usr/lib/reliefos/libreliefos.so.2"

# Configuration files.
P_RELIEFOS_CONF = P_ETC_RELIEFOS + "/leonos.conf"
P_DISPLAY_CONF = P_ETC_RELIEFOS + "/display.conf"
P_DRIVERS_CONF = P_ETC_RELIEFOS + "/drivers.conf"
P_TASKBAR_CFG = P_ETC_RELIEFOS + "/taskbar.cfg"
P_NETWORK_CONF = P_ETC_RELIEFOS + "/network.conf"
P_NETWORK_BAK = P_ETC_RELIEFOS + "/network.conf.bak"
P_NETWORK_TMP = P_ETC_RELIEFOS + "/network.conf.tmp"
P_LOCALE_CONF = P_ETC_RELIEFOS + "/locale.conf"
P_ENVIRONMENT_CONF = P_ETC_RELIEFOS + "/environment.conf"
P_FILEASSOC_CFG = P_ETC_RELIEFOS + "/fileassoc.cfg"
P_DESKTOP_ENTRIES_CONF = P_ETC_RELIEFOS + "/desktop-entries.conf"
P_LESSKEY = P_ETC_RELIEFOS + "/lesskey"

# Persistent state and volatile runtime state.
P_USERS_DB = P_VAR_LIB_RELIEFOS + "/users.db"
P_ACCOUNTS_DB = P_VAR_LIB_RELIEFOS + "/accounts.db"
P_STARTUP_DB = P_VAR_LIB_RELIEFOS + "/startup.db"
P_STARTUP_DENIALS_DB = P_VAR_LIB_RELIEFOS + "/startup-denials.db"
P_OOBE_DONE = P_VAR_LIB_RELIEFOS + "/oobe.done"
P_LICENSE = P_VAR_LIB_RELIEFOS + "/license.dat"
P_KERNELDEBUG_ENABLED = P_VAR_LIB_RELIEFOS + "/kerneldebug.enabled"
P_KERNELDEBUG_CONTROL = P_VAR_LIB_RELIEFOS + "/kernel-debug"
P_SESSION_USER = P_RUN_RELIEFOS + "/session-user"

# Runtime boot payload paths (the ESP is normally mounted at /boot).
P_BOOT_KERNEL = "/boot/reliefos/kernel.sys"
P_BOOT_KERNELDEBUG_MARKER = "/boot/reliefos/state/kerneldebug.next"
P_BOOT_DISPLAY_CONF = "/boot/reliefos/config/display.conf"
P_KERNELDEBUG_MODULE = P_RELIEFOS_LIB + "/kerneldebug.sys"

# ESP-internal (EFI FAT) paths.  GRUB and loader.elf use these before any
# root filesystem exists.  The loader is at the ESP root so GRUB's
# ``search --file /loader.elf`` convention is preserved.
ESP_LOADER = "/loader.elf"
ESP_KERNEL = "/reliefos/kernel.sys"
ESP_KERNEL_LEGACY = "/leonos/kernel.sys"
ESP_KERNELDEBUG_MARKER = "/reliefos/state/kerneldebug.next"
ESP_DISPLAY_CONF = "/reliefos/config/display.conf"
ESP_DISPLAY_CONF_LEGACY = "/leonos/config/display.conf"

# Former build-owned command paths, retained for incremental staging cleanup.
GCC_ALIASES = (
    "addr2line", "ar", "as", "c++", "c++filt", "cc", "cpp", "elfedit", "g++",
    "gcc", "gcc-15.1.0", "gcc-ar", "gcc-nm", "gcc-ranlib", "gcov", "gcov-dump",
    "gcov-tool", "gprof", "ld", "ld.bfd", "nm", "objcopy", "objdump", "ranlib",
    "readelf", "size", "strings", "strip", "musl-gcc", "musl-g++",
)


def app_package_dir(app: str) -> PurePosixPath:
    """Return the guest-relative package directory for an application id."""
    return PurePosixPath(RELIEFOS_APPS) / app


def app_exec_path(app: str, extension: str = "elf") -> PurePosixPath:
    """Return the guest-relative executable path for an application id."""
    return app_package_dir(app) / f"{app}.{extension}"


def app_package_dir_abs(app: str) -> str:
    return str(PurePosixPath("/") / app_package_dir(app))


def app_exec_path_abs(app: str, extension: str = "elf") -> str:
    return str(PurePosixPath("/") / app_exec_path(app, extension))


def relative_symlink_target(link: str, target: str) -> str:
    """Return a relative symlink target for guest-relative ``link``.

    ``target`` is the guest-relative path of the link destination.  The result
    is valid when both paths are interpreted from the guest root.
    """
    link_path = PurePosixPath(link)
    target_path = PurePosixPath(target)
    # os.path.relpath semantics for pure POSIX paths.
    link_parts = list(link_path.parent.parts)
    target_parts = list(target_path.parts)
    common = 0
    while (common < len(link_parts) and common < len(target_parts)
           and link_parts[common] == target_parts[common]):
        common += 1
    parts = [".."] * (len(link_parts) - common) + target_parts[common:]
    return "/".join(parts) or "."


def command_symlink(command: str, target: str) -> tuple[str, str]:
    """Return ``(guest-relative link, relative target)`` for a /usr/bin entry."""
    link = f"{USR_BIN}/{command}"
    return link, relative_symlink_target(link, target)


def root_symlink_entries() -> list[tuple[str, str]]:
    """Return all rootfs symlinks as ``(guest-relative link, target)``."""
    return sorted(ROOT_SYMLINKS.items())


# ---------------------------------------------------------------------------
# Build-side payload ownership.  These are guest-relative paths whose content
# is generated by a component; the staging prune and layout-link stages use
# them so disabled components and root symlinks cannot leave stale entries.
# ---------------------------------------------------------------------------
NCURSES_COMMANDS = (
    "clear", "infocmp", "infotocap", "captoinfo", "reset", "tabs", "tic",
    "toe", "tput", "tset", "ncursesw6-config",
)

_PAYLOAD_PATHS: dict[str, tuple[str, ...]] = {
    "fastfetch": (f"{USR_BIN}/fastfetch", f"{LICENSES}/fastfetch", "etc/fastfetch",
                  "usr/share/fastfetch/leonos-ascii.txt", "etc/skel/.config/hyfetch.json"),
    "pleditor": (f"{LICENSES}/pleditor",),
    "busybox": (f"{BIN}/busybox", f"{BIN}/sh", f"{LICENSES}/busybox"),
    "ncurses": (f"{TERMINFO}", "etc/terminfo",
                *tuple(f"{USR_BIN}/{name}" for name in NCURSES_COMMANDS),
                f"{LICENSES}/ncurses"),
    "sl": (f"{USR_BIN}/sl", f"{LICENSES}/sl"),
}

# Only used on host build staging, never on a user's installed filesystem.
# file, less and vim now arrive as signed upstream Alpine packages during the
# APK transaction, and lua is an ordinary repository install.
RETIRED_TOOL_PATHS = (
    OPT_DYNE, OPT_LUA, OPT_PYTHON, OPT_TCC,
    *(f"{RELIEFOS_APPS}/{name}" for name in ("file", "less", "lua", "tcc", "vim")),
    *(f"{USR_BIN}/{name}" for name in (*GCC_ALIASES, "file", "less", "lua", "nano",
                                      "python", "python3", "python3.14", "tcc", "vim")),
    *(f"{USR_BIN}/x86_64-linux-musl-{name}" for name in GCC_ALIASES
      if not name.startswith("musl-")),
    f"{USR_LIB}/libmagic.so.1", f"{USR_LIB}/liblua.so.5", f"{MISC}/magic.mgc",
    f"{USR_SHARE}/vim",
    *(f"{LICENSES}/{name}" for name in ("file", "less", "lua", "musl-gcc", "nano",
                                       "python", "tcc", "vim")),
    *(f"{EXAMPLES}/{name}" for name in ("musl-gcc", "python")),
)


def tool_payload_paths(package: str) -> tuple[str, ...]:
    """Return guest-relative staging roots owned by a tool component."""
    paths = _PAYLOAD_PATHS.get(package, ())
    if not isinstance(paths, tuple):
        raise TypeError(f"payload paths must be a tuple: {package}")
    for path in paths:
        value = PurePosixPath(path)
        if not path or value.is_absolute() or ".." in value.parts or value == PurePosixPath("."):
            raise ValueError(f"invalid guest-relative payload path: {package}: {path}")
    return paths


def all_staging_payload_paths() -> tuple[str, ...]:
    """Return every guest-relative path owned by a staged component."""
    seen: set[str] = set()
    ordered: list[str] = []
    for package in _PAYLOAD_PATHS:
        for path in tool_payload_paths(package):
            if path not in seen:
                seen.add(path)
                ordered.append(path)
    return tuple(ordered)


def builtin_command_links(enabled=None) -> list[tuple[str, str]]:
    """Return non-application command symlinks as ``(link, relative target)``.

    ``enabled`` is an optional ``callable(package_id) -> bool`` so disabled
    components do not leave dangling command links behind.
    """
    if enabled is None:
        enabled = lambda _package: True  # noqa: E731
    entries: list[tuple[str, str]] = []

    # Alpine keeps the Bourne shell and BusyBox applets in /bin.
    if enabled("busybox"):
        link = f"{BIN}/sh"
        entries.append((link, relative_symlink_target(link, f"{BIN}/busybox")))
    return entries


def app_command_links(app: str) -> list[tuple[str, str]]:
    """Return ``(link, relative target)`` for an application package entry."""
    link = f"{USR_BIN}/{app}"
    return [(link, relative_symlink_target(link, str(app_exec_path(app))))]
