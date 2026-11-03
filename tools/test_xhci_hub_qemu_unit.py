#!/usr/bin/env python3
"""Reject incomplete or misrouted Hub integration evidence."""
import subprocess
import unittest

import test_xhci_hub_qemu as qa

USB = '''  Device 0.1, Port 1, Speed 12 Mb/s, Product QEMU USB Hub, ID: hub0
  Device 0.2, Port 1.2, Speed 12 Mb/s, Product QEMU USB Hub, ID: hub1
  Device 0.3, Port 1.2.1, Speed 12 Mb/s, Product QEMU USB Keyboard, ID: kbd
  Device 0.4, Port 1.2.2, Speed 12 Mb/s, Product QEMU USB Mouse, ID: mouse
  Device 0.5, Port 1.3, Speed 12 Mb/s, Product QEMU USB MSD, ID: flash
'''
SERIAL = '''[usb] xHCI device slot=1 address=1 root_port=9 psi=1 speed=FULL bps=12000000 psi_raw=00000000 psi_source=implied vid=0409 pid=55aa route=00000 parent=0 port=0
[usb] xHCI device slot=2 address=2 root_port=9 psi=1 speed=FULL bps=12000000 psi_raw=00000000 psi_source=implied vid=0409 pid=55aa route=00002 parent=1 port=2
[usb] xHCI device slot=3 address=3 root_port=9 psi=1 speed=FULL bps=12000000 psi_raw=00000000 psi_source=implied vid=0627 pid=0001 route=00012 parent=2 port=1
[usb] xHCI device slot=4 address=4 root_port=9 psi=1 speed=FULL bps=12000000 psi_raw=00000000 psi_source=implied vid=0627 pid=0001 route=00022 parent=2 port=2
[usb] xHCI device slot=5 address=5 root_port=9 psi=1 speed=FULL bps=12000000 psi_raw=00000000 psi_source=implied vid=46f4 pid=0001 route=00003 parent=1 port=3
'''


class HubEvidence(unittest.TestCase):
    def test_hotplug_numeric_properties_use_qmp_types(self):
        parameters = qa.hotplug_arguments('usb-kbd,id=kbd,bus=xhci.0,port=1.2.1,usb_version=1')
        self.assertEqual(parameters['port'], '1.2.1')
        self.assertEqual(parameters['usb_version'], 1)
        self.assertEqual(qa.hotplug_arguments('usb-hub,id=h,ports=4')['ports'], 4)

    def test_dotted_physical_paths_match_each_guest_route(self):
        result = qa.evidence(USB, SERIAL)
        self.assertEqual(result['kbd']['route'], 0x12)
        self.assertEqual(result['flash']['qemu']['port'], '1.3')

    def test_prior_enumeration_cannot_prove_a_replacement(self):
        marker = 'HUB_TREE_REMOVE_READY'
        with self.assertRaises(AssertionError):
            qa.evidence(USB, SERIAL + marker + '\n', after=marker)
        self.assertEqual(len(qa.evidence(USB, marker + '\n' + SERIAL, after=marker)), 5)

    def test_wrong_parent_speed_or_missing_device_cannot_pass(self):
        for usb, serial in ((USB, SERIAL.replace('parent=2 port=1', 'parent=1 port=1')),
                            (USB.replace('Port 1.3', 'Port 1.4'), SERIAL),
                            (USB, SERIAL.replace('speed=FULL', 'speed=HIGH', 1)),
                            (USB, SERIAL.rsplit('\n', 2)[0] + '\n')):
            with self.subTest(usb=usb, serial=serial), self.assertRaises(AssertionError):
                qa.evidence(usb, serial)

    def test_generated_guest_programs_have_valid_shell_syntax(self):
        for modules in (False, True):
            script = qa.guest_script('http://10.0.2.2:1234/abc', 'a' * 64,
                                     '5.3.0-test', modules, 'abc', 'xhci')
            checked = subprocess.run(['sh', '-n'], input=script, capture_output=True)
            self.assertEqual(checked.returncode, 0, checked.stderr)


if __name__ == '__main__':
    unittest.main()
