#define _GNU_SOURCE 1
#include "../../userland/apps/installer/installer_update_supervisor.h"
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct fixture { int mode; pid_t descendant; unsigned pulses; volatile sig_atomic_t cancel; };
static int operation(void *context)
{
    struct fixture *test = context;
    if (test->mode == 0) return -ENOSPC;
    if (test->mode == 1) { raise(SIGBUS); return 0; }
    if (test->mode == 5) __atomic_store_n(&test->descendant, getpid(), __ATOMIC_RELEASE);
    if (test->mode == 2 || test->mode == 3) {
        pid_t child = fork();
        assert(child >= 0);
        if (!child) {
            signal(SIGTERM, SIG_IGN);
            for (;;) pause();
        }
        test->descendant = child;
        if (test->mode == 2) return 0; /* Even success must retire descendants. */
    }
    if (test->mode == 3) signal(SIGTERM, SIG_IGN);
    for (;;) (void)poll(NULL, 0, 10);
}
static void pulse(void *context, int stopping)
{
    struct fixture *test = context;
    (void)stopping;
    if (++test->pulses == 1 && test->mode == 4) test->cancel = 1;
}
static unsigned long elapsed(struct timespec start)
{
    struct timespec now;
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    return (unsigned long)((now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000);
}
static void test_coordinator_death(struct fixture *test)
{
    test->mode = 5;
    test->descendant = 0;
    pid_t coordinator = fork();
    assert(coordinator >= 0);
    if (!coordinator) {
        (void)installer_update_supervise(operation, test, 10000, NULL, NULL, NULL);
        _exit(1);
    }
    struct timespec start;
    assert(!clock_gettime(CLOCK_MONOTONIC, &start));
    pid_t stage;
    while (!(stage = __atomic_load_n(&test->descendant, __ATOMIC_ACQUIRE)) && elapsed(start) < 2000)
        (void)poll(NULL, 0, 10);
    assert(stage > 1);
    assert(!kill(coordinator, SIGKILL));
    int status;
    assert(waitpid(coordinator, &status, 0) == coordinator);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    /* The test is the nearest Linux subreaper. PDEATHSIG must retire the stage
     * even though nobody remains inside supervise() to send its group a signal. */
    assert(!clock_gettime(CLOCK_MONOTONIC, &start));
    pid_t result;
    while (!(result = waitpid(stage, &status, WNOHANG)) && elapsed(start) < 2000)
        (void)poll(NULL, 0, 10);
    assert(result == stage && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
}
int main(void)
{
    struct fixture *test = mmap(NULL, sizeof(*test), PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    assert(test != MAP_FAILED);
    assert(installer_update_supervise(operation, test, 1000, NULL, NULL, NULL) == -ENOSPC);
    test->mode = 1;
    assert(installer_update_supervise(operation, test, 1000, NULL, NULL, NULL) == -EIO);
    test->mode = 2;
    assert(!installer_update_supervise(operation, test, 1000, NULL, NULL, NULL));
    assert(test->descendant > 1 && kill(test->descendant, 0) < 0 && errno == ESRCH);
    test->mode = 3;
    struct timespec started;
    assert(!clock_gettime(CLOCK_MONOTONIC, &started));
    assert(installer_update_supervise(operation, test, 80, NULL, pulse, test) == -ETIMEDOUT);
    assert(elapsed(started) < 3000);
    assert(test->descendant > 1 && kill(test->descendant, 0) < 0 && errno == ESRCH);
    test->mode = 4; test->pulses = 0;
    assert(installer_update_supervise(operation, test, 3000, &test->cancel, pulse, test) == -ECANCELED);
    assert(test->pulses);
    assert(installer_update_supervise(operation, test, 1000, &test->cancel, NULL, NULL) == -ECANCELED);
    test_coordinator_death(test);
    int status;
    assert(waitpid(-1, &status, WNOHANG) < 0 && errno == ECHILD);
    assert(!munmap(test, sizeof(*test)));
    puts("PASS supervised errno, SIGBUS, deadline, TERM escalation, descendant retirement, cancellation and coordinator death");
}
