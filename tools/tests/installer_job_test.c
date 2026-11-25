#define _GNU_SOURCE
#include "../../userland/apps/installer/installer_job.h"
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int busy_worker(void *context, struct installer_job_packet *final)
{
    int ready = *(int *)context;
    struct installer_job_packet packet = {0};
    for (unsigned i = 0; i < 10000; ++i) {
        packet.progress = i % 100;
        snprintf(packet.message, sizeof(packet.message), "copy %u", i);
        installer_job_publish(&packet);
    }
    assert(write(ready, "x", 1) == 1);
    close(ready);
    strcpy(final->message, "complete");
    final->progress = 100;
    final->success = 1;
    return -ENOSPC;
}

static int failed_worker(void *context, struct installer_job_packet *final)
{
    (void)context; (void)final;
    _exit(7);
}

static volatile sig_atomic_t stop_requested;
static void request_stop(int signal_number) { (void)signal_number; stop_requested = 1; }

static int cancelable_worker(void *context, struct installer_job_packet *final)
{
    int ready = *(int *)context;
    struct sigaction action = {.sa_handler = request_stop};
    sigemptyset(&action.sa_mask);
    assert(!sigaction(SIGTERM, &action, NULL));
    assert(write(ready, "x", 1) == 1);
    close(ready);
    while (!stop_requested) usleep(1000);
    strcpy(final->message, "cleanup complete");
    return -ECANCELED;
}

static uint64_t now_ms(void)
{
    struct timespec now;
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static int finish(struct installer_job *job)
{
    int result;
    uint64_t started = now_ms();
    do {
        result = installer_job_step(job);
        if (result > 0) usleep(1000);
        assert(now_ms() - started < 3000);
    } while (result > 0);
    return result;
}

int main(void)
{
    int notify[2];
    assert(!pipe(notify));
    struct installer_job state;
    assert(!installer_job_start(&state, busy_worker, &notify[1], NULL));
    close(notify[1]);
    /* A stalled UI must not stop the worker's file-copy progress. */
    struct pollfd ready = {.fd = notify[0], .events = POLLIN};
    assert(poll(&ready, 1, 2000) == 1);
    close(notify[0]);
    int result;
    do {
        result = installer_job_step(&state);
        if (result > 0) usleep(1000);
    } while (result > 0);
    assert(!result && state.complete);
    assert(state.final.result == -ENOSPC && state.final.success == 1);
    assert(state.final.progress == 100 && !strcmp(state.final.message, "complete"));
    assert(!installer_job_start(&state, failed_worker, NULL, NULL));
    do {
        result = installer_job_step(&state);
        if (result > 0) usleep(1000);
    } while (result > 0);
    assert(result < 0 && !state.complete);

    assert(!pipe(notify));
    assert(!installer_job_start(&state, cancelable_worker, &notify[1], NULL));
    close(notify[1]);
    ready = (struct pollfd){.fd = notify[0], .events = POLLIN};
    assert(poll(&ready, 1, 2000) == 1);
    close(notify[0]);
    assert(!installer_job_request_stop(&state));
    assert(!finish(&state));
    assert(state.final.result == -ECANCELED && !strcmp(state.final.message, "cleanup complete"));

    /* Final result delivery must terminate if a live UI stops consuming. */
    assert(!pipe(notify));
    assert(!installer_job_start(&state, busy_worker, &notify[1], NULL));
    close(notify[1]);
    ready = (struct pollfd){.fd = notify[0], .events = POLLIN};
    assert(poll(&ready, 1, 2000) == 1);
    close(notify[0]);
    uint64_t started = now_ms();
    siginfo_t observed;
    do {
        memset(&observed, 0, sizeof(observed));
        assert(!waitid(P_PID, (id_t)state.pid, &observed, WEXITED | WNOHANG | WNOWAIT));
        if (!observed.si_pid) usleep(1000);
        assert(now_ms() - started < 3000);
    } while (!observed.si_pid);
    assert(finish(&state) < 0);
    puts("PASS nonblocking progress, bounded final delivery, cancellation cleanup and abnormal worker exit");
}
