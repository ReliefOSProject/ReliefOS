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
    if (target->root_mounted && !target->boot_mounted) {
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

int installer_update_target_open(struct installer_update_target *target,
                                 const char *disk, const char *expected_root)
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
    for (uint32_t i = 0; i < count; ++i) {
        result = mounted_elsewhere(parts[i].path);
        if (result) return result;
        if (parts[i].gpt_type == RELIEFOS_BLOCK_GPT_ESP && parts[i].filesystem == RELIEFOS_BLOCK_FILESYSTEM_FAT32) {
            if (esp >= 0) return -ENOTUNIQ;
            esp = (int)i;
        }
    }
    if (esp < 0) return -ENOENT;
    strcpy(target->workspace, "/run/reliefos/update-XXXXXX");
    if (!mkdtemp(target->workspace)) return -errno;
    snprintf(target->root, sizeof(target->root), "%s/root", target->workspace);
    snprintf(target->boot, sizeof(target->boot), "%s/root/boot", target->workspace);
    if (mkdir(target->root, 0700) < 0) return -errno;
    for (uint32_t i = 0; i < count; ++i) {
        const char *filesystem = parts[i].filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT4 ? "ext4"
            : parts[i].filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT2 ? "ext2" : NULL;
        if (!filesystem || parts[i].gpt_type != RELIEFOS_BLOCK_GPT_LINUX) continue;
        char uuid[37];
        result = reliefos_block_partition_uuid(disk, parts[i].index, uuid);
        if (result) return result;
        if (expected_root && *expected_root && strcmp(expected_root, uuid)) continue;
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
    snprintf(target->info.esp_device, sizeof(target->info.esp_device), "%s", parts[esp].path);
    result = reliefos_block_partition_uuid(disk, parts[esp].index, target->info.esp_uuid);
    if (result) return result;
    if (mount(target->info.root_device, target->root, target->filesystem, MS_RDONLY | MS_NOSUID | MS_NODEV, NULL) < 0)
        return -errno;
    target->root_mounted = 1;
    if (mount(target->info.esp_device, target->boot, "vfat", MS_RDONLY | MS_NOSUID | MS_NODEV, NULL) < 0)
        return -errno;
    target->boot_mounted = 1;
    return reliefos_boot_probe(disk, target->boot, &target->info.boot_modes);
}

int installer_update_target_writable(struct installer_update_target *target)
{
    if (!target->root_mounted || !target->boot_mounted) return -EINVAL;
    if (umount2(target->boot, 0) < 0) return -errno;
    target->boot_mounted = 0;
    if (umount2(target->root, 0) < 0) return -errno;
    target->root_mounted = 0;
    if (mount(target->info.root_device, target->root, target->filesystem, 0, NULL) < 0) return -errno;
    target->root_mounted = 1;
    if (mount(target->info.esp_device, target->boot, "vfat", MS_NOSUID | MS_NODEV, NULL) < 0) return -errno;
    target->boot_mounted = 1;
    return 0;
}
