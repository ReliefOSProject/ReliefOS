#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(mktemp -d); trap 'rm -rf "$w"' EXIT HUP INT TERM
${HOSTCC:-cc} -std=c11 -O2 -Wall -Wextra -Werror -I"$src" "$src/tools/host/manifest/reliefos-components.c" "$src/tools/host/common/io.c" "$src/tools/host/common/buffer.c" -o "$w/components"
printf 'CONFIG_RELIEFOS_COMPONENT_APP_HELLO_BUILD=y\n# CONFIG_RELIEFOS_COMPONENT_APP_HELLO_IMAGE is not set\n# CONFIG_RELIEFOS_COMPONENT_APP_DESKTOP_IMAGE is not set\n' > "$w/config"
"$w/components" --input "$src/configs/components.toml" --config "$w/config" --output "$w/mk" --metadata "$w/tsv" --selection "$w/json"
awk -F '\t' '$1=="hello" {if($3!=1 || $4!=0 || $5!=0) exit 1;found=1} END{if(!found)exit 1}' "$w/tsv"
awk -F '\t' '$1=="installer" {if($3!=1)exit 1;found=1} END{if(!found)exit 1}' "$w/tsv"
awk -F '\t' '$1=="desktop" {if($3!=0 || $4!=0)exit 1;found=1} END{if(!found)exit 1}' "$w/tsv"
grep -q '"schema_version":1' "$w/json"
printf 'component metadata: independent image selection and required policy passed\n'
