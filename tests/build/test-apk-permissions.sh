#!/bin/sh
set -eu
repo=$(CDPATH='' cd -- "$(dirname "$0")/../.." && pwd -P)
work=$(mktemp -d)
trap 'rm -rf "$work"' 0
trap 'exit 130' HUP INT TERM
mkdir -p "$work/root/usr/bin" "$work/root/lib/apk/db" \
    "$work/root/install/root/usr/bin" "$work/root/install/root/lib/apk/db" "$work/outside"
for root in "$work/root" "$work/root/install/root"; do
    printf 'fixture\n' >"$root/usr/bin/wall utility"
    printf 'fixture\n' >"$root/usr/bin/su"
    mkdir "$root/restricted"
    cat >"$root/lib/apk/db/installed" <<'DB'
P:util-linux-misc
V:2.42.4-r3
F:usr
F:usr/bin
R:wall utility
a:0:5:2755
Z:fixture
R:su
a:0:0:4755
Z:fixture
F:restricted
M:0:0:550

DB
done
# shellcheck disable=SC2016 # Variables are evaluated inside the fakeroot shell.
FAKEROOTDONTTRYCHOWN=1 fakeroot -- sh -eu -c '
    sh "$1/tools/build/apk-permissions.sh" "$2/root"
    for root in "$2/root" "$2/root/install/root"; do
        test "$(stat -c "%u:%g:%a" "$root/usr/bin/wall utility")" = 0:5:2755
        test "$(stat -c "%u:%g:%a" "$root/usr/bin/su")" = 0:0:4755
        test "$(stat -c "%u:%g:%a" "$root/restricted")" = 0:0:550
    done
' sh "$repo" "$work"
# Verify the real ext4 output, not only fakeroot stat results.
for root in "$work/root" "$work/root/install/root"; do
    chmod 755 "$root/usr/bin/su" "$root/usr/bin/wall utility"
    chmod 750 "$root/restricted"
done
${HOSTCC:-cc} -std=c11 -O2 -Wall -Wextra -Werror \
    "$repo/tools/host/images/reliefos-ext2-time.c" -lext2fs -lcom_err -o "$work/time"
RELIEFOS_EXT4_TIME=$work/time sh "$repo/tools/build/images.sh" ext4 "$work/root" \
    "$work/root.ext4" 1700000000 5c13543b-732c-4f81-8652-621124484420 >"$work/image.log" 2>&1
for prefix in '' /install/root; do
    # debugfs requires quoting for filenames containing spaces.
    debugfs -R "stat \"$prefix/usr/bin/wall utility\"" "$work/root.ext4" >"$work/stat" 2>/dev/null
    grep -Eq 'User: +0 +Group: +5' "$work/stat"
    grep -q '2755' "$work/stat"
    debugfs -R "stat $prefix/usr/bin/su" "$work/root.ext4" >"$work/stat" 2>/dev/null
    grep -q '4755' "$work/stat"
done
test "$(stat -c %a "$work/root/usr/bin/su")" = 755
cp "$work/root/lib/apk/db/installed" "$work/good"
for acl in '0:5:889' 'root:5:2755' '0:5' '0:5:2755:extra'; do
    sed "s@a:0:5:2755@a:$acl@" "$work/good" >"$work/root/lib/apk/db/installed"
    if FAKEROOTDONTTRYCHOWN=1 fakeroot -- sh "$repo/tools/build/apk-permissions.sh" "$work/root"; then
        echo 'invalid ACL accepted' >&2; exit 1
    fi
done
sed 's@R:wall utility@R:../outside@' "$work/good" >"$work/root/lib/apk/db/installed"
if FAKEROOTDONTTRYCHOWN=1 fakeroot -- sh "$repo/tools/build/apk-permissions.sh" "$work/root"; then
    echo 'traversal accepted' >&2; exit 1
fi
ln -s "$work/outside" "$work/root/redirect"
sed 's@F:usr/bin@F:redirect@' "$work/good" >"$work/root/lib/apk/db/installed"
if FAKEROOTDONTTRYCHOWN=1 fakeroot -- sh "$repo/tools/build/apk-permissions.sh" "$work/root"; then
    echo 'escaping parent accepted' >&2; exit 1
fi
printf 'APK permissions: numeric ACLs, setgid, nested payloads and invalid paths passed\n'
