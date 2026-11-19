#define _GNU_SOURCE
#include "../../userland/apps/installer/installer_job.h"
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
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
    puts("PASS nonblocking progress, final result delivery, and abnormal worker exit");
}
