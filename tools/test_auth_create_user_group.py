#!/usr/bin/env python3
"""Exercise the production account-creation command in a disposable root."""
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    work = Path(os.environ.get("RELIEFOS_TEST_OUT", ROOT / "build/auth-upstream")).resolve()
    work.mkdir(parents=True, exist_ok=True)
    binary = work / "auth-create-user-group-test"
    subprocess.run([
        "clang", "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi",
        "-Iuserland/runtime/include",
        "tools/tests/auth_create_user_group_test.c",
        "userland/runtime/src/auth_accounts.c", "userland/auth/standard_accounts.c",
        "userland/auth/account_store.c", "userland/runtime/src/auth_password.c",
        "-pthread", "-lcrypt", "-o", str(binary),
    ], cwd=ROOT, check=True)
    subprocess.run([
        "bwrap", "--unshare-user", "--uid", "0", "--gid", "0", "--unshare-pid",
        "--unshare-net", "--die-with-parent", "--ro-bind", "/", "/", "--proc", "/proc",
        "--dev", "/dev", "--tmpfs", "/etc", "--tmpfs", "/run", "--tmpfs", "/tmp",
        str(binary),
    ], check=True, timeout=30)


if __name__ == "__main__":
    main()
