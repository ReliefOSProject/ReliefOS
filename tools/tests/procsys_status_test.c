#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/poll.h>
#include <unistd.h>
#include <reliefos/fs.h>
int fixture_open(const char *path, int flags, ...);
int fixture_openat(int dir, const char *path, int flags, ...);
long fixture_syscall(long number, ...);
int fixture_poll(struct pollfd *fds, nfds_t count, int timeout);
ssize_t fixture_read(int fd, void *buffer, size_t size);
int fixture_close(int fd);
int fixture_readdir(int fd, struct reliefos_dir_entry *entry);
int fixture_getpwuid_r(uid_t uid, struct passwd *record, char *buffer, size_t size,
                       struct passwd **result);
#define open fixture_open
#define openat fixture_openat
#define syscall fixture_syscall
#define poll fixture_poll
#define read fixture_read
#define close fixture_close
#define getpwuid_r fixture_getpwuid_r
#define reliefos_readdir fixture_readdir
#include "../../userland/runtime/src/procsys.c"
#undef open
#undef openat
#undef syscall
#undef poll
#undef read
#undef close
#undef getpwuid_r
#undef reliefos_readdir

#include <stdarg.h>

static unsigned cursor, offset, closes, handle_closes;
static int current_file, missing_status, pidfd_missing, exited_after_read;
int fixture_open(const char *path, int flags, ...)
{
    (void)flags;
    offset = 0;
    if (!strcmp(path, "/proc")) { cursor = 0; return 0; }
    if (!strcmp(path, "/proc/42/stat")) { current_file = 1; return 1; }
    assert(!strcmp(path, "/proc/42/status"));
    current_file = 2;
    return missing_status ? -1 : 1;
}
int fixture_openat(int dir, const char *path, int flags, ...)
{
    (void)flags;
    if (dir == 0) { assert(!strcmp(path, "42")); return 2; }
    assert(dir == 2);
    if (!strcmp(path, "stat")) return fixture_open("/proc/42/stat", flags);
    assert(!strcmp(path, "status"));
    return fixture_open("/proc/42/status", flags);
}
long fixture_syscall(long number, ...)
{
    va_list args;
    va_start(args, number);
    assert(number == SYS_pidfd_open && va_arg(args, long) == 42L && va_arg(args, long) == 0L);
    va_end(args);
    if (pidfd_missing) { errno = ENOSYS; return -1; }
    return 7;
}
int fixture_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    assert(count == 1 && timeout == 0 && fds->fd == 7 && fds->events == POLLIN);
    fds->revents = exited_after_read ? POLLIN | POLLHUP : 0;
    return !!fds->revents;
}
ssize_t fixture_read(int fd, void *buffer, size_t size)
{
    assert(fd == 1);
    const char *text = current_file == 1 ? "42 (test) R 1 1 1 0 -1 0 0 0 0 0 17 3 0 0 20 0 1 0 123 4096 1\n" :
        "Name:\ttest\nUid:\t1001\t1001\t1001\t1001\nVmRSS:\t1234 kB\n";
    size_t length = strlen(text) - offset;
    if (length > size) length = size;
    if (length > 7) length = 7;
    memcpy(buffer, text + offset, length); offset += length;
    return length;
}
int fixture_close(int fd)
{
    assert(fd == 0 || fd == 1 || fd == 2 || fd == 7);
    if (fd == 7) ++handle_closes;
    ++closes;
    return 0;
}
int fixture_readdir(int fd, struct reliefos_dir_entry *entry)
{
    assert(fd == 0);
    if (cursor++) return 0;
    strcpy(entry->name, "42");
    return 1;
}
int fixture_getpwuid_r(uid_t uid, struct passwd *record, char *buffer, size_t size,
                       struct passwd **result)
{
    assert(!missing_status && uid == 1001 && size >= 6);
    strcpy(buffer, "admin"); record->pw_name = buffer; *result = record;
    return 0;
}
int main(void)
{
    struct reliefos_task_info task;
    assert(reliefos_task_snapshot(&task, 1, NULL) == 1);
    assert(task.cpu_ticks == 20 && task.state == 1 && task.parent_pid == 1 && task.pid == 42 && task.uid == 1001 && task.memory_kib == 1234);
    assert(!strcmp(task.username, "admin") && closes == 3);
    missing_status = 1;
    assert(reliefos_task_snapshot(&task, 1, NULL) == 1);
    assert(task.uid == UINT32_MAX && task.memory_kib == 0 && closes == 5);
    missing_status = 0;
    int fd = -1;
    assert(reliefos_task_snapshot_pidfds(&task, &fd, 1, NULL) == 1);
    assert(fd == 7 && task.pid == 42 && task.uid == 1001 && handle_closes == 0);
    assert(fixture_close(fd) == 0);
    exited_after_read = 1;
    assert(reliefos_task_snapshot_pidfds(&task, &fd, 1, NULL) == 0);
    assert(handle_closes == 2);
    pidfd_missing = 1;
    assert(reliefos_task_snapshot_pidfds(&task, &fd, 1, NULL) == 1);
    assert(fd == -1 && task.pid == 42 && handle_closes == 2);
    puts("PASS task snapshot: metadata, pidfd ownership, discarded stale row and old-kernel fallback");
}
