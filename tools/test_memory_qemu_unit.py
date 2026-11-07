#!/usr/bin/env python3
"""Offline checks for the memory VM runner's evidence parsing."""
import unittest

from test_memory_qemu import parse_serial


class MemorySerialTest(unittest.TestCase):
    nonce = "0123456789abcdef"
    release = "1.2.3-abcdef123456"

    def serial(self, body, status=0):
        return (f"MEMORY_RUN_START nonce={self.nonce} release={self.release}\n"
                f"{body}\nMEMORY_RUN_DONE nonce={self.nonce} status={status}\n")

    def test_reports_observations_and_skips_without_inventing_a_pass(self):
        body = ("LIFECYCLE_PASS case=private_fork_cow bytes=8192\n"
                "LIFECYCLE_SKIP case=evicted_pages reason=insufficient_observed_eviction\n"
                "PRESSURE_MINCORE pages=10 resident=10 nonresident=0")
        result = parse_serial(self.serial(body), self.nonce, self.release)
        self.assertEqual(result["lifecycle_cases"], ["private_fork_cow"])
        self.assertEqual(result["lifecycle_skips"][0]["case"], "evicted_pages")
        self.assertEqual(result["pressure_mincore"][0]["nonresident"], 0)
        self.assertFalse(result["pressure_pass_marker"])
        self.assertFalse(result["oom_pass_marker"])

    def test_requires_fresh_successful_markers(self):
        for text in (self.serial("", 1), self.serial("") * 2,
                     self.serial("").replace(self.nonce, "other")):
            with self.subTest(text=text), self.assertRaises(ValueError):
                parse_serial(text, self.nonce, self.release)

    def test_probe_failure_or_kernel_panic_rejects_success_status(self):
        for body in ("LIFECYCLE_FAIL case=checked_lock errno=12", "KERNEL PANIC"):
            with self.subTest(body=body), self.assertRaises(ValueError):
                parse_serial(self.serial(body), self.nonce, self.release)


if __name__ == "__main__":
    unittest.main()
