/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFOS_KERNEL_BUNDLE_INTERNAL_H
#define RELIEFOS_KERNEL_BUNDLE_INTERNAL_H
#include <reliefos/kernel_bundle.h>
#include <stddef.h>

struct kernel_bundle_product { char *name; char hash[65]; int ordered; };
struct kernel_bundle_checked {
    int root;
    struct reliefos_kernel_slot identity;
    struct kernel_bundle_product *products;
    size_t count;
    char prefix[96], modules[64];
};

/** @brief Hash exactly the regular file held by fd; seek to its start first.
 * @param fd Borrowed readable regular descriptor; final offset is at EOF.
 * @param output Caller-owned 65-byte lowercase digest storage.
 * @return Zero on success, negative errno or EIO on hash failure.
 */
int kernel_bundle_hash_fd(int fd, char output[65]);
/** @brief Hash an exact memory record with the existing Mbed TLS SHA-256 implementation.
 * @param bytes Borrowed readable input; nullable only for zero size.
 * @param size Exact input byte count.
 * @param output Caller-owned 65-byte lowercase digest storage.
 * @return Zero on success, negative EINVAL/EIO on failure.
 */
int kernel_bundle_hash_bytes(const void *bytes, size_t size, char output[65]);
/** @brief Read one exact-size, safe regular metadata member.
 * @param root Borrowed directory descriptor.
 * @param name Canonical relative product name.
 * @param output Caller-owned buffer of size bytes.
 * @param size Required exact file length.
 * @return Zero on exact data, negative errno/EINVAL on any mismatch.
 */
int kernel_bundle_read_fixed(int root, const char *name, void *output, size_t size);
/** @brief Validate and open the complete root-side signed-package payload.
 * @param bundle Zero-initialized output with root=-1; owns successful allocations.
 * @param root Absolute source root; slash is allowed for read-only installed products.
 * @param release Expected ABI-bound release, never interpreted as a path.
 * @return Zero on complete identity/checksum/module membership, negative errno otherwise.
 */
int kernel_bundle_check_open(struct kernel_bundle_checked *bundle, const char *root, const char *release);
/** @brief Validate a package through a held safe directory, retaining a duplicate fd.
 * @param bundle Initialized output with root=-1; owns partial allocations even on error.
 * @param root Borrowed directory descriptor; no displayed pathname is reopened.
 * @param release Expected canonical ABI-bound release.
 * @return Zero or negative validation/I/O errno; close bundle after either outcome.
 */
int kernel_bundle_check_at(struct kernel_bundle_checked *bundle, int root, const char *release);
/** @brief Release a checked root and its product inventory after either outcome.
 * @param bundle Caller-owned snapshot; does not remove or alter any files.
 */
void kernel_bundle_check_close(struct kernel_bundle_checked *bundle);
/** @brief Sync and recheck every final-root product before publishing boot selection.
 * @param bundle Borrowed validated package; caller serializes package updates.
 * @return Zero after files and child-before-parent directory sync, negative errno otherwise.
 */
int kernel_bundle_sync_root(const struct kernel_bundle_checked *bundle);
/** @brief Select complete, checksum-bound records and their exact immutable payloads.
 * @param root Borrowed held ESP directory.
 * @param selected Caller-owned complete identity output.
 * @param name Optional output A/B.
 * @return Zero, ENOENT if neither is complete, EUCLEAN on equal-generation conflict, or negative errno.
 */
int kernel_bundle_select(int root, struct reliefos_kernel_slot *selected, char *name);
#endif
