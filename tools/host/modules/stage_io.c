/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "tools/host/modules/stage.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int write_all(int fd, const void *data, size_t size)
{
    const unsigned char *bytes = data;
    while (size) {
        ssize_t count = write(fd, bytes, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { if (!count) errno = EIO; return -1; }
        bytes += count; size -= (size_t)count;
    }
    return 0;
}

bool kernel_stage_tree_member(const char *name)
{
    if (kernel_tools_canonical_member(name)) return true;
    const char *base = name ? strrchr(name, '/') : NULL;
    base = base ? base + 1 : name;
    if (!base || (strcmp(base, "[") && strcmp(base, "[["))) return false;
    if (base == name) return true;
    char *parent = strndup(name, (size_t)(base - name - 1));
    bool valid = parent && kernel_tools_canonical_member(parent);
    free(parent);
    return valid;
}

/* Return an owned directory, allowing spaces in host paths while rejecting
 * symlinks and traversal. Product paths use the stricter canonical validator. */
static int directories(int root, const char *path, mode_t mode)
{
    char *owned = strdup(path);
    if (!owned) return -1;
    int current = fcntl(root, F_DUPFD_CLOEXEC, 3);
    if (current < 0) { free(owned); return -1; }
    char *at = owned;
    while (*at) {
        char *slash = strchr(at, '/');
        if (slash) *slash = 0;
        if (!*at || !strcmp(at, ".") || !strcmp(at, "..")) { errno = EINVAL; goto error; }
        if (mkdirat(current, at, mode) && errno != EEXIST) goto error;
        int next = openat(current, at, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) goto error;
        close(current); current = next;
        if (!slash) break;
        at = slash + 1;
    }
    free(owned);
    return current;
error: {
    int saved = errno; close(current); free(owned); errno = saved; return -1;
}
}

int kernel_stage_mkdir(int root_fd, const char *name, mode_t mode)
{
    if (!kernel_stage_tree_member(name)) { errno = EINVAL; return -1; }
    int fd = directories(root_fd, name, mode);
    if (fd < 0) return -1;
    int result = fchmod(fd, mode);
    int saved = errno; close(fd); errno = saved;
    return result;
}

static int target_parent(int root_fd, const char *name, char **leaf)
{
    if (!kernel_stage_tree_member(name)) { errno = EINVAL; return -1; }
    char *copy = strdup(name);
    if (!copy) return -1;
    char *slash = strrchr(copy, '/');
    int fd;
    if (slash) {
        *slash = 0; *leaf = strdup(slash + 1);
        fd = *leaf ? directories(root_fd, copy, 0755) : -1;
    } else {
        *leaf = strdup(copy);
        fd = *leaf ? fcntl(root_fd, F_DUPFD_CLOEXEC, 3) : -1;
    }
    free(copy);
    if (fd < 0) { free(*leaf); *leaf = NULL; }
    return fd;
}

static int output_file(int root_fd, const char *name, mode_t mode, const struct stat *source)
{
    char *leaf = NULL;
    int parent = target_parent(root_fd, name, &leaf);
    if (parent < 0) return -1;
    struct stat status;
    int result = fstatat(parent, leaf, &status, AT_SYMLINK_NOFOLLOW);
    if ((!result && !S_ISREG(status.st_mode)) || (result && errno != ENOENT)) {
        close(parent); free(leaf); errno = EINVAL; return -1;
    }
    int fd = openat(parent, leaf, O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, mode);
    int saved = errno; close(parent); free(leaf); errno = saved;
    if (fd < 0) return -1;
    if (fstat(fd, &status) || !S_ISREG(status.st_mode) ||
        (source && status.st_dev == source->st_dev && status.st_ino == source->st_ino) ||
        fchmod(fd, mode) || ftruncate(fd, 0)) {
        saved = errno; close(fd); errno = saved ? saved : EINVAL; return -1;
    }
    return fd;
}

int kernel_stage_symlink(int root_fd, const char *name, const char *target)
{
    char *leaf = NULL;
    int parent = target_parent(root_fd, name, &leaf);
    if (parent < 0) return -1;
    int result = symlinkat(target, parent, leaf);
    int saved = errno; close(parent); free(leaf); errno = saved;
    return result;
}

int kernel_stage_write(int root_fd, const char *name, const void *data, size_t size, mode_t mode)
{
    int fd = output_file(root_fd, name, mode, NULL);
    if (fd < 0) return -1;
    int result = write_all(fd, data, size) || fsync(fd);
    int saved = errno;
    if (close(fd)) result = -1;
    errno = saved;
    return result ? -1 : 0;
}

int kernel_stage_input(const char *path)
{
    struct stat status;
    if (lstat(path, &status)) return -1;
    if (!S_ISREG(status.st_mode)) { errno = EINVAL; return -1; }
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    if (fstat(fd, &status) || !S_ISREG(status.st_mode)) { close(fd); errno = EINVAL; return -1; }
    return fd;
}

int kernel_stage_copy_fd(int input, int target_root, const char *target, const char *sha256, mode_t mode)
{
    if (!kernel_tools_digest_text(sha256)) { errno = EINVAL; return -1; }
    struct stat source;
    if (fstat(input, &source) || !S_ISREG(source.st_mode)) { errno = EINVAL; return -1; }
    int output = output_file(target_root, target, mode, &source);
    if (output < 0) return -1;
    unsigned char buffer[65536]; int result = 0;
    for (;;) {
        ssize_t count = read(input, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { result = -1; break; }
        if (!count) break;
        if (write_all(output, buffer, (size_t)count)) { result = -1; break; }
    }
    if (!result && fsync(output)) result = -1;
    int saved = errno;
    if (close(output)) result = -1;
    if (!result) {
        int copied = kernel_bundle_open_member(target_root, target);
        char digest[65];
        result = copied < 0 || kernel_tools_sha256_fd(copied, digest) || strcmp(digest, sha256);
        if (copied >= 0) close(copied);
        if (result) saved = EIO;
    }
    errno = saved;
    return result ? -1 : 0;
}

int kernel_stage_copy(int source_root, const char *source, int target_root,
                      const char *target, const char *sha256, mode_t mode)
{
    int input = kernel_bundle_open_member(source_root, source);
    if (input < 0) return -1;
    int result = kernel_stage_copy_fd(input, target_root, target, sha256, mode);
    int saved = errno; close(input); errno = saved;
    return result;
}

int kernel_stage_run(char *const argv[], struct byte_buffer *capture)
{
    int channels[2] = {-1, -1};
    if (capture && pipe(channels)) return -1;
    /* Preserve valid behavior even if the caller closed a standard stream. */
    for (size_t i = 0; i < 2; ++i) if (channels[i] >= 0 && channels[i] < 3) {
        int next = fcntl(channels[i], F_DUPFD_CLOEXEC, 3);
        close(channels[i]); channels[i] = next;
        if (next < 0) { if (channels[1 - i] >= 0) close(channels[1 - i]); return -1; }
    }
    pid_t child = fork();
    if (child < 0) { if (capture) { close(channels[0]); close(channels[1]); } return -1; }
    if (!child) {
        if (capture) {
            if (dup2(channels[1], STDOUT_FILENO) < 0) _exit(126);
            close(channels[0]); close(channels[1]);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    int result = 0;
    if (capture) {
        close(channels[1]);
        unsigned char bytes[4096];
        for (;;) {
            ssize_t count = read(channels[0], bytes, sizeof(bytes));
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) { result = -1; break; }
            if (!count) break;
            if (capture->len + (size_t)count > 16 * 1024 * 1024 ||
                buffer_reserve(capture, capture->len + (size_t)count + 1)) { result = -1; break; }
            memcpy(capture->data + capture->len, bytes, (size_t)count); capture->len += (size_t)count;
        }
        close(channels[0]);
        if (!result) {
            if (buffer_reserve(capture, capture->len + 1)) result = -1;
            else capture->data[capture->len] = 0;
        }
    }
    int status; pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status)) { errno = EIO; result = -1; }
    return result;
}

/* Only callers holding ownership of this directory may recursively remove it.
 * Symlinks are unlinked as entries; their targets are never followed. */
static int remove_tree(int parent, const char *name)
{
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    DIR *directory = fdopendir(fd);
    if (!directory) { close(fd); return -1; }
    struct dirent *entry; int result = 0;
    errno = 0;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        struct stat status;
        if (fstatat(fd, entry->d_name, &status, AT_SYMLINK_NOFOLLOW)) { result = -1; break; }
        if (S_ISDIR(status.st_mode)) result = remove_tree(fd, entry->d_name);
        else result = unlinkat(fd, entry->d_name, 0);
        if (result) break;
        errno = 0;
    }
    if (!entry && errno) result = -1;
    int saved = errno; closedir(directory); errno = saved;
    if (!result) result = unlinkat(parent, name, AT_REMOVEDIR);
    return result;
}

static int owned_directory(int parent, const char *name, const char *marker)
{
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    int member = kernel_bundle_open_member(fd, marker);
    close(fd);
    if (member < 0) return -1;
    close(member);
    return 1;
}

int kernel_stage_begin(struct kernel_stage *stage, const char *destination)
{
    *stage = (struct kernel_stage){.root_fd = -1, .parent_fd = -1};
    if (!destination || *destination != '/' || !destination[1] || destination[strlen(destination) - 1] == '/') {
        errno = EINVAL; return -1;
    }
    stage->destination = strdup(destination);
    char *parent = strdup(destination);
    if (!stage->destination || !parent) { free(parent); return -1; }
    char *slash = strrchr(parent, '/'); *slash = 0;
    stage->leaf = strdup(slash + 1);
    int root = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root >= 0) {
        stage->parent_fd = *parent ? directories(root, parent + 1, 0755) : fcntl(root, F_DUPFD_CLOEXEC, 3);
        close(root);
    }
    free(parent);
    if (stage->parent_fd < 0 || !stage->leaf || !strcmp(stage->leaf, ".") || !strcmp(stage->leaf, "..")) return -1;
    size_t length = strlen(destination);
    stage->path = malloc(length + sizeof(".new.XXXXXX"));
    if (!stage->path) return -1;
    snprintf(stage->path, length + sizeof(".new.XXXXXX"), "%s.new.XXXXXX", destination);
    if (!mkdtemp(stage->path)) return -1;
    stage->temporary_leaf = strdup(strrchr(stage->path, '/') + 1);
    if (!stage->temporary_leaf) { rmdir(stage->path); return -1; }
    stage->root_fd = openat(stage->parent_fd, stage->temporary_leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (stage->root_fd < 0 || fchmod(stage->root_fd, 0755)) return -1;
    return 0;
}

int kernel_stage_publish(struct kernel_stage *stage, const char *marker)
{
    char *previous = malloc(strlen(stage->leaf) + sizeof(".previous"));
    if (!previous) return -1;
    sprintf(previous, "%s.previous", stage->leaf);
    int current_owned = owned_directory(stage->parent_fd, stage->leaf, marker);
    int previous_owned = owned_directory(stage->parent_fd, previous, marker);
    int result = -1;
    if (current_owned < 0 || previous_owned < 0) { errno = EPERM; goto out; }
    if (previous_owned && !current_owned) {
        if (renameat(stage->parent_fd, previous, stage->parent_fd, stage->leaf) || fsync(stage->parent_fd)) goto out;
        current_owned = 1; previous_owned = 0;
    }
    if (previous_owned && remove_tree(stage->parent_fd, previous)) goto out;
    if (fsync(stage->root_fd)) goto out;
    if (current_owned && renameat(stage->parent_fd, stage->leaf, stage->parent_fd, previous)) goto out;
    if (renameat(stage->parent_fd, stage->temporary_leaf, stage->parent_fd, stage->leaf)) {
        if (current_owned) (void)renameat(stage->parent_fd, previous, stage->parent_fd, stage->leaf);
        goto out;
    }
    free(stage->temporary_leaf); stage->temporary_leaf = NULL;
    if (fsync(stage->parent_fd)) goto out;
    if (current_owned && remove_tree(stage->parent_fd, previous)) goto out;
    result = fsync(stage->parent_fd);
out:
    free(previous);
    return result;
}

void kernel_stage_end(struct kernel_stage *stage)
{
    if (stage->root_fd >= 0) close(stage->root_fd);
    if (stage->temporary_leaf && stage->parent_fd >= 0) (void)remove_tree(stage->parent_fd, stage->temporary_leaf);
    if (stage->parent_fd >= 0) close(stage->parent_fd);
    free(stage->path); free(stage->destination); free(stage->leaf); free(stage->temporary_leaf);
    *stage = (struct kernel_stage){.root_fd = -1, .parent_fd = -1};
}
