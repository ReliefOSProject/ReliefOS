/* Runs inside a private ReliefOS guest against the shipped root worker. */
#include "model.h"
extern "C" {
#include <reliefos/net.h>
}
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
using namespace nc;
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

static pid_t worker = -1;
static Text out;
static void check(bool ok, const char *expression, int line) {
  ++checks;
  if (ok)
    return;
  fprintf(stderr, "NETWORK-CONTROLLER FAIL line=%d check=%s\n%s\n", line,
          expression, out.c_str());
  if (to_worker >= 0)
    close(to_worker);
  if (from_worker >= 0)
    close(from_worker);
  if (worker > 0) {
    kill(worker, SIGTERM);
    waitpid(worker, nullptr, 0);
  }
  exit(1);
}
#define CHECK(x) check((x), #x, __LINE__)
static Request req(Op op, const char *a = "", const char *b = "") {
  Request r;
  r.op = op;
  r.a.set(a);
  r.b.set(b);
  return r;
}
static Config current(bool &exists, Text &raw) {
  CHECK(perform(req(Op::ReadConfig), out) == 0);
  Config c;
  CHECK(decode_profile(out, exists, raw, c));
  return c;
}
static void configure(const Config &c) {
  bool exists;
  Text raw, encoded, error;
  current(exists, raw);
  CHECK(serialize_config(c, encoded, error));
  Request r;
  r.op = Op::WriteConfig;
  r.a = encoded;
  r.b = raw;
  r.c.set(exists ? "1" : "0");
  CHECK(perform(r, out) == 0);
  CHECK(perform(req(Op::Apply), out) == 0);
  fprintf(stdout, "NC_APPLIED mode=%d address=%08x\n", static_cast<int>(c.mode),
          c.address);
  fflush(stdout);
}
static bool interface_state(const char *expected, bool up) {
  if (perform(req(Op::Snapshot), out))
    return false;
  Array<Interface> list;
  Text gw, dns;
  if (!parse_snapshot(out, list, gw, dns))
    return false;
  for (size_t n = 0; n < list.size(); ++n)
    if (list[n].name == "eth0")
      return list[n].address == expected &&
             static_cast<bool>(list[n].flags & 1) == up;
  return false;
}
static bool await_interface(const char *expected, bool up) {
  double deadline = now() + 45;
  while (now() < deadline) {
    if (interface_state(expected, up))
      return true;
    usleep(100000);
  }
  return false;
}
static bool await_lease() {
  double deadline = now() + 45;
  while (now() < deadline) {
    if (access("/run/reliefos/dhcp-lease", F_OK) == 0)
      return true;
    usleep(100000);
  }
  return false;
}
static unsigned dhcp_count() {
  DIR *d = opendir("/proc");
  if (!d)
    return 999;
  unsigned count = 0;
  while (auto *entry = readdir(d)) {
    if (!*entry->d_name ||
        strspn(entry->d_name, "0123456789") != strlen(entry->d_name))
      continue;
    char path[320];
    snprintf(path, sizeof(path), "/proc/%s/cmdline", entry->d_name);
    FILE *f = fopen(path, "r");
    if (!f)
      continue;
    char command[4096] = {};
    size_t length = fread(command, 1, sizeof(command) - 1, f);
    fclose(f);
    if (!length)
      continue;
    const char *first = strrchr(command, '/');
    first = first ? first + 1 : command;
    bool match = !strcmp(first, "udhcpc");
    if (!strcmp(first, "busybox")) {
      size_t next = strlen(command) + 1;
      if (next < length)
        match = !strcmp(command + next, "udhcpc");
    }
    if (match) {
      ++count;
      printf("NC_DHCP_PID %s\n", entry->d_name);
    }
  }
  closedir(d);
  return count;
}
static void restore() {
  bool exists;
  Text raw;
  current(exists, raw);
  Request r;
  r.op = Op::RestoreConfig;
  r.b = raw;
  r.c.set(exists ? "1" : "0");
  CHECK(perform(r, out) == 0);
  CHECK(perform(req(Op::Apply), out) == 0);
}
int main(int argc, char **argv) {
  CHECK(argc == 2);
  int input[2], output[2];
  CHECK(!pipe(input) && !pipe(output));
  worker = fork();
  CHECK(worker >= 0);
  if (!worker) {
    close(input[1]);
    close(output[0]);
    dup2(input[0], 0);
    dup2(output[1], 1);
    close(input[0]);
    close(output[1]);
    execl(WorkerPath, WorkerPath, "--worker", static_cast<char *>(nullptr));
    _exit(127);
  }
  close(input[0]);
  close(output[1]);
  to_worker = input[1];
  from_worker = output[0];
  Packet ready;
  CHECK(packet(ready) && ready.kind == Kind::Ready);
  bool exists;
  Text raw;
  Config c = current(exists, raw);
  const char *phase = "/var/lib/reliefos/network-controller-qa";
  if (access(phase, F_OK) == 0) {
    CHECK(c.mode == Mode::Static && c.address == 0x0a000228);
    CHECK(await_interface("10.0.2.40", true));
    CHECK(dhcp_count() == 0);
    CHECK(perform(req(Op::View, "resolver"), out) == 0 &&
          strstr(out.c_str(), "nameserver 10.0.2.2"));
    puts("NC_BOOT_PERSISTENCE_PASS");
  } else {
    CHECK(c.mode == Mode::Dhcp);
    CHECK(await_interface("10.0.2.15", true));
    CHECK(await_lease());
    CHECK(dhcp_count() == 1);
    struct reliefos_net_dns_policy legacy_policy{};
    CHECK(reliefos_net_set_dns_policy(RELIEFOS_NET_DNS_MODE_DHCP, 0,
                                      &legacy_policy) == 0);
    CHECK(perform(req(Op::DnsQuery, "fixture.test", "10.0.2.2"), out) == 0 &&
          strstr(out.c_str(), "10.0.2.2"));
    CHECK(perform(req(Op::Tcp, "10.0.2.2", argv[1]), out) == 0 &&
          strstr(out.c_str(), "succeeded"));
    CHECK(perform(req(Op::Ping, "10.0.2.2"), out) == 0);
    c.mode = Mode::Static;
    c.address = 0x0a000228;
    c.prefix = 24;
    c.gateway = 0x0a000202;
    c.dns = Dns::Custom;
    c.server_count = 1;
    c.servers[0] = 0x0a000202;
    configure(c);
    struct reliefos_net_dns_policy old_policy{};
    errno = 0;
    CHECK(reliefos_net_set_dns_policy(RELIEFOS_NET_DNS_MODE_CLOUDFLARE, 0,
                                      &old_policy) == -1 &&
          errno == EBUSY);
    CHECK(await_interface("10.0.2.40", true));
    CHECK(dhcp_count() == 0);
    CHECK(access("/run/reliefos/dhcp-lease", F_OK) != 0);
    CHECK(perform(req(Op::View, "resolver"), out) == 0 &&
          strstr(out.c_str(), "nameserver 10.0.2.2"));
    CHECK(perform(req(Op::DnsQuery, "fixture.test"), out) == 0);
    restore();
    CHECK(await_interface("10.0.2.15", true));
    CHECK(await_lease());
    CHECK(dhcp_count() == 1);
    Config disabled;
    disabled.mode = Mode::Disabled;
    configure(disabled);
    CHECK(await_interface("0.0.0.0", false));
    CHECK(dhcp_count() == 0);
    configure(Config{});
    CHECK(await_interface("10.0.2.15", true));
    CHECK(await_lease());
    CHECK(dhcp_count() == 1);
    configure(c);
    CHECK(await_interface("10.0.2.40", true));
    CHECK(dhcp_count() == 0);
    FILE *f = fopen(phase, "w");
    CHECK(f && fputs("static-profile-ready\n", f) >= 0);
    CHECK(!fclose(f));
    CHECK(!syncfs(open("/", O_RDONLY | O_DIRECTORY)));
  }
  CHECK(perform(req(Op::View, "report"), out) == 0 &&
        strstr(out.c_str(), "10.0.2.40"));
  CHECK(send_request(req(Op::Quit), ++serial));
  close(to_worker);
  to_worker = -1;
  close(from_worker);
  from_worker = -1;
  int status = 0;
  CHECK(waitpid(worker, &status, 0) == worker && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0);
  worker = -1;
  printf("NETWORK-CONTROLLER PASS checks=%u\n", checks);
  fflush(stdout);
  return 0;
}
