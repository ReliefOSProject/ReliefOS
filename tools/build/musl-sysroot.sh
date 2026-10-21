#!/bin/sh
# Build the pinned musl and mimalloc sysroot the ReliefOS userland links against.
#
# This is the whole of the musl port: verify the pinned commits, unpack them
# into a copy owned by this configuration, apply the audited patches, then let
# upstream's own configure and Makefile do the building. musl ships a POSIX
# configure script and a plain Makefile, so no Meson, Ninja or Python is
# involved anywhere below.
#
# The compiler identity, flags and patch digests arrive through the signature
# that Make already recorded; a change to any of them re-runs this script, which
# is what keeps a stale libc from surviving a toolchain switch.
set -eu

. "$(CDPATH= cd -- "$(dirname "$0")/../../scripts" && pwd)/logging.sh"
case ${MAKEFLAGS%% *} in *n*) exit 0 ;; esac
. "$(CDPATH= cd -- "$(dirname "$0")" && pwd)/upstream-make-environment.sh"
reliefos_upstream_make_environment

die() {
    printf '%s\n' "musl-sysroot: $*" >&2
    exit 1
}

usage() {
    cat >&2 <<'USAGE'
usage: musl-sysroot.sh --src ROOT --deps PATH --lock PATH --work DIR --sysroot DIR
                       --cc EXE --ar EXE --ranlib EXE --ld EXE
                       --target TRIPLE --stack-object FILE --cflags FLAGS [--log FILE] [--print-only]
USAGE
    exit 2
}

src=""
deps=""
lock=""
work=""
sysroot=""
cc=""
ar=""
ranlib=""
ld=""
target=""
stack_object=""
cflags=""
log=""
print_only=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --src) src=$2; shift 2 ;;
        --deps) deps=$2; shift 2 ;;
        --lock) lock=$2; shift 2 ;;
        --work) work=$2; shift 2 ;;
        --sysroot) sysroot=$2; shift 2 ;;
        --cc) cc=$2; shift 2 ;;
        --ar) ar=$2; shift 2 ;;
        --ranlib) ranlib=$2; shift 2 ;;
        --ld) ld=$2; shift 2 ;;
        --target) target=$2; shift 2 ;;
        --stack-object) stack_object=$2; shift 2 ;;
        --cflags) cflags=$2; shift 2 ;;
        --log) log=$2; shift 2 ;;
        --print-only) print_only=1; shift ;;
        --help|-h) usage ;;
        *) printf 'musl-sysroot: unknown argument: %s\n' "$1" >&2; usage ;;
    esac
done

for required in src deps lock work sysroot cc ar ranlib ld target stack_object cflags; do
    eval "value=\${$required}"
    [ -n "$value" ] || { printf 'musl-sysroot: --%s is required\n' "$required" >&2; exit 2; }
done
[ -x "$deps" ] || die "$deps is not executable; run make tools first"
[ -d "$src" ] || die "no such source root: $src"

# --- what the lock file says -------------------------------------------------
# The pinned commit and the patch digests are read from the lock file rather
# than repeated here, so this script cannot drift away from `make fetch`.
lock_value() {
    "$deps" --lock "$lock" --id "$1" --print "$2"
}

musl_commit=$(lock_value musl commit)
musl_directory=$(lock_value musl directory)
mimalloc_commit=$(lock_value mimalloc commit)
mimalloc_directory=$(lock_value mimalloc directory)
musl_source="$src/$musl_directory"
mimalloc_source="$src/$mimalloc_directory"

[ -d "$musl_source" ] || die "$musl_directory is not checked out; run git submodule update --init"
[ -d "$mimalloc_source" ] || die "$mimalloc_directory is not checked out"

pinned() {
    git -C "$1" rev-parse HEAD
}
[ "$(pinned "$musl_source")" = "$musl_commit" ] || \
    die "$musl_directory is not at the locked commit $musl_commit"
[ "$(pinned "$mimalloc_source")" = "$mimalloc_commit" ] || \
    die "$mimalloc_directory is not at the locked commit $mimalloc_commit"

patch_lines=$( "$deps" --lock "$lock" --id musl --list-patches || true )

if [ "$print_only" -eq 1 ]; then
    printf 'musl %s\nmimalloc %s\n' "$musl_commit" "$mimalloc_commit"
    printf '%s\n' "$patch_lines"
    exit 0
fi

# --- the work directory belongs to this configuration ------------------------
mkdir -p "$work"
key_file="$work/inputs"
{
    printf 'musl=%s\nmimalloc=%s\n' "$musl_commit" "$mimalloc_commit"
    printf '%s\n' "$patch_lines"
    printf 'cc=%s\nar=%s\nranlib=%s\nld=%s\ntarget=%s\ncflags=%s\n' \
        "$cc" "$ar" "$ranlib" "$ld" "$target" "$cflags"
} >"$key_file.new"
if [ -f "$key_file" ] && cmp -s "$key_file" "$key_file.new"; then
    rm -f "$key_file.new"
else
    # A different compiler, flag set or patch digest invalidates whatever
    # upstream's configure already probed: start the build directory over.
    reliefos_log RESET "$work/build"
    rm -rf "$work/build" "$work/src"
    mv "$key_file.new" "$key_file"
fi

build="$work/build"
tree="$work/src"
mkdir -p "$build"
# musl configures out of tree: it writes config.mak plus a symlink to its own
# Makefile into the current directory, so every step below runs from there.
cd "$build"

# --- unpack and patch --------------------------------------------------------
if [ ! -f "$tree/.leonos-unpacked" ]; then
    rm -rf "$tree"
    mkdir -p "$tree"
    # The commit was just verified and git stores paths relative to the
    # repository root, so the archive cannot name a file outside $tree.
    git -C "$musl_source" archive "$musl_commit" | tar -x -C "$tree"
    : >"$tree/.leonos-unpacked"
    reliefos_log PATCH "$musl_directory"
    printf '%s\n' "$patch_lines" | while IFS='	' read -r digest path; do
        [ -n "$digest" ] || continue
        actual=$(sha256sum "$src/$path" | cut -d' ' -f1)
        [ "$actual" = "$digest" ] || \
            die "patch $path is $actual, the lock file pins $digest"
        patch --batch --forward -p1 -d "$tree" -i "$src/$path" >/dev/null
    done
fi

# --- upstream build ----------------------------------------------------------
# musl's configure probes with `$CC <flags>` and only sometimes adds $CFLAGS,
# so the target triple has to stay inside the compiler word.
export CC="$cc --target=$target -fuse-ld=lld"
export AR="$ar"
export RANLIB="$ranlib"
export LIBCC=
export CFLAGS="$cflags"

if [ -n "$log" ]; then
    mkdir -p "$(dirname "$log")"
    out="$log"
else
    out=/dev/null
fi

run_logged() {
    if ! "$@" >>"$out" 2>&1; then
        printf 'musl-sysroot: `%s` failed' "$1" >&2
        shift
        for argument in "$@"; do
            printf ' %s' "$argument"
        done
        printf '\n' >&2
        if [ "$out" != /dev/null ]; then
            printf '--- last 40 lines of %s ---\n' "$out" >&2
            tail -n 40 "$out" >&2
        fi
        exit 1
    fi
}

run_logged "$tree/configure" --target="$target" "--prefix=$sysroot" \
    "--syslibdir=$sysroot/lib" --disable-gcc-wrapper
run_logged make --no-print-directory -C "$build" CC="$CC" AR="$AR" RANLIB="$RANLIB"
run_logged make --no-print-directory -C "$build" install CC="$CC" AR="$AR" RANLIB="$RANLIB"

# --- the pieces musl does not ship -------------------------------------------
# musl builds without stack protection; anything compiled with it still needs
# __stack_chk_fail_local, so the device supplies it the way the old build did.
mkdir -p "$sysroot/lib"
[ -f "$stack_object" ] || die "missing Kbuild stack-protector helper: $stack_object"
"$ar" rcs "$sysroot/lib/libssp_nonshared.a" "$stack_object"

mimalloc_obj="$build/mimalloc.o"
resource=$("$cc" -print-resource-dir)
"$cc" --target="$target" -O2 -fPIC -mno-avx -fno-stack-protector -nostdinc \
    -isystem "$sysroot/include" -isystem "$resource/include" \
    -I "$mimalloc_source/include" \
    -DMI_LIBC_MUSL=1 -DMI_MALLOC_OVERRIDE=1 -DMI_SHARED_LIB_EXPORT=1 \
    -DMI_FREE_USE_PAGEMAP=1 -DMI_BUILD_RELEASE=1 -DNDEBUG \
    -c "$mimalloc_source/src/static.c" -o "$mimalloc_obj"
"$ld" -shared --no-undefined -soname libmimalloc.so.3 \
    -o "$sysroot/lib/libmimalloc.so.3" "$mimalloc_obj" -L "$sysroot/lib" -lc
cp "$mimalloc_source/include/mimalloc.h" "$sysroot/include/mimalloc.h"
cp "$mimalloc_obj" "$sysroot/lib/mimalloc.o"

# --- notices and the stamp ---------------------------------------------------
# Licenses are copied from the pinned trees the sysroot was actually built
# from, so the shipped notice can never describe a different version.
musl_license=$(lock_value musl license)
mimalloc_license=$(lock_value mimalloc license)
mkdir -p "$sysroot/share/licenses/musl" "$sysroot/share/licenses/mimalloc"
cp "$src/$musl_license" "$sysroot/share/licenses/musl/COPYRIGHT"
cp "$src/$mimalloc_license" "$sysroot/share/licenses/mimalloc/LICENSE"

# The lock validator rejects quotes, backslashes, spaces and control characters
# in patch paths, so they can be written into the stamp as they are.
stamp="$sysroot/.leonos-musl.json"
printf '%s\n' "$patch_lines" | awk -F'\t' \
    -v musl="$musl_commit" -v mimalloc="$mimalloc_commit" -v triple="$target" \
    'BEGIN {
        printf "{\n  \"sources\": {\n    \"musl\": \"%s\",\n    \"mimalloc\": \"%s\"\n  },\n", musl, mimalloc
        printf "  \"patches\": [\n"
     }
     length($1) == 64 { if (written++) printf ",\n"; printf "    {\"path\": \"%s\", \"sha256\": \"%s\"}", $2, $1 }
     END {
        printf "\n  ],\n  \"target\": \"%s\"\n}\n", triple
     }' >"$stamp.tmp"
mv "$stamp.tmp" "$stamp"

reliefos_log SYSROOT "$sysroot"
