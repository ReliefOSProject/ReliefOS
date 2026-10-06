#!/bin/sh
# Build signed local packages and install them with the verified upstream apk.
set -eu
. "$(CDPATH= cd -- "$(dirname "$0")/../../scripts" && pwd)/logging.sh"

if [ "$#" -ne 12 ]; then
    echo 'usage: apk-stage.sh SRC RAW_ROOT OUTPUT_ROOT WORK APK UPSTREAM DEPS LOCK POLICY OWN_TOOL KEY VERSION' >&2
    exit 2
fi
src=$1 raw=$2 output=$3 work=$4 apk=$5 upstream=$6 deps=$7 lock=$8 policy=$9
shift 9
own=$1 key=$2
shift 2
version=$1

case $version in *[!A-Za-z0-9._+-]*|'') echo "invalid APK version: $version" >&2; exit 2;; esac
for item in "$src" "$raw" "$work" "$apk" "$upstream" "$deps" "$lock" "$policy" "$own"; do
    case $item in *"	"*|*"
"*) echo 'APK paths may not contain tabs or newlines' >&2; exit 2;; esac
done
[ -d "$raw" ] || { echo "raw APK root is missing: $raw" >&2; exit 1; }
[ -x "$apk" ] || { echo "verified apk is missing: $apk" >&2; exit 1; }
[ -x "$deps" ] || { echo "dependency query tool is missing: $deps" >&2; exit 1; }
[ -f "$lock" ] || { echo "dependency lock is missing: $lock" >&2; exit 1; }
[ -x "$own" ] || { echo "ownership tool is missing: $own" >&2; exit 1; }
[ -d "$upstream/packages" ] || { echo "verified upstream package directory is missing: $upstream/packages" >&2; exit 1; }

# The desktop backend marker is a plain single-line value; never source it.
# Unknown, empty, multi-line or missing markers abort before any staging work.
backend_file=$raw/etc/reliefos/desktop-backend
[ -f "$backend_file" ] || { echo "desktop backend marker is missing: $backend_file" >&2; exit 1; }
backend=
backend_lines=0
while IFS= read -r marker_line || [ -n "$marker_line" ]; do
    backend_lines=$((backend_lines + 1))
    [ "$backend_lines" -gt 1 ] || backend=$marker_line
    marker_line=
done <"$backend_file"
[ "$backend_lines" = 1 ] || { echo "desktop backend marker must be exactly one line: $backend_file" >&2; exit 1; }
case $backend in
    reliefos|xorg) ;;
    *) echo "invalid desktop backend marker: $backend_file" >&2; exit 1 ;;
esac

mkdir -p "$work" "$(dirname "$output")"
scratch=$(mktemp -d "$work/.apk-stage.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
tree=$scratch/tree
repository=$scratch/repository
managed=$scratch/managed
mkdir -p "$tree" "$repository" "$managed"
: >"$scratch/upstream-requests"

# Select upstream archives by their lock feature before any staging step can
# see a filtered package: repository copy, raw overlay deletion, ownership
# scan, the APK transaction and the index all consume this list. Selection is
# driven by the dependency lock, never by archive or package names.
: >"$scratch/kept-archives"
for archive in "$upstream"/packages/*.apk; do
    [ -f "$archive" ] || continue
    tar --ignore-zeros -xOf "$archive" .PKGINFO >"$scratch/upstream-pkginfo" 2>/dev/null ||
        { echo "invalid APK metadata: $archive" >&2; exit 1; }
    upstream_name=$(sed -n 's/^pkgname = //p' "$scratch/upstream-pkginfo")
    [ -n "$upstream_name" ] || { echo "invalid APK metadata: $archive" >&2; exit 1; }
    if ! feature=$("$deps" --lock "$lock" --id "alpine-$upstream_name" --print feature); then
        echo "dependency lock entry is missing for APK package: $upstream_name (lock: $lock)" >&2
        exit 1
    fi
    case $feature in
        base) ;;
        xorg) [ "$backend" = xorg ] || continue ;;
        *) echo "invalid dependency feature for APK package: $upstream_name (lock: $lock): $feature" >&2; exit 1 ;;
    esac
    printf '%s\n' "$archive" >>"$scratch/kept-archives"
done
reliefos_log APK "selected $(wc -l <"$scratch/kept-archives") upstream archives for the $backend desktop backend"

cp -a "$raw"/. "$tree"/

# Media-only payloads are copied back after the transaction and never claimed
# by an installed package. Runtime state is always created by apk itself.
for name in EFI grub reliefos leonos loader.elf install; do rm -rf "$tree/$name"; done
for name in lib/apk/db var/cache/apk usr/share/reliefos/apk/repository; do
    [ ! -L "$tree/$name" ] || { echo "invalid package-state symlink: $name" >&2; exit 1; }
    rm -rf "$tree/$name"
done
rm -f "$tree/etc/apk/world" "$tree/var/log/apk.log"
rm -f "$tree/.apk-complete" "$tree/.complete"
sh "$src/tools/build/apk-layout.sh" "$tree" "${APK_LAYOUT_TOOL:-$(dirname "$own")/reliefos-layout}"

# The development headers must describe this exact runtime, not an independently
# downloaded musl. Keep libxcrypt's header/archive in their existing package.
development=$scratch/development
if [ -n "${APK_MUSL_SYSROOT:-}" ]; then
    cmp "$tree/lib/ld-musl-x86_64.so.1" "$APK_MUSL_SYSROOT/lib/libc.so"
    mkdir -p "$development/usr/include" "$development/usr/lib" "$development/usr/share/licenses/reliefos-musl-dev"
    cp -a "$APK_MUSL_SYSROOT/include/." "$development/usr/include/"
    rm -f "$development/usr/include/crypt.h"
    for name in crt1.o Scrt1.o rcrt1.o crti.o crtn.o libc.a libdl.a libm.a libpthread.a libresolv.a librt.a libutil.a libxnet.a libssp_nonshared.a; do
        cp "$APK_MUSL_SYSROOT/lib/$name" "$development/usr/lib/$name"
    done
    ln -s ../../lib/ld-musl-x86_64.so.1 "$development/usr/lib/libc.so"
    cp "$src/third_party/musl/COPYRIGHT" "$src/userland/musl-dev/stack_chk_fail_local.c" "$development/usr/share/licenses/reliefos-musl-dev/"
    cp -a "$development/." "$tree/"
fi

# Files from pinned official archives retain their upstream signatures and
# package ownership. The verified APK archive member list is the authority for
# their file paths; metadata members are not root payload.
while IFS= read -r archive; do
    cp "$archive" "$repository/"
    tar --ignore-zeros -xOf "$archive" .PKGINFO >"$scratch/upstream-pkginfo" 2>/dev/null
    upstream_name=$(sed -n 's/^pkgname = //p' "$scratch/upstream-pkginfo")
    upstream_version=$(sed -n 's/^pkgver = //p' "$scratch/upstream-pkginfo")
    [ -n "$upstream_name" ] && [ -n "$upstream_version" ] || { echo "invalid APK metadata: $archive" >&2; exit 1; }
    printf '%s=%s\n' "$upstream_name" "$upstream_version" >>"$scratch/upstream-requests"
    tar --ignore-zeros -tzf "$archive" >"$scratch/upstream-members" 2>/dev/null
    while IFS= read -r path; do
        case $path in ''|.*|*/) continue;; esac
        case $path in /*|*../*|../*|*"	"*) echo "unsafe upstream APK path: $path" >&2; exit 1;; esac
        rm -f "$tree/$path"
    done <"$scratch/upstream-members"
done <"$scratch/kept-archives"

# These optional applets are unowned fallbacks supplied by the package trigger.
# Shipping them as package files conflicts with an already installed binutils.
for applet in ar strings; do
    if [ -L "$tree/usr/bin/$applet" ]; then
        case $(readlink "$tree/usr/bin/$applet") in */busybox) rm "$tree/usr/bin/$applet" ;; esac
    fi
done
# Only retire the BusyBox alias, never a real Bash executable or another link.
if [ -L "$tree/bin/bash" ]; then
    case $(readlink "$tree/bin/bash") in
        busybox|./busybox|/bin/busybox|../bin/busybox) rm "$tree/bin/bash" ;;
    esac
fi

# Preserve the exact dependency lock alongside the installed tools. The binary
# digest records the actual verified bootstrap executable, not a second pin.
mkdir -p "$tree/usr/share/licenses/apk-tools" "$tree/usr/share/licenses/leonos-openrc"
{
    printf '{"binary_sha256":"%s","modified":false,"dependencies_lock":' "$(sha256sum "$apk" | cut -d' ' -f1)"
    cat "$src/configs/dependencies.lock.json"
    printf '}\n'
} > "$tree/usr/share/licenses/apk-tools/SOURCE.json"
cp "$src/configs/dependencies.lock.json" "$tree/usr/share/licenses/leonos-openrc/SOURCE.json"

mkdir -p "$tree/sbin" "$tree/usr/lib/reliefos" "$tree/usr/share/reliefos" \
    "$tree/etc/apk/keys" "$tree/etc/apk/protected_paths.d"
cp "$apk" "$tree/sbin/apk"
chmod 755 "$tree/sbin/apk"
mkdir -p "$tree/usr/share/licenses/apk-tools"
cp "$src/resources/licenses/apk-tools-LICENSE" "$tree/usr/share/licenses/apk-tools/LICENSE"
cp "$src/userland/storage/leonos-apk-update" "$tree/usr/lib/reliefos/reliefos-apk-update"
chmod 755 "$tree/usr/lib/reliefos/reliefos-apk-update"
ln -sfn reliefos-apk-update "$tree/usr/lib/reliefos/leonos-apk-update"
mkdir -p "$tree/usr/lib/leonos"
ln -sfn ../reliefos/reliefos-apk-update "$tree/usr/lib/leonos/leonos-apk-update"
cp "$policy" "$tree/usr/share/reliefos/apk-ownership.json"
cp -a "$src/system/rootfs/etc/apk/keys"/. "$tree/etc/apk/keys"/
cp "$src/system/rootfs/etc/apk/protected_paths.d/leonos.list" \
    "$tree/etc/apk/protected_paths.d/leonos.list"
cp "$src/system/rootfs/etc/apk/protected_paths.d/reliefos.list" \
    "$tree/etc/apk/protected_paths.d/reliefos.list"

if [ ! -e "$key" ]; then
    mkdir -p "$(dirname "$key")"
    chmod 700 "$(dirname "$key")"
    key_tmp=$(mktemp "$(dirname "$key")/.key.XXXXXX")
    openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$key_tmp" 2>/dev/null
    chmod 600 "$key_tmp"
    if ! ln "$key_tmp" "$key" 2>/dev/null; then [ -f "$key" ] || exit 1; fi
    rm -f "$key_tmp"
fi
[ ! -L "$key" ] || { echo 'APK signing key must not be a symlink' >&2; exit 1; }
[ "$(stat -c '%a' "$key")" = 600 ] || { echo 'APK signing key must have mode 0600' >&2; exit 1; }
openssl pkey -in "$key" -pubout -out "$scratch/public.pem" 2>/dev/null
pub_digest=$(sha256sum "$scratch/public.pem" | cut -d' ' -f1)
pub_name=reliefos-$(printf %.16s "$pub_digest").rsa.pub
legacy_pub_name=leonos-$(printf %.16s "$pub_digest").rsa.pub
cp "$scratch/public.pem" "$tree/etc/apk/keys/$pub_name"
cp "$scratch/public.pem" "$tree/etc/apk/keys/$legacy_pub_name"

if [ -n "${SOURCE_DATE_EPOCH:-}" ]; then
    case $SOURCE_DATE_EPOCH in *[!0-9]*|'') echo 'SOURCE_DATE_EPOCH must be an integer' >&2; exit 2;; esac
    find "$tree" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
fi

"$own" --policy "$policy" --root "$tree" --output "$scratch/ownership.tsv" --installed-policy "$tree/usr/share/reliefos/apk-ownership.json" --elf-list "$scratch/elf.tsv"
if [ -d "$development" ]; then
    "$own" --policy "$policy" --root "$development" --group reliefos-musl-dev --output "$scratch/development.tsv"
    awk -F '\t' -v OFS='\t' 'FNR==NR {if($2!="dir") paths[$4]=1;next} $4 in paths {$1="reliefos-musl-dev"} {print}' \
        "$scratch/development.tsv" "$scratch/ownership.tsv" > "$scratch/ownership.new"
    mv "$scratch/ownership.new" "$scratch/ownership.tsv"
fi
cut -f1 "$scratch/ownership.tsv" | LC_ALL=C sort -u >"$scratch/groups"

# Content identity avoids silently publishing different bytes at one version
# during local rebuilds whose commit timestamp has not changed.
(cd "$tree" && find . -printf '%y %m %p %l\0' | LC_ALL=C sort -z;
 cd "$tree" && find . -type f -exec sha256sum --zero -- {} + | LC_ALL=C sort -z) > "$scratch/content"
content_hex=$(sha256sum "$scratch/content" | cut -c1-15)
content_number=$(printf '%d' "0x$content_hex")
version=$(printf '%s' "$version" | sed "s/-r[0-9]*$/.${content_number}-r0/")

package_name()
{
    case $1 in reliefos-*|ca-certificates-bundle) printf '%s\n' "$1";; *) printf 'reliefos-%s\n' "$1";; esac
}

# Copy exactly the classified payload. Quoting and tab-delimited reads preserve
# spaces; the inventory tool rejects tabs/newlines because this is its format.
reliefos_log APK "copying $(wc -l < "$scratch/ownership.tsv") payload entries"
copied=0
while IFS="$(printf '\t')" read -r group type mode relative target; do
    package=$(package_name "$group")
    destination=$scratch/payload/$package/$relative
    mkdir -p "$(dirname "$destination")"
    if [ "$type" = dir ]; then
        mkdir -p "$destination"
        chmod "$mode" "$destination"
    else
        cp -a "$tree/$relative" "$destination"
    fi
    copied=$((copied + 1))
    if [ $((copied % 2000)) = 0 ]; then reliefos_log APK "copied $copied entries"; fi
done <"$scratch/ownership.tsv"
# Shared parent directories must have the canonical mode in every package.
for payload in "$scratch"/payload/*; do
    find "$payload" -type d -print | while IFS= read -r directory; do
        relative=${directory#"$payload"/}
        [ "$directory" != "$payload" ] || continue
        if [ -d "$tree/$relative" ]; then chmod --reference="$tree/$relative" "$directory"; fi
    done
    find "$payload" -exec touch -h -d "@${SOURCE_DATE_EPOCH:-0}" {} +
done

# Derive real ELF capabilities and dependencies from the package payload.
reliefos_log APK 'scanning ELF dependencies and signing packages'
: >"$scratch/providers"
: >"$scratch/needed"
while IFS= read -r group; do
    package=$(package_name "$group")
    awk -F '\t' -v group="$group" '$1 == group && $2 == "file" { print $4 }' \
        "$scratch/elf.tsv" | while IFS= read -r relative; do
        file=$tree/$relative
        readelf -h "$file" >/dev/null 2>&1 || continue
        readelf -dW "$file" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\([^]]*\)\].*/\1/p' | \
            while IFS= read -r needed; do printf '%s\t%s\n' "$package" "$needed" >>"$scratch/needed"; done
        { readelf -dW "$file" 2>/dev/null | sed -n 's/.*(SONAME).*\[\([^]]*\)\].*/\1/p';
          case ${relative##*/} in *.so|*.so.[0-9]* ) printf '%s\n' "${relative##*/}";; esac; } | \
            while IFS= read -r soname; do [ -n "$soname" ] && printf '%s\t%s\n' "$soname" "$package" >>"$scratch/providers"; done
    done
done <"$scratch/groups"
LC_ALL=C sort -u "$scratch/providers" -o "$scratch/providers"
if cut -f1 "$scratch/providers" | uniq -d | grep . >"$scratch/duplicate-sonames"; then
    echo 'multiple local APK providers for ELF SONAME:' >&2
    cat "$scratch/duplicate-sonames" >&2
    exit 1
fi

run_apk()
{
    if [ "$1" = mkpkg ]; then apk_log=$work/logs/$package.log
    else apk_log=$work/logs/install.log
    fi
    if unshare -Ur true >/dev/null 2>&1; then set -- unshare -Ur "$apk" "$@"
    elif command -v fakeroot >/dev/null 2>&1; then
        if [ "$1" = mkpkg ]; then set -- fakeroot "$apk" "$@"
        else set -- fakeroot "$apk" --usermode --force-no-chroot "$@"
        fi
    else echo 'APK packaging requires user namespaces or fakeroot' >&2; exit 1
    fi
    sh "$src/tools/build/run-logged.sh" --tag APK "$apk_log" "$@"
}

: >"$scratch/local-packages"
while IFS= read -r group; do
    package=$(package_name "$group")
    payload=$scratch/payload/$package
    dependencies=
    if [ -s "$scratch/needed" ]; then
        while IFS="$(printf '\t')" read -r consumer soname; do
            [ "$consumer" = "$package" ] || continue
            provider=$(awk -F '\t' -v so="$soname" '$1 == so { print $2; exit }' "$scratch/providers")
            [ -n "$provider" ] || { echo "unresolved ELF dependency: $package: $soname" >&2; exit 1; }
            [ "$provider" = "$package" ] || dependencies="$dependencies $provider"
        done <"$scratch/needed"
    fi
    provides=$(awk -F '\t' -v package="$package" '$2 == package { printf " so:%s=0", $1 }' "$scratch/providers")
    if [ "$package" = reliefos-musl ] && [ -f "$tree/lib/ld-musl-x86_64.so.1" ]; then
        provides="$provides so:libc.musl-x86_64.so.1=1"
    fi
    if [ "$package" = reliefos-busybox ] && [ -x "$tree/bin/busybox" ] && [ -e "$tree/bin/sh" ]; then
        for applet in init ifup ifdown udhcpc ntpd; do
            "$tree/bin/busybox" --list | grep -Fx "$applet" >/dev/null || { echo "BusyBox is missing required applet: $applet" >&2; exit 1; }
        done
        provides="$provides /bin/sh ifupdown-any"
    fi
    case $package in
        reliefos-musl-dev) dependencies="$dependencies reliefos-musl=$version"; provides="$provides musl-dev=$version libc-dev=$version" ;;
        reliefos-apk-tools) dependencies="$dependencies reliefos-busybox" ;;
        reliefos-trust) dependencies="$dependencies ca-certificates-bundle" ;;
        reliefos-fastfetch) dependencies="$dependencies !fastfetch" ;;
    esac
    replaces=
    case $package in reliefos-*) replaces="leonos-${package#reliefos-}";; esac
    set -- mkpkg --files "$payload" --output "$repository/$package-$version.apk" \
        --info "name:$package" --info "version:$version" --info arch:x86_64 \
        --info "origin:$package" --info "description:ReliefOS build payload $package" \
        --info license:LicenseRef-See-Bundled-Notices --sign-key "$key"
    [ -z "$dependencies" ] || set -- "$@" --info "depends:$(printf '%s' "$dependencies" | xargs -n1 | LC_ALL=C sort -u | xargs)"
    [ -z "$provides" ] || set -- "$@" --info "provides:$(printf '%s' "$provides" | xargs -n1 | LC_ALL=C sort -u | xargs)"
    [ -z "$replaces" ] || set -- "$@" --info "replaces:$replaces"
    if [ "$package" = reliefos-busybox ]; then
        set -- "$@" --script "post-install:$src/userland/storage/busybox-binutils-links" \
            --script "post-upgrade:$src/userland/storage/busybox-binutils-links" \
            --script "trigger:$src/userland/storage/busybox-binutils-links" --trigger /usr/bin
    fi
    run_apk "$@"
    printf '%s\n' "$repository/$package-$version.apk" >>"$scratch/local-packages"
done <"$scratch/groups"

set -- "$apk" mkndx --keys-dir "$tree/etc/apk/keys" --sign-key "$key" \
    --output "$repository/packages.adb"
for archive in "$repository"/*.apk; do set -- "$@" "$archive"; done
sh "$src/tools/build/run-logged.sh" --tag APK "$work/logs/index.log" "$@"

mkdir -p "$managed/etc/apk/keys"
cp -a "$tree/etc/apk/keys"/. "$managed/etc/apk/keys"/
set -- --root "$managed" --arch x86_64 --initdb --repositories-file /dev/null \
    --repository "$repository/packages.adb" add
while IFS= read -r group; do set -- "$@" "$(package_name "$group")"; done <"$scratch/groups"
while IFS= read -r request; do set -- "$@" "$request"; done <"$scratch/upstream-requests"
run_apk "$@"
mkdir -p "$managed/usr/share/reliefos/apk"
cp -a "$repository" "$managed/usr/share/reliefos/apk/repository"
for name in EFI grub reliefos leonos loader.elf install; do [ ! -e "$raw/$name" ] || cp -a "$raw/$name" "$managed/$name"; done

mkdir -p "$work/repository.new"
rm -rf "$work/repository.new"
cp -a "$repository" "$work/repository.new"
rm -rf "$work/repository.previous"
[ ! -e "$work/repository" ] || mv "$work/repository" "$work/repository.previous"
mv "$work/repository.new" "$work/repository"
rm -rf "$work/repository.previous"
cp "$scratch/ownership.tsv" "$work/ownership.tsv.new"
mv "$work/ownership.tsv.new" "$work/ownership.tsv"
{
    printf '{\n  "schema_version": 1,\n  "version": "%s",\n' "$version"
    printf '  "database": "created-by-upstream-apk",\n  "signing_public_key": "%s",\n  "legacy_signing_public_key": "%s",\n' "$pub_name" "$legacy_pub_name"
    printf '  "packages": ['
    separator=
    while IFS= read -r group; do package=$(package_name "$group"); printf '%s"%s"' "$separator" "$package"; separator=', '; done <"$scratch/groups"
    printf '],\n  "ownership_sha256": "%s",\n' "$(sha256sum "$scratch/ownership.tsv" | cut -d' ' -f1)"
    printf '  "repository_sha256": "%s"\n}\n' "$(sha256sum "$repository/packages.adb" | cut -d' ' -f1)"
} >"$work/manifest.json.new"
mv "$work/manifest.json.new" "$work/manifest.json"

rm -rf "$output.previous"
[ ! -e "$output" ] || mv "$output" "$output.previous"
mv "$managed" "$output"
rm -rf "$output.previous"
reliefos_log APK "signed root: $(wc -l <"$scratch/groups") packages -> $output"
