#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../kernel/reliefnt/tools/tests/modules/scheduler.h"
#include "sysv_shm_fixture_stubs.h"

static uint32_t fixture_cpu;
static uint32_t fixture_cpu_count = 2;
static uint64_t fixture_online_mask = 3;
static uint64_t fixture_us;

uint32_t smp_current_cpu(void) { return fixture_cpu; }
uint32_t smp_cpu_count(void) { return fixture_cpu_count; }
bool smp_cpu_online(uint32_t cpu)
{ return cpu < fixture_cpu_count && (fixture_online_mask & (1ULL << cpu)); }
uint64_t time_uptime_us(void) { return fixture_us; }
uint64_t time_ticks(void) { return fixture_us / 10000; }
uint64_t paging_kernel_cr3(void) { return 0x1000; }
void paging_load_cr3(uint64_t cr3) { assert(cr3 == 0x1000); }
void kernel_spin_init(struct kernel_spinlock *lock) { lock->state = 0; }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ assert(!lock->state); lock->state = 1; *flags = 0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)flags; assert(lock->state); lock->state = 0; }
void *kernel_malloc(size_t size) { return malloc(size); }
void kernel_free(void *memory) { free(memory); }
int kernel_signal_fatal_pending(const struct task *task)
{ (void)task; return 0; }
void console_log(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }

static struct task *start_task(uint32_t cpu, uint32_t pid)
{
    struct task *task = alloc_task_slot();
    assert(task);
    task->pid = pid;
    task->kind = TASK_KIND_USER;
    task->state = TASK_READY;
    task->running_cpu = SCHED_CPU_NONE;
    task->entry = RELIEFNT_USER_BASE + 0x1000;
    task->stack_top = RELIEFNT_USER_TOP - 4096;
    task->stack_low = task->stack_top - 4096;
    task->affinity_mask = 1ULL << cpu;
    sched_task_as(task)->cr3 = 0x2000;
    task->frame = (struct trap_frame){
        .rip = task->entry, .rsp = task->stack_low + 128,
        .cs = RELIEFNT_USER_CS, .ss = RELIEFNT_USER_DS,
        .rflags = 1ULL << 9,
    };
    fixture_cpu = cpu;
    assert(sched_select_next_user() == task);
    assert(task->state == TASK_RUNNING && task->running_cpu == cpu);
    return task;
}

static void discard_fixture(void)
{
    for (uint32_t i = 0; i < task_count; ++i) free(tasks[i]);
    free(tasks);
    tasks = NULL;
    task_count = task_capacity = 0;
}

/* A 4 Hz AP and a 100 Hz BSP must report the same duration for one busy second.
 * This mirrors the measured VMware timer mismatch without real hardware. */
static void test_time_does_not_depend_on_interrupt_count(unsigned ap_every)
{
    fixture_cpu = 0;
    fixture_us = 0;
    fixture_online_mask = 3;
    sched_init();
    struct task *bsp = start_task(0, 41);
    struct task *ap = start_task(1, 42);
    uint64_t busy_before[2], idle_before[2], busy_after[2], idle_after[2];
    sched_cpu_runtime_snapshot(busy_before, idle_before, NULL, NULL, 2);
    for (unsigned tick = 1; tick <= 100; ++tick) {
        fixture_us = tick * 10000ULL;
        fixture_cpu = 0;
        sched_on_tick();
        if (tick % ap_every == 0) {
            fixture_cpu = 1;
            sched_on_cpu_tick();
        }
    }
    sched_cpu_runtime_snapshot(busy_after, idle_after, NULL, NULL, 2);
    assert(busy_after[0] - busy_before[0] == 100);
    assert(busy_after[1] - busy_before[1] == 100);
    assert(idle_after[0] == idle_before[0] && idle_after[1] == idle_before[1]);
    assert(bsp->cpu_ticks == 100 && ap->cpu_ticks == 100);
    unsigned char fair_before[sizeof(ap->fair)];
    memcpy(fair_before, &ap->fair, sizeof(fair_before));

    /* Repeated IRQs and reads at the same instant cannot manufacture time. */
    for (unsigned i = 0; i < 100; ++i) sched_on_cpu_tick();
    sched_cpu_runtime_snapshot(busy_after, idle_after, NULL, NULL, 2);
    assert(busy_after[0] - busy_before[0] == 100);
    assert(busy_after[1] - busy_before[1] == 100);
    assert(bsp->cpu_ticks == 100 && ap->cpu_ticks == 100);
    assert(!memcmp(fair_before, &ap->fair, sizeof(fair_before)));
    uint64_t busy_total, idle_total;
    sched_cpu_ticks(&busy_total, &idle_total);
    assert(busy_total == busy_after[0] + busy_after[1]);
    assert(idle_total == idle_after[0] + idle_after[1]);
    assert(sched_task_cpu_ticks(ap, false) == 100);
    ap->tgid = bsp->pid;
    assert(sched_task_cpu_ticks(ap, true) == 200);
    discard_fixture();
}

static void advance_ticks(unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        fixture_us += 10000;
        fixture_cpu = 0;
        sched_on_tick();
    }
}

static void test_idle_reservation_and_migration(void)
{
    fixture_cpu = 0;
    fixture_us = 0;
    fixture_online_mask = 3;
    sched_init();
    /* A RUNNING pid-0 task is still idle; state alone is not ownership. */
    struct task *idle = alloc_task_slot();
    idle->state = TASK_RUNNING;
    idle->kind = TASK_KIND_KERNEL;
    idle->running_cpu = 0;
    struct task *task = start_task(0, 51);
    advance_ticks(10);
    fixture_cpu = 0;
    sched_block_current();
    advance_ticks(5);
    assert(task->state == TASK_BLOCKED && task->running_cpu == 0);
    assert(sched_task_cpu_ticks(task, false) == 15);
    fixture_cpu = 0;
    assert(sched_select_next_user() == NULL);
    task->affinity_mask = 2;
    sched_wake_interruptible(task);
    fixture_cpu = 1;
    assert(sched_select_next_user() == task);
    advance_ticks(10);
    uint64_t busy[2], idle_ticks[2];
    sched_cpu_runtime_snapshot(busy, idle_ticks, NULL, NULL, 2);
    assert(busy[0] == 15 && idle_ticks[0] == 10);
    assert(busy[1] == 10 && idle_ticks[1] == 15);
    assert(task->cpu_ticks == 25);
    /* EXITED but reserved remains an owner until the CPU retires it. */
    task->state = TASK_EXITED;
    advance_ticks(1);
    assert(task->cpu_ticks == 26);
    fixture_cpu = 1;
    assert(sched_select_next_user() == NULL);
    advance_ticks(1);
    assert(task->cpu_ticks == 26);
    discard_fixture();
}

static void test_offline_and_slot_reuse(void)
{
    fixture_cpu = 0;
    fixture_us = 0;
    fixture_online_mask = 1;
    sched_init();
    advance_ticks(20);
    uint64_t busy[2], idle[2];
    sched_cpu_ticks_per_cpu(busy, idle, 2);
    assert(!busy[0] && idle[0] == 20 && !busy[1] && !idle[1]);
    fixture_online_mask = 3;
    struct task *task = start_task(1, 61);
    advance_ticks(10);
    assert(task->cpu_ticks == 10);
    fixture_cpu = 1;
    task->state = TASK_EXITED;
    assert(sched_select_next_user() == NULL);
    task->flags |= TASK_FLAG_RESOURCES_RELEASED;
    struct task *reused = alloc_task_slot();
    assert(reused == task && !reused->cpu_ticks);
    sched_cpu_ticks_per_cpu(busy, idle, 2);
    assert(busy[1] == 10 && !idle[1]);
    /* AP events and clock interpolation alone cannot advance USER_HZ. */
    fixture_us += 999999999;
    sched_on_cpu_tick();
    sched_cpu_ticks_per_cpu(busy, idle, 2);
    assert(busy[1] == 10 && !idle[1]);
    discard_fixture();
}

int main(void)
{
    test_time_does_not_depend_on_interrupt_count(25);
    test_time_does_not_depend_on_interrupt_count(1);
    test_idle_reservation_and_migration();
    test_offline_and_slot_reuse();
    puts("PASS CPU accounting: BSP USER_HZ, AP cadence, idle, reservations, migration, snapshots and reuse");
    return 0;
}
