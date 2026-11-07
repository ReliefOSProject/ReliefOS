/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "tools/host/modules/stage.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int walk(int root, int current, const char *prefix, struct kernel_stage_inventory *inventory)
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
        if (!kernel_tools_canonical_member(name) || fstatat(fd, entry->d_name, &status, AT_SYMLINK_NOFOLLOW)) {
            free(name); result = -1; errno = EINVAL; break;
        }
        if (S_ISDIR(status.st_mode)) {
            int child = openat(fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            result = child < 0 ? -1 : walk(root, child, name, inventory);
            if (child >= 0) close(child);
            if (!result && fsync(fd)) result = -1;
            free(name);
        } else if (S_ISREG(status.st_mode)) {
            if (inventory->count == SIZE_MAX / sizeof(*inventory->files)) { free(name); result = -1; break; }
            struct kernel_stage_file *grown = realloc(inventory->files, (inventory->count + 1) * sizeof(*grown));
            if (!grown) { free(name); result = -1; break; }
            inventory->files = grown;
            struct kernel_stage_file *file = &grown[inventory->count++];
            *file = (struct kernel_stage_file){.name = name, .size = status.st_size};
            int product = kernel_bundle_open_member(root, name);
            result = product < 0 ? -1 : kernel_tools_sha256_fd(product, file->sha256);
            if (product >= 0) close(product);
        } else { free(name); errno = EINVAL; result = -1; }
        if (result) break;
        errno = 0;
    }
    if (!entry && errno) result = -1;
    if (!result && fsync(fd)) result = -1;
    int saved = errno; closedir(directory); errno = saved;
    return result;
}

static int compare_files(const void *a, const void *b)
{
    const struct kernel_stage_file *first = a, *second = b;
    return strcmp(first->name, second->name);
}

int kernel_stage_inventory(int root_fd, struct kernel_stage_inventory *inventory)
{
    int result = walk(root_fd, root_fd, "", inventory);
    if (!result && inventory->count) qsort(inventory->files, inventory->count, sizeof(*inventory->files), compare_files);
    return result;
}

void kernel_stage_inventory_free(struct kernel_stage_inventory *inventory)
{
    for (size_t i = 0; i < inventory->count; ++i) free(inventory->files[i].name);
    free(inventory->files);
    *inventory = (struct kernel_stage_inventory){0};
}
