#include "model.h"
#include <stdio.h>
static int tests;
#define CHECK(x)                                                               \
  do {                                                                         \
    ++tests;                                                                   \
    if (!(x)) {                                                                \
      fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                          \
      return 1;                                                                \
    }                                                                          \
  } while (0)
int main() {
  using namespace oc;
  CHECK(identifier("reliefos-audio"));
  CHECK(identifier("net.eth0"));
  CHECK(identifier("foo@bar+1"));
  const char *bad[] = {"",        "..",     ".hidden",  "-foo",    "../foo",
                       "foo/bar", "foo;id", "foo\nbar", "foo bar", nullptr};
  for (const char **p = bad; *p; ++p)
    CHECK(!identifier(*p));
  CHECK(protected_level("sysinit"));
  CHECK(protected_level("shutdown"));
  CHECK(!protected_level("work"));
  Request r;
  Text error;
  Array<Text> args;
  r.op = Op::Service;
  r.a.set("reliefos-audio");
  r.b.set("restart");
  CHECK(command(r, args, error));
  CHECK(args.size() == 3);
  CHECK(args[0] == "/sbin/rc-service");
  CHECK(args[2] == "restart");
  r.b.set("--nodeps");
  CHECK(!command(r, args, error));
  r.b.set("custom_health");
  CHECK(command(r, args, error)); // custom command is one literal argv
  r.c.set("--dry-run");
  CHECK(!command(r, args, error));
  r.c.set("dry-run");
  CHECK(command(r, args, error));
  CHECK(args[1] == "--dry-run");
  r.op = Op::Add;
  r.b.set("default");
  r.c.clear();
  CHECK(command(r, args, error));
  CHECK(args[0] == "/sbin/rc-update");
  CHECK(args[1] == "add");
  r.b.set("../etc");
  CHECK(!validate(r, error));
  r.op = Op::SwitchLevel;
  r.a.set("sysinit");
  r.b.clear();
  CHECK(!validate(r, error));
  r.a.set("work");
  CHECK(command(r, args, error));
  r.op = Op::ReadConfig;
  r.a.set("../../shadow");
  CHECK(!validate(r, error));
  r.a.set("@global");
  CHECK(validate(r, error));
  r.op = Op::Logs;
  r.a.set("/etc/shadow");
  CHECK(!validate(r, error));
  r.a.set("rc.log");
  CHECK(validate(r, error));
  r.op = Op::Service;
  r.a.set("foo");
  r.b.set("start");
  r.c.clear();
  r.d.clear();
  Text wire;
  CHECK(encode_request(r, wire));
  Request decoded;
  CHECK(decode_request(wire, decoded));
  CHECK(decoded.op == r.op && decoded.a == r.a && decoded.b == r.b);
  Packet p;
  p.id = 42;
  p.kind = Kind::Request;
  p.body = wire;
  Text frame, input;
  CHECK(encode_packet(p, frame));
  Packet q;
  for (size_t n = 0; n + 1 < frame.size(); ++n) {
    CHECK(append(input, frame.c_str() + n, 1));
    CHECK(take_packet(input, q) == 0);
  }
  CHECK(append(input, frame.c_str() + frame.size() - 1, 1));
  CHECK(take_packet(input, q) == 1);
  CHECK(q.id == 42 && q.body == wire && input.empty());
  CHECK(append(input, frame.c_str(), frame.size()));
  CHECK(append(input, frame.c_str(), frame.size()));
  CHECK(take_packet(input, q) == 1);
  CHECK(take_packet(input, q) == 1);
  CHECK(input.empty());
  CHECK(input.set("BAD!abcdefghijkl", 16));
  CHECK(take_packet(input, q) == -1);
  Catalog c;
  CHECK(parse_catalog("C\tdefault\nL\tboot\nL\tdefault\nS\taudio\tdefault "
                      "boot\nS\tunassigned\t\n",
                      c));
  CHECK(c.services.size() == 2 && c.levels.size() == 2 &&
        c.current == "default");
  CHECK(parse_status("audio = started 01:24:32 (2)\nunassigned = "
                     "stopped\ncrash = crashed\nfail = failed\n",
                     c));
  CHECK(c.services.size() == 4);
  CHECK(c.services[0].state == "started");
  CHECK(c.services[0].uptime == "01:24:32 (2)");
  CHECK(matches(c.services[0], "AUDIO", 1, "default"));
  CHECK(!matches(c.services[0], "audio", 2, ""));
  CHECK(matches(c.services[1], "", 4, ""));
  CHECK(matches(c.services[2], "", 3, ""));
  CHECK(matches(c.services[3], "", 3, ""));
  CHECK(!has_word("default-extra boot", "default"));
  CHECK(has_word("boot default", "default"));
  CHECK(!parse_catalog("S\t../bad\tdefault\n", c));
  CHECK(c.services.size() == 4); // transactional parse
  RefreshModel refresh;
  CHECK(refresh.accept_catalog("C\tdefault\nL\tdefault\nS\tnew\tdefault\n"));
  CHECK(c.services.size() == 4 && c.services[0].name == "audio");
  CHECK(refresh.accept_state("new = started\n", c));
  CHECK(c.services.size() == 1 && c.services[0].name == "new" &&
        c.services[0].state == "started");
  CHECK(!refresh.accept_state("new = stopped\n", c));
  CHECK(c.services[0].state == "started");
  CHECK(!refresh.accept_catalog("S\t../bad\tdefault\n"));
  CHECK(!refresh.accept_state("new = stopped\n", c));
  CHECK(c.services[0].state == "started");
  puts("OpenRC Controller model/protocol tests passed");
  printf("%d checks\n", tests);
  return 0;
}
