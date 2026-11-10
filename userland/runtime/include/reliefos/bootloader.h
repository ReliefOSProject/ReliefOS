#ifndef RELIEFOS_BOOTLOADER_H
#define RELIEFOS_BOOTLOADER_H
#include <stdint.h>

#define RELIEFOS_BOOT_BIOS 1u
#define RELIEFOS_BOOT_UEFI 2u

/** @brief Borrowed install inputs; the caller serializes writes to its target. */
struct reliefos_boot_install_options {
    const char *source, *boot_dir, *disk_path;
    uint8_t install_bios, install_uefi, preserve_rollback;
    void (*progress)(void *context);
    void *progress_context;
    /* Optional explicit root identity; otherwise require one GPT Linux root. */
    const char *root_partuuid;
};

#ifdef __cplusplus
extern "C" {
#endif
/** @brief Check selected media files/tools without writing; return negative errno. */
int reliefos_boot_validate_payload(const struct reliefos_boot_install_options *options);
/** @brief Check a whole disk before partitioning; require_space checks an existing BIOS extent. */
int reliefos_boot_validate_disk(const struct reliefos_boot_install_options *options, int require_space);
/** @brief Check boot mount ownership, destination paths and selected BIOS space without writing. */
int reliefos_boot_validate_target(const struct reliefos_boot_install_options *options);
/** @brief Check that the final installed root has the complete media kernel package.
 * @param options Borrowed media/ESP/disk settings, with no structure size change.
 * @param root Absolute final root; slash selects this running system.
 * @return Zero for matching kernel/initramfs/module identity and disk-root ownership,
 * or negative errno without writing either filesystem.
 */
int reliefos_boot_validate_root(const struct reliefos_boot_install_options *options, const char *root);
/** @brief Publish the matching root package, resources, loaders and finally the menu.
 * @param options Borrowed firmware settings; caller serializes firmware installation.
 * @param root Absolute final installed root, already durably copied or APK-upgraded.
 * @return Zero after the checked publication and selected loader stages; negative
 * errno on failure. Completed firmware stages cannot be rolled back atomically.
 * Existing boot slots and versioned payloads are never copied from the media.
 */
int reliefos_boot_install_at_root(const struct reliefos_boot_install_options *options, const char *root);
/** @brief Install into a per-instance boot tree and publish its EFI files on esp.
 * @param options Original ABI options; boot_dir names the instance directory.
 * @param root Final installed root. @param esp Physical mounted ESP root.
 * @return Zero after publication, negative errno on failure. Other EFI loaders
 * and existing fallback executables are preserved; no implicit firmware writes.
 */
int reliefos_boot_install_instance_at_root(const struct reliefos_boot_install_options *options,
                                         const char *root, const char *esp);
/** @brief Create/open a UUID-named instance below a held physical ESP, rejecting links.
 * @param esp Absolute physical ESP. @param uuid Canonical root PARTUUID.
 * @param path Output absolute directory. @param capacity Output size.
 * @return Zero or negative errno; existing unrelated directories survive.
 */
int reliefos_boot_prepare_instance(const char *esp, const char *uuid, char *path, unsigned capacity);
/** @brief Check physical ESP and per-instance ownership without creating files. */
int reliefos_boot_validate_instance(const struct reliefos_boot_install_options *options, const char *esp);
/** @brief Install using this system's final root (/); return negative errno on failure.
 * Offline installers use reliefos_boot_install_at_root for their destination.
 */
int reliefos_boot_install(const struct reliefos_boot_install_options *options);
/** @brief Read installed mode bits; legacy media without a record can identify UEFI only. */
int reliefos_boot_probe(const char *disk_path, const char *boot_dir, uint32_t *modes);
#ifdef __cplusplus
}
#endif
#endif
