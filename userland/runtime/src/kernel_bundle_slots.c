/* SPDX-License-Identifier: Apache-2.0 */
#include "kernel_bundle_internal.h"
#include "bootloader_internal.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/** @brief Verify a record's exact bytes/checksum and both referenced boot files.
 * @param root Borrowed held ESP directory.
 * @param name Literal A or B selected by the caller.
 * @param slot Caller-owned identity output, only usable on success.
 * @return Zero on complete slot, negative errno on invalid/incomplete contents.
 */
static int validate_slot(int root, char name, struct reliefos_kernel_slot *slot)
{
    unsigned char record[RELIEFOS_KERNEL_SLOT_SIZE]; char path[192], digest[65], expected[400], checksum[400];
    snprintf(path, sizeof(path), "reliefos/state/kernel-%c.env", name);
    int result = kernel_bundle_read_fixed(root, path, record, sizeof(record));
    if (!result) result = reliefos_kernel_slot_decode(record, sizeof(record), slot);
    if (!result) result = kernel_bundle_hash_bytes(record, sizeof(record), digest);
    if (result) return result;
    int length = snprintf(expected, sizeof(expected), "%s  kernel-%c.env\n", digest, name);
    snprintf(path, sizeof(path), "reliefos/state/kernel-%c.sha256", name);
    result = kernel_bundle_read_fixed(root, path, checksum, (size_t)length);
    if (result || memcmp(checksum, expected, (size_t)length)) return result ? result : -EINVAL;
    char version[160];
    result = reliefos_kernel_boot_directory(slot, version, sizeof(version));
    if (result) return result;
    const char *files[] = {"kernel.sys", "initramfs.cpio"};
    const char *hashes[] = {slot->kernel_sha256, slot->initramfs_sha256};
    for (size_t i = 0; i < 2; ++i) {
        snprintf(path, sizeof(path), "%s/%s", version, files[i]);
        int fd = boot_open_file(root, path);
        if (fd < 0) return fd;
        result = kernel_bundle_hash_fd(fd, digest); close(fd);
        if (result || strcmp(digest, hashes[i])) return result ? result : -EINVAL;
    }
    snprintf(path, sizeof(path), "%s/boot.env", version);
    result = kernel_bundle_read_fixed(root, path, record, sizeof(record));
    struct reliefos_kernel_slot template;
    if (!result) result = reliefos_kernel_slot_decode(record, sizeof(record), &template);
    if (result || template.generation != 1) return result ? result : -EINVAL;
    template.generation = slot->generation;
    if (!reliefos_kernel_slot_equal(&template, slot)) return -EINVAL;
    result = kernel_bundle_hash_bytes(record, sizeof(record), digest);
    if (result) return result;
    length = snprintf(expected, sizeof(expected), "%s  kernel.sys\n%s  initramfs.cpio\n%s  boot.env\n",
                      slot->kernel_sha256, slot->initramfs_sha256, digest);
    snprintf(path, sizeof(path), "%s/SHA256SUMS", version);
    result = kernel_bundle_read_fixed(root, path, checksum, (size_t)length);
    return result ? result : memcmp(checksum, expected, (size_t)length) ? -EINVAL : 0;
}

int kernel_bundle_select(int root, struct reliefos_kernel_slot *selected, char *name)
{
    if (!selected) return -EINVAL;
    struct reliefos_kernel_slot slots[2];
    int valid[] = {!validate_slot(root, 'A', &slots[0]), !validate_slot(root, 'B', &slots[1])};
    if (!valid[0] && !valid[1]) return -ENOENT;
    if (valid[0] && valid[1] && slots[0].generation == slots[1].generation &&
        !reliefos_kernel_slot_equal(&slots[0], &slots[1])) return -EUCLEAN;
    unsigned index = !valid[0] || (valid[1] && slots[1].generation > slots[0].generation);
    *selected = slots[index];
    if (name) *name = index ? 'B' : 'A';
    return 0;
}
