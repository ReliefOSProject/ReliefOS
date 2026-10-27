/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFOS_KERNEL_BUNDLE_H
#define RELIEFOS_KERNEL_BUNDLE_H
#include <stddef.h>
#include <stdint.h>

#define RELIEFOS_KERNEL_SLOT_SIZE 1024u
#define RELIEFOS_KERNEL_SLOT_GENERATION_MAX 2147483647u

/* Fixed-size scalar boot identity; contains no borrowed paths or executable text. */
struct reliefos_kernel_slot {
    uint32_t generation;
    char release[32], module_abi[65], kernel_sha256[65], initramfs_sha256[65];
};

#ifdef __cplusplus
extern "C" {
#endif
/** @brief Encode a canonical upstream GRUB environment block.
 * @param slot Borrowed complete identity with generation in 1..INT32_MAX.
 * @param output Caller-owned storage of exactly RELIEFOS_KERNEL_SLOT_SIZE bytes.
 * @return Zero on success, negative EINVAL on unsafe/unbound identifiers.
 */
int reliefos_kernel_slot_encode(const struct reliefos_kernel_slot *slot,
                                unsigned char output[RELIEFOS_KERNEL_SLOT_SIZE]);
/** @brief Decode exactly one bounded environment record without executing it.
 * @param input Borrowed record bytes; must use the exact canonical variable order.
 * @param length Exact available byte count; must be RELIEFOS_KERNEL_SLOT_SIZE.
 * @param slot Caller-owned output, set only after complete validation.
 * @return Zero on success, negative EINVAL on malformed, noncanonical or incomplete bytes.
 */
int reliefos_kernel_slot_decode(const void *input, size_t length, struct reliefos_kernel_slot *slot);
/** @brief Compare complete slot identities including their publication generations.
 * @param a Borrowed validated identity.
 * @param b Borrowed validated identity.
 * @return Nonzero only when all five canonical fields agree.
 */
int reliefos_kernel_slot_equal(const struct reliefos_kernel_slot *a, const struct reliefos_kernel_slot *b);
/** @brief Derive an immutable boot directory for a complete kernel/early-userspace pair.
 * @param slot Borrowed complete validated identity, checked again by this call.
 * @param output Caller-owned relative path buffer, including its terminator.
 * @param capacity Available output bytes; 160 bytes is sufficient.
 * @return Zero, negative EINVAL for malformed input or ENAMETOOLONG for capacity.
 * The full initramfs digest distinguishes OS policy updates with the same uname
 * release; /lib/modules continues to use the kernel's unchanged release ABI.
 */
int reliefos_kernel_boot_directory(const struct reliefos_kernel_slot *slot, char *output, size_t capacity);
/** @brief Check the installed package's complete version identity without writing.
 * @param root Absolute final root containing the versioned kernel and module tree.
 * @param release Expected ABI-bound release.
 * @return Zero on complete checksum/ABI/module membership, negative errno otherwise.
 */
int reliefos_kernel_bundle_validate(const char *root, const char *release);
/** @brief Read a complete package identity only after validating all its members.
 * @param root Absolute installed or private staging root.
 * @param release Expected ABI-bound release.
 * @param identity Caller-owned scalar output, changed only on success.
 * @return Zero or negative errno; performs no publication or package mutation.
 */
int reliefos_kernel_bundle_describe(const char *root, const char *release, struct reliefos_kernel_slot *identity);
/** @brief Save a verified independent copy before APK can remove old owned files.
 * @param root Absolute installed root, nullable values are rejected.
 * @param release Expected installed kernel release, not a pathname.
 * @return Zero after the immutable retained bundle and completion record are
 * verified and synced; negative errno aborts the calling package transaction.
 * Copies only checksum-listed package products, never user modules or /etc.
 */
int reliefos_kernel_bundle_retain(const char *root, const char *release);
/** @brief Restore a removed/incomplete package from a complete retained copy.
 * @param root Absolute installed root to recover, including slash for this OS.
 * @param release Expected kernel release; other releases and unlisted files survive.
 * @return Zero for an already complete root or verified durable recovery;
 * negative errno if no complete copy exists or any path/copy/check fails.
 * No files are deleted. A complete newer early-userspace package for the same
 * kernel is left intact. Callers serialize this operation with APK transactions.
 */
int reliefos_kernel_bundle_restore(const char *root, const char *release);
/** @brief Validate the final root's complete kernel package, then publish a matched boot version.
 * @param root Absolute final installed root, including /lib/modules and its versioned kernel payload.
 * @param boot Absolute mounted ESP directory; must be distinct from root and cannot be slash.
 * @param release Expected package release; binds the kernel, initramfs and complete module tree.
 * @return Zero for an idempotent existing selection or a verified publication; negative errno otherwise.
 * Writes immutable new version files first and only then the inactive preallocated
 * record/checksum. A failed final sync may leave either complete version selected;
 * the previously valid slot and its payload are retained. This function does not
 * install firmware loaders, alter the GRUB script, update APK or delete old modules.
 */
int reliefos_kernel_bundle_publish(const char *root, const char *boot, const char *release);
#ifdef __cplusplus
}
#endif
#endif
