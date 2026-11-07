#include "worker.h"
#include "config.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
namespace oc {
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
int run(const Request &r, unsigned id, Text &input, bool &quit) {
  Array<Text> args;
  Text error;
  if (!command(r, args, error)) {
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
    if (stopping || seconds() - start > 180 || quit) {
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
    output(id, "\nOperation timed out after 180 seconds. Check service state "
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
bool list_names(const char *path, Array<Text> &list, bool directories = false) {
  DIR *dir = opendir(path);
  if (!dir)
    return false;
  bool ok = true;
  while (auto *e = readdir(dir)) {
    if (!identifier(e->d_name))
      continue;
    struct stat st;
    if (fstatat(dirfd(dir), e->d_name, &st,
                directories ? AT_SYMLINK_NOFOLLOW : 0) < 0)
      continue;
    if (directories ? !S_ISDIR(st.st_mode)
                    : (!S_ISREG(st.st_mode) || !(st.st_mode & 0111)))
      continue;
    bool exists = false;
    for (size_t i = 0; i < list.size(); ++i)
      if (list[i] == e->d_name)
        exists = true;
    Text t;
    if (!exists &&
        (!t.set(e->d_name) || list.size() >= 16384 || !list.push(t))) {
      ok = false;
      break;
    }
  }
  closedir(dir);
  for (size_t i = 1; i < list.size(); ++i) {
    Text key = list[i];
    size_t j = i;
    while (j && strcmp(list[j - 1].c_str(), key.c_str()) > 0) {
      list[j] = list[j - 1];
      --j;
    }
    list[j] = key;
  }
  return ok;
}
bool catalog(Text &result) {
  Array<Text> services, levels;
  if (!list_names("/etc/init.d", services) ||
      !list_names("/etc/runlevels", levels, true))
    return false;
  list_names("/usr/local/etc/init.d", services);
  Text current;
  if (read_text("/run/openrc/softlevel", current, 256)) {
    size_t n = strcspn(current.c_str(), "\r\n");
    Text name;
    name.set(current.c_str(), n);
    if (identifier(name.c_str()) &&
        (!append(result, "C\t") || !append(result, name.c_str()) ||
         !append(result, "\n")))
      return false;
  }
  for (size_t i = 0; i < levels.size(); ++i)
    if (!append(result, "L\t") || !append(result, levels[i].c_str()) ||
        !append(result, "\n"))
      return false;
  for (size_t i = 0; i < services.size(); ++i) {
    if (!append(result, "S\t") || !append(result, services[i].c_str()) ||
        !append(result, "\t"))
      return false;
    bool first = true;
    for (size_t j = 0; j < levels.size(); ++j) {
      char path[384];
      snprintf(path, sizeof(path), "/etc/runlevels/%s/%s", levels[j].c_str(),
               services[i].c_str());
      struct stat st;
      if (lstat(path, &st) == 0) {
        if ((!first && !append(result, " ")) ||
            !append(result, levels[j].c_str()))
          return false;
        first = false;
      }
    }
    if (!append(result, "\n"))
      return false;
  }
  return result.size() <= MaxPayload;
}
int level_directory(const Request &r, Text &error) {
  int root =
      open("/etc/runlevels", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (root < 0)
    return 1;
  struct stat st;
  bool ok = fstat(root, &st) == 0 && st.st_uid == 0 && !(st.st_mode & 0022);
  if (r.op == Op::DeleteLevel) {
    Text current;
    if (read_text("/run/openrc/softlevel", current, 256) &&
        !strncmp(current.c_str(), r.a.c_str(), r.a.size()) &&
        (current.size() == r.a.size() || current.c_str()[r.a.size()] == '\n'))
      ok = false;
    // Refuse levels referenced by a stack, including an otherwise empty level.
    DIR *levels = fdopendir(dup(root));
    if (!levels)
      ok = false;
    if (levels) {
      while (auto *entry = readdir(levels)) {
        if (!identifier(entry->d_name))
          continue;
        int dir = openat(root, entry->d_name,
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (dir >= 0) {
          struct stat member;
          if (fstatat(dir, r.a.c_str(), &member, AT_SYMLINK_NOFOLLOW) == 0)
            ok = false;
          close(dir);
        }
      }
      closedir(levels);
    }
  }
  if (ok)
    ok = r.op == Op::CreateLevel
             ? mkdirat(root, r.a.c_str(), 0755) == 0
             : unlinkat(root, r.a.c_str(), AT_REMOVEDIR) == 0;
  if (ok)
    ok = fsync(root) == 0;
  close(root);
  if (!ok)
    error.set("Cannot change runlevel: it may exist, be active, referenced, "
              "nonempty, or unsafe.");
  return ok ? 0 : 1;
}
int dispatch(const Request &r, unsigned id, Text &input, bool &quit) {
  Text text, error;
  switch (r.op) {
  case Op::Catalog:
    if (!catalog(text)) {
      output(id, "Cannot read OpenRC services and runlevels.\n");
      return 1;
    }
    return reply(Kind::Output, id, text.c_str(), text.size()) ? 0 : 1;
  case Op::ReadConfig:
  case Op::WriteConfig:
  case Op::RestoreConfig: {
    ConfigStore store;
    Snapshot s;
    s.bytes = r.b;
    s.exists = r.d == "present";
    bool ok;
    if (r.op == Op::ReadConfig) {
      ok = store.read(r.a.c_str(), s, error);
      if (ok) {
        text.set(s.exists ? "1" : "0");
        ok = append(text, s.bytes.c_str()) &&
             reply(Kind::Output, id, text.c_str(), text.size());
      }
    } else if (r.op == Op::WriteConfig)
      ok = store.save(r.a.c_str(), s, r.c, error);
    else
      ok = store.restore(r.a.c_str(), s, error);
    if (!ok)
      output(id, error.c_str());
    else if (r.op != Op::ReadConfig)
      output(
          id,
          "Configuration saved. Previous contents: .openrc-controller.bak\n");
    return ok ? 0 : 1;
  }
  case Op::ReadScript: {
    char path[256];
    snprintf(path, sizeof(path), "/etc/init.d/%s", r.a.c_str());
    if (!read_text(path, text, 256 * 1024)) {
      snprintf(path, sizeof(path), "/usr/local/etc/init.d/%s", r.a.c_str());
      if (!read_text(path, text, 256 * 1024)) {
        output(id, "Cannot read service script.\n");
        return 1;
      }
    }
    output(id, text.c_str());
    return 0;
  }
  case Op::Logs: {
    char path[256];
    snprintf(path, sizeof(path), "/var/log/%s", r.a.c_str());
    if (!read_text(path, text, 256 * 1024, true)) {
      output(id, "Log is absent or cannot be read. Enable OpenRC rc_logger or "
                 "the system logger if needed.\n");
      return 1;
    }
    output(id, text.c_str());
    return 0;
  }
  case Op::CreateLevel:
  case Op::DeleteLevel: {
    int code = level_directory(r, error);
    output(id, code ? error.c_str() : "Runlevel updated.\n");
    return code;
  }
  case Op::Report: {
    output(id, "OpenRC Controller diagnostics\n\nService membership:\n");
    if (catalog(text))
      output(id, text.c_str());
    Request status;
    status.op = Op::State;
    status.a.set("all");
    output(id, "\nOpenRC status:\n");
    int code = run(status, id, input, quit);
    if (!quit) {
      status.a.set("unused");
      output(id, "\nUnassigned services:\n");
      int c = run(status, id, input, quit);
      if (!code)
        code = c;
    }
    return code;
  }
  case Op::Cancel:
    return 0;
  case Op::Quit:
    quit = true;
    return 0;
  default:
    return run(r, id, input, quit);
  }
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
} // namespace oc
