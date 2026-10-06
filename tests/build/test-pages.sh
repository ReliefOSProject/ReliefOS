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
head -c 4096 /dev/zero > "$tmp/loader.elf"
head -c 123456 /dev/zero > "$tmp/reliefos-installer.iso"

# The kernel side of the release metadata: an install manifest whose per-artifact
# hashes match these exact bytes, and the kernel's build-version file.
mkdir -p "$tmp/kernel-install"
kernel_hash=$(sha256sum "$tmp/kernel.sys" | cut -d' ' -f1)
loader_hash=$(sha256sum "$tmp/loader.elf" | cut -d' ' -f1)
manifest="$tmp/kernel-install/manifest.txt"
printf 'format_version: 1\narch: x86_64\nartifacts:\n  %s  kernel.sys\n  %s  loader.elf\n' \
    "$kernel_hash" "$loader_hash" > "$manifest"
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
    "$tmp/kernel.sys" "$tmp/loader.elf" "$manifest" "$version_src" \
    "$tmp/fake-bin/apk" "$tmp/key" "$tmp/rpr-pages" \
    || { echo 'rpr-pages.sh failed' >&2; exit 1; }
sh "$src/tools/build/site.sh" "$tmp/rpr-pages" "$tmp/reliefos-installer.iso" \
    "$build" "$src/resources/pages/css/reliefos.css" "$tmp/pages" \
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
         rpr/kernel/kernel.sys rpr/kernel/loader.elf rpr/kernel/SHA256SUMS \
         rpr/manifest.json rpr/health.txt; do
    [ -f "$tmp/pages/$f" ] || bad "machine interface lost $f"
done
[ -f "$tmp/pages/rpr/manifest.json" ] && ok 'RPR machine interface preserved'

# 4. Release.txt version is the real 3-part version (machine contract, §37).
grep -q '^version=4.9.1$' "$tmp/pages/rpr/kernel/release.txt" \
    && ok 'release.txt version matches build' \
    || bad 'release.txt version drifted from build'

# 4b. The release.txt key set and the pairing hashes are the fixed machine
#     contract (format_version=2: kernel and loader update as one unit).
keys=$(sed 's/=.*//' "$tmp/pages/rpr/kernel/release.txt" | LC_ALL=C sort | tr '\n' ' ')
expected_keys='format_version image_version kernel_file kernel_sha256 loader_file loader_sha256 version '
if [ "$keys" = "$expected_keys" ]; then
    ok 'release.txt key set unchanged'
else
    bad "release.txt key set drifted: $keys"
fi
grep -q "^kernel_sha256=$kernel_hash\$" "$tmp/pages/rpr/kernel/release.txt" \
    && grep -q "^loader_sha256=$loader_hash\$" "$tmp/pages/rpr/kernel/release.txt" \
    && ok 'release.txt hashes match the published kernel manifest' \
    || bad 'release.txt hashes drifted from the kernel manifest'

# 5. No JavaScript anywhere and the ISO hash matches its SHA256SUMS.
[ -z "$(find "$tmp/pages" -type f -name '*.js' 2>/dev/null)" ] \
    && ok 'no .js files in Pages tree' || bad 'a .js file leaked into Pages'
( cd "$tmp/pages/download" && sha256sum -c SHA256SUMS >/dev/null 2>&1 ) \
    && ok 'installer ISO matches download/SHA256SUMS' \
    || bad 'installer ISO checksum mismatch'

# 5b. Documentation: at least one rendered doc, index exists, raw Markdown does
#     not leak, and no HTML/JS injection characters survive.
[ -f "$tmp/pages/docs/index.html" ] && ok 'docs index present' || bad 'docs index missing'
docs_rendered=$(find "$tmp/pages/docs" -mindepth 2 -name index.html 2>/dev/null | wc -l)
[ "$docs_rendered" -gt 0 ] && ok "docs rendered ($docs_rendered pages)" || bad 'no docs rendered'
md_leak=$(find "$tmp/pages/docs" -type f -name '*.md' 2>/dev/null)
[ -z "$md_leak" ] && ok 'no raw Markdown in published docs' || bad "Markdown leaked: $md_leak"
if grep -qE '<script|javascript:' "$tmp/pages/docs/index.html"; then
    bad 'script marker in docs index'
else
    ok 'docs index has no script markers'
fi

# Internal handoff/audit documents stay outside Pages; public references must
# retain their repository paths without linking to unpublished HTML pages.
brand_page="$tmp/pages/docs/branding-compatibility/index.html"
for internal in superpowers/specs/2026-09-24-ntclks-separation-design.md \
                superpowers/ntclks-separation/09-m1-authority-design-review.md \
                superpowers/specs/2026-09-27-reliefos-reliefnt-rename-baseline.md \
                superpowers/specs/2026-09-27-reliefos-reliefnt-rename-design.md \
                superpowers/plans/2026-09-27-reliefos-reliefnt-rename.md \
                superpowers/task11-verification.md; do
    internal_name=${internal##*/}
    internal_name=${internal_name%.md}
    if [ ! -e "$tmp/pages/docs/$internal_name" ] \
        && grep -Fq "<code>docs/$internal</code>" "$brand_page" \
        && ! grep -Fq "href=\"../$internal_name/index.html" "$brand_page"; then
        ok "internal reference stays readable without a Pages link: $internal"
    else
        bad "internal reference leaked or lost its repository path: $internal"
    fi
done

# 5c. md2html.awk escaping discipline: raw <tag>, javascript: link, and onX=
#     attribute must not survive the converter as live HTML/JS.
inj="$tmp/inj.md"
cat > "$inj" <<'MD'
# Test

Literal tokens: <app> <pid> <sha256>.

Inline code: `<script>alert(1)</script>`.

Link [bad](javascript:alert(1)) and [ok](https://example.com).

Event-like text: onerror=alert(1) as plain prose.

Markdown HTML attempt: <img src=x onerror=alert(1)>
MD
awk -f "$src/tools/build/md2html.awk" "$inj" > "$tmp/inj.html"
# No unescaped raw tags beyond the converter's own allowlist, and no live
# javascript: URL in the href attribute.
if grep -qE 'href="javascript:' "$tmp/inj.html"; then
    bad 'javascript: URL survived href sanitization'
else
    ok 'javascript: href neutralized'
fi
if grep -qE '<(img|script)\b' "$tmp/inj.html"; then
    bad 'raw HTML tag injection succeeded'
else
    ok 'raw HTML injection escaped'
fi
if ! grep -q '&lt;app&gt;' "$tmp/inj.html"; then
    bad '<app> not escaped to &lt;app&gt;'
else
    ok 'angle-bracket tokens escaped'
fi

# 6. Negative: a broken link must fail verification.
cp -R "$tmp/pages" "$tmp/broken"
sed -i 's#href="download/index.html"#href="download/nope.html"#' "$tmp/broken/index.html"
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
