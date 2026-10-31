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
[ ! -L "$key" ] && [ -f "$key" ] && [ "$(stat -c %a "$key")" = 600 ] || { echo 'private signing key must be a regular 0600 file' >&2; exit 1; }
version=$(sed -n 's/^release_version=//p' "$version_src" | head -n1)
printf '%s\n' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || { echo 'invalid release version' >&2; exit 1; }
image=$version

# Generators share escaping, page chrome and size formatting from one file so
# the RPR and download/home pages stay visually identical and no logic is
# duplicated across shell scripts (plan §19, §20).
# shellcheck source=/dev/null
. "$(dirname "$0")/site-common.sh"

mkdir -p "$(dirname "$output")"
work=$(mktemp -d "$output.new.XXXXXX")
work=$(CDPATH= cd -- "$work" && pwd -P)
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/site/apk" "$work/site/kernel" "$work/site/packages" "$work/site/css"

# --- signed package repository, including one complete current kernel ------
openssl pkey -in "$key" -pubout -out "$work/site/apk/reliefos-rpr.rsa.pub" 2>/dev/null
cp "$work/site/apk/reliefos-rpr.rsa.pub" "$work/site/apk/leonos-rpr.rsa.pub"
chmod 644 "$work/site/apk/reliefos-rpr.rsa.pub" "$work/site/apk/leonos-rpr.rsa.pub"
for source in "$repository"/reliefos-*.apk "$apps"/reliefos-*.apk; do
    [ -f "$source" ] || { echo "missing RPR package $source" >&2; exit 1; }
    name=${source##*/}
    case $name in *[!a-zA-Z0-9._+-]*) echo 'unsafe RPR filename' >&2; exit 1 ;; esac
    # apk resolves a package from its metadata as name-version.apk. A
    # versionless archive indexes successfully but every normal client then
    # requests a URL that does not exist.
    printf '%s\n' "$name" | grep -Eq '^reliefos-.+-[0-9][A-Za-z0-9._+-]*\.apk$' || {
        echo "RPR package filename is missing its version: $name" >&2
        exit 1
    }
    [ ! -e "$work/site/apk/$name" ] || { echo "duplicate RPR package $name" >&2; exit 1; }
    cp "$source" "$work/site/apk/$name"
done
"$apk" mkndx --keys-dir "$work/site/apk" --sign-key "$key" --output "$work/site/apk/packages.adb" "$work/site/apk"/*.apk
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

# --- human interface: RPR home ----------------------------------------------
# This subtree is designed to be published inside a Pages tree at /rpr/ (see
# tools/build/site.sh). The site-common helpers emit a nav whose "Home" and
# "Download" links point back to the enclosing site root, so every page here
# uses depth = its own level below /rpr/ plus 1 for the /rpr/ prefix itself.
# The standalone subtree serves the machine interface and RPR-internal links.
# Site-level navigation and documentation links require the enclosing Pages
# tree; use `make pages` to preview the complete human-facing website.
{
    site_page_begin "$(site_css_href 1)" "$(site_home_href 1)" \
        "ReliefOS RPR" "ReliefOS Remote Package Repository"
    printf '<p class="subtitle">Version %s &middot; architecture x86_64</p>\n' "$(printf '%s' "$version" | site_html_escape)"
    cat <<'HTML'
<section>
<h2>Packages</h2>
<p>Browse the signed APK packages published in this repository. The package
list is generated by the ReliefOS build system from the real repository contents.</p>
<p><a class="button" href="packages/index.html">Browse packages</a>
<a href="apk/index.html">APK repository</a></p>
</section>
<section>
<h2>Kernel</h2>
<p>Matching kernel, initramfs and module inventory, delivered by a signed APK package.</p>
<p><a href="kernel/index.html">Kernel information</a></p>
</section>
<section>
<h2>Documentation</h2>
<p>Separate manuals, maintained and built from each product's own repository.</p>
<p><a href="../docs/reliefos/index.html">ReliefOS system and application manuals</a><br>
<a href="../docs/reliefnt/index.html">ReliefNT kernel manuals</a></p>
</section>
<section>
<h2>Repository metadata</h2>
<p>Machine-readable endpoints consumed by the ReliefOS package tools. Do not
edit; these are protocol files.</p>
<ul>
<li><a href="apk/packages.adb">apk/packages.adb</a> (signed index)</li>
<li><a href="apk/repository.json">apk/repository.json</a></li>
<li><a href="apk/reliefos-rpr.rsa.pub">apk/reliefos-rpr.rsa.pub</a></li>
<li><a href="apk/leonos-rpr.rsa.pub">apk/leonos-rpr.rsa.pub</a> (legacy alias)</li>
<li><a href="kernel/release.json">kernel/release.json</a></li>
<li><a href="kernel/release.txt">kernel/release.txt</a></li>
<li><a href="manifest.json">manifest.json</a></li>
<li><a href="health.txt">health.txt</a></li>
</ul>
</section>
HTML
    site_page_end
} > "$work/site/index.html"

# --- APK directory listing --------------------------------------------------
{
    site_page_begin "$(site_css_href 2)" "$(site_home_href 2)" \
        "ReliefOS APK repository" "ReliefOS APK Repository"
    cat <<'HTML'
<section>
<h2>Downloadable files</h2>
<p>Signed package index, signing public key and checksums. This directory is
the APK repository root referenced by <code>/etc/apk/repositories</code>.</p>
<ul>
HTML
    for file in "$work/site/apk"/*.apk "$work/site/apk"/packages.adb "$work/site/apk"/reliefos-rpr.rsa.pub "$work/site/apk"/leonos-rpr.rsa.pub "$work/site/apk"/repository.json "$work/site/apk"/SHA256SUMS; do
        [ -f "$file" ] || continue
        printf '<li><a href="%s">%s</a></li>\n' "${file##*/}" "$(printf '%s' "${file##*/}" | site_html_escape)"
    done
    cat <<'HTML'
</ul>
</section>
HTML
    site_page_end
} > "$work/site/apk/index.html"

# --- package list + per-package pages (depth 2 for detail pages) -----------
# --- package list + per-package pages ---------------------------------------
{
    site_page_begin "$(site_css_href 2)" "$(site_home_href 2)" \
        "ReliefOS packages" "ReliefOS Packages"
    printf '<p class="subtitle">version %s &middot; x86_64</p>\n' \
        "$(printf '%s' "$version" | site_html_escape)"
    cat <<'HTML'
<section>
<h2>Available packages</h2>
<table>
<caption>APK packages published in this release</caption>
<tr><th>Package</th><th>Version</th><th>Architecture</th><th>Size</th></tr>
HTML
    for file in "$work/site/apk"/reliefos-*.apk; do
        [ -f "$file" ] || continue
        fname=${file##*/}
        base=${fname%.apk}                 # reliefos-<name>-<version>-r<epoch>
        stem=${base%%-[0-9]*}              # reliefos-<name>: up to the first -<digit>
        ver=${base#"$stem"-}               # <version>-r<epoch>
        size=$(stat -c %s "$file")
        esc_name=$(printf '%s' "$stem" | site_html_escape)
        esc_ver=$(printf '%s' "$ver" | site_html_escape)
        printf '<tr><td><a href="%s.html">%s</a></td><td>%s</td><td>x86_64</td><td>%s</td></tr>\n' \
            "$esc_name" "$esc_name" "$esc_ver" "$(site_bytes_human "$size")"
    done
    cat <<'HTML'
</table>
</section>
HTML
    site_page_end
} > "$work/site/packages/index.html"

# Per-package information pages: data is parsed from the validated APK filename
# plus real file size and the recorded SHA256SUMS hash, so it can never drift
# from what is actually published (plan §17, §21).
for file in "$work/site/apk"/reliefos-*.apk; do
    [ -f "$file" ] || continue
    fname=${file##*/}
    base=${fname%.apk}
    stem=${base%%-[0-9]*}
    ver=${base#"$stem"-}
    size=$(stat -c %s "$file")
    hash=$(site_hash_for "$work/site/apk/SHA256SUMS" "$fname")
    esc_stem=$(printf '%s' "$stem" | site_html_escape)
    {
        site_page_begin "$(site_css_href 2)" "$(site_home_href 2)" \
            "$stem $ver" "Package: $esc_stem"
        printf '<p class="subtitle"><a href="../index.html">RPR</a> &rsaquo; <a href="index.html">Packages</a> &rsaquo; %s</p>\n' "$esc_stem"
        printf '<section>\n<h2>%s</h2>\n' "$esc_stem"
        printf '<dl class="meta">\n'
        printf '<dt>Package</dt><dd>%s</dd>\n' "$esc_stem"
        printf '<dt>Version</dt><dd>%s</dd>\n' "$(printf '%s' "$ver" | site_html_escape)"
        printf '<dt>Architecture</dt><dd>x86_64</dd>\n'
        printf '<dt>Size</dt><dd>%s</dd>\n' "$(site_bytes_human "$size")"
        printf '<dt>SHA256</dt><dd class="hash">%s</dd>\n' "$(printf '%s' "$hash" | site_html_escape)"
        printf '<div class="clear"></div>\n'
        printf '</dl>\n'
        printf '<p><a class="button" href="../apk/%s">Download .apk</a></p>\n' "$fname"
        printf '<p><a href="index.html">Back to package list</a></p>\n'
        printf '</section>\n'
        site_page_end
    } > "$work/site/packages/$stem.html"
done

# --- kernel page (depth 1) --------------------------------------------------
{
    site_page_begin "$(site_css_href 2)" "$(site_home_href 2)" \
        "ReliefNT Kernel" "ReliefNT Kernel"
    printf '<p class="subtitle">Version %s &middot; architecture x86_64</p>\n' "$(printf '%s' "$version" | site_html_escape)"
    cat <<HTML
<section>
<h2>Kernel files</h2>
<dl class="meta">
<dt>Kernel release</dt><dd>$(printf '%s' "$kernel_release" | site_html_escape)</dd>
<dt>Module ABI</dt><dd class="hash">$(printf '%s' "$module_abi" | site_html_escape)</dd>
<dt>kernel.sys</dt><dd class="hash">$(printf '%s' "$kernel_hash" | site_html_escape)</dd>
<dt>initramfs.cpio</dt><dd class="hash">$(printf '%s' "$initramfs_hash" | site_html_escape)</dd>
<div class="clear"></div>
</dl>
<p>
<a class="button" href="../apk/$package_file">Download signed kernel package</a>
<a href="kernel.sys">kernel.sys</a>
<a href="initramfs.cpio">initramfs.cpio</a>
<a href="bundle.json">Module and file inventory</a>
<a href="../../docs/reliefnt/index.html">ReliefNT kernel documentation</a>
</p>
</section>
<section>
<h2>Machine-readable release metadata</h2>
<ul>
<li><a href="release.txt">release.txt</a> (strict client protocol)</li>
<li><a href="release.json">release.json</a></li>
<li><a href="SHA256SUMS">SHA256SUMS</a></li>
</ul>
</section>
HTML
    site_page_end
} > "$work/site/kernel/index.html"

: > "$work/site/.nojekyll"

# --- atomic publish (unchanged semantics) ----------------------------------
if [ -d "$output.previous" ] && [ ! -e "$output" ]; then mv "$output.previous" "$output"; fi
rm -rf "$output.previous"
if [ -e "$output" ]; then mv "$output" "$output.previous"; fi
mv "$work/site" "$output"
rm -rf "$output.previous"
