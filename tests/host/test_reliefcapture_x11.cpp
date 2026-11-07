#include "../../userland/apps/reliefcapture/capture.h"
#include "../../userland/apps/reliefcapture/job.h"
#include "../../userland/apps/reliefcapture/keyboard.h"
#include <X11/keysym.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
int main() {
  Display *d = XOpenDisplay(nullptr); assert(d);
  ::Window root = DefaultRootWindow(d);
  Display *wm = XOpenDisplay(nullptr); assert(wm);
  unsigned escape = XKeysymToKeycode(d, XK_Escape);
  // Real IceWM defaults own these combinations: do not request AnyModifier.
  XGrabKey(wm, int(escape), ShiftMask, root, False, GrabModeAsync, GrabModeAsync);
  XGrabKey(wm, int(escape), ControlMask | Mod1Mask, root, False, GrabModeAsync, GrabModeAsync);
  XSync(wm, False);
  rc::EscapeKey cancel;
  assert(cancel.acquire(d));
  XEvent key{}; key.type = KeyPress; key.xkey.window = root; key.xkey.keycode = escape;
  assert(cancel.matches(key));
  // Escape may already be queued for the editor's XID when it is hidden.
  key.xkey.window = XCreateSimpleWindow(d, root, 0, 0, 1, 1, 0, 0, 0);
  XDestroyWindow(d, key.xkey.window);
  assert(cancel.matches(key));
  key.xkey.keycode = XKeysymToKeycode(d, XK_a);
  assert(!cancel.matches(key)); cancel.release();
  key.xkey.keycode = escape; assert(!cancel.matches(key)); XCloseDisplay(wm);
  XSetWindowBackground(d, root, 0x123456); XClearWindow(d, root);
  ::Window win = XCreateSimpleWindow(d, root, 40, 50, 120, 80, 0, 0, 0x00aa44);
  XStoreName(d, win, "Capture fixture"); XMapWindow(d, win);
  Atom clients = XInternAtom(d, "_NET_CLIENT_LIST_STACKING", False);
  XChangeProperty(d, root, clients, XA_WINDOW, 32, PropModeReplace,
                  reinterpret_cast<unsigned char *>(&win), 1);
  XSync(d, False);
  rc::Snapshot snapshot; rc::Text error;
  assert(rc::capture_screen(snapshot, error));
  assert(snapshot.image.width == 640 && snapshot.image.height == 480);
  assert(snapshot.image.pixels[0] == 0x123456);
  assert(snapshot.image.pixels[60 * 640 + 50] == 0x00aa44);
  assert(snapshot.windows.size() == 1 && snapshot.windows[0].bounds.x == 40 && snapshot.windows[0].bounds.y == 50);
  rc::Job job; job.x_fd(ConnectionNumber(d));
  assert(job.capture(error));
  double deadline = rc::monotonic() + 10;
  while (job.busy() && rc::monotonic() < deadline) { job.poll(); usleep(1000); }
  assert(!job.busy() && job.ok());
  rc::Snapshot received = job.take();
  assert(received.image.pixels[60 * 640 + 50] == 0x00aa44 && received.windows.size() == 1);
  assert(job.capture(error)); job.cancel(); assert(!job.busy());
  const char *display = getenv("DISPLAY"); rc::Text saved; saved.set(display);
  setenv("DISPLAY", ":65535", 1); assert(job.capture(error));
  deadline = rc::monotonic() + 10;
  while (job.busy() && rc::monotonic() < deadline) { job.poll(); usleep(1000); }
  assert(!job.busy() && !job.ok() && !job.error().empty());
  setenv("DISPLAY", saved.c_str(), 1);
  XDestroyWindow(d, win); XCloseDisplay(d);
  puts("ReliefCapture: X11 pixels/windows, IPC, cancellation/error replies and coexistence with IceWM Escape bindings passed");
}
