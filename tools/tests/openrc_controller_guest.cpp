/* Runs inside a private ReliefOS guest against the shipped root worker. */
#include "model.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
using namespace oc;
static int to_worker = -1, from_worker = -1;
static unsigned serial = 0, checks = 0;
static Text incoming;
static double now() {
  timespec t{};
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}
static bool packet(Packet &p) {
  double deadline = now() + 90;
  while (now() < deadline) {
    int n = take_packet(incoming, p);
    if (n)
      return n == 1;
    pollfd f{from_worker, POLLIN, 0};
    if (poll(&f, 1, 100) < 0 && errno != EINTR)
      return false;
    if (f.revents) {
      char b[4096];
      ssize_t got = read(from_worker, b, sizeof(b));
      if (got <= 0)
        return false;
      if (!append(incoming, b, got))
        return false;
    }
  }
  return false;
}
static bool send_request(const Request &r, unsigned id) {
  Packet p;
  p.id = id;
  p.kind = Kind::Request;
  Text frame;
  if (!encode_request(r, p.body) || !encode_packet(p, frame))
    return false;
  size_t at = 0;
  while (at < frame.size()) {
    ssize_t n = write(to_worker, frame.c_str() + at, frame.size() - at);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return false;
    at += n;
  }
  return true;
}
static int perform(const Request &r, Text &out) {
  out.clear();
  unsigned id = ++serial;
  if (!send_request(r, id))
    return -1;
  Packet p;
  while (packet(p)) {
    if (p.id != id)
      continue;
    if (p.kind == Kind::Output) {
      if (!append(out, p.body.c_str(), p.body.size()))
        return -1;
    } else if (p.kind == Kind::Done)
      return atoi(p.body.c_str());
  }
  return -1;
}
static Request req(Op op, const char *a = "", const char *b = "",
                   const char *c = "") {
  Request r;
  r.op = op;
  r.a.set(a);
  r.b.set(b);
  r.c.set(c);
  return r;
}
static bool contents(const char *path, const char *expected) {
  FILE *f = fopen(path, "r");
  if (!f)
    return false;
  char b[512];
  size_t n = fread(b, 1, sizeof(b) - 1, f);
  fclose(f);
  b[n] = 0;
  return !strcmp(b, expected);
}
#define CHECK(x)                                                               \
  do {                                                                         \
    ++checks;                                                                  \
    if (!(x)) {                                                                \
      fprintf(stderr, "OPENRC-CONTROLLER FAIL line=%d check=%s\n", __LINE__,   \
              #x);                                                             \
      if (!out.empty())                                                        \
        fprintf(stderr, "%s\n", out.c_str());                                  \
      goto failure;                                                            \
    }                                                                          \
  } while (0)
int main() {
  int in[2], output[2];
  pid_t child = -1;
  Text out;
  Packet ready;
  Request r;
  Catalog catalog;
  if (pipe(in) || pipe(output))
    return 1;
  child = fork();
  if (!child) {
    dup2(in[0], 0);
    dup2(output[1], 1);
    close(in[0]);
    close(in[1]);
    close(output[0]);
    close(output[1]);
    execl(WorkerPath, WorkerPath, "--worker", (char *)nullptr);
    _exit(127);
  }
  if (child < 0)
    return 1;
  close(in[0]);
  close(output[1]);
  to_worker = in[1];
  from_worker = output[0];
  CHECK(packet(ready) && ready.kind == Kind::Ready);
  CHECK(perform(req(Op::Catalog), out) == 0);
  CHECK(strstr(out.c_str(), "controller-qa"));
  CHECK(parse_catalog(out.c_str(), catalog));
  CHECK(perform(req(Op::State), out) == 0);
  CHECK(parse_status(out.c_str(), catalog));
  CHECK(perform(req(Op::Service, "controller-qa", "describe"), out) == 0);
  CHECK(strstr(out.c_str(), "Controller acceptance"));
  CHECK(perform(req(Op::CreateLevel, "controller-work"), out) == 0);
  CHECK(perform(req(Op::Add, "controller-qa", "controller-work"), out) == 0);
  CHECK(access("/etc/runlevels/controller-work/controller-qa", F_OK) == 0);
  CHECK(perform(req(Op::Service, "controller-qa", "start"), out) == 0);
  CHECK(contents("/run/controller-qa", "started\n"));
  CHECK(perform(req(Op::Service, "controller-qa", "status"), out) == 0);
  CHECK(perform(req(Op::Service, "controller-qa", "reload"), out) == 0);
  CHECK(contents("/run/controller-qa", "reloaded\n"));
  CHECK(perform(req(Op::Service, "controller-qa", "health"), out) == 0);
  CHECK(strstr(out.c_str(), "controller-healthy"));
  CHECK(perform(req(Op::Service, "controller-qa", "restart", "dry-run"), out) ==
        0);
  CHECK(contents("/run/controller-qa", "reloaded\n"));
  CHECK(perform(req(Op::Service, "controller-qa", "restart"), out) == 0);
  CHECK(contents("/run/controller-qa", "started\n"));
  CHECK(perform(req(Op::Depend, "controller-qa", "ineed"), out) == 0);
  CHECK(perform(req(Op::ReadScript, "controller-qa"), out) == 0);
  CHECK(strstr(out.c_str(), "extra_commands"));
  CHECK(perform(req(Op::Rebuild), out) == 0);
  CHECK(perform(req(Op::Service, "controller-qa", "stop"), out) == 0);
  CHECK(access("/run/controller-qa", F_OK) < 0);
  CHECK(perform(req(Op::Remove, "controller-qa", "controller-work"), out) == 0);
  CHECK(access("/etc/runlevels/controller-work/controller-qa", F_OK) < 0);
  CHECK(perform(req(Op::StackAdd, "controller-work", "default"), out) == 0);
  CHECK(perform(req(Op::DeleteLevel, "controller-work"), out) !=
        0); // referenced level cannot be removed
  CHECK(perform(req(Op::StackDel, "controller-work", "default"), out) == 0);
  CHECK(perform(req(Op::DeleteLevel, "controller-work"), out) == 0);
  CHECK(perform(req(Op::ReadConfig, "controller-qa"), out) == 0);
  CHECK(out == "1# retained comment\nvalue=old\n");
  r = req(Op::WriteConfig, "controller-qa");
  r.b.set(out.c_str() + 1);
  r.c.set("# retained comment\nvalue=new\n");
  r.d.set("present");
  CHECK(perform(r, out) == 0);
  CHECK(
      contents("/etc/conf.d/controller-qa", "# retained comment\nvalue=new\n"));
  CHECK(contents("/etc/conf.d/controller-qa.openrc-controller.bak",
                 "# retained comment\nvalue=old\n"));
  CHECK(perform(r, out) != 0); // stale editor conflicts
  r = req(Op::RestoreConfig, "controller-qa");
  r.b.set("# retained comment\nvalue=new\n");
  r.d.set("present");
  CHECK(perform(r, out) == 0);
  CHECK(
      contents("/etc/conf.d/controller-qa", "# retained comment\nvalue=old\n"));
  {
    FILE *f = fopen("/etc/conf.d/controller-private", "w");
    CHECK(f);
    CHECK(fputs("private fixture\n", f) >= 0 && fclose(f) == 0);
    CHECK(chmod("/etc/conf.d/controller-private", 0600) == 0);
    r = req(Op::WriteConfig, "controller-private");
    r.b.set("private fixture\n");
    r.c.set("private edited\n");
    r.d.set("present");
    CHECK(perform(r, out) == 0);
    CHECK(unlink("/etc/conf.d/controller-private") == 0);
    r = req(Op::RestoreConfig, "controller-private");
    r.d.set("missing");
    CHECK(perform(r, out) == 0);
    struct stat permissions;
    CHECK(stat("/etc/conf.d/controller-private", &permissions) == 0);
    CHECK((permissions.st_mode & 0777) == 0600);
    CHECK(contents("/etc/conf.d/controller-private", "private fixture\n"));
  }
  CHECK(perform(req(Op::Logs, "rc.log"), out) == 0);
  CHECK(perform(req(Op::Report), out) == 0);
  CHECK(strstr(out.c_str(), "controller-qa"));
  CHECK(perform(req(Op::Service, "controller-reject", "start"), out) != 0);
  CHECK(perform(req(Op::Service, "controller-dependent", "start"), out) != 0);
  CHECK(access("/run/controller-should-not-exist", F_OK) < 0);
  r = req(Op::Service, "controller-slow", "start");
  CHECK(send_request(r, ++serial));
  out.clear();
  while (!strstr(out.c_str(), "CONTROLLER-SLOW")) {
    Packet p;
    CHECK(packet(p));
    if (p.kind == Kind::Output)
      append(out, p.body.c_str(), p.body.size());
    else if (p.kind == Kind::Done)
      goto failure;
  }
  CHECK(send_request(req(Op::Cancel), 0));
  {
    Packet p;
    bool canceled = false;
    while (packet(p)) {
      if (p.id == serial && p.kind == Kind::Done) {
        canceled = atoi(p.body.c_str()) == 130;
        break;
      }
    }
    CHECK(canceled);
  }
  CHECK(access("/run/controller-should-not-exist", F_OK) < 0);
  CHECK(perform(req(Op::Service, "controller-slow", "zap"), out) == 0);
  CHECK(send_request(req(Op::Quit), ++serial));
  close(to_worker);
  to_worker = -1;
  {
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    child = -1;
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }
  close(from_worker);
  printf("OPENRC-CONTROLLER PASS checks=%u\n", checks);
  return 0;
failure:
  if (to_worker >= 0) {
    send_request(req(Op::Quit), ++serial);
    close(to_worker);
  }
  if (from_worker >= 0)
    close(from_worker);
  if (child > 0)
    waitpid(child, nullptr, 0);
  return 1;
}
