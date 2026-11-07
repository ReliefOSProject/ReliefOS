#include "recovery.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0
#endif

namespace rm {
namespace {

constexpr size_t kMaxPathBytes = 4096;
constexpr size_t kRecoveryHeaderBytes = 24;
constexpr size_t kRecoveryHintBytes = kMaxPathBytes;
constexpr size_t kMaxRecoveryEntries = 128;
constexpr uint8_t kRecoveryMagic[8] = {'R', 'M', 'R', 'E', 'C', '\r', '\n', 0};
constexpr uint64_t kFnvOffset = UINT64_C(14695981039346656037);
constexpr uint64_t kFnvPrime = UINT64_C(1099511628211);

void fail(Error &error, Code code, const char *message) noexcept {
    error.set(code, message);
}

bool fail_errno(Error &error, const char *operation) noexcept {
    int saved = errno;
    const char *detail = strerror(saved);
    char message[256];
    int count = snprintf(message, sizeof(message), "%s: %s", operation,
                         detail ? detail : "I/O error");
    errno = saved;
    if (count < 0 || static_cast<size_t>(count) >= sizeof(message)) {
        fail(error, Code::Io, operation);
        return false;
    }
    fail(error, Code::Io, message);
    return false;
}

void report_progress(const IoControl *control, IoPhase phase,
                     size_t completed, size_t total) noexcept {
    if (control && control->progress)
        control->progress(control->context, phase, completed, total);
}

bool cancelled(const IoControl *control, Error &error) noexcept {
    if (control && control->cancelled && control->cancelled(control->context)) {
        fail(error, Code::Cancelled, "recovery I/O was cancelled");
        return true;
    }
    return false;
}

uint32_t load_u32(const uint8_t *bytes) noexcept {
    return static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8) |
        (static_cast<uint32_t>(bytes[2]) << 16) |
        (static_cast<uint32_t>(bytes[3]) << 24);
}

void store_u32(uint8_t *bytes, uint32_t value) noexcept {
    for (unsigned index = 0; index < 4; ++index)
        bytes[index] = static_cast<uint8_t>(value >> (index * 8));
}

bool same_stat(const struct stat &left, const struct stat &right) noexcept {
    if (left.st_dev != right.st_dev || left.st_ino != right.st_ino ||
        left.st_size != right.st_size || (left.st_mode & S_IFMT) != (right.st_mode & S_IFMT))
        return false;
#if defined(__APPLE__)
    return left.st_mtimespec.tv_sec == right.st_mtimespec.tv_sec &&
        left.st_mtimespec.tv_nsec == right.st_mtimespec.tv_nsec &&
        left.st_ctimespec.tv_sec == right.st_ctimespec.tv_sec &&
        left.st_ctimespec.tv_nsec == right.st_ctimespec.tv_nsec;
#else
    return left.st_mtim.tv_sec == right.st_mtim.tv_sec &&
        left.st_mtim.tv_nsec == right.st_mtim.tv_nsec &&
        left.st_ctim.tv_sec == right.st_ctim.tv_sec &&
        left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
#endif
}

void fill_stamp(const struct stat &st, uint64_t digest, FileStamp &stamp) noexcept {
    stamp = FileStamp{};
    stamp.exists = true;
    stamp.device = static_cast<uint64_t>(st.st_dev);
    stamp.inode = static_cast<uint64_t>(st.st_ino);
    stamp.size = static_cast<uint64_t>(st.st_size);
#if defined(__APPLE__)
    stamp.modified_seconds = static_cast<int64_t>(st.st_mtimespec.tv_sec);
    stamp.modified_nanoseconds = static_cast<int64_t>(st.st_mtimespec.tv_nsec);
    stamp.changed_seconds = static_cast<int64_t>(st.st_ctimespec.tv_sec);
    stamp.changed_nanoseconds = static_cast<int64_t>(st.st_ctimespec.tv_nsec);
#else
    stamp.modified_seconds = static_cast<int64_t>(st.st_mtim.tv_sec);
    stamp.modified_nanoseconds = static_cast<int64_t>(st.st_mtim.tv_nsec);
    stamp.changed_seconds = static_cast<int64_t>(st.st_ctim.tv_sec);
    stamp.changed_nanoseconds = static_cast<int64_t>(st.st_ctim.tv_nsec);
#endif
    stamp.content_digest = digest;
}

uint64_t digest_bytes(const uint8_t *bytes, size_t size) noexcept {
    uint64_t value = kFnvOffset;
    for (size_t index = 0; index < size; ++index) {
        value ^= bytes[index];
        value *= kFnvPrime;
    }
    return value;
}

bool sync_directory(int fd, const char *operation, Error &error) noexcept {
    int result;
    do { result = fsync(fd); } while (result < 0 && errno == EINTR);
    if (result < 0) return fail_errno(error, operation);
    return true;
}

bool private_path(const char *path, int &directory_fd, Error &error) noexcept {
    directory_fd = -1;
    if (!path || path[0] != '/' || strlen(path) > kMaxPathBytes) {
        fail(error, Code::Format, "recovery directory must be an absolute bounded path");
        return false;
    }
    char *copy = static_cast<char *>(malloc(strlen(path) + 1));
    if (!copy) {
        fail(error, Code::NoMemory, "recovery path allocation failed");
        return false;
    }
    strcpy(copy, path);
    int current = open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (current < 0) {
        free(copy);
        return fail_errno(error, "could not open recovery path root");
    }

    char *at = copy;
    while (*at == '/') ++at;
    bool ok = true;
    bool saw_component = false;
    while (*at) {
        char *end = at;
        while (*end && *end != '/') ++end;
        char saved = *end;
        *end = 0;
        if (!strcmp(at, "..")) {
            fail(error, Code::Format, "recovery path may not traverse a parent directory");
            ok = false;
        } else if (strcmp(at, ".")) {
            saw_component = true;
            int next = openat(current, at,
                O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
            bool needs_open = false;
            if (next < 0 && errno == ENOENT) {
                if (mkdirat(current, at, 0700) < 0 && errno != EEXIST) {
                    ok = fail_errno(error, "could not create a recovery directory");
                } else {
                    needs_open = true;
                }
            }
            if (ok && (next >= 0 || needs_open)) {
                // Existing entries may remain from an earlier failed sync or
                // a concurrent mkdir. Persist each parent before descending.
                if (!sync_directory(current, "could not sync recovery directory parent", error)) {
                    if (next >= 0) close(next);
                    ok = false;
                } else if (needs_open) {
                    next = openat(current, at,
                        O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
                }
            }
            if (ok && next < 0) {
                if (errno == ELOOP || errno == ENOTDIR)
                    fail(error, Code::Format, "recovery path contains a symbolic link or non-directory");
                else fail_errno(error, "could not open a recovery directory");
                ok = false;
            }
            if (ok) {
                close(current);
                current = next;
            }
        }
        *end = saved;
        if (!ok) break;
        at = end;
        while (*at == '/') ++at;
    }
    free(copy);
    if (!ok) {
        close(current);
        return false;
    }
    if (!saw_component) {
        close(current);
        fail(error, Code::Format, "filesystem root cannot be a recovery directory");
        return false;
    }
    struct stat st;
    if (fstat(current, &st) < 0) {
        int saved = errno;
        close(current);
        errno = saved;
        return fail_errno(error, "could not inspect recovery directory");
    }
    if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid()) {
        close(current);
        fail(error, Code::Format, "recovery directory must belong to the current user");
        return false;
    }
    if (fchmod(current, 0700) < 0) {
        int saved = errno;
        close(current);
        errno = saved;
        return fail_errno(error, "could not secure recovery directory");
    }
    if (!sync_directory(current, "could not sync recovery directory permissions", error)) {
        close(current);
        return false;
    }
    directory_fd = current;
    return true;
}

bool join_path(const char *base, const char *suffix, Text &output,
               Error &error) noexcept {
    const size_t base_size = strlen(base);
    const size_t suffix_size = strlen(suffix);
    if (base_size > kMaxPathBytes || suffix_size > kMaxPathBytes - base_size - 1) {
        fail(error, Code::Limit, "recovery path exceeds the path limit");
        return false;
    }
    char *joined = static_cast<char *>(malloc(base_size + suffix_size + 2));
    if (!joined) {
        fail(error, Code::NoMemory, "recovery path allocation failed");
        return false;
    }
    memcpy(joined, base, base_size);
    size_t at = base_size;
    if (!at || joined[at - 1] != '/') joined[at++] = '/';
    memcpy(joined + at, suffix, suffix_size + 1);
    bool ok = output.set(joined, at + suffix_size);
    free(joined);
    if (!ok) fail(error, Code::NoMemory, "recovery path allocation failed");
    return ok;
}

bool make_recovery_bytes(const Document &document, const char *hint,
                         Bytes &output, Error &error) noexcept {
    Text path_hint;
    if (hint) {
        const size_t length = strlen(hint);
        if (length > kRecoveryHintBytes) {
            fail(error, Code::Limit, "recovery path hint exceeds the limit");
            return false;
        }
        if (length && !path_hint.set(hint, length)) {
            if (!detail::valid_utf8(hint, length))
                fail(error, Code::InvalidUtf8, "recovery path hint is not valid UTF-8");
            else fail(error, Code::NoMemory, "recovery path hint allocation failed");
            return false;
        }
    }
    Bytes encoded_document;
    if (!encode_document(document, encoded_document, error)) return false;
    if (encoded_document.size() > UINT32_MAX || path_hint.size() > UINT32_MAX ||
        encoded_document.size() > kMaxAtomicFileBytes - kRecoveryHeaderBytes - path_hint.size()) {
        fail(error, Code::Limit, "recovery snapshot exceeds the bounded storage limit");
        return false;
    }
    const size_t total = kRecoveryHeaderBytes + path_hint.size() + encoded_document.size();
    Bytes candidate;
    if (!candidate.resize(total)) {
        fail(error, Code::NoMemory, "recovery snapshot allocation failed");
        return false;
    }
    uint8_t *bytes = candidate.data();
    memcpy(bytes, kRecoveryMagic, sizeof(kRecoveryMagic));
    store_u32(bytes + 8, 1);
    store_u32(bytes + 12, static_cast<uint32_t>(total));
    store_u32(bytes + 16, static_cast<uint32_t>(encoded_document.size()));
    store_u32(bytes + 20, static_cast<uint32_t>(path_hint.size()));
    if (path_hint.size()) memcpy(bytes + kRecoveryHeaderBytes, path_hint.c_str(), path_hint.size());
    memcpy(bytes + kRecoveryHeaderBytes + path_hint.size(),
           encoded_document.data(), encoded_document.size());
    output.swap(candidate);
    error.clear();
    return true;
}

bool decode_recovery_bytes(const uint8_t *bytes, size_t size, Document &document,
                           Text &hint, Error &error) noexcept {
    if (!bytes || size < kRecoveryHeaderBytes || size > kMaxAtomicFileBytes) {
        fail(error, Code::Format, "recovery snapshot has an invalid length");
        return false;
    }
    if (memcmp(bytes, kRecoveryMagic, sizeof(kRecoveryMagic)) != 0) {
        fail(error, Code::Format, "recovery snapshot magic is invalid");
        return false;
    }
    if (load_u32(bytes + 8) != 1) {
        fail(error, Code::Version, "unsupported recovery snapshot version");
        return false;
    }
    const uint32_t total = load_u32(bytes + 12);
    const uint32_t document_size = load_u32(bytes + 16);
    const uint32_t hint_size = load_u32(bytes + 20);
    if (total != size || hint_size > kRecoveryHintBytes ||
        document_size > kMaxDocumentBytes ||
        hint_size > size - kRecoveryHeaderBytes ||
        document_size != size - kRecoveryHeaderBytes - hint_size) {
        fail(error, Code::Format, "recovery snapshot lengths are inconsistent");
        return false;
    }
    Text candidate_hint;
    if (hint_size && !candidate_hint.set(
            reinterpret_cast<const char *>(bytes + kRecoveryHeaderBytes), hint_size)) {
        if (!detail::valid_utf8(reinterpret_cast<const char *>(bytes + kRecoveryHeaderBytes), hint_size))
            fail(error, Code::InvalidUtf8, "recovery path hint is not valid UTF-8");
        else fail(error, Code::NoMemory, "recovery path hint allocation failed");
        return false;
    }
    Document candidate_document;
    if (!decode_document(bytes + kRecoveryHeaderBytes + hint_size,
                         document_size, candidate_document, error)) return false;
    document.swap(candidate_document);
    hint = static_cast<Text &&>(candidate_hint);
    error.clear();
    return true;
}

bool make_filename(uint64_t id, const char *extension, char *out,
                   size_t capacity) noexcept {
    int count = snprintf(out, capacity, "%016llx%s",
                         static_cast<unsigned long long>(id), extension);
    return count > 0 && static_cast<size_t>(count) < capacity;
}

bool parse_filename(const char *name, uint64_t &id) noexcept {
    if (!name || strlen(name) != 21 || strcmp(name + 16, ".rrec")) return false;
    uint64_t value = 0;
    for (size_t index = 0; index < 16; ++index) {
        unsigned digit;
        if (name[index] >= '0' && name[index] <= '9') digit = name[index] - '0';
        else if (name[index] >= 'a' && name[index] <= 'f') digit = name[index] - 'a' + 10;
        else return false;
        value = (value << 4) | digit;
    }
    if (!value) return false;
    id = value;
    return true;
}

ssize_t read_chunk(int fd, void *data, size_t size) noexcept {
    return read(fd, data, size);
}

bool read_recovery_file(int directory_fd, uint64_t id, Bytes &output,
                        FileStamp &stamp, Error &error,
                        const IoControl *control) noexcept {
    if (cancelled(control, error)) return false;
    char name[32];
    if (!make_filename(id, ".rrec", name, sizeof(name))) {
        fail(error, Code::Format, "recovery ID is invalid");
        return false;
    }
    struct stat path_stat;
    if (fstatat(directory_fd, name, &path_stat, AT_SYMLINK_NOFOLLOW) < 0)
        return fail_errno(error, "could not inspect recovery snapshot");
    if (S_ISLNK(path_stat.st_mode) || !S_ISREG(path_stat.st_mode) ||
        path_stat.st_uid != geteuid() || (path_stat.st_mode & 0777) != 0600) {
        fail(error, Code::Format, "recovery snapshot is not a private regular file");
        return false;
    }
    if (path_stat.st_size < static_cast<off_t>(kRecoveryHeaderBytes) ||
        static_cast<uint64_t>(path_stat.st_size) > kMaxAtomicFileBytes) {
        fail(error, Code::Limit, "recovery snapshot length is outside its limit");
        return false;
    }
    int fd = openat(directory_fd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return fail_errno(error, "could not open recovery snapshot");
    struct stat before;
    if (fstat(fd, &before) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return fail_errno(error, "could not inspect opened recovery snapshot");
    }
    if (!same_stat(path_stat, before)) {
        close(fd);
        fail(error, Code::Conflict, "recovery snapshot changed while opening");
        return false;
    }
    const size_t size = static_cast<size_t>(before.st_size);
    Bytes candidate;
    if (!candidate.resize(size)) {
        close(fd);
        fail(error, Code::NoMemory, "recovery snapshot allocation failed");
        return false;
    }
    report_progress(control, IoPhase::Read, 0, size);
    size_t offset = 0;
    while (offset < size) {
        if (cancelled(control, error)) {
            close(fd);
            return false;
        }
        size_t request = size - offset;
        if (request > 64u * 1024u) request = 64u * 1024u;
        ssize_t count = read_chunk(fd, candidate.data() + offset, request);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            int saved = errno;
            close(fd);
            errno = saved;
            return fail_errno(error, "could not read recovery snapshot");
        }
        if (!count) {
            close(fd);
            fail(error, Code::Conflict, "recovery snapshot was truncated while reading");
            return false;
        }
        offset += static_cast<size_t>(count);
        report_progress(control, IoPhase::Read, offset, size);
    }
    if (cancelled(control, error)) {
        close(fd);
        return false;
    }
    struct stat after;
    if (fstat(fd, &after) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return fail_errno(error, "could not recheck recovery snapshot");
    }
    struct stat path_after;
    if (!same_stat(before, after) ||
        fstatat(directory_fd, name, &path_after, AT_SYMLINK_NOFOLLOW) < 0 ||
        !same_stat(before, path_after)) {
        close(fd);
        fail(error, Code::Conflict, "recovery snapshot changed while reading");
        return false;
    }
    if (close(fd) < 0) return fail_errno(error, "could not close recovery snapshot");
    FileStamp candidate_stamp;
    fill_stamp(after, digest_bytes(candidate.data(), candidate.size()), candidate_stamp);
    output.swap(candidate);
    stamp = candidate_stamp;
    report_progress(control, IoPhase::Read, size, size);
    error.clear();
    return true;
}

bool read_recovery(int directory_fd, uint64_t id, Document &document,
                   Text &hint, FileStamp &stamp, Error &error,
                   const IoControl *control) noexcept {
    Bytes bytes;
    FileStamp candidate_stamp;
    if (!read_recovery_file(directory_fd, id, bytes, candidate_stamp, error, control))
        return false;
    Document candidate_document;
    Text candidate_hint;
    if (!decode_recovery_bytes(bytes.data(), bytes.size(), candidate_document,
                               candidate_hint, error)) return false;
    document.swap(candidate_document);
    hint = static_cast<Text &&>(candidate_hint);
    stamp = candidate_stamp;
    error.clear();
    return true;
}

bool lock_file_name(uint64_t id, char *name, size_t capacity) noexcept {
    return make_filename(id, ".lock", name, capacity);
}

bool acquire_lock(int directory_fd, uint64_t id, int &lock_fd,
                  bool &busy, bool &missing, Error &error) noexcept {
    lock_fd = -1;
    busy = false;
    missing = false;
    char name[32];
    if (!lock_file_name(id, name, sizeof(name))) {
        fail(error, Code::Format, "recovery ID is invalid");
        return false;
    }
    int fd = openat(directory_fd, name,
                    O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT) { missing = true; return true; }
        return fail_errno(error, "could not open recovery instance lock");
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return fail_errno(error, "could not inspect recovery instance lock");
    }
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0777) != 0600) {
        close(fd);
        fail(error, Code::Format, "recovery lock is not a private regular file");
        return false;
    }
    int result;
    do { result = flock(fd, LOCK_EX | LOCK_NB); } while (result < 0 && errno == EINTR);
    if (result < 0) {
        const int saved = errno;
        close(fd);
        if (saved == EWOULDBLOCK || saved == EAGAIN) {
            busy = true;
            return true;
        }
        errno = saved;
        return fail_errno(error, "could not lock recovery snapshot");
    }
    lock_fd = fd;
    return true;
}

int duplicate_lock(int descriptor, Error &error) noexcept {
    int copy = dup(descriptor);
    if (copy < 0) {
        fail_errno(error, "could not retain recovery instance lock");
        return -1;
    }
    int flags = fcntl(copy, F_GETFD);
    if (flags < 0 || fcntl(copy, F_SETFD, flags | FD_CLOEXEC) < 0) {
        int saved = errno;
        close(copy);
        errno = saved;
        fail_errno(error, "could not secure recovery lock descriptor");
        return -1;
    }
    return copy;
}

const RecoveryItem *find_item(const Array<RecoveryItem> &items, uint64_t id,
                              size_t *index = nullptr) noexcept {
    for (size_t at = 0; at < items.size(); ++at) {
        if (items[at].id == id) {
            if (index) *index = at;
            return &items[at];
        }
    }
    return nullptr;
}

bool validate_recovery_file_mode(int directory_fd, uint64_t id,
                                 Error &error) noexcept {
    char name[32];
    if (!make_filename(id, ".rrec", name, sizeof(name))) {
        fail(error, Code::Format, "recovery ID is invalid");
        return false;
    }
    struct stat st;
    if (fstatat(directory_fd, name, &st, AT_SYMLINK_NOFOLLOW) < 0)
        return fail_errno(error, "could not inspect recovery snapshot");
    if (S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 0777) != 0600) {
        fail(error, Code::Format, "recovery snapshot is not a private regular file");
        return false;
    }
    return true;
}

} // namespace

RecoveryItem::RecoveryItem(RecoveryItem &&other) noexcept
    : lock_fd_(other.lock_fd_), stamp_(other.stamp_), id(other.id),
      title(static_cast<Text &&>(other.title)),
      original_path_hint(static_cast<Text &&>(other.original_path_hint)) {
    other.lock_fd_ = -1;
    other.id = 0;
    other.stamp_ = FileStamp{};
}

RecoveryItem &RecoveryItem::operator=(RecoveryItem &&other) noexcept {
    if (this == &other) return *this;
    if (lock_fd_ >= 0) close(lock_fd_);
    lock_fd_ = other.lock_fd_;
    other.lock_fd_ = -1;
    id = other.id;
    other.id = 0;
    title = static_cast<Text &&>(other.title);
    original_path_hint = static_cast<Text &&>(other.original_path_hint);
    stamp_ = other.stamp_;
    other.stamp_ = FileStamp{};
    return *this;
}

RecoveryItem::~RecoveryItem() {
    if (lock_fd_ >= 0) close(lock_fd_);
}

RecoveryStore::~RecoveryStore() {
    if (own_lock_fd_ >= 0) close(own_lock_fd_);
    if (directory_fd_ >= 0) close(directory_fd_);
}

bool recovery_directory(Text &output, Error &error) noexcept {
    const char *base = getenv("XDG_STATE_HOME");
    char fallback[kMaxPathBytes + 1];
    if (!base || !*base || base[0] != '/') {
        const char *home = getenv("HOME");
        if (!home || !*home || home[0] != '/') {
            fail(error, Code::NotFound, "no absolute XDG_STATE_HOME or HOME is available");
            return false;
        }
        const size_t home_size = strlen(home);
        static const char suffix[] = "/.local/state";
        if (home_size > kMaxPathBytes - sizeof(suffix)) {
            fail(error, Code::Limit, "home state path exceeds the path limit");
            return false;
        }
        int count = snprintf(fallback, sizeof(fallback), "%s/.local/state", home);
        if (count < 0 || static_cast<size_t>(count) >= sizeof(fallback)) {
            fail(error, Code::Limit, "home state path exceeds the path limit");
            return false;
        }
        base = fallback;
    }
    if (strlen(base) > kMaxPathBytes) {
        fail(error, Code::Limit, "XDG state path exceeds the path limit");
        return false;
    }
    Text target;
    if (!join_path(base, "reliefmath/recovery", target, error)) return false;
    int directory_fd = -1;
    if (!private_path(target.c_str(), directory_fd, error)) return false;
    if (close(directory_fd) < 0) return fail_errno(error, "could not close recovery directory");
    output = static_cast<Text &&>(target);
    error.clear();
    return true;
}

bool RecoveryStore::open(const char *directory, Error &error) noexcept {
    if (directory_fd_ >= 0 || own_lock_fd_ >= 0) {
        fail(error, Code::Format, "recovery store is already open");
        return false;
    }
    int candidate_fd = -1;
    if (!private_path(directory, candidate_fd, error)) return false;
    Text candidate_path;
    if (!candidate_path.set(directory)) {
        close(candidate_fd);
        fail(error, Code::NoMemory, "recovery directory path allocation failed");
        return false;
    }
    directory_fd_ = candidate_fd;
    directory_ = static_cast<Text &&>(candidate_path);
    error.clear();
    return true;
}

bool RecoveryStore::open_default(Error &error) noexcept {
    Text directory;
    if (!recovery_directory(directory, error)) return false;
    return open(directory.c_str(), error);
}

bool RecoveryStore::save(const Document &document, Error &error,
                         const IoControl *control) noexcept {
    return save(document, own_path_hint_.c_str(), error, control);
}

bool RecoveryStore::save(const Document &document, const char *original_path_hint,
                         Error &error, const IoControl *control) noexcept {
    if (directory_fd_ < 0) {
        fail(error, Code::Format, "recovery store is not open");
        return false;
    }
    Bytes bytes;
    if (!make_recovery_bytes(document, original_path_hint, bytes, error)) return false;
    Text hint_candidate;
    if (original_path_hint && !hint_candidate.set(original_path_hint)) {
        const size_t length = strlen(original_path_hint);
        if (length > kRecoveryHintBytes) fail(error, Code::Limit, "recovery path hint exceeds the limit");
        else if (!detail::valid_utf8(original_path_hint, length))
            fail(error, Code::InvalidUtf8, "recovery path hint is not valid UTF-8");
        else fail(error, Code::NoMemory, "recovery path hint allocation failed");
        return false;
    }

    if (own_id_ == 0) {
        static uint32_t sequence = 0;
        bool reserved = false;
        for (unsigned attempt = 0; attempt < 256; ++attempt) {
            uint32_t next = __atomic_add_fetch(&sequence, uint32_t(1), __ATOMIC_RELAXED);
            uint64_t id = (static_cast<uint64_t>(static_cast<uint32_t>(getpid())) << 32) | next;
            if (!id) continue;
            char lock_name[32];
            if (!lock_file_name(id, lock_name, sizeof(lock_name))) {
                fail(error, Code::Limit, "recovery lock name is too long");
                return false;
            }
            int fd = openat(directory_fd_, lock_name,
                O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
            if (fd < 0) {
                if (errno == EEXIST) continue;
                return fail_errno(error, "could not create recovery instance lock");
            }
            bool valid = fchmod(fd, 0600) == 0;
            struct stat st;
            if (valid && fstat(fd, &st) < 0) valid = false;
            if (valid && (!S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
                          (st.st_mode & 0777) != 0600)) {
                errno = EPERM;
                valid = false;
            }
            int saved = errno;
            if (valid) {
                int result;
                do { result = flock(fd, LOCK_EX | LOCK_NB); } while (result < 0 && errno == EINTR);
                if (result < 0) { saved = errno; valid = false; }
            }
            if (valid && fsync(fd) < 0) { saved = errno; valid = false; }
            if (valid && fsync(directory_fd_) < 0) { saved = errno; valid = false; }
            if (!valid) {
                close(fd);
                unlinkat(directory_fd_, lock_name, 0);
                if (saved == EEXIST) continue;
                errno = saved;
                return fail_errno(error, "could not secure recovery instance lock");
            }
            own_id_ = id;
            own_lock_fd_ = fd;
            reserved = true;
            break;
        }
        if (!reserved) {
            fail(error, Code::Io, "could not reserve a unique recovery instance ID");
            return false;
        }
    }

    char filename[32];
    if (!make_filename(own_id_, ".rrec", filename, sizeof(filename))) {
        fail(error, Code::Format, "current recovery ID is invalid");
        return false;
    }
    Text full_path;
    if (!join_path(directory_.c_str(), filename, full_path, error)) return false;
    FileStamp expected = own_stamp_;
    FileStamp saved_stamp{};
    bool saved = atomic_save_bytes(full_path.c_str(), bytes.data(), bytes.size(),
                                   expected, saved_stamp, error, control);
    if (saved_stamp.exists) {
        own_stamp_ = saved_stamp;
        own_path_hint_ = static_cast<Text &&>(hint_candidate);
    }
    if (saved) error.clear();
    return saved;
}

bool RecoveryStore::list(Error &error) noexcept {
    if (directory_fd_ < 0) {
        fail(error, Code::Format, "recovery store is not open");
        return false;
    }
    // dup() shares a directory stream offset with directory_fd_; each scan
    // needs an independent open file description so repeated lists start over.
    int scan_fd = openat(directory_fd_, ".",
                         O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (scan_fd < 0) return fail_errno(error, "could not scan recovery directory");
    DIR *directory = fdopendir(scan_fd);
    if (!directory) {
        int saved = errno;
        close(scan_fd);
        errno = saved;
        return fail_errno(error, "could not scan recovery directory");
    }
    Array<RecoveryItem> candidate;
    bool ok = true;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) {
            if (errno) ok = fail_errno(error, "could not read recovery directory");
            break;
        }
        uint64_t id = 0;
        if (!parse_filename(entry->d_name, id)) continue;
        struct stat st;
        if (fstatat(directory_fd_, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
            if (errno == ENOENT) continue;
            ok = fail_errno(error, "could not inspect recovery entry");
            break;
        }
        if (S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
            (st.st_mode & 0777) != 0600) {
            fail(error, Code::Format, "recovery entry is not a private regular file");
            ok = false;
            break;
        }

        int lock_fd = -1;
        if (id == own_id_ && own_lock_fd_ >= 0) {
            lock_fd = duplicate_lock(own_lock_fd_, error);
            if (lock_fd < 0) { ok = false; break; }
        } else {
            const RecoveryItem *old = find_item(items_, id);
            if (old && old->lock_fd_ >= 0) {
                lock_fd = duplicate_lock(old->lock_fd_, error);
                if (lock_fd < 0) { ok = false; break; }
            } else {
                bool busy = false, missing = false;
                if (!acquire_lock(directory_fd_, id, lock_fd, busy, missing, error)) {
                    ok = false;
                    break;
                }
                if (busy || missing) continue;
            }
        }

        Document decoded;
        Text hint;
        FileStamp stamp;
        if (!read_recovery(directory_fd_, id, decoded, hint, stamp, error, nullptr)) {
            close(lock_fd);
            ok = false;
            break;
        }
        if (id == own_id_ && own_stamp_.exists && !same_stamp(own_stamp_, stamp)) {
            close(lock_fd);
            fail(error, Code::Conflict, "active recovery snapshot changed outside ReliefMath");
            ok = false;
            break;
        }
        if (candidate.size() >= kMaxRecoveryEntries) {
            close(lock_fd);
            fail(error, Code::Limit, "recovery directory contains too many snapshots");
            ok = false;
            break;
        }
        RecoveryItem item;
        item.id = id;
        item.title = decoded.title;
        item.original_path_hint = static_cast<Text &&>(hint);
        item.stamp_ = stamp;
        item.lock_fd_ = lock_fd;
        if (!candidate.push(static_cast<RecoveryItem &&>(item))) {
            fail(error, Code::NoMemory, "recovery list allocation failed");
            ok = false;
            break;
        }
    }
    int close_result = closedir(directory);
    if (ok && close_result < 0) ok = fail_errno(error, "could not close recovery directory scan");
    if (!ok) return false;
    items_.swap(candidate);
    error.clear();
    return true;
}

bool RecoveryStore::load(uint64_t id, Document &output, Error &error,
                         const IoControl *control) noexcept {
    if (directory_fd_ < 0 || !id) {
        fail(error, Code::Format, "recovery store or ID is invalid");
        return false;
    }
    if (cancelled(control, error)) return false;
    bool is_own = id == own_id_ && own_lock_fd_ >= 0;
    const RecoveryItem *item = find_item(items_, id);
    if (!is_own && !item) {
        if (!list(error)) return false;
        item = find_item(items_, id);
    }
    int transient_lock = -1;
    if (!is_own && !item) {
        bool busy = false, missing = false;
        if (!acquire_lock(directory_fd_, id, transient_lock, busy, missing, error)) return false;
        if (busy) {
            fail(error, Code::Conflict, "recovery snapshot belongs to a live instance");
            return false;
        }
        if (missing) {
            fail(error, Code::NotFound, "recovery snapshot does not exist");
            return false;
        }
    }

    Document candidate;
    Text hint;
    FileStamp stamp;
    bool ok = read_recovery(directory_fd_, id, candidate, hint, stamp, error, control);
    if (ok) {
        const FileStamp *expected = is_own ? &own_stamp_ : (item ? &item->stamp_ : nullptr);
        if (expected && expected->exists && !same_stamp(*expected, stamp)) {
            fail(error, Code::Conflict, "recovery snapshot changed since it was listed");
            ok = false;
        }
    }
    if (transient_lock >= 0) close(transient_lock);
    if (!ok) return false;
    output.swap(candidate);
    error.clear();
    return true;
}

bool RecoveryStore::discard(uint64_t id, Error &error) noexcept {
    if (directory_fd_ < 0 || !id) {
        fail(error, Code::Format, "recovery store or ID is invalid");
        return false;
    }
    const bool is_own = id == own_id_ && own_lock_fd_ >= 0;
    size_t item_index = 0;
    const RecoveryItem *item = find_item(items_, id, &item_index);
    int transient_lock = -1;
    if (!is_own && !item) {
        bool busy = false, missing = false;
        if (!acquire_lock(directory_fd_, id, transient_lock, busy, missing, error)) return false;
        if (busy) {
            fail(error, Code::Conflict, "recovery snapshot belongs to a live instance");
            return false;
        }
        if (missing) {
            fail(error, Code::NotFound, "recovery snapshot does not exist");
            return false;
        }
    }
    if (!validate_recovery_file_mode(directory_fd_, id, error)) {
        if (transient_lock >= 0) close(transient_lock);
        return false;
    }
    char recovery_name[32], lock_name[32];
    if (!make_filename(id, ".rrec", recovery_name, sizeof(recovery_name)) ||
        !lock_file_name(id, lock_name, sizeof(lock_name))) {
        if (transient_lock >= 0) close(transient_lock);
        fail(error, Code::Format, "recovery ID is invalid");
        return false;
    }
    if (unlinkat(directory_fd_, recovery_name, 0) < 0) {
        if (transient_lock >= 0) close(transient_lock);
        return fail_errno(error, "could not discard recovery snapshot");
    }
    int lock_unlink_error = 0;
    if (unlinkat(directory_fd_, lock_name, 0) < 0 && errno != ENOENT)
        lock_unlink_error = errno;
    int sync_error = fsync(directory_fd_) < 0 ? errno : 0;

    if (is_own) {
        close(own_lock_fd_);
        own_lock_fd_ = -1;
        own_id_ = 0;
        own_stamp_ = FileStamp{};
        own_path_hint_.clear();
    }
    if (item) items_.erase(item_index);
    if (transient_lock >= 0) close(transient_lock);
    if (lock_unlink_error) {
        errno = lock_unlink_error;
        return fail_errno(error, "recovery snapshot was removed but its lock could not be cleaned up");
    }
    if (sync_error) {
        errno = sync_error;
        fail_errno(error, "recovery snapshot was removed but directory persistence is unconfirmed");
        return false;
    }
    error.clear();
    return true;
}

bool RecoveryStore::discard_own(Error &error) noexcept {
    if (!own_id_ || own_lock_fd_ < 0) {
        error.clear();
        return true;
    }
    char snapshot_name[32], lock_name[32];
    if (!make_filename(own_id_, ".rrec", snapshot_name, sizeof(snapshot_name)) ||
        !lock_file_name(own_id_, lock_name, sizeof(lock_name))) {
        fail(error, Code::Format, "current recovery ID is invalid");
        return false;
    }
    struct stat snapshot_stat;
    if (fstatat(directory_fd_, snapshot_name, &snapshot_stat, AT_SYMLINK_NOFOLLOW) < 0) {
        if (errno != ENOENT) return fail_errno(error, "could not inspect own recovery snapshot");
        int unlink_error = 0;
        if (unlinkat(directory_fd_, lock_name, 0) < 0 && errno != ENOENT)
            unlink_error = errno;
        int sync_error = fsync(directory_fd_) < 0 ? errno : 0;
        size_t item_index = 0;
        const RecoveryItem *item = find_item(items_, own_id_, &item_index);
        if (item) items_.erase(item_index);
        close(own_lock_fd_);
        own_lock_fd_ = -1;
        own_id_ = 0;
        own_stamp_ = FileStamp{};
        own_path_hint_.clear();
        if (unlink_error) {
            errno = unlink_error;
            return fail_errno(error, "could not remove own recovery lock");
        }
        if (sync_error) {
            errno = sync_error;
            return fail_errno(error, "own recovery lock was removed but directory persistence is unconfirmed");
        }
        error.clear();
        return true;
    }
    return discard(own_id_, error);
}

} // namespace rm
