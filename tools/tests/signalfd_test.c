#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/reliefnt/kernel/reliefnt/signal.c"
#include "../../kernel/reliefnt/kernel/reliefnt/signal_queue.c"
#include "../../kernel/reliefnt/kernel/reliefnt/signalfd.c"

static struct task readers[2], *current;
static struct task_file descriptor;
static unsigned allocations, reconfigured, sleeps;
static unsigned runnable_notifications;
static uint64_t fault_at;
void power_reboot(void) { abort(); }
void power_shutdown(void) { abort(); }
void *kernel_malloc(size_t n) { void *p = malloc(n); if (p) ++allocations; return p; }
void kernel_free(void *p) { if (p) { assert(allocations); --allocations; free(p); } }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ assert(!lock->state); lock->state = 1; *flags = 0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)flags; assert(lock->state); lock->state = 0; }
struct task *sched_current_task(void) { return current; }
struct task *sched_find(uint32_t pid) { return pid == 10 ? &readers[0] : pid == 11 ? &readers[1] : NULL; }
void sched_alarm_rearm(struct task *task) { (void)task; }
void sched_exit_group(uint32_t pid, uint64_t code) { (void)pid; (void)code; assert(0); }
void sched_signal_job_control(uint32_t pid, int sig) { (void)pid; (void)sig; assert(0); }
void sched_signal_discard(struct task *task, int sig) { (void)task; (void)sig; assert(0); }
bool user_range_ok(uint64_t p, uint64_t n) { return !n || (p >= 4096 && n <= UINT64_MAX - p); }
int user_copy_to_task(struct task *task, uint64_t pointer, const void *source, uint64_t size)
{
    assert(task == current);
    if (!user_range_ok(pointer, size)) return -LINUX_EFAULT;
    if (fault_at && pointer + size > fault_at) {
        if (pointer < fault_at) memcpy((void *)pointer, source, fault_at - pointer);
        return -LINUX_EFAULT;
    }
    memcpy((void *)pointer, source, size);
    return 0;
}
struct task_file *task_file_for_fd(struct task *task, int fd)
{ (void)task; return descriptor.used && (fd == 3 || fd == 4) ? &descriptor : NULL; }
int task_allocate_fd(struct task *task, int minimum, struct task_file **out)
{ (void)task; assert(!minimum); if (descriptor.used) return -LINUX_EMFILE; descriptor.used = 1; *out = &descriptor; return 3; }
void sched_signalfd_reconfigure(struct task *task) { assert(task == current); ++reconfigured; }
void sched_notify_runnable(void) { ++runnable_notifications; }
void sched_signal_wait_current(uint64_t deadline)
{ assert(!deadline); ++sleeps; current->state = TASK_BLOCKED; }

static void enqueue(struct task *task, bool process, int signal, uint64_t value)
{
    struct linux_siginfo info = {.signo = signal, .code = LINUX_SI_QUEUE, .error = 7,
        .fields.realtime = {.pid = 123, .uid = 456, .value = value}};
    assert(kernel_signal_enqueue(task, process, signal, &info) == 0);
}

int main(void)
{
    for (unsigned i = 0; i < 2; ++i) {
        readers[i].pid = readers[i].tgid = 10 + i;
        readers[i].kind = TASK_KIND_USER;
        readers[i].state = TASK_READY;
        readers[i].blocked_signals = UINT64_MAX;
        readers[i].limits.sigpending.rlim_cur = 32;
    }
    current = &readers[0];
    uint64_t mask = UINT64_MAX;
    assert(syscall_signalfd(-2, 1, 0, ~0u) == -LINUX_EINVAL);
    assert(syscall_signalfd(-2, 1, 8, ~0u) == -LINUX_EFAULT);
    assert(syscall_signalfd(-2, (uintptr_t)&mask, 8, ~0u) == -LINUX_EINVAL);
    assert(syscall_signalfd(-2, (uintptr_t)&mask, 8, 0) == -LINUX_EBADF);
    assert(syscall_signalfd(-1, (uintptr_t)&mask, 8, LINUX_SFD_NONBLOCK | LINUX_SFD_CLOEXEC) == 3);
    assert(descriptor.kind == TASK_FILE_KIND_SIGNALFD && descriptor.fd_flags == 1);
    assert(descriptor.aux == (mask & ~((1ULL << 8) | (1ULL << 18))));
    assert(syscall_signalfd(-1, (uintptr_t)&mask, 8, 0) == -LINUX_EMFILE);
    descriptor.kind = 0;
    assert(syscall_signalfd(3, (uintptr_t)&mask, 8, 0) == -LINUX_EINVAL);
    descriptor.kind = TASK_FILE_KIND_SIGNALFD;
    mask = (1ULL << 34) | (1ULL << 35);
    assert(syscall_signalfd(4, (uintptr_t)&mask, 8, 0) == 4 && reconfigured == 1);
    assert(descriptor.fd_flags == 1 && (descriptor.flags & LINUX_O_NONBLOCK));
    struct linux_signalfd_siginfo output[3];
    assert(task_signalfd_read(current, &descriptor, (uintptr_t)output, 0) == -LINUX_EINVAL);
    assert(task_signalfd_read(current, &descriptor, 1, 128) == -LINUX_EAGAIN);
    struct iovec oversized[2] = {{(void *)1, INT64_MAX}, {0, 0}};
    assert(task_signalfd_readv(current, &descriptor, (uintptr_t)oversized, 1, 0) == -LINUX_EAGAIN);
    assert(task_signalfd_readv(current, &descriptor, (uintptr_t)oversized, 2, 0) == -LINUX_EFAULT);
    enqueue(current, false, 35, 0x123456789abcdef0ULL);
    enqueue(current, true, 35, 2);
    enqueue(current, false, 35, 3);
    assert(task_signalfd_poll(current, &descriptor) == POLLIN);
    assert(task_signalfd_read(current, &descriptor, (uintptr_t)output, 383) == 256);
    assert(output[0].value_ptr == 0x123456789abcdef0ULL && output[0].value_int == (int32_t)0x9abcdef0u);
    assert(output[0].pid == 123 && output[0].uid == 456 && output[0].error == 7);
    assert(output[1].value_ptr == 3);
    current = &readers[1];
    assert(task_signalfd_poll(current, &descriptor) == 0);
    assert(task_signalfd_read(current, &descriptor, (uintptr_t)output, sizeof(output)) == -LINUX_EAGAIN);
    current->tgid = 10;
    current->shared_process_pending = &readers[0].process_pending_signals;
    current->shared_process_signal_queue = &readers[0].process_signal_queue;
    assert(task_signalfd_read(current, &descriptor, (uintptr_t)output, 128) == 128 && output[0].value_ptr == 2);
    enqueue(current, false, 35, 4);
    enqueue(current, false, 35, 5);
    fault_at = (uintptr_t)output + 160;
    memset(output, 0xa5, sizeof(output));
    assert(task_signalfd_read(current, &descriptor, (uintptr_t)output, sizeof(output)) == 128);
    assert(output[0].value_ptr == 4 && output[1].signo == 35 && ((unsigned char *)output)[160] == 0xa5);
    assert(!task_signalfd_poll(current, &descriptor) && !allocations);
    fault_at = 0;
    enqueue(current, false, 35, 6);
    struct iovec vectors[3] = {{(char *)output, 3}, {(char *)output + 3, 20}, {(char *)output + 23, 105}};
    assert(task_signalfd_readv(current, &descriptor, (uintptr_t)vectors, 3, 0) == 128);
    assert(output[0].value_ptr == 6);
    descriptor.flags &= ~LINUX_O_NONBLOCK;
    assert(task_signalfd_readv(current, &descriptor, (uintptr_t)vectors, 3, 8) == -LINUX_EAGAIN);
    assert(task_signalfd_readv(current, &descriptor, (uintptr_t)vectors, 3, 0x30) == -LINUX_EINVAL);
    assert(task_signalfd_readv(current, &descriptor, (uintptr_t)vectors, 3, 0x40) == -LINUX_EOPNOTSUPP);
    assert(task_signalfd_readv(current, &descriptor, 0, 0, ~0u) == 0);
    assert(task_signalfd_read(current, &descriptor, (uintptr_t)output, 128) == KERNEL_SYSCALL_BLOCKED);
    assert(sleeps == 1 && current->signalfd_waiting && current->signalfd_wait_mask == mask);
    struct linux_siginfo sent = {.signo = 35, .code = LINUX_SI_QUEUE, .fields.realtime.value = 7};
    unsigned wake_before = runnable_notifications;
    assert(kernel_signal_queue_task_info(current, 35, &sent) == 0 && current->state == TASK_READY);
    assert(runnable_notifications == wake_before + 1);
    assert(task_signalfd_read(current, &descriptor, (uintptr_t)output, 128) == 128);
    assert(!current->signalfd_waiting && !current->signalfd_wait_mask && output[0].value_ptr == 7);
    assert(task_signalfd_readv(current, &descriptor, (uintptr_t)vectors, 3, 0) == KERNEL_SYSCALL_BLOCKED);
    assert(current->signalfd_vectors == current->signalfd_fast_vectors && !allocations);
    vectors[0].iov_base = (void *)1;
    enqueue(current, false, 35, 8);
    assert(task_signalfd_readv(current, &descriptor, (uintptr_t)vectors, 3, 0) == 128);
    assert(output[0].value_ptr == 8 && !current->signalfd_vectors && !allocations);

    /* Caught handlers and sigwait sleepers also need a runnable notification. */
    const uint64_t wake_bit = 1ULL << (12 - 1);
    current->signalfd_waiting = false;
    current->state = TASK_BLOCKED;
    current->blocked_signals &= ~wake_bit;
    current->sigwait_mask = 0;
    current->signal_actions[12].handler = 0x410000;
    wake_before = runnable_notifications;
    sent = (struct linux_siginfo){.signo = 12, .code = LINUX_SI_QUEUE};
    assert(kernel_signal_queue_task_info(current, 12, &sent) == 0);
    assert(current->state == TASK_READY && runnable_notifications == wake_before + 1);
    kernel_signal_flush(current, false, wake_bit);

    current->state = TASK_BLOCKED;
    current->blocked_signals |= wake_bit;
    current->sigwait_mask = wake_bit;
    current->signal_actions[12].handler = 0;
    wake_before = runnable_notifications;
    assert(kernel_signal_queue_task_info(current, 12, &sent) == 0);
    assert(current->state == TASK_READY && runnable_notifications == wake_before + 1);
    kernel_signal_flush(current, false, wake_bit);
    current->sigwait_mask = 0;

    sent = (struct linux_siginfo){.signo = 7, .code = 4, .fields.fault = {0x1122334455667788ULL, 9}};
    output[0] = signalfd_info(&sent);
    assert(output[0].address == sent.fields.fault.address && output[0].address_lsb == 9);
    sent = (struct linux_siginfo){.signo = 17, .code = 1, .fields.child = {2, 3, 4, 0, 5, 6}};
    output[0] = signalfd_info(&sent);
    assert(output[0].pid == 2 && output[0].uid == 3 && output[0].status == 4 && output[0].utime == 5 && output[0].stime == 6);
    sent = (struct linux_siginfo){.signo = 31, .code = 1, .fields.sys = {0x12345678, 44, 0xc000003e}};
    output[0] = signalfd_info(&sent);
    assert(output[0].call_address == 0x12345678 && output[0].syscall == 44 && output[0].arch == 0xc000003e);
    assert(!allocations);
    puts("PASS real signalfd/queues: masks, reader identity, FIFO, readv records, copy faults, blocking wake, siginfo layouts");
}
