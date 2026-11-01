#ifndef RELIEFOS_INSTALLER_CAPACITY_H
#define RELIEFOS_INSTALLER_CAPACITY_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Conservative allocated-byte and inode requirements of selected payloads. */
struct installer_capacity {
    uint64_t root_bytes, root_inodes, boot_bytes;
};

/** @brief Recommend RAM-sized swap in MiB, clamped to 512 through 2048 MiB.
 * @param ram_kib Total RAM in KiB. @return Recommended whole MiB.
 */
uint64_t installer_swap_recommend_mib(uint64_t ram_kib);
/** @brief Recommend from actual total RAM, falling back to 512 MiB on error.
 * @return Recommended whole MiB.
 */
uint64_t installer_swap_recommend_system(void);
/** @brief Check a manually selected MiB size against the v1 swap header limit.
 * @param mib Whole MiB. @return Nonzero if representable, zero otherwise.
 */
int installer_swap_size_valid(uint64_t mib);
/** @brief Parse a complete decimal MiB field without overflow or partial output.
 * @param text NUL-terminated text. @param mib Output changed only on success.
 * @return Zero, negative EINVAL or negative EOVERFLOW.
 */
int installer_swap_parse_mib(const char *text,uint64_t *mib);
/** @brief Add a fully allocated swapfile and one inode using checked arithmetic.
 * @param need Mutable capacity snapshot. @param mib Selected whole MiB.
 * @return Zero or negative errno without a partial change.
 */
int installer_capacity_add_swap(struct installer_capacity *need, uint64_t mib);
/** @brief Measure POSIX file trees without following links or changing target media. */
int installer_capacity_measure(const char *root, const char *boot, int bios, int uefi,
                               struct installer_capacity *out);
/** @brief Check 512-byte partition extents, including filesystem overhead and inodes. */
int installer_capacity_check(const struct installer_capacity *need,
                             uint64_t root_sectors, uint64_t boot_sectors);
#ifdef __cplusplus
}
#endif
#endif
