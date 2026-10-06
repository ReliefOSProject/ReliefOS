#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../kernel/reliefnt/kernel/reliefnt/sched/sched.c"
#include "sysv_shm_fixture_stubs.h"
#include "../../kernel/reliefnt/kernel/reliefnt/syscall_time.c"
#include "../../kernel/reliefnt/kernel/reliefnt/signal_queue.c"

static struct task members[3];
static struct task *table[] = {&members[0], &members[1], &members[2]};
static unsigned direct_calls;
uint32_t smp_current_cpu(void) { return 0; }
uint64_t time_ticks(void) { return scheduler_ticks; }
int time_clock_get(int32_t clock, struct linux_timespec *out)
{
    (void)clock;
    *out = (struct linux_timespec){scheduler_ticks / RELIEFNT_TICK_HZ,
        (scheduler_ticks % RELIEFNT_TICK_HZ) * (1000000000 / RELIEFNT_TICK_HZ)};
    return 0;
}
int user_copy_to_task(struct task *task, uint64_t address, const void *source, uint64_t size)
{
    (void)task;
    if (!user_range_ok(address, size)) return -LINUX_EFAULT;
    __builtin_memcpy((void *)(uintptr_t)address, source, size);
    return 0;
}
bool user_range_ok(uint64_t address, uint64_t size)
{ return address >= 4096 && address + size >= address; }
bool user_range_writable(uint64_t address, uint64_t size) { return user_range_ok(address, size); }
int kernel_signal_queue_task(struct task *task, int sig)
{ assert(task && sig); ++direct_calls; return 0; }
int kernel_signal_queue_task_info(struct task *task, int sig, const struct linux_siginfo *info)
{ assert(info); return kernel_signal_queue_task(task, sig); }
void *kernel_malloc(size_t size) { return malloc(size); }
void kernel_free(void *p) { free(p); }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ assert(!lock->state); lock->state = 1; *flags = 0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)flags; assert(lock->state); lock->state = 0; }

int main(void)
{
    tasks = table;
    task_count = 3;
    for (unsigned i = 0; i < 3; ++i) {
        members[i].pid = 10 + i;
        members[i].tgid = 10;
        members[i].kind = TASK_KIND_USER;
        members[i].state = TASK_BLOCKED;
        members[i].signal_actions[12].handler = 0x410000;
        if (i) {
            members[i].shared_process_pending = &members[0].process_pending_signals;
            members[i].shared_process_signal_queue = &members[0].process_signal_queue;
        }
    }
    const uint64_t bit = 1ULL << 11;
    members[0].blocked_signals = bit;
    members[1].blocked_signals = bit;
    assert(sched_signal_user_process(10, 12) == 0);
    assert(members[0].state == TASK_BLOCKED && members[1].state == TASK_BLOCKED);
    assert(members[2].state == TASK_READY && !direct_calls);
    for (unsigned i = 0; i < 3; ++i) assert(sched_task_pending(&members[i]) == bit && !members[i].pending_signals);
    members[0].state = TASK_EXITED;
    members[2].state = TASK_BLOCKED;
    assert(sched_signal_user_process(10, 12) == 0 && members[2].state == TASK_READY);
    assert(sched_signal_user_process(11, 12) == -1); /* A TID is not a process PID. */
    members[1].pending_signals = bit;
    sched_signal_discard(&members[2], 12);
    for (unsigned i = 0; i < 3; ++i) assert(!sched_task_pending(&members[i]));
    sched_signal_job_control(10, 19);
    assert(members[0].state == TASK_EXITED && members[1].state == TASK_STOPPED && members[2].state == TASK_STOPPED);
    members[1].pending_signals = 1ULL << 21;
    members[0].process_pending_signals = 1ULL << 18;
    sched_signal_job_control(10, 18);
    assert(members[1].state == TASK_READY && members[2].state == TASK_READY);
    assert(!sched_task_pending(&members[1]));
    /* Linux removes semop waiters before entering a group stop. */
    current_pid[0] = 11;
    int semid = syscall_sysv_sem(__NR_semget, 0, 1, 0600, 0);
    assert(semid >= 0);
    struct linux_sembuf semop = {0, -1, 0};
    const unsigned sem_calls[] = {__NR_semop, __NR_semtimedop};
    struct linux_timespec sem_timeout = {10, 0};
    for (unsigned i = 0; i < 2; ++i) {
        assert(syscall_sysv_sem(sem_calls[i], semid, (uintptr_t)&semop, 1,
                               (uintptr_t)&sem_timeout) == KERNEL_SYSCALL_BLOCKED);
        assert(members[1].sysv_sem.list);
        sched_signal_job_control(10, 19);
        assert(members[1].state == TASK_STOPPED && members[1].sysv_sem.completed);
        assert(!members[1].sysv_sem.list && members[1].sysv_sem.result == -LINUX_EINTR);
        sched_signal_job_control(10, 18);
        assert(syscall_sysv_sem(sem_calls[i], semid, 1, 1, 1) == -LINUX_EINTR);
        assert(!members[1].sysv_sem.array);
    }
    assert(syscall_sysv_sem(__NR_semop, semid, (uintptr_t)&semop, 1, 0) == KERNEL_SYSCALL_BLOCKED);
    current_pid[0] = 12;
    assert(syscall_sysv_sem(__NR_semctl, semid, 0, LINUX_SETVAL, 1) == 0);
    sched_signal_job_control(10, 19);
    assert(members[1].sysv_sem.completed && !members[1].sysv_sem.result);
    sched_signal_job_control(10, 18);
    current_pid[0] = 11;
    assert(syscall_sysv_sem(__NR_semop, semid, 1, 1, 0) == 0);
    assert(syscall_sysv_sem(__NR_semctl, semid, 0, LINUX_IPC_RMID, 0) == 0);
    scheduler_ticks = 100;
    assert(sched_alarm_task(&members[1], 2) == 0 && members[0].alarm_deadline == 300);
    scheduler_ticks = 140;
    assert(sched_alarm_task(&members[2], 1) == 2 && members[0].alarm_deadline == 240);
    scheduler_ticks = 239;
    assert(sched_alarm_task(&members[1], 0) == 1 && !members[0].alarm_deadline);
    assert(sched_alarm_task(&members[1], 1) == 0);
    members[1].blocked_signals = 1ULL << 13;
    members[1].state = members[2].state = TASK_BLOCKED;
    scheduler_ticks = 339;
    sched_alarm_expire();
    assert(!members[0].alarm_deadline && sched_task_pending(&members[1]) == (1ULL << 13));
    assert(members[1].state == TASK_BLOCKED && members[2].state == TASK_READY);
    assert(!direct_calls); /* Interrupt context only queues, never destroys a task. */
    current_pid[0] = 11;
    members[1].signalfd_waiting = true;
    members[1].signalfd_wait_mask = 1ULL << 13;
    sched_signal_wait_current(0);
    assert(members[1].state == TASK_READY); /* Pending while publishing sleep. */
    members[1].state = TASK_BLOCKED;
    sched_wake_process_signal(10, 1ULL << 13);
    assert(members[1].state == TASK_READY);
    struct task_file signalfd = {.kind = TASK_FILE_KIND_SIGNALFD, .aux = bit};
    members[1].syscall_file = &signalfd;
    members[1].state = TASK_BLOCKED;
    sched_signalfd_reconfigure(&members[1]);
    assert(members[1].state == TASK_READY && members[1].signalfd_wait_mask == bit);
    members[1].signalfd_waiting = false;
    members[1].signalfd_wait_mask = 0;
    members[1].syscall_file = NULL;
    struct linux_itimerval timer = {.it_interval.tv_usec = 40000, .it_value.tv_usec = 10000}, old;
    assert(syscall_itimer(true, 0, (uintptr_t)&timer, 1) == -LINUX_EFAULT);
    assert(members[0].alarm_deadline == 340); /* Bad output must not undo the update. */
    assert(syscall_itimer(false, 0, (uintptr_t)&old, 0) == 0);
    assert(old.it_interval.tv_usec == 40000 && old.it_value.tv_usec == 10000);
    timer.it_value.tv_usec = 1000000;
    assert(syscall_itimer(true, 0, (uintptr_t)&timer, 0) == -LINUX_EINVAL);
    assert(members[0].alarm_deadline == 340);
    scheduler_ticks = 349;
    sched_alarm_expire();
    assert(syscall_itimer(false, 0, (uintptr_t)&old, 0) == 0 && !old.it_value.tv_usec);
    sched_alarm_rearm(&members[1]);
    assert(members[0].alarm_deadline == 352);
    assert(syscall_itimer(true, 0, 0, (uintptr_t)&old) == 0);
    assert(!members[0].alarm_deadline && !members[0].alarm_interval_ns);
    assert(syscall_itimer(false, 99, 0, 0) == -LINUX_EINVAL);
    assert(syscall_itimer(false, 0, 0, 0) == -LINUX_EFAULT);
    /* The last exiting thread notifies once on behalf of the zombie leader. */
    members[0].pid = members[0].tgid = 10;
    members[0].state = TASK_EXITED;
    members[0].running_cpu = SCHED_CPU_NONE;
    members[0].parent_pid = 12;
    members[0].parent_exit_signal = 17;
    members[1].state = TASK_READY;
    members[1].running_cpu = SCHED_CPU_NONE;
    members[2].tgid = 12;
    members[2].shared_process_pending = NULL;
    members[2].shared_process_signal_queue = NULL;
    members[2].process_pending_signals = 0;
    members[2].signal_actions[17].handler = 0x410000;
    members[2].state = TASK_BLOCKED;
    sched_notify_parent_exit(&members[0]);
    assert(!members[0].parent_exit_notified && !members[2].process_pending_signals);
    members[1].state = TASK_EXITED;
    members[1].running_cpu = 0;
    sched_notify_parent_exit(&members[0]);
    assert(!members[0].parent_exit_notified);
    members[1].running_cpu = SCHED_CPU_NONE;
    sched_notify_parent_exit(&members[1]);
    assert(members[0].parent_exit_notified && members[2].process_pending_signals == (1ULL << 16));
    assert(members[2].state == TASK_READY);
    members[2].process_pending_signals = 0;
    sched_notify_parent_exit(&members[1]);
    assert(!members[2].process_pending_signals);
    /* Linux validates the native sigset width before accessing user memory. */
    assert(syscall_rt_sigtimedwait(0, 0, 0, 128) == -LINUX_EINVAL);
    /* An arrival before sleep publication must keep the waiter runnable. */
    current_pid[0] = 12;
    members[2].pending_signals = 1ULL << 34;
    members[2].blocked_signals = UINT64_MAX;
    members[2].sigwait_mask = 1ULL << 34;
    sched_signal_wait_current(0);
    assert(members[2].state == TASK_READY);
    members[2].pending_signals = 0;
    sched_signal_wait_current(scheduler_ticks);
    assert(members[2].state == TASK_READY);
    sched_signal_wait_current(scheduler_ticks + 10);
    assert(members[2].state == TASK_BLOCKED);
    struct linux_siginfo queued = {.signo = 35, .code = LINUX_SI_QUEUE,
        .fields.realtime.value = 0x123456789abcdef0ULL}, received;
    members[2].limits.sigpending.rlim_cur = 8;
    assert(sched_signal_user_process_info(12, 35, &queued) == 0);
    assert(members[2].state == TASK_READY);
    assert(kernel_signal_dequeue(&members[2], UINT64_MAX, &received) == 35);
    assert(received.fields.realtime.value == queued.fields.realtime.value);
    assert(!signal_accounts);
    members[0].limits.sigpending.rlim_cur = 8;
    members[1].shared_limits = &members[0].limits;
    members[1].state = TASK_READY;
    assert(kernel_signal_enqueue(&members[0], true, 35, &queued) == 0);
    assert(kernel_signal_enqueue(&members[0], false, 35, &queued) == 0);
    sched_release_task_signals(&members[0]);
    assert(!members[0].signal_queue.head && members[0].process_signal_queue.head);
    assert(kernel_signal_dequeue(&members[1], 1ULL << 34, &received) == 35);
    assert(!signal_accounts);
    assert(kernel_signal_enqueue(&members[0], true, 35, &queued) == 0);
    kernel_signal_detach_process(&members[1]);
    assert(!members[0].process_signal_queue.head && !members[0].process_pending_signals);
    assert(!members[1].shared_process_signal_queue && !members[1].shared_process_pending);
    assert(kernel_signal_dequeue(&members[1], 1ULL << 34, &received) == 35);
    assert(!signal_accounts);
    assert(kernel_signal_enqueue(&members[1], true, 35, &queued) == 0);
    members[1].state = TASK_EXITED;
    sched_release_task_signals(&members[1]);
    assert(!signal_accounts && !members[1].process_signal_queue.head);
    members[1].state = TASK_BLOCKED;
    members[1].running_cpu = 1;
    members[1].wake_tick = 999;
    sched_wake_interruptible(&members[1]);
    assert(members[1].state == TASK_READY && members[1].running_cpu == 1 && !members[1].wake_tick);
    members[1].state = TASK_STOPPED;
    sched_wake_interruptible(&members[1]);
    assert(members[1].state == TASK_STOPPED);
    members[1].state = TASK_EXITED;
    sched_wake_interruptible(&members[1]);
    assert(members[1].state == TASK_EXITED);
    /* Neither job control nor a caught per-thread timer may complete vfork. */
    members[1].state = TASK_BLOCKED;
    members[1].vfork_child = &members[2];
    members[2].vfork_parent = &members[1];
    sched_signal_job_control(members[1].tgid, 19);
    assert(members[1].state == TASK_BLOCKED);
    sched_signal_job_control(members[1].tgid, 18);
    assert(members[1].state == TASK_BLOCKED && members[1].vfork_child);
    members[1].signal_actions[10].handler = 0x410000;
    members[1].timers[0] = (struct task_posix_timer){.used = 1,
        .expiry_tick = scheduler_ticks, .notify = 4, .signo = 10, .target_pid = 11};
    sched_posix_timer_expire();
    assert(members[1].state == TASK_BLOCKED && members[1].vfork_child);
    assert(members[1].pending_signals & (1ULL << 9));
    sched_wake_interruptible(&members[1]);
    assert(members[1].state == TASK_BLOCKED);
    vfork_child_done_locked(&members[2]);
    assert(members[1].state == TASK_READY && !members[1].vfork_child);
    kernel_signal_flush(&members[1], false, UINT64_MAX);
    kernel_signal_flush(&members[1], true, UINT64_MAX);
    puts("PASS process signals/itimer/exit: shared pending, exited leader, stop/continue, alarm, periodic rearm, final-thread parent notification");
}
