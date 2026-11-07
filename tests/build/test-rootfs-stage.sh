#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'chmod -R u+w "$work"; rm -rf "$work"' EXIT HUP INT TERM
${HOSTCC:-cc} -std=c11 -Wall -Wextra -Werror -Wpedantic -I"$src" \
 "$src/tools/host/manifest/reliefos-stage.c" "$src/tools/host/common/io.c" "$src/tools/host/common/buffer.c" "$src/tools/host/manifest/json.c" -o "$work/stage"
mkdir "$work/input" "$work/root"
printf 'payload\n' > "$work/input/file with spaces"
ln -s 'file with spaces' "$work/input/alias"
printf 't\t%s\t/usr/share/fixture\t0755\tfixture\tunique\n' "$work/input" > "$work/plan"
"$work/stage" "$work/plan" "$work/root" "$work/manifest.json"
cmp "$work/input/file with spaces" "$work/root/usr/share/fixture/file with spaces"
[ "$(readlink "$work/root/usr/share/fixture/alias")" = 'file with spaces' ]
grep -q '"schema_version":1' "$work/manifest.json"
# A duplicate claim must fail before copying any payload.
cat "$work/plan" "$work/plan" > "$work/duplicate"
mkdir "$work/duplicate-root"
if "$work/stage" "$work/duplicate" "$work/duplicate-root" "$work/bad.json" 2>/dev/null; then exit 1; fi
[ ! -e "$work/duplicate-root/usr" ]
# Explicit product overlay wins and mode is retained.
printf 'override\n' > "$work/override"
cp "$work/plan" "$work/overlay"
printf 'f\t%s\t/usr/share/fixture/file with spaces\t0600\tpolicy\toverride\n' "$work/override" >> "$work/overlay"
mkdir "$work/overlay-root"
"$work/stage" "$work/overlay" "$work/overlay-root" "$work/overlay.json"
cmp "$work/override" "$work/overlay-root/usr/share/fixture/file with spaces"
[ "$(stat -c %a "$work/overlay-root/usr/share/fixture/file with spaces")" = 600 ]
# Guest symlink parents must never cause host writes.
mkdir "$work/escape-root" "$work/outside"
printf 'l\t%s\t/escape\t0777\tfixture\tunique\nf\t%s\t/escape/bad\t0644\tfixture\tunique\n' "$work/outside" "$work/override" > "$work/escape-plan"
if "$work/stage" "$work/escape-plan" "$work/escape-root" "$work/bad.json" 2>/dev/null; then exit 1; fi
[ ! -e "$work/outside/bad" ]
printf 'f\t%s\t/../outside\t0644\tfixture\tunique\n' "$work/override" > "$work/traversal"
if "$work/stage" "$work/traversal" "$work/escape-root" "$work/bad.json" 2>/dev/null; then exit 1; fi
# Fresh staging cannot retain a removed resource.
rm "$work/input/alias" "$work/input/file with spaces"
mkdir "$work/deleted-root"
"$work/stage" "$work/plan" "$work/deleted-root" "$work/deleted.json"
[ ! -e "$work/deleted-root/usr/share/fixture/file with spaces" ]
# Column 7 is the optional guest gid: a plan without it keeps "gid":0, an
# explicit value reaches the manifest (M1 service role marks depend on it).
grep -q '"uid":0,"gid":0' "$work/deleted.json"
printf 'payload\n' > "$work/gid-input"
printf 'f\t%s\t/usr/share/fixture/data\t0755\tservice\tunique\t1234\n' "$work/gid-input" > "$work/gid-plan"
mkdir "$work/gid-root"
"$work/stage" "$work/gid-plan" "$work/gid-root" "$work/gid.json"
grep -q '"path":"/usr/share/fixture/data","mode":"0755","uid":0,"gid":1234' "$work/gid.json"
[ "$(stat -c %a "$work/gid-root/usr/share/fixture/data")" = 755 ]
# An absent, empty or non-decimal gid column is refused before staging.
for bad in '' 'bogus' '-1' '99999999999'; do
    printf 'f\t%s\t/bad\t0644\tservice\tunique\t%s\n' "$work/gid-input" "$bad" > "$work/bad-gid"
    rm -rf "$work/bad-gid-root"
    mkdir "$work/bad-gid-root"
    if "$work/stage" "$work/bad-gid" "$work/bad-gid-root" "$work/bad.json" 2>/dev/null; then exit 1; fi
    [ ! -e "$work/bad-gid-root/bad" ]
done
printf 'f\t%s\t/bad\t0644\tservice\tunique\t0\toverload\n' "$work/gid-input" > "$work/extra-field"
if "$work/stage" "$work/extra-field" "$work/gid-root" "$work/bad.json" 2>/dev/null; then exit 1; fi
printf 'rootfs stage: fixtures passed\n'
