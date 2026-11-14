# shellcheck shell=sh
# Shared helpers for the ReliefOS static GitHub Pages generators.
#
# Everything here is plain POSIX sh and produces static HTML/CSS only. There is
# deliberately no JavaScript, no client-side rendering and no runtime API use:
# all page content is fixed at build time from real build artifacts (plan §12,
# §22). Generators source this file; they must not re-implement these pieces.

# site_html_escape: HTML-escape text on stdin so it is safe to place inside an
# HTML text node or a quoted attribute. Package names, versions, filenames and
# hashes are build-controlled, but we still escape every dynamic value so the
# generators can never turn a stray character into markup (plan §26, §27).
site_html_escape() {
    sed -e 's/&/\&amp;/g' \
        -e 's/</\&lt;/g' \
        -e 's/>/\&gt;/g' \
        -e 's/"/\&quot;/g' \
        -e "s/'/\&#39;/g"
}

# site_bytes_human <size>: print a byte count as a short human string. Uses
# integer arithmetic only and always appends the unit, keeping the value
# unambiguous for a technical audience.
site_bytes_human() {
    bytes=${1:-0}
    case $bytes in ''|*[!0-9]*) bytes=0 ;; esac
    kb=$(( (bytes + 1023) / 1024 ))
    mb=$(( (bytes + 1048575) / 1048576 ))
    if [ "$bytes" -lt 1024 ]; then
        printf '%s B\n' "$bytes"
    elif [ "$mb" -lt 1024 ]; then
        printf '%s KiB\n' "$kb"
    else
        printf '%s MiB\n' "$mb"
    fi
}

# Public routes are relative to the deployment root, never the host root.
# Chinese keeps the historical paths; only human pages get language prefixes.
site_use_language() {
    case $1 in
        zh-CN) site_language_prefix= ;;
        en|ja) site_language_prefix=$1/ ;;
        *) printf 'unsupported site language: %s\n' "$1" >&2; return 1 ;;
    esac
    site_language=$1
    # All generators live in tools/build. Tests may supply the resource root.
    # shellcheck source=/dev/null
    . "${SITE_I18N_DIR:-$(dirname "$0")/../../resources/pages/i18n}/$site_language.sh"
}

site_text() { site_message "$1" | site_html_escape; }

site_page_path() (
    case $1 in
        zh-CN) printf '%s\n' "$2" ;;
        en|ja) printf '%s/%s\n' "$1" "$2" ;;
        *) return 1 ;;
    esac
)

# site_relative_href <current-page> <target-from-site-root>
# Remove shared directories before adding ../, including for file:// previews.
site_relative_href() (
    site_from=${1%/*}
    [ "$site_from" != "$1" ] || site_from=
    site_target=$2
    while [ -n "$site_from" ] && [ "${site_from%%/*}" = "${site_target%%/*}" ]; do
        case $site_from in */*) site_from=${site_from#*/} ;; *) site_from= ;; esac
        site_target=${site_target#*/}
    done
    site_up=
    while [ -n "$site_from" ]; do
        site_up=../$site_up
        case $site_from in */*) site_from=${site_from#*/} ;; *) site_from= ;; esac
    done
    printf '%s%s\n' "$site_up" "$site_target"
)

site_href() { site_relative_href "$site_current_path" "$1"; }
site_ui_href() { site_href "$(site_page_path "$site_language" "$1")"; }

# Documentation puts the language after the product (or after docs for portals).
site_docs_href() (
    site_manual=${1:-}
    site_doc_prefix=docs/
    [ -z "$site_manual" ] || site_doc_prefix=$site_doc_prefix$site_manual/
    site_href "${site_doc_prefix}${site_language_prefix}${2:-index.html}"
)

site_language_name() {
    case $1 in zh-CN) printf '%s' '简体中文' ;; en) printf '%s' 'English' ;; ja) printf '%s' '日本語' ;; esac
}

# site_page_begin <logical-page-without-language-prefix> <title> <heading>
# Every alternate and language switch uses the same logical page, including
# package detail pages. Titles and headings are plain text, escaped here.
site_page_begin() {
    site_page=$1
    site_current_path=$(site_page_path "$site_language" "$site_page")
    cat <<HEAD
<!doctype html>
<html lang="$site_language">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>$(printf '%s' "$2" | site_html_escape)</title>
<link rel="stylesheet" href="$(site_href css/reliefos.css)">
HEAD
    for site_alternate in zh-CN en ja; do
        printf '<link rel="alternate" hreflang="%s" href="%s">\n' \
            "$site_alternate" "$(site_href "$(site_page_path "$site_alternate" "$site_page")")"
    done
    cat <<HEAD
<link rel="alternate" hreflang="x-default" href="$(site_href "$site_page")">
</head>
<body>
<div class="page">
<header class="site">
<h1>$(printf '%s' "$3" | site_html_escape)</h1>
</header>
<nav class="site" aria-label="$(site_text navigation)">
HEAD
    for site_nav in index.html download/index.html rpr/index.html docs/index.html; do
        case $site_nav in
            index.html) site_nav_label=$(site_text home) ;;
            download/index.html) site_nav_label=$(site_text download) ;;
            rpr/index.html) site_nav_label=RPR ;;
            docs/index.html) site_nav_label=$(site_text docs) ;;
        esac
        site_nav_current=
        [ "$site_nav" != "$site_page" ] || site_nav_current=' aria-current="page"'
        if [ "$site_nav" = docs/index.html ]; then site_nav_href=$(site_docs_href)
        else site_nav_href=$(site_ui_href "$site_nav"); fi
        printf '<a href="%s"%s>%s</a>\n' "$site_nav_href" "$site_nav_current" "$site_nav_label"
    done
    printf '</nav>\n<nav class="languages" aria-label="%s">\n' "$(site_text language)"
    for site_alternate in zh-CN en ja; do
        site_language_current=
        [ "$site_alternate" != "$site_language" ] || site_language_current=' aria-current="page"'
        printf '<a href="%s" lang="%s" hreflang="%s"%s>%s</a>\n' \
            "$(site_href "$(site_page_path "$site_alternate" "$site_page")")" \
            "$site_alternate" "$site_alternate" "$site_language_current" "$(site_language_name "$site_alternate")"
    done
    cat <<HEAD
</nav>
<main>
HEAD
}

# site_page_end: close <main>, emit the shared footer and finish the document.
site_page_end() {
    cat <<FOOT
</main>
<footer class="site">
$(site_text footer)
</footer>
</div>
</body>
</html>
FOOT
}

# site_hash_for <sha256sums-file> <filename>: read a SHA256SUMS file and echo the
# recorded hash for <filename>, or nothing if absent. Centralised so the RPR and
# download pages all derive their displayed hashes from the same file that ships
# next to them, guaranteeing the page and the checksum agree (plan §28, §46.B).
site_hash_for() {
    [ "$#" = 2 ] || return 0
    sums=$1
    want=$2
    [ -f "$sums" ] || return 0
    awk -v w="$want" '$2==w {print $1; exit}' "$sums"
}
