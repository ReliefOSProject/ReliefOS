#!/usr/bin/env python3
"""Check standalone image accounts without building a complete OS image."""
import ctypes
import ctypes.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from make_image import make_root_tree
from make_live_root import make_live_tree
from make_ext2_root import populate_ext2

ROOT = Path(__file__).resolve().parents[1]


class ImageAccounts(unittest.TestCase):
    def test_disk_and_live_accounts(self):
        crypt = ctypes.CDLL(ctypes.util.find_library("crypt"))
        crypt.crypt.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        crypt.crypt.restype = ctypes.c_char_p
        base_video = [line.split(":") for line in
                      (ROOT / "system/rootfs/etc/group").read_text().splitlines()
                      if line.startswith("video:")]
        test_video = [line.split(":") for line in
                      (ROOT / "system/test-accounts/group").read_text().splitlines()
                      if line.startswith("video:")]
        self.assertEqual(base_video, [["video", "x", "44", ""]])
        self.assertEqual(test_video, [["video", "x", "44", "test"]])
        self.assertIn("video:!::\n", (ROOT / "system/rootfs/etc/gshadow").read_text())
        self.assertIn("video:!::test\n", (ROOT / "system/test-accounts/gshadow").read_text())
        with tempfile.TemporaryDirectory(prefix="reliefos-image-accounts-") as temporary:
            work = Path(temporary)
            source = work / "source"
            shutil.copytree(ROOT / "system/rootfs", source, symlinks=True)
            original_shadow = (source / "etc/shadow").read_bytes()
            for kind in ("vmdk", "iso"):
                with self.subTest(kind=kind):
                    stage = work / kind
                    if kind == "vmdk":
                        make_root_tree(source, stage, "en")
                    else:
                        make_live_tree(source, stage)
                    passwd = {line.split(":")[0]: line.split(":")
                              for line in (stage / "etc/passwd").read_text().splitlines()}
                    self.assertEqual(passwd["test"][2:4], ["1000", "1000"])
                    self.assertEqual(passwd["root"][2:4], ["0", "0"])
                    shadow = dict(line.split(":", 2)[:2]
                                  for line in (stage / "etc/shadow").read_text().splitlines())
                    changes = {line.split(":")[0]: line.split(":")[2]
                               for line in (stage / "etc/shadow").read_text().splitlines()}
                    for user in ("root", "test"):
                        self.assertGreater(int(changes[user]), 0)
                        self.assertEqual(crypt.crypt(user.encode(), shadow[user].encode()), shadow[user].encode())
                        self.assertNotEqual(crypt.crypt(b"incorrect", shadow[user].encode()), shadow[user].encode())
                    self.assertIn("wheel:x:10:test\n", (stage / "etc/group").read_text())
                    self.assertIn("wheel:!::test\n", (stage / "etc/gshadow").read_text())
                    self.assertIn("video:x:44:test\n", (stage / "etc/group").read_text())
                    self.assertIn("video:!::test\n", (stage / "etc/gshadow").read_text())
                    self.assertIn("%wheel ALL=(ALL:ALL) ALL", (stage / "etc/sudoers").read_text().splitlines())
                    self.assertTrue((stage / "etc/reliefos/installed").is_file())
                    self.assertEqual((stage / "etc/shadow").stat().st_mode & 0o777, 0o600)
                    self.assertEqual((stage / "home/test").stat().st_mode & 0o777, 0o700)
                    image = work / f"{kind}.ext2"
                    with image.open("wb") as stream:
                        stream.truncate(32 << 20)
                    populate_ext2(stage, image, 8192)
                    for path, uid, gid, mode in (("/home/test", 1000, 1000, "0700"),
                                                 ("/home/test/desktop/Terminal.lnk", 1000, 1000, "0644"),
                                                 ("/root", 0, 0, "0700"),
                                                 ("/etc/skel/desktop/Terminal.lnk", 0, 0, "0644"),
                                                 ("/etc/shadow", 0, 0, "0600"),
                                                 ("/etc/sudoers", 0, 0, "0440")):
                        info = subprocess.check_output(["debugfs", "-R", f"stat {path}", str(image)],
                                                       stderr=subprocess.DEVNULL, text=True)
                        self.assertRegex(info, rf"User:\s+{uid}\s+Group:\s+{gid}\b")
                        self.assertIn(f"Mode:  {mode}", info)
            self.assertEqual((source / "etc/shadow").read_bytes(), original_shadow)
            self.assertFalse((source / "home/test").exists())
            self.assertFalse((source / "etc/reliefos/installed").exists())
            image = work / "installer-seed.ext2"
            with image.open("wb") as stream:
                stream.truncate(32 << 20)
            populate_ext2(source, image, 8192)
            content = subprocess.check_output(["debugfs", "-R", "cat /etc/shadow", str(image)],
                                              stderr=subprocess.DEVNULL, text=True)
            self.assertEqual(content, original_shadow.decode())


if __name__ == "__main__":
    unittest.main()
