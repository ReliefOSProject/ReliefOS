#include "job.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
namespace rc {
double monotonic() { timespec ts{}; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }
namespace {
bool send_all(int fd, const void *data, size_t count) {
  auto *bytes = static_cast<const unsigned char *>(data);
  while (count) {
    ssize_t n = write(fd, bytes, count);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    bytes += n; count -= size_t(n);
  }
  return true;
}
}
Job::~Job() { cancel(); }
void Job::cancel() {
  if (fd_ >= 0) close(fd_);
  fd_ = -1;
  if (pid_ > 0) {
    kill(pid_, SIGKILL);
    while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
  }
  pid_ = -1; kind_ = Kind::Idle; snapshot_ = Snapshot{};
}
void Job::failed(const char *message) { cancel(); error_.set(message); ok_ = false; }
bool Job::start(Kind kind, const char *path, const Image *image, const Destination *dest, Text &error) {
  if (busy()) { error.set("A screenshot operation is already running"); return false; }
  int pipes[2];
  if (pipe(pipes) < 0) { error.set("Could not create the screenshot channel"); return false; }
  pid_t pid = fork();
  if (pid == 0) {
    close(pipes[0]);
    if (x_fd_ >= 0) close(x_fd_);
    signal(SIGPIPE, SIG_IGN);
    Snapshot snapshot; Text message;
    bool ok = kind == Kind::Capture ? capture_screen(snapshot, message) : save_png(path, *image, *dest, message);
    Header h; h.ok = ok ? 1 : 0;
    snprintf(h.error, sizeof(h.error), "%s", message.c_str());
    if (ok && kind == Kind::Capture) {
      h.width = snapshot.image.width; h.height = snapshot.image.height; h.windows = unsigned(snapshot.windows.size());
    }
    bool sent = send_all(pipes[1], &h, sizeof(h));
    if (sent && ok && kind == Kind::Capture)
      sent = send_all(pipes[1], snapshot.windows.data(), snapshot.windows.size() * sizeof(WindowInfo)) &&
             send_all(pipes[1], snapshot.image.pixels.data(), snapshot.image.pixels.size() * 4);
    close(pipes[1]); _exit(sent ? 0 : 1);
  }
  close(pipes[1]);
  if (pid < 0) { close(pipes[0]); error.set("Could not start the screenshot process"); return false; }
  pid_ = pid; fd_ = pipes[0]; kind_ = kind;
  int flags = fcntl(fd_, F_GETFL, 0);
  if (flags < 0 || fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
    failed("Could not configure the screenshot channel"); error = error_; return false;
  }
  header_ = Header{}; received_ = 0; eof_ = false; allocated_ = false; ok_ = false;
  snapshot_ = Snapshot{}; error_.clear(); deadline_ = monotonic() + 45;
  error.clear(); return true;
}
bool Job::poll() {
  if (!busy()) return false;
  if (monotonic() > deadline_) { failed("The screenshot operation timed out"); return true; }
  size_t budget = 256 * 1024;
  while (!eof_ && budget) {
    unsigned char extra;
    void *target = &extra; size_t capacity = 1;
    size_t body = 0, window_bytes = size_t(header_.windows) * sizeof(WindowInfo);
    if (received_ < sizeof(Header)) {
      target = reinterpret_cast<char *>(&header_) + received_; capacity = sizeof(Header) - received_;
    } else {
      if (!allocated_) {
        if (header_.magic != 0x52434331 || header_.ok > 1 || !memchr(header_.error, 0, sizeof(header_.error)) ||
            header_.windows > 512 || (kind_ == Kind::Save && (header_.width || header_.height || header_.windows))) {
          failed("Invalid screenshot process reply"); return true;
        }
        if (header_.ok && kind_ == Kind::Capture &&
            (!snapshot_.image.create(int(header_.width), int(header_.height)) || !snapshot_.windows.resize(header_.windows))) {
          failed("Not enough memory"); return true;
        }
        if (!header_.ok && (header_.width || header_.height || header_.windows)) {
          failed("Invalid screenshot process reply"); return true;
        }
        allocated_ = true;
      }
      body = received_ - sizeof(Header);
      if (header_.ok && kind_ == Kind::Capture) {
        if (body < window_bytes) {
          target = reinterpret_cast<char *>(snapshot_.windows.data()) + body; capacity = window_bytes - body;
        } else if (body - window_bytes < snapshot_.image.pixels.size() * 4) {
          target = reinterpret_cast<char *>(snapshot_.image.pixels.data()) + body - window_bytes;
          capacity = snapshot_.image.pixels.size() * 4 - (body - window_bytes);
        }
      }
    }
    if (capacity > budget) capacity = budget;
    ssize_t n = read(fd_, target, capacity);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    if (n < 0) { failed("Could not read the screenshot process reply"); return true; }
    if (!n) { eof_ = true; break; }
    if (target == &extra) { failed("Invalid screenshot process reply"); return true; }
    received_ += size_t(n); budget -= size_t(n);
  }
  if (!eof_) return false;
  int status = 0; pid_t result = waitpid(pid_, &status, WNOHANG);
  if (!result || (result < 0 && errno == EINTR)) return false;
  size_t expected = sizeof(Header);
  if (header_.ok && kind_ == Kind::Capture)
    expected += snapshot_.windows.size() * sizeof(WindowInfo) + snapshot_.image.pixels.size() * 4;
  if (result < 0 || !WIFEXITED(status) || WEXITSTATUS(status) || !allocated_ || received_ != expected) {
    pid_ = -1; failed("The screenshot process stopped unexpectedly"); return true;
  }
  close(fd_); fd_ = -1; pid_ = -1; kind_ = Kind::Idle; ok_ = header_.ok != 0;
  if (!ok_) error_.set(header_.error[0] ? header_.error : "The screenshot operation failed");
  return true;
}
}
