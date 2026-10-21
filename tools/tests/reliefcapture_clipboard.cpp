// Independent X11 BMP consumer, including the ICCCM INCR handshake.
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
static uint32_t u32(const unsigned char *p) { return unsigned(p[0]) | (unsigned(p[1]) << 8) | (unsigned(p[2]) << 16) | (unsigned(p[3]) << 24); }
int main() {
  Display *d = XOpenDisplay(nullptr); if (!d) return 1;
  Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1, 1, 0, 0, 0);
  XSelectInput(d, w, PropertyChangeMask);
  Atom clipboard = XInternAtom(d, "CLIPBOARD", False), bmp = XInternAtom(d, "image/bmp", False);
  Atom reply = XInternAtom(d, "RELIEFCAPTURE_TEST_REPLY", False), incr = XInternAtom(d, "INCR", False);
  bool pending = false, incremental = false, have_previous = false; unsigned count = 0;
  size_t bytes = 0, capacity = 0, previous_bytes = 0;
  unsigned char *buffer = nullptr;
  uint64_t hash = 14695981039346656037ULL, previous_hash = 0;
  time_t next = time(nullptr), deadline = next + 1800, request_time = 0;
  puts("RC_CLIPBOARD_READY"); fflush(stdout);
  while (time(nullptr) < deadline) {
    time_t now = time(nullptr);
    if (!pending && now >= next && XGetSelectionOwner(d, clipboard)) {
      XDeleteProperty(d, w, reply); XConvertSelection(d, clipboard, bmp, reply, w, CurrentTime); XFlush(d);
      pending = true; incremental = false; bytes = 0; hash = 14695981039346656037ULL; request_time = now;
    }
    while (XPending(d)) {
      XEvent e; XNextEvent(d, &e);
      bool response = e.type == SelectionNotify && e.xselection.property == reply;
      bool chunk = e.type == PropertyNotify && e.xproperty.atom == reply && e.xproperty.state == PropertyNewValue && incremental;
      if (e.type == SelectionNotify && e.xselection.property == None) { pending = false; next = now + 2; }
      if (!response && !chunk) continue;
      Atom type = 0; int format = 0; unsigned long n = 0, remaining = 0; unsigned char *data = nullptr;
      if (XGetWindowProperty(d, w, reply, 0, 20000000, True, AnyPropertyType, &type, &format, &n, &remaining, &data) != Success) return 2;
      if (type == incr) { incremental = true; if (data) XFree(data); XFlush(d); continue; }
      if (format == 8 && n) {
        const size_t limit = 64 * 1024 * 1024;
        if (n > limit - bytes) return 5;
        if (bytes + n > capacity) {
          size_t next_capacity = capacity ? capacity : 65536;
          while (next_capacity < bytes + n) next_capacity *= 2;
          auto *grown = static_cast<unsigned char *>(realloc(buffer, next_capacity));
          if (!grown) return 5;
          buffer = grown; capacity = next_capacity;
        }
        for (size_t i = 0; i < n; ++i) {
          buffer[bytes + i] = data[i]; hash = (hash ^ data[i]) * 1099511628211ULL;
        }
        bytes += n;
      }
      if (data) XFree(data);
      if (!incremental || !n) {
        if (bytes >= 54) {
          // Repeated polling must not fill a small private test filesystem.
          if (!have_previous || hash != previous_hash || bytes != previous_bytes) {
            char path[128]; snprintf(path, sizeof(path), "/home/test/capture-clipboard-%u.bmp", count++);
            FILE *file = fopen(path, "wb"); if (!file) return 3;
            bool written = fwrite(buffer, 1, bytes, file) == bytes;
            if (fclose(file) || !written) return 4;
            previous_hash = hash; previous_bytes = bytes; have_previous = true;
          }
          printf("RC_CLIPBOARD_BMP bytes=%zu width=%u height=%u\n", bytes, u32(buffer + 18), u32(buffer + 22)); fflush(stdout);
        }
        pending = false; incremental = false; next = now + 3;
      }
      XFlush(d);
    }
    if (pending && now - request_time > 15) { pending = incremental = false; next = now + 3; }
    usleep(20000);
  }
  free(buffer);
  XDestroyWindow(d, w); XCloseDisplay(d); return 0;
}
