#!/bin/sh
# Integration contract: real cross-build, ABI and incremental recovery; no target ELF execution.
set -eu
: "${PAM_TEST_SOURCE:?patched Linux-PAM 1.7.3 source required}"
: "${PAM_TEST_SYSROOT:?musl sysroot required}"
: "${PAM_TEST_DEPS:?libcrypt and Linux headers staging root required}"
repo=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
cp -a "$PAM_TEST_SOURCE" "$work/src"
mkdir -p "$work/deps/usr"
ln -s "$PAM_TEST_DEPS/lib" "$work/deps/lib"
cp -as "$PAM_TEST_DEPS/usr/include" "$work/deps/usr/include"
if [ -n "${PAM_TEST_HEADERS:-}" ]; then
    for entry in "$PAM_TEST_HEADERS"/*; do
        name=${entry##*/}
        [ -e "$work/deps/usr/include/$name" ] || ln -s "$entry" "$work/deps/usr/include/$name"
    done
fi
run() {
    make --no-print-directory -f "$repo/tools/build/pam/Makefile" \
        SRC="$work/src" BUILD="$work/build" DESTDIR="$work/root" \
        SYSROOT="$PAM_TEST_SYSROOT" DEPS="$work/deps" KERNEL_HEADERS="$work/deps/usr/include" CC="${CC:-clang}" "$@"
}
run -j4 install
for utility in pam_conv1 hmacfile; do
    readelf -h "$work/build/bin/$utility" >/dev/null
done
for lib in pam pam_misc pamc; do
    readelf -d "$work/root/lib/lib$lib.so.0" | grep -q "SONAME.*lib$lib.so.0"
done
for module in access canonicalize_user debug deny echo env exec faildelay faillock filter ftp group issue keyinit limits listfile localuser loginuid mail mkhomedir motd namespace nologin permit pwhistory rootok securetty setquota shells stress succeed_if time timestamp umask unix usertype warn wheel xauth; do
    test -f "$work/root/lib/security/pam_$module.so"
done
for helper in faillock mkhomedir_helper pwhistory_helper pam_timestamp_check unix_chkpwd pam_namespace_helper; do
    test -x "$work/root/sbin/$helper"
done
readelf -d "$work/root/lib/security/pam_unix.so" | grep -q 'NEEDED.*libcrypt.so.2'
readelf --dyn-syms --wide "$work/root/lib/security/pam_unix.so" | grep -q "UND.*crypt_rn"
readelf --dyn-syms --wide "$work/root/lib/libpam.so.0" | grep -q 'pam_start@@LIBPAM_1.0'
find "$work/build" -type f -printf '%p %T@\n' | sort > "$work/before"
run all
find "$work/build" -type f -printf '%p %T@\n' | sort > "$work/after"
cmp "$work/before" "$work/after"
rm "$work/build/lib/libpam_misc.so.0.82.1"
run all
test -f "$work/build/lib/libpam_misc.so.0.82.1"
printf 'PAM build, ABI, no-op and deleted-output recovery passed\n'
# Command-line flag changes invalidate real objects, not just an outer stamp.
before=$(stat -c %y "$work/build/obj/libpam/pam_start.o")
run all CFLAGS='-O1 -mno-avx -mno-avx2'
after=$(stat -c %y "$work/build/obj/libpam/pam_start.o")
[ "$before" != "$after" ]
find "$work/build" -type f -printf '%p %T@\n' | sort > "$work/before"
run all CFLAGS='-O1 -mno-avx -mno-avx2'
find "$work/build" -type f -printf '%p %T@\n' | sort > "$work/after"
cmp "$work/before" "$work/after"
# Failed compiler replacement must return nonzero and preserve published library.
before=$(sha256sum "$work/build/lib/libpam.so.0.85.1")
if run all CC=/bin/false >"$work/failed.log" 2>&1; then
    echo 'broken compiler incorrectly succeeded' >&2
    exit 1
fi
[ "$before" = "$(sha256sum "$work/build/lib/libpam.so.0.85.1")" ]
printf 'PAM parameter invalidation and failed compiler preservation passed\n'
# A real public-header edit must rebuild consumers without touching unrelated objects.
consumer_before=$(stat -c %y "$work/build/obj/libpam_misc/misc_conv.o")
unrelated_before=$(stat -c %y "$work/build/obj/libpam/pam_start.o")
printf '\n/* dependency regression */\n' >> "$work/src/libpam_misc/include/security/pam_misc.h"
run all CFLAGS='-O1 -mno-avx -mno-avx2'
[ "$consumer_before" != "$(stat -c %y "$work/build/obj/libpam_misc/misc_conv.o")" ]
[ "$unrelated_before" = "$(stat -c %y "$work/build/obj/libpam/pam_start.o")" ]
printf 'PAM header dependency isolation passed\n'
