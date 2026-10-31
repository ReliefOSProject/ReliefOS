#include "filesystem.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int remove_at(int parent, const char *name, unsigned depth)
{
    struct stat status;
    if (depth > 64) return -ELOOP;
    if (fstatat(parent, name, &status, AT_SYMLINK_NOFOLLOW) < 0) return -errno;
    if (!S_ISDIR(status.st_mode)) return unlinkat(parent, name, 0) < 0 ? -errno : 0;
    int descriptor = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) return -errno;
    DIR *directory = fdopendir(descriptor);
    if (!directory) { int error = -errno; close(descriptor); return error; }
    int result = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) { result = errno ? -errno : 0; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        result = remove_at(descriptor, entry->d_name, depth + 1);
        if (result < 0) break;
    }
    closedir(directory);
    if (result < 0) return result;
    return unlinkat(parent, name, AT_REMOVEDIR) < 0 ? -errno : 0;
}

int fileman_remove_tree(const char *path)
{
    if (!path || !*path) return -EINVAL;
    char *trimmed = strdup(path);
    if (!trimmed) return -ENOMEM;
    size_t length = strlen(trimmed);
    while (length && trimmed[length - 1] == '/') trimmed[--length] = 0;
    const char *base = strrchr(trimmed, '/');
    base = base ? base + 1 : trimmed;
    int result = !*base || !strcmp(base, ".") || !strcmp(base, "..") ? -EINVAL :
        remove_at(AT_FDCWD, trimmed, 0);
    free(trimmed);
    return result;
}
