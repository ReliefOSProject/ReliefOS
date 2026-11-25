#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "installer_job.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int progress_fd = -1;
static struct installer_job_packet pending;
static size_t pending_offset, pending_size;

#ifndef INSTALLER_JOB_FINISH_TIMEOUT_MS
#define INSTALLER_JOB_FINISH_TIMEOUT_MS 2000
#endif

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

int installer_job_is_worker(void) { return progress_fd >= 0; }

/* A partially written record stays intact until the parent has consumed it. */
static int flush_packet(int wait)
{
    uint64_t start = monotonic_ms();
    while (pending_offset < pending_size) {
        ssize_t count = write(progress_fd, (char *)&pending + pending_offset,
                              pending_size - pending_offset);
        if (count > 0) { pending_offset += (size_t)count; continue; }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!wait) return 1;
            uint64_t now = monotonic_ms();
            if (now < start || now - start >= INSTALLER_JOB_FINISH_TIMEOUT_MS) return -1;
            struct pollfd descriptor = {.fd = progress_fd, .events = POLLOUT};
            int result;
            int remaining = INSTALLER_JOB_FINISH_TIMEOUT_MS - (int)(now - start);
            result = poll(&descriptor, 1, remaining);
            if (result < 0 && errno == EINTR) continue;
            if (result > 0 && !(descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) continue;
        }
        return -1;
    }
    pending_offset = pending_size = 0;
    return 0;
}

void installer_job_publish(const struct installer_job_packet *packet)
{
    if (progress_fd < 0 || flush_packet(0)) return;
    pending = *packet;
    pending.complete = 0;
    pending_offset = 0;
    pending_size = sizeof(pending);
    (void)flush_packet(0);
}

int installer_job_start(struct installer_job *job, installer_job_fn run,
                        void *context, const char *log)
{
    if (!job || !run) return -EINVAL;
    int descriptors[2];
    if (pipe(descriptors) < 0) return -errno;
    for (unsigned i = 0; i < 2; ++i) {
        if (fcntl(descriptors[i], F_SETFD, FD_CLOEXEC) < 0 ||
            fcntl(descriptors[i], F_SETFL, O_NONBLOCK) < 0) {
            int error = errno; close(descriptors[0]); close(descriptors[1]); return -error;
        }
    }
    fflush(NULL);
    pid_t child = fork();
    if (child < 0) { int error = errno; close(descriptors[0]); close(descriptors[1]); return -error; }
    if (!child) {
        close(descriptors[0]);
        progress_fd = descriptors[1];
        pending_offset = pending_size = 0;
        signal(SIGPIPE, SIG_IGN);
        /* Finish an already authorized disk transaction if the UI closes. */
        signal(SIGHUP, SIG_IGN); signal(SIGINT, SIG_IGN); signal(SIGTERM, SIG_IGN);
        int input = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (input < 0 || dup2(input, STDIN_FILENO) < 0) _exit(1);
        if (input != STDIN_FILENO) close(input);
        if (log) {
            int output = open(log, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
            struct stat info;
            if (output < 0 || fstat(output, &info) < 0 || !S_ISREG(info.st_mode) ||
                dup2(output, STDOUT_FILENO) < 0 || dup2(output, STDERR_FILENO) < 0) _exit(1);
            if (output > STDERR_FILENO) close(output);
        }
        struct installer_job_packet final = {0};
        final.result = run(context, &final);
        final.complete = 1;
        final.progress = final.result ? final.progress : 100;
        int error = flush_packet(1);
        if (!error) {
            pending = final; pending_size = sizeof(pending);
            error = flush_packet(1);
        }
        close(progress_fd);
        fflush(NULL);
        _exit(error ? 1 : 0);
    }
    close(descriptors[1]);
    memset(job, 0, sizeof(*job));
    job->pid = child;
    job->fd = descriptors[0];
    return 0;
}

int installer_job_step(struct installer_job *job)
{
    if (!job || job->fd < 0) return -EINVAL;
    size_t budget = sizeof(job->incoming) * 8;
    while (!job->eof && budget) {
        size_t size = sizeof(job->incoming) - job->received;
        if (size > budget) size = budget;
        ssize_t count = read(job->fd, (char *)&job->incoming + job->received, size);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (count <= 0) {
            if (count < 0) job->error = errno;
            job->eof = 1; break;
        }
        budget -= (size_t)count;
        job->received += (size_t)count;
        if (job->received != sizeof(job->incoming)) continue;
        if (job->complete || job->incoming.progress > 100) job->error = EPROTO;
        job->incoming.message[sizeof(job->incoming.message) - 1] = 0;
        job->incoming.detail[sizeof(job->incoming.detail) - 1] = 0;
        job->latest = job->incoming;
        if (job->incoming.complete == 1) {
            job->final = job->incoming;
            job->complete = 1;
        } else if (job->incoming.complete) job->error = EPROTO;
        job->received = 0;
    }
    if (!job->exited) {
        pid_t result = waitpid(job->pid, &job->status, WNOHANG);
        if (result == job->pid) { job->exited = 1; job->exited_ms = monotonic_ms(); }
        else if (result < 0 && errno != EINTR) {
            job->exited = 1; job->exited_ms = monotonic_ms(); job->error = errno;
        }
    }
    if (job->exited && !job->eof) {
        uint64_t now = monotonic_ms();
        if (now < job->exited_ms || now - job->exited_ms >= INSTALLER_JOB_FINISH_TIMEOUT_MS) {
            job->eof = 1;
            job->error = EPIPE;
        }
    }
    if (!job->exited || !job->eof) return 1;
    close(job->fd); job->fd = -1;
    if (job->error || job->received || !job->complete ||
        !WIFEXITED(job->status) || WEXITSTATUS(job->status))
        return job->error ? -job->error : -EIO;
    return 0;
}

int installer_job_request_stop(struct installer_job *job)
{
    if (!job || job->pid <= 0 || job->exited) return -EINVAL;
    return kill(job->pid, SIGTERM) < 0 ? -errno : 0;
}
