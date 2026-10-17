#!/bin/sh
# Brand identity gate for the ReliefOS / ReliefNT rename (plan task 1).
#
# Layers (each is a sub-function; later plan tasks turn them green one by one):
#   appearance   - user-visible product names (os-release, GRUB menus)
#   paths        - canonical guest paths and OpenRC service names
#   headers_libs - canonical public headers and runtime library names
#   artifacts    - build product names (images, SDK archive)
#   old_names    - every remaining old-name hit must be listed in
#                  tests/build/brand-allowlist.tsv with a valid category.
#                  Any unlisted hit is a FAIL. Categories are defined in
#                  docs/branding-compatibility.md section 3; the transitional
#                  'migration' category is reported but only rejected when
#                  BRAND_STRICT=1 (final review, plan task 11).
#
# Old-name scan scope: Git-tracked files (git grep) and tracked filenames,
# in the main repository and in the kernel submodule. Untracked user content,
# build outputs and third-party submodules are out of scope by design; the
# final review (plan task 11) covers untracked source with a separate rg pass.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
allowlist="$root/tests/build/brand-allowlist.tsv"
strict=${BRAND_STRICT:-0}
fails=0

tmp_root=${TMPDIR:-$root/out/tmp}
mkdir -p "$tmp_root"
work=$(mktemp -d "$tmp_root/reliefos-brand-identity.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

ok() { printf 'ok - %s\n' "$1"; }
fail() { printf 'FAIL - %s\n' "$1"; fails=$((fails + 1)); }
todo() { printf 'todo - %s\n' "$1"; }

# Layer 1: user-visible product names.
check_appearance() {
    if grep -qx 'NAME="ReliefOS"' "$root/system/rootfs/etc/os-release"; then
        ok 'os-release NAME is ReliefOS'
    else
        fail 'os-release NAME must be NAME="ReliefOS"'
    fi
    if grep -qx 'ID=reliefos' "$root/system/rootfs/etc/os-release"; then
        ok 'os-release ID is reliefos'
    else
        fail 'os-release ID must be ID=reliefos'
    fi
    if grep -qx 'reliefos' "$root/system/rootfs/etc/hostname"; then
        ok 'new rootfs hostname is reliefos'
    else
        fail 'new rootfs hostname must be reliefos'
    fi
    if grep -qx 'PRETTY_NAME="ReliefOS"' "$root/system/rootfs/etc/os-release"; then
        ok 'os-release PRETTY_NAME is ReliefOS without version suffix'
    else
        fail 'os-release PRETTY_NAME must be exactly PRETTY_NAME="ReliefOS"'
    fi
    if grep -qx 'VERSION_ID=4' "$root/system/rootfs/etc/os-release"; then
        ok 'os-release VERSION_ID keeps numeric semantics'
    else
        fail 'os-release VERSION_ID must stay VERSION_ID=4'
    fi
    if grep -q 'menuentry "ReliefOS"' "$root/boot/grub/grub.cfg"; then
        ok 'GRUB menu entry uses ReliefOS'
    else
        fail 'boot/grub/grub.cfg must contain menuentry "ReliefOS"'
    fi
}

# User-visible strings use the canonical display names. Old identifiers remain
# in implementation paths and font aliases, which are not display text.
check_visible_surfaces() {
    ui_files='
boot/grub/grub.cfg
boot/grub/live.cfg
boot/grub/installer.cfg
system/rootfs/etc/os-release
system/rootfs/etc/issue
system/docs/leonos.hlp
system/docs/zh_CN/leonos.hlp
configs/components.toml
configs/nls/po/leonos.pot
configs/nls/po/zh_CN.po
userland/apps/installer/main.cpp
userland/apps/installer/installer_tty.c
userland/apps/login/main.c
userland/apps/desktop/desktop.h
userland/apps/desktop/start_menu.c
userland/apps/desktop/render.c
userland/apps/desktop/screen.c
userland/apps/osver/main.cpp
userland/apps/settings/main.c
userland/apps/run/main.c
userland/apps/fileman/main.cpp
userland/apps/browser/main.c
userland/apps/browser/input.c
userland/apps/browser/navigation.c
userland/apps/browser/state.c
userland/apps/browser/view.c
userland/apps/guitest/main.c
userland/apps/hello/main.c
userland/apps/cjktest/main.c
userland/apps/oschinpt/main.c
userland/apps/uidemo/main.c
userland/apps/minesweeper/main.cpp
userland/apps/calc/main.cpp
userland/apps/taskmgr/main.cpp
userland/apps/leonmmcoset/main.cpp
userland/apps/xiaobai/main.cpp
userland/apps/paint/main.cpp
userland/apps/imageview/main.cpp
userland/apps/doomlauncher/main.cpp
userland/apps/doom/main.cpp
userland/apps/oshlp/main.c
userland/apps/notepad/main.c
userland/apps/terminal/main.c
userland/apps/shell/main.c
'
    old_surfaces=
    for ui_file in $ui_files; do
        if grep -Fq 'LeonOS' "$root/$ui_file"; then
            old_surfaces="$old_surfaces $ui_file"
        fi
    done
    if [ -z "$old_surfaces" ]; then
        ok 'visible UI, help, translations and GRUB use ReliefOS'
    else
        fail "legacy product display strings remain in:$old_surfaces"
    fi
    if grep -Fq 'shell_write("\x1b[96mreliefos\x1b[0m:"' \
            "$root/userland/apps/shell/main.c"; then
        ok 'interactive shell prompt uses the ReliefOS machine name'
    else
        fail 'interactive shell prompt must use the ReliefOS machine name'
    fi
    if grep -qx 'CONFIG_UNAME_OSNAME="ReliefOS"' "$root/userland/busybox/leonos.config"; then
        ok 'BusyBox uname identifies the ReliefOS userland'
    else
        fail 'BusyBox uname must identify the ReliefOS userland'
    fi
    if grep -Fq 'const char *initial = "about:reliefos"' \
            "$root/userland/apps/browser/main.c"; then
        ok 'browser opens its ReliefOS home URI by default'
    else
        fail 'browser home URI must use about:reliefos'
    fi
    if grep -Fq 'ReliefOS GRUB payload installed' "$root/userland/busybox/block_storage.c"; then
        ok 'BusyBox GRUB installer reports ReliefOS'
    else
        fail 'BusyBox GRUB installer must report ReliefOS'
    fi
    if ! grep -Fq 'leonos_api_install_with_progress' \
            "$root/userland/apps/apiapp/main.c" &&
       ! grep -Fq 'leonos_stat_legacy()' "$root/userland/apps/bugtest/main.c" &&
       ! grep -Fq 'leonos_ui_text_width(' "$root/userland/apps/cjktest/main.c"; then
        ok 'application diagnostics display canonical ReliefOS API names'
    else
        fail 'application diagnostics must display ReliefOS API names'
    fi
    if grep -Fq 'Checking installed ReliefOS APKs' "$root/userland/storage/leonos-check-update" &&
       grep -Fq 'ReliefOS RPR reachable' "$root/userland/storage/leonos-rpr-ping"; then
        ok 'storage update tools report ReliefOS'
    else
        fail 'storage update tools must report ReliefOS'
    fi
    if grep -Fq 'Project-Id-Version: ReliefOS musl locale' \
            "$root/configs/locale/zh_CN.po"; then
        ok 'musl locale catalog identifies ReliefOS'
    else
        fail 'musl locale catalog project metadata must identify ReliefOS'
    fi
    if [ "$(sed -n '1p' "$root/system/rootfs/etc/motd")" = 'ReliefOS' ] &&
       [ "$(sed -n '1p' "$root/system/rootfs/etc/motd.zh_CN")" = '欢迎使用 ReliefOS' ]; then
        ok 'English and Chinese MOTD headers use ReliefOS'
    else
        fail 'MOTD headers must use ReliefOS'
    fi
    if grep -qx 'kernel_name=ReliefNT' "$root/kernel/reliefnt/configs/build-version" &&
       grep -q 'LINUX_UTS_SYSNAME "ReliefNT"' \
           "$root/kernel/reliefnt/kernel/reliefnt/include/reliefnt/uts.h" &&
       grep -q 'info.kernel_name' "$root/userland/apps/osver/main.cpp"; then
        ok 'system information surfaces the ReliefNT kernel name'
    else
        fail 'kernel identity used by Fastfetch and the version page must be ReliefNT'
    fi
}

check_current_docs() {
    if grep -qx '# ReliefOS 高级安装教程' "$root/docs/ADVANCED_INSTALL.md" &&
       grep -q '^ReliefOS Installer ISO - Advanced Installation Quick Guide$' \
           "$root/docs/ADVANCED_INSTALL.txt" &&
       grep -q 'Install ReliefOS (TTY mode)' "$root/docs/ADVANCED_INSTALL.txt"; then
        ok 'current installer guides use ReliefOS'
    else
        fail 'current installer guides must use ReliefOS'
    fi
    if grep -q 'kernel/reliefnt/drivers/' "$root/docs/DRIVERS.md" &&
       ! grep -q 'kernel/ntclks/drivers/' "$root/docs/DRIVERS.md"; then
        ok 'driver guide uses the ReliefNT submodule path'
    else
        fail 'driver guide must use kernel/reliefnt/drivers/'
    fi
    if grep -q 'about:reliefos' "$root/docs/BROWSER.md"; then
        ok 'browser guide documents the ReliefOS home URI'
    else
        fail 'browser guide must document about:reliefos'
    fi
    if grep -q 'About ReliefOS' "$root/docs/KERNEL_DEBUG.md"; then
        ok 'kernel debugger guide uses the ReliefOS product name'
    else
        fail 'kernel debugger guide must use About ReliefOS'
    fi
}

# Layer 2: canonical guest paths and service names.
check_paths() {
    if grep -qx 'kernel_name=ReliefNT' "$root/configs/build-version"; then
        ok 'host version generator uses the ReliefNT identity'
    else
        fail 'host version generator must use the ReliefNT identity'
    fi
    if grep -Fq '/etc/reliefos/locale.conf' "$root/Kconfig" &&
       grep -Fq '/etc/reliefos/locale.conf' "$root/kernel/reliefnt/Kconfig"; then
        ok 'both configuration menus describe the canonical locale path'
    else
        fail 'configuration menus must describe /etc/reliefos/locale.conf'
    fi
    if grep -qx 'provider_path=/usr/lib/reliefos/apps/oschinpt/oschinpt.elf' \
           "$root/tools/oschinpt-apk-post-install" &&
       grep -qx 'settings_path=/usr/lib/reliefos/apps/oschinpt/settings.ini' \
           "$root/tools/oschinpt-apk-post-install"; then
        ok 'APK input method registration uses canonical application paths'
    else
        fail 'APK input method registration must use canonical application paths'
    fi
    if grep -Fq '$images/reliefos.vmdk' "$root/scripts/test-smoke.sh" &&
       grep -Fq '$images/reliefos-$variant.iso' "$root/scripts/test-smoke.sh"; then
        ok 'VM smoke test consumes the canonical release artifact names'
    else
        fail 'VM smoke test must consume canonical release artifact names'
    fi
    if [ -f "$root/include/reliefos/layout.h" ]; then
        ok 'include/reliefos/layout.h exists'
    else
        fail 'include/reliefos/layout.h is missing (canonical layout header)'
    fi
    if grep -q '"/etc/reliefos"' "$root/include/reliefos/layout.h" 2>/dev/null; then
        ok 'layout.h declares /etc/reliefos'
    else
        fail 'include/reliefos/layout.h must declare /etc/reliefos'
    fi
    if ls "$root/system/rootfs/etc/init.d/reliefos-"* >/dev/null 2>&1; then
        ok 'OpenRC services use the reliefos-* prefix'
    else
        fail 'system/rootfs/etc/init.d must contain reliefos-* services'
    fi
}

# Layer 3: canonical public headers and runtime library names.
check_headers_libs() {
    if [ -d "$root/include/reliefos" ] && ls "$root/include/reliefos/"*.h >/dev/null 2>&1; then
        ok 'include/reliefos/ holds the canonical public headers'
    else
        fail 'include/reliefos/ must hold the canonical public headers'
    fi
    if [ -d "$root/userland/runtime/include/reliefos" ]; then
        ok 'runtime include/reliefos/ exists'
    else
        fail 'userland/runtime/include/reliefos/ is missing'
    fi
    if [ -f "$root/tools/reliefos_layout.py" ] &&
       grep -Fq 'from reliefos_layout import' "$root/tools/make_image.py"; then
        ok 'host image tools import the canonical ReliefOS layout module'
    else
        fail 'host image tools must use tools/reliefos_layout.py'
    fi
    if grep -Eq 'LEONOS_(APPS|LIB|RUN|VAR_LIB)' \
            "$root/tools/make_installer_root.py"; then
        fail 'host installer tools must use canonical ReliefOS layout constants'
    else
        ok 'host installer tools use canonical ReliefOS layout constants'
    fi
    if grep -rq 'libreliefos\.so\.2' "$root/mk" "$root/tools/build" 2>/dev/null; then
        ok 'build graph produces libreliefos.so.2'
    else
        fail 'mk/ and tools/build/ must produce libreliefos.so.2'
    fi
    if grep -q '^include/uapi/reliefos/' "$root/configs/header-export.list"; then
        ok 'header export lists include/uapi/reliefos/'
    else
        fail 'configs/header-export.list must list include/uapi/reliefos/ headers'
    fi
}

# Layer 4: build product names.
check_artifacts() {
    if grep -q 'reliefos\.vmdk' "$root/mk/images.mk" 2>/dev/null; then
        ok 'images.mk produces reliefos.vmdk'
    else
        fail 'mk/images.mk must produce reliefos.vmdk'
    fi
    if grep -q 'reliefos-live\.iso' "$root/mk/images.mk" 2>/dev/null; then
        ok 'images.mk produces reliefos-live.iso'
    else
        fail 'mk/images.mk must produce reliefos-live.iso'
    fi
    if grep -q 'reliefos-installer\.iso' "$root/mk/images.mk" 2>/dev/null; then
        ok 'images.mk produces reliefos-installer.iso'
    else
        fail 'mk/images.mk must produce reliefos-installer.iso'
    fi
    if grep -rq 'reliefos-musl-sdk' "$root/mk" "$root/tools/build" 2>/dev/null; then
        ok 'SDK archive is named reliefos-musl-sdk'
    else
        fail 'SDK archive must be named reliefos-musl-sdk'
    fi
}

# Allowlist lookup: scope + path must appear exactly once as columns 1 and 3.
hit_allowed() {
    awk -F'\t' -v s="$1" -v p="$2" \
        '!/^#/ && $1==s && $3==p {found=1} END {exit found?0:1}' "$allowlist"
}

# Audit one repository: tracked non-binary text hits and tracked filenames.
audit_repo() {
    hit_scope=$1
    hit_dir=$2
    hits="$work/hits.$hit_scope"
    [ -e "$hit_dir/.git" ] || return 0
    : > "$hits"
    git -C "$hit_dir" grep -I -i -l -E 'leonos|ntclks' >> "$hits" 2>/dev/null || true
    git -C "$hit_dir" ls-files | grep -Ei 'leonos|ntclks' >> "$hits" || true
    sort -u "$hits" | while IFS= read -r hit_path; do
        [ -n "$hit_path" ] || continue
        if ! hit_allowed "$hit_scope" "$hit_path"; then
            printf 'FAIL - non-allowlisted old name in %s:%s (rename it or add a categorised entry to brand-allowlist.tsv)\n' \
                "$hit_scope" "$hit_path"
        fi
    done > "$work/audit.$hit_scope"
    # The scan loop above must not hide failures behind the pipe: replay the
    # report into the real counters now that we are back in the main shell.
    if [ -s "$work/audit.$hit_scope" ]; then
        cat "$work/audit.$hit_scope"
        fails=$((fails + $(grep -c 'FAIL - ' "$work/audit.$hit_scope" || true)))
    else
        ok "old-name audit ($hit_scope): every hit is allowlisted"
    fi
    # Files with hits must be listed exactly once; duplicates are a data error.
    dup=$(cut -f1,3 "$allowlist" 2>/dev/null | grep -v '^#' | sort | uniq -d || true)
    if [ -n "$dup" ]; then
        fail "allowlist has duplicate entries: $dup"
    fi
    return 0
}

check_appearance
check_visible_surfaces
check_current_docs
check_paths
check_headers_libs
check_artifacts

if [ -f "$allowlist" ]; then
    # Validate category vocabulary up front.
    while IFS='	' read -r a_scope a_cat a_path a_reason; do
        case $a_scope in ''|'#'*) continue ;; esac
        case $a_cat in
            compatibility|history|external-url|attribution|migration) ;;
            *) fail "allowlist invalid category '$a_cat' for $a_path" ;;
        esac
    done < "$allowlist"
    audit_repo main "$root"
    if [ -d "$root/kernel/reliefnt" ]; then
        audit_repo kernel "$root/kernel/reliefnt"
    fi
else
    fail "missing $allowlist"
fi

# Report allowlist entries still in the transitional 'migration' category.
migration_left=$(awk -F'\t' '!/^#/ && $2=="migration" {n++} END {print n+0}' "$allowlist")
if [ "$migration_left" -gt 0 ]; then
    if [ "$strict" = 1 ]; then
        fail "BRAND_STRICT: $migration_left allowlist entries still in 'migration' category"
    else
        todo "$migration_left allowlist entries still in 'migration' category (final review must clear)"
    fi
fi

if [ "$fails" -ne 0 ]; then
    printf 'not ok - brand identity: %d assertion(s) failed\n' "$fails"
    exit 1
fi
printf 'brand identity: all gates passed\n'
