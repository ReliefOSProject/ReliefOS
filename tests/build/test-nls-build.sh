#!/bin/sh
# Each application catalog must be independently loadable and reproducible.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(mktemp -d); trap 'rm -rf "$w"' EXIT HUP INT TERM

grep -qx 'zh_CN' "$src/configs/nls/LINGUAS"
grep -qx 'ja_JP' "$src/configs/nls/LINGUAS"
test -s "$src/configs/nls/DOMAINS"
catalogs=0
for domain in $(cat "$src/configs/nls/DOMAINS"); do
    test -s "$src/configs/nls/po/$domain/$domain.pot"
    for language in $(cat "$src/configs/nls/LINGUAS"); do
        po=$src/configs/nls/po/$domain/$language.po
        directory=$w/$language/LC_MESSAGES
        mkdir -p "$directory"
        msgfmt -c --check --endianness=little -o "$directory/$domain.mo" "$po" >"$w/log" 2>&1
        test ! -s "$w/log"
        if [ "$language" = ja_JP ]; then
            msgcmp --no-fuzzy-matching "$po" "$src/configs/nls/po/$domain/$domain.pot"
            msgattrib --untranslated --no-obsolete "$po" > "$w/untranslated.po"
            if [ -s "$w/untranslated.po" ]; then
                printf 'FAIL - %s has untranslated Japanese messages\n' "$domain" >&2
                exit 1
            fi
        fi
        SOURCE_DATE_EPOCH=1000000 TZ=UTC+9 msgfmt -c --check --endianness=little \
            -o "$w/repeated.mo" "$po" >"$w/log" 2>&1
        test ! -s "$w/log"
        cmp "$directory/$domain.mo" "$w/repeated.mo"
        msgfmt -c --check --endianness=big -o "$w/big.mo" "$po"
        if cmp -s "$directory/$domain.mo" "$w/big.mo"; then
            printf 'FAIL - %s endianness does not affect output\n' "$domain"
            exit 1
        fi
        catalogs=$((catalogs + 1))
    done
done

python3 - "$w" <<'PY'
import gettext
from pathlib import Path
import re
import sys

fileman = gettext.translation("fileman", sys.argv[1], languages=["zh_CN"])
taskmgr = gettext.translation("taskmgr", sys.argv[1], languages=["zh_CN"])
assert fileman.gettext("New Folder") == "新建文件夹"
assert taskmgr.gettext("Processes") == "进程"
assert taskmgr.gettext("New Folder") == "New Folder"
assert fileman.gettext("Processes") == "Processes"
assert fileman.gettext("Cancel") == "取消"
assert taskmgr.gettext("Cancel") == "取消"
excel = gettext.translation("reliefexcel", sys.argv[1], languages=["zh_CN"])
word = gettext.translation("reliefword", sys.argv[1], languages=["zh_CN"])
network = gettext.translation("network-controller", sys.argv[1], languages=["zh_CN"])
assert excel.gettext("I/O error") == "输入输出错误"
assert word.gettext("The file changed while it was being opened") == "文件在打开期间发生变化"
assert network.gettext("Authenticating") == "正在验证身份"
fileman_ja = gettext.translation("fileman", sys.argv[1], languages=["ja_JP"])
taskmgr_ja = gettext.translation("taskmgr", sys.argv[1], languages=["ja_JP"])
driverinfo_ja = gettext.translation("driverinfo", sys.argv[1], languages=["ja_JP"])
assert fileman_ja.gettext("New Folder") == "新規フォルダ"
assert taskmgr_ja.gettext("Processes") == "プロセス"
assert fileman_ja.gettext("Cancel") == "キャンセル"
assert taskmgr_ja.gettext("New Folder") == "New Folder"
assert driverinfo_ja.gettext("Loaded") != "Loaded"
format_argument = re.compile(
    r'%(?:[1-9][0-9]*\$)?[-+#0]*(?:[0-9]+|\*(?:[1-9][0-9]*\$)?)?'
    r'(?:\.(?:[0-9]+|\*(?:[1-9][0-9]*\$)?))?(?:hh|ll|[hljztL])?[diouxXfFeEgGaAcspn%m]'
)
for path in (Path(sys.argv[1]) / "ja_JP/LC_MESSAGES").glob("*.mo"):
    with path.open("rb") as stream:
        catalog = gettext.GNUTranslations(stream)
    for message, translation in catalog._catalog.items():
        if not message:
            continue
        assert format_argument.findall(message) == format_argument.findall(translation), (path, message)
        assert message.count("\n") == translation.count("\n"), (path, message)
        assert message.count("\t") == translation.count("\t"), (path, message)
print("nls domains: independent translations and shared dialog labels passed")
PY

# The recipe must carry the flags; a bare msgfmt would be host-endian.
grep -q -- '--endianness=little' "$src/scripts/reliefos/Makefile.nls"

printf 'nls build: %s independent catalogs, reproducibility and endianness passed\n' "$catalogs"
