"""Installer input validation and retained legacy account regressions."""
import hashlib
import errno
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CRYPTO = [f"third_party/mbedtls/library/{name}.c" for name in
          ("md", "pkcs5", "sha1", "sha256", "sha512", "platform_util", "aes")]


class InstallerSetupTests(unittest.TestCase):
    def test_console_home_does_not_create_desktop_shortcuts(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-console-home-") as temporary:
            root = Path(temporary)
            executable = root / "console-home"
            subprocess.run([
                "cc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi",
                "-idirafter", "userland/runtime/include",
                "tools/tests/installer_console_home_test.c", "-o", str(executable),
            ], cwd=ROOT, check=True)
            (root / "home").mkdir()
            (root / "etc/skel").mkdir(parents=True)
            for name in (".bash_profile", ".bashrc", ".inputrc"):
                (root / "etc/skel" / name).write_text("# shell defaults\n")
            subprocess.run([str(executable), root], check=True, timeout=10)
            self.assertEqual(list((root / "home/test/desktop").iterdir()), [])
            self.assertEqual((root / "home/test/.bashrc").read_text(), "# shell defaults\n")

    def test_hyfetch_defaults(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-hyfetch-home-") as temporary:
            work = Path(temporary)
            executable = work / "prepare-home"
            subprocess.run([
                "cc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer", "-ffunction-sections", "-fdata-sections",
                "-Wl,--gc-sections", "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi",
                "-idirafter", "userland/runtime/include",
                "tools/tests/installer_hyfetch_config_test.c", "-o", str(executable),
            ], cwd=ROOT, check=True)
            root = work / "root"
            home = root / "home/test"
            home.mkdir(parents=True)
            subprocess.run([executable, root], check=True)
            self.assertFalse((home / ".config").exists())
            template = root / "etc/skel/.config/hyfetch.json"
            template.parent.mkdir(parents=True)
            template.write_bytes((ROOT / "userland/fastfetch/hyfetch.json").read_bytes())
            subprocess.run([executable, root], check=True)
            config = home / ".config/hyfetch.json"
            self.assertEqual(config.read_bytes(), template.read_bytes())
            self.assertEqual(config.stat().st_mode & 0o777, 0o600)
            self.assertEqual(config.stat().st_uid, os.getuid())
            config.write_text('{"preset":"custom"}\n')
            subprocess.run([executable, root], check=True)
            self.assertEqual(config.read_text(), '{"preset":"custom"}\n')
            config.unlink()
            protected = work / "protected"
            protected.write_text("untouched")
            config.symlink_to(protected)
            subprocess.run([executable, root], check=True)
            self.assertTrue(config.is_symlink())
            self.assertEqual(protected.read_text(), "untouched")
            config.unlink()
            config.parent.rmdir()
            outside = work / "outside"
            outside.mkdir()
            config.parent.symlink_to(outside, target_is_directory=True)
            result = subprocess.run([executable, root])
            self.assertIn(result.returncode, (errno.ELOOP, errno.ENOTDIR))
            self.assertEqual(list(outside.iterdir()), [])

    def test_tty_input(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-login-input-") as temporary:
            executable = Path(temporary) / "login-input"
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-Wl,--wrap=read,--wrap=write,--wrap=tcgetattr,--wrap=tcsetattr",
                "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-idirafter", "userland/runtime/include",
                "tools/tests/installer_tty_input_test.c", "-o", str(executable),
            ], cwd=ROOT, check=True)
            subprocess.run([str(executable)], check=True, timeout=10)

    def test_setup_validation(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-components-test-") as temporary:
            root = Path(temporary)
            executable = root / "setup"
            subprocess.run([
                "cc", "-std=gnu11", "-O1", "-g",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi",
                "-idirafter", "userland/runtime/include", "tools/tests/installer_setup_validation_test.c",
                "userland/apps/installer/installer_setup.c", "userland/auth/standard_accounts.c",
                "userland/runtime/src/auth_password.c", "-o", str(executable),
            ], cwd=ROOT, check=True)
            subprocess.run([str(executable)], check=True, timeout=30)

    def test_legacy_accounts(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-setup-test-") as temporary:
            executable = Path(temporary) / "accounts"
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                '-DMBEDTLS_CONFIG_FILE="reliefos_mbedtls_config.h"',
                "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-Ithird_party/mbedtls/include",
                "-idirafter", "userland/runtime/include", "tools/tests/installer_accounts_test.c",
                "-Itools/tests/legacy_authd/include",
                "tools/tests/legacy_authd/accounts.c", "userland/runtime/src/auth_password.c",
                *CRYPTO, "-o", str(executable),
            ], cwd=ROOT, check=True)
            salt = bytes(range(16))
            derived = hashlib.pbkdf2_hmac("sha256", b"reference-password", salt, 100000)
            reference = f"$pbkdf2-sha256$100000${salt.hex()}${derived.hex()}"
            subprocess.run([str(executable)], check=True, timeout=30,
                           env={**os.environ, "LEONOS_TEST_PASSWORD_HASH": reference})


if __name__ == "__main__":
    unittest.main()
