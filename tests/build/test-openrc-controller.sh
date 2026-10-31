#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
TMPDIR="$src/out/tmp"
export TMPDIR
mkdir -p "$TMPDIR"
work=$(mktemp -d "$TMPDIR/openrc-controller.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOSTCXX:-c++} -std=c++17 -Wall -Wextra -Werror -nostdinc++ -fno-exceptions -fno-rtti \
    -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -I"$src/userland/apps/openrc-controller" \
    "$src/tools/tests/openrc_controller_model_test.cpp" \
    "$src/userland/apps/openrc-controller/model.cpp" -o "$work/model"
ASAN_OPTIONS=detect_leaks=1 "$work/model"
${HOSTCXX:-c++} -std=c++17 -Wall -Wextra -Werror -nostdinc++ -fno-exceptions -fno-rtti \
    -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -I"$src/userland/apps/openrc-controller" \
    "$src/tools/tests/openrc_controller_config_test.cpp" \
    "$src/userland/apps/openrc-controller/model.cpp" \
    "$src/userland/apps/openrc-controller/config.cpp" -o "$work/config"
mkdir "$work/etc"
ASAN_OPTIONS=detect_leaks=1 "$work/config" "$work/etc"
# Exercise the actual session and worker in an exclusive user-namespace chroot.
# Commands below are private fixtures, never host service commands.
${HOSTCXX:-c++} -std=c++17 -Wall -Wextra -Werror -nostdinc++ -fno-exceptions -fno-rtti \
    -I"$src/userland/apps/openrc-controller" \
    "$src/tools/tests/openrc_controller_session_test.cpp" \
    "$src/userland/apps/openrc-controller/model.cpp" \
    "$src/userland/apps/openrc-controller/config.cpp" \
    "$src/userland/apps/openrc-controller/session.cpp" \
    "$src/userland/apps/openrc-controller/worker.cpp" -o "$work/session"
mkdir -p "$work/root/etc/conf.d" "$work/root/etc/init.d" "$work/root/etc/runlevels/default" \
    "$work/root/run/openrc" "$work/root/sbin" "$work/root/bin" "$work/root/dev"
${HOSTCC:-cc} -static -O2 -Wall -Wextra -Werror "$src/tools/tests/openrc_controller_fake_command.c" -o "$work/root/sbin/rc-service"
cp "$work/root/sbin/rc-service" "$work/root/bin/rc-status"
cp "$work/session" "$work/root/session"
mkdir -p "$work/root/usr/lib/reliefos/apps/openrc-controller"
cp "$work/session" "$work/root/usr/lib/reliefos/apps/openrc-controller/openrc-controller.elf"
${HOSTCC:-cc} -static -O2 -Wall -Wextra -Werror "$src/tools/tests/openrc_controller_fake_su.c" -o "$work/root/bin/su"
ldd "$work/session" | awk '{for(i=1;i<=NF;i++)if($i~/^\//)print $i}' | while read -r lib; do
    mkdir -p "$work/root$(dirname "$lib")"; cp "$lib" "$work/root$lib"
done
touch "$work/root/etc/init.d/probe" "$work/root/dev/null"
chmod 0755 "$work/root/etc/init.d/probe"
printf 'default\n' > "$work/root/run/openrc/softlevel"
mkdir -p "$work/root/dev/pts"
ln -s pts/ptmx "$work/root/dev/ptmx"
unshare -Urm --propagation private sh -ec '
    mount -t devpts devpts "$1/dev/pts" -o newinstance,ptmxmode=0666,mode=0620
    exec chroot "$1" /session
' sh "$work/root"
