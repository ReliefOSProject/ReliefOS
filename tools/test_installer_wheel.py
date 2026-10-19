#!/usr/bin/env python3
"""Verify real installer account creation in a disposable root namespace."""
from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    work = Path(os.environ.get("RELIEFOS_TEST_OUT", ROOT / "build/auth-upstream")).resolve()
    work.mkdir(parents=True, exist_ok=True)
    binary = work / "installer-wheel-test"
    subprocess.run([
        "clang", "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "tools/tests/installer_wheel_test.c",
        "userland/auth/standard_accounts.c", "userland/auth/account_store.c",
        "userland/runtime/src/auth_password.c", "-pthread", "-lcrypt", "-o", str(binary),
    ], cwd=ROOT, check=True)
    subprocess.run([
        "bwrap", "--unshare-user", "--uid", "0", "--gid", "0",
        "--unshare-pid", "--unshare-net", "--die-with-parent", "--ro-bind", "/", "/",
        "--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp", str(binary),
    ], check=True, timeout=60)
    policy = (ROOT / "system/rootfs/etc/sudoers").read_text().splitlines()
    assert "%wheel ALL=(ALL:ALL) ALL" in policy, "wheel needs password-authenticated sudo policy"
    subprocess.run([
        "bwrap", "--unshare-user", "--uid", "0", "--gid", "0",
        "--unshare-pid", "--unshare-net", "--die-with-parent", "--ro-bind", "/", "/",
        "--proc", "/proc", "--dev", "/dev", "--tmpfs", "/etc",
        "--ro-bind", str(ROOT / "system/rootfs/etc/passwd"), "/etc/passwd",
        "--ro-bind", str(ROOT / "system/rootfs/etc/group"), "/etc/group",
        "--dir", "/etc/sudoers.d", "--ro-bind", str(ROOT / "system/rootfs/etc/sudoers"),
        "/etc/sudoers", "visudo", "-c", "-f", "/etc/sudoers",
    ], check=True, timeout=30)
    print("installer wheel: PASS default sudoers rule")


if __name__ == "__main__":
    main()
