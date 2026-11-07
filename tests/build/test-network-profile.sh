#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
export TMPDIR="$src/out/tmp"
mkdir -p "$TMPDIR"
work=$(mktemp -d "$TMPDIR/network-profile.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
root=$work/root
mkdir -p "$root/bin" "$root/sbin" "$root/etc/reliefos" "$root/etc/udhcpc" "$root/etc/init.d" "$root/usr/lib/reliefos" "$root/run/reliefos" "$root/dev"
# Only this private chroot can see the fake adapter commands. No host network
# command or resolver path is used by the production scripts under test.
for utility in sh stat cat mkdir mv rm chmod mktemp grep tr wc; do
    binary=$(command -v "$utility")
    cp "$binary" "$root/bin/$utility"
    ldd "$binary" | awk '{for(i=1;i<=NF;i++)if($i~/^\//)print $i}' | while read -r lib; do
        mkdir -p "$root$(dirname "$lib")";cp "$lib" "$root$lib"
    done
done
cp "$src/system/rootfs/usr/lib/reliefos/network-profile" "$src/system/rootfs/usr/lib/reliefos/publish-resolver" "$root/usr/lib/reliefos/"
cp "$src/system/rootfs/etc/init.d/reliefos-dhcp" "$root/etc/init.d/"
cat > "$root/sbin/ifconfig" <<'SCRIPT'
#!/bin/sh
printf '%s' "$0" >> /commands
for arg do printf ' <%s>' "$arg" >> /commands; done
printf '\n' >> /commands
SCRIPT
cp "$root/sbin/ifconfig" "$root/sbin/route"
chmod 755 "$root/sbin/ifconfig" "$root/sbin/route"
touch "$root/dev/null"
cat > "$root/test.sh" <<'SCRIPT'
set -eu
export PATH=/bin:/sbin
profile=/etc/reliefos/network.conf
tool=/usr/lib/reliefos/network-profile
publisher=/usr/lib/reliefos/publish-resolver
write_profile() {
    printf 'version=1\nmode=%s\naddress=10.0.2.15\nprefix=24\ngateway=10.0.2.2\ndns_mode=%s\ndns_servers=%s\n' "$1" "$2" "$3" > "$profile"
    chmod 644 "$profile"
}
test "$($tool mode)" = dhcp
printf '$(touch /injected)\n' > /run/reliefos/udhcpc.pid
if $tool stop-dhcp; then echo 'FAIL - malicious DHCP PID accepted';exit 1;fi
test ! -e /injected
printf '999999999\n' > /run/reliefos/udhcpc.pid
$tool stop-dhcp
test ! -e /run/reliefos/udhcpc.pid
printf 'dns_mode=dhcp\n' > "$profile"
test "$($tool mode)" = dhcp
printf '2 9.9.9.9\n' > /etc/udhcpc/leonos-dns
$publisher
grep -q '^nameserver 9.9.9.9$' /etc/resolv.conf
write_profile static custom '1.1.1.1 8.8.8.8'
test "$($tool mode)" = static
test "$($tool dns)" = '2 1.1.1.1 8.8.8.8'
$tool prepare
grep -q '<10.0.2.15> <netmask> <255.255.255.0> <up>' /commands
grep -q '<add> <default> <gw> <10.0.2.2> <dev> <eth0>' /commands
grep -q '^nameserver 1.1.1.1$' /etc/resolv.conf
grep -q '^nameserver 8.8.8.8$' /etc/resolv.conf
write_profile static automatic ''
if $tool check; then echo 'FAIL - static mode silently accepted DHCP DNS';exit 1;fi
write_profile static custom '1.1.1.1'
printf 'mode=dhcp\n' >> "$profile"
if $tool check; then echo 'FAIL - duplicate field accepted';exit 1;fi
write_profile dhcp automatic ''
printf 'command=touch /injected\n' >> "$profile"
if $tool prepare; then echo 'FAIL - unknown command field accepted';exit 1;fi
test ! -e /injected
write_profile dhcp automatic ''
printf '\000' >> "$profile"
if $tool check; then echo 'FAIL - embedded NUL accepted';exit 1;fi
write_profile dhcp automatic ''
chmod 666 "$profile"
if $tool check; then echo 'FAIL - writable profile accepted';exit 1;fi
write_profile static cloudflare ''
# Exercise the actual service functions; the supervisor boundary is isolated.
. /etc/init.d/reliefos-dhcp
ebegin() { :; }
eend() { return "$1"; }
default_start() { printf 'dhcp-start\n' >> /events; }
default_stop() { printf 'dhcp-stop\n' >> /events; }
service_set_value() { printf '%s\n' "$2" > /mode-value; }
service_get_value() { cat /mode-value; }
start_pre;start
write_profile dhcp automatic ''
stop;stop_post
test ! -e /events
start_pre;start
write_profile disabled automatic ''
stop;stop_post
grep -q '^dhcp-stop$' /events
start_pre;start
grep -q '<eth0> <down>' /commands
test "$(cat /mode-value)" = disabled
printf 'PASS Network profile: static/DHCP/disabled, resolver compatibility, validation and old-mode stop\n'
SCRIPT
LC_ALL=C LANG=C unshare -Ur chroot "$root" /bin/sh /test.sh
