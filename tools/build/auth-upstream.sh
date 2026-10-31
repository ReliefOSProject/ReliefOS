#!/bin/sh
# Stage the autoconf-driven parts of the authentication chain.
#
# Linux-PAM 1.7.x is Meson-only upstream, and the ruling for this project is that
# the build may not use Meson or Ninja; PAM therefore needs a hand-written
# Makefile port and is deliberately *not* built here. Everything that still ships
# a POSIX `configure` can be built the way plan section 9 allows, and that is
# exactly the split this script makes:
#
#   linux-headers -> `make headers_install` (upstream's own Makefile)
#   libxcrypt     -> configure, make, make install DESTDIR
#
# libcrypt.so.2 and crypt.h are what the runtime DSO links and what
# userland/auth/*.c includes, so staging them here is the first honest step of
# that chain rather than a workaround: nothing about libpam is claimed.
#
# Pinned versions, digests and license paths come from the lock file, never from
# this script, so the two cannot drift apart.
set -eu
# A '+' recipe preserves Make's jobserver, but also runs during make -n.
. "$(CDPATH= cd -- "$(dirname "$0")/../../scripts" && pwd)/logging.sh"
case ${MAKEFLAGS%% *} in *n*) exit 0 ;; esac
. "$(CDPATH= cd -- "$(dirname "$0")" && pwd)/upstream-make-environment.sh"
reliefos_upstream_make_environment

die() {
    printf '%s\n' "auth-upstream: $*" >&2
    exit 1
}

usage() {
    cat >&2 <<'USAGE'
usage: auth-upstream.sh --src ROOT --deps PATH --lock PATH --cache DIR --work DIR
                        --stage DIR --sysroot DIR --cc EXE --ar EXE --ranlib EXE
                        --target TRIPLE --cflags FLAGS [--log FILE]
USAGE
    exit 2
}

src=""
deps=""
lock=""
cache=""
work=""
stage=""
sysroot=""
cc=""
ar=""
ranlib=""
target=""
cflags=""
log=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --src) src=$2; shift 2 ;;
        --deps) deps=$2; shift 2 ;;
        --lock) lock=$2; shift 2 ;;
        --cache) cache=$2; shift 2 ;;
        --work) work=$2; shift 2 ;;
        --stage) stage=$2; shift 2 ;;
        --sysroot) sysroot=$2; shift 2 ;;
        --cc) cc=$2; shift 2 ;;
        --ar) ar=$2; shift 2 ;;
        --ranlib) ranlib=$2; shift 2 ;;
        --target) target=$2; shift 2 ;;
        --cflags) cflags=$2; shift 2 ;;
        --log) log=$2; shift 2 ;;
        --help|-h) usage ;;
        *) printf 'auth-upstream: unknown argument: %s\n' "$1" >&2; usage ;;
    esac
done

for required in src deps lock cache work stage sysroot cc ar ranlib target cflags; do
    eval "value=\${$required}"
    [ -n "$value" ] || { printf 'auth-upstream: --%s is required\n' "$required" >&2; exit 2; }
done
[ -x "$deps" ] || die "$deps is not executable; run make tools first"
[ -d "$sysroot/lib" ] || die "the musl sysroot is missing: $sysroot"

# The packages are a fixed list because each one needs its own configure argv: an
# unknown id fails below rather than being configured with generic flags.
packages="linux-headers libxcrypt"
jobs=1

lock_value() {
    "$deps" --lock "$lock" --id "$1" --print "$2"
}

# Each package is a tarball whose cache file is named after the URL basename, so
# the archive verified here is the same bytes `make fetch` checked in.
cache_archive() {
    url=$(lock_value "$1" url)
    [ -n "$url" ] || die "the lock file gives no url for $1"
    printf '%s/%s\n' "$cache" "${url##*/}"
}

unpack() {
    id=$1
    archive=$2
    directory=$3
    [ -f "$archive" ] || die "$id is not in the cache: $archive (run make fetch)"
    wanted=$(lock_value "$id" sha256)
    actual=$(sha256sum "$archive" | cut -d' ' -f1)
    [ "$wanted" = "$actual" ] ||
        die "$archive digest mismatch: $actual != $wanted"
    rm -rf "$work/src/$directory"
    mkdir -p "$work/src"
    # `--no-same-owner` plus a destination this script chose: tar itself rejects
    # absolute paths and `..` escapes, and nothing here lets an archive pick
    # where it lands (plan section 9).
    tar -xf "$archive" -C "$work/src" --no-same-owner ||
        die "could not unpack $archive"
    [ -d "$work/src/$directory" ] ||
        die "$directory is not the top-level directory of $archive"
}

run_logged() {
    if [ -n "$log" ]; then
        printf '\n$ %s\n' "$*" >>"$log"
        "$@" >>"$log" 2>&1 || {
            tail -n 25 "$log" >&2
            die "command failed: $* (see $log)"
        }
    else
        "$@" || die "command failed: $*"
    fi
}

# --- inputs that invalidate everything --------------------------------------
# Anything that changes what gets compiled has to change this key, and a changed
# key wipes the unpacked sources, the build directories and the staged tree
# instead of layering a new configuration over an old one.
key_file="$work/.leonos-auth-inputs"
mkdir -p "$work"
{
    for package in $packages; do
        printf '%s %s %s\n' "$package" \
            "$(lock_value "$package" sha256)" \
            "$(lock_value "$package" directory)"
    done
    printf 'target %s\ncflags %s\n' "$target" "$cflags"
    "$cc" --version 2>&1 | head -n1
    sha256sum "$0"
} >"$key_file.new"

# One decision, two consequences: the key is always published (otherwise the next
# run would wipe and rebuild again), and a run with an unchanged key may still
# have to do the work when the stage was cleaned away underneath it.
if [ -f "$key_file" ] && cmp -s "$key_file.new" "$key_file"; then
    inputs_changed=0
else
    inputs_changed=1
fi
mv -f "$key_file.new" "$key_file"

if [ "$inputs_changed" = 0 ]; then
    # Nothing about the inputs changed and the stage is already there, so leave
    # upstream's configure alone: re-running it costs twenty seconds to rediscover
    # the same 304 defines.
    # Make calls this adapter only after a declared input/output invalidation.
    # A stamp alone cannot prove installed files still exist. Reinstall from
    # the configured tree below when Make requests repair.
    :
else
    reliefos_log RESET "$work/build"
    rm -rf "$work/build" "$work/src" "$stage"
fi

mkdir -p "$work/build" "$stage"

# --- the cross compiler word -------------------------------------------------
# Reproduced flag for flag from the retired Python driver
# (tools/build_auth_upstream.py:89-98): --sysroot plus an explicit -nostdinc with
# the musl and clang resource directories, so nothing can fall back to the host
# glibc headers, and --gcc-toolchain=/nonexistent to make sure it cannot.
resource=$("$cc" -print-resource-dir 2>/dev/null) ||
    die "$cc cannot print its resource directory; compiler-rt headers would be missing"
compiler="$cc --target=$target --sysroot=$sysroot --gcc-toolchain=/nonexistent"
compiler="$compiler -fuse-ld=lld --rtlib=compiler-rt --unwindlib=none"
compiler="$compiler -nostdinc -isystem $sysroot/include -isystem $resource/include"
compiler="$compiler -idirafter $stage/usr/include"

# --- Linux kernel UAPI headers ----------------------------------------------
# Later packages put this usr/include on their CPPFLAGS, so it has to exist
# before anything is configured.
build_linux_headers() {
    directory=$(lock_value linux-headers directory)
    unpack linux-headers "$(cache_archive linux-headers)" "$directory"
    # The kernel Makefile implements out-of-tree builds with `O=`, and that
    # variable reaches it through MAKEFLAGS from any `make O=...` invocation of
    # this project. Without an explicit value here the kernel would write its own
    # arch/ and scripts/ build tree into the caller's output directory, so the
    # build directory is named rather than inherited.
    mkdir -p "$work/build/linux-headers"
    run_logged make -C "$work/src/$directory" -f Makefile \
        O="$work/build/linux-headers" \
        ARCH=x86_64 -j"$jobs" headers_install \
        INSTALL_HDR_PATH="$stage/usr"
}

# --- libxcrypt ---------------------------------------------------------------
# Upstream configure argv kept verbatim from the retired driver
# (tools/build_auth_upstream.py:166-195), so the staged library has the same
# SONAME and the same hash coverage as the one the images ship today.
#
# One deliberate deviation: libxcrypt's own asserts expand __FILE__ into the
# binary, so the absolute build directory is baked into .rodata. That makes the
# library byte-dependent on where it was compiled, which is exactly what plan
# section 6.4 forbids for staged inputs, so the source prefix is mapped away.
build_libxcrypt() {
    directory=$(lock_value libxcrypt directory)
    unpack libxcrypt "$(cache_archive libxcrypt)" "$directory"
    build="$work/build/libxcrypt"
    mkdir -p "$build"
    (
        cd "$build"
        run_logged env \
            ac_cv_path_python3_passlib='not found' \
            CC="$compiler" AR="$ar" RANLIB="$ranlib" \
            CFLAGS="$cflags -fmacro-prefix-map=$work/src/$directory=libxcrypt" \
            CPPFLAGS="-I$stage/usr/include" LIBS= \
            LDFLAGS="-Wl,-z,relro,-z,now -L$stage/lib -Wl,-rpath-link,$stage/lib" \
            PKG_CONFIG_PATH= \
            PKG_CONFIG_LIBDIR="$stage/lib/pkgconfig:$stage/usr/lib/pkgconfig" \
            PKG_CONFIG_SYSROOT_DIR="$stage" \
            "$work/src/$directory/configure" \
            --host="$target" --prefix=/usr --sysconfdir=/etc --localstatedir=/var \
            --libdir=/lib --enable-hashes=all --enable-obsolete-api=no
        run_logged make -C "$build" -j"$jobs" CC="$compiler" AR="$ar" RANLIB="$ranlib"
        run_logged make -C "$build" install DESTDIR="$stage" CC="$compiler" AR="$ar" RANLIB="$ranlib"
    )
}

for package in $packages; do
    case "$package" in
        linux-headers) build_linux_headers ;;
        libxcrypt) build_libxcrypt ;;
        *) die "auth-upstream does not know how to build $package without Meson" ;;
    esac
done

# --- licenses and the stamp --------------------------------------------------
# A shipped notice has to describe the version that was actually staged, so it is
# copied out of the unpacked tree rather than from a checked-in guess.
for package in $packages; do
    license_in_source=$(lock_value "$package" license_in_source)
    [ -n "$license_in_source" ] || continue
    directory=$(lock_value "$package" directory)
    source_license="$work/src/$directory/$license_in_source"
    [ -f "$source_license" ] || die "$package ships no $license_in_source"
    mkdir -p "$stage/share/licenses/$package"
    cp "$source_license" "$stage/share/licenses/$package/LICENSE"
done

{
    printf '{\n  "packages": {\n'
    first=1
    for package in $packages; do
        [ "$first" = 1 ] || printf ',\n'
        first=0
        printf '    "%s": {"version": "%s", "sha256": "%s"}' \
            "$package" "$(lock_value "$package" version)" \
            "$(lock_value "$package" sha256)"
    done
    printf '\n  },\n  "target": "%s"\n}\n' "$target"
} >"$work/.leonos-auth.json.tmp"
mv "$work/.leonos-auth.json.tmp" "$work/.leonos-auth.json"

reliefos_log AUTH "$stage"
