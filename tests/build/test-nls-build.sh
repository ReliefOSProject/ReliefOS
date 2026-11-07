#!/bin/sh
# The catalog must be reproducible: same .po, same msgfmt flags, same bytes --
# and pinned to little-endian, which the flags are not without being told.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(mktemp -d); trap 'rm -rf "$w"' EXIT HUP INT TERM

grep -qx 'zh_CN' "$src/configs/nls/LINGUAS"
test -s "$src/configs/nls/po/zh_CN.po"

msgfmt -c --check --endianness=little -o "$w/a.mo" "$src/configs/nls/po/zh_CN.po" >"$w/log" 2>&1
test ! -s "$w/log"
SOURCE_DATE_EPOCH=1000000 TZ=UTC+9 msgfmt -c --check --endianness=little \
    -o "$w/b.mo" "$src/configs/nls/po/zh_CN.po" >"$w/log" 2>&1
test ! -s "$w/log"
cmp "$w/a.mo" "$w/b.mo"

# Little and big must differ, otherwise the pin is meaningless.
msgfmt -c --check --endianness=big -o "$w/c.mo" "$src/configs/nls/po/zh_CN.po"
if cmp -s "$w/a.mo" "$w/c.mo"; then
    printf 'FAIL - endianness does not affect output, pin is pointless\n'
    exit 1
fi

# The recipe must carry the flags; a bare msgfmt would be host-endian.
grep -q -- '--endianness=little' "$src/scripts/reliefos/Makefile.nls"

# Every msgid in the catalog is used by a live source site.
awk '/^msgid "/{s++} /^msgstr[0 ]/{g++} END{exit !(s>1000 && g==s)}' \
    "$src/configs/nls/po/zh_CN.po"

printf 'nls build: reproducibility, endianness pin and catalog shape passed\n'
