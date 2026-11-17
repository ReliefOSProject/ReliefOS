#!/usr/bin/env python3
"""Test production APK metadata and the online updater's package ownership transfer."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apk', type=Path, required=True)
    args = parser.parse_args()
    apk = args.apk.resolve()
    temporary = ROOT / 'out/tmp'
    temporary.mkdir(parents=True, exist_ok=True)

    def run(argv, **kwargs):
        result = subprocess.run(list(map(str, argv)), capture_output=True, text=True, **kwargs)
        if result.returncode:
            raise RuntimeError((argv, result.returncode, result.stdout, result.stderr))
        return result.stdout

    with tempfile.TemporaryDirectory(prefix='system-update-packaging.', dir=temporary) as name:
        work = Path(name)
        own, layout = work / 'own', work / 'layout'
        run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT),
             ROOT / 'tools/host/apk/reliefos-apk-own.c', ROOT / 'tools/host/manifest/json.c',
             ROOT / 'tools/host/common/buffer.c', ROOT / 'tools/host/common/io.c', '-o', own])
        run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT),
             ROOT / 'tools/host/manifest/reliefos-layout.c', '-o', layout])

        def put(base, path, text, mode=0o644):
            target = base / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text)
            target.chmod(mode)
            return target

        raw = work / 'raw'
        put(raw, 'etc/reliefos/desktop-backend', 'console\n')
        put(raw, 'etc/reliefos/rpr.conf', 'RPR_BASE_URL=https://new.invalid/rpr\n')
        put(raw, 'usr/sbin/reliefos-update', 'native updater fixture\n', 0o755)
        put(raw, 'usr/libexec/reliefos/kernel-bundle', 'kernel bundle fixture\n')
        put(raw, 'usr/lib/reliefos/apps/shell/shell.elf', 'new applications\n')
        # Text fixtures deliberately have no ELF dependency or executable bit.
        put(raw, 'bin/busybox', 'busybox fixture\n')
        put(raw, 'usr/bin/cat', 'coreutils fixture\n')
        for prefix in ('reliefos', 'leonos'):
            for command in ('kernel-update', 'check-update', 'rpr-apkcheck', 'rpr-ping'):
                (raw / 'usr/sbin' / f'{prefix}-{command}').symlink_to('reliefos-update')
        upstream = work / 'upstream'
        (upstream / 'packages').mkdir(parents=True)
        deps = put(work, 'deps', '#!/bin/sh\n[ "$4" = alpine-ca-certificates-bundle ] || exit 1\nprintf "1-r0\\n"\n', 0o755)
        key = work / 'key.pem'
        run(['openssl', 'genpkey', '-algorithm', 'RSA', '-pkeyopt', 'rsa_keygen_bits:2048', '-out', key])
        key.chmod(0o600)
        keys = work / 'keys'
        keys.mkdir()
        run(['openssl', 'pkey', '-in', key, '-pubout', '-out', keys / 'test.rsa.pub'])
        # Keep the producer's real mkpkg/mkndx invocations. Skip only its initial
        # staging install, then test actual upgrades in independent real roots.
        wrapper = put(work, 'apk-wrapper', '''#!/usr/bin/env python3
import os, sys
args = sys.argv[1:]
if args and args[0] == '--usermode': args = args[1:]
if 'add' in args: sys.exit(0)
os.execv(os.environ['PACKAGING_REAL_APK'], [os.environ['PACKAGING_REAL_APK'], *args])
''', 0o755)
        stage = work / 'stage'
        try:
            run(['sh', ROOT / 'tools/build/apk-stage.sh', ROOT, raw, work / 'managed', stage,
                 wrapper, upstream, deps, ROOT / 'configs/dependencies.lock.json',
                 ROOT / 'configs/apk-ownership.json', own, key, '9.1-r0'],
                env={**os.environ, 'APK_LAYOUT_TOOL': str(layout), 'PACKAGING_REAL_APK': str(apk),
                     'SOURCE_DATE_EPOCH': '1'}, umask=0o077)
        except RuntimeError:
            for log in sorted((stage / 'logs').glob('*.log')):
                print(log.name + ':\n' + log.read_text(), flush=True)
            raise
        repository = stage / 'repository'
        for relative in ('etc', 'etc/apk', 'etc/apk/keys'):
            assert ((work / 'managed' / relative).stat().st_mode & 0o777) == 0o755, relative
        version = json.loads((stage / 'manifest.json').read_text())['version']
        assert version.startswith('9.1.')
        update_archive = repository / f'reliefos-system-update-{version}.apk'
        assert update_archive.is_file()

        def mkpkg(base, output, package, version, *extra):
            run(['unshare', '-Ur', apk, 'mkpkg', '--files', base, '--output', output,
                 '--info', 'name:' + package, '--info', 'version:' + version,
                 '--info', 'arch:x86_64', '--info', 'description:ownership transfer fixture',
                 '--info', 'license:MIT', '--sign-key', key, *extra])

        # Test old ReliefOS and pre-rename installs. No overwrite or force flag
        # is accepted: the real producer's dependency and replaces metadata
        # must move the old regular files to the native package's symlinks.
        for old_name in ('reliefos-apps', 'leonos-apps'):
            old_payload = work / (old_name + '-payload')
            old_config = 'RPR_BASE_URL=https://administrator.invalid/rpr\n'
            put(old_payload, 'etc/reliefos/rpr.conf', 'RPR_BASE_URL=https://old.invalid/rpr\n')
            for prefix in ('reliefos', 'leonos'):
                for command in ('kernel-update', 'check-update', 'rpr-apkcheck', 'rpr-ping'):
                    put(old_payload, f'usr/sbin/{prefix}-{command}', '#!/bin/sh\nexit 0\n', 0o755)
            old_archive = work / (old_name + '-8.1-r0.apk')
            mkpkg(old_payload, old_archive, old_name, '8.1-r0')
            target = work / (old_name + '-root')
            target.mkdir()
            common = [apk, '--root', target, '--arch', 'x86_64', '--keys-dir', keys,
                      '--repositories-file', '/dev/null', '--no-scripts']
            initdb = [*common, '--initdb']
            if os.geteuid() != 0:
                initdb.append('--usermode')
            run([*initdb, 'add', old_archive])
            (target / 'etc/reliefos/rpr.conf').write_text(old_config)
            # Local archive installs pin their version; image assembly records
            # ordinary package names in world so repository upgrades can move.
            (target / 'etc/apk/world').write_text(old_name + '\n')
            if old_name == 'reliefos-apps':
                run([*common, '--repository', repository / 'packages.adb', 'upgrade'])
            else:
                # Existing old distributions explicitly migrate their world
                # request; APK replaces governs only path ownership transfer.
                run([*common, '--repository', repository / 'packages.adb', 'add', 'reliefos-apps'])
            assert (target / 'usr/sbin/reliefos-update').read_text() == 'native updater fixture\n'
            assert (target / 'etc/reliefos/rpr.conf').read_text() == old_config
            for prefix in ('reliefos', 'leonos'):
                for command in ('kernel-update', 'check-update', 'rpr-apkcheck', 'rpr-ping'):
                    alias = target / 'usr/sbin' / f'{prefix}-{command}'
                    assert alias.is_symlink() and os.readlink(alias) == 'reliefos-update'
                    owner = run([*common[:-1], 'info', '--who-owns', '/usr/sbin/' + alias.name])
                    assert 'reliefos-system-update-' in owner, owner
            # Ordinary apps now require the updater; its real dependency chain
            # must solve successfully without adding the new package to world.
            world = (target / 'etc/apk/world').read_text()
            assert 'reliefos-apps' in world and 'reliefos-system-update' not in world
            assert (target / 'usr/libexec/reliefos/kernel-bundle').is_file()
            assert (target / 'sbin/apk').is_file()
            assert (target / 'usr/bin/cat').is_file()
            assert (target / 'bin/busybox').is_file()
        print('System update packaging: production signed metadata, old owners, regular-file to symlink transfer, config preservation and dependency chain passed')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
