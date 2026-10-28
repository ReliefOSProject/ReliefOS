#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
export TMPDIR="$src/out/tmp"
mkdir -p "$TMPDIR"
work=$(mktemp -d "$TMPDIR/network-controller.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOSTCXX:-c++} -std=c++17 -Wall -Wextra -Werror -nostdinc++ -fno-exceptions -fno-rtti \
    -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -I"$src/userland/apps/network-controller" \
    "$src/tools/tests/network_controller_test.cpp" \
    "$src/userland/apps/network-controller/model.cpp" -o "$work/model"
"$work/model"
${HOSTCXX:-c++} -std=c++17 -Wall -Wextra -Werror -nostdinc++ -fno-exceptions -fno-rtti \
    -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -I"$src/userland/apps/network-controller" \
    "$src/tools/tests/network_controller_config_test.cpp" \
    "$src/userland/apps/network-controller/model.cpp" \
    "$src/userland/apps/network-controller/config.cpp" -o "$work/config"
mkdir "$work/etc"
"$work/config" "$work/etc"
headers=${RELIEFOS_TEST_HEADERS:-$src/out/xorg-vmware-debug/kernel-export/include}
test -f "$headers/reliefos/net_control.h" || { echo 'Build exported headers with make headers_install or set RELIEFOS_TEST_HEADERS' >&2; exit 1; }
${HOSTCXX:-c++} -std=c++17 -Wall -Wextra -Werror -nostdinc++ -fno-exceptions -fno-rtti \
    -I"$src/userland/apps/network-controller" -idirafter "$headers" \
    "$src/tools/tests/network_controller_session_test.cpp" \
    "$src/userland/apps/network-controller/model.cpp" "$src/userland/apps/network-controller/config.cpp" \
    "$src/userland/apps/network-controller/session.cpp" "$src/userland/apps/network-controller/worker.cpp" -o "$work/session"
mkdir -p "$work/root/etc/reliefos" "$work/root/bin" "$work/root/sbin" "$work/root/dev" \
    "$work/root/usr/lib/reliefos/apps/network-controller"
${HOSTCC:-cc} -static -O2 -Wall -Wextra -Werror "$src/tools/tests/network_controller_fake_command.c" -o "$work/root/bin/busybox"
cp "$work/root/bin/busybox" "$work/root/sbin/rc-service"
cp "$work/session" "$work/root/session"
cp "$work/session" "$work/root/usr/lib/reliefos/apps/network-controller/network-controller.elf"
ldd "$work/session" | awk '{for(i=1;i<=NF;i++)if($i~/^\//)print $i}' | while read -r lib; do
    mkdir -p "$work/root$(dirname "$lib")";cp "$lib" "$work/root$lib"
done
touch "$work/root/dev/null"
LC_ALL=C LANG=C unshare -Urn chroot "$work/root" /session
