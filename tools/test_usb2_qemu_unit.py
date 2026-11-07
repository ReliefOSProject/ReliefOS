#!/usr/bin/env python3
"""Host contracts for USB OS-QA command and evidence handling."""
import hashlib
import http.client
import inspect
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import unittest

import test_usb2_qemu as qa


USB = '''  Device 0.1, Port 1, Speed 5000 Mb/s, Product QEMU USB MSD, ID: flash0
  Device 0.2, Port 4, Speed 5000 Mb/s, Product QEMU USB MSD, ID: flash1
  Device 0.3, Port 5, Speed 5000 Mb/s, Product QEMU USB MSD, ID: flash2
  Device 0.4, Port 2, Speed 480 Mb/s, Product QEMU USB Keyboard, ID: kbd
  Device 0.5, Port 3, Speed 480 Mb/s, Product QEMU USB Mouse, ID: mouse
'''
QTREE = '''  bus: pcie.0
    dev: qemu-xhci, id "xhci"
      class USB controller, addr 00:04.0, pci id 1b36:000d (sub 1af4:1100)
      bus: xhci.0
        type usb-bus
        dev: usb-storage, id "flash0"
          drive = ""
          bus: scsi0
            type SCSI
            dev: scsi-hd, id ""
              drive = "stick0"
        dev: usb-storage, id "flash1"
          drive = ""
          bus: scsi1
            type SCSI
            dev: scsi-hd, id ""
              drive = "stick1"
        dev: usb-storage, id "flash2"
          drive = ""
          bus: scsi2
            type SCSI
            dev: scsi-hd, id ""
              drive = "stick2"
        dev: usb-kbd, id "kbd"
        dev: usb-mouse, id "mouse"
    dev: nvme, id ""
'''
SERIAL = '''[ 1.23] [INFO] [usb] xHCI pci=0:4.0 ports=16 slots=64 context=32 ac64=1 scratchpads=0
[ 2.01] [INFO] [usb] xHCI device slot=1 address=1 root_port=1 psi=4 speed=SUPER bps=5000000000 psi_raw=00000000 psi_source=implied vid=46f4 pid=0001
[ 2.02] [INFO] [usb] xHCI device slot=2 address=2 root_port=4 psi=4 speed=SUPER bps=5000000000 psi_raw=00000000 psi_source=implied vid=46f4 pid=0001
[ 2.03] [INFO] [usb] xHCI device slot=3 address=3 root_port=5 psi=4 speed=SUPER bps=5000000000 psi_raw=00000000 psi_source=implied vid=46f4 pid=0001
[ 2.04] [INFO] [usb] xHCI device slot=4 address=4 root_port=10 psi=3 speed=HIGH bps=480000000 psi_raw=00000000 psi_source=implied vid=0627 pid=0001
[ 2.05] [INFO] [usb] xHCI device slot=5 address=5 root_port=11 psi=3 speed=HIGH bps=480000000 psi_raw=00000000 psi_source=implied vid=0627 pid=0001
'''


class Topology(unittest.TestCase):
    def test_cli_exposes_firmware_and_both_xhci_models(self):
        process = subprocess.run(['python3', str(Path(qa.__file__)), '--help'], capture_output=True, text=True)
        self.assertEqual(process.returncode, 0)
        self.assertIn('nec-usb-xhci', process.stdout)
        self.assertIn('qemu-xhci', process.stdout)
        self.assertIn('--firmware', process.stdout)
        self.assertIn('--hid-usb-version', process.stdout)

    def test_controller_configuration_and_hid_defaults(self):
        builder = getattr(qa, 'topology_devices', None)
        self.assertTrue(callable(builder), 'runner needs a topology builder')
        sticks = [Path('/media/one'), Path('/media/two'), Path('/media/three')]
        for model in ('qemu-xhci', 'nec-usb-xhci'):
            bus, devices = builder(model, '1', sticks)
            self.assertEqual(bus, 'xhci.0')
            self.assertIn(model + ',id=xhci,p2=8,p3=8', devices)
            self.assertIn('usb-storage,bus=xhci.0,drive=stick1,id=flash1,port=4', devices)
            self.assertIn('usb-kbd,bus=xhci.0,id=kbd,port=2,usb_version=1', devices)
            self.assertIn('usb-mouse,bus=xhci.0,id=mouse,port=3,usb_version=1', devices)
        _, high = builder('highspeed', None, sticks)
        _, companion = builder('companion', None, sticks)
        self.assertIn('usb-ehci,id=ehci', high)
        self.assertIn('usb-kbd,bus=ehci.0,id=kbd,port=2,usb_version=2', high)
        self.assertIn('usb-kbd,bus=ehci.0,id=kbd,port=2,usb_version=1', companion)

    def test_xhci_rejects_a_probe_that_does_not_match_its_identity_record(self):
        check = getattr(qa, 'verified_probe', None)
        self.assertTrue(callable(check), 'runner needs a verified probe input')
        with tempfile.TemporaryDirectory() as directory:
            probe = Path(directory) / 'probe'
            identity = Path(directory) / 'probe-identity.json'
            probe.write_bytes(b'accepted probe')
            identity.write_text(json.dumps({'probe_sha256': hashlib.sha256(b'accepted probe').hexdigest()}))
            self.assertEqual(check(probe, identity, True), b'accepted probe')
            probe.write_bytes(b'unexpected probe')
            with self.assertRaises(ValueError):
                check(probe, identity, True)
            with self.assertRaises(ValueError):
                check(probe, None, True)

    def test_uefi_uses_shared_firmware_read_only_and_low_mmio(self):
        helper = getattr(qa, 'firmware_options', None)
        self.assertTrue(callable(helper), 'runner needs firmware options')
        self.assertIn('name=opt/org.seabios/pci64,string=0', helper('bios'))
        with tempfile.NamedTemporaryFile() as firmware:
            old = os.environ.get('QEMU_FIRMWARE')
            os.environ['QEMU_FIRMWARE'] = firmware.name
            try:
                options = helper('uefi')
            finally:
                if old is None:
                    del os.environ['QEMU_FIRMWARE']
                else:
                    os.environ['QEMU_FIRMWARE'] = old
            self.assertEqual(options[options.index('-bios') + 1], firmware.name)
            self.assertIn('name=opt/ovmf/X-PciMmio64Mb,string=0', options)
            self.assertFalse(any('pflash' in item for item in options))

    def test_each_owned_medium_contains_its_distinct_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            for name, layout in [('stick', 'mbr'), ('readonly', 'gpt'), ('whole', 'whole')]:
                path = qa.make_stick(output, name, layout)
                image = str(path) + ('' if layout == 'whole' else '@@1048576')
                token = subprocess.run(['mtype', '-i', image, '::fixture.txt'], capture_output=True)
                self.assertEqual(token.returncode, 0, 'missing fixture identity')
                self.assertEqual(token.stdout, (name + '\n').encode())
                payload = subprocess.check_output(['mtype', '-i', image, '::payload.bin'])
                self.assertEqual(hashlib.sha256(payload).hexdigest(), qa.PAYLOAD_HASH)


class Evidence(unittest.TestCase):
    def test_live_wait_defers_an_unterminated_exit_nonce_prefix(self):
        class Process:
            def poll(self): return None
        class Serial:
            def __init__(self, records): self.records = iter(records)
            def exists(self): return True
            def read_text(self, **kwargs): return next(self.records)
        for nonce in ('89756e45cc', '1234567890'):
            with self.subTest(nonce=nonce):
                marker = 'USB_QA_OK:' + nonce
                partial = 'USB_QA_EXIT:' + nonce[:5]
                self.assertFalse(qa.guest_failed(partial))
                serial = Serial([partial, marker + '\nUSB_QA_EXIT:' + nonce + ':0\n'])
                qa.wait_marker(serial, marker, Process(), timeout=1)
                self.assertFalse(qa.guest_failed('USB_QA_EXIT:' + nonce + ':10'))
                self.assertTrue(qa.guest_failed('USB_QA_EXIT:' + nonce + ':10\n'))

    def test_live_wait_does_not_read_a_numeric_nonce_as_the_exit_status(self):
        class Process:
            def poll(self): return None
        with tempfile.TemporaryDirectory() as directory:
            serial = Path(directory) / 'serial.log'
            for nonce in ('89756e45cc', '1234567890', '0000000001', 'abcdef1234'):
                with self.subTest(nonce=nonce):
                    marker = 'USB_QA_OK:' + nonce
                    serial.write_text(marker + '\nUSB_QA_EXIT:' + nonce + ':0\n')
                    qa.wait_marker(serial, marker, Process(), timeout=.1)
                    for status in (1, 10, 32):
                        serial.write_text(marker + '\nUSB_QA_EXIT:' + nonce + ':' + str(status) + '\n')
                        with self.assertRaises(AssertionError):
                            qa.wait_marker(serial, marker, Process(), timeout=.1)
            for failure in ('USB_QA_EXIT:32', '[fatal] kernel panic: test'):
                serial.write_text('READY\n' + failure + '\n')
                with self.assertRaises(AssertionError):
                    qa.wait_marker(serial, 'READY', Process(), timeout=.1)

    def test_each_guest_device_must_match_its_own_qmp_port_speed_and_address(self):
        check = getattr(qa, 'validate_xhci_evidence', None)
        self.assertTrue(callable(check), 'runner needs xHCI evidence validation')
        observed = check(USB, QTREE, SERIAL, 'qemu-xhci', '2')
        self.assertEqual(observed['flash1']['guest']['speed'], 'SUPER')
        self.assertEqual(observed['flash1']['guest']['root_port'], 4)
        self.assertEqual(observed['flash1']['qemu']['port'], 4)
        mutations = [
            (USB.replace('Port 4, Speed 5000', 'Port 4, Speed 480'), QTREE, SERIAL),
            (USB, QTREE, SERIAL.replace('root_port=4 psi=4 speed=SUPER', 'root_port=4 psi=3 speed=HIGH')),
            (USB, QTREE, SERIAL.replace('address=2 root_port=4', 'address=9 root_port=4')),
            (USB, QTREE, SERIAL.replace('root_port=4 psi=4 speed=SUPER bps=5000000000 psi_raw=00000000 psi_source=implied vid=46f4', 'root_port=4 psi=4 speed=SUPER bps=5000000000 psi_raw=00000000 psi_source=implied vid=ffff')),
            (USB, QTREE.replace('drive = "stick1"', 'drive = "other"'), SERIAL),
            (USB, QTREE.replace('bus: xhci.0', 'bus: another.0'), SERIAL),
            (USB, QTREE, SERIAL.replace('root_port=10 psi=3 speed=HIGH', 'root_port=10 psi=1 speed=FULL')),
        ]
        for usb, tree, serial in mutations:
            with self.subTest(usb=usb, tree=tree, serial=serial), self.assertRaises(AssertionError):
                check(usb, tree, serial, 'qemu-xhci', '2')

    def test_latest_same_port_event_cannot_be_hidden_by_an_older_matching_address(self):
        old = SERIAL.splitlines()[1]
        changed = old.replace('address=1 root_port=1', 'address=6 root_port=1').replace(
            'psi=4 speed=SUPER bps=5000000000', 'psi=3 speed=HIGH bps=480000000')
        with self.assertRaises(AssertionError):
            qa.validate_xhci_evidence(USB, QTREE, SERIAL + changed + '\n', 'qemu-xhci', '2')
        with self.assertRaises(AssertionError):
            qa.validate_xhci_evidence(USB, QTREE, SERIAL + old.replace('speed=SUPER', 'speed=BOGUS') + '\n', 'qemu-xhci', '2')
        different_address = old.replace('address=1 root_port=1', 'address=6 root_port=1')
        with self.assertRaises(AssertionError):
            qa.validate_xhci_evidence(USB, QTREE, SERIAL + different_address + '\n', 'qemu-xhci', '2')

    def test_replug_ignores_events_before_host_attach_boundary(self):
        self.assertIn('replug_serial', inspect.signature(qa.validate_xhci_evidence).parameters,
                      'replug validation needs the post-attach serial epoch')
        original = SERIAL.splitlines()[1] + '\n'
        usb = USB.replace('ID: flash0', 'ID: flash-new')
        tree = QTREE.replace('id "flash0"', 'id "flash-new"').replace('drive = "stick0"', 'drive = "stick-replug"')
        serial = SERIAL + 'USB_REMOVE_READY\n' + original
        with self.assertRaises(AssertionError):
            qa.validate_xhci_evidence(usb, tree, serial, 'qemu-xhci', '2', replug=True, replug_serial='')
        changed = original.replace('address=1 root_port=1', 'address=6 root_port=1')
        with self.assertRaises(AssertionError):
            qa.validate_xhci_evidence(usb, tree, serial + original + changed, 'qemu-xhci', '2',
                                      replug=True, replug_serial=original + changed)
        evidence = qa.validate_xhci_evidence(usb, tree, serial + original, 'qemu-xhci', '2',
                                             replug=True, replug_serial=original)
        self.assertEqual(evidence['flash-new']['guest']['root_port'], 1)

    def test_reload_requires_all_current_controller_events_after_unload(self):
        self.assertIn('reload', inspect.signature(qa.validate_xhci_evidence).parameters,
                      'reload validation needs a fresh enumeration epoch')
        usb = USB.replace('ID: flash0', 'ID: flash-new')
        tree = QTREE.replace('id "flash0"', 'id "flash-new"').replace('drive = "stick0"', 'drive = "stick-replug"')
        before = SERIAL + 'USB_REMOVE_READY\n' + SERIAL.splitlines()[1] + '\nUSB_MODULE_UNLOADED\n'
        with self.assertRaises(AssertionError):
            qa.validate_xhci_evidence(usb, tree, before, 'qemu-xhci', '2', replug=True, reload=True)
        with self.assertRaises(AssertionError):
            qa.validate_xhci_evidence(usb, tree, before + '\n'.join(SERIAL.splitlines()[:-1]),
                                      'qemu-xhci', '2', replug=True, reload=True)
        evidence = qa.validate_xhci_evidence(usb, tree, before + SERIAL, 'qemu-xhci', '2', replug=True, reload=True)
        self.assertEqual(evidence['mouse']['guest']['root_port'], 11)
        wrong = SERIAL.splitlines()[-1].replace('address=5 root_port=11', 'address=9 root_port=11')
        for serial in (before + SERIAL + wrong + '\n', before + 'USB_MODULE_UNLOADED\n' + SERIAL):
            with self.subTest(serial=serial), self.assertRaises(AssertionError):
                qa.validate_xhci_evidence(usb, tree, serial, 'qemu-xhci', '2', replug=True, reload=True)

    def test_post_reload_waits_for_delayed_guest_and_current_qmp_devices(self):
        wait = getattr(qa, 'wait_xhci_reload', None)
        self.assertTrue(callable(wait), 'runner needs a bounded reload evidence wait')
        usb = USB.replace('ID: flash0', 'ID: flash-new')
        tree = QTREE.replace('id "flash0"', 'id "flash-new"').replace('drive = "stick0"', 'drive = "stick-replug"')
        events = SERIAL.splitlines(keepends=True)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            serial = output / 'serial.log'
            serial.write_text(SERIAL + 'USB_MODULE_UNLOADED\n' + ''.join(events[:2]))
            class Clock:
                now = 0.0
                def monotonic(self): return self.now
                def sleep(self, duration): self.now += duration
            clock = Clock()
            class Process:
                def poll(self): return None
            class QMP:
                def __init__(self): self.usb_samples = []
                def command(self, name, args):
                    self_test = name == 'human-monitor-command'
                    if not self_test: raise AssertionError('unexpected QMP command')
                    if args == {'command-line': 'info usb'}:
                        sample = usb if clock.now >= .2 else usb.replace(usb.splitlines(keepends=True)[-1], '')
                        self.usb_samples.append(sample)
                        return sample
                    if args == {'command-line': 'info qtree'}:
                        if clock.now >= .1 and not hasattr(self, 'appended'):
                            with serial.open('a') as stream: stream.writelines(events[2:])
                            self.appended = True
                        return tree
                    raise AssertionError('unexpected QMP query')
            probe = QMP()
            evidence, final_serial = wait(probe, serial, Process(), output, 'qemu-xhci', '2', timeout=1,
                                          monotonic=clock.monotonic, sleep=clock.sleep)
            self.assertIn('root_port=11', final_serial)
            self.assertEqual(evidence['mouse']['guest']['root_port'], 11)
            self.assertGreaterEqual(clock.now, .2)
            self.assertEqual((output / 'usb-after.txt').read_text(), usb)
            self.assertEqual((output / 'qtree-after.txt').read_text(), tree)

    def test_reload_observes_events_arriving_during_qmp_query_without_extra_delay(self):
        wait = getattr(qa, 'wait_xhci_reload', None)
        self.assertTrue(callable(wait), 'runner needs a bounded reload evidence wait')
        usb = USB.replace('ID: flash0', 'ID: flash-new')
        tree = QTREE.replace('id "flash0"', 'id "flash-new"').replace('drive = "stick0"', 'drive = "stick-replug"')
        events = SERIAL.splitlines(keepends=True)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            serial = output / 'serial.log'
            serial.write_text(SERIAL + 'USB_MODULE_UNLOADED\n' + ''.join(events[:2]))
            class Clock:
                now = 0.0
                def monotonic(self): return self.now
                def sleep(self, duration): self.now += duration
            class Process:
                def poll(self): return None
            class QMP:
                def command(self, name, args):
                    if name != 'human-monitor-command': raise AssertionError(name)
                    if args == {'command-line': 'info usb'}: return usb
                    if args == {'command-line': 'info qtree'}:
                        if not hasattr(self, 'appended'):
                            with serial.open('a') as stream: stream.writelines(events[2:])
                            self.appended = True
                        return tree
                    raise AssertionError(args)
            clock = Clock()
            evidence, _ = wait(QMP(), serial, Process(), output, 'qemu-xhci', '2', timeout=.05,
                               monotonic=clock.monotonic, sleep=clock.sleep)
            self.assertEqual(evidence['mouse']['guest']['root_port'], 11)
            self.assertEqual(clock.now, 0)

    def test_qmp_state_is_resampled_if_guest_changes_during_the_query(self):
        wait = getattr(qa, 'wait_xhci_reload', None)
        self.assertTrue(callable(wait), 'runner needs a bounded reload evidence wait')
        usb = USB.replace('ID: flash0', 'ID: flash-new')
        wrong_usb = usb.replace('Port 3, Speed 480', 'Port 3, Speed 12')
        tree = QTREE.replace('id "flash0"', 'id "flash-new"').replace('drive = "stick0"', 'drive = "stick-replug"')
        events = SERIAL.splitlines(keepends=True)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            serial = output / 'serial.log'
            serial.write_text(SERIAL + 'USB_MODULE_UNLOADED\n' + ''.join(events[:2]))
            class Clock:
                now = 0.0
                def monotonic(self): return self.now
                def sleep(self, duration): self.now += duration
            class Process:
                def poll(self): return None
            class QMP:
                calls = 0
                def command(self, name, args):
                    if name != 'human-monitor-command': raise AssertionError(name)
                    if args == {'command-line': 'info usb'}:
                        self.calls += 1
                        return usb if self.calls == 1 else wrong_usb
                    if args == {'command-line': 'info qtree'}:
                        if self.calls == 1:
                            with serial.open('a') as stream: stream.writelines(events[2:])
                        return tree
                    raise AssertionError(args)
            clock = Clock()
            with self.assertRaisesRegex(AssertionError, 'timed out.*mouse'):
                wait(QMP(), serial, Process(), output, 'qemu-xhci', '2', timeout=.2,
                     monotonic=clock.monotonic, sleep=clock.sleep)
            self.assertEqual((output / 'usb-after.txt').read_text(), wrong_usb)

    def test_post_reload_timeout_preserves_the_last_missing_or_contradictory_evidence(self):
        wait = getattr(qa, 'wait_xhci_reload', None)
        self.assertTrue(callable(wait), 'runner needs a bounded reload evidence wait')
        usb = USB.replace('ID: flash0', 'ID: flash-new')
        tree = QTREE.replace('id "flash0"', 'id "flash-new"').replace('drive = "stick0"', 'drive = "stick-replug"')
        full = SERIAL + 'USB_MODULE_UNLOADED\n' + SERIAL
        wrong = SERIAL.splitlines()[1].replace('address=1 root_port=1', 'address=9 root_port=1')
        cases = [(full.replace(SERIAL.splitlines()[-1] + '\n', '', 2), 'mouse'),
                 (full + wrong + '\n', 'flash-new')]
        for log, missing in cases:
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as directory:
                output = Path(directory)
                serial = output / 'serial.log'
                serial.write_text(log)
                class Clock:
                    now = 0.0
                    def monotonic(self): return self.now
                    def sleep(self, duration): self.now += duration
                class Process:
                    def poll(self): return None
                class QMP:
                    def command(self, name, args):
                        if name != 'human-monitor-command': raise AssertionError('unexpected QMP command')
                        if args == {'command-line': 'info usb'}: return usb
                        if args == {'command-line': 'info qtree'}: return tree
                        raise AssertionError('unexpected QMP query')
                clock = Clock()
                with self.assertRaisesRegex(AssertionError, 'timed out.*' + missing):
                    wait(QMP(), serial, Process(), output, 'qemu-xhci', '2', timeout=.3,
                         monotonic=clock.monotonic, sleep=clock.sleep)
                self.assertGreaterEqual(clock.now, .3)
                self.assertEqual((output / 'usb-after.txt').read_text(), usb)
                self.assertEqual((output / 'qtree-after.txt').read_text(), tree)

    def test_post_reload_wait_stops_on_panic_or_qemu_exit(self):
        wait = getattr(qa, 'wait_xhci_reload', None)
        self.assertTrue(callable(wait), 'runner needs a bounded reload evidence wait')
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            serial = output / 'serial.log'
            serial.write_text('Kernel panic: failure\n')
            class Process:
                def __init__(self, result): self.result = result
                def poll(self): return self.result
            class QMP:
                def command(self, name, args): raise AssertionError('query after fatal failure')
            with self.assertRaisesRegex(AssertionError, 'panic'):
                wait(QMP(), serial, Process(None), output, 'qemu-xhci', '2', timeout=.1)
            serial.write_text('USB_QA_EXIT:abcdef1234:42\n')
            with self.assertRaisesRegex(AssertionError, 'failure'):
                wait(QMP(), serial, Process(None), output, 'qemu-xhci', '2', timeout=.1)
            serial.write_text('USB_MODULE_UNLOADED\n')
            with self.assertRaisesRegex(RuntimeError, 'exited'):
                wait(QMP(), serial, Process(9), output, 'qemu-xhci', '2', timeout=.1)

    def test_host_attach_boundary_excludes_events_before_qmp_resume(self):
        attach = getattr(qa, 'attach_usb_device', None)
        self.assertTrue(callable(attach), 'runner needs an atomic stopped-guest attach boundary')
        with tempfile.TemporaryDirectory() as directory:
            serial = Path(directory) / 'serial.log'
            serial.write_bytes(b'old event\n')
            class ControlledQMP:
                def __init__(self): self.paused = False
                def command(self, name, args=None):
                    if name == 'stop':
                        self.paused = True
                    elif name == 'device_add':
                        if not self.paused or args != {'driver': 'usb-storage', 'bus': 'xhci.0', 'drive': 'stick-replug', 'id': 'flash-new', 'port': '1'}:
                            raise AssertionError('device added outside the stopped VM')
                        with serial.open('ab') as log: log.write(b'buffered old event\n')
                    elif name == 'cont':
                        if not self.paused: raise AssertionError('VM not paused')
                        self.paused = False
                        with serial.open('ab') as log: log.write(b'immediate fresh event\n')
                    else: raise AssertionError(name)
            probe = ControlledQMP()
            offset = attach(probe, serial, 'xhci.0')
            self.assertFalse(probe.paused)
            self.assertEqual(serial.read_bytes()[offset:], b'immediate fresh event\n')

    def test_full_speed_hid_endpoints_are_distinct_from_storage(self):
        check = getattr(qa, 'validate_xhci_evidence', None)
        self.assertTrue(callable(check), 'runner needs xHCI evidence validation')
        usb = USB.replace('Speed 480 Mb/s', 'Speed 12 Mb/s')
        serial = SERIAL.replace('psi=3 speed=HIGH bps=480000000', 'psi=1 speed=FULL bps=12000000')
        result = check(usb, QTREE, serial, 'qemu-xhci', '1')
        self.assertEqual(result['mouse']['guest']['speed'], 'FULL')
        self.assertEqual(result['flash0']['guest']['speed'], 'SUPER')

    def test_generated_shells_parse_and_exit_is_nonce_scoped(self):
        self.assertGreaterEqual(len(inspect.signature(qa.guest_script).parameters), 6, 'guest script needs a module choice')
        for module in ('ehci', 'xhci'):
            for modular in (False, True):
                script = qa.guest_script('http://10.0.2.2:1234/abcdef1234', 'a' * 64,
                                         '5.1.2-release', modular, 'abcdef1234', module)
                subprocess.run(['sh', '-n'], input=script, check=True)
                self.assertIn(b'USB_QA_EXIT:abcdef1234:', script)
                if modular:
                    self.assertIn(('rmmod ' + module).encode(), script)

    def test_success_requires_exact_current_nonce_identity_and_fresh_replug(self):
        check = getattr(qa, 'validate_guest_output', None)
        self.assertTrue(callable(check), 'runner needs guest result validation')
        nonce, release, sha = 'abcdef1234', '5.1.2-release', 'a' * 64
        serial = (f'[boot] firmware=legacy\nUSB_QA_START:{nonce}\n'
                  f'USB_KERNEL_RELEASE:{release}\nUSB_KEYBOARD_OK\nUSB_PROBE_SHA256:{sha}\n'
                  'USB_FIXTURE:stick:/dev/sda1\nUSB_MBR_RW_OK\n'
                  'USB_RAW_WRITE_EROFS\nUSB_RO_MOUNT_REMOUNT_OK\n'
                  'USB_FIXTURE:readonly:/dev/sdb1\nUSB_GPT_RO_OK\n'
                  'USB_FIXTURE:whole:/dev/sdc\nUSB_WHOLE_OK\nUSB_MOUSE_READY\nUSB_MOUSE_OK\n'
                  'USB_REMOVE_READY\nUSB_OLD_FD_READY\nUSB_OLD_FD_ENODEV\nUSB_REPLUG_DEVICE:/dev/sdd1\n'
                  f'USB_REPLUG_OK\nUSB_QA_OK:{nonce}\nUSB_QA_EXIT:{nonce}:0\n')
        result = check(serial, nonce, release, sha, False, 'bios')
        self.assertEqual(result['fixtures']['stick'], '/dev/sda1')
        self.assertEqual(result['replug'], '/dev/sdd1')
        for wrong in (serial.replace('USB_QA_EXIT:abcdef1234:0', 'USB_QA_EXIT:other:0'),
                      serial.replace(sha, 'b' * 64),
                      serial.replace('/dev/sdd1', '/dev/sda1'),
                      serial.replace('USB_RAW_WRITE_EROFS', 'USB_RAW_WRITE_FAILED'),
                      serial.replace('firmware=legacy', 'firmware=UEFI'),
                      serial.replace('USB_MOUSE_OK\n', '').replace('USB_QA_START:abcdef1234', 'USB_MOUSE_OK\nUSB_QA_START:abcdef1234')):
            with self.subTest(wrong=wrong), self.assertRaises(AssertionError):
                check(wrong, nonce, release, sha, False, 'bios')

    def test_lifecycle_rejects_missing_duplicated_and_reordered_events(self):
        nonce, release, sha = 'abcdef1234', '5.1.2-release', 'a' * 64
        serial = (f'[boot] firmware=legacy\nUSB_QA_START:{nonce}\nUSB_KERNEL_RELEASE:{release}\n'
                  f'USB_KEYBOARD_OK\nUSB_PROBE_SHA256:{sha}\nUSB_FIXTURE:stick:/dev/sda1\n'
                  'USB_MBR_RW_OK\nUSB_RAW_WRITE_EROFS\nUSB_RO_MOUNT_REMOUNT_OK\n'
                  'USB_FIXTURE:readonly:/dev/sdb1\nUSB_GPT_RO_OK\nUSB_FIXTURE:whole:/dev/sdc\n'
                  'USB_WHOLE_OK\nUSB_MOUSE_READY\nUSB_MOUSE_OK\nUSB_REMOVE_READY\n'
                  'USB_OLD_FD_READY\nUSB_OLD_FD_ENODEV\nUSB_REPLUG_DEVICE:/dev/sdd1\nUSB_REPLUG_OK\n'
                  'USB_MODULE_BUSY_OK\nUSB_MODULE_UNLOADED\nUSB_MODULE_DEVICE:/dev/sdg1\nUSB_MODULE_RELOADED\n'
                  f'USB_QA_OK:{nonce}\nUSB_QA_EXIT:{nonce}:0\n')
        qa.validate_guest_output(serial, nonce, release, sha, True, 'bios')
        bad = [serial.replace('USB_REMOVE_READY\n', ''),
               serial.replace('USB_REMOVE_READY\n', 'USB_REMOVE_READY\nUSB_REMOVE_READY\n'),
               serial.replace('USB_OLD_FD_ENODEV\nUSB_REPLUG_DEVICE:/dev/sdd1\nUSB_REPLUG_OK',
                              'USB_REPLUG_DEVICE:/dev/sdd1\nUSB_REPLUG_OK\nUSB_OLD_FD_ENODEV'),
               serial.replace('USB_MODULE_BUSY_OK\nUSB_MODULE_UNLOADED\nUSB_MODULE_DEVICE:/dev/sdg1\nUSB_MODULE_RELOADED',
                              'USB_MODULE_RELOADED\nUSB_MODULE_UNLOADED\nUSB_MODULE_DEVICE:/dev/sdg1\nUSB_MODULE_BUSY_OK'),
               serial.replace('USB_MODULE_DEVICE:/dev/sdg1\n', ''),
               serial.replace('USB_REPLUG_DEVICE:/dev/sdd1\n', 'USB_REPLUG_DEVICE:/dev/sdd1\nUSB_REPLUG_DEVICE:/dev/sde1\n')]
        for wrong in bad:
            with self.subTest(wrong=wrong), self.assertRaises(AssertionError):
                qa.validate_guest_output(wrong, nonce, release, sha, True, 'bios')

    def test_replug_gate_holds_guest_until_host_releases_evidence_snapshot(self):
        with qa.payload_server(b'probe bytes', '5.1.2-release', False, 'abcdef1234') as payload:
            self.assertEqual(len(payload), 3, 'server needs a host-controlled replug gate')
            base, _, release = payload
            port = int(base.split(':')[2].split('/')[0])
            done = threading.Event()
            response = []
            def fetch():
                connection = http.client.HTTPConnection('127.0.0.1', port, timeout=3)
                connection.request('GET', '/abcdef1234/gate')
                result = connection.getresponse()
                response.append((result.status, result.read()))
                connection.close()
                done.set()
            thread = threading.Thread(target=fetch)
            thread.start()
            try:
                self.assertFalse(done.wait(.15), 'gate released before the host snapshot')
            finally:
                release()
                thread.join(3)
            self.assertFalse(thread.is_alive())
            self.assertEqual(response, [(200, b'continue\n')])

    def test_private_server_only_serves_exact_nonce_urls(self):
        with qa.payload_server(b'probe bytes', '5.1.2-release', False, 'abcdef1234') as (base, script, _release):
            port = int(base.split(':')[2].split('/')[0])
            conn = http.client.HTTPConnection('127.0.0.1', port)
            for path, status, contents in [('/abcdef1234/p', 200, b'probe bytes'),
                                           ('/abcdef1234/u', 200, script),
                                           ('/wrong/p', 404, None),
                                           ('/abcdef1234/p?secret=1', 404, None)]:
                conn.request('GET', path)
                response = conn.getresponse()
                self.assertEqual(response.status, status)
                data = response.read()
                if contents is not None:
                    self.assertEqual(data, contents)
            conn.close()


if __name__ == '__main__':
    unittest.main()
