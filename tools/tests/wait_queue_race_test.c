/* Concurrent host fixture for the production wait-queue implementation. */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../kernel/reliefnt/kernel/sync/wait.c"

#define CONCURRENT_TASKS 24u
#define REMOVE_WAKE_ROUNDS 64u

static _Thread_local struct task *current_task;

/* The spinlock is an actual acquire/release lock. The irqsave contract is a
 * no-op on the host; the queue's mutual exclusion and publication stay real. */
static atomic_bool notifier_active;
static atomic_bool notifier_lock_contended;
static pthread_mutex_t race_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t race_cond = PTHREAD_COND_INITIALIZER;
static bool race_sleep_entered;
static atomic_bool race_wait_for_notify;
static bool race_block_before_wait;
static atomic_uint sleep_call_count;

void kernel_spin_init(struct kernel_spinlock *lock)
{
    __atomic_store_n(&lock->state, 0u, __ATOMIC_RELAXED);
}

void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{
    *flags = 0;
    for (;;) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&lock->state, &expected, 1u, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return;

        /* The poll race scheduler stub waits for proof that notify has passed
         * its generation increment and is contending on the real queue lock. */
        if (atomic_load_explicit(&notifier_active, memory_order_acquire) &&
            !atomic_exchange_explicit(&notifier_lock_contended, true,
                                      memory_order_acq_rel)) {
            pthread_mutex_lock(&race_mutex);
            pthread_cond_broadcast(&race_cond);
            pthread_mutex_unlock(&race_mutex);
        }
        sched_yield();
    }
}

void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{
    (void)flags;
    __atomic_store_n(&lock->state, 0u, __ATOMIC_RELEASE);
}

struct task *sched_current_task(void)
{
    return current_task;
}

void sched_block_current(void)
{
    if (current_task && current_task->state != TASK_EXITED)
        __atomic_store_n(&current_task->state, TASK_BLOCKED, __ATOMIC_SEQ_CST);
}

void sched_sleep_current_until(uint64_t wake_tick)
{
    (void)wake_tick;
    atomic_fetch_add_explicit(&sleep_call_count, 1u, memory_order_relaxed);

    if (!atomic_load_explicit(&race_wait_for_notify, memory_order_acquire)) {
        sched_block_current();
        return;
    }

    pthread_mutex_lock(&race_mutex);
    race_sleep_entered = true;
    pthread_cond_broadcast(&race_cond);
    pthread_mutex_unlock(&race_mutex);

    if (race_block_before_wait) {
        pthread_mutex_lock(&race_mutex);
        while (!atomic_load_explicit(&notifier_lock_contended,
                                     memory_order_acquire))
            pthread_cond_wait(&race_cond, &race_mutex);
        pthread_mutex_unlock(&race_mutex);
        sched_block_current();
        return;
    }

    sched_block_current();
    pthread_mutex_lock(&race_mutex);
    while (!atomic_load_explicit(&notifier_lock_contended,
                                 memory_order_acquire))
        pthread_cond_wait(&race_cond, &race_mutex);
    pthread_mutex_unlock(&race_mutex);
}

void sched_wake_interruptible(struct task *task)
{
    if (!task || task->vfork_child) return;
    enum task_state expected = TASK_BLOCKED;
    (void)__atomic_compare_exchange_n(&task->state, &expected, TASK_READY, false,
                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static void task_init(struct task *task, uint32_t pid, enum task_state state)
{
    memset(task, 0, sizeof(*task));
    task->pid = pid;
    task->kind = TASK_KIND_USER;
    task->state = state;
}

static int task_is_registered(struct kernel_wait_queue *queue,
                              struct task *task)
{
    uint64_t flags;
    int found = 0;
    kernel_spin_lock_irqsave(&queue->lock, &flags);
    for (uint32_t i = 0; i < queue->count; ++i) {
        if (queue->waiters[i] == task) {
            found = 1;
            break;
        }
    }
    kernel_spin_unlock_irqrestore(&queue->lock, flags);
    return found;
}

static int test_poll_stale_generation_does_not_sleep(void)
{
    struct task task;
    task_init(&task, 10u, TASK_RUNNING);
    current_task = &task;
    uint64_t stale_generation = kernel_poll_readiness_generation();
    unsigned sleeps_before = atomic_load_explicit(&sleep_call_count,
                                                   memory_order_relaxed);
    kernel_poll_readiness_notify();

    int result = kernel_poll_wait_current(stale_generation, 0);
    int passed = result == 0 && task.state == TASK_RUNNING &&
                 task.waiting_queue == NULL &&
                 !kernel_poll_wait_registered(&task) &&
                 atomic_load_explicit(&sleep_call_count, memory_order_relaxed) ==
                     sleeps_before;
    current_task = NULL;
    if (!passed) fprintf(stderr, "FAIL: stale poll generation slept or registered\n");
    return passed ? 0 : 1;
}

enum poll_race_stage {
    POLL_NOTIFY_BEFORE_BLOCKED,
    POLL_NOTIFY_AFTER_BLOCKED,
};

static void *poll_notifier(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&race_mutex);
    while (!race_sleep_entered)
        pthread_cond_wait(&race_cond, &race_mutex);
    pthread_mutex_unlock(&race_mutex);

    atomic_store_explicit(&notifier_active, true, memory_order_release);
    kernel_poll_readiness_notify();
    atomic_store_explicit(&notifier_active, false, memory_order_release);
    return NULL;
}

static int test_poll_notify_race(enum poll_race_stage stage)
{
    struct task task;
    pthread_t notifier;
    task_init(&task, 20u + (uint32_t)stage, TASK_RUNNING);
    current_task = &task;

    pthread_mutex_lock(&race_mutex);
    race_sleep_entered = false;
    race_block_before_wait = stage == POLL_NOTIFY_BEFORE_BLOCKED;
    pthread_mutex_unlock(&race_mutex);
    atomic_store_explicit(&race_wait_for_notify, true, memory_order_release);
    atomic_store_explicit(&notifier_lock_contended, false, memory_order_release);

    if (pthread_create(&notifier, NULL, poll_notifier, NULL) != 0) {
        atomic_store_explicit(&race_wait_for_notify, false, memory_order_release);
        current_task = NULL;
        fprintf(stderr, "FAIL: could not start poll notifier\n");
        return 1;
    }

    uint64_t generation = kernel_poll_readiness_generation();
    int result = kernel_poll_wait_current(generation, 0);
    int join_result = pthread_join(notifier, NULL);
    atomic_store_explicit(&race_wait_for_notify, false, memory_order_release);

    int passed = join_result == 0 && result == 1 && task.state == TASK_READY &&
                 task.waiting_queue == NULL &&
                 !kernel_poll_wait_registered(&task);
    current_task = NULL;
    if (!passed) {
        fprintf(stderr,
                "FAIL: poll notify race stage=%u result=%d state=%u registered=%d\n",
                (unsigned)stage, result, (unsigned)task.state,
                kernel_poll_wait_registered(&task));
    }
    return passed ? 0 : 1;
}

static int test_threshold_stop_and_unconditional_wake(void)
{
    struct kernel_wait_queue queue;
    struct task low, maximum, stopped;
    kernel_wait_queue_init(&queue);
    task_init(&low, 31u, TASK_RUNNING);
    task_init(&maximum, 32u, TASK_RUNNING);
    task_init(&stopped, 33u, TASK_RUNNING);

    current_task = &low;
    kernel_wait_queue_block_current_minimum(&queue, 512u);
    current_task = &maximum;
    kernel_wait_queue_block_current_minimum(&queue, UINT32_MAX);
    current_task = &stopped;
    kernel_wait_queue_block_current_minimum(&queue, 512u);
    stopped.state = TASK_STOPPED;

    int passed = low.state == TASK_BLOCKED && maximum.state == TASK_BLOCKED &&
                 kernel_wait_queue_wake_satisfied(&queue, 127u) == 0 &&
                 low.state == TASK_BLOCKED && maximum.state == TASK_BLOCKED &&
                 stopped.state == TASK_STOPPED;
    passed = passed && kernel_wait_queue_wake_satisfied(&queue, 512u) == 2u &&
             low.state == TASK_READY && maximum.state == TASK_BLOCKED &&
             stopped.state == TASK_STOPPED && low.waiting_queue == NULL &&
             stopped.waiting_queue == NULL;
    passed = passed &&
             kernel_wait_queue_wake_satisfied(&queue, UINT32_MAX - 1u) == 0u &&
             maximum.state == TASK_BLOCKED && maximum.waiting_queue == &queue;
    passed = passed &&
             kernel_wait_queue_wake_satisfied(&queue, UINT32_MAX) == 1u &&
             maximum.state == TASK_READY && maximum.waiting_queue == NULL &&
             stopped.state == TASK_STOPPED && queue.count == 0u;
    current_task = NULL;

    if (!passed)
        fprintf(stderr, "FAIL: threshold boundary or stopped-task wake semantics\n");
    return passed ? 0 : 1;
}

struct enqueue_args {
    struct kernel_wait_queue *queue;
    struct task *task;
    pthread_barrier_t *start;
    atomic_bool *waker_started;
    atomic_bool *failed;
};

static void *concurrent_enqueue(void *opaque)
{
    struct enqueue_args *args = opaque;
    int barrier_result = pthread_barrier_wait(args->start);
    if (barrier_result != 0 && barrier_result != PTHREAD_BARRIER_SERIAL_THREAD) {
        atomic_store(args->failed, true);
        return NULL;
    }
    while (!atomic_load_explicit(args->waker_started, memory_order_acquire))
        sched_yield();
    if (kernel_wait_queue_add(args->queue, args->task) < 0)
        atomic_store(args->failed, true);
    return NULL;
}

struct wake_args {
    struct kernel_wait_queue *queue;
    pthread_barrier_t *start;
    atomic_bool *waker_started;
    atomic_bool *producers_done;
    atomic_bool *failed;
};

static void *concurrent_wake_loop(void *opaque)
{
    struct wake_args *args = opaque;
    int barrier_result = pthread_barrier_wait(args->start);
    if (barrier_result != 0 && barrier_result != PTHREAD_BARRIER_SERIAL_THREAD) {
        atomic_store(args->failed, true);
        return NULL;
    }

    (void)kernel_wait_queue_wake_satisfied(args->queue, 0u);
    atomic_store_explicit(args->waker_started, true, memory_order_release);
    while (!atomic_load_explicit(args->producers_done, memory_order_acquire)) {
        (void)kernel_wait_queue_wake_satisfied(args->queue, 0u);
        (void)kernel_wait_queue_wake_all(args->queue);
        sched_yield();
    }
    (void)kernel_wait_queue_wake_all(args->queue);
    return NULL;
}

static int test_concurrent_enqueue_threshold_and_close_wakes(void)
{
    struct kernel_wait_queue queue;
    struct task tasks[CONCURRENT_TASKS];
    struct enqueue_args enqueue[CONCURRENT_TASKS];
    struct wake_args wake_args;
    pthread_t producers[CONCURRENT_TASKS], wakers[2];
    pthread_barrier_t start;
    atomic_bool waker_started = false;
    atomic_bool producers_done = false;
    atomic_bool failed = false;

    kernel_wait_queue_init(&queue);
    for (uint32_t i = 0; i < CONCURRENT_TASKS; ++i)
        task_init(&tasks[i], 100u + i,
                  i == CONCURRENT_TASKS - 1u ? TASK_STOPPED : TASK_BLOCKED);
    if (pthread_barrier_init(&start, NULL, CONCURRENT_TASKS + 3u) != 0) {
        fprintf(stderr, "FAIL: could not initialize enqueue barrier\n");
        return 1;
    }

    wake_args = (struct wake_args){
        .queue = &queue,
        .start = &start,
        .waker_started = &waker_started,
        .producers_done = &producers_done,
        .failed = &failed,
    };
    for (uint32_t i = 0; i < 2u; ++i) {
        if (pthread_create(&wakers[i], NULL, concurrent_wake_loop, &wake_args) != 0) {
            fprintf(stderr, "FAIL: could not start queue waker\n");
            return 1;
        }
    }
    for (uint32_t i = 0; i < CONCURRENT_TASKS; ++i) {
        enqueue[i] = (struct enqueue_args){
            .queue = &queue,
            .task = &tasks[i],
            .start = &start,
            .waker_started = &waker_started,
            .failed = &failed,
        };
        if (pthread_create(&producers[i], NULL, concurrent_enqueue, &enqueue[i]) != 0) {
            fprintf(stderr, "FAIL: could not start queue enqueuer\n");
            return 1;
        }
    }

    int barrier_result = pthread_barrier_wait(&start);
    if (barrier_result != 0 && barrier_result != PTHREAD_BARRIER_SERIAL_THREAD)
        atomic_store(&failed, true);
    for (uint32_t i = 0; i < CONCURRENT_TASKS; ++i)
        if (pthread_join(producers[i], NULL) != 0) atomic_store(&failed, true);
    atomic_store_explicit(&producers_done, true, memory_order_release);
    for (uint32_t i = 0; i < 2u; ++i)
        if (pthread_join(wakers[i], NULL) != 0) atomic_store(&failed, true);
    pthread_barrier_destroy(&start);

    int passed = !atomic_load(&failed);
    for (uint32_t i = 0; i < CONCURRENT_TASKS; ++i) {
        enum task_state expected = i == CONCURRENT_TASKS - 1u
            ? TASK_STOPPED : TASK_READY;
        passed = passed && tasks[i].state == expected &&
                 tasks[i].waiting_queue == NULL &&
                 !task_is_registered(&queue, &tasks[i]);
    }
    passed = passed && queue.count == 0u &&
             kernel_wait_queue_wake_all(&queue) == 0u;
    if (!passed)
        fprintf(stderr, "FAIL: concurrent enqueue/satisfied/close wake left stale waiters\n");
    return passed ? 0 : 1;
}

struct remove_wake_args {
    struct kernel_wait_queue *queue;
    struct task *task;
    pthread_barrier_t *start;
    bool remove;
};

static void *remove_or_wake(void *opaque)
{
    struct remove_wake_args *args = opaque;
    int barrier_result = pthread_barrier_wait(args->start);
    if (barrier_result != 0 && barrier_result != PTHREAD_BARRIER_SERIAL_THREAD)
        return (void *)(uintptr_t)1u;
    if (args->remove)
        kernel_wait_queue_remove(args->queue, args->task);
    else
        (void)kernel_wait_queue_wake_one(args->queue);
    return NULL;
}

static int test_exit_unlink_races_wake(void)
{
    struct kernel_wait_queue queue;
    struct task task;
    for (uint32_t round = 0; round < REMOVE_WAKE_ROUNDS; ++round) {
        pthread_t remover, waker;
        pthread_barrier_t start;
        void *remove_result = NULL, *wake_result = NULL;
        kernel_wait_queue_init(&queue);
        task_init(&task, 500u + round, TASK_BLOCKED);
        if (kernel_wait_queue_add(&queue, &task) < 0 ||
            pthread_barrier_init(&start, NULL, 3u) != 0) {
            fprintf(stderr, "FAIL: could not prepare exit/wake race\n");
            return 1;
        }
        struct remove_wake_args remove_args = {
            .queue = &queue, .task = &task, .start = &start, .remove = true,
        };
        struct remove_wake_args wake_args = {
            .queue = &queue, .task = &task, .start = &start, .remove = false,
        };
        if (pthread_create(&remover, NULL, remove_or_wake, &remove_args) != 0 ||
            pthread_create(&waker, NULL, remove_or_wake, &wake_args) != 0) {
            fprintf(stderr, "FAIL: could not start exit/wake race\n");
            return 1;
        }
        int barrier_result = pthread_barrier_wait(&start);
        if (barrier_result != 0 && barrier_result != PTHREAD_BARRIER_SERIAL_THREAD)
            return 1;
        if (pthread_join(remover, &remove_result) != 0 ||
            pthread_join(waker, &wake_result) != 0 ||
            remove_result || wake_result) {
            fprintf(stderr, "FAIL: exit/wake race worker failed\n");
            return 1;
        }
        pthread_barrier_destroy(&start);

        if (task.waiting_queue != NULL || task_is_registered(&queue, &task) ||
            queue.count != 0u || kernel_wait_queue_wake_one(&queue) != 0u ||
            (task.state != TASK_BLOCKED && task.state != TASK_READY)) {
            fprintf(stderr, "FAIL: exit unlink race left a live queue reference\n");
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    if (test_poll_stale_generation_does_not_sleep()) return EXIT_FAILURE;
    if (test_poll_notify_race(POLL_NOTIFY_BEFORE_BLOCKED)) return EXIT_FAILURE;
    if (test_poll_notify_race(POLL_NOTIFY_AFTER_BLOCKED)) return EXIT_FAILURE;
    if (test_threshold_stop_and_unconditional_wake()) return EXIT_FAILURE;
    if (test_concurrent_enqueue_threshold_and_close_wakes()) return EXIT_FAILURE;
    if (test_exit_unlink_races_wake()) return EXIT_FAILURE;
    puts("PASS wait queue concurrency: poll generation races, threshold boundaries, concurrent close/wake, and exit unlink");
    return EXIT_SUCCESS;
}
