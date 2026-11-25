#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "installer_update_internal.h"
#include "installer_update_supervisor.h"
#include <reliefos/blockdev.h>
#include "../../auth/standard_accounts.h"
#include <reliefos/bootloader.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define N_(text) (text)

#ifndef UPDATE_ESP
#define UPDATE_ESP "/install/esp"
#endif
#define UPDATE_PACKAGES "/usr/share/reliefos/apk/repository"
#ifndef UPDATE_MEDIA_CONSOLE
#define UPDATE_MEDIA_CONSOLE "/install/root-console"
#define UPDATE_MEDIA_XFCE "/install/root-xfce"
#define UPDATE_MEDIA_XORG "/install/root"
#endif
#ifndef UPDATE_HELPER
#define UPDATE_HELPER "/usr/lib/reliefos/reliefos-apk-update"
#endif
#ifndef UPDATE_LOG
#define UPDATE_LOG "/var/log/installer-update.log"
#endif

/* IPC contains no pointers. Only a stage child writes it; a sequence check
 * prevents a pulse from consuming a partly published observation. */
struct update_event {
    unsigned sequence, progress;
    char message[128];
    struct installer_update_activity activity;
};

struct update_run {
    installer_update_observer observer;
    void *context;
    FILE *log;
    unsigned progress;
    const char *stage;
    struct installer_update_info *out;
    struct update_event *event;
    char message[128];
    struct installer_update_activity activity;
    int applying;
    const char *workspace;
};

static void publish_event(struct update_run *run)
{
    if (run->event) {
        unsigned sequence = __atomic_load_n(&run->event->sequence, __ATOMIC_RELAXED);
        __atomic_store_n(&run->event->sequence, sequence + 1, __ATOMIC_SEQ_CST);
        run->event->progress = run->progress;
        snprintf(run->event->message, sizeof(run->event->message), "%s", run->stage);
        run->event->activity = run->activity;
        __atomic_store_n(&run->event->sequence, sequence + 2, __ATOMIC_RELEASE);
    }
    if (run->observer) run->observer(run->context, run->progress, run->stage);
}

static void report(struct update_run *run, unsigned progress, const char *stage)
{
    run->progress = progress;
    run->stage = stage;
    memset(&run->activity, 0, sizeof(run->activity));
    if (run->out) run->out->activity = run->activity;
    fprintf(stderr, "[installer-update] %u%% %s\n", progress, stage);
    if (run->log) { fprintf(run->log, "[installer-update] %u%% %s\n", progress, stage); fflush(run->log); }
    publish_event(run);
}

/** @brief Present bounded helper telemetry while preserving monotonic progress. */
static void present_activity(struct update_run *run, const struct installer_update_activity *activity)
{
    if (run->activity.action == UPDATE_ACTION_FAILED) return;
    run->activity = *activity;
    run->out->activity = *activity;
    run->stage = installer_update_activity_stage(activity->step);
    unsigned value = installer_update_activity_percent(activity, run->applying);
    if (value > run->progress) run->progress = value;
    publish_event(run);
}

/** @brief Decode complete helper lines only; ordinary command output stays data. */
static int helper_line(struct update_run *run, const char *line)
{
    struct installer_update_activity activity = run->activity;
    int result = installer_update_activity_decode(&activity, line);
    int control = !strncmp(line, "activity\t", 9);
    if (!result) result = installer_update_activity_apk(&activity, line);
    if (result > 0) present_activity(run, &activity);
    if (!control) fprintf(stderr, "%s\n", line);
    else if (result > 0) {
        char detail[768];
        if (!installer_update_activity_format(&activity, detail, sizeof(detail)) && *detail)
            fprintf(stderr, "%s\n", detail);
        else fprintf(stderr, "[installer-update] %s\n", installer_update_activity_stage(activity.step));
    }
    return result < 0 ? result : 0;
}

/* Each command is owned by the isolated worker. APK output is streamed to
 * the frontend's log pipe and the installer session's persistent log. */
static int command(struct update_run *run, const char *const arguments[], char *output, size_t capacity)
{
    int descriptors[2];
    if (pipe(descriptors) < 0) return -errno;
    if (fcntl(descriptors[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(descriptors[1], F_SETFD, FD_CLOEXEC) < 0) {
        int result = -errno;
        close(descriptors[0]); close(descriptors[1]);
        return result;
    }
    posix_spawn_file_actions_t actions;
    int result = posix_spawn_file_actions_init(&actions);
    if (result) { close(descriptors[0]); close(descriptors[1]); return -result; }
    result = posix_spawn_file_actions_addclose(&actions, descriptors[0]);
    if (!result) result = posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO);
    if (!result) result = posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDERR_FILENO);
    if (!result) result = posix_spawn_file_actions_addclose(&actions, descriptors[1]);
    char temporary_environment[PATH_MAX + 8];
    snprintf(temporary_environment, sizeof(temporary_environment), "TMPDIR=%s",
        run->workspace && run->workspace[0] ? run->workspace : "/run");
    char *const environment[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL=C", "LANG=C",
        "HOME=/root", temporary_environment, NULL};
    pid_t child = -1;
    if (!result) result = posix_spawn(&child, arguments[0], &actions, NULL,
                                     (char *const *)arguments, environment);
    posix_spawn_file_actions_destroy(&actions);
    close(descriptors[1]);
    if (result) { close(descriptors[0]); return -result; }
    size_t used = 0;
    char buffer[4096];
    char line[512]; size_t line_used = 0; int dropping = 0;
    int helper = !output && !strcmp(arguments[0], "/bin/sh") && arguments[1] && !strcmp(arguments[1], UPDATE_HELPER);
    int error = 0;
    for (;;) {
        ssize_t length = read(descriptors[0], buffer, sizeof(buffer));
        if (length < 0 && errno == EINTR) continue;
        if (length < 0) { error = -errno; break; }
        if (!length) break;
        if (output) {
            if ((size_t)length >= capacity - used) error = -EOVERFLOW;
            else { memcpy(output + used, buffer, (size_t)length); used += (size_t)length; }
        } else {
            if (!helper) (void)fwrite(buffer, 1, (size_t)length, stderr);
            if (run->log && fwrite(buffer, 1, (size_t)length, run->log) != (size_t)length) error = -EIO;
            if (helper) for (ssize_t i = 0; i < length; ++i) {
                if (buffer[i] == '\n') {
                    if (!dropping) {
                        if (line_used && line[line_used - 1] == '\r') --line_used;
                        line[line_used] = 0;
                        int decoded = helper_line(run, line);
                        if (decoded && !error) error = decoded;
                    } else if (dropping == 1) {
                        (void)fwrite(line, 1, line_used, stderr);
                        fputc('\n', stderr);
                    }
                    line_used = 0; dropping = 0;
                } else if (dropping != 2) {
                    if (line_used + 1 == sizeof(line)) {
                        if (!dropping && !memcmp(line, "activity\t", 9)) {
                            if (!error) error = -EOVERFLOW;
                            dropping = 2;
                        } else {
                            (void)fwrite(line, 1, line_used, stderr);
                            line_used = 0; dropping = 1;
                        }
                    }
                    if (dropping != 2) line[line_used++] = buffer[i];
                }
            }
        }
    }
    close(descriptors[0]);
    if (helper && line_used && dropping != 2) {
        if (!dropping && !memcmp(line, "activity\t", line_used < 9 ? line_used : 9)) {
            if (!error) error = -EPROTO;
        } else (void)fwrite(line, 1, line_used, stderr);
    }
    if (output) output[used] = 0;
    if (run->log && fflush(run->log) && !error) error = -errno;
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return -errno;
    }
    if (error) return error;
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        if (helper && run->activity.step && run->activity.action != UPDATE_ACTION_FAILED) {
            struct installer_update_activity failed = run->activity;
            failed.action = UPDATE_ACTION_FAILED;
            failed.status = WIFEXITED(status) ? (unsigned)WEXITSTATUS(status) : 128u + (unsigned)WTERMSIG(status);
            present_activity(run, &failed);
        }
        fprintf(stderr, "[installer-update] command %s wait_status=%d\n", arguments[0], status);
        return -EPROTO;
    }
    return 0;
}

static int hash_file(struct update_run *run, const char *path, char digest[65])
{
    const char *const arguments[] = {"/bin/busybox", "sha256sum", path, NULL};
    char output[PATH_MAX + 80];
    int result = command(run, arguments, output, sizeof(output));
    if (result) return result;
    if (strlen(output) < 66 || output[64] != ' ' || strspn(output, "0123456789abcdef") != 64) return -EPROTO;
    memcpy(digest, output, 64);
    digest[64] = 0;
    return 0;
}

static int sync_root(const char *root)
{
    int fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -errno;
    int result = syncfs(fd) < 0 ? -errno : 0;
    if (close(fd) < 0 && !result) result = -errno;
    return result;
}

static int package_state(struct update_run *run, const char *root, struct installer_update_info *info)
{
    char path[PATH_MAX];
    int length = snprintf(path, sizeof(path), "%s/lib/apk/db/installed", root);
    if (length < 0 || (size_t)length >= sizeof(path)) return -ENAMETOOLONG;
    int result = hash_file(run, path, info->database_sha256);
    if (result) return result;
    length = snprintf(path, sizeof(path), "%s/etc/apk/world", root);
    if (length < 0 || (size_t)length >= sizeof(path)) return -ENAMETOOLONG;
    return hash_file(run, path, info->world_sha256);
}

static int repository_bytes(const char *repository, uint64_t *bytes)
{
    DIR *directory = opendir(repository);
    if (!directory) return -errno;
    struct dirent *entry;
    int result = 0;
    *bytes = 0;
    errno = 0;
    while ((entry = readdir(directory))) {
        size_t length = strlen(entry->d_name);
        if (length < 4 || strcmp(entry->d_name + length - 4, ".apk")) continue;
        struct stat status;
        if (fstatat(dirfd(directory), entry->d_name, &status, AT_SYMLINK_NOFOLLOW) < 0) { result = -errno; break; }
        if (!S_ISREG(status.st_mode) || status.st_size < 0) { result = -EINVAL; break; }
        if (UINT64_MAX - *bytes < (uint64_t)status.st_size) { result = -EOVERFLOW; break; }
        *bytes += (uint64_t)status.st_size;
        errno = 0;
    }
    if (!result && errno) result = -errno;
    if (closedir(directory) < 0 && !result) result = -errno;
    return result;
}

static int check_media(struct update_run *run, struct installer_update_target *target,
                       char repository[PATH_MAX])
{
    struct installer_update_info source = {0};
    struct installer_update_packages installed, incoming;
    int result = installer_update_read_packages(target->root, &installed);
    if (result) return result;
    const char *media = !strcmp(target->info.profile, "console") ? UPDATE_MEDIA_CONSOLE :
        installed.has_xfce_session ? UPDATE_MEDIA_XFCE : UPDATE_MEDIA_XORG;
    result = installer_update_read_media(media, target->info.profile, &source);
    if (!result) result = installer_update_read_packages(media, &incoming);
    if (result) return result;
    if (!incoming.kernel_version[0]) return -ENOENT;
    strcpy(target->info.media_version, source.installed_version);
    strcpy(target->info.installed_kernel, installed.kernel_version);
    strcpy(target->info.media_kernel, incoming.kernel_version);
    const char *old_version = installed.kernel_version[0] ? installed.kernel_version : target->info.installed_version;
    const char *new_version = installed.kernel_version[0] ? incoming.kernel_version : source.installed_version;
    const char *const compare[] = {"/sbin/apk", "version", "-t", old_version, new_version, NULL};
    char relation[16];
    result = command(run, compare, relation, sizeof(relation));
    if (result) return result;
    if (!strcmp(relation, ">\n")) {
        report(run, run->progress, N_("Installation media is older than the installed system"));
        return -EPERM;
    }
    if (strcmp(relation, "<\n") && strcmp(relation, "=\n")) return -EPROTO;
    int length = snprintf(repository, PATH_MAX, "%s%s", media, UPDATE_PACKAGES);
    if (length < 0 || length >= PATH_MAX) return -ENAMETOOLONG;
    char path[PATH_MAX];
    length = snprintf(path, sizeof(path), "%s/packages.adb", repository);
    if (length < 0 || (size_t)length >= sizeof(path)) return -ENAMETOOLONG;
    result = hash_file(run, path, target->info.index_sha256);
    if (result) return result;
    result = package_state(run, target->root, &target->info);
    if (result) return result;
    struct statvfs space;
    if (statvfs(target->root, &space) < 0) return -errno;
    uint64_t unit = space.f_frsize ? space.f_frsize : space.f_bsize;
    if (!unit || space.f_bavail > UINT64_MAX / unit) return -EOVERFLOW;
    target->info.free_bytes = (uint64_t)space.f_bavail * unit;
    uint64_t archives;
    result = repository_bytes(repository, &archives);
    if (result) return result;
    uint64_t growth, largest;
    result = installer_update_measure_growth(target->root, media, &growth, &largest);
    if (result) return result;
    uint64_t reserve = 64u * 1024u * 1024u;
    if (UINT64_MAX - reserve < archives || UINT64_MAX - reserve - archives < growth ||
        UINT64_MAX - reserve - archives - growth < largest) return -EOVERFLOW;
    target->info.required_bytes = reserve + archives + growth + largest;
    return 0;
}

static int save_checkpoint(struct installer_update_target *target, struct installer_update_checkpoint *state,
                           const char *phase)
{
    snprintf(state->phase, sizeof(state->phase), "%s", phase);
    strcpy(state->root_uuid, target->info.root_uuid);
    strcpy(state->index_sha256, target->info.index_sha256);
    strcpy(state->database_sha256, target->info.database_sha256);
    strcpy(state->world_sha256, target->info.world_sha256);
    return installer_update_write_checkpoint(target->root, state);
}

static int recover_pending(struct update_run *run, struct installer_update_target *target,
                            const struct installer_update_checkpoint *checkpoint)
{
    if (!checkpoint->phase[0] || !strcmp(checkpoint->phase, "done")) return 0;
    report(run, 32, N_("Recovering the previous update attempt"));
    const char *const restore[] = {"/usr/libexec/reliefos/kernel-bundle", "--restore-all", target->root, NULL};
    int result = command(run, restore, NULL, 0);
    if (result) return result;
    char path[PATH_MAX], text[96];
    snprintf(path, sizeof(path), "%s/run/reliefos-kernel-activation.defer", target->root);
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -errno;
    struct stat status;
    if (fstat(fd, &status) < 0) { result = -errno; close(fd); return result; }
    if (!S_ISREG(status.st_mode) || status.st_uid || status.st_nlink != 1 || status.st_size >= (off_t)sizeof(text)) {
        close(fd);
        return -EINVAL;
    }
    size_t used = 0;
    while (used < sizeof(text) - 1) {
        ssize_t length = read(fd, text + used, sizeof(text) - 1 - used);
        if (length < 0 && errno == EINTR) continue;
        if (length < 0) { result = -errno; break; }
        if (!length) break;
        used += (size_t)length;
    }
    if (close(fd) < 0 && !result) result = -errno;
    if (result) return result;
    text[used] = 0;
    if (strcmp(text, "ReliefOS installer package transaction\n")) return -EBUSY;
    return unlink(path) < 0 ? -errno : 0;
}

enum update_phase {
    PHASE_IDENTIFY, PHASE_MEDIA, PHASE_BOOT_SOURCE, PHASE_BOOT_TARGET,
    PHASE_PLAN, PHASE_WRITABLE, PHASE_RECOVER, PHASE_PACKAGES,
    PHASE_ROOT_READY, PHASE_BOOT, PHASE_DONE, PHASE_CLEANUP,
};

struct update_transaction {
    struct installer_update_target target;
    struct installer_update_checkpoint checkpoint;
    struct installer_update_info expected;
    struct update_event event;
    char disk[64], root_uuid[37], esp_uuid[37], repository[PATH_MAX];
    int root_index, esp_index, use_indices, applying;
    unsigned modes;
    enum update_phase phase;
    struct update_run *coordinator;
};

static int boot_can_resume(const struct update_transaction *transaction);
static volatile sig_atomic_t update_cancelled;
static void request_cancel(int number) { (void)number; update_cancelled = 1; }

/* The stage's process group borrows the coordinator's mounts. All paths and
 * mount flags live in shared memory so a fault cannot erase cleanup state. */
static int transaction_task(void *context)
{
    struct update_transaction *transaction = context;
    struct installer_update_target *target = &transaction->target;
    struct update_run run = *transaction->coordinator;
    run.observer = NULL;
    run.out = &target->info;
    run.event = &transaction->event;
    /* Keep helper scratch below the owned workspace so a stage killed before
     * its shell trap runs cannot leak planning databases into later checks. */
    run.workspace = target->workspace;
    struct reliefos_boot_install_options boot = {
        .source = UPDATE_ESP, .boot_dir = target->boot, .disk_path = transaction->disk,
        .install_bios = ((transaction->applying ? transaction->modes : target->info.boot_modes) & RELIEFOS_BOOT_BIOS) != 0,
        .install_uefi = ((transaction->applying ? transaction->modes : target->info.boot_modes) & RELIEFOS_BOOT_UEFI) != 0,
        .preserve_rollback = 1, .root_partuuid = target->info.root_uuid,
    };
    /* flock belongs to the shared open file description. Helpers inherit it
     * across exec so a dead coordinator cannot unlock a surviving writer. */
    if (target->lock_fd >= 0 && fcntl(target->lock_fd, F_SETFD, 0) < 0) return -errno;
    int result = 0;
    switch (transaction->phase) {
    case PHASE_IDENTIFY:
        if (transaction->use_indices) {
            if (transaction->root_index >= 0)
                result = reliefos_block_partition_uuid(transaction->disk,
                    transaction->root_index, transaction->root_uuid);
            if (!result && transaction->esp_index >= 0)
                result = reliefos_block_partition_uuid(transaction->disk,
                    transaction->esp_index, transaction->esp_uuid);
        }
        if (!result) result = installer_update_target_open(target, transaction->disk,
            transaction->root_uuid, transaction->esp_uuid);
        if (!result && reliefos_account_legacy_check(target->root) < 0) result = -errno;
        break;
    case PHASE_MEDIA:
        result = check_media(&run, target, transaction->repository);
        if (!result) result = installer_update_read_checkpoint(target->root, &transaction->checkpoint);
        if (!result && transaction->checkpoint.phase[0] && strcmp(transaction->checkpoint.phase, "done")) {
            if (strcmp(transaction->checkpoint.root_uuid, target->info.root_uuid)) result = -EXDEV;
            else strcpy(target->info.pending, transaction->checkpoint.phase);
        }
        if (!result && !boot_can_resume(transaction) && target->info.free_bytes < target->info.required_bytes)
            result = -ENOSPC;
        if (!result && transaction->applying) {
            const struct installer_update_info *expected = &transaction->expected;
            if (strcmp(expected->disk, target->info.disk) || strcmp(expected->root_uuid, target->info.root_uuid) ||
                strcmp(expected->esp_uuid, target->info.esp_uuid) || strcmp(expected->index_sha256, target->info.index_sha256) ||
                strcmp(expected->database_sha256, target->info.database_sha256) ||
                strcmp(expected->world_sha256, target->info.world_sha256)) result = -ESTALE;
        }
        break;
    case PHASE_BOOT_SOURCE:
        result = reliefos_boot_validate_payload(&boot);
        break;
    case PHASE_BOOT_TARGET:
        if (target->namespaced) result = reliefos_boot_validate_instance(&boot, target->esp);
        if (!result) result = reliefos_boot_validate_target(&boot);
        break;
    case PHASE_PLAN: {
        const char *const arguments[] = {"/bin/sh", UPDATE_HELPER, "--check", target->root, transaction->repository, NULL};
        result = command(&run, arguments, NULL, 0);
        break;
    }
    case PHASE_WRITABLE:
        result = installer_update_target_writable(target);
        break;
    case PHASE_RECOVER:
        result = recover_pending(&run, target, &transaction->checkpoint);
        break;
    case PHASE_PACKAGES: {
        result = save_checkpoint(target, &transaction->checkpoint, "packages");
        const char *const arguments[] = {"/bin/sh", UPDATE_HELPER, "--apply", target->root,
            transaction->repository, target->info.index_sha256, NULL};
        if (!result) result = command(&run, arguments, NULL, 0);
        if (result) (void)save_checkpoint(target, &transaction->checkpoint, "packages-failed");
        break;
    }
    case PHASE_ROOT_READY:
        result = package_state(&run, target->root, &target->info);
        if (!result) result = sync_root(target->root);
        if (!result) result = save_checkpoint(target, &transaction->checkpoint, "root-ready");
        break;
    case PHASE_BOOT:
        result = save_checkpoint(target, &transaction->checkpoint, "boot");
        if (!result) result = target->namespaced
            ? reliefos_boot_install_instance_at_root(&boot, target->root, target->esp)
            : reliefos_boot_install_at_root(&boot, target->root);
        if (result) (void)save_checkpoint(target, &transaction->checkpoint, "boot-failed");
        break;
    case PHASE_DONE:
        result = sync_root(target->root);
        if (!result) result = save_checkpoint(target, &transaction->checkpoint, "done");
        if (!result) target->info.pending[0] = 0;
        break;
    case PHASE_CLEANUP:
        result = installer_update_target_unmount(target);
        break;
    }
    return result;
}

static void transaction_pulse(void *context, int stopping_reason)
{
    struct update_transaction *transaction = context;
    struct update_run *run = transaction->coordinator;
    struct update_event *event = &transaction->event;
    unsigned sequence = __atomic_load_n(&event->sequence, __ATOMIC_ACQUIRE);
    if (!(sequence & 1u)) {
        unsigned progress = event->progress;
        struct installer_update_activity activity = event->activity;
        char message[sizeof(event->message)];
        memcpy(message, event->message, sizeof(message));
        if (sequence == __atomic_load_n(&event->sequence, __ATOMIC_ACQUIRE) && sequence && message[0]) {
            run->activity = activity;
            run->out->activity = activity;
            if (progress > run->progress) run->progress = progress;
            memcpy(run->message, message, sizeof(run->message));
            run->message[sizeof(run->message) - 1] = 0;
            run->stage = run->message;
        }
    }
    const char *message = stopping_reason == -ECANCELED ? N_("Stopping update check and cleaning up")
        : stopping_reason == -ETIMEDOUT ? N_("Update stage timed out; stopping its processes")
        : run->stage;
    if (run->observer) run->observer(run->context, run->progress, message);
}

static int run_phase(struct update_transaction *transaction, enum update_phase phase,
                      unsigned progress, const char *message, unsigned seconds, int cancelable)
{
    struct update_run *run = transaction->coordinator;
    transaction->phase = phase;
    memset(&transaction->event, 0, sizeof(transaction->event));
    report(run, progress, message);
    int result = installer_update_supervise(transaction_task, transaction, (uint64_t)seconds * 1000,
        cancelable ? &update_cancelled : NULL, transaction_pulse, transaction);
    transaction_pulse(transaction, 0);
    fprintf(stderr, "[installer-update] phase=%u result=%d\n", (unsigned)phase, result);
    if (run->log) {
        fprintf(run->log, "[installer-update] phase=%u result=%d\n", (unsigned)phase, result);
        fflush(run->log);
    }
    return result;
}

static int boot_can_resume(const struct update_transaction *transaction)
{
    const struct installer_update_checkpoint *state = &transaction->checkpoint;
    const struct installer_update_info *info = &transaction->target.info;
    return (!strcmp(state->phase, "root-ready") || !strcmp(state->phase, "boot") || !strcmp(state->phase, "boot-failed")) &&
        !strcmp(state->root_uuid, info->root_uuid) && !strcmp(state->index_sha256, info->index_sha256) &&
        !strcmp(state->database_sha256, info->database_sha256) && !strcmp(state->world_sha256, info->world_sha256);
}

/* One coordinator owns the update lock from discovery through unmount. Every
 * blocking stage has a separate process group and hard deadline. Inspection
 * never remounts writable and apply never reruns an independent --check. APK
 * performs its own signature/solver preflight immediately before committing. */
static int execute(const char *disk, const struct installer_update_info *expected, unsigned modes,
    struct installer_update_info *out, installer_update_observer observer, void *context,
    const char *root_uuid, const char *esp_uuid, int use_indices, int root_index, int esp_index)
{
    memset(out, 0, sizeof(*out));
    if (geteuid()) { out->error_number = EACCES; strcpy(out->failure, N_("Administrator privileges are required")); return -EACCES; }
    struct update_transaction *transaction = mmap(NULL, sizeof(*transaction), PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (transaction == MAP_FAILED) { out->error_number = errno; return -errno; }
    memset(transaction, 0, sizeof(*transaction));
    struct update_run run = {.observer = observer, .context = context, .stage = N_("Preparing update session"),
        .out = out, .applying = expected != NULL};
    transaction->coordinator = &run;
    transaction->target.lock_fd = -1;
    transaction->applying = expected != NULL;
    transaction->modes = modes;
    transaction->use_indices = use_indices;
    transaction->root_index = root_index; transaction->esp_index = esp_index;
    snprintf(transaction->disk, sizeof(transaction->disk), "%s", disk);
    if (root_uuid) snprintf(transaction->root_uuid, sizeof(transaction->root_uuid), "%s", root_uuid);
    if (esp_uuid) snprintf(transaction->esp_uuid, sizeof(transaction->esp_uuid), "%s", esp_uuid);
    if (expected) {
        transaction->expected = *expected;
        strcpy(transaction->root_uuid, expected->root_uuid);
        strcpy(transaction->esp_uuid, expected->esp_uuid);
    }
    int log_fd = open(UPDATE_LOG, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    struct stat log_status;
    int result = 0;
    if (log_fd < 0) result = -errno;
    else if (fstat(log_fd, &log_status) < 0) result = -errno;
    else if (!S_ISREG(log_status.st_mode) || log_status.st_uid || log_status.st_nlink != 1) result = -EINVAL;
    if (!result) {
        run.log = fdopen(log_fd, "a");
        if (!run.log) result = -errno;
    }
    if (result) {
        if (log_fd >= 0) close(log_fd);
        strcpy(out->failure, N_("Writing the update log")); out->error_number = -result;
        munmap(transaction, sizeof(*transaction)); return result;
    }
    update_cancelled = 0;
    struct sigaction action = {.sa_handler = request_cancel}, old_term, old_int, old_hup;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, &old_term); sigaction(SIGINT, &action, &old_int); sigaction(SIGHUP, &action, &old_hup);
    result = installer_update_target_lock(&transaction->target);
    if (result) goto finish;
#define PHASE(id, pct, text, limit) do { \
    result = run_phase(transaction, id, pct, text, limit, !transaction->applying); \
    if (result) goto finish; \
} while (0)
    PHASE(PHASE_IDENTIFY, 2, N_("Identifying the installed system"), 120);
    PHASE(PHASE_MEDIA, 10, N_("Checking installation media and free space"), 180);
    PHASE(PHASE_BOOT_SOURCE, 16, N_("Checking bootloader files on the installation media"), 300);
    PHASE(PHASE_BOOT_TARGET, 20, N_("Checking the installed bootloader layout"), 180);
    if (!transaction->applying) {
        if (!boot_can_resume(transaction)) PHASE(PHASE_PLAN, 24, N_("Verifying signed packages and dependencies"), 900);
        goto finish;
    }
    /* Cancellation cannot interrupt a commit. Deadlines still terminate failed
     * commands; the durable checkpoint then selects a repair or boot retry. */
    int resume_boot = boot_can_resume(transaction);
    PHASE(PHASE_WRITABLE, 30, N_("Preparing the installed system for update"), 120);
    PHASE(PHASE_RECOVER, 32, N_("Recovering the previous update attempt"), 300);
    if (!resume_boot) {
        PHASE(PHASE_PACKAGES, 38, N_("Applying signed system packages"), 3600);
        PHASE(PHASE_ROOT_READY, 78, N_("Synchronizing the updated root filesystem"), 300);
    }
    PHASE(PHASE_BOOT, 86, N_("Publishing kernel and bootloader files"), 600);
    PHASE(PHASE_DONE, 98, N_("Finishing the system update"), 300);
#undef PHASE
finish:
    *out = transaction->target.info;
    out->activity = run.activity;
    char failure[128];
    snprintf(failure, sizeof(failure), "%s", run.stage);
    /* Cleanup has its own deadline and cannot be cancelled by another click.
     * The parent retains its inherited file lock until all writers are reaped. */
    if (transaction->target.lock_fd >= 0) {
        int cleanup = run_phase(transaction, PHASE_CLEANUP, run.progress, N_("Closing the update target"), 120, 0);
        int unlock = installer_update_target_unlock(&transaction->target);
        if (!result) { result = cleanup ? cleanup : unlock; if (result) snprintf(failure, sizeof(failure), "%s", run.stage); }
        if (cleanup) fprintf(stderr, "[installer-update] cleanup failed: %s (%d)\n", strerror(-cleanup), cleanup);
    }
    if (result) {
        out->error_number = -result;
        snprintf(out->failure, sizeof(out->failure), "%s", result == -ECANCELED ? N_("Update check cancelled") : failure);
        fprintf(stderr, "[installer-update] %s: %s (%d)\n", out->failure, strerror(-result), result);
        fprintf(run.log, "[installer-update] %s: %s (%d)\n", out->failure, strerror(-result), result);
    } else report(&run, 100, transaction->applying ? N_("System update completed successfully") : N_("Update checks completed"));
    if (fclose(run.log) && !result) { result = -errno; out->error_number = -result; strcpy(out->failure, N_("Writing the update log")); }
    sigaction(SIGTERM, &old_term, NULL); sigaction(SIGINT, &old_int, NULL); sigaction(SIGHUP, &old_hup, NULL);
    munmap(transaction, sizeof(*transaction));
    return result;
}

int installer_update_check(const char *disk, struct installer_update_info *info,
    installer_update_observer observer, void *context)
{
    return installer_update_check_target(disk, NULL, NULL, info, observer, context);
}

int installer_update_check_target(const char *disk, const char *root_uuid, const char *esp_uuid,
    struct installer_update_info *info, installer_update_observer observer, void *context)
{
    if (!disk || !info || strlen(disk) >= sizeof(info->disk) ||
        (root_uuid && *root_uuid && strlen(root_uuid) != 36) || (esp_uuid && *esp_uuid && strlen(esp_uuid) != 36)) return -EINVAL;
    return execute(disk, NULL, 0, info, observer, context, root_uuid, esp_uuid, 0, -1, -1);
}

int installer_update_check_indices(const char *disk, int root_index, int esp_index,
    struct installer_update_info *info, installer_update_observer observer, void *context)
{
    if (!disk || !info || strlen(disk) >= sizeof(info->disk) || root_index < -1 || esp_index < -1) return -EINVAL;
    return execute(disk, NULL, 0, info, observer, context, NULL, NULL, 1, root_index, esp_index);
}

int installer_update_apply(const struct installer_update_info *expected, unsigned modes,
    struct installer_update_info *result, installer_update_observer observer, void *context)
{
    if (!expected || !result || !expected->root_uuid[0] || !expected->esp_uuid[0] ||
        !memchr(expected->disk, 0, sizeof(expected->disk)) ||
        strnlen(expected->root_uuid, sizeof(expected->root_uuid)) != 36 ||
        strnlen(expected->esp_uuid, sizeof(expected->esp_uuid)) != 36 ||
        strnlen(expected->index_sha256, sizeof(expected->index_sha256)) != 64 ||
        strnlen(expected->database_sha256, sizeof(expected->database_sha256)) != 64 ||
        strnlen(expected->world_sha256, sizeof(expected->world_sha256)) != 64 || modes > 3) {
        if (result) { memset(result, 0, sizeof(*result)); result->error_number = EINVAL; }
        return -EINVAL;
    }
    /* Copy before zeroing result: callers may reuse the inspected structure. */
    struct installer_update_info inspected = *expected;
    return execute(inspected.disk, &inspected, modes, result, observer, context, NULL, NULL, 0, -1, -1);
}


/* Telemetry is dispensable; final metadata is not. A stalled reader can never
 * hold the storage lock: writes are atomic bounded lines with a finite drain. */
static int worker_line(int final, const char *format, ...)
{
    char line[512];
    va_list arguments;
    va_start(arguments, format);
    int size = vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    if (size < 0 || (size_t)size >= sizeof(line)) return -EOVERFLOW;
    for (unsigned retries = 0; ; ++retries) {
        ssize_t count = write(STDOUT_FILENO, line, (size_t)size);
        if (count == size) return 0;
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && final && retries < 20) {
            struct pollfd descriptor = {.fd = STDOUT_FILENO, .events = POLLOUT};
            (void)poll(&descriptor, 1, 100);
            continue;
        }
        return count < 0 ? -errno : -EIO;
    }
}

static void worker_progress(void *context, unsigned progress, const char *message)
{
    (void)worker_line(0, "progress\t%u\t%s\n", progress, message);
    const struct installer_update_info *info = context;
    char activity[512];
    if (info && info->activity.step && !installer_update_activity_encode(&info->activity, activity, sizeof(activity)))
        (void)worker_line(0, "%s\n", activity);
}

static int worker_info(const struct installer_update_info *info)
{
    int result = 0;
#define FIELD(name) if (!result) result = worker_line(1, "info\t%s\t%s\n", #name, info->name)
    FIELD(disk); FIELD(root_device); FIELD(esp_device); FIELD(root_uuid); FIELD(esp_uuid);
    FIELD(boot_file);
    FIELD(installed_version); FIELD(media_version); FIELD(profile); FIELD(installed_kernel);
    FIELD(media_kernel); FIELD(index_sha256); FIELD(database_sha256); FIELD(world_sha256); FIELD(pending);
#undef FIELD
    if (!result) result = worker_line(1, "info\tfree_bytes\t%llu\ninfo\trequired_bytes\t%llu\ninfo\tboot_modes\t%u\n",
        (unsigned long long)info->free_bytes, (unsigned long long)info->required_bytes, info->boot_modes);
    return result;
}

static int worker_index(const char *text, int *value)
{
    char *end;
    errno = 0;
    long number = strtol(text, &end, 10);
    if (errno || !*text || *end || number < -1 || number > INT_MAX) return -EINVAL;
    *value = (int)number;
    return 0;
}

int installer_update_worker(int argc, char **argv)
{
    struct installer_update_info info = {0};
    int result = -EINVAL;
    setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
    signal(SIGPIPE, SIG_IGN);
    int flags = fcntl(STDOUT_FILENO, F_GETFL);
    if (flags < 0 || fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) return 1;
    if (argc == 4 && !strcmp(argv[2], "check")) {
        result = installer_update_check(argv[3], &info, worker_progress, &info);
    } else if (argc == 6 && !strcmp(argv[2], "check")) {
        result = installer_update_check_target(argv[3], argv[4], argv[5], &info, worker_progress, &info);
    } else if (argc == 6 && !strcmp(argv[2], "check-index")) {
        int root, esp;
        result = worker_index(argv[4], &root);
        if (!result) result = worker_index(argv[5], &esp);
        if (!result) result = installer_update_check_indices(argv[3], root, esp, &info, worker_progress, &info);
    } else if (argc == 10 && !strcmp(argv[2], "apply")) {
        struct installer_update_info expected = {0};
        if (strlen(argv[3]) < sizeof(expected.disk) && strlen(argv[4]) == 36 && strlen(argv[5]) == 36 &&
            strlen(argv[6]) == 64 && strlen(argv[7]) == 64 && strlen(argv[8]) == 64 &&
            strlen(argv[9]) == 1 && *argv[9] >= '0' && *argv[9] <= '3') {
            strcpy(expected.disk, argv[3]); strcpy(expected.root_uuid, argv[4]); strcpy(expected.esp_uuid, argv[5]);
            strcpy(expected.index_sha256, argv[6]); strcpy(expected.database_sha256, argv[7]);
            strcpy(expected.world_sha256, argv[8]);
            result = installer_update_apply(&expected, (unsigned)(*argv[9] - '0'), &info, worker_progress, &info);
        }
    }
    int output = worker_info(&info);
    if (result && !output) output = worker_line(1, "error\t%d\t%s\n", -result,
        info.failure[0] ? info.failure : N_("Update worker failed"));
    if (!output) output = worker_line(1, "result\t%d\t%s\n", result ? -result : 0,
        result ? N_("Update worker failed") : N_("Update session completed"));
    return result || output ? 1 : 0;
}
