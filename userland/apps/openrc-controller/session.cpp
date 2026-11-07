#include "session.h"
#include "worker.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
namespace oc {
namespace {
double now() {
  struct timespec t{};
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}
void closefd(int &fd) {
  if (fd >= 0)
    close(fd);
  fd = -1;
}
bool nonblock(int fd) {
  int f = fcntl(fd, F_GETFL);
  return f >= 0 && fcntl(fd, F_SETFL, f | O_NONBLOCK) >= 0;
}
} // namespace
void Session::wipe() {
  volatile char *p = secret_;
  for (size_t n = 0; n < sizeof(secret_); ++n)
    p[n] = 0;
  secret_size_ = secret_sent_ = 0;
}
void Session::error(const char *message) {
  problem.set(message);
  stop();
}
Session::~Session() {
  stop();
  // Closing the request channel tells the privileged helper to cancel/reap its
  // command. Do not send signals to unrelated or privileged process groups.
  double until = now() + 3;
  while (pid_ > 0 && now() < until) {
    pump();
    struct pollfd f{-1, 0, 0};
    poll(&f, 0, 20);
  }
  if (pid_ > 0) {
    if (!ready_)
      kill(pid_, SIGTERM);
    waitpid(pid_, nullptr, WNOHANG);
  }
}
bool Session::launch(Auth auth) {
  if (alive())
    return false;
  signal(SIGPIPE, SIG_IGN);
  wipe();
  problem.clear();
  diagnostics_.clear();
  incoming_.clear();
  outgoing_.clear();
  events.clear();
  auth_ = auth;
  ready_ = prompt_ = closing_ = false;
  sent_ = 0;
  int in[2] = {-1, -1}, out[2] = {-1, -1}, err[2] = {-1, -1}, master = -1,
      slave = -1;
  bool ok = pipe2(out, O_CLOEXEC) == 0 && pipe2(err, O_CLOEXEC) == 0;
  if (ok && auth == Auth::Root) {
    master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    ok = master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0;
    char *name = ok ? ptsname(master) : nullptr;
    slave = name ? open(name, O_RDWR | O_NOCTTY | O_CLOEXEC) : -1;
    ok = ok && slave >= 0;
  } else if (ok)
    ok = pipe2(in, O_CLOEXEC) == 0;
  if (!ok) {
    int *fds[] = {&in[0],  &in[1],  &out[0], &out[1],
                  &err[0], &err[1], &master, &slave};
    for (int *fd : fds)
      closefd(*fd);
    problem.set("Cannot create authentication channels.");
    return false;
  }
  pid_ = fork();
  if (pid_ == 0) {
    close(out[0]);
    close(err[0]);
    if (auth == Auth::Root) {
      close(master);
      if (setsid() < 0 || ioctl(slave, TIOCSCTTY, 0) < 0 || dup2(slave, 0) < 0)
        _exit(125);
      close(slave);
    } else {
      close(in[1]);
      if (dup2(in[0], 0) < 0)
        _exit(125);
      close(in[0]);
    }
    if (dup2(out[1], 1) < 0 || dup2(err[1], 2) < 0)
      _exit(125);
    close(out[1]);
    close(err[1]);
    // Authentication prompts are a private protocol, independent of GUI locale.
    setenv("LC_ALL", "C", 1);
    setenv("LANG", "C", 1);
    if (auth == Auth::Direct)
      _exit(worker_main());
    if (auth == Auth::Sudo) {
      const char *argv[] = {"/usr/bin/sudo",
                            "-k",
                            "-S",
                            "-p",
                            "__OPENRC_PASSWORD__",
                            "-u",
                            "root",
                            "--",
                            WorkerPath,
                            "--worker",
                            nullptr};
      execv(argv[0], const_cast<char *const *>(argv));
    } else {
      const char *argv[] = {"/bin/su", "--shell",  WorkerPath, "--",
                            "root",    "--worker", nullptr};
      execv(argv[0], const_cast<char *const *>(argv));
    }
    dprintf(2, "Authentication program failed: %s\n", strerror(errno));
    _exit(127);
  }
  closefd(out[1]);
  closefd(err[1]);
  closefd(in[0]);
  closefd(slave);
  if (pid_ < 0) {
    closefd(master);
    closefd(in[1]);
    closefd(out[0]);
    closefd(err[0]);
    problem.set("Cannot start authentication worker.");
    return false;
  }
  input_ = auth == Auth::Root ? master : in[1];
  output_ = out[0];
  errors_ = err[0];
  if (!nonblock(input_) || !nonblock(output_) || !nonblock(errors_)) {
    error("Cannot configure authentication channels.");
    return false;
  }
  started_ = now();
  return true;
}
bool Session::password(const char *s) {
  size_t n = strlen(s);
  if (ready_ || closing_ || n > 1024 || memchr(s, '\n', n) ||
      memchr(s, '\r', n))
    return false;
  wipe();
  memcpy(secret_, s, n);
  secret_[n] = '\n';
  secret_size_ = n + 1;
  return true;
}
bool Session::send(const Request &r, unsigned id) {
  if (!ready_ || closing_ || outgoing_.size() > 2 * MaxPayload)
    return false;
  Text error, body, frame;
  if (!validate(r, error) || !encode_request(r, body))
    return false;
  Packet p;
  p.id = id;
  p.kind = Kind::Request;
  p.body = body;
  return encode_packet(p, frame) &&
         append(outgoing_, frame.c_str(), frame.size());
}
void Session::drain(int &fd, bool data) {
  for (size_t budget = 0; fd >= 0 && budget < 65536;) {
    char b[4096];
    ssize_t n = read(fd, b, sizeof(b));
    if (n > 0) {
      budget += n;
      if (data) {
        if (incoming_.size() + n > 2 * MaxPayload || !append(incoming_, b, n)) {
          error("Invalid worker response.");
          return;
        }
      } else {
        if (!append(diagnostics_, b, n)) {
          error("Authentication output is too large.");
          return;
        }
        if (diagnostics_.size() > 16384) {
          Text tail;
          tail.set(diagnostics_.c_str() + diagnostics_.size() - 8192, 8192);
          diagnostics_ = tail;
        }
        if (auth_ == Auth::Sudo &&
            strstr(diagnostics_.c_str(), "__OPENRC_PASSWORD__")) {
          prompt_ = true;
          diagnostics_.clear();
        }
        if (auth_ == Auth::Root) {
          struct termios t;
          if (input_ >= 0 && tcgetattr(input_, &t) == 0 &&
              !(t.c_lflag & ECHO) &&
              (strstr(diagnostics_.c_str(), "assword:") ||
               strstr(diagnostics_.c_str(), "PASSWORD:"))) {
            prompt_ = true;
            diagnostics_.clear();
          }
        }
      }
    } else if (n < 0 && errno == EINTR)
      continue;
    else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      break;
    else {
      closefd(fd);
      break;
    }
  }
}
void Session::pump() {
  if (pid_ < 0)
    return;
  drain(output_, true);
  drain(errors_, false);
  if (auth_ == Auth::Root && input_ >= 0 && !ready_) {
    // su may write its password prompt directly to /dev/tty.
    int fd = input_;
    drain(fd, false);
    if (fd < 0)
      input_ = -1;
    // ECHO alone is insufficient: the authenticated worker also makes its
    // terminal raw. Only a real password prompt may consume the secret.
  }
  Packet p;
  int parsed = 0;
  for (unsigned n = 0; n < 128 && (parsed = take_packet(incoming_, p)) == 1;
       ++n) {
    if (p.kind == Kind::Ready && !ready_) {
      ready_ = true;
      prompt_ = false;
      wipe();
    } else if ((p.kind == Kind::Output || p.kind == Kind::Done) && ready_) {
      if (events.size() >= 512 || !events.push(p)) {
        error("Too many unread worker responses.");
        return;
      }
    } else {
      error("Invalid worker handshake.");
      return;
    }
  }
  if (parsed < 0) {
    error("Malformed worker response.");
    return;
  }
  if (input_ >= 0 && !ready_ && prompt_ && secret_size_) {
    bool safe = true;
    if (auth_ == Auth::Root) {
      struct termios t;
      safe = tcgetattr(input_, &t) == 0 && !(t.c_lflag & ECHO);
    }
    if (safe) {
      ssize_t n =
          write(input_, secret_ + secret_sent_, secret_size_ - secret_sent_);
      if (n > 0) {
        volatile char *p = secret_ + secret_sent_;
        for (ssize_t i = 0; i < n; ++i)
          p[i] = 0;
        secret_sent_ += n;
        if (secret_sent_ == secret_size_) {
          wipe();
          prompt_ = false;
        }
      } else if (n < 0 && errno != EINTR && errno != EAGAIN &&
                 errno != EWOULDBLOCK)
        error("Cannot send authentication password.");
    }
  }
  if (input_ >= 0 && ready_ && sent_ < outgoing_.size()) {
    ssize_t n =
        write(input_, outgoing_.c_str() + sent_, outgoing_.size() - sent_);
    if (n > 0) {
      sent_ += n;
      if (sent_ == outgoing_.size()) {
        outgoing_.clear();
        sent_ = 0;
      }
    } else if (n < 0 && errno != EINTR && errno != EAGAIN &&
               errno != EWOULDBLOCK)
      error("Worker connection closed.");
  }
  int status = 0;
  pid_t got = waitpid(pid_, &status, WNOHANG);
  if (got == pid_ || (got < 0 && errno == ECHILD)) {
    drain(output_, true);
    drain(errors_, false);
    closefd(input_);
    closefd(output_);
    closefd(errors_);
    pid_ = -1;
    wipe();
    if (!closing_ && problem.empty())
      problem.set(ready_
                      ? "Privileged worker exited. Reopen the application to "
                        "authenticate again."
                      : "Authentication failed. Check the password and "
                        "permission to use sudo, or select the root account.");
  } else if (!ready_ && !closing_ && now() - started_ > 90)
    error("Authentication timed out.");
}
void Session::stop() {
  if (closing_)
    return;
  if (ready_ && input_ >= 0) {
    Request q;
    q.op = Op::Quit;
    if (send(q, 0)) { // best effort; EOF also triggers command cleanup
      while (sent_ < outgoing_.size()) {
        ssize_t n =
            write(input_, outgoing_.c_str() + sent_, outgoing_.size() - sent_);
        if (n <= 0)
          break;
        sent_ += n;
      }
    }
  }
  closing_ = true;
  wipe();
  closefd(input_);
  if (pid_ > 0 && !ready_)
    kill(pid_, SIGTERM);
}
} // namespace oc
