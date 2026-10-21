#include "certificates.h"
#include <errno.h>
#include <fcntl.h>
#include <mbedtls/x509_crt.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ra {
bool safe_ca_bundle_at(int directory_fd, unsigned long expected_uid, Text &error) {
    error.clear();
    struct stat directory, current;
    if (fstat(directory_fd, &directory) != 0 || !S_ISDIR(directory.st_mode) ||
        directory.st_uid != expected_uid || (directory.st_mode & 0022)) {
        error.set("CA directory ownership or permissions are unsafe");
        return false;
    }
    if (fstatat(directory_fd, "ca-certificates.crt", &current, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return true;
        error.set("Cannot inspect the current CA trust path");
        return false;
    }
    if (!S_ISREG(current.st_mode) || current.st_uid != expected_uid ||
        (current.st_mode & 0022) || current.st_size < 0 || current.st_size > 4 * 1024 * 1024) {
        error.set("CA trust file must be an owned regular file, not group/other writable, and at most 4 MiB");
        return false;
    }
    return true;
}

bool relay_certificate_apk_output(int input_fd) {
    char line[4096];
    size_t used = 0;
    bool complete = true, overflow = false;
    auto flush = [&]() {
        line[used] = 0;
        if (strstr(line, "APK unavailable, skipped")) complete = false;
        if (overflow || strstr(line, "://")) {
            fputs("APK repository diagnostic redacted; check configured sources and TLS.\n", stdout);
        } else if (used) fwrite(line, 1, used, stdout);
        if (overflow) complete = false;
        fflush(stdout);
        used = 0;
        overflow = false;
    };
    char bytes[2048];
    for (;;) {
        const ssize_t count = read(input_fd, bytes, sizeof(bytes));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { complete = false; break; }
        if (!count) break;
        for (ssize_t i = 0; i < count; ++i) {
            if (used + 1 < sizeof(line)) line[used++] = bytes[i];
            else overflow = true;
            if (bytes[i] == '\n') flush();
        }
    }
    if (used || overflow) flush();
    if (!complete) fputs("APK skipped an unavailable package or its output was incomplete; certificate repair failed.\n", stdout);
    return complete;
}

namespace {
struct File {
    int fd = -1;
    ~File() { if (fd >= 0) close(fd); }
};
struct Temporary : File {
    int directory;
    char name[96] = {};
    bool keep = false;
    explicit Temporary(int dir) : directory(dir) {}
    ~Temporary() { if (*name && !keep) unlinkat(directory, name, 0); }
    bool open(const char *kind) {
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            char candidate[96];
            snprintf(candidate, sizeof(candidate), ".reliefapk-ca-%s-%ld-%u", kind,
                static_cast<long>(getpid()), attempt);
            fd = openat(directory, candidate, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (fd >= 0) { memcpy(name, candidate, strlen(candidate) + 1); return true; }
            if (errno != EEXIST) return false;
        }
        return false;
    }
};
bool failed(Text &error, const char *message) { error.set(message); return false; }
bool write_all(int fd, const char *bytes, size_t size) {
    size_t used = 0;
    while (used < size) {
        const ssize_t count = write(fd, bytes + used, size - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        used += static_cast<size_t>(count);
    }
    return true;
}
bool same_file(const struct stat &a, const struct stat &b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
        a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
        a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
}

bool restore_ca_bundle_at(int directory_fd, const Text &bundle, Text &backup_name, Text &error) {
    backup_name.clear();
    error.clear();
    struct stat directory, before, current;
    if (fstat(directory_fd, &directory) != 0 || !S_ISDIR(directory.st_mode) ||
        directory.st_uid != geteuid() || (directory.st_mode & 0022))
        return failed(error, "CA recovery directory ownership or permissions are unsafe");
    if (bundle.empty() || bundle.size() > 4u * 1024u * 1024u)
        return failed(error, "OS recovery CA bundle is empty or too large");
    mbedtls_x509_crt roots;
    mbedtls_x509_crt_init(&roots);
    const int parsed = mbedtls_x509_crt_parse(&roots,
        reinterpret_cast<const unsigned char *>(bundle.c_str()), bundle.size() + 1);
    const bool usable = parsed >= 0 && roots.raw.p && roots.version > 0;
    mbedtls_x509_crt_free(&roots);
    if (!usable) return failed(error, "OS recovery CA bundle has no usable certificates");

    constexpr const char *target = "ca-certificates.crt";
    const bool exists = fstatat(directory_fd, target, &before, AT_SYMLINK_NOFOLLOW) == 0;
    if (!exists && errno != ENOENT) return failed(error, "Cannot inspect the current CA bundle");
    if (exists && (!S_ISREG(before.st_mode) || before.st_uid != geteuid() ||
            (before.st_mode & 0022) || before.st_size < 0 || before.st_size > 4 * 1024 * 1024))
        return failed(error, "Existing CA bundle must be an owned regular file of at most 4 MiB");
    Temporary replacement(directory_fd), backup(directory_fd);
    if (!replacement.open("new") || !write_all(replacement.fd, bundle.c_str(), bundle.size()) ||
        fchmod(replacement.fd, 0644) != 0 || fsync(replacement.fd) != 0)
        return failed(error, "Could not prepare the complete OS recovery CA bundle");
    if (exists) {
        File original;
        original.fd = openat(directory_fd, target, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        if (original.fd < 0 || fstat(original.fd, &current) != 0 || !same_file(before, current) ||
            !backup.open("backup")) return failed(error, "Could not preserve the previous CA bundle");
        char bytes[4096];
        size_t used = 0;
        while (used < static_cast<size_t>(before.st_size)) {
            size_t count = static_cast<size_t>(before.st_size) - used;
            if (count > sizeof(bytes)) count = sizeof(bytes);
            const ssize_t got = read(original.fd, bytes, count);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0 || !write_all(backup.fd, bytes, static_cast<size_t>(got)))
                return failed(error, "Could not preserve the complete previous CA bundle");
            used += static_cast<size_t>(got);
        }
        if (fsync(backup.fd) != 0 || !backup_name.set(backup.name))
            return failed(error, "Could not sync the previous CA bundle backup");
    }
    if (fsync(directory_fd) != 0) return failed(error, "Could not sync CA recovery files");
    const bool still_exists = fstatat(directory_fd, target, &current, AT_SYMLINK_NOFOLLOW) == 0;
    if ((exists && (!still_exists || !same_file(before, current))) ||
        (!exists && (still_exists || errno != ENOENT)))
        return failed(error, "CA bundle changed during recovery; retry after checking the system state");
    if (renameat(directory_fd, replacement.name, directory_fd, target) != 0)
        return failed(error, "Could not atomically install the OS recovery CA bundle");
    replacement.keep = true;
    backup.keep = true;
    if (fsync(directory_fd) != 0)
        return failed(error, "OS CA bundle was restored, but directory synchronization failed");
    return true;
}
}
