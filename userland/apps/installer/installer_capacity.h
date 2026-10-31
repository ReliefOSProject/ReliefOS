#ifndef RELIEFOS_INSTALLER_CAPACITY_H
#define RELIEFOS_INSTALLER_CAPACITY_H
#include <stdint.h>

/** @brief Conservative allocated-byte and inode requirements of selected payloads. */
struct installer_capacity {
    uint64_t root_bytes, root_inodes, boot_bytes;
};

/** @brief Measure POSIX file trees without following links or changing target media. */
int installer_capacity_measure(const char *root, const char *boot, int bios, int uefi,
                               struct installer_capacity *out);
/** @brief Check 512-byte partition extents, including filesystem overhead and inodes. */
int installer_capacity_check(const struct installer_capacity *need,
                             uint64_t root_sectors, uint64_t boot_sectors);
#endif
