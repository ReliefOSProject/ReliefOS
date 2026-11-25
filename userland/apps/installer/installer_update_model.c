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

#define SYSTEM_METADATA_MAX (64u * 1024u)
#define PACKAGE_DATABASE_MAX (64u * 1024u * 1024u)
#define PACKAGE_COUNT_MAX 8192u

/* Walk each ancestor through a held directory. O_NONBLOCK makes a replaced
 * FIFO/device rejectable by fstat instead of hanging before type validation. */
static int open_metadata(const char *root, const char *relative, off_t maximum)
{
    char path[PATH_MAX];
    if (!root || !relative || !*relative || *relative == '/' || strlen(relative) >= sizeof(path))
        return -EINVAL;
    strcpy(path, relative);
    int parent = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parent < 0) return -errno;
    char *state, *part = strtok_r(path, "/", &state);
    while (part) {
        if (!strcmp(part, ".") || !strcmp(part, "..")) { close(parent); return -EINVAL; }
        char *next = strtok_r(NULL, "/", &state);
        int fd = openat(parent, part, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC |
                        (next ? O_DIRECTORY : 0));
        int error = errno;
        close(parent);
        if (fd < 0) return -error;
        if (!next) {
            struct stat status;
            int result = fstat(fd, &status) < 0 ? -errno :
                !S_ISREG(status.st_mode) || status.st_size < 0 ? -EINVAL :
                status.st_size > maximum ? -EOVERFLOW : 0;
            if (result) { close(fd); return result; }
            return fd;
        }
        parent = fd;
        part = next;
    }
    close(parent);
    return -EINVAL;
}

static int metadata_line(FILE *stream, char *line, size_t capacity, size_t *remaining)
{
    size_t used = 0;
    for (;;) {
        int byte = fgetc(stream);
        if (byte == EOF) {
            if (ferror(stream)) return errno ? -errno : -EIO;
            line[used] = 0;
            return used != 0;
        }
        if (!*remaining || used + 1 >= capacity) return -EOVERFLOW;
        --*remaining;
        if (!byte) return -EINVAL;
        line[used++] = (char)byte;
        if (byte == '\n') { line[used] = 0; return 1; }
    }
}

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
    const char *relative = path + strlen(root);
    while (*relative == '/') ++relative;
    int fd = open_metadata(root, relative, SYSTEM_METADATA_MAX);
    if (fd < 0) { errno = -fd; return NULL; }
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
    size_t remaining = SYSTEM_METADATA_MAX;
    unsigned fields = 0;
    int got;
    while ((got = metadata_line(stream, line, sizeof(line), &remaining)) > 0) {
        line[strcspn(line, "\r\n")] = 0;
        char *value = strchr(line, '=');
        if (!value) continue;
        *value++ = 0;
        if (strcmp(line, "ID") && strcmp(line, "VERSION_ID")) continue;
        unsigned bit = !strcmp(line, "ID") ? 1 : 2;
        if (fields & bit) { result = -EINVAL; break; }
        fields |= bit;
        result = unquote(value);
        if (!result) result = !strcmp(line, "ID")
            ? copy_field(id, sizeof(id), value)
            : copy_field(info->installed_version, sizeof(info->installed_version), value);
        if (result) break;
    }
    if (got < 0 && !result) result = got;
    if (ferror(stream) && !result) result = -EIO;
    if (fclose(stream) && !result) result = -errno;
    if (result) return result;
    if (strcmp(id, "reliefos") && strcmp(id, "leonos")) return -ENODEV;
    if (!info->installed_version[0]) return -EINVAL;
    int fd = open_metadata(root, "etc/reliefos/desktop-backend", sizeof(line) - 1);
    if (fd == -ENOENT) fd = open_metadata(root, "etc/leonos/desktop-backend", sizeof(line) - 1);
    if (fd < 0) {
        if (fd != -ENOENT) return fd;
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
        int fd = open_metadata(root, metadata[i], PACKAGE_DATABASE_MAX);
        if (fd < 0) return fd;
        struct stat status;
        result = fstat(fd, &status) < 0 ? -errno : status.st_size <= 0 ? -EINVAL : 0;
        if (close(fd) < 0 && !result) result = -errno;
        if (result) return result;
    }
    return 0;
}

typedef int (*package_visitor)(void *, const char *, const char *, uint64_t);

static int finish_package(package_visitor visitor, void *context, const char *name,
                          const char *version, uint64_t bytes, unsigned fields,
                          unsigned *count)
{
    if (!fields) return 0;
    if (fields != 15 || !*name || !*version ||
        strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+_.-") != strlen(name) ||
        !strcmp(name, ".") || !strcmp(name, "..")) return -EINVAL;
    if (*count == PACKAGE_COUNT_MAX) return -EOVERFLOW;
    ++*count;
    return visitor(context, name, version, bytes);
}

/* One scanner validates the bounded text database for both profile discovery
 * and space estimates. An incomplete or duplicate scalar must not silently
 * replace a name/size used by the estimate. */
static int walk_packages(const char *root, package_visitor visitor, void *context)
{
    char line[8192], name[128] = "", version[96] = "";
    uint64_t bytes = 0;
    unsigned fields = 0, count = 0;
    int fd = open_metadata(root, "lib/apk/db/installed", PACKAGE_DATABASE_MAX);
    if (fd < 0) return fd;
    FILE *stream = fdopen(fd, "r");
    if (!stream) { int result = -errno; close(fd); return result; }
    size_t remaining = PACKAGE_DATABASE_MAX;
    int got, result = 0;
    while ((got = metadata_line(stream, line, sizeof(line), &remaining)) > 0) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) {
            result = finish_package(visitor, context, name, version, bytes, fields, &count);
            name[0] = version[0] = 0;
            bytes = 0;
            fields = 0;
        } else {
            unsigned field = !strncmp(line, "P:", 2) ? 1u :
                !strncmp(line, "V:", 2) ? 2u : !strncmp(line, "I:", 2) ? 4u : 0;
            if (field && (fields & field)) { result = -EINVAL; break; }
            fields |= field | 8u;
            if (field == 1) result = copy_field(name, sizeof(name), line + 2);
            else if (field == 2) result = copy_field(version, sizeof(version), line + 2);
            else if (field == 4) result = parse_number(line + 2, &bytes);
        }
        if (result) break;
    }
    if (got < 0 && !result) result = got;
    if (!result) result = ferror(stream) ? -EIO :
        finish_package(visitor, context, name, version, bytes, fields, &count);
    if (fclose(stream) && !result) result = -errno;
    return result;
}

static int package_record(void *context, const char *name, const char *version, uint64_t bytes)
{
    struct installer_update_packages *out = context;
    if (!strcmp(name, "xfce4-session")) out->has_xfce_session = 1;
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
    if (!root || !out) return -EINVAL;
    memset(out, 0, sizeof(*out));
    return walk_packages(root, package_record, out);
}

struct package_size { char name[128]; uint64_t bytes; };
struct package_sizes { struct package_size *items; size_t count, capacity; };

static int collect_size(void *context, const char *name, const char *version, uint64_t bytes)
{
    (void)version;
    struct package_sizes *table = context;
    if (table->count == table->capacity) {
        size_t capacity = table->capacity ? table->capacity * 2 : 64;
        if (capacity > PACKAGE_COUNT_MAX) capacity = PACKAGE_COUNT_MAX;
        if (capacity <= table->count || capacity > SIZE_MAX / sizeof(*table->items)) return -EOVERFLOW;
        struct package_size *items = realloc(table->items, capacity * sizeof(*items));
        if (!items) return -ENOMEM;
        table->items = items;
        table->capacity = capacity;
    }
    struct package_size *item = &table->items[table->count++];
    strcpy(item->name, name);
    item->bytes = bytes;
    return 0;
}

static int compare_size(const void *left, const void *right)
{
    const struct package_size *a = left, *b = right;
    return strcmp(a->name, b->name);
}

static int read_sizes(const char *root, struct package_sizes *table)
{
    int result = walk_packages(root, collect_size, table);
    if (result) return result;
    if (table->count) qsort(table->items, table->count, sizeof(*table->items), compare_size);
    for (size_t i = 1; i < table->count; ++i)
        if (!strcmp(table->items[i - 1].name, table->items[i].name)) return -EINVAL;
    return 0;
}

int installer_update_measure_growth(const char *installed_root, const char *media_root,
                                    uint64_t *growth, uint64_t *largest)
{
    if (!growth || !largest) return -EINVAL;
    *growth = *largest = 0;
    if (growth == largest || !installed_root || !media_root) return -EINVAL;
    struct package_sizes installed = {0}, incoming = {0};
    int result = read_sizes(installed_root, &installed);
    if (!result) result = read_sizes(media_root, &incoming);
    uint64_t needed = 0, peak = 0;
    for (size_t i = 0, old = 0; !result && i < incoming.count; ++i) {
        const struct package_size *package = &incoming.items[i];
        while (old < installed.count && strcmp(installed.items[old].name, package->name) < 0) ++old;
        uint64_t previous = old < installed.count && !strcmp(installed.items[old].name, package->name)
            ? installed.items[old].bytes : 0;
        uint64_t added = package->bytes > previous ? package->bytes - previous : 0;
        if (UINT64_MAX - needed < added) result = -EOVERFLOW;
        else needed += added;
        if (peak < package->bytes) peak = package->bytes;
    }
    free(installed.items);
    free(incoming.items);
    if (!result) { *growth = needed; *largest = peak; }
    return result;
}

int installer_update_decode(struct installer_update_info *info, unsigned *progress,
                            char *message, size_t capacity, const char *line)
{
    if (!info || !progress || !message || !capacity || !line) return -EINVAL;
    int activity = installer_update_activity_decode(&info->activity, line);
    if (activity) return activity < 0 ? activity : 4;
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
        FIELD(boot_file),
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
        memset(&info->activity, 0, sizeof(info->activity));
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
