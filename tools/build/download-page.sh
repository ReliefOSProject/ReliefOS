#!/bin/sh
# Generate the /download/ page and its SHA256SUMS from the real installer ISO.
#
# The page is a static build artifact: the ISO hash, size and version are
# computed here from the file that will actually be published, so what the page
# shows can never disagree with the downloadable file (plan §28, §46.B). The
# ISO download link is a bare relative "reliefos-installer.iso", so it works at
# whatever base path Pages serves the site from (plan §25).
set -eu
[ "$#" = 3 ] || exit 2
iso=$1 build=$2 output=$3
# output is the site root; the download page lives in <output>/download.
site=$output
[ -s "$iso" ] || { echo "missing installer ISO $iso" >&2; exit 1; }
[ "${iso##*/}" = reliefos-installer.iso ] || {
    echo "installer ISO must be named reliefos-installer.iso: $iso" >&2
    exit 1
}
version=$(sed -n 's/^#define RELIEFOS_KERNEL_VERSION "\([0-9.]*\)"$/\1/p' "$build")
printf '%s\n' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || { echo 'invalid release version' >&2; exit 1; }
# shellcheck source=/dev/null
. "$(dirname "$0")/site-common.sh"

mkdir -p "$site/download"
name=${iso##*/}
size=$(stat -c %s "$iso")
iso_hash=$(sha256sum "$iso" | cut -d' ' -f1)
# SHA256SUMS lists the bare filename so `sha256sum -c` runs inside download/.
printf '%s  %s\n' "$iso_hash" "$name" > "$site/download/SHA256SUMS"

for language in zh-CN en ja; do
    site_use_language "$language"
    page=$site/$(site_page_path "$language" download/index.html)
    mkdir -p "$(dirname "$page")"
    {
    site_page_begin download/index.html "$(site_message download_title)" "$(site_message download_title)"
    printf '<p class="subtitle">%s &middot; %s %s &middot; x86_64</p>\n' \
        "$(site_text latest_release)" "$(site_text version)" "$(printf '%s' "$version" | site_html_escape)"
    cat <<HTML
<section>
<h2>$(site_text installer_iso)</h2>
<dl class="meta">
<dt>$(site_text file)</dt><dd>$(printf '%s' "$name" | site_html_escape)</dd>
<dt>$(site_text architecture)</dt><dd>x86_64</dd>
<dt>$(site_text size)</dt><dd>$(site_bytes_human "$size")</dd>
<dt>SHA256</dt><dd class="hash">$(printf '%s' "$iso_hash" | site_html_escape)</dd>
</dl>
<p><a class="button" href="$(site_href "download/$name")">$(site_text download)</a></p>
<p><a href="$(site_href download/SHA256SUMS)">SHA256SUMS</a> &mdash; $(site_text checksum_help)</p>
<pre>sha256sum -c SHA256SUMS</pre>
</section>
HTML
    site_page_end
    } > "$page"
done
