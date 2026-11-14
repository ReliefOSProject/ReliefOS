#!/bin/sh
# Assemble a local Pages tree only. Publication is a separate user/CI action.
#
# The RPR machine interface keeps its existing URLs and health token. The
# ReliefOS public-key filename is canonical while the old key path remains an
# identical trust alias for clients already configured with it. On top of that
# interface this script now also emits a small human-readable static site
# (index.html plus /packages/ and its per-package pages). Every page is plain
# HTML with a single shared stylesheet and no JavaScript (plan §12, §22).
#
# All links in the emitted HTML are relative. The tree is designed so that this
# whole directory can be dropped into a larger Pages tree as /rpr/ and every
# <a href> / <link href> still resolves, because the base path is never baked
# into the output (plan §25).
set -eu
[ "$#" = 8 ] || exit 2
repository=$1 apps=$2 kernel=$3 payload=$4 version_src=$5 apk=$6 key=$7 output=$8
: "${RELIEFOS_KERNEL_TOOLS:?set the native Kbuild kernel tool}"
: "${RELIEFOS_KERNEL_RECOVERY:?set the static Kbuild bundle verifier}"
# Kbuild passes paths relative to its output directory; the runtime verifier
# requires a canonical absolute staging root. Refuse a symlinked final
# component before resolving references to a parent directory.
case "$payload" in
    /*) ;;
    ?*)
        [ ! -L "$payload" ] || { echo 'symlinked RPR kernel payload root' >&2; exit 1; }
        payload=$(CDPATH= cd -- "$payload" && pwd -P)
        ;;
esac
[ ! -L "$key" ] && [ -f "$key" ] && [ "$(stat -c %a "$key")" = 600 ] || { echo 'private signing key must be a regular 0600 file' >&2; exit 1; }
version=$(sed -n 's/^release_version=//p' "$version_src" | head -n1)
printf '%s\n' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || { echo 'invalid release version' >&2; exit 1; }
image=$version

mkdir -p "$(dirname "$output")"
work=$(mktemp -d "$output.new.XXXXXX")
work=$(CDPATH= cd -- "$work" && pwd -P)
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/site/apk" "$work/site/kernel" "$work/site/packages" "$work/site/css"

# --- signed package repository, including one complete current kernel ------
openssl pkey -in "$key" -pubout -out "$work/site/apk/reliefos-rpr.rsa.pub" 2>/dev/null
cp "$work/site/apk/reliefos-rpr.rsa.pub" "$work/site/apk/leonos-rpr.rsa.pub"
chmod 644 "$work/site/apk/reliefos-rpr.rsa.pub" "$work/site/apk/leonos-rpr.rsa.pub"
# Indexing verifies the original signatures too: trust both the local signing
# key and the pinned Alpine keys, without re-signing upstream packages.
mkdir "$work/keys"
source_root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
cp "$source_root"/system/rootfs/etc/apk/keys/*.pub "$work/keys/"
cp "$work/site/apk/reliefos-rpr.rsa.pub" "$work/keys/"
# Include the complete verified dependency closure; official APK bytes and
# signatures are preserved when publishing the local RPR tree.
for source in "$repository"/*.apk "$apps"/reliefos-*.apk; do
    [ -f "$source" ] || { echo "missing RPR package $source" >&2; exit 1; }
    name=${source##*/}
    case $name in *[!a-zA-Z0-9._+-]*) echo 'unsafe RPR filename' >&2; exit 1 ;; esac
    # apk resolves a package from its metadata as name-version.apk. A
    # versionless archive indexes successfully but every normal client then
    # requests a URL that does not exist.
    printf '%s\n' "$name" | grep -Eq '^[A-Za-z0-9][A-Za-z0-9+_.-]*-[0-9][A-Za-z0-9._+-]*\.apk$' || {
        echo "RPR package filename is missing its version: $name" >&2
        exit 1
    }
    [ ! -e "$work/site/apk/$name" ] || { echo "duplicate RPR package $name" >&2; exit 1; }
    cp "$source" "$work/site/apk/$name"
done
"$apk" mkndx --keys-dir "$work/keys" --sign-key "$key" --output "$work/site/apk/packages.adb" "$work/site/apk"/*.apk
kernel_package=
for file in "$work/site/apk"/reliefos-kernel-[0-9]*.apk; do
    [ -f "$file" ] || continue
    [ -z "$kernel_package" ] || { echo 'RPR must contain exactly one current kernel package' >&2; exit 1; }
    kernel_package=$file
done
[ -n "$kernel_package" ] || { echo 'RPR is missing its complete signed kernel package' >&2; exit 1; }
"$RELIEFOS_KERNEL_TOOLS" release-info "$payload" "$kernel" "$kernel_package" \
    "$work/site/apk/packages.adb" "$image" "$RELIEFOS_KERNEL_RECOVERY" > "$work/site/kernel/release.txt"
release_field() { sed -n "s/^$1=//p" "$work/site/kernel/release.txt"; }
kernel_release=$(release_field kernel_release)
module_abi=$(release_field module_abi)
kernel_hash=$(release_field kernel_sha256)
initramfs_hash=$(release_field initramfs_sha256)
package_version=$(release_field package_version)
package_file=$(release_field package_file)
package_hash=$(release_field package_sha256)
repository_hash=$(release_field repository_sha256)
bundle_hash=$(release_field bundle_manifest_sha256)
# Bind the description to the actual signed APK, not just to a separately valid
# build directory. extract never executes package scripts or edits an APK DB.
mkdir "$work/kernel-check"
"$apk" --keys-dir "$work/site/apk" extract --no-chown --destination "$work/kernel-check" "$kernel_package"
cmp -s "$payload/kernel-payload.json" "$work/kernel-check/usr/lib/reliefos/kernel/$kernel_release/bundle.json" || {
    echo 'RPR signed kernel package and described payload disagree' >&2; exit 1;
}
"$RELIEFOS_KERNEL_RECOVERY" --inspect "$work/kernel-check" "$kernel_release" > "$work/kernel-actual"
printf 'kernel_release=%s\nmodule_abi=%s\nkernel_sha256=%s\ninitramfs_sha256=%s\n' \
    "$kernel_release" "$module_abi" "$kernel_hash" "$initramfs_hash" > "$work/kernel-expected"
cmp -s "$work/kernel-expected" "$work/kernel-actual" || { echo 'RPR APK boot identity mismatch' >&2; exit 1; }
(cd "$work/site/apk" && sha256sum ./*.apk leonos-rpr.rsa.pub reliefos-rpr.rsa.pub packages.adb | sed 's|  ./|  |' | LC_ALL=C sort -k2 > SHA256SUMS)
{
    printf '{"architecture":"x86_64","index":"packages.adb","public_key":"reliefos-rpr.rsa.pub","legacy_public_key":"leonos-rpr.rsa.pub","schema":1,"packages":['
    comma=
    for file in "$work/site/apk"/*.apk; do printf '%s"%s"' "$comma" "${file##*/}"; comma=,; done
    printf ']}\n'
} > "$work/site/apk/repository.json"
cp "$kernel" "$work/site/kernel/kernel.sys"
cp "$payload/usr/lib/reliefos/kernel/$kernel_release/initramfs.cpio" "$work/site/kernel/initramfs.cpio"
cp "$payload/kernel-payload.json" "$work/site/kernel/bundle.json"
printf '%s  kernel.sys\n%s  initramfs.cpio\n%s  bundle.json\n' \
    "$kernel_hash" "$initramfs_hash" "$bundle_hash" > "$work/site/kernel/SHA256SUMS"
printf '{"architecture":"x86_64","schema":4,"image_version":"%s","version":"%s","kernel_release":"%s","module_abi":"%s","module_directory":"lib/modules/%s","files":{"kernel.sys":{"sha256":"%s"},"initramfs.cpio":{"sha256":"%s"},"bundle.json":{"sha256":"%s"}},"package":{"name":"reliefos-kernel","version":"%s","file":"%s","sha256":"%s"},"repository_sha256":"%s"}\n' \
    "$image" "$version" "$kernel_release" "$module_abi" "$kernel_release" "$kernel_hash" "$initramfs_hash" "$bundle_hash" \
    "$package_version" "$package_file" "$package_hash" "$repository_hash" > "$work/site/kernel/release.json"
printf '{"apk":"/apk/packages.adb","kernel":"/kernel/release.txt","schema":1,"version":"%s"}\n' "$version" > "$work/site/manifest.json"
printf 'leonos-rpr-ok\n' > "$work/site/health.txt"
printf '%s\n' "$version" > "$work/site/.complete"

# --- shared stylesheet (static source copied verbatim, never generated) -----
cp "$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)/resources/pages/css/reliefos.css" "$work/site/css/reliefos.css"
chmod 644 "$work/site/css/reliefos.css"

# Human HTML is regenerated from these finished artifacts without signing.
sh "$(dirname "$0")/rpr-human-pages.sh" "$work/site" "$work/site" --rpr-only

: > "$work/site/.nojekyll"

# --- atomic publish (unchanged semantics) ----------------------------------
if [ -d "$output.previous" ] && [ ! -e "$output" ]; then mv "$output.previous" "$output"; fi
rm -rf "$output.previous"
if [ -e "$output" ]; then mv "$output" "$output.previous"; fi
mv "$work/site" "$output"
rm -rf "$output.previous"
