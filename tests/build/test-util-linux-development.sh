#!/bin/sh
# Offline adapter publication/integrity tests; no downloads or target execution.
set -eu
LC_ALL=C
export LC_ALL
repo=$(CDPATH='' cd -- "$(dirname "$0")/../.." && pwd -P)
mkdir -p "${TMPDIR:-$repo/out/tmp}"
work=$(mktemp -d "${TMPDIR:-$repo/out/tmp}/util-linux-development.XXXXXX")
trap 'rm -rf "$work"' 0
trap 'exit 130' HUP INT TERM
export FIXTURE_WORK="$work"
mkdir -p "$work/source/resources/licenses" "$work/source/system/rootfs/etc/apk/keys" \
    "$work/bin" "$work/cache" "$work/good" "$work/digests"
cp "$repo/resources/licenses/util-linux-README.licensing" "$work/source/resources/licenses/"
printf 'fixture\n' >"$work/lock"
real_mv=$(command -v mv)
export FIXTURE_MV="$real_mv"
cat >"$work/bin/deps" <<'SH'
#!/bin/sh
set -eu
[ "$#" = 6 ] && [ "$1" = --lock ] && [ "$3" = --id ] && [ "$5" = --print ]
case $4 in alpine-*) name=${4#alpine-};; runtime-dev-*) name=${4#runtime-dev-};; *) exit 2;; esac
case $6 in
    url) printf 'https://dl-cdn.alpinelinux.org/alpine/edge/main/x86_64/%s-1.0-r0.apk\n' "$name";;
    sha256) cat "$FIXTURE_WORK/digests/$name";;
    version) printf '1.0-r0\n';;
    *) exit 2;;
esac
SH
cat >"$work/bin/apk" <<'SH'
#!/bin/sh
set -eu
[ "$#" = 4 ] && [ "$1" = --keys-dir ] && [ "$3" = verify ]
printf 'verified\n' >>"$FIXTURE_WORK/verifications"
[ "${FAIL_VERIFY:-0}" != 1 ]
SH
cat >"$work/bin/mv" <<'SH'
#!/bin/sh
set -eu
if [ "$#" = 2 ] && [ "$2" = "${FAIL_DEST:-}" ] && [ -f "$FIXTURE_WORK/fail-once" ]; then
    rm "$FIXTURE_WORK/fail-once"
    exit 9
fi
exec "$FIXTURE_MV" "$@"
SH
chmod +x "$work/bin/"*
python3 - "$work" <<'PY'
import io
from pathlib import Path
import sys
import tarfile

work = Path(sys.argv[1])
packages = {name: {} for name in ('libblkid', 'libuuid', 'libeconf', 'util-linux-dev',
                                 'util-linux-static', 'libeconf-dev', 'libeconf-static')}
for lib, package in (('blkid', 'util-linux'), ('uuid', 'util-linux'), ('econf', 'libeconf')):
    packages['lib' + lib]['usr/lib/lib' + lib + '.so.1'] = b'fixture library\n'
    packages[package + '-dev']['usr/lib/lib' + lib + '.so'] = ('link', 'lib' + lib + '.so.1')
    packages[package + '-dev']['usr/lib/pkgconfig/' + ('libeconf' if lib == 'econf' else lib) + '.pc'] = (
        'prefix=/usr\nexec_prefix=/usr\nlibdir=/usr/lib\nincludedir=/usr/include\n'
        f'Name: {lib}\nDescription: fixture\nVersion: 1.0\n'
        f'Libs: -L${{libdir}} -l{lib}\nCflags: -I${{includedir}}\n').encode()
    packages[package + '-static']['usr/lib/lib' + lib + '.a'] = b'fixture archive\n'
packages['util-linux-dev'].update({
    'usr/include/blkid/blkid.h': b'fixture header\n',
    'usr/include/blkid/nested/extra.h': b'fixture nested header\n',
    'usr/include/uuid/uuid.h': b'fixture header\n',
    'usr/include/linux/types.h': b'unrelated header\n',
    'usr/lib/libfdisk.a': b'unrelated static archive\n'})
packages['libeconf-dev']['usr/include/libeconf.h'] = b'fixture header\n'
for name, payload in packages.items():
    payload['.PKGINFO'] = f'pkgname = {name}\npkgver = 1.0-r0\narch = x86_64\n'.encode()
    with tarfile.open(work / 'good' / f'{name}-1.0-r0.apk', 'w:gz') as archive:
        for path, data in payload.items():
            member = tarfile.TarInfo(path)
            if isinstance(data, tuple):
                member.type, member.linkname = tarfile.SYMTYPE, data[1]
                archive.addfile(member)
            else:
                member.size = len(data)
                archive.addfile(member, io.BytesIO(data))
PY
reset_cache() {
    cp "$work/good/"*.apk "$work/cache/"
    for archive in "$work/cache/"*.apk; do
        name=${archive##*/}; name=${name%-1.0-r0.apk}
        sum=$(sha256sum "$archive")
        printf '%s\n' "${sum%% *}" >"$work/digests/$name"
    done
    : >"$work/verifications"
}
output=$work/out\ with\ spaces/root
invoke() {
    PATH="$work/bin:$PATH" sh "$repo/tools/build/util-linux-development.sh" \
        "$work/source" "$work/bin/deps" "$work/lock" "$work/cache" "$work/bin/apk" "$output"
}
snapshot() {
    (cd "$output" && find . -printf '%y %p %l\n' && find . -type f -exec sha256sum {} \;) | sort
}
expect_failure() {
    if invoke >"$work/failure.log" 2>&1; then echo 'accepted invalid input/publication' >&2; exit 1; fi
    snapshot >"$work/after"
    cmp "$work/before" "$work/after"
    cmp "$work/before.files" "$output.files"
    test -z "$(find "$(dirname "$output")" -maxdepth 1 -name '.root.new.*' -print)"
}
reset_cache
invoke
test "$(wc -l <"$work/verifications" | tr -d ' ')" = 7
test -s "$output/usr/include/blkid/nested/extra.h"
test ! -e "$output/usr/include/linux/types.h"
test ! -e "$output/usr/lib/libfdisk.a"
test ! -e "$output/bin"
(cd "$output" && find . ! -type d -print | sed 's@^./@@' | sort) >"$work/expected.files"
cmp "$work/expected.files" "$output.files"
cp -a "$output" "$work/relocated"
for module in blkid uuid libeconf; do
    libdir=$(PKG_CONFIG_PATH='' PKG_CONFIG_LIBDIR="$work/relocated/usr/lib/pkgconfig" pkg-config --variable=libdir "$module")
    test "$(CDPATH='' cd -- "$libdir" && pwd -P)" = "$work/relocated/usr/lib"
done
snapshot >"$work/before"
cp "$output.files" "$work/before.files"
: >"$work/verifications"
printf 'corrupt\n' >>"$work/cache/libblkid-1.0-r0.apk"
expect_failure
test ! -s "$work/verifications"
reset_cache
FAIL_VERIFY=1 expect_failure
reset_cache
rm "$work/cache/libeconf-static-1.0-r0.apk"
expect_failure
reset_cache
for destination in "$output" "$output.files"; do
    touch "$work/fail-once"
    FAIL_DEST=$destination expect_failure
done
# A missing non-primary input must invalidate the inventory and be restored.
touch -t 200001010000 "$work/presence"
cp -p "$work/presence" "$work/old-presence"
rm "$output/usr/include/blkid/nested/extra.h"
sh "$repo/tools/build/upstream-verify.sh" "$output" "$output.files" "$work/presence"
test "$work/presence" -nt "$work/old-presence"
invoke
test -s "$output/usr/include/blkid/nested/extra.h"
snapshot >"$work/after"
cmp "$work/before" "$work/after"
for rejected in "$work/source/resources/licenses" "$work/cache" "$output/.."; do
    if PATH="$work/bin:$PATH" sh "$repo/tools/build/util-linux-development.sh" \
        "$work/source" "$work/bin/deps" "$work/lock" "$work/cache" "$work/bin/apk" "$rejected" \
        >"$work/failure.log" 2>&1; then echo "accepted input overlap: $rejected" >&2; exit 1; fi
done
printf 'util-linux development: isolated inputs, relocation, signatures, rollback and inventory recovery passed\n'
