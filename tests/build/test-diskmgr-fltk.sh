#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-$src/out/tmp}/diskmgr-test.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
test -f "$src/userland/apps/diskmgr/main.cpp" || {
    echo 'FAIL: Diskmgr still has no FLTK/X11 frontend' >&2
    exit 1
}
test ! -e "$src/userland/apps/diskmgr/main.c"
${HOSTCXX:-c++} -std=c++17 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -idirafter "$src/userland/runtime/include" \
    -I"$src/userland/apps/diskmgr" \
    "$src/tests/host/test_diskmgr_model.cpp" \
    "$src/userland/apps/diskmgr/model.cpp" -o "$work/model"
"$work/model"
${HOSTCXX:-c++} -std=c++17 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -idirafter "$src/userland/runtime/include" \
    -I"$src/userland/apps/diskmgr" \
    "$src/tests/host/test_diskmgr_worker.cpp" \
    "$src/userland/apps/diskmgr/worker.cpp" -o "$work/worker"
"$work/worker"
${HOSTCC:-cc} -std=c11 -Wall -Wextra -Werror -Wno-address-of-packed-member \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -idirafter "$src/userland/runtime/include" \
    "$src/tests/host/test_blockdev_names.c" -o "$work/names"
"$work/names"
msgfmt -c --check -o "$work/zh_CN.mo" "$src/configs/nls/po/zh_CN.po"
python3 - "$src" "$work/zh_CN.mo" <<'PY'
import ast,gettext,re,sys
from pathlib import Path
with open(sys.argv[2],'rb') as stream:
    catalog=gettext.GNUTranslations(stream)
source=(Path(sys.argv[1])/'userland/apps/diskmgr/main.cpp').read_text()
ids={ast.literal_eval('"'+value+'"') for value in re.findall(r'\bT\("((?:[^"\\]|\\.)*)"\)',source)}
missing=sorted(value for value in ids if catalog.gettext(value)==value)
assert not missing, 'Diskmgr Chinese translations missing: '+repr(missing)
print('PASS all Diskmgr UI strings have active Chinese translations')
PY
if command -v desktop-file-validate >/dev/null 2>&1; then
    desktop-file-validate "$src/userland/apps/diskmgr/diskmgr.desktop"
fi
