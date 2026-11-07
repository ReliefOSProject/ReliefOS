#!/bin/sh
# Remove regenerable build products from the selected output directory.
#
# Safety rules from the migration plan section 6.3: never `rm -rf $(O)` blindly,
# require an ownership marker, refuse the source root and the filesystem root,
# and keep the shared download cache in every mode.
set -u

target=${O:-}
src=${SRC:-}
keep=${KEEP_CONFIG:-1}

refuse() {
    printf 'clean: refusing to clean: %s\n' "$1" >&2
    exit 1
}

case "$target" in
    /*) ;;
    *)  case "$target" in
            "") refuse 'O is empty' ;;
            *)  target="$PWD/$target" ;;
        esac ;;
esac

[ "$target" = "/" ] && refuse 'O is the filesystem root'
[ -n "$src" ] && [ "$target" = "$src" ] && refuse "O is the source root ($src)"

# Resolve all components before reading the marker or removing products.
resolved=$(realpath -ms -- "$target" 2>/dev/null) || refuse 'cannot resolve O'
physical=$(realpath -m -- "$target" 2>/dev/null) || refuse 'cannot resolve O'
[ "$physical" = "$resolved" ] || refuse 'O contains a symlink component'
[ "$resolved" != / ] || refuse 'resolved O is the filesystem root'
if [ -n "$src" ]; then
    source_root=$(realpath -m -- "$src") || refuse 'cannot resolve source root'
    [ "$resolved" != "$source_root" ] || refuse 'resolved O is the source root'
fi
target=$resolved
marker_root=''
marker_found=0
for marker_name in .reliefos-out .leonos-out; do
    marker="$target/$marker_name"
    [ ! -L "$marker" ] || refuse "ownership marker $marker_name is a symlink"
    if [ -e "$marker" ]; then
        [ -f "$marker" ] || refuse "ownership marker $marker_name is not a regular file"
        this_root=$(sed -n \
            -e 's/^reliefos-build-out version=1 root=//p' \
            -e 's/^leonos4-build-out version=1 root=//p' "$marker" | head -n1)
        [ -n "$this_root" ] || refuse "ownership marker in $target is not a recognised build output marker"
        if [ -n "$marker_root" ] && [ "$marker_root" != "$this_root" ]; then
            refuse 'ownership markers name different source roots'
        fi
        marker_root=$this_root
        marker_found=1
    fi
done
[ "$marker_found" = 1 ] || refuse "no ownership marker in $target (not a build output directory created by this Makefile)"
if [ -n "$src" ] && [ -n "$marker_root" ] && [ "$marker_root" != "$src" ]; then
    refuse "output directory belongs to a different source root ($marker_root)"
fi

# Explicit list rather than a glob: an unknown entry in the tree is not ours to
# delete, and this is what makes `clean` auditable.
# third-party holds upstream build directories this configuration owns (see scripts/reliefos/Makefile.third-party).
# ntclks is the kernel checkout's own sub-build output directory (scripts/reliefos/Makefile.kernel).
products="scripts tools userland userland-installer userland-installer-policy upstream grub-host grub-efi rootfs resources rpr-apps rpr-pages pages Documentation obj generated host include auth pam system musl installer sdk sysroot stage packages images logs meta third-party kernel-export kernel-install reliefnt ntclks"
if [ "$keep" = 0 ]; then
    products="$products config"
fi

printf 'clean: %s (keep config=%s)\n' "$resolved" "$keep"
for entry in $products; do
    if [ -e "$resolved/$entry" ] || [ -L "$resolved/$entry" ]; then
        printf '  remove %s\n' "$entry"
        rm -rf -- "$resolved/$entry" || exit 1
    fi
done
if [ "$keep" = 0 ]; then
    rm -f -- "$resolved/.config" "$resolved/.config.old"
    for marker_name in .reliefos-out .leonos-out; do
        if [ -e "$resolved/$marker_name" ]; then
            printf '  remove %s\n' "$marker_name"
            rm -f -- "$resolved/$marker_name"
        fi
    done
fi
printf 'clean: the shared download cache under cache/downloads was not touched\n'
