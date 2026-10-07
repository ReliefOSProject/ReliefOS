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
printf '%s\n' /bin/ash /bin/sh /bin/false /bin/ping /bin/ping6 /sbin/mount /usr/bin/dd /usr/bin/clear /bin/reset /bin/passwd /usr/bin/tree /usr/bin/hexdump > "$work/links"

# Exercise the link composer itself; the plan is the authority, including trees.
if sh "$src/tools/build/busybox-links.sh" "$work/plan" "$work/links" > "$work/generated"; then :; else
    echo 'BusyBox link composer rejected valid fixture' >&2; exit 1
fi
for name in mount dd clear reset passwd; do
    if awk -F '\t' -v name="$name" '$5=="busybox" {n=split($3,a,"/"); if(a[n]==name) found=1} END {exit !found}' "$work/generated"; then
        echo "BusyBox shadows another command provider: $name" >&2; exit 1
    fi
done
for spec in '/bin/ash busybox' '/bin/sh busybox' '/bin/false busybox' '/bin/ping busybox' '/bin/ping6 busybox' '/usr/bin/tree ../../bin/busybox' '/usr/bin/hexdump ../../bin/busybox'; do
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

# busybox-check contract: the resolved profile is validated too, including the
# generated .config, because a suppressed link still leaves `busybox NAME`
# callable when the applet is compiled in.
for name in sh ash false ping ping6 df free top dmesg hexdump killall nc netstat \
    traceroute traceroute6 crond crontab mdev insmod lsmod modprobe rmmod \
    unzip bunzip2 xz unxz cpio bzip2 bc dc tree timeout watch; do
    printf '/bin/%s\n' "$name"
done > "$work/check-links"
: > "$work/check-config"
sh "$src/tools/build/busybox-check.sh" "$work/check-config" "$work/check-links" > "$work/check-out"
grep -q 'ownership contract: PASS' "$work/check-out"
for spec in CONFIG_DD=y CONFIG_CLEAR=y CONFIG_START_STOP_DAEMON=y; do
    printf '%s\n' "$spec" > "$work/check-bad-config"
    if sh "$src/tools/build/busybox-check.sh" "$work/check-bad-config" "$work/check-links" > "$work/check-rejected" 2>&1; then
        echo "busybox-check accepted enabled external applet: $spec" >&2; exit 1
    fi
done
grep -v '^/bin/sh$' "$work/check-links" > "$work/check-no-sh"
if sh "$src/tools/build/busybox-check.sh" "$work/check-config" "$work/check-no-sh" > "$work/check-rejected" 2>&1; then
    echo 'busybox-check accepted a profile without /bin/sh' >&2; exit 1
fi
grep -v '^/bin/tree$' "$work/check-links" > "$work/check-no-tree"
if sh "$src/tools/build/busybox-check.sh" "$work/check-config" "$work/check-no-tree" > "$work/check-rejected" 2>&1; then
    echo 'busybox-check accepted a profile without a required applet' >&2; exit 1
fi
if sh "$src/tools/build/busybox-check.sh" "$work/check-config" "$work/links" > "$work/check-rejected" 2>&1; then
    echo 'busybox-check accepted links owned by other providers' >&2; exit 1
fi
printf '%s\n' 'busybox staging ownership and path checks: PASS'
