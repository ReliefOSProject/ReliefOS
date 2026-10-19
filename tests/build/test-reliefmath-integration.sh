#!/bin/sh
set -eu

src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)
tmp_base=${TMPDIR:-$src/out/tmp}
mkdir -p "$tmp_base"
work=$(mktemp -d "$tmp_base/reliefmath-integration.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

${HOSTCC:-cc} -std=c11 -O2 -Wall -Wextra -Werror -I"$src" \
    "$src/tools/host/manifest/reliefos-components.c" \
    "$src/tools/host/common/io.c" "$src/tools/host/common/buffer.c" \
    -o "$work/components"
printf 'CONFIG_RELIEFOS_COMPONENT_APP_HELLO_BUILD=y\n' > "$work/config"
"$work/components" --input "$src/configs/components.toml" --config "$work/config" \
    --output "$work/components.mk" --metadata "$work/components.tsv"
awk -F '\t' '$1=="reliefmath" && $2=="program-app" && $3==1 && $4==1 && $5==1 && $8=="ReliefMath" && $9=="Office applications" && $10==".rmath" && $11==1 {ok=1} END{exit !ok}' "$work/components.tsv" || {
    echo 'ReliefMath must be a default Xorg Office component with .rmath Open With support' >&2
    exit 1
}
grep -q '^RELIEFOS_COMPONENT_APPS :=.* reliefmath ' "$work/components.mk"
awk '/^config RELIEFOS_COMPONENT_APP_RELIEFMATH_BUILD$/{found=1;next} found && /default y/{ok=1;exit} found && /^config /{exit} END{exit !ok}' "$src/Kconfig.components" || {
    echo 'Kconfig.components does not default ReliefMath on' >&2
    exit 1
}

# The caller adds worker prerequisites while RELIEFOS_APP is being expanded.
macro_end=$(awk '/^define RELIEFOS_APP$/{inside=1;next} inside && /^endef$/{print NR;exit}' "$src/mk/userland.mk")
include_line=$(grep -nF 'include $(RELIEFOS_SRC)/mk/components/reliefmath.mk' "$src/mk/userland.mk" | cut -d: -f1)
foreach_line=$(grep -nF '$(foreach app,$(USERLAND_APPS),$(eval $(call RELIEFOS_APP' "$src/mk/userland.mk" | cut -d: -f1)
test -n "$macro_end" && test -n "$include_line" && test -n "$foreach_line"
test "$macro_end" -lt "$include_line" && test "$include_line" -lt "$foreach_line"
grep -Fqx 'reliefmath' "$src/configs/fltk-apps.list"
grep -Fq 'USERLAND_FLTK_APPS := $(strip $(file <$(RELIEFOS_SRC)/configs/fltk-apps.list))' "$src/mk/userland.mk"
for target in reliefmath-development app-reliefmath-render reliefmath-backend-probe; do
    grep -Fq ".PHONY: $target" "$src/mk/components/reliefmath.mk"
done
grep -Fq 'USERLAND_DEPS_reliefmath += $(USERLAND_DIR)/reliefmath-render.elf' "$src/mk/components/reliefmath.mk"
grep -Fq 'RELIEFMATH_WORKER_CORE := model.cpp render_types.cpp protocol.cpp' "$src/mk/components/reliefmath.mk"
grep -Fq 'RELIEFOS_SIG_reliefmath-worker-cc := cxx=$(TARGET_CXX)|flags=$(RELIEFMATH_WORKER_FLAGS)|sources=$(RELIEFMATH_WORKER_SOURCES)' "$src/mk/components/reliefmath.mk"
grep -Fq 'RELIEFOS_SIG_reliefmath-worker-ld := ld=$(TARGET_LD)|flags=$(USERLAND_LINK_FLAGS)|sources=$(RELIEFMATH_WORKER_SOURCES)|libs=$(RELIEFMATH_WORKER_LIBS)' "$src/mk/components/reliefmath.mk"
grep -Fq '$(if $(filter reliefmath,$(RELIEFOS_COMPONENT_APPS)),$(USERLAND_DIR)/reliefmath-render.elf)' "$src/mk/rootfs.mk"
grep -Fq '$(RELIEFOS_SRC)/resources/reliefmath' "$src/mk/rootfs.mk"
grep -Fq '$(RELIEFOS_SRC)/resources/licenses/reliefmath' "$src/mk/rootfs.mk"
grep -Fq 'reliefmath-resources-stage.sh' "$src/mk/rootfs.mk"
grep -Fq 'reliefmath-resources-stage.sh' "$src/tools/build/rootfs-stage.sh"
grep -Fq 'RELIEFMATH_WORKER_LIBS :=' "$src/mk/components/reliefmath.mk"
grep -Fq 'libcairo.so $(RELIEFMATH_DEV)/usr/lib/libfreetype.so' "$src/mk/components/reliefmath.mk"
grep -Fq 'grep -Fxq "$soname" "$scratch/upstream-sonames"' "$src/tools/build/apk-stage.sh"
if grep -Fq 'reliefmath-backend-probe' "$src/mk/rootfs.mk" "$src/tools/build/rootfs-stage.sh"; then
    echo 'ReliefMath backend probe must not enter a product root' >&2
    exit 1
fi
python3 - "$src/configs/dependencies.lock.json" <<'PY'
import json, sys
entries = {item["id"]: item for item in json.load(open(sys.argv[1], encoding="utf-8"))["dependencies"]}
for runtime, development in (("alpine-cairo", "reliefmath-dev-cairo"),
                             ("alpine-freetype", "reliefmath-dev-freetype")):
    item = entries.get(runtime)
    if not item or item.get("target") != "apk-root" or item.get("feature") != "xorg":
        raise SystemExit(f"ReliefMath runtime provider is not selected for Xorg APKs: {runtime}")
    dev = entries.get(development)
    if not dev or dev.get("version") != item.get("version"):
        raise SystemExit(f"ReliefMath development package does not match runtime: {development}")
PY

# Exercise complete-member validation and the rootfs plan emitted by the
# component-only resource stage helper without touching any configured image.
mkdir -p "$work/source/resources/reliefmath" "$work/source/resources/licenses/reliefmath"
printf 'font payload\n' > "$work/source/resources/reliefmath/STIXTwoMath-Regular.otf"
printf 'license summary\n' > "$work/source/resources/licenses/reliefmath/README.md"
printf 'OFL text\n' > "$work/source/resources/licenses/reliefmath/STIX-OFL.txt"
sh "$src/tools/build/reliefmath-resources-stage.sh" "$work/source" "$work/resources.plan"
tab=$(printf '\t')
for spec in \
    "resources/reliefmath/STIXTwoMath-Regular.otf${tab}/usr/share/reliefmath/STIXTwoMath-Regular.otf${tab}0444" \
    "resources/licenses/reliefmath/README.md${tab}/usr/share/licenses/reliefmath/README.md${tab}0444" \
    "resources/licenses/reliefmath/STIX-OFL.txt${tab}/usr/share/licenses/reliefmath/STIX-OFL.txt${tab}0444"; do
    source=${spec%%${tab}*}
    remainder=${spec#*${tab}}
    guest=${remainder%%${tab}*}
    mode=${remainder##*${tab}}
    awk -F '\t' -v source="$work/source/$source" -v guest="$guest" -v mode="$mode" \
        '$1=="f" && $2==source && $3==guest && $4==mode && $5=="reliefmath" && $6=="unique" {found=1} END{exit !found}' \
        "$work/resources.plan" || { echo "resource plan is missing $guest" >&2; exit 1; }
done
cp "$work/resources.plan" "$work/resources.before"
rm "$work/source/resources/licenses/reliefmath/STIX-OFL.txt"
if sh "$src/tools/build/reliefmath-resources-stage.sh" "$work/source" "$work/resources.plan" 2>/dev/null; then
    echo 'incomplete ReliefMath resource product was accepted' >&2
    exit 1
fi
cmp "$work/resources.before" "$work/resources.plan"

python3 - "$src" <<'PY'
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

root = Path(sys.argv[1])
desktop = root / "userland/apps/reliefmath/reliefmath.desktop"
fields = dict(line.split("=", 1) for line in desktop.read_text(encoding="utf-8").splitlines()
              if "=" in line and not line.startswith("#"))
expected = {
    "Exec": "/usr/bin/reliefmath %f",
    "TryExec": "/usr/bin/reliefmath",
    "Icon": "reliefos-reliefmath",
    "StartupWMClass": "reliefos-reliefmath",
    "Categories": "Office;Math;",
    "MimeType": "application/x-reliefmath;",
}
for key, value in expected.items():
    if fields.get(key) != value:
        raise SystemExit(f"desktop field {key}: {fields.get(key)!r}, expected {value!r}")
mime_path = root / "system/rootfs/usr/share/mime/packages/reliefmath.xml"
if not mime_path.is_file():
    raise SystemExit(f"ReliefMath MIME package is missing from its specified source location: {mime_path}")
if (root / "userland/apps/reliefmath/reliefmath-mime.xml").exists():
    raise SystemExit("ReliefMath MIME package must not remain under the userland app source")
mime = ET.parse(mime_path).getroot()
entries = [node for node in mime.iter() if node.tag.endswith("mime-type")]
if not any(node.get("type") == "application/x-reliefmath" and
           any(glob.get("pattern") == "*.rmath" for glob in node if glob.tag.endswith("glob"))
           for node in entries):
    raise SystemExit("ReliefMath MIME package does not register *.rmath")
PY

for label in ReliefMath Formula Symbols Structure; do
    grep -Fq "N_(\"$label\")" "$src/configs/nls/messages.c"
    grep -Fxq "msgid \"$label\"" "$src/configs/nls/po/leonos.pot"
    grep -Fq "msgid \"$label\"" "$src/configs/nls/po/zh_CN.po"
done
for translated in 'msgstr "ReliefMath"' 'msgstr "公式"' 'msgstr "符号"' 'msgstr "结构"'; do
    grep -Fq "$translated" "$src/configs/nls/po/zh_CN.po"
done

# This fixture drives rootfs-stage.sh twice: with ReliefMath selected and
# without it. No product image is built.
TMPDIR="$tmp_base" sh "$src/tests/build/test-desktop-backend.sh"
echo 'ReliefMath integration contracts passed'
