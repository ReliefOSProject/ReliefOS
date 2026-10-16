#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define POLL_ROUNDS 128U
#define OTHER_ROUNDS 32U
#define RUN_TIMEOUT_SECONDS 15U
#define REQUEST_MAGIC UINT32_C(0x70525131)
#define RESPONSE_MAGIC UINT32_C(0x70525331)
#define WORDS 8U

enum wait_mode {
    WAIT_POLL,
    WAIT_SELECT,
    WAIT_EPOLL,
};

enum affinity_mode {
    AFFINITY_NONE,
    AFFINITY_SAME_CPU0,
    AFFINITY_SPLIT_01,
};

struct message {
    uint32_t magic;
    uint32_t sequence;
    uint64_t words[WORDS];
};

struct waiter {
    enum wait_mode mode;
    int socket_fd;
    int epoll_fd;
};

static volatile sig_atomic_t run_timed_out;
static clockid_t timing_clock = CLOCK_MONOTONIC_RAW;

static void timeout_handler(int signal_number)
{
    (void)signal_number;
    run_timed_out = 1;
}

static uint64_t message_word(uint32_t sequence, unsigned index)
{
    uint64_t value = UINT64_C(0x9e3779b97f4a7c15) * (uint64_t)(sequence + 1U);
    value ^= UINT64_C(0xd6e8feb86659fd93) * (uint64_t)(index + 3U);
    value ^= value >> 29;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    return value ^ (value >> 31);
}

static void make_message(struct message *message, uint32_t magic, uint32_t sequence)
{
    message->magic = magic;
    message->sequence = sequence;
    for (unsigned i = 0; i < WORDS; ++i)
        message->words[i] = message_word(sequence, i);
}

static int validate_message(const struct message *message, uint32_t magic,
                            uint32_t sequence)
{
    if (message->magic != magic || message->sequence != sequence)
        return 0;
    for (unsigned i = 0; i < WORDS; ++i)
        if (message->words[i] != message_word(sequence, i))
            return 0;
    return 1;
}

static int write_full(int fd, const void *buffer, size_t size)
{
    const unsigned char *bytes = buffer;
    size_t offset = 0;
    while (offset < size) {
        ssize_t written = write(fd, bytes + offset, size - offset);
        if (written > 0) {
            offset += (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR && !run_timed_out)
            continue;
        if (written == 0)
            errno = EPIPE;
        return -1;
    }
    return 0;
}

static int read_full(int fd, void *buffer, size_t size)
{
    unsigned char *bytes = buffer;
    size_t offset = 0;
    while (offset < size) {
        ssize_t received = read(fd, bytes + offset, size - offset);
        if (received > 0) {
            offset += (size_t)received;
            continue;
        }
        if (received < 0 && errno == EINTR && !run_timed_out)
            continue;
        if (received == 0)
            errno = EPIPE;
        return -1;
    }
    return 0;
}

static int waiter_init(struct waiter *waiter, enum wait_mode mode, int fd)
{
    waiter->mode = mode;
    waiter->socket_fd = fd;
    waiter->epoll_fd = -1;
    if (mode != WAIT_EPOLL)
        return 0;

    waiter->epoll_fd = epoll_create1(0);
    if (waiter->epoll_fd < 0)
        return -1;
    struct epoll_event event = {.events = EPOLLIN | EPOLLERR | EPOLLHUP,
                                .data.fd = fd};
    if (epoll_ctl(waiter->epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
        int saved_errno = errno;
        close(waiter->epoll_fd);
        waiter->epoll_fd = -1;
        errno = saved_errno;
        return -1;
    }
    return 0;
}

static void waiter_destroy(struct waiter *waiter)
{
    if (waiter->epoll_fd >= 0)
        close(waiter->epoll_fd);
    waiter->epoll_fd = -1;
}

static int wait_readable(struct waiter *waiter)
{
    for (;;) {
        if (run_timed_out) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (waiter->mode == WAIT_POLL) {
            struct pollfd descriptor = {.fd = waiter->socket_fd, .events = POLLIN};
            int result = poll(&descriptor, 1, -1);
            if (result < 0 && errno == EINTR && !run_timed_out)
                continue;
            if (result < 0)
                return -1;
            if (result == 0)
                continue;
            if (descriptor.revents & POLLIN)
                return 0;
            errno = EPIPE;
            return -1;
        }
        if (waiter->mode == WAIT_SELECT) {
            if (waiter->socket_fd >= FD_SETSIZE) {
                errno = EOVERFLOW;
                return -1;
            }
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(waiter->socket_fd, &readable);
            int result = select(waiter->socket_fd + 1, &readable, NULL, NULL, NULL);
            if (result < 0 && errno == EINTR && !run_timed_out)
                continue;
            if (result < 0)
                return -1;
            if (result == 0)
                continue;
            if (FD_ISSET(waiter->socket_fd, &readable))
                return 0;
            errno = EPIPE;
            return -1;
        }

        struct epoll_event event;
        int result = epoll_wait(waiter->epoll_fd, &event, 1, -1);
        if (result < 0 && errno == EINTR && !run_timed_out)
            continue;
        if (result < 0)
            return -1;
        if (result == 0)
            continue;
        if (event.events & EPOLLIN)
            return 0;
        errno = EPIPE;
        return -1;
    }
}

static const char *mode_name(enum wait_mode mode)
{
    switch (mode) {
    case WAIT_POLL: return "poll";
    case WAIT_SELECT: return "select";
    case WAIT_EPOLL: return "epoll";
    }
    return "unknown";
}

static const char *affinity_name(enum affinity_mode affinity)
{
    switch (affinity) {
    case AFFINITY_NONE: return "none";
    case AFFINITY_SAME_CPU0: return "same-cpu0";
    case AFFINITY_SPLIT_01: return "cpu0-cpu1";
    }
    return "unknown";
}

static int pin_cpu(unsigned cpu)
{
    if (cpu >= CPU_SETSIZE) {
        errno = EINVAL;
        return -1;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set);
}

static void child_respond(int fd, enum wait_mode mode, unsigned rounds)
{
    struct sigaction default_alarm = {.sa_handler = SIG_DFL};
    sigemptyset(&default_alarm.sa_mask);
    sigaction(SIGALRM, &default_alarm, NULL);
    signal(SIGPIPE, SIG_IGN);

    struct waiter waiter;
    if (waiter_init(&waiter, mode, fd) < 0)
        _exit(20);

    const char ready = 'R';
    if (write_full(fd, &ready, sizeof(ready)) < 0)
        _exit(21);

    for (unsigned sequence = 0; sequence < rounds; ++sequence) {
        struct message request;
        struct message response;
        if (wait_readable(&waiter) < 0 || read_full(fd, &request, sizeof(request)) < 0)
            _exit(22);
        if (!validate_message(&request, REQUEST_MAGIC, sequence))
            _exit(23);
        make_message(&response, RESPONSE_MAGIC, sequence);
        if (write_full(fd, &response, sizeof(response)) < 0)
            _exit(24);
    }
    waiter_destroy(&waiter);
    close(fd);
    _exit(0);
}

static int now_ns(uint64_t *value)
{
    struct timespec time;
    if (clock_gettime(timing_clock, &time) < 0)
        return -1;
    *value = (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec;
    return 0;
}

static int run_mode(enum wait_mode mode, unsigned rounds,
                    enum affinity_mode affinity)
{
    if (mode == WAIT_EPOLL) {
        int probe_fd = epoll_create1(0);
        if (probe_fd < 0 && (errno == ENOSYS || errno == EINVAL)) {
            printf("[poll-roundtrip] mode=epoll skipped=unsupported\n");
            return 0;
        }
        if (probe_fd < 0) {
            perror("epoll_create1 capability check");
            return -1;
        }
        close(probe_fd);
    }

    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0) {
        perror("socketpair(AF_UNIX, SOCK_STREAM)");
        return -1;
    }
    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }
    if (child == 0) {
        close(sockets[0]);
        if (affinity == AFFINITY_SPLIT_01 && pin_cpu(1) < 0)
            _exit(25);
        child_respond(sockets[1], mode, rounds);
    }

    close(sockets[1]);
    if (affinity == AFFINITY_SAME_CPU0 && pin_cpu(0) < 0) {
        perror("sched_setaffinity(cpu0)");
        goto fail_child;
    }
    if (affinity == AFFINITY_SPLIT_01 && pin_cpu(0) < 0) {
        perror("sched_setaffinity(parent cpu0)");
        goto fail_child;
    }

    struct sigaction alarm_action = {.sa_handler = timeout_handler};
    sigemptyset(&alarm_action.sa_mask);
    alarm_action.sa_flags = 0;
    if (sigaction(SIGALRM, &alarm_action, NULL) < 0) {
        perror("sigaction(SIGALRM)");
        goto fail_child;
    }
    signal(SIGPIPE, SIG_IGN);
    run_timed_out = 0;
    alarm(RUN_TIMEOUT_SECONDS);

    struct waiter waiter;
    if (waiter_init(&waiter, mode, sockets[0]) < 0) {
        perror("waiter_init");
        goto fail_alarm;
    }
    char ready = 0;
    if (read_full(sockets[0], &ready, sizeof(ready)) < 0 || ready != 'R') {
        if (run_timed_out)
            fprintf(stderr, "[poll-roundtrip] mode=%s timed out awaiting child readiness\n",
                    mode_name(mode));
        else
            fprintf(stderr, "[poll-roundtrip] mode=%s invalid child readiness\n",
                    mode_name(mode));
        waiter_destroy(&waiter);
        goto fail_alarm;
    }

    uint64_t start_ns, finish_ns;
    if (now_ns(&start_ns) < 0) {
        perror("clock_gettime(start)");
        waiter_destroy(&waiter);
        goto fail_alarm;
    }
    for (unsigned sequence = 0; sequence < rounds; ++sequence) {
        struct message request;
        struct message response;
        make_message(&request, REQUEST_MAGIC, sequence);
        if (write_full(sockets[0], &request, sizeof(request)) < 0 ||
            wait_readable(&waiter) < 0 ||
            read_full(sockets[0], &response, sizeof(response)) < 0) {
            if (run_timed_out)
                fprintf(stderr, "[poll-roundtrip] mode=%s timed out at round=%u/%u\n",
                        mode_name(mode), sequence, rounds);
            else
                fprintf(stderr, "[poll-roundtrip] mode=%s I/O failed at round=%u/%u: %s\n",
                        mode_name(mode), sequence, rounds, strerror(errno));
            waiter_destroy(&waiter);
            goto fail_alarm;
        }
        if (!validate_message(&response, RESPONSE_MAGIC, sequence)) {
            fprintf(stderr, "[poll-roundtrip] mode=%s corrupt response at round=%u\n",
                    mode_name(mode), sequence);
            waiter_destroy(&waiter);
            goto fail_alarm;
        }
    }
    if (now_ns(&finish_ns) < 0) {
        perror("clock_gettime(finish)");
        waiter_destroy(&waiter);
        goto fail_alarm;
    }
    waiter_destroy(&waiter);

    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR && !run_timed_out)
            continue;
        if (errno == EINTR && run_timed_out)
            break;
        perror("waitpid(child)");
        close(sockets[0]);
        alarm(0);
        kill(child, SIGKILL);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
        return -1;
    }
    close(sockets[0]);
    if (run_timed_out) {
        fprintf(stderr, "[poll-roundtrip] mode=%s timed out waiting for child exit\n",
                mode_name(mode));
        kill(child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        return -1;
    }
    alarm(0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "[poll-roundtrip] mode=%s child failed (status=0x%x)\n",
                mode_name(mode), status);
        return -1;
    }

    printf("[poll-roundtrip] mode=%s transport=AF_UNIX/SOCK_STREAM rounds=%u "
           "elapsed_ms=%.3f us_per_roundtrip=%.3f affinity=%s timer=%s\n",
           mode_name(mode), rounds,
           (double)(finish_ns - start_ns) / 1000000.0,
           (double)(finish_ns - start_ns) / (double)rounds / 1000.0,
           affinity_name(affinity),
           timing_clock == CLOCK_MONOTONIC_RAW ? "MONOTONIC_RAW" : "MONOTONIC");
    fflush(stdout);
    return 0;

fail_alarm:
    alarm(0);
fail_child:
    close(sockets[0]);
    kill(child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
    return -1;
}

static void usage(const char *program)
{
    fprintf(stderr, "usage: %s [--pin-same | --pin-split]\n"
                    "  poll: 128 measured AF_UNIX request/reply round trips\n"
                    "  select/epoll: 32 each when supported\n",
            program);
}

int main(int argc, char **argv)
{
    enum affinity_mode affinity = AFFINITY_NONE;
    if (argc == 2 && strcmp(argv[1], "--pin-same") == 0)
        affinity = AFFINITY_SAME_CPU0;
    else if (argc == 2 && strcmp(argv[1], "--pin-split") == 0)
        affinity = AFFINITY_SPLIT_01;
    else if (argc != 1) {
        usage(argv[0]);
        return 2;
    }

    struct timespec check;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &check) < 0) {
        if (clock_gettime(CLOCK_MONOTONIC, &check) < 0) {
            perror("clock_gettime");
            return 1;
        }
        timing_clock = CLOCK_MONOTONIC;
    }

    signal(SIGPIPE, SIG_IGN);
    printf("[poll-roundtrip] start affinity=%s rounds=poll:%u/select:%u/epoll:%u "
           "timer=%s timing_is_diagnostic_only=1\n",
           affinity_name(affinity), POLL_ROUNDS, OTHER_ROUNDS, OTHER_ROUNDS,
           timing_clock == CLOCK_MONOTONIC_RAW ? "MONOTONIC_RAW" : "MONOTONIC");
    fflush(stdout);

    if (run_mode(WAIT_POLL, POLL_ROUNDS, affinity) < 0 ||
        run_mode(WAIT_SELECT, OTHER_ROUNDS, affinity) < 0 ||
        run_mode(WAIT_EPOLL, OTHER_ROUNDS, affinity) < 0)
        return 1;

    puts("[poll-roundtrip] PASS socketpair protocol integrity; no timing threshold asserted");
    return 0;
}
