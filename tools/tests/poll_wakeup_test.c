#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/object.h>
#include <reliefnt/sched.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/wait.h>
#include <reliefnt/heap.h>
#include <reliefnt/permissions.h>
#include <reliefnt/pty.h>
#include <reliefnt/audio.h>
#include <reliefnt/net_packet.h>
#include <reliefnt/net_udp.h>
#include <linux/poll.h>

/* Exercise the real AF_UNIX readiness publisher and real poll implementation. */
#include "../../kernel/reliefnt/kernel/syscall/ipc/socket.c"

static struct task current;
static struct kernel_object_table objects;
static uint64_t fake_ticks = 100;
static struct task_file *time_tick_writer;

void kernel_spin_init(struct kernel_spinlock *lock)
{
    lock->state = 0;
}

void kernel_spin_lock(struct kernel_spinlock *lock)
{
    while (__atomic_exchange_n(&lock->state, 1, __ATOMIC_ACQUIRE)) { }
}

void kernel_spin_unlock(struct kernel_spinlock *lock)
{
    __atomic_store_n(&lock->state, 0, __ATOMIC_RELEASE);
}

void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{
    *flags = 0;
    kernel_spin_lock(lock);
}

void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{
    (void)flags;
    kernel_spin_unlock(lock);
}

struct task *sched_current_task(void) { return &current; }
uint32_t sched_current_pid(void) { return current.pid; }
struct task *sched_find(uint32_t pid) { return pid == current.pid ? &current : NULL; }
uint64_t time_ticks(void)
{
    if (time_tick_writer) {
        struct task_file *writer = time_tick_writer;
        time_tick_writer = NULL;
        assert(task_socket_write(writer, "r", 1) == 1);
    }
    return fake_ticks;
}

void sched_sleep_current_until(uint64_t wake_tick)
{
    current.wake_tick = wake_tick;
    current.state = TASK_BLOCKED;
}

void sched_block_current(void) { current.state = TASK_BLOCKED; }

void sched_mark_ready(uint32_t pid)
{
    if (pid == current.pid && current.state == TASK_BLOCKED) {
        current.wake_tick = 0;
        current.state = TASK_READY;
    }
}

void sched_wake_interruptible(struct task *task)
{
    if (task && task->state == TASK_BLOCKED) {
        task->wake_tick = 0;
        task->state = TASK_READY;
    }
}

struct kernel_object_table *kernel_objects(void) { return &objects; }

void *kernel_malloc(size_t size) { return malloc(size); }
void kernel_free(void *pointer) { free(pointer); }

uint32_t kernel_object_insert(struct kernel_object_table *table, void *object,
                              enum kernel_object_type type)
{
    for (uint32_t i = 0; i < KERNEL_OBJECT_MAX; ++i) {
        if (table->objects[i]) continue;
        table->objects[i] = object;
        table->types[i] = (uint16_t)type;
        table->generations[i] = 1;
        return i + 1;
    }
    return KERNEL_HANDLE_INVALID;
}

void *kernel_object_lookup(struct kernel_object_table *table, uint32_t handle,
                           enum kernel_object_type type)
{
    if (!handle || handle > KERNEL_OBJECT_MAX ||
        table->types[handle - 1] != (uint16_t)type) return NULL;
    return table->objects[handle - 1];
}

int kernel_object_remove(struct kernel_object_table *table, uint32_t handle,
                         enum kernel_object_type type, void **removed)
{
    if (!handle || handle > KERNEL_OBJECT_MAX ||
        table->types[handle - 1] != (uint16_t)type) return -1;
    if (removed) *removed = table->objects[handle - 1];
    table->objects[handle - 1] = NULL;
    table->types[handle - 1] = 0;
    return 0;
}

void task_packet_release(struct task_file *file) { (void)file; }
void task_udp_release(struct task_file *file) { (void)file; }
void net_socket_release_fd(int32_t handle) { (void)handle; }
int storage_inode_put(struct storage_inode_ref *reference) { (void)reference; return 0; }
void task_pipe_release(struct task_file *file) { (void)file; }
void task_shm_release(struct task_file *file) { (void)file; }
int audio_device_close(struct task_file *file) { (void)file; return 0; }
int audio_control_close(struct task_file *file) { (void)file; return 0; }
int audio_mixer_close(struct task_file *file) { (void)file; return 0; }
int audio_timer_close(struct task_file *file) { (void)file; return 0; }
int audio_oss_close(struct task_file *file) { (void)file; return 0; }
void input_evdev_release(uint32_t device_kind, uint64_t grab_token)
{ (void)device_kind; (void)grab_token; }
void pty_transfer_put(uint32_t pty_id, uint32_t endpoint) { (void)pty_id; (void)endpoint; }
void pty_reap_hungup(uint32_t pty_id) { (void)pty_id; }

bool user_range_writable(uint64_t pointer, uint64_t size)
{
    return !size || (pointer >= 4096 && size <= UINT64_MAX - pointer);
}

bool user_range_ok(uint64_t pointer, uint64_t size)
{
    return user_range_writable(pointer, size);
}

struct task_file *task_descriptor_for_fd(struct task *task, int fd)
{
    return task && fd == 3 ? &task->files[0] : NULL;
}

int fs_permissions_resolve_flags(const struct task *task, const char *base,
                                 const char *path, char *out, uint32_t capacity,
                                 bool real_ids, uint32_t flags)
{ (void)task; (void)base; (void)path; (void)out; (void)capacity; (void)real_ids; (void)flags; return -1; }

int fs_permissions_check(const struct task *task, const char *path,
                         uint32_t access, bool real_ids)
{ (void)task; (void)path; (void)access; (void)real_ids; return -1; }

uint32_t pty_vt_number(uint32_t pty_id) { (void)pty_id; return 1; }
int pty_is_hungup(uint32_t pty_id) { (void)pty_id; return 0; }
uint32_t pty_input_available(uint32_t pty_id) { (void)pty_id; return 0; }
uint32_t pty_output_available(uint32_t pty_id) { (void)pty_id; return 0; }
short task_signalfd_poll(struct task *task, const struct task_file *file)
{ (void)task; (void)file; return 0; }
short task_pipe_poll(const struct task_file *file, short events)
{ (void)file; (void)events; return 0; }
short net_socket_poll_fd(int32_t handle, uint32_t owner, short events)
{ (void)handle; (void)owner; (void)events; return 0; }
short task_packet_poll(struct task_file *file, short events)
{ (void)file; (void)events; return 0; }
short task_udp_poll(struct task_file *file, short events)
{ (void)file; (void)events; return 0; }
short input_evdev_available_vt(uint32_t kind, uint32_t index, uint64_t cursor,
                               uint32_t vt)
{ (void)kind; (void)index; (void)cursor; (void)vt; return 0; }
short audio_mixer_poll(const struct task_file *file, short events)
{ (void)file; (void)events; return 0; }
short audio_device_poll(const struct task_file *file, short events)
{ (void)file; (void)events; return 0; }
short audio_oss_poll(const struct task_file *file, short events)
{ (void)file; (void)events; return 0; }
short audio_control_poll_file(const struct audio_control_file *file, short events)
{ (void)file; (void)events; return 0; }
short audio_timer_poll(const struct task_file *file, short events)
{ (void)file; (void)events; return 0; }

static void setup_socketpair(struct unix_socket **reader_out,
                             struct unix_socket **writer_out)
{
    struct unix_socket *reader = unix_alloc(current.pid, SOCK_STREAM);
    struct unix_socket *writer = unix_alloc(current.pid, SOCK_STREAM);
    assert(reader && writer);
    reader->state = writer->state = UNIX_SOCKET_CONNECTED;
    reader->peer_handle = writer->handle;
    writer->peer_handle = reader->handle;
    current.files[0] = (struct task_file){
        .used = 1,
        .flags = TASK_FILE_FLAG_SOCKET | TASK_FILE_FLAG_SOCKET_UNIX | RELIEFOS_O_RDWR,
        .aux = reader->handle,
        .node = {.type = RELIEFOS_FS_TYPE_SOCKET},
    };
    *reader_out = reader;
    *writer_out = writer;
}

int main(void)
{
    struct unix_socket *reader, *writer;
    struct task_file writer_file;
    struct pollfd request = {.fd = 3, .events = POLLIN};

    current = (struct task){
        .kind = TASK_KIND_USER,
        .pid = 17,
        .tgid = 17,
        .state = TASK_RUNNING,
    };
    current.limits.nofile.rlim_cur = 64;
    setup_socketpair(&reader, &writer);
    writer_file = (struct task_file){
        .used = 1,
        .flags = TASK_FILE_FLAG_SOCKET | TASK_FILE_FLAG_SOCKET_UNIX | RELIEFOS_O_RDWR,
        .aux = writer->handle,
        .node = {.type = RELIEFOS_FS_TYPE_SOCKET},
    };

    /* Publish a real peer write between the descriptor scan and queue arm.
     * The generation handshake must return for an immediate retry without
     * sleeping, even though the poll task was not queued at publication time. */
    time_tick_writer = &writer_file;
    assert(syscall_poll((uint64_t)(uintptr_t)&request, 1, -1) == KERNEL_SYSCALL_BLOCKED);
    assert(current.state == TASK_RUNNING);
    assert(current.waiting_queue == NULL);
    assert(syscall_poll((uint64_t)(uintptr_t)&request, 1, -1) == 1);
    assert(request.revents & POLLIN);
    char consumed;
    assert(task_socket_read(&current.files[0], &consumed, 1) == 1 && consumed == 'r');
    request.revents = 0;

    /* No tick advances while the peer publishes data. A poll waiter must be
     * made runnable by the real socket wake path before its 10 ms backstop. */
    assert(syscall_poll((uint64_t)(uintptr_t)&request, 1, -1) == KERNEL_SYSCALL_BLOCKED);
    assert(current.state == TASK_BLOCKED);
    assert(current.wake_tick == fake_ticks + 1);
    assert(kernel_poll_wait_registered(&current));
    assert(task_socket_write(&writer_file, "x", 1) == 1);
    assert(fake_ticks == 100);
    assert(current.state == TASK_READY);
    assert(current.waiting_queue == NULL);

    current.state = TASK_RUNNING;
    assert(syscall_poll((uint64_t)(uintptr_t)&request, 1, -1) == 1);
    assert(request.revents & POLLIN);

    /* A readiness publication must not turn a stopped polling task runnable. */
    assert(task_socket_read(&current.files[0], &consumed, 1) == 1);
    request.revents = 0;
    assert(syscall_poll((uint64_t)(uintptr_t)&request, 1, -1) == KERNEL_SYSCALL_BLOCKED);
    assert(kernel_poll_wait_registered(&current));
    current.state = TASK_STOPPED;
    assert(task_socket_write(&writer_file, "y", 1) == 1);
    assert(current.state == TASK_STOPPED);
    assert(current.waiting_queue == NULL);
    current.state = TASK_RUNNING; /* model an independent SIGCONT */
    assert(syscall_poll((uint64_t)(uintptr_t)&request, 1, -1) == 1);
    assert(request.revents & POLLIN);

    assert(task_socket_read(&current.files[0], &consumed, 1) == 1);
    request.revents = 0;
    assert(syscall_poll((uint64_t)(uintptr_t)&request, 1, -1) == KERNEL_SYSCALL_BLOCKED);
    current.state = TASK_EXITED;
    kernel_poll_readiness_notify();
    assert(current.state == TASK_EXITED);
    assert(current.waiting_queue == NULL);
    assert(!kernel_poll_wait_registered(&current));
    current.state = TASK_RUNNING;
    current.poll_deadline_ticks = 0;
    assert(syscall_poll(0, 0, 20) == -RELIEFOS_EAGAIN);
    assert(current.state == TASK_RUNNING);
    assert(!kernel_poll_wait_registered(&current));

    /* Repeated backstop rescans must retain the original finite deadline,
     * rather than extending it every time the task re-enters the syscall. */
    current.poll_deadline_ticks = 0;
    assert(syscall_poll((uint64_t)(uintptr_t)&request, 1, 20) == KERNEL_SYSCALL_BLOCKED);
    assert(current.poll_deadline_ticks == 103); /* existing tick rounding */
    fake_ticks = 101;
    current.state = TASK_RUNNING; /* model the timer backstop selecting us */
    assert(syscall_poll((uint64_t)(uintptr_t)&request, 1, 20) == KERNEL_SYSCALL_BLOCKED);
    assert(current.poll_deadline_ticks == 103);
    fake_ticks = 103;
    current.state = TASK_RUNNING;
    assert(syscall_poll((uint64_t)(uintptr_t)&request, 1, 20) == 0);
    assert(!current.poll_deadline_ticks && !current.waiting_queue);
    assert(!kernel_poll_wait_registered(&current));
    puts("PASS poll readiness wake, stopped/exited cleanup, finite timeout, and nfds=0 fallback");
    return 0;
}
