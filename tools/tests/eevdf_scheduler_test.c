#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../kernel/reliefnt/kernel/reliefnt/sched/sched.c"
#include "sysv_shm_fixture_stubs.h"

_Static_assert(sizeof(struct task) == 104176, "task size changed with EEVDF tree integration");
_Static_assert(__builtin_offsetof(struct task, fair) == 0, "task fair offset changed");
_Static_assert(sizeof(((struct task *)0)->fair) == 64, "task fair size changed");
_Static_assert(__builtin_offsetof(struct task, priority) == 5856, "task priority offset changed");
_Static_assert(__builtin_offsetof(struct task, frame) == 101368, "task frame offset changed");
_Static_assert(__builtin_offsetof(struct task, running_cpu) == 104160,
               "task running_cpu offset changed");

static uint32_t fake_cpu;
static uint64_t fake_time;
static bool fail_kernel_allocation;
uint32_t smp_current_cpu(void) { return fake_cpu; }
uint32_t smp_cpu_count(void) { return 2; }
static bool offline_ap;
bool smp_cpu_online(uint32_t cpu) { return cpu < 2 && !(cpu == 1 && offline_ap); }
uint64_t paging_kernel_cr3(void) { return 0x1000; }
void paging_load_cr3(uint64_t cr3)
{
    assert(cr3 == 0x1000);
    struct task *old = sched_find(current_pid[fake_cpu]);
    if (old && old->pid) assert(old->running_cpu == fake_cpu);
}
uint64_t time_uptime_us(void) { return fake_time; }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ (void)lock; *flags = 0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)lock; (void)flags; }
int kernel_signal_fatal_pending(const struct task *task) { (void)task; return 0; }
void *kernel_malloc(size_t size)
{ return fail_kernel_allocation ? NULL : malloc(size); }
void kernel_free(void *memory) { free(memory); }

static void finish(struct task *task, uint64_t elapsed)
{
    fake_time += elapsed;
    assert(sched_capture_current_user_frame(&task->frame));
    assert(task->state == TASK_READY && task->running_cpu == fake_cpu);
}

int main(void)
{
    task_count = 3;
    task_capacity = 4;
    tasks = calloc(task_capacity, sizeof(*tasks));
    for (unsigned i = 0; i < task_count; ++i) {
        struct sched_task_object *object = calloc(1, sizeof(*object));
        assert(object);
        tasks[i] = &object->task;
        task_zero(tasks[i]);
        tasks[i]->pid = i + 1;
        tasks[i]->kind = TASK_KIND_USER;
        tasks[i]->state = TASK_READY;
        tasks[i]->running_cpu = SCHED_CPU_NONE;
        tasks[i]->entry = RELIEFNT_USER_BASE;
        tasks[i]->stack_top = RELIEFNT_USER_TOP - 4096;
        tasks[i]->stack_low = tasks[i]->stack_top - 4096;
        tasks[i]->as.cr3 = 4096;
        tasks[i]->frame = (struct trap_frame){.rip = tasks[i]->entry,
            .rsp = tasks[i]->stack_top - 16, .cs = RELIEFNT_USER_CS,
            .ss = RELIEFNT_USER_DS, .rflags = 1ULL << 9};
        tasks[i]->affinity_mask = 1;
    }
    fail_kernel_allocation = true;
    assert(!alloc_task_slot() && task_count == 3);
    fail_kernel_allocation = false;
    struct sched_task_object *clone_object = calloc(1, sizeof(*clone_object));
    assert(clone_object);
    struct task *clone_task = &clone_object->task;
    *clone_task = *tasks[0];
    task_fair_reset(clone_task);
    assert(clone_task->fair.node == &clone_object->fair_node);
    assert(clone_task->fair.node != tasks[0]->fair.node);
    assert(!clone_task->fair.initialized && !clone_task->fair.queued);
    free(clone_object);
    tasks[1]->priority = 5;
    tasks[2]->affinity_mask = 2;
    offline_ap = true;
    assert(fair_choose_cpu(tasks[2]) == SCHED_CPU_NONE);
    offline_ap = false;
    fake_cpu = 0;
    struct task *a = sched_select_next_user();
    assert(a && a != tasks[2]);
    fake_cpu = 1;
    struct task *b = sched_select_next_user();
    assert(b == tasks[2] && b != a);
    assert(a->running_cpu == 0 && b->running_cpu == 1);
    /* The BSP's scheduler round must not consume the AP's local preemption
     * request, even when the BSP tick runs first. */
    fake_cpu = 0;
    sched_on_tick();
    assert(sched_current_cpu_needs_resched());
    finish(a, 0);
    a = sched_select_next_user();
    assert(a && a->running_cpu == 0);
    assert(!sched_current_cpu_needs_resched());
    sched_yield_current();
    assert(sched_current_cpu_needs_resched());
    fake_cpu = 1;
    assert(!sched_current_cpu_needs_resched());
    fake_cpu = 0;
    finish(a, 0);
    a = sched_select_next_user();
    assert(a && a->running_cpu == 0);
    assert(!sched_current_cpu_needs_resched());
    fake_cpu = 1;
    sched_on_cpu_tick();
    assert(sched_current_cpu_needs_resched());
    fake_cpu = 0;
    assert(!sched_current_cpu_needs_resched());
    fake_cpu = 1;
    finish(b, 0);
    assert(sched_select_next_user() == b);
    assert(!sched_current_cpu_needs_resched());
    finish(b, 100);
    /* A captured frame remains reserved until its owner retires CR3. */
    tasks[2]->affinity_mask = 3;
    fake_cpu = 0;
    assert(b->running_cpu == 1);
    tasks[2]->affinity_mask = 2;
    fake_cpu = 1;
    tasks[2]->state = TASK_BLOCKED;
    fake_cpu = 0;
    sched_mark_ready(b->pid);
    assert(b->state == TASK_READY && b->running_cpu == 1);
    assert(sched_current_cpu_needs_resched());
    fake_cpu = 1;
    assert(sched_current_cpu_needs_resched());
    tasks[2]->state = TASK_BLOCKED;
    fake_cpu = 1;
    assert(!sched_select_next_user());
    assert(b->running_cpu == SCHED_CPU_NONE && current_pid[1] == 0);
    tasks[2]->state = TASK_READY;
    fake_cpu = 0;
    finish(a, 100);
    for (unsigned i = 0; i < 10000; ++i) {
        a = sched_select_next_user();
        assert(a && a != tasks[2]);
        finish(a, 1000);
    }
    assert(tasks[0]->fair.runtime > 7300000 && tasks[0]->fair.runtime < 7700000);
    assert(tasks[1]->fair.runtime > 2300000 && tasks[1]->fair.runtime < 2700000);
    /* Blocking all local tasks must not prevent an idle CPU stealing work. */
    tasks[0]->state = tasks[1]->state = TASK_BLOCKED;
    tasks[2]->affinity_mask = 3;
    a = sched_select_next_user();
    assert(a == tasks[2] && a->running_cpu == 0);
    finish(a, 1000);
    assert(sched_set_task_affinity(a->pid, 2) == 0);
    assert(!sched_select_next_user());
    /* Reweighting a dequeued sleeper conserves its stored service credit. */
    struct task *sleeper = tasks[0];
    eevdf_dequeue(&fair_queues[sleeper->fair.cpu], &sleeper->fair);
    sleeper->fair.lag = 1000;
    assert(sched_task_priority(sleeper->pid, 5, 1) == 5);
    assert(!sleeper->fair.queued && sleeper->state == TASK_BLOCKED);
    assert(sleeper->fair.lag == 1000 * 1024 / 335);
    sleeper->state = TASK_READY;
    fake_cpu = 0;
    assert(sched_select_next_user() == sleeper);
    assert(sleeper->fair.weight == 335);
    finish(sleeper, 1000);
    fake_cpu = 1;
    assert(sched_select_next_user() == a && a->running_cpu == 1);
    finish(a, 1000);
    a->state = TASK_EXITED;
    assert(!sched_select_next_user());
    a->running_cpu = SCHED_CPU_NONE;
    a->flags = TASK_FLAG_RESOURCES_RELEASED;
    a->parent_pid = 0;
    struct eevdf_node *recycled_node = &((struct sched_task_object *)a)->fair_node;
    assert(alloc_task_slot() == a);
    assert(a->fair.node == recycled_node && !a->fair.initialized && !a->fair.queued);
    for (unsigned i = 0; i < task_count; ++i) { fair_forget(tasks[i]); free(tasks[i]); }
    free(tasks);
    assert(!fair_queues[0].root && !fair_queues[0].nr_active &&
           !fair_queues[1].root && !fair_queues[1].nr_active);
    puts("PASS scheduler EEVDF integration: per-CPU preemption, wake propagation, nice shares, affinity migration, block/steal, exit");
}
