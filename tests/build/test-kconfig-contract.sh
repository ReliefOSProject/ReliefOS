#!/bin/sh
# Real native configuration must agree with the component selection reader.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
fail() { printf 'FAIL - %s\n' "$1" >&2; exit 1; }

if grep -E '^(menu)?config ((BUILD_|TEST_|SDK_|RELEASE_)[A-Z0-9_]+|RELIEFOS_COMPONENT_[A-Z0-9_]+_(SDK|API))$' \
    "$src/Kconfig" "$src/Kconfig.components"; then
    fail 'Kconfig exposes controls without product consumers'
fi
make -s -C "$src" O="$work/out" defconfig
make -s -C "$src" O="$work/out" config-sync
config=$work/out/.config
# Starting with olddefconfig must use the same product seed as defconfig.
# Otherwise a first menuconfig/olddefconfig can silently change image policy.
make -s -C "$src" O="$work/fresh" olddefconfig
cmp "$config" "$work/fresh/.config"
set_option() {
    awk -v key="$1" -v value="$2" '
        index($0,key "=")==1 || $0=="# " key " is not set" {next}
        {print} END {print key "=" value}
    ' "$config" > "$work/next"
    mv "$work/next" "$config"
}

# A dependent build is enabled by native Kconfig; an entry cannot outlive its
# image member, even if an old config explicitly requested the entry.
set_option CONFIG_RELIEFOS_COMPONENT_APP_TASKMGR_BUILD y
set_option CONFIG_RELIEFOS_COMPONENT_APP_SERVICEMGR_BUILD n
set_option CONFIG_RELIEFOS_COMPONENT_APP_TASKMGR_IMAGE n
set_option CONFIG_RELIEFOS_COMPONENT_APP_TASKMGR_ENTRY y
make -s -C "$src" O="$work/out" olddefconfig
make -s -C "$src" O="$work/out" config-sync
grep -qx 'CONFIG_RELIEFOS_COMPONENT_APP_SERVICEMGR_BUILD=y' "$config"
if grep -qx 'CONFIG_RELIEFOS_COMPONENT_APP_TASKMGR_ENTRY=y' "$config"; then
    fail 'native Kconfig kept an entry for an excluded image member'
fi
tool=$work/out/host/bin/reliefos-components
"$tool" --input "$src/configs/components.toml" --config "$config" \
    --output "$work/selected.mk" --metadata "$work/selected.tsv"
awk -F '\t' '$1=="taskmgr" {found=1;if($3!=1 || $4!=0 || $5!=0)exit 1} END{if(!found)exit 1}' "$work/selected.tsv"

# Obsolete SDK and unsupported library image/entry keys must not remain a
# second configuration interface behind the native frontend.
cat > "$work/obsolete.config" <<'CONFIG'
CONFIG_RELIEFOS_COMPONENT_APP_HELLO_SDK=y
CONFIG_RELIEFOS_COMPONENT_LIB_NCURSES_BUILD=y
# CONFIG_RELIEFOS_COMPONENT_LIB_NCURSES_SDK is not set
CONFIG_RELIEFOS_COMPONENT_LIB_NCURSES_ENTRY=y
CONFIG_RELIEFOS_COMPONENT_LIB_STARDUSTUI_BUILD=y
CONFIG_RELIEFOS_COMPONENT_LIB_STARDUSTUI_IMAGE=y
CONFIG
"$tool" --input "$src/configs/components.toml" --config "$work/obsolete.config" \
    --output "$work/obsolete.mk" --metadata "$work/obsolete.tsv"
awk -F '\t' '$1=="hello" {found=1;if($6!=0)exit 1} END{if(!found)exit 1}' "$work/obsolete.tsv"
awk -F '\t' '$1=="ncurses" {found=1;if($5!=0 || $6!=1)exit 1} END{if(!found)exit 1}' "$work/obsolete.tsv"
awk -F '\t' '$1=="stardustui" {found=1;if($4!=0 || $5!=0)exit 1} END{if(!found)exit 1}' "$work/obsolete.tsv"

cp "$config" "$work/before"
make -s -C "$src" O="$work/out" olddefconfig
cmp "$work/before" "$config"
printf 'Kconfig product ownership, native dependencies and obsolete controls passed\n'
