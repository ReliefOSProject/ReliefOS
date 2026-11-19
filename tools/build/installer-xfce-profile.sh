#!/bin/sh
# Derive an offline XFCE payload using the same verified official APKs and
# signed local packages as the IceWM system. No build-time network access.
set -eu
umask 022
[ "$#" = 7 ] || { echo 'usage: installer-xfce-profile SRC BASE OUTPUT WORK APK UPSTREAM DEPS' >&2; exit 2; }
src=$1 base=$2 output=$3 work=$4 apk=$5 upstream=$6 deps=$7
: "${APK_KEY:?}" "${APK_LOCK:=$src/configs/dependencies.lock.json}"
[ -f "$base/lib/apk/db/installed" ] || { echo 'missing base package database' >&2; exit 1; }
mkdir -p "$work"
cp -a "$base" "$output"
repository=$output/usr/share/reliefos/apk/repository
"$deps" --lock "$APK_LOCK" --list > "$work/xfce-ids"
count=0
while IFS= read -r id; do
    case $id in alpine-*) ;; *) continue ;; esac
    feature=$("$deps" --lock "$APK_LOCK" --id "$id" --print feature)
    [ "$feature" = xfce ] || continue
    url=$("$deps" --lock "$APK_LOCK" --id "$id" --print url)
    archive=$upstream/packages/${url##*/}
    [ -f "$archive" ] || { echo "missing verified XFCE archive: $id" >&2; exit 1; }
    cp "$archive" "$repository/"
    count=$((count + 1))
done < "$work/xfce-ids"
[ "$count" -gt 0 ] || { echo 'no XFCE packages in the dependency lock' >&2; exit 1; }
set -- "$apk" mkndx --keys-dir "$output/etc/apk/keys" --sign-key "$APK_KEY" --output "$repository/packages.adb"
for archive in "$repository"/*.apk; do set -- "$@" "$archive"; done
"$@"
set -- "$apk"
if [ "$(id -u)" != 0 ]; then set -- "$@" --usermode; fi
"$@" --root "$output" --arch x86_64 --no-network --repositories-file /dev/null \
    --repository "$repository/packages.adb" add xfce4 xfce4-terminal dbus dbus-x11 dbus-openrc
for program in startxfce4 xfce4-session xfce4-terminal dbus-run-session; do
    [ -x "$output/usr/bin/$program" ] || { echo "XFCE payload is missing $program" >&2; exit 1; }
done

target_command() {
    if [ "$(id -u)" = 0 ]; then chroot "$output" "$@";
    else unshare -Ur chroot "$output" "$@"; fi
}
service_account() {
    # The upstream pre-install scripts use BusyBox adduser/addgroup; ReliefOS
    # uses Shadow to manage the standard password and group files.
    target_command /usr/sbin/groupadd -rf "$1"
    if ! target_command /usr/bin/id -u "$1" >/dev/null 2>&1; then
        target_command /usr/sbin/useradd -r -M -d "$2" -s /sbin/nologin -g "$1" "$1"
    fi
}
service_account messagebus /dev/null
service_account polkitd /var/empty
# D-Bus/OpenRC creates an installation-specific machine ID on first boot.
rm -f "$output/etc/machine-id"

# Provide an XFCE default; Xfconf still honors per-user preferences.
install -Dm 0644 "$src/system/xorg/xfce-xfwm4.xml" \
    "$output/etc/xdg/xfce4/xfconf/xfce-perchannel-xml/xfwm4.xml"
mkdir -p "$output/etc/X11" "$output/etc/xdg/autostart" "$output/etc/runlevels/default"
cp "$src/system/xorg/xfce-xinitrc" "$output/etc/X11/Xsession"
chmod 755 "$output/etc/X11/Xsession"
ln -sf ../../init.d/dbus "$output/etc/runlevels/default/dbus"
cat > "$output/etc/xdg/autostart/reliefclipboard.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=ReliefOS Clipboard
Exec=reliefclipboard --daemon
TryExec=reliefclipboard
OnlyShowIn=XFCE;
NoDisplay=true
DESKTOP
