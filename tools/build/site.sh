#!/bin/sh
# Assemble the unified GitHub Pages tree from the pieces the build system has
# already produced. Publication (uploading/deploying this tree) is a separate CI
# action; this script only assembles it locally (plan §29, §31).
#
# The tree is one deployment artifact so that RPR, the download page and the
# home page all come from a single release build and cannot disagree (plan §45).
# The RPR machine interface is preserved verbatim under /rpr/ and its paths are
# relative, so the client base URL only needs the /rpr suffix added (plan §24).
#
#   arguments:
#     rpr_pages  $(O)/rpr-pages   fully generated RPR tree (apk/, kernel/, ...)
#     iso        .../images/reliefos-installer.iso
#     build      build_info.h (for the release version)
#     css        resources/pages/css/reliefos.css
#     output     $(O)/pages       the assembled site root
#     kernel_docs  selected ReliefNT checkout's Documentation/ directory
#
# Runs the download and home page generators from its own directory so it uses
# the matching scripts regardless of the working directory.
set -eu
[ "$#" = 6 ] || exit 2
rpr_pages=$1 iso=$2 build=$3 css=$4 output=$5 kernel_docs=$6
here=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
docs_src=$root/Documentation
[ -d "$rpr_pages" ] && [ -f "$rpr_pages/manifest.json" ] || { echo "missing RPR tree at $rpr_pages" >&2; exit 1; }
[ -s "$iso" ] || { echo "missing installer ISO $iso" >&2; exit 1; }
[ -f "$css" ] || { echo "missing shared stylesheet $css" >&2; exit 1; }
version=$(sed -n 's/^#define RELIEFOS_KERNEL_VERSION "\([0-9.]*\)"$/\1/p' "$build")
printf '%s\n' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || { echo 'invalid release version' >&2; exit 1; }

mkdir -p "$(dirname "$output")"
work=$(mktemp -d "$output.new.XXXXXX")
work=$(CDPATH= cd -- "$work" && pwd -P)
trap 'rm -rf "$work"' EXIT HUP INT TERM

mkdir -p "$work/pages"
# 1. RPR subtree: copy the machine interface and human pages as-is under /rpr/.
cp -R "$rpr_pages" "$work/pages/rpr"
# 2. Shared top-level stylesheet referenced by home and download pages at the
#    site root. The RPR subtree keeps its own /rpr/css copy for standalone use.
mkdir -p "$work/pages/css"
cp "$css" "$work/pages/css/reliefos.css"
# 3. Installer ISO into /download/ alongside its page.
mkdir -p "$work/pages/download"
cp "$iso" "$work/pages/download/${iso##*/}"
# 4. Generate the download page (uses the copied ISO so hash/size match exactly)
#    and the home page into the assembled root.
sh "$here/download-page.sh" "$work/pages/download/${iso##*/}" "$build" "$work/pages"
sh "$here/home-page.sh" "$build" "$work/pages"
# 5. Build the indexed RST manuals with Sphinx, preserving their directories.
#    This remains static HTML without JavaScript; process records are not shipped.
sh "$here/docs-page.sh" "$docs_src" "$kernel_docs" "$work/pages/docs"
: > "$work/pages/.nojekyll"
# Single completion stamp naming this atomic assembly. Make treats the whole
# Pages tree as one output keyed on this file, so a partially generated tree is
# never observed as complete (plan §44, §45).
printf '%s\n' "$version" > "$work/pages/.site-complete"

# --- atomic publish ---------------------------------------------------------
if [ -d "$output.previous" ] && [ ! -e "$output" ]; then mv "$output.previous" "$output"; fi
rm -rf "$output.previous"
if [ -e "$output" ]; then mv "$output" "$output.previous"; fi
mv "$work/pages" "$output"
rm -rf "$output.previous"
