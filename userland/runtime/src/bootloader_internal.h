#ifndef RELIEFOS_BOOTLOADER_INTERNAL_H
#define RELIEFOS_BOOTLOADER_INTERNAL_H
#include <reliefos/bootloader.h>
#include <stddef.h>
#define BOOT_PATH_MAX 1024
#ifndef BOOT_GRUB_INSTALL
#define BOOT_GRUB_INSTALL "/usr/sbin/grub-install"
#endif
#ifndef BOOT_GRUB_BIOS_SETUP
#define BOOT_GRUB_BIOS_SETUP "/usr/sbin/grub-bios-setup"
#endif
#ifndef BOOT_GRUB_PROBE
#define BOOT_GRUB_PROBE "/usr/sbin/grub-probe"
#endif
#ifndef BOOT_GRUB_MKIMAGE
#define BOOT_GRUB_MKIMAGE "/usr/bin/grub-mkimage"
#endif

/** @brief Join trusted absolute base and validated relative child into bounded storage. */
int boot_join(char out[BOOT_PATH_MAX], const char *base, const char *relative);
/** @brief Open an absolute directory without following any symlink component. */
int boot_open_directory(const char *path);
/** @brief Open a relative regular file through held safe ancestor descriptors. */
int boot_open_file(int rootfd, const char *relative);
/** @brief Open/create safe destination ancestors; return owned parent or negative errno.
 * @param rootfd Borrowed held boot directory.
 * @param relative Canonical relative file path, never absolute or traversal.
 * @param create Whether missing parent directories may be created.
 * @param leaf Caller-owned 256-byte basename output.
 * @return Owned directory descriptor or negative errno; no symlinks followed.
 */
int boot_open_parent(int rootfd, const char *relative, int create, char leaf[256]);
/** @brief Validate/copy selected trees; rootfd=-1 checks only source payloads. */
int boot_walk_payload(const struct reliefos_boot_install_options *options, int rootfd, int publish);
/** @brief Publish one verified regular file through an exclusive temporary and renameat. */
int boot_publish_file(const struct reliefos_boot_install_options *options, int rootfd,
                      const char *relative, int only_missing);
/** @brief Atomically publish a small caller-owned memory record in the boot tree. */
int boot_publish_record(const struct reliefos_boot_install_options *options, int rootfd,
                        const char *relative, const char *data, size_t length);
/** @brief Copy and byte-verify a borrowed regular descriptor through a synced private file.
 * @param options Borrowed callback settings; source pathname is unused.
 * @param rootfd Borrowed safe boot root.
 * @param relative Canonical destination below rootfd.
 * @param source Borrowed regular descriptor, initially positioned at zero; ends at EOF.
 * @return Zero or negative copy/sync/rename errno; caller retains source ownership.
 */
int boot_publish_descriptor(const struct reliefos_boot_install_options *options, int rootfd,
                            const char *relative, int source);
/** @brief Remove retired second-stage files only after the replacement menu is published. */
int boot_retire_legacy(int rootfd);
/** @brief Run fixed-path upstream grub-install, pumping the caller's optional UI callback. */
int boot_install_bios(const struct reliefos_boot_install_options *options);
#endif
