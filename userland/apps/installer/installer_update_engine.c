#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "installer_update_internal.h"
#include "../../auth/standard_accounts.h"
#include <reliefos/bootloader.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define N_(text) (text)

#define UPDATE_ESP "/install/esp"
#define UPDATE_PACKAGES "/usr/share/reliefos/apk/repository"
#define UPDATE_HELPER "/usr/lib/reliefos/reliefos-apk-update"
#define UPDATE_LOG "/var/log/installer-update.log"

struct update_run {
    installer_update_observer observer;
    void *context;
    FILE *log;
    unsigned progress;
    const char *stage;
    struct timespec presented;
};

static void report(struct update_run *run, unsigned progress, const char *stage)
{
    run->progress = progress;
    run->stage = stage;
    fprintf(stderr, "[installer-update] %u%% %s\n", progress, stage);
    if (run->log) { fprintf(run->log, "[installer-update] %u%% %s\n", progress, stage); fflush(run->log); }
    if (run->observer) run->observer(run->context, progress, stage);
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
    char *const environment[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL=C", "LANG=C", "HOME=/root", NULL};
    pid_t child = -1;
    if (!result) result = posix_spawn(&child, arguments[0], &actions, NULL,
                                     (char *const *)arguments, environment);
    posix_spawn_file_actions_destroy(&actions);
    close(descriptors[1]);
    if (result) { close(descriptors[0]); return -result; }
    size_t used = 0;
    char buffer[4096];
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
            (void)fwrite(buffer, 1, (size_t)length, stderr);
            if (run->log && fwrite(buffer, 1, (size_t)length, run->log) != (size_t)length) error = -EIO;
        }
    }
    close(descriptors[0]);
    if (output) output[used] = 0;
    if (run->log && fflush(run->log) && !error) error = -errno;
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return -errno;
    }
    if (error) return error;
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
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
                       const char *media, char repository[PATH_MAX])
{
    struct installer_update_info source = {0};
    struct installer_update_packages installed, incoming;
    int result = installer_update_read_media(media, target->info.profile, &source);
    if (!result) result = installer_update_read_packages(target->root, &installed);
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
    uint64_t growth = incoming.installed_bytes > installed.installed_bytes
        ? incoming.installed_bytes - installed.installed_bytes : 0;
    uint64_t reserve = 64u * 1024u * 1024u;
    if (UINT64_MAX - reserve < archives || UINT64_MAX - reserve - archives < growth ||
        UINT64_MAX - reserve - archives - growth < incoming.largest_package) return -EOVERFLOW;
    target->info.required_bytes = reserve + archives + growth + incoming.largest_package;
    return target->info.free_bytes < target->info.required_bytes ? -ENOSPC : 0;
}

static void boot_progress(void *context)
{
    struct update_run *run = context;
    struct timespec now;
    if (!clock_gettime(CLOCK_MONOTONIC, &now) &&
        (now.tv_sec > run->presented.tv_sec || now.tv_nsec - run->presented.tv_nsec >= 500000000L)) {
        run->presented = now;
        if (run->observer) run->observer(run->context, run->progress, run->stage);
    }
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
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
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

static int execute(const char *disk, const struct installer_update_info *expected, unsigned modes,
                    struct installer_update_info *out, installer_update_observer observer, void *context,
                    const char *root_uuid, const char *esp_uuid)
{
    struct installer_update_target target;
    struct installer_update_checkpoint checkpoint = {0};
    struct update_run run = {.observer = observer, .context = context, .stage = N_("Checking update target")};
    int applying = expected != NULL;
    memset(out, 0, sizeof(*out));
    if (geteuid()) { out->error_number = EACCES; strcpy(out->failure, N_("Administrator privileges are required")); return -EACCES; }
    int log_fd = open(UPDATE_LOG, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (log_fd >= 0) {
        run.log = fdopen(log_fd, "a");
        if (!run.log) { int saved = errno; close(log_fd); errno = saved; }
    }
    if (!run.log) {
        out->error_number = errno;
        strcpy(out->failure, N_("Writing the update log"));
        return -out->error_number;
    }
    report(&run, 2, N_("Identifying the installed system"));
    int result = installer_update_target_open(&target, disk, expected ? expected->root_uuid : root_uuid,
                                              expected ? expected->esp_uuid : esp_uuid);
    if (result) goto done;
    if (reliefos_account_legacy_check(target.root) < 0) { result = -errno; goto done; }
    const char *media = !strcmp(target.info.profile, "console") ? "/install/root-console" : "/install/root";
    char repository[PATH_MAX];
    report(&run, 10, N_("Checking installation media and free space"));
    result = check_media(&run, &target, media, repository);
    if (result) goto done;
    result = installer_update_read_checkpoint(target.root, &checkpoint);
    if (result) goto done;
    if (checkpoint.phase[0] && strcmp(checkpoint.phase, "done")) {
        if (strcmp(checkpoint.root_uuid, target.info.root_uuid)) { result = -EXDEV; goto done; }
        strcpy(target.info.pending, checkpoint.phase);
    }
    if (expected && (strcmp(expected->disk, target.info.disk) || strcmp(expected->esp_uuid, target.info.esp_uuid) ||
        strcmp(expected->root_uuid, target.info.root_uuid) || strcmp(expected->index_sha256, target.info.index_sha256) ||
        strcmp(expected->database_sha256, target.info.database_sha256) ||
        strcmp(expected->world_sha256, target.info.world_sha256))) {
        report(&run, run.progress, N_("Target or media changed. Check the update again."));
        result = -ESTALE;
        goto done;
    }
    struct reliefos_boot_install_options boot = {
        .source = UPDATE_ESP, .boot_dir = target.boot, .disk_path = disk,
        .install_bios = ((applying ? modes : target.info.boot_modes) & RELIEFOS_BOOT_BIOS) != 0,
        .install_uefi = ((applying ? modes : target.info.boot_modes) & RELIEFOS_BOOT_UEFI) != 0,
        .preserve_rollback = 1, .root_partuuid = target.info.root_uuid,
        .progress = boot_progress, .progress_context = &run,
    };
    report(&run, 16, N_("Checking bootloader compatibility"));
    result = target.namespaced ? reliefos_boot_validate_instance(&boot, target.esp) : 0;
    if (!result) result = reliefos_boot_validate_payload(&boot);
    if (!result) result = reliefos_boot_validate_target(&boot);
    if (result) goto done;
    report(&run, 22, N_("Verifying signed packages and dependencies"));
    const char *const check[] = {"/bin/sh", UPDATE_HELPER, "--check", target.root, repository, NULL};
    result = command(&run, check, NULL, 0);
    if (result) goto done;
    if (!applying) { report(&run, 100, N_("Update checks completed")); goto done; }

    int resume_boot = (!strcmp(checkpoint.phase, "root-ready") || !strcmp(checkpoint.phase, "boot") ||
                       !strcmp(checkpoint.phase, "boot-failed")) &&
        !strcmp(checkpoint.root_uuid, target.info.root_uuid) &&
        !strcmp(checkpoint.index_sha256, target.info.index_sha256) &&
        !strcmp(checkpoint.database_sha256, target.info.database_sha256) &&
        !strcmp(checkpoint.world_sha256, target.info.world_sha256);
    report(&run, 30, N_("Preparing the installed system for update"));
    result = installer_update_target_writable(&target);
    if (result) goto done;
    result = recover_pending(&run, &target, &checkpoint);
    if (result) goto done;
    if (!resume_boot) {
        result = save_checkpoint(&target, &checkpoint, "packages");
        if (result) goto done;
        report(&run, 38, N_("Applying signed system packages"));
        const char *const apply[] = {"/bin/sh", UPDATE_HELPER, "--apply", target.root, repository, target.info.index_sha256, NULL};
        result = command(&run, apply, NULL, 0);
        if (result) { (void)save_checkpoint(&target, &checkpoint, "packages-failed"); goto done; }
        result = package_state(&run, target.root, &target.info);
        if (result) goto done;
        report(&run, 78, N_("Synchronizing the updated root filesystem"));
        result = sync_root(target.root);
        if (!result) result = save_checkpoint(&target, &checkpoint, "root-ready");
        if (result) goto done;
    }
    report(&run, 86, resume_boot ? N_("Resuming bootloader publication") : N_("Publishing kernel and bootloader files"));
    result = save_checkpoint(&target, &checkpoint, "boot");
    if (!result) result = target.namespaced
        ? reliefos_boot_install_instance_at_root(&boot, target.root, target.esp)
        : reliefos_boot_install_at_root(&boot, target.root);
    if (result) { (void)save_checkpoint(&target, &checkpoint, "boot-failed"); goto done; }
    result = sync_root(target.root);
    if (!result) result = save_checkpoint(&target, &checkpoint, "done");
    if (!result) report(&run, 98, N_("Finishing the system update"));
done:
    *out = target.info;
    int cleanup = installer_update_target_close(&target);
    if (!result) result = cleanup;
    if (result) {
        out->error_number = -result;
        snprintf(out->failure, sizeof(out->failure), "%s", run.stage);
        fprintf(stderr, "[installer-update] %s failed with %s (%d)\n", run.stage, strerror(-result), result);
        if (run.log) fprintf(run.log, "[installer-update] %s failed with %s (%d)\n", run.stage, strerror(-result), result);
    } else if (applying) report(&run, 100, N_("System update completed successfully"));
    if (run.log && fclose(run.log) && !result) {
        result = -errno;
        out->error_number = -result;
        strcpy(out->failure, N_("Writing the update log"));
    }
    return result;
}

int installer_update_check(const char *disk, struct installer_update_info *info,
                           installer_update_observer observer, void *context)
{
    if (!disk || !info) {
        if (info) { memset(info, 0, sizeof(*info)); info->error_number = EINVAL; }
        return -EINVAL;
    }
    return execute(disk, NULL, 0, info, observer, context, NULL, NULL);
}

int installer_update_check_target(const char *disk, const char *root_uuid, const char *esp_uuid,
                                 struct installer_update_info *info,
                                 installer_update_observer observer, void *context)
{
    if (!disk || !info || (root_uuid && *root_uuid && strlen(root_uuid) != 36) ||
        (esp_uuid && *esp_uuid && strlen(esp_uuid) != 36)) return -EINVAL;
    return execute(disk, NULL, 0, info, observer, context, root_uuid, esp_uuid);
}

int installer_update_apply(const struct installer_update_info *expected, unsigned modes,
                           struct installer_update_info *result,
                           installer_update_observer observer, void *context)
{
    if (!expected || !result || !expected->root_uuid[0] || !expected->esp_uuid[0] ||
        strlen(expected->index_sha256) != 64 || strlen(expected->database_sha256) != 64 ||
        strlen(expected->world_sha256) != 64 || modes > 3) {
        if (result) { memset(result, 0, sizeof(*result)); result->error_number = EINVAL; }
        return -EINVAL;
    }
    return execute(expected->disk, expected, modes, result, observer, context, NULL, NULL);
}

static void worker_progress(void *context, unsigned progress, const char *message)
{
    (void)context;
    printf("progress\t%u\t%s\n", progress, message);
    fflush(stdout);
}

static void worker_info(const struct installer_update_info *info)
{
#define FIELD(name) printf("info\t%s\t%s\n", #name, info->name)
    FIELD(disk); FIELD(root_device); FIELD(esp_device); FIELD(root_uuid); FIELD(esp_uuid);
    FIELD(boot_file);
    FIELD(installed_version); FIELD(media_version); FIELD(profile); FIELD(installed_kernel);
    FIELD(media_kernel); FIELD(index_sha256); FIELD(database_sha256); FIELD(world_sha256); FIELD(pending);
#undef FIELD
    printf("info\tfree_bytes\t%llu\ninfo\trequired_bytes\t%llu\ninfo\tboot_modes\t%u\n",
           (unsigned long long)info->free_bytes, (unsigned long long)info->required_bytes, info->boot_modes);
}

int installer_update_worker(int argc, char **argv)
{
    struct installer_update_info info = {0};
    int result;
    setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
    if (argc == 4 && !strcmp(argv[2], "check")) {
        result = installer_update_check(argv[3], &info, worker_progress, NULL);
    } else if (argc == 6 && !strcmp(argv[2], "check")) {
        result = installer_update_check_target(argv[3], argv[4], argv[5], &info, worker_progress, NULL);
    } else if (argc == 10 && !strcmp(argv[2], "apply")) {
        struct installer_update_info expected = {0};
        if (strlen(argv[3]) >= sizeof(expected.disk) || strlen(argv[4]) != 36 || strlen(argv[5]) != 36 ||
            strlen(argv[6]) != 64 || strlen(argv[7]) != 64 || strlen(argv[8]) != 64 ||
            strlen(argv[9]) != 1 || *argv[9] < '0' || *argv[9] > '3') return 2;
        strcpy(expected.disk, argv[3]); strcpy(expected.root_uuid, argv[4]); strcpy(expected.esp_uuid, argv[5]);
        strcpy(expected.index_sha256, argv[6]); strcpy(expected.database_sha256, argv[7]);
        strcpy(expected.world_sha256, argv[8]);
        result = installer_update_apply(&expected, (unsigned)(*argv[9] - '0'), &info, worker_progress, NULL);
    } else return 2;
    worker_info(&info);
    if (result) printf("error\t%d\t%s\n", -result, info.failure[0] ? info.failure : N_("Update worker failed"));
    fflush(stdout);
    return result ? 1 : 0;
}
