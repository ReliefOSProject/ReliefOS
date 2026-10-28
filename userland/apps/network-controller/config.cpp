#include "config.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
namespace nc {
namespace {
constexpr const char *Absent = "NETWORK-CONTROLLER-ABSENT\n";
bool failure(Text &error, const char *what) {
  char b[512];
  snprintf(b, sizeof(b), "%s: %s", what, strerror(errno));
  error.set(b);
  return false;
}
bool safe(const struct stat &st) {
  return S_ISREG(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & 0022);
}
int directory(const char *path, Text &error) {
  int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  struct stat st{};
  if (fd < 0) {
    failure(error, "Cannot open network configuration directory");
    return -1;
  }
  if (fstat(fd, &st) < 0 || st.st_uid != geteuid() || (st.st_mode & 0022)) {
    close(fd);
    errno = EPERM;
    failure(error, "Unsafe configuration directory");
    return -1;
  }
  return fd;
}
bool load(int dir, const char *name, Snapshot &out, Text &error) {
  Snapshot next;
  int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) {
      out = next;
      return true;
    }
    return failure(error, "Cannot read network configuration");
  }
  struct stat st{};
  bool ok = fstat(fd, &st) == 0 && safe(st) && st.st_size >= 0 &&
            static_cast<uint64_t>(st.st_size) <= MaxConfig;
  if (!ok) {
    close(fd);
    errno = EPERM;
    return failure(error, "Unsafe network configuration file");
  }
  char b[4096];
  while (ok) {
    ssize_t n = read(fd, b, sizeof(b));
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0) {
      ok = false;
      break;
    }
    if (!n)
      break;
    if (next.bytes.size() + n > MaxConfig || memchr(b, 0, n) ||
        !append(next.bytes, b, n)) {
      errno = EFBIG;
      ok = false;
      break;
    }
  }
  int saved = errno;
  if (close(fd) < 0 && ok) {
    saved = errno;
    ok = false;
  }
  errno = saved;
  if (!ok)
    return failure(error, "Cannot read network configuration");
  next.exists = true;
  out = next;
  return true;
}
bool atomic(int dir, const char *name, const Text &bytes, Text &error) {
  Snapshot existing;
  if (!load(dir, name, existing, error))
    return false;
  static unsigned serial;
  char temporary[96];
  int fd = -1;
  for (unsigned retry = 0; retry < 16; ++retry) {
    snprintf(temporary, sizeof(temporary), ".network-controller-%ld-%u",
             static_cast<long>(getpid()), ++serial);
    fd = openat(dir, temporary,
                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0 || errno != EEXIST)
      break;
  }
  if (fd < 0)
    return failure(error, "Cannot create network configuration temporary file");
  bool ok = true;
  size_t at = 0;
  while (at < bytes.size()) {
    ssize_t n = write(fd, bytes.c_str() + at, bytes.size() - at);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      ok = false;
      break;
    }
    at += n;
  }
  if (ok && (fchmod(fd, 0644) < 0 || fsync(fd) < 0))
    ok = false;
  int saved = errno;
  if (close(fd) < 0 && ok) {
    ok = false;
    saved = errno;
  }
  errno = saved;
  if (ok && renameat(dir, temporary, dir, name) < 0)
    ok = false;
  if (!ok) {
    saved = errno;
    unlinkat(dir, temporary, 0);
    errno = saved;
    return failure(error, "Cannot save network configuration");
  }
  if (fsync(dir) < 0)
    return failure(
        error, "Configuration replaced but directory synchronization failed");
  return true;
}
} // namespace
bool ConfigStore::read(Snapshot &out, Text &error) {
  int dir = directory(directory_, error);
  if (dir < 0)
    return false;
  bool ok = load(dir, "network.conf", out, error);
  close(dir);
  return ok;
}
bool ConfigStore::replace(const Snapshot &expected, const Snapshot &next,
                          Text &error) {
  int dir = directory(directory_, error);
  if (dir < 0)
    return false;
  int lock = openat(dir, ".network-controller.lock",
                    O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
  struct stat st{};
  bool ok = lock >= 0 && fstat(lock, &st) == 0 && safe(st) &&
            flock(lock, LOCK_EX | LOCK_NB) == 0;
  if (!ok) {
    if (lock >= 0)
      close(lock);
    close(dir);
    return failure(error, "Network configuration is locked or unsafe");
  }
  Snapshot current;
  ok = load(dir, "network.conf", current, error);
  if (ok && (current.exists != expected.exists ||
             !(current.bytes == expected.bytes))) {
    error.set(
        "Configuration changed outside this window. Reload it before saving.");
    ok = false;
  }
  if (ok) {
    Text backup;
    if (current.exists)
      backup = current.bytes;
    else
      backup.set(Absent);
    ok = atomic(dir, "network.conf.bak", backup, error);
  }
  if (ok) {
    if (next.exists)
      ok = atomic(dir, "network.conf", next.bytes, error);
    else if (unlinkat(dir, "network.conf", 0) < 0 && errno != ENOENT)
      ok = failure(error, "Cannot restore absent network profile");
    else if (fsync(dir) < 0)
      ok = failure(error, "Cannot synchronize restored network profile");
  }
  flock(lock, LOCK_UN);
  close(lock);
  close(dir);
  return ok;
}
bool ConfigStore::save(const Snapshot &expected, const Text &bytes,
                       Text &error) {
  Config c;
  Text canonical;
  if (!parse_config(bytes, c, error) || !serialize_config(c, canonical, error))
    return false;
  Snapshot next;
  next.exists = true;
  next.bytes = canonical;
  return replace(expected, next, error);
}
bool ConfigStore::restore(const Snapshot &expected, Text &error) {
  int dir = directory(directory_, error);
  if (dir < 0)
    return false;
  Snapshot next;
  bool ok = load(dir, "network.conf.bak", next, error);
  close(dir);
  if (!ok)
    return false;
  if (!next.exists) {
    error.set("No previous network configuration backup is available.");
    return false;
  }
  if (next.bytes == Absent) {
    next.exists = false;
    next.bytes.clear();
  } else {
    Config c;
    if (!parse_config(next.bytes, c, error))
      return false;
  }
  return replace(expected, next, error);
}
} // namespace nc
