#!/usr/bin/env python3
"""Check image staging using actual ELF modinfo and the target BusyBox tools."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUSYBOX = Path(os.environ.get("RELIEFOS_TEST_BUSYBOX", ROOT / "out/kbuild-os-native/userland/initramfs/busybox.elf"))
KERNEL_TOOLS = Path(os.environ.get("RELIEFOS_KERNEL_TOOLS", ROOT / "out/kbuild-os-native/host/bin/reliefos-kernel-tools"))
RECOVERY = Path(os.environ.get("RELIEFOS_TEST_RECOVERY", ROOT / "out/kbuild-os-native/userland/initramfs/kernel-bundle.elf"))
RELEASE = "5.1.2-ko1-0123456789ab"
ABI = "0123456789ab" + "0" * 52


class ModulesStage(unittest.TestCase):
    def setUp(self):
        if not BUSYBOX.is_file():
            self.skipTest("build upstream-initramfs-busybox or set RELIEFOS_TEST_BUSYBOX")
        self.temporary = tempfile.TemporaryDirectory(prefix="reliefos-module-stage-")
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        self.install = self.work / "install"
        self.module_dir = self.install / "lib/modules" / RELEASE
        (self.module_dir / "kernel").mkdir(parents=True)
        self.output = self.work / "stage"
        for name, data in {"kernel.sys": b"kernel fixture", "kerneldebug.sys": b"debug tool",
                           "kernel.debug": b"kernel symbols"}.items():
            (self.install / name).write_bytes(data)
        for name, data in {"config": b"CONFIG_MODULES=y\nCONFIG_EXT4_FS=m\nCONFIG_FAT_FS=y\n",
                           "module.abi": (ABI + "\n").encode(), "modules.order": b"",
                           "Module.symvers": b"", "System.map": b"",
                           "modules.builtin": b"kernel/builtin.ko\n",
                           "modules.builtin.modinfo": b"builtin.name=builtin\0"}.items():
            (self.module_dir / name).write_bytes(data)
        self.make_module("helper", "", "")
        self.make_module("rootfs", "helper", "filesystem")
        self.make_module("optional", "", "")
        self.update_manifest()

    def make_module(self, name, dependencies, phase, magic=None):
        source = self.work / (name + ".c")
        records = {"reliefnt_phase": phase} if phase else {}
        records.update({"name": name, "depends": dependencies, "vermagic": magic or f"{RELEASE} ReliefNT-1 {ABI}"})
        source.write_text("int stub(void) { return 55; }\n" + "\n".join('static const char v%d[] __attribute__((used,section(".modinfo")))=%s;'
                                    % (i, json.dumps(key + "=" + value))
                                    for i, (key, value) in enumerate(records.items())) + "\n")
        subprocess.run(["cc", "-c", str(source), "-o", str(self.module_dir / "kernel" / (name + ".ko"))], check=True)
        # A section's first record has no preceding NUL requirement. Force a
        # nonzero padding byte outside that section, which BusyBox's old raw
        # file scan incorrectly interpreted as part of the metadata record.
        path = self.module_dir / "kernel" / (name + ".ko")
        data = bytearray(path.read_bytes())
        table = struct.unpack_from("<Q", data, 40)[0]
        size, count, names = struct.unpack_from("<HHH", data, 58)
        strings = struct.unpack_from("<Q", data, table + names * size + 24)[0]
        for index in range(count):
            header = table + index * size
            field = strings + struct.unpack_from("<I", data, header)[0]
            if data[field:field + 9] == b".modinfo\0":
                offset = struct.unpack_from("<Q", data, header + 24)[0]
                self.assertGreater(offset, 64)
                data[offset - 1] = 0x55
                path.write_bytes(data)
                break

    def update_manifest(self, modules=True):
        ko = sorted(self.module_dir.glob("kernel/*.ko")) if modules else []
        (self.module_dir / "modules.order").write_text("".join("kernel/" + p.name + "\n" for p in ko))
        config = {"MODULES": "y", "EXT4_FS": "m" if modules else "y", "FAT_FS": "y"}
        (self.module_dir / "config").write_text("".join("CONFIG_%s=%s\n" % item for item in config.items()))
        files = {}
        for file in self.install.rglob("*"):
            if file.is_file() and file.name != "install.bundle.json":
                files[str(file.relative_to(self.install))] = {"sha256": hashlib.sha256(file.read_bytes()).hexdigest(),
                                                             "size": file.stat().st_size}
        data = {"schema": 1, "arch": "x86_64", "profile": "debug", "release": RELEASE, "module_abi": ABI,
                "module_directory": "lib/modules/" + RELEASE, "configuration": config,
                "files": files, "modules": [str(p.relative_to(self.install)) for p in ko]}
        (self.install / "install.bundle.json").write_text(json.dumps(data))

    def stage(self, success=True, *requirements):
        result = subprocess.run([str(KERNEL_TOOLS), "modules-stage", str(self.install), str(BUSYBOX),
                                 str(self.output), *requirements], capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    def test_real_metadata_dependency_closure_and_stale_output(self):
        self.stage(True, "EXT4_FS", "FAT_FS")
        data = json.loads((self.output / "modules-stage.json").read_text())
        self.assertEqual(data["boot_modules"], ["rootfs"])
        self.assertEqual(data["early_modules"], ["helper", "rootfs"])
        dep = self.output / "lib/modules" / RELEASE / "modules.dep"
        self.assertIn("kernel/rootfs.ko: kernel/helper.ko", dep.read_text())
        for file in self.module_dir.glob("kernel/*.ko"):
            file.unlink()
        self.update_manifest(False)
        self.stage(True, "EXT4_FS")
        self.assertEqual(list(self.output.rglob("*.ko")), [])
        self.assertEqual(json.loads((self.output / "modules-stage.json").read_text())["early_modules"], [])

    def test_wrong_abi_missing_dependency_and_required_feature(self):
        self.stage()
        before = (self.output / "modules-stage.json").read_bytes()
        self.make_module("rootfs", "helper", "filesystem", "wrong ReliefNT-1 " + ABI)
        self.update_manifest()
        self.stage(False)
        self.assertEqual((self.output / "modules-stage.json").read_bytes(), before)
        self.make_module("rootfs", "missing", "filesystem")
        self.update_manifest()
        self.stage(False)
        self.make_module("rootfs", "helper", "filesystem")
        self.update_manifest()
        self.stage(False, "SATA_AHCI")

    def test_corrupt_bytes_and_symlink_escape(self):
        (self.module_dir / "kernel/rootfs.ko").write_bytes(b"changed after install")
        self.stage(False)
        (self.module_dir / "kernel/rootfs.ko").unlink()
        (self.module_dir / "kernel/rootfs.ko").symlink_to(self.install / "kernel.sys")
        self.stage(False)


if __name__ == "__main__":
    unittest.main()
