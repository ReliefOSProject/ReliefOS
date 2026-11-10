#!/bin/sh
# Actual signed APK transaction on a pre-migration managed root.
set -eu
[ "$#" = 2 ] || { echo 'usage: test-runtime-apk-upgrade.sh OUTPUT OLD_MANAGED_ROOT' >&2; exit 2; }
out=$(CDPATH= cd -- "$1" && pwd)
old=$(CDPATH= cd -- "$2" && pwd)
work=$(mktemp -d "$out/runtime-apk-upgrade.XXXXXX")
cleanup() {
    if [ "${KEEP_UPGRADE_WORK:-0}" = 1 ]; then printf 'upgrade work: %s\n' "$work"
    else rm -rf "$work"
    fi
}
trap cleanup EXIT HUP INT TERM
cp -a --reflink=auto "$out/rootfs/managed" "$work/runner"
cp -a --reflink=auto "$old" "$work/runner/target"
cp -a --reflink=auto "${APK_UPGRADE_REPOSITORY:-$out/packages/apk/repository}" "$work/runner/newrepo"
printf 'migration-admin-config=yes\n' > "$work/runner/target/etc/reliefos/apk-upgrade-fixture.conf"
# Exercise legacy aliases/tags and old local version pins without making them
# constraints on the unrelated upstream version scheme. An existing official
# pin remains authoritative; other missing requests inherit the retired tag.
if [ "${APK_UPGRADE_TEST_TAGGED_WORLD:-0}" = 1 ]; then
    python3 - "$work/runner/target" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
versions = {}
for block in (root / 'lib/apk/db/installed').read_text().split('\n\n'):
    fields = dict(line.split(':', 1) for line in block.splitlines() if line[:2] in ('P:', 'V:'))
    if 'P' in fields:
        versions[fields['P']] = fields['V']
world = root / 'etc/apk/world'
requests = []
for request in world.read_text().splitlines():
    name = request.split('=', 1)[0]
    if name in ('sl', 'sqlite', 'sqlite-libs'):
        continue
    if name == 'reliefos-sl':
        request = 'leonos-sl@migration=' + versions[name]
    elif name in ('reliefos-ncurses', 'reliefos-sqlite'):
        request = name + '@migration=' + versions[name]
    requests.append(request)
world.write_text('\n'.join(requests) + '\n')
PY
fi
cp "$work/runner/target/etc/apk/world" "$work/world.before"
cp "$work/runner/target/lib/apk/db/installed" "$work/database.before"
python3 - "$work/world.before" "$work/world.expected" <<'PY'
from pathlib import Path
import re, sys
requests = Path(sys.argv[1]).read_text().splitlines()
replacements = {'reliefos-sl': ('sl',), 'reliefos-ncurses': ('ncurses', 'ncurses-terminfo'),
                'reliefos-sqlite': ('sqlite', 'sqlite-libs')}
requests = [re.sub('^leonos-', 'reliefos-', request) for request in requests]
existing = {re.split(r'[@<>=~]', request, 1)[0] for request in requests}
expected = set()
for request in requests:
    name = re.split(r'[@<>=~]', request, 1)[0]
    if name not in replacements:
        expected.add(request)
        continue
    tag = re.search(r'@([a-zA-Z0-9_.+-]+)', request)
    expected.update(package + ('@' + tag[1] if tag else '')
                    for package in replacements[name] if package not in existing)
Path(sys.argv[2]).write_text('\n'.join(sorted(expected)) + '\n')
PY
run_update() {
unshare -mp --fork sh -eu -c '
    mount --make-rprivate /
    mount --bind "$1" "$1"
    mkdir -p "$1/proc" "$1/dev"
    mount -t proc proc "$1/proc"
    for device in null zero random urandom; do
        touch "$1/dev/$device"
        mount --bind "/dev/$device" "$1/dev/$device"
    done
    cd "$1"
    mkdir -p .oldroot
    pivot_root . .oldroot
    exec /usr/lib/reliefos/reliefos-apk-update "$2" /target "$3"
' sh "$work/runner" "$1" "${2:-/newrepo}"
}
# A signed index alone is insufficient: all newly selected command archives
# must be fetched and verified before changing the installed world/database.
cp -al "$work/runner/newrepo" "$work/runner/incomplete-repo"
rm "$work/runner/incomplete-repo/"ncurses-[0-9]*.apk
if run_update --check /incomplete-repo >"$work/missing-archive.log" 2>&1; then
    echo 'updater accepted a missing replacement command archive' >&2
    exit 1
fi
cmp "$work/world.before" "$work/runner/target/etc/apk/world"
cmp "$work/database.before" "$work/runner/target/lib/apk/db/installed"
run_update --check
root=$work/runner/target
cmp "$work/world.before" "$root/etc/apk/world"
cmp "$work/database.before" "$root/lib/apk/db/installed"
run_update --apply
LC_ALL=C sort "$root/etc/apk/world" > "$work/world.after"
cmp "$work/world.expected" "$work/world.after"
printf 'migration-admin-config=yes\n' | cmp - "$root/etc/reliefos/apk-upgrade-fixture.conf"
apk=$out/upstream/apk/apk.static
for spec in 'sl usr/bin/sl' 'ncurses usr/bin/tput' 'sqlite-libs usr/lib/libsqlite3.so.0' \
    'mbedtls3 usr/lib/libmbedtls.so.21' 'zlib usr/lib/libz.so.1' 'libpng usr/lib/libpng16.so.16'; do
    set -- $spec
    unshare -Ur "$apk" --root "$root" info --who-owns "/$2" | grep -F "$1-"
done
for component in sl ncurses sqlite; do
    for prefix in reliefos leonos; do
        package=$prefix-$component
        if unshare -Ur "$apk" --root "$root" info --exists "$package"; then
            printf 'retired package remains installed: %s\n' "$package" >&2
            exit 1
        fi
        for archive in "$root/usr/share/reliefos/apk/repository/$package-"[0-9]*.apk; do
            test ! -e "$archive" && test ! -L "$archive"
        done
    done
done
test ! -e "$root/usr/lib/sqlite.so.3"
test ! -e "$root/usr/lib/reliefos/apps/sl"
test ! -e "$root/usr/share/doc/reliefos/apk-transitions"
if grep -Eq '^(leonos|reliefos)-(sl|ncurses|sqlite)([@<>=~]|$)' "$root/etc/apk/world"; then
    echo 'retired world requests remain after migration' >&2
    exit 1
fi
cp "$root/etc/apk/world" "$work/world.committed"
cp "$root/lib/apk/db/installed" "$work/database.committed"
run_update --check
cmp "$work/world.committed" "$root/etc/apk/world"
cmp "$work/database.committed" "$root/lib/apk/db/installed"
printf 'ok - signed upgrade removes retired packages/files/cache, migrates world and preserves other requests/configuration\n'
