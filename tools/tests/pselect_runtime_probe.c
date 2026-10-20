#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "[pselect] FAIL line=%d errno=%d: %s\n", \
    __LINE__, errno, #c); exit(1); } } while (0)

static volatile sig_atomic_t caught;
static void handler(int signal) { caught = signal; }

static void check_restored(void)
{
    sigset_t current;
    CHECK(sigprocmask(SIG_SETMASK, NULL, &current) == 0);
    CHECK(sigismember(&current, SIGUSR1) == 1);
}

/* GNU make's jobserver waits on an empty pipe while temporarily unblocking
 * SIGCHLD. No timeout or pipe data may be needed to notice a child exit. */
static void check_child_exit_interrupts_parked_descriptor(void)
{
    sigset_t blocked, original, empty, current;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGCHLD);
    sigemptyset(&empty);
    CHECK(sigprocmask(SIG_BLOCK, &blocked, &original) == 0);
    struct sigaction action = {.sa_handler = handler, .sa_flags = SA_RESTART};
    struct sigaction old_child, old_alarm;
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGCHLD, &action, &old_child) == 0);
    CHECK(sigaction(SIGALRM, &action, &old_alarm) == 0);
    int descriptors[2];
    CHECK(pipe(descriptors) == 0);
    caught = 0;
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        close(descriptors[0]);
        close(descriptors[1]);
        usleep(50000);
        _exit(0);
    }
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(descriptors[0], &readable);
    struct timespec start, end;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    alarm(2); /* Failure fuse; a correct return is caused by SIGCHLD. */
    int result = pselect(descriptors[0] + 1, &readable, NULL, NULL, NULL, &empty);
    int saved_errno = errno;
    int observed = caught;
    alarm(0);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    int64_t elapsed_ns = (end.tv_sec - start.tv_sec) * INT64_C(1000000000) +
                         end.tv_nsec - start.tv_nsec;
    CHECK(sigprocmask(SIG_SETMASK, NULL, &current) == 0);
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    close(descriptors[0]);
    close(descriptors[1]);
    CHECK(result == -1 && saved_errno == EINTR && observed == SIGCHLD);
    CHECK(elapsed_ns >= 0 && elapsed_ns < INT64_C(1500000000));
    CHECK(sigismember(&current, SIGCHLD) == 1);
    CHECK(sigprocmask(SIG_SETMASK, &original, NULL) == 0);
    CHECK(sigaction(SIGCHLD, &old_child, NULL) == 0);
    CHECK(sigaction(SIGALRM, &old_alarm, NULL) == 0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    sigset_t blocked, empty;
    sigemptyset(&empty);
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    CHECK(sigprocmask(SIG_SETMASK, &blocked, NULL) == 0);
    struct sigaction action = {.sa_handler = handler, .sa_flags = SA_RESTART};
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGUSR1, &action, NULL) == 0);
    struct timespec timeout = {0, 20000000};
    CHECK(pselect(0, NULL, NULL, NULL, &timeout, &empty) == 0);
    check_restored();
    CHECK(raise(SIGUSR1) == 0 && !caught);
    timeout = (struct timespec){1, 0};
    CHECK(pselect(0, NULL, NULL, NULL, &timeout, &empty) == -1 && errno == EINTR);
    CHECK(caught == SIGUSR1);
    check_restored();

    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        usleep(50000);
        _exit(write(pipefd[1], "x", 1) == 1 ? 0 : 1);
    }
    fd_set readset;
    FD_ZERO(&readset);
    FD_SET(pipefd[0], &readset);
    CHECK(pselect(pipefd[0] + 1, &readset, NULL, NULL, &timeout, &empty) == 1);
    CHECK(FD_ISSET(pipefd[0], &readset));
    check_restored();
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    close(pipefd[0]);
    close(pipefd[1]);
    FD_ZERO(&readset);
    FD_SET(pipefd[0], &readset);
    CHECK(pselect(pipefd[0] + 1, &readset, NULL, NULL, &timeout, &empty) == -1 && errno == EBADF);
    check_restored();
    check_child_exit_interrupts_parked_descriptor();
    puts("[pselect] DONE failures=0 (timeout, SA_RESTART interruption, pipe readiness, error restoration)");
    return 0;
}
