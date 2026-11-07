/* SPDX-License-Identifier: Apache-2.0 */
#include "kernel_bundle_internal.h"
#include "bootloader_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/** @brief Hash an existing immutable member, distinguishing absence from corruption.
 * @param root Borrowed ESP root.
 * @param path Safe relative path.
 * @param expected Required lowercase digest.
 * @return Zero for matching bytes, negative ENOENT if absent, negative errno otherwise.
 */
static int existing_product(int root, const char *path, const char *expected)
{
    int fd = boot_open_file(root, path);
    if (fd < 0) return fd;
    char digest[65]; int result = kernel_bundle_hash_fd(fd, digest); close(fd);
    return result ? result : strcmp(digest, expected) ? -EEXIST : 0;
}

/** @brief Copy a missing immutable version member without rewriting a published inode.
 * @param bundle Borrowed root-side checked package, still pinned by the caller.
 * @param root Borrowed ESP root.
 * @param name Constant basename inside the package version.
 * @param expected Required verified root-side digest.
 * @return Zero after destination readback, negative errno on source change or existing mismatch.
 */
static int copy_version(const struct kernel_bundle_checked *bundle, int root,
                        const char *name, const char *expected)
{
    char version[160], target[192], source[128];
    int result = reliefos_kernel_boot_directory(&bundle->identity, version, sizeof(version));
    if (result) return result;
    snprintf(target, sizeof(target), "%s/%s", version, name);
    result = existing_product(root, target, expected);
    if (result != -ENOENT) return result;
    snprintf(source, sizeof(source), "%s%s", bundle->prefix, name);
    int fd = boot_open_file(bundle->root, source);
    if (fd < 0) return fd;
    char digest[65]; result = kernel_bundle_hash_fd(fd, digest);
    if (!result && strcmp(digest, expected)) result = -EIO;
    if (!result && lseek(fd, 0, SEEK_SET) < 0) result = -errno;
    struct reliefos_boot_install_options options = {0};
    if (!result) result = boot_publish_descriptor(&options, root, target, fd);
    close(fd);
    if (!result) result = existing_product(root, target, expected);
    return result;
}

/** @brief Publish deterministic version metadata once, reusing only identical bytes.
 * @param root Borrowed ESP root.
 * @param path Canonical immutable destination.
 * @param bytes Borrowed complete data.
 * @param size Exact byte count.
 * @return Zero after exact readback; an existing different file is EEXIST.
 */
static int immutable_record(int root, const char *path, const char *bytes, size_t size)
{
    char digest[65]; int result = kernel_bundle_hash_bytes(bytes, size, digest);
    if (result) return result;
    result = existing_product(root, path, digest);
    if (result != -ENOENT) return result;
    struct reliefos_boot_install_options options = {0};
    result = boot_publish_record(&options, root, path, bytes, size);
    return result ? result : existing_product(root, path, digest);
}

/** @brief Sync an already created safe directory and its namespace entries.
 * @param root Borrowed ESP root.
 * @param relative Directory path expressed through a harmless virtual leaf.
 * @return Zero or negative open/sync errno; never creates a directory.
 */
static int sync_parent(int root, const char *relative)
{
    char leaf[256]; int fd = boot_open_parent(root, relative, 0, leaf);
    if (fd < 0) return fd;
    int result = fsync(fd) ? -errno : 0; close(fd);
    return result;
}

/** @brief Complete and sync the immutable version before touching either selector.
 * @param bundle Borrowed checked final root payload.
 * @param root Borrowed ESP root.
 * @return Zero after all files, checksums and ancestor entries are synced; negative errno otherwise.
 */
static int publish_version(const struct kernel_bundle_checked *bundle, int root)
{
    const char *names[] = {"kernel.sys", "initramfs.cpio", "kerneldebug.sys", "boot.env"};
    const char *hashes[4] = {bundle->identity.kernel_sha256, bundle->identity.initramfs_sha256, NULL, NULL};
    char source[128];
    for (size_t i = 2; i < 4; ++i) {
        snprintf(source, sizeof(source), "%s%s", bundle->prefix, names[i]);
        for (size_t j = 0; j < bundle->count; ++j)
            if (!strcmp(source, bundle->products[j].name)) { hashes[i] = bundle->products[j].hash; break; }
        if (!hashes[i]) return -EINVAL;
    }
    for (size_t i = 0; i < 4; ++i) {
        int result = copy_version(bundle, root, names[i], hashes[i]);
        if (result) return result;
    }
    char checksum[400], path[192], version[160];
    /* Match the original native-media format exactly; the debugger is verified
     * before publication but is not a GRUB-loaded member of this checksum list. */
    int length = snprintf(checksum, sizeof(checksum), "%s  kernel.sys\n%s  initramfs.cpio\n%s  boot.env\n", hashes[0], hashes[1], hashes[3]);
    int result = reliefos_kernel_boot_directory(&bundle->identity, version, sizeof(version));
    if (result) return result;
    snprintf(path, sizeof(path), "%s/SHA256SUMS", version);
    result = immutable_record(root, path, checksum, (size_t)length);
    snprintf(path, sizeof(path), "%s/entry", version);
    if (!result) result = sync_parent(root, path);
    if (!result) result = sync_parent(root, version);
    if (!result) result = sync_parent(root, "reliefos/versions/entry");
    if (!result) result = sync_parent(root, "reliefos/entry");
    if (!result && fsync(root)) result = -errno;
    return result;
}

/** @brief Create a missing fixed-size inactive file without truncating any inode.
 * @param parent Borrowed safe state directory.
 * @param leaf Validated constant slot basename.
 * @param size Required fixed length, at most one environment block.
 * @param repair Permit extending an interrupted inactive allocation; never truncate.
 * @return Zero for an exact regular file or a completed, synced zero extension.
 */
static int preallocate(int parent, const char *leaf, size_t size, int repair)
{
    struct stat status;
    int present = !fstatat(parent, leaf, &status, AT_SYMLINK_NOFOLLOW);
    if (present && (!S_ISREG(status.st_mode) || status.st_size < 0 || status.st_size > (off_t)size)) return -EINVAL;
    if (present && status.st_size == (off_t)size) return 0;
    if (present && !repair) return -EINVAL;
    if (!present && errno != ENOENT) return -errno;
    int fd = openat(parent, leaf, O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC |
                    (present ? 0 : O_CREAT | O_EXCL), 0644);
    if (fd < 0) return -errno;
    int result = fstat(fd, &status) ? -errno : !S_ISREG(status.st_mode) || status.st_size < 0 || status.st_size > (off_t)size ? -EINVAL : 0;
    unsigned char zero[RELIEFOS_KERNEL_SLOT_SIZE] = {0};
    size_t used = result ? 0 : (size_t)status.st_size;
    while (!result && used < size) {
        ssize_t count = pwrite(fd, zero + used, size - used, (off_t)used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { result = count ? -errno : -EIO; break; }
        used += (size_t)count;
    }
    if (!result && fsync(fd)) result = -errno;
    if (close(fd) && !result) result = -errno;
    if (!result && fsync(parent)) result = -errno;
    return result;
}

/** @brief Replace bytes of a fixed preallocated inactive record, then verify them.
 * @param parent Borrowed state directory, protected by the update lock.
 * @param leaf Constant inactive filename.
 * @param bytes Borrowed exact new bytes.
 * @param size Required fixed length; cannot exceed one environment block.
 * @return Zero after sync/readback, negative errno otherwise; never renames or truncates.
 */
static int store_fixed(int parent, const char *leaf, const void *bytes, size_t size)
{
    int fd = openat(parent, leaf, O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -errno;
    struct stat status; int result = fstat(fd, &status) ? -errno :
        !S_ISREG(status.st_mode) || status.st_size != (off_t)size ? -EINVAL : 0;
    size_t used = 0;
    while (!result && used < size) {
        ssize_t count = pwrite(fd, (const unsigned char *)bytes + used, size - used, (off_t)used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { result = count ? -errno : -EIO; break; }
        used += (size_t)count;
    }
    if (!result && fsync(fd)) result = -errno;
    unsigned char readback[RELIEFOS_KERNEL_SLOT_SIZE]; used = 0;
    while (!result && used < size) {
        ssize_t count = pread(fd, readback + used, size - used, (off_t)used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { result = count ? -errno : -EIO; break; }
        used += (size_t)count;
    }
    if (!result && memcmp(bytes, readback, size)) result = -EIO;
    if (close(fd) && !result) result = -errno;
    return result;
}

/** @brief Admit only zero allocation or candidate bytes from an interrupted initial store.
 * @param state Borrowed locked state directory.
 * @param name Constant record basename.
 * @param expected Exact generation-one bytes for the completely validated root.
 * @param size Required complete record length, at most one environment block.
 * @return Zero for missing/compatible bytes; negative errno for foreign or unsafe state.
 * No valid previous slot exists at this point. Nonzero bytes of another identity
 * or a later generation are not overwritten by guessing a fresh installation.
 */
static int initial_record(int state, const char *name, const void *expected, size_t size)
{
    int fd = boot_open_file(state, name);
    if (fd < 0) return fd == -ENOENT ? 0 : fd;
    struct stat status; unsigned char bytes[RELIEFOS_KERNEL_SLOT_SIZE];
    int result = fstat(fd, &status) ? -errno : status.st_size < 0 || status.st_size > (off_t)size ? -EUCLEAN : 0;
    size_t used = 0;
    while (!result && used < (size_t)status.st_size) {
        ssize_t count = read(fd, bytes + used, (size_t)status.st_size - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { result = count ? -errno : -EIO; break; }
        used += (size_t)count;
    }
    close(fd);
    const unsigned char *wanted = expected;
    for (size_t i = 0; !result && i < used; ++i)
        if (bytes[i] && bytes[i] != wanted[i]) result = -EUCLEAN;
    return result;
}

/** @brief Validate resumable first-install allocation without destroying unknown state.
 * @param state Borrowed locked state directory with no valid boot selection.
 * @param identity Validated generation-one candidate from the final installed root.
 * @return Zero only if all four records are missing, zero or candidate-compatible.
 */
static int initial_state(int state, const struct reliefos_kernel_slot *identity)
{
    unsigned char record[RELIEFOS_KERNEL_SLOT_SIZE]; char digest[65], name[32], checksum[80];
    int result = reliefos_kernel_slot_encode(identity, record);
    if (!result) result = kernel_bundle_hash_bytes(record, sizeof(record), digest);
    for (char slot = 'A'; !result && slot <= 'B'; ++slot) {
        snprintf(name, sizeof(name), "kernel-%c.env", slot);
        result = initial_record(state, name, record, sizeof(record));
        int length = snprintf(checksum, sizeof(checksum), "%s  %s\n", digest, name);
        snprintf(name, sizeof(name), "kernel-%c.sha256", slot);
        if (!result) result = initial_record(state, name, checksum, (size_t)length);
    }
    return result;
}

int reliefos_kernel_bundle_publish(const char *root_path, const char *boot, const char *release)
{
    const char *stage = "validate installed package";
    struct kernel_bundle_checked bundle = {.root = -1};
    int result = kernel_bundle_check_open(&bundle, root_path, release);
    int root = -1, state = -1, lock_fd = -1;
    if (result) goto out; /* A bad package cannot create even the lock directory. */
    stage = "open boot directory";
    root = boot_open_directory(boot);
    if (root < 0) { result = root; goto out; }
    struct stat source_status, target_status;
    if (fstat(root, &target_status) || fstat(bundle.root, &source_status)) { result = -errno; goto out; }
    if (source_status.st_dev == target_status.st_dev && source_status.st_ino == target_status.st_ino) { result = -EINVAL; goto out; }
    stage = "synchronize installed root products";
    result = kernel_bundle_sync_root(&bundle);
    if (result) goto out;
    stage = "open update lock";
    char leaf[256]; state = boot_open_parent(root, "reliefos/state/.update.lock", 1, leaf);
    if (state < 0) { result = state; goto out; }
    lock_fd = openat(state, leaf, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    if (lock_fd < 0) { result = -errno; goto out; }
    struct stat lock_status;
    if (fstat(lock_fd, &lock_status) || !S_ISREG(lock_status.st_mode)) { result = -EINVAL; goto out; }
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    stage = "acquire POSIX update lock";
    if (fcntl(lock_fd, F_SETLK, &lock)) { result = errno == EACCES || errno == EAGAIN ? -EBUSY : -errno; goto out; }
    struct reliefos_kernel_slot previous; char current = 0;
    stage = "select retained complete boot version";
    result = kernel_bundle_select(root, &previous, &current);
    if (result == -ENOENT) {
        bundle.identity.generation = 1;
        result = initial_state(state, &bundle.identity);
        if (result) goto out;
    } else if (result) goto out;
    else {
        bundle.identity.generation = previous.generation;
        if (reliefos_kernel_slot_equal(&previous, &bundle.identity)) { result = 0; goto out; }
        if (previous.generation == RELIEFOS_KERNEL_SLOT_GENERATION_MAX) { result = -EOVERFLOW; goto out; }
        bundle.identity.generation = previous.generation + 1;
    }
    stage = "publish immutable boot version";
    result = publish_version(&bundle, root);
    if (result) goto out;
    stage = "preallocate inactive boot records";
    for (char name = 'A'; name <= 'B'; ++name) {
        char file[32]; snprintf(file, sizeof(file), "kernel-%c.env", name);
        result = preallocate(state, file, RELIEFOS_KERNEL_SLOT_SIZE, !current || name != current);
        snprintf(file, sizeof(file), "kernel-%c.sha256", name);
        if (!result) result = preallocate(state, file, 79, !current || name != current);
        if (result) goto out;
    }
    stage = "synchronize boot record directories";
    if (fsync(state) || sync_parent(root, "reliefos/entry") || fsync(root)) { result = -EIO; goto out; }
    char inactive = current == 'A' ? 'B' : 'A';
    unsigned char record[RELIEFOS_KERNEL_SLOT_SIZE];
    result = reliefos_kernel_slot_encode(&bundle.identity, record);
    char record_name[32], checksum_name[32], checksum[80], digest[65];
    snprintf(record_name, sizeof(record_name), "kernel-%c.env", inactive);
    snprintf(checksum_name, sizeof(checksum_name), "kernel-%c.sha256", inactive);
    if (!result) result = kernel_bundle_hash_bytes(record, sizeof(record), digest);
    int length = result ? 0 : snprintf(checksum, sizeof(checksum), "%s  %s\n", digest, record_name);
    if (!result && length != 79) result = -EINVAL;
    stage = "write inactive boot environment";
    if (!result) result = store_fixed(state, record_name, record, sizeof(record));
    if (!result) { stage = "commit inactive boot checksum"; result = store_fixed(state, checksum_name, checksum, (size_t)length); }
    if (!result) {
        stage = "verify committed boot selection";
        struct reliefos_kernel_slot selected; char name;
        result = kernel_bundle_select(root, &selected, &name);
        if (!result && (name != inactive || !reliefos_kernel_slot_equal(&selected, &bundle.identity))) result = -EIO;
    }
out:
    if (result) fprintf(stderr, "[kernel-bundle] %s: %d\n", stage, result);
    if (lock_fd >= 0) close(lock_fd);
    if (state >= 0) close(state);
    if (root >= 0) close(root);
    kernel_bundle_check_close(&bundle);
    return result;
}
