#!/bin/sh
# Install only the PAM-owned files into the caller's unpublished staging tree.
set -eu
if [ "${1:-}" = --help ]; then
    echo "usage: install.sh SOURCE BUILD DESTDIR"
    exit 0
fi
[ "$#" = 3 ] || { echo 'usage: install.sh SOURCE BUILD DESTDIR' >&2; exit 2; }
src=$1 build=$2 dest=$3
mkdir -p "$dest/lib/security/pam_filter" "$dest/sbin" "$dest/usr/include/security" \
    "$dest/etc/security/limits.d" "$dest/etc/security/namespace.d" "$dest/lib/pkgconfig" \
    "$dest/usr/lib/systemd/system"
cp -P "$build"/lib/libpam*.so* "$dest/lib/"
cp "$build"/modules/*.so "$dest/lib/security/"
for helper in faillock mkhomedir_helper pwhistory_helper pam_timestamp_check unix_chkpwd; do
    install -m 0755 "$build/bin/$helper" "$dest/sbin/$helper"
done
install -m 0755 "$build/bin/upperLOWER" "$dest/lib/security/pam_filter/upperLOWER"
for h in _pam_compat _pam_macros _pam_types pam_appl pam_ext pam_modules pam_modutil; do
    install -m 0644 "$src/libpam/include/security/$h.h" "$dest/usr/include/security/"
done
install -m 0644 "$src/libpamc/include/security/pam_client.h" "$dest/usr/include/security/"
install -m 0644 "$src/libpam_misc/include/security/pam_misc.h" "$dest/usr/include/security/"
install -m 0644 "$src/modules/pam_filter/pam_filter.h" "$dest/usr/include/security/"
for pair in access:access env:pam_env group:group faillock:faillock limits:limits namespace:namespace pwhistory:pwhistory time:time; do
    module=${pair%%:*} conf=${pair#*:}
    install -m 0644 "$src/modules/pam_$module/$conf.conf" "$dest/etc/security/"
done
install -m 0644 "$src/modules/pam_env/environment" "$dest/etc/environment"
install -m 0755 "$src/modules/pam_namespace/namespace.init" "$dest/etc/security/"
sed 's|@SCONFIGDIR@|/etc/security|g;s|@sbindir@|/sbin|g' \
    "$src/modules/pam_namespace/pam_namespace_helper.in" > "$dest/sbin/pam_namespace_helper"
chmod 0755 "$dest/sbin/pam_namespace_helper"
sed 's|@SCONFIGDIR@|/etc/security|g;s|@sbindir@|/sbin|g' \
    "$src/modules/pam_namespace/pam_namespace.service.in" > "$dest/usr/lib/systemd/system/pam_namespace.service"
for lib in pam pam_misc pamc; do
    cat > "$dest/lib/pkgconfig/$lib.pc" <<PC
prefix=/usr
includedir=\${prefix}/include
libdir=/lib

Name: $lib
Description: Linux-PAM $lib library
Version: 1.7.3
Libs: -L\${libdir} -l$lib
Cflags: -I\${includedir}
PC
done
# Translations are shipped by the release tarball as PO sources. Match upstream
# gettext install when msgfmt is available; require it if catalogs exist.
set -- "$src"/po/*.po
if [ -f "$1" ]; then
    command -v msgfmt >/dev/null || { echo 'PAM: msgfmt required for catalogs' >&2; exit 1; }
    for po in "$src"/po/*.po; do
        language=${po##*/} language=${language%.po}
        mkdir -p "$dest/usr/share/locale/$language/LC_MESSAGES"
        msgfmt -o "$dest/usr/share/locale/$language/LC_MESSAGES/Linux-PAM.mo" "$po"
    done
fi
