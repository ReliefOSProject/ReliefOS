/* Run the real coordinator, stage supervisor, shared-memory publication and
 * command pipes against deterministic offline storage adapters. No host disk,
 * mount operation, installed APK executable or root privilege is needed. */
#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

static char fixture_target[PATH_MAX], fixture_media[PATH_MAX], fixture_log[PATH_MAX];
static int fixture_spawn(pid_t *, const char *, const posix_spawn_file_actions_t *,
                         const posix_spawnattr_t *, char *const [], char *const []);
static int fixture_syncfs(int);
static int fixture_statvfs(const char *, struct statvfs *);
static int fixture_fstat(int, struct stat *);
static uid_t fixture_geteuid(void);

#define UPDATE_MEDIA_CONSOLE fixture_media
#define UPDATE_MEDIA_XFCE fixture_media
#define UPDATE_MEDIA_XORG fixture_media
#define UPDATE_ESP fixture_media
#define UPDATE_LOG fixture_log
#define UPDATE_HELPER "/fixture/reliefos-apk-update"
#define posix_spawn fixture_spawn
#define syncfs fixture_syncfs
#define statvfs(...) fixture_statvfs(__VA_ARGS__)
#define fstat fixture_fstat
#define geteuid fixture_geteuid
#include "../../userland/apps/installer/installer_update_engine.c"
#undef posix_spawn
#undef syncfs
#undef statvfs
#undef fstat
#undef geteuid

enum fixture_call {
    CALL_LOCK, CALL_OPEN, CALL_LEGACY, CALL_PACKAGES_READ, CALL_MEDIA_READ,
    CALL_COMPARE, CALL_HASH, CALL_JOURNAL_READ, CALL_PAYLOAD, CALL_LAYOUT,
    CALL_PLAN, CALL_WRITABLE, CALL_RESTORE, CALL_JOURNAL_WRITE, CALL_APPLY,
    CALL_SYNC, CALL_BOOT, CALL_CLEANUP, CALL_UNLOCK,
};
struct fixture_event { enum fixture_call call; char phase[32]; };
struct fixture_state {
    struct fixture_event events[256];
    unsigned event_count, successes, sync_calls;
    int packages_committed, packages_error, boot_error, cleanup_error;
    int sync_fail_at, fault_payload, no_free_space;
    uint64_t growth, largest;
    struct installer_update_checkpoint journal;
};
static struct fixture_state *fixture;
static const char root_uuid[] = "11111111-2222-3333-4444-555555555555";
static const char esp_uuid[] = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";

static void record_phase(enum fixture_call call, const char *phase)
{
    unsigned index = fixture->event_count++;
    assert(index < sizeof(fixture->events) / sizeof(fixture->events[0]));
    fixture->events[index].call = call;
    snprintf(fixture->events[index].phase, sizeof(fixture->events[index].phase), "%s", phase ? phase : "");
}

static void record(enum fixture_call call) { record_phase(call, NULL); }

static unsigned count(enum fixture_call call)
{
    unsigned result = 0;
    for (unsigned i = 0; i < fixture->event_count; ++i) result += fixture->events[i].call == call;
    return result;
}

static unsigned position(enum fixture_call call, const char *phase)
{
    for (unsigned i = 0; i < fixture->event_count; ++i)
        if (fixture->events[i].call == call && (!phase || !strcmp(phase, fixture->events[i].phase))) return i;
    assert(!"required transaction operation was skipped");
    return 0;
}

static void digest(char output[65], char value) { memset(output, value, 64); output[64] = 0; }

static uid_t fixture_geteuid(void) { return 0; }

static int fixture_fstat(int fd, struct stat *status)
{
    int result = fstat(fd, status);
    if (!result) status->st_uid = 0;
    return result;
}

static int fixture_statvfs(const char *path, struct statvfs *space)
{
    assert(!strcmp(path, fixture_target));
    memset(space, 0, sizeof(*space));
    space->f_frsize = 4096;
    space->f_bavail = fixture->no_free_space ? 0 : 1024 * 1024;
    return 0;
}

static int fixture_syncfs(int fd)
{
    assert(fcntl(fd, F_GETFD) >= 0);
    record(CALL_SYNC);
    if (++fixture->sync_calls == (unsigned)fixture->sync_fail_at) { errno = EIO; return -1; }
    return 0;
}

/* Preserve the actual spawn file actions, pipe lifetime, wait status and
 * inherited process group. Only the external programs' behavior is replaced. */
static int fixture_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                         const posix_spawnattr_t *attributes, char *const arguments[], char *const environment[])
{
    char script[256] = "exit 0";
    if (!strcmp(path, "/sbin/apk")) {
        assert(!strcmp(arguments[1], "version") && !strcmp(arguments[2], "-t"));
        record(CALL_COMPARE);
        strcpy(script, "printf '<\\n'");
    } else if (!strcmp(path, "/bin/busybox")) {
        assert(!strcmp(arguments[1], "sha256sum"));
        record(CALL_HASH);
        char hash[65];
        char value = strstr(arguments[2], "/packages.adb") ? 'a' :
            strstr(arguments[2], "/lib/apk/db/installed") ? (fixture->packages_committed ? 'd' : 'b') :
            strstr(arguments[2], "/etc/apk/world") ? (fixture->packages_committed ? 'e' : 'c') : 0;
        assert(value);
        digest(hash, value);
        snprintf(script, sizeof(script), "printf '%s  fixture\\n'", hash);
    } else if (!strcmp(path, "/bin/sh")) {
        assert(!strcmp(arguments[1], UPDATE_HELPER));
        if (!strcmp(arguments[2], "--check")) record(CALL_PLAN);
        else {
            assert(!strcmp(arguments[2], "--apply"));
            record(CALL_APPLY);
            assert(!strcmp(fixture->journal.phase, "packages"));
            if (fixture->packages_error) strcpy(script, "exit 23");
            else fixture->packages_committed = 1;
        }
    } else {
        assert(!strcmp(path, "/usr/libexec/reliefos/kernel-bundle") && !strcmp(arguments[1], "--restore-all"));
        record(CALL_RESTORE);
    }
    char *const shell[] = {"sh", "-c", script, NULL};
    return posix_spawn(pid, "/bin/sh", actions, attributes, shell, environment);
}

int installer_update_target_lock(struct installer_update_target *target)
{
    record(CALL_LOCK);
    memset(target, 0, sizeof(*target));
    target->lock_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(target->lock_fd >= 0);
    return 0;
}

int installer_update_target_open(struct installer_update_target *target, const char *disk,
                                 const char *expected_root, const char *expected_esp)
{
    record(CALL_OPEN);
    assert(!strcmp(disk, "/dev/fixture"));
    assert(!expected_root || !*expected_root || !strcmp(expected_root, root_uuid));
    assert(!expected_esp || !*expected_esp || !strcmp(expected_esp, esp_uuid));
    assert(strlen(fixture_target) < sizeof(target->root));
    strcpy(target->root, fixture_target);
    strcpy(target->boot, fixture_target);
    strcpy(target->esp, fixture_target);
    strcpy(target->info.disk, disk);
    strcpy(target->info.root_uuid, root_uuid);
    strcpy(target->info.esp_uuid, esp_uuid);
    strcpy(target->info.profile, "console");
    strcpy(target->info.installed_version, "1.0");
    target->info.boot_modes = RELIEFOS_BOOT_BIOS | RELIEFOS_BOOT_UEFI;
    return 0;
}

int installer_update_target_writable(struct installer_update_target *target)
{
    (void)target;
    record(CALL_WRITABLE);
    return 0;
}

int installer_update_target_unmount(struct installer_update_target *target)
{
    (void)target;
    record(CALL_CLEANUP);
    return fixture->cleanup_error;
}

int installer_update_target_unlock(struct installer_update_target *target)
{
    record(CALL_UNLOCK);
    assert(target->lock_fd >= 0 && !close(target->lock_fd));
    target->lock_fd = -1;
    return 0;
}

int reliefos_block_partition_uuid(const char *disk, uint32_t index, char uuid[37])
{
    assert(!strcmp(disk, "/dev/fixture") && index < 2);
    strcpy(uuid, index ? esp_uuid : root_uuid);
    return 0;
}

int reliefos_account_legacy_check(const char *target)
{
    assert(!strcmp(target, fixture_target));
    record(CALL_LEGACY);
    return 0;
}

int installer_update_read_media(const char *root, const char *profile, struct installer_update_info *info)
{
    assert(!strcmp(root, fixture_media) && !strcmp(profile, "console"));
    record(CALL_MEDIA_READ);
    memset(info, 0, sizeof(*info));
    strcpy(info->installed_version, "2.0");
    return 0;
}

int installer_update_read_packages(const char *root, struct installer_update_packages *packages)
{
    assert(!strcmp(root, fixture_target) || !strcmp(root, fixture_media));
    record(CALL_PACKAGES_READ);
    memset(packages, 0, sizeof(*packages));
    strcpy(packages->kernel_version, !strcmp(root, fixture_media) || fixture->packages_committed ? "2.0" : "1.0");
    packages->installed_bytes = !strcmp(root, fixture_media) ? 2048 : 1024;
    packages->largest_package = 512;
    return 0;
}

int installer_update_measure_growth(const char *installed_root, const char *media_root,
                                    uint64_t *growth, uint64_t *largest)
{
    assert(!strcmp(installed_root, fixture_target) && !strcmp(media_root, fixture_media));
    *growth = fixture->growth;
    *largest = fixture->largest;
    return 0;
}

int installer_update_read_checkpoint(const char *root, struct installer_update_checkpoint *checkpoint)
{
    assert(!strcmp(root, fixture_target));
    record(CALL_JOURNAL_READ);
    *checkpoint = fixture->journal;
    return 0;
}

int installer_update_write_checkpoint(const char *root, const struct installer_update_checkpoint *checkpoint)
{
    assert(!strcmp(root, fixture_target));
    record_phase(CALL_JOURNAL_WRITE, checkpoint->phase);
    fixture->journal = *checkpoint;
    return 0;
}

int reliefos_boot_validate_payload(const struct reliefos_boot_install_options *options)
{
    assert(options->source == fixture_media);
    record(CALL_PAYLOAD);
    if (fixture->fault_payload) { signal(SIGBUS, SIG_DFL); raise(SIGBUS); _exit(125); }
    return 0;
}

int reliefos_boot_validate_target(const struct reliefos_boot_install_options *options)
{
    assert(!strcmp(options->boot_dir, fixture_target));
    record(CALL_LAYOUT);
    return 0;
}

int reliefos_boot_validate_instance(const struct reliefos_boot_install_options *options, const char *esp)
{
    (void)options; (void)esp;
    assert(!"fixture uses a conventional ESP layout");
    return -EINVAL;
}

int reliefos_boot_install_at_root(const struct reliefos_boot_install_options *options, const char *root)
{
    assert(!strcmp(root, fixture_target) && !strcmp(options->root_partuuid, root_uuid));
    assert(!strcmp(fixture->journal.phase, "boot"));
    record(CALL_BOOT);
    return fixture->boot_error;
}

int reliefos_boot_install_instance_at_root(const struct reliefos_boot_install_options *options,
                                         const char *root, const char *esp)
{
    (void)options; (void)root; (void)esp;
    assert(!"fixture uses a conventional ESP layout");
    return -EINVAL;
}

static void observe(void *context, unsigned progress, const char *message)
{
    (void)context;
    assert(message && *message && progress <= 100);
    if (progress == 100) ++fixture->successes;
}

static void clear_observations(void)
{
    fixture->event_count = fixture->successes = fixture->sync_calls = 0;
    memset(fixture->events, 0, sizeof(fixture->events));
}

static void fresh(void)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->growth = 1024;
    fixture->largest = 512;
}

static struct installer_update_info checked(void)
{
    struct installer_update_info info;
    assert(!installer_update_check("/dev/fixture", &info, observe, NULL));
    assert(!count(CALL_WRITABLE) && !count(CALL_JOURNAL_WRITE) && !count(CALL_APPLY) && !count(CALL_BOOT) && !count(CALL_SYNC));
    assert(count(CALL_CLEANUP) == 1 && count(CALL_UNLOCK) == 1 && fixture->successes == 1);
    return info;
}

static void assert_closed(void)
{
    assert(count(CALL_CLEANUP) == 1 && count(CALL_UNLOCK) == 1);
    assert(position(CALL_CLEANUP, NULL) < position(CALL_UNLOCK, NULL));
}

static void test_success(void)
{
    fresh();
    struct installer_update_info expected = checked(), result;
    assert(count(CALL_PLAN) == 1);
    clear_observations();
    assert(!installer_update_apply(&expected, 3, &result, observe, NULL));
    assert(!count(CALL_PLAN) && count(CALL_APPLY) == 1 && count(CALL_BOOT) == 1 && count(CALL_SYNC) == 2);
    assert(fixture->successes == 1 && !result.pending[0] && !strcmp(fixture->journal.phase, "done"));
    assert(position(CALL_WRITABLE, NULL) < position(CALL_JOURNAL_WRITE, "packages"));
    assert(position(CALL_JOURNAL_WRITE, "packages") < position(CALL_APPLY, NULL));
    assert(position(CALL_APPLY, NULL) < position(CALL_SYNC, NULL));
    assert(position(CALL_SYNC, NULL) < position(CALL_JOURNAL_WRITE, "root-ready"));
    assert(position(CALL_JOURNAL_WRITE, "root-ready") < position(CALL_JOURNAL_WRITE, "boot"));
    assert(position(CALL_JOURNAL_WRITE, "boot") < position(CALL_BOOT, NULL));
    assert(position(CALL_BOOT, NULL) < position(CALL_JOURNAL_WRITE, "done"));
    assert(position(CALL_JOURNAL_WRITE, "done") < position(CALL_CLEANUP, NULL));
    assert_closed();
}

static void test_stale(void)
{
    fresh();
    struct installer_update_info expected = checked(), result;
    clear_observations();
    expected.database_sha256[0] = 'f';
    assert(installer_update_apply(&expected, 3, &result, observe, NULL) == -ESTALE);
    assert(!count(CALL_WRITABLE) && !count(CALL_APPLY) && !count(CALL_BOOT) && !count(CALL_JOURNAL_WRITE));
    assert(!fixture->successes && result.error_number == ESTALE);
    assert_closed();
}

static void test_insufficient_space(void)
{
    fresh();
    struct installer_update_info expected = checked(), result;
    clear_observations(); fixture->no_free_space = 1;
    assert(installer_update_apply(&expected, 3, &result, observe, NULL) == -ENOSPC);
    assert(!count(CALL_WRITABLE) && !count(CALL_APPLY) && !count(CALL_BOOT) && !count(CALL_JOURNAL_WRITE));
    assert(!fixture->successes && result.error_number == ENOSPC);
    assert_closed();
    clear_observations();
    assert(installer_update_check("/dev/fixture", &result, observe, NULL) == -ENOSPC);
    assert(!count(CALL_PLAN) && !count(CALL_PAYLOAD) && !count(CALL_WRITABLE));
    assert(!fixture->successes);
    assert_closed();
}

static void test_sync_failure(void)
{
    fresh();
    struct installer_update_info expected = checked(), result;
    clear_observations(); fixture->sync_fail_at = 1;
    assert(installer_update_apply(&expected, 3, &result, observe, NULL) == -EIO);
    assert(count(CALL_APPLY) == 1 && count(CALL_SYNC) == 1 && !count(CALL_BOOT));
    assert(!strcmp(fixture->journal.phase, "packages") && !fixture->successes);
    assert_closed();
}

static void test_package_failure(void)
{
    fresh();
    struct installer_update_info expected = checked(), result;
    clear_observations(); fixture->packages_error = 1;
    assert(installer_update_apply(&expected, 3, &result, observe, NULL) == -EPROTO);
    assert(count(CALL_APPLY) == 1 && !count(CALL_BOOT) && !count(CALL_SYNC));
    assert(!strcmp(fixture->journal.phase, "packages-failed") && !fixture->successes);
    assert_closed();
}

static void test_boot_failure_and_resume(void)
{
    fresh();
    struct installer_update_info expected = checked(), result;
    clear_observations(); fixture->boot_error = -EIO;
    assert(installer_update_apply(&expected, 3, &result, observe, NULL) == -EIO);
    assert(count(CALL_APPLY) == 1 && count(CALL_BOOT) == 1 && fixture->packages_committed);
    assert(!strcmp(fixture->journal.phase, "boot-failed") && !fixture->successes);
    assert_closed();
    clear_observations(); fixture->boot_error = 0;
    expected = checked();
    assert(!strcmp(expected.pending, "boot-failed") && !count(CALL_PLAN));
    clear_observations();
    assert(!installer_update_apply(&expected, 3, &result, observe, NULL));
    assert(!count(CALL_APPLY) && !count(CALL_PLAN) && count(CALL_BOOT) == 1 && count(CALL_RESTORE) == 1);
    assert(!strcmp(fixture->journal.phase, "done") && fixture->successes == 1);
    assert_closed();
}

static void test_root_ready_resume(void)
{
    fresh();
    fixture->packages_committed = 1;
    strcpy(fixture->journal.phase, "root-ready");
    strcpy(fixture->journal.root_uuid, root_uuid);
    digest(fixture->journal.index_sha256, 'a');
    digest(fixture->journal.database_sha256, 'd');
    digest(fixture->journal.world_sha256, 'e');
    fixture->no_free_space = 1;
    struct installer_update_info expected = checked(), result;
    assert(!strcmp(expected.pending, "root-ready") && !count(CALL_PLAN));
    assert(expected.free_bytes < expected.required_bytes);
    clear_observations();
    assert(!installer_update_apply(&expected, 3, &result, observe, NULL));
    assert(!count(CALL_APPLY) && !count(CALL_PLAN) && count(CALL_BOOT) == 1 && count(CALL_RESTORE) == 1);
    assert(!strcmp(fixture->journal.phase, "done"));
    assert_closed();
}

static void test_cleanup_failure(void)
{
    fresh();
    struct installer_update_info expected = checked(), result;
    clear_observations(); fixture->cleanup_error = -EBUSY;
    assert(installer_update_apply(&expected, 3, &result, observe, NULL) == -EBUSY);
    assert(count(CALL_BOOT) == 1 && !strcmp(fixture->journal.phase, "done"));
    assert(!fixture->successes && result.error_number == EBUSY);
    assert_closed();
}

static void test_stage_fault(void)
{
    fresh(); fixture->fault_payload = 1;
    struct installer_update_info result;
    assert(installer_update_check("/dev/fixture", &result, observe, NULL) == -EIO);
    assert(count(CALL_PAYLOAD) == 1 && !count(CALL_LAYOUT) && !count(CALL_PLAN));
    assert(!count(CALL_WRITABLE) && !count(CALL_JOURNAL_WRITE) && !fixture->successes);
    assert(result.error_number == EIO);
    assert_closed();
}

int main(int argc, char **argv)
{
    assert(argc == 2 && strlen(argv[1]) < 180);
    snprintf(fixture_target, sizeof(fixture_target), "%s/target", argv[1]);
    snprintf(fixture_media, sizeof(fixture_media), "%s/media", argv[1]);
    snprintf(fixture_log, sizeof(fixture_log), "%s/update.log", argv[1]);
    fixture = mmap(NULL, sizeof(*fixture), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    assert(fixture != MAP_FAILED);
    test_success();
    test_stale();
    test_insufficient_space();
    test_sync_failure();
    test_package_failure();
    test_boot_failure_and_resume();
    test_root_ready_resume();
    test_cleanup_failure();
    test_stage_fault();
    assert(!munmap(fixture, sizeof(*fixture)));
    puts("PASS real update engine ordering, stale snapshots, durable retry phases, failures and SIGBUS cleanup");
    return 0;
}
