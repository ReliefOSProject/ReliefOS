#!/bin/sh
# Verify the generated table against the upstream data, not an old build tree.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc=${HOSTCC:-cc}
$cc -std=c11 -Wall -Wextra -Wpedantic -Werror -I"$root" \
    "$root/tools/host/assets/reliefos-gbk.c" \
    "$root/tools/host/common/io.c" "$root/tools/host/common/buffer.c" -o "$work/gen"
"$work/gen" "$root/third_party/litehtml/src/encodings.cpp" "$work/table.h"
cat > "$work/check.c" <<'EOF'
#include <assert.h>
#include "table.h"
int main(void) {
    assert(RELIEFOS_GBK_POINTER_COUNT == 23940);
    unsigned count = 0;
    for (unsigned i = 0; i < RELIEFOS_GBK_POINTER_COUNT; ++i)
        if (reliefos_gbk_to_unicode[i]) ++count;
    assert(count == RELIEFOS_GBK_MAPPED_COUNT);
    for (unsigned i = 0; i < count; ++i) {
        unsigned p = reliefos_gbk_unicode_pointers[i];
        assert(p < RELIEFOS_GBK_POINTER_COUNT && reliefos_gbk_to_unicode[p]);
        if (i) {
            unsigned prev = reliefos_gbk_unicode_pointers[i-1];
            assert(reliefos_gbk_to_unicode[prev] < reliefos_gbk_to_unicode[p] ||
                (reliefos_gbk_to_unicode[prev] == reliefos_gbk_to_unicode[p] && prev < p));
        }
    }
    /* GBK D6 D0 is U+4E2D; CE C4 is U+6587. */
    assert(reliefos_gbk_to_unicode[(0xd6 - 0x81)*190+(0xd0 - 0x41)] == 0x4e2d);
    assert(reliefos_gbk_to_unicode[(0xce - 0x81)*190+(0xc4 - 0x41)] == 0x6587);
}
EOF
$cc -std=c11 -Wall -Werror "$work/check.c" -o "$work/check"
"$work/check"
before=$(stat -c '%y' "$work/table.h")
"$work/gen" "$root/third_party/litehtml/src/encodings.cpp" "$work/table.h"
[ "$before" = "$(stat -c '%y' "$work/table.h")" ]
cp "$work/table.h" "$work/expected"
for invalid in 'garbage' 'int gb18030_decoder::m_index[] = { 1, null };' \
    'int gb18030_decoder::m_index[] = { 99999999999999999999999 };'; do
    printf '%s\n' "$invalid" > "$work/bad.cpp"
    if "$work/gen" "$work/bad.cpp" "$work/table.h" 2>/dev/null; then
        echo 'FAIL - invalid GBK input accepted'; exit 1
    fi
    cmp "$work/table.h" "$work/expected"
done
printf 'ok - GBK mapping, reverse order, stable output, invalid-input preservation\n'
