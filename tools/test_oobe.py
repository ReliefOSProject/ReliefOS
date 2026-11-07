"""Host regressions for account and boot-state handling."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_installer_setup import CRYPTO

AUTH_SOURCES = ["userland/apps/authd/accounts.c", "userland/runtime/src/auth_password.c", *CRYPTO]
AUTH_FLAGS = ["-Ithird_party/mbedtls/include", "-idirafter", "userland/runtime/include",
              "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
              '-DMBEDTLS_CONFIG_FILE="reliefos_mbedtls_config.h"']

ROOT = Path(__file__).resolve().parents[1]


class OobeTests(unittest.TestCase):
    def test_account_request_boundaries(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-auth-input-") as tmp:
            executable = str(Path(tmp) / "input")
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-idirafter", "userland/runtime/include", "-idirafter", "include", "-Ikernel/reliefnt/include", "-Ikernel/reliefnt/include/uapi",
                "tools/tests/authd_input_test.c", *AUTH_SOURCES, *AUTH_FLAGS,
                "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=10)

    def test_posix_account_exports(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-auth-accounts-") as tmp:
            executable = str(Path(tmp) / "accounts")
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi",
                "tools/tests/authd_accounts_test.c", *AUTH_SOURCES, *AUTH_FLAGS,
                "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=10)

    def test_reboot_discards_session_but_keeps_accounts(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-auth-boot-") as tmp:
            executable = str(Path(tmp) / "auth-boot")
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-idirafter", "userland/runtime/include", "-idirafter", "include", "-Ikernel/reliefnt/include", "-Ikernel/reliefnt/include/uapi",
                "tools/tests/oobe_auth_boot_test.c", "-o", executable,
            ], cwd=ROOT, check=True)
            for scenario in ("stale", "missing", "denied", "database-formats"):
                with self.subTest(scenario=scenario):
                    subprocess.run([executable, scenario], cwd=ROOT, check=True, timeout=10)

    def test_input_method_requests_after_login(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-oobe-") as tmp:
            executable = str(Path(tmp) / "inputm")
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g",
                "-ftrivial-auto-var-init=zero",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-idirafter", "userland/runtime/include", "-idirafter", "include", "-Ikernel/reliefnt/include", "-Ikernel/reliefnt/include/uapi",
                "tools/tests/oobe_inputm_test.c", "-o", executable,
            ], cwd=ROOT, check=True)
            for scenario in ("list", "state", "active", "context", "notify", "denied"):
                with self.subTest(scenario=scenario):
                    subprocess.run([executable, scenario], cwd=ROOT, check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
