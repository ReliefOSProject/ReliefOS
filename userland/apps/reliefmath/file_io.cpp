#include "file_io.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#ifndef NAME_MAX
#define NAME_MAX 255
#endif

namespace rm {
namespace {

constexpr size_t kMaxPathBytes = 4096;
constexpr size_t kIoChunkBytes = 64u * 1024u;
constexpr uint64_t kFnvOffset = UINT64_C(14695981039346656037);
constexpr uint64_t kFnvPrime = UINT64_C(1099511628211);

#ifdef RM_TESTING
testing::IoTestControl g_test_control;
size_t g_test_write_calls = 0;
#endif

void fail(Error &error, Code code, const char *message) noexcept {
    error.set(code, message);
}

bool fail_errno(Error &error, const char *operation) noexcept {
    const int saved = errno;
    const char *detail = strerror(saved);
    char message[256];
    int count = snprintf(message, sizeof(message), "%s: %s", operation,
                         detail ? detail : "I/O error");
    errno = saved;
    if (count < 0 || static_cast<size_t>(count) >= sizeof(message)) {
        error.set(Code::Io, operation);
        return false;
    }
    error.set(Code::Io, message);
    return false;
}

void report_progress(const IoControl *control, IoPhase phase,
                     size_t completed, size_t total) noexcept {
    if (control && control->progress)
        control->progress(control->context, phase, completed, total);
}

bool was_cancelled(const IoControl *control, Error &error) noexcept {
    if (control && control->cancelled && control->cancelled(control->context)) {
        fail(error, Code::Cancelled, "document I/O was cancelled");
        return true;
    }
    return false;
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

void fill_stat_stamp(const struct stat &st, uint64_t digest,
                     FileStamp &stamp) noexcept {
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

bool parse_path(const char *path, char **parent, char **basename,
                Error &error) noexcept {
    *parent = nullptr;
    *basename = nullptr;
    if (!path || !*path) {
        fail(error, Code::Format, "document path is empty");
        return false;
    }
    const size_t length = strlen(path);
    if (length > kMaxPathBytes) {
        fail(error, Code::Limit, "document path exceeds the path limit");
        return false;
    }
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const size_t base_length = strlen(base);
    if (!base_length || base_length > NAME_MAX ||
        (base_length == 1 && base[0] == '.') ||
        (base_length == 2 && base[0] == '.' && base[1] == '.')) {
        fail(error, Code::Format, "document path has an unsafe file name");
        return false;
    }
    const size_t parent_length = slash ?
        (slash == path ? 1 : static_cast<size_t>(slash - path)) : 1;
    char *parent_copy = static_cast<char *>(malloc(parent_length + 1));
    char *base_copy = static_cast<char *>(malloc(base_length + 1));
    if (!parent_copy || !base_copy) {
        free(parent_copy);
        free(base_copy);
        fail(error, Code::NoMemory, "document path allocation failed");
        return false;
    }
    if (slash) {
        memcpy(parent_copy, path, parent_length);
        parent_copy[parent_length] = 0;
    } else {
        parent_copy[0] = '.';
        parent_copy[1] = 0;
    }
    memcpy(base_copy, base, base_length + 1);
    *parent = parent_copy;
    *basename = base_copy;
    return true;
}

bool open_directory_without_links(const char *path, int &descriptor,
                                 Error &error) noexcept {
    descriptor = -1;
    if (!path || !*path || strlen(path) > kMaxPathBytes) {
        fail(error, Code::Format, "document parent directory is invalid");
        return false;
    }
    const bool absolute = path[0] == '/';
    int current = open(absolute ? "/" : ".", O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (current < 0) return fail_errno(error, "could not open document parent directory");

    char *copy = static_cast<char *>(malloc(strlen(path) + 1));
    if (!copy) {
        close(current);
        fail(error, Code::NoMemory, "document parent path allocation failed");
        return false;
    }
    strcpy(copy, path);
    char *at = copy;
    while (*at == '/') ++at;
    bool ok = true;
    while (*at) {
        char *end = at;
        while (*end && *end != '/') ++end;
        char saved = *end;
        *end = 0;
        if (!strcmp(at, "..")) {
            fail(error, Code::Format, "document path may not traverse a parent directory");
            ok = false;
        } else if (strcmp(at, ".")) {
            int next = openat(current, at,
                O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
            if (next < 0) {
                if (errno == ELOOP || errno == ENOTDIR)
                    fail(error, Code::Format, "document path contains a symbolic link or non-directory");
                else fail_errno(error, "could not open document parent directory");
                ok = false;
            } else {
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
    descriptor = current;
    return true;
}

bool open_parent(const char *path, int &directory_fd, char **basename,
                 Error &error) noexcept {
    char *parent = nullptr;
    if (!parse_path(path, &parent, basename, error)) return false;
    bool ok = open_directory_without_links(parent, directory_fd, error);
    free(parent);
    if (!ok) {
        free(*basename);
        *basename = nullptr;
    }
    return ok;
}

int open_read_at(int directory_fd, const char *basename) noexcept {
#ifdef RM_TESTING
    if (g_test_control.fail_read_open_errno) {
        errno = g_test_control.fail_read_open_errno;
        return -1;
    }
#endif
    return openat(directory_fd, basename,
                  O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
}

uint64_t digest_bytes(const uint8_t *bytes, size_t size) noexcept {
    uint64_t digest = kFnvOffset;
    for (size_t index = 0; index < size; ++index) {
        digest ^= bytes[index];
        digest *= kFnvPrime;
    }
    return digest;
}

bool read_bytes_at(int directory_fd, const char *basename, Bytes &output,
                   FileStamp &stamp, Error &error,
                   const IoControl *control, size_t max_bytes) noexcept {
    if (was_cancelled(control, error)) return false;
    struct stat path_stat;
    if (fstatat(directory_fd, basename, &path_stat, AT_SYMLINK_NOFOLLOW) < 0) {
        if (errno == ENOENT) {
            fail(error, Code::NotFound, "document does not exist");
            return false;
        }
        return fail_errno(error, "could not inspect document");
    }
    if (S_ISLNK(path_stat.st_mode) || !S_ISREG(path_stat.st_mode)) {
        fail(error, Code::Format, "document must be a regular non-symbolic-link file");
        return false;
    }
    if (path_stat.st_size < 0 ||
        static_cast<uint64_t>(path_stat.st_size) > max_bytes) {
        fail(error, Code::Limit, "file exceeds the bounded storage limit");
        return false;
    }
    const size_t size = static_cast<size_t>(path_stat.st_size);
    int fd = open_read_at(directory_fd, basename);
    if (fd < 0) return fail_errno(error, "could not open document");
    struct stat before;
    if (fstat(fd, &before) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return fail_errno(error, "could not inspect opened document");
    }
    if (!S_ISREG(before.st_mode) || !same_stat(path_stat, before)) {
        close(fd);
        fail(error, Code::Conflict, "document changed while it was being opened");
        return false;
    }

    Bytes candidate;
    if (!candidate.resize(size)) {
        close(fd);
        fail(error, Code::NoMemory, "document read allocation failed");
        return false;
    }
    report_progress(control, IoPhase::Read, 0, size);
    size_t offset = 0;
    while (offset < size) {
        if (was_cancelled(control, error)) {
            close(fd);
            return false;
        }
        size_t request = size - offset;
        if (request > kIoChunkBytes) request = kIoChunkBytes;
        ssize_t count = read(fd, candidate.data() + offset, request);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            int saved = errno;
            close(fd);
            errno = saved;
            return fail_errno(error, "could not read document");
        }
        if (!count) {
            close(fd);
            fail(error, Code::Conflict, "document was truncated while it was being read");
            return false;
        }
        offset += static_cast<size_t>(count);
        report_progress(control, IoPhase::Read, offset, size);
    }
    if (was_cancelled(control, error)) {
        close(fd);
        return false;
    }
    uint8_t extra = 0;
    ssize_t extra_count;
    do { extra_count = read(fd, &extra, 1); } while (extra_count < 0 && errno == EINTR);
    if (extra_count < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return fail_errno(error, "could not verify document length");
    }
    if (extra_count != 0) {
        close(fd);
        fail(error, Code::Conflict, "document grew while it was being read");
        return false;
    }
    struct stat after;
    if (fstat(fd, &after) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return fail_errno(error, "could not recheck document");
    }
    if (!same_stat(before, after)) {
        close(fd);
        fail(error, Code::Conflict, "document changed while it was being read");
        return false;
    }
    struct stat path_after;
    if (fstatat(directory_fd, basename, &path_after, AT_SYMLINK_NOFOLLOW) < 0 ||
        !same_stat(before, path_after)) {
        int saved = errno;
        close(fd);
        if (saved && saved != ENOENT) errno = saved;
        fail(error, Code::Conflict, "document path changed while it was being read");
        return false;
    }
    if (close(fd) < 0) return fail_errno(error, "could not close document");

    FileStamp candidate_stamp;
    fill_stat_stamp(after, digest_bytes(candidate.data(), candidate.size()), candidate_stamp);
    output.swap(candidate);
    stamp = candidate_stamp;
    report_progress(control, IoPhase::Read, size, size);
    error.clear();
    return true;
}

bool check_target_at(int directory_fd, const char *basename,
                     FileStamp &stamp, Error &error,
                     const IoControl *control) noexcept {
    struct stat st;
    if (fstatat(directory_fd, basename, &st, AT_SYMLINK_NOFOLLOW) < 0) {
        if (errno == ENOENT) {
            stamp = FileStamp{};
            return true;
        }
        return fail_errno(error, "could not inspect save target");
    }
    if (S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode)) {
        fail(error, Code::Format, "save target must be a regular non-symbolic-link file");
        return false;
    }
    Bytes ignored;
    return read_bytes_at(directory_fd, basename, ignored, stamp, error, control,
                         kMaxAtomicFileBytes);
}

ssize_t write_chunk(int fd, const void *bytes, size_t size) noexcept {
#ifdef RM_TESTING
    if (g_test_control.write_eintr_count) {
        --g_test_control.write_eintr_count;
        errno = EINTR;
        return -1;
    }
    if (g_test_control.fail_write_after >= 0 &&
        g_test_write_calls >= static_cast<size_t>(g_test_control.fail_write_after)) {
        errno = g_test_control.fail_write_errno;
        return -1;
    }
    if (g_test_control.max_write_size && size > g_test_control.max_write_size)
        size = g_test_control.max_write_size;
    ++g_test_write_calls;
#endif
    return write(fd, bytes, size);
}

int open_temp_at(int directory_fd, const char *name) noexcept {
#ifdef RM_TESTING
    if (g_test_control.fail_temp_open_errno) {
        errno = g_test_control.fail_temp_open_errno;
        return -1;
    }
#endif
    return openat(directory_fd, name,
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
}

int sync_file(int fd) noexcept {
#ifdef RM_TESTING
    if (g_test_control.fail_file_sync_errno) {
        errno = g_test_control.fail_file_sync_errno;
        return -1;
    }
#endif
    return fsync(fd);
}

int sync_directory(int fd) noexcept {
#ifdef RM_TESTING
    if (g_test_control.fail_directory_sync_errno) {
        errno = g_test_control.fail_directory_sync_errno;
        return -1;
    }
#endif
    return fsync(fd);
}

int rename_at(int directory_fd, const char *old_name, const char *new_name) noexcept {
#ifdef RM_TESTING
    if (g_test_control.fail_rename_errno) {
        errno = g_test_control.fail_rename_errno;
        return -1;
    }
#endif
    return renameat(directory_fd, old_name, directory_fd, new_name);
}

bool make_temp_name(char *name, size_t capacity, Error &error) noexcept {
    static uint64_t serial = 0;
    const uint64_t next = __atomic_add_fetch(&serial, UINT64_C(1), __ATOMIC_RELAXED);
    int count = snprintf(name, capacity, ".rmath-tmp.%ld.%llu",
                         static_cast<long>(getpid()),
                         static_cast<unsigned long long>(next));
    if (count < 0 || static_cast<size_t>(count) >= capacity) {
        fail(error, Code::Limit, "temporary file name is too long");
        return false;
    }
    return true;
}

bool open_temp(int directory_fd, char *name, size_t capacity, int &fd,
               Error &error) noexcept {
    fd = -1;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        if (!make_temp_name(name, capacity, error)) return false;
        fd = open_temp_at(directory_fd, name);
        if (fd >= 0) break;
        if (errno != EEXIST) return fail_errno(error, "could not create temporary document");
    }
    if (fd < 0) {
        fail(error, Code::Io, "could not reserve a unique temporary document");
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        int saved = errno;
        close(fd);
        fd = -1;
        unlinkat(directory_fd, name, 0);
        errno = saved;
        return fail_errno(error, "could not inspect temporary document");
    }
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0777) != 0600) {
        close(fd);
        fd = -1;
        unlinkat(directory_fd, name, 0);
        fail(error, Code::Io, "temporary document is not a private regular file");
        return false;
    }
    return true;
}

} // namespace

bool file_stamp(const char *path, FileStamp &stamp, Error &error,
                const IoControl *control) noexcept {
    if (was_cancelled(control, error)) return false;
    int directory_fd = -1;
    char *basename = nullptr;
    if (!open_parent(path, directory_fd, &basename, error)) return false;
    FileStamp candidate;
    struct stat st;
    if (fstatat(directory_fd, basename, &st, AT_SYMLINK_NOFOLLOW) < 0 && errno == ENOENT) {
        candidate = FileStamp{};
        close(directory_fd);
        free(basename);
        stamp = candidate;
        error.clear();
        return true;
    }
    bool ok = check_target_at(directory_fd, basename, candidate, error, control);
    int saved = errno;
    if (close(directory_fd) < 0 && ok) {
        errno = saved;
        ok = fail_errno(error, "could not close document directory");
    }
    free(basename);
    if (!ok) return false;
    stamp = candidate;
    error.clear();
    return true;
}

bool same_stamp(const FileStamp &left, const FileStamp &right) noexcept {
    if (left.exists != right.exists) return false;
    if (!left.exists) return true;
    return left.device == right.device && left.inode == right.inode &&
        left.size == right.size && left.modified_seconds == right.modified_seconds &&
        left.modified_nanoseconds == right.modified_nanoseconds &&
        left.changed_seconds == right.changed_seconds &&
        left.changed_nanoseconds == right.changed_nanoseconds &&
        left.content_digest == right.content_digest;
}

bool read_document(const char *path, Document &document, FileStamp &stamp,
                   Error &error, const IoControl *control) noexcept {
    if (was_cancelled(control, error)) return false;
    int directory_fd = -1;
    char *basename = nullptr;
    if (!open_parent(path, directory_fd, &basename, error)) return false;
    Bytes bytes;
    FileStamp candidate_stamp;
    bool ok = read_bytes_at(directory_fd, basename, bytes, candidate_stamp, error,
                            control, kMaxDocumentBytes);
    int saved = errno;
    if (close(directory_fd) < 0 && ok) {
        errno = saved;
        ok = fail_errno(error, "could not close document directory");
    }
    free(basename);
    if (!ok) return false;
    Document candidate;
    if (!decode_document(bytes.data(), bytes.size(), candidate, error)) return false;
    document.swap(candidate);
    stamp = candidate_stamp;
    error.clear();
    return true;
}

bool atomic_save_bytes(const char *path, const uint8_t *bytes, size_t size,
                       const FileStamp &expected, FileStamp &new_stamp,
                       Error &error, const IoControl *control) noexcept {
    if (was_cancelled(control, error)) return false;
    if ((!bytes && size) || size > kMaxAtomicFileBytes) {
        fail(error, size > kMaxAtomicFileBytes ? Code::Limit : Code::Format,
             "atomic file data is null or exceeds the bounded storage limit");
        return false;
    }
    if (was_cancelled(control, error)) return false;

    int directory_fd = -1;
    char *basename = nullptr;
    if (!open_parent(path, directory_fd, &basename, error)) return false;
    FileStamp initial;
    bool ok = check_target_at(directory_fd, basename, initial, error, control);
    if (ok && !same_stamp(initial, expected)) {
        fail(error, Code::Conflict, "document changed outside ReliefMath; original was preserved");
        ok = false;
    }
    if (!ok) {
        close(directory_fd);
        free(basename);
        return false;
    }

    char temporary[96];
    int fd = -1;
    if (!open_temp(directory_fd, temporary, sizeof(temporary), fd, error)) {
        if (fd >= 0) close(fd);
        close(directory_fd);
        free(basename);
        return false;
    }
    bool renamed = false;
    bool file_open = true;
    auto cleanup = [&]() noexcept {
        if (file_open) {
            close(fd);
            file_open = false;
        }
        if (!renamed && unlinkat(directory_fd, temporary, 0) < 0 && errno != ENOENT)
            error.set(Code::Io, "save failed and its temporary file could not be removed");
        close(directory_fd);
        free(basename);
    };

    report_progress(control, IoPhase::Write, 0, size);
    size_t offset = 0;
    while (offset < size) {
        if (was_cancelled(control, error)) { cleanup(); return false; }
        size_t request = size - offset;
        if (request > kIoChunkBytes) request = kIoChunkBytes;
        ssize_t count = write_chunk(fd, bytes + offset, request);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { fail_errno(error, "could not write temporary document"); cleanup(); return false; }
        if (!count) { fail(error, Code::Io, "temporary document write made no progress"); cleanup(); return false; }
        offset += static_cast<size_t>(count);
        report_progress(control, IoPhase::Write, offset, size);
    }
    if (was_cancelled(control, error)) { cleanup(); return false; }
    report_progress(control, IoPhase::FileSync, 0, 1);
    if (sync_file(fd) < 0) { fail_errno(error, "could not sync temporary document"); cleanup(); return false; }
    report_progress(control, IoPhase::FileSync, 1, 1);
    struct stat temp_stat;
    if (fstat(fd, &temp_stat) < 0) { fail_errno(error, "could not inspect synced document"); cleanup(); return false; }
    int close_result = close(fd);
    int close_errno = errno;
    file_open = false;
    if (close_result < 0) {
        errno = close_errno;
        fail_errno(error, "could not close temporary document");
        cleanup();
        return false;
    }

    FileStamp latest;
    if (!check_target_at(directory_fd, basename, latest, error, control)) {
        cleanup();
        return false;
    }
    if (!same_stamp(initial, latest)) {
        fail(error, Code::Conflict, "document changed outside ReliefMath; original was preserved");
        cleanup();
        return false;
    }
    if (was_cancelled(control, error)) { cleanup(); return false; }
    report_progress(control, IoPhase::Rename, 0, 1);
    if (was_cancelled(control, error)) { cleanup(); return false; }
    if (rename_at(directory_fd, temporary, basename) < 0) {
        fail_errno(error, "could not replace document");
        cleanup();
        return false;
    }
    renamed = true;
    struct stat installed_stat;
    if (fstatat(directory_fd, basename, &installed_stat, AT_SYMLINK_NOFOLLOW) < 0 ||
        !S_ISREG(installed_stat.st_mode) ||
        installed_stat.st_dev != temp_stat.st_dev ||
        installed_stat.st_ino != temp_stat.st_ino) {
        error.set(Code::Io, "document was replaced, but its installed stamp could not be confirmed");
        report_progress(control, IoPhase::Rename, 1, 1);
        close(directory_fd);
        free(basename);
        return false;
    }
    FileStamp installed_stamp;
    fill_stat_stamp(installed_stat, digest_bytes(bytes, size), installed_stamp);
    new_stamp = installed_stamp;
    report_progress(control, IoPhase::Rename, 1, 1);

    report_progress(control, IoPhase::DirectorySync, 0, 1);
    if (sync_directory(directory_fd) < 0) {
        const int sync_errno = errno;
        char message[256];
        const char *detail = strerror(sync_errno);
        int count = snprintf(message, sizeof(message),
            "document was replaced, but directory persistence is unconfirmed: %s",
            detail ? detail : "I/O error");
        if (count < 0 || static_cast<size_t>(count) >= sizeof(message))
            error.set(Code::Io, "document was replaced, but directory persistence is unconfirmed");
        else error.set(Code::Io, message);
        errno = sync_errno;
        close(directory_fd);
        free(basename);
        return false;
    }
    report_progress(control, IoPhase::DirectorySync, 1, 1);
    close(directory_fd);
    free(basename);
    error.clear();
    return true;
}

bool atomic_save(const char *path, const Document &document,
                 const FileStamp &expected, FileStamp &new_stamp,
                 Error &error, const IoControl *control) noexcept {
    if (was_cancelled(control, error)) return false;
    Bytes encoded;
    if (!encode_document(document, encoded, error)) return false;
    return atomic_save_bytes(path, encoded.data(), encoded.size(), expected,
                             new_stamp, error, control);
}

#ifdef RM_TESTING
namespace testing {

void set_io_test_control(const IoTestControl &control) noexcept {
    g_test_control = control;
    g_test_write_calls = 0;
}

void reset_io_test_control() noexcept {
    g_test_control = IoTestControl{};
    g_test_write_calls = 0;
}

} // namespace testing
#endif

} // namespace rm
