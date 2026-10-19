#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#if !defined(__x86_64__)
#error "user_gp_runtime_probe requires x86_64"
#endif

#define CASE_TIMEOUT_MS 3000
#define HEARTBEAT_TIMEOUT_MS 700
#define CHILD_ALARM_SECONDS 5
#define EFLAGS_CHECK_MASK UINT64_C(0x0dd5)
#define EXPECTED_MXCSR UINT32_C(0x1f80)

#define EXPECT_R8  UINT64_C(0x8102030405060708)
#define EXPECT_R9  UINT64_C(0x9203040506070809)
#define EXPECT_R10 UINT64_C(0xa304050607080a0b)
#define EXPECT_R11 UINT64_C(0xb405060708090c0d)
#define EXPECT_R12 UINT64_C(0xc5060708090a0e0f)
#define EXPECT_R13 UINT64_C(0xd60708090a0b1011)
#define EXPECT_R14 UINT64_C(0xe708090a0b0c1213)
#define EXPECT_R15 UINT64_C(0xf8090a0b0c0d1415)
#define EXPECT_RDI UINT64_C(0x190a0b0c0d0e1617)
#define EXPECT_RSI UINT64_C(0x2a0b0c0d0e0f1819)
#define EXPECT_RBP UINT64_C(0x3b0c0d0e0f101a1b)
#define EXPECT_RBX UINT64_C(0x4c0d0e0f10111c1d)
#define EXPECT_RDX UINT64_C(0x5d0e0f1012131e1f)
#define EXPECT_RAX UINT64_C(0x6e0f101112142021)
#define EXPECT_RCX UINT64_C(0x7f10111213152223)

static volatile uintptr_t gp_fault_rip;
static volatile uintptr_t gp_resume_rip;
static volatile uintptr_t gp_expected_rsp;
static volatile uint64_t gp_expected_eflags;
static volatile uint64_t gp_after_eflags;
static volatile uint64_t gp_expected_cs;
static volatile uint64_t gp_expected_gs;
static volatile uint64_t gp_expected_fs;
static volatile uint64_t gp_after_cs;
static volatile uint64_t gp_after_gs;
static volatile uint64_t gp_after_fs;
static volatile uint64_t gp_after_rax;
static volatile uint64_t gp_after_rbx;
static volatile uint64_t gp_after_rcx;
static volatile uint64_t gp_after_rdx;
static volatile uint64_t gp_after_rsi;
static volatile uint64_t gp_after_rdi;
static volatile uint64_t gp_after_rbp;
static volatile uint64_t gp_after_r8;
static volatile uint64_t gp_after_r9;
static volatile uint64_t gp_after_r10;
static volatile uint64_t gp_after_r11;
static volatile uint64_t gp_after_r12;
static volatile uint64_t gp_after_r13;
static volatile uint64_t gp_after_r14;
static volatile uint64_t gp_after_r15;
static volatile uint64_t gp_after_rsp;
static volatile uint32_t gp_expected_mxcsr = EXPECTED_MXCSR;
static volatile uint32_t gp_after_mxcsr;
static volatile uint32_t gp_expected_xmm0[4] = {
    UINT32_C(0x10203040), UINT32_C(0x55667788),
    UINT32_C(0x90abcdef), UINT32_C(0xfedcba98)
};
static volatile uint32_t gp_after_xmm0[4];
static volatile sig_atomic_t gp_handler_seen;
static volatile sig_atomic_t gp_handler_ok;

static int heartbeat_read_fd = -1;
static pid_t heartbeat_pid = -1;
static unsigned heartbeat_bytes;

/**
 * @brief Read the monotonic clock as milliseconds.
 * @param milliseconds Receives the current monotonic time.
 * @return 0 on success, or -1 if the clock read failed.
 */
static int monotonic_milliseconds(int64_t *milliseconds)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -1;
    *milliseconds = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    return 0;
}

/**
 * @brief Sleep briefly while allowing signal interruptions.
 * @return No value.
 */
static void sleep_briefly(void)
{
    const struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000 };
    struct timespec remaining = delay;

    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR)
        ;
}

/**
 * @brief Write heartbeat bytes until the parent terminates this child.
 * @param fd Pipe descriptor kept by the heartbeat child.
 * @return No value; this routine exits the child if the pipe becomes unusable.
 */
static void heartbeat_loop(int fd)
{
    const struct timespec interval = { .tv_sec = 0, .tv_nsec = 50000000 };

    for (;;) {
        const unsigned char byte = 'H';
        ssize_t written = write(fd, &byte, sizeof(byte));

        if (written < 0 && errno != EINTR && errno != EAGAIN)
            _exit(121);
        if (written == 0)
            _exit(121);
        nanosleep(&interval, NULL);
    }
}

/**
 * @brief Start an unrelated child that periodically proves parent progress.
 * @return 0 on success, or -1 if pipe setup or fork fails.
 */
static int start_heartbeat(void)
{
    int pipe_fds[2];
    int flags;

    if (pipe(pipe_fds) != 0)
        return -1;
    flags = fcntl(pipe_fds[0], F_GETFL);
    if (flags < 0 || fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK) != 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }

    heartbeat_pid = fork();
    if (heartbeat_pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        heartbeat_pid = -1;
        return -1;
    }
    if (heartbeat_pid == 0) {
        close(pipe_fds[0]);
        heartbeat_loop(pipe_fds[1]);
    }

    close(pipe_fds[1]);
    heartbeat_read_fd = pipe_fds[0];
    return 0;
}

/**
 * @brief Drain available heartbeat bytes and update the byte count.
 * @return 0 on success, or -1 on an unexpected read error.
 */
static int drain_heartbeat(void)
{
    unsigned char buffer[128];

    for (;;) {
        ssize_t count = read(heartbeat_read_fd, buffer, sizeof(buffer));

        if (count > 0) {
            heartbeat_bytes += (unsigned)count;
            continue;
        }
        if (count == 0)
            return -1;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
}

/**
 * @brief Wait for a fresh heartbeat while confirming its child remains alive.
 * @param previous_count Heartbeat count before waiting.
 * @param timeout_ms Maximum wait in milliseconds.
 * @return 1 if a fresh byte arrived from a live child, otherwise 0.
 */
static int wait_for_heartbeat(unsigned previous_count, unsigned timeout_ms)
{
    int64_t now;
    int64_t deadline;

    if (heartbeat_pid <= 0 || monotonic_milliseconds(&now) != 0)
        return 0;
    deadline = now + timeout_ms;
    for (;;) {
        int status;
        pid_t result;

        if (drain_heartbeat() != 0)
            return 0;
        result = waitpid(heartbeat_pid, &status, WNOHANG);
        if (result == heartbeat_pid) {
            heartbeat_pid = -1;
            return 0;
        }
        if (result < 0 && errno != EINTR)
            return 0;
        if (heartbeat_bytes > previous_count)
            return 1;
        if (monotonic_milliseconds(&now) != 0 || now >= deadline)
            return 0;
        sleep_briefly();
    }
}

/**
 * @brief Stop and reap the heartbeat child and close its pipe.
 * @return No value.
 */
static void stop_heartbeat(void)
{
    if (heartbeat_pid > 0) {
        int status;

        kill(heartbeat_pid, SIGTERM);
        while (waitpid(heartbeat_pid, &status, 0) < 0 && errno == EINTR)
            ;
        heartbeat_pid = -1;
    }
    if (heartbeat_read_fd >= 0) {
        close(heartbeat_read_fd);
        heartbeat_read_fd = -1;
    }
}

/**
 * @brief Wait for a child with a bounded deadline, killing it on timeout.
 * @param child Child process identifier.
 * @param timeout_ms Maximum wait in milliseconds.
 * @param status Receives the child's wait status.
 * @return 1 if reaped, 0 on timeout, or -1 on a wait/clock error.
 */
static int wait_for_child(pid_t child, unsigned timeout_ms, int *status)
{
    int64_t now;
    int64_t deadline;

    if (monotonic_milliseconds(&now) != 0)
        return -1;
    deadline = now + timeout_ms;
    for (;;) {
        pid_t result = waitpid(child, status, WNOHANG);

        if (result == child)
            return 1;
        if (result < 0 && errno != EINTR)
            return -1;
        if (monotonic_milliseconds(&now) != 0)
            return -1;
        if (now >= deadline) {
            int ignored_status;

            kill(child, SIGKILL);
            while (waitpid(child, &ignored_status, 0) < 0 && errno == EINTR)
                ;
            return 0;
        }
        sleep_briefly();
    }
}

/**
 * @brief Execute one user-mode HLT without installing a recovery handler.
 * @return Returns only if the processor unexpectedly advances past HLT.
 */
static void execute_unhandled_hlt(void)
{
    __asm__ volatile("hlt" ::: "memory");
}

/**
 * @brief Validate a Linux SIGSEGV context and redirect execution past HLT.
 * @param signal_number Delivered signal number.
 * @param info Kernel-provided signal information.
 * @param context Saved user register and floating-point context.
 * @return No value; writes only signal-safe state and the saved RIP.
 */
static void recovery_handler(int signal_number, siginfo_t *info, void *context)
{
    ucontext_t *uc = (ucontext_t *)context;
    greg_t *registers;
    int ok = 1;
    uint64_t packed_segments;

    gp_handler_seen = 1;
    if (signal_number != SIGSEGV || info == NULL ||
        info->si_code != SI_KERNEL || (uintptr_t)info->si_addr != 0 ||
        uc == NULL) {
        ok = 0;
    }

    if (uc == NULL)
        _exit(122);
    registers = uc->uc_mcontext.gregs;
    packed_segments = (uint64_t)registers[REG_CSGSFS];
    if ((uintptr_t)registers[REG_RIP] != gp_fault_rip ||
        (uintptr_t)registers[REG_RSP] != gp_expected_rsp ||
        registers[REG_TRAPNO] != 13 || registers[REG_ERR] != 0 ||
        ((uint64_t)registers[REG_EFL] & EFLAGS_CHECK_MASK) !=
            (gp_expected_eflags & EFLAGS_CHECK_MASK) ||
        ((packed_segments >> 0) & UINT64_C(0xffff)) != gp_expected_cs ||
        ((packed_segments >> 16) & UINT64_C(0xffff)) != gp_expected_gs ||
        ((packed_segments >> 32) & UINT64_C(0xffff)) != gp_expected_fs ||
        (((packed_segments >> 0) & UINT64_C(0xffff)) & 3) != 3 ||
        (uint64_t)registers[REG_R8] != EXPECT_R8 ||
        (uint64_t)registers[REG_R9] != EXPECT_R9 ||
        (uint64_t)registers[REG_R10] != EXPECT_R10 ||
        (uint64_t)registers[REG_R11] != EXPECT_R11 ||
        (uint64_t)registers[REG_R12] != EXPECT_R12 ||
        (uint64_t)registers[REG_R13] != EXPECT_R13 ||
        (uint64_t)registers[REG_R14] != EXPECT_R14 ||
        (uint64_t)registers[REG_R15] != EXPECT_R15 ||
        (uint64_t)registers[REG_RDI] != EXPECT_RDI ||
        (uint64_t)registers[REG_RSI] != EXPECT_RSI ||
        (uint64_t)registers[REG_RBP] != EXPECT_RBP ||
        (uint64_t)registers[REG_RBX] != EXPECT_RBX ||
        (uint64_t)registers[REG_RDX] != EXPECT_RDX ||
        (uint64_t)registers[REG_RAX] != EXPECT_RAX ||
        (uint64_t)registers[REG_RCX] != EXPECT_RCX)
        ok = 0;

    if (uc->uc_mcontext.fpregs == NULL ||
        uc->uc_mcontext.fpregs->mxcsr != gp_expected_mxcsr ||
        uc->uc_mcontext.fpregs->_xmm[0].element[0] != gp_expected_xmm0[0] ||
        uc->uc_mcontext.fpregs->_xmm[0].element[1] != gp_expected_xmm0[1] ||
        uc->uc_mcontext.fpregs->_xmm[0].element[2] != gp_expected_xmm0[2] ||
        uc->uc_mcontext.fpregs->_xmm[0].element[3] != gp_expected_xmm0[3])
        ok = 0;

    gp_handler_ok = ok;
    registers[REG_RIP] = (greg_t)gp_resume_rip;
}

/**
 * @brief Set register, flags, segment, and FPU sentinels around one HLT.
 * @return No value; returns at the exact instruction immediately after HLT.
 */
static void execute_recoverable_hlt(void)
{
    __asm__ volatile(
        "pushq %%rbp\n\t"
        "pushq $0\n\t"
        "xorq %%rax, %%rax\n\t"
        "movw %%cs, %%ax\n\t"
        "movq %%rax, gp_expected_cs(%%rip)\n\t"
        "xorq %%rax, %%rax\n\t"
        "movw %%gs, %%ax\n\t"
        "movq %%rax, gp_expected_gs(%%rip)\n\t"
        "xorq %%rax, %%rax\n\t"
        "movw %%fs, %%ax\n\t"
        "movq %%rax, gp_expected_fs(%%rip)\n\t"
        "movq $0x246, gp_expected_eflags(%%rip)\n\t"
        "pushq $0x246\n\t"
        "popfq\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "movq %%rax, gp_fault_rip(%%rip)\n\t"
        "leaq 2f(%%rip), %%rax\n\t"
        "movq %%rax, gp_resume_rip(%%rip)\n\t"
        "movq %%rsp, gp_expected_rsp(%%rip)\n\t"
        "movdqu gp_expected_xmm0(%%rip), %%xmm0\n\t"
        "ldmxcsr gp_expected_mxcsr(%%rip)\n\t"
        "movabsq $0x8102030405060708, %%r8\n\t"
        "movabsq $0x9203040506070809, %%r9\n\t"
        "movabsq $0xa304050607080a0b, %%r10\n\t"
        "movabsq $0xb405060708090c0d, %%r11\n\t"
        "movabsq $0xc5060708090a0e0f, %%r12\n\t"
        "movabsq $0xd60708090a0b1011, %%r13\n\t"
        "movabsq $0xe708090a0b0c1213, %%r14\n\t"
        "movabsq $0xf8090a0b0c0d1415, %%r15\n\t"
        "movabsq $0x190a0b0c0d0e1617, %%rdi\n\t"
        "movabsq $0x2a0b0c0d0e0f1819, %%rsi\n\t"
        "movabsq $0x3b0c0d0e0f101a1b, %%rbp\n\t"
        "movabsq $0x4c0d0e0f10111c1d, %%rbx\n\t"
        "movabsq $0x5d0e0f1012131e1f, %%rdx\n\t"
        "movabsq $0x7f10111213152223, %%rcx\n\t"
        "movabsq $0x6e0f101112142021, %%rax\n\t"
        "1: hlt\n\t"
        "2:\n\t"
        "movq %%rax, gp_after_rax(%%rip)\n\t"
        "movq %%rbx, gp_after_rbx(%%rip)\n\t"
        "movq %%rcx, gp_after_rcx(%%rip)\n\t"
        "movq %%rdx, gp_after_rdx(%%rip)\n\t"
        "movq %%rsi, gp_after_rsi(%%rip)\n\t"
        "movq %%rdi, gp_after_rdi(%%rip)\n\t"
        "movq %%rbp, gp_after_rbp(%%rip)\n\t"
        "movq %%r8, gp_after_r8(%%rip)\n\t"
        "movq %%r9, gp_after_r9(%%rip)\n\t"
        "movq %%r10, gp_after_r10(%%rip)\n\t"
        "movq %%r11, gp_after_r11(%%rip)\n\t"
        "movq %%r12, gp_after_r12(%%rip)\n\t"
        "movq %%r13, gp_after_r13(%%rip)\n\t"
        "movq %%r14, gp_after_r14(%%rip)\n\t"
        "movq %%r15, gp_after_r15(%%rip)\n\t"
        "movq %%rsp, gp_after_rsp(%%rip)\n\t"
        "movdqu %%xmm0, gp_after_xmm0(%%rip)\n\t"
        "stmxcsr gp_after_mxcsr(%%rip)\n\t"
        "pushfq\n\t"
        "popq %%rax\n\t"
        "movq %%rax, gp_after_eflags(%%rip)\n\t"
        "xorq %%rax, %%rax\n\t"
        "movw %%cs, %%ax\n\t"
        "movq %%rax, gp_after_cs(%%rip)\n\t"
        "xorq %%rax, %%rax\n\t"
        "movw %%gs, %%ax\n\t"
        "movq %%rax, gp_after_gs(%%rip)\n\t"
        "xorq %%rax, %%rax\n\t"
        "movw %%fs, %%ax\n\t"
        "movq %%rax, gp_after_fs(%%rip)\n\t"
        "addq $8, %%rsp\n\t"
        "popq %%rbp\n\t"
        :
        :
        : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
          "xmm0", "cc", "memory");
}

/**
 * @brief Compare recovered integer, flags, segment, and FPU state to sentinels.
 * @return 1 if every tested state item matches, otherwise 0.
 */
static int recovered_state_matches(void)
{
    if (gp_after_rax != EXPECT_RAX || gp_after_rbx != EXPECT_RBX ||
        gp_after_rcx != EXPECT_RCX || gp_after_rdx != EXPECT_RDX ||
        gp_after_rsi != EXPECT_RSI || gp_after_rdi != EXPECT_RDI ||
        gp_after_rbp != EXPECT_RBP || gp_after_r8 != EXPECT_R8 ||
        gp_after_r9 != EXPECT_R9 || gp_after_r10 != EXPECT_R10 ||
        gp_after_r11 != EXPECT_R11 || gp_after_r12 != EXPECT_R12 ||
        gp_after_r13 != EXPECT_R13 || gp_after_r14 != EXPECT_R14 ||
        gp_after_r15 != EXPECT_R15 || gp_after_rsp != gp_expected_rsp ||
        (gp_after_eflags & EFLAGS_CHECK_MASK) !=
            (gp_expected_eflags & EFLAGS_CHECK_MASK) ||
        gp_after_cs != gp_expected_cs || gp_after_gs != gp_expected_gs ||
        gp_after_fs != gp_expected_fs || gp_after_mxcsr != gp_expected_mxcsr ||
        gp_after_xmm0[0] != gp_expected_xmm0[0] ||
        gp_after_xmm0[1] != gp_expected_xmm0[1] ||
        gp_after_xmm0[2] != gp_expected_xmm0[2] ||
        gp_after_xmm0[3] != gp_expected_xmm0[3])
        return 0;
    return 1;
}

/**
 * @brief Install the requested SIGSEGV action and execute a faulting HLT child.
 * @param name Stable case label used in the result marker.
 * @param ignored If nonzero, request SIG_IGN; otherwise request SIG_DFL.
 * @param blocked If nonzero, block SIGSEGV before executing HLT.
 * @return 0 if the child was terminated by SIGSEGV and heartbeat advanced;
 *         otherwise 1.
 */
static int run_forced_fault_case(const char *name, int ignored, int blocked)
{
    pid_t child;
    int status = 0;
    int waited;
    unsigned before;

    if (drain_heartbeat() != 0) {
        printf("[user-gp] FAIL %s reason=heartbeat-read\n", name);
        fflush(stdout);
        return 1;
    }
    before = heartbeat_bytes;

    child = fork();
    if (child < 0) {
        printf("[user-gp] FAIL %s reason=fork\n", name);
        fflush(stdout);
        return 1;
    }
    if (child == 0) {
        struct sigaction action = { 0 };
        sigset_t set;

        if (heartbeat_read_fd >= 0)
            close(heartbeat_read_fd);
        alarm(CHILD_ALARM_SECONDS);
        action.sa_handler = ignored ? SIG_IGN : SIG_DFL;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGSEGV, &action, NULL) != 0)
            _exit(123);
        if (blocked) {
            sigemptyset(&set);
            sigaddset(&set, SIGSEGV);
            if (sigprocmask(SIG_BLOCK, &set, NULL) != 0)
                _exit(124);
        }
        execute_unhandled_hlt();
        _exit(125);
    }

    waited = wait_for_child(child, CASE_TIMEOUT_MS, &status);
    if (waited != 1 || !WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV) {
        printf("[user-gp] FAIL %s reason=wait-status status=%d waited=%d\n",
               name, status, waited);
        fflush(stdout);
        return 1;
    }
    if (!wait_for_heartbeat(before, HEARTBEAT_TIMEOUT_MS)) {
        printf("[user-gp] FAIL %s reason=heartbeat-not-live\n", name);
        fflush(stdout);
        return 1;
    }
    printf("[user-gp] PASS %s\n", name);
    fflush(stdout);
    return 0;
}

/**
 * @brief Exercise SIGSEGV delivery and rt_sigreturn recovery in a child.
 * @return 0 if context and post-return state match, otherwise 1.
 */
static int run_recovery_case(void)
{
    pid_t child;
    int status = 0;
    int waited;
    unsigned before;

    if (drain_heartbeat() != 0) {
        printf("[user-gp] FAIL recovery reason=heartbeat-read\n");
        fflush(stdout);
        return 1;
    }
    before = heartbeat_bytes;

    child = fork();
    if (child < 0) {
        printf("[user-gp] FAIL recovery reason=fork\n");
        fflush(stdout);
        return 1;
    }
    if (child == 0) {
        struct sigaction action = { 0 };

        if (heartbeat_read_fd >= 0)
            close(heartbeat_read_fd);
        alarm(CHILD_ALARM_SECONDS);
        action.sa_sigaction = recovery_handler;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_SIGINFO;
        if (sigaction(SIGSEGV, &action, NULL) != 0)
            _exit(126);
        execute_recoverable_hlt();
        if (!gp_handler_seen || !gp_handler_ok || !recovered_state_matches())
            _exit(127);
        _exit(0);
    }

    waited = wait_for_child(child, CASE_TIMEOUT_MS, &status);
    if (waited != 1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("[user-gp] FAIL recovery reason=wait-status status=%d waited=%d\n",
               status, waited);
        fflush(stdout);
        return 1;
    }
    if (!wait_for_heartbeat(before, HEARTBEAT_TIMEOUT_MS)) {
        printf("[user-gp] FAIL recovery reason=heartbeat-not-live\n");
        fflush(stdout);
        return 1;
    }
    printf("[user-gp] PASS recovery-siginfo-rt_sigreturn-gpr-fpu\n");
    fflush(stdout);
    return 0;
}

/**
 * @brief Run the bounded native user-#GP lifecycle and recovery probe.
 * @return 0 if every disposition, recovery, and heartbeat assertion passes.
 */
int main(void)
{
    unsigned failures = 0;
    unsigned initial_heartbeat;

    printf("[user-gp] START cases=default,ignored,blocked,recovery\n");
    fflush(stdout);
    if (start_heartbeat() != 0) {
        printf("[user-gp] FAIL heartbeat-start\n");
        printf("[user-gp] DONE failures=1\n");
        fflush(stdout);
        return 1;
    }
    initial_heartbeat = heartbeat_bytes;
    if (!wait_for_heartbeat(initial_heartbeat, HEARTBEAT_TIMEOUT_MS)) {
        printf("[user-gp] FAIL heartbeat-start reason=no-live-heartbeat\n");
        ++failures;
    } else {
        printf("[user-gp] PASS heartbeat-start\n");
        fflush(stdout);
    }

    failures += (unsigned)run_forced_fault_case("default-sigsegv", 0, 0);
    failures += (unsigned)run_forced_fault_case("ignored-forced-sigsegv", 1, 0);
    failures += (unsigned)run_forced_fault_case("blocked-forced-sigsegv", 0, 1);
    failures += (unsigned)run_recovery_case();

    if (drain_heartbeat() == 0 && heartbeat_pid > 0 &&
        wait_for_heartbeat(heartbeat_bytes, HEARTBEAT_TIMEOUT_MS)) {
        printf("[user-gp] PASS heartbeat-alive\n");
        fflush(stdout);
    } else {
        printf("[user-gp] FAIL heartbeat-alive\n");
        fflush(stdout);
        ++failures;
    }
    stop_heartbeat();
    printf("[user-gp] DONE failures=%u\n", failures);
    fflush(stdout);
    return failures == 0 ? 0 : 1;
}
