#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <reliefnt/paging.h>
/* Model the kernel direct map with host pointers to the destination pages. */
#undef RELIEFNT_KERNEL_DIRECT_MAP_BASE
#define RELIEFNT_KERNEL_DIRECT_MAP_BASE 0
#include "../../kernel/reliefnt/kernel/reliefnt/signal.c"
#include "../../kernel/reliefnt/kernel/reliefnt/signal_queue.c"
#include "../../kernel/reliefnt/kernel/exec/usercopy_task.c"
#include "../../kernel/reliefnt/kernel/reliefnt/syscall_socket_batch.c"

#define STACK_ADDRESS 0x0f400000ULL
static unsigned char target_pages[8192] __attribute__((aligned(4096)));
static struct task target;
static unsigned translated_pages;
static unsigned exits;
static int deferred_error;
static uint64_t lazy_page;
static unsigned demand_faults;
static unsigned drained_io;
static uint64_t clock_ticks = 100;
static unsigned fpu_restore_calls;
static uint8_t restored_fpu_state[512];
void power_reboot(void) { abort(); }
void power_shutdown(void) { abort(); }
void storage_drain_task_io(uint32_t pid) { assert(pid==target.pid); ++drained_io; }
void kernel_execution_lock_irqsave(uint64_t *flags) { *flags = 0; }
void kernel_execution_unlock_irqrestore(uint64_t flags) { (void)flags; }
void *kernel_malloc(size_t size) { return malloc(size); }
void kernel_free(void *p) { free(p); }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ assert(!lock->state); lock->state = 1; *flags = 0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)flags; assert(lock->state); lock->state = 0; }
uint64_t time_ticks(void) { return 100; }
void arch_fpu_restore(const void *state)
{
    memcpy(restored_fpu_state, state, sizeof(restored_fpu_state));
    ++fpu_restore_calls;
}
void kernel_wait_queue_remove(struct kernel_wait_queue *queue, struct task *task)
{ (void)queue; (void)task; assert(0); }
void futex_cancel_wait(struct task *task) { (void)task; assert(0); }
uint64_t task_socket_cancel_receive(struct task *task)
{ uint64_t n = task->socket_receive_done; task->socket_receive_done = 0; return n; }
int task_socket_message_error(struct task_file *file, int error, bool setting)
{ (void)file; assert(setting); deferred_error = error; return 0; }
int time_clock_get(int32_t id, struct linux_timespec *out)
{
    assert(id == LINUX_CLOCK_MONOTONIC);
    *out = (struct linux_timespec){clock_ticks / RELIEFNT_TICK_HZ,
        clock_ticks % RELIEFNT_TICK_HZ * (1000000000ULL / RELIEFNT_TICK_HZ)};
    return 0;
}
void task_release_syscall_file(struct task *task) { memset(&task->regular_io,0,sizeof(task->regular_io)); }
struct task *sched_find(uint32_t pid) { (void)pid; return &target; }
void sched_exit_group(uint32_t pid, uint64_t code)
{ assert(pid == 1 && code == 139); target.state = TASK_EXITED; ++exits; }
void sched_signal_job_control(uint32_t pid, int sig) { (void)pid; (void)sig; assert(0); }
void sched_alarm_rearm(struct task *task) { (void)task; assert(0); }
void sched_notify_runnable(void) { }
void sched_wake_interruptible(struct task *task)
{
    if (task && task->state == TASK_BLOCKED && !task->vfork_child) {
        task->wake_tick = 0;
        task->wait_window_id = 0;
        task->state = TASK_READY;
    }
}
bool drm_vmwgfx_task_uninterruptible(const struct task *task)
{ (void)task; return false; }
bool drm_vmwgfx_task_signal_busy(const struct task *task)
{ (void)task; return false; }
void sched_signal_discard(struct task *task, int sig)
{ task->pending_signals &= ~(1ULL << (sig - 1)); *sched_task_process_pending(task) &= ~(1ULL << (sig - 1)); }

bool address_space_user_page_writable(const struct address_space *as, uint64_t address)
{ assert(as == &target.as); return (address & ~4095ULL) != lazy_page && user_range_writable(address, 1); }
bool address_space_handle_cow_fault(struct address_space *as, uint64_t address)
{ (void)as; (void)address; return false; }
int syscall_handle_task_page_fault(struct task *task, uint64_t address, uint64_t error)
{
    assert(task == &target && error == 6);
    if (!lazy_page || (address & ~4095ULL) != lazy_page) return 0;
    lazy_page = 0;
    ++demand_faults;
    return 1;
}

bool user_range_writable(uint64_t ptr, uint64_t len)
{ return ptr >= STACK_ADDRESS && ptr + len <= STACK_ADDRESS + sizeof(target_pages); }
bool user_range_ok(uint64_t ptr, uint64_t len)
{ return ptr >= STACK_ADDRESS && ptr <= STACK_ADDRESS + sizeof(target_pages) &&
         len <= STACK_ADDRESS + sizeof(target_pages) - ptr; }
uint64_t address_space_user_page_phys(const struct address_space *as, uint64_t vaddr)
{
    assert(as == &target.as);
    assert(vaddr >= STACK_ADDRESS && vaddr < STACK_ADDRESS + sizeof(target_pages));
    translated_pages |= 1u << ((vaddr - STACK_ADDRESS) / 4096);
    return (uintptr_t)target_pages + ((vaddr - STACK_ADDRESS) & ~4095ULL);
}

int main(void)
{
    unsigned char *previous_stack = mmap((void *)STACK_ADDRESS, sizeof(target_pages),
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(previous_stack == (void *)STACK_ADDRESS);
    memset(previous_stack, 0xa5, sizeof(target_pages));
    memset(target_pages, 0x5a, sizeof(target_pages));
    target.kind = TASK_KIND_USER;
    target.pid = 1;
    target.state = TASK_READY;
    target.stack_low = STACK_ADDRESS;
    target.limits.sigpending.rlim_cur = 8;
    target.signal_actions[2] = (struct kernel_signal_action){
        .handler = 0x410000, .restorer = 0x420000, .flags = LINUX_SA_SIGINFO};
    struct task vfork_child = {0};
    target.vfork_child = &vfork_child;
    target.state = TASK_BLOCKED;
    struct trap_frame blocked_frame = {.rsp = STACK_ADDRESS + 4096 + 800,
        .rip = 0x430000, .cs = 0x23};
    assert(kernel_signal_queue_task(&target, 2) == 0);
    assert(kernel_signal_deliver_pending(&target, &blocked_frame) == 0);
    assert(blocked_frame.rip == 0x430000 && target.pending_signals == 2);
    kernel_signal_flush(&target, false, UINT64_MAX);
    target.vfork_child = NULL;
    target.state = TASK_READY;
    struct linux_siginfo sent = {.signo = 2, .code = LINUX_SI_QUEUE,
        .fields.realtime = {.pid = 123, .uid = 456, .value = 0x123456789abcdef0ULL}};
    assert(kernel_signal_queue_task_info(&target, 2, &sent) == 0);
    assert(target.pending_signals == (1u << (2 - 1)));
    struct trap_frame frame = {.rsp = STACK_ADDRESS + 4096 + 800,
                                .rip = 0x430000, .cs = 0x23};
    uint64_t saved_rsp = frame.rsp;
    lazy_page = STACK_ADDRESS + 4096;
    assert(kernel_signal_deliver_pending(&target, &frame) == 1);
    assert(!lazy_page && demand_faults == 1);
    struct linux_rt_sigframe saved;
    memcpy(&saved, target_pages + frame.rsp - STACK_ADDRESS, sizeof(saved));
    assert(saved.restorer == 0x420000 && saved.uc.context.rip == 0x430000);
    assert(!memcmp(&saved.info, &sent, sizeof(sent)));
    assert(frame.rsi == frame.rsp + offsetof(struct linux_rt_sigframe, info));
    assert(saved.uc.context.rsp == saved_rsp && frame.rip == 0x410000);
    assert(translated_pages == 3);
    assert((frame.rsp & 15) == 8 && (saved.uc.context.fpstate & 63) == 0);
    for (unsigned i = saved_rsp - STACK_ADDRESS - 128; i < saved_rsp - STACK_ADDRESS; ++i)
        assert(target_pages[i] == 0x5a);
    assert(!target.pending_signals && (target.blocked_signals & (1u << (2 - 1))));
    target.blocked_signals = 0;
    target.restart_syscall = __NR_nanosleep + 1;
    target.nanosleep_clock = LINUX_CLOCK_MONOTONIC;
    clock_ticks = 140; /* Disciplined clock is ahead of the raw 100 ticks. */
    target.nanosleep_deadline = 160;
    target.nanosleep_remaining = STACK_ADDRESS + 64;
    target.signal_actions[2].flags = LINUX_SA_RESTART;
    frame = (struct trap_frame){.rsp = saved_rsp, .rip = 0x430000, .cs = 0x23};
    assert(kernel_signal_queue_task(&target, 2) == 0);
    assert(kernel_signal_deliver_pending(&target, &frame) == 1);
    memcpy(&saved, target_pages + frame.rsp - STACK_ADDRESS, sizeof(saved));
    assert(saved.uc.context.rip == 0x430002 && (int64_t)saved.uc.context.rax == -4);
    struct linux_timespec remaining;
    memcpy(&remaining, target_pages + 64, sizeof(remaining));
    assert(remaining.tv_sec == 0 && remaining.tv_nsec == 200000000);
    assert(!target.nanosleep_deadline && !target.nanosleep_remaining);
    clock_ticks = 100;
    target.blocked_signals = 0;
    target.sigsuspend_saved_mask = 1ULL << 1;
    target.sigsuspend_active = 1;
    target.restart_syscall = __NR_pselect6 + 1;
    target.poll_deadline_ticks = 200;
    frame = (struct trap_frame){.rsp = saved_rsp, .rip = 0x430000, .cs = 0x23};
    assert(kernel_signal_queue_task(&target, 2) == 0);
    assert(kernel_signal_deliver_pending(&target, &frame) == 1);
    memcpy(&saved, target_pages + frame.rsp - STACK_ADDRESS, sizeof(saved));
    assert(saved.uc.context.rip == 0x430002 && (int64_t)saved.uc.context.rax == -LINUX_EINTR);
    assert(saved.uc.mask == (1ULL << 1));
    assert(!target.sigsuspend_active && !target.restart_syscall && !target.poll_deadline_ticks);
    target.blocked_signals = 0;
    target.restart_syscall = __NR_ppoll + 1;
    target.poll_deadline_ticks = 200;
    target.signal_actions[2].flags = LINUX_SA_RESTART;
    frame = (struct trap_frame){.rsp = saved_rsp, .rip = 0x430000, .cs = 0x23};
    assert(kernel_signal_queue_task(&target, 2) == 0);
    assert(kernel_signal_deliver_pending(&target, &frame) == 1);
    memcpy(&saved, target_pages + frame.rsp - STACK_ADDRESS, sizeof(saved));
    assert(saved.uc.context.rip == 0x430002 &&
           (int64_t)saved.uc.context.rax == -LINUX_EINTR);
    assert(!target.restart_syscall && !target.poll_deadline_ticks);
    const uint64_t epoll_calls[] = {__NR_epoll_wait, __NR_epoll_pwait, __NR_epoll_pwait2};
    for (unsigned i = 0; i < sizeof(epoll_calls) / sizeof(epoll_calls[0]); ++i) {
        target.blocked_signals = 0;
        target.restart_syscall = epoll_calls[i] + 1;
        target.poll_deadline_ticks = 200;
        target.signal_actions[2].flags = LINUX_SA_RESTART;
        frame = (struct trap_frame){.rsp = saved_rsp, .rip = 0x430000, .cs = 0x23};
        assert(kernel_signal_queue_task(&target, 2) == 0);
        assert(kernel_signal_deliver_pending(&target, &frame) == 1);
        memcpy(&saved, target_pages + frame.rsp - STACK_ADDRESS, sizeof(saved));
        assert(saved.uc.context.rip == 0x430002 &&
               (int64_t)saved.uc.context.rax == -LINUX_EINTR);
        assert(!target.restart_syscall && !target.poll_deadline_ticks);
    }
    for (unsigned restart=0;restart<2;++restart) {
        target.blocked_signals=0;
        target.restart_syscall=__NR_pread64+1;
        target.regular_io.active=true;
        target.regular_io.done=65536;
        target.signal_actions[2].flags=restart ? LINUX_SA_RESTART : 0;
        assert(kernel_signal_queue_task(&target,2)==0);
        frame=(struct trap_frame){.rsp=saved_rsp,.rip=0x430000,.cs=0x23};
        assert(kernel_signal_deliver_pending(&target,&frame)==1);
        memcpy(&saved,target_pages+frame.rsp-STACK_ADDRESS,sizeof(saved));
        assert(saved.uc.context.rax==65536 && saved.uc.context.rip==0x430002);
        assert(drained_io==restart+1 && !target.regular_io.active);
    }
    uint64_t process_pending = 1ULL << 1;
    target.shared_process_pending = &process_pending;
    target.blocked_signals = 0;
    frame = (struct trap_frame){.rsp = saved_rsp, .rip = 0x430000, .cs = 0x23};
    assert(kernel_signal_deliver_pending(&target, &frame) == 1);
    assert(!process_pending && !target.pending_signals);
    process_pending = 1ULL << 1;
    target.pending_signals = 1ULL << 9;
    target.signal_actions[10] = target.signal_actions[2];
    target.blocked_signals = 0;
    frame = (struct trap_frame){.rsp = saved_rsp, .rip = 0x430000, .cs = 0x23};
    assert(kernel_signal_deliver_pending(&target, &frame) == 1 && frame.rdi == 10);
    assert(process_pending == (1ULL << 1) && !target.pending_signals);
    target.blocked_signals = 0;
    frame = (struct trap_frame){.rsp = saved_rsp, .rip = 0x430000, .cs = 0x23};
    assert(kernel_signal_deliver_pending(&target, &frame) == 1 && frame.rdi == 2);
    assert(!process_pending);
    target.blocked_signals = 0;
    target.sigwait_mask = 1ULL << 34;
    target.sigwait_active = 1;
    target.sigwait_deadline = 123;
    target.restart_syscall = __NR_rt_sigtimedwait + 1;
    assert(kernel_signal_queue_task(&target, 2) == 0);
    frame = (struct trap_frame){.rsp = saved_rsp, .rip = 0x430000, .cs = 0x23};
    assert(kernel_signal_deliver_pending(&target, &frame) == 1);
    memcpy(&saved, target_pages + frame.rsp - STACK_ADDRESS, sizeof(saved));
    assert((int64_t)saved.uc.context.rax == -4 && saved.uc.context.rip == 0x430002);
    assert(!target.sigwait_active && !target.sigwait_mask && !target.sigwait_deadline);
    /* A parked signalfd read resumes dequeue before handling an unblocked signal. */
    struct task_file signalfd_file = {.kind = TASK_FILE_KIND_SIGNALFD, .aux = 1ULL << 1};
    target.blocked_signals = 0;
    target.signalfd_waiting = true;
    target.syscall_file = &signalfd_file;
    target.restart_syscall = __NR_readv + 1;
    assert(kernel_signal_queue_task(&target, 2) == 0);
    frame = (struct trap_frame){.rsp = saved_rsp, .rip = 0x430000, .cs = 0x23};
    assert(kernel_signal_deliver_pending(&target, &frame) == 0);
    assert(frame.rip == 0x430000 && target.pending_signals == (1ULL << 1));
    struct linux_siginfo consumed;
    assert(kernel_signal_dequeue(&target, signalfd_file.aux, &consumed) == 2);
    target.signalfd_waiting = false;
    target.syscall_file = NULL;
    const uint64_t vector_calls[] = {__NR_readv, __NR_preadv, __NR_preadv2,
                                    __NR_read, __NR_write, __NR_ioctl};
    for (unsigned i = 0; i < sizeof(vector_calls) / sizeof(vector_calls[0]); ++i) {
        for (unsigned restart = 0; restart < 2; ++restart) {
            target.blocked_signals = 0;
            target.restart_syscall = vector_calls[i] + 1;
            target.signal_actions[2].flags = restart ? LINUX_SA_RESTART : 0;
            assert(kernel_signal_queue_task(&target, 2) == 0);
            frame = (struct trap_frame){.rsp = saved_rsp, .rip = 0x430000,
                                       .rax = vector_calls[i], .cs = 0x23};
            assert(kernel_signal_deliver_pending(&target, &frame) == 1);
            memcpy(&saved, target_pages + frame.rsp - STACK_ADDRESS, sizeof(saved));
            assert(saved.uc.context.rip == 0x430000 + (restart ? 0 : 2));
            assert(saved.uc.context.rax == (restart ? vector_calls[i] : (uint64_t)-4LL));
        }
    }
    const uint64_t sysv_calls[] = {__NR_msgsnd, __NR_msgrcv};
    for (unsigned i = 0; i < 2; ++i) {
        target.blocked_signals = 0;
        target.restart_syscall = sysv_calls[i] + 1;
        target.signal_actions[2].flags = LINUX_SA_RESTART;
        assert(kernel_signal_queue_task(&target, 2) == 0);
        frame = (struct trap_frame){.rsp = saved_rsp, .rip = 0x430000,
                                   .rax = sysv_calls[i], .cs = 0x23};
        target.sysv_msg.completed = true;
        assert(kernel_signal_deliver_pending(&target, &frame) == 0);
        assert(target.pending_signals == (1ULL << 1) && frame.rip == 0x430000);
        target.sysv_msg.completed = false;
        assert(kernel_signal_deliver_pending(&target, &frame) == 1);
        memcpy(&saved, target_pages + frame.rsp - STACK_ADDRESS, sizeof(saved));
        assert(saved.uc.context.rip == 0x430002 && (int64_t)saved.uc.context.rax == -LINUX_EINTR);
    }
    target.blocked_signals = 0;
    target.restart_syscall = __NR_recvmmsg + 1;
    const uint64_t semaphore_calls[] = {__NR_semop, __NR_semtimedop};
    for (unsigned i = 0; i < 2; ++i) {
        target.blocked_signals = 0;
        target.restart_syscall = semaphore_calls[i] + 1;
        target.signal_actions[2].flags = LINUX_SA_RESTART;
        assert(kernel_signal_queue_task(&target, 2) == 0);
        frame = (struct trap_frame){.rsp = saved_rsp, .rip = 0x430000,
                                   .rax = semaphore_calls[i], .cs = 0x23};
        target.sysv_sem.array = (struct sysv_sem_array *)1;
        target.sysv_sem.completed = true;
        assert(kernel_signal_deliver_pending(&target, &frame) == 0);
        target.sysv_sem.completed = false;
        target.sysv_sem.timed = true;
        target.sysv_sem.deadline = 1000000000;
        assert(kernel_signal_deliver_pending(&target, &frame) == 0);
        target.sysv_sem.deadline++;
        assert(kernel_signal_deliver_pending(&target, &frame) == 1);
        memcpy(&saved, target_pages + frame.rsp - STACK_ADDRESS, sizeof(saved));
        assert(saved.uc.context.rip == 0x430002 && (int64_t)saved.uc.context.rax == -LINUX_EINTR);
        target.sysv_sem = (struct task_sysv_sem_state){0};
    }
    target.blocked_signals = 0;
    target.restart_syscall = __NR_recvmmsg + 1;
    target.mmsg = (struct task_mmsg_state){.active = true, .receiving = true,
        .vector = STACK_ADDRESS + 128, .length = 3, .count = 1,
        .timeout_pointer = STACK_ADDRESS + 64, .deadline = {2, 0}};
    target.socket_receive_done = 4;
    target.socket_receive_flags = MSG_CMSG_CLOEXEC;
    assert(kernel_signal_queue_task(&target, 2) == 0);
    frame = (struct trap_frame){.rsp = saved_rsp, .rip = 0x430000, .cs = 0x23};
    assert(kernel_signal_deliver_pending(&target, &frame) == 1);
    memcpy(&saved, target_pages + frame.rsp - STACK_ADDRESS, sizeof(saved));
    assert(saved.uc.context.rax == 2 && saved.uc.context.rip == 0x430002);
    struct mmsghdr second;
    memcpy(&second, target_pages + 128 + sizeof(second), sizeof(second));
    assert(second.msg_len == 4 && second.msg_hdr.msg_flags == MSG_CMSG_CLOEXEC && !second.msg_hdr.msg_controllen);
    memcpy(&remaining, target_pages + 64, sizeof(remaining));
    assert(remaining.tv_sec == 1 && !remaining.tv_nsec && deferred_error == 512);
    assert(!target.mmsg.active && !target.socket_receive_done);
    target.blocked_signals = 1ULL << 10;
    assert(kernel_signal_queue_task(&target, 2) == 0);
    frame = (struct trap_frame){.rsp = 0, .rip = 0x430000, .cs = 0x23};
    assert(kernel_signal_deliver_pending(&target, &frame) == 0);
    assert(exits == 1 && target.exit_signal == 11 && !signal_accounts);
    target.state = TASK_READY;
    target.blocked_signals = 0;
    target.signal_stack_base = STACK_ADDRESS;
    target.signal_stack_size = sizeof(target_pages);
    target.signal_actions[11] = (struct kernel_signal_action){
        .handler = 0x410000, .restorer = 0x420000,
        .flags = LINUX_SA_ONSTACK | LINUX_SA_SIGINFO};
    frame = (struct trap_frame){.rsp = 4096, .rip = 0x430000, .cs = 0x23,
        .vector = 14, .error = 6};
    kernel_signal_force_fault(&target, 11, LINUX_SEGV_MAPERR, 0x1234);
    assert(kernel_signal_deliver_pending(&target, &frame) == 1);
    memcpy(&saved, target_pages + frame.rsp - STACK_ADDRESS, sizeof(saved));
    assert(saved.info.code == LINUX_SEGV_MAPERR && saved.info.fields.fault.address == 0x1234);
    assert(saved.uc.context.cr2 == 0x1234 && saved.uc.context.vector == 14 && saved.uc.context.error == 6);

    /* A user #GP must carry SI_KERNEL with a zero address, preserve the full
     * register frame, and round-trip a handler's edited RIP plus FPU image. */
    target.signal_stack_base = 0;
    target.signal_stack_size = 0;
    target.signal_stack_flags = 0;
    target.blocked_signals = 0;
    target.ignored_signals = 0;
    memset(target.fpu_state, 0x3c, sizeof(target.fpu_state));
    uint32_t mxcsr = 0x1f80;
    memcpy(target.fpu_state + 24, &mxcsr, sizeof(mxcsr));
    frame = (struct trap_frame){
        .r15 = 0x1515151515151515ULL, .r14 = 0x1414141414141414ULL,
        .r13 = 0x1313131313131313ULL, .r12 = 0x1212121212121212ULL,
        .r11 = 0x1111111111111111ULL, .r10 = 0x1010101010101010ULL,
        .r9 = 0x0909090909090909ULL, .r8 = 0x0808080808080808ULL,
        .rbp = 0x0b0b0b0b0b0b0b0bULL, .rdi = 0x0d0d0d0d0d0d0d0dULL,
        .rsi = 0x0606060606060606ULL, .rdx = 0x0202020202020202ULL,
        .rcx = 0x0c0c0c0c0c0c0c0cULL, .rbx = 0x0b0b0b0b0b0b0b0bULL,
        .rax = 0x0a0a0a0a0a0a0a0aULL, .rip = 0x430000,
        .cs = 0x23, .rflags = 0x3202,
        .rsp = STACK_ADDRESS + 4096 + 800, .ss = 0x1b,
        .vector = 13, .error = 0,
    };
    const struct trap_frame fault_frame = frame;
    const uint64_t interrupted_rsp = frame.rsp;
    kernel_signal_force_fault(&target, LINUX_SIGSEGV, LINUX_SI_KERNEL, 0);
    assert(kernel_signal_deliver_pending(&target, &frame) == 1);
    memcpy(&saved, target_pages + frame.rsp - STACK_ADDRESS, sizeof(saved));
    assert(saved.info.signo == LINUX_SIGSEGV && saved.info.code == LINUX_SI_KERNEL);
    assert(saved.info.fields.fault.address == 0 && saved.uc.context.cr2 == 0);
    assert(saved.uc.context.vector == 13 && saved.uc.context.error == 0);
    assert(saved.uc.context.rip == 0x430000 && saved.uc.context.rsp == interrupted_rsp);
    assert(saved.uc.context.r15 == fault_frame.r15 && saved.uc.context.r14 == fault_frame.r14);
    assert(saved.uc.context.r13 == fault_frame.r13 && saved.uc.context.r12 == fault_frame.r12);
    assert(saved.uc.context.r11 == fault_frame.r11 && saved.uc.context.r10 == fault_frame.r10);
    assert(saved.uc.context.r9 == fault_frame.r9 && saved.uc.context.r8 == fault_frame.r8);
    assert(saved.uc.context.rbp == fault_frame.rbp && saved.uc.context.rdi == fault_frame.rdi);
    assert(saved.uc.context.rsi == fault_frame.rsi && saved.uc.context.rdx == fault_frame.rdx);
    assert(saved.uc.context.rcx == fault_frame.rcx && saved.uc.context.rbx == fault_frame.rbx);
    assert(saved.uc.context.rax == fault_frame.rax && saved.uc.context.rflags == fault_frame.rflags);
    uint8_t *saved_fpstate = target_pages + (saved.uc.context.fpstate - STACK_ADDRESS);
    assert(saved_fpstate[160] == 0x3c);
    uint8_t previous_stack_backup[sizeof(target_pages)];
    memcpy(previous_stack_backup, previous_stack, sizeof(previous_stack_backup));
    memcpy(previous_stack, target_pages, sizeof(target_pages));
    struct linux_rt_sigframe *user_frame = (struct linux_rt_sigframe *)(uintptr_t)frame.rsp;
    user_frame->uc.context.rip = 0x440100;
    user_frame->uc.context.r12 = 0xfeed123456789abcULL;
    user_frame->uc.context.rax = 0x1234abcd5678ef90ULL;
    user_frame->uc.context.rflags = 0x3202;
    uint8_t *user_fpstate = (uint8_t *)(uintptr_t)user_frame->uc.context.fpstate;
    for (unsigned i = 0; i < 16; ++i) user_fpstate[160 + i] = (uint8_t)(0x80 + i);
    memcpy(user_fpstate + 24, &mxcsr, sizeof(mxcsr));
    frame.rsp += 8; /* The signal trampoline's ret popped the restorer. */
    assert(kernel_signal_rt_sigreturn(&target, &frame) == 0);
    assert(frame.rip == 0x440100 && frame.rsp == interrupted_rsp);
    assert(frame.r12 == 0xfeed123456789abcULL && frame.rax == 0x1234abcd5678ef90ULL);
    assert(frame.vector == 13 && frame.error == 0 && frame.cs == 0x23 && frame.ss == 0x1b);
    assert((frame.rflags & 0x202) == 0x202 && !(frame.rflags & 0x3000));
    assert(fpu_restore_calls == 1 && restored_fpu_state[160] == 0x80 &&
           restored_fpu_state[175] == 0x8f);
    memcpy(&mxcsr, restored_fpu_state + 24, sizeof(mxcsr));
    assert(mxcsr == 0x1f80);
    memcpy(previous_stack, previous_stack_backup, sizeof(previous_stack_backup));

    /* A synchronous blocked fault must become fatal, not run a blocked handler. */
    target.blocked_signals |= 1ULL << (LINUX_SIGSEGV - 1);
    kernel_signal_force_fault(&target, 11, LINUX_SEGV_ACCERR, 0x2345);
    assert(!target.signal_actions[11].handler && !(target.blocked_signals & (1ULL << 10)));
    assert(kernel_signal_deliver_pending(&target, &frame) == 0);
    assert(exits == 2 && target.exit_signal == 11 && !signal_accounts);
    for (unsigned i = 0; i < sizeof(target_pages); ++i) assert(previous_stack[i] == 0xa5);
    munmap(previous_stack, sizeof(target_pages));
    puts("Signal delivery writes both target stack pages without touching the previous address space");
    return 0;
}
