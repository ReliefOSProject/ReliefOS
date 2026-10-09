#!/usr/bin/env python3
"""Inventory the M1 service role marks (ntclks separation, phase 5.7).

The M1 authority decision (docs/superpowers/ntclks-separation/
09-m1-authority-design-review.md) rides on reserved role gids recorded in the
image inode.  The windowd stack (desktop.elf/windowd.elf/imd.elf) that used to
carry those marks is out of the image, so the userspace side of the mark is
gone and the kernel grants stay dormant.  This test pins that end state:

1. kernel defines: RELIEFOS_GID_WINDOW_SERVER/RELIEFOS_GID_SERVICE in
   kernel/reliefnt/kernel/exec/userland.c still carry the pinned values;
2. staging plan: tools/build/rootfs-stage.sh assigns no role gid to any app;
3. image build: tools/build/images.sh contains no role-gid re-chown;
4. guest groups: system/rootfs/etc/group and system/test-accounts/group still
   declare the reserved, memberless groups at the same numbers (installed
   systems may carry the gids on old files);
5. built manifest (optional, --manifest): no entry carries a role gid at all.

Checks 1-4 run against the checked-in sources; check 5 needs a built tree
(`make O=<dir> all`, manifest at <dir>/rootfs/manifest.json) and is reported
as skipped when no manifest is available.
"""
from pathlib import Path
import argparse
import json
import re

ROOT = Path(__file__).resolve().parents[1]
KERNEL_USERLAND = (ROOT / "kernel/reliefnt/kernel/exec/userland.c")
ROOTFS_STAGE = ROOT / "tools/build/rootfs-stage.sh"
IMAGES = ROOT / "tools/build/images.sh"
GROUP_FILES = [ROOT / "system/rootfs/etc/group", ROOT / "system/test-accounts/group"]

WINDOW_SERVER_GID = 60001
SERVICE_GID = 60002

GROUP_NAMES = {
    "leonos-window-server": WINDOW_SERVER_GID,
    "leonos-service": SERVICE_GID,
}


def check_kernel_defines(failures):
    text = KERNEL_USERLAND.read_text()
    for name, value in (("RELIEFOS_GID_WINDOW_SERVER", WINDOW_SERVER_GID),
                        ("RELIEFOS_GID_SERVICE", SERVICE_GID)):
        match = re.search(rf"^#define {name} (\d+)u$", text, re.M)
        if not match:
            failures.append(f"KERNEL: {name} define missing in {KERNEL_USERLAND}")
        elif int(match.group(1)) != value:
            failures.append(f"KERNEL: {name}={match.group(1)}, expected {value}")


def check_staging_plan(failures):
    text = ROOTFS_STAGE.read_text()
    lines = [line for line in text.splitlines()
             if "gid=60001" in line or "gid=60002" in line]
    if lines:
        failures.append("STAGE: rootfs-stage.sh still assigns a role gid: "
                        + "; ".join(line.strip() for line in lines))


def check_images_rechown(failures):
    text = IMAGES.read_text()
    for name, gid in (("desktop.elf", WINDOW_SERVER_GID),
                      ("windowd.elf", SERVICE_GID),
                      ("imd.elf", SERVICE_GID)):
        if f"{name}:{gid}" in text:
            failures.append(f"IMAGES: role re-chown for {name}:{gid} still "
                            "present in images.sh")


def check_group_files(failures):
    for path in GROUP_FILES:
        names = {}
        for line in path.read_text().splitlines():
            if not line or line.startswith("#"):
                continue
            name, password, gid, members = (line.split(":") + [""] * 4)[:4]
            if name in GROUP_NAMES:
                names[name] = (password, int(gid), members)
        for name, gid in GROUP_NAMES.items():
            if name not in names:
                failures.append(f"GROUP: {name} missing from {path}")
                continue
            password, actual, members = names[name]
            if actual != gid:
                failures.append(f"GROUP: {path} {name} gid={actual}, expected {gid}")
            if members:
                failures.append(f"GROUP: {path} {name} is not memberless "
                                f"(members={members!r}); non-root must not chgrp in")


def check_manifest(manifest, failures):
    document = json.loads(manifest.read_text())
    marked = {entry["path"]: entry.get("gid", 0)
              for entry in document.get("entries", []) if entry.get("gid", 0)}
    for path, gid in sorted(marked.items()):
        failures.append(f"MANIFEST: unapproved role mark on {path} (gid={gid})")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path,
                        help="built rootfs manifest.json (e.g. <O>/rootfs/manifest.json); "
                             "skips the built-mark inventory when omitted")
    args = parser.parse_args()

    failures = []
    check_kernel_defines(failures)
    check_staging_plan(failures)
    check_images_rechown(failures)
    check_group_files(failures)
    if args.manifest:
        check_manifest(args.manifest, failures)
    else:
        print("note - built manifest not provided; source inventory only "
              "(pass --manifest <O>/rootfs/manifest.json after a build)")

    if failures:
        print("SERVICE MARKER inventory violations:")
        for line in failures:
            print(f"  {line}")
        raise SystemExit(1)
    scope = "source + built manifest" if args.manifest else "source"
    print(f"PASS service marker: no role marks assigned anywhere ({scope})")


if __name__ == "__main__":
    main()
