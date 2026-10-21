#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !__has_include("../../kernel/reliefnt/kernel/syscall/ipc/sysv_sem.c")
int main(void) { fputs("FAIL: SysV semaphore implementation is missing\n", stderr); return 1; }
#else
#include "../../kernel/reliefnt/kernel/syscall/ipc/sysv_sem.c"

static struct task tasks[4], *current;
static unsigned allocations;
static int fail_allocation;
static uint64_t monotonic_ns = 1000000000, fault_at;
void *kernel_malloc(size_t n)
{ if (fail_allocation) return NULL; void *p = malloc(n); if (p) ++allocations; return p; }
void kernel_free(void *p) { if (p) { assert(allocations); --allocations; free(p); } }
struct task *sched_current_task(void) { return current; }
int time_clock_get(int32_t clock, struct linux_timespec *out)
{
    *out = clock ? (struct linux_timespec){monotonic_ns / 1000000000, monotonic_ns % 1000000000}
                 : (struct linux_timespec){1234567890, 0};
    return 0;
}
uint64_t time_ticks(void) { return monotonic_ns / 10000000; }
void sched_signal_wait_current(uint64_t deadline)
{ current->wake_tick = deadline; current->state = TASK_BLOCKED; }
void sched_wake_interruptible(struct task *task)
{ if (task->state == TASK_BLOCKED) { task->state = TASK_READY; task->wake_tick = 0; } }
bool user_range_ok(uint64_t p, uint64_t n)
{ return !n || (p >= 4096 && n <= UINT64_MAX - p && (!fault_at || p + n <= fault_at)); }
int user_copy_to_task(struct task *task, uint64_t p, const void *src, uint64_t n)
{
    assert(task == current);
    if (p < 4096 || n > UINT64_MAX - p) return -LINUX_EFAULT;
    if (fault_at && p + n > fault_at) {
        if (p < fault_at) memcpy((void *)p, src, fault_at - p);
        return -LINUX_EFAULT;
    }
    memcpy((void *)p, src, n); return 0;
}
static int64_t get(int32_t key, int32_t n, uint32_t flags)
{ return syscall_sysv_sem(__NR_semget, (uint32_t)key, (uint32_t)n, flags, 0); }
static int64_t ctl(int id, int num, int cmd, uint64_t arg)
{ return syscall_sysv_sem(__NR_semctl, id, num, cmd, arg); }
static int64_t op(int id, struct linux_sembuf *ops, uint64_t n)
{ return syscall_sysv_sem(__NR_semop, id, (uintptr_t)ops, n, 0); }
static int64_t timed(int id, struct linux_sembuf *ops, uint64_t n, struct linux_timespec *t)
{ return syscall_sysv_sem(__NR_semtimedop, id, (uintptr_t)ops, n, (uintptr_t)t); }

int main(void)
{
    for (unsigned i = 0; i < 4; ++i) {
        tasks[i].pid = tasks[i].tgid = 101 + i;
        tasks[i].euid = 1000; tasks[i].egid = tasks[i].fsgid = 100;
        tasks[i].kind = TASK_KIND_USER; tasks[i].state = TASK_READY;
    }
    current = &tasks[0];
    struct linux_semid64_ds ds;
    struct linux_sembuf ops[] = {{0, -1, 0}, {1, 1, 0}, {0, 1, 0}};
    struct linux_timespec zero = {0}, invalid = {-1, 0};
    assert(sizeof(ds) == 104 && offsetof(struct linux_semid64_ds, sem_ctime) == 64);
    assert(sizeof(struct linux_sembuf) == 6);
    assert(get(0, 0, 0600) == -LINUX_EINVAL);
    assert(get(777, 0, 0) == -LINUX_ENOENT);
    assert(get(777, -1, 0) == -LINUX_EINVAL);
    fail_allocation = 1; assert(get(0, 2, 0600) == -LINUX_ENOMEM); fail_allocation = 0;
    int id = get(777, 2, LINUX_IPC_CREAT | 0600); assert(id >= 0);
    assert(get(777, 0, 0) == id && get(777, 3, 0) == -LINUX_EINVAL);
    assert(get(777, 3, LINUX_IPC_CREAT | LINUX_IPC_EXCL) == -LINUX_EEXIST);
    assert(ctl(id, -1, LINUX_IPC_STAT, (uintptr_t)&ds) == 0 && ds.sem_nsems == 2 && !ds.sem_otime);
    assert(ds.sem_ctime == 1234567890 && ds.sem_perm.uid == 1000 && ds.sem_perm.gid == 100);
    assert(ctl(id, 0, LINUX_IPC_STAT | 0x100, (uintptr_t)&ds) == -LINUX_EINVAL);
    assert(ctl(-1, -1, LINUX_SETVAL, 65535) == -LINUX_EINVAL);
    assert(ctl(INT32_MAX, -1, LINUX_SETVAL, 65535) == -LINUX_ERANGE);
    assert(op(-1, (void *)1, 1) == -LINUX_EFAULT);
    assert(timed(-1, (void *)1, 0, (void *)1) == -LINUX_EFAULT);
    assert(timed(id, (void *)1, 501, &invalid) == -LINUX_E2BIG);
    assert(timed(id, (void *)1, 1, &invalid) == -LINUX_EFAULT);
    assert(timed(id, ops, 1, &invalid) == -LINUX_EINVAL);
    assert(timed(id, ops, 1, &zero) == -LINUX_EAGAIN);
    ops[0].sem_num = 2; assert(op(id, ops, 1) == -LINUX_EFBIG); ops[0].sem_num = 0;
    assert(ctl(id, 0, LINUX_SETVAL, 1) == 0);
    assert(ctl(id, 1, LINUX_SETVAL, 32767) == 0);
    assert(op(id, ops, 2) == -LINUX_ERANGE);
    assert(ctl(id, 0, LINUX_GETVAL, 0) == 1 && ctl(id, 0, LINUX_GETPID, 0) == 101);
    assert(ctl(id, 1, LINUX_SETVAL, 0) == 0);
    assert(op(id, ops, 3) == 0);
    assert(ctl(id, 0, LINUX_GETVAL, 0) == 1 && ctl(id, 1, LINUX_GETVAL, 0) == 1);
    ops[0] = (struct linux_sembuf){0, -1, LINUX_SEM_UNDO};
    ops[1] = (struct linux_sembuf){1, -2, LINUX_IPC_NOWAIT | LINUX_SEM_UNDO};
    assert(op(id, ops, 2) == -LINUX_EAGAIN);
    task_sysv_sem_exit(current);
    assert(ctl(id, 0, LINUX_GETVAL, 0) == 1);

    /* Wakers execute whole operations before the blocked task resumes. */
    current = &tasks[1];
    ops[0] = (struct linux_sembuf){0, -2, 0};
    assert(op(id, ops, 1) == KERNEL_SYSCALL_BLOCKED);
    ops[0].sem_op = 30000;
    current = &tasks[0]; assert(ctl(id, 0, LINUX_GETNCNT, 0) == 1);
    assert(ctl(id, 0, LINUX_SETVAL, 2) == 0);
    assert(tasks[1].sysv_sem.completed && ctl(id, 0, LINUX_GETVAL, 0) == 0);
    assert(ctl(id, 0, LINUX_GETPID, 0) == 102 && ctl(id, 0, LINUX_GETNCNT, 0) == 0);
    current = &tasks[1]; assert(op(id, (void *)1, UINT64_MAX) == 0);
    current = &tasks[0];
    ops[0] = (struct linux_sembuf){0, 1, LINUX_SEM_UNDO}; assert(op(id, ops, 1) == 0);
    assert(task_sysv_sem_clone(current, &tasks[2], CLONE_SYSVSEM) == 0);
    assert(task_sysv_sem_clone(current, &tasks[3], 0) == 0 && !tasks[3].sysv_undo);
    task_sysv_sem_exit(current);
    assert(ctl(id, 0, LINUX_GETVAL, 0) == 1);
    task_sysv_sem_exit(&tasks[2]);
    assert(ctl(id, 0, LINUX_GETVAL, 0) == 0 && ctl(id, 0, LINUX_GETPID, 0) == 103);
    assert(op(id, ops, 1) == 0);
    assert(ctl(id, 0, LINUX_SETVAL, 8) == 0);
    task_sysv_sem_exit(current);
    assert(ctl(id, 0, LINUX_GETVAL, 0) == 8);

    current = &tasks[1];
    ops[0] = (struct linux_sembuf){0, 0, 0};
    struct linux_timespec duration = {0, 20000000};
    assert(timed(id, ops, 1, &duration) == KERNEL_SYSCALL_BLOCKED);
    current = &tasks[0]; assert(ctl(id, 0, LINUX_GETZCNT, 0) == 1);
    current = &tasks[1]; monotonic_ns += 20000000;
    assert(timed(id, (void *)1, 0, (void *)1) == -LINUX_EAGAIN);
    assert(duration.tv_sec == 0 && duration.tv_nsec == 20000000);
    current = &tasks[0]; assert(ctl(id, 0, LINUX_GETZCNT, 0) == 0);
    unsigned short values[] = {5, 6}, output[2] = {0};
    assert(ctl(id, -1, LINUX_SETALL, (uintptr_t)values) == 0);
    assert(ctl(id, -1, LINUX_GETALL, (uintptr_t)output) == 0 && !memcmp(values, output, sizeof(values)));
    values[1] = 32768; assert(ctl(id, -1, LINUX_SETALL, (uintptr_t)values) == -LINUX_ERANGE);
    assert(ctl(id, 0, LINUX_GETVAL, 0) == 5);
    current = &tasks[1]; ops[0] = (struct linux_sembuf){1, -7, LINUX_SEM_UNDO};
    assert(op(id, ops, 1) == KERNEL_SYSCALL_BLOCKED);
    current = &tasks[0]; assert(ctl(id, 0, LINUX_IPC_RMID, 0) == 0);
    current = &tasks[1]; assert(op(id, ops, 1) == -LINUX_EIDRM);
    task_sysv_sem_exit(current);
    assert(!allocations);

    /* Wait-for-zero completes before a later increment can obscure the zero. */
    current = &tasks[0]; id = get(0, 2, 0600);
    assert(ctl(id, 0, LINUX_SETVAL, 1) == 0);
    current = &tasks[1]; ops[0] = (struct linux_sembuf){0, 0, 0};
    assert(op(id, ops, 1) == KERNEL_SYSCALL_BLOCKED);
    current = &tasks[2];
    ops[0] = (struct linux_sembuf){0, 0, 0}; ops[1] = (struct linux_sembuf){0, 1, 0};
    assert(op(id, ops, 2) == KERNEL_SYSCALL_BLOCKED);
    current = &tasks[0];
    assert(ctl(id, 0, LINUX_GETZCNT, 0) == 2);
    assert(ctl(id, 0, LINUX_SETVAL, 0) == 0);
    assert(tasks[1].sysv_sem.completed && tasks[2].sysv_sem.completed);
    assert(ctl(id, 0, LINUX_GETVAL, 0) == 1);
    current = &tasks[1]; assert(op(id, NULL, 0) == 0);
    current = &tasks[2]; assert(op(id, NULL, 0) == 0);

    /* IPC_SET does not evict existing waiters or recheck their original DAC. */
    ops[0] = (struct linux_sembuf){1, -1, 0};
    assert(op(id, ops, 1) == KERNEL_SYSCALL_BLOCKED);
    current = &tasks[0];
    assert(ctl(id, 0, LINUX_IPC_STAT, (uintptr_t)&ds) == 0);
    ds.sem_perm.mode = 0;
    assert(ctl(id, 0, LINUX_IPC_SET, (uintptr_t)&ds) == 0);
    assert(tasks[2].state == TASK_BLOCKED);
    current = &tasks[1]; current->euid = 2000;
    assert(ctl(id, -1, LINUX_GETVAL, 0) == -LINUX_EACCES);
    assert(ctl(id, -1, LINUX_SETVAL, 1) == -LINUX_EINVAL);
    assert(ctl(id, 0, LINUX_SEM_STAT_ANY, (uintptr_t)&ds) == id);
    assert(ctl(id, 0, LINUX_IPC_RMID, 0) == -LINUX_EPERM);
    current = &tasks[0]; current->cap_effective = 1ULL << CAP_IPC_OWNER;
    assert(ctl(id, 1, LINUX_SETVAL, 1) == 0 && tasks[2].sysv_sem.completed);
    current = &tasks[2]; assert(op(id, NULL, 0) == 0);
    current = &tasks[0]; ds.sem_perm.mode = 0600;
    assert(ctl(id, 0, LINUX_IPC_SET, (uintptr_t)&ds) == 0); current->cap_effective = 0;
    current = &tasks[1]; current->euid = 1000;
    ops[0] = (struct linux_sembuf){0, -2, 0};
    assert(op(id, ops, 1) == KERNEL_SYSCALL_BLOCKED);
    ops[0].sem_num = 1;
    current = &tasks[2]; assert(op(id, ops, 1) == KERNEL_SYSCALL_BLOCKED);
    current = &tasks[3]; ops[0] = (struct linux_sembuf){0, -2, 0}; ops[1] = (struct linux_sembuf){1, -1, 0};
    assert(op(id, ops, 2) == KERNEL_SYSCALL_BLOCKED);
    current = &tasks[0];
    assert(ctl(id, 0, LINUX_GETNCNT, 0) == 2 && ctl(id, 1, LINUX_GETNCNT, 0) == 1);
    task_sysv_sem_cancel(&tasks[3]);
    assert(ctl(id, 0, LINUX_GETNCNT, 0) == 1);
    values[0] = values[1] = 2;
    assert(ctl(id, -1, LINUX_SETALL, (uintptr_t)values) == 0);
    assert(tasks[1].sysv_sem.completed && tasks[2].sysv_sem.completed);
    task_sysv_sem_cancel(&tasks[1]); task_sysv_sem_cancel(&tasks[2]);

    /* Undo range errors roll back; exit clamps both bounds and SETALL clears all. */
    ops[0] = (struct linux_sembuf){0, 32767, LINUX_SEM_UNDO};
    assert(op(id, ops, 1) == 0);
    current = &tasks[1]; ops[0].sem_op = -32767; ops[0].sem_flg = 0;
    assert(op(id, ops, 1) == 0);
    current = &tasks[0]; ops[0].sem_op = 2; ops[0].sem_flg = LINUX_SEM_UNDO;
    assert(op(id, ops, 1) == -LINUX_ERANGE && ctl(id, 0, LINUX_GETVAL, 0) == 0);
    task_sysv_sem_exit(current);
    assert(ctl(id, 0, LINUX_GETVAL, 0) == 0);
    ops[0].sem_op = 1; ops[1] = (struct linux_sembuf){1, 1, LINUX_SEM_UNDO};
    assert(op(id, ops, 2) == 0);
    values[0] = 2; values[1] = 3;
    assert(ctl(id, 0, LINUX_SETALL, (uintptr_t)values) == 0);
    task_sysv_sem_exit(current);
    assert(ctl(id, 0, LINUX_GETVAL, 0) == 2 && ctl(id, 1, LINUX_GETVAL, 0) == 3);
    struct linux_seminfo info;
    assert(ctl(0, 0, LINUX_SEM_INFO, (uintptr_t)&info) >= 0 && info.semusz == 1 && info.semaem == 2);
    assert(ctl(0, 0, LINUX_IPC_INFO, (uintptr_t)&info) >= 0 && info.semopm == 500 && info.semusz == 20);
    assert(ctl(id, 0, LINUX_IPC_RMID, 0) == 0);
    assert(!allocations);

    /* Large native arrays and vectors use real fallible allocations. */
    id = get(888, 300, LINUX_IPC_CREAT | 0600); assert(id >= 0);
    uint16_t large[300] = {0}; struct linux_sembuf many[65] = {{0}};
    fail_allocation = 1;
    assert(ctl(id, 0, LINUX_GETALL, (uintptr_t)large) == -LINUX_ENOMEM);
    assert(op(id, many, 65) == -LINUX_ENOMEM);
    assert(task_sysv_sem_clone(current, &tasks[3], CLONE_SYSVSEM) == -LINUX_ENOMEM);
    fail_allocation = 0;
    assert(op(id, many, 65) == 0);
    fault_at = (uintptr_t)large + 400;
    assert(ctl(id, 0, LINUX_SETALL, (uintptr_t)large) == -LINUX_EFAULT);
    fault_at = 0;
    assert(ctl(id, 0, LINUX_IPC_RMID, 0) == 0 && !allocations);
    puts("PASS kernel SysV semaphores: atomic rollback, blocked snapshots, timeouts, commands, undo sharing and removal");
    return 0;
}
#endif
