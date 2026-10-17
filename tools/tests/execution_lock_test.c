#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <reliefnt/lock.h>

static _Thread_local uint32_t cpu;
static _Thread_local uint64_t irq_flags = 1u << 9;
static atomic_int entered, release_worker, polling, active_readers, active_writers;
static atomic_int suspended_writer_entered, suspended_writer_release;
static atomic_int suspended_owner_count;
static uint32_t suspended_payload;

uint32_t smp_current_cpu(void) { return cpu; }
void smp_membarrier_poll(void) { atomic_store(&polling, 1); }
uint64_t kernel_irq_save(void) { uint64_t f = irq_flags; irq_flags = 0; return f; }
void kernel_irq_restore(uint64_t f) { irq_flags = f; }

static void wait_for(atomic_int *flag)
{
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!atomic_load(flag)) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        assert(now.tv_sec - start.tv_sec < 3 && "lock progress timed out");
    }
}

static void *reader(void *unused)
{
    (void)unused;
    cpu = 1;
    uint64_t flags;
    assert(kernel_execution_try_read_lock_irqsave(&flags));
    assert(!irq_flags);
    atomic_store(&entered, 1);
    wait_for(&release_worker);
    kernel_execution_read_unlock_irqrestore(flags);
    assert(irq_flags == (1u << 9));
    return NULL;
}

static void *writer(void *unused)
{
    (void)unused;
    cpu = 1;
    uint64_t flags;
    kernel_execution_lock_irqsave(&flags);
    atomic_store(&entered, 1);
    wait_for(&release_worker);
    kernel_execution_unlock_irqrestore(flags);
    return NULL;
}

static void *suspended_writer(void *unused)
{
    (void)unused;
    cpu = 2;
    uint64_t flags;
    kernel_execution_lock_irqsave(&flags);
    assert(!irq_flags);
    assert(atomic_fetch_add_explicit(&suspended_owner_count, 1,
                                     memory_order_relaxed) == 0);
    atomic_store(&suspended_writer_entered, 1);
    wait_for(&suspended_writer_release);
    suspended_payload = 0x51a7c0deu;
    assert(atomic_fetch_sub_explicit(&suspended_owner_count, 1,
                                     memory_order_relaxed) == 1);
    kernel_execution_unlock_irqrestore(flags);
    assert(irq_flags == (1u << 9));
    return NULL;
}

static void *release_suspended_writer(void *unused)
{
    (void)unused;
    cpu = 3;
    /* Main reaches this poll only after resume has queued behind the writer. */
    wait_for(&polling);
    assert(atomic_load_explicit(&suspended_owner_count, memory_order_relaxed) == 1);
    atomic_store(&suspended_writer_release, 1);
    return NULL;
}

static void *stress(void *argument)
{
    cpu = (uintptr_t)argument;
    for (unsigned i = 0; i < 20000; ++i) {
        uint64_t flags;
        if (i % 3 && kernel_execution_try_read_lock_irqsave(&flags)) {
            atomic_fetch_add(&active_readers, 1);
            assert(!atomic_load(&active_writers));
            atomic_fetch_sub(&active_readers, 1);
            kernel_execution_read_unlock_irqrestore(flags);
        } else {
            kernel_execution_lock_irqsave(&flags);
            assert(!atomic_fetch_add(&active_writers, 1));
            assert(!atomic_load(&active_readers));
            uint64_t nested;
            kernel_execution_lock_irqsave(&nested);
            kernel_execution_unlock_irqrestore(nested);
            assert(!irq_flags);
            atomic_fetch_sub(&active_writers, 1);
            kernel_execution_unlock_irqrestore(flags);
        }
        assert(irq_flags == (1u << 9));
    }
    return NULL;
}

int main(void)
{
    pthread_t thread;
    uint64_t flags, other;
    /* Independent memory transactions must overlap, unlike the old mutex. */
    assert(kernel_execution_try_read_lock_irqsave(&flags));
    assert(pthread_create(&thread, NULL, reader, NULL) == 0);
    wait_for(&entered);
    atomic_store(&release_worker, 1);
    assert(pthread_join(thread, NULL) == 0);
    kernel_execution_read_unlock_irqrestore(flags);

    /* A queued writer closes admission, then drains readers before entry. */
    atomic_store(&entered, 0);
    atomic_store(&release_worker, 0);
    atomic_store(&polling, 0);
    assert(kernel_execution_try_read_lock_irqsave(&flags));
    assert(pthread_create(&thread, NULL, writer, NULL) == 0);
    wait_for(&polling);
    assert(!atomic_load(&entered));
    assert(!kernel_execution_try_read_lock_irqsave(&other));
    assert(!irq_flags);
    kernel_execution_read_unlock_irqrestore(flags);
    wait_for(&entered);
    assert(!kernel_execution_try_read_lock_irqsave(&other));
    assert(irq_flags == (1u << 9));
    atomic_store(&release_worker, 1);
    assert(pthread_join(thread, NULL) == 0);

    /* Suspend releases the ticket to a writer; resume queues behind it and
     * observes the writer's protected data before restoring the transaction. */
    atomic_store(&polling, 0);
    atomic_store(&suspended_writer_entered, 0);
    atomic_store(&suspended_writer_release, 0);
    atomic_store(&suspended_owner_count, 0);
    suspended_payload = 0;
    assert(irq_flags == (1u << 9));
    kernel_execution_lock_irqsave(&flags);
    assert(!irq_flags);
    assert(atomic_fetch_add_explicit(&suspended_owner_count, 1,
                                     memory_order_relaxed) == 0);
    struct kernel_execution_suspend_token token = {0};
    assert(atomic_fetch_sub_explicit(&suspended_owner_count, 1,
                                     memory_order_relaxed) == 1);
    assert(kernel_execution_suspend_irqrestore(&token) == 0);
    assert(token.active && irq_flags == (1u << 9));

    pthread_t suspended_thread, release_thread;
    assert(pthread_create(&suspended_thread, NULL, suspended_writer, NULL) == 0);
    wait_for(&suspended_writer_entered);
    assert(atomic_load_explicit(&suspended_owner_count, memory_order_relaxed) == 1);
    assert(pthread_create(&release_thread, NULL, release_suspended_writer, NULL) == 0);
    assert(kernel_execution_resume_irqsave(&token) == 0);
    assert(!token.active && !irq_flags);
    assert(atomic_load_explicit(&suspended_owner_count, memory_order_relaxed) == 0);
    assert(suspended_payload == 0x51a7c0deu);
    kernel_execution_unlock_irqrestore(flags);
    assert(irq_flags == (1u << 9));
    assert(pthread_join(suspended_thread, NULL) == 0);
    assert(pthread_join(release_thread, NULL) == 0);

    pthread_t workers[4];
    for (uintptr_t i = 0; i < 4; ++i)
        assert(pthread_create(&workers[i], NULL, stress, (void *)i) == 0);
    for (unsigned i = 0; i < 4; ++i) assert(pthread_join(workers[i], NULL) == 0);
    puts("PASS execution gate: concurrent readers, writer exclusion/progress, recursive writers, IRQ restoration");
}
