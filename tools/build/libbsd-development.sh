#!/bin/sh
# Offline libbsd/libmd inputs for shadow. Never overlay on the guest/sysroot.
set -eu
LC_ALL=C
export LC_ALL
die() { printf 'libbsd-development: %s\n' "$*" >&2; exit 1; }
[ "$#" = 6 ] || { printf 'usage: libbsd-development.sh SRC DEPS LOCK CACHE APK ROOT\n' >&2; exit 2; }
src=$(CDPATH='' cd -- "$1" && pwd -P)
deps=$(CDPATH='' cd -- "$(dirname "$2")" && pwd -P)/$(basename "$2")
lock=$(CDPATH='' cd -- "$(dirname "$3")" && pwd -P)/$(basename "$3")
cache=$(CDPATH='' cd -- "$4" && pwd -P)
apk=$(CDPATH='' cd -- "$(dirname "$5")" && pwd -P)/$(basename "$5")
root_name=$(basename "$6")
case $root_name in ''|.|..|/) die 'expected a dedicated output directory';; esac
mkdir -p "$(dirname "$6")"
root_parent=$(CDPATH='' cd -- "$(dirname "$6")" && pwd -P)
root=$root_parent/$root_name
for input in "$src" "$deps" "$lock" "$cache" "$apk" \
    "$src/resources/licenses/libbsd-LICENSE" "$src/system/rootfs/etc/apk/keys"; do
    case $input in "$root"|"$root"/*) die "output overlaps an input: $input";; esac
    [ "$input" != "$root.files" ] || die "manifest overlaps an input: $input"
done
case $root in "$cache"/*) die 'output is inside the download cache';; esac
if [ -L "$root" ] || { [ -e "$root" ] && [ ! -d "$root" ]; }; then
    die "development root is not a real directory: $root"
fi
if [ -L "$root.files" ] || { [ -e "$root.files" ] && [ ! -f "$root.files" ]; }; then
    die "manifest is not a regular file: $root.files"
fi
[ -s "$src/resources/licenses/libbsd-LICENSE" ] || die 'missing libbsd license resource'

transaction=$(mktemp -d "$root_parent/.${root_name}.new.XXXXXX")
stage=$transaction/root
installed=no
manifest_installed=no
committed=no
cleanup() {
    status=$?
    trap - 0 HUP INT TERM
    if [ "$committed" = no ]; then
        if [ "$manifest_installed" = yes ]; then rm -f "$root.files" || status=1; fi
        if [ "$installed" = yes ]; then rm -rf "$root" || status=1; fi
        if [ -e "$transaction/previous-root" ]; then
            mv "$transaction/previous-root" "$root" || status=1
        fi
        if [ -e "$transaction/previous-files" ]; then
            mv "$transaction/previous-files" "$root.files" || status=1
        fi
    fi
    # Preserve the backup if restoring it failed.
    if [ -e "$transaction/previous-root" ] || [ -e "$transaction/previous-files" ]; then
        if [ "$committed" = no ]; then
            printf 'libbsd-development: retained backup: %s\n' "$transaction" >&2
            exit 1
        fi
    fi
    rm -rf "$transaction" || status=1
    exit "$status"
}
trap cleanup 0
trap 'exit 130' HUP INT TERM
mkdir -p "$stage/usr/share/reliefos/sdk/apk"

for id in alpine-libbsd runtime-dev-libbsd-dev runtime-dev-libbsd-static \
    alpine-libmd runtime-dev-libmd-dev; do
    case $id in
        alpine-libbsd) package=libbsd;;
        runtime-dev-libbsd-dev) package=libbsd-dev;;
        runtime-dev-libbsd-static) package=libbsd-static;;
        alpine-libmd) package=libmd;;
        runtime-dev-libmd-dev) package=libmd-dev;;
    esac
    case $package in
        libmd*) channel=edge; version=1.3.0-r0;;
        *) channel=v3.24; version=0.12.2-r0;;
    esac
    url=$("$deps" --lock "$lock" --id "$id" --print url)
    digest=$("$deps" --lock "$lock" --id "$id" --print sha256)
    [ "$url" = "https://dl-cdn.alpinelinux.org/alpine/$channel/main/x86_64/$package-$version.apk" ] || die "unexpected locked URL for $id"
    [ "${#digest}" = 64 ] || die "invalid locked digest for $id"
    case $digest in *[!0-9a-f]*) die "invalid locked digest for $id";; esac
    archive=$cache/${url##*/}
    [ -f "$archive" ] || die "missing $archive; run make fetch"
    # Snapshot the original bytes before verification to avoid cache races.
    cp "$archive" "$transaction/input.apk"
    archive=$transaction/input.apk
    actual=$(sha256sum "$archive")
    [ "${actual%% *}" = "$digest" ] || die "checksum mismatch: $id"
    "$apk" --keys-dir "$src/system/rootfs/etc/apk/keys" verify "$archive"

    # GNU tar escapes unusual names. Reject those, aliases, duplicate members,
    # hardlinks and special files before writing even a selected payload file.
    tar --ignore-zeros --warning=no-unknown-keyword --numeric-owner \
        --quoting-style=escape -tvzf "$archive" >"$transaction/list"
    awk '
        function safe(path, n,a,i) {
            if (path !~ /^[A-Za-z0-9_+@.\/-]+$/ || path ~ /^\//) return 0
            n=split(path,a,"/")
            for (i=1;i<=n;i++) if (a[i]=="" || a[i]=="." || a[i]=="..") return 0
            return 1
        }
        {
            type=substr($1,1,1); name=$6
            if (type=="d") sub(/\/$/,"",name)
            if (!safe(name) || seen[name]++ || (type!="-" && type!="d" && type!="l")) exit 1
            if (type=="l") {
                # Official libbsd links only to a sibling, never an ancestor.
                if (NF!=8 || $7!="->" || $8 !~ /^[A-Za-z0-9_+@.-]+$/ || $8=="." || $8=="..") exit 1
            } else if (NF!=6) exit 1
            print type, name, (type=="l" ? $8 : "-")
            count++
        }
        END {if (!count) exit 1}
    ' "$transaction/list" >"$transaction/members" || die "unsafe archive members: $id"

    while read -r type member target; do
        [ "$type" != d ] || continue
        selected=no
        case $id:$member in
            *:.PKGINFO) selected=yes; destination=usr/share/reliefos/sdk/apk/$id.PKGINFO;;
            alpine-libbsd:usr/lib/libbsd.so.0|alpine-libbsd:usr/lib/libbsd.so.0.12.2|\
            runtime-dev-libbsd-dev:usr/include/bsd/*|runtime-dev-libbsd-dev:usr/lib/libbsd.so|\
            runtime-dev-libbsd-dev:usr/lib/pkgconfig/libbsd.pc|runtime-dev-libbsd-dev:usr/lib/pkgconfig/libbsd-overlay.pc|\
            runtime-dev-libbsd-static:usr/lib/libbsd.a|\
            alpine-libmd:usr/lib/libmd.so.0|alpine-libmd:usr/lib/libmd.so.0.3.0|\
            runtime-dev-libmd-dev:usr/include/md[245].h|runtime-dev-libmd-dev:usr/include/ripemd.h|\
            runtime-dev-libmd-dev:usr/include/rmd160.h|runtime-dev-libmd-dev:usr/include/sha.h|\
            runtime-dev-libmd-dev:usr/include/sha1.h|runtime-dev-libmd-dev:usr/include/sha2.h|\
            runtime-dev-libmd-dev:usr/include/sha256.h|runtime-dev-libmd-dev:usr/include/sha3.h|\
            runtime-dev-libmd-dev:usr/include/sha512.h|runtime-dev-libmd-dev:usr/lib/libmd.a|\
            runtime-dev-libmd-dev:usr/lib/libmd.so|runtime-dev-libmd-dev:usr/lib/pkgconfig/libmd.pc)
                selected=yes; destination=$member;;
        esac
        [ "$selected" = yes ] || continue
        [ ! -e "$stage/$destination" ] && [ ! -L "$stage/$destination" ] || die "duplicate product: $destination"
        mkdir -p "$stage/$(dirname "$destination")"
        if [ "$type" = l ]; then
            case $id:$member:$target in
                alpine-libbsd:usr/lib/libbsd.so.0:libbsd.so.0.12.2|\
                alpine-libmd:usr/lib/libmd.so.0:libmd.so.0.3.0|\
                runtime-dev-libmd-dev:usr/lib/libmd.so:libmd.so.0.3.0) ;;
                *) die "unexpected product symlink: $member";;
            esac
            ln -s "$target" "$stage/$destination"
        else
            # Stdout extraction cannot follow archive links or apply owners,
            # modes, devices or directory metadata from the input archive.
            tar --ignore-zeros --warning=no-unknown-keyword --occurrence=1 \
                -xOzf "$archive" -- "$member" >"$stage/$destination"
        fi
    done <"$transaction/members"
    awk -v package="$package" -v expected_version="$version" '
        $0=="pkgname = "package {name++}
        /^pkgname = / {names++}
        $0=="pkgver = "expected_version {version++}
        /^pkgver = / {versions++}
        $0=="arch = x86_64" {arch++}
        /^arch = / {arches++}
        END {exit !(name==1 && names==1 && version==1 && versions==1 && arch==1 && arches==1)}
    ' "$stage/usr/share/reliefos/sdk/apk/$id.PKGINFO" || die "unexpected package metadata: $id"
done

for product in usr/include/bsd/stdlib.h usr/include/bsd/string.h usr/include/bsd/unistd.h \
    usr/include/bsd/sys/queue.h usr/lib/libbsd.so.0 usr/lib/libbsd.so.0.12.2 \
    usr/lib/libbsd.so usr/lib/libbsd.a usr/lib/pkgconfig/libbsd.pc usr/lib/pkgconfig/libbsd-overlay.pc \
    usr/include/md5.h usr/include/sha256.h usr/include/sha512.h usr/lib/libmd.so.0 \
    usr/lib/libmd.so.0.3.0 usr/lib/libmd.so usr/lib/libmd.a usr/lib/pkgconfig/libmd.pc; do
    [ -s "$stage/$product" ] || die "missing product: $product"
done
[ -L "$stage/usr/lib/libbsd.so.0" ] || die 'missing runtime SONAME symlink'

# This pinned development APK contains a linker script, not a .so symlink.
# Check the complete input before removing its absolute target-library path.
cat >"$transaction/linker-script" <<'DATA'
/* GNU ld script
 * The MD5 functions are provided by the libmd library. */
OUTPUT_FORMAT(elf64-x86-64)
GROUP(/usr/lib/libbsd.so.0.12.2 AS_NEEDED(-lmd))
DATA
cmp -s "$transaction/linker-script" "$stage/usr/lib/libbsd.so" || die 'unexpected libbsd linker script'
sed 's@GROUP(/usr/lib/@GROUP(@' "$transaction/linker-script" >"$stage/usr/lib/libbsd.so"
for pc in libbsd libbsd-overlay libmd; do
    [ "$(sed -n '/^prefix=/p' "$stage/usr/lib/pkgconfig/$pc.pc")" = 'prefix=/usr' ] || die "unexpected $pc.pc prefix"
    # Keep pcfiledir literal: pkg-config expands it at the relocated destination.
    # shellcheck disable=SC2016
    sed 's@^prefix=/usr$@prefix=${pcfiledir}/../..@' "$stage/usr/lib/pkgconfig/$pc.pc" >"$transaction/pc"
    mv "$transaction/pc" "$stage/usr/lib/pkgconfig/$pc.pc"
done

# Consumers search both deps/lib and deps/usr/lib. All aliases stay within this
# build root, including after relocation with the official libmd inputs.
# Alpine does not ship libbsd-ctor.a; do not invent an empty archive for it.
mkdir -p "$stage/lib" "$stage/usr/share/licenses/libbsd"
for library in libbsd.so libbsd.so.0 libbsd.so.0.12.2 libbsd.a \
    libmd.so libmd.so.0 libmd.so.0.3.0 libmd.a; do
    ln -s "../usr/lib/$library" "$stage/lib/$library"
done
cp "$src/resources/licenses/libbsd-LICENSE" "$stage/usr/share/licenses/libbsd/LICENSE"
printf '%s\n' 'official libbsd 0.12.2-r0 / libmd 1.3.0-r0 build-only' >"$stage/.complete"
(cd "$stage" && find . ! -type d -print) >"$transaction/paths"
sed 's@^./@@' "$transaction/paths" >"$transaction/relative-paths"
sort "$transaction/relative-paths" >"$transaction/root.files"

# Prepare both artifacts before publication, and roll back both on any failure.
if [ -e "$root" ]; then mv "$root" "$transaction/previous-root"; fi
if [ -e "$root.files" ]; then mv "$root.files" "$transaction/previous-files"; fi
installed=yes
mv "$stage" "$root"
manifest_installed=yes
mv "$transaction/root.files" "$root.files"
committed=yes
