#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

python3 "$root/tools/generate_component_kconfig.py" \
    --output "$tmp/Kconfig.components"
cmp "$root/Kconfig.components" "$tmp/Kconfig.components"
if grep -q '^config LEON_COMPONENT_' "$tmp/Kconfig.components"; then
    echo 'generated Kconfig still contains old component symbols' >&2
    exit 1
fi
if grep -E '^[[:space:]]*(config|select|depends on)[[:space:]]+LEON_COMPONENT_' \
        "$root/Kconfig" "$root/Kconfig.components"; then
    echo 'Kconfig contains a dangling old component symbol' >&2
    exit 1
fi

old="$tmp/old.config"
new="$tmp/new.config"
cat > "$old" <<'CONFIG'
CONFIG_LEON_COMPONENT_APP_HELLO_BUILD=y
# CONFIG_LEON_COMPONENT_APP_DESKTOP_IMAGE is not set
CONFIG_VMDK_DEFAULT_LANG="zh_CN.UTF-8"
CONFIG
cp "$old" "$tmp/old.before"

sh "$root/tools/build/reliefos-config-migrate.sh" "$old" "$new"
grep -qx 'CONFIG_RELIEFOS_COMPONENT_APP_HELLO_BUILD=y' "$new"
grep -qx '# CONFIG_RELIEFOS_COMPONENT_APP_DESKTOP_IMAGE is not set' "$new"
grep -qx 'CONFIG_VMDK_DEFAULT_LANG="zh_CN.UTF-8"' "$new"
if grep -q 'CONFIG_LEON_COMPONENT_' "$new"; then
    echo 'old component symbols remain in migrated output' >&2
    exit 1
fi
cmp "$old" "$tmp/old.before"

cat > "$tmp/conflict.config" <<'CONFIG'
CONFIG_LEON_COMPONENT_APP_HELLO_BUILD=y
# CONFIG_RELIEFOS_COMPONENT_APP_HELLO_BUILD is not set
CONFIG
if sh "$root/tools/build/reliefos-config-migrate.sh" \
        "$tmp/conflict.config" "$tmp/conflict.out"; then
    echo 'conflicting old and new symbols were accepted' >&2
    exit 1
fi
[ ! -e "$tmp/conflict.out" ]

mkdir -p "$tmp/existing"
cp "$old" "$tmp/existing/.config"
make -s -C "$root" O="$tmp/existing" migrate-config
cmp "$old" "$tmp/existing/.config.leonos.bak"
grep -qx 'CONFIG_RELIEFOS_COMPONENT_APP_HELLO_BUILD=y' \
    "$tmp/existing/.config"
if grep -q 'CONFIG_LEON_COMPONENT_' "$tmp/existing/.config"; then
    echo 'old component symbols remain in explicitly migrated O config' >&2
    exit 1
fi

make -s -C "$root" O="$tmp/new-output" defconfig
grep -q '^CONFIG_RELIEFOS_COMPONENT_APP_HELLO_BUILD=y$' \
    "$tmp/new-output/.config"
if grep -q '^CONFIG_LEON_COMPONENT_' "$tmp/new-output/.config"; then
    echo 'generated default config still contains old component symbols' >&2
    exit 1
fi
printf 'config migration and ReliefOS component symbol tests passed\n'
