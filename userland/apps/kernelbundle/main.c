/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE 1
#include <reliefos/kernel_bundle.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/** @brief Join a known absolute root and constant or validated relative suffix.
 * @param output Caller-owned 1024-byte path buffer.
 * @param root Borrowed absolute root; slash is handled without double separators.
 * @param member Borrowed safe relative suffix.
 * @return Zero or negative EINVAL/ENAMETOOLONG.
 */
static int path_join(char output[1024], const char *root, const char *member)
{
    if (!root || root[0] != '/') return -EINVAL;
    size_t length = strlen(root);
    int size = snprintf(output, 1024, "%s%s%s", root, root[length - 1] == '/' ? "" : "/", member);
    return size < 0 || size >= 1024 ? -ENAMETOOLONG : 0;
}

/** @brief Read the signed package's current-release record as a bounded scalar.
 * @param root Borrowed installed root.
 * @param release Caller-owned 32-byte release output.
 * @return Zero, negative ENOENT if no modular package is installed, or negative errno.
 */
static int installed_release(const char *root, char release[32])
{
    char path[1024]; int result = path_join(path, root, "usr/lib/reliefos/kernel/current-release");
    if (result) return result;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -errno;
    struct stat status; char record[33];
    result = fstat(fd, &status) ? -errno : !S_ISREG(status.st_mode) || status.st_size < 2 || status.st_size > 32 ? -EINVAL : 0;
    size_t used = 0;
    while (!result && used < (size_t)status.st_size) {
        ssize_t size = read(fd, record + used, (size_t)status.st_size - used);
        if (size < 0 && errno == EINTR) continue;
        if (size <= 0) { result = size ? -errno : -EIO; break; }
        used += (size_t)size;
    }
    close(fd);
    if (result) return result;
    if (record[used - 1] != '\n') return -EINVAL;
    record[--used] = 0;
    if (!used || strspn(record, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._+-") != used ||
        !strcmp(record, ".") || !strcmp(record, "..")) return -EINVAL;
    memcpy(release, record, used + 1);
    return 0;
}

/** @brief Preserve the currently signed package before APK unlinks its members.
 * @param root Borrowed final root, serialized by APK's write transaction lock.
 * @return Zero, including a pre-module installation without a release record;
 * negative errno prevents the package commit through the standard pre hook.
 */
static int retain_installed(const char *root)
{
    char release[32]; int result = installed_release(root, release);
    return result == -ENOENT ? 0 : result ? result : reliefos_kernel_bundle_retain(root, release);
}

/** @brief Restore every previously completed retained release after APK's commit.
 * @param root Borrowed final root; this never deletes user/external module files.
 * @return Zero if no cache is needed or every cached release is complete; negative errno otherwise.
 */
static int restore_all(const char *root)
{
    char path[1024]; int result = path_join(path, root, "var/lib/reliefos/kernel-retained");
    if (result) return result;
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -errno;
    DIR *directory = fdopendir(fd);
    if (!directory) { result = -errno; close(fd); return result; }
    result = 0;
    for (;;) {
        errno = 0; struct dirent *entry = readdir(directory);
        if (!entry) { if (errno) result = -errno; break; }
        if (entry->d_name[0] == '.') continue;
        result = reliefos_kernel_bundle_restore(root, entry->d_name);
        if (result) break;
    }
    closedir(directory);
    return result;
}

/** @brief Publish only the complete package named by the installed signed pointer.
 * @param root Borrowed final installed root.
 * @param boot Borrowed mounted ESP, whose stable selector must already be installed.
 * @return Zero after restoration/publication, negative errno otherwise. Firmware
 * loaders and menu scripts are never rewritten by ordinary package upgrades.
 */
static int publish_installed(const char *root, const char *boot)
{
    int result = restore_all(root);
    if (result) return result;
    char release[32]; result = installed_release(root, release);
    if (result == -ENOENT) return 0;
    if (result) return result;
    char path[1024]; result = path_join(path, boot, "grub/kernel-slots.cfg");
    struct stat status;
    if (!result && lstat(path, &status)) result = -errno;
    if (!result && (!S_ISREG(status.st_mode) || status.st_size <= 0)) result = -EINVAL;
    if (result) {
        fputs("kernel-bundle: install the matched GRUB resources before activating this package\n", stderr);
        return result;
    }
    return reliefos_kernel_bundle_publish(root, boot, release);
}

/** @brief Recover the running kernel before normal modules/services can start.
 * @param root Borrowed mounted final root, normally /newroot in early userspace.
 * @return Zero when no retained copy exists or recovery succeeds; negative errno
 * refuses an incomplete known retention tree. Reads uname through the Linux ABI.
 */
static int recover_running(const char *root)
{
    struct utsname system;
    if (uname(&system)) return -errno;
    if (strcmp(system.sysname, "ReliefNT")) return -EPERM;
    char member[128], path[1024];
    if (strlen(system.release) >= 32) return -EINVAL;
    snprintf(member, sizeof(member), "var/lib/reliefos/kernel-retained/%s", system.release);
    int result = path_join(path, root, member);
    struct stat status;
    if (!result && lstat(path, &status)) return errno == ENOENT ? 0 : -errno;
    if (!result && !S_ISDIR(status.st_mode)) result = -EINVAL;
    return result ? result : reliefos_kernel_bundle_restore(root, system.release);
}

/** @brief Emit bounded scalar identity for both build and update preflight.
 * @param root Borrowed absolute package root; never interpreted as shell text.
 * @param release Borrowed expected release.
 * @return Zero for a completely checked package and successful stdout, negative errno otherwise.
 */
static int inspect(const char *root, const char *release)
{
    struct reliefos_kernel_slot identity;
    int result = reliefos_kernel_bundle_describe(root, release, &identity);
    if (result) return result;
    int size = printf("kernel_release=%s\nmodule_abi=%s\nkernel_sha256=%s\ninitramfs_sha256=%s\n",
                      identity.release, identity.module_abi, identity.kernel_sha256, identity.initramfs_sha256);
    return size < 0 || fflush(stdout) ? -EIO : 0;
}

/** @brief Operate on explicit staging roots or the admitted ReliefNT running root.
 * @return Zero on completion, 2 for usage, 77 for host-root refusal or 1 for an
 * observed validation/I/O failure. This static binary also runs from initramfs
 * when an interrupted package upgrade has removed normal shared libraries.
 */
int main(int argc, char **argv)
{
    if (argc < 3 || argc > 4 || argv[2][0] != '/') {
        fputs("Usage: kernel-bundle --retain|--restore|--inspect ROOT RELEASE\n"
              "       kernel-bundle --retain-installed|--restore-all|--recover-running ROOT\n"
              "       kernel-bundle --publish-installed ROOT BOOT\n", stderr);
        return 2;
    }
    struct utsname system;
    if (strcmp(argv[1], "--inspect") && !strcmp(argv[2], "/") && (uname(&system) || strcmp(system.sysname, "ReliefNT"))) {
        fputs("kernel-bundle: refusing mutations of a non-ReliefNT host root\n", stderr);
        return 77;
    }
    int result;
    if (argc == 4 && !strcmp(argv[1], "--inspect")) result = inspect(argv[2], argv[3]);
    else if (argc == 4 && !strcmp(argv[1], "--retain")) result = reliefos_kernel_bundle_retain(argv[2], argv[3]);
    else if (argc == 4 && !strcmp(argv[1], "--restore")) result = reliefos_kernel_bundle_restore(argv[2], argv[3]);
    else if (argc == 4 && !strcmp(argv[1], "--publish-installed")) result = publish_installed(argv[2], argv[3]);
    else if (argc == 3 && !strcmp(argv[1], "--retain-installed")) result = retain_installed(argv[2]);
    else if (argc == 3 && !strcmp(argv[1], "--restore-all")) result = restore_all(argv[2]);
    else if (argc == 3 && !strcmp(argv[1], "--recover-running")) result = recover_running(argv[2]);
    else return 2;
    if (result) fprintf(stderr, "kernel-bundle: %s: %s (%d)\n", argv[1], strerror(-result), result);
    return result ? 1 : 0;
}
