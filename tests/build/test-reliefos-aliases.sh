#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)
w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT HUP INT TERM

cat > "$w/check.mk" <<'MAKE'
ifneq ($(RELIEFOS_CACHE),$(EXPECTED_CACHE))
$(error ReliefOS cache variable did not resolve to the requested value)
endif
ifneq ($(RELIEFOS_LOCK),$(EXPECTED_LOCK))
$(error ReliefOS lock variable did not resolve to the requested value)
endif
ifneq ($(RELIEFOS_FETCH_SCRIPT),$(EXPECTED_FETCH))
$(error ReliefOS fetch script variable did not resolve to the requested value)
endif
ifneq ($(RELIEFOS_EMIT),$(EXPECTED_EMIT))
$(error ReliefOS emit variable did not resolve to the requested value)
endif
ifneq ($(RELIEFOS_VERSION_TOOL),$(EXPECTED_VERSION_TOOL))
$(error ReliefOS version tool variable did not resolve to the requested value)
endif
ifneq ($(RELIEFOS_DEPS_TOOL),$(EXPECTED_DEPS_TOOL))
$(error ReliefOS deps tool variable did not resolve to the requested value)
endif
ifneq ($(RELIEFOS_GBK_TOOL),$(EXPECTED_GBK_TOOL))
$(error ReliefOS GBK tool variable did not resolve to the requested value)
endif
ifneq ($(RELIEFOS_SDK_DRIVER),$(EXPECTED_SDK_DRIVER))
$(error ReliefOS SDK driver variable did not resolve to the requested value)
endif
ifneq ($(RELIEFOS_APK_OWN),$(EXPECTED_APK_OWN))
$(error ReliefOS APK ownership tool variable did not resolve to the requested value)
endif
ifneq ($(RELIEFOS_NLS_EXTRACT),$(EXPECTED_NLS_EXTRACT))
$(error ReliefOS NLS extractor variable did not resolve to the requested value)
endif
ifneq ($(RELIEFOS_QEMU_IDE),$(EXPECTED_QEMU_IDE))
$(error ReliefOS QEMU IDE variable did not resolve to the requested value)
endif
ifneq ($(RELIEFOS_QEMU_NVME),$(EXPECTED_QEMU_NVME))
$(error ReliefOS QEMU NVME variable did not resolve to the requested value)
endif
ifneq ($(RELIEFOS_BUILD_OWNER),$(EXPECTED_BUILD_OWNER))
$(error ReliefOS build owner variable did not resolve to the requested value)
endif
MAKE

make -s -C "$root" -f Makefile -f "$w/check.mk" help \
    RELIEFOS_CACHE="$w/new-cache" LEONOS_CACHE="$w/old-cache" \
    RELIEFOS_LOCK="$w/new-lock" LEONOS_LOCK="$w/old-lock" \
    RELIEFOS_FETCH_SCRIPT="$w/new-fetch" LEONOS_FETCH_SCRIPT="$w/old-fetch" \
    RELIEFOS_EMIT="$w/new-emit" LEONOS_EMIT="$w/old-emit" \
    RELIEFOS_VERSION_TOOL="$w/new-version-tool" LEONOS_VERSION_TOOL="$w/old-version-tool" \
    RELIEFOS_DEPS_TOOL="$w/new-deps-tool" LEONOS_DEPS_TOOL="$w/old-deps-tool" \
    RELIEFOS_GBK_TOOL="$w/new-gbk-tool" LEONOS_GBK_TOOL="$w/old-gbk-tool" \
    RELIEFOS_SDK_DRIVER="$w/new-sdk-driver" LEONOS_SDK_DRIVER="$w/old-sdk-driver" \
    RELIEFOS_APK_OWN="$w/new-apk-own" LEONOS_APK_OWN="$w/old-apk-own" \
    RELIEFOS_NLS_EXTRACT="$w/new-nls-extract" LEONOS_NLS_EXTRACT="$w/old-nls-extract" \
    RELIEFOS_QEMU_IDE=new-ide LEONOS_QEMU_IDE=old-ide \
    RELIEFOS_QEMU_NVME=new-nvme LEONOS_QEMU_NVME=old-nvme \
    RELIEFOS_BUILD_OWNER=new-owner LEONOS_BUILD_OWNER=old-owner \
    EXPECTED_CACHE="$w/new-cache" EXPECTED_LOCK="$w/new-lock" \
    EXPECTED_FETCH="$w/new-fetch" EXPECTED_EMIT="$w/new-emit" \
    EXPECTED_VERSION_TOOL="$w/new-version-tool" \
    EXPECTED_DEPS_TOOL="$w/new-deps-tool" EXPECTED_GBK_TOOL="$w/new-gbk-tool" \
    EXPECTED_SDK_DRIVER="$w/new-sdk-driver" EXPECTED_APK_OWN="$w/new-apk-own" \
    EXPECTED_NLS_EXTRACT="$w/new-nls-extract" EXPECTED_QEMU_IDE=new-ide \
    EXPECTED_QEMU_NVME=new-nvme EXPECTED_BUILD_OWNER=new-owner >/dev/null

cat > "$w/entry-check.mk" <<'MAKE'
ifneq ($(ARCH),$(EXPECTED_ARCH))
$(error ARCH resolved to '$(ARCH)', expected '$(EXPECTED_ARCH)')
endif
ifneq ($(PROFILE),$(EXPECTED_PROFILE))
$(error PROFILE resolved to '$(PROFILE)', expected '$(EXPECTED_PROFILE)')
endif
ifneq ($(O),$(EXPECTED_O))
$(error O resolved to '$(O)', expected '$(EXPECTED_O)')
endif
MAKE

# Environment-only build selectors are ignored; explicit make assignments win.
env ARCH=unsupported PROFILE=unsupported O="$w/inherited-output" \
    make -s -C "$root" -f Makefile -f "$w/entry-check.mk" help \
    EXPECTED_ARCH=x86_64 EXPECTED_PROFILE=release \
    EXPECTED_O="$root/out/x86_64/release" >/dev/null
env ARCH=unsupported PROFILE=unsupported O="$w/inherited-output" \
    make -s -C "$root" -f Makefile -f "$w/entry-check.mk" help \
    ARCH=x86_64 PROFILE=debug O="$w/command-output" \
    EXPECTED_ARCH=x86_64 EXPECTED_PROFILE=debug EXPECTED_O="$w/command-output" >/dev/null

# The total test runner exports canonical tool paths. Remove those ambient
# inputs so this case actually exercises legacy-only fallback, not precedence.
unset RELIEFOS_CACHE RELIEFOS_LOCK RELIEFOS_FETCH_SCRIPT RELIEFOS_EMIT \
    RELIEFOS_VERSION_TOOL RELIEFOS_DEPS_TOOL RELIEFOS_GBK_TOOL \
    RELIEFOS_SDK_DRIVER RELIEFOS_APK_OWN RELIEFOS_NLS_EXTRACT RELIEFOS_QEMU_IDE \
    RELIEFOS_QEMU_NVME RELIEFOS_BUILD_OWNER

LEONOS_CACHE="$w/legacy-cache" LEONOS_LOCK="$w/legacy-lock" \
LEONOS_FETCH_SCRIPT="$w/legacy-fetch" LEONOS_EMIT="$w/legacy-emit" \
LEONOS_VERSION_TOOL="$w/legacy-version-tool" \
LEONOS_DEPS_TOOL="$w/legacy-deps-tool" LEONOS_GBK_TOOL="$w/legacy-gbk-tool" \
LEONOS_SDK_DRIVER="$w/legacy-sdk-driver" LEONOS_APK_OWN="$w/legacy-apk-own" \
LEONOS_NLS_EXTRACT="$w/legacy-nls-extract" LEONOS_QEMU_IDE=legacy-ide \
LEONOS_QEMU_NVME=legacy-nvme LEONOS_BUILD_OWNER=legacy-owner \
    make -s -C "$root" -f Makefile -f "$w/check.mk" help \
    EXPECTED_CACHE="$w/legacy-cache" EXPECTED_LOCK="$w/legacy-lock" \
    EXPECTED_FETCH="$w/legacy-fetch" EXPECTED_EMIT="$w/legacy-emit" \
    EXPECTED_VERSION_TOOL="$w/legacy-version-tool" \
    EXPECTED_DEPS_TOOL="$w/legacy-deps-tool" EXPECTED_GBK_TOOL="$w/legacy-gbk-tool" \
    EXPECTED_SDK_DRIVER="$w/legacy-sdk-driver" EXPECTED_APK_OWN="$w/legacy-apk-own" \
    EXPECTED_NLS_EXTRACT="$w/legacy-nls-extract" EXPECTED_QEMU_IDE=legacy-ide \
    EXPECTED_QEMU_NVME=legacy-nvme EXPECTED_BUILD_OWNER=legacy-owner >/dev/null

# Dry-run purity is a separate default-configuration check.
for suffix in CACHE LOCK FETCH_SCRIPT EMIT VERSION_TOOL DEPS_TOOL \
    GBK_TOOL SDK_DRIVER APK_OWN NLS_EXTRACT QEMU_IDE QEMU_NVME BUILD_OWNER; do
    unset "RELIEFOS_$suffix" "LEONOS_$suffix"
done
dry_o="$w/dry-output"
if make -s -n -C "$root" O="$dry_o" userland >"$w/dry.log" 2>&1; then
    echo 'fresh dry-run must request defconfig' >&2; exit 1
fi
grep -q 'defconfig' "$w/dry.log"
if make -s -q -C "$root" O="$dry_o" userland >/dev/null 2>&1; then
    query_status=0
else
    query_status=$?
fi
[ "$query_status" -eq 2 ] || { echo 'fresh make -q must request defconfig' >&2; exit 1; }
[ ! -e "$dry_o/.build-lock" ]
[ ! -e "$dry_o/.reliefos-out" ]
[ ! -e "$dry_o/.config" ]

printf 'ReliefOS variables take precedence; legacy LEONOS aliases remain accepted\n'
