#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/reliefnt/kernel/signal/signal.c"
#include "../../kernel/reliefnt/kernel/signal/signal_queue.c"
#include "../../kernel/reliefnt/kernel/syscall/time/time.c"

static struct task people[3], *current;
static uint64_t ticks = 100, inaccessible;
static unsigned allocations, exits;
static bool fail_alloc;
void power_reboot(void) { abort(); }
void power_shutdown(void) { abort(); }
void *kernel_malloc(size_t size)
{ if (fail_alloc) return NULL; void *p = malloc(size); if (p) ++allocations; return p; }
void kernel_free(void *p) { assert(allocations); --allocations; free(p); }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ assert(!lock->state); lock->state = 1; *flags = 0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)flags; assert(lock->state); lock->state = 0; }
struct task *sched_current_task(void) { return current; }
struct task *sched_find(uint32_t pid)
{ for (unsigned i = 0; i < 3; ++i) if (people[i].pid == pid) return &people[i]; return NULL; }
void sched_alarm_rearm(struct task *task) { (void)task; }
void sched_notify_runnable(void) { }
uint64_t time_ticks(void) { return ticks; }
bool user_range_ok(uint64_t p, uint64_t n)
{ return p >= 4096 && n <= UINT64_MAX - p && (!inaccessible || p + n <= inaccessible); }
bool user_range_writable(uint64_t p, uint64_t n) { return user_range_ok(p, n); }
void sched_block_current(void) { current->state = TASK_BLOCKED; }
void sched_sleep_current_until(uint64_t deadline) { current->wake_tick = deadline; sched_block_current(); }
void sched_signal_wait_current(uint64_t deadline) { sched_sleep_current_until(deadline); }
void sched_exit_group(uint32_t pid, uint64_t code)
{ (void)pid; (void)code; ++exits; }
void sched_signal_job_control(uint32_t pid, int sig) { (void)pid; (void)sig; }
void sched_signal_discard(struct task *task, int sig)
{
    kernel_signal_flush(task, true, 1ULL << (sig - 1));
    for (unsigned i = 0; i < 3; ++i)
        if (sched_task_tgid(&people[i]) == sched_task_tgid(task))
            kernel_signal_flush(&people[i], false, 1ULL << (sig - 1));
}
/* Process selection/wakeup is exercised against real sched.c in the adjacent test. */
int sched_signal_user_process_info(uint32_t pid, int sig, const struct linux_siginfo *info)
{ return kernel_signal_enqueue(sched_find(pid), true, sig, info); }

static int send_info(unsigned target, int sig, uint64_t value, bool thread)
{
    struct linux_siginfo info = {.signo = 1, .error = 7, .code = LINUX_SI_QUEUE,
        .fields.realtime = {.pid = 567, .uid = 890, .value = value}};
    return kernel_signal_queueinfo(people[target].tgid, people[target].pid,
        sig, (uintptr_t)&info, thread);
}

int main(void)
{
    for (unsigned i = 0; i < 3; ++i) {
        struct task *task = &people[i];
        task->pid = task->tgid = 10 + i;
        task->uid = task->euid = task->suid = 100;
        task->kind = TASK_KIND_USER;
        task->state = TASK_READY;
        task->blocked_signals = UINT64_MAX & ~((1ULL << 8) | (1ULL << 18));
        task->limits.sigpending = (struct linux_rlimit64){8, 8};
    }
    people[1].tgid = 10;
    people[1].shared_process_pending = &people[0].process_pending_signals;
    people[1].shared_process_signal_queue = &people[0].process_signal_queue;
    current = &people[0];
    struct linux_siginfo info, sent = {.code = LINUX_SI_QUEUE};
    assert(send_info(0, 35, 111, false) == 0);
    assert(send_info(0, 35, 222, false) == 0);
    assert(send_info(1, 35, 333, true) == 0);
    assert(kernel_signal_dequeue(&people[1], UINT64_MAX, &info) == 35);
    assert(info.fields.realtime.value == 333 && info.fields.realtime.pid == 567 && info.error == 7);
    assert(kernel_signal_dequeue(&people[1], UINT64_MAX, &info) == 35 && info.fields.realtime.value == 111);
    assert(kernel_signal_dequeue(&people[0], UINT64_MAX, &info) == 35 && info.fields.realtime.value == 222);
    assert(!kernel_signal_dequeue(current, UINT64_MAX, &info) && !allocations);
    assert(send_info(0, 12, 1, false) == 0 && send_info(0, 12, 2, false) == 0);
    assert(allocations == 1);
    assert(kernel_signal_dequeue(current, UINT64_MAX, &info) == 12 && info.fields.realtime.value == 1);
    /* Synchronous signals take priority, but private still precedes shared. */
    assert(send_info(0, 2, 2, false) == 0 && send_info(0, 11, 11, false) == 0);
    assert(send_info(0, 36, 36, true) == 0);
    assert(kernel_signal_dequeue(current, UINT64_MAX, &info) == 36);
    assert(kernel_signal_dequeue(current, UINT64_MAX, &info) == 11);
    assert(kernel_signal_dequeue(current, UINT64_MAX, &info) == 2);
    /* The UID charge is shared across processes and survives a UID change. */
    people[2].limits.sigpending.rlim_cur = 1;
    assert(send_info(0, 35, 1, true) == 0);
    current->uid = 200;
    assert(kernel_signal_enqueue(&people[2], false, 35, &sent) == -LINUX_EAGAIN);
    kernel_signal_flush(current, false, UINT64_MAX);
    assert(kernel_signal_enqueue(&people[2], false, 35, &sent) == 0);
    kernel_signal_flush(&people[2], false, UINT64_MAX);
    current->uid = 100;
    fail_alloc = true;
    assert(send_info(0, 35, 1, true) == -LINUX_EAGAIN && !current->pending_signals);
    assert(send_info(0, 12, 1, true) == 0);
    assert(kernel_signal_dequeue(current, UINT64_MAX, &info) == 12 && info.code == 0 && !info.fields.realtime.value);
    fail_alloc = false;
    /* Native copy ordering, 48-byte kernel layout, unknown extension checks. */
    assert(kernel_signal_queueinfo(0, 0, 99, 0, true) == -LINUX_EFAULT);
    assert(kernel_signal_queueinfo(0, 0, 99, (uintptr_t)&sent, true) == -LINUX_EINVAL);
    assert(kernel_signal_queueinfo(10, 12, 35, (uintptr_t)&sent, true) == -LINUX_ESRCH);
    sent.code = 0;
    assert(kernel_signal_queueinfo(12, 12, 0, (uintptr_t)&sent, false) == -LINUX_EPERM);
    sent.code = LINUX_SI_TKILL;
    assert(kernel_signal_queueinfo(10, 11, 0, (uintptr_t)&sent, true) == -LINUX_EPERM);
    sent.code = LINUX_SI_QUEUE;
    inaccessible = (uintptr_t)&sent + 48;
    assert(kernel_signal_queueinfo(10, 10, 0, (uintptr_t)&sent, true) == 0);
    sent.code = -99;
    assert(kernel_signal_queueinfo(10, 10, 0, (uintptr_t)&sent, true) == -LINUX_EFAULT);
    inaccessible = 0;
    ((unsigned char *)&sent)[127] = 1;
    assert(kernel_signal_queueinfo(10, 10, 0, (uintptr_t)&sent, true) == -LINUX_E2BIG);
    ((unsigned char *)&sent)[127] = 0;
    assert(kernel_signal_queueinfo(10, 10, 0, (uintptr_t)&sent, true) == 0);
    sent.code = LINUX_SI_QUEUE;
    people[2].uid = people[2].suid = 999;
    assert(kernel_signal_queueinfo(12, 12, 0, (uintptr_t)&sent, true) == -LINUX_EPERM);
    current->cap_effective = 1ULL << CAP_KILL;
    assert(kernel_signal_queueinfo(12, 12, 0, (uintptr_t)&sent, true) == 0);
    current->cap_effective = 0;
    assert(kernel_signal_queueinfo(12, 12, 18, (uintptr_t)&sent, true) == 0);
    assert(kernel_signal_dequeue(&people[2], UINT64_MAX, &info) == 18);
    people[2].uid = people[2].suid = 100;
    /* Setting SIG_IGN flushes all queues; a blocked ignored signal may pend. */
    assert(send_info(0, 35, 1, true) == 0 && send_info(1, 35, 2, true) == 0);
    assert(kernel_signal_set_action(current, 35, 1, 0, 0, 0, NULL) == 0);
    assert(!allocations);
    assert(send_info(0, 35, 3, true) == 0 && allocations == 1);
    kernel_signal_flush(current, false, UINT64_MAX);
    current->blocked_signals = 0;
    assert(send_info(0, 35, 4, true) == 0 && !allocations);
    current->blocked_signals = UINT64_MAX;
    /* Invalid timeout precedes dequeue; one wait consumes only one source. */
    uint64_t mask = 1ULL << 34;
    struct linux_timespec timeout = {.tv_nsec = -1};
    assert(send_info(0, 35, 5, true) == 0 && send_info(0, 35, 6, false) == 0);
    assert(syscall_rt_sigtimedwait((uintptr_t)&mask, 0, (uintptr_t)&timeout, 8) == -LINUX_EINVAL);
    assert(allocations == 2);
    timeout.tv_nsec = 0;
    assert(syscall_rt_sigtimedwait((uintptr_t)&mask, (uintptr_t)&info, (uintptr_t)&timeout, 8) == 35);
    assert(info.fields.realtime.value == 5 && allocations == 1);
    assert(syscall_rt_sigtimedwait((uintptr_t)&mask, 1, (uintptr_t)&timeout, 8) == -LINUX_EFAULT);
    assert(!allocations && !sched_task_pending(current));
    assert(send_info(0, 35, 77, true) == 0);
    memset(&info, 0, sizeof(info));
    inaccessible = (uintptr_t)&info + 48;
    /* Resume a wait to isolate the output fault from input addresses. */
    current->sigwait_active = 1;
    current->sigwait_mask = mask;
    assert(syscall_rt_sigtimedwait(0, (uintptr_t)&info, 0, 8) == -LINUX_EFAULT);
    assert(info.signo == 35 && info.fields.realtime.value == 77 && !allocations);
    inaccessible = 0;
    assert(syscall_rt_sigtimedwait((uintptr_t)&mask, 0, (uintptr_t)&timeout, 8) == -LINUX_EAGAIN);
    timeout.tv_nsec = 10000000;
    assert(syscall_rt_sigtimedwait((uintptr_t)&mask, 0, (uintptr_t)&timeout, 8) == KERNEL_SYSCALL_BLOCKED);
    assert(current->state == TASK_BLOCKED && current->sigwait_active && current->wake_tick == 101);
    assert(send_info(0, 35, 9, true) == 0 && current->state == TASK_READY);
    ticks = 101; /* A queued signal wins even at the deadline. */
    assert(syscall_rt_sigtimedwait(0, (uintptr_t)&info, 0, 8) == 35 && info.fields.realtime.value == 9);
    assert(!current->sigwait_active);
    mask = 0;
    timeout.tv_sec = INT64_MAX;
    assert(syscall_rt_sigtimedwait((uintptr_t)&mask, 0, (uintptr_t)&timeout, 8) == KERNEL_SYSCALL_BLOCKED);
    assert(current->sigwait_active && current->sigwait_deadline > ticks);
    current->blocked_signals &= ~(1ULL << 11);
    current->pending_signals = 1ULL << 11;
    assert(syscall_rt_sigtimedwait(0, 0, 0, 8) == -LINUX_EINTR && !current->sigwait_active);
    kernel_signal_flush(current, false, UINT64_MAX);
    assert(!allocations && !exits && !signal_accounts);
    puts("PASS siginfo: FIFO/coalescing, queue priority, per-UID limits, faults/permissions, ignored signals, sigtimedwait lifecycle");
}
