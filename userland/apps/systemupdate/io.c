/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mbedtls/sha256.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

volatile sig_atomic_t update_cancelled;

int update_error(const char *operation, int result)
{
    if (result >= 0) result = -EIO;
    fprintf(stderr, "[reliefos-update] %s: %s (%d)\n", operation, strerror(-result), result);
    return result;
}

int update_path(char out[PATH_MAX], const char *root, const char *suffix)
{
    if (!root || root[0] != '/' || !suffix || suffix[0] == '/' || strstr(suffix, "../")) return -EINVAL;
    int size = snprintf(out, PATH_MAX, "%s%s%s", root, !strcmp(root, "/") ? "" : "/", suffix);
    return size < 0 || size >= PATH_MAX ? -ENAMETOOLONG : 0;
}

/* Open every ancestor without following symlinks, including a preexisting leaf. */
int update_directory(const char *path, int create)
{
    if (!path || path[0] != '/' || strlen(path) >= PATH_MAX) return -EINVAL;
    char copy[PATH_MAX]; strcpy(copy, path);
    int directory = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) return -errno;
    char *save = NULL;
    for (char *name = strtok_r(copy, "/", &save); name; name = strtok_r(NULL, "/", &save)) {
        if (!strcmp(name, ".") || !strcmp(name, "..")) { close(directory); return -EINVAL; }
        if (create) {
            int made = mkdirat(directory, name, 0700);
            if ((made < 0 && errno != EEXIST) || (!made && fsync(directory))) {
                int result = -errno; close(directory); return result;
            }
        }
        int child = openat(directory, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int result = child < 0 ? -errno : 0;
        close(directory);
        if (result) return result;
        directory = child;
    }
    return directory;
}

static int parent(const char *path, char leaf[NAME_MAX + 1])
{
    if (!path || strlen(path) >= PATH_MAX) return -EINVAL;
    char copy[PATH_MAX]; strcpy(copy, path);
    char *slash = strrchr(copy, '/');
    if (!slash || !slash[1] || strlen(slash + 1) > NAME_MAX) return -EINVAL;
    strcpy(leaf, slash + 1);
    if (!strcmp(leaf, ".") || !strcmp(leaf, "..")) return -EINVAL;
    if (slash == copy) slash[1] = 0; else *slash = 0;
    return update_directory(copy, 0);
}

static int file_open(const char *path)
{
    char leaf[NAME_MAX + 1]; int directory = parent(path, leaf);
    if (directory < 0) return directory;
    int fd = openat(directory, leaf, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    int result = fd < 0 ? -errno : fd;
    close(directory); return result;
}

int update_regular(const char *path, uint64_t limit)
{
    int fd = file_open(path); if (fd < 0) return fd;
    struct stat status;
    int result = fstat(fd, &status) ? -errno : !S_ISREG(status.st_mode) || status.st_size < 0 ||
        (uint64_t)status.st_size > limit ? -EINVAL : 0;
    close(fd); return result;
}

static int write_all(int fd, const void *bytes, size_t size)
{
    const char *at = bytes;
    while (size) {
        ssize_t count = write(fd, at, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return count < 0 ? -errno : -EIO;
        at += count; size -= (size_t)count;
    }
    return 0;
}

int update_write(const char *path, const void *bytes, size_t size, unsigned mode)
{
    char leaf[NAME_MAX + 1], temporary[80], random[65];
    int directory = parent(path, leaf); if (directory < 0) return directory;
    int fd = -1, result = 0;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        result = update_random_id(random);
        if (result) break;
        snprintf(temporary, sizeof(temporary), ".new-%s", random);
        fd = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (result) { close(directory); return result; }
    if (fd < 0) { int result = -errno; close(directory); return result; }
    result = write_all(fd, bytes, size);
    if (!result && fchmod(fd, mode)) result = -errno;
    if (!result && fsync(fd)) result = -errno;
    if (close(fd) && !result) result = -errno;
    if (!result && renameat(directory, temporary, directory, leaf)) result = -errno;
    if (!result && fsync(directory)) result = -errno;
    if (result) (void)unlinkat(directory, temporary, 0);
    close(directory); return result;
}

int update_read(const char *path, char **bytes, size_t *size, size_t limit)
{
    *bytes = NULL; *size = 0;
    int fd = file_open(path); if (fd < 0) return fd;
    struct stat status;
    int result = fstat(fd, &status) ? -errno : !S_ISREG(status.st_mode) || status.st_size < 0 ||
        (uint64_t)status.st_size > limit ? -EFBIG : 0;
    if (result) { close(fd); return result; }
    size_t wanted = (size_t)status.st_size;
    char *buffer = malloc(wanted + 1);
    if (!buffer) { close(fd); return -ENOMEM; }
    size_t used = 0;
    while (used < wanted) {
        ssize_t count = read(fd, buffer + used, wanted - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { result = count < 0 ? -errno : -EIO; break; }
        used += (size_t)count;
    }
    close(fd);
    if (result) { free(buffer); return result; }
    buffer[used] = 0; *bytes = buffer; *size = used; return 0;
}

int update_copy(const char *source, const char *destination, unsigned mode)
{
    char *bytes; size_t size;
    int result = update_read(source, &bytes, &size, UPDATE_METADATA_MAX);
    if (!result) { result = update_write(destination, bytes, size, mode); free(bytes); }
    return result;
}

static int names(const char *path, struct dirent ***entries)
{
    int fd = update_directory(path, 0); if (fd < 0) return fd;
    close(fd);
    int count = scandir(path, entries, NULL, alphasort);
    return count < 0 ? -errno : count;
}

/* The APK package owns this standard link. Resolve only its two canonical
 * spellings inside the managed root; all opened ancestors and the bundle
 * itself still use the no-follow helpers. Never resolve an arbitrary link. */
static int canonical_ca_bundle(const char *path, char bundle[PATH_MAX], char target[64])
{
    static const char suffix[] = "/etc/apk/ca.pem";
    size_t length = strlen(path), suffix_size = sizeof(suffix) - 1;
    if (length < suffix_size || strcmp(path + length - suffix_size, suffix)) return -EINVAL;
    char leaf[NAME_MAX + 1]; int directory = parent(path, leaf);
    if (directory < 0) return directory;
    ssize_t size = readlinkat(directory, leaf, target, 64);
    int result = size < 0 ? -errno : size >= 64 ? -EINVAL : 0;
    close(directory);
    if (result) return result;
    target[size] = 0;
    if (strcmp(target, "../ssl/certs/ca-certificates.crt") &&
        strcmp(target, "/etc/ssl/certs/ca-certificates.crt")) return -EINVAL;
    char root[PATH_MAX]; size_t prefix = length - suffix_size;
    memcpy(root, path, prefix); root[prefix] = 0;
    return update_path(bundle, prefix ? root : "/", "etc/ssl/certs/ca-certificates.crt");
}

static int copy_tree(const char *source, const char *destination, int allow_ca)
{
    int fd = update_directory(destination, 1); if (fd < 0) return fd; close(fd);
    struct dirent **entries = NULL; int count = names(source, &entries);
    if (count < 0) return count;
    int result = 0;
    for (int i = 0; i < count; ++i) {
        const char *name = entries[i]->d_name;
        if (!result && strcmp(name, ".") && strcmp(name, "..") && strcmp(name, "lock") && strcmp(name, "cache")) {
            char from[PATH_MAX], to[PATH_MAX]; struct stat status;
            result = update_path(from, source, name);
            if (!result) result = update_path(to, destination, name);
            if (!result && lstat(from, &status)) result = -errno;
            if (!result) {
                if (S_ISDIR(status.st_mode)) result = copy_tree(from, to, 0);
                else if (S_ISREG(status.st_mode)) result = update_copy(from, to, 0600);
                else if (S_ISLNK(status.st_mode)) {
                    char bundle[PATH_MAX], target[64];
                    result = allow_ca ? canonical_ca_bundle(from, bundle, target) : -EINVAL;
                    if (!result) result = update_copy(bundle, to, 0600);
                }
                else result = -EINVAL;
            }
        }
        free(entries[i]);
    }
    free(entries); return result;
}

int update_copy_tree(const char *source, const char *destination)
{
    return copy_tree(source, destination, 1);
}

static void hex(const unsigned char digest[32], char output[65])
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) { output[2*i] = digits[digest[i] >> 4]; output[2*i+1] = digits[digest[i] & 15]; }
    output[64] = 0;
}

int update_hash(const char *path, char output[65])
{
    int fd = file_open(path); if (fd < 0) return fd;
    struct stat status; int result = fstat(fd, &status) ? -errno : !S_ISREG(status.st_mode) ? -EINVAL : 0;
    mbedtls_sha256_context digest; mbedtls_sha256_init(&digest);
    if (!result && mbedtls_sha256_starts(&digest, 0)) result = -EIO;
    char buffer[65536]; unsigned char binary[32];
    while (!result) {
        ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { result = -errno; break; }
        if (!count) break;
        if (mbedtls_sha256_update(&digest, (unsigned char *)buffer, (size_t)count)) result = -EIO;
    }
    if (!result && mbedtls_sha256_finish(&digest, binary)) result = -EIO;
    if (!result) hex(binary, output);
    mbedtls_sha256_free(&digest); close(fd); return result;
}

/* Length-separated names and file hashes make directory membership part of the seal. */
static int hash_tree(mbedtls_sha256_context *digest, const char *path, unsigned depth, size_t *members)
{
    if (depth > 24 || *members > 100000) return -EFBIG;
    struct dirent **entries = NULL; int count = names(path, &entries);
    if (count < 0) return count;
    int result = 0;
    for (int i = 0; i < count; ++i) {
        const char *name = entries[i]->d_name;
        if (!result && strcmp(name, ".") && strcmp(name, "..") && strcmp(name, "lock") && strcmp(name, "cache")) {
            char full[PATH_MAX], header[NAME_MAX + 40], content[65]; struct stat status;
            result = update_path(full, path, name);
            if (!result && lstat(full, &status)) result = -errno;
            if (!result && !S_ISREG(status.st_mode) && !S_ISDIR(status.st_mode) && !S_ISLNK(status.st_mode)) result = -EINVAL;
            if (!result) {
                ++*members;
                int length = snprintf(header, sizeof(header), "%c%zu:%s:",
                    S_ISDIR(status.st_mode) ? 'd' : S_ISLNK(status.st_mode) ? 'l' : 'f', strlen(name), name);
                if (length < 0 || (size_t)length >= sizeof(header) ||
                    mbedtls_sha256_update(digest, (unsigned char *)header, (size_t)length)) result = -EIO;
                if (!result && S_ISDIR(status.st_mode)) result = hash_tree(digest, full, depth + 1, members);
                else if (!result) {
                    if (S_ISLNK(status.st_mode)) {
                        char bundle[PATH_MAX], target[64], link_record[80];
                        result = !depth ? canonical_ca_bundle(full, bundle, target) : -EINVAL;
                        if (!result) {
                            int size = snprintf(link_record, sizeof(link_record), "%zu:%s:", strlen(target), target);
                            if (size < 0 || (size_t)size >= sizeof(link_record) ||
                                mbedtls_sha256_update(digest, (unsigned char *)link_record, (size_t)size)) result = -EIO;
                        }
                        if (!result) result = update_hash(bundle, content);
                    } else result = update_hash(full, content);
                    if (!result && mbedtls_sha256_update(digest, (unsigned char *)content, 64)) result = -EIO;
                }
                if (!result && mbedtls_sha256_update(digest, (const unsigned char *)";", 1)) result = -EIO;
            }
        }
        free(entries[i]);
    }
    free(entries); return result;
}

int update_hash_tree(const char *path, char output[65])
{
    mbedtls_sha256_context digest; mbedtls_sha256_init(&digest);
    int result = mbedtls_sha256_starts(&digest, 0) ? -EIO : 0;
    size_t members = 0; unsigned char binary[32];
    if (!result) result = hash_tree(&digest, path, 0, &members);
    if (!result && mbedtls_sha256_finish(&digest, binary)) result = -EIO;
    if (!result) hex(binary, output);
    mbedtls_sha256_free(&digest); return result;
}

static int configuration_hash(struct update_context *context, char output[65], int database)
{
    const char *inputs[] = {"etc/apk", "lib/apk/db", "etc/reliefos/rpr.conf", "lib/apk/repositories.d"};
    char records[4 * 65 + 128] = {0}; size_t used = 0;
    struct stat status;
    if (stat(context->root, &status)) return -errno;
    int length = snprintf(records, sizeof(records), "%llu:%llu:", (unsigned long long)status.st_dev,
                          (unsigned long long)status.st_ino);
    if (length < 0 || (size_t)length >= sizeof(records)) return -EIO;
    used = (size_t)length;
    for (size_t i = 0; i < sizeof(inputs)/sizeof(inputs[0]); ++i) {
        if (i == 1 && !database) continue;
        char path[PATH_MAX], digest[65]; int result = update_path(path, context->root, inputs[i]);
        if (!result) result = i == 2 ? update_hash(path, digest) : update_hash_tree(path, digest);
        /* The same root has a different pathname/isolation view inside APK's
         * hook chroot. Hash absence identically; source policy is checked by
         * update_sources before any production plan can become ready. */
        if (result == -ENOENT && (i == 3 || i == 2)) strcpy(digest, "absent");
        else if (result) return result;
        size_t size = strlen(digest); memcpy(records + used, digest, size); used += size; records[used++] = '\n';
    }
    unsigned char binary[32];
    if (mbedtls_sha256((unsigned char *)records, used, binary, 0)) return -EIO;
    hex(binary, output); return 0;
}

int update_target_hash(struct update_context *context, char output[65])
{
    return configuration_hash(context, output, 1);
}

int update_policy_hash(struct update_context *context, char output[65])
{
    return configuration_hash(context, output, 0);
}

int update_token(const char *text)
{
    return text && strlen(text) == 64 && strspn(text, "0123456789abcdef") == 64;
}

int update_random_id(char output[65])
{
    unsigned char entropy[32]; size_t used = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC); if (fd < 0) return -errno;
    while (used < sizeof(entropy)) {
        ssize_t count = read(fd, entropy + used, sizeof(entropy) - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { int result = count < 0 ? -errno : -EIO; close(fd); return result; }
        used += (size_t)count;
    }
    close(fd); hex(entropy, output); return 0;
}

int update_state_save(struct update_context *context, const char *phase)
{
    if (!phase || !*phase || strlen(phase) >= sizeof(context->state.phase) ||
        strspn(phase, "abcdefghijklmnopqrstuvwxyz-") != strlen(phase)) return -EINVAL;
    strcpy(context->state.phase, phase);
    char path[PATH_MAX], bytes[512]; int result = update_path(path, context->plan, "state");
    int size = snprintf(bytes, sizeof(bytes), "format=1\nphase=%s\ntarget=%s\npolicy=%s\nseal=%s\nreboot_required=%d\n",
                        phase, context->state.target, context->state.policy,
                        context->state.seal[0] ? context->state.seal : "-", context->state.reboot);
    if (size < 0 || (size_t)size >= sizeof(bytes)) return -EOVERFLOW;
    if (!result) result = update_write(path, bytes, (size_t)size, 0600);
    return result;
}

int update_state_load(struct update_context *context)
{
    char path[PATH_MAX], *bytes; size_t size; int result = update_path(path, context->plan, "state");
    if (!result) result = update_read(path, &bytes, &size, 511);
    if (result) return result;
    struct update_state state = {0}; int consumed = 0;
    int count = sscanf(bytes, "format=1\nphase=%31[a-z-]\ntarget=%64[0-9a-f]\npolicy=%64[0-9a-f]\nseal=%64[0-9a-f-]\nreboot_required=%d\n%n",
                       state.phase, state.target, state.policy, state.seal, &state.reboot, &consumed);
    if (count != 5 || (size_t)consumed != size || memchr(bytes, 0, size) || !update_token(state.target) || !update_token(state.policy) ||
        (strcmp(state.seal, "-") && !update_token(state.seal)) || (state.reboot != 0 && state.reboot != 1)) result = -EINVAL;
    free(bytes); if (!result) context->state = state; return result;
}

int update_active(struct update_context *context, int publish)
{
    char path[PATH_MAX], *bytes; size_t size;
    int result = update_path(path, context->base, "active"); if (result) return result;
    if (publish) {
        char record[66]; snprintf(record, sizeof(record), "%s\n", context->id);
        return update_write(path, record, 65, 0600);
    }
    result = update_read(path, &bytes, &size, 65);
    if (result) return result;
    if (size != 65 || bytes[64] != '\n') result = -EINVAL;
    else { bytes[64] = 0; result = update_token(bytes) ? update_plan_open(context, bytes) : -EINVAL; }
    free(bytes); return result;
}

int update_space(const char *path, uint64_t required)
{
    struct statvfs space;
    if (statvfs(path, &space)) return -errno;
    uint64_t unit = space.f_frsize ? space.f_frsize : space.f_bsize;
    if (!unit || space.f_bavail > UINT64_MAX/unit) return -EOVERFLOW;
    uint64_t available = (uint64_t)space.f_bavail * unit;
    if (available < required) {
        fprintf(stderr, "[reliefos-update] insufficient space: available=%llu required=%llu\n",
                (unsigned long long)available, (unsigned long long)required);
        return -ENOSPC;
    }
    return 0;
}

int update_tree_bytes(const char *path, uint64_t *bytes)
{
    struct dirent **entries = NULL; int count = names(path, &entries); if (count < 0) return count;
    int result = 0;
    for (int i = 0; i < count; ++i) {
        const char *name = entries[i]->d_name;
        if (!result && strcmp(name, ".") && strcmp(name, "..")) {
            char full[PATH_MAX]; struct stat status; result = update_path(full, path, name);
            if (!result && lstat(full, &status)) result = -errno;
            if (!result && S_ISDIR(status.st_mode)) result = update_tree_bytes(full, bytes);
            else if (!result && (S_ISREG(status.st_mode) || S_ISLNK(status.st_mode)) && status.st_size >= 0 && (uint64_t)status.st_size <= UINT64_MAX - *bytes)
                *bytes += (uint64_t)status.st_size;
            else if (!result) result = -EINVAL;
        }
        free(entries[i]);
    }
    free(entries); return result;
}

void update_event(struct update_context *context, const char *event)
{
    if (context->machine) printf("event=%s\n", event);
    else printf("[reliefos-update] %s\n", event);
    fflush(stdout);
}

static void terminate_owned_group(int signal_number)
{
    (void)signal_number;
    (void)kill(0, SIGKILL);
    _exit(126);
}

/* All privileged subprocesses are direct execve calls with a fixed environment. */
int update_run(struct update_context *context, char *const arguments[], const char *output, int offline_commit)
{
    int channel[2]; if (pipe2(channel, O_CLOEXEC)) return -errno;
    int capture = -1;
    if (output) {
        char leaf[NAME_MAX + 1]; int directory = parent(output, leaf);
        if (directory < 0) { close(channel[0]); close(channel[1]); return directory; }
        capture = openat(directory, leaf, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
        close(directory);
        if (capture < 0) { int result = -errno; close(channel[0]); close(channel[1]); return result; }
    }
    pid_t owner = getpid();
    pid_t pid = fork();
    if (pid < 0) { int result = -errno; close(channel[0]); close(channel[1]); if (capture >= 0) close(capture); return result; }
    if (!pid) {
        /* A native supervisor owns the entire APK/script group. Parent death
         * must stop grandchildren as well as APK before recovery is admitted. */
        if (setpgid(0, 0)) _exit(126);
        struct sigaction death = {.sa_handler = terminate_owned_group};
        sigemptyset(&death.sa_mask);
        if (sigaction(SIGUSR1, &death, NULL) || prctl(PR_SET_PDEATHSIG, SIGUSR1) || getppid() != owner)
            terminate_owned_group(SIGUSR1);
        pid_t supervisor = getpid();
        pid_t worker = fork();
        if (worker < 0) _exit(126);
        if (worker) {
            close(channel[0]); close(channel[1]);
            if (capture >= 0) close(capture);
            int status;
            while (waitpid(worker, &status, 0) < 0) { if (errno != EINTR) _exit(126); }
            _exit(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
        }
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != supervisor) _exit(126);
        close(channel[0]);
        if (dup2(channel[1], STDOUT_FILENO) < 0 || dup2(channel[1], STDERR_FILENO) < 0) _exit(126);
        close(channel[1]);
        signal(SIGUSR1, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL); signal(SIGHUP, SIG_DFL);
        char marker[96]; snprintf(marker, sizeof(marker), "RELIEFOS_UPDATE_PLAN=%s", context->id);
        char *environment[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL=C", "LANG=C", "HOME=/root", "APK_CONFIG=/dev/null",
                               offline_commit ? marker : NULL, NULL};
        execve(arguments[0], arguments, environment); _exit(127);
    }
    (void)setpgid(pid, pid); close(channel[1]);
    size_t used = 0; int result = 0, ended = 0, status = 0;
    struct timespec start, worker_finished = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &start)) result = -errno;
    for (;;) {
        if (result) break;
        struct pollfd descriptor = {.fd = channel[0], .events = POLLIN};
        int ready = poll(&descriptor, 1, 200);
        if (ready < 0 && errno != EINTR) { result = -errno; break; }
        if (ready > 0 && (descriptor.revents & (POLLIN | POLLHUP))) {
            char buffer[8192]; ssize_t count = read(channel[0], buffer, sizeof(buffer));
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) { result = -errno; break; }
            if (!count) break;
            if ((size_t)count > UPDATE_OUTPUT_MAX - used) { result = -EFBIG; break; }
            used += (size_t)count;
            if (write_all(STDERR_FILENO, buffer, (size_t)count) && !result) result = -EIO;
            if (capture >= 0 && write_all(capture, buffer, (size_t)count) && !result) result = -EIO;
        }
        if (!offline_commit && update_cancelled) { result = -ECANCELED; break; }
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now)) { result = -errno; break; }
        if (!offline_commit && now.tv_sec - start.tv_sec > (time_t)(context->timeout_seconds ? context->timeout_seconds : 1800)) {
            result = -ETIMEDOUT; break;
        }
        if (!ended) {
            pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid) { ended = 1; worker_finished = now; }
            else if (waited < 0 && errno != EINTR) { result = -errno; break; }
        } else if (now.tv_sec - worker_finished.tv_sec > 3) {
            /* APK has exited but an owned script descendant still holds its
             * output pipe. Stop that group and report an incomplete lifecycle. */
            (void)kill(-pid, SIGKILL); result = -ECHILD; break;
        }
    }
    close(channel[0]);
    if (capture >= 0) { if (fsync(capture) && !result) result = -errno; close(capture); }
    if (result && !offline_commit) (void)kill(-pid, SIGTERM);
    struct timespec stopping; (void)clock_gettime(CLOCK_MONOTONIC, &stopping);
    while (!ended) {
        pid_t waited = waitpid(pid, &status, result && !offline_commit ? WNOHANG : 0);
        if (waited == pid) ended = 1;
        else if (waited < 0 && errno != EINTR) { if (!result) result = -errno; break; }
        else if (!waited) {
            struct timespec now; (void)clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec - stopping.tv_sec >= 3) (void)kill(-pid, SIGKILL);
            struct timespec interval = {.tv_nsec = 20000000}; nanosleep(&interval, NULL);
        }
    }
    if (!result && (!WIFEXITED(status) || WEXITSTATUS(status))) result = -EIO;
    return result;
}
