#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/root"
printf 'payload\n' > "$tmp/root/file"
ln -s /guest/absolute "$tmp/root/link"
printf './file\n./link\n' > "$tmp/manifest"
sh "$src/tools/build/upstream-verify.sh" "$tmp/root" "$tmp/manifest" "$tmp/present.parameters"
touch -t 200001010000 "$tmp/present.parameters"
before=$(stat -c %Y "$tmp/present.parameters")
sh "$src/tools/build/upstream-verify.sh" "$tmp/root" "$tmp/manifest" "$tmp/present.parameters"
test "$(stat -c %Y "$tmp/present.parameters")" = "$before"
rm "$tmp/root/file"
sh "$src/tools/build/upstream-verify.sh" "$tmp/root" "$tmp/manifest" "$tmp/present.parameters"
test "$(stat -c %Y "$tmp/present.parameters")" -gt "$before"
printf '%s\n' 'upstream presence invalidation: PASS'
