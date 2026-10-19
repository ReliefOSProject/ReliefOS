#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../kernel/reliefnt/kernel/reliefnt/sched/sched.c"
#include "sysv_shm_fixture_stubs.h"

/* Implemented by the scheduler fast-resume path. The caller holds the kernel
 * execution transaction, as userland_schedule_from_frame() does. */
extern bool sched_try_resume_current_user_frame(const struct trap_frame *frame);

static uint32_t fake_cpu;
static uint32_t fake_cpu_count = 1;
static uint64_t fake_time;

uint32_t smp_current_cpu(void) { return fake_cpu; }
uint32_t smp_cpu_count(void) { return fake_cpu_count; }
bool smp_cpu_online(uint32_t cpu) { return cpu < fake_cpu_count; }
uint64_t paging_kernel_cr3(void) { return 0x1000; }
void paging_load_cr3(uint64_t cr3) { assert(cr3 == 0x1000); }
uint64_t time_uptime_us(void) { return fake_time; }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ (void)lock; *flags = 0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)lock; (void)flags; }
void kernel_spin_init(struct kernel_spinlock *lock) { if (lock) lock->state = 0; }
int kernel_signal_fatal_pending(const struct task *task) { (void)task; return 0; }
void *kernel_malloc(size_t size) { return malloc(size); }
void kernel_free(void *memory) { free(memory); }
void console_log(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }

static void discard_fixture(void)
{
    if (tasks) {
        for (uint32_t i = 0; i < task_count; ++i) free(tasks[i]);
        free(tasks);
    }
    tasks = NULL;
    task_count = task_capacity = 0;
}

static struct task *start_fixture(void)
{
    discard_fixture();
    fake_cpu = 0;
    fake_cpu_count = 1;
    fake_time = 1000;
    sched_init();

    struct task *task = alloc_task_slot();
    assert(task);
    task->pid = 41;
    task->kind = TASK_KIND_USER;
    task->state = TASK_READY;
    task->running_cpu = SCHED_CPU_NONE;
    task->entry = RELIEFNT_USER_BASE + 0x1000;
    task->stack_top = RELIEFNT_USER_TOP - 4096;
    task->stack_low = task->stack_top - 4096;
    task->affinity_mask = 1;
    sched_task_as(task)->cr3 = 0x2000;
    task->frame = (struct trap_frame){
        .rip = task->entry,
        .rsp = task->stack_low + 128,
        .cs = RELIEFNT_USER_CS,
        .ss = RELIEFNT_USER_DS,
        .rflags = 1ULL << 9,
    };

    assert(sched_select_next_user() == task);
    assert(task->state == TASK_RUNNING);
    assert(task->running_cpu == fake_cpu);
    assert(fair_running[fake_cpu] == task);
    return task;
}

static struct trap_frame next_syscall_frame(const struct task *task, uint64_t sequence)
{
    struct trap_frame frame = task->frame;
    frame.rip = task->entry + 2 * sequence;
    frame.rax = sequence;
    frame.rdi = sequence + 10;
    return frame;
}

static void assert_rejected_without_mutation(struct task *task,
                                             const struct trap_frame *frame)
{
    const struct trap_frame saved = task->frame;
    enum task_state state = task->state;
    uint32_t running_cpu = task->running_cpu;
    uint64_t started = fair_started[fake_cpu];
    struct task *running = fair_running[fake_cpu];

    assert(!sched_try_resume_current_user_frame(frame));
    assert(task->state == state);
    assert(task->running_cpu == running_cpu);
    assert(task->frame.rip == saved.rip);
    assert(task->frame.rsp == saved.rsp);
    assert(task->frame.rax == saved.rax);
    assert(fair_running[fake_cpu] == running);
    assert(fair_started[fake_cpu] == started);
}

static void test_repeated_fast_returns_keep_fair_accounting(void)
{
    struct task *task = start_fixture();
    uint64_t initial_runtime = task->fair.runtime;

    for (uint64_t i = 1; i <= 3; ++i) {
        fake_time += 100;
        struct trap_frame frame = next_syscall_frame(task, i);
        assert(sched_try_resume_current_user_frame(&frame));
        assert(task->state == TASK_RUNNING);
        assert(task->running_cpu == fake_cpu);
        assert(fair_running[fake_cpu] == task);
        assert(task->frame.rip == frame.rip);
        assert(task->frame.rsp == frame.rsp);
        assert(task->frame.rax == frame.rax);
    }

    /* A real scheduler boundary must charge the whole interval since the task
     * was selected, including time spanning all of the fast returns. */
    fake_time += 200;
    sched_request_reschedule_cpu(fake_cpu);
    struct trap_frame frame = next_syscall_frame(task, 4);
    assert_rejected_without_mutation(task, &frame);
    assert(sched_capture_current_user_frame(&frame));
    assert(task->state == TASK_READY);
    assert(fair_running[fake_cpu] == NULL);
    assert(sched_select_next_user() == task);
    assert(task->fair.runtime == initial_runtime + 500);
}

static void test_reschedule_signal_state_and_frame_guards(void)
{
    struct task *task = start_fixture();
    struct trap_frame frame = next_syscall_frame(task, 1);

    sched_request_reschedule_cpu(fake_cpu);
    assert_rejected_without_mutation(task, &frame);
    __atomic_store_n(&sched_resched_pending[fake_cpu], 0, __ATOMIC_RELEASE);

    task->state = TASK_BLOCKED;
    assert_rejected_without_mutation(task, &frame);
    task->state = TASK_RUNNING;

    task->state = TASK_EXITED;
    assert_rejected_without_mutation(task, &frame);
    task->state = TASK_RUNNING;

    task->flags |= TASK_FLAG_PENDING_LOAD;
    assert_rejected_without_mutation(task, &frame);
    task->flags &= ~TASK_FLAG_PENDING_LOAD;

    task->pending_signals = 1;
    task->blocked_signals = 0;
    assert_rejected_without_mutation(task, &frame);
    task->pending_signals = 0;

    task->process_pending_signals = 1ULL << 5;
    assert_rejected_without_mutation(task, &frame);
    assert(task->process_pending_signals == (1ULL << 5));
    task->process_pending_signals = 0;

    uint32_t saved_current_pid = current_pid[fake_cpu];
    current_pid[fake_cpu] = task->pid + 1;
    assert_rejected_without_mutation(task, &frame);
    current_pid[fake_cpu] = saved_current_pid;

    uint32_t saved_running_cpu = task->running_cpu;
    task->running_cpu = SCHED_CPU_NONE;
    assert_rejected_without_mutation(task, &frame);
    task->running_cpu = saved_running_cpu;

    task->affinity_mask = 2;
    assert_rejected_without_mutation(task, &frame);
    task->affinity_mask = 1;

    frame.rip = RELIEFNT_USER_BASE - 1;
    assert_rejected_without_mutation(task, &frame);
    frame = next_syscall_frame(task, 1);
    frame.cs = 0x8;
    assert_rejected_without_mutation(task, &frame);

    assert(!sched_try_resume_current_user_frame(NULL));
}

static void test_blocked_pending_signals_preserve_fast_resume(void)
{
    const uint64_t blocked_signal = 1ULL << 5;
    struct task *task = start_fixture();

    task->blocked_signals = blocked_signal;
    task->pending_signals = blocked_signal;
    struct trap_frame task_pending_frame = next_syscall_frame(task, 1);
    assert(sched_try_resume_current_user_frame(&task_pending_frame));
    assert(task->state == TASK_RUNNING);
    assert(task->running_cpu == fake_cpu);
    assert(fair_running[fake_cpu] == task);
    assert(task->frame.rip == task_pending_frame.rip);
    assert(task->pending_signals == blocked_signal);
    assert(task->process_pending_signals == 0);

    task->pending_signals = 0;
    task->process_pending_signals = blocked_signal;
    struct trap_frame process_pending_frame = next_syscall_frame(task, 2);
    assert(sched_try_resume_current_user_frame(&process_pending_frame));
    assert(task->state == TASK_RUNNING);
    assert(task->running_cpu == fake_cpu);
    assert(fair_running[fake_cpu] == task);
    assert(task->frame.rip == process_pending_frame.rip);
    assert(task->pending_signals == 0);
    assert(task->process_pending_signals == blocked_signal);
}

static void test_current_reschedule_request_is_local_and_fair(void)
{
    struct task *current = start_fixture();
    uint64_t initial_runtime = current->fair.runtime;
    uint64_t initial_deadline = current->fair.deadline;

    struct task *competitor = alloc_task_slot();
    assert(competitor);
    competitor->pid = 42;
    competitor->kind = TASK_KIND_USER;
    competitor->state = TASK_READY;
    competitor->running_cpu = SCHED_CPU_NONE;
    competitor->entry = RELIEFNT_USER_BASE + 0x2000;
    competitor->stack_top = RELIEFNT_USER_TOP - 8192;
    competitor->stack_low = competitor->stack_top - 4096;
    competitor->affinity_mask = 1;
    sched_task_as(competitor)->cr3 = 0x3000;
    competitor->frame = (struct trap_frame){
        .rip = competitor->entry,
        .rsp = competitor->stack_low + 128,
        .cs = RELIEFNT_USER_CS,
        .ss = RELIEFNT_USER_DS,
        .rflags = 1ULL << 9,
    };

    fake_cpu_count = 2;
    __atomic_store_n(&sched_resched_pending[0], 0, __ATOMIC_RELEASE);
    __atomic_store_n(&sched_resched_pending[1], 0, __ATOMIC_RELEASE);
    fake_time += 2u * EEVDF_SLICE_US;
    struct trap_frame frame = next_syscall_frame(current, 1);

    sched_request_reschedule_current();
    sched_request_reschedule_current();
    assert(sched_current_cpu_needs_resched());
    assert(!__atomic_load_n(&sched_resched_pending[1], __ATOMIC_ACQUIRE));
    assert(current->state == TASK_RUNNING && current->running_cpu == fake_cpu);
    assert(current_task[fake_cpu] == current && fair_running[fake_cpu] == current);
    assert(current->fair.runtime == initial_runtime &&
           current->fair.deadline == initial_deadline);
    assert(!sched_try_resume_current_user_frame(&frame));
    assert(current->state == TASK_RUNNING && current->running_cpu == fake_cpu);
    assert(current->fair.runtime == initial_runtime &&
           current->fair.deadline == initial_deadline);

    assert(sched_capture_current_user_frame(&frame));
    assert(current->state == TASK_READY && current->fair.runtime ==
           initial_runtime + 2u * EEVDF_SLICE_US);
    assert(current->fair.deadline != initial_deadline);
    assert(sched_select_next_user() == competitor);
    assert(sched_current_pid() == competitor->pid);
    assert(competitor->state == TASK_RUNNING && competitor->running_cpu == fake_cpu);
    assert(current->state == TASK_READY && current->running_cpu == SCHED_CPU_NONE);
    assert(!sched_current_cpu_needs_resched());
    assert(!__atomic_load_n(&sched_resched_pending[1], __ATOMIC_ACQUIRE));
}

int main(void)
{
    test_repeated_fast_returns_keep_fair_accounting();
    test_reschedule_signal_state_and_frame_guards();
    test_blocked_pending_signals_preserve_fast_resume();
    test_current_reschedule_request_is_local_and_fair();
    discard_fixture();
    puts("PASS scheduler fast resume preserves RUNNING ownership and EEVDF runtime; deliverable signals fall back while blocked task/process signals remain pending");
    return 0;
}
