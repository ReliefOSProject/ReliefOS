#!/usr/bin/env python3
"""Check rejection of invalid VM automation inputs before touching a guest.

Real widget, password, swap and firmware behavior is covered by the ncurses
PTY suite in test_installer_swap_tty.py.
"""
import argparse
import inspect
from pathlib import Path
from test_installer_accounts_qemu import install_gui, install_tty


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.parse_args()
    assert inspect.signature(install_gui).parameters['swap_mib'].default is None
    assert inspect.signature(install_tty).parameters['swap_mib'].default is None
    for invalid in (0, -1, 16777217, '512'):
        try:
            install_tty(None, None, None, swap_mib=invalid, select_entry=False)
        except ValueError:
            pass
        else:
            raise AssertionError('invalid requested swap size accepted')
    for invalid in ({'unknown': 0}, {'root': -1}, {'esp': '1'}):
        try:
            install_tty(None, None, None, partition_roles=invalid, select_entry=False)
        except ValueError:
            pass
        else:
            raise AssertionError('invalid partition selection accepted')
    print('PASS GUI/TUI helper defaults and invalid swap/partition inputs')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
