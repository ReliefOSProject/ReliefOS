#!/bin/sh
# Contract for the staged authentication chain: the Linux UAPI headers and
# libxcrypt built by tools/build/auth-upstream.sh.
#
# What this suite deliberately does NOT do is pretend Linux-PAM is migrated: its
# upstream build system is Meson-only, and the absence of libpam.so.0 from the
# staged tree is asserted here so the gap cannot be quietly forgotten.
set -u

LC_ALL=C
export LC_ALL

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
cd "$repo_root" || exit 1

deps=${RELIEFOS_DEPS:-${LEONOS_DEPS:-$repo_root/tools/host/gen/reliefos-deps}}
[ -x "$deps" ] || deps=$(find "$repo_root/out" -path '*/host/bin/reliefos-deps' -type f | head -n 1)
lock=$repo_root/configs/dependencies.lock.json
auth_script=$repo_root/tools/build/auth-upstream.sh

work=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-auth.XXXXXX") || exit 1
O="$work/out"
failures=0
checks=0

cleanup() {
    [ -n "${KEEP_WORK:-}" ] || rm -rf "$work"
}
trap cleanup EXIT INT TERM

pass() { checks=$((checks + 1)); printf 'ok   - %s\n' "$1"; }
fail() {
    checks=$((checks + 1))
    failures=$((failures + 1))
    printf 'FAIL - %s\n' "$1"
    if [ "$#" -gt 1 ]; then
        shift
        printf '       %s\n' "$@"
    fi
}
stage=$O/auth/root

printf '=== staged authentication chain ===\n'
make -s O="$O" leonos-auth >"$work/first.log" 2>&1
status=$?
if [ "$status" -eq 0 ]; then
    pass 'make leonos-auth builds the stage from a clean output directory'
else
    fail 'make leonos-auth builds the stage from a clean output directory' \
        "status $status"
    tail -n 20 "$work/first.log" | sed 's/^/       | /'
fi

for artifact in usr/include/crypt.h usr/include/linux/openat2.h \
        lib/libcrypt.so.2 share/licenses/libxcrypt/LICENSE \
        share/licenses/linux-headers/LICENSE; do
    if [ -e "$stage/$artifact" ]; then
        pass "$artifact is a declared, present product"
    else
        fail "$artifact is a declared, present product" "missing under $stage"
    fi
done

if [ ! -e "$stage/lib/libpam.so.0" ] && [ ! -e "$stage/usr/include/security/pam_appl.h" ]; then
    pass 'Linux-PAM is honestly absent (Meson-only upstream, hand port pending)'
else
    fail 'Linux-PAM is honestly absent (Meson-only upstream, hand port pending)' \
        'a PAM artifact appeared in a stage that cannot build PAM without Meson'
fi

# A staged library has to be the one the runtime will link against: a SONAME the
# dynamic loader can resolve, and exported crypt() entry points.
soname=$(readelf -d "$stage/lib/libcrypt.so.2" 2>/dev/null |
        sed -n 's/.*SONAME.*\[\(.*\)\].*/\1/p')
if [ "$soname" = libcrypt.so.2 ]; then
    pass 'libcrypt.so.2 carries the SONAME the runtime links against'
else
    fail 'libcrypt.so.2 carries the SONAME the runtime links against' \
        "SONAME='${soname:-unreadable}'"
fi
needed=$(readelf -d "$stage/lib/libcrypt.so.2" 2>/dev/null |
        sed -n 's/.*NEEDED.*\[\(.*\)\].*/\1/p' | tr '\n' ' ')
# musl's own SONAME is plain `libc.so`; `libc.so.1` is the glibc name, and a
# staged library that needed it would be linking against the host.
printf '%s\n' "$needed" | grep -qw 'libc.so' ||
    fail 'libcrypt.so.2 needs the musl shared library (libc.so)' "NEEDED: $needed"
printf '%s\n' "$needed" | grep -qw 'libgcc_s.so.1' &&
    fail 'libcrypt.so.2 must not pull in the host libgcc' "NEEDED: $needed"
pass "libcrypt.so.2 dependencies are target-only ($needed)"

# Versioned symbols print as `crypt@@XCRYPT_2.0`, so compare bare names.
symbols=$(llvm-nm -D --defined-only "$stage/lib/libcrypt.so.2" 2>/dev/null |
        awk '{ print $NF }' | sed 's/@.*//' | LC_ALL=C sort)
for symbol in crypt crypt_r crypt_gensalt crypt_gensalt_rn crypt_checksalt; do
    if printf '%s\n' "$symbols" | grep -qx -- "$symbol"; then
        pass "libcrypt exports $symbol"
    else
        fail "libcrypt exports $symbol" 'not in the dynamic symbol table'
    fi
done

printf '\n=== the stage is reproducible and the inputs are pinned ===\n'
objects_first=$work/objects-first
find "$O/third-party/auth/build" -name '*.o' -printf '%p %T@\n' | LC_ALL=C sort \
    >"$objects_first"
before=$(find "$stage" -name '*.so*' -o -name 'crypt.h' | LC_ALL=C sort |
        xargs sha256sum 2>/dev/null)
make -s O="$O" leonos-auth >"$work/second.log" 2>&1
after=$(find "$stage" -name '*.so*' -o -name 'crypt.h' | LC_ALL=C sort |
        xargs sha256sum 2>/dev/null)
if [ "$before" = "$after" ]; then
    pass 'repeating the stage produces identical bytes'
else
    fail 'repeating the stage produces identical bytes'
    diff <(printf '%s\n' "$before") <(printf '%s\n' "$after") | sed 's/^/       | /'
fi
# The recipe re-runs by design (the stamp is promoted through a signature that
# FORCE re-checks), so "does no work" has to mean what it measures: upstream make
# recompiled nothing. Object timestamps are the fact; Make's log is not.
after_objects=$work/objects-second
find "$O/third-party/auth/build" -name '*.o' -printf '%p %T@\n' | LC_ALL=C sort \
    >"$after_objects"
if cmp -s "$objects_first" "$after_objects"; then
    pass 'the second run recompiles no object at all'
else
    fail 'the second run recompiles no object at all' \
        "$(cmp "$objects_first" "$after_objects" 2>&1 | head -1)"
fi

printf '\n=== the staged library does not depend on where it was built ===\n'
# libxcrypt expands __FILE__ into its binary, so an unmapped build directory
# leaks into .rodata and shifts every later section: two output trees would then
# produce different bytes for the same configuration. That is the failure mode
# this checks, and it is why the adapter passes -fmacro-prefix-map.
other="$work/nested/deeper/tree/out"
make -s O="$other" leonos-auth >"$work/other.log" 2>&1
other_status=$?
if [ "$other_status" -ne 0 ]; then
    fail 'the stage builds at a second, deeper output path' "status $other_status"
    tail -n 12 "$work/other.log" | sed 's/^/       | /'
fi
for lib in libcrypt.so.2.0.0 libcrypt.a; do
    a="$stage/lib/$lib"
    b="$other/auth/root/lib/$lib"
    if [ ! -f "$a" ] || [ ! -f "$b" ]; then
        fail "$lib is staged in both trees" "missing $( [ -f "$a" ] || echo $a )$( [ -f "$b" ] || echo " $b" )"
        continue
    fi
    if cmp -s "$a" "$b"; then
        pass "$lib is byte-identical across two output paths"
    else
        fail "$lib is byte-identical across two output paths" \
            "$(cmp "$a" "$b" 2>&1 | head -1)"
    fi
done

printf '\n=== the stage refuses inputs it cannot honour ===\n'
# A tampered archive must be refused, not silently unpacked. The cache is a
# private copy: the archives are symlinked in, then the one under test is
# replaced by a real file before a byte is appended, so the shared
# cache/downloads tree is never written to.
fake="$work/cache"
mkdir -p "$fake"
for id in linux-headers libxcrypt; do
    url=$("$deps" --lock "$lock" --id "$id" --print url)
    ln -s "$repo_root/cache/downloads/${url##*/}" "$fake/${url##*/}"
done
cp "$fake/libxcrypt-4.5.2.tar.xz" "$fake/tampered"
mv "$fake/tampered" "$fake/libxcrypt-4.5.2.tar.xz"
printf 'x' >>"$fake/libxcrypt-4.5.2.tar.xz"
make -s O="$work/tampered" LEONOS_CACHE="$fake" leonos-auth >"$work/tampered.log" 2>&1
tampered_status=$?
if [ "$tampered_status" -ne 0 ] && grep -q 'digest mismatch' "$work/tampered.log"; then
    pass 'a cache archive whose bytes changed is refused before unpacking'
else
    fail 'a cache archive whose bytes changed is refused before unpacking' \
        "status $tampered_status: $(tail -c 200 "$work/tampered.log")"
fi
if [ -f "$repo_root/cache/downloads/libxcrypt-4.5.2.tar.xz" ] &&
        [ "$(sha256sum "$repo_root/cache/downloads/libxcrypt-4.5.2.tar.xz" |
                cut -d' ' -f1)" = "$("$deps" --lock "$lock" --id libxcrypt --print sha256)" ]; then
    pass 'the shared download cache is untouched by the tampered run'
else
    fail 'the shared download cache is untouched by the tampered run' \
        'cache/downloads no longer matches the lock file'
fi

printf '\n=== the stage leaves the output tree as the layout declares ===\n'
# The kernel Makefile reads `O=` for its own out-of-tree build, and that variable
# reaches it through MAKEFLAGS from `make O=...` here. If the adapter does not name
# its own build directory, a whole kernel tree appears inside this project's
# output directory -- so the top level of O is asserted against the layout rather
# than trusted.
# Native Kbuild also creates its declared object directories while parsing the
# product graph, even when this invocation only builds authentication inputs.
allowed="auth config generated host images include logs meta musl obj packages pam scripts stage sysroot system tests third-party tools userland userland-installer"
stray=''
for entry in "$O"/*; do
    name=${entry##*/}
    case " $allowed " in
        *" $name "*) ;;
        *) stray="$stray $name" ;;
    esac
done
if [ -z "$stray" ]; then
    pass 'the output directory holds only the declared layout entries'
else
    fail 'the output directory holds only the declared layout entries' \
        "stray:$stray"
fi

# `make clean` is only safe because of the ownership marker, and only useful if a
# build goal actually writes it: a tree that clean refuses to delete is a leak.
if [ -f "$O/.reliefos-out" ]; then
    pass 'a build goal leaves the ownership marker clean needs to recognise the tree'
else
    fail 'a build goal leaves the ownership marker clean needs to recognise the tree' \
        'missing .reliefos-out'
fi
make -s O="$O" clean >"$work/clean.log" 2>&1
clean_status=$?
if [ "$clean_status" -eq 0 ] && [ ! -e "$O/auth" ] && [ -d "$O" ]; then
    pass 'make clean removes the staged tree and keeps the directory'
else
    fail 'make clean removes the staged tree and keeps the directory' \
        "status $clean_status: $(tail -2 "$work/clean.log" | head -1)"
fi

printf '\n%s: %d checks, %d failures\n' 'test-auth-stage' "$checks" "$failures"
[ "$failures" -eq 0 ] || exit 1
exit 0
