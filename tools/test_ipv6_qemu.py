#!/usr/bin/env python3
"""Run IPv6 interoperability against Linux on an isolated, private Ethernet LAN.

Invoke with unshare -Urn. Only an owned image copy, TAP, dnsmasq, echo servers
and QEMU are used. Production kernel and daemon are read back from the image.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import threading
import time
import uuid

import test_vdso_qemu as disk
from run_gcc_probe_qemu import qmp_quit

ROOT = Path(__file__).resolve().parents[1]
PEER = 'fd42:7265:6c69:6566::1'
PORT = 18086
GUEST = '/usr/lib/reliefos/tests/ipv6'


def run(command, **kwargs):
    return subprocess.run(list(map(str, command)), cwd=ROOT, check=True, **kwargs)


def prepare(build, output):
    source = build / 'images/reliefos.vmdk'
    evidence = {'source': str(source), 'source_sha256': disk.file_sha256(source),
                'vmware_verified': False, 'hardware_verified': False}
    run([build / 'sdk/reliefos-musl-sdk/bin/reliefos-musl-cc', '-std=c11', '-O2',
         '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare', '-static',
         ROOT / 'kernel/reliefnt/tools/tests/ipv6_guest.c', '-o', output / 'probe'])
    raw, layout = disk.private_raw_image(source, output)
    filesystem = disk.extract_root(raw, layout, output)
    nonce = uuid.uuid4().hex
    runner = output / 'runner'
    runner.write_text(f'''#!/bin/sh
exec >/dev/ttyS0 2>&1
echo "IPV6_RUN_START nonce={nonce} release=$(uname -r)"
status=0
{GUEST}/probe --loopback || status=$?
    count=0
    while test "$count" -lt 30; do
        grep -q '^nameserver {PEER}$' /etc/resolv.conf && break
        sleep 1; count=$((count + 1))
    done
    echo IPV6_CONFIG
    cat /proc/net/if_inet6 /proc/net/ipv6_route /etc/resolv.conf
    ip -6 addr show || status=$?
    ip -6 route show || status=$?
    ip -6 addr add fd42:7265:6c69:7777::123/64 dev eth0 || status=$?
    ip -6 addr del fd42:7265:6c69:7777::123/64 dev eth0 || status=$?
    ip -6 route add fd42:7265:6c69:8888::/64 dev eth0 || status=$?
    ip -6 route del fd42:7265:6c69:8888::/64 dev eth0 || status=$?
    grep -q '^nameserver {PEER}$' /etc/resolv.conf || status=1
    ping6 -c 2 -W 3 {PEER} || status=$?
    IPV6_GUEST_TEST_DNS=1 {GUEST}/probe --network {PEER} {PORT} || status=$?
    ping -c 2 -W 3 10.0.2.2 || status=$?
    cat /var/log/rdnssd.log /var/log/udhcpc.log 2>/dev/null
echo "IPV6_RUN_DONE nonce={nonce} status=$status"
exit "$status"
''')
    service = output / 'service'
    service.write_text(f'''#!/sbin/openrc-run
description="Private IPv6 interoperability probe"
depend() {{ need reliefos-runtime; after reliefos-dhcp reliefos-rdnss; before xdm reliefos-session; }}
start() {{ /bin/sh {GUEST}/runner; return 0; }}
''')
    commands, directories = [], set()

    def directory(path):
        if path in directories or disk.debugfs_exists(filesystem, path):
            return
        directory(str(Path(path).parent))
        commands.append('mkdir ' + path)
        directories.add(path)

    payload = [(output / 'probe', GUEST + '/probe', '0100755'),
               (runner, GUEST + '/runner', '0100755'),
               (service, '/etc/init.d/reliefos-ipv6-qa', '0100755')]
    for origin, target, mode in payload:
        directory(str(Path(target).parent))
        if disk.debugfs_exists(filesystem, target):
            commands.append('rm ' + target)
        commands += [f'write "{origin}" {target}', f'set_inode_field {target} mode {mode}']
    for link in ('/etc/runlevels/default/xdm', '/etc/runlevels/default/reliefos-session',
                 '/etc/runlevels/default/reliefos-ntp', '/etc/runlevels/default/reliefos-ipv6-qa'):
        if disk.debugfs_exists(filesystem, link):
            commands.append('rm ' + link)
    commands.append('symlink /etc/runlevels/default/reliefos-ipv6-qa /etc/init.d/reliefos-ipv6-qa')
    batch = output / 'inject.commands'
    batch.write_text('\n'.join(commands) + '\n')
    with (output / 'inject.log').open('w') as log:
        run(['debugfs', '-w', '-f', batch, filesystem], stdout=log, stderr=log)
    pins = {}
    for target, origin in [('/usr/lib/reliefos/apps/rdnssd/rdnssd.elf', build / 'userland/rdnssd.elf'),
                           ('/usr/lib/reliefos/publish-resolver', ROOT / 'system/rootfs/usr/lib/reliefos/publish-resolver'),
                           *[(target, origin) for origin, target, _ in payload]]:
        back = output / ('readback-' + Path(target).name)
        run(['debugfs', '-R', f'dump {target} "{back}"', filesystem], capture_output=True)
        if disk.file_sha256(back) != disk.file_sha256(origin):
            raise RuntimeError('root payload readback differs: ' + target)
        pins[target] = disk.file_sha256(back)
    back = output / 'readback-kernel.sys'
    run(['mcopy', '-i', f'{raw}@@{int(layout["esp_first"]) * 512}',
         '::/reliefos/kernel.sys', back], capture_output=True)
    if disk.file_sha256(back) != disk.file_sha256(build / 'generated/system/kernel.sys'):
        raise RuntimeError('image has a stale kernel')
    pins['kernel'] = disk.file_sha256(back)
    run(['e2fsck', '-fn', filesystem], capture_output=True)
    disk.put_root_back(raw, filesystem, layout)
    evidence.update(nonce=nonce, pins=pins, layout=layout)
    return raw, evidence


def echoes(stop, counters, servers):
    threads = []
    def udp(server):
        while not stop.is_set():
            try:
                data, source = server.recvfrom(65535)
                server.sendto(data, source)
                counters['udp'] += 1
            except socket.timeout:
                pass

    def tcp(server):
        while not stop.is_set():
            try:
                client, _ = server.accept()
            except socket.timeout:
                continue
            with client:
                client.settimeout(2)
                try:
                    while data := client.recv(65535):
                        client.sendall(data)
                        counters['tcp'] += 1
                except (socket.timeout, ConnectionError):
                    pass
    for kind, worker in ((socket.SOCK_DGRAM, udp), (socket.SOCK_STREAM, tcp)):
        server = socket.socket(socket.AF_INET6, kind)
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((PEER, PORT))
        if kind == socket.SOCK_STREAM:
            server.listen(4)
        server.settimeout(.25)
        servers.append(server)
        thread = threading.Thread(target=worker, args=(server,))
        thread.start()
        threads.append(thread)
    return threads


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'out/x86_64/release')
    args = parser.parse_args()
    output = args.output.resolve()
    if not output.is_relative_to(ROOT / 'out') or output.exists():
        parser.error('output must be a new directory under this checkout out/')
    namespace = os.open('/proc/self/ns/net', os.O_RDONLY)
    try:
        owner = fcntl.ioctl(namespace, 0xb701)
        try:
            if os.geteuid() or os.fstat(owner).st_ino != os.stat('/proc/self/ns/user').st_ino:
                parser.error('run with unshare -Urn in an owned network namespace')
        finally:
            os.close(owner)
    finally:
        os.close(namespace)
    output.mkdir(parents=True)
    raw, evidence = prepare(args.build_dir.resolve(), output)
    for command in (['ip', 'link', 'set', 'lo', 'up'],
                    ['ip', 'tuntap', 'add', 'dev', 'ipv6qa', 'mode', 'tap'],
                    ['ip', 'addr', 'add', '10.0.2.2/24', 'dev', 'ipv6qa'],
                    ['ip', '-6', 'addr', 'add', PEER + '/64', 'dev', 'ipv6qa', 'nodad'],
                    ['ip', 'link', 'set', 'ipv6qa', 'up']):
        run(command)
    stop, counters, servers = threading.Event(), {'tcp': 0, 'udp': 0}, []
    threads = echoes(stop, counters, servers)
    children, streams = [], []
    qmp = output / 'qmp.sock'
    serial = output / 'serial.log'
    try:
        dnslog = (output / 'dnsmasq.log').open('w'); streams.append(dnslog)
        # Debug/foreground mode retains this private user-namespace identity;
        # dropping supplementary groups is forbidden by unshare -Ur.
        dns = subprocess.Popen(['dnsmasq', '--no-daemon', '--user=root', '--group=root',
            '--conf-file=/dev/null', '--interface=ipv6qa', '--bind-interfaces', '--no-resolv', '--no-hosts',
            '--log-queries', '--log-dhcp', '--no-ping', '--log-facility=-', '--pid-file=' + str(output / 'dnsmasq.pid'),
            '--dhcp-leasefile=' + str(output / 'dnsmasq.leases'),
            '--dhcp-range=10.0.2.15,10.0.2.30,255.255.255.0,1h', '--enable-ra',
            '--dhcp-option=option:dns-server',
            '--dhcp-range=::,constructor:ipv6qa,ra-stateless,64,10m',
            '--dhcp-option=option6:dns-server,[' + PEER + ']',
            '--host-record=ipv6.reliefos.test,' + PEER], stdout=dnslog, stderr=dnslog)
        children.append(dns)
        time.sleep(1.5)
        if dns.poll() is not None:
            raise RuntimeError('dnsmasq failed; see dnsmasq.log')
        capture_log = (output / 'tcpdump.log').open('w'); streams.append(capture_log)
        capture = subprocess.Popen(['tcpdump', '-U', '-n', '-p', '-i', 'ipv6qa',
                                    '-w', str(output / 'wire.pcap')],
                                   stdout=capture_log, stderr=capture_log)
        children.append(capture)
        errors = (output / 'qemu.log').open('w'); streams.append(errors)
        command = ['qemu-system-x86_64', '-accel', 'kvm', '-cpu', 'host', '-machine', 'q35',
            '-m', '1024', '-smp', '2', '-vga', 'vmware', '-display', 'none',
            '-serial', 'file:' + str(serial), '-drive', f'file={raw},format=raw,if=ide',
            '-netdev', 'tap,id=net0,ifname=ipv6qa,script=no,downscript=no',
            '-device', 'e1000,netdev=net0', '-qmp', f'unix:{qmp},server=on,wait=off',
            '-no-reboot', '-no-shutdown']
        vm = subprocess.Popen(command, cwd=ROOT, stdout=errors, stderr=errors)
        children.append(vm)
        evidence['qemu'] = {'pid': vm.pid, 'command': command}
        (output / 'prepare.json').write_text(json.dumps(evidence, indent=2))
        deadline, last = time.monotonic() + 240, ''
        text = ''
        while time.monotonic() < deadline:
            text = serial.read_text(errors='replace') if serial.exists() else ''
            markers = [line for line in text.splitlines() if line.startswith(('IPV6_', 'PASS ', 'FAIL ', 'SKIP '))]
            if markers and markers[-1] != last:
                last = markers[-1]; print(last, flush=True)
            if f'IPV6_RUN_DONE nonce={evidence["nonce"]}' in text or vm.poll() is not None or re.search('kernel panic|bugcheck', text, re.I):
                break
            time.sleep(.25)
        done = f'IPV6_RUN_DONE nonce={evidence["nonce"]} status=0'
        evidence.update(passed=done in text and not re.search('kernel panic|bugcheck|^FAIL ', text, re.I | re.M),
                        peer_counters=counters)
        (output / 'result.json').write_text(json.dumps(evidence, indent=2))
        if not evidence['passed'] or not counters['tcp'] or not counters['udp']:
            raise RuntimeError('IPv6 guest interoperability failed; see serial.log')
        print('PASS QEMU IPv6 + Linux peer, SLAAC/RDNSS/DNS, TCP, fragmented UDP, IPv4', flush=True)
    finally:
        stop.set()
        for process in reversed(children):
            if process.poll() is None:
                if process is children[-1] and qmp.exists():
                    try:
                        qmp_quit(qmp, process)
                    except Exception:
                        process.terminate()
                else:
                    process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait(timeout=5)
        for stream in streams:
            stream.close()
        for thread in threads:
            thread.join(timeout=3)
        for server in servers:
            server.close()


if __name__ == '__main__':
    main()
