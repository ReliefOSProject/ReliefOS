#include "worker.h"
#include "config.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netdb.h>
#include <poll.h>
#include <reliefos/net_control.h>
#include <signal.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
namespace nc {
namespace {
volatile sig_atomic_t stopping;
void stop_signal(int) { stopping = 1; }
double seconds() {
  struct timespec t{};
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}
bool nonblock(int fd) {
  int f = fcntl(fd, F_GETFL);
  return f >= 0 && fcntl(fd, F_SETFL, f | O_NONBLOCK) >= 0;
}
bool reply(Kind kind, unsigned id, const char *bytes, size_t size) {
  Packet p;
  p.kind = kind;
  p.id = id;
  Text frame;
  if (!p.body.set(bytes, size) || !encode_packet(p, frame))
    return false;
  size_t at = 0;
  double deadline = seconds() + 10;
  while (at < frame.size() && !stopping) {
    ssize_t n = write(STDOUT_FILENO, frame.c_str() + at, frame.size() - at);
    if (n > 0) {
      at += n;
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      struct pollfd f{STDOUT_FILENO, POLLOUT, 0};
      poll(&f, 1, 100);
      if (seconds() < deadline)
        continue;
    }
    return false;
  }
  return at == frame.size();
}
bool output(unsigned id, const char *s) {
  return reply(Kind::Output, id, s, strlen(s));
}
bool done(unsigned id, int code) {
  char s[32];
  snprintf(s, sizeof(s), "%d", code);
  return reply(Kind::Done, id, s, strlen(s));
}
bool get_input(Text &input, bool &eof) {
  char b[8192];
  ssize_t n = read(STDIN_FILENO, b, sizeof(b));
  if (n > 0)
    return input.size() + n <= MaxPayload + 16 && append(input, b, n);
  if (!n || (n < 0 && errno == EIO)) {
    eof = true;
    return true;
  }
  return n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK);
}
bool management(unsigned op, struct reliefos_net_control &c) {
  int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return false;
  c.version = RELIEFOS_NET_CONTROL_VERSION;
  c.operation = op;
  int ret = ioctl(fd, RELIEFOS_NET_CONTROL_IOCTL, &c), saved = errno;
  close(fd);
  errno = saved;
  if (ret < 0)
    return false;
  if (c.result < 0) {
    errno = -c.result;
    return false;
  }
  return true;
}
int native_ping(const Request &r) {
  struct reliefos_net_control capability{};
  if (!management(RELIEFOS_NET_CONTROL_CONFIG, capability)) {
    if (errno == ENOTTY || errno == EOPNOTSUPP || errno == ENOSYS)
      return -2;
    dprintf(1, "Network query failed: %s\n", strerror(errno));
    return 1;
  }
  struct addrinfo hint{}, *list = nullptr;
  hint.ai_family = AF_INET;
  hint.ai_socktype = SOCK_DGRAM;
  int error = getaddrinfo(r.a.c_str(), nullptr, &hint, &list);
  if (error) {
    dprintf(1, "DNS lookup failed: %s\n", gai_strerror(error));
    return 1;
  }
  uint32_t address = ntohl(
      reinterpret_cast<struct sockaddr_in *>(list->ai_addr)->sin_addr.s_addr);
  freeaddrinfo(list);
  unsigned received = 0;
  char ip[16];
  format_ip(address, ip);
  for (unsigned n = 0; n < 4; ++n) {
    struct reliefos_net_control c{};
    c.data.ping.target_ip = address;
    c.data.ping.timeout_ms = 2000;
    if (!management(RELIEFOS_NET_CONTROL_PING, c)) {
      if (!n && (errno == ENOTTY || errno == EOPNOTSUPP || errno == ENOSYS))
        return -2;
      dprintf(1, "Ping failed: %s\n", strerror(errno));
      return 1;
    }
    if (c.data.ping.status == RELIEFOS_NET_STATUS_OK && c.data.ping.received) {
      ++received;
      dprintf(1, "Reply from %s: sequence=%u RTT=%u ms\n", ip,
              c.data.ping.sequence, c.data.ping.rtt_ms);
    } else
      dprintf(1, "Ping to %s failed: network status=%u\n", ip,
              c.data.ping.status);
    if (n < 3)
      usleep(250000);
  }
  dprintf(1, "Ping statistics: sent=4 received=%u lost=%u\n", received,
          4 - received);
  return received ? 0 : 1;
}
int tcp_probe(const Request &);
int run(const Request &r, unsigned id, Text &input, bool &quit) {
  Array<Text> args;
  Text error;
  if (r.op != Op::Tcp && !command(r, args, error)) {
    output(id, error.c_str());
    return 126;
  }
  int pipefd[2];
  if (pipe2(pipefd, O_CLOEXEC) < 0) {
    output(id, "Cannot create command output pipe.\n");
    return 125;
  }
  char *argv[16];
  if (args.size() >= 16) {
    close(pipefd[0]);
    close(pipefd[1]);
    return 126;
  }
  for (size_t n = 0; n < args.size(); ++n)
    argv[n] = const_cast<char *>(args[n].c_str());
  argv[args.size()] = nullptr;
  pid_t child = fork();
  if (!child) {
    setpgid(0, 0);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    close(pipefd[0]);
    dup2(pipefd[1], 1);
    dup2(pipefd[1], 2);
    close(pipefd[1]);
    int nullfd = open("/dev/null", O_RDONLY);
    if (nullfd < 0 || dup2(nullfd, 0) < 0)
      _exit(125);
    if (nullfd > 2)
      close(nullfd);
    if (r.op == Op::Ping) {
      int result = native_ping(r);
      if (result != -2)
        _exit(result);
    }
    if (r.op == Op::Tcp)
      _exit(tcp_probe(r));
    execv(argv[0], argv);
    dprintf(2, "Cannot execute %s: %s\n", argv[0], strerror(errno));
    _exit(127);
  }
  close(pipefd[1]);
  if (child < 0) {
    close(pipefd[0]);
    return 125;
  }
  setpgid(child, child);
  nonblock(pipefd[0]);
  bool ended = false, eof = false, canceled = false, timed_out = false;
  int status = 0;
  double start = seconds(), killed = 0, exited = 0;
  while (!ended || !eof) {
    if (stopping ||
        seconds() - start >
            ((r.op == Op::Apply || r.op == Op::Renew) ? 60 : 20) ||
        quit) {
      timed_out = !stopping && !quit;
      canceled = true;
    }
    if (canceled && !killed) {
      kill(-child, SIGTERM);
      killed = seconds();
    }
    if (killed && seconds() - killed > 2)
      kill(-child, SIGKILL);
    struct pollfd f[2] = {{STDIN_FILENO, POLLIN, 0}, {pipefd[0], POLLIN, 0}};
    poll(f, 2, 50);
    if (f[0].revents & (POLLIN | POLLHUP | POLLERR)) {
      bool closed = false;
      if (!get_input(input, closed) || closed) {
        quit = true;
        canceled = true;
      }
      Packet p;
      int parsed;
      while ((parsed = take_packet(input, p)) == 1) {
        Request request;
        if (p.kind != Kind::Request || !decode_request(p.body, request)) {
          quit = true;
          canceled = true;
          break;
        }
        if (request.op == Op::Cancel)
          canceled = true;
        else if (request.op == Op::Quit) {
          quit = true;
          canceled = true;
        } else {
          output(p.id, "Another operation is running.\n");
          done(p.id, 125);
        }
      }
      if (parsed < 0) {
        quit = true;
        canceled = true;
      }
    }
    // A per-iteration budget ensures noisy services cannot starve cancellation.
    for (size_t budget = 0; budget < 65536 && !eof;) {
      char b[4096];
      ssize_t n = read(pipefd[0], b, sizeof(b));
      if (n > 0) {
        budget += n;
        if (!quit && !reply(Kind::Output, id, b, n)) {
          quit = true;
          canceled = true;
        }
      } else if (!n)
        eof = true;
      else if (errno == EINTR)
        continue;
      else {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
          eof = true;
        break;
      }
    }
    if (!ended) {
      pid_t got = waitpid(child, &status, WNOHANG);
      if (got == child || (got < 0 && errno == ECHILD)) {
        ended = true;
        exited = seconds();
      }
    }
    // A daemon inheriting output must not keep the GUI operation open
    // indefinitely.
    if (ended && !eof && seconds() - exited > 1) {
      eof = true;
    }
  }
  close(pipefd[0]);
  if (timed_out)
    output(id, "\nOperation timed out. Check service state "
               "before retrying.\n");
  else if (canceled && !quit)
    output(id, "\nOperation canceled. Check service state before retrying.\n");
  if (canceled)
    return timed_out ? 124 : 130;
  return WIFEXITED(status) ? WEXITSTATUS(status)
                           : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}
bool read_text(const char *path, Text &text, size_t limit, bool tail = false) {
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0)
    return false;
  struct stat st;
  bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
  if (ok && tail && st.st_size > static_cast<off_t>(limit))
    ok = lseek(fd, st.st_size - limit, SEEK_SET) >= 0;
  text.clear();
  char b[4096];
  while (ok) {
    ssize_t n = read(fd, b, sizeof(b));
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0) {
      ok = false;
      break;
    }
    if (!n)
      break;
    if (text.size() + n > limit || memchr(b, 0, n) || !append(text, b, n)) {
      ok = false;
      break;
    }
  }
  close(fd);
  return ok;
}

int tcp_probe(const Request &r) {
  struct addrinfo hint{}, *list = nullptr;
  hint.ai_family = AF_INET;
  hint.ai_socktype = SOCK_STREAM;
  double begin = seconds();
  int result = getaddrinfo(r.a.c_str(), r.b.c_str(), &hint, &list);
  if (result) {
    dprintf(1, "DNS lookup failed: %s\n", gai_strerror(result));
    return 1;
  }
  int code = 1;
  unsigned tried = 0;
  for (auto ai = list; ai && tried++ < 8; ai = ai->ai_next) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
      continue;
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET,
              &reinterpret_cast<struct sockaddr_in *>(ai->ai_addr)->sin_addr,
              ip, sizeof(ip));
    dprintf(1, "Connecting to %s (%s):%s ...\n", r.a.c_str(), ip, r.b.c_str());
    int ret = connect(fd, ai->ai_addr, ai->ai_addrlen),
        error = ret < 0 ? errno : 0;
    if (ret < 0 && error == EINPROGRESS) {
      struct pollfd p{fd, POLLOUT, 0};
      ret = poll(&p, 1, 5000);
      if (ret > 0) {
        socklen_t length = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0)
          error = errno;
      } else
        error = ret == 0 ? ETIMEDOUT : errno;
    }
    if (!error) {
      dprintf(1, "TCP connection succeeded in %.1f ms.\n",
              (seconds() - begin) * 1000);
      code = 0;
      close(fd);
      break;
    }
    dprintf(1, "TCP connection failed: %s\n", strerror(error));
    close(fd);
  }
  freeaddrinfo(list);
  return code;
}
bool snapshot(Text &out) {
  int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return false;
  struct ifreq entries[64]{};
  struct ifconf conf{};
  conf.ifc_len = sizeof(entries);
  conf.ifc_req = entries;
  if (ioctl(fd, SIOCGIFCONF, &conf) < 0) {
    close(fd);
    return false;
  }
  Array<Interface> interfaces;
  size_t count = conf.ifc_len / sizeof(struct ifreq);
  if (count > 64)
    count = 64;
  // SIOCGIFCONF lists configured IPv4 addresses, not all adapters. Include
  // real interfaces without an address so Disabled remains reversible.
  // ReliefOS currently exposes eth0 at index 2; probe bounded indices using
  // the standard name ioctl rather than inventing interface names.
  for (int index = 1; index <= 64 && count < 64; ++index) {
    struct ifreq candidate{};
    candidate.ifr_ifindex = index;
    if (ioctl(fd, SIOCGIFNAME, &candidate) < 0)
      continue;
    candidate.ifr_name[IFNAMSIZ - 1] = 0;
    if (!interface_name(candidate.ifr_name))
      continue;
    bool duplicate = false;
    for (size_t n = 0; n < count; ++n)
      if (!strncmp(entries[n].ifr_name, candidate.ifr_name, IFNAMSIZ))
        duplicate = true;
    if (!duplicate)
      entries[count++] = candidate;
  }
  for (size_t n = 0; n < count; ++n) {
    entries[n].ifr_name[IFNAMSIZ - 1] = 0;
    if (!interface_name(entries[n].ifr_name))
      continue;
    bool duplicate = false;
    for (size_t j = 0; j < interfaces.size(); ++j)
      if (interfaces[j].name == entries[n].ifr_name)
        duplicate = true;
    if (duplicate)
      continue;
    Interface i;
    struct ifreq r{};
    strcpy(r.ifr_name, entries[n].ifr_name);
    if (!i.name.set(r.ifr_name)) {
      close(fd);
      return false;
    }
    char b[64];
    if (!ioctl(fd, SIOCGIFFLAGS, &r))
      i.flags = static_cast<unsigned short>(r.ifr_flags);
    if (!ioctl(fd, SIOCGIFMTU, &r))
      i.mtu = r.ifr_mtu;
    if (!ioctl(fd, SIOCGIFADDR, &r)) {
      char ip[16];
      format_ip(ntohl(reinterpret_cast<struct sockaddr_in *>(&r.ifr_addr)
                          ->sin_addr.s_addr),
                ip);
      i.address.set(ip);
    } else
      i.address.set("0.0.0.0");
    if (!ioctl(fd, SIOCGIFNETMASK, &r)) {
      char ip[16];
      format_ip(ntohl(reinterpret_cast<struct sockaddr_in *>(&r.ifr_netmask)
                          ->sin_addr.s_addr),
                ip);
      i.mask.set(ip);
    } else
      i.mask.set("0.0.0.0");
    if (!ioctl(fd, SIOCGIFHWADDR, &r)) {
      auto m = reinterpret_cast<unsigned char *>(r.ifr_hwaddr.sa_data);
      snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2],
               m[3], m[4], m[5]);
      i.mac.set(b);
    } else
      i.mac.set("-");
    if (!interfaces.push(i)) {
      close(fd);
      return false;
    }
  }
  close(fd);
  Text bytes;
  if (read_text("/proc/net/dev", bytes, 65536))
    parse_counters(bytes.c_str(), interfaces);
  out.clear();
  char b[512];
  for (size_t n = 0; n < interfaces.size(); ++n) {
    const auto &i = interfaces[n];
    snprintf(
        b, sizeof(b),
        "I\t%s\t%s\t%s\t%s\t%u\t%u\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%u\n",
        i.name.c_str(), i.address.c_str(), i.mask.c_str(), i.mac.c_str(),
        i.flags, i.mtu, static_cast<unsigned long long>(i.rx),
        static_cast<unsigned long long>(i.tx),
        static_cast<unsigned long long>(i.rx_packets),
        static_cast<unsigned long long>(i.tx_packets),
        static_cast<unsigned long long>(i.rx_errors),
        static_cast<unsigned long long>(i.tx_errors), i.counters ? 1u : 0u);
    if (!append(out, b))
      return false;
  }
  struct reliefos_net_control config{};
  if (management(RELIEFOS_NET_CONTROL_CONFIG, config)) {
    if (config.data.config.gateway_ip) {
      char ip[16];
      format_ip(config.data.config.gateway_ip, ip);
      snprintf(b, sizeof(b), "G\t%s\n", ip);
      if (!append(out, b))
        return false;
    }
  } else if (read_text("/proc/net/route", bytes, 65536)) {
    const char *s = bytes.c_str();
    while (s && *s) {
      char name[16];
      unsigned dst, gw, flags;
      if (sscanf(s, "%15s %x %x %x", name, &dst, &gw, &flags) == 4 && !dst &&
          (flags & 2) && !strcmp(name, "eth0")) {
        char ip[16];
        format_ip(ntohl(gw), ip);
        snprintf(b, sizeof(b), "G\t%s\n", ip);
        if (!append(out, b))
          return false;
        break;
      }
      s = strchr(s, '\n');
      if (s)
        ++s;
    }
  }
  if (read_text("/etc/resolv.conf", bytes, 16384)) {
    const char *s = bytes.c_str();
    while (s && *s) {
      char word[64], value[64];
      uint32_t ip;
      if (sscanf(s, "%63s %63s", word, value) == 2 &&
          !strcmp(word, "nameserver") && ipv4(value, ip)) {
        snprintf(b, sizeof(b), "D\t%s\n", value);
        if (!append(out, b))
          return false;
      }
      s = strchr(s, '\n');
      if (s)
        ++s;
    }
  }
  return true;
}
void legacy_dns(Config &c) {
  Text bytes;
  if (!read_text("/etc/udhcpc/leonos-dns", bytes, 1024))
    return;
  unsigned mode = 1;
  char a[16] = {}, b[16] = {}, d[16] = {};
  int fields = sscanf(bytes.c_str(), "%u %15s %15s %15s", &mode, a, b, d);
  if (mode == 0)
    c.dns = Dns::Cloudflare;
  else if (mode == 2) {
    Config next = c;
    next.dns = Dns::Custom;
    const char *items[] = {a, b, d};
    for (int n = 1; n < fields; ++n) {
      uint32_t ip;
      if (!ipv4(items[n - 1], ip))
        return;
      next.servers[next.server_count++] = ip;
    }
    Text e;
    if (valid_config(next, e))
      c = next;
  }
}
bool native_information(const Text &what, Text &text) {
  struct reliefos_net_control c{};
  char b[512];
  text.clear();
  if (what == "routes") {
    if (!management(RELIEFOS_NET_CONTROL_CONFIG, c))
      return false;
    text.set(
        "Effective IPv4 routes (ReliefOS network management "
        "interface)\nDestination              Gateway          Interface\n");
    if (c.data.config.local_ip) {
      Subnet s{};
      unsigned prefix = 0;
      uint32_t mask = c.data.config.subnet_mask;
      while (mask & 0x80000000u) {
        ++prefix;
        mask <<= 1;
      }
      subnet(c.data.config.local_ip, prefix, s);
      char net[16], gateway[16];
      format_ip(s.network, net);
      snprintf(b, sizeof(b), "%s/%u        on-link          eth0\n", net,
               prefix);
      append(text, b);
      if (c.data.config.gateway_ip) {
        format_ip(c.data.config.gateway_ip, gateway);
        snprintf(b, sizeof(b), "0.0.0.0/0                %s       eth0\n",
                 gateway);
        append(text, b);
      }
    } else
      append(text, "No configured IPv4 address.\n");
    return true;
  }
  if (what == "connections") {
    if (!management(RELIEFOS_NET_CONTROL_CONNECTIONS, c))
      return false;
    text.set("Active TCP connections (ReliefOS network management "
             "interface)\nPID      State          Local                   "
             "Remote                  TX / RX bytes\n");
    if (c.data.connections.count > RELIEFOS_NET_SOCKET_MAX)
      return false;
    for (unsigned n = 0; n < c.data.connections.count; ++n) {
      const auto &e = c.data.connections.entries[n];
      char local[16], remote[16];
      format_ip(e.local_ip, local);
      format_ip(e.remote_ip, remote);
      const char *state = e.state == RELIEFOS_NET_TCP_ESTABLISHED
                              ? "ESTABLISHED"
                          : e.state == RELIEFOS_NET_TCP_SYN_SENT  ? "SYN_SENT"
                          : e.state == RELIEFOS_NET_TCP_TIME_WAIT ? "TIME_WAIT"
                                                                  : "CLOSED";
      snprintf(b, sizeof(b), "%-8u %-14s %s:%u     %s:%u     %u / %u\n",
               e.owner_pid, state, local, e.local_port, remote, e.remote_port,
               e.tx_bytes, e.rx_bytes);
      if (!append(text, b))
        return false;
    }
    if (!c.data.connections.count)
      append(text, "No active TCP connections.\n");
    return true;
  }
  return false;
}
int view(const Request &r, unsigned id) {
  Text bytes;
  const char *path = r.a == "routes"        ? "/proc/net/route"
                     : r.a == "arp"         ? "/proc/net/arp"
                     : r.a == "connections" ? "/proc/net/tcp"
                     : r.a == "lease"       ? "/run/reliefos/dhcp-lease"
                     : r.a == "log"         ? "/var/log/udhcpc.log"
                                            : "/etc/resolv.conf";
  if ((r.a == "routes" || r.a == "connections") &&
      native_information(r.a, bytes)) {
    output(id, bytes.c_str());
    return 0;
  }
  if (r.a == "report") {
    output(id, "Network Controller diagnostics\n\nAdapters (live ioctl and "
               "counters):\n");
    if (snapshot(bytes))
      output(id, bytes.c_str());
    Request detail;
    detail.op = Op::View;
    detail.a.set("routes");
    view(detail, id);
    detail.a.set("connections");
    view(detail, id);
    const char *paths[] = {
        "/proc/net/route",          "/proc/net/arp",
        "/etc/resolv.conf",         "/etc/reliefos/network.conf",
        "/run/reliefos/dhcp-lease", "/var/log/udhcpc.log"};
    for (auto p : paths) {
      output(id, "\n");
      output(id, p);
      output(id, "\n");
      if (read_text(p, bytes, 65536, true))
        output(id, bytes.c_str());
      else
        output(id, "Not available.\n");
    }
    return 0;
  }
  if (!read_text(path, bytes, 131072, true)) {
    output(id, "Requested network information is absent or cannot be read.\n");
    return 1;
  }
  output(id, bytes.c_str());
  return 0;
}
int dispatch(const Request &r, unsigned id, Text &input, bool &quit) {
  Text text, error;
  ConfigStore store;
  Snapshot state;
  switch (r.op) {
  case Op::Snapshot:
    if (!snapshot(text)) {
      output(id, "Cannot query network interfaces.\n");
      return 1;
    }
    return reply(Kind::Output, id, text.c_str(), text.size()) ? 0 : 1;
  case Op::ReadConfig:
    if (!store.read(state, error)) {
      output(id, error.c_str());
      return 1;
    }
    {
      Config c;
      if (state.exists) {
        if (!parse_config(state.bytes, c, error))
          c = Config{};
      }
      if (!state.exists || c.legacy)
        legacy_dns(c);
      if (!encode_profile(state.exists, state.bytes, c, text))
        return 1;
      return reply(Kind::Output, id, text.c_str(), text.size()) ? 0 : 1;
    }
  case Op::WriteConfig:
  case Op::RestoreConfig: {
    state.exists = r.c == "1";
    state.bytes = r.b;
    bool ok = r.op == Op::WriteConfig ? store.save(state, r.a, error)
                                      : store.restore(state, error);
    output(id, ok ? "Network configuration saved. Apply it to update the "
                    "running network.\n"
                  : error.c_str());
    return ok ? 0 : 1;
  }
  case Op::Apply:
  case Op::Renew: {
    if (!store.read(state, error)) {
      output(id, error.c_str());
      return 1;
    }
    Config c;
    if (state.exists && !parse_config(state.bytes, c, error)) {
      output(id, error.c_str());
      return 1;
    }
    if (r.op == Op::Renew && c.mode != Mode::Dhcp) {
      output(id, "DHCP renewal requires DHCP mode.\n");
      return 1;
    }
    return run(r, id, input, quit);
  }
  case Op::View:
    return view(r, id);
  case Op::Ping:
  case Op::DnsQuery:
  case Op::Tcp:
    return run(r, id, input, quit);
  case Op::Cancel:
    return 0;
  case Op::Quit:
    quit = true;
    return 0;
  }
  return 126;
}
} // namespace
int worker_main() {
  if (geteuid() != 0) {
    fputs("Root authentication required.\n", stderr);
    return 126;
  }
  // su authenticates on the controlling terminal. After authentication use it
  // solely as a raw request channel; stdout carries framed responses.
  if (isatty(0)) {
    struct termios t;
    if (tcgetattr(0, &t) < 0)
      return 125;
    cfmakeraw(&t);
    if (tcsetattr(0, TCSANOW, &t) < 0)
      return 125;
  }
  clearenv();
  setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
  setenv("LC_ALL", "C", 1);
  setenv("RC_NOCOLOR", "YES", 1);
  setenv("EINFO_COLOR", "NO", 1);
  setenv("HOME", "/root", 1);
  chdir("/");
  umask(022);
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, stop_signal);
  signal(SIGHUP, stop_signal);
  if (!nonblock(0) || !nonblock(1) || !reply(Kind::Ready, 0, "", 0))
    return 125;
  Text input;
  bool quit = false;
  while (!quit && !stopping) {
    Packet p;
    int ready = take_packet(input, p);
    if (ready < 0)
      break;
    if (!ready) {
      struct pollfd f{0, POLLIN, 0};
      if (poll(&f, 1, 100) < 0 && errno != EINTR)
        break;
      if (f.revents) {
        bool eof = false;
        if (!get_input(input, eof) || eof)
          break;
      }
      continue;
    }
    Request r;
    if (p.kind != Kind::Request || !decode_request(p.body, r)) {
      output(p.id, "Invalid request rejected.\n");
      done(p.id, 126);
      continue;
    }
    int code = dispatch(r, p.id, input, quit);
    if (!quit && !done(p.id, code))
      break;
  }
  return 0;
}
} // namespace nc
