#define _POSIX_C_SOURCE 200809L
#include "installer_update.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECKPOINT_MAX 1024

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

static int phase_valid(const char *phase)
{
    static const char *const phases[] = {
        "packages", "packages-failed", "root-ready", "boot", "boot-failed", "done",
    };
    for (size_t i = 0; i < sizeof(phases) / sizeof(phases[0]); ++i)
        if (!strcmp(phase, phases[i])) return 1;
    return 0;
}

static int digest_valid(const char *digest)
{
    return strnlen(digest, 65) == 64 && strspn(digest, "0123456789abcdef") == 64;
}

static int uuid_valid(const char *uuid)
{
    if (strnlen(uuid, 37) != 36) return 0;
    unsigned nonzero = 0;
    for (unsigned i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (uuid[i] != '-') return 0;
        } else {
            if (!strchr("0123456789abcdef", uuid[i])) return 0;
            nonzero |= uuid[i] != '0';
        }
    }
    return nonzero != 0;
}

static int checkpoint_valid(const struct installer_update_checkpoint *state)
{
    return memchr(state->phase, 0, sizeof(state->phase)) && phase_valid(state->phase) &&
        uuid_valid(state->root_uuid) && digest_valid(state->index_sha256) &&
        digest_valid(state->database_sha256) && digest_valid(state->world_sha256);
}

/** @brief Publish a durable checkpoint relative to its held directory.
 * The new record becomes visible only after its contents are fsynced. A
 * directory fsync after rename makes that publication survive a power loss.
 */
int installer_update_write_checkpoint(const char *root, const struct installer_update_checkpoint *state)
{
    if (!root || !state || !checkpoint_valid(state)) return -EINVAL;
    char record[CHECKPOINT_MAX];
    int length = snprintf(record, sizeof(record),
        "format=1\nphase=%s\nroot_uuid=%s\nindex_sha256=%s\ndatabase_sha256=%s\nworld_sha256=%s\n",
        state->phase, state->root_uuid, state->index_sha256, state->database_sha256, state->world_sha256);
    if (length < 0 || (size_t)length >= sizeof(record)) return -EOVERFLOW;
    int directory = state_directory(root, 1);
    if (directory < 0) return directory;
    char temporary[64];
    int fd = -1;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        snprintf(temporary, sizeof(temporary), ".state-%ld-%u", (long)getpid(), attempt);
        fd = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) { int result = -errno; close(directory); return result; }
    int result = 0;
    size_t used = 0;
    while (used < (size_t)length) {
        ssize_t count = write(fd, record + used, (size_t)length - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { result = count < 0 ? -errno : -EIO; break; }
        used += (size_t)count;
    }
    if (!result && fsync(fd) < 0) result = -errno;
    if (close(fd) < 0 && !result) result = -errno;
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
    int fd = openat(directory, "state", O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    int result = fd < 0 ? -errno : 0;
    close(directory);
    if (result) return result == -ENOENT ? 0 : result;
    struct stat status;
    if (fstat(fd, &status) < 0) result = -errno;
    else if (!S_ISREG(status.st_mode) || status.st_size <= 0) result = -EINVAL;
    else if (status.st_size > CHECKPOINT_MAX) result = -EOVERFLOW;
    char record[CHECKPOINT_MAX + 1];
    size_t used = 0;
    while (!result && used < sizeof(record)) {
        ssize_t count = read(fd, record + used, sizeof(record) - used);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { result = -errno; break; }
        if (!count) break;
        used += (size_t)count;
    }
    if (close(fd) < 0 && !result) result = -errno;
    if (!result && (used > CHECKPOINT_MAX || memchr(record, 0, used))) result = -EINVAL;
    if (result) return result;
    record[used] = 0;
    struct installer_update_checkpoint candidate = {0};
    unsigned fields = 0;
    char *line = record;
    while (*line) {
        char *end = strchr(line, '\n');
        if (!end) return -EINVAL;
        *end = 0;
        char *value = strchr(line, '=');
        if (!value) return -EINVAL;
        *value++ = 0;
        char *out = NULL;
        size_t capacity = 0;
        unsigned bit = 0;
        if (!strcmp(line, "format") && !strcmp(value, "1")) bit = 1;
        else if (!strcmp(line, "phase")) { out = candidate.phase; capacity = sizeof(candidate.phase); bit = 2; }
        else if (!strcmp(line, "root_uuid")) { out = candidate.root_uuid; capacity = sizeof(candidate.root_uuid); bit = 4; }
        else if (!strcmp(line, "index_sha256")) { out = candidate.index_sha256; capacity = sizeof(candidate.index_sha256); bit = 8; }
        else if (!strcmp(line, "database_sha256")) { out = candidate.database_sha256; capacity = sizeof(candidate.database_sha256); bit = 16; }
        else if (!strcmp(line, "world_sha256")) { out = candidate.world_sha256; capacity = sizeof(candidate.world_sha256); bit = 32; }
        else return -EINVAL;
        if ((fields & bit) || (out && strlen(value) >= capacity)) return -EINVAL;
        if (out) strcpy(out, value);
        fields |= bit;
        line = end + 1;
    }
    if (fields != 63 || !checkpoint_valid(&candidate)) return -EINVAL;
    *state = candidate;
    return 0;
}
