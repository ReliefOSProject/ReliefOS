#include "keyboard.h"
#include <X11/keysym.h>
namespace rc {
namespace {
int error_code = 0;
int grab_error(Display *, XErrorEvent *error) { error_code = error->error_code; return 0; }
}
bool EscapeKey::acquire(Display *display) {
  release();
  if (!display) return false;
  unsigned key = XKeysymToKeycode(display, XK_Escape);
  if (!key) return false;
  unsigned num_mask = 0;
  KeyCode num = XKeysymToKeycode(display, XK_Num_Lock);
  XModifierKeymap *mapping = XGetModifierMapping(display);
  if (mapping) {
    for (unsigned mod = 0; mod < 8; ++mod) for (int n = 0; n < mapping->max_keypermod; ++n)
      if (num && mapping->modifiermap[mod * mapping->max_keypermod + n] == num) num_mask |= 1u << mod;
    XFreeModifiermap(mapping);
  }
  // Other Escape combinations belong to IceWM or focused applications.
  unsigned masks[] = {0, LockMask, num_mask, LockMask | num_mask};
  XSync(display, False); error_code = 0;
  auto previous = XSetErrorHandler(grab_error);
  for (unsigned mask : masks)
    XGrabKey(display, int(key), mask, DefaultRootWindow(display), False, GrabModeAsync, GrabModeAsync);
  XSync(display, False); XSetErrorHandler(previous);
  if (error_code) {
    XUngrabKey(display, int(key), AnyModifier, DefaultRootWindow(display)); XFlush(display); return false;
  }
  display_ = display; key_ = key; return true;
}
void EscapeKey::release() {
  if (!display_ || !key_) return;
  XUngrabKey(display_, int(key_), AnyModifier, DefaultRootWindow(display_));
  XUngrabKeyboard(display_, CurrentTime); XFlush(display_);
  key_ = 0; display_ = nullptr;
}
bool EscapeKey::matches(const XEvent &event) const {
  return display_ && key_ && (event.type == KeyPress || event.type == KeyRelease) &&
         unsigned(event.xkey.keycode) == key_;
}
}
