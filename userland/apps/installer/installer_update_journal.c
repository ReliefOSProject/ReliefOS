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

static int state_directory(const char *root, int create)
{
    static const char *const components[] = {"var", "lib", "reliefos", "installer-update"};
    int fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -errno;
    for (size_t i = 0; i < sizeof(components) / sizeof(components[0]); ++i) {
        if (create && i >= 2) {
            int made = mkdirat(fd, components[i], i == 3 ? 0700 : 0755);
            if ((made < 0 && errno != EEXIST) || (!made && fsync(fd) < 0)) {
                int result = -errno;
                close(fd);
                return result;
            }
        }
        int child = openat(fd, components[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int result = child < 0 ? -errno : 0;
        close(fd);
        if (result) return result;
        fd = child;
    }
    return fd;
}

static int valid_field(const char *text, size_t capacity)
{
    return memchr(text, 0, capacity) && text[0] &&
        strspn(text, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == strlen(text);
}

int installer_update_write_checkpoint(const char *root, const struct installer_update_checkpoint *state)
{
    if (!root || !state || !valid_field(state->phase, sizeof(state->phase)) ||
        !valid_field(state->root_uuid, sizeof(state->root_uuid)) ||
        !valid_field(state->index_sha256, sizeof(state->index_sha256)) ||
        !valid_field(state->database_sha256, sizeof(state->database_sha256)) ||
        !valid_field(state->world_sha256, sizeof(state->world_sha256))) return -EINVAL;
    int directory = state_directory(root, 1);
    if (directory < 0) return directory;
    char path[PATH_MAX];
    int length = snprintf(path, sizeof(path), "%s/var/lib/reliefos/installer-update/.state-XXXXXX", root);
    if (length < 0 || (size_t)length >= sizeof(path)) { close(directory); return -ENAMETOOLONG; }
    int fd = mkstemp(path);
    if (fd < 0) { int result = -errno; close(directory); return result; }
    const char *temporary = strrchr(path, '/') + 1;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        int result = -errno;
        close(fd);
        (void)unlinkat(directory, temporary, 0);
        close(directory);
        return result;
    }
    FILE *stream = fdopen(fd, "w");
    int result = stream ? 0 : -errno;
    if (stream) {
        if (fprintf(stream, "format=1\nphase=%s\nroot_uuid=%s\nindex_sha256=%s\ndatabase_sha256=%s\nworld_sha256=%s\n",
                    state->phase, state->root_uuid, state->index_sha256, state->database_sha256, state->world_sha256) < 0 ||
            fflush(stream) || fsync(fd)) result = -errno;
        if (fclose(stream) && !result) result = -errno;
    } else close(fd);
    if (!result && renameat(directory, temporary, directory, "state") < 0) result = -errno;
    if (!result && fsync(directory) < 0) result = -errno;
    if (result) (void)unlinkat(directory, temporary, 0);
    if (close(directory) < 0 && !result) result = -errno;
    return result;
}

int installer_update_read_checkpoint(const char *root, struct installer_update_checkpoint *state)
{
    if (!root || !state) return -EINVAL;
    memset(state, 0, sizeof(*state));
    int directory = state_directory(root, 0);
    if (directory == -ENOENT) return 0;
    if (directory < 0) return directory;
    int fd = openat(directory, "state", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    int result = fd < 0 ? -errno : 0;
    close(directory);
    if (result) return result == -ENOENT ? 0 : result;
    FILE *stream = fdopen(fd, "r");
    if (!stream) { result = -errno; close(fd); return result; }
    char line[160];
    unsigned fields = 0;
    while (fgets(line, sizeof(line), stream)) {
        if (!strchr(line, '\n')) { result = -EINVAL; break; }
        line[strcspn(line, "\n")] = 0;
        char *value = strchr(line, '=');
        if (!value) { result = -EINVAL; break; }
        *value++ = 0;
        char *out = NULL;
        size_t capacity = 0;
        unsigned bit = 0;
        if (!strcmp(line, "format") && !strcmp(value, "1")) bit = 1;
        else if (!strcmp(line, "phase")) { out = state->phase; capacity = sizeof(state->phase); bit = 2; }
        else if (!strcmp(line, "root_uuid")) { out = state->root_uuid; capacity = sizeof(state->root_uuid); bit = 4; }
        else if (!strcmp(line, "index_sha256")) { out = state->index_sha256; capacity = sizeof(state->index_sha256); bit = 8; }
        else if (!strcmp(line, "database_sha256")) { out = state->database_sha256; capacity = sizeof(state->database_sha256); bit = 16; }
        else if (!strcmp(line, "world_sha256")) { out = state->world_sha256; capacity = sizeof(state->world_sha256); bit = 32; }
        else { result = -EINVAL; break; }
        if ((fields & bit) || (out && (strlen(value) >= capacity || !valid_field(value, strlen(value) + 1)))) {
            result = -EINVAL;
            break;
        }
        if (out) strcpy(out, value);
        fields |= bit;
    }
    if (!result && (fields != 63 || ferror(stream))) result = -EINVAL;
    if (fclose(stream) && !result) result = -errno;
    return result;
}
