#!/bin/sh
# Exercise command routing across the non-usr-merge executable directories.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
mkdir -p "$src/out/tmp"
work=$(TMPDIR="$src/out/tmp" mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/gnu/usr/bin" "$work/vendor/usr/bin"
printf '#!/bin/sh\nprintf "GNU fixture\\n"\n' > "$work/gnu/usr/bin/coreutils"
chmod 755 "$work/gnu/usr/bin/coreutils"
for name in cat cp chroot false '['; do
    ln -s coreutils "$work/gnu/usr/bin/$name"
done
printf '#!/bin/sh\nexit 0\n' > "$work/vendor/usr/bin/find"
printf 't\t%s\t/\t0755\tcoreutils\tunique\n' "$work/gnu" > "$work/plan"
printf 't\t%s\t/\t0755\tfindutils\tunique\n' "$work/vendor" >> "$work/plan"
printf '%s\n' /bin/cat /bin/cp /usr/sbin/chroot /bin/false '/usr/bin/[' \
    /usr/bin/find /bin/ash /bin/sh /usr/bin/tree > "$work/links"
sh "$src/tools/build/busybox-links.sh" "$work/plan" "$work/links" > "$work/generated"
for spec in 'cat ../usr/bin/cat' 'cp ../usr/bin/cp' 'false ../usr/bin/false'; do
    set -- $spec
    awk -F '\t' -v guest="/bin/$1" -v target="$2" \
        '$1=="l" && $2==target && $3==guest && $5=="coreutils" {ok=1} END{exit !ok}' "$work/generated" || {
        echo "FAIL - missing GNU compatibility command: /bin/$1" >&2; exit 1;
    }
done
awk -F '\t' '$1=="l" && $2=="../bin/chroot" && $3=="/usr/sbin/chroot" && $5=="coreutils" {ok=1} END{exit !ok}' "$work/generated"
if awk -F '\t' '$5=="busybox" && $3 ~ /\/(cat|cp|chroot|false|find)$/ {bad=1} END{exit !bad}' "$work/generated"; then
    echo 'FAIL - BusyBox shadows a selected GNU command' >&2; exit 1
fi
for path in '/usr/bin/[' /usr/bin/find; do
    if awk -F '\t' -v path="$path" '$3==path {found=1} END{exit !found}' "$work/generated"; then
        echo "FAIL - existing command was emitted twice: $path" >&2; exit 1
    fi
done
for path in /bin/ash /bin/sh /usr/bin/tree; do
    awk -F '\t' -v path="$path" '$3==path && $5=="busybox" {ok=1} END{exit !ok}' "$work/generated"
done
# A direct file rule claims a GNU command in the same way as a source tree.
printf 'f\t%s\t/usr/bin/cat\t0755\tcoreutils\tunique\n' "$work/gnu/usr/bin/coreutils" > "$work/direct-plan"
printf '%s\n' /bin/cat > "$work/direct-links"
sh "$src/tools/build/busybox-links.sh" "$work/direct-plan" "$work/direct-links" > "$work/direct-generated"
awk -F '\t' '$2=="../usr/bin/cat" && $3=="/bin/cat" && $5=="coreutils" {ok=1} END{exit !ok}' "$work/direct-generated"
printf '%s\n' 'GNU/BusyBox command routing and compatibility paths: PASS'
