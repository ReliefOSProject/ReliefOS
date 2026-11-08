#!/bin/sh
# Exercise the actual guest updater in a private Linux mount namespace. This proves package
# transactions, not ReliefOS kernel/VM execution. All writes go to a private copy.
set -eu
[ "$#" = 2 ] || { echo 'usage: test-apk-upgrade OUTPUT_TREE OLD_MANAGED_ROOT' >&2; exit 2; }
out=$(CDPATH= cd -- "$1" && pwd -P)
old=$(CDPATH= cd -- "$2" && pwd -P)
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)
key=${APK_SIGNING_KEY:-$HOME/.local/share/leonos/apk-signing/key.pem}
work=$(mktemp -d "$out/apk-upgrade-test.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
apk=$out/upstream/apk/apk.static
cp -a "$out/rootfs/managed" "$work/runner"
cp -a "$old" "$work/runner/target"
cp -a "$out/packages/apk/repository" "$work/runner/newrepo"
python3 - "$work/runner/newrepo" "$out/packages/apk/manifest.json" <<'PY'
import json, pathlib, sys
repository = pathlib.Path(sys.argv[1])
manifest = json.loads(pathlib.Path(sys.argv[2]).read_text())
packages = manifest["packages"]
if "reliefos-apps" not in packages:
    raise SystemExit("new signed repository is missing reliefos-apps")
if any(name.startswith("leonos-") for name in packages):
    raise SystemExit("new signed repository still publishes leonos-* packages")
missing = []
for name in packages:
    matches = [path for path in repository.glob(f"{name}-*.apk")
               if not any(other.startswith(name + "-") and path.name.startswith(other + "-")
                          for other in packages)]
    if len(matches) != 1:
        missing.append(name)
if missing:
    raise SystemExit(f"signed repository manifest references missing packages: {missing}")
PY
if [ -n "${PRIOR_BAD_REPOSITORY:-}" ]; then
    cp -a "$PRIOR_BAD_REPOSITORY" "$work/runner/badrepo"
fi
mkdir -p "$work/external/usr/share/external-fixture" "$work/runner/fixture" \
    "$work/runner/target/etc/leonos"
printf 'external package preserved\n' > "$work/external/usr/share/external-fixture/data"
unshare -Ur "$apk" mkpkg --files "$work/external" --output "$work/runner/fixture/external.apk" \
    --info name:external-fixture --info version:1.0-r0 --info arch:x86_64 --sign-key "$key"
prior_errors=$(awk '/^f:/ && substr($0,3) ~ /[fs]/ {n++} END {print n+0}' "$work/runner/target/lib/apk/db/installed")
status=0
unshare -Ur "$apk" --root "$work/runner/target" --repositories-file /dev/null add "$work/runner/fixture/external.apk" > "$work/add.log" 2>&1 || status=$?
cat "$work/add.log"
[ "$status" = "$prior_errors" ]
! grep -q '^ERROR:' "$work/add.log"
unshare -Ur "$apk" --root "$work/runner/target" info --exists external-fixture
for tool in ar strings; do
    if [ -f "$old/usr/bin/$tool" ] && [ ! -L "$old/usr/bin/$tool" ]; then
        cmp "$old/usr/bin/$tool" "$work/runner/target/usr/bin/$tool"
    fi
done
printf 'local-admin-setting=yes\n' > "$work/runner/target/etc/leonos/upgrade-fixture.conf"
mkdir -p "$work/runner/target/etc/reliefos"
printf 'canonical-local-setting=yes\n' > "$work/runner/target/etc/reliefos/upgrade-fixture.conf"
printf 'LANG=C\nMUSL_LOCPATH=/usr/share/musl/locales\n' > "$work/runner/target/etc/leonos/locale.conf"
cp "$work/runner/target/etc/leonos/locale.conf" "$work/legacy-locale-before"
cp "$work/runner/target/etc/apk/world" "$work/world-before"
sed 's/^leonos-/reliefos-/' "$work/world-before" > "$work/world-after-rename"
awk '/^P:/{if($0=="P:leonos-apps") found=1} END{exit !found}' \
    "$work/runner/target/lib/apk/db/installed" || {
    echo 'legacy fixture does not contain an installed leonos-apps package' >&2
    exit 1
}
legacy_key=$(python3 - "$out/packages/apk/manifest.json" <<'PY'
import json, sys
print(json.load(open(sys.argv[1], encoding="utf-8"))["legacy_signing_public_key"])
PY
)
test -s "$work/runner/target/etc/apk/keys/$legacy_key" || {
    echo "legacy installed signing key is missing: $legacy_key" >&2
    exit 1
}
cp "$work/runner/target/etc/apk/keys/$legacy_key" "$work/old-signing-key.pub"
awk '/^P:/{p=$0} /^V:/{if(p=="P:external-fixture")print}' "$work/runner/target/lib/apk/db/installed" > "$work/external-before"
# Install the new command path while keeping old command aliases in the target.
mkdir -p "$work/runner/usr/lib/reliefos" "$work/runner/usr/lib/leonos"
cp "$src/userland/storage/reliefos-apk-update" "$work/runner/usr/lib/reliefos/reliefos-apk-update"
ln -sfn reliefos-apk-update "$work/runner/usr/lib/reliefos/leonos-apk-update"
ln -sfn ../reliefos/reliefos-apk-update "$work/runner/usr/lib/leonos/leonos-apk-update"
# pivot_root, rather than chroot, lets APK create its script sandbox on Linux.
# The old root remains mounted only inside this short-lived private namespace.
run_update() {
unshare -Urmp --fork sh -eu -c '
    mount --make-rprivate /
    mount --bind "$1" "$1"
    cd "$1"
    mkdir -p .oldroot
    pivot_root . .oldroot
    exec /usr/lib/reliefos/reliefos-apk-update /target "$2"
' sh "$work/runner" "$1"
}
# Reindex the signed package archives without signing the index. The updater must
# reject this otherwise complete package set using the installed trust anchors.
mkdir "$work/runner/unsignedrepo"
for package in "$work/runner/newrepo"/*.apk; do
    cp "$package" "$work/runner/unsignedrepo/"
done
"$apk" --allow-untrusted mkndx --output "$work/runner/unsignedrepo/packages.adb" \
    "$work/runner/unsignedrepo"/*.apk
cp "$work/runner/target/lib/apk/db/installed" "$work/database-before-signature-check"
if run_update /unsignedrepo > "$work/signature.log" 2>&1; then
    echo 'updater accepted an unsigned repository index' >&2
    exit 1
fi
grep -Eiq 'signature|trusted' "$work/signature.log" || {
    cat "$work/signature.log" >&2
    echo 'unsigned repository failed for a reason other than signature verification' >&2
    exit 1
}
cmp "$work/world-before" "$work/runner/target/etc/apk/world"
cmp "$work/database-before-signature-check" "$work/runner/target/lib/apk/db/installed"
cmp "$work/old-signing-key.pub" "$work/runner/target/etc/apk/keys/$legacy_key"
# A trusted index still cannot authorize a package payload with a bad digest.
# Validate this before the updater begins purging old packages.
cp -a "$work/runner/newrepo" "$work/runner/corruptrepo"
python3 - "$work/runner/corruptrepo" <<'PY'
import pathlib, sys
repository = pathlib.Path(sys.argv[1])
packages = list(repository.glob("reliefos-apps-*.apk"))
if len(packages) != 1:
    raise SystemExit(f"expected one signed reliefos-apps archive, found {len(packages)}")
with packages[0].open("r+b") as archive:
    archive.seek(0, 2)
    offset = archive.tell() // 2
    archive.seek(offset)
    byte = archive.read(1)
    archive.seek(offset)
    archive.write(bytes([byte[0] ^ 1]))
PY
if run_update /corruptrepo > "$work/corrupt.log" 2>&1; then
    echo 'updater accepted a package archive with a bad integrity digest' >&2
    exit 1
fi
grep -Eiq 'checksum|file integrity|signature' "$work/corrupt.log" || {
    cat "$work/corrupt.log" >&2
    echo 'corrupt package failed for a reason other than package integrity verification' >&2
    exit 1
}
cmp "$work/world-before" "$work/runner/target/etc/apk/world"
cmp "$work/database-before-signature-check" "$work/runner/target/lib/apk/db/installed"
cmp "$work/old-signing-key.pub" "$work/runner/target/etc/apk/keys/$legacy_key"
if [ -n "${PRIOR_BAD_REPOSITORY:-}" ]; then
    if run_update /badrepo > "$work/bad.log" 2>&1; then
        echo 'expected the pre-existing package conflict to fail' >&2
        exit 1
    fi
    grep 'trying to overwrite usr/bin/ar owned by binutils' "$work/bad.log"
    grep 'trying to overwrite usr/bin/strings owned by binutils' "$work/bad.log"
fi
run_update /newrepo
LC_ALL=C sort "$work/world-after-rename" > "$work/world-after-rename.sorted"
LC_ALL=C sort "$work/runner/target/etc/apk/world" > "$work/world-after-update.sorted"
cmp "$work/world-after-rename.sorted" "$work/world-after-update.sorted"
cmp "$work/old-signing-key.pub" "$work/runner/target/etc/apk/keys/$legacy_key"
for tool in ar strings; do
    if [ -f "$old/usr/bin/$tool" ] && [ ! -L "$old/usr/bin/$tool" ]; then
        cmp "$old/usr/bin/$tool" "$work/runner/target/usr/bin/$tool"
    fi
done
printf 'local-admin-setting=yes\n' | cmp - "$work/runner/target/etc/leonos/upgrade-fixture.conf"
printf 'canonical-local-setting=yes\n' | cmp - "$work/runner/target/etc/reliefos/upgrade-fixture.conf"
printf 'external package preserved\n' | cmp - "$work/runner/target/usr/share/external-fixture/data"
awk '/^P:/{p=$0} /^V:/{if(p=="P:external-fixture")print}' "$work/runner/target/lib/apk/db/installed" | cmp - "$work/external-before"
new_version=$(sed -n 's/^  "version": "\([^"]*\)",/\1/p' "$out/packages/apk/manifest.json")
awk -v wanted="$new_version" '/^P:/{p=substr($0,3)} /^V:/{if(p~/^reliefos-/ && substr($0,3)!=wanted)exit 1}' "$work/runner/target/lib/apk/db/installed"
if awk '/^P:/{if(substr($0,3)~/^leonos-/) found=1} END {exit !found}' "$work/runner/target/lib/apk/db/installed"; then
    echo 'legacy local APK packages remain installed after the ReliefOS upgrade' >&2
    exit 1
fi
unshare -Ur "$apk" --root "$work/runner/target" info --exists reliefos-apps
if unshare -Ur "$apk" --root "$work/runner/target" info --exists leonos-apps; then
    echo 'obsolete leonos-apps package remains installed after replacement' >&2
    exit 1
fi
awk '
    /^P:/ { package=substr($0,3); in_apps=(package=="reliefos-apps"); replaced=0; app_dir=0 }
    in_apps && /^r:leonos-apps$/ { replaced=1 }
    in_apps && /^F:usr\/lib\/reliefos$/ { app_dir=1 }
    in_apps && /^R:libreliefos\.so\.2$/ && app_dir { owns_runtime=1 }
    /^$/ { if (in_apps && replaced && owns_runtime) found=1; in_apps=0 }
    END { if (found) exit 0; exit 1 }
' "$work/runner/target/lib/apk/db/installed" || {
    echo 'ReliefOS APK database does not record replacement and ownership of the canonical runtime library' >&2
    exit 1
}
cmp "$work/legacy-locale-before" "$work/runner/target/etc/reliefos/locale.conf"
printf 'APK upgrade: unsigned repository rejection, real ReliefOS package replacements, external package, world and local configuration verified\n'
