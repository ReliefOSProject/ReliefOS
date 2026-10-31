#ifndef RELIEFCAPTURE_PNG_IO_H
#define RELIEFCAPTURE_PNG_IO_H
#include "model.h"
namespace rc {
struct Destination {
  bool exists = false;
  uint64_t dir_dev = 0, dir_ino = 0, dev = 0, ino = 0, size = 0;
  int64_t mtime = 0, mtime_ns = 0, ctime = 0, ctime_ns = 0;
  unsigned mode = 0600;
};
bool probe_destination(const char *, Destination &, Text &);
bool save_png(const char *, const Image &, const Destination &, Text &);
}
#endif
