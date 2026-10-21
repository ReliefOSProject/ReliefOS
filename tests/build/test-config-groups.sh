#!/bin/sh
# Exercise native syncconfig, parallel consumers, missing-member repair and policy.
set -eu
repo=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)
w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT HUP INT TERM
make -s -C "$repo" O="$w/out" defconfig
# defconfig creates native Kconfig files, but product inspection needs the
# separately generated component selection as well. Refuse an incomplete graph
# without touching the output tree under any inspection mode.
test ! -e "$w/out/config/components.mk"
find "$w/out" -printf '%y %P %T@\n' | LC_ALL=C sort > "$w/defconfig.before"
for goal in userland all; do
    if make -s -n -C "$repo" O="$w/out" "$goal" >"$w/dry-$goal.log" 2>&1; then
        echo "make -n $goal accepted a missing component selection" >&2
        exit 1
    fi
    grep -Fq 'config-sync' "$w/dry-$goal.log"
    find "$w/out" -printf '%y %P %T@\n' | LC_ALL=C sort > "$w/defconfig.after"
    cmp "$w/defconfig.before" "$w/defconfig.after"
done
if make -s -q -C "$repo" O="$w/out" all >"$w/query.log" 2>&1; then
    echo 'make -q all accepted a missing component selection' >&2
    exit 1
else
    status=$?
fi
[ "$status" -eq 2 ]
grep -Fq 'config-sync' "$w/query.log"
find "$w/out" -printf '%y %P %T@\n' | LC_ALL=C sort > "$w/defconfig.after"
cmp "$w/defconfig.before" "$w/defconfig.after"
make -s -j8 -C "$repo" O="$w/out" config-sync
for path in .config include/config/auto.conf include/config/auto.conf.cmd include/generated/autoconf.h include/generated/autoconf-installer.h config/components.mk; do
    test -s "$w/out/$path"
done
cp "$w/out/include/generated/autoconf.h" "$w/native.before"
rm "$w/out/include/generated/autoconf.h"
make -s -j8 -C "$repo" O="$w/out" config-sync
cmp "$w/native.before" "$w/out/include/generated/autoconf.h"
# Switch only the normal policy, leaving the installer policy disabled.
sed -i 's/^# CONFIG_VMDK_REQUIRE_LICENSE is not set$/CONFIG_VMDK_REQUIRE_LICENSE=y/' "$w/out/.config"
make -s -C "$repo" O="$w/out" olddefconfig
make -s -j8 -C "$repo" O="$w/out" config-sync
printf 'RELIEFOS_LICENSE_REQUIRE\n' > "$w/policy.c"
cc=${HOSTCC:-cc}
"$cc" -E -P -I"$w/out/include" -include "$repo/userland/runtime/include/build_policy.h" "$w/policy.c" > "$w/normal"
grep -qx 1 "$w/normal"
"$cc" -E -P -I"$w/out/include" -include "$w/out/include/generated/autoconf-installer.h" -include "$repo/userland/runtime/include/build_policy.h" "$w/policy.c" > "$w/installer"
grep -qx 0 "$w/installer"
# -n and -q are inspections; even missing native object directories stay absent.
rm -rf "$w/out/tools"
find "$w/out" -printf '%y %P %T@\n' | LC_ALL=C sort > "$w/before"
make -s -n -C "$repo" O="$w/out" tools > "$w/dry.log"
if make -s -q -C "$repo" O="$w/out" tools > "$w/query.log" 2>&1; then status=0; else status=$?; fi
[ "$status" -le 1 ]
find "$w/out" -printf '%y %P %T@\n' | LC_ALL=C sort > "$w/after"
cmp "$w/before" "$w/after"
# clean keeps the selected config, distclean removes it, both preserve unknown files.
cp "$w/out/.config" "$w/selected"
touch "$w/out/user-owned-image.vmdk"
make -s -C "$repo" O="$w/out" clean
cmp "$w/selected" "$w/out/.config"
test -f "$w/out/user-owned-image.vmdk"
test ! -e "$w/out/scripts"
make -s -C "$repo" O="$w/out" distclean
test ! -e "$w/out/.config"
test -f "$w/out/user-owned-image.vmdk"
printf 'native config: parallel consumers, repair, policy and inspection passed\n'
