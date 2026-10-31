#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)
default_sdk="$root/out/x86_64/release/sdk/reliefos-musl-sdk"
if [ ! -x "$default_sdk/bin/reliefos-musl-cc" ]; then
    default_sdk="$root/out/x86_64/release/sdk/reliefos-musl-sdk"
fi
sdk=${SDK_ROOT:-$default_sdk}
driver="$sdk/bin/reliefos-musl-cc"
compat_lib="$sdk/lib/libleonos.so.2"
new_lib="$sdk/lib/libreliefos.so.2"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM

# Keep the historical goldens unchanged. The explicitly removed window stack
# is a breaking API removal: these families must be absent, including aliases.
retired='^((leonos|reliefos)_(ui_|gui_|mouse_|display_|appearance_|inputm_(note_gui_window|observe_gui_key|poll_gui_commit|take_text|take_key|set_current_context))|text_input_(note_gui_window|observe_gui_key|poll_gui_commit|take_text|take_key|set_current_context)|ui_)'
awk -v retired="$retired" '$1 !~ retired' \
    "$root/tests/fixtures/brand-abi/old-sdk-exports.tsv" > "$work/old.expected"
awk -v retired="$retired" '$1 !~ retired' \
    "$root/tests/fixtures/brand-abi/old-shared-symbols.tsv" > "$work/shared.expected"

test -x "$driver"
mkdir -p "$work/legacy-headers/leonos"
cp "$root/tests/fixtures/brand-abi/old-sdk/include/leonos/api.h" \
   "$work/legacy-headers/leonos/api.h"
cp "$root/tests/fixtures/brand-abi/old-sdk/include/leonos/tar.h" \
   "$work/legacy-headers/leonos/tar.h"

RELIEFOS_SDK_ABI=leonos "$driver" -I"$work/legacy-headers" \
    "$root/tests/fixtures/brand-abi/old_api.c" -o "$work/old.elf"
cat > "$work/compat.c" <<'C'
#include <stddef.h>
#include <leonos/api.h>
#include <leonos/tar.h>
_Static_assert(sizeof(struct leonos_api_info) == 2072U, "legacy header layout changed");
_Static_assert(offsetof(struct leonos_api_info, input_method_settings) == 1560U,
               "legacy header offset changed");
int (*volatile legacy_header_entry)(const char *, char *, uint32_t) = leonos_tar_list;
int main(void) { return legacy_header_entry == NULL; }
C
RELIEFOS_SDK_ABI=leonos "$driver" "$work/compat.c" -o "$work/compat.elf"
"$driver" "$root/tests/fixtures/brand-abi/new_api.c" -o "$work/new.elf"

test -f "$compat_lib"
test -f "$new_lib"
test "$(cat "$root/tests/fixtures/brand-abi/old-soname.txt")" = libleonos.so.2
readelf -d "$work/old.elf" | grep -q 'libleonos.so.2'
readelf -d "$work/compat.elf" | grep -q 'libleonos.so.2'
readelf -d "$work/new.elf" | grep -q 'libreliefos.so.2'
readelf -d "$compat_lib" | grep -q 'Library soname: \[libleonos.so.2\]'
readelf -d "$new_lib" | grep -q 'Library soname: \[libreliefos.so.2\]'

nm -D --defined-only --format=posix "$compat_lib" |
    awk '$1 ~ /^leonos_/ {print $1 "\t" $2}' | LC_ALL=C sort > "$work/old.actual"
cmp "$work/old.expected" "$work/old.actual"
nm -D --defined-only --format=posix "$compat_lib" |
    awk '{print $1 "\t" $2}' | LC_ALL=C sort > "$work/compat.all"
awk -v retired="$retired" '$1 ~ retired {print; failed=1} END {exit failed}' \
    "$work/compat.all"
nm -D --defined-only --format=posix "$new_lib" |
    awk -v retired="$retired" '$1 ~ retired {print; failed=1} END {exit failed}'
tab=$(printf '\t')
join -t "$tab" -1 1 -2 1 \
    "$work/shared.expected" "$work/compat.all" \
    > "$work/shared.joined"
awk -F '\t' '$2 != $3 {print $1}' "$work/shared.joined" > "$work/shared.type-mismatch"
test ! -s "$work/shared.type-mismatch"
join -t "$tab" -v1 -1 1 -2 1 \
    "$work/shared.expected" "$work/compat.all" \
    > "$work/shared.missing"
test ! -s "$work/shared.missing"

nm -D --defined-only --format=posix "$new_lib" |
    awk '$1 ~ /^reliefos_/ {print $1 "\t" $2}' | LC_ALL=C sort > "$work/new.actual"
awk -F '\t' '{sub(/^leonos_/, "reliefos_", $1); print $1 "\t" $2}' \
    "$work/old.expected" > "$work/new.expected"
while IFS="$(printf '\t')" read -r symbol type; do
    awk -F '\t' -v symbol="$symbol" -v type="$type" \
        '$1 == symbol && $2 == type { found = 1 } END { exit !found }' \
        "$work/new.actual"
done < "$work/new.expected"

echo 'ok - supported SDK ABIs retain their SONAMEs and removed graphics exports are absent'
