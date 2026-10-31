#include "session.h"
#include "worker.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
static unsigned tests;
#define CHECK(x)                                                               \
  do {                                                                         \
    ++tests;                                                                   \
    if (!(x)) {                                                                \
      fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                          \
      return 1;                                                                \
    }                                                                          \
  } while (0)
static double now() {
  struct timespec t{};
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}
static void step(oc::Session &s) {
  s.pump();
  poll(nullptr, 0, 5);
}
static bool wait(oc::Session &s, unsigned id, int expected, oc::Text &output) {
  double until = now() + 8;
  while (now() < until && s.alive()) {
    step(s);
    for (size_t i = 0; i < s.events.size(); ++i)
      if (s.events[i].id == id) {
        auto &p = s.events[i];
        if (p.kind == oc::Kind::Output)
          oc::append(output, p.body.c_str(), p.body.size());
        if (p.kind == oc::Kind::Done) {
          bool ok = atoi(p.body.c_str()) == expected;
          s.events.clear();
          return ok;
        }
      }
    s.events.clear();
  }
  return false;
}
int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--worker"))
    return oc::worker_main();
  using namespace oc;
  CHECK(geteuid() == 0);
  Session s;
  CHECK(s.launch(Auth::Direct));
  double until = now() + 5;
  while (!s.ready() && s.alive() && now() < until)
    step(s);
  CHECK(s.ready());
  CHECK(s.problem.empty());
  Request r;
  r.op = Op::Catalog;
  Text out;
  CHECK(s.send(r, 1));
  CHECK(wait(s, 1, 0, out));
  Catalog catalog;
  CHECK(parse_catalog(out.c_str(), catalog));
  CHECK(catalog.services.size() == 1);
  r.op = Op::Service;
  r.a.set("probe");
  r.b.set("fail");
  out.clear();
  CHECK(s.send(r, 2));
  CHECK(wait(s, 2, 7, out));
  CHECK(strstr(out.c_str(), "intentional failure"));
  CHECK(s.alive());
  r.b.set("wait");
  CHECK(s.send(r, 3));
  out.clear();
  int child = 0;
  until = now() + 5;
  while (!child && now() < until) {
    step(s);
    for (size_t i = 0; i < s.events.size(); ++i)
      if (s.events[i].kind == Kind::Output)
        append(out, s.events[i].body.c_str());
    s.events.clear();
    sscanf(out.c_str(), "CHILD %d", &child);
  }
  CHECK(child > 0);
  Request cancel;
  cancel.op = Op::Cancel;
  CHECK(s.send(cancel, 0));
  CHECK(wait(s, 3, 130, out));
  CHECK(kill(child, 0) < 0 && errno == ESRCH);
  CHECK(s.alive());
  r = Request{};
  r.op = Op::WriteConfig;
  r.a.set("probe");
  r.d.set("missing");
  char bytes[80001];
  memset(bytes, 'x', 80000);
  bytes[80000] = 0;
  CHECK(r.c.set(bytes));
  CHECK(s.send(r, 4));
  out.clear();
  CHECK(wait(s, 4, 0, out));
  r = Request{};
  r.op = Op::ReadConfig;
  r.a.set("probe");
  CHECK(s.send(r, 5));
  out.clear();
  CHECK(wait(s, 5, 0, out));
  CHECK(out.size() == 80001 && out.c_str()[0] == '1' &&
        !strcmp(out.c_str() + 1, bytes));
  r = Request{};
  r.op = Op::Service;
  r.a.set("probe");
  r.b.set("wait");
  CHECK(s.send(r, 6));
  out.clear();
  child = 0;
  until = now() + 5;
  while (!child && now() < until) {
    step(s);
    for (size_t i = 0; i < s.events.size(); ++i)
      if (s.events[i].kind == Kind::Output)
        append(out, s.events[i].body.c_str());
    s.events.clear();
    sscanf(out.c_str(), "CHILD %d", &child);
  }
  CHECK(child > 0);
  s.stop();
  until = now() + 5;
  while (s.alive() && now() < until)
    step(s);
  CHECK(!s.alive());
  CHECK(kill(child, 0) < 0 && errno == ESRCH);
  {
    Session auth;
    CHECK(auth.launch(Auth::Root));
    CHECK(auth.password("fixture"));
    until = now() + 5;
    while (!auth.ready() && auth.alive() && now() < until)
      step(auth);
    CHECK(auth.ready());
    r = Request{};
    r.op = Op::Catalog;
    CHECK(auth.send(r, 7));
    out.clear();
    CHECK(wait(auth, 7, 0, out));
  }
  {
    CHECK(setenv("OC_TEST_PASSWORDLESS", "1", 1) == 0);
    Session auth;
    CHECK(auth.launch(Auth::Root));
    CHECK(unsetenv("OC_TEST_PASSWORDLESS") == 0);
    CHECK(auth.password("must-not-enter-protocol"));
    until = now() + 5;
    while (!auth.ready() && auth.alive() && now() < until)
      step(auth);
    CHECK(auth.ready());
    r = Request{};
    r.op = Op::Catalog;
    CHECK(auth.send(r, 8));
    out.clear();
    CHECK(wait(auth, 8, 0, out));
  }
  puts("OpenRC Controller session/cancellation/authentication tests passed");
  printf("%u checks\n", tests);
  return 0;
}
