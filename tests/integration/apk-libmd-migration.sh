#!/bin/sh
# Run in a disposable signed root/guest with two public executable probes.
# The account created here is temporary and has no home or usable password.
set -eu
[ "$#" = 2 ] || { echo 'usage: apk-libmd-migration.sh DYNAMIC_PROBE STATIC_PROBE' >&2; exit 2; }
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
test "$(id -u)" = 0
apk info -e libmd
apk info -e libmd-doc
apk info --who-owns /usr/lib/libmd.so.0 | grep -F 'owned by libmd-'
apk info --who-owns /usr/share/licenses/libmd/COPYING | grep -F 'owned by libmd-doc-'
for library in libmd.so libmd.so.0 libmd.so.0.3.0; do
    apk info --who-owns "/lib/$library" | grep -F 'owned by reliefos-auth-support-libraries-'
    test -s "/lib/$library"
done
test "$(readlink /lib/libmd.so.0)" = ../usr/lib/libmd.so.0
test "$(readlink /lib/libmd.so)" = libmd.so.0
test "$(readlink /lib/libmd.so.0.3.0)" = libmd.so.0
for path in /lib/libmd.a /usr/lib/libmd.a /usr/include/md5.h /lib/pkgconfig/libmd.pc; do
    test ! -e "$path"
done
work=$(mktemp -d /tmp/apk-libmd.XXXXXX)
account=mdqa$$
group=mdqg$$
group_created=no user_created=no
cleanup() {
    status=$?
    trap - 0
    if [ "$user_created" = yes ]; then userdel "$account" || status=1; fi
    if [ "$group_created" = yes ]; then groupdel "$group" || status=1; fi
    rm -rf "$work" || status=1
    exit "$status"
}
trap cleanup 0
trap 'exit 130' HUP INT TERM
printf abc >"$work/input"
chmod 600 "$work/input"
for probe do
    "$probe" "$work/input"
    runuser -u nobody -- "$probe" --deny "$work/input"
done
groupadd "$group"
group_created=yes
useradd -l -M -g "$group" -s /bin/sh "$account"
user_created=yes
usermod -c libmd-qa -s /bin/bash "$account"
test "$(id -gn "$account")" = "$group"
userdel "$account"
user_created=no
groupdel "$group"
group_created=no
printf '%s\n' 'PASS official libmd ownership, compatibility links, digests and shadow account tools'
