#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
export TMPDIR="$src/out/tmp"
mkdir -p "$TMPDIR"
work=$(mktemp -d "$TMPDIR/network-profile.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
root=$work/root
mkdir -p "$root/bin" "$root/sbin" "$root/etc/reliefos" "$root/etc/udhcpc" "$root/etc/init.d" "$root/usr/lib/reliefos" "$root/usr/share/udhcpc" "$root/run/reliefos" "$root/dev" "$root/proc/net"
# Only this private chroot can see the fake adapter commands. No host network
# command or resolver path is used by the production scripts under test.
for utility in sh stat cat mkdir mv rm chmod mktemp grep tr wc flock; do
    binary=$(command -v "$utility")
    cp "$binary" "$root/bin/$utility"
    ldd "$binary" | awk '{for(i=1;i<=NF;i++)if($i~/^\//)print $i}' | while read -r lib; do
        mkdir -p "$root$(dirname "$lib")";cp "$lib" "$root$lib"
    done
done
cp "$src/system/rootfs/usr/lib/reliefos/network-profile" "$src/system/rootfs/usr/lib/reliefos/publish-resolver" "$root/usr/lib/reliefos/"
cp "$src/system/rootfs/etc/init.d/reliefos-dhcp" "$root/etc/init.d/"
cp "$src/system/rootfs/usr/share/udhcpc/default.script" "$root/usr/share/udhcpc/"
printf 'Inter-| Receive | Transmit\n  eth0: 0\n  eth1: 0\n' > "$root/proc/net/dev"
printf '100.00 0.00\n' > "$root/proc/uptime"
cat > "$root/sbin/ifconfig" <<'SCRIPT'
#!/bin/sh
printf '%s' "$0" >> /commands
for arg do printf ' <%s>' "$arg" >> /commands; done
printf '\n' >> /commands
SCRIPT
cp "$root/sbin/ifconfig" "$root/sbin/route"
cat > "$root/sbin/supervise-daemon" <<'SCRIPT'
#!/bin/sh
printf '%s' "$RC_SVCNAME" >> /events
for arg do printf ' <%s>' "$arg" >> /events; done
printf '\n' >> /events
SCRIPT
cat > "$root/sbin/rc-service" <<'SCRIPT'
#!/bin/sh
case "$2" in
    status) test ! -e /service-stopped ;;
    start) printf 'rc-service-start\n' >> /events; rm -f /service-stopped ;;
    *) exit 1 ;;
esac
SCRIPT
chmod 755 "$root/sbin/ifconfig" "$root/sbin/route" "$root/sbin/supervise-daemon" "$root/sbin/rc-service"
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
# Different profiles and leases belong to the named, enumerated adapter.
write_profile dhcp automatic ''
printf '10.0.0.53\n' > /run/reliefos/dhcp-dns
mkdir -p /etc/reliefos/network.d
printf 'version=1\nmode=static\naddress=192.168.9.2\nprefix=24\ngateway=192.168.9.1\ndns_mode=custom\ndns_servers=9.9.9.9\n' > /etc/reliefos/network.d/eth1.conf
test "$("$tool" mode eth1)" = static
"$tool" prepare eth1
grep -q '<eth1> <192.168.9.2> <netmask> <255.255.255.0> <up>' /commands
test "$(grep '^nameserver ' /etc/resolv.conf)" = "nameserver 9.9.9.9
nameserver 10.0.0.53"
"$tool" stop eth1
grep -q '^nameserver 10.0.0.53$' /etc/resolv.conf
if grep -q '^nameserver 9.9.9.9$' /etc/resolv.conf; then echo 'FAIL - stopped interface retained explicit DNS'; exit 1; fi
"$tool" prepare eth1
if "$tool" mode '../eth1'; then echo 'FAIL - invalid interface accepted'; exit 1; fi
if "$tool" mode eth9; then echo 'FAIL - nonexistent interface accepted'; exit 1; fi
mkdir /proc/123
printf '/sbin/udhcpc\000-f\000-i\000eth0\000-p\000/run/reliefos/udhcpc.pid\000' > /proc/123/cmdline
printf '123\n' > /run/reliefos/udhcpc.eth1.pid
if "$tool" stop-dhcp eth1; then echo 'FAIL - another interface PID accepted'; exit 1; fi
rm -f /run/reliefos/udhcpc.eth1.pid
printf 'version=1\nmode=dhcp\naddress=0.0.0.0\nprefix=24\ngateway=0.0.0.0\ndns_mode=automatic\ndns_servers=\n' > /etc/reliefos/network.d/eth1.conf
printf '10.0.0.53\n' > /run/reliefos/dhcp-dns
interface=eth1 ip=192.168.9.2 subnet=255.255.255.0 router=192.168.9.1 dns=192.168.9.53 lease=300 /usr/share/udhcpc/default.script bound
grep -q '^nameserver 10.0.0.53$' /etc/resolv.conf
grep -q '^nameserver 192.168.9.53$' /etc/resolv.conf
test -f /run/reliefos/dhcp-lease.eth1
interface=eth1 /usr/share/udhcpc/default.script deconfig
test -f /run/reliefos/dhcp-dns
test ! -f /run/reliefos/dhcp-lease.eth1
grep -q '^nameserver 10.0.0.53$' /etc/resolv.conf
if grep -q '^nameserver 192.168.9.53$' /etc/resolv.conf; then echo 'FAIL - stale DNS retained'; exit 1; fi
# OpenRC starts every real Ethernet interface, and apply only stops one.
write_profile dhcp automatic ''
printf 'version=1\nmode=disabled\naddress=0.0.0.0\nprefix=24\ngateway=0.0.0.0\ndns_mode=automatic\ndns_servers=\n' > /etc/reliefos/network.d/eth1.conf
. /etc/init.d/reliefos-dhcp
start
grep -q 'reliefos-dhcp.eth0 <reliefos-dhcp.eth0> <--start>' /events
grep -q '<eth1> <down>' /commands
printf 'version=1\nmode=dhcp\naddress=0.0.0.0\nprefix=24\ngateway=0.0.0.0\ndns_mode=automatic\ndns_servers=\n' > /etc/reliefos/network.d/eth1.conf
: > /events
"$tool" apply eth1
grep -q 'reliefos-dhcp.eth1 <reliefos-dhcp.eth1> <--stop>' /events
grep -q 'reliefos-dhcp.eth1 <reliefos-dhcp.eth1> <--start>' /events
if grep -q 'reliefos-dhcp.eth0' /events; then echo 'FAIL - applying eth1 interrupted eth0'; exit 1; fi
stop
grep -q 'reliefos-dhcp.eth0 <reliefos-dhcp.eth0> <--stop>' /events
: > /events
: > /service-stopped
"$tool" apply eth1
grep -q '^rc-service-start$' /events
printf 'PASS Network profiles: per-interface modes, DNS priority, lease isolation and scoped apply\n'
SCRIPT
LC_ALL=C LANG=C unshare -Ur chroot "$root" /bin/sh /test.sh
