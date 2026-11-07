/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "tools/host/modules/stage.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct archive_entry { char *name; mode_t mode; off_t size; };
struct archive_tree { struct archive_entry *entries; size_t count; };

static int list_tree(int current, const char *prefix, struct archive_tree *tree)
{
    int fd = openat(current, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    DIR *directory = fdopendir(fd);
    if (!directory) { close(fd); return -1; }
    struct dirent *entry; int result = 0;
    errno = 0;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        size_t length = strlen(prefix) + strlen(entry->d_name) + 2;
        char *name = malloc(length);
        if (!name) { result = -1; break; }
        snprintf(name, length, "%s%s%s", prefix, *prefix ? "/" : "", entry->d_name);
        struct stat status;
        if (!kernel_stage_tree_member(name) || fstatat(fd, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) ||
            !(S_ISDIR(status.st_mode) || S_ISREG(status.st_mode) || S_ISLNK(status.st_mode))) {
            free(name); errno = EINVAL; result = -1; break;
        }
        if (tree->count == SIZE_MAX / sizeof(*tree->entries)) { free(name); result = -1; break; }
        struct archive_entry *grown = realloc(tree->entries, (tree->count + 1) * sizeof(*grown));
        if (!grown) { free(name); result = -1; break; }
        tree->entries = grown;
        grown[tree->count++] = (struct archive_entry){.name = name, .mode = status.st_mode, .size = status.st_size};
        if (S_ISDIR(status.st_mode)) {
            int child = openat(fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            result = child < 0 ? -1 : list_tree(child, name, tree);
            if (child >= 0) close(child);
        }
        if (result) break;
        errno = 0;
    }
    if (!entry && errno) result = -1;
    int saved = errno; closedir(directory); errno = saved;
    return result;
}

static int compare_entries(const void *a, const void *b)
{
    const struct archive_entry *first = a, *second = b;
    return strcmp(first->name, second->name);
}

static int pad(FILE *stream, unsigned alignment)
{
    off_t position = ftello(stream);
    if (position < 0) return -1;
    unsigned count = (unsigned)(-(uint64_t)position % alignment);
    while (count--) if (fputc(0, stream) == EOF) return -1;
    return 0;
}

static int record(FILE *stream, unsigned inode, const char *name, unsigned mode,
                  unsigned epoch, unsigned size, unsigned major, unsigned minor,
                  const void *payload, int source)
{
    size_t name_size = strlen(name) + 1;
    if (name_size > UINT32_MAX) { errno = EOVERFLOW; return -1; }
    if (fprintf(stream, "070701%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x",
                inode, mode, 0u, 0u, 1u, epoch, size, 0u, 0u, major, minor, (unsigned)name_size, 0u) != 110 ||
        fwrite(name, 1, name_size, stream) != name_size || pad(stream, 4)) return -1;
    if (source >= 0) {
        unsigned char buffer[65536]; unsigned left = size;
        while (left) {
            ssize_t count = read(source, buffer, left < sizeof(buffer) ? left : sizeof(buffer));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0 || fwrite(buffer, 1, (size_t)count, stream) != (size_t)count) { errno = EIO; return -1; }
            left -= (unsigned)count;
        }
        unsigned char extra; ssize_t count;
        do { count = read(source, &extra, 1); } while (count < 0 && errno == EINTR);
        if (count) { errno = EIO; return -1; }
    } else if (size && fwrite(payload, 1, size, stream) != size) return -1;
    return pad(stream, 4);
}

int kernel_stage_newc(int tree_fd, int output_fd, unsigned epoch)
{
    struct archive_tree tree = {0};
    int result = list_tree(tree_fd, "", &tree);
    if (tree.count > UINT32_MAX - 4) result = -1;
    if (!result && tree.count) qsort(tree.entries, tree.count, sizeof(*tree.entries), compare_entries);
    int duplicate = result ? -1 : fcntl(output_fd, F_DUPFD_CLOEXEC, 3);
    FILE *stream = duplicate < 0 ? NULL : fdopen(duplicate, "wb");
    if (!stream) { if (duplicate >= 0) close(duplicate); result = -1; }
    unsigned inode = 0;
    if (!result) result = record(stream, ++inode, ".", S_IFDIR | 0755, epoch, 0, 0, 0, NULL, -1);
    for (size_t i = 0; !result && i < tree.count; ++i) {
        struct archive_entry *entry = &tree.entries[i];
        if (S_ISDIR(entry->mode)) {
            result = record(stream, ++inode, entry->name, S_IFDIR | (!strcmp(entry->name, "tmp") ? 01777 : 0755),
                            epoch, 0, 0, 0, NULL, -1);
        } else if (S_ISLNK(entry->mode)) {
            if (entry->size < 0 || (uint64_t)entry->size >= UINT32_MAX) { errno = EOVERFLOW; result = -1; break; }
            size_t capacity = (size_t)entry->size + 1;
            char *target = malloc(capacity);
            if (!target) { result = -1; break; }
            ssize_t count = readlinkat(tree_fd, entry->name, target, capacity);
            if (count < 0 || (size_t)count >= capacity) result = -1;
            else result = record(stream, ++inode, entry->name, S_IFLNK | 0777, epoch, (unsigned)count, 0, 0, target, -1);
            free(target);
        } else {
            int source = kernel_bundle_open_member(tree_fd, entry->name);
            if (source < 0 || entry->size < 0 || (uint64_t)entry->size > UINT32_MAX) {
                if (source >= 0) close(source);
                errno = EOVERFLOW; result = -1; break;
            }
            /* The stage assigns explicit modes. Preserve ordinary rwx bits
             * for every independently built executable, not a basename list;
             * privilege bits are never imported into the early archive. */
            unsigned mode = (unsigned)entry->mode & 0777u;
            result = record(stream, ++inode, entry->name, S_IFREG | mode, epoch, (unsigned)entry->size, 0, 0, NULL, source);
            close(source);
        }
    }
    if (!result) result = record(stream, ++inode, "dev/console", S_IFCHR | 0600, epoch, 0, 5, 1, NULL, -1);
    if (!result) result = record(stream, ++inode, "dev/null", S_IFCHR | 0666, epoch, 0, 1, 3, NULL, -1);
    if (!result) result = record(stream, ++inode, "TRAILER!!!", 0, epoch, 0, 0, 0, NULL, -1);
    if (!result) result = pad(stream, 512);
    if (stream && fclose(stream)) result = -1;
    for (size_t i = 0; i < tree.count; ++i) free(tree.entries[i].name);
    free(tree.entries);
    return result;
}
