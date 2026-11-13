#include "volumes.h"
extern "C" {
#include <reliefos/blockdev.h>
}
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace fm {
namespace {
struct Mount {
    Text path, type;
    uint64_t device = 0, id = 0, parent = 0;
    unsigned depth = 0;
};

int read_text(const char *path, Text &text)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    char bytes[PATH_MAX + 1];
    size_t used = 0;
    int result = 0;
    for (;;) {
        ssize_t n = read(fd, bytes + used, sizeof(bytes) - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { result = -errno; break; }
        if (!n) break;
        used += static_cast<size_t>(n);
        if (used == sizeof(bytes)) { result = -EOVERFLOW; break; }
    }
    close(fd);
    while (used && (bytes[used - 1] == '\n' || bytes[used - 1] == '\r')) --used;
    if (!result && (!used || memchr(bytes, 0, used))) result = -EIO;
    if (!result && !text.set(bytes, used)) result = -ENOMEM;
    return result;
}

int attribute(const char *name, const char *key, Text &text)
{
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "/sys/class/block/%s/%s", name, key);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(path)) return -ENAMETOOLONG;
    return read_text(path, text);
}

int numeric_attribute(const char *name, const char *key, uint64_t &value)
{
    Text text;
    int result = attribute(name, key, text);
    if (!result && !number(text.c_str(), value)) result = -EIO;
    return result;
}

int inspect(const char *name, Volume &volume, bool &partition)
{
    if (!device_name(name)) return -EINVAL;
    uint64_t index;
    int result = numeric_attribute(name, "partition", index);
    partition = !result;
    if (result && result != -ENOENT) return result;
    char sys[PATH_MAX], resolved[PATH_MAX];
    snprintf(sys, sizeof(sys), "/sys/class/block/%s", name);
    if (!realpath(sys, resolved)) return -errno;
    char *last = strrchr(resolved, '/');
    if (!last || strcmp(last + 1, name)) return -ESTALE;
    const char *disk = name;
    if (partition) {
        *last = 0;
        last = strrchr(resolved, '/');
        if (!last || !device_name(last + 1)) return -ESTALE;
        disk = last + 1;
    }
    uint64_t removable, sector_size, sectors, readonly;
    result = numeric_attribute(disk, "removable", removable);
    if (!result && removable != 1) result = -ENODEV;
    if (!result) result = numeric_attribute(disk, "queue/logical_block_size", sector_size);
    if (!result && sector_size != 512) result = -ENODEV;
    if (!result) result = numeric_attribute(disk, "diskseq", volume.sequence);
    if (!result && !volume.sequence) result = -ESTALE;
    if (!result) result = numeric_attribute(name, "size", sectors);
    if (!result && (!sectors || sectors > UINT64_MAX / 512)) result = -EOVERFLOW;
    if (!result) result = numeric_attribute(disk, "ro", readonly);
    if (result) return result;
    Text dev;
    result = attribute(name, "dev", dev);
    unsigned int major_number, minor_number;
    char extra;
    if (!result && sscanf(dev.c_str(), "%u:%u%c", &major_number, &minor_number, &extra) != 2)
        result = -EIO;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/dev/%s", name);
    struct stat node{};
    if (!result && lstat(path, &node) < 0) result = -errno;
    if (!result && (!S_ISBLK(node.st_mode) || major(node.st_rdev) != major_number ||
                     minor(node.st_rdev) != minor_number)) result = -ESTALE;
    if (result) return result;
    volume.device = node.st_rdev;
    volume.bytes = sectors * 512;
    volume.readonly = readonly != 0;
    if (!volume.name.set(name) || !volume.disk.set(disk)) return -ENOMEM;
    uint64_t after;
    result = numeric_attribute(disk, "diskseq", after);
    return result ? result : after != volume.sequence ? -ESTALE : 0;
}

bool protected_mount(const char *path)
{
    return !strcmp(path, "/") || path_under(path, "/boot") || path_under(path, "/usr") ||
           path_under(path, "/etc") || !strcmp(path, "/var") || !strcmp(path, "/home");
}

int mounts(Array<Mount> &list)
{
    FILE *stream = fopen("/proc/self/mountinfo", "r");
    if (!stream) return -errno;
    int result = 0;
    char *line = nullptr;
    size_t capacity = 0;
    while (getline(&line, &capacity, stream) >= 0) {
        Mount item;
        if (!parse_mountinfo(line, item.device, item.path, item.type, &item.id, &item.parent)) {
            result = -EIO;
            break;
        }
        if (!list.push(item)) { result = -ENOMEM; break; }
    }
    if (!result && ferror(stream)) result = -EIO;
    free(line);
    fclose(stream);
    return result;
}

void hex(const Text &text)
{
    for (size_t i = 0; i < text.size(); ++i) printf("%02x", static_cast<unsigned char>(text.c_str()[i]));
}

bool unhex(const char *bytes, size_t size, Text &text)
{
    if (size % 2 || size > 2 * PATH_MAX) return false;
    char decoded[PATH_MAX];
    for (size_t i = 0; i < size / 2; ++i) {
        unsigned value = 0;
        for (unsigned digit = 0; digit < 2; ++digit) {
            char c = bytes[i * 2 + digit];
            unsigned n = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 16;
            if (n > 15) return false;
            value = value * 16 + n;
        }
        if (!value) return false;
        decoded[i] = static_cast<char>(value);
    }
    return text.set(decoded, size / 2);
}

// All privileged helpers share a root-owned advisory lock and rescan after
// authentication. No password, user-provided mount path or shell command enters
// the operation protocol. The open block FD pins the original disk identity.
int action_lock()
{
    if (mkdir("/run/lock", 0755) < 0 && errno != EEXIST) return -errno;
    struct stat status{};
    if (lstat("/run/lock", &status) < 0) return -errno;
    if (!S_ISDIR(status.st_mode) || status.st_uid || (status.st_mode & 0022)) return -EPERM;
    int fd = open("/run/lock/reliefos-volumes.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -errno;
    int result = fstat(fd, &status) < 0 ? -errno : 0;
    if (!result && (!S_ISREG(status.st_mode) || status.st_uid || status.st_nlink != 1 ||
                    (status.st_mode & 0077))) result = -EPERM;
    struct flock lock{};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (!result && fcntl(fd, F_SETLK, &lock) < 0)
        result = errno == EACCES || errno == EAGAIN ? -EBUSY : -errno;
    if (result) { close(fd); return result; }
    return fd;
}

int directory(const char *path, mode_t mode)
{
    bool created = mkdir(path, mode) == 0;
    if (!created && errno != EEXIST) return -errno;
    if (created && (chown(path, 0, 0) < 0 || chmod(path, mode) < 0)) return -errno;
    struct stat status{};
    if (lstat(path, &status) < 0) return -errno;
    if (!S_ISDIR(status.st_mode) || status.st_uid || (status.st_mode & 0777) != mode)
        return -EPERM;
    return 0;
}

int mount_volume(const Volume &target, int fd, Text &path)
{
    const char *uid_text = getenv("SUDO_UID");
    uint64_t uid = 0;
    if (uid_text && (!number(uid_text, uid) || uid >= UINT32_MAX)) return -EINVAL;
    char user[PATH_MAX], point[PATH_MAX];
    snprintf(user, sizeof(user), "/run/media/%llu", static_cast<unsigned long long>(uid));
    int length = snprintf(point, sizeof(point), "%s/%s-%llu", user, target.name.c_str(),
                          static_cast<unsigned long long>(target.sequence));
    if (length < 0 || static_cast<size_t>(length) >= sizeof(point)) return -ENAMETOOLONG;
    int result = directory("/run/media", 0755);
    // Keep both the parent and target root-owned, so a user cannot rename a
    // target into a symlink while a privileged mount is entering the kernel.
    if (!result) result = directory(user, 0755);
    if (!result) result = directory(point, 0755);
    if (result) return result;
    Array<Mount> attached;
    result = mounts(attached);
    if (result) return result;
    for (const Mount &item : attached)
        if (item.path == point) return -EBUSY;
    DIR *entries = opendir(point);
    if (!entries) return -errno;
    errno = 0;
    while (dirent *entry = readdir(entries))
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) { result = -ENOTEMPTY; break; }
    if (!result && errno) result = -errno;
    closedir(entries);
    if (result) return result;
    char source[64];
    snprintf(source, sizeof(source), "/proc/self/fd/%d", fd);
    uint32_t filesystem;
    result = reliefos_block_probe_filesystem(source, &filesystem);
    if (result) return result;
    const char *type = filesystem == RELIEFOS_BLOCK_FILESYSTEM_FAT32 ? "vfat" :
                       filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXFAT ? "exfat" :
                       filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT2 ? "ext2" :
                       filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXT4 ? "ext4" : nullptr;
    if (!type) return -EOPNOTSUPP;
    uint64_t gid = 0;
    const char *gid_text = getenv("SUDO_GID");
    if (gid_text && (!number(gid_text, gid) || gid >= UINT32_MAX)) return -EINVAL;
    char options[128];
    snprintf(options, sizeof(options), "uid=%llu,gid=%llu,fmask=0022,dmask=0022",
             static_cast<unsigned long long>(uid), static_cast<unsigned long long>(gid));
    unsigned long flags = MS_NOSUID | MS_NODEV;
    if (target.readonly) flags |= MS_RDONLY;
    const char *data = filesystem == RELIEFOS_BLOCK_FILESYSTEM_FAT32 ||
                       filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXFAT ? options : nullptr;
    if (mount(source, point, type, flags, data) < 0) return -errno;
    if (!path.set(point)) {
        (void)umount2(point, UMOUNT_NOFOLLOW);
        return -ENOMEM;
    }
    return 0;
}

unsigned busy_processes(const Array<Mount> &attached, bool raw_only = false)
{
    DIR *proc = opendir("/proc");
    if (!proc) return 0;
    unsigned shown = 0;
    while (dirent *entry = readdir(proc)) {
        uint64_t pid;
        if (!number(entry->d_name, pid) || !pid || pid == static_cast<uint64_t>(getpid())) continue;
        char path[PATH_MAX];
        bool busy = false;
        const char *special[] = {"cwd", "root", "exe"};
        for (const char *name : special) {
            if (raw_only) break;
            snprintf(path, sizeof(path), "/proc/%s/%s", entry->d_name, name);
            struct stat status{};
            if (stat(path, &status) == 0)
                for (const Mount &mount : attached) if (mount.device == status.st_dev) busy = true;
        }
        snprintf(path, sizeof(path), "/proc/%s/fd", entry->d_name);
        DIR *fds = opendir(path);
        if (fds) {
            while (!busy) {
                dirent *file = readdir(fds);
                if (!file) break;
                if (file->d_name[0] == '.') continue;
                struct stat status{};
                if (fstatat(dirfd(fds), file->d_name, &status, 0) == 0)
                    for (const Mount &mount : attached)
                        if ((S_ISBLK(status.st_mode) && mount.device == status.st_rdev) ||
                            (!raw_only && mount.device == status.st_dev)) busy = true;
            }
            closedir(fds);
        }
        if (!busy) continue;
        Text name;
        snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
        (void)read_text(path, name);
        printf("BUSY\t%llu\t", static_cast<unsigned long long>(pid)); hex(name); putchar('\n');
        if (++shown == 16) break;
    }
    closedir(proc);
    return shown;
}

int eject_volume(const Volume &target, const Array<Volume> &all)
{
    Array<Mount> all_mounts, attached, devices;
    for (const Volume &volume : all) {
        if (volume.disk != target.disk) continue;
        Mount device;
        device.device = volume.device;
        if (!devices.push(device)) return -ENOMEM;
    }
    if (busy_processes(devices, true)) return -EBUSY;
    int result = mounts(all_mounts);
    if (result) return result;
    for (const Mount &mount : all_mounts) {
        for (const Volume &volume : all) {
            if (volume.disk != target.disk || volume.device != mount.device) continue;
            if (protected_mount(mount.path.c_str())) return -EPERM;
            if (!attached.push(mount)) return -ENOMEM;
            break;
        }
    }
    // Descendant attachments leave first. At equal depth, newer bind views
    // leave before the original mount whose lifetime they may retain.
    for (Mount &mount : attached) {
        uint64_t parent = mount.parent;
        for (size_t steps = 0; parent != mount.id && steps < all_mounts.size(); ++steps) {
            const Mount *ancestor = nullptr;
            for (const Mount &item : all_mounts)
                if (item.id == parent) { ancestor = &item; break; }
            if (!ancestor) break;
            ++mount.depth;
            if (ancestor->id == ancestor->parent) break;
            parent = ancestor->parent;
        }
    }
    for (size_t i = 0; i < attached.size(); ++i)
        for (size_t j = i + 1; j < attached.size(); ++j)
            if (mount_before(attached[j].depth, attached[j].id, attached[i].depth, attached[i].id)) {
                Mount swap = attached[i]; attached[i] = attached[j]; attached[j] = swap;
            }
    for (const Mount &item : attached) {
        if (umount2(item.path.c_str(), UMOUNT_NOFOLLOW) < 0) {
            result = -errno;
            if (result == -EBUSY) busy_processes(attached);
            return result;
        }
        if (path_under(item.path.c_str(), "/run/media")) (void)rmdir(item.path.c_str());
    }
    Array<Mount> remaining;
    result = mounts(remaining);
    if (result) return result;
    for (const Mount &item : remaining)
        for (const Volume &volume : all)
            if (volume.disk == target.disk && volume.device == item.device) return -EBUSY;
    return 0;
}
} // namespace

bool device_name(const char *name)
{
    if (!name || !*name || strlen(name) > NAME_MAX || !strcmp(name, ".") || !strcmp(name, ".."))
        return false;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(name); *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.')) return false;
    return true;
}

bool number(const char *text, uint64_t &value)
{
    if (!text || !*text) return false;
    uint64_t next = 0;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9' || next > (UINT64_MAX - (*p - '0')) / 10) return false;
        next = next * 10 + *p - '0';
    }
    value = next;
    return true;
}

bool path_under(const char *path, const char *directory)
{
    size_t size = strlen(directory);
    return size && !strncmp(path, directory, size) && (!path[size] || path[size] == '/' ||
                                                      (size == 1 && directory[0] == '/'));
}

bool same_volume(const Volume &a, const Volume &b)
{
    return a.name == b.name && a.disk == b.disk && a.sequence == b.sequence && a.device == b.device;
}

bool mount_before(unsigned depth, uint64_t id, unsigned other_depth, uint64_t other_id)
{
    return depth > other_depth || (depth == other_depth && id > other_id);
}

bool parse_mountinfo(const char *line, uint64_t &device, Text &path, Text &type,
                     uint64_t *mount_id, uint64_t *parent_id)
{
    const char *fields[6];
    const char *at = line;
    for (unsigned i = 0; i < 6; ++i) {
        fields[i] = at;
        at = strchr(at, ' ');
        if (!at) return false;
        ++at;
    }
    unsigned major_number, minor_number;
    char extra;
    Text identity, id_text, parent_text;
    uint64_t id, parent;
    if (!id_text.set(fields[0], fields[1] - fields[0] - 1) || !number(id_text.c_str(), id) || !id ||
        !parent_text.set(fields[1], fields[2] - fields[1] - 1) || !number(parent_text.c_str(), parent)) return false;
    if (!identity.set(fields[2], fields[3] - fields[2] - 1) ||
        sscanf(identity.c_str(), "%u:%u%c", &major_number, &minor_number, &extra) != 2 ||
        major_number > 4095 || minor_number > 1048575) return false;
    device = makedev(major_number, minor_number);
    const char *start = fields[4], *end = fields[5] - 1;
    char decoded[PATH_MAX];
    size_t used = 0;
    for (const char *p = start; p != end; ++p) {
        unsigned char c = *p;
        if (c == '\\') {
            if (end - p < 4 || p[1] < '0' || p[1] > '7' || p[2] < '0' || p[2] > '7' ||
                p[3] < '0' || p[3] > '7') return false;
            unsigned value = (p[1] - '0') * 64 + (p[2] - '0') * 8 + p[3] - '0';
            if (!value || value > 255) return false;
            c = value; p += 3;
        }
        if (used == sizeof(decoded) - 1) return false;
        decoded[used++] = c;
    }
    if (!used || decoded[0] != '/' || !path.set(decoded, used)) return false;
    const char *separator = strstr(at - 1, " - ");
    if (!separator) return false;
    const char *type_end = strchr(separator + 3, ' ');
    if (!type_end || type_end == separator + 3 || !type.set(separator + 3, type_end - separator - 3)) return false;
    if (mount_id) *mount_id = id;
    if (parent_id) *parent_id = parent;
    return true;
}

int scan_volumes(Array<Volume> &output, bool include_disks)
{
    Array<Volume> all, filtered;
    DIR *stream = opendir("/sys/class/block");
    if (!stream) return -errno;
    int result = 0;
    errno = 0;
    while (dirent *entry = readdir(stream)) {
        if (entry->d_name[0] == '.') continue;
        Volume volume;
        bool partition;
        int inspected = inspect(entry->d_name, volume, partition);
        if (inspected == -ENOMEM) { result = inspected; break; }
        if (!inspected && !all.push(volume)) { result = -ENOMEM; break; }
        errno = 0;
    }
    if (!result && errno) result = -errno;
    closedir(stream);
    if (result) return result;
    Array<Mount> attached;
    result = mounts(attached);
    if (result) return result;
    for (Volume &volume : all) {
        bool has_partitions = false;
        if (volume.name == volume.disk)
            for (const Volume &child : all)
                if (child.disk == volume.disk && child.name != volume.name) has_partitions = true;
        if (has_partitions && !include_disks) continue;
        for (const Mount &mount : attached) {
            if (mount.device != volume.device) continue;
            if (!volume.mounts++) { volume.mount = mount.path; volume.filesystem = mount.type; }
            volume.system |= protected_mount(mount.path.c_str());
        }
        if (!filtered.push(volume)) return -ENOMEM;
    }
    // Stable ordering keeps periodic discovery from moving keyboard selection.
    for (size_t i = 0; i < filtered.size(); ++i)
        for (size_t j = i + 1; j < filtered.size(); ++j)
            if (strcmp(filtered[i].name.c_str(), filtered[j].name.c_str()) > 0) {
                Volume swap = filtered[i]; filtered[i] = filtered[j]; filtered[j] = swap;
            }
    output.swap(filtered);
    return 0;
}

void print_volumes(const Array<Volume> &volumes)
{
    for (const Volume &volume : volumes) {
        printf("%s\t%s\t%llu\t%llu\t%llu\t%u\t%u\t%u\t", volume.name.c_str(),
               volume.disk.c_str(), static_cast<unsigned long long>(volume.sequence),
               static_cast<unsigned long long>(volume.device), static_cast<unsigned long long>(volume.bytes),
               volume.mounts, volume.readonly, volume.system);
        hex(volume.mount); putchar('\t'); hex(volume.filesystem); putchar('\n');
    }
}

bool parse_volumes(const char *bytes, size_t size, Array<Volume> &output)
{
    Array<Volume> parsed;
    const char *end = bytes + size;
    while (bytes < end) {
        const char *line_end = static_cast<const char *>(memchr(bytes, '\n', end - bytes));
        if (!line_end || line_end - bytes > 5 * PATH_MAX) return false;
        Text fields[10];
        const char *at = bytes;
        for (unsigned i = 0; i < 10; ++i) {
            const char *next = i == 9 ? line_end : static_cast<const char *>(memchr(at, '\t', line_end - at));
            if (!next || !fields[i].set(at, next - at)) return false;
            at = next + 1;
        }
        Volume volume;
        uint64_t count, readonly, system;
        if (!device_name(fields[0].c_str()) || !device_name(fields[1].c_str()) ||
            !number(fields[2].c_str(), volume.sequence) || !volume.sequence ||
            !number(fields[3].c_str(), volume.device) || !volume.device ||
            !number(fields[4].c_str(), volume.bytes) ||
            !number(fields[5].c_str(), count) || count > UINT_MAX ||
            !number(fields[6].c_str(), readonly) || readonly > 1 ||
            !number(fields[7].c_str(), system) || system > 1 ||
            !unhex(fields[8].c_str(), fields[8].size(), volume.mount) ||
            !unhex(fields[9].c_str(), fields[9].size(), volume.filesystem)) return false;
        volume.name = fields[0]; volume.disk = fields[1]; volume.mounts = count;
        volume.readonly = readonly; volume.system = system;
        if ((volume.mounts && (volume.mount.empty() || volume.mount.c_str()[0] != '/')) ||
            (!volume.mounts && !volume.mount.empty())) return false;
        for (const Volume &old : parsed) if (old.name == volume.name) return false;
        if (!parsed.push(volume)) return false;
        bytes = line_end + 1;
    }
    output.swap(parsed);
    return true;
}

int parse_volume_reply(const char *bytes, size_t size, Text &mount, Text &users)
{
    char processes[2048]{};
    size_t used = 0;
    bool complete = false;
    int result = -EPROTO;
    const char *end = bytes + size;
    while (bytes < end) {
        const char *next = static_cast<const char *>(memchr(bytes, '\n', end - bytes));
        if (!next || complete) return -EPROTO;
        const char *line_end = next > bytes && next[-1] == '\r' ? next - 1 : next;
        Text line;
        if (!line.set(bytes, line_end - bytes)) return -ENOMEM;
        if (!strncmp(line.c_str(), "OK\t", 3)) {
            if (!unhex(line.c_str() + 3, line.size() - 3, mount) ||
                (!mount.empty() && mount.c_str()[0] != '/')) return -EPROTO;
            result = 0; complete = true;
        } else if (!strncmp(line.c_str(), "ERROR\t", 6)) {
            uint64_t error;
            if (!number(line.c_str() + 6, error) || !error || error > 4095) return -EPROTO;
            result = -static_cast<int>(error); complete = true;
        } else if (!strncmp(line.c_str(), "BUSY\t", 5)) {
            const char *separator = strchr(line.c_str() + 5, '\t');
            Text pid, name;
            uint64_t id;
            if (!separator || !pid.set(line.c_str() + 5, separator - line.c_str() - 5) ||
                !number(pid.c_str(), id) || !id ||
                !unhex(separator + 1, strlen(separator + 1), name)) return -EPROTO;
            char safe[128];
            size_t length = name.size() < sizeof(safe) - 1 ? name.size() : sizeof(safe) - 1;
            for (size_t i = 0; i < length; ++i) {
                unsigned char c = name.c_str()[i];
                safe[i] = c >= 32 && c < 127 ? c : '?';
            }
            safe[length] = 0;
            int n = snprintf(processes + used, sizeof(processes) - used, "\nPID %llu  %s",
                             static_cast<unsigned long long>(id), safe);
            if (n < 0 || static_cast<size_t>(n) >= sizeof(processes) - used) return -EOVERFLOW;
            used += n;
        } else return -EPROTO;
        bytes = next + 1;
    }
    if (!complete) return -EPROTO;
    if (!users.set(processes, used)) return -ENOMEM;
    return result;
}

int volume_command(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--volume-list")) {
        Array<Volume> volumes;
        int result = scan_volumes(volumes);
        if (result) { fprintf(stderr, "[fileman] volumes: %s\n", strerror(-result)); return 1; }
        print_volumes(volumes);
        return ferror(stdout) ? 1 : 0;
    }
    bool authenticate = argc == 6 && !strcmp(argv[1], "--volume-auth");
    bool helper = argc == 5 && !strcmp(argv[1], "--volume-action");
    if (!authenticate && !helper) {
        return argc > 1 && !strncmp(argv[1], "--volume-", 9) ? 2 : -1;
    }
    uint64_t sequence;
    if ((strcmp(argv[2], "mount") && strcmp(argv[2], "eject")) || !device_name(argv[3]) ||
        !number(argv[4], sequence) || !sequence) return 2;
    if (authenticate && geteuid() != 0) {
        // Pass the application's selected locale to the existing askpass GUI.
        if (setenv("LC_ALL", argv[5], 1) || setenv("LANG", argv[5], 1) ||
            setenv("SUDO_ASKPASS", "/usr/lib/reliefos/apps/reliefaskpass/reliefaskpass.elf", 1)) return 1;
        const char *args[] = {"/usr/bin/sudo", "-A", "--", Program, "--volume-action",
                              argv[2], argv[3], argv[4], nullptr};
        execv(args[0], const_cast<char *const *>(args));
        return 127;
    }
    if (geteuid() != 0) { puts("ERROR\t1"); return 1; }
    int lock = action_lock();
    int result = lock < 0 ? lock : 0;
    Volume target;
    bool partition;
    if (!result) result = inspect(argv[3], target, partition);
    if (!result && target.sequence != sequence) result = -ESTALE;
    char node[PATH_MAX];
    snprintf(node, sizeof(node), "/dev/%s", argv[3]);
    int fd = result ? -1 : open(node, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (!result && fd < 0) result = -errno;
    uint64_t opened_sequence = 0;
    struct stat status{};
    if (!result && fstat(fd, &status) < 0) result = -errno;
    if (!result && (!S_ISBLK(status.st_mode) || status.st_rdev != target.device)) result = -ESTALE;
    if (!result && ioctl(fd, BLKGETDISKSEQ, &opened_sequence) < 0) result = -errno;
    if (!result && opened_sequence != sequence) result = -ESTALE;
    Array<Volume> all;
    if (!result) result = scan_volumes(all, true);
    Text mount_path;
    if (!result && !strcmp(argv[2], "mount")) {
        for (const Volume &volume : all)
            if (same_volume(target, volume)) { target = volume; break; }
        if (target.system) result = -EPERM;
        else if (target.mounts) mount_path = target.mount;
        else result = mount_volume(target, fd, mount_path);
    } else if (!result) {
        result = eject_volume(target, all);
        if (!result && fsync(fd) < 0) result = -errno;
    }
    if (fd >= 0) close(fd);
    if (lock >= 0) close(lock);
    if (result) { printf("ERROR\t%d\n", -result); return 1; }
    fputs("OK\t", stdout); hex(mount_path); putchar('\n');
    return ferror(stdout) ? 1 : 0;
}
} // namespace fm
