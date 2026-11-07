#ifndef RELIEFMATH_FILE_IO_H
#define RELIEFMATH_FILE_IO_H

#include "document.h"

namespace rm {

// Protocol exporters currently cap PNG/RGBA at 64 MiB. Keep room for a
// complete raw artifact while .rmath remains independently capped at 16 MiB.
constexpr size_t kMaxAtomicFileBytes = 128u * 1024u * 1024u;

struct FileStamp {
    bool exists = false;
    uint64_t device = 0;
    uint64_t inode = 0;
    uint64_t size = 0;
    int64_t modified_seconds = 0;
    int64_t modified_nanoseconds = 0;
    int64_t changed_seconds = 0;
    int64_t changed_nanoseconds = 0;
    uint64_t content_digest = 0;
};

enum class IoPhase : uint8_t {
    Read,
    Write,
    FileSync,
    Rename,
    DirectorySync
};

/**
 * Optional same-thread callbacks for bounded synchronous storage operations.
 * Progress callbacks are observational; cancellation is sampled between I/O
 * chunks and once more immediately before an atomic save's rename.
 */
struct IoControl {
    bool (*cancelled)(void *context) = nullptr;
    void *context = nullptr;
    void (*progress)(void *context, IoPhase phase, size_t completed,
                     size_t total) = nullptr;
};

/** Computes a stat-and-content stamp, rejecting links and non-regular files. */
bool file_stamp(const char *path, FileStamp &stamp, Error &error,
                const IoControl *control = nullptr) noexcept;

/** Compares presence, supported stat fields, and the content digest. */
bool same_stamp(const FileStamp &left, const FileStamp &right) noexcept;

/** Reads and validates into temporary state; cancellation or failure preserves outputs. */
bool read_document(const char *path, Document &document, FileStamp &stamp,
                   Error &error, const IoControl *control = nullptr) noexcept;

/** Atomically replaces path with bounded raw bytes using the same stamp contract. */
bool atomic_save_bytes(const char *path, const uint8_t *bytes, size_t size,
                       const FileStamp &expected, FileStamp &new_stamp,
                       Error &error, const IoControl *control = nullptr) noexcept;

/**
 * Atomically replaces path only if it still matches expected. A false result
 * with Code::Io may report that rename completed but directory persistence was
 * not confirmed; the error text distinguishes that post-rename state.
 */
bool atomic_save(const char *path, const Document &document,
                 const FileStamp &expected, FileStamp &new_stamp,
                 Error &error, const IoControl *control = nullptr) noexcept;

#ifdef RM_TESTING
namespace testing {
struct IoTestControl {
    size_t max_write_size = 0;
    unsigned write_eintr_count = 0;
    int fail_write_after = -1;
    int fail_write_errno = 28;
    int fail_file_sync_errno = 0;
    int fail_rename_errno = 0;
    int fail_directory_sync_errno = 0;
    int fail_read_open_errno = 0;
    int fail_temp_open_errno = 0;
};
void set_io_test_control(const IoTestControl &control) noexcept;
void reset_io_test_control() noexcept;
} // namespace testing
#endif

} // namespace rm

#endif
