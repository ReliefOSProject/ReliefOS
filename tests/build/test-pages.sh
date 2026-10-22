#!/bin/sh
# Contract test for the GitHub Pages assembly: build a full tree from synthetic
# build artifacts, verify it passes verify-pages.sh, and confirm verify-pages.sh
# rejects the failure modes that must block a deployment (plan §46, §44).
#
# No cross toolchain is needed: rpr-pages.sh is exercised here only through the
# site.sh assembly of an already-built RPR subtree, and the APK index is produced
# by a stub `apk` exactly like test-rpr-packages.sh does.
set -eu

src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
fail=0
ok()   { printf 'ok - %s\n' "$1"; }
bad()  { printf 'FAIL - %s\n' "$1"; fail=1; }

build="$tmp/build_info.h"
printf '#define RELIEFOS_KERNEL_VERSION "4.9.1"\n' > "$build"
head -c 8192 /dev/zero > "$tmp/kernel.sys"
head -c 123456 /dev/zero > "$tmp/reliefos-installer.iso"

# The kernel side of the release metadata: an install manifest whose per-artifact
# hashes match these exact bytes, and the kernel's build-version file.
mkdir -p "$tmp/kernel-install"
kernel_hash=$(sha256sum "$tmp/kernel.sys" | cut -d' ' -f1)
manifest="$tmp/kernel-install/manifest.txt"
printf 'format_version: 1\narch: x86_64\nartifacts:\n  %s  kernel.sys\n' \
    "$kernel_hash" > "$manifest"
version_src="$tmp/build-version"
printf 'kernel_name=ReliefNT\nrelease_version=4.9.1\n' > "$version_src"

# Stub apk: mkndx just creates the requested output, as in the RPR test.
mkdir -p "$tmp/fake-bin"
cat > "$tmp/fake-bin/apk" <<'EOF'
#!/bin/sh
set -eu
[ "$1" = mkndx ] || exit 0
out=
while [ "$#" -gt 0 ]; do [ "$1" = --output ] && out=$2; shift; done
[ -n "$out" ] && : > "$out"
EOF
chmod 755 "$tmp/fake-bin/apk"

# Real 0600 signing key so the pub-key export path is exercised.
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$tmp/key" 2>/dev/null
chmod 600 "$tmp/key"

# Two package sources so the generated list has >1 row.
mkdir -p "$tmp/repository" "$tmp/apps"
printf aaa > "$tmp/repository/reliefos-musl-4.9.1-r5.apk"
printf bb  > "$tmp/apps/reliefos-helloworld-4.9.1-r5.apk"

# 1. Build the RPR subtree, then assemble the full Pages tree.
sh "$src/tools/build/rpr-pages.sh" "$tmp/repository" "$tmp/apps" \
    "$tmp/kernel.sys" "$manifest" "$version_src" \
    "$tmp/fake-bin/apk" "$tmp/key" "$tmp/rpr-pages" \
    || { echo 'rpr-pages.sh failed' >&2; exit 1; }
sh "$src/tools/build/site.sh" "$tmp/rpr-pages" "$tmp/reliefos-installer.iso" \
    "$build" "$src/resources/pages/css/reliefos.css" "$tmp/pages" "$src/kernel/reliefnt/Documentation" \
    || { echo 'site.sh failed' >&2; exit 1; }

# Public brand and download contract: visible page titles use the current
# product names, and the advertised checksum names the exact ISO beside it.
grep -q '<title>ReliefOS</title>' "$tmp/pages/index.html" \
    && grep -q '<h1>ReliefOS</h1>' "$tmp/pages/index.html" \
    && ok 'home page is titled ReliefOS' \
    || bad 'home page title is not ReliefOS'
grep -q '<title>ReliefNT Kernel</title>' "$tmp/pages/rpr/kernel/index.html" \
    && grep -q '<h1>ReliefNT Kernel</h1>' "$tmp/pages/rpr/kernel/index.html" \
    && ok 'kernel page is titled ReliefNT' \
    || bad 'kernel page title is not ReliefNT'
grep -q '<a class="button" href="reliefos-installer.iso">Download</a>' "$tmp/pages/download/index.html" \
    && ok 'download page links to reliefos-installer.iso' \
    || bad 'download page does not link to reliefos-installer.iso'
sum_names=$(awk '{print $2}' "$tmp/pages/download/SHA256SUMS")
[ "$sum_names" = reliefos-installer.iso ] \
    && ok 'SHA256SUMS names the published installer ISO' \
    || bad "SHA256SUMS names the wrong download: $sum_names"

# The installer workflow must validate and upload the same canonical image
# paths. Extract the relevant YAML step bodies so a path in an unrelated
# comment or summary cannot satisfy the contract.
workflow="$src/.github/workflows/build-installer.yml"
verify_step="$tmp/ci-verify-step"
upload_step="$tmp/ci-upload-step"
awk '/^      - name: Verify generated images$/ { active=1; next }
     active && /^      - name:/ { exit }
     active { print }' "$workflow" > "$verify_step"
awk '/^      - name: Upload ReliefOS artifacts$/ { active=1; next }
     active && /^      - name:/ { exit }
     active { print }' "$workflow" > "$upload_step"
for image in reliefos.vmdk reliefos-live.iso reliefos-installer.iso; do
    grep -Fq "test -s out/x86_64/release/images/$image" "$verify_step" \
        && grep -Fq "out/x86_64/release/images/$image" "$upload_step" \
        && ok "CI validates and uploads $image" \
        || bad "CI does not validate and upload the same path for $image"
done
pages_workflow="$src/.github/workflows/build-pages.yml"
pages_verify="$tmp/pages-ci-verify-step"
pages_upload="$tmp/pages-ci-upload-step"
awk '/^      - name: Verify Pages tree$/ { active=1; next }
     active && /^      - name:/ { exit }
     active { print }' "$pages_workflow" > "$pages_verify"
awk '/^      - name: Upload GitHub Pages artifact$/ { active=1; next }
     active && /^      - name:/ { exit }
     active { print }' "$pages_workflow" > "$pages_upload"
grep -Fq 'sh tools/build/verify-pages.sh out/x86_64/release/pages' "$pages_verify" \
    && grep -Fq 'path: out/x86_64/release/pages' "$pages_upload" \
    && ok 'Pages CI verifies and uploads the same assembled tree' \
    || bad 'Pages CI does not verify and upload the same assembled tree'

# 2. The assembled tree must pass verification.
if sh "$src/tools/build/verify-pages.sh" "$tmp/pages" >"$tmp/verify-good.log" 2>&1; then
    ok 'verify-pages.sh accepts the assembled tree'
else
    cat "$tmp/verify-good.log" >&2
    bad 'verify-pages.sh rejected a good tree'
fi

# 3. Every machine-interface file survived the move under /rpr/.
for f in rpr/apk/packages.adb rpr/apk/repository.json rpr/apk/reliefos-rpr.rsa.pub rpr/apk/leonos-rpr.rsa.pub \
         rpr/apk/SHA256SUMS rpr/kernel/release.txt rpr/kernel/release.json \
         rpr/kernel/kernel.sys rpr/kernel/SHA256SUMS \
         rpr/manifest.json rpr/health.txt; do
    [ -f "$tmp/pages/$f" ] || bad "machine interface lost $f"
done
[ -f "$tmp/pages/rpr/manifest.json" ] && ok 'RPR machine interface preserved'

# 4. Release.txt version is the real 3-part version (machine contract, §37).
grep -q '^version=4.9.1$' "$tmp/pages/rpr/kernel/release.txt" \
    && ok 'release.txt version matches build' \
    || bad 'release.txt version drifted from build'

# 4b. The release.txt key set and the pairing hash are the fixed machine
#     contract (format_version=3: kernel.sys is the single release unit).
keys=$(sed 's/=.*//' "$tmp/pages/rpr/kernel/release.txt" | LC_ALL=C sort | tr '\n' ' ')
expected_keys='format_version image_version kernel_file kernel_sha256 version '
if [ "$keys" = "$expected_keys" ]; then
    ok 'release.txt key set unchanged'
else
    bad "release.txt key set drifted: $keys"
fi
grep -q "^kernel_sha256=$kernel_hash\$" "$tmp/pages/rpr/kernel/release.txt" \
    && ok 'release.txt hash matches the published kernel manifest' \
    || bad 'release.txt hash drifted from the kernel manifest'

# 5. No JavaScript anywhere and the ISO hash matches its SHA256SUMS.
[ -z "$(find "$tmp/pages" -type f -name '*.js' 2>/dev/null)" ] \
    && ok 'no .js files in Pages tree' || bad 'a .js file leaked into Pages'
( cd "$tmp/pages/download" && sha256sum -c SHA256SUMS >/dev/null 2>&1 ) \
    && ok 'installer ISO matches download/SHA256SUMS' \
    || bad 'installer ISO checksum mismatch'

# 5b. The indexed manual preserves its hierarchy without raw source or scripts.
[ -f "$tmp/pages/docs/index.html" ] && ok 'docs index present' || bad 'docs index missing'
for doc in admin-guide/installation applications/diskmgr kbuild/building process/releasing; do
    [ -s "$tmp/pages/docs/reliefos/$doc.html" ] && ok "Sphinx page present: $doc" || bad "missing Sphinx page: $doc"
done
source_leak=$(find "$tmp/pages/docs" -type f \( -name '*.md' -o -name '*.rst' -o -name '*.rst.txt' -o -name '*.pickle' \) -print)
[ -z "$source_leak" ] && ok 'no raw source or doctrees in published manual' || bad "source leaked: $source_leak"

# The kernel manual comes from the kernel repository, not the system book.
if [ -s "$tmp/pages/docs/reliefnt/core-api/modules.html" ] &&
   grep -q 'ReliefNT 内核文档' "$tmp/pages/docs/reliefnt/index.html" &&
   [ ! -e "$tmp/pages/docs/reliefos/core-api/modules.html" ]; then
    ok 'system and kernel manuals remain separate'
else
    bad 'kernel manual is absent or mixed into the system book'
fi
grep -Fq 'href="../docs/reliefos/index.html"' "$tmp/pages/rpr/index.html" &&
grep -Fq 'href="../../docs/reliefnt/index.html"' "$tmp/pages/rpr/kernel/index.html" &&
    ok 'RPR links to both owning manuals' || bad 'RPR documentation links missing'

# 5c. An unresolved RST cross-reference must fail without replacing a good manual.
mkdir -p "$tmp/bad-docs"
cp "$src/Documentation/conf.py" "$tmp/bad-docs/conf.py"
# The production conf reads release data relative to its repository root.
mkdir -p "$tmp/configs"
cp "$src/configs/build-version" "$tmp/configs/build-version"
cp -R "$src/Documentation/_templates" "$tmp/bad-docs/_templates"
cat > "$tmp/bad-docs/index.rst" <<'RST'
Broken reference
================

See :doc:`missing-page`.
RST
before_docs=$(sha256sum "$tmp/pages/docs/reliefos/index.html")
if sh "$src/scripts/build-docs.sh" "$tmp/bad-docs" "$tmp/pages/docs/reliefos" >"$tmp/bad-docs.log" 2>&1; then
    bad 'Sphinx accepted an unresolved document reference'
elif grep -q 'missing-page' "$tmp/bad-docs.log" && [ "$before_docs" = "$(sha256sum "$tmp/pages/docs/reliefos/index.html")" ]; then
    ok 'unresolved RST reference fails and preserves the published manual'
else
    cat "$tmp/bad-docs.log" >&2
    bad 'documentation failure was unrelated or replaced the published tree'
fi

# A bad kernel manual must not publish even if the system manual built.
before_site=$(sha256sum "$tmp/pages/index.html" "$tmp/pages/docs/reliefnt/index.html")
if sh "$src/tools/build/site.sh" "$tmp/rpr-pages" "$tmp/reliefos-installer.iso" \
    "$build" "$src/resources/pages/css/reliefos.css" "$tmp/pages" "$tmp/bad-docs" \
    >"$tmp/bad-kernel-docs.log" 2>&1; then
    bad 'site publication accepted a broken kernel manual'
elif grep -q 'missing-page' "$tmp/bad-kernel-docs.log" &&
     [ "$before_site" = "$(sha256sum "$tmp/pages/index.html" "$tmp/pages/docs/reliefnt/index.html")" ]; then
    ok 'kernel documentation failure preserves the complete published site'
else
    bad 'kernel documentation failure was unrelated or replaced the site'
fi

# The production Make rule must notice deleted pages in either repository.
# A query-only fixture exercises that rule without rebuilding product images.
fixture="$tmp/make-fixture"
mkdir -p "$fixture/os/Documentation" "$fixture/nt/Documentation" \
    "$fixture/os/resources/pages/css" "$fixture/os/tools/build" \
    "$fixture/os/scripts" "$fixture/os/configs" "$fixture/nt/configs" \
    "$fixture/rpr" "$fixture/out/pages"
for path in site.sh download-page.sh home-page.sh docs-page.sh site-common.sh; do
    : > "$fixture/os/tools/build/$path"
done
for path in os/Documentation/index.rst os/Documentation/old.rst \
    nt/Documentation/index.rst nt/Documentation/old.rst \
    os/configs/build-version nt/configs/build-version os/scripts/build-docs.sh \
    os/resources/pages/css/reliefos.css rpr/.complete rpr/manifest.json \
    installer.iso build_info.h; do
    : > "$fixture/$path"
done
cat > "$fixture/Makefile" <<EOF
RELIEFOS_SRC := $fixture/os
RELIEFNT_DIR := $fixture/nt
O := $fixture/out
RPR_PAGES := $fixture/rpr
INSTALLER_ISO := $fixture/installer.iso
BUILD_INFO_HEADER := $fixture/build_info.h
include $src/scripts/reliefos/Makefile.site
EOF
for repo in os nt; do
    : > "$fixture/$repo/Documentation/old.rst"
    find "$fixture" -exec touch -t 200001010000 {} +
    touch -t 200001020000 "$fixture/out/pages/.site-complete" "$fixture/out/pages/index.html"
    if ! make --no-print-directory -f "$fixture/Makefile" -q "$fixture/out/pages/.site-complete"; then
        bad "Pages dependency fixture was not initially current for $repo"
    fi
    rm "$fixture/$repo/Documentation/old.rst"
    if make --no-print-directory -f "$fixture/Makefile" -q "$fixture/out/pages/.site-complete"; then
        bad "deleted $repo documentation would remain published"
    elif [ "$?" = 1 ]; then
        ok "deleted $repo documentation triggers Pages rebuilding"
    else
        bad "Pages dependency query failed for $repo"
    fi
done

# Both repositories' clean targets own generated documentation, while unknown
# files inside the output directory must survive. Pages belongs to the system.
for repo in os nt; do
    clean_source=$src
    [ "$repo" = os ] || clean_source=$src/kernel/reliefnt
    clean_output="$tmp/clean-$repo"
    mkdir -p "$clean_output/Documentation/html"
    : > "$clean_output/Documentation/html/index.html"
    printf 'keep user notes\n' > "$clean_output/notes.txt"
    printf 'reliefos-build-out version=1 root=%s\n' "$clean_source" > "$clean_output/.reliefos-out"
    if [ "$repo" = os ]; then mkdir -p "$clean_output/pages"; fi
    if O="$clean_output" SRC="$clean_source" KEEP_CONFIG=1 \
       sh "$clean_source/scripts/clean.sh" >"$tmp/clean-$repo.log" 2>&1 &&
       [ ! -e "$clean_output/Documentation" ] && [ ! -e "$clean_output/pages" ] &&
       [ -s "$clean_output/notes.txt" ]; then
        ok "$repo clean removes generated manuals and keeps unknown files"
    else
        cat "$tmp/clean-$repo.log" >&2
        bad "$repo documentation cleanup failed"
    fi
done

# 6. Negative: a broken link must fail verification.
cp -R "$tmp/pages" "$tmp/broken"
sed -i 's#href="download/index.html"#href="download/nope.html?release=4.9.1"#' "$tmp/broken/index.html"
if sh "$src/tools/build/verify-pages.sh" "$tmp/broken" >/dev/null 2>&1; then
    bad 'verify-pages.sh accepted a broken link'
else
    ok 'verify-pages.sh rejects a broken link'
fi

# 7. Negative: injected JavaScript must fail verification.
cp -R "$tmp/pages" "$tmp/js"
printf 'alert(1)\n' > "$tmp/js/tracker.js"
if sh "$src/tools/build/verify-pages.sh" "$tmp/js" >/dev/null 2>&1; then
    bad 'verify-pages.sh accepted a .js file'
else
    ok 'verify-pages.sh rejects a .js file'
fi

# 8. Negative: a missing machine file must fail verification.
cp -R "$tmp/pages" "$tmp/noindex"
rm -f "$tmp/noindex/rpr/apk/packages.adb"
if sh "$src/tools/build/verify-pages.sh" "$tmp/noindex" >/dev/null 2>&1; then
    bad 'verify-pages.sh accepted a missing packages.adb'
else
    ok 'verify-pages.sh rejects a missing packages.adb'
fi

[ "$fail" = 0 ] || exit 1
printf 'Pages assembly and verification contract passed\n'
