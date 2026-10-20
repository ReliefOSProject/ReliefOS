#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Both locks and the scheduler are production code. Only hardware, allocation
 * and unused device boundaries are supplied by this host fixture. */
#include "../../kernel/reliefnt/kernel/reliefnt/lock.c"
#include "../../kernel/reliefnt/kernel/reliefnt/sched/sched.c"
#include "sysv_shm_fixture_stubs.h"

static _Thread_local uint32_t cpu;
static _Thread_local uint64_t irq;
static atomic_bool waiting, entered, release_worker, finished;
static struct task *task;
static struct trap_frame incoming;

uint32_t smp_current_cpu(void) { return cpu; }
uint32_t smp_cpu_count(void) { return 2; }
bool smp_cpu_online(uint32_t index) { return index < 2; }
void smp_membarrier_poll(void) { atomic_store(&waiting, true); }
uint64_t kernel_irq_save(void) { uint64_t saved = irq; irq = 0; return saved; }
void kernel_irq_restore(uint64_t flags) { irq = flags; }
uint64_t time_uptime_us(void) { return 1000; }
uint64_t time_ticks(void) { return 0; }
uint64_t paging_kernel_cr3(void) { return 0x1000; }
void paging_load_cr3(uint64_t cr3) { assert(cr3 == 0x1000); }
void *kernel_malloc(size_t size) { return malloc(size); }
void kernel_free(void *memory) { free(memory); }
int kernel_signal_fatal_pending(const struct task *current)
{ (void)current; return 0; }
void console_log(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }

static bool await(atomic_bool *flag, unsigned ms)
{
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    do {
        if (atomic_load(flag)) return true;
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while ((now.tv_sec - start.tv_sec) * 1000000000LL + now.tv_nsec - start.tv_nsec < ms * 1000000LL);
    return atomic_load(flag);
}

static void *affinity_writer(void *unused)
{
    (void)unused;
    cpu = 1;
    uint64_t flags;
    kernel_execution_lock_irqsave(&flags);
    atomic_store(&entered, true);
    assert(!sched_set_task_affinity(task->pid, 2));
    kernel_execution_unlock_irqrestore(flags);
    return NULL;
}

static void *return_worker(void *unused)
{
    (void)unused;
    cpu = 0;
    uint64_t flags;
    assert(kernel_execution_try_read_lock_irqsave(&flags));
    assert(!sched_try_resume_current_user_frame(&incoming));
    kernel_execution_read_unlock_irqrestore(flags);
    atomic_store(&finished, true);
    return NULL;
}

static void *scheduler_writer(void *unused)
{
    (void)unused;
    cpu = 1;
    uint64_t flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    atomic_store(&entered, true);
    assert(await(&release_worker, 1000));
    /* Model a scheduler ownership update, protected by its actual lock. */
    task->running_cpu = 1;
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    return NULL;
}

int main(void)
{
    sched_init();
    task = alloc_task_slot();
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
    task->frame = (struct trap_frame){ .rip = task->entry,
        .rsp = task->stack_low + 128, .cs = RELIEFNT_USER_CS,
        .ss = RELIEFNT_USER_DS, .rflags = 1ULL << 9 };
    assert(sched_select_next_user() == task);
    incoming = task->frame;
    incoming.rax = 123;
    uint64_t flags;
    assert(kernel_execution_try_read_lock_irqsave(&flags));
    pthread_t writer;
    assert(!pthread_create(&writer, NULL, affinity_writer, NULL));
    assert(await(&waiting, 1000));
    assert(!atomic_load(&entered));
    assert(sched_try_resume_current_user_frame(&incoming));
    assert(task->frame.rax == 123 && task->running_cpu == 0);
    assert(!task->fair.runtime);
    cpu = 1;
    uint64_t denied;
    assert(!kernel_execution_try_read_lock_irqsave(&denied));
    cpu = 0;
    kernel_execution_read_unlock_irqrestore(flags);
    assert(!pthread_join(writer, NULL));
    assert(atomic_load(&entered));
    assert(kernel_execution_try_read_lock_irqsave(&flags));
    incoming.rax = 456;
    assert(!sched_try_resume_current_user_frame(&incoming));
    assert(task->frame.rax == 123); /* Remote affinity invalidates fast return. */
    kernel_execution_read_unlock_irqrestore(flags);
    assert(!sched_set_task_affinity(task->pid, 1));

    /* A remote reschedule must be visible even inside an admitted read gate. */
    assert(kernel_execution_try_read_lock_irqsave(&flags));
    cpu = 1;
    sched_request_reschedule_cpu(0);
    cpu = 0;
    assert(!sched_try_resume_current_user_frame(&incoming));
    assert(task->frame.rax == 123);
    kernel_execution_read_unlock_irqrestore(flags);
    __atomic_store_n(&sched_resched_pending[0], 0, __ATOMIC_RELEASE);

    atomic_store(&entered, false);
    assert(!pthread_create(&writer, NULL, scheduler_writer, NULL));
    assert(await(&entered, 1000));
    pthread_t reader;
    assert(!pthread_create(&reader, NULL, return_worker, NULL));
    assert(!await(&finished, 20));
    atomic_store(&release_worker, true);
    assert(!pthread_join(writer, NULL));
    assert(!pthread_join(reader, NULL));
    assert(atomic_load(&finished) && task->frame.rax == 123);
    assert(!execution_readers && execution_next_ticket == execution_serving_ticket);
    for (uint32_t i = 0; i < task_count; ++i) free(tasks[i]);
    free(tasks);
    puts("PASS real scheduler/execution locks: queued writer, affinity, reschedule and ownership publication");
    return 0;
}
