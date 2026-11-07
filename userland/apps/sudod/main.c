/* Fixed file-operation worker launched by upstream sudo after command authorization.
 * Results travel through stdout's pipe; no privileged result pathname is accepted. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <errno.h>
#include <fcntl.h>
#include <reliefos/sudo.h>
#include <reliefos/auth.h>
#include <libintl.h>
#define T(s) gettext(s)
#include <locale.h>
#include <reliefos/layout.h>
#include <reliefos/fs.h>
#include <reliefos/stdio.h>
#include <reliefos/syscall.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>

#include "paths.h"

#define SUDOD_RESULT_MAGIC 0x524f4453u /* 'SDOR' */

/* Wire layout shared with userland/runtime/src/sudo_client.c. Both sides read the
 * same bytes; keep them in step when either changes. The public ABI headers
 * deliberately do not carry this: it is private to the broker. */
struct sudod_result {
    uint32_t magic;
    int32_t status;
    uint32_t count;
    uint32_t reserved;
    struct reliefos_dir_entry entries[RELIEFOS_FS_MAX_ENTRIES];
};

struct sudod_request {
    uint32_t op;
    char path1[RELIEFOS_FS_PATH_LEN];
    char path2[RELIEFOS_FS_PATH_LEN];
};

static struct sudod_result sudod_output;

/* Pin every parent directory without following symlinks. All mutations below
 * are relative to these descriptors, including recursive removal. */
static int sudod_open_dir(const char *path)
{
    if (!sudo_path_allowed(path)) { errno = EINVAL; return -1; }
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    const char *part = path + 1;
    while (fd >= 0 && *part) {
        const char *end = strchr(part, '/');
        size_t length = end ? (size_t)(end - part) : strlen(part);
        char name[RELIEFOS_FS_PATH_LEN];
        if (!length || length >= sizeof(name)) { close(fd); errno = EINVAL; return -1; }
        memcpy(name, part, length);
        name[length] = 0;
        int next = openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int saved = errno;
        close(fd);
        errno = saved;
        fd = next;
        part += length;
        if (*part == '/') ++part;
    }
    return fd;
}

static int sudod_parent_fd(const char *path, char *name)
{
    char parent[RELIEFOS_FS_PATH_LEN];
    size_t length = strlen(path);
    if (!sudo_path_allowed(path) || length < 2 || length >= sizeof(parent) ||
        path[length - 1] == '/') { errno = EINVAL; return -1; }
    const char *slash = strrchr(path, '/');
    strcpy(name, slash + 1);
    size_t prefix = (size_t)(slash - path);
    memcpy(parent, path, prefix);
    parent[prefix] = 0;
    return sudod_open_dir(prefix ? parent : "/");
}

static int sudod_straight_stat(const char *path, struct stat *st, void *context)
{
    (void)context;
    char name[RELIEFOS_FS_PATH_LEN];
    int fd = sudod_parent_fd(path, name);
    if (fd < 0) return -1;
    int result = fstatat(fd, name, st, AT_SYMLINK_NOFOLLOW);
    int saved = errno;
    close(fd);
    errno = saved;
    return result;
}

/* Bounded copy: an argument may be arbitrarily long, so never measure it with
 * strlen before deciding it fits. */
static int sudod_copy(char *dst, uint32_t capacity, const char *src)
{
    uint32_t i = 0;
    if (!dst || !capacity) {
        return -1;
    }
    while (src && src[i]) {
        if (i + 1 >= capacity) {
            return -1;
        }
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
    return 0;
}

static int sudod_text_eq(const char *a, const char *b)
{
    if (!a || !b) {
        return 0;
    }
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

struct sudod_path {
    char text[RELIEFOS_FS_PATH_LEN];
};

/* Derive the directory that must be re-listed after a write. */
static int sudod_parent_of(struct sudod_path *out, const char *path)
{
    uint32_t length;
    if (!out || !path || path[0] != '/') {
        return -1;
    }
    for (length = 0; path[length]; ++length) {
        if (length + 1 >= sizeof(out->text)) {
            return -1;
        }
    }
    while (length > 1 && path[length - 1] == '/') {
        --length;
    }
    while (length > 1 && path[length - 1] != '/') {
        --length;
    }
    while (length > 1 && path[length - 1] == '/') {
        --length;
    }
    if (length >= sizeof(out->text)) {
        return -1;
    }
    memcpy(out->text, path, length);
    out->text[length] = 0;
    if (!out->text[0]) {
        out->text[0] = '/';
        out->text[1] = 0;
    }
    return 0;
}

/* Enumerate `path` into the shared result buffer. This is the same
 * open + reliefos_readdir pair Fileman itself uses, so the entries are exactly
 * what its list view already understands. */
static int sudod_load_dir(const char *path)
{
    int fd = sudod_open_dir(path);
    uint32_t count = 0;
    if (fd < 0) {
        return -errno;
    }
    for (;;) {
        struct reliefos_dir_entry entry;
        int ret;
        if (count >= RELIEFOS_FS_MAX_ENTRIES) {
            /* Report a partial view as an error instead of silently dropping
             * entries the user would then believe are gone. */
            close(fd);
            return -EOVERFLOW;
        }
        ret = reliefos_readdir(fd, &entry);
        if (ret < 0) {
            close(fd);
            return ret;
        }
        if (ret == 0) {
            break;
        }
        sudod_output.entries[count++] = entry;
    }
    close(fd);
    sudod_output.count = count;
    return 0;
}

static int sudod_remove_at(int parent, const char *name, unsigned depth, int recursive)
{
    struct stat st;
    if (depth > 128) return -ELOOP;
    if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
        return -errno;
    }
    if ((st.st_mode & S_IFMT) != S_IFDIR) {
        return unlinkat(parent, name, 0) < 0 ? -errno : 0;
    }
    if (!recursive) return -EINVAL;
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }
    for (;;) {
        struct reliefos_dir_entry entry;
        int ret = reliefos_readdir(fd, &entry);
        if (ret < 0) {
            close(fd);
            return ret;
        }
        if (ret == 0) {
            break;
        }
        if (sudod_text_eq(entry.name, ".") || sudod_text_eq(entry.name, "..")) {
            continue;
        }
        int nested = sudod_remove_at(fd, entry.name, depth + 1, recursive);
        if (nested < 0) {
            close(fd);
            return nested;
        }
        /* Removal can compact directory records; restart rather than skip
         * the next entry at the old offset. */
        if (lseek(fd, 0, SEEK_SET) < 0) { int error = errno; close(fd); return -error; }
    }
    close(fd);
    return unlinkat(parent, name, AT_REMOVEDIR) < 0 ? -errno : 0;
}

static int sudod_remove_tree(const char *path, int recursive)
{
    char name[RELIEFOS_FS_PATH_LEN];
    int fd = sudod_parent_fd(path, name);
    if (fd < 0) return -errno;
    int result = sudod_remove_at(fd, name, 0, recursive);
    close(fd);
    return result;
}

static int sudod_write_result(void)
{
    size_t written = 0;
    while (written < sizeof(sudod_output)) {
        ssize_t n = write(STDOUT_FILENO, (const char *)&sudod_output + written,
                          sizeof(sudod_output) - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        written += (size_t)n;
    }
    return 0;
}

static int sudod_parse_arguments(int argc, char **argv, struct sudod_request *request)
{
    if (argc != 7 || strcmp(argv[1], "--op") || strcmp(argv[3], "--path1") ||
        strcmp(argv[5], "--path2") || strlen(argv[2]) != 1 ||
        argv[2][0] < '1' || argv[2][0] > '4') return -1;
    memset(request, 0, sizeof(*request));
    request->op = (uint32_t)(argv[2][0] - '0');
    return sudod_copy(request->path1, sizeof(request->path1), argv[4]) < 0 ||
           sudod_copy(request->path2, sizeof(request->path2), argv[6]) < 0 ? -1 : 0;
}

static int sudod_finish(int32_t status)
{
    sudod_output.status = status;
    return sudod_write_result() < 0 || status ? 1 : 0;
}

static int askpass(const char *prompt)
{
    if (!prompt || !strcmp(prompt, "Password:") || !strcmp(prompt, "Password: "))
        prompt = T("Password:");
    execl("/usr/lib/reliefos/apps/reliefaskpass/reliefaskpass.elf",
          "reliefaskpass", prompt, (char *)0);
    perror("Start authentication prompt");
    return 1;
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    bindtextdomain("leonos", RELIEFOS_LAYOUT_LOCALE);
    textdomain("leonos");
    struct sudod_request request;
    struct sudod_path dir;
    const char *confirm;
    int result;

    if (argc == 2) return askpass(argv[1]);
    struct stat output;
    if (geteuid() != 0 || fstat(STDOUT_FILENO, &output) < 0 ||
        (!S_ISFIFO(output.st_mode) && !S_ISSOCK(output.st_mode))) return 126;
    if (sudod_parse_arguments(argc, argv, &request) < 0) {
        return 2;
    }
    confirm = request.path2[0] ? request.path2 : 0;
    sudod_output.magic = SUDOD_RESULT_MAGIC;
    sudod_output.reserved = 0;
    /* Validate the exact arguments authorized by sudoers before touching paths. */
    if (sudo_fileop_check(request.op, request.path1, confirm) < 0) {
        return sudod_finish(-EINVAL);
    }
    if (request.op == SUDO_FILEOP_UNLINK &&
        sudo_delete_needs_confirm(SUDO_FILEOP_UNLINK, request.path1,
                                  sudod_straight_stat, NULL) &&
        (!confirm || strcmp(confirm, SUDO_DELETE_CONFIRM) != 0)) {
        /* Recursive removal never proceeds on the password dialog alone. */
        return sudod_finish(-EINVAL);
    }

    if (request.op == SUDO_FILEOP_LIST) {
        return sudod_finish(sudod_load_dir(request.path1));
    }
    if (request.op == SUDO_FILEOP_MKDIR) {
        int fd = sudod_open_dir(request.path1);
        result = fd < 0 || mkdirat(fd, request.path2, 0755) < 0 ? -errno : 0;
        if (fd >= 0) close(fd);
    } else if (request.op == SUDO_FILEOP_RENAME) {
        char first[RELIEFOS_FS_PATH_LEN], second[RELIEFOS_FS_PATH_LEN];
        int src = sudod_parent_fd(request.path1, first);
        int dst = src < 0 ? -1 : sudod_parent_fd(request.path2, second);
        result = src < 0 || dst < 0 || renameat(src, first, dst, second) < 0 ? -errno : 0;
        if (src >= 0) close(src);
        if (dst >= 0) close(dst);
    } else if (request.op == SUDO_FILEOP_UNLINK) {
        result = sudod_remove_tree(request.path1, confirm && !strcmp(confirm, SUDO_DELETE_CONFIRM));
    } else {
        result = -EINVAL;
    }
    if (result < 0) {
        return sudod_finish(result);
    }
    /* Report the affected directory's fresh contents. */
    if (request.op == SUDO_FILEOP_MKDIR) {
        result = sudod_load_dir(request.path1);
    } else if (sudod_parent_of(&dir, request.path1) < 0) {
        result = -ENAMETOOLONG;
    } else {
        result = sudod_load_dir(dir.text);
    }
    return sudod_finish(result);
}
