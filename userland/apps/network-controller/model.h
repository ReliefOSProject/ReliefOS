#ifndef NETWORK_CONTROLLER_MODEL_H
#define NETWORK_CONTROLLER_MODEL_H
#include "storage.h"
#ifndef N_
#define N_(text) text
#endif
namespace nc {
constexpr size_t MaxPayload = 1024 * 1024, MaxConfig = 16384;
constexpr const char *WorkerPath =
    "/usr/lib/reliefos/apps/network-controller/network-controller.elf";
enum class Mode { Dhcp, Static, Disabled };
enum class Dns { Automatic, Custom, Cloudflare };
struct Config {
  bool legacy = false;
  Mode mode = Mode::Dhcp;
  uint32_t address = 0, gateway = 0, servers[3]{};
  unsigned prefix = 24, server_count = 0;
  Dns dns = Dns::Automatic;
};
struct Subnet {
  uint32_t mask, network, broadcast, first, last;
  uint64_t hosts;
};
bool ipv4(const char *, uint32_t &);
void format_ip(uint32_t, char (&)[16]);
bool subnet(uint32_t, unsigned, Subnet &);
bool valid_config(const Config &, Text &);
bool parse_config(const Text &, Config &, Text &);
bool serialize_config(const Config &, Text &, Text &);
bool target(const char *);
bool interface_name(const char *);
bool append(Text &, const char *, size_t);
inline bool append(Text &t, const char *s) { return append(t, s, strlen(s)); }
enum class Op : unsigned {
  Snapshot = 1,
  ReadConfig,
  WriteConfig,
  RestoreConfig,
  Apply,
  Renew,
  View,
  Ping,
  DnsQuery,
  Tcp,
  Cancel,
  Quit
};
enum class Kind : unsigned { Request = 1, Output, Done, Ready };
struct Request {
  Op op = Op::Snapshot;
  Text a, b, c, d;
};
struct Packet {
  Kind kind = Kind::Request;
  unsigned id = 0;
  Text body;
};
bool validate(const Request &, Text &);
bool command(const Request &, Array<Text> &, Text &);
bool encode_request(const Request &, Text &);
bool decode_request(const Text &, Request &);
bool encode_packet(const Packet &, Text &);
int take_packet(Text &, Packet &);
bool encode_profile(bool exists, const Text &original, const Config &, Text &);
bool decode_profile(const Text &, bool &exists, Text &original, Config &);
struct Interface {
  bool counters = false;
  Text name, address, mask, mac;
  unsigned flags = 0, mtu = 0;
  uint64_t rx = 0, tx = 0, rx_packets = 0, tx_packets = 0, rx_errors = 0,
           tx_errors = 0;
};
bool parse_snapshot(const Text &, Array<Interface> &, Text &gateway, Text &dns);
void parse_counters(const char *, Array<Interface> &);
} // namespace nc
#endif
