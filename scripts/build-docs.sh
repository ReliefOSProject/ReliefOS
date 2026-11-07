#!/bin/sh
# Build a complete RST manual privately, then publish the generated directory.
set -efu
[ "$#" = 2 ] || exit 2
source=$1 output=$2
[ -f "$source/conf.py" ] && [ -f "$source/index.rst" ] || {
    echo "missing Sphinx documentation source: $source" >&2; exit 1;
}
case "$output" in ''|/|.|..) echo 'unsafe documentation output' >&2; exit 1 ;; esac
[ ! -L "$output" ] || { echo 'documentation output is a symlink' >&2; exit 1; }
if [ -e "$output" ] && [ ! -f "$output/.reliefos-docs-output" ]; then
    echo "refusing to replace an unowned documentation directory: $output" >&2
    exit 1
fi
command -v "${SPHINXBUILD:-sphinx-build}" >/dev/null 2>&1 || {
    echo 'Sphinx is required for documentation; see Documentation/requirements.txt' >&2
    exit 1
}
mkdir -p "$(dirname "$output")"
work=$(mktemp -d "$output.new.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
# Options use the usual space-separated convention; globbing is disabled.
"${SPHINXBUILD:-sphinx-build}" -b html -n -W --keep-going ${SPHINXOPTS:-} \
    -d "$work/doctrees" "$source" "$work/html"
: > "$work/html/.reliefos-docs-output"
if [ -d "$output.previous" ] && [ ! -e "$output" ]; then
    [ -f "$output.previous/.reliefos-docs-output" ] || exit 1
    mv "$output.previous" "$output"
fi
if [ -e "$output.previous" ]; then
    [ -f "$output.previous/.reliefos-docs-output" ] || exit 1
    rm -rf "$output.previous"
fi
if [ -e "$output" ]; then mv "$output" "$output.previous"; fi
mv "$work/html" "$output"
rm -rf "$output.previous"
