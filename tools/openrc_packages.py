"""Pinned, signature-verified Alpine APKs. Never synthesize installed records."""
import json
from pathlib import Path, PurePosixPath
import tarfile

ROOT = Path(__file__).resolve().parents[1]


def packages(apk):
    from apk_distribution import download_verified, digest, run
    manifest = json.loads((ROOT / 'configs/openrc-packages.json').read_text())
    # Keep downloaded Alpine packages outside build outputs so all image
    # targets and subsequent invocations share the same verified cache.
    cache = ROOT / 'buildsystem/cache/apk/packages'
    cache.mkdir(parents=True, exist_ok=True)
    archives, paths = [], set()
    for entry in manifest['packages']:
        filename = f"{entry['name']}-{entry['version']}.apk"
        archive = cache / filename
        if not archive.is_file() or digest(archive) != entry['sha256']:
            download_verified((entry.get('url', manifest['repository'] + '/' + filename),), archive, entry['sha256'])
        run([apk, '--keys-dir', ROOT / 'system/rootfs/etc/apk/keys', 'verify', archive])
        with tarfile.open(archive, 'r:gz', ignore_zeros=True) as packed:
            info = packed.extractfile('.PKGINFO').read().decode()
            if f"pkgname = {entry['name']}\n" not in info or f"pkgver = {entry['version']}\n" not in info:
                raise ValueError(f'APK identity mismatch: {filename}')
            for member in packed:
                if member.name.startswith('.') or member.isdir():
                    continue
                path = PurePosixPath(member.name)
                if path.is_absolute() or '..' in path.parts:
                    raise ValueError(f'Unsafe APK member: {member.name}')
                if not (member.isfile() or member.issym() or member.islnk()):
                    raise ValueError(f'Unsupported APK member: {member.name}')
                if str(path) in paths:
                    raise ValueError(f'Conflicting Alpine payload: {member.name}')
                paths.add(str(path))
        archives.append(archive)
    return archives, paths, manifest
