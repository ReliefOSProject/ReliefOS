#!/bin/sh
# Pinned configure/Make packages. Work and publication are private to O.
set -eu
MAKEFLAGS=${MAKEFLAGS-}
case ${MAKEFLAGS%% *} in *n*) exit 0 ;; esac
[ "$#" = 12 ] || { echo 'usage: upstream.sh PACKAGE SRC DEPS LOCK CACHE WORK STAGE MUSL AUTH PAM CC TARGET' >&2; exit 2; }
pkg=$1 src=$2 deps=$3 lock=$4 cache=$5 work=$6 stage=$7 musl=$8 auth=$9
shift 9
pam=$1 cc=$2 target=$3
mkdir -p "$work" "$(dirname "$stage")"
get() { "$deps" --lock "$lock" --id "$pkg" --print "$1"; }
url=$(get url) digest=$(get sha256) directory=$(get directory)
archive=$cache/${url##*/}
[ -f "$archive" ] || { echo "missing $pkg archive; run make fetch" >&2; exit 1; }
[ "$(sha256sum "$archive" | cut -d' ' -f1)" = "$digest" ] || { echo "$pkg: archive digest mismatch" >&2; exit 1; }
# Archives are pinned byte-for-byte; reject unsafe member names before unpacking.
tar -tf "$archive" > "$work/members"
if LC_ALL=C grep -E '(^/|(^|/)\.\.(/|$))' "$work/members"; then exit 1; fi
rm -rf "$work/source" "$work/build"
mkdir -p "$work/source" "$work/build"
tar -xf "$archive" -C "$work/source" --no-same-owner
source=$work/source/$directory
# Keep the pinned ALSA direct-plugin behavior reproducible for ReliefOS's
# single physical HDA endpoint leases.  The patch is applied only after the
# verified archive has been unpacked and before configure compiles it.
if [ "$pkg" = alsa-lib ]; then
    patch -d "$source" -p1 < "$src/patches/alsa-lib/0001-reliefos-dmix-server.patch"
fi
resource=$("$cc" -print-resource-dir)
CC="$cc --target=$target --sysroot=$musl --gcc-toolchain=/nonexistent -fuse-ld=lld --rtlib=compiler-rt --unwindlib=none -nostdinc -isystem $musl/include -isystem $resource/include -idirafter $auth/usr/include -L$musl/lib"
case $pkg in libbsd|shadow) CC="$cc --target=$target --sysroot=$musl --gcc-toolchain=/nonexistent -fuse-ld=lld --rtlib=compiler-rt --unwindlib=none -nostdinc -idirafter $musl/include -isystem $resource/include -idirafter $auth/usr/include -L$musl/lib" ;; esac
AR=llvm-ar RANLIB=llvm-ranlib STRIP=llvm-strip BUILD_CC=cc
CFLAGS='-O2 -std=gnu17 -mno-avx -mno-avx2'
CPPFLAGS="-I$auth/usr/include -I$pam/usr/include"
LDFLAGS="-Wl,-z,relro,-z,now -L$auth/lib -L$auth/usr/lib -L$pam/lib -Wl,-rpath-link,$auth/lib -Wl,-rpath-link,$pam/lib"
LIBS= PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$auth/lib/pkgconfig:$auth/usr/lib/pkgconfig:$pam/lib/pkgconfig" PKG_CONFIG_SYSROOT_DIR="$auth"
export CC AR RANLIB STRIP BUILD_CC CFLAGS CPPFLAGS LDFLAGS LIBS PKG_CONFIG_PATH PKG_CONFIG_LIBDIR PKG_CONFIG_SYSROOT_DIR
MAKEFLAGS=${MAKEFLAGS%% -- *}; export MAKEFLAGS; unset MAKEOVERRIDES
set -- --host="$target" --prefix=/usr --sysconfdir=/etc --localstatedir=/var --libdir=/usr/lib
case $pkg in
libmd|libbsd) set -- "$@" --libdir=/lib ;;
sudo) set -- "$@" --with-pam --with-pam-login --with-secure-path=/usr/sbin:/usr/bin:/sbin:/bin --with-rundir=/run/sudo --with-vardir=/var/lib/sudo --libexecdir=/usr/lib ;;
shadow) set -- "$@" --bindir=/bin --sbindir=/sbin --with-libpam --with-yescrypt --without-su --disable-logind ;;
util-linux) CPPFLAGS="$CPPFLAGS -include linux/openat2.h"; export CPPFLAGS
 # Without libudev, lsblk probes the device directly. Upstream's flags omit
 # VERSION even though -f requests FSVER; request it from the real superblock.
 awk '
 /BLKID_SUBLKS_TYPE\);/ {sub(/BLKID_SUBLKS_TYPE\);/, "BLKID_SUBLKS_TYPE | BLKID_SUBLKS_VERSION);"); changed++}
 {print}
 END {if (changed != 1) exit 1}
 ' "$source/misc-utils/lsblk-properties.c" > "$source/misc-utils/lsblk-properties.c.new"
 mv "$source/misc-utils/lsblk-properties.c.new" "$source/misc-utils/lsblk-properties.c"
 set -- "$@" --sbindir=/usr/sbin --disable-all-programs --enable-su --enable-runuser --enable-libuuid --enable-libfdisk --enable-libsmartcols --enable-fdisks=check --enable-libblkid --enable-libmount --enable-mount --enable-blkid --enable-lsblk --enable-fsck --without-python --without-systemd --without-systemdsystemunitdir --disable-makeinstall-chown ;;
e2fsprogs) LDFLAGS="-static -L$auth/usr/lib"; export LDFLAGS
 set -- "$@" --sbindir=/usr/sbin --with-root-prefix=/usr --disable-libuuid --disable-libblkid --disable-elf-shlibs --disable-fsck --disable-uuidd --disable-nls --disable-fuse2fs --without-libarchive --with-udev-rules-dir=no --with-systemd-unit-dir=no --with-crond-dir=no ;;
dosfstools) LDFLAGS="-static -L$auth/usr/lib"; export LDFLAGS; set -- "$@" --sbindir=/usr/sbin --enable-compat-symlinks ;;
exfatprogs) LDFLAGS="-static -L$auth/usr/lib"; export LDFLAGS; set -- "$@" --sbindir=/usr/sbin --disable-shared --enable-static ;;
alsa-lib) set -- "$@" --enable-shared --disable-static --disable-python ;;
alsa-utils) set -- "$@" --disable-alsatest --enable-alsa-topology --disable-alsamixer --disable-bat \
 --disable-alsaconf --disable-alsaloop --disable-nhlt --disable-nls \
 --disable-xmlto --disable-rst2man --disable-rpath \
 --with-udev-rules-dir=/usr/lib/udev/rules.d ;;
*) echo "unsupported upstream package: $pkg" >&2; exit 2 ;;
esac
tmp=$(mktemp -d "$stage.new.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
cd "$work/build"
"$source/configure" "$@"
if [ "$pkg" = exfatprogs ]; then make "LDFLAGS=$LDFLAGS -all-static" "BLKID_LIBS=$auth/usr/lib/libblkid.a"; else make; fi
if [ "$pkg" = sudo ]; then make install "DESTDIR=$tmp" INSTALL_OWNER=; else make install "DESTDIR=$tmp"; fi
# alsa-lib 1.2.14 forbids shared/static in one configure (upstream INSTALL).
# Use two private builds of the same verified source; publish just the static
# archive from the second build alongside the complete shared installation.
if [ "$pkg" = alsa-lib ]; then
    rm -rf "$work/build-static"
    mkdir -p "$work/build-static"
    cd "$work/build-static"
    "$source/configure" --host="$target" --prefix=/usr --sysconfdir=/etc \
        --localstatedir=/var --libdir=/usr/lib --disable-shared --enable-static \
        --disable-python
    make
    cp "$work/build-static/src/.libs/libasound.a" "$tmp/usr/lib/"
    # Installed libtool archives encode /usr/lib and redirect downstream
    # links to the host. Target ELF libraries and pkg-config are sufficient.
    rm "$tmp/usr/lib/libasound.la" "$tmp/usr/lib/libatopology.la"
fi
case $pkg in
libbsd) sed 's@GROUP(/lib/@GROUP(@' "$tmp/lib/libbsd.so" > "$tmp/lib/libbsd.so.new"; mv "$tmp/lib/libbsd.so.new" "$tmp/lib/libbsd.so"; rm "$tmp/lib/libbsd.la" ;;
libmd) rm "$tmp/lib/libmd.la" ;;
esac
license=$(get license_in_source)
mkdir -p "$tmp/usr/share/licenses/$pkg"
cp "$source/$license" "$tmp/usr/share/licenses/$pkg/"
if [ "$pkg" = shadow ] && [ -d "$source/LICENSES" ]; then cp -R "$source/LICENSES" "$tmp/usr/share/licenses/$pkg/"; fi
printf '%s\n' "$pkg $digest $target" > "$tmp/.complete"
if [ -d "$stage.previous" ] && [ ! -e "$stage" ]; then mv "$stage.previous" "$stage"; fi
rm -rf "$stage.previous"
if [ -e "$stage" ]; then mv "$stage" "$stage.previous"; fi
mv "$tmp" "$stage"
rm -rf "$stage.previous"
# No installed paths in these pinned packages contain Make-special characters.
(cd "$stage" && find . \( -type f -o -type l \) ! -name .complete -print | LC_ALL=C sort) > "$work/installed-files"
