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
/** @brief Publish resources, selected loaders, mode record and finally the boot menu. */
int reliefos_boot_install(const struct reliefos_boot_install_options *options);
/** @brief Read installed mode bits; legacy media without a record can identify UEFI only. */
int reliefos_boot_probe(const char *disk_path, const char *boot_dir, uint32_t *modes);
#ifdef __cplusplus
}
#endif
#endif
