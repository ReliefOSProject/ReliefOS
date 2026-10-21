/* Host-side regression fixture for the real pipe and wait-queue code. */
#include <stddef.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../kernel/reliefnt/kernel/sync/wait.c"

#ifdef PIPE_WAKE_IPC_SOURCE
#include PIPE_WAKE_IPC_SOURCE
#else
#include "../../kernel/reliefnt/kernel/syscall/ipc/ipc.c"
#endif

#define TEST_TASK_COUNT 6u
#define READER_INDEX 5u
#define TEST_HANDLE 17u
#define TEST_PIPE_BUF 4096u

#define REQUIRE(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", (message)); \
        return 1; \
    } \
} while (0)

static struct task_pipe pipe_fixture;
static struct task test_tasks[TEST_TASK_COUNT];
static struct task *current_task;
static struct task_file read_end;
static struct task_file write_end;
static struct task_file nonblocking_write_end;
static struct kernel_object_table object_table;
static uint8_t source_data[TASK_PIPE_CAP];
static uint8_t writer_a_data[8192];
static uint8_t writer_b_data[8192];
static uint8_t read_data[TASK_PIPE_CAP];
static uint32_t spin_lock_acquisition_count;
static uint32_t signal_count;
static uint32_t signal_pid;
static int signal_number;

void kernel_spin_init(struct kernel_spinlock *lock)
{
    lock->state = 0;
}

void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{
    (void)lock;
    *flags = 0;
    ++spin_lock_acquisition_count;
}

void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{
    (void)lock;
    (void)flags;
}

struct kernel_object_table *kernel_objects(void)
{
    return &object_table;
}

void *kernel_object_lookup(struct kernel_object_table *table, uint32_t handle,
                           enum kernel_object_type type)
{
    (void)table;
    return handle == TEST_HANDLE && type == KERNEL_OBJECT_PIPE ? &pipe_fixture : NULL;
}

int kernel_object_remove(struct kernel_object_table *table, uint32_t handle,
                         enum kernel_object_type type, void **object)
{
    (void)table;
    (void)handle;
    (void)type;
    if (object) *object = NULL;
    return 0;
}

void kernel_free(void *memory)
{
    (void)memory;
}

int storage_inode_put(struct storage_inode_ref *reference)
{
    (void)reference;
    return 0;
}

struct task *sched_current_task(void)
{
    return current_task;
}

uint32_t sched_current_pid(void)
{
    return current_task ? current_task->pid : 0;
}

static struct task *task_for_pid(uint32_t pid)
{
    for (uint32_t i = 0; i < TEST_TASK_COUNT; ++i)
        if (test_tasks[i].pid == pid) return &test_tasks[i];
    return NULL;
}

void sched_mark_ready(uint32_t pid)
{
    struct task *task = task_for_pid(pid);
    if (task && task->state != TASK_EXITED && !task->vfork_child)
        task->state = TASK_READY;
}

void sched_wake_interruptible(struct task *task)
{
    if (task && task->state == TASK_BLOCKED && !task->vfork_child)
        task->state = TASK_READY;
}

void sched_block_current(void)
{
    if (current_task && current_task->state != TASK_EXITED)
        current_task->state = TASK_BLOCKED;
}

void sched_sleep_current_until(uint64_t wake_tick)
{
    (void)wake_tick;
    sched_block_current();
}

int sched_signal_user_task(uint32_t pid, int signo)
{
    ++signal_count;
    signal_pid = pid;
    signal_number = signo;
    return 0;
}

static void reset_tasks(void)
{
    for (uint32_t i = 0; i < TEST_TASK_COUNT; ++i)
        kernel_poll_wait_remove(&test_tasks[i]);
    memset(test_tasks, 0, sizeof(test_tasks));
    for (uint32_t i = 0; i < TEST_TASK_COUNT; ++i) {
        test_tasks[i].pid = 100u + i;
        test_tasks[i].state = TASK_RUNNING;
        test_tasks[i].kind = TASK_KIND_USER;
    }
    current_task = &test_tasks[0];
}

static void reset_pipe(void)
{
    memset(&pipe_fixture, 0, sizeof(pipe_fixture));
    pipe_fixture.used = 1;
    pipe_fixture.handle = TEST_HANDLE;
    pipe_fixture.readers = 1;
    pipe_fixture.writers = 1;
    kernel_wait_queue_init(&pipe_fixture.wait_read);
    kernel_wait_queue_init(&pipe_fixture.wait_write);
    memset(&read_end, 0, sizeof(read_end));
    read_end.flags = TASK_FILE_FLAG_PIPE | RELIEFOS_O_RDONLY;
    read_end.aux = TEST_HANDLE;
    memset(&write_end, 0, sizeof(write_end));
    write_end.flags = TASK_FILE_FLAG_PIPE | TASK_FILE_FLAG_PIPE_WRITE | RELIEFOS_O_WRONLY;
    write_end.aux = TEST_HANDLE;
    nonblocking_write_end = write_end;
    nonblocking_write_end.flags |= RELIEFOS_O_NONBLOCK;
    signal_count = signal_pid = 0;
    signal_number = 0;
    reset_tasks();
    for (uint32_t i = 0; i < TASK_PIPE_CAP; ++i)
        source_data[i] = (uint8_t)((i * 37u + 11u) & 0xffu);
    memset(writer_a_data, 0xa5, sizeof(writer_a_data));
    memset(writer_b_data, 0x5a, sizeof(writer_b_data));
}

static int fill_pipe(uint32_t length)
{
    current_task = &test_tasks[0];
    int result = task_pipe_write(&write_end, source_data, length);
    REQUIRE(result == (int)length, "fixture fill must use the real pipe writer");
    return 0;
}

static int block_writer(uint32_t task_index, const void *data, uint32_t length,
                        struct task_file *file)
{
    current_task = &test_tasks[task_index];
    int result = task_pipe_write(file, data, length);
    REQUIRE(result == -RELIEFOS_EAGAIN, "full-pipe write must block internally");
    REQUIRE(current_task->state == TASK_BLOCKED, "blocked writer must enter wait state");
    return 0;
}

static int read_pipe(uint32_t length)
{
    current_task = &test_tasks[READER_INDEX];
    return task_pipe_read(&read_end, read_data, length);
}

static int block_global_poll_waiter(uint32_t task_index)
{
    current_task = &test_tasks[task_index];
    uint64_t generation = kernel_poll_readiness_generation();
    REQUIRE(kernel_poll_wait_current(generation, 0) == 1,
            "global poll waiter must register and enter the blocked state");
    REQUIRE(current_task->state == TASK_BLOCKED &&
            kernel_poll_wait_registered(current_task) &&
            current_task->waiting_queue != NULL,
            "poll waiter must be linked to the real global wait queue");
    return 0;
}

static int assert_global_poll_waiter_woken(uint32_t task_index,
                                           const char *message)
{
    struct task *task = &test_tasks[task_index];
    REQUIRE(task->state == TASK_READY &&
            !kernel_poll_wait_registered(task) && task->waiting_queue == NULL,
            message);
    return 0;
}

static int test_nonfull_read_does_not_notify_global_poll_waiters(void)
{
    reset_pipe();
    if (fill_pipe(2048u)) return 1;
    if (block_global_poll_waiter(4u)) return 1;
    uint64_t generation = kernel_poll_readiness_generation();

    REQUIRE(read_pipe(512u) == 512,
            "reading data from a nonfull pipe must return the requested bytes");
    REQUIRE(kernel_poll_readiness_generation() == generation,
            "nonfull read must not publish a global poll readiness change");
    REQUIRE(test_tasks[4].state == TASK_BLOCKED &&
            kernel_poll_wait_registered(&test_tasks[4]) &&
            test_tasks[4].waiting_queue != NULL,
            "nonfull read must leave unrelated global poll waiter asleep and queued");

    kernel_poll_wait_remove(&test_tasks[4]);
    return 0;
}

static int test_full_to_nonfull_read_notifies_global_poll_waiters(void)
{
    reset_pipe();
    if (fill_pipe(TASK_PIPE_CAP)) return 1;
    if (block_global_poll_waiter(4u)) return 1;
    uint64_t generation = kernel_poll_readiness_generation();

    REQUIRE(read_pipe(512u) == 512,
            "reading from a full pipe must return the requested bytes");
    REQUIRE(kernel_poll_readiness_generation() == generation + 1u,
            "full-to-nonfull transition must publish global writable readiness");
    if (assert_global_poll_waiter_woken(
            4u, "full-to-nonfull transition must wake and unlink the global poller"))
        return 1;
    return 0;
}

static int test_pipe_close_still_notifies_global_poll_waiters(void)
{
    reset_pipe();
    if (block_global_poll_waiter(4u)) return 1;
    uint64_t generation = kernel_poll_readiness_generation();

    current_task = &test_tasks[0];
    task_pipe_release(&write_end);
    REQUIRE(kernel_poll_readiness_generation() == generation + 1u,
            "pipe close must continue publishing global readiness changes");
    if (assert_global_poll_waiter_woken(
            4u, "pipe close must wake and unlink the global poller"))
        return 1;
    return 0;
}

static int test_satisfied_writer_wakes_without_nonfull_poll_notification(void)
{
    reset_pipe();
    if (fill_pipe(TASK_PIPE_CAP - 512u)) return 1;
    if (block_writer(1u, writer_a_data, 1024u, &write_end)) return 1;
    if (block_global_poll_waiter(4u)) return 1;
    uint64_t generation = kernel_poll_readiness_generation();

    REQUIRE(read_pipe(512u) == 512,
            "reading from a nonfull pipe must free bytes for the queued writer");
    REQUIRE(test_tasks[1].state == TASK_READY &&
            pipe_fixture.wait_write.count == 0u,
            "writer threshold wake must remain independent of global poll wakeups");
    REQUIRE(kernel_poll_readiness_generation() == generation,
            "satisfying a writer threshold on an already nonfull pipe must not notify pollers");
    REQUIRE(test_tasks[4].state == TASK_BLOCKED &&
            kernel_poll_wait_registered(&test_tasks[4]) &&
            test_tasks[4].waiting_queue != NULL,
            "writer threshold wake must leave the unrelated global poller asleep and queued");

    kernel_poll_wait_remove(&test_tasks[4]);
    return 0;
}

static int test_8192_writer_threshold_and_data(void)
{
    reset_pipe();
    if (fill_pipe(TASK_PIPE_CAP)) return 1;
    if (block_writer(1, writer_a_data, 8192u, &write_end)) return 1;
    REQUIRE(pipe_fixture.wait_write.count == 1u, "large writer must be queued once");

    for (uint32_t i = 0; i < 15u; ++i) {
        REQUIRE(read_pipe(512u) == 512, "each dd-sized read must return 512 bytes");
        REQUIRE(test_tasks[1].state == TASK_BLOCKED,
                "8192-byte writer woke before 16 * 512 bytes were available");
    }
    REQUIRE(pipe_fixture.wait_write.minimum[0] == 8192u,
            "queued writer must retain its full atomic request size");
    REQUIRE(read_pipe(512u) == 512, "sixteenth dd-sized read must return 512 bytes");
    REQUIRE(test_tasks[1].state == TASK_READY,
            "writer must wake when 16 * 512 bytes are available");
    REQUIRE(pipe_fixture.wait_write.count == 0u, "satisfied writer must leave wait queue");

    current_task = &test_tasks[1];
    REQUIRE(task_pipe_write(&write_end, writer_a_data, 8192u) == 8192,
            "woken atomic writer must transfer all bytes");
    current_task = &test_tasks[READER_INDEX];
    REQUIRE(task_pipe_read(&read_end, read_data, TASK_PIPE_CAP) == (int)TASK_PIPE_CAP,
            "pipe must retain a full-capacity sequence after retry");
    REQUIRE(memcmp(read_data, source_data + 8192u, TASK_PIPE_CAP - 8192u) == 0,
            "reader must receive the untouched tail of original pipe data");
    REQUIRE(memcmp(read_data + TASK_PIPE_CAP - 8192u, writer_a_data, 8192u) == 0,
            "reader must receive the complete writer payload after original data");
    return 0;
}

static int test_multiple_writer_thresholds_and_hol(void)
{
    reset_pipe();
    if (fill_pipe(TASK_PIPE_CAP)) return 1;
    if (block_writer(1, writer_a_data, 8192u, &write_end)) return 1;
    if (block_writer(2, writer_b_data, 1024u, &write_end)) return 1;
    if (block_writer(3, writer_a_data, 512u, &write_end)) return 1;

    REQUIRE(pipe_fixture.wait_write.count == 3u, "three writers must retain FIFO order");
    REQUIRE(read_pipe(1024u) == 1024, "freeing 1024 bytes must succeed");
    REQUIRE(test_tasks[1].state == TASK_BLOCKED,
            "large queue head must remain asleep when its threshold is unmet");
    REQUIRE(test_tasks[2].state == TASK_READY && test_tasks[3].state == TASK_READY,
            "all smaller writers must bypass an unsatisfied queue head");
    REQUIRE(pipe_fixture.wait_write.count == 1u &&
            pipe_fixture.wait_write.waiters[0] == &test_tasks[1] &&
            pipe_fixture.wait_write.minimum[0] == 8192u,
            "removing satisfied middle waiters must preserve the large waiter's threshold");

    REQUIRE(read_pipe(512u) == 512, "additional 512-byte read must succeed");
    REQUIRE(test_tasks[1].state == TASK_BLOCKED &&
            pipe_fixture.wait_write.minimum[0] == 8192u,
            "waiter metadata must stay aligned after a middle removal");
    REQUIRE(read_pipe(6656u) == 6656, "remaining bytes must reach the large threshold");
    REQUIRE(test_tasks[1].state == TASK_READY && pipe_fixture.wait_write.count == 0u,
            "large waiter must wake exactly when its request fits");
    return 0;
}

static int test_multiple_satisfied_writers_retry_without_lost_wakeup(void)
{
    reset_pipe();
    if (fill_pipe(TASK_PIPE_CAP)) return 1;
    if (block_writer(1, writer_a_data, 512u, &write_end)) return 1;
    if (block_writer(2, writer_b_data, 512u, &write_end)) return 1;

    REQUIRE(read_pipe(512u) == 512, "one 512-byte read must free space");
    REQUIRE(test_tasks[1].state == TASK_READY && test_tasks[2].state == TASK_READY,
            "wake_satisfied must wake every writer whose 512-byte threshold fits");
    REQUIRE(pipe_fixture.wait_write.count == 0u,
            "all currently satisfied writers must be removed from the queue");

    current_task = &test_tasks[1];
    REQUIRE(task_pipe_write(&write_end, writer_a_data, 512u) == 512,
            "first eligible writer must complete");
    current_task = &test_tasks[2];
    REQUIRE(task_pipe_write(&write_end, writer_b_data, 512u) == -RELIEFOS_EAGAIN,
            "second writer must safely requeue if another writer consumed its space");
    REQUIRE(test_tasks[2].state == TASK_BLOCKED &&
            pipe_fixture.wait_write.minimum[0] == 512u,
            "requeued writer must retain its threshold");

    REQUIRE(read_pipe(512u) == 512 && test_tasks[2].state == TASK_READY,
            "a later read must wake the requeued writer without handoff state");
    current_task = &test_tasks[2];
    REQUIRE(task_pipe_write(&write_end, writer_b_data, 512u) == 512,
            "requeued writer must complete after its next threshold wake");

    current_task = &test_tasks[READER_INDEX];
    REQUIRE(task_pipe_read(&read_end, read_data, TASK_PIPE_CAP) == (int)TASK_PIPE_CAP,
            "pipe data must remain complete after competing atomic writers");
    REQUIRE(memcmp(read_data, source_data + 1024u, TASK_PIPE_CAP - 1024u) == 0,
            "competing writers must not overwrite unread pipe bytes");
    REQUIRE(memcmp(read_data + TASK_PIPE_CAP - 1024u, writer_a_data, 512u) == 0 &&
            memcmp(read_data + TASK_PIPE_CAP - 512u, writer_b_data, 512u) == 0,
            "each competing writer's complete payload must appear once in order");
    return 0;
}

static int test_nonblocking_writer_does_not_queue(void)
{
    reset_pipe();
    if (fill_pipe(TASK_PIPE_CAP)) return 1;
    uint32_t head_before = pipe_fixture.head;
    uint32_t tail_before = pipe_fixture.tail;
    current_task = &test_tasks[1];
    REQUIRE(task_pipe_write(&nonblocking_write_end, writer_a_data, 512u) ==
            -RELIEFOS_EAGAIN, "nonblocking full-pipe write must report EAGAIN");
    REQUIRE(current_task->state == TASK_RUNNING && pipe_fixture.wait_write.count == 0u,
            "nonblocking writer must not sleep or enter the wait queue");
    REQUIRE(pipe_fixture.head == head_before && pipe_fixture.tail == tail_before,
            "failed nonblocking atomic write must not transfer partial bytes");
    return 0;
}

static int test_nonblocking_large_full_pipe_returns_eagain(void)
{
    reset_pipe();
    if (fill_pipe(TASK_PIPE_CAP)) return 1;
    uint32_t head_before = pipe_fixture.head;
    uint32_t tail_before = pipe_fixture.tail;
    current_task = &test_tasks[1];
    REQUIRE(task_pipe_write(&nonblocking_write_end, writer_a_data, 8192u) ==
            -RELIEFOS_EAGAIN,
            "nonblocking write larger than PIPE_BUF must return EAGAIN on a full pipe");
    REQUIRE(current_task->state == TASK_RUNNING && pipe_fixture.wait_write.count == 0u,
            "full-pipe nonblocking large writer must never queue");
    REQUIRE(pipe_fixture.head == head_before && pipe_fixture.tail == tail_before,
            "full-pipe nonblocking large write must not change ring state");
    return 0;
}

static int test_nonblocking_pipe_buf_write_remains_atomic(void)
{
    reset_pipe();
    if (fill_pipe(TASK_PIPE_CAP)) return 1;
    REQUIRE(read_pipe(512u) == 512, "read must leave exactly 512 bytes free");
    uint32_t head_before = pipe_fixture.head;
    uint32_t tail_before = pipe_fixture.tail;
    current_task = &test_tasks[1];
    REQUIRE(task_pipe_write(&nonblocking_write_end, writer_a_data, TEST_PIPE_BUF) ==
            -RELIEFOS_EAGAIN,
            "nonblocking PIPE_BUF write must fail as a whole when space is short");
    REQUIRE(pipe_fixture.head == head_before && pipe_fixture.tail == tail_before &&
            pipe_fixture.wait_write.count == 0u,
            "failed nonblocking PIPE_BUF write must not transfer or queue any bytes");

    current_task = &test_tasks[READER_INDEX];
    REQUIRE(task_pipe_read(&read_end, read_data, TASK_PIPE_CAP - 512u) ==
            (int)(TASK_PIPE_CAP - 512u), "remaining pipe contents must drain completely");
    REQUIRE(memcmp(read_data, source_data + 512u, TASK_PIPE_CAP - 512u) == 0,
            "failed atomic write must leave every existing byte unchanged");
    return 0;
}

static int test_nonblocking_large_write_short_transfers_across_ring_end(void)
{
    reset_pipe();
    if (fill_pipe(TASK_PIPE_CAP)) return 1;
    REQUIRE(read_pipe(512u) == 512, "read must leave exactly 512 bytes free");
    REQUIRE(pipe_fixture.head == TASK_PIPE_RING_CAP - 1u,
            "fixture must position the write head at the ring boundary");
    uint32_t tail_before = pipe_fixture.tail;

    current_task = &test_tasks[1];
    REQUIRE(task_pipe_write(&nonblocking_write_end, writer_a_data, 8192u) == 512,
            "nonblocking write larger than PIPE_BUF must transfer available bytes");
    REQUIRE(pipe_fixture.wait_write.count == 0u &&
            test_tasks[1].state == TASK_RUNNING,
            "partial nonblocking large write must return without queueing");
    REQUIRE(pipe_fixture.tail == tail_before &&
            pipe_fixture.head == (tail_before - 1u) % TASK_PIPE_RING_CAP,
            "512-byte partial write must advance the ring head across its wrap");

    current_task = &test_tasks[READER_INDEX];
    REQUIRE(task_pipe_read(&read_end, read_data, TASK_PIPE_CAP) == (int)TASK_PIPE_CAP,
            "pipe must drain after the wrapped partial write");
    REQUIRE(memcmp(read_data, source_data + 512u, TASK_PIPE_CAP - 512u) == 0,
            "short write must preserve the existing unread bytes");
    REQUIRE(memcmp(read_data + TASK_PIPE_CAP - 512u, writer_a_data, 512u) == 0,
            "wrapped short write must preserve every transferred payload byte in order");
    return 0;
}

static int test_pipe_buf_atomicity_and_contents(void)
{
    reset_pipe();
    if (fill_pipe(TASK_PIPE_CAP - (TEST_PIPE_BUF - 1u))) return 1;
    uint32_t head_before = pipe_fixture.head;
    if (block_writer(1, writer_a_data, TEST_PIPE_BUF, &write_end)) return 1;
    REQUIRE(pipe_fixture.head == head_before,
            "PIPE_BUF-sized blocked write must not commit a partial prefix");
    REQUIRE(pipe_fixture.wait_write.minimum[0] == TEST_PIPE_BUF,
            "PIPE_BUF-sized waiter must register the complete atomic threshold");

    REQUIRE(read_pipe(1u) == 1 && test_tasks[1].state == TASK_READY,
            "one freed byte must satisfy the PIPE_BUF request threshold");
    current_task = &test_tasks[1];
    REQUIRE(task_pipe_write(&write_end, writer_a_data, TEST_PIPE_BUF) ==
            (int)TEST_PIPE_BUF, "PIPE_BUF-sized retry must be transferred as one write");

    current_task = &test_tasks[READER_INDEX];
    REQUIRE(task_pipe_read(&read_end, read_data, TASK_PIPE_CAP) == (int)TASK_PIPE_CAP,
            "PIPE_BUF test must drain the whole ring");
    REQUIRE(memcmp(read_data, source_data + 1u,
                   TASK_PIPE_CAP - TEST_PIPE_BUF) == 0,
            "unread prefix must remain unchanged around the atomic write");
    REQUIRE(memcmp(read_data + TASK_PIPE_CAP - TEST_PIPE_BUF,
                   writer_a_data, TEST_PIPE_BUF) == 0,
            "PIPE_BUF payload must be complete and contiguous");
    return 0;
}

static int test_close_eof_and_unconditional_epipe_wake(void)
{
    reset_pipe();
    current_task = &test_tasks[READER_INDEX];
    REQUIRE(task_pipe_read(&read_end, read_data, 512u) == -RELIEFOS_EAGAIN,
            "empty pipe with a writer must block the reader internally");
    REQUIRE(current_task->state == TASK_BLOCKED && pipe_fixture.wait_read.count == 1u,
            "blocked reader must be registered for data or EOF");
    current_task = &test_tasks[0];
    task_pipe_release(&write_end);
    REQUIRE(test_tasks[READER_INDEX].state == TASK_READY &&
            pipe_fixture.wait_read.count == 0u,
            "last-writer close must unconditionally wake the reader");
    current_task = &test_tasks[READER_INDEX];
    REQUIRE(task_pipe_read(&read_end, read_data, 512u) == 0,
            "empty pipe after last-writer close must return EOF");

    reset_pipe();
    if (fill_pipe(TASK_PIPE_CAP)) return 1;
    if (block_writer(1, writer_a_data, 8192u, &write_end)) return 1;
    current_task = &test_tasks[0];
    task_pipe_release(&read_end);
    REQUIRE(test_tasks[1].state == TASK_READY && pipe_fixture.wait_write.count == 0u,
            "last-reader close must wake an unsatisfied writer regardless of threshold");
    current_task = &test_tasks[1];
    REQUIRE(task_pipe_write(&write_end, writer_a_data, 8192u) == -RELIEFOS_EPIPE,
            "writer retry after last-reader close must return EPIPE");
    REQUIRE(signal_count == 1u && signal_pid == test_tasks[1].pid && signal_number == 13,
            "EPIPE path must also send SIGPIPE to the writing task");
    return 0;
}

static int test_generic_queue_minimum_defaults_to_zero(void)
{
    struct kernel_wait_queue queue;
    kernel_wait_queue_init(&queue);
    current_task = &test_tasks[1];
    REQUIRE(kernel_wait_queue_add(&queue, current_task) == 0,
            "generic wait-queue add must succeed");
    REQUIRE(queue.minimum[0] == 0u,
            "generic waiters must have an unconditional zero threshold");
    REQUIRE(kernel_wait_queue_wake_satisfied(&queue, 0u) == 1u &&
            current_task->state == TASK_READY,
            "zero-threshold generic waiters must wake with zero available");

    kernel_wait_queue_init(&queue);
    current_task = &test_tasks[1];
    kernel_wait_queue_block_current_minimum(&queue, 512u);
    current_task = &test_tasks[2];
    kernel_wait_queue_block_current_minimum(&queue, 8192u);
    current_task = &test_tasks[3];
    kernel_wait_queue_block_current_minimum(&queue, 1024u);
    kernel_wait_queue_remove(&queue, &test_tasks[1]);
    REQUIRE(queue.count == 2u && queue.waiters[0] == &test_tasks[2] &&
            queue.minimum[0] == 8192u && queue.minimum[1] == 1024u &&
            queue.minimum[2] == 0u,
            "remove must shift thresholds with waiters and clear the old tail");
    REQUIRE(kernel_wait_queue_wake_satisfied(&queue, 1024u) == 1u &&
            test_tasks[2].state == TASK_BLOCKED && test_tasks[3].state == TASK_READY,
            "satisfied scan must skip an unsatisfied head and wake the matching waiter");
    REQUIRE(queue.count == 1u && queue.waiters[0] == &test_tasks[2] &&
            queue.minimum[0] == 8192u && queue.minimum[1] == 0u,
            "satisfied removal must preserve the remaining threshold metadata");
    return 0;
}

static int test_satisfied_wake_preserves_stopped_waiter(void)
{
    struct kernel_wait_queue queue;
    reset_tasks();
    kernel_wait_queue_init(&queue);

    current_task = &test_tasks[1];
    kernel_wait_queue_block_current_minimum(&queue, 1024u);
    REQUIRE(test_tasks[1].state == TASK_BLOCKED &&
            test_tasks[1].waiting_queue == &queue,
            "threshold waiter must first block on the real wait queue");
    test_tasks[1].state = TASK_STOPPED;

    current_task = &test_tasks[2];
    kernel_wait_queue_block_current_minimum(&queue, 512u);
    REQUIRE(queue.count == 2u && queue.minimum[0] == 1024u &&
            queue.minimum[1] == 512u,
            "stopped and blocked waiters must both retain their thresholds");

    REQUIRE(kernel_wait_queue_wake_satisfied(&queue, 1024u) == 2u,
            "satisfied wake must dequeue every waiter whose threshold fits");
    REQUIRE(test_tasks[1].state == TASK_STOPPED,
            "threshold wake must preserve TASK_STOPPED instead of resuming the task");
    REQUIRE(test_tasks[2].state == TASK_READY,
            "threshold wake must make an ordinary blocked waiter ready");
    REQUIRE(queue.count == 0u && queue.waiters[0] == NULL &&
            queue.minimum[0] == 0u && queue.minimum[1] == 0u &&
            test_tasks[1].waiting_queue == NULL &&
            test_tasks[2].waiting_queue == NULL,
            "satisfied wake must unlink both waiters and clear threshold metadata");
    return 0;
}

static int test_pipe_close_wake_all_preserves_stopped_waiter(void)
{
    reset_pipe();
    if (fill_pipe(TASK_PIPE_CAP)) return 1;
    if (block_writer(1u, writer_a_data, 8192u, &write_end)) return 1;
    if (block_writer(2u, writer_b_data, 512u, &write_end)) return 1;
    REQUIRE(pipe_fixture.wait_write.minimum[0] == 8192u &&
            pipe_fixture.wait_write.minimum[1] == 512u,
            "close fixture writers must be registered with their thresholds");
    test_tasks[1].state = TASK_STOPPED;

    current_task = &test_tasks[0];
    task_pipe_release(&read_end);

    REQUIRE(test_tasks[1].state == TASK_STOPPED,
            "last-reader close wake_all must preserve a stopped writer");
    REQUIRE(test_tasks[2].state == TASK_READY,
            "last-reader close wake_all must wake a blocked writer for EPIPE retry");
    REQUIRE(pipe_fixture.wait_write.count == 0u &&
            pipe_fixture.wait_write.waiters[0] == NULL &&
            pipe_fixture.wait_write.minimum[0] == 0u &&
            pipe_fixture.wait_write.minimum[1] == 0u &&
            test_tasks[1].waiting_queue == NULL &&
            test_tasks[2].waiting_queue == NULL,
            "close wake_all must unlink all writers and clear threshold metadata");
    return 0;
}

static void check_fast_path(int condition, const char *message, uint32_t *failures)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++*failures;
    }
}

static int test_wait_queue_fast_paths_reduce_lock_acquisitions(void)
{
    struct kernel_wait_queue queue;
    uint32_t failures = 0;
    uint32_t woke;

    reset_tasks();
    kernel_wait_queue_init(&queue);
    spin_lock_acquisition_count = 0;
    kernel_wait_queue_remove(&queue, &test_tasks[1]);
    check_fast_path(spin_lock_acquisition_count == 0,
                    "removing from an empty wait queue must not acquire its lock", &failures);

    spin_lock_acquisition_count = 0;
    woke = kernel_wait_queue_wake_one(&queue);
    check_fast_path(woke == 0 && spin_lock_acquisition_count == 0,
                    "waking one task on an empty queue must not acquire its lock", &failures);

    spin_lock_acquisition_count = 0;
    woke = kernel_wait_queue_wake_all(&queue);
    check_fast_path(woke == 0 && spin_lock_acquisition_count == 0,
                    "waking all tasks on an empty queue must not acquire its lock", &failures);

    spin_lock_acquisition_count = 0;
    woke = kernel_wait_queue_wake_satisfied(&queue, 0);
    check_fast_path(woke == 0 && spin_lock_acquisition_count == 0,
                    "threshold wake on an empty queue must not acquire its lock", &failures);

    current_task = &test_tasks[1];
    kernel_wait_queue_block_current_minimum(&queue, 4096u);
    spin_lock_acquisition_count = 0;
    woke = kernel_wait_queue_wake_satisfied(&queue, 512u);
    check_fast_path(woke == 0 && spin_lock_acquisition_count == 0 &&
                        test_tasks[1].state == TASK_BLOCKED,
                    "wake below every waiter threshold must skip the lock and leave waiters blocked",
                    &failures);

    /* Updating one registered waiter's requirement upward must publish the
     * new minimum; lowering it must publish the newly eligible requirement. */
    current_task = &test_tasks[1];
    kernel_wait_queue_block_current_minimum(&queue, 8192u);
    spin_lock_acquisition_count = 0;
    woke = kernel_wait_queue_wake_satisfied(&queue, 4096u);
    check_fast_path(woke == 0 && spin_lock_acquisition_count == 0 &&
                        test_tasks[1].state == TASK_BLOCKED,
                    "raising a waiter's threshold must refresh the fast snapshot", &failures);

    current_task = &test_tasks[1];
    kernel_wait_queue_block_current_minimum(&queue, 4096u);
    spin_lock_acquisition_count = 0;
    woke = kernel_wait_queue_wake_satisfied(&queue, 4096u);
    check_fast_path(woke == 1 && test_tasks[1].state == TASK_READY,
                    "lowering a waiter's threshold must allow the eligible wake", &failures);

    /* The first eligible waiter is removed; the remaining higher threshold
     * must become the fast minimum before wake_satisfied retries. */
    kernel_wait_queue_init(&queue);
    reset_tasks();
    current_task = &test_tasks[1];
    kernel_wait_queue_block_current_minimum(&queue, 512u);
    current_task = &test_tasks[2];
    kernel_wait_queue_block_current_minimum(&queue, 4096u);
    spin_lock_acquisition_count = 0;
    woke = kernel_wait_queue_wake_satisfied(&queue, 512u);
    check_fast_path(woke == 1 && test_tasks[1].state == TASK_READY &&
                        test_tasks[2].state == TASK_BLOCKED &&
                        spin_lock_acquisition_count == 1,
                    "wake must publish the remaining threshold so a retry avoids another lock",
                    &failures);

    /* Removing the waiter that supplied the minimum must publish the new
     * minimum from the remaining queue entries. */
    kernel_wait_queue_init(&queue);
    reset_tasks();
    current_task = &test_tasks[1];
    kernel_wait_queue_block_current_minimum(&queue, 512u);
    current_task = &test_tasks[2];
    kernel_wait_queue_block_current_minimum(&queue, 4096u);
    kernel_wait_queue_remove(&queue, &test_tasks[1]);
    spin_lock_acquisition_count = 0;
    woke = kernel_wait_queue_wake_satisfied(&queue, 512u);
    check_fast_path(woke == 0 && test_tasks[2].state == TASK_BLOCKED &&
                        spin_lock_acquisition_count == 0,
                    "remove must publish the remaining threshold for a lock-free early return",
                    &failures);

    kernel_wait_queue_wake_all(&queue);
    current_task = &test_tasks[1];
    kernel_wait_queue_block_current_minimum(&queue, UINT32_MAX);
    spin_lock_acquisition_count = 0;
    woke = kernel_wait_queue_wake_satisfied(&queue, UINT32_MAX - 1u);
    check_fast_path(woke == 0 && test_tasks[1].state == TASK_BLOCKED &&
                        spin_lock_acquisition_count == 0,
                    "maximum resource requirement must reject a smaller available amount",
                    &failures);
    woke = kernel_wait_queue_wake_satisfied(&queue, UINT32_MAX);
    check_fast_path(woke == 1 && test_tasks[1].state == TASK_READY &&
                        test_tasks[1].waiting_queue == NULL,
                    "maximum available amount must wake a maximum-threshold waiter",
                    &failures);

    return failures ? 1 : 0;
}

static int test_poll_wait_queue_fast_snapshot_keeps_generation_locking(void)
{
    uint32_t failures = 0;
    uint64_t generation;

    reset_tasks();
    generation = kernel_poll_readiness_generation();
    spin_lock_acquisition_count = 0;
    kernel_poll_readiness_notify();
    check_fast_path(kernel_poll_readiness_generation() == generation + 1u &&
                        spin_lock_acquisition_count == 1,
                    "poll notify must publish and take its queue lock even when empty", &failures);

    /* A readiness change between the caller's sample and registration must
     * be detected under poll_waiters.lock rather than lost to an empty fast path. */
    generation = kernel_poll_readiness_generation();
    kernel_poll_readiness_notify();
    current_task = &test_tasks[4];
    spin_lock_acquisition_count = 0;
    int wait_result = kernel_poll_wait_current(generation, 0);
    uint32_t registration_locks = spin_lock_acquisition_count;
    check_fast_path(wait_result == 0 && registration_locks == 1 &&
                        current_task->state == TASK_RUNNING &&
                        !kernel_poll_wait_registered(current_task),
                    "poll registration must lock and reject a stale readiness generation",
                    &failures);

    if (block_global_poll_waiter(4u)) return 1;
    spin_lock_acquisition_count = 0;
    kernel_poll_wait_remove(&test_tasks[4]);
    check_fast_path(spin_lock_acquisition_count == 1 &&
                        !kernel_poll_wait_registered(&test_tasks[4]),
                    "removing a registered poll waiter must unlink it under the queue lock",
                    &failures);
    spin_lock_acquisition_count = 0;
    kernel_poll_wait_remove(&test_tasks[4]);
    check_fast_path(spin_lock_acquisition_count == 0,
                    "removing from an empty poll queue must skip its lock", &failures);

    if (block_global_poll_waiter(4u)) return 1;
    generation = kernel_poll_readiness_generation();
    spin_lock_acquisition_count = 0;
    kernel_poll_readiness_notify();
    check_fast_path(kernel_poll_readiness_generation() == generation + 1u &&
                        test_tasks[4].state == TASK_READY &&
                        !kernel_poll_wait_registered(&test_tasks[4]),
                    "poll notify must still wake a registered waiter", &failures);
    spin_lock_acquisition_count = 0;
    kernel_poll_wait_remove(&test_tasks[4]);
    check_fast_path(spin_lock_acquisition_count == 0,
                    "poll wake must publish the empty snapshot before later removal", &failures);

    return failures ? 1 : 0;
}

int main(void)
{
    int fast_path_failures = 0;
    reset_pipe();
    fast_path_failures |= test_wait_queue_fast_paths_reduce_lock_acquisitions();
    fast_path_failures |= test_poll_wait_queue_fast_snapshot_keeps_generation_locking();
    if (fast_path_failures) return EXIT_FAILURE;
    if (test_8192_writer_threshold_and_data()) return EXIT_FAILURE;
    if (test_multiple_writer_thresholds_and_hol()) return EXIT_FAILURE;
    if (test_multiple_satisfied_writers_retry_without_lost_wakeup()) return EXIT_FAILURE;
    if (test_nonblocking_writer_does_not_queue()) return EXIT_FAILURE;
    if (test_nonblocking_large_full_pipe_returns_eagain()) return EXIT_FAILURE;
    if (test_nonblocking_pipe_buf_write_remains_atomic()) return EXIT_FAILURE;
    if (test_nonblocking_large_write_short_transfers_across_ring_end()) return EXIT_FAILURE;
    if (test_pipe_buf_atomicity_and_contents()) return EXIT_FAILURE;
    if (test_close_eof_and_unconditional_epipe_wake()) return EXIT_FAILURE;
    if (test_generic_queue_minimum_defaults_to_zero()) return EXIT_FAILURE;
    if (test_satisfied_wake_preserves_stopped_waiter()) return EXIT_FAILURE;
    if (test_pipe_close_wake_all_preserves_stopped_waiter()) return EXIT_FAILURE;
    if (test_nonfull_read_does_not_notify_global_poll_waiters()) return EXIT_FAILURE;
    if (test_full_to_nonfull_read_notifies_global_poll_waiters()) return EXIT_FAILURE;
    if (test_pipe_close_still_notifies_global_poll_waiters()) return EXIT_FAILURE;
    if (test_satisfied_writer_wakes_without_nonfull_poll_notification()) return EXIT_FAILURE;
    puts("PASS pipe wake thresholds, multiwriter semantics, close/HUP, and data integrity");
    return EXIT_SUCCESS;
}
