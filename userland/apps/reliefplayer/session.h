#ifndef RELIEFPLAYER_SESSION_H
#define RELIEFPLAYER_SESSION_H
#include "protocol.h"
#include <sys/types.h>
namespace rp {
class Session {
  int commands_ = -1, statuses_ = -1;
  pid_t child_ = -1;
  bool closing_ = false, pending_open_ = false;
  uint64_t deadline_ = 0;
  char path_[4096]{};
  unsigned initial_volume_ = 70, received_ = 0, queued_ = 0;
  Command queue_[16];
  PlaybackStatus partial_, status_;
  void start();
  void close_pipes();
  void flush();

public:
  Session();
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;
  ~Session();
  bool open(const char *path, unsigned volume);
  bool command(Op op, uint64_t value = 0);
  void poll();
  void shutdown();
  bool active() const { return child_ > 0 || pending_open_; }
  const PlaybackStatus &status() const { return status_; }
};
} // namespace rp
#endif
