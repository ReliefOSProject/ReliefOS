#!/bin/sh
# Incremental-correctness contract tests for the ReliefNT kernel adapter.
#
# Since the kernel/userland separation (phase 3) the kernel is built by the
# standalone checkout named by RELIEFNT_DIR and published into this build's
# output tree by mk/kernel.mk. Every assertion below is about the adapter's
# observed behavior: which actions the delegation emitted, which published
# products and userland objects changed mtime, and what the sub-build's own
# dependency graph selected. Nothing here trusts a stamp.
set -u

# comm checks that its inputs are sorted in *its* locale, and every list here is
# sorted with LC_ALL=C. Without the export the comparison warns and can mis-order
# paths that contain bytes the ambient collation ignores.
LC_ALL=C
export LC_ALL

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
cd "$repo_root" || exit 1

# The kernel checkout under test (env-overridable). The default is the phase-5
# location inside this repository, where the tracked kernel sources live until
# they are removed; phase-3 verification points it at the standalone checkout.
reliefnt=${RELIEFNT_DIR:-${NTCLKS_DIR:-$repo_root/kernel/reliefnt}}

work=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-incremental.XXXXXX") || exit 1
O="$work/out"
reliefnt_o=${RELIEFNT_O:-${NTCLKS_O:-$O/reliefnt}}
failures=0
checks=0

cleanup() {
    [ -n "${KEEP_WORK:-}" ] || rm -rf "$work"
}
trap cleanup EXIT INT TERM

# Make decides "rebuild?" by comparing mtimes, so a prerequisite that changes in
# the same timestamp tick as the last build's output reads as not newer and is
# correctly skipped. Every invalidation below waits a tick first, so these
# assertions measure the dependency graph rather than the filesystem clock.
advance_clock() { sleep 1; }

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

# Actions the build actually ran, identified by the fixed-width label column.
# The kernel actions appear through the adapter (the sub-make logs with the same
# labels), so this stream covers both layers.
#
# A pipeline cannot carry Make's own exit status through grep, and a suite that
# reads "make failed" as "make did no work" would report a broken build as an
# incremental-correctness pass. Make's status is therefore captured separately
# and stops the run, with its output kept for the report.
build() {
    make -s -j"$(nproc)" O="$O" "$@" kernel runtime >"$work/make.out" 2>&1
    status=$?
    grep -E '^  (CC|AS|LD|IMAGE|GEN|HOSTCC|CONFIG) ' "$work/make.out" || true
    if [ "$status" -ne 0 ]; then
        printf 'FAIL - make exited with %s; the incremental assertions below mean nothing\n' "$status"
        tail -n 25 "$work/make.out" | sed 's/^/       | /'
        exit 1
    fi
}

count_label() { printf '%s\n' "$1" | grep -cE "^  $2 " || true; }

# path + full mtime with nanoseconds, for everything this suite watches:
# kernel sub-build objects, userland objects, the published products and the
# exported headers. $(O)/kernel-install is deliberately excluded: the sub-make's
# `install` recopies its four products there on every ask.
snapshot() {
    {
        find "$O/obj" "$reliefnt_o/obj" -name '*.o' -printf '%p %T@\n' 2>/dev/null
        find "$O/kernel-export" -type f -printf '%p %T@\n' 2>/dev/null
        for product in "$O/generated/system/kernel.sys" \
                       "$O/generated/system/kernel.debug" \
                       "$O/generated/system/kerneldebug.sys" \
                       "$O/generated/boot/loader.elf" \
                       "$O/include/generated/autoconf.h" \
                       "$O/include/generated/build_info.h"; do
            [ -e "$product" ] && printf '%s %s\n' "$product" "$(stat -c %y "$product")"
        done
    } | LC_ALL=C sort
}

# The userland half of the snapshot: parent-side objects only. A kernel change
# must never move any of these.
userland_snapshot() {
    find "$O/obj" -name '*.o' -printf '%p %T@\n' 2>/dev/null | LC_ALL=C sort
}

# Which object files changed mtime between two snapshots. Counting Make's log
# lines is a weaker measure than this: the log is a stream that passes through a
# pipeline, while an object's timestamp is the fact the dependency graph is
# supposed to control.
changed_objects() {
    awk 'NR==FNR { seen[$1] = $2; next }
                ($1 in seen) && seen[$1] != $2 { print $1 }' "$1" "$2" | LC_ALL=C sort
}

expect_same() {
    if [ "$1" = "$2" ]; then pass "$3"; else fail "$3" "mtimes differ"; fi
}

expect_count() {
    got=$(printf '%s\n' "$1" | grep -cE "^  $2 " || true)
    if [ "$got" = "$3" ]; then
        pass "$4"
    else
        fail "$4" "expected $3 '$2' actions, got $got"
    fi
}

expect_contains() {
    if printf '%s\n' "$1" | grep -qF -- "$2"; then
        pass "$3"
    else
        fail "$3" "missing: $2"
    fi
}

printf '=== clean output build ===\n'
first=$(build)
if [ ! -f "$O/generated/system/kernel.sys" ]; then
    fail "clean output directory produces a kernel image" "no kernel.sys"
    printf '%s: aborting\n' "$0" >&2
    exit 1
fi
pass 'clean output directory produces a kernel image'
printf '       clean build actions: CC=%s AS=%s LD=%s IMAGE=%s\n' \
    "$(printf '%s\n' "$first" | grep -cE '^  CC ' || true)" \
    "$(printf '%s\n' "$first" | grep -cE '^  AS ' || true)" \
    "$(printf '%s\n' "$first" | grep -cE '^  LD ' || true)" \
    "$(printf '%s\n' "$first" | grep -cE '^  IMAGE ' || true)"

# The parent must not compile any kernel, driver or boot-loader source itself:
# its object tree holds userland and host objects only.
parent_kernel_objects=$(find "$O/obj/kernel" -name '*.o' 2>/dev/null | grep -c '' || true)
if [ "$parent_kernel_objects" = 0 ]; then
    pass 'the parent compiles no kernel, driver or loader sources'
else
    fail 'the parent compiles no kernel, driver or loader sources' \
        "$parent_kernel_objects objects under $O/obj/kernel"
fi

# Every product the rest of the build consumes is published to its legacy path.
missing=''
for product in system/kernel.sys system/kernel.debug system/kerneldebug.sys \
               boot/loader.elf; do
    [ -s "$O/generated/$product" ] || missing="$missing $product"
done
if [ -z "$missing" ]; then
    pass 'all four kernel products are published to their legacy paths'
else
    fail 'all four kernel products are published to their legacy paths' "missing:$missing"
fi

# Nothing may be linked in that the deletion-detecting manifest does not list:
# a leftover object from an earlier source set is the classic way a removed file
# keeps haunting an image.
find "$reliefnt_o/obj/kernel" \( -name '*.c.o' -o -name '*.S.o' \) |
        sed "s#^$reliefnt_o/obj/kernel/##; s#\.o\$##" | LC_ALL=C sort >"$work/on-disk"
LC_ALL=C sort "$reliefnt_o/obj/kernel/sources.list" >"$work/listed"
orphans=$(comm -23 "$work/on-disk" "$work/listed" | tr '\n' ' ')
if [ -z "$orphans" ]; then
    pass 'every object on disk is accounted for by the source manifest'
else
    fail 'every object on disk is accounted for by the source manifest' \
        "not in the manifest: $orphans"
fi

kernel_objects=$(find "$reliefnt_o/obj/kernel" -name '*.c.o' | wc -l)
assembly_objects=$(find "$reliefnt_o/obj/kernel" -name '*.S.o' | wc -l)
if [ "$kernel_objects" -gt 50 ] && [ "$assembly_objects" -gt 0 ]; then
    pass "manifest discovered the kernel sources ($kernel_objects C, $assembly_objects asm)"
else
    fail "manifest discovered the kernel sources" "got $kernel_objects C, $assembly_objects asm"
fi

printf '\n=== A02: a second build does no work ===\n'
before=$(snapshot)
second=$(build)
after=$(snapshot)
if [ -z "$(printf '%s\n' "$second" | tr -d '[:space:]')" ]; then
    pass 'no-op build emits no compile, link, image or generate action'
else
    fail 'no-op build emits no compile, link, image or generate action'
    printf '%s\n' "$second" | sed 's/^/       | /'
    # Name the trigger instead of leaving an undiagnosable flake: the version
    # chain is build_info.h <- {configs/build-version, reliefos-version,
    # version.sig} and a churn here is one of those reading as newer.
    printf '       | version.sig: %s\n' \
        "$(cat "$reliefnt_o/meta/version.sig" 2>/dev/null || echo missing)"
    for suspect in "$reliefnt_o/include/generated/build_info.h" \
                   "$reliefnt_o/meta/version.sig" \
                   "$reliefnt_o/host/bin/reliefos-version" \
                   "$reliefnt/configs/build-version"; do
        printf '       | %s\n' "$(stat -c 'mtime=%Y size=%s' "$suspect" 2>/dev/null \
            | sed "s|^|$suspect |")"
    done
fi
expect_same "$before" "$after" 'no-op build leaves every object and product mtime unchanged'

printf '\n=== A02b: deleted headers in existing dependency files ===\n'
legacy_object="$reliefnt_o/obj/kernel/kernel/reliefnt/kernel.c.o"
legacy_header="$reliefnt/kernel/reliefnt/include/ntclks/boot_splash.h"
# Reproduce a dependency file from before the splash removal and before -MP.
# No empty header target is present in these old compiler-generated files.
printf '\n%s: %s\n' "$legacy_object" "$legacy_header" >>"$legacy_object.d"
if migration=$(build); then
    expect_count "$migration" CC 1 'a legacy deleted header recompiles its consumer'
    expect_count "$migration" LD 1 'a legacy deleted header allows the kernel to relink'
else
    printf '%s\n' "$migration"
    exit 1
fi
if grep -qF "$legacy_header" "$legacy_object.d"; then
    fail 'recompilation removes the obsolete header dependency'
else
    pass 'recompilation removes the obsolete header dependency'
fi

# Simulate a later header removal in a real compiler-generated depfile. Retain
# any empty targets emitted by the compiler, so dropping -MP breaks this case.
old_header="$reliefnt/kernel/reliefnt/include/reliefnt/arch.h"
grep -qF "$old_header" "$legacy_object.d" || exit 1
sed "s|$old_header|$work/removed-header.h|g" "$legacy_object.d" >"$work/removed.d"
mv "$work/removed.d" "$legacy_object.d"
if removal=$(build); then
    expect_count "$removal" CC 1 'new depfiles tolerate a subsequently removed header'
else
    printf '%s\n' "$removal"
    exit 1
fi
if settled=$(build); then
    expect_count "$settled" CC 0 'header dependency recovery settles to a no-op'
    expect_count "$settled" LD 0 'header dependency recovery does not repeatedly relink'
else
    printf '%s\n' "$settled"
    exit 1
fi

printf '\n=== A03: one ordinary source file in the checkout ===\n'
target_source=$reliefnt/kernel/reliefnt/futex.c
before_checksum=$(sha256sum "$O/generated/system/kernel.sys" | cut -d' ' -f1)
userland_before=$(userland_snapshot)
    advance_clock
    touch "$target_source"
third=$(build)
expect_count "$third" CC 1 'changing one C file recompiles exactly one object'
expect_count "$third" LD 1 'changing one C file relinks once'
expect_count "$third" IMAGE 2 'changing one C file refreshes exactly the two kernel images'
expect_contains "$third" "$target_source" 'the recompiled file is the one that changed'
# Only the affected products take part: the loader depends on the kernel only
# through the integrity chain, and a content-preserving touch does not move it.
if printf '%s\n' "$third" | grep -qE 'loader\.elf|kerneldebug\.sys'; then
    fail 'changing one C file leaves loader.elf and kerneldebug.sys alone' \
        "$(printf '%s\n' "$third" | grep -E 'loader\.elf|kerneldebug\.sys' | head -2 | tr '\n' ' ')"
else
    pass 'changing one C file leaves loader.elf and kerneldebug.sys alone'
fi
userland_after=$(userland_snapshot)
expect_same "$userland_before" "$userland_after" \
    'changing one kernel source rebuilds no userland object'
# A touch changes no bytes and the adapter publishes content-stably, so the
# published image must not have moved: re-publishing happens only on a real
# content change (covered by tests/build/test-kernel-adapter.sh).
after_checksum=$(sha256sum "$O/generated/system/kernel.sys" | cut -d' ' -f1)
if [ "$before_checksum" = "$after_checksum" ]; then
    pass 'a content-preserving touch leaves the published image byte-identical'
else
    fail 'a content-preserving touch leaves the published image byte-identical' \
        "$before_checksum != $after_checksum"
fi
untouched=$(snapshot | grep -c '\.o ' || true)
tracked=$(( $(find "$O/obj" "$reliefnt_o/obj" -name '*.o' 2>/dev/null | wc -l) ))
if [ "$untouched" = "$tracked" ] && [ "$tracked" -gt 0 ]; then
    pass "all $tracked kernel and userland objects are tracked by the snapshot"
else
    fail "snapshot covers every object" "tracked $untouched of $tracked"
fi

printf '\n=== A04: a kernel header several objects share ===\n'
# The depfiles are the oracle: every object that recorded the header must be
# rebuilt and no object that did not record it may be. The oracle covers the
# whole freestanding side of the sub-build (kernel, loader and drivers: they
# share the ReliefNT headers); generated headers under the sub-build output tree
# are excluded so the candidate is a real source header.
depfiles=$(find "$reliefnt_o/obj" -name '*.o.d' | tr '\n' ' ')
header=$(for depfile in $depfiles; do
            # Join backslash continuations before splitting: clang emits one
            # prerequisite per word across several lines, and the headers are
            # almost always on the continuation lines.
            sed -e :a -e '/\\$/N; s/\\\n//; ta' "$depfile"
         done | tr ' ' '\n' | grep -E '\.h$' | grep -v "^$reliefnt_o/" |
         sort | uniq -c | sort -rn | awk 'NR == 1 { print $2 }')

if [ -z "$header" ] || [ ! -f "$header" ]; then
    fail 'a shared source header was found to test' "candidate: ${header:-none}"
else
    # Count depfiles that list the header as a whole word. A substring match
    # would count `reliefnt/signal.h` as a consumer of `posix/signal.h` and blame
    # Make for a discrepancy the test invented.
    expected=0
    expected_list=$work/expected-objects
    : >"$expected_list"
    for depfile in $depfiles; do
        if sed -e :a -e '/\\$/N; s/\\\n//; ta' "$depfile" | tr ' \t' '\n\n' |
                grep -qxF -- "$header"; then
            expected=$((expected + 1))
            printf '%s\n' "${depfile%.d}" >>"$expected_list"
        fi
    done
    snapshot >"$work/header-before"
    advance_clock
    touch "$header"
    fourth=$(build)
    snapshot >"$work/header-after"
    changed_objects "$work/header-before" "$work/header-after" >"$work/changed-objects"
    LC_ALL=C sort -o "$expected_list" "$expected_list"
    missing=$(comm -23 "$expected_list" "$work/changed-objects" | xargs -n1 basename 2>/dev/null | tr '\n' ' ')
    extra=$(comm -13 "$expected_list" "$work/changed-objects" | xargs -n1 basename 2>/dev/null | tr '\n' ' ')
    actual=$(grep -c '' "$work/changed-objects")
    relinked=$(printf '%s\n' "$fourth" | grep -cE '^  LD ' || true)
    if [ "$actual" = "$expected" ] && [ "$actual" -gt 1 ] && [ -z "$missing$extra" ]; then
        pass "changing $(basename "$header") rebuilds exactly the $expected recorded consumers"
    else
        fail "changing $(basename "$header") rebuilds exactly the recorded consumers" \
            "depfiles recorded $expected, mtime moved on $actual" \
            "not rebuilt: ${missing:-none}  unexpectedly rebuilt: ${extra:-none}"
    fi
    if [ "$relinked" = 1 ]; then
        pass 'the header change relinks once'
    else
        fail 'the header change relinks once' "got $relinked"
    fi
fi

printf '\n=== A04b: the linker script ===\n'
advance_clock
touch "$reliefnt/arch/x86_64/linker.ld"
script_build=$(build)
script_cc=$(printf '%s\n' "$script_build" | grep -cE '^  CC ' || true)
script_ld=$(printf '%s\n' "$script_build" | grep -cE '^  LD ' || true)
if [ "$script_cc" = 0 ] && [ "$script_ld" = 1 ]; then
    pass 'changing the linker script relinks without recompiling'
else
    fail 'changing the linker script relinks without recompiling' \
        "CC=$script_cc LD=$script_ld"
fi

printf '\n=== A04c: a kernel-private header never reaches userland ===\n'
# The userland consumes the exported headers only. A private kernel header is
# outside that boundary: neither the export nor any userland object may move.
private_header=$reliefnt/kernel/reliefnt/include/reliefnt/sched.h
export_before=$work/export-before
export_after=$work/export-after
find "$O/kernel-export" -type f -printf '%p %T@\n' | LC_ALL=C sort >"$export_before"
userland_before=$(userland_snapshot)
advance_clock
touch "$private_header"
fifth=$(build)
find "$O/kernel-export" -type f -printf '%p %T@\n' | LC_ALL=C sort >"$export_after"
userland_after=$(userland_snapshot)
expect_same "$userland_before" "$userland_after" \
    'a kernel-private header change rebuilds no userland object'
if cmp -s "$export_before" "$export_after"; then
    pass 'a kernel-private header change does not churn the export'
else
    fail 'a kernel-private header change does not churn the export' \
        "$(diff "$export_before" "$export_after" | head -3 | tr '\n' ' ')"
fi
if [ "$(printf '%s\n' "$fifth" | grep -cE '^  CC ' || true)" -gt 0 ]; then
    pass 'the kernel side still rebuilds its own consumer'
else
    fail 'the kernel side still rebuilds its own consumer' 'no CC action recorded'
fi

printf '\n=== A05: compile flags, tool identity and profile isolation ===\n'
# An override has to move every C object and no assembly object. Measured on the
# objects themselves rather than on Make's log stream. The override reaches the
# sub-make through MAKEFLAGS and invalidates its kernel-cc signature (which the
# drivers share, so the comparison stays inside the kernel object set).
advance_clock
snapshot >"$work/flags-before"
build KERNEL_CFLAGS=-DRELIEFOS_SIG_PROBE >/dev/null
snapshot >"$work/flags-after"
find "$reliefnt_o/obj/kernel" -name '*.c.o' | LC_ALL=C sort >"$work/all-c"
find "$reliefnt_o/obj/kernel" -name '*.S.o' | LC_ALL=C sort >"$work/all-asm"
changed_objects "$work/flags-before" "$work/flags-after" |
        grep "^$reliefnt_o/obj/kernel/" >"$work/flag-changed"
missed=$(comm -23 "$work/all-c" "$work/flag-changed" | xargs -n1 basename 2>/dev/null | tr '\n' ' ')
asm_touched=$(comm -12 "$work/all-asm" "$work/flag-changed" | xargs -n1 basename 2>/dev/null | tr '\n' ' ')
cc_actions=$(grep -c '' "$work/flag-changed")
if [ -z "$missed" ] && [ -z "$asm_touched" ] && [ "$cc_actions" = "$(grep -c '' "$work/all-c")" ]; then
    pass "KERNEL_CFLAGS override rebuilds all $cc_actions C objects and no asm object"
else
    fail 'KERNEL_CFLAGS override rebuilds only the C action class' \
        "moved=$cc_actions of $(grep -c '' "$work/all-c")" \
        "not rebuilt: ${missed:-none}  asm touched: ${asm_touched:-none}"
fi
if ! cmp -s "$work/flags-before" "$work/flags-after"; then
    pass 'the flag change moved object mtimes'
else
    fail 'the flag change moved object mtimes' 'nothing rebuilt'
fi

# The other profile must not see this output directory at all.
other=$(make -s O="$O" PROFILE=debug -n kernel >/dev/null 2>&1; echo $?)
debug_out="$work/out-debug"
make -s O="$debug_out" PROFILE=debug defconfig >/dev/null 2>&1
if [ -f "$debug_out/config/.config" ] && [ "$debug_out" != "$O" ]; then
    pass 'PROFILE=debug uses a separate output directory'
else
    fail 'PROFILE=debug uses a separate output directory'
fi

printf '\n=== A07: deleting a published product restores it ===\n'
checksum=$(sha256sum "$O/generated/system/kernel.sys" | cut -d' ' -f1)
rm -f "$O/generated/system/kernel.sys"
eighth=$(build)
if [ -f "$O/generated/system/kernel.sys" ]; then
    if [ "$(sha256sum "$O/generated/system/kernel.sys" | cut -d' ' -f1)" = "$checksum" ]; then
        pass 'a deleted image product is regenerated byte-identical'
    else
        fail 'a deleted image product is regenerated byte-identical' 'bytes differ'
    fi
else
    fail 'a deleted image product is regenerated' 'still missing'
fi
rm -f "$O/include/generated/build_info.h"
make -s O="$O" build-info >/dev/null 2>&1
if [ -f "$O/include/generated/build_info.h" ]; then
    pass 'a deleted parent version header is regenerated on demand'
else
    fail 'a deleted parent version header is regenerated on demand' 'still missing'
fi

printf '\n%s: %d checks, %d failures\n' 'test-incremental' "$checks" "$failures"
[ "$failures" -eq 0 ] || exit 1
exit 0
