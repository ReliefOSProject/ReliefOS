#include "capture.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
namespace rc {
namespace {
int ignored_x_error(Display *, XErrorEvent *) { return 0; }
unsigned channel(unsigned long p, unsigned long mask) {
  if (!mask) return 0;
  while (!(mask & 1)) { mask >>= 1; p >>= 1; }
  return unsigned(((p & mask) * 255 + mask / 2) / mask);
}
bool property(Display *d, ::Window w, Atom atom, Atom type,
              unsigned long limit, unsigned char **data, unsigned long &count) {
  Atom actual = 0; int format = 0; unsigned long remaining = 0;
  return XGetWindowProperty(d, w, atom, 0, long(limit), False, type,
    &actual, &format, &count, &remaining, data) == Success && actual == type && format == 32;
}
bool excluded(Display *d, ::Window w) {
  unsigned char *raw = nullptr; unsigned long n = 0;
  Atom kind = XInternAtom(d, "_NET_WM_WINDOW_TYPE", False);
  bool skip = false;
  if (property(d, w, kind, XA_ATOM, 16, &raw, n)) {
    auto *types = reinterpret_cast<Atom *>(raw);
    Atom desktop = XInternAtom(d, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    Atom dock = XInternAtom(d, "_NET_WM_WINDOW_TYPE_DOCK", False);
    for (unsigned long i = 0; i < n; ++i) if (types[i] == desktop || types[i] == dock) skip = true;
  }
  if (raw) XFree(raw);
  return skip;
}
bool window_info(Display *d, ::Window root, ::Window w, int width, int height, WindowInfo &out) {
  XWindowAttributes a{};
  if (!XGetWindowAttributes(d, w, &a) || a.map_state != IsViewable || a.override_redirect || excluded(d, w)) return false;
  ::Window frame = w;
  for (int depth = 0; depth < 16; ++depth) {
    ::Window returned_root, parent, *children = nullptr; unsigned n = 0;
    bool ok = XQueryTree(d, frame, &returned_root, &parent, &children, &n) != 0;
    if (children) XFree(children);
    if (!ok || !parent || parent == root) break;
    frame = parent;
  }
  if (!XGetWindowAttributes(d, frame, &a) || a.map_state != IsViewable) return false;
  int x, y; ::Window child;
  if (!XTranslateCoordinates(d, frame, root, 0, 0, &x, &y, &child)) return false;
  int border = a.border_width;
  out.bounds = selection(x - border, y - border, x + a.width + border,
                         y + a.height + border, width, height);
  if (!out.bounds.valid()) return false;
  out.id = w;
  char *name = nullptr;
  if (XFetchName(d, w, &name) && name) {
    size_t n = strlen(name); if (n > 127) n = 127;
    while (n && !valid_utf8(name, n)) --n;
    memcpy(out.title, name, n); out.title[n] = 0;
    XFree(name);
  }
  return true;
}
}
bool capture_screen(Snapshot &out, Text &error) {
  Display *d = XOpenDisplay(nullptr);
  if (!d) { error.set("Could not connect to the X11 display"); return false; }
  auto previous = XSetErrorHandler(ignored_x_error);
  ::Window root = DefaultRootWindow(d); XWindowAttributes a{};
  Snapshot next;
  bool ok = XGetWindowAttributes(d, root, &a) && a.visual &&
    a.visual->c_class == TrueColor && next.image.create(a.width, a.height);
  if (!ok) error.set("The screen is too large or its pixel format is unsupported");
  XImage *image = ok ? XGetImage(d, root, 0, 0, unsigned(a.width), unsigned(a.height), AllPlanes, ZPixmap) : nullptr;
  if (ok && !image) { ok = false; error.set("Could not read the screen image"); }
  if (ok) {
    for (int y = 0; y < a.height; ++y) for (int x = 0; x < a.width; ++x) {
      unsigned long pixel;
      if (image->bits_per_pixel == 32 && image->byte_order == LSBFirst) {
        uint32_t value; memcpy(&value, image->data + size_t(y) * image->bytes_per_line + size_t(x) * 4, 4); pixel = value;
      } else pixel = XGetPixel(image, x, y);
      next.image.pixels[size_t(y) * a.width + x] =
        (channel(pixel, image->red_mask) << 16) | (channel(pixel, image->green_mask) << 8) | channel(pixel, image->blue_mask);
    }
    unsigned char *raw = nullptr; unsigned long count = 0;
    Atom list = XInternAtom(d, "_NET_CLIENT_LIST_STACKING", False);
    if (property(d, root, list, XA_WINDOW, 4096, &raw, count)) {
      auto *windows = reinterpret_cast<::Window *>(raw);
      for (unsigned long n = count; n && next.windows.size() < 512; --n) {
        WindowInfo w;
        if (window_info(d, root, windows[n - 1], a.width, a.height, w) && !next.windows.push(w)) { ok = false; break; }
      }
    } else {
      ::Window rr, parent, *children = nullptr; unsigned n = 0;
      if (XQueryTree(d, root, &rr, &parent, &children, &n)) {
        while (n && next.windows.size() < 512) {
          WindowInfo w;
          if (window_info(d, root, children[--n], a.width, a.height, w) && !next.windows.push(w)) { ok = false; break; }
        }
      }
      if (children) XFree(children);
    }
    if (raw) XFree(raw);
  }
  if (image) XDestroyImage(image);
  XCloseDisplay(d); XSetErrorHandler(previous);
  if (!ok) { if (error.empty()) error.set("Not enough memory"); return false; }
  out = static_cast<Snapshot &&>(next); error.clear(); return true;
}
}
