#!/usr/bin/env python3
"""Check the boot media contract and the files in a standalone desktop root."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from make_live_root import make_live_tree
from make_ext2_root import write_ext2_root
from make_installer_root import share_identical_payload_files

ROOT = Path(__file__).resolve().parents[1]


class LiveRootTests(unittest.TestCase):
    def test_root_has_runtime_shell_and_unmodified_vim(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-live-test-") as directory:
            work = Path(directory)
            tree = work / "esp"
            for name, content in {
                "EFI/BOOT/BOOTX64.EFI": b"boot",
                "grub/grub.cfg": b"set root=(hd0,gpt1)",
                "reliefos/kernel.sys": b"kernel",
                "usr/lib/reliefos/apps/desktop/desktop.elf": b"desktop",
                "usr/lib/reliefos/apps/terminal/terminal.elf": b"terminal",
                "bin/busybox": b"shell",
                "usr/bin/vim": b"unmodified Linux executable",
                "usr/lib/reliefos/apps/vim/vim.elf": b"unmodified Linux executable",
                "usr/lib/reliefos/apps/vim/manifest.ini": b"commands=vim\nterminal=1\n",
                "usr/share/vim/vim91/defaults.vim": b"set nocompatible\n",
                "usr/share/terminfo/x/xterm": b"terminfo",
                "lib/ld-musl-x86_64.so.1": b"musl",
                "etc/sudoers": b"%wheel ALL=(ALL:ALL) ALL\n",
                "etc/skel/.profile": b"# test account skeleton\n",
                "opt/python/bin/python3.14": b"static Python executable",
                "opt/python/lib/python3.14/encodings/__init__.py": b"encodings",
            }.items():
                path = tree / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(content)
            (tree / "bin/sh").symlink_to("busybox")
            vim = tree / "usr/bin/vim"
            output = work / "live"
            make_live_tree(tree, output)
            self.assertEqual((output / "usr/bin/vim").read_bytes(), vim.read_bytes())
            self.assertTrue((output / "bin/sh").is_symlink())
            self.assertTrue((output / "usr/share/vim/vim91/defaults.vim").is_file())
            self.assertEqual((output / "usr/share/terminfo/x/xterm").read_bytes(), b"terminfo")
            self.assertTrue((output / "usr/lib/reliefos/apps/terminal/terminal.elf").is_file())
            self.assertFalse((output / "EFI").exists())
            self.assertFalse((output / "reliefos").exists())
            self.assertFalse((output / "leonos").exists())
            self.assertEqual((output / "opt/python/bin/python3.14").read_bytes(), b"static Python executable")
            self.assertEqual((output / "opt/python/lib/python3.14/encodings/__init__.py").read_bytes(), b"encodings")
            # The live desktop root is ext2; FAT cannot carry the real
            # /bin/sh and /var/run symlinks required by the layout.
            for name, content in (("xt_CONNMARK.h", b"upper"), ("xt_connmark.h", b"lower")):
                header = output / "opt/dyne/include" / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_bytes(content)
            image = work / "root.ext2"
            installed = output / "install/root/usr/bin/vim"
            installed.parent.mkdir(parents=True)
            installed.write_bytes((output / "usr/bin/vim").read_bytes())
            for name in ("var/lib/leonos/users.db", "install/root/var/lib/leonos/users.db"):
                seed = output / name
                seed.parent.mkdir(parents=True, exist_ok=True)
                seed.write_bytes(b"")
            share_identical_payload_files(output)
            self.assertEqual(installed.stat().st_ino, (output / "usr/bin/vim").stat().st_ino)
            self.assertNotEqual((output / "var/lib/leonos/users.db").stat().st_ino,
                                (output / "install/root/var/lib/leonos/users.db").stat().st_ino)
            write_ext2_root(output, image)
            stats = [subprocess.check_output(["debugfs", "-R", f"stat /{name}", image], text=True)
                     for name in ("usr/bin/vim", "install/root/usr/bin/vim")]
            self.assertEqual(stats[0].splitlines()[0], stats[1].splitlines()[0])
            for name, content in (("xt_CONNMARK.h", b"upper"), ("xt_connmark.h", b"lower")):
                extracted = subprocess.check_output(["debugfs", "-R", f"cat /opt/dyne/include/{name}", image])
                self.assertEqual(extracted, content)

    def test_grub_loads_the_root_from_cd_not_the_first_hard_disk(self):
        config = (ROOT / "boot/grub/live.cfg").read_text()
        self.assertNotIn("(hd", config)
        self.assertIn("search --no-floppy --file /reliefos-installer-iso.marker --set=root", config)
        self.assertIn("search --no-floppy --file /leonos-installer-iso.marker --set=root", config)
        self.assertIn("mode=live", config)
        self.assertNotIn("startup=", config)
        self.assertIn("module2 /install/root.fat reliefos-installer-root", config)
        self.assertIn("multiboot2 /reliefos/kernel.sys", config)


if __name__ == "__main__":
    unittest.main()
