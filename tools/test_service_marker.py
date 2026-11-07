#!/usr/bin/env python3
"""Reject retired service group identities in the OS group database and manifests."""
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path)
    args = parser.parse_args()
    failures = []
    for relative in ('system/rootfs/etc/group', 'system/test-accounts/group'):
        for line in (ROOT / relative).read_text().splitlines():
            fields = line.split(':')
            if len(fields) == 4 and fields[2] in ('60001', '60002'):
                failures.append(f'{relative}: reserved desktop service group remains')
    if args.manifest:
        for entry in json.loads(args.manifest.read_text()).get('entries', []):
            if entry.get('gid') in (60001, 60002):
                failures.append(f'manifest: retired group on {entry["path"]}')
    if failures:
        raise SystemExit('\n'.join(failures))
    print('PASS no retired service group marks (' +
          ('source and manifest' if args.manifest else 'source only') + ')')


if __name__ == '__main__':
    main()
