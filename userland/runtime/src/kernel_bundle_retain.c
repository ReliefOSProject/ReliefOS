/* SPDX-License-Identifier: Apache-2.0 */
#include "kernel_bundle_internal.h"
#include "bootloader_internal.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/** @brief Admit a single bounded release component before creating cache paths.
 * @param release Borrowed NUL-terminated release string.
 * @return Nonzero for the canonical product alphabet and a valid single component.
 */
static int release_valid(const char *release)
{
    if (!release || !*release || strlen(release) >= 32 || !strcmp(release, ".") || !strcmp(release, "..")) return 0;
    return strspn(release, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._+-") == strlen(release);
}

/** @brief Copy a checked package product without links, with optional repair.
 * @param source Borrowed retained/final root directory.
 * @param target Borrowed distinct destination root directory.
 * @param name Canonical package-owned relative member.
 * @param expected Exact known checksum; NULL hashes the source checksum-list file.
 * @param repair Nonzero permits replacement of differing regular destination bytes.
 * @return Zero after matching bytes or verified publication; negative errno otherwise.
 */
static int copy_product(int source, int target, const char *name, const char *expected, int repair)
{
    int input = boot_open_file(source, name);
    if (input < 0) return input;
    char digest[65]; int result = kernel_bundle_hash_fd(input, digest);
    if (!result && expected && strcmp(digest, expected)) result = -EIO;
    int existing = result ? -1 : boot_open_file(target, name);
    if (!result && existing >= 0) {
        char actual[65]; result = kernel_bundle_hash_fd(existing, actual); close(existing);
        if (!result && !strcmp(digest, actual)) { close(input); return 0; }
        if (!result && !repair) result = -EIO;
    } else if (!result && existing != -ENOENT) result = existing;
    if (!result && lseek(input, 0, SEEK_SET) < 0) result = -errno;
    struct reliefos_boot_install_options options = {0};
    if (!result) result = boot_publish_descriptor(&options, target, name, input);
    close(input);
    return result;
}

/** @brief Copy only the checksum inventory's members, then its checksum list last.
 * @param bundle Borrowed complete source snapshot.
 * @param target Borrowed destination root descriptor.
 * @param repair Whether differing regular destination members may be repaired.
 * @return Zero on complete copies, negative errno on first failure.
 */
static int copy_bundle(const struct kernel_bundle_checked *bundle, int target, int repair)
{
    for (size_t i = 0; i < bundle->count; ++i) {
        int result = copy_product(bundle->root, target, bundle->products[i].name, bundle->products[i].hash, repair);
        if (result) return result;
    }
    char sums[128]; snprintf(sums, sizeof(sums), "%sSHA256SUMS", bundle->prefix);
    return copy_product(bundle->root, target, sums, NULL, repair);
}

/** @brief Sync newly created cache ancestors all the way to the held root.
 * @param root Borrowed installed root directory.
 * @param member Canonical completion-record path whose ancestors already exist.
 * @return Zero after child-before-parent sync, negative errno otherwise.
 */
static int sync_ancestors(int root, const char *member)
{
    char path[256];
    if (strlen(member) >= sizeof(path)) return -ENAMETOOLONG;
    strcpy(path, member);
    for (;;) {
        char leaf[256]; int parent = boot_open_parent(root, path, 0, leaf);
        if (parent < 0) return parent;
        int result = fsync(parent) ? -errno : 0; close(parent);
        if (result) return result;
        char *slash = strrchr(path, '/');
        if (!slash) return 0;
        *slash = 0;
    }
}

/** @brief Encode the cache completion marker bound to module ABI and initramfs.
 * @param bundle Borrowed verified package snapshot.
 * @param marker Caller-owned 131-byte output (130 published bytes plus NUL).
 */
static void marker_bytes(const struct kernel_bundle_checked *bundle, char marker[131])
{
    snprintf(marker, 131, "%s\n%s\n", bundle->identity.module_abi, bundle->identity.initramfs_sha256);
}

int reliefos_kernel_bundle_retain(const char *root, const char *release)
{
    if (!release_valid(release)) return -EINVAL;
    struct kernel_bundle_checked source = {.root = -1}, cache = {.root = -1};
    int result = kernel_bundle_check_open(&source, root, release);
    int target = -1;
    if (result) goto out;
    char path[256], leaf[256];
    snprintf(path, sizeof(path), "var/lib/reliefos/kernel-retained/%s/%s/root/entry", release, source.identity.initramfs_sha256);
    target = boot_open_parent(source.root, path, 1, leaf);
    if (target < 0) { result = target; goto out; }
    result = copy_bundle(&source, target, 0);
    if (!result) result = kernel_bundle_check_at(&cache, target, release);
    if (!result && !reliefos_kernel_slot_equal(&source.identity, &cache.identity)) result = -EIO;
    if (!result) result = kernel_bundle_sync_root(&cache);
    if (!result) {
        snprintf(path, sizeof(path), "var/lib/reliefos/kernel-retained/%s/%s/complete", release, source.identity.initramfs_sha256);
        char marker[131]; marker_bytes(&source, marker);
        struct reliefos_boot_install_options options = {0};
        result = boot_publish_record(&options, source.root, path, marker, 130);
        if (!result) result = sync_ancestors(source.root, path);
    }
out:
    if (target >= 0) close(target);
    kernel_bundle_check_close(&cache); kernel_bundle_check_close(&source);
    return result;
}

/** @brief Check and restore one completed retained copy through held directories.
 * @param variants Borrowed per-release retained-copy directory.
 * @param name Canonical 64-hex initramfs digest directory.
 * @param target Borrowed final installed root.
 * @param release Expected ABI-bound release.
 * @return Zero after durable recovery; negative errno for incomplete/bad copies.
 */
static int restore_variant(int variants, const char *name, int target, const char *release)
{
    int version = openat(variants, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (version < 0) return -errno;
    char marker[130], expected[131];
    int result = kernel_bundle_read_fixed(version, "complete", marker, sizeof(marker));
    int root = result ? -1 : openat(version, "root", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (!result && root < 0) result = -errno;
    struct kernel_bundle_checked cache = {.root = -1}, restored = {.root = -1};
    if (!result) result = kernel_bundle_check_at(&cache, root, release);
    if (!result) {
        marker_bytes(&cache, expected);
        if (memcmp(marker, expected, sizeof(marker)) || strcmp(name, cache.identity.initramfs_sha256)) result = -EINVAL;
    }
    if (!result) result = copy_bundle(&cache, target, 1);
    if (!result) result = kernel_bundle_check_at(&restored, target, release);
    if (!result && !reliefos_kernel_slot_equal(&cache.identity, &restored.identity)) result = -EIO;
    if (!result) result = kernel_bundle_sync_root(&restored);
    kernel_bundle_check_close(&restored); kernel_bundle_check_close(&cache);
    if (root >= 0) close(root);
    close(version);
    return result;
}

int reliefos_kernel_bundle_restore(const char *path, const char *release)
{
    if (!path || !release_valid(release)) return -EINVAL;
    struct kernel_bundle_checked current = {.root = -1};
    int result = kernel_bundle_check_open(&current, path, release);
    kernel_bundle_check_close(&current);
    if (!result) return 0; /* In particular, retain a complete newer initramfs. */
    int root = !strcmp(path, "/") ? open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC) : boot_open_directory(path);
    if (root < 0) return !strcmp(path, "/") ? -errno : root;
    char member[128], leaf[256];
    snprintf(member, sizeof(member), "var/lib/reliefos/kernel-retained/%s/entry", release);
    int variants = boot_open_parent(root, member, 0, leaf);
    if (variants < 0) { close(root); return variants; }
    DIR *directory = fdopendir(variants);
    if (!directory) { result = -errno; close(variants); close(root); return result; }
    result = -ENOENT;
    for (;;) {
        errno = 0; struct dirent *entry = readdir(directory);
        if (!entry) { if (errno) result = -errno; break; }
        if (strlen(entry->d_name) != 64 || strspn(entry->d_name, "0123456789abcdef") != 64) continue;
        int candidate = restore_variant(variants, entry->d_name, root, release);
        if (!candidate) { result = 0; break; }
        result = candidate;
    }
    closedir(directory); close(root);
    return result;
}
