#include "png_io.h"
#include <png.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
namespace rc {
namespace {
bool fail(Text &e, const char *s) { e.set(s); return false; }
Destination stamp(const struct stat &s) {
  Destination d; d.exists = true; d.dev = s.st_dev; d.ino = s.st_ino; d.size = s.st_size;
  d.mtime = s.st_mtim.tv_sec; d.mtime_ns = s.st_mtim.tv_nsec;
  d.ctime = s.st_ctim.tv_sec; d.ctime_ns = s.st_ctim.tv_nsec; d.mode = s.st_mode & 0777;
  return d;
}
bool same(const Destination &a, const Destination &b) {
  return a.exists == b.exists && (!a.exists ||
    (a.dev == b.dev && a.ino == b.ino && a.size == b.size && a.mtime == b.mtime &&
     a.mtime_ns == b.mtime_ns && a.ctime == b.ctime && a.ctime_ns == b.ctime_ns));
}
int directory(const char *path, char *base, Text &error) {
  if (!path || !*path || strlen(path) >= PATH_MAX) {
    fail(error, "Invalid destination path"); return -1;
  }
  char folder[PATH_MAX]; const char *slash = strrchr(path, '/');
  if (!slash) { strcpy(folder, "."); strcpy(base, path); }
  else {
    size_t n = size_t(slash - path); if (!n) n = 1;
    memcpy(folder, path, n); folder[n] = 0; strcpy(base, slash + 1);
  }
  if (!*base || !strcmp(base, ".") || !strcmp(base, "..")) {
    fail(error, "Invalid destination path"); return -1;
  }
  int fd = open(folder, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) fail(error, "Could not open the destination folder");
  return fd;
}
bool destination_at(int fd, const char *base, Destination &dest, Text &error) {
  struct stat st{}, parent{};
  if (fstat(fd, &parent) < 0) return fail(error, "Could not inspect the destination");
  if (fstatat(fd, base, &st, AT_SYMLINK_NOFOLLOW) < 0) {
    if (errno != ENOENT) return fail(error, "Could not inspect the destination");
    dest = Destination{};
  } else {
    if (!S_ISREG(st.st_mode)) return fail(error, "The destination must be a regular file");
    dest = stamp(st);
  }
  dest.dir_dev = parent.st_dev; dest.dir_ino = parent.st_ino;
  return true;
}
}
bool probe_destination(const char *path, Destination &out, Text &error) {
  char base[PATH_MAX]; int fd = directory(path, base, error);
  if (fd < 0) return false;
  Destination dest; bool ok = destination_at(fd, base, dest, error); close(fd);
  if (ok) { out = dest; error.clear(); }
  return ok;
}
bool save_png(const char *path, const Image &im, const Destination &expected, Text &error) {
  if (!im.valid()) return fail(error, "There is no screenshot to save");
  char base[PATH_MAX]; int dir = directory(path, base, error);
  if (dir < 0) return false;
  Destination current;
  if (!destination_at(dir, base, current, error)) { close(dir); return false; }
  if (!same(current, expected) || current.dir_dev != expected.dir_dev || current.dir_ino != expected.dir_ino) {
    close(dir); return fail(error, "The destination changed; choose Save again");
  }
  Array<unsigned char> rgb, encoded;
  if (!rgb.resize(im.pixels.size() * 3)) { close(dir); return fail(error, "Not enough memory"); }
  for (size_t n = 0; n < im.pixels.size(); ++n) {
    rgb[n * 3] = im.pixels[n] >> 16; rgb[n * 3 + 1] = im.pixels[n] >> 8; rgb[n * 3 + 2] = im.pixels[n];
  }
  png_image png{}; png.version = PNG_IMAGE_VERSION; png.width = im.width; png.height = im.height;
  png.format = PNG_FORMAT_RGB; png_alloc_size_t count = 0;
  bool ok = png_image_write_to_memory(&png, nullptr, &count, 0, rgb.data(), 0, nullptr) &&
    count <= MaxPixels * 4 + 1048576 && encoded.resize(size_t(count));
  if (ok) ok = png_image_write_to_memory(&png, encoded.data(), &count, 0, rgb.data(), 0, nullptr) != 0;
  png_image_free(&png);
  if (!ok) { close(dir); return fail(error, "Could not encode the screenshot"); }
  // The temporary name is independent of the selected basename (NAME_MAX).
  char tmp[80]; int fd = -1;
  for (unsigned n = 0; n < 100 && fd < 0; ++n) {
    snprintf(tmp, sizeof(tmp), ".reliefcapture-%ld-%u.tmp", long(getpid()), n);
    fd = openat(dir, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0 && errno != EEXIST) break;
  }
  if (fd < 0) { close(dir); return fail(error, "Could not create the temporary screenshot"); }
  size_t done = 0;
  while (done < size_t(count)) {
    ssize_t n = write(fd, encoded.data() + done, size_t(count) - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { ok = false; break; }
    done += size_t(n);
  }
  if (ok) ok = fchmod(fd, expected.exists ? expected.mode : 0600) == 0 && fsync(fd) == 0;
  if (close(fd) < 0) ok = false;
  if (ok) {
    ok = destination_at(dir, base, current, error) && same(current, expected);
    if (!ok) error.set("The destination changed; choose Save again");
  }
  if (ok && !expected.exists) {
    // linkat is no-replace: a file created during encoding is never overwritten.
    ok = linkat(dir, tmp, dir, base, 0) == 0;
    if (ok) unlinkat(dir, tmp, 0);
  } else if (ok) ok = renameat(dir, tmp, dir, base) == 0;
  if (!ok) {
    unlinkat(dir, tmp, 0); close(dir);
    if (error.empty()) error.set("Could not save the screenshot");
    return false;
  }
  ok = fsync(dir) == 0; close(dir);
  if (!ok) return fail(error, "Screenshot saved, but folder synchronization failed");
  error.clear(); return true;
}
}
