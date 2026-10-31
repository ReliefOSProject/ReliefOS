#!/usr/bin/env python3
"""Validate installed kernel bundles through the native production host tool."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import test_kernel_modules_stage as module_fixtures
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent / "tests"))
import kernel_slot_oracle as slot_oracle

ROOT = Path(__file__).resolve().parents[1]
RELEASE = "5.1.2-ko1-0123456789ab"
ABI = "0123456789ab" + "0" * 52


class NativeBundle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="reliefos-native-module-tools-")
        cls.addClassCleanup(cls.build.cleanup)
        cls.binary = Path(cls.build.name) / "reliefos-kernel-tools"
        subprocess.run(["cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", str(ROOT),
                        "-I", str(ROOT / "userland/runtime/include"),
                        str(ROOT / "tools/host/modules/main.c"), str(ROOT / "tools/host/modules/bundle.c"),
                        *map(str, (ROOT / "tools/host/modules").glob("stage_*.c")),
                        str(ROOT / "tools/host/modules/modules_stage.c"),
                        str(ROOT / "tools/host/modules/initramfs_stage.c"), str(ROOT / "tools/host/modules/newc.c"),
                        str(ROOT / "tools/host/modules/payload_stage.c"),
                        str(ROOT / "tools/host/modules/release_info.c"),
                        str(ROOT / "tools/host/modules/slots.c"), str(ROOT / "userland/runtime/shared/boot_slots.c"),
                        str(ROOT / "tools/host/manifest/json.c"), str(ROOT / "tools/host/common/buffer.c"),
                        "-o", str(cls.binary)], check=True)

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="reliefos-native-bundle-")
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        self.install = self.work / "install with spaces"
        self.prefix = "lib/modules/" + RELEASE + "/"
        self.modules = self.install / self.prefix
        (self.modules / "kernel").mkdir(parents=True)
        for name, value in {"kernel.sys": b"native kernel", "kernel.debug": b"symbols",
                            "kerneldebug.sys": b"debug tool"}.items():
            (self.install / name).write_bytes(value)
        self.configuration = {"MODULES": "y", "EXT4_FS": "m", "FAT_FS": "y"}
        self.products = {"config": "".join("CONFIG_%s=%s\n" % item for item in self.configuration.items()),
                         "module.abi": ABI + "\n", "Module.symvers": "", "System.map": "",
                         "modules.order": "kernel/ext4.ko\n", "modules.builtin": "kernel/vfat.ko\n",
                         "modules.builtin.modinfo": "vfat.name=vfat\0", "kernel/ext4.ko": "module bytes"}
        for name, value in self.products.items():
            (self.modules / name).write_bytes(value.encode())
        self.publish()

    def publish(self):
        self.manifest = {"schema": 1, "arch": "x86_64", "profile": "debug", "release": RELEASE,
                         "module_abi": ABI, "module_directory": self.prefix[:-1],
                         "configuration": self.configuration.copy(), "modules": [self.prefix + "kernel/ext4.ko"],
                         "files": {str(p.relative_to(self.install)): {"sha256": hashlib.sha256(p.read_bytes()).hexdigest(),
                                                                     "size": p.stat().st_size}
                                   for p in self.install.rglob("*") if p.is_file() and p.name != "install.bundle.json"}}
        self.save()

    def save(self):
        (self.install / "install.bundle.json").write_text(json.dumps(self.manifest))

    def check(self, success=True, *requirements):
        result = subprocess.run([str(self.binary), "check-install", str(self.install), *requirements],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    def test_real_files_and_configuration(self):
        self.assertIn(RELEASE, self.check(True, "EXT4_FS", "FAT_FS").stdout)
        before = (self.install / "install.bundle.json").read_bytes()
        self.check(True, "EXT4_FS")
        self.assertEqual(before, (self.install / "install.bundle.json").read_bytes())
        self.check(False, "NVME")
        self.check(False, "../EXT4_FS")

    def test_kernel_module_and_metadata_corruption(self):
        for name in ("kernel.sys", self.prefix + "kernel/ext4.ko", self.prefix + "config"):
            with self.subTest(member=name):
                path = self.install / name
                before = path.read_bytes()
                path.write_bytes(before + b"corrupt")
                self.check(False)
                path.write_bytes(before)
        self.manifest["configuration"]["EXT4_FS"] = "y"
        self.save()
        self.check(False)

    def test_symlinks_never_escape_the_bundle(self):
        outside = self.work / "outside"
        outside.write_bytes((self.install / "kernel.sys").read_bytes())
        (self.install / "kernel.sys").unlink()
        (self.install / "kernel.sys").symlink_to(outside)
        self.check(False)
        (self.install / "kernel.sys").unlink()
        shutil.copyfile(outside, self.install / "kernel.sys")
        (self.modules / "kernel").rename(self.work / "outside-modules")
        (self.modules / "kernel").symlink_to(self.work / "outside-modules", target_is_directory=True)
        self.check(False)

    def test_exact_module_order_and_disabled_loader(self):
        (self.modules / "modules.order").write_text("kernel/ext4.ko\nkernel/ext4.ko\n")
        self.publish()
        self.check(False)
        (self.modules / "modules.order").write_text("kernel/ext4.ko\n")
        self.configuration["MODULES"] = "n"
        (self.modules / "config").write_text("".join("CONFIG_%s=%s\n" % item for item in self.configuration.items()))
        self.publish()
        self.check(False)

    def test_identity_required_members_and_path_containment(self):
        original = json.loads(json.dumps(self.manifest))
        for mutate in (lambda: self.manifest.update(module_abi="f" * 64),
                       lambda: self.manifest.update(module_directory="lib/modules/other"),
                       lambda: self.manifest["files"].pop("kerneldebug.sys"),
                       lambda: self.manifest["files"].update({"../escape": {"sha256": "0" * 64, "size": 0}}),
                       lambda: self.manifest["files"].update({self.prefix + "kernel/../escape": {"sha256": "0" * 64, "size": 0}})):
            self.manifest = json.loads(json.dumps(original))
            mutate()
            self.save()
            self.check(False)

    def test_empty_module_set_is_valid(self):
        (self.modules / "kernel/ext4.ko").unlink()
        (self.modules / "modules.order").write_text("")
        self.configuration["MODULES"] = "n"
        self.configuration["EXT4_FS"] = "y"
        (self.modules / "config").write_text("".join("CONFIG_%s=%s\n" % item for item in self.configuration.items()))
        self.publish()
        self.manifest["modules"] = []
        self.save()
        self.check(True, "EXT4_FS")

    def test_embedded_nul_cannot_truncate_manifest_or_text_products(self):
        original = json.loads(json.dumps(self.manifest))
        for field in ("release", "module_abi", "module_directory"):
            self.manifest = json.loads(json.dumps(original))
            self.manifest[field] += "\0ignored"
            self.save()
            self.check(False)
        self.manifest = original
        self.save()
        for name in ("config", "module.abi", "modules.order"):
            path = self.modules / name
            before = path.read_bytes()
            path.write_bytes(before + b"\0ignored")
            self.publish()
            self.check(False)
            path.write_bytes(before)
            self.publish()

    def test_nonregular_product_is_rejected_without_blocking(self):
        (self.install / "kernel.sys").unlink()
        os.mkfifo(self.install / "kernel.sys")
        self.check(False)

    def test_literal_backslash_unicode_text_is_preserved(self):
        self.configuration["PATTERN"] = r'"\u0000"'
        (self.modules / "config").write_text("".join("CONFIG_%s=%s\n" % item for item in self.configuration.items()))
        self.publish()
        self.check()

    def test_native_stage_uses_real_modinfo_depmod_and_preserves_previous(self):
        fixture = module_fixtures.ModulesStage("test_real_metadata_dependency_closure_and_stale_output")
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        command = [str(self.binary), "modules-stage", str(fixture.install),
                   str(module_fixtures.BUSYBOX), str(fixture.output), "EXT4_FS", "FAT_FS"]
        result = subprocess.run(command, text=True, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        metadata = json.loads((fixture.output / "modules-stage.json").read_text())
        self.assertEqual(metadata["early_modules"], ["helper", "rootfs"])
        self.assertEqual(metadata["boot_modules"], ["rootfs"])
        self.assertEqual(metadata["platform_modules"], [])
        self.assertTrue((fixture.output / f"lib/modules/{RELEASE}/modules-load.d/reliefnt-platform.conf").is_file())
        self.assertFalse((fixture.output / "usr/lib/modules-load.d/reliefnt-platform.conf").exists())
        dependencies = fixture.output / "lib/modules" / RELEASE / "modules.dep"
        self.assertIn("kernel/rootfs.ko: kernel/helper.ko", dependencies.read_text())
        previous = (fixture.output / "modules-stage.json").read_bytes()
        fixture.make_module("helper", "rootfs", "")
        fixture.update_manifest()
        result = subprocess.run(command, text=True, capture_output=True, timeout=20)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cycle", result.stderr)
        self.assertEqual(previous, (fixture.output / "modules-stage.json").read_bytes())
        for path in fixture.module_dir.glob("kernel/*.ko"):
            path.unlink()
        fixture.update_manifest(False)
        subprocess.run(command, check=True, timeout=20)
        self.assertEqual(list(fixture.output.rglob("*.ko")), [])
        self.assertEqual(json.loads((fixture.output / "modules-stage.json").read_text())["early_modules"], [])

    def test_native_newc_is_deterministic_and_has_only_early_dependency_closure(self):
        fixture = module_fixtures.ModulesStage("test_real_metadata_dependency_closure_and_stale_output")
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        subprocess.run([str(self.binary), "modules-stage", str(fixture.install),
                        str(module_fixtures.BUSYBOX), str(fixture.output)], check=True)
        archive = fixture.work / "early image.cpio"
        command = [str(self.binary), "initramfs-stage", str(ROOT), str(fixture.output),
                   str(module_fixtures.BUSYBOX), str(module_fixtures.BUSYBOX.with_name("busybox.links")),
                   str(archive), "1234567890", str(module_fixtures.RECOVERY)]
        result = subprocess.run(command, text=True, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        first = archive.read_bytes()
        subprocess.run(command, check=True)
        self.assertEqual(first, archive.read_bytes())
        self.check_newc(first)
        (fixture.output / f"lib/modules/{RELEASE}/kernel/helper.ko").write_bytes(b"corrupt")
        result = subprocess.run(command, text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(first, archive.read_bytes())

    def check_newc(self, first):
        at, entries = 0, {}
        while first[at:at + 6] == b"070701":
            fields = [int(first[at + 6 + i * 8:at + 14 + i * 8], 16) for i in range(13)]
            at += 110
            name = first[at:at + fields[11] - 1].decode()
            at = (at + fields[11] + 3) & ~3
            entries[name] = fields, first[at:at + fields[6]]
            at = (at + fields[6] + 3) & ~3
            self.assertEqual(fields[2:4], [0, 0])
            self.assertEqual(fields[5], 1234567890)
            if name == "TRAILER!!!":
                break
        self.assertIn("TRAILER!!!", entries)
        self.assertEqual(len(first) % 512, 0)
        self.assertEqual(entries["init"][1], (ROOT / "system/initramfs/init").read_bytes())
        self.assertEqual(entries["bin/sh"][1], b"busybox")
        self.assertEqual(entries["dev/console"][0][9:11], [5, 1])
        self.assertIn(f"lib/modules/{RELEASE}/kernel/helper.ko", entries)
        self.assertNotIn(f"lib/modules/{RELEASE}/kernel/optional.ko", entries)

    def test_native_payload_and_boot_pair_reject_mixed_or_corrupted_products(self):
        fixture = module_fixtures.ModulesStage("test_real_metadata_dependency_closure_and_stale_output")
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        subprocess.run([str(self.binary), "modules-stage", str(fixture.install),
                        str(module_fixtures.BUSYBOX), str(fixture.output)], check=True)
        archive = fixture.work / "early.cpio"
        subprocess.run([str(self.binary), "initramfs-stage", str(ROOT), str(fixture.output),
                        str(module_fixtures.BUSYBOX), str(module_fixtures.BUSYBOX.with_name("busybox.links")),
                        str(archive), "0", str(module_fixtures.RECOVERY)], check=True)
        output = fixture.work / "payload"
        command = [str(self.binary), "kernel-payload", str(fixture.install), str(fixture.output), str(archive), str(output)]
        result = subprocess.run(command, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        previous = (output / "kernel-payload.json").read_bytes()
        manifest = json.loads(previous)
        self.assertEqual((output / manifest["initramfs"]).read_bytes(), archive.read_bytes())
        esp = fixture.work / "private-esp"
        esp.mkdir()
        boot_command = [str(self.binary), "boot-payload", str(fixture.install / "kernel.sys"), str(archive), str(esp)]
        subprocess.run(boot_command, check=True)
        self.assertEqual((esp / f"reliefos/versions/{RELEASE}/{hashlib.sha256(archive.read_bytes()).hexdigest()}/initramfs.cpio").read_bytes(), archive.read_bytes())
        selected = json.loads(subprocess.check_output([str(self.binary), "slot-select", str(esp)]))
        self.assertEqual((selected["release"], selected["generation"]), (RELEASE, 1))
        original_kernel = (esp / "reliefos/kernel.sys").read_bytes()
        self_copy = [str(self.binary), "boot-payload", str(esp / "reliefos/kernel.sys"), str(archive), str(esp)]
        self.assertNotEqual(subprocess.run(self_copy, capture_output=True).returncode, 0)
        self.assertEqual((esp / "reliefos/kernel.sys").read_bytes(), original_kernel)
        archive.write_bytes(archive.read_bytes() + b"corrupt")
        for arguments in (command, boot_command):
            result = subprocess.run(arguments, text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("checksum", result.stderr)
        self.assertEqual(previous, (output / "kernel-payload.json").read_bytes())
        metadata = Path(str(archive) + ".json")
        data = json.loads(metadata.read_bytes())
        data["release"] = "wrong"
        metadata.write_text(json.dumps(data))
        result = subprocess.run(command, text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("identity mismatch", result.stderr)

    def test_native_slot_codec_matches_standard_grub_oracle(self):
        fields = dict(generation=2, release=RELEASE, module_abi=ABI,
                      kernel_sha256="a" * 64, initramfs_sha256="b" * 64)
        command = [str(self.binary), "slot-encode", *map(str, fields.values())]
        result = subprocess.run(command, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, slot_oracle.encode(fields))
        for index, invalid in ((2, "00"), (2, "2147483648"), (3, "../../escape"), (4, "f" * 64)):
            arguments = command.copy()
            arguments[index] = invalid
            self.assertNotEqual(subprocess.run(arguments, capture_output=True).returncode, 0)

    def test_release_metadata_binds_signed_package_index_and_complete_runtime_bundle(self):
        fixture = module_fixtures.ModulesStage("test_real_metadata_dependency_closure_and_stale_output")
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        fixture.stage()
        archive = fixture.work / "release.cpio"
        subprocess.run([str(self.binary), "initramfs-stage", str(ROOT), str(fixture.output),
                        str(module_fixtures.BUSYBOX), str(module_fixtures.BUSYBOX.with_name("busybox.links")),
                        str(archive), "0", str(module_fixtures.RECOVERY)], check=True)
        payload = fixture.work / "release-payload"
        subprocess.run([str(self.binary), "kernel-payload", str(fixture.install), str(fixture.output), str(archive), str(payload)], check=True)
        package, index = fixture.work / "reliefos-kernel-1.5.1.2-r0.apk", fixture.work / "packages.adb"
        package.write_bytes(b"package encoder is checked by separate real signed APK integration")
        index.write_bytes(b"signed index integrity is checked by the real upstream APK tests")
        command = [str(self.binary), "release-info", str(payload), str(fixture.install / "kernel.sys"),
                   str(package), str(index), "5.1.2", str(module_fixtures.RECOVERY)]
        result = subprocess.run(command, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        fields = dict(line.split("=", 1) for line in result.stdout.splitlines())
        self.assertEqual(fields["format_version"], "4")
        self.assertEqual(fields["kernel_release"], RELEASE)
        self.assertEqual(fields["module_abi"], ABI)
        self.assertEqual(fields["package"], "reliefos-kernel")
        self.assertEqual(fields["package_version"], "1.5.1.2-r0")
        self.assertEqual(fields["package_sha256"], hashlib.sha256(package.read_bytes()).hexdigest())
        self.assertEqual(fields["repository_sha256"], hashlib.sha256(index.read_bytes()).hexdigest())
        self.assertEqual(fields["initramfs_sha256"], hashlib.sha256(archive.read_bytes()).hexdigest())
        (payload / f"lib/modules/{RELEASE}/kernel/helper.ko").write_bytes(b"corrupt")
        refused = subprocess.run(command, text=True, capture_output=True)
        self.assertNotEqual(refused.returncode, 0)
        self.assertEqual(refused.stdout, "")

    def test_native_slot_selector_falls_back_from_torn_records_and_payloads(self):
        esp = self.work / "slot-esp"
        state = esp / "reliefos/state"
        state.mkdir(parents=True)
        fields_by_slot = {}
        for slot, generation in (("A", 1), ("B", 2)):
            release = "5.1.2-ko1-" + ("%012x" % generation)
            kernel, archive = b"kernel" + bytes([generation]), b"newc" + bytes([generation])
            fields = dict(generation=generation, release=release, module_abi=("%012x" % generation) + "0" * 52,
                          kernel_sha256=hashlib.sha256(kernel).hexdigest(), initramfs_sha256=hashlib.sha256(archive).hexdigest())
            slot_oracle.write_version(esp, fields, kernel, archive)
            record = slot_oracle.encode(fields)
            (state / f"kernel-{slot}.env").write_bytes(record)
            (state / f"kernel-{slot}.sha256").write_bytes(slot_oracle.checksum(slot, record))
            fields_by_slot[slot] = fields
        command = [str(self.binary), "slot-select", str(esp)]
        result = subprocess.run(command, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)["slot"], "B")
        original = (state / "kernel-B.env").read_bytes()
        for offset in (0, 24, 64, 128, 511, 1023):
            torn = bytearray(original)
            torn[offset] ^= 1
            (state / "kernel-B.env").write_bytes(torn)
            self.assertEqual(json.loads(subprocess.check_output(command))["slot"], "A")
        (state / "kernel-B.env").write_bytes(original)
        (esp / "reliefos/versions" / fields_by_slot["B"]["release"] /
         fields_by_slot["B"]["initramfs_sha256"] / "initramfs.cpio").write_bytes(b"corrupt")
        self.assertEqual(json.loads(subprocess.check_output(command))["slot"], "A")
        (state / "kernel-A.sha256").write_bytes(b"")
        self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)


if __name__ == "__main__":
    unittest.main()
