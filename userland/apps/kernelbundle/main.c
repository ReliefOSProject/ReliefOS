/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE 1
#include <reliefos/kernel_bundle.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
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

/** @brief Emit stable scalar boot state for the system updater and recovery UI.
 * @param boot Absolute mounted ESP. @return Zero or negative validation/I/O errno.
 */
static int boot_status(const char *boot)
{
    struct reliefos_kernel_boot_status status;
    int result = reliefos_kernel_bundle_boot_status(boot, &status);
    if (result) return result;
    int size = printf("format_version=1\nboot_protocol=%u\nlatest_slot=%c\nlatest_generation=%u\n"
        "latest_release=%s\nlatest_initramfs=%s\nlatest_confirmed=%u\nlatest_remaining=%u\n"
        "confirmed_slot=%c\nconfirmed_generation=%u\nconfirmed_release=%s\nconfirmed_initramfs=%s\n",
        status.protocol, status.latest_name, status.latest.generation, status.latest.release,
        status.latest.initramfs_sha256, status.health.confirmed, status.health.remaining,
        status.confirmed_name, status.confirmed.generation, status.confirmed.release,
        status.confirmed.initramfs_sha256);
    return size < 0 || fflush(stdout) ? -EIO : 0;
}

/** @brief Confirm only the exact GRUB-selected pair after the boot service is ready.
 * @param root Final ReliefNT root. @param boot Its mounted ESP namespace.
 * @return Zero for successful confirmation or a manual/legacy boot with no trial
 * identity; negative errno for partial, duplicate or mismatched boot arguments.
 */
static int confirm_running(const char *root, const char *boot)
{
    struct utsname system;
    if (uname(&system) || strcmp(system.sysname, "ReliefNT")) return -EPERM;
    char path[1024]; int result = path_join(path, root, "proc/cmdline");
    if (result) return result;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -errno;
    char line[4097]; size_t used = 0;
    while (used < sizeof(line) - 1) {
        ssize_t count = read(fd, line + used, sizeof(line) - 1 - used);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { result = -errno; break; }
        if (!count) break;
        used += (size_t)count;
    }
    close(fd);
    if (result) return result;
    if (used == sizeof(line) - 1 || memchr(line, 0, used)) return -E2BIG;
    line[used] = 0;
    char slot = 0, initramfs[65] = {0}; uint32_t generation = 0; unsigned fields = 0;
    char *save, *token = strtok_r(line, " \t\r\n", &save);
    while (token) {
        const char *value;
        if (!strncmp(token, "reliefos.boot_slot=", sizeof("reliefos.boot_slot=") - 1)) {
            value = token + sizeof("reliefos.boot_slot=") - 1;
            if ((fields & 1) || strlen(value) != 1 || (*value != 'A' && *value != 'B')) return -EINVAL;
            fields |= 1; slot = *value;
        } else if (!strncmp(token, "reliefos.boot_generation=", sizeof("reliefos.boot_generation=") - 1)) {
            value = token + sizeof("reliefos.boot_generation=") - 1;
            if ((fields & 2) || !*value || *value == '0') return -EINVAL;
            for (; *value; ++value) {
                if (*value < '0' || *value > '9' || generation > (RELIEFOS_KERNEL_SLOT_GENERATION_MAX - (unsigned)(*value - '0')) / 10) return -EINVAL;
                generation = generation * 10 + (unsigned)(*value - '0');
            }
            fields |= 2;
        } else if (!strncmp(token, "reliefos.boot_initramfs=", sizeof("reliefos.boot_initramfs=") - 1)) {
            value = token + sizeof("reliefos.boot_initramfs=") - 1;
            if ((fields & 4) || strlen(value) != 64 || strspn(value, "0123456789abcdef") != 64) return -EINVAL;
            memcpy(initramfs, value, 65); fields |= 4;
        }
        token = strtok_r(NULL, " \t\r\n", &save);
    }
    if (!fields) return 0; /* Explicit recovery/manual legacy boots cannot confirm a trial. */
    if (fields != 7) return -EINVAL;
    return reliefos_kernel_bundle_confirm(root, boot, slot, generation, system.release, initramfs);
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
              "       kernel-bundle --publish-installed|--confirm-running|--boot-status ROOT BOOT\n", stderr);
        return 2;
    }
    struct utsname system;
    if (strcmp(argv[1], "--inspect") && strcmp(argv[1], "--boot-status") && !strcmp(argv[2], "/") && (uname(&system) || strcmp(system.sysname, "ReliefNT"))) {
        fputs("kernel-bundle: refusing mutations of a non-ReliefNT host root\n", stderr);
        return 77;
    }
    int result;
    if (argc == 4 && !strcmp(argv[1], "--inspect")) result = inspect(argv[2], argv[3]);
    else if (argc == 4 && !strcmp(argv[1], "--retain")) result = reliefos_kernel_bundle_retain(argv[2], argv[3]);
    else if (argc == 4 && !strcmp(argv[1], "--restore")) result = reliefos_kernel_bundle_restore(argv[2], argv[3]);
    else if (argc == 4 && !strcmp(argv[1], "--publish-installed")) result = publish_installed(argv[2], argv[3]);
    else if (argc == 4 && !strcmp(argv[1], "--confirm-running")) result = confirm_running(argv[2], argv[3]);
    else if (argc == 4 && !strcmp(argv[1], "--boot-status")) result = boot_status(argv[3]);
    else if (argc == 3 && !strcmp(argv[1], "--retain-installed")) result = retain_installed(argv[2]);
    else if (argc == 3 && !strcmp(argv[1], "--restore-all")) result = restore_all(argv[2]);
    else if (argc == 3 && !strcmp(argv[1], "--recover-running")) result = recover_running(argv[2]);
    else return 2;
    if (result) fprintf(stderr, "kernel-bundle: %s: %s (%d)\n", argv[1], strerror(-result), result);
    return result ? 1 : 0;
}
