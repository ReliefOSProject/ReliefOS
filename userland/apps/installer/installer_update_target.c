#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "installer_update_internal.h"
#include <reliefos/blockdev.h>
#include <reliefos/bootloader.h>

#include <errno.h>
#include <fcntl.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

static int mounted_elsewhere(const char *path)
{
    struct stat device;
    if (stat(path, &device) < 0) return -errno;
    if (!S_ISBLK(device.st_mode)) return -ENOTBLK;
    FILE *stream = setmntent("/proc/self/mounts", "r");
    if (!stream) return -errno;
    int result = 0;
    struct mntent *entry;
    while ((entry = getmntent(stream))) {
        struct stat source;
        if (!strcmp(entry->mnt_fsname, path) ||
            (!stat(entry->mnt_fsname, &source) && S_ISBLK(source.st_mode) && source.st_rdev == device.st_rdev)) {
            result = -EBUSY;
            break;
        }
    }
    if (!result && ferror(stream)) result = -EIO;
    endmntent(stream);
    return result;
}

static int acquire_lock(void)
{
    struct stat status;
    if (mkdir("/run/reliefos", 0755) < 0 && errno != EEXIST) return -errno;
    if (lstat("/run/reliefos", &status) < 0) return -errno;
    if (!S_ISDIR(status.st_mode) || status.st_uid || (status.st_mode & 0022)) return -EPERM;
    int fd = open("/run/reliefos/installer-update.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -errno;
    if (fstat(fd, &status) < 0) {
        int result = -errno;
        close(fd);
        return result;
    }
    if (!S_ISREG(status.st_mode) || status.st_uid || status.st_nlink != 1) { close(fd); return -EPERM; }
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    if (fcntl(fd, F_SETLK, &lock) < 0) {
        int result = errno == EACCES || errno == EAGAIN ? -EBUSY : -errno;
        close(fd);
        return result;
    }
    return fd;
}

int installer_update_target_close(struct installer_update_target *target)
{
    int result = 0;
    if (target->boot_mounted) {
        if (umount2(target->boot, 0) < 0) result = -errno;
        else target->boot_mounted = 0;
    }
    if (target->esp_mounted && !target->boot_mounted) {
        if (umount2(target->esp, 0) < 0 && !result) result = -errno;
        else if (!result) target->esp_mounted = 0;
    }
    if (target->root_mounted && !target->boot_mounted && !target->esp_mounted) {
        if (umount2(target->root, 0) < 0 && !result) result = -errno;
        else if (!result) target->root_mounted = 0;
    }
    if (!target->root_mounted && target->root[0]) {
        if (rmdir(target->root) < 0 && !result) result = -errno;
        if (rmdir(target->workspace) < 0 && !result) result = -errno;
    }
    if (target->lock_fd >= 0) {
        if (close(target->lock_fd) < 0 && !result) result = -errno;
        target->lock_fd = -1;
    }
    return result;
}

/** @brief Resolve only the selected root's declarative ESP and bind layout. */
static int read_boot_layout(struct installer_update_target *target, char source[128])
{
    int root = open(target->root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int etc = root < 0 ? -1 : openat(root, "etc", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int fd = etc < 0 ? -1 : openat(etc, "fstab", O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    int saved = errno;
    if (etc >= 0) close(etc);
    if (root >= 0) close(root);
    if (fd < 0) return -saved;
    struct stat status;
    if (fstat(fd, &status) < 0) { saved = errno; close(fd); return -saved; }
    if (!S_ISREG(status.st_mode) || status.st_size < 0 || status.st_size > 65536) { close(fd); return -EINVAL; }
    FILE *stream = fdopen(fd, "r");
    if (!stream) { saved = errno; close(fd); return -saved; }
    char line[1024], device[128], point[128], type[32], flags[128], bind[128] = {0};
    int result = 0, physical = 0, logical = 0, physical_namespace = 0;
    while (fgets(line, sizeof(line), stream)) {
        if (!strchr(line, '\n') && !feof(stream)) { result = -EOVERFLOW; break; }
        int fields = sscanf(line, "%127s %127s %31s %127s", device, point, type, flags);
        if (fields < 1 || device[0] == '#') continue;
        if (fields < 2 || (strcmp(point, "/boot") && strcmp(point, "/efi"))) continue;
        if (fields != 4) { result = -EINVAL; break; }
        if (!strcmp(point, "/boot") && !strcmp(type, "none") && !strcmp(flags, "bind")) {
            if (logical++) { result = -ENOTUNIQ; break; }
            snprintf(bind, sizeof(bind), "/efi/reliefos/instances/%s", target->info.root_uuid);
            if (strcmp(device, bind)) { result = -EXDEV; break; }
        } else if ((!strcmp(point, "/boot") || !strcmp(point, "/efi")) &&
                   (!strcmp(type, "vfat") || !strcmp(type, "fat32"))) {
            if (physical++) { result = -ENOTUNIQ; break; }
            strcpy(source, device);
            physical_namespace = !strcmp(point, "/efi");
        } else { result = -EINVAL; break; }
    }
    if (!result && ferror(stream)) result = -EIO;
    fclose(stream);
    if (!result && (!physical || logical != physical_namespace)) result = -EINVAL;
    if (!result) target->namespaced = physical_namespace;
    return result;
}

/** @brief Mount the chosen physical ESP and its optional per-root boot view. */
static int mount_boot(struct installer_update_target *target, unsigned long flags)
{
    const char *physical = target->namespaced ? target->esp : target->boot;
    struct stat point;
    if (lstat(physical, &point) < 0) return -errno;
    if (!S_ISDIR(point.st_mode)) return -EINVAL;
    if (target->namespaced) {
        if (lstat(target->boot, &point) < 0) return -errno;
        if (!S_ISDIR(point.st_mode)) return -EINVAL;
    }
    if (mount(target->info.esp_device, physical, "vfat", flags, NULL) < 0) return -errno;
    if (!target->namespaced) { target->boot_mounted = 1; return 0; }
    target->esp_mounted = 1;
    char instance[512];
    snprintf(instance, sizeof(instance), "%s/reliefos/instances/%s", target->esp, target->info.root_uuid);
    struct stat status;
    if (lstat(instance, &status) < 0) return -errno;
    if (!S_ISDIR(status.st_mode)) return -EINVAL;
    if (mount(instance, target->boot, NULL, MS_BIND, NULL) < 0) return -errno;
    target->boot_mounted = 1;
    return 0;
}

int installer_update_target_open(struct installer_update_target *target,
                                 const char *disk, const char *expected_root, const char *expected_esp)
{
    struct reliefos_block_partition parts[RELIEFOS_BLOCK_MAX_PARTITIONS];
    uint32_t count = 0;
    int result, esp = -1, found = -1;
    memset(target, 0, sizeof(*target));
    target->lock_fd = -1;
    if (!disk || strncmp(disk, "/dev/", 5) || strlen(disk) >= sizeof(target->info.disk)) return -EINVAL;
    target->lock_fd = acquire_lock();
    if (target->lock_fd < 0) return target->lock_fd;
    result = reliefos_block_list_partitions(disk, parts, RELIEFOS_BLOCK_MAX_PARTITIONS, &count);
    if (result) return result;
    if (count > RELIEFOS_BLOCK_MAX_PARTITIONS) return -EOVERFLOW;
    strcpy(target->workspace, "/run/reliefos/update-XXXXXX");
    if (!mkdtemp(target->workspace)) return -errno;
    char *paths[] = {target->root, target->boot, target->esp};
    const char *suffixes[] = {"/root", "/root/boot", "/root/efi"};
    for (unsigned i = 0; i < 3; ++i) {
        int n = snprintf(paths[i], sizeof(target->root), "%s%s", target->workspace, suffixes[i]);
        if (n < 0 || n >= (int)sizeof(target->root)) return -ENAMETOOLONG;
    }
    if (mkdir(target->root, 0700) < 0) return -errno;
    for (uint32_t i = 0; i < count; ++i) {
        const char *filesystem = parts[i].filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT4 ? "ext4"
            : parts[i].filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT2 ? "ext2" : NULL;
        if (!filesystem || parts[i].gpt_type != RELIEFOS_BLOCK_GPT_LINUX) continue;
        char uuid[37];
        result = reliefos_block_partition_uuid(disk, parts[i].index, uuid);
        if (result) return result;
        if (expected_root && *expected_root && strcmp(expected_root, uuid)) continue;
        result = mounted_elsewhere(parts[i].path);
        if (result) return result;
        if (mount(parts[i].path, target->root, filesystem, MS_RDONLY | MS_NOSUID | MS_NODEV, NULL) < 0)
            return -errno;
        target->root_mounted = 1;
        struct installer_update_info candidate = {0};
        result = installer_update_read_system(target->root, &candidate);
        if (umount2(target->root, 0) < 0) return -errno;
        target->root_mounted = 0;
        if (result) {
            fprintf(stderr, "[installer-update] root inspection %s returned %d\n", parts[i].path, result);
            continue;
        }
        if (found >= 0) return -ENOTUNIQ;
        found = (int)i;
        target->info = candidate;
        strcpy(target->info.root_uuid, uuid);
        target->filesystem = filesystem;
    }
    if (found < 0) return -ENODEV;
    strcpy(target->info.disk, disk);
    snprintf(target->info.root_device, sizeof(target->info.root_device), "%s", parts[found].path);
    if (mount(target->info.root_device, target->root, target->filesystem, MS_RDONLY | MS_NOSUID | MS_NODEV, NULL) < 0)
        return -errno;
    target->root_mounted = 1;
    char source[128] = {0};
    result = read_boot_layout(target, source);
    if (result) return result;
    for (uint32_t i = 0; i < count; ++i) {
        if (parts[i].gpt_type != RELIEFOS_BLOCK_GPT_ESP || parts[i].filesystem != RELIEFOS_BLOCK_FILESYSTEM_FAT32) continue;
        char uuid[37];
        result = reliefos_block_partition_uuid(disk, parts[i].index, uuid);
        if (result) return result;
        const char *id = !strncmp(source, "PARTUUID=", 9) ? source + 9 :
            !strncmp(source, "/dev/disk/by-partuuid/", 22) ? source + 22 : NULL;
        int matches = id ? !strcasecmp(id, uuid) : !strcmp(source, parts[i].path);
        if (!id && !matches) {
            struct stat alias, partition;
            matches = !stat(source, &alias) && S_ISBLK(alias.st_mode) &&
                !stat(parts[i].path, &partition) && alias.st_rdev == partition.st_rdev;
        }
        if (!matches) continue;
        if (expected_esp && *expected_esp && strcmp(expected_esp, uuid)) return -EXDEV;
        if (esp >= 0) return -ENOTUNIQ;
        esp = (int)i;
        strcpy(target->info.esp_uuid, uuid);
    }
    if (esp < 0) return -ENODEV;
    snprintf(target->info.esp_device, sizeof(target->info.esp_device), "%s", parts[esp].path);
    result = mounted_elsewhere(target->info.esp_device);
    if (result) return result;
    result = mount_boot(target, MS_RDONLY | MS_NOSUID | MS_NODEV);
    if (result) return result;
    if (target->namespaced)
        snprintf(target->info.boot_file, sizeof(target->info.boot_file), "/EFI/ReliefOS/%s/grubx64.efi", target->info.root_uuid);
    else strcpy(target->info.boot_file, "/EFI/BOOT/BOOTX64.EFI");
    return reliefos_boot_probe(disk, target->boot, &target->info.boot_modes);
}

int installer_update_target_writable(struct installer_update_target *target)
{
    if (!target->root_mounted || !target->boot_mounted) return -EINVAL;
    if (umount2(target->boot, 0) < 0) return -errno;
    target->boot_mounted = 0;
    if (target->esp_mounted) {
        if (umount2(target->esp, 0) < 0) return -errno;
        target->esp_mounted = 0;
    }
    if (umount2(target->root, 0) < 0) return -errno;
    target->root_mounted = 0;
    if (mount(target->info.root_device, target->root, target->filesystem, 0, NULL) < 0) return -errno;
    target->root_mounted = 1;
    return mount_boot(target, MS_NOSUID | MS_NODEV);
}
