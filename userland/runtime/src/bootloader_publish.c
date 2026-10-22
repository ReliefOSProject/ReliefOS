#include "bootloader_internal.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/** @brief Reject empty, traversal, overlong and absolute relative path components. */
static int relative_valid(const char *path)
{
    if (!path || !*path || *path == '/' || strlen(path) >= BOOT_PATH_MAX) return 0;
    const char *component = path;
    for (const char *p = path;; ++p) {
        if (*p && *p != '/') continue;
        size_t length = (size_t)(p - component);
        if (!length || length > 255 || (length == 1 && *component == '.') ||
            (length == 2 && component[0] == '.' && component[1] == '.')) return 0;
        if (!*p) return 1;
        component = p + 1;
    }
}

/** @brief Bound path construction; no path text is evaluated as shell syntax. */
int boot_join(char out[BOOT_PATH_MAX], const char *base, const char *relative)
{
    if (!out || !base || base[0] != '/' || !relative_valid(relative)) return -EINVAL;
    int n = snprintf(out, BOOT_PATH_MAX, "%s/%s", base, relative);
    return n < 0 || n >= BOOT_PATH_MAX ? -ENAMETOOLONG : 0;
}

/**
 * @brief Walk a directory relative to a held descriptor, optionally creating it.
 * Returns a caller-owned descriptor or negative errno. No symlinks are followed.
 */
static int walk_directory(int root, const char *relative, int create)
{
    char copy[BOOT_PATH_MAX];
    if (!relative_valid(relative)) return -EINVAL;
    strcpy(copy, relative);
    int fd = fcntl(root, F_DUPFD_CLOEXEC, 3);
    if (fd < 0) return -errno;
    char *state, *part = strtok_r(copy, "/", &state);
    while (part) {
        if (create && mkdirat(fd, part, 0755) < 0 && errno != EEXIST) {
            int error = -errno; close(fd); return error;
        }
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int error = errno;
        close(fd);
        if (next < 0) return -error;
        fd = next;
        part = strtok_r(NULL, "/", &state);
    }
    return fd;
}

/** @brief Obtain a caller-owned absolute directory descriptor with safe ancestors. */
int boot_open_directory(const char *path)
{
    if (!path || path[0] != '/' || !path[1]) return -EINVAL;
    char normalized[BOOT_PATH_MAX];
    if (strlen(path) >= sizeof(normalized)) return -ENAMETOOLONG;
    strcpy(normalized, path);
    size_t length = strlen(normalized);
    while (length > 1 && normalized[length - 1] == '/') normalized[--length] = 0;
    int root = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root < 0) return -errno;
    int result = walk_directory(root, normalized + 1, 0);
    close(root);
    return result;
}

/** @brief Return the held safe parent and bounded leaf for a relative destination. */
static int open_parent(int root, const char *relative, int create, char leaf[256])
{
    if (!relative_valid(relative)) return -EINVAL;
    char path[BOOT_PATH_MAX];
    strcpy(path, relative);
    char *slash = strrchr(path, '/');
    if (!slash) {
        strcpy(leaf, path);
        int fd = fcntl(root, F_DUPFD_CLOEXEC, 3);
        return fd < 0 ? -errno : fd;
    }
    *slash = 0;
    strcpy(leaf, slash + 1);
    return walk_directory(root, path, create);
}

/** @brief Return a caller-owned regular-file descriptor without following links. */
int boot_open_file(int root, const char *relative)
{
    char leaf[256];
    int parent = open_parent(root, relative, 0, leaf);
    if (parent < 0) return parent;
    int fd = openat(parent, leaf, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    int error = errno;
    close(parent);
    if (fd < 0) return -error;
    struct stat st;
    if (fstat(fd, &st) < 0) error = errno;
    else if (!S_ISREG(st.st_mode)) error = EINVAL;
    else return fd;
    close(fd);
    return -error;
}

/** @brief Validate one existing destination without creating paths or following links. */
static int check_destination(int root, const char *relative, int directory,
                              const struct stat *source)
{
    char leaf[256];
    int parent = open_parent(root, relative, 0, leaf);
    if (parent == -ENOENT) return 0;
    if (parent < 0) return parent;
    struct stat target;
    int result = 0;
    if (fstatat(parent, leaf, &target, AT_SYMLINK_NOFOLLOW) < 0) {
        if (errno != ENOENT) result = -errno;
    } else if (directory ? !S_ISDIR(target.st_mode) : !S_ISREG(target.st_mode)) result = -ELOOP;
    else if (source && source->st_dev == target.st_dev && source->st_ino == target.st_ino) result = -EINVAL;
    close(parent);
    return result;
}

/** @brief Write a complete buffer, retrying interrupted and short POSIX transfers. */
static int write_complete(int fd, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    while (length) {
        ssize_t done = write(fd, bytes, length);
        if (done < 0 && errno == EINTR) continue;
        if (done <= 0) return done ? -errno : -EIO;
        bytes += done; length -= (size_t)done;
    }
    return 0;
}

/** @brief Read up to length bytes, distinguishing EOF, interruptions and errors. */
static ssize_t read_complete(int fd, void *data, size_t length)
{
    size_t total = 0;
    while (total < length) {
        ssize_t done = read(fd, (unsigned char *)data + total, length - total);
        if (done < 0 && errno == EINTR) continue;
        if (done < 0) return -errno;
        if (!done) break;
        total += (size_t)done;
    }
    return (ssize_t)total;
}

/**
 * @brief Publish from a borrowed source descriptor or memory into a safe parent.
 * Sync and byte-verify the private file before rename; caller owns all inputs.
 * Disk/firmware updates as a whole cannot be made power-failure atomic.
 */
static int publish(const struct reliefos_boot_install_options *options, int root,
                    const char *relative, int source, const char *data, size_t length)
{
    char leaf[256], temporary[64];
    int parent = open_parent(root, relative, 1, leaf);
    if (parent < 0) return parent;
    struct stat existing;
    if (!fstatat(parent, leaf, &existing, AT_SYMLINK_NOFOLLOW) && !S_ISREG(existing.st_mode)) {
        close(parent); return -ELOOP;
    }
    int fd = -1;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        snprintf(temporary, sizeof(temporary), ".reliefos-boot-%ld-%u", (long)getpid(), attempt);
        fd = openat(parent, temporary, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) { int error = -errno; close(parent); return error; }
    int result = 0;
    if (source >= 0) {
        unsigned char buffer[16384];
        for (;;) {
            ssize_t count = read_complete(source, buffer, sizeof(buffer));
            if (count < 0) { result = (int)count; break; }
            if (!count) break;
            result = write_complete(fd, buffer, (size_t)count);
            if (result) break;
            if (options->progress) options->progress(options->progress_context);
        }
        if (!result && (lseek(source, 0, SEEK_SET) < 0 || lseek(fd, 0, SEEK_SET) < 0)) result = -errno;
        while (!result) {
            unsigned char verify[16384];
            ssize_t count = read_complete(source, buffer, sizeof(buffer));
            if (count < 0) { result = (int)count; break; }
            ssize_t copied = read_complete(fd, verify, sizeof(verify));
            if (copied < 0) { result = (int)copied; break; }
            if (count != copied || (count && memcmp(buffer, verify, (size_t)count))) result = -EIO;
            if (!count) break;
            if (options->progress) options->progress(options->progress_context);
        }
    } else result = write_complete(fd, data, length);
    if (!result && fchmod(fd, 0644) < 0) result = -errno;
    if (!result && fsync(fd) < 0) result = -errno;
    if (close(fd) < 0 && !result) result = -errno;
    if (!result && renameat(parent, temporary, parent, leaf) < 0) result = -errno;
    if (!result && fsync(parent) < 0) result = -errno;
    if (result) (void)unlinkat(parent, temporary, 0);
    close(parent);
    return result;
}

/** @brief Publish one regular media file, preserving an existing rollback when requested. */
int boot_publish_file(const struct reliefos_boot_install_options *options, int root,
                      const char *relative, int only_missing)
{
    if (only_missing) {
        char leaf[256];
        int parent = open_parent(root, relative, 0, leaf);
        if (parent >= 0) {
            struct stat st;
            int found = fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW);
            int error = errno;
            close(parent);
            if (!found) return S_ISREG(st.st_mode) ? 0 : -ELOOP;
            if (error != ENOENT) return -error;
        } else if (parent != -ENOENT) return parent;
    }
    char path[BOOT_PATH_MAX];
    int result = boot_join(path, options->source, relative);
    if (result) return result;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -errno;
    struct stat st;
    if (fstat(fd, &st) < 0) result = -errno;
    else if (!S_ISREG(st.st_mode)) result = -EINVAL;
    else result = publish(options, root, relative, fd, NULL, 0);
    if (close(fd) < 0 && !result) result = -errno;
    return result;
}

/** @brief Publish a small validated mode record through the same atomic writer. */
int boot_publish_record(const struct reliefos_boot_install_options *options, int root,
                        const char *relative, const char *data, size_t length)
{
    return publish(options, root, relative, -1, data, length);
}

/** @brief Retire old loader files without following their names outside the boot tree. */
int boot_retire_legacy(int root)
{
    const char *const files[] = {"loader.elf", "reliefos/loader.elf", "leonos/loader.elf"};
    for (unsigned i = 0; i < 3; ++i) {
        char leaf[256];
        int parent = open_parent(root, files[i], 0, leaf);
        if (parent == -ENOENT) continue;
        if (parent < 0) return parent;
        int result = unlinkat(parent, leaf, 0) < 0 && errno != ENOENT ? -errno : 0;
        if (!result && fsync(parent) < 0) result = -errno;
        close(parent);
        if (result) return result;
    }
    return 0;
}

/** @brief Traverse only selected media subtrees with bounded depth and no links. */
static int walk(const struct reliefos_boot_install_options *options, int root,
                 const char *relative, unsigned depth, int publish_files)
{
    if (depth > 16) return -ELOOP;
    if (!strcmp(relative, "reliefos/config/bootloaders.conf") ||
        !strcmp(relative, "reliefos/config/root.cfg")) return 0;
    if (!strcmp(relative, "grub/i386-pc") && !options->install_bios) return 0;
    char path[BOOT_PATH_MAX];
    int result = boot_join(path, options->source, relative);
    if (result) return result;
    struct stat st;
    if (lstat(path, &st) < 0) return -errno;
    if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) return -ELOOP;
    if (root >= 0 && !publish_files) {
        result = check_destination(root, relative, S_ISDIR(st.st_mode), &st);
        if (result) return result;
    }
    if (S_ISREG(st.st_mode)) {
        if (!publish_files || !strcmp(relative, "grub/grub.cfg")) return 0;
        return boot_publish_file(options, root, relative,
            options->preserve_rollback && !strcmp(relative, "leonos/kernel.sys"));
    }
    /* Upstream grub-install owns the selected BIOS platform publication. */
    if (publish_files && !strcmp(relative, "grub/i386-pc")) return 0;
    DIR *directory = opendir(path);
    if (!directory) return -errno;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) { if (errno) result = -errno; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char child[BOOT_PATH_MAX];
        int n = snprintf(child, sizeof(child), "%s/%s", relative, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) { result = -ENAMETOOLONG; break; }
        result = walk(options, root, child, depth + 1, publish_files);
        if (result) break;
    }
    closedir(directory);
    return result;
}

/** @brief Use one subtree policy for source preflight, target preflight and publication. */
int boot_walk_payload(const struct reliefos_boot_install_options *options, int root, int publish_files)
{
    const char *const trees[] = {"reliefos", "leonos", "grub", "EFI"};
    for (unsigned i = 0; i < 4; ++i) {
        if (i == 3 && !options->install_uefi) continue;
        int result = walk(options, root, trees[i], 0, publish_files);
        if (result) return result;
    }
    return 0;
}
