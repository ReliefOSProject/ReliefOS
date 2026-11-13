#include "config.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

namespace rs {
namespace {
bool problem(Text &error, const char *message = nullptr) {
    error.set(message ? message : strerror(errno));
    return false;
}
bool allowed(const char *name) {
    return !strcmp(name, "locale.conf") || !strcmp(name, "desktop.conf");
}
bool read_at(int directory, const char *name, Snapshot &snapshot, Text &error) {
    Snapshot next;
    int fd = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT) {
            snapshot = next;
            return true;
        }
        return problem(error);
    }
    struct stat st{};
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size <= 16384;
    char bytes[16385];
    size_t size = 0;
    while (ok && size < sizeof(bytes)) {
        ssize_t n = read(fd, bytes + size, sizeof(bytes) - size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            ok = false;
            break;
        }
        if (!n)
            break;
        size += n;
    }
    close(fd);
    if (!ok || size > 16384 || memchr(bytes, 0, size))
        return problem(error, "Invalid or oversized configuration file.");
    next.exists = true;
    if (!next.bytes.set(bytes, size))
        return problem(error, "Out of memory.");
    snapshot = next;
    return true;
}
bool write_all(int fd, const Text &bytes) {
    size_t at = 0;
    while (at < bytes.size()) {
        ssize_t n = write(fd, bytes.c_str() + at, bytes.size() - at);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (!n)
                errno = EIO;
            return false;
        }
        at += n;
    }
    return true;
}
int lock_at(int directory, const char *name, Text &error) {
    int fd = openat(directory, name, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct flock guard{};
    guard.l_type = F_WRLCK;
    guard.l_whence = SEEK_SET;
    if (fd >= 0 && fcntl(fd, F_SETLK, &guard) == 0)
        return fd;
    if (fd >= 0)
        close(fd);
    problem(error, "Settings are being saved by another program. Try again.");
    return -1;
}
} // namespace
int Store::open_directory(bool create, Text &error) {
    if (!directory_ || *directory_ != '/') {
        errno = EINVAL;
        problem(error);
        return -1;
    }
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    char *copy = strdup(directory_ + 1);
    if (!copy) {
        if (fd >= 0)
            close(fd);
        problem(error, "Out of memory.");
        return -1;
    }
    char *parts;
    for (char *part = strtok_r(copy, "/", &parts); part && fd >= 0;
         part = strtok_r(nullptr, "/", &parts)) {
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (next < 0 && errno == ENOENT && create &&
            (mkdirat(fd, part, 0700) == 0 || errno == EEXIST))
            next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        int saved = errno;
        close(fd);
        fd = next;
        errno = saved;
    }
    free(copy);
    if (fd < 0)
        problem(error);
    return fd;
}
bool Store::read(const char *name, Snapshot &snapshot, Text &error) {
    if (!allowed(name))
        return problem(error, "Invalid configuration name.");
    int directory = open_directory(false, error);
    if (directory < 0) {
        if (errno == ENOENT) {
            snapshot = Snapshot{};
            error.clear();
            return true;
        }
        return false;
    }
    bool ok = read_at(directory, name, snapshot, error);
    close(directory);
    return ok;
}
int Store::lock_desktop(Text &error) {
    int directory = open_directory(true, error);
    if (directory < 0)
        return -1;
    int lock = lock_at(directory, ".desktop-apply.lock", error);
    close(directory);
    return lock;
}
bool Store::save(const char *name, const Snapshot &expected, const Text &bytes, Text &error,
                 bool *published) {
    if (published)
        *published = false;
    if (!allowed(name) || bytes.size() > 16384)
        return problem(error, "Invalid configuration name or size.");
    int directory = open_directory(true, error);
    if (directory < 0)
        return false;
    int lock = lock_at(directory, ".settings.lock", error);
    bool ok = lock >= 0;
    Snapshot current;
    if (ok)
        ok = read_at(directory, name, current, error);
    if (ok && (current.exists != expected.exists || current.bytes != expected.bytes))
        ok = problem(error, "The settings changed in another program. Reload and try again.");
    char temporary[80]{};
    int output = -1;
    for (unsigned i = 0; ok && output < 0 && i < 100; ++i) {
        snprintf(temporary, sizeof(temporary), ".settings-%ld-%u", static_cast<long>(getpid()), i);
        output = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                        0600);
        if (output < 0 && errno != EEXIST) {
            ok = problem(error);
            break;
        }
    }
    if (ok && output < 0)
        ok = problem(error, "Cannot create a temporary configuration file.");
    if (ok && (fchmod(output, 0600) < 0 || !write_all(output, bytes) || fsync(output) < 0))
        ok = problem(error);
    if (output >= 0 && close(output) < 0 && ok)
        ok = problem(error);
    if (ok && renameat(directory, temporary, directory, name) < 0)
        ok = problem(error);
    if (ok && published)
        *published = true;
    if (*temporary && output >= 0)
        unlinkat(directory, temporary, 0);
    // Once rename succeeds the published bytes are authoritative, including
    // after a directory-sync error. Callers reload before another mutation.
    if (ok && fsync(directory) < 0)
        ok = problem(error);
    if (lock >= 0)
        close(lock);
    close(directory);
    return ok;
}
bool set_timezone(const char *zone, Text &error, const char *etc, const char *zoneinfo,
                  bool *published) {
    if (published)
        *published = false;
    if (!valid_zone(zone))
        return problem(error, "Invalid time zone.");
    Text target;
    if (!target.set(zoneinfo) || !append(target, "/") || !append(target, zone))
        return problem(error, "Out of memory.");
    int file = open(target.c_str(), O_RDONLY | O_CLOEXEC);
    char magic[4];
    struct stat st{};
    bool valid = file >= 0 && fstat(file, &st) == 0 && S_ISREG(st.st_mode) &&
                 read(file, magic, 4) == 4 && !memcmp(magic, "TZif", 4);
    if (file >= 0)
        close(file);
    if (!valid)
        return problem(error, "The selected IANA time zone is not installed.");
    int directory = open(etc, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0)
        return problem(error);
    char name[80];
    snprintf(name, sizeof(name), ".reliefsettings-localtime-%ld", static_cast<long>(getpid()));
    bool created = symlinkat(target.c_str(), directory, name) == 0;
    bool ok = created;
    if (ok)
        ok = renameat(directory, name, directory, "localtime") == 0;
    if (ok && published)
        *published = true;
    if (!ok)
        problem(error);
    if (created)
        unlinkat(directory, name, 0);
    if (ok && fsync(directory) < 0)
        ok = problem(error);
    close(directory);
    return ok;
}
} // namespace rs
