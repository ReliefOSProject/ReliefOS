#ifndef RELIEFOS_DISKMGR_MODEL_H
#define RELIEFOS_DISKMGR_MODEL_H

extern "C" {
#include <reliefos/blockdev.h>
}
#include <stddef.h>
#include <stdint.h>

namespace dm {
constexpr size_t MountPathSize = 256;
enum class Action : uint32_t { Refresh, Create, Format, Delete, Mount, Unmount };

struct Disk {
    reliefos_block_disk_info info;
    uint8_t header[512];
    uint64_t node_device, node_inode, device_number, sequence;
    bool identity_valid;
    bool read_only;
    bool mounted;
    int inspect_error;
};
struct Partition {
    reliefos_block_partition info;
    char uuid[37];
    char mount_path[MountPathSize];
    uint32_t mount_count;
    bool system;
    bool read_only;
};
struct Snapshot {
    Disk disks[RELIEFOS_BLOCK_MAX_DISKS];
    Partition partitions[RELIEFOS_BLOCK_MAX_PARTITIONS];
    uint32_t disk_count;
    uint32_t partition_count;
    int selected_disk;
    int enumeration_error;
    int partition_error;
    int mount_error;
};
struct Request {
    Action action;
    Disk disk;
    Partition partition;
    uint32_t filesystem;
    uint32_t size_mib;
    char label[RELIEFOS_BLOCK_NAME_LEN];
};
struct Reply {
    uint32_t magic;
    int status;
    bool created;
    char result_path[RELIEFOS_BLOCK_PATH_LEN];
    Snapshot snapshot;
};

/** @brief Parse a positive decimal MiB count without truncation or overflow. */
bool parse_size(const char *text, uint32_t &value);
/** @brief Accept only the exact destructive-operation word, never whitespace variants. */
bool confirmation_matches(Action action, const char *text);
/** @brief Return the untranslated confirmation word for a destructive operation. */
const char *confirmation_word(Action action);
/** @brief Validate selection and current permissions/mount protections; return negative errno. */
int action_allowed(const Snapshot &snapshot, Action action, int partition, uint32_t uid);
/** @brief Identify supported new filesystem choices; legacy ext2 remains readable/mountable. */
bool valid_filesystem(uint32_t filesystem);
/** @brief Match the captured GPT/device identity, rejecting replaced media and stale tables. */
bool same_disk(const Disk &saved, const Disk &fresh);
/** @brief Format a byte count into a bounded human-readable binary-unit string. */
void format_size(char *text, size_t capacity, uint64_t bytes);

class Confirmation {
    Action previous_ = Action::Refresh;
    bool armed_ = false;
public:
    /** @brief Disarm whenever a dialog field, target or operation changes. */
    void reset() { armed_ = false; previous_ = Action::Refresh; }
    /** @brief Require two separate matching submissions; a mismatch clears the first. */
    bool accept(Action action, const char *text);
    /** @brief Report whether the first correct submission has been received. */
    bool armed() const { return armed_; }
};
/** @brief Run a request in the private worker process, then return a fresh device snapshot. */
void perform_request(const Request &request, Reply &reply);
}
#endif
