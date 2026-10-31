#!/usr/bin/env python3
"""Select only a complete filesystem boot module for owned VM diagnostics."""
import unittest
from test_installer_accounts_qemu import installer_root_extent


class BootExtent(unittest.TestCase):
    def test_initramfs_and_root_are_independent_of_their_module_order(self):
        initial = "[boot] module=0 name=initramfs start=0x120000 bytes=2224640\n"
        root = "[boot] module=1 name=reliefos-installer-root start=0xa2f6000 bytes=1577058304\n"
        for text in (initial + root, root + initial):
            self.assertEqual(installer_root_extent(text), (0xa2f6000, 1577058304))

    def test_missing_duplicate_and_invalid_extents_are_refused(self):
        root = "[boot] module=1 name=reliefos-installer-root start=0x100000 bytes=4096\n"
        for text in ("", root + root,
                     root.replace("0x100000", "0x0"),
                     root.replace("4096", "4097"),
                     root.replace("0x100000", "0xfffffffffffff800")):
            with self.assertRaises(AssertionError):
                installer_root_extent(text)


if __name__ == "__main__":
    unittest.main()
