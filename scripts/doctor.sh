#!/bin/sh
# Grouped host / target / third-party checks. Exits non-zero on anything missing
# so CI and a fresh checkout get a single actionable report (plan section 6.2).
set -u

. "$(CDPATH= cd -- "$(dirname "$0")" && pwd)/logging.sh"

failures=0
group() { printf '%s\n' "$1"; }
missing() { reliefos_log MISSING "$1"; failures=$((failures + 1)); }
present() { reliefos_log OK "$1 -> $2"; }

check_tool() {
    found=$(command -v "$1" 2>/dev/null || true)
    if [ -z "$found" ]; then
        missing "$1"
    else
        present "$1" "$found"
    fi
}

printf 'ReliefOS build doctor\n'
printf 'source root\t%s\n' "${SRC:-.}"
printf 'output dir\t%s\n' "${O:-unset}"
printf 'arch\t\t%s\nprofile\t%s\n' "${ARCH:-unset}" "${PROFILE:-unset}"
printf 'toolchain\t%s\n\n' "${TOOLCHAIN:-unset}"

group 'host tools (required by make itself and the C helpers)'
for tool in sh grep sed awk find sort cmp mv ln mkdir rm printf date tr head \
            cut expr uname nproc xz tar patch curl flock make readelf stat od \
            gzip sha256sum timeout perl autoconf automake libtoolize gperf bison flex pkg-config msgfmt; do
    check_tool "$tool"
done
reliefos_log HOSTCC "${HOSTCC:-cc}"
if ! command -v "${HOSTCC:-cc}" >/dev/null 2>&1; then
    missing "HOSTCC=${HOSTCC:-cc}"
fi

group ''
group 'target toolchain (validated by actually compiling)'
# Named explicitly: HOSTCC and TARGET_CC must be visible as two different
# variables in one output, or "the host compiler works" proves nothing about the
# cross compiler.
reliefos_log TARGET_CC "${TARGET_CC:-unset}"
reliefos_log TARGET_LD "${TARGET_LD:-unset}"
for tool in "$TARGET_CC" "$TARGET_LD" "$TARGET_AR" "$TARGET_OBJCOPY" \
            "$TARGET_STRIP"; do
    check_tool "$tool"
done

# A clang on PATH that cannot emit the freestanding triple, or that has no
# compiler-rt builtins archive, is worse than no compiler at all: the link would
# fail much later with a confusing message.
if command -v "$TARGET_CC" >/dev/null 2>&1; then
    probe_dir=$(mktemp -d 2>/dev/null || echo "")
    if [ -n "$probe_dir" ]; then
        printf 'int main(void){return 0;}\n' > "$probe_dir/probe.c"
        if "$TARGET_CC" -target "${TARGET_TRIPLE_KERNEL}" -ffreestanding -c \
                "$probe_dir/probe.c" -o "$probe_dir/probe.o" >/dev/null 2>&1; then
            reliefos_log OK "$TARGET_CC can target $TARGET_TRIPLE_KERNEL"
        else
            missing "$TARGET_CC cannot target $TARGET_TRIPLE_KERNEL"
        fi
        resource_dir=$("$TARGET_CC" -print-resource-dir 2>/dev/null || echo "")
        if [ -n "$resource_dir" ] && [ -d "$resource_dir/include" ]; then
            reliefos_log OK "compiler-rt headers $resource_dir/include"
        else
            missing "$TARGET_CC compiler-rt headers (clang -print-resource-dir)"
        fi
        builtins=$("$TARGET_CC" --target="${TARGET_TRIPLE_USER}" --rtlib=compiler-rt \
            --print-libgcc-file-name 2>/dev/null || :)
        if [ -z "$builtins" ] || [ ! -s "$builtins" ]; then
            missing "$TARGET_CC compiler-rt builtins archive for $TARGET_TRIPLE_USER ($builtins)"
        else
            # Force a runtime helper reference; file existence alone can accept
            # an empty archive or one built for a different architecture.
            printf 'unsigned __int128 reliefos_probe(unsigned __int128 a, unsigned __int128 b){return a/b;}\n' \
                > "$probe_dir/runtime.c"
            if "$TARGET_CC" --target="$TARGET_TRIPLE_USER" -ffreestanding -fno-stack-protector \
                    -c "$probe_dir/runtime.c" -o "$probe_dir/runtime.o" >/dev/null 2>&1 &&
               "$TARGET_LD" -e reliefos_probe --no-undefined "$probe_dir/runtime.o" \
                    "$builtins" -o "$probe_dir/runtime.elf" >/dev/null 2>&1; then
                present 'compiler-rt builtins' "$builtins"
            else
                missing "$TARGET_CC compiler-rt builtins cannot link for $TARGET_TRIPLE_USER ($builtins)"
            fi
        fi
        if "$TARGET_LD" --version >/dev/null 2>&1; then
            reliefos_log OK "linker $TARGET_LD responds"
        else
            missing "$TARGET_LD is not runnable"
        fi
        rm -rf "$probe_dir"
    else
        missing 'cannot create temporary directory for compiler validation'
    fi
fi

group ''
group 'image and run tools (needed by the image and run phases)'
for tool in mke2fs e2fsck mkfs.fat mformat mcopy xorriso qemu-img qemu-system-x86_64 grub-mkstandalone grub-mkfont sfdisk fakeroot openssl zip; do
    check_tool "$tool"
done
image_probe=$(mktemp -d)
if printf '#include <sys/types.h>\n#include <ext2fs/ext2fs.h>\nint main(void){ext2_filsys fs=0; ext2fs_close(fs); return 0;}\n' | \
        "${HOSTCC:-cc}" -x c - -lext2fs -lcom_err -o "$image_probe/probe" >/dev/null 2>&1; then
    present 'libext2fs development headers and library' 'host link verified'
else
    missing 'libext2fs development headers/library (Debian: libext2fs-dev; Alpine: e2fsprogs-dev)'
fi
rm -rf "$image_probe"

# ReliefOS boots through UEFI, so a QEMU without firmware cannot be tested at
# all. Reporting this as satisfied is how a run target ends up looking green
# while the guest never starts.
firmware_found=''
for candidate in "${SRC:-.}/buildsystem/firmware/OVMF.fd" \
                 "${SRC:-.}/build/firmware/OVMF.fd" \
                 /usr/share/edk2/x64/OVMF.4m.fd /usr/share/ovmf/OVMF.fd \
                 /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/qemu/OVMF.fd; do
    if [ -f "$candidate" ] && [ "$(wc -c < "$candidate")" -ge 1048576 ]; then
        firmware_found=$candidate
        break
    fi
done
if [ -n "$firmware_found" ]; then
    reliefos_log OK "UEFI firmware $firmware_found"
else
    missing 'UEFI firmware for QEMU (install edk2-ovmf, or place OVMF.fd in buildsystem/firmware/)'
fi

group ''
group 'third-party build inputs'
if [ -f scripts/kconfig/conf.c ] && [ -f scripts/Makefile.build ]; then
    reliefos_log OK 'vendored Linux Kbuild / Kconfig source present'
else
    missing 'scripts/Makefile.build or scripts/kconfig/conf.c'
fi
if [ -f third_party/zlib/contrib/puff/puff.c ]; then
    reliefos_log OK 'zlib reference inflate (contrib/puff) present'
else
    missing 'third_party/zlib/contrib/puff/puff.c'
fi
# Which upstream sources the build needs is the lock file's job, so doctor asks
# it rather than keeping a second, easily stale list here.
lock=${LOCK:-}
deps_tool=${DEPS:-}
cache=${CACHE:-}
if [ -n "$lock" ] && [ ! -f "$lock" ]; then
    missing "$lock (the dependency lock file is gone)"
elif [ -n "$deps_tool" ] && [ -x "$deps_tool" ] && [ -n "$lock" ]; then
    if "$deps_tool" --lock "$lock" --check --root "$PWD" >/dev/null 2>&1; then
        reliefos_log OK "$lock ($("$deps_tool" --lock "$lock" --list | grep -c '') dependencies"
    else
        missing "$lock does not validate: $deps_tool --lock $lock --check --root ."
    fi
    # A `for` loop, not a pipeline: a piped `while` runs in a subshell, and a
    # MISSING submodule there would print without failing doctor. Ids are
    # validated by reliefos-deps to contain no whitespace.
    for dependency in $("$deps_tool" --lock "$lock" --list 2>/dev/null); do
        directory=$("$deps_tool" --lock "$lock" --id "$dependency" --print directory 2>/dev/null) || continue
        kind=$("$deps_tool" --lock "$lock" --id "$dependency" --print kind 2>/dev/null) || continue
        [ "$kind" = submodule ] || continue
        if [ -n "$directory" ] && [ -n "$(ls -A "$directory" 2>/dev/null)" ]; then
            reliefos_log OK "$directory"
        else
            reliefos_log MISSING "$directory (git submodule update --init)"
        fi
    done
    if [ -n "$cache" ]; then
        wanted=$("$deps_tool" --lock "$lock" --fetch-list 2>/dev/null | grep -c '')
        have=0
        [ -d "$cache" ] && have=$(cd "$cache" && find . -type f -name '.*.partial.*' -prune -o -type f -print | wc -l)
        reliefos_log "$([ "$have" -ge "$wanted" ] && echo OK || echo NOTE)" "$have of $wanted locked downloads in $cache (make fetch)"
    fi
elif [ -n "$lock" ]; then
    reliefos_log NOTE "lock file not validated: $deps_tool is not built yet (make tools)"
fi

printf '\n'
if [ "$failures" -ne 0 ]; then
    printf 'doctor: %d missing requirement(s)\n' "$failures" >&2
    exit 1
fi
printf 'doctor: all checked requirements present\n'
