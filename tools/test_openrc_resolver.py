#!/usr/bin/env python3
"""Run the real resolver publisher and numeric-address helper in a private root."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
OUT = Path(os.environ.get('RELIEFOS_OUT', ROOT / 'out/x86_64/release'))
class Resolver(unittest.TestCase):
    def test_policy_and_rejected_input(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for folder in ('bin', 'etc/udhcpc', 'run/reliefos', 'dev',
                           'usr/lib/reliefos/apps/rdnssd'):
                (root / folder).mkdir(parents=True)
            def libraries(binary):
                result = subprocess.run(['ldd', str(binary)], capture_output=True, text=True)
                for name in re.findall(r'/[^\s()]+', result.stdout):
                    source = Path(name)
                    destination = root / source.relative_to('/')
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source, destination)
            # BusyBox no longer provides every coreutils applet. Exercise the
            # POSIX utility boundary, without assuming an obsolete applet set.
            for name in ('sh', 'cat', 'rm', 'mktemp', 'chmod', 'mv'):
                source = Path(shutil.which(name))
                shutil.copy2(source, root / 'bin' / name)
                libraries(source)
            helper = root / 'usr/lib/reliefos/apps/rdnssd/rdnssd.elf'
            subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=c11', '-D_GNU_SOURCE',
                            '-O2', str(ROOT / 'userland/apps/rdnssd/main.c'),
                            str(ROOT / 'userland/apps/rdnssd/rdnss.c'), '-o', str(helper)], check=True)
            libraries(helper)
            (root / 'dev/null').touch()
            shutil.copy2(ROOT / 'system/rootfs/usr/lib/reliefos/publish-resolver', root / 'publish')
            (root / 'run/reliefos/dhcp-dns').write_text('10.0.2.3\n')
            def publish(policy):
                (root / 'etc/udhcpc/leonos-dns').write_text(policy)
                return subprocess.run(['unshare', '-Ur', 'chroot', root, '/bin/sh', '/publish'],
                                      env={**os.environ, 'PATH': '/bin:/usr/bin',
                                           'LANG': 'C', 'LC_ALL': 'C'}).returncode
            for policy, expected in [('0 0.0.0.0\n','1.1.1.1'), ('1 0.0.0.0\n','10.0.2.3'), ('2 8.8.8.8\n','8.8.8.8')]:
                self.assertEqual(publish(policy), 0)
                self.assertIn('nameserver ' + expected + '\n', (root / 'etc/resolv.conf').read_text())
            ra = root / 'run/reliefos/ra-dns'
            ra.write_text('2001:db8::53\nfe80::53%2\n2001:db8::53\n')
            self.assertEqual(publish('1 0.0.0.0\n'), 0)
            text = (root / 'etc/resolv.conf').read_text()
            self.assertEqual([line for line in text.splitlines() if line.startswith('nameserver ')],
                             ['nameserver 2001:db8::53', 'nameserver fe80::53%2', 'nameserver 10.0.2.3'])
            before = (root / 'etc/resolv.conf').read_bytes()
            ra.write_text('2001:db8::53\n::\n')
            self.assertNotEqual(publish('1 0.0.0.0\n'), 0)
            self.assertEqual(before, (root / 'etc/resolv.conf').read_bytes())
            ra.unlink()
            before = (root / 'etc/resolv.conf').read_bytes()
            self.assertNotEqual(publish('2 $(touch_/owned)\n'), 0)
            self.assertEqual(before, (root / 'etc/resolv.conf').read_bytes())
            self.assertFalse((root / 'owned').exists())
            for address in ('999.1.2.3', '1.2.3', '1..2.3', '1.2.3.4.5', '.1.2.3', '1.2.3.4.'):
                with self.subTest(address=address):
                    self.assertNotEqual(publish('2 ' + address + '\n'), 0)
                    self.assertEqual(before, (root / 'etc/resolv.conf').read_bytes())
if __name__ == '__main__': unittest.main()
