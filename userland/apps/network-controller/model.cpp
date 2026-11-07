#include "model.h"
#include <ctype.h>
#include <stdio.h>
namespace nc {
bool append(Text &t, const char *s, size_t n) {
  if (!s || n > MaxPayload || t.size() > MaxPayload - n)
    return false;
  char *b = static_cast<char *>(malloc(t.size() + n + 1));
  if (!b)
    return false;
  memcpy(b, t.c_str(), t.size());
  memcpy(b + t.size(), s, n);
  bool ok = t.set(b, t.size() + n);
  free(b);
  return ok;
}
static bool number(const char *s, unsigned max, unsigned &out) {
  if (!s || !*s)
    return false;
  unsigned n = 0;
  for (; *s; ++s) {
    if (*s < '0' || *s > '9' ||
        n > (max - static_cast<unsigned>(*s - '0')) / 10)
      return false;
    n = n * 10 + *s - '0';
    if (n > max)
      return false;
  }
  out = n;
  return true;
}
bool ipv4(const char *s, uint32_t &out) {
  if (!s)
    return false;
  uint32_t n = 0;
  for (unsigned i = 0; i < 4; ++i) {
    const char *start = s;
    unsigned v = 0, digits = 0;
    while (*s >= '0' && *s <= '9') {
      if (++digits > 3)
        return false;
      v = v * 10 + *s++ - '0';
    }
    if (!digits || v > 255 || (digits > 1 && *start == '0'))
      return false;
    n = (n << 8) | v;
    if (i < 3) {
      if (*s++ != '.')
        return false;
    } else if (*s)
      return false;
  }
  out = n;
  return true;
}
void format_ip(uint32_t ip, char (&s)[16]) {
  snprintf(s, sizeof(s), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255,
           (ip >> 8) & 255, ip & 255);
}
bool subnet(uint32_t ip, unsigned prefix, Subnet &s) {
  if (prefix > 32)
    return false;
  s.mask = prefix ? 0xffffffffu << (32 - prefix) : 0;
  s.network = ip & s.mask;
  s.broadcast = s.network | ~s.mask;
  s.hosts = prefix >= 31 ? 1ULL << (32 - prefix) : (1ULL << (32 - prefix)) - 2;
  s.first = s.network + (prefix < 31);
  s.last = s.broadcast - (prefix < 31);
  return true;
}
static bool unicast(uint32_t ip) { return ip && ip < 0xe0000000u; }
static bool fail(Text &e, const char *s) {
  e.set(s);
  return false;
}
bool valid_config(const Config &c, Text &e) {
  if (c.mode < Mode::Dhcp || c.mode > Mode::Disabled ||
      c.dns < Dns::Automatic || c.dns > Dns::Cloudflare || c.prefix > 32 ||
      c.server_count > 3)
    return fail(e, N_("Invalid network profile."));
  if (c.mode == Mode::Static) {
    Subnet s{};
    subnet(c.address, c.prefix, s);
    if (!unicast(c.address) || (c.address >> 24) == 127 ||
        (c.prefix < 31 && (c.address == s.network || c.address == s.broadcast)))
      return fail(e, N_("Enter a usable static IPv4 address and prefix."));
    if (c.gateway && (!unicast(c.gateway) || c.gateway == c.address ||
                      (c.gateway & s.mask) != s.network ||
                      (c.prefix < 31 &&
                       (c.gateway == s.network || c.gateway == s.broadcast))))
      return fail(
          e,
          "The gateway must be a different usable address in the same subnet.");
    if (c.dns == Dns::Automatic)
      return fail(e, N_("Static mode requires custom or Cloudflare DNS."));
  }
  if (c.dns == Dns::Custom) {
    if (!c.server_count)
      return fail(e, N_("Enter one to three IPv4 DNS servers."));
    for (unsigned i = 0; i < c.server_count; ++i)
      if (!unicast(c.servers[i]))
        return fail(e, N_("Enter one to three IPv4 DNS servers."));
  } else if (c.server_count)
    return fail(e, N_("Only custom DNS accepts server addresses."));
  return true;
}
bool serialize_config(const Config &c, Text &t, Text &e) {
  if (!valid_config(c, e))
    return false;
  char ip[16], gw[16], b[512], dns[64] = {};
  format_ip(c.address, ip);
  format_ip(c.gateway, gw);
  for (unsigned i = 0; i < c.server_count; ++i) {
    char s[16];
    format_ip(c.servers[i], s);
    if (i)
      strcat(dns, " ");
    strcat(dns, s);
  }
  snprintf(b, sizeof(b),
           "version=1\nmode=%s\naddress=%s\nprefix=%u\ngateway=%s\ndns_mode=%"
           "s\ndns_servers=%s\n",
           c.mode == Mode::Dhcp     ? "dhcp"
           : c.mode == Mode::Static ? "static"
                                    : "disabled",
           ip, c.prefix, gw,
           c.dns == Dns::Automatic ? "automatic"
           : c.dns == Dns::Custom  ? "custom"
                                   : "cloudflare",
           dns);
  return t.set(b);
}
bool parse_config(const Text &t, Config &out, Text &e) {
  if (t.empty() || t.size() > MaxConfig || strlen(t.c_str()) != t.size())
    return fail(e, N_("Invalid network profile."));
  Config c;
  unsigned seen = 0;
  bool legacy_dhcp = false;
  const char *s = t.c_str();
  while (*s) {
    const char *end = strchr(s, '\n');
    if (!end)
      end = s + strlen(s);
    size_t n = end - s;
    if (n && s[0] != '#') {
      if (n > 255)
        return fail(e, N_("Invalid network profile."));
      char line[256];
      memcpy(line, s, n);
      line[n] = 0;
      char *v = strchr(line, '=');
      if (!v)
        return fail(e, N_("Invalid network profile."));
      *v++ = 0;
      unsigned bit = 0;
      if (!strcmp(line, "version")) {
        bit = 1;
        if (strcmp(v, "1"))
          return fail(e, N_("Unsupported network profile version."));
      } else if (!strcmp(line, "mode")) {
        bit = 2;
        if (!strcmp(v, "dhcp"))
          c.mode = Mode::Dhcp;
        else if (!strcmp(v, "static"))
          c.mode = Mode::Static;
        else if (!strcmp(v, "disabled"))
          c.mode = Mode::Disabled;
        else
          return fail(e, N_("Invalid network profile."));
      } else if (!strcmp(line, "address")) {
        bit = 4;
        if (!ipv4(v, c.address))
          return fail(e, N_("Invalid IPv4 address."));
      } else if (!strcmp(line, "prefix")) {
        bit = 8;
        if (!number(v, 32, c.prefix))
          return fail(e, N_("Invalid IPv4 prefix."));
      } else if (!strcmp(line, "gateway")) {
        bit = 16;
        if (!ipv4(v, c.gateway))
          return fail(e, N_("Invalid IPv4 gateway."));
      } else if (!strcmp(line, "dns_mode")) {
        bit = 32;
        if (!strcmp(v, "automatic"))
          c.dns = Dns::Automatic;
        else if (!strcmp(v, "dhcp")) {
          c.dns = Dns::Automatic;
          legacy_dhcp = true;
        } else if (!strcmp(v, "custom"))
          c.dns = Dns::Custom;
        else if (!strcmp(v, "cloudflare"))
          c.dns = Dns::Cloudflare;
        else
          return fail(e, N_("Invalid DNS policy."));
      } else if (!strcmp(line, "dns_custom")) {
        bit = 128;
        if (!ipv4(v, c.servers[0]))
          return fail(e, N_("Invalid DNS server."));
        c.server_count = 1;
      } else if (!strcmp(line, "dns_servers")) {
        bit = 64;
        while (*v) {
          if (c.server_count >= 3)
            return fail(e, N_("Enter one to three IPv4 DNS servers."));
          char *space = strchr(v, ' ');
          if (space)
            *space = 0;
          if (!ipv4(v, c.servers[c.server_count++]))
            return fail(e, N_("Invalid DNS server."));
          if (!space)
            break;
          v = space + 1;
          if (!*v)
            return fail(e, N_("Invalid DNS server."));
        }
      } else
        return fail(e, N_("Unknown network profile field."));
      if (seen & bit)
        return fail(e, N_("Duplicate network profile field."));
      seen |= bit;
    }
    s = *end ? end + 1 : end;
  }
  if (!(seen & 1)) {
    if (seen != 32 && seen != 160)
      return fail(e, N_("Incomplete network profile."));
    if ((c.dns == Dns::Custom) != (seen == 160))
      return fail(e, N_("Invalid DNS policy."));
    c.legacy = true;
  } else if (seen != 127 || legacy_dhcp)
    return fail(e, N_("Incomplete network profile."));
  if (!valid_config(c, e))
    return false;
  out = c;
  return true;
}
bool target(const char *s) {
  if (!s || !isalnum(static_cast<unsigned char>(*s)) || strlen(s) > 253)
    return false;
  for (; *s; ++s)
    if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
          (*s >= '0' && *s <= '9') || *s == '.' || *s == '-'))
      return false;
  return true;
}
bool interface_name(const char *s) {
  if (!s || !*s || strlen(s) > 15)
    return false;
  for (; *s; ++s)
    if (!isalnum(static_cast<unsigned char>(*s)) && *s != '_' && *s != '-' &&
        *s != '.')
      return false;
  return true;
}
bool validate(const Request &r, Text &e) {
  const Text *fields[] = {&r.a, &r.b, &r.c, &r.d};
  for (auto f : fields)
    if (f->size() > MaxConfig || strlen(f->c_str()) != f->size())
      return fail(e, N_("Invalid network request."));
  bool ok = false;
  uint32_t ip;
  unsigned port;
  Config config;
  switch (r.op) {
  case Op::Snapshot:
  case Op::ReadConfig:
  case Op::Apply:
  case Op::Renew:
  case Op::Cancel:
  case Op::Quit:
    ok = r.a.empty() && r.b.empty() && r.c.empty() && r.d.empty();
    break;
  case Op::WriteConfig:
    ok = (r.c == "0" || r.c == "1") && r.d.empty() &&
         parse_config(r.a, config, e);
    break;
  case Op::RestoreConfig:
    ok = r.a.empty() && (r.c == "0" || r.c == "1") && r.d.empty();
    break;
  case Op::View:
    ok = (r.a == "routes" || r.a == "arp" || r.a == "connections" ||
          r.a == "lease" || r.a == "log" || r.a == "resolver" ||
          r.a == "report") &&
         r.b.empty() && r.c.empty() && r.d.empty();
    break;
  case Op::Ping:
    ok = target(r.a.c_str()) && r.b.empty() && r.c.empty() && r.d.empty();
    break;
  case Op::DnsQuery:
    ok = target(r.a.c_str()) &&
         (r.b.empty() || (ipv4(r.b.c_str(), ip) && unicast(ip))) &&
         r.c.empty() && r.d.empty();
    break;
  case Op::Tcp:
    ok = target(r.a.c_str()) && number(r.b.c_str(), 65535, port) && port &&
         r.c.empty() && r.d.empty();
    break;
  }
  return ok || fail(e, N_("Invalid or unsupported network request."));
}
static bool arg(Array<Text> &v, const char *s) {
  Text t;
  return t.set(s) && v.push(t);
}
bool command(const Request &r, Array<Text> &v, Text &e) {
  v.clear();
  if (!validate(r, e))
    return false;
  if (r.op == Op::Apply || r.op == Op::Renew)
    return arg(v, "/sbin/rc-service") && arg(v, "reliefos-dhcp") &&
           arg(v, "restart");
  if (r.op == Op::Ping)
    return arg(v, "/bin/busybox") && arg(v, "ping") && arg(v, "-c") &&
           arg(v, "4") && arg(v, "-W") && arg(v, "2") && arg(v, r.a.c_str());
  if (r.op == Op::DnsQuery)
    return arg(v, "/bin/busybox") && arg(v, "nslookup") &&
           arg(v, r.a.c_str()) && (r.b.empty() || arg(v, r.b.c_str()));
  return fail(e, N_("This operation does not execute a command."));
}
static bool u32(Text &b, unsigned v) {
  uint32_t n = v;
  return append(b, reinterpret_cast<const char *>(&n), 4);
}
static bool field(Text &b, const Text &s) {
  return u32(b, s.size()) && append(b, s.c_str(), s.size());
}
bool encode_request(const Request &r, Text &b) {
  b.clear();
  return u32(b, static_cast<unsigned>(r.op)) && field(b, r.a) &&
         field(b, r.b) && field(b, r.c) && field(b, r.d) &&
         b.size() <= MaxPayload;
}
static bool read32(const char *&p, size_t &left, unsigned &v) {
  if (left < 4)
    return false;
  uint32_t n;
  memcpy(&n, p, 4);
  v = n;
  p += 4;
  left -= 4;
  return true;
}
bool decode_request(const Text &b, Request &r) {
  const char *p = b.c_str();
  size_t n = b.size();
  unsigned op, len;
  Request next;
  if (!read32(p, n, op) || op < static_cast<unsigned>(Op::Snapshot) ||
      op > static_cast<unsigned>(Op::Quit))
    return false;
  next.op = static_cast<Op>(op);
  Text *fields[] = {&next.a, &next.b, &next.c, &next.d};
  for (auto *f : fields) {
    if (!read32(p, n, len) || len > n || !f->set(p, len))
      return false;
    p += len;
    n -= len;
  }
  Text error;
  if (n || !validate(next, error))
    return false;
  r = static_cast<Request &&>(next);
  return true;
}
bool encode_packet(const Packet &p, Text &b) {
  b.clear();
  return p.body.size() <= MaxPayload && u32(b, 0x4e435452) &&
         u32(b, p.body.size()) && u32(b, static_cast<unsigned>(p.kind)) &&
         u32(b, p.id) && append(b, p.body.c_str(), p.body.size());
}
int take_packet(Text &b, Packet &result) {
  if (b.size() < 16)
    return 0;
  const char *p = b.c_str();
  size_t left = b.size();
  unsigned magic, len, kind, id;
  if (!read32(p, left, magic) || !read32(p, left, len) ||
      !read32(p, left, kind) || !read32(p, left, id))
    return -1;
  if (magic != 0x4e435452 || len > MaxPayload || kind < 1 || kind > 4)
    return -1;
  if (left < len)
    return 0;
  Packet next;
  next.kind = static_cast<Kind>(kind);
  next.id = id;
  Text rest;
  if (!next.body.set(p, len) || !rest.set(p + len, left - len))
    return -1;
  result = static_cast<Packet &&>(next);
  b = rest;
  return 1;
}

bool encode_profile(bool exists, const Text &original, const Config &config,
                    Text &out) {
  Text canonical, error;
  if (original.size() > MaxConfig || (!exists && !original.empty()) ||
      !serialize_config(config, canonical, error))
    return false;
  out.clear();
  return u32(out, exists ? 1 : 0) && field(out, original) &&
         field(out, canonical);
}
bool decode_profile(const Text &body, bool &exists, Text &original,
                    Config &config) {
  const char *p = body.c_str();
  size_t left = body.size();
  unsigned flag, len;
  Text raw, canonical, error;
  Config next;
  if (!read32(p, left, flag) || flag > 1 || !read32(p, left, len) ||
      len > left || len > MaxConfig || !raw.set(p, len))
    return false;
  p += len;
  left -= len;
  if (!read32(p, left, len) || len != left || len > MaxConfig ||
      !canonical.set(p, len) || !parse_config(canonical, next, error) ||
      (!flag && !raw.empty()))
    return false;
  exists = flag != 0;
  original = raw;
  config = next;
  return true;
}
void parse_counters(const char *s, Array<Interface> &interfaces) {
  while (s && *s) {
    const char *end = strchr(s, '\n');
    if (!end)
      end = s + strlen(s);
    const char *colon = static_cast<const char *>(memchr(s, ':', end - s));
    if (colon) {
      const char *name = s;
      while (name < colon && *name == ' ')
        ++name;
      size_t len = colon - name;
      unsigned long long v[16]{};
      if (sscanf(colon + 1,
                 "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu "
                 "%llu %llu %llu %llu",
                 &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8],
                 &v[9], &v[10], &v[11], &v[12], &v[13], &v[14], &v[15]) == 16)
        for (size_t i = 0; i < interfaces.size(); ++i)
          if (interfaces[i].name.size() == len &&
              !memcmp(interfaces[i].name.c_str(), name, len)) {
            interfaces[i].counters = true;
            interfaces[i].rx = v[0];
            interfaces[i].rx_packets = v[1];
            interfaces[i].rx_errors = v[2];
            interfaces[i].tx = v[8];
            interfaces[i].tx_packets = v[9];
            interfaces[i].tx_errors = v[10];
          }
    }
    s = *end ? end + 1 : end;
  }
}
bool parse_snapshot(const Text &t, Array<Interface> &out, Text &gateway,
                    Text &dns) {
  Array<Interface> next;
  Text g, d;
  const char *s = t.c_str();
  while (*s) {
    const char *end = strchr(s, '\n');
    if (!end)
      end = s + strlen(s);
    if (end - s > 1023)
      return false;
    char line[1024];
    memcpy(line, s, end - s);
    line[end - s] = 0;
    if (line[0] == 'I' && line[1] == '\t') {
      char name[16], ip[16], mask[16], mac[32];
      unsigned flags, mtu, counters;
      unsigned long long rx, tx, rp, tp, re, te;
      int used = 0;
      if (sscanf(line + 2,
                 "%15s %15s %15s %31s %u %u %llu %llu %llu %llu %llu %llu %u%n",
                 name, ip, mask, mac, &flags, &mtu, &rx, &tx, &rp, &tp, &re,
                 &te, &counters, &used) != 13 ||
          line[used + 2] || !interface_name(name) || next.size() >= 64)
        return false;
      Interface i;
      if (!i.name.set(name) || !i.address.set(ip) || !i.mask.set(mask) ||
          !i.mac.set(mac))
        return false;
      i.counters = counters != 0;
      i.flags = flags;
      i.mtu = mtu;
      i.rx = rx;
      i.tx = tx;
      i.rx_packets = rp;
      i.tx_packets = tp;
      i.rx_errors = re;
      i.tx_errors = te;
      if (!next.push(i))
        return false;
    } else if (line[0] == 'G' && line[1] == '\t') {
      if (!g.set(line + 2))
        return false;
    } else if (line[0] == 'D' && line[1] == '\t') {
      if (!d.empty() && !append(d, ", "))
        return false;
      if (!append(d, line + 2))
        return false;
    } else
      return false;
    s = *end ? end + 1 : end;
  }
  out.swap(next);
  gateway = g;
  dns = d;
  return true;
}
} // namespace nc
