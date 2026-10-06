/* Run unchanged against the Linux ABI on both Linux and ReliefOS. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "[mprotect-probe] FAIL line=%d: %s errno=%d\n", \
            __LINE__, #condition, errno); return 1; } } while (0)

static _Thread_local unsigned tls = 73;
static void *worker(void *argument)
{
    if (tls != 73) return (void *)1;
    tls = 42;
    return argument;
}

static int denied(volatile char *address, int write_access)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (write_access) *address = 99;
        else { volatile char value = *address; (void)value; }
        _exit(99);
    }
    int status;
    return pid > 0 && waitpid(pid, &status, 0) == pid &&
           WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
}

int main(void)
{
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    char *fillers[300];
    /* Separated mappings ensure the VMA store exceeds the fixed 128 entries. */
    for (unsigned i = 0; i < 300; ++i) {
        fillers[i] = mmap(NULL, 2 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(fillers[i] != MAP_FAILED);
        CHECK(mprotect(fillers[i] + page, page, PROT_READ) == 0);
    }
    char *stack = mmap(NULL, 5 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(stack != MAP_FAILED);
    CHECK(mprotect(stack + page, 3 * page - 1, PROT_READ | PROT_WRITE) == 0);
    CHECK(stack[page] == 0 && stack[4 * page - 1] == 0);
    stack[page] = 42;
    stack[4 * page - 1] = 17;
    CHECK(denied(stack, 0) && denied(stack + 4 * page, 1));
    CHECK(mprotect(stack + page, 3 * page, PROT_READ) == 0);
    CHECK(stack[page] == 42 && denied(stack + page, 1));
    CHECK(mprotect(stack + page, 3 * page, PROT_NONE) == 0);
    CHECK(denied(stack + page, 0));
    CHECK(mprotect(stack + page, 3 * page, PROT_READ | PROT_WRITE) == 0);
    CHECK(stack[page] == 42 && stack[4 * page - 1] == 17);
    stack[page] = 43;
    errno = 0;
    /* musl's wrapper rounds addr down; check the raw Linux ABI alignment. */
    CHECK(syscall(SYS_mprotect, stack + 1, page, PROT_READ) == -1 && errno == EINVAL);
    CHECK(mprotect(NULL, page, PROT_READ) == -1 && errno == ENOMEM);
    pthread_t thread;
    void *result = NULL;
    CHECK(pthread_create(&thread, NULL, worker, (void *)73) == 0);
    CHECK(pthread_join(thread, &result) == 0 && result == (void *)73 && tls == 73);
    CHECK(munmap(stack, 5 * page) == 0);
    for (unsigned i = 0; i < 300; ++i) CHECK(munmap(fillers[i], 2 * page) == 0);
    puts("[mprotect-probe] PASS VMA growth, partial protection, guards, rounding, data preservation, errno and pthread TLS");
    return 0;
}
