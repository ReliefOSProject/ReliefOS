#include "model.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <mntent.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

namespace dm {
/** @brief Query standard Linux block readonly state; failure is never treated as writable. */
static int readonly_state(const char *path, bool &value)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    int flag = 0;
    int result = ioctl(fd, BLKROGET, &flag) < 0 ? -errno : 0;
    close(fd);
    value = flag != 0;
    return result;
}

/** @brief Capture media identity before confirmation, using POSIX reads and optional Linux diskseq. */
static int disk_identity(Disk &disk)
{
    int fd = open(disk.info.path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    struct stat node{};
    int result = fstat(fd, &node) < 0 ? -errno : !S_ISBLK(node.st_mode) ? -ENOTBLK : 0;
    size_t done = 0;
    while (!result && done < sizeof(disk.header)) {
        ssize_t got = pread(fd, disk.header + done, sizeof(disk.header) - done, 512 + done);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { result = got < 0 ? -errno : -EIO; break; }
        done += static_cast<size_t>(got);
    }
#ifdef BLKGETDISKSEQ
    if (!result && ioctl(fd, BLKGETDISKSEQ, &disk.sequence) < 0 && errno != ENOTTY && errno != EINVAL)
        result = -errno;
#endif
    close(fd);
    if (!result) {
        disk.node_device = node.st_dev; disk.node_inode = node.st_ino;
        disk.device_number = node.st_rdev; disk.identity_valid = true;
    }
    return result;
}

/** @brief Match whole-disk mount sources, with an exact SATA/NVMe partition suffix. */
static bool on_disk(const char *source, const char *disk)
{
    size_t length = strlen(disk);
    if (strncmp(source, disk, length)) return false;
    const char *suffix = source + length;
    if (!*suffix) return true;
    if (!strncmp(disk, "/dev/nvme", 9) && *suffix == 'p') ++suffix;
    if (*suffix < '0' || *suffix > '9') return false;
    while (*suffix >= '0' && *suffix <= '9') ++suffix;
    return !*suffix;
}

/** @brief Read actual mount targets, including aliases and escaped spaces, without guessing a path. */
static void collect_mounts(Snapshot &snapshot)
{
    FILE *stream = setmntent("/proc/self/mounts", "r");
    if (!stream) { snapshot.mount_error = errno ? -errno : -EIO; return; }
    mntent *entry;
    while ((entry = getmntent(stream))) {
        struct stat source{}, target{};
        bool have_source = stat(entry->mnt_fsname, &source) == 0 && S_ISBLK(source.st_mode);
        bool have_target = stat(entry->mnt_dir, &target) == 0;
        for (uint32_t i = 0; i < snapshot.disk_count; ++i)
            if (on_disk(entry->mnt_fsname, snapshot.disks[i].info.path)) snapshot.disks[i].mounted = true;
        for (uint32_t i = 0; i < snapshot.partition_count; ++i) {
            Partition &part = snapshot.partitions[i];
            struct stat device{};
            bool have_device = stat(part.info.path, &device) == 0 && S_ISBLK(device.st_mode);
            bool match = !strcmp(entry->mnt_fsname, part.info.path) ||
                         (have_device && ((have_source && source.st_rdev == device.st_rdev) ||
                                          (have_target && target.st_dev == device.st_rdev)));
            if (!match) continue;
            if (snapshot.selected_disk >= 0) snapshot.disks[snapshot.selected_disk].mounted = true;
            ++part.mount_count;
            if (strlen(entry->mnt_dir) >= sizeof(part.mount_path)) snapshot.mount_error = -ENAMETOOLONG;
            else if (part.mount_count == 1) strcpy(part.mount_path, entry->mnt_dir);
            if (!strcmp(entry->mnt_dir, "/") || !strcmp(entry->mnt_dir, "/boot") ||
                !strcmp(entry->mnt_dir, "/boot/efi")) part.system = true;
        }
    }
    if (ferror(stream)) snapshot.mount_error = -EIO;
    endmntent(stream);
}

/** @brief Collect a bounded read-only snapshot in the worker, preserving a selected whole-disk path. */
static void collect_snapshot(const char *preferred, Snapshot &snapshot)
{
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.selected_disk = -1;
    reliefos_block_disk_info disks[RELIEFOS_BLOCK_MAX_DISKS]{};
    uint32_t count = 0;
    snapshot.enumeration_error = reliefos_block_list_disks(disks, RELIEFOS_BLOCK_MAX_DISKS, &count);
    snapshot.disk_count = count < RELIEFOS_BLOCK_MAX_DISKS ? count : RELIEFOS_BLOCK_MAX_DISKS;
    for (uint32_t i = 0; i < snapshot.disk_count; ++i) {
        Disk &disk = snapshot.disks[i];
        disk.info = disks[i];
        disk.inspect_error = readonly_state(disk.info.path, disk.read_only);
        if (!disk.inspect_error) disk.inspect_error = disk_identity(disk);
        if (preferred && !strcmp(preferred, disk.info.path)) snapshot.selected_disk = static_cast<int>(i);
    }
    if (snapshot.selected_disk < 0 && snapshot.disk_count && (!preferred || !*preferred))
        snapshot.selected_disk = 0;
    if (snapshot.selected_disk >= 0) {
        const char *path = snapshot.disks[snapshot.selected_disk].info.path;
        reliefos_block_partition parts[RELIEFOS_BLOCK_MAX_PARTITIONS]{};
        count = 0;
        snapshot.partition_error = reliefos_block_list_partitions(path, parts, RELIEFOS_BLOCK_MAX_PARTITIONS, &count);
        if (!snapshot.partition_error) {
            snapshot.partition_count = count < RELIEFOS_BLOCK_MAX_PARTITIONS ? count : RELIEFOS_BLOCK_MAX_PARTITIONS;
            for (uint32_t i = 0; i < snapshot.partition_count; ++i) {
                Partition &part = snapshot.partitions[i];
                part.info = parts[i];
                int result = reliefos_block_partition_uuid(path, part.info.index, part.uuid);
                if (!result) result = readonly_state(part.info.path, part.read_only);
                if (result && !snapshot.partition_error) snapshot.partition_error = result;
            }
        }
    }
    collect_mounts(snapshot);
}

/** @brief Reject a stale selection after rescanning; compare GUID and extent, not row number alone. */
static int selected_target(const Request &request, Snapshot &fresh, int &row)
{
    if (fresh.selected_disk < 0) return -ESTALE;
    const Disk &disk = fresh.disks[fresh.selected_disk];
    if (!same_disk(request.disk, disk)) return -ESTALE;
    row = -1;
    if (request.action == Action::Create) return 0;
    for (uint32_t i = 0; i < fresh.partition_count; ++i) {
        const Partition &part = fresh.partitions[i];
        if (part.info.index != request.partition.info.index) continue;
        if (strcmp(part.info.path, request.partition.info.path) || !request.partition.uuid[0] ||
            strcmp(part.uuid, request.partition.uuid) || part.info.first_lba != request.partition.info.first_lba ||
            part.info.sector_count != request.partition.info.sector_count) return -ESTALE;
        row = static_cast<int>(i);
        return 0;
    }
    return -ESTALE;
}

/** @brief Create a dedicated root-owned mount directory without following a target symlink. */
static int mount_partition(const Disk &disk, const Partition &part)
{
    char path[MountPathSize];
    int length = snprintf(path, sizeof(path), "/mnt/disk%up%u", disk.info.id, part.info.index + 1U);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(path)) return -ENAMETOOLONG;
    struct stat target{};
    if (mkdir(path, 0755) < 0 && errno != EEXIST) return -errno;
    if (lstat(path, &target) < 0) return -errno;
    if (!S_ISDIR(target.st_mode) || target.st_uid != 0 || (target.st_mode & 0022)) return -EPERM;
    FILE *mounts = setmntent("/proc/self/mounts", "r");
    if (!mounts) return errno ? -errno : -EIO;
    int result = 0;
    mntent *entry;
    while ((entry = getmntent(mounts)))
        if (!strcmp(entry->mnt_dir, path)) { result = -EBUSY; break; }
    if (ferror(mounts)) result = -EIO;
    endmntent(mounts);
    if (result) return result;
    DIR *directory = opendir(path);
    if (!directory) return -errno;
    errno = 0;
    while (dirent *item = readdir(directory))
        if (strcmp(item->d_name, ".") && strcmp(item->d_name, "..")) { result = -ENOTEMPTY; break; }
    if (!result && errno) result = -errno;
    closedir(directory);
    if (result) return result;
    const char *type = part.info.filesystem == RELIEFOS_BLOCK_FILESYSTEM_FAT32 ? "vfat" :
                       part.info.filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXFAT ? "exfat" :
                       part.info.filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT2 ? "ext2" : "ext4";
    return mount(part.info.path, path, type, disk.read_only || part.read_only ? MS_RDONLY : 0, nullptr) < 0
               ? -errno : 0;
}

void perform_request(const Request &request, Reply &reply)
{
    const char *path = request.disk.info.path;
    collect_snapshot(path, reply.snapshot);
    if (request.action == Action::Refresh) {
        reply.status = reply.snapshot.partition_error ? reply.snapshot.partition_error :
                       reply.snapshot.mount_error ? reply.snapshot.mount_error : reply.snapshot.enumeration_error;
        return;
    }
    if (geteuid() != 0) { reply.status = -EACCES; return; }
    int row = -1;
    int result = selected_target(request, reply.snapshot, row);
    if (!result) result = action_allowed(reply.snapshot, request.action, row, static_cast<uint32_t>(geteuid()));
    if (!result && (request.action == Action::Create || request.action == Action::Format) &&
        !valid_filesystem(request.filesystem)) result = -EINVAL;
    if (!result && request.action == Action::Create && !request.size_mib) result = -EINVAL;
    if (!result) {
        const Disk &disk = reply.snapshot.disks[reply.snapshot.selected_disk];
        const Partition *part = row >= 0 ? &reply.snapshot.partitions[row] : nullptr;
        switch (request.action) {
        case Action::Create: {
            uint32_t index = 0;
            result = reliefos_block_gpt_create(path, request.filesystem, request.size_mib, request.label, &index);
            if (result) break;
            reply.created = true;
            char partition[RELIEFOS_BLOCK_PATH_LEN];
            result = reliefos_block_partition_path(path, index, partition, sizeof(partition));
            if (!result) {
                strcpy(reply.result_path, partition);
                result = reliefos_block_format(partition, request.filesystem, nullptr);
            }
            break;
        }
        case Action::Format: result = reliefos_block_format(part->info.path, request.filesystem, nullptr); break;
        case Action::Delete: result = reliefos_block_gpt_delete(path, part->info.index); break;
        case Action::Mount: result = mount_partition(disk, *part); break;
        case Action::Unmount: result = umount2(part->mount_path, 0) < 0 ? -errno : 0; break;
        default: result = -EINVAL; break;
        }
    }
    collect_snapshot(path, reply.snapshot);
    reply.status = result;
}
}
