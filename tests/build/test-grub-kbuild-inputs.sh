#!/bin/sh
# Prebuilt GRUB EFI modules are image inputs; native Kbuild must not synthesize
# its own .mod lists over them while the ESP target is being evaluated.
set -eu

src=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
. "$src/tests/build/grub-fixture.sh"
tmp_base=${TMPDIR:-$src/out/kbuild-migration}
mkdir -p "$tmp_base"
work=$(mktemp -d "$tmp_base/grub-kbuild-inputs.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

fixture=$work/fixture
mkdir -p "$fixture"
modules=$fixture/efi-modules
grub_fixture_efi "$modules"
grub_fixture_bios "$work/bios-modules"
test -s "$modules/normal.mod"
test "$(LC_ALL=C file -b "$modules/normal.mod" | cut -d' ' -f1)" = ELF

cat > "$fixture/Kbuild" <<'EOF'
include $(RELIEFOS_SRC)/scripts/reliefos/Makefile.parameters
include $(RELIEFOS_SRC)/scripts/reliefos/Makefile.images
EOF

mkdir -p "$fixture/fake-bin"
cat > "$fixture/fake-bin/grub-mkstandalone" <<'EOF'
#!/bin/sh
set -eu
if [ "${1-}" = --version ]; then
    printf 'grub-mkstandalone fixture 1\n'
    exit 0
fi
output=
module_dir=
requested_modules=
while [ "$#" -gt 0 ]; do
    if [ "$1" = -d ]; then
        module_dir=$2
        shift 2
    elif [ "$1" = -o ]; then
        output=$2
        shift 2
    elif [ "${1#--modules=}" != "$1" ]; then
        requested_modules=${1#--modules=}
        shift
    else
        shift
    fi
done
test -n "$output"
test "$module_dir" = "$GRUB_EXPECTED_MODULES"
for name in $requested_modules; do
    test -s "$module_dir/$name.mod" || {
        echo "requested unavailable EFI module: $name" >&2
        exit 1
    }
done
mkdir -p "$(dirname "$output")"
printf 'fixture EFI payload\n' > "$output"
printf 'built\n' >> "$GRUB_STUB_LOG"
EOF
chmod +x "$fixture/fake-bin/grub-mkstandalone"

prepare_output() {
    target_out=$1
    mkdir -p "$target_out/generated/system" "$target_out/generated/grub" \
        "$target_out/generated/config" "$target_out/host/bin" "$target_out/meta"
    printf 'fixture config\n' > "$target_out/.config"
    printf 'fixture kernel\n' > "$target_out/generated/system/kernel.sys"
    printf 'fixture font\n' > "$target_out/generated/grub/leonos-unicode.pf2"
    printf 'theme=metro\nwallpaper.mode=fill\n' > "$target_out/generated/config/display.conf"
    printf '#!/bin/sh\nexit 0\n' > "$target_out/host/bin/reliefos-emit"
    chmod +x "$target_out/host/bin/reliefos-emit"
}

run_esp() {
    target_out=$1
    module_dir=$2
    stub_log=$3
    PATH="$fixture/fake-bin:$PATH" GRUB_STUB_LOG="$stub_log" \
        GRUB_EXPECTED_MODULES="$module_dir" \
        CONFIG_SHELL=/bin/sh AWK=awk srctree="$src" objtree=. VPATH="$src" \
        make --no-print-directory -rR -C "$target_out" \
            -f "$src/scripts/Makefile.port" -f "$src/scripts/Makefile.build" \
            obj=. kbuild-file="$fixture/Kbuild" KBUILD_BUILTIN=1 \
            RELIEFOS_SRC="$src" O_GENERATED="$target_out/generated" \
            O_STAGE="$target_out/stage" O_META="$target_out/meta" \
            O_IMAGES="$target_out/images" RELIEFOS_HOST_BIN="$target_out/host/bin" \
            RELIEFOS_CONFIG_FILE="$target_out/.config" \
            RELIEFOS_EMIT="$target_out/host/bin/reliefos-emit" \
            GRUB_FONT="$target_out/generated/grub/leonos-unicode.pf2" \
            GRUB_BIOS_DIR="$work/bios-modules" GRUB_MKSTANDALONE=grub-mkstandalone \
            GRUB_EFI_DIR="$module_dir" SOURCE_DATE_EPOCH=1700000000 esp
}

hash_modules() {
    find "$1" -maxdepth 1 -type f -name '*.mod' -print | LC_ALL=C sort |
        xargs sha256sum
}

prepare_output "$work/output"
hash_modules "$modules" > "$work/modules.before"
if ! run_esp "$work/output" "$modules" "$work/grub-stub.log" > "$work/first.log" 2>&1; then
    cat "$work/first.log" >&2
    echo 'FAIL - native ESP target fixture did not build' >&2
    exit 1
fi
if ! hash_modules "$modules" > "$work/modules.after-first" ||
    ! cmp -s "$work/modules.before" "$work/modules.after-first"; then
    diff -u "$work/modules.before" "$work/modules.after-first" > "$work/modules.diff" || :
    sed -n '1,20p' "$work/modules.diff" >&2
    cat "$work/first.log" >&2
    echo 'FAIL - native Kbuild changed prebuilt GRUB .mod image inputs' >&2
    exit 1
fi

if ! run_esp "$work/output" "$modules" "$work/grub-stub.log" > "$work/second.log" 2>&1; then
    cat "$work/second.log" >&2
    echo 'FAIL - repeated native ESP target fixture did not build' >&2
    exit 1
fi
hash_modules "$modules" > "$work/modules.after-second"
cmp "$work/modules.before" "$work/modules.after-second"
test "$(wc -l < "$work/grub-stub.log" | tr -d ' ')" = 1 || {
    echo 'FAIL - repeated native ESP target rebuilt its GRUB EFI stage' >&2
    exit 1
}

# The wildcard dependency list alone is insufficient: Make drops an absent
# path before dependency resolution. The fixed modules requested by
# boot-stage.sh must retain explicit prerequisites so a missing source errors.
missing_modules=$work/missing-modules
cp -a "$modules" "$missing_modules"
rm "$missing_modules/normal.mod"
prepare_output "$work/missing-output"
if run_esp "$work/missing-output" "$missing_modules" "$work/missing-stub.log" \
    > "$work/missing.log" 2>&1; then
    cat "$work/missing.log" >&2
    echo 'FAIL - ESP target accepted a missing required GRUB module source' >&2
    exit 1
fi
if ! grep -Fq 'missing required GRUB EFI module' "$work/missing.log"; then
    cat "$work/missing.log" >&2
    echo 'FAIL - missing required GRUB module did not report the source error' >&2
    exit 1
fi
test ! -e "$work/missing-stub.log" || {
    echo 'FAIL - ESP stage ran despite a missing required GRUB module' >&2
    exit 1
}

# BIOS .mod files are equally protected; absent generated inputs must not
# become Kbuild's plain-text module lists.
rm "$work/bios-modules/biosdisk.mod"
prepare_output "$work/missing-bios-output"
if run_esp "$work/missing-bios-output" "$modules" "$work/missing-bios-stub.log" \
    > "$work/missing-bios.log" 2>&1; then
    echo 'FAIL - ESP target accepted a missing required BIOS module' >&2; exit 1
fi
grep -Fq 'missing required GRUB BIOS input' "$work/missing-bios.log"
test ! -e "$work/bios-modules/biosdisk.mod"
test ! -e "$work/missing-bios-stub.log"
printf 'GRUB Kbuild input regression passed: EFI/BIOS ELF inputs preserved, no-op repeat stable, missing required sources rejected\n'
