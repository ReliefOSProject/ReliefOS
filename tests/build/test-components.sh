#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
cc=${HOSTCC:-cc}
"$cc" -std=c11 -Wall -Wextra -Wpedantic -Werror -Wformat=2 -Wshadow -Wstrict-prototypes -Wmissing-prototypes -I"$root" "$root/tools/host/manifest/reliefos-components.c" "$root/tools/host/common/buffer.c" "$root/tools/host/common/io.c" -o "$tmp/parser"
printf '# CONFIG_RELIEFOS_COMPONENT_APP_HELLO_BUILD is not set\nCONFIG_RELIEFOS_COMPONENT_APP_INSTALLER_BUILD=n\nCONFIG_RELIEFOS_COMPONENT_APP_DESKTOP_BUILD=n\nCONFIG_RELIEFOS_COMPONENT_APP_WINDOWD_BUILD=n\nCONFIG_RELIEFOS_COMPONENT_APP_IMD_BUILD=n\nCONFIG_RELIEFOS_COMPONENT_APP_SESSIOND_BUILD=n\n' > "$tmp/config"
"$tmp/parser" --input "$root/configs/components.toml" --config "$tmp/config" --output "$tmp/out" --metadata "$tmp/metadata"
grep '^RELIEFOS_COMPONENTS_DISABLED :=.* hello ' "$tmp/out"
grep '^RELIEFOS_COMPONENT_APPS :=.* installer ' "$tmp/out"
grep '^RELIEFOS_COMPONENT_APPS :=.* reliefexcel ' "$tmp/out"
grep '^RELIEFOS_COMPONENT_APPS :=.* reliefword ' "$tmp/out"
grep '^RELIEFOS_COMPONENT_APPS :=.* reliefppt ' "$tmp/out"
grep '^RELIEFOS_COMPONENT_APPS :=.* reliefmath ' "$tmp/out"
grep '^RELIEFOS_COMPONENT_APPS :=.* reliefapk ' "$tmp/out"
grep '^RELIEFOS_COMPONENT_APPS :=.* driverinfo ' "$tmp/out"
awk -F '\t' '$1=="reliefexcel" && $2=="program-app" && $3==1 && $4==1 && $5==1 && $8=="ReliefExcel" && $9=="Office applications" && $10==".rexl,.csv" && $11==1 { found=1 } END { exit !found }' "$tmp/metadata"
awk -F '\t' '$1=="reliefword" && $2=="program-app" && $3==1 && $4==1 && $5==1 && $8=="ReliefWord" && $9=="Office applications" && $10==".rword,.txt,.html,.htm" && $11==1 { found=1 } END { exit !found }' "$tmp/metadata"
awk -F '\t' '$1=="reliefppt" && $2=="program-app" && $3==1 && $4==1 && $5==1 && $8=="ReliefPPT" && $9=="Office applications" && $10==".rppt" && $11==1 { found=1 } END { exit !found }' "$tmp/metadata"
awk -F '\t' '$1=="reliefmath" && $2=="program-app" && $3==1 && $4==1 && $5==1 && $8=="ReliefMath" && $9=="Office applications" && $10==".rmath" && $11==1 { found=1 } END { exit !found }' "$tmp/metadata"
for id in desktop windowd imd sessiond; do
    grep "^RELIEFOS_COMPONENTS_DISABLED :=.* $id " "$tmp/out"
done
cp "$tmp/out" "$tmp/before"
"$tmp/parser" --input "$root/configs/components.toml" --config "$tmp/config" --output "$tmp/out"
cmp "$tmp/out" "$tmp/before"
if grep -E '^RELIEFOS_COMPONENT_(APPS|ENTRY_APPS) :=.*(^|[[:space:]])ping([[:space:]]|$)' "$tmp/out"; then
    echo 'standalone network command was included as an application' >&2
    exit 1
fi
printf 'version = 1\n[[components]]\nid = "../escape"\n' > "$tmp/bad"
if "$tmp/parser" --input "$tmp/bad" --config "$tmp/config" --output "$tmp/out"; then exit 1; fi
cmp "$tmp/out" "$tmp/before"
printf 'version = 1\n[[components]]\nid = "unterminated\n' > "$tmp/bad"
if "$tmp/parser" --input "$tmp/bad" --config "$tmp/config" --output "$tmp/out"; then exit 1; fi
printf 'component parser tests passed\n'
# Dependency closure wins over explicit disabled dependencies.
printf 'CONFIG_RELIEFOS_COMPONENT_APP_TASKMGR_BUILD=y\nCONFIG_RELIEFOS_COMPONENT_APP_SERVICEMGR_BUILD=n\n' > "$tmp/config"
"$tmp/parser" --input "$root/configs/components.toml" --config "$tmp/config" --output "$tmp/out"
grep '^RELIEFOS_COMPONENTS_ENABLED :=.* servicemgr ' "$tmp/out" >/dev/null
cat > "$tmp/bad" <<'DATA'
version = 1
[[components]]
id = "a"
symbol = "A"
kind = "program-app"
default = true
required = false
depends = ["b"]
[[components]]
id = "b"
symbol = "B"
kind = "program-app"
default = false
required = false
depends = ["a"]
DATA
if "$tmp/parser" --input "$tmp/bad" --config "$tmp/config" --output "$tmp/out"; then exit 1; fi
