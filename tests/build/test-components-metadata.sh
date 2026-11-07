#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(mktemp -d); trap 'rm -rf "$w"' EXIT HUP INT TERM
${HOSTCC:-cc} -std=c11 -O2 -Wall -Wextra -Werror -I"$src" "$src/tools/host/manifest/reliefos-components.c" "$src/tools/host/common/io.c" "$src/tools/host/common/buffer.c" -o "$w/components"
printf 'CONFIG_RELIEFOS_COMPONENT_APP_MEMTEST_BUILD=y\n# CONFIG_RELIEFOS_COMPONENT_APP_MEMTEST_IMAGE is not set\n# CONFIG_RELIEFOS_COMPONENT_APP_FILEMAN_IMAGE is not set\n' > "$w/config"
"$w/components" --input "$src/configs/components.toml" --config "$w/config" --output "$w/mk" --metadata "$w/tsv" --selection "$w/json"
awk -F '\t' '$1=="memtest" {if($3!=1 || $4!=0 || $5!=0) exit 1;found=1} END{if(!found)exit 1}' "$w/tsv"
awk -F '\t' '$1=="installer" {if($3!=1)exit 1;found=1} END{if(!found)exit 1}' "$w/tsv"
awk -F '\t' '$1=="reliefmath" {if($3!=1 || $4!=1 || $5!=1 || $8!="ReliefMath" || $9!="Office applications" || $10!=".rmath" || $11!=1)exit 1;found=1} END{if(!found)exit 1}' "$w/tsv"
awk -F '\t' '$1=="reliefmathpp" {if($3!=1 || $4!=1 || $5!=1 || $8!="ReliefMath++" || $9!="Office applications" || $10!=".rmathpp" || $11!=1)exit 1;found=1} END{if(!found)exit 1}' "$w/tsv"
grep -q '"schema_version":1' "$w/json"
awk -F '\t' '$1=="reliefdiagram" {if($3!=1 || $4!=1 || $5!=1 || $8!="ReliefDiagram" || $9!="Office applications" || $10!=".rdiagram" || $11!=1)exit 1;found=1} END{if(!found)exit 1}' "$w/tsv"
printf 'component metadata: independent image selection and required policy passed\n'
printf '# CONFIG_RELIEFOS_COMPONENT_APP_RELIEFDIAGRAM_IMAGE is not set\n' > "$w/diagram-config"
"$w/components" --input "$src/configs/components.toml" --config "$w/diagram-config" --output "$w/diagram-mk" --metadata "$w/diagram-tsv"
awk -F '\t' '$1=="reliefdiagram" {if($3!=1 || $4!=0 || $5!=0)exit 1;found=1} END{if(!found)exit 1}' "$w/diagram-tsv"
printf '# CONFIG_RELIEFOS_COMPONENT_APP_RELIEFDIAGRAM_BUILD is not set\n' > "$w/diagram-config"
"$w/components" --input "$src/configs/components.toml" --config "$w/diagram-config" --output "$w/diagram-mk" --metadata "$w/diagram-tsv"
awk -F '\t' '$1=="reliefdiagram" {if($3!=0 || $4!=0 || $5!=0)exit 1;found=1} END{if(!found)exit 1}' "$w/diagram-tsv"
awk -F '\t' '$1=="reliefplayer" {if($3!=1 || $4!=1 || $5!=1 || $8!="ReliefPlayer" || $9!="Network and multimedia" || $10!=".mp3,.wav" || $11!=1)exit 1;found=1} END{if(!found)exit 1}' "$w/tsv"
printf '# CONFIG_RELIEFOS_COMPONENT_APP_RELIEFPLAYER_IMAGE is not set\n' > "$w/player-config"
"$w/components" --input "$src/configs/components.toml" --config "$w/player-config" --output "$w/player-mk" --metadata "$w/player-tsv"
awk -F '\t' '$1=="reliefplayer" {if($3!=1 || $4!=0 || $5!=0)exit 1;found=1} END{if(!found)exit 1}' "$w/player-tsv"
awk -F '\t' '$1=="reliefcapture" {if($3!=1 || $4!=1 || $5!=1 || $8!="ReliefCapture" || $9!="Desktop applications" || $10!="" || $11!=0)exit 1;found=1} END{if(!found)exit 1}' "$w/tsv"
printf '# CONFIG_RELIEFOS_COMPONENT_APP_RELIEFCAPTURE_IMAGE is not set\n' > "$w/capture-config"
"$w/components" --input "$src/configs/components.toml" --config "$w/capture-config" --output "$w/capture-mk" --metadata "$w/capture-tsv"
awk -F '\t' '$1=="reliefcapture" {if($3!=1 || $4!=0 || $5!=0)exit 1;found=1} END{if(!found)exit 1}' "$w/capture-tsv"
