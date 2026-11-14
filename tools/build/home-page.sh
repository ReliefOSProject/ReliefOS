#!/bin/sh
# Generate the ReliefOS site home page at the Pages root.
#
# Minimal by design (plan §9, §40): an entry point with the version and links to
# Download, RPR/packages, Source and Documentation. It is not a README and it
# carries no commit history, badges or marketing. Version comes from the real
# build artifact; every link is relative so the page works under any Pages base
# path (plan §25). No JavaScript, single shared stylesheet.
set -eu
[ "$#" = 2 ] || exit 2
build=$1 output=$2
version=$(sed -n 's/^#define RELIEFOS_KERNEL_VERSION "\([0-9.]*\)"$/\1/p' "$build")
printf '%s\n' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || { echo 'invalid release version' >&2; exit 1; }
# shellcheck source=/dev/null
. "$(dirname "$0")/site-common.sh"

for language in zh-CN en ja; do
    site_use_language "$language"
    page=$output/$(site_page_path "$language" index.html)
    mkdir -p "$(dirname "$page")"
    {
    site_page_begin index.html ReliefOS ReliefOS
    printf '<p class="subtitle">%s &middot; %s %s</p>\n' \
        "$(site_text tagline)" "$(site_text latest_version)" "$(printf '%s' "$version" | site_html_escape)"
    cat <<HTML
<section>
<h2>$(site_text get_started)</h2>
<p>
<a class="button" href="$(site_ui_href download/index.html)">$(site_text download)</a>
<a class="button" href="$(site_ui_href rpr/packages/index.html)">$(site_text packages)</a>
</p>
<ul>
<li><a href="$(site_ui_href rpr/index.html)">RPR</a> &mdash; $(site_text rpr_summary)</li>
<li><a href="$(site_ui_href download/index.html)">$(site_text download)</a> &mdash; $(site_text download_summary)</li>
<li><a href="https://github.com/ReliefOSProject/ReliefOS">$(site_text source)</a> &mdash; $(site_text source_summary)</li>
<li><a href="$(site_docs_href)">$(site_text docs)</a> &mdash; $(site_text docs_summary)</li>
</ul>
</section>
HTML
    site_page_end
    } > "$page"
done
