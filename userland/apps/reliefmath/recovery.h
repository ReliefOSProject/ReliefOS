#ifndef RELIEFMATH_RECOVERY_H
#define RELIEFMATH_RECOVERY_H

#include "file_io.h"

namespace rm {

/** Resolves the private XDG state recovery directory, creating it if needed. */
bool recovery_directory(Text &output, Error &error) noexcept;

class RecoveryStore;

/** Metadata for one available recovery snapshot; it never names an overwrite target. */
class RecoveryItem {
    friend class RecoveryStore;
    int lock_fd_ = -1;
    FileStamp stamp_{};

public:
    uint64_t id = 0;
    Text title;
    Text original_path_hint;

    RecoveryItem() = default;
    RecoveryItem(const RecoveryItem &) = delete;
    RecoveryItem &operator=(const RecoveryItem &) = delete;
    RecoveryItem(RecoveryItem &&other) noexcept;
    RecoveryItem &operator=(RecoveryItem &&other) noexcept;
    ~RecoveryItem();
};

/**
 * Owns one live recovery lock and a bounded list of stale snapshots leased by
 * this instance until they are loaded, discarded, or the store is destroyed.
 */
class RecoveryStore {
    int directory_fd_ = -1;
    int own_lock_fd_ = -1;
    uint64_t own_id_ = 0;
    FileStamp own_stamp_{};
    Text directory_;
    Text own_path_hint_;
    Array<RecoveryItem> items_;

    RecoveryStore(const RecoveryStore &) = delete;
    RecoveryStore &operator=(const RecoveryStore &) = delete;

public:
    RecoveryStore() = default;
    ~RecoveryStore();

    /** Opens or creates a private, non-symlink recovery directory. */
    bool open(const char *directory, Error &error) noexcept;
    /** Opens the XDG_STATE_HOME or HOME-derived ReliefMath recovery directory. */
    bool open_default(Error &error) noexcept;

    /** Saves a snapshot without an original-path hint. */
    bool save(const Document &document, Error &error,
              const IoControl *control = nullptr) noexcept;
    /** Saves a snapshot with a display-only original-path hint. */
    bool save(const Document &document, const char *original_path_hint,
              Error &error, const IoControl *control = nullptr) noexcept;

    /** Lists valid snapshots not locked by another live instance. */
    bool list(Error &error) noexcept;
    /** Loads a snapshot into a new document state without saving over its hint. */
    bool load(uint64_t id, Document &output, Error &error,
              const IoControl *control = nullptr) noexcept;
    /** Explicitly discards one snapshot and its lock after syncing the directory. */
    bool discard(uint64_t id, Error &error) noexcept;
    /** Discards this instance's snapshot for normal shutdown; a no-op if absent. */
    bool discard_own(Error &error) noexcept;

    const Array<RecoveryItem> &items() const noexcept { return items_; }
    uint64_t current_id() const noexcept { return own_id_; }
};

} // namespace rm

#endif
