#ifndef RELIEFCAPTURE_JOB_H
#define RELIEFCAPTURE_JOB_H
#include "capture.h"
#include "png_io.h"
#include <sys/types.h>
namespace rc {
class Job {
public:
  enum class Kind { Idle, Capture, Save };
  struct Header { uint32_t magic = 0x52434331, ok = 0, width = 0, height = 0, windows = 0; char error[192]{}; };
private:
  Kind kind_ = Kind::Idle;
  pid_t pid_ = -1;
  int fd_ = -1, x_fd_ = -1;
  Header header_{};
  size_t received_ = 0;
  bool eof_ = false, allocated_ = false, ok_ = false;
  double deadline_ = 0;
  Snapshot snapshot_;
  Text error_;
  bool start(Kind, const char *, const Image *, const Destination *, Text &);
  void failed(const char *);
public:
  void x_fd(int fd) { x_fd_ = fd; }
  bool capture(Text &error) { return start(Kind::Capture, nullptr, nullptr, nullptr, error); }
  bool save(const char *p, const Image &im, const Destination &d, Text &e) { return start(Kind::Save, p, &im, &d, e); }
  bool busy() const { return kind_ != Kind::Idle; }
  Kind kind() const { return kind_; }
  bool poll();
  bool ok() const { return ok_; }
  const Text &error() const { return error_; }
  Snapshot take() { return static_cast<Snapshot &&>(snapshot_); }
  void cancel();
  ~Job();
};
double monotonic();
}
#endif
