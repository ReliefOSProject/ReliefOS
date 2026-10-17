#!/bin/sh
set -eu

src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

mkdir -p "$tmp/src/third_party/rime-pinyin-simp" \
    "$tmp/src/third_party/doomgeneric" "$tmp/src/userland/apps/oschinpt" \
    "$tmp/src/tools" "$tmp/src/resources/build-art/app-icons" \
    "$tmp/out/userland" "$tmp/fake-bin" "$tmp/repository"
printf 'dictionary\n' > "$tmp/src/third_party/rime-pinyin-simp/pinyin_simp.dict.yaml"
printf 'license\n' > "$tmp/src/third_party/rime-pinyin-simp/LICENSE"
printf 'attribution\n' > "$tmp/src/third_party/rime-pinyin-simp/ATTRIBUTION.txt"
printf 'settings\n' > "$tmp/src/userland/apps/oschinpt/settings.ini"
printf 'wad\n' > "$tmp/src/third_party/doomgeneric/freedoom1.wad"
printf 'license\n' > "$tmp/src/third_party/doomgeneric/LICENSE"
printf 'copying\n' > "$tmp/src/third_party/doomgeneric/FREEDOOM-COPYING.txt"
printf 'icon\n' > "$tmp/src/resources/build-art/app-icons/helloworld.bmp"
printf 'index\n' > "$tmp/index"
for app in helloworld doom doomlauncher oschinpt; do
    printf '%s\n' "$app" > "$tmp/out/userland/$app.elf"
done
cat > "$tmp/build_info.h" <<'EOF'
#define RELIEFOS_KERNEL_VERSION "4.7.1"
EOF
printf 'key\n' > "$tmp/key"
chmod 600 "$tmp/key"

# The fixture does not need a real APK encoder. It records each requested
# output path so the package naming contract can be checked without downloads.
cat > "$tmp/fake-bin/apk" <<'EOF'
#!/bin/sh
set -eu
output=
metadata=
files=
while [ "$#" -gt 0 ]; do
    case $1 in
        --output) output=$2; shift 2 ;;
        --files) files=$2; shift 2 ;;
        --info) metadata="$metadata\n$2"; shift 2 ;;
        *) shift ;;
    esac
done
[ -n "$output" ]
: > "$output"
printf '%b\n' "$metadata" > "$output.metadata"
if [ -n "$files" ]; then
    cp "$files"/usr/lib/reliefos/apps/*/manifest.ini "$output.app-manifest"
    find "$files" -name '*.bmp' > "$output.icons"
fi
EOF
chmod 755 "$tmp/fake-bin/apk"

sh "$src/tools/build/rpr-apps.sh" "$tmp/src" "$tmp/out" "$tmp/build_info.h" \
    "$tmp/index" "$tmp/fake-bin/apk" "$tmp/key" "$tmp/repository" 123

for app in helloworld doom oschinpt; do
    package="$tmp/repository/reliefos-$app-4.7.1-r123.apk"
    test -f "$package"
    test ! -e "$tmp/repository/reliefos-$app.apk"
    grep -Fx "name:reliefos-$app" "$package.metadata"
    grep -Fx "origin:reliefos-$app" "$package.metadata"
    grep -F 'depends:reliefos-apps reliefos-musl' "$package.metadata"
    grep -Fx "replaces:leonos-$app" "$package.metadata"
done
grep -qx 'icon=' "$tmp/repository/reliefos-doom-4.7.1-r123.apk.app-manifest"
test ! -s "$tmp/repository/reliefos-doom-4.7.1-r123.apk.icons"
grep -qx 'icon=helloworld.bmp' "$tmp/repository/reliefos-helloworld-4.7.1-r123.apk.app-manifest"
printf '%s\n' \
    reliefos-helloworld-4.7.1-r123.apk \
    reliefos-doom-4.7.1-r123.apk \
    reliefos-oschinpt-4.7.1-r123.apk > "$tmp/expected.list"
cmp "$tmp/expected.list" "$tmp/repository/packages.list"
test "$(cat "$tmp/repository/.complete")" = 4.7.1

# The Pages assembler must reject a versionless package before publication;
# this protects every package source, including future RPR applications.
mkdir -p "$tmp/pages-repository" "$tmp/pages-apps"
printf package > "$tmp/pages-repository/reliefos-base-1-r0.apk"
printf package > "$tmp/pages-apps/reliefos-broken.apk"
printf reliefos-broken.apk > "$tmp/pages-apps/packages.list"
printf kernel > "$tmp/kernel.sys"
kernel_hash=$(sha256sum "$tmp/kernel.sys" | cut -d' ' -f1)
mkdir -p "$tmp/pages-kernel"
printf 'format_version: 1\narch: x86_64\nartifacts:\n  %s  kernel.sys\n' \
    "$kernel_hash" > "$tmp/pages-kernel/manifest.txt"
printf 'kernel_name=ntclks\nrelease_version=4.7.1\n' > "$tmp/pages-version"
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 \
    -out "$tmp/pages-key" 2>/dev/null
chmod 600 "$tmp/pages-key"
if sh "$src/tools/build/rpr-pages.sh" "$tmp/pages-repository" "$tmp/pages-apps" \
    "$tmp/kernel.sys" "$tmp/pages-kernel/manifest.txt" \
    "$tmp/pages-version" "$tmp/fake-bin/apk" "$tmp/pages-key" "$tmp/pages-output" \
    2>"$tmp/pages-error"; then
    echo 'versionless RPR package was accepted' >&2
    exit 1
fi
grep -q 'missing its version' "$tmp/pages-error"
test ! -e "$tmp/pages-output"

# The old RPR public-key URL remains byte-identical for installed clients; the
# canonical filename points at the same key during the package transition.
rm "$tmp/pages-apps/reliefos-broken.apk" "$tmp/pages-apps/packages.list"
printf 'app package\n' > "$tmp/pages-apps/reliefos-helloworld-1-r0.apk"
sh "$src/tools/build/rpr-pages.sh" "$tmp/pages-repository" "$tmp/pages-apps" \
    "$tmp/kernel.sys" "$tmp/pages-kernel/manifest.txt" \
    "$tmp/pages-version" "$tmp/fake-bin/apk" "$tmp/pages-key" "$tmp/pages-output"
cmp "$tmp/pages-output/apk/leonos-rpr.rsa.pub" "$tmp/pages-output/apk/reliefos-rpr.rsa.pub"
grep -F '"public_key":"reliefos-rpr.rsa.pub"' "$tmp/pages-output/apk/repository.json"
grep -F '"legacy_public_key":"leonos-rpr.rsa.pub"' "$tmp/pages-output/apk/repository.json"
test -f "$tmp/pages-output/apk/reliefos-base-1-r0.apk"
printf 'CONFIG_RPR_BASE_URL="https://reliefosproject.github.io/ReliefOS/rpr"\n' > "$tmp/rpr.conf"
sh "$src/tools/build/rpr-config.sh" "$tmp/rpr.conf" > "$tmp/rpr.env"
grep -Fx 'RPR_PUBLIC_KEY=reliefos-rpr.rsa.pub' "$tmp/rpr.env"
grep -Fx 'RPR_BASE_URL=https://reliefosproject.github.io/ReliefOS/rpr' "$tmp/rpr.env"

printf 'RPR package names, dual key trust and publication validation passed\n'
