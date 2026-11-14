#!/bin/sh
# Render HTML from a finished RPR tree, without signing or changing machine data.
# Usage: rpr-human-pages.sh RPR_TREE SITE_ROOT [--rpr-only]
# --rpr-only writes the historical Chinese HTML into an RPR producer's tree.
# The full site always regenerates all three languages, even from an older RPR.
set -eu
case $# in 2|3) ;; *) exit 2 ;; esac
rpr=$1 output=$2
case ${3:-} in
    '') languages='zh-CN en ja'; rpr_only=no ;;
    --rpr-only) languages=zh-CN; rpr_only=yes ;;
    *) exit 2 ;;
esac
# shellcheck source=/dev/null
. "$(dirname "$0")/site-common.sh"
site_language=zh-CN
[ -s "$rpr/kernel/release.txt" ] && [ -f "$rpr/apk/SHA256SUMS" ] || {
    echo 'missing finished RPR metadata' >&2; exit 1;
}
rpr_field() { sed -n "s/^$1=//p" "$rpr/kernel/release.txt"; }
version=$(rpr_field version)
printf '%s\n' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || exit 1
kernel_release=$(rpr_field kernel_release)
module_abi=$(rpr_field module_abi)
kernel_hash=$(rpr_field kernel_sha256)
initramfs_hash=$(rpr_field initramfs_sha256)
package_file=$(rpr_field package_file)
case $package_file in ''|*[!A-Za-z0-9._+-]*) echo 'unsafe kernel package filename' >&2; exit 1 ;; esac

rpr_destination() (
    if [ "$rpr_only" = yes ]; then rpr_page=$output/${1#rpr/}
    else rpr_page=$output/$(site_page_path "$site_language" "$1"); fi
    mkdir -p "$(dirname "$rpr_page")"
    printf '%s\n' "$rpr_page"
)
rpr_subtitle() {
    printf '<p class="subtitle">%s %s &middot; %s x86_64</p>\n' \
        "$(site_text version)" "$(printf '%s' "$version" | site_html_escape)" "$(site_text architecture)"
}
rpr_meta() {
    printf '<dt>%s</dt><dd>%s</dd>\n' "$(site_text "$1")" "$(printf '%s' "$2" | site_html_escape)"
}

for language in $languages; do
    site_use_language "$language"
    {
        site_page_begin rpr/index.html "$(site_message rpr_title)" "$(site_message rpr_title)"
        rpr_subtitle
        cat <<HTML
<section>
<h2>$(site_text packages)</h2>
<p>$(site_text packages_description)</p>
<p><a class="button" href="$(site_ui_href rpr/packages/index.html)">$(site_text browse_packages)</a>
<a href="$(site_ui_href rpr/apk/index.html)">$(site_text apk_repository)</a></p>
</section>
<section>
<h2>$(site_text kernel)</h2>
<p>$(site_text kernel_description)</p>
<p><a href="$(site_ui_href rpr/kernel/index.html)">$(site_text kernel_information)</a></p>
</section>
<section>
<h2>$(site_text docs)</h2>
<p>$(site_text manuals_description)</p>
<p><a href="$(site_docs_href reliefos)">$(site_text os_manual)</a><br>
<a href="$(site_docs_href reliefnt)">$(site_text nt_manual)</a></p>
</section>
<section>
<h2>$(site_text repository_metadata)</h2>
<p>$(site_text metadata_description)</p>
<ul>
HTML
        for member in apk/packages.adb apk/repository.json apk/reliefos-rpr.rsa.pub apk/leonos-rpr.rsa.pub \
                      kernel/release.json kernel/release.txt manifest.json health.txt; do
            annotation=
            case $member in
                apk/packages.adb) annotation=" ($(site_text signed_index))" ;;
                apk/leonos-rpr.rsa.pub) annotation=" ($(site_text legacy_alias))" ;;
            esac
            printf '<li><a href="%s">%s</a>%s</li>\n' "$(site_href "rpr/$member")" "$member" "$annotation"
        done
        printf '</ul>\n</section>\n'
        site_page_end
    } > "$(rpr_destination rpr/index.html)"

    {
        site_page_begin rpr/apk/index.html "$(site_message apk_title)" "$(site_message apk_title)"
        cat <<HTML
<section>
<h2>$(site_text downloadable_files)</h2>
<p>$(site_text apk_description) <code>/etc/apk/repositories</code>.</p>
<ul>
HTML
        for file in "$rpr"/apk/*.apk "$rpr"/apk/packages.adb "$rpr"/apk/reliefos-rpr.rsa.pub \
                    "$rpr"/apk/leonos-rpr.rsa.pub "$rpr"/apk/repository.json "$rpr"/apk/SHA256SUMS; do
            [ -f "$file" ] || continue
            printf '<li><a href="%s">%s</a></li>\n' \
                "$(site_href "rpr/apk/${file##*/}")" "$(printf '%s' "${file##*/}" | site_html_escape)"
        done
        printf '</ul>\n</section>\n'
        site_page_end
    } > "$(rpr_destination rpr/apk/index.html)"

    {
        site_page_begin rpr/packages/index.html "$(site_message packages_title)" "$(site_message packages_title)"
        rpr_subtitle
        cat <<HTML
<section>
<h2>$(site_text available_packages)</h2>
<table>
<caption>$(site_text packages_caption)</caption>
<tr><th>$(site_text package)</th><th>$(site_text version)</th><th>$(site_text architecture)</th><th>$(site_text size)</th></tr>
HTML
        for file in "$rpr"/apk/*.apk; do
            [ -f "$file" ] || continue
            fname=${file##*/}
            base=${fname%.apk}
            without_revision=${base%-*}
            stem=${without_revision%-*}
            ver=${base#"$stem"-}
            size=$(stat -c %s "$file")
            printf '<tr><td><a href="%s">%s</a></td><td>%s</td><td>x86_64</td><td>%s</td></tr>\n' \
                "$(site_ui_href "rpr/packages/$stem.html")" "$(printf '%s' "$stem" | site_html_escape)" \
                "$(printf '%s' "$ver" | site_html_escape)" "$(site_bytes_human "$size")"
        done
        printf '</table>\n</section>\n'
        site_page_end
    } > "$(rpr_destination rpr/packages/index.html)"

    for file in "$rpr"/apk/*.apk; do
        [ -f "$file" ] || continue
        fname=${file##*/}
        # Preserve names containing digits and dots, e.g. libatk-1.0.
        base=${fname%.apk}
        without_revision=${base%-*}
        stem=${without_revision%-*}
        ver=${base#"$stem"-}
        size=$(stat -c %s "$file")
        hash=$(site_hash_for "$rpr/apk/SHA256SUMS" "$fname")
        {
            site_page_begin "rpr/packages/$stem.html" "$stem $ver" "$(site_message package): $stem"
            printf '<p class="breadcrumb"><a href="%s">RPR</a> &rsaquo; <a href="%s">%s</a> &rsaquo; %s</p>\n' \
                "$(site_ui_href rpr/index.html)" "$(site_ui_href rpr/packages/index.html)" \
                "$(site_text packages)" "$(printf '%s' "$stem" | site_html_escape)"
            printf '<section>\n<h2>%s</h2>\n<dl class="meta">\n' "$(printf '%s' "$stem" | site_html_escape)"
            rpr_meta package "$stem"
            rpr_meta version "$ver"
            rpr_meta architecture x86_64
            rpr_meta size "$(site_bytes_human "$size")"
            printf '<dt>SHA256</dt><dd class="hash">%s</dd>\n</dl>\n' "$(printf '%s' "$hash" | site_html_escape)"
            printf '<p><a class="button" href="%s">%s</a></p>\n' "$(site_href "rpr/apk/$fname")" "$(site_text download_apk)"
            printf '<p><a href="%s">%s</a></p>\n</section>\n' "$(site_ui_href rpr/packages/index.html)" "$(site_text back_packages)"
            site_page_end
        } > "$(rpr_destination "rpr/packages/$stem.html")"
    done

    {
        site_page_begin rpr/kernel/index.html "$(site_message kernel_title)" "$(site_message kernel_title)"
        rpr_subtitle
        printf '<section>\n<h2>%s</h2>\n<dl class="meta">\n' "$(site_text kernel_files)"
        rpr_meta kernel_release "$kernel_release"
        rpr_meta module_abi "$module_abi"
        printf '<dt>kernel.sys</dt><dd class="hash">%s</dd>\n' "$(printf '%s' "$kernel_hash" | site_html_escape)"
        printf '<dt>initramfs.cpio</dt><dd class="hash">%s</dd>\n</dl>\n' "$(printf '%s' "$initramfs_hash" | site_html_escape)"
        cat <<HTML
<p>
<a class="button" href="$(site_href "rpr/apk/$package_file")">$(site_text download_kernel)</a>
<a href="$(site_href rpr/kernel/kernel.sys)">kernel.sys</a>
<a href="$(site_href rpr/kernel/initramfs.cpio)">initramfs.cpio</a>
<a href="$(site_href rpr/kernel/bundle.json)">$(site_text module_inventory)</a>
<a href="$(site_docs_href reliefnt)">$(site_text nt_manual)</a>
</p>
</section>
<section>
<h2>$(site_text release_metadata)</h2>
<ul>
<li><a href="$(site_href rpr/kernel/release.txt)">release.txt</a> ($(site_text strict_protocol))</li>
<li><a href="$(site_href rpr/kernel/release.json)">release.json</a></li>
<li><a href="$(site_href rpr/kernel/SHA256SUMS)">SHA256SUMS</a></li>
</ul>
</section>
HTML
        site_page_end
    } > "$(rpr_destination rpr/kernel/index.html)"
done
