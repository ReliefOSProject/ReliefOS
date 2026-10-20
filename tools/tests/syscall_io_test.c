/* Production read/write bodies plus production pipe/wait code. Hardware and
 * unrelated storage backends are unavailable on the host and fail if reached.
 * The fixture checks descriptor selection, validation and pipe/eventfd/null
 * results; full dispatcher routing is covered separately in the guest. */
#include <assert.h>
#define main pipe_wake_regressions
#include "pipe_wake_test.c"
#undef main
#include <reliefos/auth_abi.h>
#include <reliefos/text.h>

static _Noreturn int io_unexpected(const char *name, ...)
{
    fprintf(stderr, "FAIL: unexpected backend %s\n", name);
    abort();
}
#define IO_UNAVAILABLE(...) io_unexpected(__func__, ##__VA_ARGS__)
#define task_signalfd_read IO_UNAVAILABLE
#define syscall_drm_read IO_UNAVAILABLE
#define drm_prime_file_rw IO_UNAVAILABLE
#define pty_write_input IO_UNAVAILABLE
#define pty_check_change IO_UNAVAILABLE
#define pty_write_output IO_UNAVAILABLE
#define pty_output_available IO_UNAVAILABLE
#define pty_is_hungup IO_UNAVAILABLE
#define pty_read_output IO_UNAVAILABLE
#define pty_input_available IO_UNAVAILABLE
#define pty_read_input IO_UNAVAILABLE
#define task_socket_write IO_UNAVAILABLE
#define task_socket_read IO_UNAVAILABLE
#define task_inet_write IO_UNAVAILABLE
#define task_inet_read IO_UNAVAILABLE
#define console_write_len IO_UNAVAILABLE
#define task_shm_write IO_UNAVAILABLE
#define task_shm_read IO_UNAVAILABLE
#define task_effective_role IO_UNAVAILABLE
#define storage_installer_root_active IO_UNAVAILABLE
#define syscall_regular_io IO_UNAVAILABLE
#define task_device_write_length IO_UNAVAILABLE
#define task_device_write IO_UNAVAILABLE
#define task_device_read IO_UNAVAILABLE
#define task_timerfd_update IO_UNAVAILABLE
#define proc_readdir IO_UNAVAILABLE
#define proc_read IO_UNAVAILABLE
#define storage_readdir_node IO_UNAVAILABLE

/* Descriptor tables are empty: only the actual entry-pinned description is
 * available. Accidentally falling back to a stale fd must fail with EBADF. */
struct task_file *task_file_for_fd(struct task *task, int fd)
{ (void)task; (void)fd; return NULL; }
static struct task_pty_fd *task_pty_endpoint_for_fd(struct task *task, int fd)
{ (void)task; (void)fd; return NULL; }
static int task_pty_stream_for_fd(struct task *task, int fd)
{ (void)task; (void)fd; return -1; }

static uint8_t user_bytes[131072];
static bool user_writable = true;
bool user_range_ok(uint64_t address, uint64_t length)
{
    uintptr_t base = (uintptr_t)user_bytes;
    return address >= base && address <= base + sizeof(user_bytes) &&
           length <= base + sizeof(user_bytes) - address;
}
bool user_range_writable(uint64_t address, uint64_t length)
{ return user_writable && user_range_ok(address, length); }

#include "syscall_io_body.inc"

static void pin(struct task *task, int fd, struct task_file *file)
{
    current_task = task;
    task->syscall_fd = fd;
    task->syscall_file = file;
}

static void test_redirected_stdio_and_validation(void)
{
    reset_pipe();
    for (unsigned i = 0; i < 512; ++i) user_bytes[i] = (uint8_t)i;
    pin(&test_tasks[0], 1, &write_end);
    assert(syscall_io_write(1, (uintptr_t)user_bytes, 512) == 512);
    assert(syscall_io_write(1, 0, 1) == -RELIEFOS_EFAULT);
    pin(&test_tasks[READER_INDEX], 0, &read_end);
    user_writable = false;
    assert(syscall_io_read(0, (uintptr_t)user_bytes, 512) == -RELIEFOS_EFAULT);
    user_writable = true;
    memset(user_bytes, 0, 512);
    assert(syscall_io_read(0, (uintptr_t)user_bytes, 512) == 512);
    for (unsigned i = 0; i < 512; ++i) assert(user_bytes[i] == (uint8_t)i);
    assert(syscall_io_read(0, (uintptr_t)user_bytes, 0) == 0);
    assert(syscall_io_read(7, (uintptr_t)user_bytes, 1) == -RELIEFOS_EBADF);
}

static void test_pipe_wait_retry_eof_and_signal(void)
{
    reset_pipe();
    pin(&test_tasks[READER_INDEX], 3, &read_end);
    assert(syscall_io_read(3, (uintptr_t)user_bytes, 512) == -RELIEFOS_EAGAIN);
    assert(current_task->state == TASK_BLOCKED);
    pin(&test_tasks[0], 4, &write_end);
    memset(user_bytes, 0xa5, 512);
    assert(syscall_io_write(4, (uintptr_t)user_bytes, 512) == 512);
    assert(test_tasks[READER_INDEX].state == TASK_READY);
    pin(&test_tasks[READER_INDEX], 3, &read_end);
    assert(syscall_io_read(3, (uintptr_t)user_bytes, 512) == 512);
    read_end.flags |= RELIEFOS_O_NONBLOCK;
    current_task->state = TASK_RUNNING;
    assert(syscall_io_read(3, (uintptr_t)user_bytes, 1) == -RELIEFOS_EAGAIN);
    assert(current_task->state == TASK_RUNNING);
    pipe_fixture.writers = 0;
    assert(syscall_io_read(3, (uintptr_t)user_bytes, 1) == 0);
    pipe_fixture.readers = 0;
    pin(&test_tasks[0], 4, &write_end);
    assert(syscall_io_write(4, (uintptr_t)user_bytes, 1) == -RELIEFOS_EPIPE);
    assert(signal_count == 1 && signal_pid == test_tasks[0].pid && signal_number == 13);
}

static void test_null_access_and_full_count(void)
{
    reset_pipe();
    struct task_file file = {.used = 1, .flags = TASK_FILE_FLAG_DEV_NULL | RELIEFOS_O_RDWR};
    pin(&test_tasks[0], 3, &file);
    assert(syscall_io_write(3, (uintptr_t)user_bytes, sizeof(user_bytes)) == sizeof(user_bytes));
    assert(syscall_io_read(3, (uintptr_t)user_bytes, sizeof(user_bytes)) == 0);
    file.flags = TASK_FILE_FLAG_DEV_NULL | RELIEFOS_O_RDONLY;
    assert(syscall_io_write(3, (uintptr_t)user_bytes, 1) == -RELIEFOS_EBADF);
    file.flags = TASK_FILE_FLAG_DEV_NULL | RELIEFOS_O_WRONLY;
    assert(syscall_io_read(3, (uintptr_t)user_bytes, 1) == -RELIEFOS_EBADF);
    file.kind = TASK_FILE_KIND_SIGNALFD;
    assert(syscall_io_write(3, (uintptr_t)user_bytes, 8) == -LINUX_EINVAL);
}

static void test_eventfd_results(void)
{
    reset_pipe();
    struct task_file file = {.used = 1, .flags = TASK_FILE_FLAG_EVENTFD | RELIEFOS_O_RDWR};
    pin(&test_tasks[0], 3, &file);
    uint64_t value = 7;
    memcpy(user_bytes, &value, sizeof(value));
    assert(syscall_io_write(3, (uintptr_t)user_bytes, 7) == -RELIEFOS_EINVAL);
    assert(file.aux == 0);
    assert(syscall_io_write(3, (uintptr_t)user_bytes, 8) == 8);
    assert(file.aux == 7);
    assert(syscall_io_read(3, (uintptr_t)user_bytes, 8) == 8);
    memcpy(&value, user_bytes, sizeof(value));
    assert(value == 7 && file.aux == 0);
    assert(syscall_io_read(3, (uintptr_t)user_bytes, 8) == -RELIEFOS_EAGAIN);
    file.aux = 3; file.aux2 = 1;
    assert(syscall_io_read(3, (uintptr_t)user_bytes, 8) == 8);
    memcpy(&value, user_bytes, sizeof(value));
    assert(value == 1 && file.aux == 2);
    value = UINT64_MAX;
    memcpy(user_bytes, &value, sizeof(value));
    assert(syscall_io_write(3, (uintptr_t)user_bytes, 8) == -RELIEFOS_EINVAL);
    value = UINT64_MAX - 1;
    memcpy(user_bytes, &value, sizeof(value));
    assert(syscall_io_write(3, (uintptr_t)user_bytes, 8) == -RELIEFOS_EAGAIN);
    assert(file.aux == 2);
}

int main(void)
{
    assert(pipe_wake_regressions() == 0);
    test_redirected_stdio_and_validation();
    test_pipe_wait_retry_eof_and_signal();
    test_null_access_and_full_count();
    test_eventfd_results();
    puts("syscall I/O: redirected stdio, EFAULT, EBADF, pipe wait/retry/EOF/SIGPIPE, null and eventfd passed");
    return 0;
}
