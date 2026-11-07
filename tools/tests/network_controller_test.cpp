#include "model.h"
#include <assert.h>
#include <stdio.h>
using namespace nc;
static Text text(const char *s) {
  Text t;
  assert(t.set(s));
  return t;
}
int main() {
  uint32_t ip = 0;
  assert(ipv4("10.0.2.15", ip) && ip == 0x0a00020f);
  const char *invalid_ips[] = {"",          "1.2.3",      "01.2.3.4",
                               "1.2.3.256", "1.2.3.4;id", "1.2.3.4\n",
                               "-1.2.3.4"};
  for (const char *s : invalid_ips)
    assert(!ipv4(s, ip));
  char out[16];
  format_ip(0xc0a80101, out);
  assert(!strcmp(out, "192.168.1.1"));
  Subnet n{};
  assert(subnet(0xc0a80a7b, 24, n));
  assert(n.mask == 0xffffff00 && n.network == 0xc0a80a00 &&
         n.broadcast == 0xc0a80aff && n.hosts == 254);
  assert(n.first == 0xc0a80a01 && n.last == 0xc0a80afe);
  assert(subnet(0x0a000201, 31, n) && n.hosts == 2 && n.first == 0x0a000200);
  assert(subnet(0x0a000201, 32, n) && n.hosts == 1 && n.first == n.last);
  assert(subnet(0, 0, n) && n.hosts == 4294967294ULL);
  assert(!subnet(1, 33, n));
  Config c;
  Text error, encoded;
  assert(valid_config(c, error));
  c.mode = Mode::Static;
  c.address = 0x0a00020f;
  c.gateway = 0x0a000202;
  assert(!valid_config(c, error)); // Static has no DHCP DNS provider.
  c.dns = Dns::Custom;
  c.servers[0] = 0x01010101;
  c.servers[1] = 0x08080808;
  c.server_count = 2;
  assert(valid_config(c, error));
  assert(serialize_config(c, encoded, error));
  assert(encoded ==
         "version=1\nmode=static\naddress=10.0.2.15\nprefix=24\ngateway=10.0.2."
         "2\ndns_mode=custom\ndns_servers=1.1.1.1 8.8.8.8\n");
  Config decoded;
  Config legacy;
  assert(parse_config(text("dns_mode=dhcp\n"), legacy, error) &&
         legacy.legacy && legacy.mode == Mode::Dhcp &&
         legacy.dns == Dns::Automatic);
  assert(parse_config(text("dns_mode=custom\ndns_custom=9.9.9.9\n"), legacy,
                      error) &&
         legacy.dns == Dns::Custom && legacy.servers[0] == 0x09090909);
  assert(parse_config(encoded, decoded, error));
  assert(decoded.address == 0x0a00020f && decoded.server_count == 2);
  assert(!parse_config(text("version=1\nmode=static\n"), decoded, error));
  Text bad = encoded;
  assert(append(bad, "mode=dhcp\n"));
  assert(!parse_config(bad, decoded, error));
  bad = encoded;
  assert(append(bad, "command=id\n"));
  assert(!parse_config(bad, decoded, error));
  c.gateway = 0xc0a80101;
  assert(!valid_config(c, error));
  c.gateway = 0x0a000202;
  c.address = 0x0a000200;
  assert(!valid_config(c, error));
  c.address = 0x0a0002ff;
  assert(!valid_config(c, error));
  assert(target("example.com") && target("10.0.2.2") && !target("-c") &&
         !target("x;id") && !target("a b") && !target("$(id)"));
  Request r;
  r.op = Op::Ping;
  r.a = text("-f");
  assert(!validate(r, error));
  r.a = text("10.0.2.2");
  Array<Text> args;
  assert(command(r, args, error));
  assert(args.size() == 7 && args[0] == "/bin/busybox" && args[1] == "ping" &&
         args[6] == "10.0.2.2");
  r.op = Op::Tcp;
  r.b = text("65536");
  assert(!validate(r, error));
  r.b = text("443");
  assert(validate(r, error));
  r.op = Op::View;
  r.a = text("../../etc/shadow");
  r.b.clear();
  assert(!validate(r, error));
  r.op = Op::DnsQuery;
  r.a = text("example.com");
  r.b = text("1.1.1.1");
  assert(command(r, args, error));
  assert(args.size() == 4 && args[1] == "nslookup" && args[3] == "1.1.1.1");
  Text body, frame, input;
  assert(encode_request(r, body));
  Request roundtrip;
  assert(decode_request(body, roundtrip) && roundtrip.a == "example.com");
  Text profile, original;
  bool exists;
  assert(encode_profile(true, text("dns_mode=dhcp\n"), decoded, profile));
  assert(decode_profile(profile, exists, original, decoded) && exists &&
         original == "dns_mode=dhcp\n" && decoded.dns == Dns::Custom);
  Packet p;
  p.id = 42;
  p.body = body;
  assert(encode_packet(p, frame));
  Packet got;
  assert(input.set(frame.c_str(), 9));
  assert(take_packet(input, got) == 0);
  assert(append(input, frame.c_str() + 9, frame.size() - 9));
  assert(take_packet(input, got) == 1 && got.id == 42 && input.empty());
  char invalid[16] = {};
  assert(input.set(invalid, sizeof(invalid)));
  assert(take_packet(input, got) == -1);
  Array<Interface> interfaces;
  Text gateway, dns;
  assert(parse_snapshot(
      text("I\teth0\t10.0.2.15\t255.255.255.0\t52:54:00:12:34:"
           "56\t65\t1500\t4096\t2048\t12\t8\t0\t1\t1\nG\t10.0.2.2\nD\t10.0.2.3\n"),
      interfaces, gateway, dns));
  assert(interfaces.size() == 1 && interfaces[0].rx == 4096 &&
         interfaces[0].tx_errors == 1 && gateway == "10.0.2.2" &&
         dns == "10.0.2.3");
  parse_counters("Inter-| Receive | Transmit\n  eth0: 9000 32 2 0 0 0 0 0 5000 "
                 "19 1 0 0 0 0 0\n",
                 interfaces);
  assert(interfaces[0].counters && interfaces[0].rx == 9000 && interfaces[0].tx == 5000 &&
         interfaces[0].rx_errors == 2);
  puts("PASS Network Controller: IPv4, subnet boundaries, profiles, injection "
       "rejection and fragmented protocol");
}
