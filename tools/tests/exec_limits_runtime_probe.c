/* Run the same execve boundary cases on Linux and ReliefNT. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *self;
static unsigned failures;

static void run_case(const char *label, unsigned argc, unsigned envc, size_t envlen,
                     size_t arglen, rlim_t stack, int expected_errno)
{
    char **args = calloc((size_t)argc + 1, sizeof(*args));
    char **env = calloc((size_t)envc + 1, sizeof(*env));
    char *arg = malloc(arglen + 1), *entry = malloc(envlen + 1);
    if (!args || !env || !arg || !entry) exit(90);
    memset(arg, 'a', arglen); arg[arglen] = 0;
    memset(entry, 'e', envlen); entry[envlen] = 0;
    if (envlen > 1) { entry[0] = 'E'; entry[1] = '='; }
    char nenv[32], nlen[32], nargs[32], alen[32];
    snprintf(nenv, sizeof(nenv), "%u", envc);
    snprintf(nlen, sizeof(nlen), "%zu", envlen);
    snprintf(nargs, sizeof(nargs), "%u", argc);
    snprintf(alen, sizeof(alen), "%zu", arglen);
    args[0] = (char *)self; args[1] = expected_errno ? "--unexpected-exec" : "--child";
    args[2] = nenv; args[3] = nlen; args[4] = nargs; args[5] = alen;
    for (unsigned i = 6; i < argc; ++i) args[i] = arg;
    for (unsigned i = 0; i < envc; ++i) env[i] = entry;
    fflush(NULL);
    pid_t pid = fork();
    if (!pid) {
        struct rlimit limit;
        if (getrlimit(RLIMIT_STACK, &limit)) _exit(91);
        limit.rlim_cur = stack;
        if (setrlimit(RLIMIT_STACK, &limit)) _exit(92);
        int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (fd < 0) _exit(93);
        syscall(SYS_execve, self, args, env);
        int error = errno;
        if (!expected_errno || error != expected_errno)
            dprintf(STDERR_FILENO, "[exec-limits] unexpected exec error=%d expected=%d (%s)\n",
                    error, expected_errno, label);
        /* A rejected exec must leave the old image and CLOEXEC descriptor intact. */
        _exit(expected_errno && error == expected_errno &&
              fcntl(fd, F_GETFD) == FD_CLOEXEC ? 0 : 94);
    }
    int status = 0;
    int ok = pid > 0 && waitpid(pid, &status, 0) == pid &&
             WIFEXITED(status) && WEXITSTATUS(status) == 0;
    printf("[exec-limits] %s %s status=%d\n", ok ? "PASS" : "FAIL", label, status);
    failures += !ok;
    free(args); free(env); free(arg); free(entry);
}

int main(int argc, char **argv, char **envp)
{
    if (argc > 1 && !strcmp(argv[1], "--unexpected-exec")) return 74;
    if (argc > 1 && !strcmp(argv[1], "--child")) {
        if (argc < 6 || argc != atoi(argv[4])) return 70;
        unsigned envc = (unsigned)atoi(argv[2]), count = 0;
        size_t len = (size_t)strtoul(argv[3], NULL, 10);
        while (envp[count]) {
            if (strlen(envp[count]) != len || envp[count][0] != 'E') return 71;
            ++count;
        }
        if (count != envc) return 72;
        for (int i = 6; i < argc; ++i)
            if (strlen(argv[i]) != strtoul(argv[5], NULL, 10)) return 73;
        return 0;
    }
    self = argv[0];
    if (!self || self[0] != '/') { fputs("absolute argv[0] required\n", stderr); return 2; }
    run_case("491 build environment entries", 7, 491, 96, 1, 8u<<20, 0);
    run_case("2048 environment entries and expanded initial stack", 7, 2048, 96, 1, 8u<<20, 0);
    run_case("4096 arguments", 4096, 1, 3, 3, 8u<<20, 0);
    run_case("20000 environment pointers", 7, 20000, 3, 1, 8u<<20, 0);
    run_case("single argument at 128 KiB including NUL", 7, 0, 0, 131071, 8u<<20, 0);
    run_case("single argument exceeds 128 KiB", 7, 0, 0, 131072, 8u<<20, E2BIG);
    run_case("single environment entry at 128 KiB", 6, 1, 131071, 0, 8u<<20, 0);
    run_case("single environment entry exceeds 128 KiB", 6, 1, 131072, 0, 8u<<20, E2BIG);
    run_case("pointer words count against the 128 KiB floor", 7, 17000, 3, 1, 512u<<10, E2BIG);
    run_case("small RLIMIT_STACK still bounds initial stack pages", 7, 1, 80000, 1, 64u<<10, E2BIG);
    run_case("default 2 MiB aggregate budget", 7, 40, 65535, 1, 8u<<20, E2BIG);
    run_case("1 MiB stack accepts 192 KiB", 7, 3, 65535, 1, 1u<<20, 0);
    run_case("1 MiB stack rejects 320 KiB", 7, 5, 65535, 1, 1u<<20, E2BIG);
    run_case("infinite stack accepts more than 2 MiB", 7, 40, 65535, 1, RLIM_INFINITY, 0);
    run_case("infinite stack retains 6 MiB ceiling", 7, 97, 65535, 1, RLIM_INFINITY, E2BIG);
    printf("EXEC_LIMITS_DONE failures=%u\n", failures);
    return failures ? 1 : 0;
}
