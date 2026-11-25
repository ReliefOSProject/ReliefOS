/* Exercise cleanup after an interrupted mount stage without requiring root or
 * mounting any host filesystem. The mount table, not publication flags, is the
 * independent observation of what the kernel accepted before a child died. */
#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <mntent.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static FILE *fixture_setmntent(const char *, const char *);
static struct mntent *fixture_getmntent(FILE *);
static int fixture_endmntent(FILE *);
static int fixture_umount2(const char *, int);
static int fixture_rmdir(const char *);
static int fixture_mkdir(const char *, mode_t);
static int fixture_lstat(const char *, struct stat *);
static int fixture_fstat(int, struct stat *);
static int fixture_open(const char *, int, ...);
#define setmntent fixture_setmntent
#define getmntent fixture_getmntent
#define endmntent fixture_endmntent
#define umount2 fixture_umount2
#define rmdir fixture_rmdir
#define mkdir fixture_mkdir
#define lstat fixture_lstat
#define fstat fixture_fstat
#define open fixture_open
#include "../../userland/apps/installer/installer_update_target.c"
#undef setmntent
#undef getmntent
#undef endmntent
#undef umount2
#undef rmdir
#undef mkdir
#undef lstat
#undef fstat
#undef open

static const char *const points[] = {
    "/run/reliefos/update-a1B2c3/root/boot", "/run/reliefos/update-a1B2c3/root/efi",
    "/run/reliefos/update-a1B2c3/root", "/run/reliefos/update-other1/root",
    "/run/reliefos/update-not-a-workspace/root", "/run/reliefos/update-a1B2c3-other/root",
    "/mnt/unrelated/root", "/run/reliefos/update-a1B2c3/root/proc",
    "/run/reliefos/update-a1B2c3",
    "/run/reliefos/update-a1B2c3/reliefos-update.aB3dE5/root/lib",
};
enum { POINTS = sizeof(points) / sizeof(points[0]) };
static int active[POINTS], cursor, fail_at = -1, table_error, calls, removes;
static char called[4][256];
static char lock_file[] = "/tmp/installer-update-lock.XXXXXX";
static char fixture_workspace[] = "/tmp/installer-update-workspace.XXXXXX";

/* Redirect only the runtime directory and lock pathname. Lock acquisition,
 * inheritance, contention and close all use real host kernel operations. */
static int fixture_mkdir(const char *path, mode_t mode)
{
    assert(!strcmp(path, "/run/reliefos") && mode == 0755);
    errno = EEXIST;
    return -1;
}

static int fixture_lstat(const char *path, struct stat *status)
{
    assert(!strcmp(path, "/run/reliefos"));
    memset(status, 0, sizeof(*status));
    status->st_mode = S_IFDIR | 0755;
    return 0;
}

static int fixture_fstat(int fd, struct stat *status)
{
    int result = fstat(fd, status);
    if (!result) status->st_uid = 0; /* Keep the test usable by an unprivileged builder. */
    return result;
}

static int fixture_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list arguments;
        va_start(arguments, flags);
        mode = va_arg(arguments, int);
        va_end(arguments);
    }
    if (!strcmp(path, "/run/reliefos/installer-update.lock")) path = lock_file;
    else if (!strcmp(path, "/run/reliefos/update-a1B2c3")) path = fixture_workspace;
    return open(path, flags, mode);
}

static FILE *fixture_setmntent(const char *path, const char *mode)
{
    assert(!strcmp(path, "/proc/self/mounts") && !strcmp(mode, "r"));
    if (table_error) { errno = EIO; return NULL; }
    cursor = 0;
    FILE *stream = tmpfile();
    assert(stream);
    return stream;
}

static struct mntent *fixture_getmntent(FILE *stream)
{
    (void)stream;
    static struct mntent entry;
    while (cursor < POINTS) {
        int index = cursor++;
        if (!active[index]) continue;
        entry.mnt_dir = (char *)points[index];
        return &entry;
    }
    return NULL;
}

static int fixture_endmntent(FILE *stream) { assert(!fclose(stream)); return 1; }

static int fixture_umount2(const char *path, int flags)
{
    assert(!flags && calls < 4);
    snprintf(called[calls++], sizeof(called[0]), "%s", path);
    for (unsigned i = 0; i < POINTS; ++i) {
        if (strcmp(path, points[i])) continue;
        assert(active[i]);
        if ((int)i == fail_at) { errno = EBUSY; return -1; }
        active[i] = 0;
        return 0;
    }
    assert(!"unmounting an unobserved path");
    return -1;
}

static int fixture_rmdir(const char *path)
{
    assert(!active[0] && !active[1] && !active[2]);
    assert(!strcmp(path, "/run/reliefos/update-a1B2c3/root") ||
           !strcmp(path, "/run/reliefos/update-a1B2c3"));
    ++removes;
    return 0;
}

static struct installer_update_target target(void)
{
    struct installer_update_target result = {.lock_fd = -1};
    strcpy(result.workspace, "/run/reliefos/update-a1B2c3");
    return result;
}

static void test_lock_inheritance(void)
{
    int fixture = mkstemp(lock_file);
    assert(fixture >= 0 && !close(fixture));
    memset(active, 0, sizeof(active));
    active[4] = active[5] = active[6] = 1;
    struct installer_update_target state, other;
    assert(!installer_update_target_lock(&state));
    assert(fcntl(state.lock_fd, F_GETFD) & FD_CLOEXEC);
    assert(installer_update_target_lock(&other) == -EBUSY && other.lock_fd == -1);

    int control[2], ready[2];
    assert(!pipe(control) && !pipe(ready));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        assert(!close(control[1]) && !close(ready[0]));
        assert(dup2(control[0], STDIN_FILENO) == STDIN_FILENO);
        assert(dup2(ready[1], STDOUT_FILENO) == STDOUT_FILENO);
        assert(!close(control[0]) && !close(ready[1]));
        /* The stage clears CLOEXEC before launching its command, exactly as
         * the coordinator does for real update descendants. */
        assert(!fcntl(state.lock_fd, F_SETFD, 0));
        execl("/bin/sh", "sh", "-c", "printf r; read line", (char *)NULL);
        _exit(127);
    }
    assert(!close(control[0]) && !close(ready[1]));
    char byte;
    assert(read(ready[0], &byte, 1) == 1 && byte == 'r');
    assert(!close(ready[0]));
    assert(!installer_update_target_unlock(&state));
    assert(installer_update_target_lock(&other) == -EBUSY && other.lock_fd == -1);
    assert(write(control[1], "\n", 1) == 1 && !close(control[1]));
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(!installer_update_target_lock(&state));
    assert(!installer_update_target_unlock(&state));

    /* An idle advisory lock does not prove that a crashed coordinator's
     * global mounts are gone. Refusal must release its newly acquired fd. */
    active[7] = 1;
    assert(installer_update_target_lock(&state) == -EBUSY && state.lock_fd == -1);
    active[7] = 0; active[8] = 1;
    assert(installer_update_target_lock(&state) == -EBUSY && state.lock_fd == -1);
    active[8] = 0;
    table_error = 1;
    assert(installer_update_target_lock(&state) == -EIO && state.lock_fd == -1);
    table_error = 0;
    assert(!installer_update_target_lock(&state));
    assert(!installer_update_target_unlock(&state));
    assert(!unlink(lock_file));
}

static void test_owned_planner_cleanup(void)
{
    memset(active, 0, sizeof(active));
    char plan[256], metadata[320], link[320];
    snprintf(plan, sizeof(plan), "%s/reliefos-update.aB3dE5", fixture_workspace);
    snprintf(metadata, sizeof(metadata), "%s/root/lib/apk/db/installed", plan);
    /* An uncatchable helper fault leaves metadata behind. Its coordinator
     * still knows the workspace and must release these tmpfs allocations. */
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        execl("/bin/sh", "sh", "-c",
            "umask 077; mkdir -p \"$1/reliefos-update.aB3dE5/root/lib/apk/db\"; "
            "printf metadata > \"$1/reliefos-update.aB3dE5/root/lib/apk/db/installed\"; kill -KILL $$",
            "sh", fixture_workspace, (char *)NULL);
        _exit(127);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    assert(!access(metadata, F_OK));
    char outside[] = "/tmp/installer-update-outside.XXXXXX", outside_file[256];
    assert(mkdtemp(outside));
    snprintf(outside_file, sizeof(outside_file), "%s/keep", outside);
    int fd = open(outside_file, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && write(fd, "untouched", 9) == 9 && !close(fd));
    snprintf(link, sizeof(link), "%s/root/foreign-directory", plan);
    assert(!symlink(outside, link));
    struct installer_update_target state = target();
    calls = removes = 0;
    assert(!installer_update_target_unmount(&state));
    assert(!access(outside_file, F_OK) && access(plan, F_OK) < 0 && errno == ENOENT);
    assert(!state.workspace[0] && !calls && removes == 2);

    /* Reject unknown workspace contents before deleting any owned plan. */
    assert(!mkdir(plan, 0700));
    snprintf(metadata, sizeof(metadata), "%s/installed-before", plan);
    fd = open(metadata, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && !close(fd));
    char unknown[256];
    snprintf(unknown, sizeof(unknown), "%s/administrator-notes", fixture_workspace);
    fd = open(unknown, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && !close(fd));
    assert(cleanup_plans("/run/reliefos/update-a1B2c3") == -ENOTEMPTY);
    assert(!access(metadata, F_OK) && !access(unknown, F_OK));
    assert(!unlink(unknown));

    /* A same-filesystem bind mount must block cleanup even when st_dev would
     * let a naive recursive remover cross it. No real mount is needed. */
    active[9] = 1;
    assert(cleanup_plans("/run/reliefos/update-a1B2c3") == -EBUSY);
    assert(!access(metadata, F_OK));
    active[9] = 0;
    table_error = 1;
    assert(cleanup_plans("/run/reliefos/update-a1B2c3") == -EIO);
    assert(!access(metadata, F_OK));
    table_error = 0;
    assert(!cleanup_plans("/run/reliefos/update-a1B2c3"));
    assert(access(plan, F_OK) < 0 && errno == ENOENT);

    /* An entry with the expected planner name still cannot redirect cleanup
     * outside its workspace. The directory link itself is retained on error. */
    assert(!symlink(outside, plan));
    assert(cleanup_plans("/run/reliefos/update-a1B2c3") == -EPERM);
    struct stat preserved;
    assert(!lstat(plan, &preserved) && S_ISLNK(preserved.st_mode));
    assert(!unlink(plan));
    fd = open(outside_file, O_RDONLY);
    char content[10] = {0};
    assert(fd >= 0 && read(fd, content, 9) == 9 && !strcmp(content, "untouched") && !close(fd));
    assert(!unlink(outside_file) && !rmdir(outside));
}

int main(void)
{
    assert(mkdtemp(fixture_workspace));
    struct installer_update_target state = target();
    active[0] = active[1] = active[2] = active[3] = 1;
    assert(!installer_update_target_unmount(&state));
    assert(calls == 3 && removes == 2 && active[3]);
    for (unsigned i = 0; i < 3; ++i) assert(!strcmp(called[i], points[i]));
    assert(!state.workspace[0] && !state.root_mounted && !state.boot_mounted && !state.esp_mounted);

    state = target(); calls = removes = 0;
    state.root_mounted = state.boot_mounted = state.esp_mounted = 1;
    assert(!installer_update_target_unmount(&state));
    assert(!calls && removes == 2);
    assert(!state.root_mounted && !state.boot_mounted && !state.esp_mounted);

    state = target(); calls = removes = 0;
    active[0] = active[1] = active[2] = 1; fail_at = 0;
    assert(installer_update_target_unmount(&state) == -EBUSY);
    assert(calls == 1 && !removes && state.boot_mounted && active[0] && active[1] && active[2]);
    fail_at = -1; calls = 0;
    assert(!installer_update_target_unmount(&state));
    assert(calls == 3 && removes == 2 && active[3]);

    state = target(); calls = removes = 0; table_error = 1;
    assert(installer_update_target_unmount(&state) == -EIO);
    assert(!calls && !removes && state.workspace[0]);
    table_error = 0;
    strcpy(state.workspace, "/run/reliefos/update-a1B2c3/../other");
    assert(installer_update_target_unmount(&state) == -EINVAL);
    assert(!calls && !removes);

    state.lock_fd = open("/dev/null", O_RDONLY);
    assert(state.lock_fd >= 0);
    int previous = state.lock_fd;
    assert(!installer_update_target_unlock(&state) && state.lock_fd == -1);
    assert(fcntl(previous, F_GETFD) < 0 && errno == EBADF);
    assert(!installer_update_target_unlock(&state));
    test_lock_inheritance();
    test_owned_planner_cleanup();
    assert(!rmdir(fixture_workspace));
    puts("PASS interrupted update cleanup, inherited locks and owned planner recovery");
    return 0;
}
