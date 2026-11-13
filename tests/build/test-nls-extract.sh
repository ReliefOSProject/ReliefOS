#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(mktemp -d); trap 'rm -rf "$w"' EXIT HUP INT TERM
${HOSTCC:-cc} -std=c11 -Wall -Wextra -Wpedantic -Werror -Wshadow -Wformat=2 \
    -Wstrict-prototypes -Wmissing-prototypes "$src/tools/host/nls/reliefos-nls-extract.c" -o "$w/extract"
cat > "$w/source.c" <<'EOF'
#define T(en, zh) old(en, zh)
/* T("comment", "ignored") */
const char *fake = "T(\"string\", \"ignored\")";
T("A" "B", "translation");
UI_T("File", "file translation");
N_("Static");
T(dynamic, other);
EOF
"$w/extract" --rewrite "$w/source.c"
cp "$w/source.c" "$w/once.c"
"$w/extract" --rewrite "$w/source.c"
cmp "$w/source.c" "$w/once.c"
"$w/extract" "$w/messages.pot" "$w/source.c"
test "$(grep -c '^msgid ' "$w/messages.pot")" = 4
grep -qx 'msgid "AB"' "$w/messages.pot"
cat > "$w/old.po" <<'EOF'
msgid ""
msgstr ""
"Content-Type: text/plain; charset=UTF-8\n"

msgid "AB"
msgstr "kept"

msgid "Removed"
msgstr "history"

EOF
"$w/extract" --merge "$w/old.po" "$w/new.po" "$w/source.c"
grep -qx 'msgstr "kept"' "$w/new.po"
grep -qx '#~ msgid "Removed"' "$w/new.po"
"$w/extract" --merge "$w/new.po" "$w/again.po" "$w/source.c"
cmp "$w/new.po" "$w/again.po"
msgfmt -o "$w/catalog.mo" "$w/new.po"
for catalog in "$w/messages.pot" "$w/new.po"; do
    if [ -z "$(tail -c 2 "$catalog")" ]; then
        printf 'FAIL - extracted catalogs must end with one newline\n'
        exit 1
    fi
done
printf 'nls extract: lexical extraction, rewrite and merge idempotence passed\n'
