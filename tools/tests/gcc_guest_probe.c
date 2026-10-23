#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../../kernel/reliefnt/tools/tests/vfork_linux_edges.c"

#if defined(__x86_64__) && !defined(SYS_vfork)
#define SYS_vfork 58
#endif
#ifndef CLONE_VM
#define CLONE_VM      0x00000100
#define CLONE_SIGHAND 0x00000800
#define CLONE_VFORK   0x00004000
#define CLONE_THREAD  0x00010000
#endif

#define GCC "/opt/dyne/gcc-musl/bin/x86_64-linux-musl-gcc"
#include "../../userland/apps/installer/installer_directory.h"
#define SYSROOT "--sysroot=/opt/dyne/gcc-musl/x86_64-linux-musl"
#define SELF_FALLBACK "/usr/lib/reliefos/tests/gcc-probe.elf"

static char self_path[256];
static volatile int shared_marker;
static volatile sig_atomic_t shared_signal_seen;

__attribute__((noreturn)) static void raw_exit(int code)
{
    register long rax __asm__("rax") = SYS_exit;
    register long rdi __asm__("rdi") = code;
    __asm__ volatile("syscall" : "+a"(rax) : "D"(rdi) : "rcx", "r11", "memory");
    __builtin_unreachable();
}

static void raw_nanosleep_ms(long milliseconds)
{
    struct timespec delay = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
    while (syscall(SYS_nanosleep, &delay, &delay) == -1 && errno == EINTR) {}
}

/* CLONE_VM|CLONE_VFORK|SIGCHLD with a real child stack; the child enters
 * child_fn() directly instead of returning through a libc wrapper whose
 * return address lives on the old stack. */
__attribute__((noinline))
static long raw_clone_vfork(int (*child_fn)(void *), void *argument, void *stack_top)
{
    register long rax __asm__("rax") = SYS_clone;
    register long rdi __asm__("rdi") = CLONE_VM | CLONE_VFORK | SIGCHLD;
    register void *rsi __asm__("rsi") = stack_top;
    register long rdx __asm__("rdx") = 0;
    register long r10 __asm__("r10") = 0;
    register long r8 __asm__("r8") = 0;
    __asm__ volatile(
        "syscall\n\t"
        "test %%rax, %%rax\n\t"
        "jnz 1f\n\t"
        "mov %[arg], %%rdi\n\t"
        "call *%[fn]\n\t"
        "mov $60, %%rax\n\t"
        "xor %%rdi, %%rdi\n\t"
        "syscall\n\t"
        "1:\n\t"
        : "+a"(rax)
        : "D"(rdi), "S"(rsi), "d"(rdx), "r"(r10), "r"(r8),
          [arg]"r"(argument), [fn]"r"(child_fn)
        : "rcx", "r11", "memory", "cc");
    return rax;
}

static long raw_vfork_child_exit(int code)
{
    register long rax __asm__("rax") = SYS_vfork;
    register long rdi __asm__("rdi") = code;
    __asm__ volatile(
        "syscall\n\t"
        "test %%rax, %%rax\n\t"
        "jnz 1f\n\t"
        "mov $60, %%rax\n\t"
        "syscall\n\t"
        "1:\n\t"
        : "+a"(rax) : "D"(rdi) : "rcx", "r11", "memory", "cc");
    return rax;
}

static void signal_handler(int sig)
{
    (void)sig;
    shared_signal_seen = 1;
}

static int marker_child(void *unused)
{
    (void)unused;
    shared_marker = 1234;
    raw_exit(7);
}

static int delayed_marker_child(void *unused)
{
    (void)unused;
    raw_nanosleep_ms(400);
    shared_marker = 4321;
    raw_exit(9);
}

static int vfork_lifecycle(void)
{
    size_t stack_size = 64 * 1024;
    void *stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) {
        printf("[vfork-stack] life FAIL mmap errno=%d\n", errno);
        return 1;
    }
    for (int iteration = 0; iteration < 8; ++iteration) {
        shared_marker = 0;
        long child = raw_clone_vfork(marker_child, NULL, (char *)stack + stack_size);
        if (child <= 0) {
            printf("[vfork-stack] life FAIL clone iter=%d errno=%d\n", iteration, errno);
            munmap(stack, stack_size);
            return 1;
        }
        if (shared_marker != 1234) {
            printf("[vfork-stack] life FAIL shared-marker iter=%d value=%d\n", iteration, shared_marker);
            munmap(stack, stack_size);
            return 1;
        }
        int status = 0;
        if (waitpid((pid_t)child, &status, 0) != child ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 7) {
            printf("[vfork-stack] life FAIL wait iter=%d status=%#x\n", iteration, status);
            munmap(stack, stack_size);
            return 1;
        }
    }
    /* Failing flag validation must not leak a wait link or corrupt the task. */
    errno = 0;
    long invalid = syscall(SYS_clone, SIGCHLD | CLONE_THREAD,
                           (char *)stack + stack_size, 0, 0, 0);
    if (invalid != -1 || errno != EINVAL) {
        printf("[vfork-stack] rollback FAIL clone(THREAD) ret=%ld errno=%d\n", invalid, errno);
        munmap(stack, stack_size);
        return 1;
    }
    munmap(stack, stack_size);
    pid_t fallback = fork();
    if (fallback < 0) {
        printf("[vfork-stack] rollback FAIL fork errno=%d\n", errno);
        return 1;
    }
    if (!fallback) _exit(0);
    int status = 0;
    if (waitpid(fallback, &status, 0) != fallback ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("[vfork-stack] rollback FAIL fork status=%#x\n", status);
        return 1;
    }
    printf("[vfork-stack] lifecycle PASS (CLONE_VM visibility, wait/reap x8, rollback)\n");
    return 0;
}

static int vfork_syscall(void)
{
    long child = raw_vfork_child_exit(5);
    if (child <= 0) {
        printf("[vfork-stack] vfork FAIL ret=%ld errno=%d\n", child, errno);
        return 1;
    }
    int status = 0;
    if (waitpid((pid_t)child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 5) {
        printf("[vfork-stack] vfork FAIL status=%#x\n", status);
        return 1;
    }
    printf("[vfork-stack] vfork(58) PASS\n");
    return 0;
}

struct exec_child_args {
    int write_fd;
    const char *self;
};

static int exec_child(void *argument)
{
    struct exec_child_args *args = argument;
    char marker = 'A';
    if (syscall(SYS_write, args->write_fd, &marker, 1) != 1) raw_exit(112);
    char *const argv[] = {(char *)args->self, (char *)"--vfork-exec-child", NULL};
    char *const envp[] = {"PATH=/bin:/sbin:/usr/bin:/usr/sbin", NULL};
    execve(args->self, argv, envp);
    raw_exit(111);
}

static int vfork_exec(const char *self)
{
    int pipefd[2];
    if (pipe(pipefd)) {
        printf("[vfork-stack] exec FAIL pipe errno=%d\n", errno);
        return 1;
    }
    size_t stack_size = 64 * 1024;
    void *stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) {
        printf("[vfork-stack] exec FAIL mmap errno=%d\n", errno);
        return 1;
    }
    struct exec_child_args args = {.write_fd = pipefd[1], .self = self};
    long child = raw_clone_vfork(exec_child, &args, (char *)stack + stack_size);
    if (child <= 0) {
        printf("[vfork-stack] exec FAIL clone errno=%d\n", errno);
        return 1;
    }
    char marker = 0;
    if (read(pipefd[0], &marker, 1) != 1 || marker != 'A') {
        printf("[vfork-stack] exec FAIL pre-exec marker=%d errno=%d\n", marker, errno);
        return 1;
    }
    int status = 0;
    /* The parent must wake when execve commits, while the new image is still
     * sleeping; waking only at child exit would return this pid here. */
    pid_t early = waitpid((pid_t)child, &status, WNOHANG);
    if (early != 0) {
        printf("[vfork-stack] exec FAIL early-reap ret=%d status=%#x\n", (int)early, status);
        return 1;
    }
    if (waitpid((pid_t)child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("[vfork-stack] exec FAIL status=%#x\n", status);
        return 1;
    }
    close(pipefd[0]);
    close(pipefd[1]);
    munmap(stack, stack_size);
    printf("[vfork-stack] exec PASS (wake at exec commit, exit code 0)\n");
    return 0;
}

static int vfork_handled_signal(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL)) {
        printf("[vfork-stack] signal FAIL sigaction errno=%d\n", errno);
        return 1;
    }
    shared_signal_seen = 0;
    shared_marker = 0;
    pid_t self = getpid();
    pid_t sender = fork();
    if (sender < 0) {
        printf("[vfork-stack] signal FAIL fork errno=%d\n", errno);
        return 1;
    }
    if (!sender) {
        raw_nanosleep_ms(100);
        kill(self, SIGUSR1);
        _exit(0);
    }
    size_t stack_size = 64 * 1024;
    void *stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) return 1;
    long child = raw_clone_vfork(delayed_marker_child, NULL, (char *)stack + stack_size);
    if (child <= 0) {
        printf("[vfork-stack] signal FAIL clone errno=%d\n", errno);
        return 1;
    }
    /* TASK_KILLABLE keeps a caught non-fatal signal pending until exec/exit. */
    if (shared_marker != 4321 || shared_signal_seen != 1) {
        printf("[vfork-stack] signal FAIL early marker=%d seen=%d\n",
               shared_marker, (int)shared_signal_seen);
        return 1;
    }
    int status = 0;
    if (waitpid((pid_t)child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 9) {
        printf("[vfork-stack] signal FAIL child status=%#x\n", status);
        return 1;
    }
    if (waitpid(sender, &status, 0) != sender ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("[vfork-stack] signal FAIL sender status=%#x\n", status);
        return 1;
    }
    munmap(stack, stack_size);
    printf("[vfork-stack] signal PASS (caught SIGUSR1 deferred across vfork)\n");
    return 0;
}

__attribute__((noinline)) static int grow_stack(int depth)
{
    /* __builtin_alloca() is the portable way to force one full page per frame;
     * a fixed volatile array may be packed into a few bytes by the optimizer
     * when only its ends are touched. */
    volatile char *page = __builtin_alloca(4096);
    for (int offset = 0; offset < 4096; offset += 64) page[offset] = (char)depth;
    int result = depth < 4096 ? grow_stack(depth + 1) : depth;
    return result + page[0] + page[4095];
}

static volatile pid_t *shared_child_pid;

static int sleeping_child(void *unused)
{
    (void)unused;
    *shared_child_pid = getpid();
    raw_nanosleep_ms(2000);
    raw_exit(0);
}

static int vfork_parent_exit_lifecycle(void)
{
    shared_child_pid = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared_child_pid == MAP_FAILED) {
        printf("[vfork-stack] parent-exit FAIL mmap errno=%d\n", errno);
        return 1;
    }
    *shared_child_pid = 0;
    pid_t victim = fork();
    if (victim < 0) {
        printf("[vfork-stack] parent-exit FAIL fork errno=%d\n", errno);
        return 1;
    }
    if (!victim) {
        size_t stack_size = 64 * 1024;
        void *stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (stack == MAP_FAILED) _exit(2);
        long child = raw_clone_vfork(sleeping_child, NULL, (char *)stack + stack_size);
        if (child > 0) _exit(3); /* blocked here until the child exits */
        _exit(4);
    }
    raw_nanosleep_ms(200);
    if (kill(victim, SIGKILL)) {
        printf("[vfork-stack] parent-exit FAIL kill(errno=%d)\n", errno);
        return 1;
    }
    int status = 0;
    if (waitpid(victim, &status, 0) != victim ||
        !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
        printf("[vfork-stack] parent-exit FAIL victim status=%#x\n", status);
        return 1;
    }
    for (unsigned attempt = 0; attempt < 1000 && !*shared_child_pid; ++attempt) raw_nanosleep_ms(1);
    if (*shared_child_pid <= 0) {
        printf("[vfork-stack] parent-exit FAIL grandchild pid=%d\n", (int)*shared_child_pid);
        return 1;
    }
    /* Kill the orphaned vfork child so the probe image shuts down cleanly. */
    if (kill(*shared_child_pid, SIGKILL)) {
        printf("[vfork-stack] parent-exit FAIL kill grandchild errno=%d\n", errno);
        return 1;
    }
    munmap((void *)shared_child_pid, 4096);
    printf("[vfork-stack] parent-exit PASS (fatal SIGKILL detaches, mm survives for child)\n");
    return 0;
}

static int faulting_child(void *unused)
{
    (void)unused;
    shared_marker = 55;
    *(volatile int *)(uintptr_t)1 = 1;
    raw_exit(0);
}

static int vfork_abnormal_exit(void)
{
    size_t stack_size = 64 * 1024;
    void *stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) return 1;
    shared_marker = 0;
    long child = raw_clone_vfork(faulting_child, NULL, (char *)stack + stack_size);
    if (child <= 0) {
        printf("[vfork-stack] abnormal FAIL clone errno=%d\n", errno);
        return 1;
    }
    if (shared_marker != 55) {
        printf("[vfork-stack] abnormal FAIL marker=%d\n", shared_marker);
        return 1;
    }
    int status = 0;
    if (waitpid((pid_t)child, &status, 0) != child) {
        printf("[vfork-stack] abnormal FAIL wait errno=%d\n", errno);
        return 1;
    }
    int killed_by_fault = WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
    if (!killed_by_fault) {
        printf("[vfork-stack] abnormal FAIL status=%#x\n", status);
        return 1;
    }
    munmap(stack, stack_size);
    printf("[vfork-stack] abnormal-exit PASS (page fault releases the vfork parent)\n");
    return 0;
}

static pthread_mutex_t thread_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t thread_cond = PTHREAD_COND_INITIALIZER;
static int thread_condition_ready;
static volatile int thread_counter;

static void *pthread_worker(void *argument)
{
    intptr_t value = (intptr_t)argument;
    pthread_mutex_lock(&thread_lock);
    thread_condition_ready = (int)value;
    pthread_cond_signal(&thread_cond);
    pthread_mutex_unlock(&thread_lock);
    __atomic_add_fetch(&thread_counter, 1, __ATOMIC_SEQ_CST);
    return (void *)(value * 2);
}

static int pthread_regression(void)
{
    for (int iteration = 0; iteration < 8; ++iteration) {
        pthread_t thread;
        intptr_t value = 10 + iteration;
        thread_condition_ready = 0;
        if (pthread_create(&thread, NULL, pthread_worker, (void *)value)) {
            printf("[vfork-stack] pthread FAIL create iter=%d errno=%d\n", iteration, errno);
            return 1;
        }
        pthread_mutex_lock(&thread_lock);
        while (!thread_condition_ready) pthread_cond_wait(&thread_cond, &thread_lock);
        int observed = thread_condition_ready;
        pthread_mutex_unlock(&thread_lock);
        void *result = NULL;
        if (pthread_join(thread, &result) || observed != value ||
            (intptr_t)result != value * 2) {
            printf("[vfork-stack] pthread FAIL join iter=%d observed=%d result=%ld errno=%d\n",
                   iteration, observed, (long)(intptr_t)result, errno);
            return 1;
        }
    }
    if (thread_counter != 8) {
        printf("[vfork-stack] pthread FAIL counter=%d\n", thread_counter);
        return 1;
    }
    printf("[vfork-stack] pthread PASS (create/cond/futex/join x8)\n");
    return 0;
}

static int stack_limit_checks(const char *self)
{
    struct rlimit original, value, current;
    if (getrlimit(RLIMIT_STACK, &original)) {
        printf("[vfork-stack] stack FAIL default getrlimit errno=%d\n", errno);
        return 1;
    }
    if (original.rlim_cur != 8ULL * 1024 * 1024 || original.rlim_max != RLIM_INFINITY) {
        printf("[vfork-stack] stack FAIL defaults cur=%llu max=%llu\n",
               (unsigned long long)original.rlim_cur, (unsigned long long)original.rlim_max);
        return 1;
    }
    value = original;
    value.rlim_cur = 256 * 1024;
    if (setrlimit(RLIMIT_STACK, &value) || getrlimit(RLIMIT_STACK, &current) ||
        current.rlim_cur != value.rlim_cur || current.rlim_max != value.rlim_max) {
        printf("[vfork-stack] stack FAIL set/get soft errno=%d\n", errno);
        return 1;
    }
    struct rlimit invalid = {512 * 1024, 256 * 1024};
    errno = 0;
    if (setrlimit(RLIMIT_STACK, &invalid) != -1 || errno != EINVAL) {
        printf("[vfork-stack] stack FAIL cur>max errno=%d\n", errno);
        return 1;
    }

    pid_t child = fork();
    if (child < 0) {
        printf("[vfork-stack] stack FAIL fork errno=%d\n", errno);
        return 1;
    }
    if (!child) {
        struct rlimit inherited;
        if (getrlimit(RLIMIT_STACK, &inherited) || inherited.rlim_cur != 256 * 1024) _exit(3);
        (void)grow_stack(0);
        _exit(0); /* Reaching here means RLIMIT_STACK did not limit growth. */
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child) {
        printf("[vfork-stack] stack FAIL wait errno=%d\n", errno);
        return 1;
    }
    int killed_by_fault = WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
    if (!killed_by_fault) {
        printf("[vfork-stack] stack FAIL overflow status=%#x\n", status);
        return 1;
    }

    value.rlim_cur = 1024 * 1024;
    if (setrlimit(RLIMIT_STACK, &value)) {
        printf("[vfork-stack] stack FAIL exec soft errno=%d\n", errno);
        return 1;
    }
    child = fork();
    if (child < 0) return 1;
    if (!child) {
        char *const argv[] = {(char *)self, (char *)"--check-stack-child", NULL};
        char *const envp[] = {"PATH=/bin:/sbin:/usr/bin:/usr/sbin", NULL};
        execve(self, argv, envp);
        _exit(4);
    }
    if (waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("[vfork-stack] stack FAIL exec inheritance status=%#x\n", status);
        return 1;
    }
    if (setrlimit(RLIMIT_STACK, &original)) {
        printf("[vfork-stack] stack FAIL restore errno=%d\n", errno);
        return 1;
    }
    printf("[vfork-stack] stack PASS (defaults, round-trip, growth fault, fork/exec inheritance)\n");
    return 0;
}

static int run(const char *label, char *const argv[], const char *capture_path,
               const char *expect)
{
    printf("[gcc-probe] BEGIN %s\n", label);
    pid_t pid = fork();
    if (!pid) {
        if (capture_path) {
            int fd = open(capture_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd >= 0) {
                dup2(fd, 1);
                close(fd);
            }
        }
        char *env[] = {"PATH=/opt/dyne/bin:/opt/dyne/gcc-musl/bin:/bin:/sbin:/usr/bin:/usr/sbin",
                       "TMPDIR=/tmp", "LC_ALL=C", "HOME=/tmp", NULL};
        execve(argv[0], argv, env);
        printf("[gcc-probe] EXECFAIL %s errno=%d\n", label, errno);
        _exit(127);
    }
    if (pid < 0) { printf("[gcc-probe] FORKFAIL errno=%d\n", errno); return 1; }
    struct timespec start, now, delay = {0, 20000000};
    clock_gettime(CLOCK_MONOTONIC, &start);
    int status, expired = 0;
    for (;;) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) break;
        if (result < 0 && errno != EINTR) { printf("[gcc-probe] WAITFAIL errno=%d\n", errno); return 1; }
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - start.tv_sec >= 120) {
            printf("[gcc-probe] TIMEOUT %s pid=%d\n", label, pid);
            expired = 1; kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            break;
        }
        nanosleep(&delay, NULL);
    }
    int code = expired ? 124 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (capture_path && !expired && code == 0) {
        char output[4096];
        int fd = open(capture_path, O_RDONLY);
        ssize_t got = fd >= 0 ? read(fd, output, sizeof(output) - 1) : -1;
        if (fd >= 0) close(fd);
        if (got <= 0) {
            printf("[gcc-probe] OUTPUTMISSING %s errno=%d\n", label, errno);
            code = 1;
        } else {
            output[got] = 0;
            if (!strstr(output, expect)) {
                printf("[gcc-probe] OUTPUTMISMATCH %s got=%s\n", label, output);
                code = 1;
            }
        }
    }
    printf("[gcc-probe] END %s code=%d status=%d\n", label, code, status);
    return code;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2 && !strcmp(argv[1], "--vfork-exec-child")) {
        struct timespec delay = {2, 500000000};
        while (nanosleep(&delay, &delay) && errno == EINTR) {}
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--check-stack-child")) {
        struct rlimit limit;
        return getrlimit(RLIMIT_STACK, &limit) || limit.rlim_cur != 1024 * 1024;
    }

    self_path[0] = 0;
    int edge_failures = vfork_linux_edges();
    ssize_t linked = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (linked > 0) {
        self_path[linked] = 0;
    } else if (argc > 0 && argv[0] && argv[0][0] == '/') {
        snprintf(self_path, sizeof(self_path), "%s", argv[0]);
    } else {
        snprintf(self_path, sizeof(self_path), "%s", SELF_FALLBACK);
    }

    unsigned failed = edge_failures;
    struct reliefos_dir_entry *headers = NULL;
    uint32_t header_count = 0;
    int directory_result = installer_list_dir(
        "/opt/dyne/gcc-musl/x86_64-linux-musl/include/linux", &headers, &header_count);
    printf("[gcc-probe] installer-directory result=%d count=%u\n", directory_result, header_count);
    failed += directory_result != 0 || header_count != 543;
    free(headers);
    failed += vfork_lifecycle();
    failed += vfork_syscall();
    failed += vfork_exec(self_path);
    failed += vfork_handled_signal();
    failed += vfork_parent_exit_lifecycle();
    failed += vfork_abnormal_exit();
    failed += pthread_regression();
    failed += stack_limit_checks(self_path);

    mkdir("/tmp", 01777);
    if (chdir("/tmp")) { printf("[gcc-probe] CHDIR errno=%d\n", errno); return 1; }
    FILE *source = fopen("gcc-probe.c", "w");
    if (!source) { printf("[gcc-probe] SOURCE errno=%d\n", errno); return 1; }
    fputs("#include <stdio.h>\nint main(void) { puts(\"GCC_GENERATED_OK\"); return 0; }\n", source);
    if (fclose(source)) return 1;
    char *version[] = {GCC, "--version", NULL};
    char *config[] = {GCC, "-v", NULL};
    char *preprocess[] = {GCC, SYSROOT, "-E", "gcc-probe.c", "-o", "gcc-probe.i", NULL};
    char *compile[] = {GCC, SYSROOT, "-v", "-O0", "-c", "gcc-probe.c", "-o", "gcc-probe.o", NULL};
    char *link[] = {GCC, SYSROOT, "-v", "-static", "gcc-probe.o", "-o", "gcc-probe", NULL};
    char *generated[] = {"/tmp/gcc-probe", NULL};
    int compiled;
    failed += run("version", version, NULL, NULL) != 0;
    failed += run("configuration", config, NULL, NULL) != 0;
    char *packaged[] = {"/usr/bin/musl-gcc", "-static", "gcc-probe.c", "-o", "gcc-packaged", NULL};
    char *packaged_run[] = {"/tmp/gcc-packaged", NULL};
    if (run("packaged-musl-gcc", packaged, NULL, NULL)) {
        failed++;
    } else {
        failed += run("packaged-generated", packaged_run, "/tmp/gcc-packaged.out", "GCC_GENERATED_OK") != 0;
    }
    char *packaged_as[] = {"/usr/bin/as", "--version", NULL};
    char *packaged_ld[] = {"/usr/bin/ld", "--version", NULL};
    failed += run("packaged-as", packaged_as, NULL, NULL) != 0;
    failed += run("packaged-ld", packaged_ld, NULL, NULL) != 0;
    failed += run("preprocess", preprocess, NULL, NULL) != 0;
    compiled = run("compile", compile, NULL, NULL);
    failed += compiled != 0;
    if (!compiled) {
        int linked = run("link", link, NULL, NULL);
        failed += linked != 0;
        if (!linked) {
            /* Execute the freshly linked stdio program and verify its output
             * and exit status, not only the compiler's exit code. */
            failed += run("generated", generated, "/tmp/gcc-generated.out",
                          "GCC_GENERATED_OK") != 0;
        }
    }
    printf("[gcc-probe] DONE failures=%u\n", failed);
    return failed != 0;
}
