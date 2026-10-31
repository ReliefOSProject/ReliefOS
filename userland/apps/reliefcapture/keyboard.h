#ifndef RELIEFCAPTURE_KEYBOARD_H
#define RELIEFCAPTURE_KEYBOARD_H
#include <X11/Xlib.h>
namespace rc {
class EscapeKey {
  Display *display_ = nullptr;
  unsigned key_ = 0;
public:
  bool acquire(Display *);
  void release();
  bool matches(const XEvent &) const;
  ~EscapeKey() { release(); }
};
}
#endif
