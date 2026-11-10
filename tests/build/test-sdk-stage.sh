#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)
w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT

mkdir -p "$w/src/include/uapi" "$w/src/include/leonos" "$w/src/include/reliefos" \
    "$w/src/userland/runtime/include/leonos" "$w/src/userland/runtime/include/reliefos" "$w/runtime-deps/usr/include" \
    "$w/runtime-deps/usr/lib" "$w/runtime-deps/usr/share/licenses" "$w/runtime-deps/usr/share/reliefos/sdk/apk" "$w/sysroot/include" "$w/sysroot/lib" \
    "$w/sysroot/share/licenses/musl" "$w/pam/usr/include/security" \
    "$w/pam/lib/pkgconfig" "$w/pam/share/licenses/linux-pam" \
    "$w/auth/usr/include" "$w/auth/lib" "$w/auth/share/licenses/libxcrypt" \
    "$w/export/linux"
printf 'stdio fixture\n' > "$w/sysroot/include/stdio.h"
printf 'crt fixture\n' > "$w/sysroot/lib/crt1.o"
for f in libc.so libc.a libmimalloc.so.3 mimalloc.o Scrt1.o crti.o crtn.o rcrt1.o libssp_nonshared.a; do
    printf '%s\n' "$f" > "$w/sysroot/lib/$f"
done
printf 'license\n' > "$w/sysroot/share/licenses/musl/COPYRIGHT"
printf 'header\n' > "$w/src/include/leonos/api.h"
printf 'legacy header\n' > "$w/src/include/leonos/tar.h"
printf 'canonical header\n' > "$w/src/include/reliefos/api.h"
printf 'canonical header\n' > "$w/src/include/reliefos/tar.h"
# UAPI fixture: the SDK must take ABI headers from the kernel header export
# (headers_install output), never from the kernel-owned source tree.
printf 'uapi fixture\n' > "$w/export/linux/types.h"
printf 'header\n' > "$w/pam/usr/include/security/pam_appl.h"
printf 'header\n' > "$w/auth/usr/include/crypt.h"
printf 'license\n' > "$w/pam/share/licenses/linux-pam/LICENSE"
printf 'license\n' > "$w/auth/share/licenses/libxcrypt/LICENSE"
printf 'so\n' > "$w/pam/lib/libpam.so.0"
printf 'so\n' > "$w/auth/lib/libcrypt.so.2"
printf 'archive\n' > "$w/auth/lib/libcrypt.a"
printf 'reliefos runtime\n' > "$w/reliefos-runtime.so"
printf 'reliefos archive\n' > "$w/reliefos-runtime.a"
printf 'leonos runtime\n' > "$w/leonos-runtime.so"
printf 'leonos archive\n' > "$w/leonos-runtime.a"
printf 'builtins\n' > "$w/builtins.a"
printf 'driver\n' > "$w/driver"
printf 'header\n' > "$w/runtime-deps/usr/include/pnglibconf.h"
printf 'archive\n' > "$w/runtime-deps/usr/lib/libmbedcrypto.a"
printf 'license\n' > "$w/runtime-deps/usr/share/licenses/LICENSE"
printf 'pkgname = mbedtls3\n' > "$w/runtime-deps/usr/share/reliefos/sdk/apk/mbedtls3.PKGINFO"

RUNTIME_BUILTINS="$w/builtins.a" RUNTIME_DEVELOPMENT_ROOT="$w/runtime-deps" \
    sh "$root/tools/build/musl-sdk.sh" "$w/src" "$w/sysroot" \
    "$w/reliefos-runtime.so" "$w/reliefos-runtime.a" \
    "$w/leonos-runtime.so" "$w/leonos-runtime.a" "$w/pam" "$w/auth" "$w/driver" \
    "$w/musl-sdk" 1234 "$w/export"
rm "$w/musl-sdk/include/stdio.h" "$w/musl-sdk/lib/crt1.o"
RUNTIME_BUILTINS="$w/builtins.a" RUNTIME_DEVELOPMENT_ROOT="$w/runtime-deps" \
    sh "$root/tools/build/musl-sdk.sh" "$w/src" "$w/sysroot" \
    "$w/reliefos-runtime.so" "$w/reliefos-runtime.a" \
    "$w/leonos-runtime.so" "$w/leonos-runtime.a" "$w/pam" "$w/auth" "$w/driver" \
    "$w/musl-sdk" 1234 "$w/export"
cmp "$w/export/linux/types.h" "$w/musl-sdk/include/linux/types.h"
cmp "$w/sysroot/include/stdio.h" "$w/musl-sdk/include/stdio.h"
cmp "$w/sysroot/lib/crt1.o" "$w/musl-sdk/lib/crt1.o"
cmp "$w/src/include/reliefos/api.h" "$w/musl-sdk/include/reliefos/api.h"
cmp "$w/src/include/leonos/tar.h" "$w/musl-sdk/include/leonos/tar.h"
grep -Fx 'reliefos runtime' "$w/musl-sdk/lib/libreliefos.so.2"
grep -Fx 'leonos runtime' "$w/musl-sdk/lib/libleonos.so.2"
grep -Fx 'reliefos archive' "$w/musl-sdk/lib/libreliefos.a"
grep -Fx 'leonos archive' "$w/musl-sdk/lib/libleonos.a"
echo 'ok - musl SDK stages both runtime ABIs and keeps canonical and legacy headers'
cmp "$w/runtime-deps/usr/lib/libmbedcrypto.a" "$w/musl-sdk/lib/libmbedcrypto.a"
cmp "$w/runtime-deps/usr/share/reliefos/sdk/apk/mbedtls3.PKGINFO" "$w/musl-sdk/share/reliefos/sdk/apk/mbedtls3.PKGINFO"
