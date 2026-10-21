#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <reliefnt/arch.h>
#include <reliefnt/console.h>
#include <reliefnt/input.h>
#include <reliefnt/sched.h>
#include <reliefnt/signal.h>
#include <reliefnt/userland.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/storage.h>
#include <reliefnt/framebuffer.h>
#include <reliefos/fb.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/fb.h>
#include <reliefnt/futex.h>
#define RELIEFOS_FLOCK_NB 4u

/* Real execution admission, including queued-writer exclusion. Privileged
 * IRQ operations and loader/scheduler boundaries are the fixture's scope. */
#include "../../kernel/reliefnt/kernel/sync/lock.c"
static _Thread_local uint32_t cpu;
static _Thread_local uint64_t irq_state;
static struct task current, replacement;
static struct task *selected;
static atomic_bool key_pending;
static unsigned drained, delivered, released, fpu_saved, fpu_restored;
static uint64_t installed_fs;

uint32_t smp_current_cpu(void) { return cpu; }
void smp_membarrier_poll(void) { }
uint64_t kernel_irq_save(void) { uint64_t saved=irq_state; irq_state=0; return saved; }
void kernel_irq_restore(uint64_t saved) { irq_state=saved; }
static bool exclusive(void) { return execution_owner==cpu && execution_depth[cpu]==1; }
void input_process_pending(void)
{ assert(exclusive()); if (atomic_exchange(&key_pending, false)) ++drained; }
struct task *sched_current_task(void) { return &current; }
bool sched_try_resume_current_user_frame(const struct trap_frame *frame)
{
    assert(exclusive());
    if (!frame || (frame->cs&3)!=3 || current.state!=TASK_RUNNING ||
        current.flags&TASK_FLAG_PENDING_LOAD ||
        sched_task_pending(&current)&~current.blocked_signals) return false;
    current.frame=*frame;
    return true;
}
bool sched_capture_current_user_frame(const struct trap_frame *frame)
{ assert(exclusive()); current.frame=*frame; return true; }
struct task *sched_select_next_user(void) { assert(exclusive()); return selected; }
struct task *sched_reclaim_current_user(void) { assert(exclusive()); return selected; }
void sched_quiesce_exited_current(void) { assert(exclusive()); }
void sched_release_task_resources(struct task *task)
{ assert(exclusive() && task==&current); ++released; }
void sched_exit(uint32_t pid, uint64_t code) { (void)pid;(void)code; assert(false); }
int kernel_signal_deliver_pending(struct task *task, struct trap_frame *frame)
{
    assert(exclusive());
    if (sched_task_pending(task)&~task->blocked_signals) {
        ++delivered; task->pending_signals=task->process_pending_signals=0;
        frame->rax=0x51a7;
    }
    return 0;
}
void arch_set_user_fs(uint64_t base)
{
    assert(!irq_state && exclusive());
    installed_fs=base;
}
void arch_fpu_save(void *state) { (void)state; assert(exclusive()); ++fpu_saved; }
void arch_fpu_restore(const void *state) { (void)state; assert(exclusive()); ++fpu_restored; }
uint64_t paging_kernel_cr3(void) { return 0x1000; }
void paging_load_cr3(uint64_t cr3) { assert(exclusive() && cr3==0x1000); }
void console_log(enum console_log_level level, const char *format, ...)
{ (void)level;(void)format; assert(exclusive()); }
static bool userland_load_task_image(struct task *task)
{ (void)task; assert(exclusive()); return true; }
static struct task *wait_for_runnable_task(uint64_t *flags)
{ (void)flags; assert(false); return NULL; }

/* The runner inserts the complete, unmodified production dispatch function.
 * The real scheduler's eligibility rules have separate sched_resume coverage. */
#include "user_return_body.inc"

static void reset(void)
{
    cpu=0; irq_state=0;
    assert(!execution_readers && execution_next_ticket==execution_serving_ticket);
    memset(&current,0,sizeof(current)); memset(&replacement,0,sizeof(replacement));
    current.pid=41;current.kind=TASK_KIND_USER;current.state=TASK_RUNNING;
    current.fs_base=0x1234000;
    current.frame=(struct trap_frame){.rip=0x4001000,.rsp=0x7fff000,.cs=0x23,.ss=0x1b};
    replacement.pid=42;replacement.kind=TASK_KIND_USER;replacement.state=TASK_RUNNING;
    replacement.fs_base=0x5678000;replacement.frame=current.frame;
    selected=&current;drained=delivered=released=fpu_saved=fpu_restored=0;
    installed_fs=0;
    atomic_store(&key_pending,false);
}
static bool async_io, inject_input;
static int64_t backend_result;
static unsigned slept, io_releases;
static bool syscall_trace_task(const struct task *task) { (void)task; return false; }
static int task_begin_syscall_io(struct task *task, uint64_t nr, int32_t fd)
{ (void)task; (void)nr; (void)fd; assert(exclusive()); return 0; }
void task_release_syscall_file(struct task *task) { (void)task; assert(exclusive()); }
static int syscall_eagain_is_nonblocking_device(struct task *task, uint64_t nr, uint64_t fd)
{ (void)task; (void)nr; (void)fd; return 0; }
uint32_t drm_vmwgfx_process_retirements(uint32_t budget)
{ (void)budget; assert(exclusive()); return 0; }
void driver_manager_process_audio_faults(void) { assert(exclusive()); }
void task_sysv_sem_cancel(struct task *task) { (void)task; assert(false); }
void task_sysv_msg_cancel(struct task *task) { (void)task; assert(false); }
void task_fifo_cancel(struct task *task) { (void)task; assert(false); }
uint32_t sched_current_pid(void) { return current.pid; }
void storage_release_task_io(uint32_t pid) { assert(exclusive() && pid==current.pid); ++io_releases; }
void storage_set_io_async_context(bool enabled) { assert(exclusive()); async_io=enabled; }
uint64_t time_ticks(void) { return 100; }
void sched_sleep_current_until(uint64_t tick)
{ assert(exclusive() && tick==101); current.state=TASK_BLOCKED; ++slept; }
int64_t kernel_signal_rt_sigreturn(struct task *task, struct trap_frame *frame)
{ assert(exclusive() && task==&current); frame->rax=0x5151; return 0; }
int64_t sched_clone_current(const struct trap_frame *frame, uint64_t flags,
    uint64_t stack, uint64_t parent, uint64_t child, uint64_t tls)
{ (void)frame;(void)flags;(void)stack;(void)parent;(void)child;(void)tls;assert(false);return 0; }
int64_t syscall_clone3(const struct trap_frame *frame, uint64_t args, uint64_t size)
{ (void)frame;(void)args;(void)size;assert(false);return 0; }
int64_t sched_fork_current(const struct trap_frame *frame)
{ (void)frame;assert(false);return 0; }
int64_t sched_vfork_current(const struct trap_frame *frame)
{ (void)frame;assert(false);return 0; }
static int64_t backend(struct task *task, uint64_t nr, uint64_t a0,
    uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)nr;(void)a0;(void)a1;(void)a2;(void)a3;(void)a4;(void)a5;
    assert(exclusive() && async_io && task==&current);
    if (inject_input) atomic_store(&key_pending,true);
    return backend_result;
}
#define RELIEFOS_SYSCALL_FRAME_DISPATCH backend
#include "syscall_return_body.inc"
#include "int80_return_body.inc"

static struct trap_frame prepare(uint64_t number)
{
    reset();async_io=inject_input=false;backend_result=123;slept=io_releases=0;
    struct trap_frame frame=current.frame;frame.rax=number;return frame;
}
static void test_one_transaction_for_completed_syscall(void)
{
    struct trap_frame frame=prepare(LINUX_SYS_GETPID);
    uint32_t before=execution_next_ticket;
    assert(int80_dispatch(&frame)==&current);
    assert(frame.rax==123 && current.frame.rax==123);
    assert(installed_fs==current.fs_base && io_releases==1 && !async_io);
    assert(!fpu_saved && !fpu_restored && !irq_state);
    if (execution_next_ticket-before!=1)
        fputs("FAIL: completed syscall reacquired the execution transaction on user return\n",stderr);
    assert(execution_next_ticket-before==1);
}
static void test_signal_input_and_retry(void)
{
    struct trap_frame frame=prepare(LINUX_SYS_GETPID);
    current.pending_signals=1ULL<<5;
    assert(int80_dispatch(&frame)==&current);
    assert(delivered==1 && fpu_saved==1 && current.frame.rax==0x51a7);
    frame=prepare(LINUX_SYS_GETPID);inject_input=true;
    assert(int80_dispatch(&frame)==&current);
    assert(drained==1 && !atomic_load(&key_pending));
    frame=prepare(LINUX_SYS_GETPID);backend_result=-RELIEFOS_EAGAIN;
    selected=&replacement;uint64_t rip=frame.rip;
    assert(int80_dispatch(&frame)==&replacement);
    assert(frame.rax==LINUX_SYS_GETPID && frame.rip==rip-2 && slept==1);
    assert(current.restart_syscall==LINUX_SYS_GETPID+1 && !io_releases && !async_io);
}
static void test_restored_context_and_fallback(void)
{
    struct trap_frame frame=prepare(LINUX_SYS_RT_SIGRETURN);
    assert(int80_dispatch(&frame)==&current);
    assert(frame.rax==0x5151 && current.frame.rax==0x5151);
    frame=prepare(LINUX_SYS_EXECVE);backend_result=0;
    current.frame.rip+=0x100;current.frame.error=1;current.frame.rax=0x6161;
    assert(int80_dispatch(&frame)==&current);
    assert(frame.rip==current.frame.rip && !frame.error && frame.rax==0x6161);
    frame=prepare(LINUX_SYS_GETPID);current.state=TASK_EXITED;selected=&replacement;
    assert(int80_dispatch(&frame)==&replacement);
    assert(released==1 && fpu_restored==1);
    frame=prepare(LINUX_SYS_GETPID);frame.cs=8;
    assert(!int80_dispatch(&frame));
}
int main(void)
{
    test_one_transaction_for_completed_syscall();
    test_signal_input_and_retry();
    test_restored_context_and_fallback();
    puts("PASS syscall user return: one transaction, result/TLS, signal, deferred input, retry, exec/sigreturn and retirement");
}
