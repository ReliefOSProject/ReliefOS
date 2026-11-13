#include "history.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace clip {
bool History::add(Entry &&entry)
{
    if (entry.formats.empty() || entry.size() > MaxEntryBytes) return false;
    if (!entries.empty() && same(entry, entries[0])) return true;
    if (!entries.reserve(entries.size() + 1)) return false;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (same(entry, entries[i])) { entries.erase(i); break; }
    }
    if (!entry.created) entry.created = static_cast<uint64_t>(time(nullptr));
    entry.id = next_++;
    entries.push_reserved(static_cast<Entry &&>(entry));
    for (size_t i = entries.size() - 1; i > 0; --i) {
        Entry swap = static_cast<Entry &&>(entries[i]);
        entries[i] = static_cast<Entry &&>(entries[i - 1]);
        entries[i - 1] = static_cast<Entry &&>(swap);
    }
    size_t bytes = 0, keep = 0;
    while (keep < entries.size() && keep < MaxEntries &&
           entries[keep].size() <= MaxHistoryBytes - bytes) bytes += entries[keep++].size();
    entries.resize(keep);
    ++revision;
    return true;
}

void History::remove(size_t index)
{
    if (index < entries.size()) { entries.erase(index); ++revision; }
}

void History::clear()
{
    entries.clear();
    ++revision;
}

namespace {
constexpr size_t MaxFileBytes = MaxHistoryBytes + 5 * 1024 * 1024;

int directory_fd(const char *directory, bool create)
{
    if (!directory || directory[0] != '/' || strlen(directory) >= 4096) return -EINVAL;
    if (create) {
        char path[4096];
        strcpy(path, directory);
        for (char *p = path + 1;; ++p) {
            if (*p && *p != '/') continue;
            char saved = *p;
            *p = 0;
            if (mkdir(path, 0700) < 0 && errno != EEXIST) return -errno;
            *p = saved;
            if (!saved) break;
        }
    }
    int fd = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -errno;
    struct stat st{};
    if (fstat(fd, &st) < 0 || st.st_uid != getuid() || (st.st_mode & 0077)) {
        close(fd);
        return -EACCES;
    }
    return fd;
}

int read_file(const char *directory, const char *name, Text &data, size_t limit)
{
    int dir = directory_fd(directory, false);
    if (dir < 0) return dir;
    int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    int error = errno;
    close(dir);
    if (fd < 0) return -error;
    struct stat st{};
    int result = 0;
    if (fstat(fd, &st) < 0) result = -errno;
    else if (!S_ISREG(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 0077) || st.st_nlink != 1)
        result = -EACCES;
    else if (st.st_size < 0 || uint64_t(st.st_size) > limit) result = -EFBIG;
    Array<char> bytes;
    if (!result && !bytes.resize(size_t(st.st_size))) result = -ENOMEM;
    size_t used = 0;
    while (!result && used < bytes.size()) {
        ssize_t count = read(fd, bytes.data() + used, bytes.size() - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { result = count ? -errno : -EIO; break; }
        used += size_t(count);
    }
    close(fd);
    if (!result && !data.set(bytes.data(), bytes.size())) result = -ENOMEM;
    return result;
}

int write_all(int fd, const void *bytes, size_t size)
{
    const char *p = static_cast<const char *>(bytes);
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return n ? -errno : -EIO;
        p += n;
        size -= size_t(n);
    }
    return 0;
}

int atomic_write(const char *directory, const char *name, const Text &data)
{
    int dir = directory_fd(directory, true);
    if (dir < 0) return dir;
    char temporary[80];
    int fd = -1;
    for (unsigned i = 0; i < 100; ++i) {
        snprintf(temporary, sizeof(temporary), ".clipboard-%ld-%u.tmp", long(getpid()), i);
        fd = openat(dir, temporary, O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) { int result = -errno; close(dir); return result; }
    int result = write_all(fd, data.c_str(), data.size());
    if (!result && fsync(fd) < 0) result = -errno;
    if (close(fd) < 0 && !result) result = -errno;
    if (!result && renameat(dir, temporary, dir, name) < 0) result = -errno;
    if (!result && fsync(dir) < 0) result = -errno;
    if (result) unlinkat(dir, temporary, 0);
    close(dir);
    return result;
}

bool put(Array<char> &bytes, const void *data, size_t size)
{
    size_t offset = bytes.size();
    if (offset > MaxFileBytes || size > MaxFileBytes - offset || !bytes.resize(offset + size)) return false;
    if (size) memcpy(bytes.data() + offset, data, size);
    return true;
}

bool number(Array<char> &bytes, uint64_t value, size_t width)
{
    char encoded[8];
    for (size_t i = 0; i < width; ++i) encoded[i] = char(value >> (i * 8));
    return put(bytes, encoded, width);
}

class Reader {
    const char *data_;
    size_t left_;
public:
    explicit Reader(const Text &bytes) : data_(bytes.c_str()), left_(bytes.size()) {}
    size_t left() const { return left_; }
    bool number(uint64_t &value, size_t width) {
        if (left_ < width) return false;
        value = 0;
        for (size_t i = 0; i < width; ++i) value |= uint64_t(static_cast<unsigned char>(data_[i])) << (i * 8);
        data_ += width;
        left_ -= width;
        return true;
    }
    bool text(Text &out, size_t size, bool atom = false) {
        if (size > left_ || (atom && (!size || size > 255 || memchr(data_, 0, size))) || !out.set(data_, size))
            return false;
        data_ += size;
        left_ -= size;
        return true;
    }
};

bool state_path(const char *variable, const char *fallback, Text &out)
{
    const char *base = getenv(variable);
    char path[4096];
    int n;
    if (base && base[0] == '/') n = snprintf(path, sizeof(path), "%s/reliefclipboard", base);
    else {
        const char *home = getenv("HOME");
        if (!home || home[0] != '/') return false;
        n = snprintf(path, sizeof(path), "%s/%s/reliefclipboard", home, fallback);
    }
    return n > 0 && size_t(n) < sizeof(path) && out.set(path);
}
}

bool StatePaths::initialize()
{
    return state_path("XDG_STATE_HOME", ".local/state", state) &&
        state_path("XDG_CONFIG_HOME", ".config", config);
}

int save_history(const char *directory, const History &history)
{
    Array<char> bytes;
    if (!put(bytes, "RCLIP1\r\n", 8) || !number(bytes, history.entries.size(), 4)) return -ENOMEM;
    for (const Entry &entry : history.entries) {
        if (!number(bytes, entry.created, 8) || !number(bytes, entry.formats.size(), 4)) return -ENOMEM;
        for (const Payload &p : entry.formats) {
            if (!number(bytes, p.target.size(), 4) || !number(bytes, p.type.size(), 4) ||
                !number(bytes, p.bytes.size(), 8) || !put(bytes, p.target.c_str(), p.target.size()) ||
                !put(bytes, p.type.c_str(), p.type.size()) || !put(bytes, p.bytes.c_str(), p.bytes.size()))
                return -ENOMEM;
        }
    }
    Text encoded;
    if (!encoded.set(bytes.data(), bytes.size())) return -ENOMEM;
    return atomic_write(directory, "history", encoded);
}

int load_history(const char *directory, History &out)
{
    Text bytes;
    int result = read_file(directory, "history", bytes, MaxFileBytes);
    if (result) return result;
    if (bytes.size() < 12 || memcmp(bytes.c_str(), "RCLIP1\r\n", 8)) return -EINVAL;
    Reader reader(bytes);
    Text magic;
    uint64_t count;
    if (!reader.text(magic, 8) || !reader.number(count, 4) || count > MaxEntries) return -EINVAL;
    Array<Entry> saved;
    size_t total = 0;
    for (uint64_t i = 0; i < count; ++i) {
        Entry entry;
        uint64_t formats;
        if (!reader.number(entry.created, 8) || !reader.number(formats, 4) || !formats || formats > MaxFormats)
            return -EINVAL;
        for (uint64_t j = 0; j < formats; ++j) {
            uint64_t target, type, size;
            Payload p;
            if (!reader.number(target, 4) || !reader.number(type, 4) || !reader.number(size, 8) ||
                size > MaxEntryBytes - entry.size() || size > MaxHistoryBytes - total ||
                !reader.text(p.target, size_t(target), true) || !reader.text(p.type, size_t(type), true) ||
                !reader.text(p.bytes, size_t(size)) || entry.find(p.target.c_str()) ||
                !entry.formats.push(static_cast<Payload &&>(p))) return -EINVAL;
            total += size_t(size);
        }
        if (!saved.push(static_cast<Entry &&>(entry))) return -ENOMEM;
    }
    if (reader.left()) return -EINVAL;
    History next;
    for (size_t i = saved.size(); i > 0; --i)
        if (!next.add(static_cast<Entry &&>(saved[i - 1]))) return -ENOMEM;
    out = static_cast<History &&>(next);
    return 0;
}

int load_preferences(const char *directory, bool &persist)
{
    Text bytes;
    int result = read_file(directory, "settings.conf", bytes, 1024);
    if (result) return result;
    if (bytes == "PersistHistory=1\n") persist = true;
    else if (bytes == "PersistHistory=0\n") persist = false;
    else return -EINVAL;
    return 0;
}

int save_preferences(const char *directory, bool persist)
{
    Text bytes;
    if (!bytes.set(persist ? "PersistHistory=1\n" : "PersistHistory=0\n")) return -ENOMEM;
    return atomic_write(directory, "settings.conf", bytes);
}

int delete_history(const char *directory)
{
    int dir = directory_fd(directory, false);
    if (dir == -ENOENT) return 0;
    if (dir < 0) return dir;
    int result = 0;
    if (unlinkat(dir, "history", 0) < 0 && errno != ENOENT) result = -errno;
    if (!result && fsync(dir) < 0) result = -errno;
    close(dir);
    return result;
}
} // namespace clip
