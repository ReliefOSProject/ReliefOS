#!/bin/sh
# Verified build-only blkid/uuid inputs for exfatprogs; never a guest overlay.
set -eu
LC_ALL=C
export LC_ALL
die() { printf 'util-linux-development: %s\n' "$*" >&2; exit 1; }
[ "$#" = 6 ] || { echo 'usage: util-linux-development.sh SRC DEPS LOCK CACHE APK ROOT' >&2; exit 2; }
src=$(CDPATH='' cd -- "$1" && pwd -P)
deps=$(CDPATH='' cd -- "$(dirname "$2")" && pwd -P)/$(basename "$2")
lock=$(CDPATH='' cd -- "$(dirname "$3")" && pwd -P)/$(basename "$3")
cache=$(CDPATH='' cd -- "$4" && pwd -P)
apk=$(CDPATH='' cd -- "$(dirname "$5")" && pwd -P)/$(basename "$5")
name=$(basename "$6")
case $name in ''|.|..|/) die 'expected a dedicated output directory';; esac
mkdir -p "$(dirname "$6")"
parent=$(CDPATH='' cd -- "$(dirname "$6")" && pwd -P)
root=$parent/$name
for input in "$src" "$deps" "$lock" "$cache" "$apk" \
    "$src/resources/licenses/util-linux-README.licensing" "$src/system/rootfs/etc/apk/keys"; do
    case $input in "$root"|"$root"/*|"$root.files") die "output overlaps input: $input";; esac
done
case $root in "$cache"/*) die 'output is inside the download cache';; esac
for output in "$root" "$root.files"; do [ ! -L "$output" ] || die "symlink output: $output"; done
[ ! -e "$root" ] || [ -d "$root" ] || die 'output is not a directory'
[ ! -e "$root.files" ] || [ -f "$root.files" ] || die 'inventory is not a regular file'
[ -s "$src/resources/licenses/util-linux-README.licensing" ] || die 'missing util-linux license resource'
transaction=$(mktemp -d "$parent/.${name}.new.XXXXXX")
stage=$transaction/root
installed=no manifest_installed=no committed=no
cleanup() {
    status=$?
    trap - 0 HUP INT TERM
    if [ "$committed" = no ]; then
        if [ "$manifest_installed" = yes ]; then rm -f "$root.files" || status=1; fi
        if [ "$installed" = yes ]; then rm -rf "$root" || status=1; fi
        if [ -e "$transaction/previous-root" ]; then mv "$transaction/previous-root" "$root" || status=1; fi
        if [ -e "$transaction/previous-files" ]; then mv "$transaction/previous-files" "$root.files" || status=1; fi
        if [ -e "$transaction/previous-root" ] || [ -e "$transaction/previous-files" ]; then
            echo "util-linux-development: retained backup: $transaction" >&2
            exit 1
        fi
    fi
    rm -rf "$transaction" || status=1
    exit "$status"
}
trap cleanup 0
trap 'exit 130' HUP INT TERM
mkdir -p "$stage/usr/share/reliefos/sdk/apk"
for id in alpine-libblkid alpine-libuuid alpine-libeconf \
    runtime-dev-util-linux-dev runtime-dev-util-linux-static \
    runtime-dev-libeconf-dev runtime-dev-libeconf-static; do
    case $id in alpine-*) package=${id#alpine-};; runtime-dev-*) package=${id#runtime-dev-};; esac
    url=$("$deps" --lock "$lock" --id "$id" --print url)
    digest=$("$deps" --lock "$lock" --id "$id" --print sha256)
    version=$("$deps" --lock "$lock" --id "$id" --print version)
    [ "$url" = "https://dl-cdn.alpinelinux.org/alpine/edge/main/x86_64/$package-$version.apk" ] || die "unexpected URL: $id"
    [ "${#digest}" = 64 ] || die "invalid digest: $id"
    case $digest in *[!0-9a-f]*) die "invalid digest: $id";; esac
    archive=$cache/${url##*/}
    [ -f "$archive" ] || die "missing $archive; run make fetch"
    cp "$archive" "$transaction/input.apk"
    archive=$transaction/input.apk
    actual=$(sha256sum "$archive")
    [ "${actual%% *}" = "$digest" ] || die "checksum mismatch: $id"
    "$apk" --keys-dir "$src/system/rootfs/etc/apk/keys" verify "$archive"
    tar --ignore-zeros --warning=no-unknown-keyword --numeric-owner \
        --quoting-style=escape -tvzf "$archive" >"$transaction/list"
    awk '
        function safe(path,n,a,i) {
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
                if (NF!=8 || $7!="->" || $8 !~ /^[A-Za-z0-9_+@.-]+$/ || $8=="." || $8=="..") exit 1
            } else if (NF!=6) exit 1
            print type, name, (type=="l" ? $8 : "-"); count++
        }
        END {if (!count) exit 1}
    ' "$transaction/list" >"$transaction/members" || die "unsafe archive: $id"
    while read -r type member target; do
        [ "$type" != d ] || continue
        case $id:$member in
            *:.PKGINFO) destination=usr/share/reliefos/sdk/apk/$id.PKGINFO;;
            alpine-libblkid:usr/lib/libblkid.so*|alpine-libuuid:usr/lib/libuuid.so*|alpine-libeconf:usr/lib/libeconf.so*|\
            runtime-dev-util-linux-dev:usr/include/blkid/*|runtime-dev-util-linux-dev:usr/include/uuid/*|\
            runtime-dev-util-linux-dev:usr/lib/libblkid.so|runtime-dev-util-linux-dev:usr/lib/libuuid.so|\
            runtime-dev-util-linux-dev:usr/lib/pkgconfig/blkid.pc|runtime-dev-util-linux-dev:usr/lib/pkgconfig/uuid.pc|\
            runtime-dev-util-linux-static:usr/lib/libblkid.a|runtime-dev-util-linux-static:usr/lib/libuuid.a|\
            runtime-dev-libeconf-dev:usr/include/libeconf*.h|runtime-dev-libeconf-dev:usr/lib/libeconf.so|\
            runtime-dev-libeconf-dev:usr/lib/pkgconfig/libeconf.pc|runtime-dev-libeconf-static:usr/lib/libeconf.a)
                destination=$member;;
            *) continue;;
        esac
        [ ! -e "$stage/$destination" ] && [ ! -L "$stage/$destination" ] || die "duplicate product: $destination"
        mkdir -p "$stage/$(dirname "$destination")"
        if [ "$type" = l ]; then
            case ${member##*/}:$target in
                libblkid.so*:libblkid.so*|libuuid.so*:libuuid.so*|libeconf.so*:libeconf.so*) ;;
                *) die "unexpected library link: $member";;
            esac
            ln -s "$target" "$stage/$destination"
        else
            tar --ignore-zeros --warning=no-unknown-keyword --occurrence=1 \
                -xOzf "$archive" -- "$member" >"$stage/$destination"
        fi
    done <"$transaction/members"
    awk -v package="$package" -v version="$version" '
        $0=="pkgname = "package {name++} /^pkgname = / {names++}
        $0=="pkgver = "version {ver++} /^pkgver = / {versions++}
        $0=="arch = x86_64" {arch++} /^arch = / {arches++}
        END {exit !(name==1 && names==1 && ver==1 && versions==1 && arch==1 && arches==1)}
    ' "$stage/usr/share/reliefos/sdk/apk/$id.PKGINFO" || die "unexpected package metadata: $id"
done
find "$stage" -type l -print >"$transaction/links"
while IFS= read -r link; do
    [ -f "$link" ] || die "dangling or non-file product link: $link"
done <"$transaction/links"
for header in blkid/blkid.h uuid/uuid.h libeconf.h; do
    [ -s "$stage/usr/include/$header" ] || die "missing header: $header"
done
for lib in blkid uuid econf; do
    [ -s "$stage/usr/lib/lib$lib.a" ] && [ -s "$stage/usr/lib/lib$lib.so" ] || die "missing library: $lib"
done
for pc in blkid uuid libeconf; do
    [ -s "$stage/usr/lib/pkgconfig/$pc.pc" ] || die "missing pkg-config input: $pc"
    # Keep all four paths relocatable, including util-linux's absolute fields.
    awk '
        /^prefix=/ {print "prefix=${pcfiledir}/../.."; next}
        /^exec_prefix=/ {print "exec_prefix=${prefix}"; next}
        /^libdir=/ {print "libdir=${prefix}/lib"; next}
        /^includedir=/ {print "includedir=${prefix}/include"; next}
        {print}
    ' "$stage/usr/lib/pkgconfig/$pc.pc" >"$transaction/pc"
    mv "$transaction/pc" "$stage/usr/lib/pkgconfig/$pc.pc"
done
mkdir -p "$stage/usr/share/licenses/util-linux"
cp "$src/resources/licenses/util-linux-README.licensing" "$stage/usr/share/licenses/util-linux/README.licensing"
printf '%s\n' 'official util-linux development inputs (build-only)' >"$stage/.complete"
(cd "$stage" && find . ! -type d -print) >"$transaction/paths"
sed 's@^./@@' "$transaction/paths" >"$transaction/relative"
sort "$transaction/relative" >"$transaction/root.files"
if [ -e "$root" ]; then mv "$root" "$transaction/previous-root"; fi
if [ -e "$root.files" ]; then mv "$root.files" "$transaction/previous-files"; fi
installed=yes
mv "$stage" "$root"
manifest_installed=yes
mv "$transaction/root.files" "$root.files"
committed=yes
