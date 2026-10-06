#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/vendor/bin" "$work/vendor/sbin" "$work/vendor/usr/bin" "$work/inactive/usr/bin"
printf 'GNU dd\n' > "$work/vendor/bin/dd"
printf 'util-linux mount\n' > "$work/vendor/bin/mount"
printf 'ncurses clear\n' > "$work/vendor/usr/bin/clear"
ln -s missing-target "$work/vendor/usr/bin/reset"
printf 'shadow passwd\n' > "$work/vendor/usr/bin/passwd"
printf 'not selected\n' > "$work/inactive/usr/bin/tree"
printf 't\t%s\t/\t0755\tvendor\tunique\n' "$work/vendor" > "$work/plan"
printf 'l\t../lib/reliefos/apps/ping/ping.elf\t/usr/bin/ping\t0777\tping\tunique\n' >> "$work/plan"
printf '%s\n' /bin/ash /bin/sh /bin/false /bin/ping /sbin/mount /usr/bin/dd /usr/bin/clear /bin/reset /bin/passwd /usr/bin/tree /usr/bin/hexdump > "$work/links"

# Exercise the link composer itself; the plan is the authority, including trees.
if sh "$src/tools/build/busybox-links.sh" "$work/plan" "$work/links" > "$work/generated"; then :; else
    echo 'BusyBox link composer rejected valid fixture' >&2; exit 1
fi
for name in ping mount dd clear reset passwd; do
    if awk -F '\t' -v name="$name" '$5=="busybox" {n=split($3,a,"/"); if(a[n]==name) found=1} END {exit !found}' "$work/generated"; then
        echo "BusyBox shadows another command provider: $name" >&2; exit 1
    fi
done
for spec in '/bin/ash busybox' '/bin/sh busybox' '/bin/false busybox' '/usr/bin/tree ../../bin/busybox' '/usr/bin/hexdump ../../bin/busybox'; do
    set -- $spec
    awk -F '\t' -v path="$1" -v target="$2" '$1=="l" && $2==target && $3==path && $5=="busybox" {found=1} END {exit !found}' "$work/generated"
done
for bad in /bin/../sbin/evil /bin/./evil /bin/subdir/evil /bin/busybox /outside/tool /bin/; do
    printf '%s\n' "$bad" > "$work/bad-links"
    if sh "$src/tools/build/busybox-links.sh" "$work/plan" "$work/bad-links" > "$work/rejected" 2>/dev/null; then
        echo "BusyBox accepted unsafe link: $bad" >&2; exit 1
    fi
    [ ! -s "$work/rejected" ]
done
printf '%s\n' /bin/ash /bin/ash > "$work/bad-links"
if sh "$src/tools/build/busybox-links.sh" "$work/plan" "$work/bad-links" > "$work/rejected" 2>/dev/null; then
    echo 'BusyBox accepted duplicate links' >&2; exit 1
fi
[ ! -s "$work/rejected" ]
# No other command provider is a valid plan, not an empty output condition.
: > "$work/empty-plan"
printf '%s\n' /bin/ash /usr/bin/tree > "$work/empty-links"
sh "$src/tools/build/busybox-links.sh" "$work/empty-plan" "$work/empty-links" > "$work/empty-generated"
[ "$(wc -l < "$work/empty-generated")" = 2 ]
printf '%s\n' 'busybox staging ownership and path checks: PASS'
