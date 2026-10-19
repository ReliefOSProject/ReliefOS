#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/paging.h>

/* Let the usercopy fixture treat host backing memory as the direct map. */
#undef RELIEFNT_KERNEL_DIRECT_MAP_BASE
#define RELIEFNT_KERNEL_DIRECT_MAP_BASE 0

#include "../../kernel/reliefnt/kernel/reliefnt/sched/sched.c"
#include "sysv_shm_fixture_stubs.h"
void signal_fixture_exit_group(uint32_t tgid, uint64_t code);
void signal_fixture_job_control(uint32_t tgid, int signal_number);
#define sched_exit_group signal_fixture_exit_group
#define sched_signal_job_control signal_fixture_job_control
#include "../../kernel/reliefnt/kernel/reliefnt/signal.c"
#undef sched_exit_group
#undef sched_signal_job_control
#include "../../kernel/reliefnt/kernel/reliefnt/signal_queue.c"
#include "../../kernel/reliefnt/kernel/exec/usercopy_task.c"

#define STACK_ADDRESS 0x10000000ULL
#define ALARM_SIGNAL 14
#define ALARM_BIT (1ULL << (ALARM_SIGNAL - 1))
#define FUTEX_WAIT_PRIVATE 0x80ULL

static struct task *fixture_task;
static unsigned char user_stack[8192] __attribute__((aligned(4096)));
static uint32_t fake_cpu;
static uint64_t fake_time;
static unsigned futex_cancellations;
static bool synccpu_uninterruptible;

uint32_t smp_current_cpu(void) { return fake_cpu; }
uint32_t smp_cpu_count(void) { return 1; }
bool smp_cpu_online(uint32_t cpu) { return cpu == 0; }
uint64_t paging_kernel_cr3(void) { return 0x1000; }
void paging_load_cr3(uint64_t cr3) { assert(cr3 == 0x1000); }
uint64_t time_uptime_us(void) { return fake_time; }
uint64_t time_ticks(void) { return fake_time / 10000; }
uint64_t mm_total_memory_kib(void) { return 1024 * 1024; }

void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{
    assert(lock && !lock->state);
    lock->state = 1;
    *flags = 0;
}
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{
    (void)flags;
    assert(lock && lock->state);
    lock->state = 0;
}
void kernel_spin_init(struct kernel_spinlock *lock)
{ if (lock) lock->state = 0; }
void kernel_execution_lock_irqsave(uint64_t *flags) { *flags = 0; }
void kernel_execution_unlock_irqrestore(uint64_t flags) { (void)flags; }
void *kernel_malloc(size_t size) { return malloc(size); }
void kernel_free(void *memory) { free(memory); }
void console_log(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }

void power_reboot(void) { abort(); }
void power_shutdown(void) { abort(); }
void signal_fixture_exit_group(uint32_t tgid, uint64_t code)
{
    assert(fixture_task && tgid == fixture_task->pid);
    fixture_task->exit_signal = (uint32_t)(code - 128);
    fixture_task->state = TASK_EXITED;
}
void signal_fixture_job_control(uint32_t tgid, int signal_number)
{
    assert(fixture_task && tgid == fixture_task->pid);
    assert(signal_number >= 19 && signal_number <= 22);
    fixture_task->state = TASK_STOPPED;
}
void storage_drain_task_io(uint32_t pid) { (void)pid; }
void kernel_wait_queue_remove(struct kernel_wait_queue *queue, struct task *task)
{ (void)queue; (void)task; assert(0); }
void syscall_record_lock_cancel(struct task *task) { (void)task; }
void task_release_syscall_file(struct task *task)
{ task->syscall_file = NULL; }
uint64_t task_socket_cancel_receive(struct task *task)
{ uint64_t done = task->socket_receive_done; task->socket_receive_done = 0; return done; }
int64_t task_socket_mmsg_interrupt(struct task *task, uint64_t received)
{ (void)task; (void)received; return -LINUX_EINTR; }
int time_clock_get(int32_t clock_id, struct linux_timespec *value)
{ (void)clock_id; (void)value; return -LINUX_EINVAL; }
void futex_cancel_wait(struct task *task)
{
    assert(task == fixture_task && task->futex_state == 1);
    task->futex_state = 0;
    task->futex_next = NULL;
    task->futex_deadline = 0;
    ++futex_cancellations;
}
bool drm_vmwgfx_task_uninterruptible(const struct task *task)
{ (void)task; return synccpu_uninterruptible; }
bool drm_vmwgfx_task_signal_busy(const struct task *task)
{ (void)task; return false; }
int syscall_handle_task_page_fault(struct task *task, uint64_t address,
                                   uint64_t error)
{ (void)task; (void)address; (void)error; return 0; }

bool address_space_user_page_writable(const struct address_space *as,
                                      uint64_t address)
{ return as == sched_task_as(fixture_task) && user_range_writable(address, 1); }
bool address_space_user_page_readable(const struct address_space *as,
                                      uint64_t address)
{ return as == sched_task_as(fixture_task) && user_range_ok(address, 1); }
bool address_space_handle_cow_fault(struct address_space *as, uint64_t address)
{ (void)as; (void)address; return false; }
uint64_t address_space_user_page_phys(const struct address_space *as,
                                      uint64_t address)
{
    assert(as == sched_task_as(fixture_task));
    assert(address >= STACK_ADDRESS && address < STACK_ADDRESS + sizeof(user_stack));
    return (uintptr_t)user_stack + ((address - STACK_ADDRESS) & ~4095ULL);
}
bool user_range_writable(uint64_t address, uint64_t size)
{
    uint64_t end = STACK_ADDRESS + sizeof(user_stack);
    return address >= STACK_ADDRESS && address <= end && size <= end - address;
}
bool user_range_ok(uint64_t address, uint64_t size)
{
    uint64_t end = STACK_ADDRESS + sizeof(user_stack);
    return address >= STACK_ADDRESS && address <= end && size <= end - address;
}

static void discard_fixture(void)
{
    if (!tasks) return;
    if (fixture_task) kernel_signal_flush(fixture_task, false, UINT64_MAX);
    for (uint32_t i = 0; i < task_count; ++i) free(tasks[i]);
    free(tasks);
    tasks = NULL;
    task_count = task_capacity = 0;
    fixture_task = NULL;
}

static struct task *start_fixture(void)
{
    discard_fixture();
    fake_cpu = 0;
    fake_time = 100000;
    futex_cancellations = 0;
    synccpu_uninterruptible = false;
    memset(user_stack, 0, sizeof(user_stack));
    sched_init();

    fixture_task = alloc_task_slot();
    assert(fixture_task);
    fixture_task->pid = 41;
    fixture_task->kind = TASK_KIND_USER;
    fixture_task->state = TASK_READY;
    fixture_task->running_cpu = SCHED_CPU_NONE;
    fixture_task->entry = RELIEFNT_USER_BASE + 0x1000;
    fixture_task->stack_low = STACK_ADDRESS;
    fixture_task->stack_top = STACK_ADDRESS + sizeof(user_stack);
    fixture_task->affinity_mask = 1;
    sched_task_as(fixture_task)->cr3 = 0x2000;
    fixture_task->frame = (struct trap_frame){
        .rip = fixture_task->entry,
        .rsp = fixture_task->stack_top - 64,
        .cs = RELIEFNT_USER_CS,
        .ss = RELIEFNT_USER_DS,
        .rflags = 1ULL << 9,
    };
    sched_task_actions(fixture_task)[ALARM_SIGNAL] =
        (struct kernel_signal_action){
            .handler = RELIEFNT_USER_BASE + 0x2000,
            .restorer = RELIEFNT_USER_BASE + 0x3000,
            .flags = LINUX_SA_RESTART,
        };
    assert(sched_select_next_user() == fixture_task);
    assert(fixture_task->state == TASK_RUNNING);
    return fixture_task;
}

static struct trap_frame untimed_private_futex_frame(const struct task *task)
{
    struct trap_frame frame = task->frame;
    frame.rip = task->entry + 2;
    frame.rsp = task->stack_top - 64;
    frame.rax = __NR_futex;
    frame.rdi = STACK_ADDRESS + 32;
    frame.rsi = FUTEX_WAIT_PRIVATE;
    frame.rdx = 0;
    frame.r10 = 0; /* no timeout: the native Xorg case has no timer wake */
    frame.cs = RELIEFNT_USER_CS;
    frame.ss = RELIEFNT_USER_DS;
    frame.rflags = 1ULL << 9;
    return frame;
}

static void assert_handler_context(const struct trap_frame *frame,
                                  uint64_t original_rip)
{
    struct linux_rt_sigframe saved;
    memcpy(&saved, user_stack + (frame->rsp - STACK_ADDRESS), sizeof(saved));
    assert(saved.uc.context.rip == original_rip);
    assert(saved.uc.context.rax == __NR_futex);
    assert(saved.uc.context.rsi == FUTEX_WAIT_PRIVATE);
    assert(saved.uc.context.r10 == 0);
    assert(frame->rip == RELIEFNT_USER_BASE + 0x2000);
    assert(frame->rdi == ALARM_SIGNAL);
    assert(fixture_task->blocked_signals & ALARM_BIT);
    assert(!(fixture_task->pending_signals & ALARM_BIT));
    assert(fixture_task->restart_syscall == 0);
    assert(futex_cancellations == 1);
    assert(fixture_task->futex_state == 0);
}

static void capture_and_reselect(struct task *task, struct trap_frame *frame)
{
    assert(sched_capture_current_user_frame(frame));
    assert(task->state == TASK_READY);
    assert(task->frame.rip == frame->rip);
    assert(sched_select_next_user() == task);
    assert(task->state == TASK_RUNNING);
}

static void test_running_signal_then_untimed_futex_park(void)
{
    struct task *task = start_fixture();
    struct trap_frame frame = untimed_private_futex_frame(task);
    uint64_t original_rip = frame.rip;

    task->restart_syscall = __NR_futex + 1;
    task->futex_state = 1;
    assert(kernel_signal_queue_task(task, ALARM_SIGNAL) == 0);
    assert(task->state == TASK_RUNNING);
    assert(task->pending_signals & ALARM_BIT);

    /* Reproduce enqueue-while-running followed by the untimed futex park. */
    sched_block_current();
    assert(task->state == TASK_BLOCKED && task->wake_tick == 0);
    assert(kernel_signal_deliver_pending(task, &frame) == 1);
    assert_handler_context(&frame, original_rip);
    assert(task->state == TASK_READY);
    assert(task->wake_tick == 0);
    capture_and_reselect(task, &frame);

    /* A timed syscall wait is also interrupted by the installed handler. */
    task->blocked_signals &= ~ALARM_BIT;
    task->restart_syscall = __NR_futex + 1;
    task->futex_state = 1;
    frame = untimed_private_futex_frame(task);
    original_rip = frame.rip;
    assert(kernel_signal_queue_task(task, ALARM_SIGNAL) == 0);
    sched_sleep_current_until(27);
    assert(task->state == TASK_BLOCKED && task->wake_tick == 27);
    assert(kernel_signal_deliver_pending(task, &frame) == 1);
    assert(task->state == TASK_READY && task->wake_tick == 0);
    /* Check restart semantics without depending on accumulated test counts. */
    struct linux_rt_sigframe saved;
    memcpy(&saved, user_stack + (frame.rsp - STACK_ADDRESS), sizeof(saved));
    assert(saved.uc.context.rip == original_rip);
    assert(saved.uc.context.rsi == FUTEX_WAIT_PRIVATE && saved.uc.context.r10 == 0);
    capture_and_reselect(task, &frame);

    /* A delivered signal does not convert an already RUNNING task to READY. */
    task->blocked_signals &= ~ALARM_BIT;
    assert(kernel_signal_queue_task(task, ALARM_SIGNAL) == 0);
    frame = untimed_private_futex_frame(task);
    assert(kernel_signal_deliver_pending(task, &frame) == 1);
    assert(task->state == TASK_RUNNING);
    capture_and_reselect(task, &frame);

    /* Nor does it disturb a READY task that still owns its captured frame. */
    task->blocked_signals &= ~ALARM_BIT;
    struct trap_frame pre_delivery = task->frame;
    assert(sched_capture_current_user_frame(&pre_delivery));
    assert(task->state == TASK_READY);
    assert(kernel_signal_queue_task(task, ALARM_SIGNAL) == 0);
    frame = untimed_private_futex_frame(task);
    assert(kernel_signal_deliver_pending(task, &frame) == 1);
    assert(task->state == TASK_READY);
    capture_and_reselect(task, &frame);

    /* A masked signal remains pending and does not release an untimed wait. */
    task->blocked_signals |= ALARM_BIT;
    assert(kernel_signal_queue_task(task, ALARM_SIGNAL) == 0);
    frame = untimed_private_futex_frame(task);
    sched_block_current();
    assert(kernel_signal_deliver_pending(task, &frame) == 0);
    assert(task->state == TASK_BLOCKED && task->pending_signals & ALARM_BIT);
    kernel_signal_flush(task, false, ALARM_BIT);
    task->blocked_signals &= ~ALARM_BIT;
    sched_wake_interruptible(task);
    capture_and_reselect(task, &frame);

    /* The existing uninterruptible SYNCCPU guard still owns its wait. */
    synccpu_uninterruptible = true;
    assert(kernel_signal_queue_task(task, ALARM_SIGNAL) == 0);
    frame = untimed_private_futex_frame(task);
    sched_block_current();
    assert(kernel_signal_deliver_pending(task, &frame) == 0);
    assert(task->state == TASK_BLOCKED && task->pending_signals & ALARM_BIT);
    synccpu_uninterruptible = false;
    kernel_signal_flush(task, false, ALARM_BIT);
    sched_wake_interruptible(task);
    capture_and_reselect(task, &frame);

    /* vfork parents remain parked until their child releases the relation. */
    struct task vfork_child = {0};
    task->vfork_child = &vfork_child;
    assert(kernel_signal_queue_task(task, ALARM_SIGNAL) == 0);
    frame = untimed_private_futex_frame(task);
    sched_block_current();
    assert(kernel_signal_deliver_pending(task, &frame) == 0);
    assert(task->state == TASK_BLOCKED && task->pending_signals & ALARM_BIT);
    sched_wake_interruptible(task);
    assert(task->state == TASK_BLOCKED);
    task->vfork_child = NULL;
    kernel_signal_flush(task, false, ALARM_BIT);
    sched_wake_interruptible(task);
    capture_and_reselect(task, &frame);

    /* Default job-control delivery keeps a task stopped; later generic wake
     * events cannot promote it to READY. */
    task->state = TASK_RUNNING;
    assert(kernel_signal_queue_task(task, 20) == 0);
    assert(task->state == TASK_STOPPED);
    sched_wake_interruptible(task);
    assert(task->state == TASK_STOPPED);

    /* A failed frame takes the fatal SIGSEGV path without publishing READY. */
    task->state = TASK_RUNNING;
    task->blocked_signals &= ~ALARM_BIT;
    assert(kernel_signal_queue_task(task, ALARM_SIGNAL) == 0);
    frame = untimed_private_futex_frame(task);
    frame.rsp = RELIEFNT_USER_BASE + 512;
    sched_block_current();
    assert(kernel_signal_deliver_pending(task, &frame) == 0);
    assert(task->state == TASK_EXITED && task->exit_signal == 11);
    sched_wake_interruptible(task);
    assert(task->state == TASK_EXITED);

    discard_fixture();
}

int main(void)
{
    test_running_signal_then_untimed_futex_park();
    puts("PASS signal delivery wakes a parked SA_RESTART untimed private futex and preserves scheduler ownership");
    return 0;
}
