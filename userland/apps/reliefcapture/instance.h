#ifndef RELIEFCAPTURE_INSTANCE_H
#define RELIEFCAPTURE_INSTANCE_H
#include "model.h"
namespace rc {
class Instance {
  unsigned long window_ = 0, message_ = 0;
  void (*request_)(bool, void *) = nullptr;
  void *data_ = nullptr;
  static int event(void *, void *);
public:
  enum class Result { Primary, Forwarded, Failed };
  Result claim(bool select, Text &);
  void callback(void (*fn)(bool, void *), void *data) { request_ = fn; data_ = data; }
  ~Instance();
};
}
#endif
