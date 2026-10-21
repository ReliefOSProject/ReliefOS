#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !__has_include("../../kernel/reliefnt/kernel/syscall/ipc/sysv_msg.c")
int main(void) { fputs("FAIL: SysV message queue implementation is missing\n", stderr); return 1; }
#else
#include "../../kernel/reliefnt/kernel/syscall/ipc/sysv_msg.c"

static struct task tasks[4], *current;
static unsigned allocations;
static int allocation_fail;
static uint64_t read_fault, write_fault;
void *kernel_malloc(size_t n)
{ if (allocation_fail) return NULL; void *p = malloc(n); if (p) ++allocations; return p; }
void kernel_free(void *p)
{ if (p) { assert(allocations); --allocations; free(p); } }
struct task *sched_current_task(void) { return current; }
void sched_signal_wait_current(uint64_t deadline)
{ assert(!deadline); current->state = TASK_BLOCKED; }
void sched_wake_interruptible(struct task *task)
{ if (task->state == TASK_BLOCKED) { task->state = TASK_READY; task->wake_tick = 0; } }
int time_clock_get(int32_t clock, struct linux_timespec *out)
{ assert(!clock); *out = (struct linux_timespec){1234567890, 0}; return 0; }
bool user_range_ok(uint64_t p, uint64_t n)
{ return !n || (p >= 4096 && n <= UINT64_MAX - p && (!read_fault || p + n <= read_fault)); }
int user_copy_to_task(struct task *task, uint64_t p, const void *src, uint64_t n)
{
    assert(task == current);
    if (!n) return 0;
    if (p < 4096 || n > UINT64_MAX - p) return -LINUX_EFAULT;
    if (write_fault && p + n > write_fault) {
        if (p < write_fault) memcpy((void *)p, src, write_fault - p);
        return -LINUX_EFAULT;
    }
    memcpy((void *)p, src, n);
    return 0;
}
static int64_t msgget(int32_t key, uint32_t flags)
{ return syscall_sysv_msg(__NR_msgget, (uint32_t)key, flags, 0, 0, 0); }
static int64_t send_msg(int q, void *msg, uint64_t size, uint32_t flags)
{ return syscall_sysv_msg(__NR_msgsnd, q, (uintptr_t)msg, size, flags, 0); }
static int64_t receive_msg(int q, void *msg, uint64_t size, int64_t type, uint32_t flags)
{ return syscall_sysv_msg(__NR_msgrcv, q, (uintptr_t)msg, size, type, flags); }
static int64_t control(int q, int cmd, void *data)
{ return syscall_sysv_msg(__NR_msgctl, q, cmd, (uintptr_t)data, 0, 0); }

int main(void)
{
    for (unsigned i = 0; i < 4; ++i) {
        tasks[i].pid = 101 + i; tasks[i].tgid = 101;
        tasks[i].euid = 1000; tasks[i].egid = tasks[i].fsgid = 100;
        tasks[i].kind = TASK_KIND_USER; tasks[i].state = TASK_READY;
    }
    current = &tasks[0];
    struct { int64_t type; char data[32]; } m = {7, "original"}, out = {0};
    struct linux_msqid64_ds ds;
    assert(sizeof(ds) == 120 && sizeof(ds.msg_perm) == 48);
    assert(msgget(123, 0) == -LINUX_ENOENT);
    allocation_fail = 1;
    assert(msgget(0, 0600) == -LINUX_ENOMEM);
    allocation_fail = 0;
    int q = msgget(123, LINUX_IPC_CREAT | 0600);
    assert(q >= 0 && msgget(123, 0600) == q);
    assert(msgget(123, LINUX_IPC_CREAT | LINUX_IPC_EXCL) == -LINUX_EEXIST);
    assert(control(q, LINUX_IPC_STAT, &ds) == 0);
    assert(ds.msg_perm.uid == 1000 && ds.msg_perm.cuid == 1000 && ds.msg_perm.gid == 100);
    assert(ds.msg_ctime == 1234567890 && !ds.msg_stime && ds.msg_qbytes == 16384);
    assert(control(q, LINUX_IPC_STAT | 0x100, &ds) == -LINUX_EINVAL);
    assert(send_msg(-1, (void *)1, UINT64_MAX, 0) == -LINUX_EFAULT);
    assert(send_msg(-1, &m, UINT64_MAX, 0) == -LINUX_EINVAL);
    assert(receive_msg(-1, (void *)1, 0, 0, 0) == -LINUX_EINVAL);
    assert(receive_msg(q, (void *)1, 32, 0, LINUX_IPC_NOWAIT) == -LINUX_ENOMSG);
    assert(send_msg(q, &m, 8, 0) == 0);
    assert(receive_msg(q, &out, 7, 0, LINUX_IPC_NOWAIT) == -LINUX_E2BIG);
    assert(receive_msg(q, &out, 32, 0, LINUX_MSG_COPY | LINUX_IPC_NOWAIT) == 8);
    assert(out.type == 7 && !memcmp(out.data, "original", 8));
    assert(receive_msg(q, &out, 7, 0, LINUX_MSG_COPY | LINUX_MSG_NOERROR | LINUX_IPC_NOWAIT) == -LINUX_EINVAL);
    write_fault = (uintptr_t)out.data + 3;
    memset(&out, 0, sizeof(out));
    assert(receive_msg(q, &out, 8, 0, 0) == -LINUX_EFAULT);
    assert(out.type == 7 && !memcmp(out.data, "ori", 3) && !out.data[3]);
    write_fault = 0;
    assert(control(q, LINUX_IPC_STAT, &ds) == 0 && !ds.msg_qnum && ds.msg_lrpid == 101);
    const int64_t types[] = {9, 3, 7, 3, INT64_MAX};
    for (unsigned i = 0; i < 5; ++i) { m.type = types[i]; m.data[0] = (char)i; assert(send_msg(q, &m, 1, 0) == 0); }
    assert(receive_msg(q, &out, 32, -8, 0) == 1 && out.type == 3 && out.data[0] == 1);
    assert(receive_msg(q, &out, 32, 3, LINUX_MSG_EXCEPT) == 1 && out.type == 9);
    assert(receive_msg(q, &out, 32, INT64_MIN, 0) == 1 && out.type == 3 && out.data[0] == 3);
    assert(receive_msg(q, &out, 32, 0, 0) == 1 && out.type == 7);
    assert(receive_msg(q, &out, 32, INT64_MIN, 0) == 1 && out.type == INT64_MAX);

    /* Pending send owns the original payload, even if the user changes memory. */
    assert(control(q, LINUX_IPC_STAT, &ds) == 0); ds.msg_qbytes = 1;
    assert(control(q, LINUX_IPC_SET, &ds) == 0);
    m.type = 7; m.data[0] = 'a'; assert(send_msg(q, &m, 1, 0) == 0);
    assert(send_msg(q, &m, 1, LINUX_IPC_NOWAIT) == -LINUX_EAGAIN);
    current = &tasks[1]; m.data[0] = 'b';
    assert(send_msg(q, &m, 1, 0) == KERNEL_SYSCALL_BLOCKED && current->state == TASK_BLOCKED);
    m.data[0] = 'c';
    current = &tasks[0]; assert(receive_msg(q, &out, 32, 0, 0) == 1 && out.data[0] == 'a');
    assert(tasks[1].state == TASK_READY);
    current = &tasks[1]; assert(send_msg(q, (void *)1, UINT64_MAX, 0) == 0);
    current = &tasks[0]; assert(receive_msg(q, &out, 32, 0, 0) == 1 && out.data[0] == 'b');

    /* A matching oversized send completes one receiver with E2BIG, then the next. */
    current = &tasks[1]; assert(receive_msg(q, &out, 0, 7, 0) == KERNEL_SYSCALL_BLOCKED);
    current = &tasks[2]; assert(receive_msg(q, &out, 1, 7, 0) == KERNEL_SYSCALL_BLOCKED);
    current = &tasks[0]; assert(send_msg(q, &m, 1, 0) == 0);
    assert(tasks[1].sysv_msg.completed && tasks[2].sysv_msg.completed);
    assert(control(q, LINUX_IPC_STAT, &ds) == 0 && !ds.msg_qnum && ds.msg_lrpid == tasks[2].pid);
    assert(control(q, LINUX_IPC_RMID, NULL) == 0);
    current = &tasks[1]; assert(receive_msg(q, &out, 0, 7, 0) == -LINUX_E2BIG);
    current = &tasks[2]; assert(receive_msg(q, &out, 1, 7, 0) == 1 && out.data[0] == 'c');
    assert(!allocations);

    current = &tasks[0]; q = msgget(0, 0600); assert(q >= 0);
    current = &tasks[1]; assert(receive_msg(q, &out, 32, 0, 0) == KERNEL_SYSCALL_BLOCKED);
    current = &tasks[0]; assert(control(q, LINUX_IPC_STAT, &ds) == 0); ds.msg_qbytes = 0;
    assert(control(q, LINUX_IPC_SET, &ds) == 0);
    current = &tasks[2]; assert(send_msg(q, &m, 0, 0) == KERNEL_SYSCALL_BLOCKED);
    current = &tasks[0]; assert(control(q, LINUX_IPC_RMID, NULL) == 0);
    current = &tasks[1]; assert(receive_msg(q, &out, 32, 0, 0) == -LINUX_EIDRM);
    current = &tasks[2]; assert(send_msg(q, &m, 0, 0) == -LINUX_EIDRM);
    assert(control(q, LINUX_IPC_STAT, &ds) == -LINUX_EINVAL && !allocations);

    /* Effective UID and filesystem/supplementary groups, not file owner policy. */
    current = &tasks[0]; q = msgget(333, 0640 | LINUX_IPC_CREAT);
    current = &tasks[1]; current->euid = 2000;
    assert(control(q, LINUX_IPC_STAT, &ds) == 0);
    assert(send_msg(q, &m, 0, LINUX_IPC_NOWAIT) == -LINUX_EACCES);
    assert(control(q, LINUX_IPC_RMID, NULL) == -LINUX_EPERM);
    current->fsgid = 200;
    assert(control(q, LINUX_IPC_STAT, &ds) == -LINUX_EACCES);
    assert(control(q, LINUX_MSG_STAT_ANY, &ds) == q);
    current->cap_effective = 1ULL << CAP_IPC_OWNER;
    assert(control(q, LINUX_IPC_STAT, &ds) == 0);
    current->cap_effective = 0; current->euid = 1000;
    ds.msg_perm.uid = 3000; ds.msg_perm.gid = 300; ds.msg_perm.mode = 0600;
    ds.msg_qbytes = 32768;
    assert(control(q, LINUX_IPC_SET, &ds) == -LINUX_EPERM);
    ds.msg_qbytes = UINT32_MAX;
    assert(control(q, LINUX_IPC_SET, &ds) == -LINUX_EPERM);
    current->cap_effective = 1ULL << CAP_SYS_RESOURCE;
    assert(control(q, LINUX_IPC_SET, &ds) == 0);
    assert(control(q, LINUX_IPC_STAT, &ds) == 0 && ds.msg_qbytes == UINT64_MAX);
    assert(ds.msg_perm.uid == 3000 && ds.msg_perm.cuid == 1000);
    ds.msg_perm.uid = UINT32_MAX;
    assert(control(q, LINUX_IPC_SET, &ds) == -LINUX_EINVAL);
    assert(control(q, LINUX_IPC_RMID, NULL) == 0);
    assert(!allocations);

    current = &tasks[0];
    q = msgget(0, 0600);
    struct linux_msginfo info;
    assert(control(0, LINUX_IPC_INFO, &info) >= 0 && info.msgmni == 32000 && info.msgmax == 8192);
    assert(control(q, LINUX_IPC_STAT, &ds) == 0); ds.msg_qbytes = 1;
    assert(control(q, LINUX_IPC_SET, &ds) == 0);
    m.type = 7;
    assert(send_msg(q, &m, 1, 0) == 0);
    assert(control(0, LINUX_MSG_INFO, &info) >= 0 && info.msgpool == 1 && info.msgmap == 1 && info.msgtql == 1);
    allocation_fail = 1;
    assert(send_msg(q, &m, 1, 0) == -LINUX_ENOMEM);
    allocation_fail = 0;
    current = &tasks[1]; current->cap_effective = 0;
    assert(send_msg(q, &m, 1, 0) == KERNEL_SYSCALL_BLOCKED);
    current = &tasks[2];
    assert(receive_msg(q, &out, 32, 99, 0) == KERNEL_SYSCALL_BLOCKED);
    current = &tasks[0]; ds.msg_perm.mode = 0; ds.msg_qbytes = 2;
    assert(control(q, LINUX_IPC_SET, &ds) == 0);
    assert(tasks[1].state == TASK_READY && tasks[2].state == TASK_READY);
    current = &tasks[1]; assert(send_msg(q, &m, 1, 0) == -LINUX_EACCES);
    current = &tasks[2]; assert(receive_msg(q, &out, 32, 99, 0) == -LINUX_EACCES);
    current = &tasks[0]; assert(control(q, LINUX_IPC_RMID, NULL) == 0 && !allocations);

    /* Exit/handler cancellation drops wait references without removing the queue. */
    q = msgget(0, 0600);
    current = &tasks[1]; current->pending_signals = 1;
    assert(receive_msg(q, &out, 32, 0, 0) == KERNEL_SYSCALL_BLOCKED && current->state == TASK_READY);
    current->pending_signals = 0;
    task_sysv_msg_cancel(current);
    current = &tasks[0]; assert(send_msg(q, &m, 1, 0) == 0);
    assert(receive_msg(q, &out, 32, 0, 0) == 1);
    assert(control(q, LINUX_IPC_RMID, NULL) == 0 && !allocations);

    /* Reuse an index and verify the old generation never aliases its replacement. */
    int old = msgget(0, 0600);
    assert(control(old, LINUX_IPC_RMID, NULL) == 0);
    bool reused = false;
    for (unsigned i = 0; i < 64; ++i) {
        q = msgget(0, 0600); assert(q >= 0 && q != old);
        if ((q & 32767) == (old & 32767)) {
            reused = true;
            assert(control(old, LINUX_IPC_STAT, &ds) == -LINUX_EINVAL);
            assert(control(old, LINUX_MSG_STAT, &ds) == q);
        }
        assert(control(q, LINUX_IPC_RMID, NULL) == 0);
    }
    assert(reused && !allocations);
    puts("PASS kernel SysV queues: data/order, copy faults, permissions, blocked payload, pipeline, RMID and references");
    return 0;
}
#endif
