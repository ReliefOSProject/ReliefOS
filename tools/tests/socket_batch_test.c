#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/reliefnt/kernel/syscall/ipc/socket_batch.c"

static struct task current;
static struct task_file socket_file = {.flags = TASK_FILE_FLAG_SOCKET | TASK_FILE_FLAG_SOCKET_UNIX};
static uint64_t readonly;
static int socket_error;
static unsigned cursor, calls, mode;
static struct linux_timespec clock_now = {100, 0};
static struct mmsghdr messages[1030];
struct task *sched_current_task(void) { return &current; }
struct task_file *task_file_for_io(struct task *task, int fd)
{ (void)task; return fd == 3 ? &socket_file : NULL; }
bool user_range_ok(uint64_t p, uint64_t size) { return p >= 4096 && size <= UINT64_MAX - p; }
bool user_range_writable(uint64_t p, uint64_t size) { return user_range_ok(p, size) && p != readonly; }
int user_copy_to_task(struct task *task, uint64_t p, const void *source, uint64_t size)
{ assert(task == &current); if (!user_range_writable(p, size)) return -LINUX_EFAULT; memcpy((void *)p, source, size); return 0; }
int time_clock_get(int32_t id, struct linux_timespec *out)
{ assert(id == LINUX_CLOCK_MONOTONIC); *out = clock_now; return 0; }
uint64_t task_socket_cancel_receive(struct task *task)
{ uint64_t n = task->socket_receive_done; task->socket_receive_done = 0; return n; }
int task_socket_message_error(struct task_file *file, int error, bool setting)
{ assert(file == &socket_file); int previous = socket_error; socket_error = setting ? error : 0; return previous; }
int64_t task_socket_message(struct task *task, struct task_file *file, uint64_t message,
                            uint32_t flags, bool receive, bool batch, struct socket_message_result *out)
{
    assert(task == &current && file == &socket_file && batch);
    assert(message == (uintptr_t)&messages[cursor]);
    ++calls;
    out->requested = 8;
    if (mode == 1 && cursor == 1) return -LINUX_EAGAIN;
    if (mode == 2 && cursor == 1) return -LINUX_EFAULT;
    if (mode == 3 && calls == 2) return KERNEL_SYSCALL_BLOCKED;
    if (mode == 4 && cursor == 0) { ++cursor; return 4; }
    if (mode == 5) { assert(receive && !(flags & MSG_WAITFORONE));
        if (cursor) { assert(flags & MSG_DONTWAIT); return -LINUX_EAGAIN; }
        assert(!(flags & MSG_DONTWAIT)); }
    if (mode == 6) return KERNEL_SYSCALL_BLOCKED;
    if (mode == 7 && cursor == 0) out->flags = MSG_OOB;
    if (mode == 8 && cursor == 1) return -LINUX_EINTR;
    if (mode == 9) clock_now.tv_nsec += 1000000;
    if (!receive) assert(!!(flags & MSG_BATCH) == (cursor + 1 < current.mmsg.length));
    ++cursor;
    return 8;
}

static void reset(unsigned next)
{
    assert(!current.mmsg.active);
    current.syscall_file = &socket_file;
    cursor = calls = 0;
    socket_error = 0;
    readonly = 0;
    mode = next;
    memset(messages, 0, sizeof(messages));
}

int main(void)
{
    reset(0);
    assert(syscall_socket_mmsg(false, -1, 0, 0, MSG_CMSG_COMPAT, 0) == -LINUX_EINVAL);
    assert(syscall_socket_mmsg(false, -1, 0, 0, 0, 0) == -LINUX_EBADF);
    assert(syscall_socket_mmsg(false, 3, 0, 0, 0, 0) == 0 && !calls);
    struct linux_timespec timeout = {.tv_sec = -1};
    assert(syscall_socket_mmsg(true, -1, 0, 0, 0, 1) == -LINUX_EFAULT);
    assert(syscall_socket_mmsg(true, -1, 0, 0, 0, (uintptr_t)&timeout) == -LINUX_EINVAL);
    socket_file.flags = 0;
    assert(syscall_socket_mmsg(false, 3, 0, 0, 0, 0) == -LINUX_ENOTSOCK);
    socket_file.flags = TASK_FILE_FLAG_SOCKET;
    socket_error = LINUX_ECONNRESET;
    assert(syscall_socket_mmsg(true, 3, 0, 0, 0, 0) == -LINUX_ECONNRESET && !socket_error);
    assert(syscall_socket_mmsg(false, 3, (uintptr_t)messages, 1030, 0, 0) == 1024 && cursor == 1024);
    reset(0);
    assert(syscall_socket_mmsg(true, 3, (uintptr_t)messages, 1030, 0, 0) == 1030 && cursor == 1030);
    reset(1);
    assert(syscall_socket_mmsg(false, 3, (uintptr_t)messages, 3, 0, 0) == 1);
    assert(messages[0].msg_len == 8 && !messages[1].msg_len && !socket_error);
    reset(2);
    assert(syscall_socket_mmsg(true, 3, (uintptr_t)messages, 3, 0, 0) == 1);
    assert(socket_error == LINUX_EFAULT);
    assert(syscall_socket_mmsg(true, 3, 0, 0, 0, 0) == -LINUX_EFAULT && !socket_error);
    reset(4);
    assert(syscall_socket_mmsg(false, 3, (uintptr_t)messages, 3, 0, 0) == 1 && calls == 1);
    assert(messages[0].msg_len == 4);
    reset(3);
    assert(syscall_socket_mmsg(false, 3, (uintptr_t)messages, 3, 0, 0) == KERNEL_SYSCALL_BLOCKED);
    assert(current.mmsg.count == 1 && cursor == 1 && messages[0].msg_len == 8);
    assert(syscall_socket_mmsg(false, 3, (uintptr_t)messages, 3, 0, 0) == 3);
    assert(cursor == 3 && calls == 4 && !current.mmsg.active);
    reset(0);
    readonly = (uintptr_t)&messages[0].msg_len;
    assert(syscall_socket_mmsg(false, 3, (uintptr_t)messages, 3, 0, 0) == -LINUX_EFAULT && cursor == 1);
    reset(0);
    readonly = (uintptr_t)&messages[1].msg_len;
    assert(syscall_socket_mmsg(true, 3, (uintptr_t)messages, 3, 0, 0) == 1 && cursor == 2);
    assert(socket_error == LINUX_EFAULT);
    reset(5);
    assert(syscall_socket_mmsg(true, 3, (uintptr_t)messages, 3, MSG_WAITFORONE, 0) == 1 && !socket_error);
    reset(0);
    timeout = (struct linux_timespec){0};
    assert(syscall_socket_mmsg(true, 3, (uintptr_t)messages, 3, 0, (uintptr_t)&timeout) == 1);
    reset(6);
    assert(syscall_socket_mmsg(true, 3, (uintptr_t)messages, 3, 0, (uintptr_t)&timeout) == KERNEL_SYSCALL_BLOCKED);
    assert(current.mmsg.active && !current.mmsg.count);
    assert(task_socket_mmsg_interrupt(&current, 0) == -LINUX_EINTR && !socket_error);
    reset(9);
    timeout = (struct linux_timespec){0, 2500000};
    assert(syscall_socket_mmsg(true, 3, (uintptr_t)messages, 8, 0, (uintptr_t)&timeout) == 3);
    assert(!timeout.tv_sec && !timeout.tv_nsec);
    reset(0);
    timeout = (struct linux_timespec){INT64_MAX, 999999999};
    readonly = (uintptr_t)&timeout;
    assert(syscall_socket_mmsg(true, 3, (uintptr_t)messages, 1, 0, (uintptr_t)&timeout) == -LINUX_EFAULT);
    assert(cursor == 1 && messages[0].msg_len == 8 && !socket_error);
    reset(3);
    assert(syscall_socket_mmsg(true, 3, (uintptr_t)messages, 3, 0, 0) == KERNEL_SYSCALL_BLOCKED);
    assert(task_socket_mmsg_interrupt(&current, 4) == 2);
    assert(messages[1].msg_len == 4 && socket_error == 512);
    reset(3);
    assert(syscall_socket_mmsg(true, 3, (uintptr_t)messages, 3, 0, 0) == KERNEL_SYSCALL_BLOCKED);
    current.socket_io_timed = true;
    assert(task_socket_mmsg_interrupt(&current, 0) == 1 && socket_error == LINUX_EINTR);
    current.socket_io_timed = false;
    reset(6);
    assert(syscall_socket_mmsg(true, 3, (uintptr_t)messages, 3, MSG_WAITFORONE, 0) == KERNEL_SYSCALL_BLOCKED);
    assert(task_socket_mmsg_interrupt(&current, 4) == 1 && !socket_error);
    assert(messages[0].msg_len == 4);
    reset(3);
    assert(syscall_socket_mmsg(false, 3, (uintptr_t)messages, 3, 0, 0) == KERNEL_SYSCALL_BLOCKED);
    assert(task_socket_mmsg_interrupt(&current, 0) == 1 && !socket_error);
    reset(7);
    assert(syscall_socket_mmsg(true, 3, (uintptr_t)messages, 3, 0, 0) == 1 && calls == 1);
    puts("PASS mmsg kernel iteration: ABI widths, partial success, output faults, retained progress, timeouts, WAITFORONE, deferred errors and interruption");
}
