/* The user #GP path must enqueue a forced SIGSEGV and schedule from the full
 * interrupted frame; a kernel #GP must retain the fatal exception path. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <reliefnt/bugcheck.h>
#include <reliefnt/console.h>
#include <reliefnt/lock.h>
#include <reliefnt/sched.h>
#include <reliefnt/time.h>
#include <reliefnt/userland.h>

#include "../../kernel/reliefnt/arch/x86_64/interrupt/idt.c"
#include "../../kernel/reliefnt/kernel/signal/signal.c"
#include "../../kernel/reliefnt/kernel/signal/signal_queue.c"

static struct task current_task;
static struct task next_task;
static struct trap_frame *scheduled_frame;
static unsigned schedule_calls;
static unsigned cr2_reads;
static unsigned bugcheck_calls;
static jmp_buf bugcheck_return;
static struct {
    uint64_t vector, error, rip, cs, rflags, rsp, ss, cr2;
} fatal_frame;

struct task *sched_current_task(void) { return &current_task; }
struct task *sched_find(uint32_t pid)
{
    if (pid == current_task.pid) return &current_task;
    if (pid == next_task.pid) return &next_task;
    return NULL;
}
void sched_alarm_rearm(struct task *task) { (void)task; }

struct task *userland_schedule_from_frame(struct trap_frame *frame)
{
    ++schedule_calls;
    scheduled_frame = frame;
    return &next_task;
}

uint64_t x86_64_read_cr2(void)
{
    ++cr2_reads;
    return 0xfeedfacecafebeefULL;
}

void console_log(enum console_log_level level, const char *format, ...)
{
    (void)level;
    (void)format;
}

__attribute__((noreturn)) void bugcheck_exception(uint64_t vector, uint64_t error,
                                                   uint64_t rip, uint64_t cs,
                                                   uint64_t rflags, uint64_t rsp,
                                                   uint64_t ss, uint64_t cr2)
{
    ++bugcheck_calls;
    fatal_frame.vector = vector;
    fatal_frame.error = error;
    fatal_frame.rip = rip;
    fatal_frame.cs = cs;
    fatal_frame.rflags = rflags;
    fatal_frame.rsp = rsp;
    fatal_frame.ss = ss;
    fatal_frame.cr2 = cr2;
    longjmp(bugcheck_return, 1);
}

__attribute__((noreturn)) void bugcheck_trap(const char *reason,
                                              const struct trap_frame *frame,
                                              uint64_t cr2)
{
    (void)reason;
    (void)frame;
    (void)cr2;
    abort();
}

void kernel_execution_lock_irqsave(uint64_t *flags) { *flags = 0; }
void kernel_execution_unlock_irqrestore(uint64_t flags) { (void)flags; }
void *kernel_malloc(size_t size) { return malloc(size); }
void kernel_free(void *pointer) { free(pointer); }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{
    assert(lock->state == 0);
    lock->state = 1;
    *flags = 0;
}
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{
    (void)flags;
    assert(lock->state == 1);
    lock->state = 0;
}
uint64_t time_ticks(void) { return 0; }

static void test_user_general_protection_is_contained(void)
{
    current_task = (struct task){
        .kind = TASK_KIND_USER,
        .pid = 164,
        .uid = 1000,
        .state = TASK_RUNNING,
        .blocked_signals = 1ULL << (LINUX_SIGSEGV - 1),
        .ignored_signals = 1ULL << (LINUX_SIGSEGV - 1),
        .limits.sigpending.rlim_cur = 8,
    };
    current_task.signal_actions[LINUX_SIGSEGV].handler = 1;
    next_task = (struct task){.kind = TASK_KIND_USER, .pid = 165};
    struct trap_frame frame = {
        .r15 = 0x1515151515151515ULL, .r14 = 0x1414141414141414ULL,
        .r13 = 0x1313131313131313ULL, .r12 = 0x1212121212121212ULL,
        .r11 = 0x1111111111111111ULL, .r10 = 0x1010101010101010ULL,
        .r9 = 0x0909090909090909ULL, .r8 = 0x0808080808080808ULL,
        .rbp = 0x0b0b0b0b0b0b0b0bULL, .rdi = 0x0d0d0d0d0d0d0d0dULL,
        .rsi = 0x0606060606060606ULL, .rdx = 0x0202020202020202ULL,
        .rcx = 0x0c0c0c0c0c0c0c0cULL, .rbx = 0x0b0b0b0b0b0b0b0bULL,
        .rax = 0x0a0a0a0a0a0a0a0aULL, .vector = 13, .error = 0,
        .rip = 0x400c508, .cs = 0x23, .rflags = 0x246,
        .rsp = 0x7fffffffd000, .ss = 0x1b,
    };
    const struct trap_frame original = frame;

    struct task *selected = general_protection_dispatch(&frame);
    assert(selected == &next_task);
    assert(!bugcheck_calls);
    assert(schedule_calls == 1 && scheduled_frame == &frame);
    assert(!memcmp(&frame, &original, sizeof(frame)));
    assert(current_task.signal_actions[LINUX_SIGSEGV].handler == 0);
    assert(!(current_task.blocked_signals & (1ULL << (LINUX_SIGSEGV - 1))));
    assert(!(current_task.ignored_signals & (1ULL << (LINUX_SIGSEGV - 1))));
    assert(current_task.pending_signals & (1ULL << (LINUX_SIGSEGV - 1)));
    assert(cr2_reads == 0);

    struct linux_siginfo info = {0};
    assert(kernel_signal_dequeue(&current_task,
                                 1ULL << (LINUX_SIGSEGV - 1), &info) == LINUX_SIGSEGV);
    assert(info.signo == LINUX_SIGSEGV && info.code == LINUX_SI_KERNEL);
    assert(info.fields.fault.address == 0);
    assert(!current_task.pending_signals);
}

static void test_user_general_protection_preserves_pending_sigsegv(void)
{
    const uint64_t sigsegv_bit = 1ULL << (LINUX_SIGSEGV - 1);
    current_task = (struct task){
        .kind = TASK_KIND_USER,
        .pid = 164,
        .uid = 1000,
        .state = TASK_RUNNING,
        .limits.sigpending.rlim_cur = 8,
    };
    current_task.signal_actions[LINUX_SIGSEGV].handler = 0x410000;
    next_task = (struct task){.kind = TASK_KIND_USER, .pid = 165};

    /* A private standard signal already pending keeps its first siginfo. */
    const struct linux_siginfo first_info = {
        .signo = LINUX_SIGSEGV,
        .code = LINUX_SI_USER,
        .fields.sender = {.pid = 2718, .uid = 1100},
    };
    assert(kernel_signal_enqueue(&current_task, false, LINUX_SIGSEGV,
                                 &first_info) == 0);
    assert(current_task.pending_signals == sigsegv_bit);
    assert(*sched_task_process_pending(&current_task) == 0);

    struct trap_frame frame = {
        .r15 = 0x1515151515151515ULL, .r14 = 0x1414141414141414ULL,
        .r13 = 0x1313131313131313ULL, .r12 = 0x1212121212121212ULL,
        .r11 = 0x1111111111111111ULL, .r10 = 0x1010101010101010ULL,
        .r9 = 0x0909090909090909ULL, .r8 = 0x0808080808080808ULL,
        .rbp = 0x0b0b0b0b0b0b0b0bULL, .rdi = 0x0d0d0d0d0d0d0d0dULL,
        .rsi = 0x0606060606060606ULL, .rdx = 0x0202020202020202ULL,
        .rcx = 0x0c0c0c0c0c0c0c0cULL, .rbx = 0x0b0b0b0b0b0b0b0bULL,
        .rax = 0x0a0a0a0a0a0a0a0aULL, .vector = 13, .error = 0,
        .rip = 0x400c508, .cs = 0x23, .rflags = 0x246,
        .rsp = 0x7fffffffd000, .ss = 0x1b,
    };
    const struct trap_frame original_frame = frame;
    unsigned schedule_calls_before = schedule_calls;
    unsigned cr2_reads_before = cr2_reads;

    struct task *selected = general_protection_dispatch(&frame);
    assert(selected == &next_task && !bugcheck_calls);
    assert(schedule_calls == schedule_calls_before + 1);
    assert(scheduled_frame == &frame);
    assert(!memcmp(&frame, &original_frame, sizeof(frame)));
    assert(current_task.pending_signals == sigsegv_bit);
    assert(cr2_reads == cr2_reads_before);

    struct linux_siginfo received_info = {0};
    assert(kernel_signal_dequeue(&current_task, sigsegv_bit,
                                 &received_info) == LINUX_SIGSEGV);
    assert(!memcmp(&received_info, &first_info, sizeof(first_info)));
    assert(!current_task.pending_signals);
}

static void test_kernel_general_protection_remains_fatal(void)
{
    struct trap_frame frame = {
        .vector = 13, .error = 0x1234, .rip = 0xffffffff80001234ULL,
        .cs = 0x08, .rflags = 0x202,
    };
    unsigned calls_before = schedule_calls;
    unsigned reads_before = cr2_reads;
    if (setjmp(bugcheck_return) == 0) {
        general_protection_dispatch(&frame);
        assert(!"kernel #GP unexpectedly returned");
    }
    assert(bugcheck_calls == 1);
    assert(fatal_frame.vector == 13 && fatal_frame.error == frame.error);
    assert(fatal_frame.rip == frame.rip && fatal_frame.cs == frame.cs);
    assert(fatal_frame.rflags == frame.rflags);
    assert(fatal_frame.cr2 == 0xfeedfacecafebeefULL);
    assert(cr2_reads == reads_before + 1 && schedule_calls == calls_before);
    assert(!(current_task.pending_signals & (1ULL << (LINUX_SIGSEGV - 1))));
}

int main(void)
{
    test_user_general_protection_is_contained();
    test_user_general_protection_preserves_pending_sigsegv();
    test_kernel_general_protection_remains_fatal();
    puts("PASS user #GP: empty-queue SI_KERNEL, first-record coalescing, full frame and kernel fatal policy");
    return 0;
}
