/* Per-installation UEFI entry publication; never take over a foreign fallback. */
#include "bootloader_internal.h"
#include "kernel_bundle_internal.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/** @brief Require a canonical nonzero UUID before placing it in paths or GRUB data. */
static int instance_uuid(const char *uuid)
{
    if (!uuid || strlen(uuid) != 36) return 0;
    unsigned nonzero = 0;
    for (unsigned i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (uuid[i] != '-') return 0; }
        else { if (!isdigit((unsigned char)uuid[i]) && (uuid[i] < 'a' || uuid[i] > 'f')) return 0; nonzero |= uuid[i] != '0'; }
    }
    return nonzero;
}

/** @brief Read an exact small ownership record, rejecting links and extra bytes. */
static int read_record(int root, const char *path, char *text, size_t size)
{
    int fd = boot_open_file(root, path);
    if (fd < 0) return fd;
    size_t used = 0;
    int result = 0;
    while (used < size) {
        ssize_t n = read(fd, text + used, size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { result = -errno; break; }
        if (!n) break;
        used += (size_t)n;
    }
    char extra;
    ssize_t tail;
    do { tail = read(fd, &extra, 1); } while (tail < 0 && errno == EINTR);
    if (!result && tail != 0) result = tail < 0 ? -errno : -EINVAL;
    close(fd);
    return result ? result : used == size ? 0 : -EINVAL;
}

int reliefos_boot_prepare_instance(const char *esp, const char *uuid, char *path, unsigned capacity)
{
    if (!esp || !path || !instance_uuid(uuid)) return -EINVAL;
    char relative[192], leaf[256];
    int n = snprintf(path, capacity, "%s/reliefos/instances/%s", esp, uuid);
    if (n < 0 || (unsigned)n >= capacity) return -ENAMETOOLONG;
    int root = boot_open_directory(esp);
    if (root < 0) return root;
    snprintf(relative, sizeof(relative), "reliefos/instances/%s/entry", uuid);
    int parent = boot_open_parent(root, relative, 1, leaf);
    if (parent < 0) { close(root); return parent; }
    char marker[96], existing[96];
    int length = snprintf(marker, sizeof(marker), "ReliefOS instance v1\n%s\n", uuid);
    int result = read_record(parent, "reliefos-instance.id", existing, (size_t)length);
    if (result == -ENOENT) {
        int copy = fcntl(parent, F_DUPFD_CLOEXEC, 3);
        DIR *directory = copy < 0 ? NULL : fdopendir(copy);
        if (!directory) { if (copy >= 0) close(copy); result = -errno; }
        else {
            struct dirent *entry;
            result = 0;
            errno = 0;
            while ((entry = readdir(directory))) {
                if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) { result = -EEXIST; break; }
            }
            if (!result && errno) result = -errno;
            closedir(directory);
            if (!result) { struct reliefos_boot_install_options options = {0};
                result = boot_publish_record(&options, parent, "reliefos-instance.id", marker, (size_t)length); }
        }
    } else if (!result && memcmp(marker, existing, (size_t)length)) result = -EEXIST;
    close(root);
    if (result) { close(parent); return result; }
    result = fsync(parent) < 0 ? -errno : 0;
    close(parent);
    return result;
}

/** @brief Reject a mismatched physical ESP or instance view before any boot writes. */
int boot_validate_instance(const struct reliefos_boot_install_options *options, const char *esp)
{
    if (!options || !instance_uuid(options->root_partuuid) || !esp) return -EINVAL;
    char expected[BOOT_PATH_MAX];
    int n = snprintf(expected, sizeof(expected), "%s/reliefos/instances/%s", esp, options->root_partuuid);
    if (n < 0 || n >= (int)sizeof(expected)) return -ENAMETOOLONG;
    int a = boot_open_directory(expected), b = boot_open_directory(options->boot_dir);
    struct stat sa, sb;
    int result = a < 0 ? a : b < 0 ? b : fstat(a, &sa) || fstat(b, &sb) ? -errno :
        sa.st_dev != sb.st_dev || sa.st_ino != sb.st_ino ? -EXDEV : 0;
    char text[96], marker[96];
    n = snprintf(text, sizeof(text), "ReliefOS instance v1\n%s\n", options->root_partuuid);
    if (!result) result = read_record(a, "reliefos-instance.id", marker, (size_t)n);
    if (!result && memcmp(text, marker, (size_t)n)) result = -EXDEV;
    if (a >= 0) close(a);
    if (b >= 0) close(b);
    return result;
}

int boot_install_uefi(const struct reliefos_boot_install_options *options, const char *esp)
{
    if (!instance_uuid(options->root_partuuid)) return -EINVAL;
    int root = boot_open_directory(esp), instance = boot_open_directory(options->boot_dir);
    if (root < 0 || instance < 0) { if (root >= 0) close(root); if (instance >= 0) close(instance); return root < 0 ? root : instance; }
    char path[256], expected[BOOT_PATH_MAX], record[160], digest[65] = {0};
    snprintf(path, sizeof(path), "reliefos/instances/%s/grub/entry", options->root_partuuid);
    char leaf[256]; int parent = boot_open_parent(root, path, 0, leaf);
    struct stat a, b;
    int result = parent < 0 ? parent : fstat(instance, &a) ? -errno : 0;
    if (!result) {
        snprintf(expected, sizeof(expected), "%s/reliefos/instances/%s", esp, options->root_partuuid);
        int actual = boot_open_directory(expected);
        if (actual < 0) result = actual;
        else { if (fstat(actual, &b) || a.st_dev != b.st_dev || a.st_ino != b.st_ino) result = -EXDEV; close(actual); }
    }
    if (parent >= 0) close(parent);
    close(instance);
    if (result) { close(root); return result; }
    int fallback = boot_open_file(root, "EFI/BOOT/BOOTX64.EFI");
    int fallback_missing = fallback == -ENOENT;
    int owned = 0;
    if (fallback >= 0) {
        result = kernel_bundle_hash_fd(fallback, digest);
        close(fallback);
        int size = snprintf(expected, sizeof(expected), "ReliefOS fallback v1\n%s\n%s\n", options->root_partuuid, digest);
        if (!result && !read_record(root, "EFI/BOOT/reliefos-owner", record, (size_t)size))
            owned = !memcmp(expected, record, (size_t)size);
    } else if (!fallback_missing) { close(root); return fallback; }
    int source_root = boot_open_directory(options->source);
    int source = source_root < 0 ? source_root : boot_open_file(source_root, "EFI/BOOT/BOOTX64.EFI");
    if (source_root >= 0) close(source_root);
    if (source < 0) { close(root); return source; }
    snprintf(path, sizeof(path), "EFI/ReliefOS/%s/grubx64.efi", options->root_partuuid);
    result = boot_publish_descriptor(options, root, path, source);
    char config[512];
    int n = snprintf(config, sizeof(config), "# ReliefOS installation %s\nset prefix=($root)/reliefos/instances/%s/grub\nconfigfile $prefix/grub.cfg\n", options->root_partuuid, options->root_partuuid);
    snprintf(path, sizeof(path), "EFI/ReliefOS/%s/grub.cfg", options->root_partuuid);
    if (!result) result = boot_publish_record(options, root, path, config, (size_t)n);
    // An existing sibling config without an executable is still foreign data.
    int sibling = boot_open_file(root, "EFI/BOOT/grub.cfg");
    int can_create = fallback_missing && sibling == -ENOENT;
    if (sibling >= 0) close(sibling);
    if (!result && (can_create || owned)) {
        result = boot_publish_record(options, root, "EFI/BOOT/grub.cfg", config, (size_t)n);
        if (!result && lseek(source, 0, SEEK_SET) < 0) result = -errno;
        if (!result) result = boot_publish_descriptor(options, root, "EFI/BOOT/BOOTX64.EFI", source);
        if (!result && lseek(source, 0, SEEK_SET) < 0) result = -errno;
        if (!result) result = kernel_bundle_hash_fd(source, digest);
        n = snprintf(record, sizeof(record), "ReliefOS fallback v1\n%s\n%s\n", options->root_partuuid, digest);
        if (!result) result = boot_publish_record(options, root, "EFI/BOOT/reliefos-owner", record, (size_t)n);
    } else if (!result) {
        fprintf(stderr, "[boot-install] preserved existing EFI fallback; firmware entry: /EFI/ReliefOS/%s/grubx64.efi\n", options->root_partuuid);
    }
    // A conventional vendor entry lets another boot manager discover ReliefOS
    // without guessing an installation UUID. Its selected default is retained
    // when a different installation on the shared ESP is updated.
    int vendor = boot_open_file(root, "EFI/ReliefOS/grubx64.efi");
    int vendor_missing = vendor == -ENOENT, vendor_owned = 0;
    if (vendor >= 0) {
        int error = kernel_bundle_hash_fd(vendor, digest);
        close(vendor);
        int length = snprintf(expected, sizeof(expected), "ReliefOS dispatcher v1\n%s\n%s\n", options->root_partuuid, digest);
        if (!error && !read_record(root, "EFI/ReliefOS/dispatcher-owner", record, (size_t)length))
            vendor_owned = !memcmp(expected, record, (size_t)length);
    }
    int vendor_config = boot_open_file(root, "EFI/ReliefOS/grub.cfg");
    int vendor_create = vendor_missing && vendor_config == -ENOENT;
    if (vendor_config >= 0) close(vendor_config);
    if (!result && (vendor_create || vendor_owned)) {
        n = snprintf(config, sizeof(config), "# ReliefOS installation %s\nset prefix=($root)/reliefos/instances/%s/grub\nconfigfile $prefix/grub.cfg\n", options->root_partuuid, options->root_partuuid);
        result = boot_publish_record(options, root, "EFI/ReliefOS/grub.cfg", config, (size_t)n);
        if (!result && lseek(source, 0, SEEK_SET) < 0) result = -errno;
        if (!result) result = boot_publish_descriptor(options, root, "EFI/ReliefOS/grubx64.efi", source);
        if (!result && lseek(source, 0, SEEK_SET) < 0) result = -errno;
        if (!result) result = kernel_bundle_hash_fd(source, digest);
        n = snprintf(record, sizeof(record), "ReliefOS dispatcher v1\n%s\n%s\n", options->root_partuuid, digest);
        if (!result) result = boot_publish_record(options, root, "EFI/ReliefOS/dispatcher-owner", record, (size_t)n);
    }
    close(source); close(root);
    return result;
}

int reliefos_boot_validate_instance(const struct reliefos_boot_install_options *options, const char *esp)
{
    return boot_validate_instance(options, esp);
}
