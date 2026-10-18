#!/bin/sh
set -eu

src=${1:-$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

python3 - "$src/configs/apk-ownership.json" <<'PY'
import json, sys
policy = json.load(open(sys.argv[1], encoding="utf-8"))
groups = policy["groups"]
required = {"reliefos-nls", "reliefos-apps", "reliefos-fastfetch", "reliefos-xdm-config",
            "reliefos-mimalloc", "reliefos-musl-dev", "reliefos-apk-tools", "reliefos-base"}
missing = sorted(required - groups.keys())
legacy = sorted(name for name in groups if name.startswith("leonos-"))
if missing or legacy or policy.get("current_distributor") != "reliefos-build":
    raise SystemExit(f"APK ownership identity is not migrated: missing={missing}, legacy={legacy}, distributor={policy.get('current_distributor')}")
for app in ("reliefexcel", "reliefword", "reliefppt"):
    if app not in groups["reliefos-apps"]["components"]:
        raise SystemExit(f"{app} is missing from reliefos-apps ownership")
PY

cc -std=c11 -Wall -Wextra -Wpedantic -Werror -Wformat=2 -Wshadow \
  -Wstrict-prototypes -Wmissing-prototypes -I"$src" \
  "$src/tools/host/apk/reliefos-apk-own.c" \
  "$src/tools/host/manifest/json.c" "$src/tools/host/common/buffer.c" \
  "$src/tools/host/common/io.c" -o "$tmp/reliefos-apk-own"

mkdir -p "$tmp/root/usr/bin" "$tmp/root/usr/lib/leonos" "$tmp/root/usr/lib/reliefos/apps/demo"
printf elf >"$tmp/root/usr/bin/tool"
chmod 755 "$tmp/root/usr/bin/tool"
ln -s tool "$tmp/root/usr/bin/tool-link"
printf app >"$tmp/root/usr/lib/reliefos/apps/demo/demo.elf"
printf legacy-library >"$tmp/root/usr/lib/leonos/libleonos.so.2"
printf library >"$tmp/root/usr/lib/reliefos/libreliefos.so.2"
for size in 16 32 48 64 128 256; do
    mkdir -p "$tmp/root/usr/share/icons/hicolor/${size}x${size}/apps"
    printf png > "$tmp/root/usr/share/icons/hicolor/${size}x${size}/apps/reliefos-demo.png"
done
printf png > "$tmp/root/usr/share/icons/hicolor/48x48/apps/folder.png"
cat >"$tmp/policy.json" <<'EOF'
{"version":1,"current_distributor":"reliefos-build","apk_registration":"not-installed","groups":{
 "reliefos-tools":{"destination":"reliefos","components":[],"paths":["usr/bin/tool"]},
 "reliefos-demo":{"destination":"reliefos","components":["demo"]},
 "reliefos-apps":{"destination":"reliefos","components":[],"paths":["usr/lib/leonos","usr/lib/reliefos"]},
 "reliefos-base":{"destination":"reliefos","components":[]}
}}
EOF

"$tmp/reliefos-apk-own" --policy "$tmp/policy.json" --root "$tmp/root" --output "$tmp/list" \
  --installed-policy "$tmp/installed.json"

# A component-specific group must beat the broad application-library rule.
cat >"$tmp/components.toml" <<'EOF'
[[components]]
id = "demo"
EOF
PYTHONDONTWRITEBYTECODE=1 python3 - "$src" "$tmp" <<'PY'
import json, sys
from pathlib import Path
sys.path.insert(0, str(Path(sys.argv[1]) / "tools"))
import apk_ownership
root = Path(sys.argv[2])
_, rules = apk_ownership.load_policy(root / "policy.json", root / "components.toml")
rows = {row[3]: row for row in (line.split("\t") for line in (root / "list").read_text().splitlines())}
failures = []
for path, owner in (("usr/lib/reliefos/apps/demo/demo.elf", "reliefos-demo"),
                    *((f"usr/share/icons/hicolor/{size}x{size}/apps/reliefos-demo.png", "reliefos-demo") for size in (16,32,48,64,128,256)),
                    ("usr/share/icons/hicolor/48x48/apps/folder.png", "reliefos-base"),
                    ("usr/lib/reliefos/libreliefos.so.2", "reliefos-apps"),
                    ("usr/lib/leonos/libleonos.so.2", "reliefos-apps")):
    for implementation, actual in (("C", rows[path][0]),
                                   ("Python", apk_ownership.classify(path, rules)[0])):
        if actual != owner:
            failures.append(f"{implementation}: {path} belongs to {actual}, expected {owner}")
installed = json.loads((root / "installed.json").read_text())
if installed["current_distributor"] != "reliefos-apk":
    failures.append(f"installed distributor: {installed['current_distributor']}, expected reliefos-apk")
if failures:
    raise SystemExit("\n".join(failures))
PY
awk -F '\t' '$1=="reliefos-demo" && $2=="file" && $3=="0644" && $4=="usr/lib/reliefos/apps/demo/demo.elf" && $5=="-" { ok=1 } END { exit !ok }' "$tmp/list"
awk -F '\t' '$1=="reliefos-apps" && $2=="file" && $4=="usr/lib/reliefos/libreliefos.so.2" && $5=="-" { ok=1 } END { exit !ok }' "$tmp/list"
awk -F '\t' '$1=="reliefos-apps" && $2=="file" && $4=="usr/lib/leonos/libleonos.so.2" && $5=="-" { ok=1 } END { exit !ok }' "$tmp/list"
awk -F '\t' '$1=="reliefos-tools" && $2=="file" && $3=="0755" && $4=="usr/bin/tool" && $5=="-" { ok=1 } END { exit !ok }' "$tmp/list"
awk -F '\t' '$1=="reliefos-tools" && $2=="symlink" && $3=="0777" && $4=="usr/bin/tool-link" && $5=="tool" { ok=1 } END { exit !ok }' "$tmp/list"

# Ambiguous policy claims must fail before any package is built.
cat >"$tmp/bad.json" <<'EOF'
{"version":1,"apk_registration":"not-installed","groups":{
 "one":{"destination":"reliefos","components":[],"paths":["usr/bin/tool"]},
 "two":{"destination":"reliefos","components":[],"paths":["usr/bin/tool"]}
}}
EOF
if "$tmp/reliefos-apk-own" --policy "$tmp/bad.json" --root "$tmp/root" --output "$tmp/bad-list" 2>"$tmp/error"; then
  echo 'ambiguous ownership was accepted' >&2
  exit 1
fi
grep -q 'ambiguous ownership' "$tmp/error"

# Binary detection must use bytes, not executable modes or filename suffixes.
printf '\177ELFfixture' > "$tmp/root/usr/bin/non-executable-data"
"$tmp/reliefos-apk-own" --policy "$tmp/policy.json" --root "$tmp/root" --output "$tmp/list" \
  --elf-list "$tmp/elf" --installed-policy "$tmp/installed.json"
test "$(wc -l < "$tmp/elf")" = 1
grep -q 'non-executable-data' "$tmp/elf"
grep -q '"apk_registration":"installed-by-upstream-apk"' "$tmp/installed.json"
# XDM policy overlaps exactly five upstream paths. All other XDM files retain
# upstream ownership when the package stage strips the verified archive list.
for path in etc/X11/xdm/xdm-config etc/X11/xdm/Xservers etc/X11/xdm/Xresources \
    usr/lib/X11/xdm/Xsession usr/lib/X11/xdm/Xsetup_0 usr/lib/X11/xdm/chooser; do
    mkdir -p "$tmp/root/${path%/*}"
    printf 'XDM fixture\n' > "$tmp/root/$path"
done
"$tmp/reliefos-apk-own" --policy "$src/configs/apk-ownership.json" --root "$tmp/root" --output "$tmp/xdm-list"
PYTHONDONTWRITEBYTECODE=1 python3 - "$src" "$tmp" <<'PY'
import sys
from pathlib import Path
root = Path(sys.argv[2])
rows = {row[3]: row[0] for row in (line.split("\t") for line in (root / "xdm-list").read_text().splitlines())}
expected = {"etc/X11/xdm/xdm-config", "etc/X11/xdm/Xservers", "etc/X11/xdm/Xresources",
            "usr/lib/X11/xdm/Xsession", "usr/lib/X11/xdm/Xsetup_0"}
assert {path for path, owner in rows.items() if owner == "reliefos-xdm-config"} == expected
assert rows["usr/lib/X11/xdm/chooser"] != "reliefos-xdm-config"
PY
printf 'apk ownership: component groups, legacy libraries, installed identity, policy conflicts, symlinks and ELF inventory passed\n'
