#include "session.h"
#include "worker.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
using namespace nc;
static double now() {
  struct timespec t{};
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}
static Text text(const char *s) {
  Text t;
  assert(t.set(s));
  return t;
}
static int result(Session &s, unsigned id, Text &out) {
  double deadline = now() + 8;
  while (now() < deadline) {
    s.pump();
    for (size_t n = 0; n < s.events.size(); ++n) {
      const auto &p = s.events[n];
      if (p.id != id)
        continue;
      if (p.kind == Kind::Output)
        assert(append(out, p.body.c_str(), p.body.size()));
      else if (p.kind == Kind::Done) {
        int code = atoi(p.body.c_str());
        s.events.clear();
        return code;
      }
    }
    s.events.clear();
    usleep(10000);
  }
  assert(!"worker response timed out");
  return -1;
}
int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--worker"))
    return worker_main();
  Session s;
  assert(s.launch(Auth::Direct));
  double deadline = now() + 5;
  while (!s.ready() && s.alive() && now() < deadline) {
    s.pump();
    usleep(10000);
  }
  assert(s.ready());
  Request r;
  Text out;
  r.op = Op::Snapshot;
  assert(s.send(r, 1));
  assert(!result(s, 1, out));
  Array<Interface> interfaces;
  Text gw, dns;
  // SIOCGIFCONF omits interfaces without IPv4 addresses. They must still be
  // visible, otherwise disabling an adapter makes it impossible to select.
  assert(parse_snapshot(out, interfaces, gw, dns));
  assert(interfaces.size() == 1 && interfaces[0].name == "lo" &&
         interfaces[0].address == "0.0.0.0" && !(interfaces[0].flags & 1));
  r.op = Op::ReadConfig;
  out.clear();
  assert(s.send(r, 2));
  assert(!result(s, 2, out));
  bool exists;
  Text original;
  Config initial;
  assert(decode_profile(out, exists, original, initial) && !exists);
  Config c;
  c.dns = Dns::Cloudflare;
  Text bytes, error;
  assert(serialize_config(c, bytes, error));
  r.op = Op::WriteConfig;
  r.a = bytes;
  r.c = text("0");
  out.clear();
  assert(s.send(r, 3));
  assert(!result(s, 3, out));
  out.clear();
  assert(s.send(r, 4));
  assert(result(s, 4, out) ==
         1); // Stale snapshot cannot overwrite saved settings.
  r = Request{};
  r.op = Op::Ping;
  r.a = text("example.com");
  assert(s.send(r, 5));
  bool active = false;
  deadline = now() + 4;
  while (!active && now() < deadline) {
    s.pump();
    for (size_t n = 0; n < s.events.size(); ++n)
      if (s.events[n].kind == Kind::Output &&
          strstr(s.events[n].body.c_str(), "active"))
        active = true;
    s.events.clear();
    usleep(10000);
  }
  assert(active);
  r = Request{};
  r.op = Op::Cancel;
  out.clear();
  assert(s.send(r, 6));
  assert(result(s, 5, out) == 130);
  r = Request{};
  r.op = Op::Apply;
  out.clear();
  assert(s.send(r, 7));
  assert(!result(s, 7, out));
  r = Request{};
  r.op = Op::Ping;
  r.a = text("-f");
  assert(!s.send(r, 8));
  s.stop();
  deadline = now() + 5;
  while (s.alive() && now() < deadline) {
    s.pump();
    usleep(10000);
  }
  assert(!s.alive());
  puts("PASS Network Controller worker: real IPC, interface query, conflict "
       "refusal, cancellation and EOF cleanup");
}
