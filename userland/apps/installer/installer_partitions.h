#ifndef RELIEFOS_INSTALLER_PARTITIONS_H
#define RELIEFOS_INSTALLER_PARTITIONS_H
#include <reliefos/blockdev.h>

/** @brief A zero-based GPT slot and its 512-byte logical sector range. */
struct installer_partition_extent {
    uint32_t index;
    uint64_t first_lba, sector_count;
};

/** @brief Validated default geometry, with an unformatted BIOS Boot extent. */
struct installer_partition_plan {
    struct installer_partition_extent bios, esp, root;
};

#ifdef __cplusplus
extern "C" {
#endif
/** @brief Calculate geometry without I/O; zero output on a negative errno result. */
int installer_partition_plan_default(const struct reliefos_block_disk_info *disk,
                                     struct installer_partition_plan *out);
/** @brief Revalidate geometry and replace the selected unmounted disk's GPT. */
int installer_partition_apply_default(const char *disk_path,
                                      const struct installer_partition_plan *plan);
#ifdef __cplusplus
}
#endif
#endif
