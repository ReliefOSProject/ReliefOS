#!/usr/bin/env python3
"""Verify cross-repository test dispatch and honest host/guest evidence."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import test_fastfetch_qemu as inventory
import test_linux_ioctl_cloexec as ioctl
import test_security_regressions as security

ROOT = Path(__file__).resolve().parents[1]


class TestDelegation(unittest.TestCase):
    def test_guest_inventory_does_not_claim_kernel_host_pass(self):
        evidence = inventory.inventory_evidence("example-digest", 2, Path("guest.iso"))
        self.assertIn("Kernel host inventory: not run", evidence)
        self.assertNotIn("Host ASan/UBSan inventory: PASS", evidence)
        self.assertIn("VMware: not run", evidence)

    def test_sdk_reference_does_not_claim_kernel_unit_pass(self):
        with tempfile.TemporaryDirectory(prefix="ioctl-evidence-") as temporary:
            work = Path(temporary)
            probe = work / "build/ioctl-cloexec/linux-ioctl-cloexec.elf"
            probe.parent.mkdir(parents=True)
            probe.write_bytes(b"fixture probe")
            kernel = work / "kernel.sys"
            kernel.write_bytes(b"fixture kernel")
            output = work / "evidence.txt"
            with patch.object(ioctl, "ROOT", work):
                ioctl.write_evidence("Linux reference fixture", {"checks": 3, "failures": 0, "skips": 0},
                                     None, None, None, output=output, kernel=kernel)
            evidence = output.read_text()
            self.assertIn("kernel descriptor-table unit test: not run", evidence)
            self.assertNotIn("kernel descriptor-table unit test (real syscall.c, ASan/UBSan): PASS", evidence)
            self.assertIn("checks=3 failures=0 skips=0", evidence)

    def test_security_dispatch_preserves_kernel_failure_categories(self):
        import json
        report = {"kernel_private_ioctl_violations": ["kernel/example.c:1: forbidden request"],
                  "credential_regression_failures": []}
        child = subprocess.CompletedProcess([], 1, stdout=json.dumps(report), stderr="")
        with patch.object(security.subprocess, "run", return_value=child):
            self.assertEqual(security.kernel_report(), report)

    def dispatch(self, parallel):
        with tempfile.TemporaryDirectory(prefix="test-delegation-") as temporary:
            work = Path(temporary)
            child = work / "kernel checkout"
            child.mkdir()
            result = work / "delegated"
            result.mkdir()
            (child / "Makefile").write_text(
                ".PHONY: test-ownership test-smp-time\ntest-ownership test-smp-time:\n"
                "\t@mkdir -p '$(O)'; mkdir '$(O)/lock' || exit 1; "
                "trap \"rmdir '$(O)/lock'\" EXIT; sleep 0.2; "
                "printf '%s\\n' '$(RELIEFNT_TEST_PYTHON)' > '" + str(result) + "/$@'\n")
            harness = work / "Makefile"
            harness.write_text(
                f"RELIEFOS_SRC := {ROOT}\nRELIEFNT_DIR := {child}\n"
                "RELIEFOS_TEST_PYTHON := /selected/python-for-tests\n"
                f"O := {work}/output\nO_HOST := {work}/host\nQ := @\n"
                f"include {ROOT}/scripts/reliefos/Makefile.tests\n")
            goals = ["test-kernel-ownership", "test-smp-time"] if parallel else ["test-kernel-ownership"]
            command = subprocess.run(["make", "--no-print-directory", "-j2", "-f", str(harness), *goals],
                                     cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(command.returncode, 0, command.stdout + command.stderr)
            for name in (["test-ownership", "test-smp-time"] if parallel else ["test-ownership"]):
                self.assertEqual((result / name).read_text(), "/selected/python-for-tests\n")

    def test_make_dispatch_preserves_checkout_path_and_python(self):
        self.dispatch(False)

    def test_parallel_make_dispatch_uses_independent_output_locks(self):
        self.dispatch(True)


if __name__ == "__main__":
    unittest.main()
