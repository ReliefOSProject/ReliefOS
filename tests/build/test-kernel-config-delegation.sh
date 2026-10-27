#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Exercise the public OS configuration entry points, using a recording child.
set -eu
src=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
work=$(mktemp -d "${TMPDIR:?TMPDIR must name a data-disk directory}/reliefos-kernel-config.XXXXXX")
trap 'rm -rf "$work"' 0 HUP INT TERM
mkdir "$work/child"
export DELEGATION_LOG=$work/child.log
cat > "$work/child/Makefile" <<'MAKEFILE'
.PHONY: defconfig olddefconfig menuconfig allmodconfig
defconfig olddefconfig menuconfig allmodconfig:
	@test '$(origin KCONFIG_CONFIG)' = undefined
	@test '$(origin KCONFIG_AUTOHEADER)' = undefined
	@mkdir -p '$(O)'
	@printf '%s|%s|%s|%s\n' '$@' '$(O)' '$(ARCH)' '$(PROFILE)' >> "$$DELEGATION_LOG"
	@case '$@' in defconfig) printf 'CONFIG_DRIVER_FIXTURE=y\n' > '$(O)/.config' ;; allmodconfig) printf 'CONFIG_DRIVER_FIXTURE=m\n' > '$(O)/.config' ;; esac
MAKEFILE
cd "$src"
run() {
    make -s -j4 O="$work/os" RELIEFNT_DIR="$work/child" \
        RELIEFNT_O="$work/kernel" ARCH=x86_64 PROFILE=debug "$@" > "$work/make.log" 2>&1 || {
        cat "$work/make.log" >&2; exit 1;
    }
}
# Exported parent Kconfig paths must not escape into the child configuration.
KCONFIG_CONFIG=parent-only KCONFIG_AUTOHEADER=parent-only run defconfig
test "$(cat "$DELEGATION_LOG")" = "defconfig|$work/kernel|x86_64|debug"
test "$(cat "$work/kernel/.config")" = 'CONFIG_DRIVER_FIXTURE=y'
printf 'CONFIG_DRIVER_FIXTURE=m\n' > "$work/kernel/.config"
run olddefconfig
test "$(cat "$work/kernel/.config")" = 'CONFIG_DRIVER_FIXTURE=m'
test "$(wc -l < "$DELEGATION_LOG")" -eq 1
run kernel-olddefconfig
run kernel-menuconfig
test "$(cat "$work/kernel/.config")" = 'CONFIG_DRIVER_FIXTURE=m'
grep -Fx "olddefconfig|$work/kernel|x86_64|debug" "$DELEGATION_LOG"
grep -Fx "menuconfig|$work/kernel|x86_64|debug" "$DELEGATION_LOG"
run kernel-allmodconfig
test "$(cat "$work/kernel/.config")" = 'CONFIG_DRIVER_FIXTURE=m'
run defconfig
test "$(cat "$work/kernel/.config")" = 'CONFIG_DRIVER_FIXTURE=y'
# Relative overrides are resolved by the source entry, before either make
# changes directory into its own O= directory.
relative=out/tmp/relative-kernel-config-$$
make -s O="$work/relative-os" RELIEFNT_DIR="$work/child" RELIEFNT_O="$relative" \
    kernel-olddefconfig > "$work/relative.log" 2>&1 || { cat "$work/relative.log" >&2; exit 1; }
grep -Fx "olddefconfig|$src/$relative|x86_64|release" "$DELEGATION_LOG"
# The recording child creates only this task's owned empty directory.
rmdir "$src/$relative"
printf 'PASS kernel configuration: default reset, persistent m, config isolation, delegates and absolute O\n'
