#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
/* Default installer GPT policy, shared by preview/apply and fresh installation. */
#include "installer_partitions.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/**
 * @brief Calculate the BIOS/ESP/root layout without changing the disk.
 * Caller owns both records. No allocations, locks or global state are used.
 * The root uses all remaining whole MiB before the backup GPT; at most one
 * MiB remains unused, matching the shared block API's MiB sizing contract.
 */
int installer_partition_plan_default(const struct reliefos_block_disk_info *disk,
                                     struct installer_partition_plan *out)
{
    if (!out) return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!disk) return -EINVAL;
    if (disk->sector_size != 512) return -ENOTSUP;
    if (disk->sector_count > INT64_MAX / 512u) return -EOVERFLOW;
    if (disk->sector_count < 655360) return -ENOSPC;
    uint64_t root_sectors = (disk->sector_count - 33u - 266240u) & ~2047ULL;
    if (root_sectors / 2048u > UINT32_MAX) return -EOVERFLOW;
    out->bios = (struct installer_partition_extent){0, 2048, 2048};
    out->esp = (struct installer_partition_extent){1, 4096, 262144};
    out->root = (struct installer_partition_extent){2, 266240, root_sectors};
    return 0;
}

/** @brief Compare public range fields without depending on structure padding. */
static int same_extent(const struct installer_partition_extent *a,
                       const struct installer_partition_extent *b)
{
    return a->index == b->index && a->first_lba == b->first_lba &&
           a->sector_count == b->sector_count;
}

/** @brief Erase only the validated reserved extent after confirmed default disk erasure. */
static int clear_default_bios(const char *disk_path,
                              const struct installer_partition_extent *bios)
{
    int fd = open(disk_path, O_WRONLY | O_EXCL | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -errno;
    unsigned char zero[4096] = {0};
    uint64_t offset = bios->first_lba * 512, left = bios->sector_count * 512;
    int result = 0;
    while (left) {
        size_t amount = left > sizeof(zero) ? sizeof(zero) : (size_t)left;
        ssize_t written = pwrite(fd, zero, amount, (off_t)offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { result = written < 0 ? -errno : -EIO; break; }
        offset += (uint64_t)written;
        left -= (uint64_t)written;
    }
    if (!result && fsync(fd) < 0) result = -errno;
    if (close(fd) < 0 && !result) result = -errno;
    return result;
}

/**
 * @brief Apply only the validated default layout, using production GPT raw I/O.
 * The caller has confirmed erasure and owns the unmounted target. Each block
 * operation checks Linux geometry/read-only/mount state before writing. Errors
 * propagate without claiming rollback of a partly written physical GPT.
 */
int installer_partition_apply_default(const char *disk_path,
                                      const struct installer_partition_plan *plan)
{
    if (!disk_path || !plan) return -EINVAL;
    struct reliefos_block_disk_info disk;
    struct installer_partition_plan expected;
    int result = reliefos_block_get_info(disk_path, &disk);
    if (!result) result = installer_partition_plan_default(&disk, &expected);
    if (result < 0) return result;
    if (!same_extent(&plan->bios, &expected.bios) ||
        !same_extent(&plan->esp, &expected.esp) ||
        !same_extent(&plan->root, &expected.root)) return -EINVAL;
    result = reliefos_block_gpt_initialize(disk_path, 1);
    if (!result) result = clear_default_bios(disk_path, &plan->bios);
    const struct installer_partition_extent *extents[] = {&plan->bios, &plan->esp, &plan->root};
    const uint32_t filesystems[] = {RELIEFOS_BLOCK_FILESYSTEM_UNKNOWN,
        RELIEFOS_BLOCK_FILESYSTEM_FAT32, RELIEFOS_BLOCK_FILESYSTEM_EXT4};
    const uint32_t types[] = {RELIEFOS_BLOCK_GPT_BIOS_BOOT, RELIEFOS_BLOCK_GPT_ESP,
        RELIEFOS_BLOCK_GPT_LINUX};
    const char *names[] = {"RELIEFOS_BIOS", "RELIEFOS_ESP", "RELIEFOS_ROOT"};
    for (unsigned i = 0; !result && i < 3; ++i) {
        uint32_t index;
        result = reliefos_block_gpt_create(disk_path, filesystems[i],
            (uint32_t)(extents[i]->sector_count / 2048u), names[i], &index);
        if (!result && index != extents[i]->index) result = -EIO;
        if (!result) result = reliefos_block_gpt_set_type(disk_path, index, types[i]);
    }
    return result;
}
