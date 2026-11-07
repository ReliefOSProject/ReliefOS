#ifndef RELIEFOS_INSTALLER_SWAP_H
#define RELIEFOS_INSTALLER_SWAP_H
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

typedef void (*installer_swap_progress_fn)(uint64_t done,uint64_t total,void *context);
struct installer_swap_identity { dev_t root_dev,file_dev; ino_t root_ino,file_ino; };

#ifdef __cplusplus
extern "C" {
#endif
/** @brief Fully allocate, format and durably publish a new root-owned swapfile.
 * @param target_root Existing exclusive root-owned target directory without
 * group/other write permission, absolute and not slash. The caller must keep
 * its mount and ancestor paths private and stable throughout the transaction.
 * @param bytes Positive whole MiB representable by the swap v1 header.
 * @param progress Optional synchronous progress callback.
 * @param context Borrowed callback context. @return Zero or negative errno.
 * Never activates swap or replaces an existing path. On failure it attempts to
 * remove only names observed to match this invocation and sync the directory.
 * Linux AT_EMPTY_PATH needs CAP_DAC_READ_SEARCH. The namespace precondition
 * excludes concurrent privileged mutation: identity inspection and unlinkat
 * are separate operations, not an atomic conditional unlink.
 */
int installer_swap_create(const char *target_root,uint64_t bytes,
                          installer_swap_progress_fn progress,void *context);
/** @brief Create a swapfile and return its root and file identities for rollback.
 * @param root Private target. @param bytes Whole MiB to allocate. @param progress
 * Optional callback. @param context Borrowed callback context. @param identity
 * Receives identities only on success. @return Zero or negative errno.
 */
int installer_swap_create_owned(const char *root,uint64_t bytes,installer_swap_progress_fn progress,
                                void *context,struct installer_swap_identity *identity);
/** @brief Remove only the previously published swapfile at the same target.
 * @param root Private target. @param identity Identity from create_owned.
 * @return Zero or negative errno, including directory sync and close errors.
 */
int installer_swap_remove_owned(const char *root,const struct installer_swap_identity *identity);
/** @brief Accept only a block disk under a verified SATA/AHCI PCI parent.
 * @param disk_path Existing selected block device. @return Zero or negative
 * errno when its standard sysfs identity is missing, unsupported or invalid.
 */
int installer_swap_storage_supported(const char *disk_path);
/** @brief Resolve a block dev_t below a specified sysfs root for inspection.
 * @param sysroot Sysfs root. @param major_number Device major. @param minor_number
 * Device minor. @return Zero only for an AHCI-class PCI parent, negative errno
 * otherwise. Exposed separately for controlled host topology fixtures.
 */
int installer_swap_storage_supported_at(const char *sysroot,unsigned major_number,unsigned minor_number);
/** @brief Validate the selected payload's bundle and its CONFIG_SWAP setting.
 * @param payload Absolute selected root payload. @return Zero only when its
 * current-release identifies a fully validated swap-enabled bundle.
 */
int installer_swap_bundle_supported(const char *payload);
/** @brief Publish fresh fstab and, when selected, OpenRC swapfile activation.
 * @param root Private newly formatted target root. @param root_uuid Root GPT
 * partition UUID. @param esp_uuid Boot partition UUID. @param enabled Include
 * the already prepared swapfile. @param retain_file Set when configuration
 * was published or fstab reference/rollback durability remains uncertain.
 * @return Zero or negative errno. Failed rollback is reported on stderr.
 */
int installer_swap_configure(const char *root,const char *root_uuid,const char *esp_uuid,
                             bool enabled,bool *retain_file);
/** @brief Prepare optional swap then publish fresh fstab and activation policy.
 * @param root Fresh target. @param root_uuid Root partition UUID. @param esp_uuid
 * Boot partition UUID. @param mib Zero disables swap; otherwise the selected
 * whole MiB. @param progress Optional callback. @param context Callback data.
 * @return Zero or negative errno; failed cleanup is reported on stderr.
 */
int installer_swap_prepare(const char *root,const char *root_uuid,const char *esp_uuid,
                           uint64_t mib,installer_swap_progress_fn progress,void *context);
#ifdef __cplusplus
}
#endif
#endif
