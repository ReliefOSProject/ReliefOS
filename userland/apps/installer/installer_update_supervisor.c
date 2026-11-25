#define _GNU_SOURCE 1
#include "installer_update_supervisor.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct task_result { int result; unsigned complete; };

static int monotonic_ms(uint64_t *value)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -errno;
    *value = (uint64_t)now.tv_sec * 1000u + (unsigned long)now.tv_nsec / 1000000u;
    return 0;
}

int installer_update_supervise(installer_update_task_fn task, void *context,
    uint64_t timeout_ms, const volatile sig_atomic_t *cancelled,
    installer_update_pulse_fn pulse, void *pulse_context)
{
    if (!task || !timeout_ms) return -EINVAL;
    if (cancelled && *cancelled) return -ECANCELED;
    uint64_t started;
    int error = monotonic_ms(&started);
    if (error) return error;
    struct task_result *shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) return -errno;
    shared->complete = 0;
    /* Linux can adopt grandchildren locally. ReliefNT reparents them to its
     * system reaper and may reject this optional prctl. */
#ifdef PR_SET_CHILD_SUBREAPER
    (void)prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);
#endif
    pid_t coordinator = getpid();
    pid_t child = fork();
    if (child < 0) { error = -errno; munmap(shared, sizeof(*shared)); return error; }
    if (!child) {
        /* Every command inherits this group and the transaction's flock.
         * Only the coordinator publishes the terminal protocol. */
        if (setpgid(0, 0) < 0) _exit(125);
#ifdef PR_SET_PDEATHSIG
        if (prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) < 0 || getppid() != coordinator) _exit(125);
#endif
        signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGHUP, SIG_DFL);
        signal(SIGPIPE, SIG_IGN);
        shared->result = task(context);
        __atomic_store_n(&shared->complete, 1, __ATOMIC_RELEASE);
        _exit(0);
    }
    if (setpgid(child, child) < 0 && errno != EACCES && errno != ESRCH) error = -errno;
    uint64_t stopped = 0, presented = started;
    int stopping = 0, killed = 0, status = 0;
    for (;;) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) break;
        if (result < 0 && errno != EINTR) { if (!error) error = -errno; break; }
        uint64_t now = started;
        int clock_error = monotonic_ms(&now);
        if (clock_error && !error) error = clock_error;
        if (!error && cancelled && *cancelled) error = -ECANCELED;
        if (!error && now - started >= timeout_ms) error = -ETIMEDOUT;
        if (error && !stopping) {
            (void)kill(-child, SIGTERM);
            stopped = now; stopping = 1;
        }
        if (stopping && !killed && (clock_error || now - stopped >= 1000)) {
            (void)kill(-child, SIGKILL);
            killed = 1;
        }
        if (pulse && (now - presented >= 250 || clock_error)) {
            pulse(pulse_context, error); presented = now;
        }
        /* No read(), waitpid(0), or unbounded poll is allowed here. If a
         * kernel operation cannot yet retire after SIGKILL, keep ownership
         * and report liveness; never release the lock over a live writer. */
        (void)poll(NULL, 0, 25);
    }
    /* A successful leader must not leave a background command writing later.
     * Reap direct children in this group; adopted descendants belong to PID 1. */
    (void)kill(-child, SIGKILL);
    for (;;) {
        int ignored;
        pid_t result;
        do { result = waitpid(-child, &ignored, WNOHANG); } while (result > 0);
        if (kill(-child, 0) < 0 && errno == ESRCH) break;
        if (pulse) pulse(pulse_context, error ? error : -EINPROGRESS);
        (void)poll(NULL, 0, 25);
    }
    if (!error) {
        if (!WIFEXITED(status) || WEXITSTATUS(status) ||
            !__atomic_load_n(&shared->complete, __ATOMIC_ACQUIRE)) {
            if (WIFSIGNALED(status))
                fprintf(stderr, "[installer-update] stage process terminated signal=%d\n", WTERMSIG(status));
            else if (WIFEXITED(status))
                fprintf(stderr, "[installer-update] stage process exited status=%d without a result\n", WEXITSTATUS(status));
            error = -EIO;
        }
        else error = shared->result;
    }
    munmap(shared, sizeof(*shared));
    return error;
}
