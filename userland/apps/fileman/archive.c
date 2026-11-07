#define _XOPEN_SOURCE 700
#include "archive.h"
#include "boundary.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int run_tar(char *const *arguments)
{
    pid_t child = fork();
    if (child < 0) return -errno;
    if (!child) {
        execv("/bin/tar", arguments);
        _exit(127);
    }
    int status;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0) return -errno;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -EIO;
}

static int archive_target(const char *path, char *target, size_t capacity)
{
    if (!path || !*path || strlen(path) >= PATH_MAX) return -EINVAL;
    char parent[PATH_MAX], resolved[PATH_MAX];
    strcpy(parent, path);
    char *separator = strrchr(parent, '/');
    const char *name = separator ? separator + 1 : path;
    if (!fileman_name_valid(name)) return -EINVAL;
    if (separator) {
        if (separator == parent) strcpy(parent, "/");
        else *separator = 0;
    } else strcpy(parent, ".");
    if (!realpath(parent, resolved)) return -errno;
    name = strrchr(path, '/');
    name = name ? name + 1 : path;
    return fileman_join_path(target, capacity, resolved, name) < 0 ? -ENAMETOOLONG : 0;
}

int fileman_tar_create(const char *archive, const char *directory,
                       const char *const *members, size_t count)
{
    if (!directory || !members || !count || count > 256) return -EINVAL;
    char source[PATH_MAX], target[PATH_MAX], temporary[PATH_MAX];
    if (!realpath(directory, source)) return -errno;
    int result = archive_target(archive, target, sizeof(target));
    if (result < 0) return result;
    for (size_t i = 0; i < count; ++i) {
        if (!members[i]) return -EINVAL;
        char member[PATH_MAX];
        if (!strcmp(members[i], ".") && count == 1) strcpy(member, source);
        else {
            if (!fileman_name_valid(members[i])) return -EINVAL;
            if (fileman_join_path(member, sizeof(member), source, members[i]) < 0)
                return -ENAMETOOLONG;
        }
        if (fileman_path_contains(member, target)) return -EINVAL;
    }
    int length = snprintf(temporary, sizeof(temporary), "%s.XXXXXX", target);
    if (length < 0 || (size_t)length >= sizeof(temporary)) return -ENAMETOOLONG;
    int descriptor = mkstemp(temporary);
    if (descriptor < 0) return -errno;
    close(descriptor);
    char **arguments = calloc(count + 7, sizeof(*arguments));
    if (!arguments) { unlink(temporary); return -ENOMEM; }
    arguments[0] = "tar";
    arguments[1] = "-cf";
    arguments[2] = temporary;
    arguments[3] = "-C";
    arguments[4] = source;
    arguments[5] = "--";
    for (size_t i = 0; i < count; ++i) arguments[i + 6] = (char *)members[i];
    result = run_tar(arguments);
    free(arguments);
    if (!result && rename(temporary, target) < 0) result = -errno;
    if (result < 0) unlink(temporary);
    return result;
}

int fileman_tar_extract(const char *archive, const char *directory)
{
    if (!archive || !directory) return -EINVAL;
    char source[PATH_MAX], destination[PATH_MAX];
    if (!realpath(archive, source) || !realpath(directory, destination)) return -errno;
    char *arguments[] = {"tar", "-xpf", source, "-C", destination, NULL};
    return run_tar(arguments);
}
