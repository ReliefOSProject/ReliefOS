#define _POSIX_C_SOURCE 200809L
#include "installer_update.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int join_path(char *path, size_t capacity, const char *root, const char *relative)
{
    int length = snprintf(path, capacity, "%s/%s", root, relative);
    return length < 0 || (size_t)length >= capacity ? -ENAMETOOLONG : 0;
}

static int copy_field(char *out, size_t capacity, const char *value)
{
    size_t length = strlen(value);
    if (length >= capacity || strpbrk(value, "\r\n\t")) return -EINVAL;
    memcpy(out, value, length + 1);
    return 0;
}

static int parse_number(const char *text, uint64_t *number)
{
    char *end;
    if (!*text || *text < '0' || *text > '9') return -EINVAL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || *end) return -EINVAL;
    *number = value;
    return 0;
}

static int unquote(char *value)
{
    size_t length = strlen(value);
    if (length && (value[0] == '"' || value[0] == '\'')) {
        if (length < 2 || value[length - 1] != value[0]) return -EINVAL;
        memmove(value, value + 1, length - 2);
        value[length - 2] = 0;
    }
    if (!*value || strspn(value, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+-") != strlen(value))
        return -EINVAL;
    return 0;
}

/* os-release permits the standard vendor-file symlink. Resolve that one
 * inside the mounted target instead of following an absolute host path. */
static FILE *open_release(const char *root)
{
    char path[PATH_MAX], link[PATH_MAX];
    struct stat status;
    int result = join_path(path, sizeof(path), root, "etc/os-release");
    if (result) { errno = -result; return NULL; }
    if (lstat(path, &status) < 0) return NULL;
    if (S_ISLNK(status.st_mode)) {
        ssize_t length = readlink(path, link, sizeof(link) - 1);
        if (length < 0) return NULL;
        link[length] = 0;
        if (strcmp(link, "../usr/lib/os-release") && strcmp(link, "/usr/lib/os-release")) {
            errno = EINVAL;
            return NULL;
        }
        result = join_path(path, sizeof(path), root, "usr/lib/os-release");
        if (result) { errno = -result; return NULL; }
    }
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return NULL;
    FILE *stream = fdopen(fd, "r");
    if (!stream) { int saved = errno; close(fd); errno = saved; }
    return stream;
}

int installer_update_read_system(const char *root, struct installer_update_info *info)
{
    static const char *const directories[] = {
        "etc", "etc/apk", "lib", "lib/apk", "lib/apk/db", "var", "var/lib",
        "usr", "usr/bin", "usr/lib", "usr/share", "bin", "sbin", "boot",
    };
    char path[PATH_MAX], line[512], id[32] = "";
    struct stat status;
    if (!root || !info) return -EINVAL;
    for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); ++i) {
        int result = join_path(path, sizeof(path), root, directories[i]);
        if (result) return result;
        if (lstat(path, &status) < 0) return -errno;
        if (!S_ISDIR(status.st_mode)) return -ENOTDIR;
    }
    static const char *const namespaces[] = {
        "etc/reliefos", "etc/leonos", "var/lib/reliefos", "var/lib/leonos",
        "usr/lib/reliefos", "usr/lib/leonos",
    };
    for (size_t i = 0; i < sizeof(namespaces) / sizeof(namespaces[0]); ++i) {
        int error = join_path(path, sizeof(path), root, namespaces[i]);
        if (error) return error;
        if (lstat(path, &status) < 0) { if (errno == ENOENT) continue; return -errno; }
        if (!S_ISDIR(status.st_mode)) return -ENOTDIR;
    }
    FILE *stream = open_release(root);
    if (!stream) return -errno;
    int result = 0;
    info->installed_version[0] = 0;
    while (fgets(line, sizeof(line), stream)) {
        if (!strchr(line, '\n') && !feof(stream)) { result = -EOVERFLOW; break; }
        line[strcspn(line, "\r\n")] = 0;
        char *value = strchr(line, '=');
        if (!value) continue;
        *value++ = 0;
        if (strcmp(line, "ID") && strcmp(line, "VERSION_ID")) continue;
        result = unquote(value);
        if (!result) result = !strcmp(line, "ID")
            ? copy_field(id, sizeof(id), value)
            : copy_field(info->installed_version, sizeof(info->installed_version), value);
        if (result) break;
    }
    if (ferror(stream) && !result) result = -EIO;
    if (fclose(stream) && !result) result = -errno;
    if (result) return result;
    if (strcmp(id, "reliefos") && strcmp(id, "leonos")) return -ENODEV;
    if (!info->installed_version[0]) return -EINVAL;
    if (join_path(path, sizeof(path), root, "etc/reliefos/desktop-backend")) return -ENAMETOOLONG;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 && errno == ENOENT) {
        if (join_path(path, sizeof(path), root, "etc/leonos/desktop-backend")) return -ENAMETOOLONG;
        fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    }
    if (fd < 0) {
        if (errno != ENOENT) return -errno;
        strcpy(info->profile, "xorg");
        return 0;
    }
    size_t used = 0;
    while (used < sizeof(line) - 1) {
        ssize_t length = read(fd, line + used, sizeof(line) - 1 - used);
        if (length < 0 && errno == EINTR) continue;
        if (length < 0) { result = -errno; break; }
        if (!length) break;
        used += (size_t)length;
    }
    if (close(fd) < 0 && !result) result = -errno;
    if (result) return result;
    if ((used == 4 && !memcmp(line, "xorg", 4)) ||
        (used == 5 && !memcmp(line, "xorg\n", 5))) strcpy(info->profile, "xorg");
    else if ((used == 7 && !memcmp(line, "console", 7)) ||
             (used == 8 && !memcmp(line, "console\n", 8))) strcpy(info->profile, "console");
    else return -EINVAL;
    return 0;
}

int installer_update_read_media(const char *root, const char *profile, struct installer_update_info *info)
{
    if (!root || !profile || !info) return -EINVAL;
    int result = installer_update_read_system(root, info);
    if (result) return result;
    if (strcmp(profile, info->profile)) return -EINVAL;
    const char *const metadata[] = {"etc/apk/world", "lib/apk/db/installed",
        "usr/share/reliefos/apk/repository/packages.adb"};
    for (size_t i = 0; i < sizeof(metadata) / sizeof(metadata[0]); ++i) {
        char path[PATH_MAX];
        struct stat status;
        result = join_path(path, sizeof(path), root, metadata[i]);
        if (result) return result;
        if (lstat(path, &status) < 0) return -errno;
        if (!S_ISREG(status.st_mode) || status.st_size <= 0) return -EINVAL;
    }
    return 0;
}

static int package_record(struct installer_update_packages *out, const char *name,
                          const char *version, uint64_t bytes)
{
    if (strncmp(name, "reliefos-", 9) && strncmp(name, "leonos-", 7)) return 0;
    if (UINT64_MAX - out->installed_bytes < bytes) return -EOVERFLOW;
    out->installed_bytes += bytes;
    if (out->largest_package < bytes) out->largest_package = bytes;
    if (!strcmp(name, "reliefos-kernel") || !strcmp(name, "leonos-kernel"))
        return copy_field(out->kernel_version, sizeof(out->kernel_version), version);
    return 0;
}

int installer_update_read_packages(const char *root, struct installer_update_packages *out)
{
    char path[PATH_MAX], line[8192], name[128] = "", version[96] = "";
    uint64_t bytes = 0;
    if (!root || !out) return -EINVAL;
    memset(out, 0, sizeof(*out));
    int result = join_path(path, sizeof(path), root, "lib/apk/db/installed");
    if (result) return result;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -errno;
    FILE *stream = fdopen(fd, "r");
    if (!stream) { result = -errno; close(fd); return result; }
    while (fgets(line, sizeof(line), stream)) {
        if (!strchr(line, '\n') && !feof(stream)) { result = -EOVERFLOW; break; }
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) {
            result = package_record(out, name, version, bytes);
            name[0] = version[0] = 0;
            bytes = 0;
        } else if (!strncmp(line, "P:", 2)) result = copy_field(name, sizeof(name), line + 2);
        else if (!strncmp(line, "V:", 2)) result = copy_field(version, sizeof(version), line + 2);
        else if (!strncmp(line, "I:", 2)) result = parse_number(line + 2, &bytes);
        if (result) break;
    }
    if (!result) result = ferror(stream) ? -EIO : package_record(out, name, version, bytes);
    if (fclose(stream) && !result) result = -errno;
    return result;
}

int installer_update_decode(struct installer_update_info *info, unsigned *progress,
                            char *message, size_t capacity, const char *line)
{
    if (!info || !progress || !message || !capacity || !line) return -EINVAL;
    if (!strncmp(line, "error\t", 6)) {
        char *end;
        errno = 0;
        long value = strtol(line + 6, &end, 10);
        if (errno || value < 1 || value > 4095 || *end != '\t') return -EINVAL;
        int result = copy_field(message, capacity, end + 1);
        if (!result) result = copy_field(info->failure, sizeof(info->failure), end + 1);
        if (result) return result;
        info->error_number = (int)value;
        return 3;
    }
    struct field { const char *key; char *value; size_t size; };
#define FIELD(name) {#name, info->name, sizeof(info->name)}
    struct field fields[] = {
        FIELD(disk), FIELD(root_device), FIELD(esp_device), FIELD(root_uuid), FIELD(esp_uuid),
        FIELD(installed_version), FIELD(media_version), FIELD(profile), FIELD(installed_kernel),
        FIELD(media_kernel), FIELD(index_sha256), FIELD(database_sha256), FIELD(world_sha256), FIELD(pending),
    };
#undef FIELD
    if (!strncmp(line, "progress\t", 9)) {
        const char *tab = strchr(line + 9, '\t');
        if (!tab || (size_t)(tab - line - 9) >= 4) return -EINVAL;
        char text[4];
        memcpy(text, line + 9, (size_t)(tab - line - 9));
        text[tab - line - 9] = 0;
        uint64_t value;
        int result = parse_number(text, &value);
        if (result || value > 100) return -EINVAL;
        result = copy_field(message, capacity, tab + 1);
        if (result) return result;
        *progress = (unsigned)value;
        return 2;
    }
    if (strncmp(line, "info\t", 5)) return 0;
    const char *tab = strchr(line + 5, '\t');
    if (!tab) return -EINVAL;
    size_t length = (size_t)(tab - line - 5);
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        if (strlen(fields[i].key) == length && !memcmp(fields[i].key, line + 5, length)) {
            int result = copy_field(fields[i].value, fields[i].size, tab + 1);
            return result ? result : 1;
        }
    }
    uint64_t value;
    int result = parse_number(tab + 1, &value);
    if (result) return result;
    if (length == 10 && !memcmp(line + 5, "free_bytes", length)) info->free_bytes = value;
    else if (length == 14 && !memcmp(line + 5, "required_bytes", length)) info->required_bytes = value;
    else if (length == 10 && !memcmp(line + 5, "boot_modes", length) && value <= 3) info->boot_modes = (uint32_t)value;
    else return -EINVAL;
    return 1;
}
