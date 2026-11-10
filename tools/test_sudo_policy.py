#!/usr/bin/env python3
"""Host regressions for the sudo/su authorization policy and authd handlers.

Two suites run under ASan/UBSan:

* `sudo_policy_test.c` exercises the pure policy unit (target resolution, the
  UID 0 passwordless switch, the cached-credential window, FILEOP validation).
* `authd_sudo_test.c` links the real `authd_sudo.c` and drives it through its
  injected channel and spawn hooks, so the decisions that actually gate
  elevation are asserted on the host instead of only inside QEMU.
"""
from __future__ import annotations

import subprocess
from runtime_test_dependencies import crypto_flags
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

COMMON_FLAGS = [
    "-std=c11", "-D_GNU_SOURCE", "-O1", "-g",
    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
    "-Wall", "-Wextra", "-Werror",
    "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-idirafter", "userland/runtime/include",
    "-Iuserland/apps/authd",
]

SUITES = (
    ("sudo-result", ["tools/tests/sudo_result_test.c"]),
    ("sudo-password", ["tools/tests/sudo_password_test.c", "userland/runtime/src/sudo_client.c"]),
    ("sudod-paths", ["tools/tests/sudod_paths_test.c"]),
    ("sudo-policy", ["tools/tests/sudo_policy_test.c"]),
    ("authd-sudo", ["tools/tests/authd_sudo_test.c",
                    "userland/apps/authd/authd_sudo.c"]),
    ("sudo-spawn", ["tools/tests/sudo_spawn_test.c",
                    "userland/apps/authd/authd_sudo.c"]),
    ("sudo-client", ["tools/tests/sudo_client_test.c",
                     "userland/runtime/src/sudo_client.c", "userland/runtime/src/authd_client.c",
                     "userland/runtime/src/unix_ipc.c"]),
    ("sudo-security", ["tools/tests/sudo_security_test.c",
                       "userland/apps/authd/authd_sudo.c",
                       "userland/apps/authd/accounts.c",
                       "userland/runtime/src/auth_password.c",
                       *crypto_flags()]),
)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="reliefos-sudo-") as tmp:
        for name, sources in SUITES:
            executable = str(Path(tmp) / name)
            extra = ["-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections"]
            if name == "sudo-client":
                extra += ["-Wl,--wrap=reliefos_ipc_connect,--wrap=reliefos_ipc_peer_credentials"]
            subprocess.run(["cc", *COMMON_FLAGS, *extra, *sources, "-o", executable],
                           cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=60)
    return 0


if __name__ == "__main__":
    sys.exit(main())
