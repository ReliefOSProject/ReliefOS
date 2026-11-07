#include "session.h"
#include "worker.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
namespace rp {
namespace {
bool nonblocking(int fd) {
  return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) >= 0 &&
         fcntl(fd, F_SETFD, FD_CLOEXEC) >= 0;
}
} // namespace
Session::Session() { signal(SIGPIPE, SIG_IGN); }
Session::~Session() {
  pending_open_ = false;
  if (child_ > 0) {
    kill(child_, SIGKILL);
    while (waitpid(child_, nullptr, 0) < 0 && errno == EINTR) {
    }
  }
  close_pipes();
}
void Session::close_pipes() {
  if (commands_ >= 0)
    close(commands_);
  if (statuses_ >= 0)
    close(statuses_);
  commands_ = statuses_ = -1;
  queued_ = received_ = 0;
}
void Session::start() {
  pending_open_ = false;
  closing_ = false;
  int command_pipe[2] = {-1, -1}, status_pipe[2] = {-1, -1};
  if (pipe(command_pipe) < 0 || pipe(status_pipe) < 0 ||
      !nonblocking(command_pipe[0]) || !nonblocking(command_pipe[1]) ||
      !nonblocking(status_pipe[0]) || !nonblocking(status_pipe[1])) {
    for (int fd : command_pipe)
      if (fd >= 0)
        close(fd);
    for (int fd : status_pipe)
      if (fd >= 0)
        close(fd);
    status_.state = State::Failed;
    status_.error = Error::Worker;
    return;
  }
  child_ = fork();
  if (child_ == 0) {
    close(command_pipe[1]);
    close(status_pipe[0]);
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    int result =
        worker(path_, command_pipe[0], status_pipe[1], initial_volume_);
    close(command_pipe[0]);
    close(status_pipe[1]);
    _exit(result);
  }
  close(command_pipe[0]);
  close(status_pipe[1]);
  if (child_ < 0) {
    close(command_pipe[1]);
    close(status_pipe[0]);
    status_.state = State::Failed;
    status_.error = Error::Worker;
    return;
  }
  commands_ = command_pipe[1];
  statuses_ = status_pipe[0];
  status_ = PlaybackStatus{};
  status_.state = State::Loading;
}
bool Session::open(const char *path, unsigned volume) {
  if (!path || !*path || strlen(path) >= sizeof(path_))
    return false;
  memcpy(path_, path, strlen(path) + 1);
  initial_volume_ = volume > 100 ? 100 : volume;
  if (child_ > 0) {
    shutdown();
    pending_open_ = true;
    status_ = PlaybackStatus{};
    status_.state = State::Loading;
  } else
    start();
  return true;
}
void Session::shutdown() {
  pending_open_ = false;
  if (child_ <= 0 || closing_)
    return;
  queued_ = 0;
  command(Op::Quit);
  closing_ = true;
  deadline_ = monotonic_ms() + 1500;
}
bool Session::command(Op op, uint64_t value) {
  if (commands_ < 0 || closing_)
    return false;
  // Continuous slider motion coalesces only adjacent commands of the same kind.
  if (queued_ && queue_[queued_ - 1].op == op &&
      (op == Op::Volume || op == Op::Seek))
    queue_[queued_ - 1].value = value;
  else {
    if (queued_ == 16)
      return false;
    queue_[queued_++] = {Protocol, op, value};
  }
  flush();
  return true;
}
void Session::flush() {
  while (queued_ && commands_ >= 0) {
    ssize_t n = write(commands_, queue_, sizeof(Command));
    if (n < 0 && errno == EINTR)
      continue;
    if (n != ssize_t(sizeof(Command)))
      return;
    --queued_;
    memmove(queue_, queue_ + 1, queued_ * sizeof(Command));
  }
}
void Session::poll() {
  if (child_ <= 0)
    return;
  flush();
  for (unsigned messages = 0; messages < 128 && statuses_ >= 0; ++messages) {
    ssize_t n = read(statuses_, reinterpret_cast<char *>(&partial_) + received_,
                     sizeof(PlaybackStatus) - received_);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    received_ += unsigned(n);
    if (received_ == sizeof(PlaybackStatus)) {
      received_ = 0;
      if (partial_.magic == Protocol && !closing_)
        status_ = partial_;
    }
  }
  int result = 0;
  pid_t reaped = waitpid(child_, &result, WNOHANG);
  if (reaped == child_ || (reaped < 0 && errno == ECHILD)) {
    child_ = -1;
    close_pipes();
    if (pending_open_)
      start();
    else if (closing_) {
      closing_ = false;
      status_.state = State::Stopped;
    } else if (status_.state != State::Failed) {
      status_.state = State::Failed;
      status_.error = Error::Worker;
    }
  } else if (closing_ && monotonic_ms() >= deadline_) {
    kill(child_, SIGKILL);
    deadline_ = monotonic_ms() + 1500;
  }
}
} // namespace rp
