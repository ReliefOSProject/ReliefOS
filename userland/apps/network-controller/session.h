#ifndef NETWORK_CONTROLLER_SESSION_H
#define NETWORK_CONTROLLER_SESSION_H
#include "model.h"
#include <sys/types.h>
namespace nc {
enum class Auth { Direct, Sudo, Root };
class Session {
  pid_t pid_ = -1;
  int input_ = -1, output_ = -1, errors_ = -1;
  Auth auth_ = Auth::Direct;
  Text incoming_, outgoing_, diagnostics_;
  size_t sent_ = 0;
  char secret_[1026]{};
  size_t secret_size_ = 0, secret_sent_ = 0;
  bool ready_ = false, prompt_ = false, closing_ = false;
  double started_ = 0;
  void wipe();
  void error(const char *);
  void drain(int &, bool);

public:
  Array<Packet> events;
  Text problem;
  ~Session();
  bool launch(Auth);
  void pump();
  bool password(const char *);
  bool send(const Request &, unsigned id);
  void stop();
  bool ready() const { return ready_; }
  bool alive() const { return pid_ > 0; }
  bool password_requested() const { return prompt_; }
};
} // namespace nc
#endif
