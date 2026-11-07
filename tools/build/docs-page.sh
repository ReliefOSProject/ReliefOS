#!/bin/sh
# Publish separate manuals from the system and kernel repositories.
set -efu
[ "$#" = 3 ] || exit 2
os_docs=$1 nt_docs=$2 output=$3
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
[ -f "$os_docs/index.rst" ] && [ -f "$nt_docs/index.rst" ] || {
    echo 'both ReliefOS and ReliefNT documentation sources are required' >&2; exit 1;
}
[ "$(CDPATH= cd -- "$os_docs" && pwd -P)" != "$(CDPATH= cd -- "$nt_docs" && pwd -P)" ] || {
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
sh "$root/scripts/build-docs.sh" "$os_docs" "$work/portal/reliefos"
sh "$root/scripts/build-docs.sh" "$nt_docs" "$work/portal/reliefnt"
cat > "$work/portal/index.html" <<'HTML'
<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ReliefOS / ReliefNT 文档中心</title>
<link rel="stylesheet" href="reliefos/_static/basic.css">
<link rel="stylesheet" href="reliefos/_static/alabaster.css">
</head><body><div class="document"><div class="body">
<h1>文档中心</h1>
<p>两套手册分别在各自源码仓库维护和构建，按产品选择。</p>
<ul><li><a href="reliefos/index.html">ReliefOS：安装、桌面、应用和系统构建</a></li>
<li><a href="reliefnt/index.html">ReliefNT：内核启动、部件、驱动与 UAPI</a></li></ul>
</div></div></body></html>
HTML
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
