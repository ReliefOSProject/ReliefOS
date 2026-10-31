/* Private userspace acceptance runner: bounded execution and monotonic timing. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct sample { uint64_t elapsed; struct rusage usage; int status; };

static uint64_t now(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value)) { perror("clock_gettime"); exit(125); }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + value.tv_nsec;
}

static unsigned long long number(const char *value)
{
    char *end;
    errno = 0;
    unsigned long long n = strtoull(value, &end, 10);
    if (errno || !*value || *end || *value == '-') { fprintf(stderr, "invalid number\n"); exit(125); }
    return n;
}

static void redirect(int descriptor, const char *path)
{
    if (!strcmp(path, "-")) return;
    int flags = descriptor == STDIN_FILENO ? O_RDONLY : O_WRONLY | O_CREAT | O_TRUNC;
    int fd = open(path, flags, 0600);
    if (fd < 0 || dup2(fd, descriptor) < 0) { perror(path); _exit(125); }
    if (fd != descriptor) close(fd);
}

static struct sample execute(char **command, uint64_t timeout_ms,
                             const char *in, const char *out, const char *err)
{
    struct sample result = {0};
    uint64_t start = now();
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(125); }
    if (!pid) {
        if (setpgid(0, 0) < 0) { perror("setpgid"); _exit(125); }
        redirect(STDIN_FILENO, in); redirect(STDOUT_FILENO, out); redirect(STDERR_FILENO, err);
        execvp(command[0], command);
        perror(command[0]); _exit(127);
    }
    /* The child also sets its group, covering both sides of the fork race. */
    if (setpgid(pid, pid) < 0 && errno != EACCES && errno != ESRCH) {
        perror("parent setpgid");
    }
    int status, timed_out = 0;
    for (;;) {
        pid_t done = wait4(pid, &status, WNOHANG, &result.usage);
        if (done == pid) break;
        if (done < 0) {
            if (errno == EINTR) continue;
            perror("wait4"); exit(125);
        }
        if (!timed_out && now() - start >= timeout_ms * UINT64_C(1000000)) {
            timed_out = 1;
            /* Only this newly created command group belongs to the runner. */
            if (kill(-pid, SIGKILL) < 0 && errno != ESRCH) perror("kill command group");
            if (kill(pid, SIGKILL) < 0 && errno != ESRCH) perror("kill command");
        }
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    result.elapsed = now() - start;
    result.status = timed_out ? 124 : WIFEXITED(status) ? WEXITSTATUS(status) :
        WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 125;
    return result;
}

static uint64_t micros(struct timeval value)
{
    return (uint64_t)value.tv_sec * UINT64_C(1000000) + value.tv_usec;
}

static void token(const char *value)
{
    if (!*value || strspn(value, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != strlen(value)) {
        fprintf(stderr, "invalid measurement label\n"); exit(125);
    }
}

int main(int argc, char **argv)
{
    if (argc >= 8 && !strcmp(argv[1], "run") && !strcmp(argv[6], "--")) {
        uint64_t timeout = number(argv[2]);
        if (!timeout || timeout > 600000) return 125;
        return execute(argv + 7, timeout, argv[3], argv[4], argv[5]).status;
    }
    if (argc >= 11 && !strcmp(argv[1], "bench") && !strcmp(argv[9], "--")) {
        token(argv[2]); token(argv[3]);
        unsigned long long repeat = number(argv[4]), bytes = number(argv[5]);
        if (!repeat || repeat > 20) return 125;
        struct sample warm = execute(argv + 10, 120000, argv[6], argv[7], argv[8]);
        if (warm.status) { fprintf(stderr, "benchmark warmup failed: %d\n", warm.status); return warm.status; }
        for (unsigned long long i = 1; i <= repeat; ++i) {
            struct sample sample = execute(argv + 10, 120000, argv[6], argv[7], argv[8]);
            printf("CU_BENCH %s %s %llu %llu %" PRIu64 " %" PRIu64 " %" PRIu64 " %d\n",
                   argv[2], argv[3], i, bytes, sample.elapsed,
                   micros(sample.usage.ru_utime), micros(sample.usage.ru_stime), sample.status);
            fflush(stdout);
            if (sample.status) return sample.status;
        }
        return 0;
    }
    fprintf(stderr, "usage: probe run MS IN OUT ERR -- PROGRAM ...\n"
                    "       probe bench LABEL IMPL REPEAT BYTES IN OUT ERR -- PROGRAM ...\n");
    return 125;
}
