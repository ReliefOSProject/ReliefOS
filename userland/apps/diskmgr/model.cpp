#include "model.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

namespace dm {
bool parse_size(const char *text, uint32_t &value)
{
    if (!text || !*text) return false;
    uint32_t parsed = 0;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p) {
        if (*p < '0' || *p > '9' || parsed > (UINT32_MAX - (*p - '0')) / 10U) return false;
        parsed = parsed * 10U + (*p - '0');
    }
    if (!parsed) return false;
    value = parsed;
    return true;
}

const char *confirmation_word(Action action)
{
    switch (action) {
    case Action::Create: return "CREATE";
    case Action::Format: return "FORMAT";
    case Action::Delete: return "DELETE";
    default: return nullptr;
    }
}

bool confirmation_matches(Action action, const char *text)
{
    const char *word = confirmation_word(action);
    return word && text && !strcmp(word, text);
}

bool Confirmation::accept(Action action, const char *text)
{
    if (!confirmation_matches(action, text)) { reset(); return false; }
    if (armed_ && previous_ == action) { reset(); return true; }
    armed_ = true;
    previous_ = action;
    return false;
}

bool valid_filesystem(uint32_t filesystem)
{
    return filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT4 ||
           filesystem == RELIEFOS_BLOCK_FILESYSTEM_FAT32 ||
           filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXFAT;
}

bool same_disk(const Disk &saved, const Disk &fresh)
{
    return saved.identity_valid && fresh.identity_valid &&
           !strcmp(saved.info.path, fresh.info.path) &&
           saved.info.sector_size == fresh.info.sector_size &&
           saved.info.sector_count == fresh.info.sector_count &&
           saved.node_device == fresh.node_device && saved.node_inode == fresh.node_inode &&
           saved.device_number == fresh.device_number && saved.sequence == fresh.sequence &&
           !memcmp(saved.header, fresh.header, sizeof(saved.header));
}

int action_allowed(const Snapshot &state, Action action, int row, uint32_t uid)
{
    if (action == Action::Refresh) return 0;
    if (uid != 0) return -EACCES;
    if (state.selected_disk < 0 || static_cast<uint32_t>(state.selected_disk) >= state.disk_count ||
        state.disk_count > RELIEFOS_BLOCK_MAX_DISKS || state.partition_count > RELIEFOS_BLOCK_MAX_PARTITIONS)
        return -EINVAL;
    const Disk &disk = state.disks[state.selected_disk];
    if (disk.inspect_error) return disk.inspect_error;
    if (state.mount_error) return state.mount_error;
    if (state.partition_error) return state.partition_error;
    if (disk.info.sector_size != 512) return -ENOTSUP;
    if (action == Action::Create) return disk.read_only ? -EROFS : disk.mounted ? -EBUSY : 0;
    if (row < 0 || static_cast<uint32_t>(row) >= state.partition_count) return -EINVAL;
    const Partition &part = state.partitions[row];
    if (part.system) return -EPERM;
    if (action == Action::Unmount)
        return part.mount_count == 1 ? 0 : part.mount_count ? -EBUSY : -EINVAL;
    if (part.mount_count) return -EBUSY;
    if (action == Action::Mount) {
        if (part.info.gpt_type == RELIEFOS_BLOCK_GPT_BIOS_BOOT) return -ENOTSUP;
        return valid_filesystem(part.info.filesystem) || part.info.filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT2
                   ? 0 : -ENOTSUP;
    }
    if (disk.read_only || part.read_only) return -EROFS;
    if (action == Action::Format)
        return part.info.gpt_type == RELIEFOS_BLOCK_GPT_BIOS_BOOT ? -EPERM : 0;
    if (action == Action::Delete) return disk.mounted ? -EBUSY : 0;
    return -EINVAL;
}

void format_size(char *text, size_t capacity, uint64_t bytes)
{
    if (bytes >= (1ULL << 40)) {
        snprintf(text, capacity, "%llu.%02llu TiB", static_cast<unsigned long long>(bytes >> 40),
                 static_cast<unsigned long long>((bytes % (1ULL << 40)) * 100 / (1ULL << 40)));
    } else if (bytes >= (1ULL << 30)) {
        snprintf(text, capacity, "%llu.%02llu GiB", static_cast<unsigned long long>(bytes >> 30),
                 static_cast<unsigned long long>((bytes % (1ULL << 30)) * 100 / (1ULL << 30)));
    } else if (bytes >= (1ULL << 20)) {
        snprintf(text, capacity, "%llu MiB", static_cast<unsigned long long>(bytes >> 20));
    } else snprintf(text, capacity, "%llu B", static_cast<unsigned long long>(bytes));
}
}
