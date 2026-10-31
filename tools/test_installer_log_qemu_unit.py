#!/usr/bin/env python3
"""Exercise live installer-log reads with a finite serial transport budget."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_installer_accounts_qemu import installer_finish_log


class SerialProbe:
    """Model only QMP typing and a slow serial link, using the real log file."""
    def __init__(self, output, log, *, status=0, stale=False):
        self.output, self.log = output, log
        self.serial = output / 'serial.log'
        self.serial.write_text('earlier boot output\n')
        self.status, self.stale, self.command = status, stale, None

    def text(self, command):
        self.command = command

    def key(self, key):
        if key != 'ret':
            return
        if self.command.startswith('tail -c '):
            count = self.command.split()[2]
            data = subprocess.check_output(['tail', '-c', count, self.log]) if not self.status else b''
        elif self.command.startswith('cat '):
            data = self.log.read_bytes() if not self.status else b''
        else:
            raise AssertionError('Unexpected guest log reader')
        marker = re.search(r'INSTALLER_LOG_READ_DONE(?:_[0-9a-f]+)?', self.command).group()
        if self.stale:
            marker = 'INSTALLER_LOG_READ_DONE'
        reply = data[:32768]
        if len(data) <= 32768:
            reply += ('\n' + marker + ':' + str(self.status) + '\n').encode()
        with self.serial.open('ab') as stream:
            stream.write(reply)


class InstallerLogTests(unittest.TestCase):
    def read(self, probe):
        class Running:
            def poll(self):
                return None
        with patch('test_installer_accounts_qemu.time.monotonic', side_effect=(0, 0, 61)), \
             patch('test_installer_accounts_qemu.time.sleep'):
            return installer_finish_log(probe, probe.serial, Running())

    def test_large_log_delivers_its_completion_before_transport_deadline(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            log = out / 'session.log'
            log.write_bytes(b'copy progress\n' * 500000 +
                            b'[installer.elf] installation completed successfully\n')
            probe = SerialProbe(out, log)
            result = self.read(probe)
            self.assertIn('[installer.elf] installation completed successfully', result)
            self.assertLess(len(result.encode()), 32768)

    def test_log_reader_failure_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            log = out / 'session.log'
            log.write_text('irrelevant\n')
            with self.assertRaisesRegex(AssertionError, 'log read failed'):
                self.read(SerialProbe(out, log, status=1))

    def test_unrelated_completion_token_cannot_finish_the_read(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            log = out / 'session.log'
            log.write_text('[installer.elf] installation completed successfully\n')
            with self.assertRaisesRegex(AssertionError, 'did not return'):
                self.read(SerialProbe(out, log, stale=True))


if __name__ == '__main__':
    unittest.main(verbosity=2)
