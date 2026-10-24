#include "config.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
namespace oc {
namespace {
bool fail(Text &e, const char *what) {
  e.set(what);
  return false;
}
bool trusted(const struct stat &s) {
  return s.st_uid == geteuid() && !(s.st_mode & 0022);
}
int directory(int root, const char *name, char filename[192], bool backup,
              Text &error) {
  if (root < 0 || (!identifier(name) && strcmp(name, "@global"))) {
    fail(error, "Invalid configuration path.");
    return -1;
  }
  snprintf(filename, 192, "%s%s", !strcmp(name, "@global") ? "rc.conf" : name,
           backup ? ".openrc-controller.bak" : "");
  int dir = !strcmp(name, "@global")
                ? dup(root)
                : openat(root, "conf.d",
                         O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
  struct stat s;
  if (dir < 0 || fstat(dir, &s) < 0 || !trusted(s)) {
    if (dir >= 0)
      close(dir);
    fail(error, "Configuration directory is missing or unsafe.");
    return -1;
  }
  return dir;
}
bool load(int dir, const char *name, Snapshot &out, Text &error,
          struct stat *metadata = nullptr) {
  int fd = openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) {
    if (errno == ENOENT) {
      out = Snapshot{};
      return true;
    }
    return fail(error,
                "Cannot open configuration (symbolic links are not allowed).");
  }
  struct stat s;
  bool ok = fstat(fd, &s) == 0 && S_ISREG(s.st_mode) && trusted(s) &&
            s.st_size >= 0 && s.st_size <= 256 * 1024;
  Text contents;
  char buffer[4096];
  while (ok) {
    ssize_t n = read(fd, buffer, sizeof(buffer));
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0) {
      ok = false;
      break;
    }
    if (!n)
      break;
    ok = contents.size() + n <= 256 * 1024 && !memchr(buffer, 0, n) &&
         append(contents, buffer, n);
  }
  close(fd);
  if (!ok)
    return fail(
        error,
        "Configuration must be a safe, readable text file up to 256 KiB.");
  out.bytes = contents;
  out.exists = true;
  if (metadata)
    *metadata = s;
  return true;
}
bool put(int dir, const char *name, const Text &bytes, const struct stat &mode,
         Text &error) {
  static unsigned sequence;
  char tmp[96];
  snprintf(tmp, sizeof(tmp), ".openrc-controller-%ld-%u", (long)getpid(),
           ++sequence);
  int fd = openat(dir, tmp,
                  O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0)
    return fail(error, "Cannot create temporary configuration file.");
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
  if (ok && (fchown(fd, mode.st_uid, mode.st_gid) < 0 ||
             fchmod(fd, mode.st_mode & 0777) < 0 || fsync(fd) < 0))
    ok = false;
  if (close(fd) < 0)
    ok = false;
  if (ok && renameat(dir, tmp, dir, name) < 0)
    ok = false;
  if (!ok) {
    unlinkat(dir, tmp, 0);
    return fail(error, "Configuration write or atomic replacement failed.");
  }
  if (fsync(dir) < 0)
    return fail(error, "Configuration replaced, but directory synchronization "
                       "failed. Check disk status.");
  return true;
}
} // namespace
ConfigStore::ConfigStore(const char *etc) {
  root_ = open(etc, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
  struct stat s;
  if (root_ >= 0 && (fstat(root_, &s) < 0 || !trusted(s))) {
    close(root_);
    root_ = -1;
  }
}
ConfigStore::~ConfigStore() {
  if (root_ >= 0)
    close(root_);
}
bool ConfigStore::read(const char *name, Snapshot &out, Text &error,
                       bool backup) {
  char filename[192];
  int dir = directory(root_, name, filename, backup, error);
  if (dir < 0)
    return false;
  bool ok = load(dir, filename, out, error);
  close(dir);
  return ok;
}
bool ConfigStore::save(const char *name, const Snapshot &expected,
                       const Text &bytes, Text &error) {
  return save_with_permissions(name, expected, bytes, error, nullptr);
}
bool ConfigStore::save_with_permissions(const char *name,
                                        const Snapshot &expected,
                                        const Text &bytes, Text &error,
                                        const struct stat *permissions) {
  if (bytes.size() > 256 * 1024 || memchr(bytes.c_str(), 0, bytes.size()))
    return fail(error, "Configuration is too large or contains NUL bytes.");
  char filename[192];
  int dir = directory(root_, name, filename, false, error);
  if (dir < 0)
    return false;
  // Use an ordinary lock file: ReliefNT's flock currently accepts regular
  // files, while Linux also accepts directory descriptors.
  int lock =
      openat(dir, ".openrc-controller.lock",
             O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
  struct stat lock_info;
  if (lock < 0 || fstat(lock, &lock_info) < 0 || !S_ISREG(lock_info.st_mode) ||
      !trusted(lock_info) || lock_info.st_nlink != 1 ||
      flock(lock, LOCK_EX | LOCK_NB) < 0) {
    if (lock >= 0)
      close(lock);
    close(dir);
    return fail(error, "Configuration lock is unsafe or another editor is "
                       "saving. Retry after it finishes.");
  }
  Snapshot current;
  struct stat mode{};
  mode.st_uid = geteuid();
  mode.st_gid = getegid();
  mode.st_mode = 0644;
  if (permissions)
    mode = *permissions;
  bool ok = load(dir, filename, current, error, &mode);
  if (ok &&
      (current.exists != expected.exists || current.bytes != expected.bytes))
    ok = fail(error, "Configuration changed externally. Reload before saving.");
  if (ok && current.exists) {
    char bak[224];
    snprintf(bak, sizeof(bak), "%s.openrc-controller.bak", filename);
    struct stat previous;
    if (fstatat(dir, bak, &previous, AT_SYMLINK_NOFOLLOW) == 0 &&
        (!S_ISREG(previous.st_mode) || !trusted(previous)))
      ok = fail(error, "Backup path is unsafe.");
    if (ok)
      ok = put(dir, bak, current.bytes, mode, error);
  }
  if (ok) {
    Snapshot check;
    ok = load(dir, filename, check, error) && check.exists == current.exists &&
         check.bytes == current.bytes;
    if (!ok)
      fail(error, "Configuration changed while saving. Reload before saving.");
  }
  if (ok)
    ok = put(dir, filename, bytes, mode, error);
  flock(lock, LOCK_UN);
  close(lock);
  close(dir);
  return ok;
}
bool ConfigStore::restore(const char *name, const Snapshot &expected,
                          Text &error) {
  char filename[192];
  int dir = directory(root_, name, filename, true, error);
  if (dir < 0)
    return false;
  Snapshot backup;
  struct stat permissions{};
  bool ok = load(dir, filename, backup, error, &permissions);
  close(dir);
  if (!ok)
    return false;
  if (!backup.exists)
    return fail(error, "No configuration backup is available.");
  return save_with_permissions(name, expected, backup.bytes, error,
                               &permissions);
}
} // namespace oc
