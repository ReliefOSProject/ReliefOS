#!/bin/sh
# Verify an assembled Pages tree before it is published. Fails on the first
# problem so the workflow is atomic: no half site ever gets deployed (plan §44).
#
# Checks (plan §33, §34, §35, §36):
#   HTML   the six expected pages exist and no *.js file or inline script tag
#          appears anywhere in the tree.
#   CSS    the shared stylesheet is present at the site root and inside /rpr/.
#   RPR    every machine-interface file is present and packages.adb references
#          only APKs that actually exist under /rpr/apk/.
#   ISO    the installer ISO exists, is non-empty and matches the recorded
#          download/SHA256SUMS entry.
set -eu
[ "$#" = 1 ] || exit 2
site=$1
[ -d "$site" ] || { echo "not a Pages tree: $site" >&2; exit 1; }
verify_work=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-pages-check.XXXXXX")
trap 'rm -rf "$verify_work"' EXIT HUP INT TERM

# Every failing check appends to a list so a single run surfaces as much as
# possible; a non-empty list at the end is the failure signal.
fail=
report() { printf 'verify-pages: %s\n' "$1" >&2; fail="yes"; }

# --- expected files ---------------------------------------------------------
for f in index.html download/index.html download/reliefos-installer.iso download/SHA256SUMS \
         css/reliefos.css .nojekyll \
         rpr/index.html rpr/packages/index.html rpr/kernel/index.html \
         rpr/apk/index.html \
         rpr/manifest.json rpr/health.txt \
         rpr/css/reliefos.css \
         rpr/apk/packages.adb rpr/apk/repository.json rpr/apk/reliefos-rpr.rsa.pub rpr/apk/leonos-rpr.rsa.pub rpr/apk/SHA256SUMS \
         rpr/kernel/kernel.sys rpr/kernel/initramfs.cpio rpr/kernel/bundle.json \
         rpr/kernel/release.txt rpr/kernel/release.json rpr/kernel/SHA256SUMS; do
    [ -e "$site/$f" ] || report "missing $f"
done

# --- Documentation section --------------------------------------------------
# The manuals are rendered from each repository's Documentation/ tree, so the
# index and at least one rendered document must exist, and no raw source may
# leak into the published tree (only generated HTML and copied assets).
for manual in index.html reliefos/index.html reliefnt/index.html; do
    [ -s "$site/docs/$manual" ] || report "missing docs/$manual"
done
if [ -z "$(find "$site/docs" -name index.html 2>/dev/null | tail -n +2)" ]; then
    report "no rendered documentation pages under docs/"
fi
md_leak=$(find "$site/docs" -type f \( -name '*.md' -o -name '*.rst' -o -name '*.rst.txt' -o -name '*.pickle' \) 2>/dev/null || true)
[ -z "$md_leak" ] || report "raw source or doctrees leaked into published docs: $md_leak"

# --- JavaScript must be absent ----------------------------------------------
js=$(find "$site" -type f \( -name '*.js' -o -name '*.mjs' -o -name '*.cjs' \) -print 2>/dev/null || true)
[ -z "$js" ] || report "JavaScript files present: $js"
# Inline JS is only a real risk when an event-handler attribute (onX=...) sits
# inside an HTML tag. A bare '<script' or a 'javascript:' URL is always a
# problem, but the string 'online=2' in technical prose is not, so the event
# pattern is anchored to an open tag rather than to any whitespace-preceded
# 'on...' token.
scripts=$(grep -RInE -e '<script' -e 'javascript:' -e '<[a-zA-Z][^>]* on[a-z]+=' "$site" 2>/dev/null || true)
[ -z "$scripts" ] || report "inline JS markers present in Pages tree:
$scripts"

# --- private keys must never reach the public tree --------------------------
if grep -RIn -E -- '-----BEGIN (RSA |EC |OPENSSH )?PRIVATE KEY-----' "$site" >/dev/null 2>&1; then
    report "private key material detected in Pages tree"
fi

# --- RPR machine interface consistency --------------------------------------
# Every package referenced by repository.json must exist under rpr/apk/, and
# every APK file must be covered by apk/SHA256SUMS.
if [ -f "$site/rpr/apk/repository.json" ]; then
    awk -F '"' '
        $0 ~ /"packages"/ { inlist=1 }
        inlist { for (i=1;i<=NF;i++) if ($i ~ /^reliefos-.*\.apk$/) print $i }
    ' "$site/rpr/apk/repository.json" > "$verify_work/packages"
    while IFS= read -r pkg; do
        [ -n "$pkg" ] || continue
        [ -f "$site/rpr/apk/$pkg" ] || report "repository.json references missing APK $pkg"
    done < "$verify_work/packages"
fi

# Check the exact published cohort, not only the existence of protocol files.
for directory in apk kernel; do
    (cd "$site/rpr/$directory" && sha256sum -c SHA256SUMS >/dev/null 2>&1) \
        || report "rpr/$directory checksums do not match the published files"
done
release=$site/rpr/kernel/release.txt
if [ -f "$release" ]; then
    field() { sed -n "s/^$1=//p" "$release"; }
    if ! awk -F= '
        BEGIN { split("format_version image_version version kernel_release module_abi kernel_sha256 initramfs_sha256 package package_version package_file package_sha256 repository_sha256 bundle_manifest_sha256 module_directory", keys, " "); for (i in keys) allowed[keys[i]]=1 }
        NF!=2 || !allowed[$1] || seen[$1]++ || $2=="" { exit 1 }
        END { if (NR!=14) exit 1 }
    ' "$release" || [ "$(field format_version)" != 4 ]; then
        report 'invalid RPR format 4 release metadata'
    else
        image=$(field image_version)
        version=$(field version)
        kernel_release=$(field kernel_release)
        abi=$(field module_abi)
        kernel_hash=$(field kernel_sha256)
        initramfs_hash=$(field initramfs_sha256)
        bundle_hash=$(field bundle_manifest_sha256)
        package_version=$(field package_version)
        package_file=$(field package_file)
        package_hash=$(field package_sha256)
        index_hash=$(field repository_sha256)
        if [ "$image" != "$version" ] || [ "$(field package)" != reliefos-kernel ] ||
           [ "$package_file" != "reliefos-kernel-$package_version.apk" ] ||
           [ "$(field module_directory)" != "lib/modules/$kernel_release" ]; then
            report 'RPR release/package/module identities disagree'
        fi
        case $package_file in ''|*[!A-Za-z0-9._+-]*) report 'unsafe RPR kernel package filename'; package_file=invalid ;; esac
        for record in "$kernel_hash kernel/kernel.sys" "$initramfs_hash kernel/initramfs.cpio" \
                      "$bundle_hash kernel/bundle.json" "$package_hash apk/$package_file" "$index_hash apk/packages.adb"; do
            digest=${record%% *}
            member=${record#* }
            actual=$(sha256sum "$site/rpr/$member" 2>/dev/null) || actual=missing
            [ "${actual%% *}" = "$digest" ] || report "RPR metadata checksum mismatch: $member"
        done
        printf '{"architecture":"x86_64","schema":4,"image_version":"%s","version":"%s","kernel_release":"%s","module_abi":"%s","module_directory":"lib/modules/%s","files":{"kernel.sys":{"sha256":"%s"},"initramfs.cpio":{"sha256":"%s"},"bundle.json":{"sha256":"%s"}},"package":{"name":"reliefos-kernel","version":"%s","file":"%s","sha256":"%s"},"repository_sha256":"%s"}\n' \
            "$image" "$version" "$kernel_release" "$abi" "$kernel_release" "$kernel_hash" "$initramfs_hash" "$bundle_hash" \
            "$package_version" "$package_file" "$package_hash" "$index_hash" > "$verify_work/release.json"
        cmp -s "$verify_work/release.json" "$site/rpr/kernel/release.json" || report 'RPR release.json and release.txt disagree'
    fi
fi
if [ -f "$site/rpr/apk/SHA256SUMS" ]; then
    for apk in "$site"/rpr/apk/reliefos-*.apk; do
        [ -f "$apk" ] || continue
        name=${apk##*/}
        awk -v n="$name" '$2==n {found=1} END{exit !found}' "$site/rpr/apk/SHA256SUMS" \
            || report "$name not covered by rpr/apk/SHA256SUMS"
    done
fi

# --- ISO hash consistency ---------------------------------------------------
if [ -f "$site/download/SHA256SUMS" ] && [ -s "$site/download/reliefos-installer.iso" ]; then
    ( cd "$site/download" && sha256sum -c SHA256SUMS >/dev/null 2>&1 ) \
        || report "installer ISO does not match download/SHA256SUMS"
fi

# --- HTML links resolve to something inside the tree ------------------------
# All href values except external http(s) URLs must exist relative to the
# containing HTML file. This guards against a broken base path (plan §25).
linkfails=$verify_work/links
: > "$linkfails"
for page in $(find "$site" -name '*.html' -print); do
    dir=$(dirname "$page")
    grep -oE 'href="[^"#]+"' "$page" 2>/dev/null \
        | sed -e 's/^href="//' -e 's/"$//' \
        | while IFS= read -r href; do
            case "$href" in
                ''|\#*) continue ;;
                http://*|https://*|mailto:*) continue ;;
            esac
            target="${href%%#*}"
            target="${target%%\?*}"
            [ -n "$target" ] || continue
            [ -e "$dir/$target" ] || printf '%s: broken href %s\n' "${page#"$site"/}" "$href" >> "$linkfails"
        done
done
if [ -s "$linkfails" ]; then
    sort -u "$linkfails" | sed 's/^/verify-pages: /' >&2
    report "broken href detected"
fi

# --- summary ----------------------------------------------------------------
[ -z "$fail" ] || { echo "verify-pages: FAILED" >&2; exit 1; }
printf 'verify-pages: ok (%s)\n' "$(find "$site" -name '*.html' | wc -l) HTML pages, RPR machine interface preserved"
