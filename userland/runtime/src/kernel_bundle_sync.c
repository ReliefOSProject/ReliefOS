/* SPDX-License-Identifier: Apache-2.0 */
#include "kernel_bundle_internal.h"
#include "bootloader_internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct sync_directory { char *name, *child; };
struct sync_directories { struct sync_directory *entries; size_t count; };

/** @brief Record each ancestor once, retaining an existing child path for safe opening.
 * @param directories Mutable owned directory inventory.
 * @param member Borrowed checked product path.
 * @return Zero on success, negative ENOMEM/EOVERFLOW on allocation failure.
 */
static int ancestors(struct sync_directories *directories, const char *member)
{
    char *child = strdup(member);
    if (!child) return -ENOMEM;
    int result = 0;
    for (;;) {
        char *parent = strdup(child);
        if (!parent) { result = -ENOMEM; break; }
        char *slash = strrchr(parent, '/');
        if (!slash) { free(parent); break; }
        *slash = 0;
        size_t i;
        for (i = 0; i < directories->count; ++i) if (!strcmp(directories->entries[i].name, parent)) break;
        if (i == directories->count) {
            if (i == SIZE_MAX / sizeof(*directories->entries)) { free(parent); result = -EOVERFLOW; break; }
            struct sync_directory *grown = realloc(directories->entries, (i + 1) * sizeof(*grown));
            if (!grown) { free(parent); result = -ENOMEM; break; }
            directories->entries = grown;
            grown[i] = (struct sync_directory){.name = strdup(parent), .child = strdup(child)};
            ++directories->count;
            if (!grown[i].name || !grown[i].child) { free(parent); result = -ENOMEM; break; }
        }
        free(child); child = parent;
    }
    free(child);
    return result;
}

/** @brief Order nested directory syncs before their parents.
 * @param a Borrowed directory inventory member.
 * @param b Borrowed directory inventory member.
 * @return Descending path-length order, with lexical ties for reproducibility.
 */
static int directory_order(const void *a, const void *b)
{
    const struct sync_directory *first = a, *second = b;
    size_t x = strlen(first->name), y = strlen(second->name);
    return x > y ? -1 : x < y ? 1 : strcmp(first->name, second->name);
}

int kernel_bundle_sync_root(const struct kernel_bundle_checked *bundle)
{
    if (!bundle || bundle->root < 0) return -EINVAL;
    struct sync_directories directories = {0};
    char sums[128]; snprintf(sums, sizeof(sums), "%sSHA256SUMS", bundle->prefix);
    int result = 0;
    for (size_t i = 0; !result && i <= bundle->count + 1; ++i) {
        const char *name = i == bundle->count + 1 ? "usr/lib/reliefos/kernel/current-release" :
                           i == bundle->count ? sums : bundle->products[i].name;
        int fd = boot_open_file(bundle->root, name);
        /* A retained per-release copy deliberately omits the APK's mutable
         * pointer. A real installed package's pointer is flushed before ESP. */
        if (i == bundle->count + 1 && fd == -ENOENT) continue;
        if (fd < 0) { result = fd; break; }
        result = fsync(fd) ? -errno : 0;
        if (!result && i < bundle->count) {
            char digest[65]; result = kernel_bundle_hash_fd(fd, digest);
            if (!result && strcmp(digest, bundle->products[i].hash)) result = -EIO;
        }
        close(fd);
        if (!result) result = ancestors(&directories, name);
    }
    if (!result && directories.count) qsort(directories.entries, directories.count, sizeof(*directories.entries), directory_order);
    for (size_t i = 0; !result && i < directories.count; ++i) {
        char leaf[256]; int fd = boot_open_parent(bundle->root, directories.entries[i].child, 0, leaf);
        if (fd < 0) { result = fd; break; }
        result = fsync(fd) ? -errno : 0; close(fd);
    }
    if (!result && fsync(bundle->root)) result = -errno;
    for (size_t i = 0; i < directories.count; ++i) { free(directories.entries[i].name); free(directories.entries[i].child); }
    free(directories.entries);
    return result;
}
