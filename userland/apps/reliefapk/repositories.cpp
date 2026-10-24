#define _POSIX_C_SOURCE 200809L
#include "repositories.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
namespace ra {
namespace {
constexpr size_t Limit = 128 * 1024;
struct Bytes {
    char *data = nullptr;
    size_t size = 0, capacity = 0;
    ~Bytes() { free(data); }
    bool append(const char *s, size_t n) {
        if (n > Limit - size)
            return false;
        if (size + n + 1 > capacity) {
            size_t c = (size + n + 1) * 2;
            char *p = static_cast<char *>(realloc(data, c));
            if (!p)
                return false;
            data = p;
            capacity = c;
        }
        memcpy(data + size, s, n);
        size += n;
        data[size] = 0;
        return true;
    }
    bool append(const char *s) { return append(s, strlen(s)); }
};
bool error_text(Text &error, const char *context) {
    char s[512];
    snprintf(s, sizeof(s), "%s: %s", context, strerror(errno));
    error.set(s);
    return false;
}
bool read_file(const char *path, Text &out, struct stat *metadata, Text &error) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return error_text(error, "Open repositories");
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 &&
              st.st_size <= static_cast<off_t>(Limit);
    Bytes bytes;
    char block[4096];
    while (ok) {
        ssize_t n = read(fd, block, sizeof(block));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            ok = false;
            break;
        }
        if (!n)
            break;
        if (memchr(block, 0, static_cast<size_t>(n)) || !bytes.append(block, static_cast<size_t>(n))) {
            ok = false;
            errno = EFBIG;
            break;
        }
    }
    int saved = errno;
    close(fd);
    errno = saved;
    if (!ok)
        return error_text(error, "Read repositories");
    if (!out.set(bytes.data ? bytes.data : "", bytes.size)) {
        errno = ENOMEM;
        return error_text(error, "Read repositories");
    }
    if (metadata)
        *metadata = st;
    return true;
}
bool write_all(int fd, const char *s, size_t n) {
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return false;
        s += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}
bool make_file(const char *target, const char *contents, const struct stat &st, Text &temporary,
               Text &error) {
    size_t n = strlen(target);
    char *name = static_cast<char *>(malloc(n + 32));
    if (!name) {
        errno = ENOMEM;
        return error_text(error, "Save repositories");
    }
    snprintf(name, n + 32, "%s.reliefapk-XXXXXX", target);
    int fd = mkstemp(name);
    bool ok = fd >= 0;
    if (ok && geteuid() == 0)
        ok = fchown(fd, st.st_uid, st.st_gid) == 0;
    if (ok)
        ok =
            fchmod(fd, st.st_mode & 0777) == 0 && write_all(fd, contents, strlen(contents)) && fsync(fd) == 0;
    int saved = errno;
    if (fd >= 0 && close(fd) < 0 && ok) {
        ok = false;
        saved = errno;
    }
    if (ok)
        ok = temporary.set(name);
    if (!ok)
        unlink(name);
    free(name);
    errno = saved;
    return ok ? true : error_text(error, "Save repositories");
}
} // namespace
bool valid_repository(const char *tag, const char *url) {
    if (!tag || !url || !*url || strlen(tag) > 128 || strlen(url) > 4096)
        return false;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(tag); *p; ++p)
        if (!isalnum(*p) && *p != '_' && *p != '-' && *p != '.')
            return false;
    if (strncmp(url, "https://", 8) && strncmp(url, "http://", 7) && strncmp(url, "file://", 7) &&
        url[0] != '/')
        return false;
    if (!strncmp(url, "https://", 8) && !url[8])
        return false;
    if (!strncmp(url, "http://", 7) && !url[7])
        return false;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(url); *p; ++p)
        if (*p <= 32 || *p == 127 || *p == '#')
            return false;
    return true;
}
bool secure_cache_directory(const char *path, Text &error) {
    if (!path || path[0] != '/' || strlen(path) > 4096) {
        error.set("The administrator cache must be an absolute directory.");
        return false;
    }
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return error_text(error, "Open cache directory");
    bool ok = true;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_uid != 0 || (st.st_mode & 0022))
        ok = false;
    size_t at = 1;
    while (ok && path[at]) {
        while (path[at] == '/')
            ++at;
        if (!path[at])
            break;
        size_t end = at;
        while (path[end] && path[end] != '/')
            ++end;
        size_t n = end - at;
        char part[256];
        if (n >= sizeof(part) || (n == 2 && path[at] == '.' && path[at + 1] == '.')) {
            ok = false;
            break;
        }
        memcpy(part, path + at, n);
        part[n] = 0;
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) {
            ok = false;
            break;
        }
        close(fd);
        fd = next;
        if (fstat(fd, &st) < 0 || st.st_uid != 0 || (st.st_mode & 0022)) {
            ok = false;
            break;
        }
        at = end;
    }
    close(fd);
    if (!ok)
        error.set("The administrator cache and its parent directories must exist, be root-owned, and not be "
                  "writable by other users.");
    else
        error.clear();
    return ok;
}
bool standard_repository_path(const char *p) {
    if (!p)
        return false;
    if (!strcmp(p, "/etc/apk/repositories"))
        return true;
    const char *prefix = "/etc/apk/repositories.d/";
    size_t n = strlen(prefix);
    if (strncmp(p, prefix, n))
        return false;
    const char *name = p + n;
    size_t len = strlen(name);
    if (len < 6 || len > 240 || strcmp(name + len - 5, ".list") || strstr(name, ".."))
        return false;
    for (; *name; ++name)
        if (!isalnum(static_cast<unsigned char>(*name)) && *name != '_' && *name != '-' && *name != '.')
            return false;
    return true;
}
bool Repositories::parse(const char *bytes, size_t length) {
    if ((!bytes && length) || length > Limit || (length && memchr(bytes, 0, length))) {
        error.set("Invalid repository file");
        return false;
    }
    Array<RepositoryLine> next;
    for (size_t at = 0; at < length;) {
        size_t end = at;
        while (end < length && bytes[end] != '\n')
            ++end;
        if (end < length)
            ++end;
        RepositoryLine line;
        if (!line.raw.set(bytes + at, end - at)) {
            error.set("Out of memory");
            return false;
        }
        size_t start = at, stop = end;
        while (stop > start && (bytes[stop - 1] == '\n' || bytes[stop - 1] == '\r' ||
                                bytes[stop - 1] == ' ' || bytes[stop - 1] == '\t'))
            --stop;
        while (start < stop && (bytes[start] == ' ' || bytes[start] == '\t'))
            ++start;
        line.enabled = true;
        if (start < stop && bytes[start] == '#') {
            line.enabled = false;
            ++start;
            while (start < stop && (bytes[start] == ' ' || bytes[start] == '\t'))
                ++start;
        }
        size_t token = start;
        while (token < stop && bytes[token] != ' ' && bytes[token] != '\t')
            ++token;
        if (start < stop && bytes[start] == '@') {
            line.tag.set(bytes + start + 1, token - start - 1);
            start = token;
            while (start < stop && (bytes[start] == ' ' || bytes[start] == '\t'))
                ++start;
        }
        if (!line.url.set(bytes + start, stop - start)) {
            error.set("Out of memory");
            return false;
        }
        line.recognized = valid_repository(line.tag.c_str(), line.url.c_str());
        if (!next.push(static_cast<RepositoryLine &&>(line))) {
            error.set("Out of memory");
            return false;
        }
        at = end;
    }
    Text saved;
    if (!saved.set(bytes ? bytes : "", length)) {
        error.set("Out of memory");
        return false;
    }
    original = static_cast<Text &&>(saved);
    lines.swap(next);
    error.clear();
    return true;
}
bool Repositories::load(const char *filename) {
    Text data;
    if (!read_file(filename, data, nullptr, error))
        return false;
    if (!parse(data.c_str(), data.size()))
        return false;
    return path.set(filename);
}
bool Repositories::replace(size_t index, const char *tag, const char *url, bool enabled) {
    if (index >= lines.size() || !valid_repository(tag, url)) {
        error.set("Invalid repository address or tag");
        return false;
    }
    RepositoryLine replacement;
    replacement.recognized = true;
    replacement.changed = true;
    replacement.enabled = enabled;
    if (!replacement.tag.set(tag) || !replacement.url.set(url)) {
        error.set("Out of memory");
        return false;
    }
    lines[index] = static_cast<RepositoryLine &&>(replacement);
    return true;
}
bool Repositories::add(const char *tag, const char *url, bool enabled) {
    if (!valid_repository(tag, url)) {
        error.set("Invalid repository address or tag");
        return false;
    }
    RepositoryLine r;
    r.recognized = true;
    r.changed = true;
    r.enabled = enabled;
    if (!r.tag.set(tag) || !r.url.set(url) || !lines.push(static_cast<RepositoryLine &&>(r))) {
        error.set("Out of memory");
        return false;
    }
    return true;
}
bool Repositories::serialize(Text &result) const {
    Bytes bytes;
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto &r = lines[i];
        if (bytes.size && bytes.data[bytes.size - 1] != '\n' && !bytes.append("\n"))
            return false;
        if (!r.changed) {
            if (!bytes.append(r.raw.c_str(), r.raw.size()))
                return false;
            continue;
        }
        if (!r.enabled && !bytes.append("# "))
            return false;
        if (!r.tag.empty() && (!bytes.append("@") || !bytes.append(r.tag.c_str()) || !bytes.append(" ")))
            return false;
        if (!bytes.append(r.url.c_str()) || !bytes.append("\n"))
            return false;
    }
    return result.set(bytes.data ? bytes.data : "", bytes.size);
}
bool save_repository_file(const char *path, const char *expected, const char *contents, Text &error) {
    if (!path || !expected || !contents || strlen(expected) > Limit || strlen(contents) > Limit) {
        error.set("Repository configuration is too large");
        return false;
    }
    Repositories parsed;
    if (!parsed.parse(contents, strlen(contents))) {
        error = parsed.error;
        return false;
    }
    Text current;
    struct stat before;
    if (!read_file(path, current, &before, error))
        return false;
    if (strcmp(current.c_str(), expected)) {
        error.set("Repositories changed outside ReliefAPK. Reload before saving.");
        return false;
    }
    Text replacement, backup_file;
    char backup[4096];
    if (snprintf(backup, sizeof(backup), "%s.reliefapk.bak", path) >= static_cast<int>(sizeof(backup))) {
        error.set("Repository path too long");
        return false;
    }
    if (!make_file(path, contents, before, replacement, error))
        return false;
    if (!make_file(backup, current.c_str(), before, backup_file, error)) {
        unlink(replacement.c_str());
        return false;
    }
    Text latest;
    struct stat after;
    if (!read_file(path, latest, &after, error) || before.st_dev != after.st_dev ||
        before.st_ino != after.st_ino || strcmp(latest.c_str(), expected)) {
        error.set("Repositories changed outside ReliefAPK. Reload before saving.");
        unlink(replacement.c_str());
        unlink(backup_file.c_str());
        return false;
    }
    if (rename(backup_file.c_str(), backup) < 0) {
        int saved = errno;
        unlink(replacement.c_str());
        unlink(backup_file.c_str());
        errno = saved;
        return error_text(error, "Save backup");
    }
    if (rename(replacement.c_str(), path) < 0) {
        int saved = errno;
        unlink(replacement.c_str());
        errno = saved;
        return error_text(error, "Replace repositories");
    }
    char parent[4096];
    if (strlen(path) >= sizeof(parent)) {
        error.set("Repository path too long");
        return false;
    }
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    if (slash) {
        if (slash == parent)
            slash[1] = 0;
        else
            *slash = 0;
    } else
        strcpy(parent, ".");
    int dir = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0)
        return error_text(error, "Sync repositories directory");
    bool ok = fsync(dir) == 0;
    int saved = errno;
    close(dir);
    errno = saved;
    if (!ok)
        return error_text(error, "Sync repositories directory");
    error.clear();
    return true;
}
} // namespace ra
