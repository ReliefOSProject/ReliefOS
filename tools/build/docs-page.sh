#!/bin/sh
# Publish separate manuals from the system and kernel repositories.
set -efu
[ "$#" = 3 ] || exit 2
os_docs=$1 nt_docs=$2 output=$3
[ -f "$os_docs/index.rst" ] && [ -f "$nt_docs/index.rst" ] || {
    echo 'both ReliefOS and ReliefNT documentation sources are required' >&2; exit 1;
}
os_docs=$(CDPATH= cd -- "$os_docs" && pwd -P)
nt_docs=$(CDPATH= cd -- "$nt_docs" && pwd -P)
[ "$os_docs" != "$nt_docs" ] || {
    echo 'ReliefOS and ReliefNT must use separate documentation sources' >&2; exit 1;
}
case "$output" in ''|/|.|..) exit 1 ;; esac
[ ! -L "$output" ] || exit 1
if [ -e "$output" ] && [ ! -f "$output/.reliefos-docs-portal" ]; then
    echo 'refusing to replace an unowned documentation portal' >&2; exit 1
fi
mkdir -p "$(dirname "$output")"
work=$(mktemp -d "$output.new.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/portal"
sh "$(dirname "$os_docs")/scripts/build-docs.sh" "$os_docs" "$work/portal/reliefos"
sh "$(dirname "$nt_docs")/scripts/build-docs.sh" "$nt_docs" "$work/portal/reliefnt"
for language in zh-CN en ja; do
    back= directory= locale_dir= zh_current= en_current= ja_current=
    case "$language" in
        zh-CN)
            title='文档中心'
            description='两套手册分别在各自源码仓库维护和构建，按产品选择。'
            os_label='ReliefOS：安装、桌面、应用和系统构建'
            nt_label='ReliefNT：内核启动、部件、驱动与 UAPI'
            language_label='语言'
            zh_current=' aria-current="page"'
            chinese=index.html english=en/index.html japanese=ja/index.html ;;
        en)
            title='Documentation'
            description='Each manual is maintained and built in its own source repository. Choose a product.'
            os_label='ReliefOS: installation, desktop, applications and system builds'
            nt_label='ReliefNT: kernel boot, components, drivers and UAPI'
            language_label='Language'
            en_current=' aria-current="page"'
            chinese=../index.html english=index.html japanese=../ja/index.html ;;
        ja)
            title='ドキュメント'
            description='各マニュアルはそれぞれのソースリポジトリで管理・ビルドされます。製品を選んでください。'
            os_label='ReliefOS：インストール、デスクトップ、アプリケーション、システムビルド'
            nt_label='ReliefNT：カーネル起動、コンポーネント、ドライバー、UAPI'
            language_label='言語'
            ja_current=' aria-current="page"'
            chinese=../index.html english=../en/index.html japanese=index.html ;;
    esac
    if [ "$language" != zh-CN ]; then
        back=../ directory=$language/ locale_dir=$language/
    fi
    os_manual=${back}reliefos/$locale_dir
    nt_manual=${back}reliefnt/$locale_dir
    mkdir -p "$work/portal/$directory"
    cat > "$work/portal/${directory}index.html" <<HTML
<!doctype html>
<html lang="$language"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ReliefOS / ReliefNT $title</title>
<link rel="stylesheet" href="${os_manual}_static/basic.css">
<link rel="stylesheet" href="${os_manual}_static/alabaster.css">
<link rel="stylesheet" href="${os_manual}_static/manual.css">
<link rel="alternate" hreflang="zh-CN" href="$chinese">
<link rel="alternate" hreflang="en" href="$english">
<link rel="alternate" hreflang="ja" href="$japanese">
</head><body>
<nav class="language-switcher" aria-label="$language_label">
<a href="$chinese" lang="zh-CN" hreflang="zh-CN"$zh_current>简体中文</a>
<a href="$english" lang="en" hreflang="en"$en_current>English</a>
<a href="$japanese" lang="ja" hreflang="ja"$ja_current>日本語</a>
</nav>
<div class="document"><div class="body" role="main">
<h1>$title</h1>
<p>$description</p>
<ul><li><a href="${os_manual}index.html">$os_label</a></li>
<li><a href="${nt_manual}index.html">$nt_label</a></li></ul>
</div></div></body></html>
HTML
done
: > "$work/portal/.reliefos-docs-portal"
if [ -d "$output.previous" ] && [ ! -e "$output" ]; then
    [ -f "$output.previous/.reliefos-docs-portal" ] || exit 1
    mv "$output.previous" "$output"
fi
if [ -e "$output.previous" ]; then
    [ -f "$output.previous/.reliefos-docs-portal" ] || exit 1
    rm -rf "$output.previous"
fi
if [ -e "$output" ]; then mv "$output" "$output.previous"; fi
mv "$work/portal" "$output"
rm -rf "$output.previous"
