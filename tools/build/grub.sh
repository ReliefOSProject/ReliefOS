#!/bin/sh
# Build the locked GNU GRUB firmware platform with its own upstream build rules.
set -eu
[ "$#" = 9 ] || { echo 'usage: grub.sh host|efi|guest DEPS LOCK CACHE WORK STAGE MUSL CC TARGET' >&2; exit 2; }
mode=$1 deps=$2 lock=$3 cache=$4 work=$5 stage=$6 musl=$7 cc=$8 target=$9
repo=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
case $mode in host|efi|guest) ;; *) exit 2;; esac
. "$(CDPATH= cd -- "$(dirname "$0")" && pwd)/upstream-make-environment.sh"
reliefos_upstream_make_environment
get() { "$deps" --lock "$lock" --id grub --print "$1"; }
url=$(get url) digest=$(get sha256) directory=$(get directory) version=$(get version)
archive=$cache/${url##*/}
[ -f "$archive" ] || { echo 'missing grub archive; run make fetch' >&2; exit 1; }
[ "$(sha256sum "$archive" | cut -d' ' -f1)" = "$digest" ] || { echo 'grub: archive digest mismatch' >&2; exit 1; }
mkdir -p "$work" "$(dirname "$stage")"
tar -tf "$archive" > "$work/members"
if LC_ALL=C grep -E '(^/|(^|/)\.\.(/|$))' "$work/members"; then exit 1; fi
rm -rf "$work/source" "$work/build"
mkdir -p "$work/source" "$work/build"
tar -xf "$archive" -C "$work/source" --no-same-owner
source=$work/source/$directory
# Upstream generates extra_deps.lst in the build tree. A source-tree copy would
# shadow that rule through VPATH and leave the build-tree input missing.
patch -d "$source" -p1 < "$repo/patches/grub/0001-explicit-root-for-raw-image-setup.patch"
tmp=$(mktemp -d "$stage.new.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
if [ "$mode" != guest ]; then
    prefix=$stage
    sbindir=$prefix/bin
    CC=${GRUB_HOST_CC:-cc}
    # Upstream appends utility CFLAGS to target flags too. Preserve its -Os:
    # -O2 can place .text.unlikely before the PC core's required first entry.
    CFLAGS='-Os -std=gnu17'
    LDFLAGS=
    set --
else
    prefix=/usr
    sbindir=$prefix/sbin
    resource=$("$cc" -print-resource-dir)
    : "${GRUB_UAPI:?locked Linux UAPI headers are required for guest tools}"
    # configure uses -Werror for header probes; link-only driver arguments must
    # not make every compile-only probe falsely report a missing header.
    CC="$cc --target=$target --sysroot=$musl --gcc-toolchain=/nonexistent -Wno-unused-command-line-argument -fuse-ld=lld --rtlib=compiler-rt --unwindlib=none -nostdinc -isystem $musl/include -isystem $resource/include -idirafter $GRUB_UAPI -L$musl/lib"
    CFLAGS='-Os -std=gnu17'
    LDFLAGS=-static
    set -- --host=x86_64-linux-musl --build=x86_64-pc-linux-gnu --sysconfdir=/etc --localstatedir=/var
fi
BUILD_CC=${GRUB_HOST_CC:-cc}
TARGET_CC=${GRUB_PC_CC:-gcc}
export CC CFLAGS LDFLAGS BUILD_CC TARGET_CC
platform=pc arch=i386
if [ "$mode" = efi ]; then platform=efi; arch=x86_64; fi
cd "$work/build"
"$source/configure" --prefix="$prefix" --bindir="$prefix/bin" --sbindir="$sbindir" \
    --libdir="$prefix/lib" --target="$arch" --with-platform="$platform" --disable-werror \
    --disable-nls --disable-device-mapper --disable-libzfs --disable-efiemu \
    --disable-grub-mkfont --disable-grub-mount "$@"
make -j"${GRUB_JOBS:-4}"
make DESTDIR="$tmp" install
if [ "$mode" != guest ]; then
    # configure uses the final stable prefix; keep the publication atomic.
    installed=$tmp$prefix
    license_dir=$installed/share/licenses/grub
else
    installed=$tmp
    license_dir=$installed/usr/share/licenses/grub
fi
mkdir -p "$license_dir"
cp "$source/COPYING" "$license_dir/"
mkdir -p "$license_dir/patches"
cp "$repo/patches/grub/"*.patch "$license_dir/patches/"
printf 'GNU GRUB %s\nSource: %s\nSHA-256: %s\n' "$version" "$url" "$digest" > "$license_dir/SOURCE"
printf 'Local integration changes are included in patches/ and maintained with ReliefOS tools/build/grub.sh.\n' >> "$license_dir/SOURCE"
printf '%s\n' "$digest $mode" > "$installed/.complete"
# Record every installed input, including non-primary modules and notices, so
# an incremental build cannot silently reuse a stage with a deleted file.
(cd "$installed" && find . \( -type f -o -type l \) \
    ! -name .products ! -name .complete -print | sed 's|^\./||' | LC_ALL=C sort) > "$installed/.products"
if [ -d "$stage.previous" ] && [ ! -e "$stage" ]; then mv "$stage.previous" "$stage"; fi
rm -rf "$stage.previous"
if [ -e "$stage" ]; then mv "$stage" "$stage.previous"; fi
mv "$installed" "$stage"
rm -rf "$stage.previous"
