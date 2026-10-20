#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define WORKING_PAGES 6000u
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "[file-cache] FAIL line=%d errno=%d: %s\n", \
    __LINE__, errno, #c); exit(1); } } while (0)

static void mapping_fault(int signal)
{
    (void)signal;
    static const char message[] = "[file-cache] FAIL valid shared mapping faulted\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(1);
}

static void check_pages(const volatile unsigned char *bytes, size_t page_size)
{
    for (unsigned i = 0; i < WORKING_PAGES; ++i)
        CHECK(bytes[i * page_size] == (unsigned char)(i % 251));
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    struct sigaction action = {.sa_handler = mapping_fault};
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGSEGV, &action, NULL) == 0);
    CHECK(sigaction(SIGBUS, &action, NULL) == 0);
    long page_size = sysconf(_SC_PAGESIZE);
    CHECK(page_size > 0);
    unsigned char *buffer = malloc((size_t)page_size);
    CHECK(buffer);
    char path[] = "/tmp/reliefos-file-cache-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    for (unsigned i = 0; i < WORKING_PAGES; ++i) {
        memset(buffer, (int)(i % 251), (size_t)page_size);
        CHECK(write(fd, buffer, (size_t)page_size) == page_size);
    }
    free(buffer);
    size_t length = (size_t)page_size * WORKING_PAGES;
    volatile unsigned char *first = mmap(NULL, length, PROT_READ, MAP_SHARED, fd, 0);
    volatile unsigned char *second = mmap(NULL, length, PROT_READ, MAP_SHARED, fd, 0);
    CHECK(first != MAP_FAILED && second != MAP_FAILED && first != second);
    puts("[file-cache] touching 6000 distinct shared file pages");
    check_pages(first, (size_t)page_size);
    check_pages(second, (size_t)page_size);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        check_pages(first, (size_t)page_size);
        CHECK(munmap((void *)first, length) == 0);
        check_pages(second, (size_t)page_size);
        _exit(0);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(munmap((void *)first, length) == 0);
    check_pages(second, (size_t)page_size);
    CHECK(munmap((void *)second, length) == 0);
    CHECK(close(fd) == 0 && unlink(path) == 0);
    puts("[file-cache] DONE failures=0 (large shared file, aliases, fork, unmap and contents)");
    return 0;
}
