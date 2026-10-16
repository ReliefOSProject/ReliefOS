#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct counters { volatile uint64_t work[3]; volatile int stop; };

static void pin(unsigned cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set); CPU_SET(cpu, &set);
    assert(sched_setaffinity(0, sizeof(set), &set) == 0);
    CPU_ZERO(&set);
    assert(sched_getaffinity(0, sizeof(set), &set) == 0 && CPU_ISSET(cpu, &set));
}

static void compete(int nice_second)
{
    struct counters *c = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    assert(c != MAP_FAILED);
    /* Materialize the shared backing before fork in this kernel's lazy VM. */
    c->work[0] = c->work[1] = 0;
    c->stop = 0;
    int gate[2]; assert(pipe(gate) == 0);
    pid_t children[2];
    for (unsigned i = 0; i < 2; ++i) {
        children[i] = fork(); assert(children[i] >= 0);
        if (!children[i]) {
            close(gate[1]);
            pin(0);
            assert(setpriority(PRIO_PROCESS, 0, i ? nice_second : 0) == 0);
            char start; assert(read(gate[0], &start, 1) == 1);
            close(gate[0]);
            uint64_t value = 1;
            while (!__atomic_load_n(&c->stop, __ATOMIC_ACQUIRE)) {
                for (unsigned j = 0; j < 1024; ++j) {
                    value = value * 6364136223846793005ULL + 1;
                    __asm__ volatile("" : "+r"(value));
                }
                ++c->work[i];
            }
            _exit(0);
        }
    }
    close(gate[0]);
    assert(write(gate[1], "go", 2) == 2); close(gate[1]);
    struct timespec delay = {.tv_sec = 4};
    while (nanosleep(&delay, &delay) && errno == EINTR) {}
    __atomic_store_n(&c->stop, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < 2; ++i) {
        int status;
        assert(waitpid(children[i], &status, 0) == children[i]);
        assert(WIFEXITED(status) && !WEXITSTATUS(status));
    }
    assert(c->work[0] && c->work[1]);
    double ratio = (double)c->work[0] / c->work[1];
    printf("[eevdf-probe] nice=0:%d work=%llu:%llu ratio=%.3f\n", nice_second,
           (unsigned long long)c->work[0], (unsigned long long)c->work[1], ratio);
    fflush(stdout);
    assert(nice_second ? (ratio > 1.8 && ratio < 4.8) : (ratio > 0.65 && ratio < 1.5));
    assert(munmap(c, 4096) == 0);
}

/* Keep CPU0's BSP tick path active while two CPU-bound children contend on
 * CPU1. The AP's local tick must keep requesting its own scheduler round even
 * after BSP scheduling has consumed CPU0's request. */
static void compete_ap(void)
{
    struct counters *c = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    assert(c != MAP_FAILED);
    c->work[0] = c->work[1] = c->work[2] = 0;
    c->stop = 0;
    int gate[2]; assert(pipe(gate) == 0);
    pid_t children[3];
    for (unsigned i = 0; i < 3; ++i) {
        children[i] = fork(); assert(children[i] >= 0);
        if (!children[i]) {
            close(gate[1]);
            pin(i ? 1 : 0);
            char start; assert(read(gate[0], &start, 1) == 1);
            close(gate[0]);
            uint64_t value = i + 1;
            while (!__atomic_load_n(&c->stop, __ATOMIC_ACQUIRE)) {
                for (unsigned j = 0; j < 1024; ++j) {
                    value = value * 6364136223846793005ULL + 1;
                    __asm__ volatile("" : "+r"(value));
                }
                ++c->work[i];
            }
            _exit(0);
        }
    }
    close(gate[0]);
    assert(write(gate[1], "go!", 3) == 3); close(gate[1]);
    struct timespec delay = {.tv_sec = 4};
    while (nanosleep(&delay, &delay) && errno == EINTR) {}
    __atomic_store_n(&c->stop, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < 3; ++i) {
        int status;
        assert(waitpid(children[i], &status, 0) == children[i]);
        assert(WIFEXITED(status) && !WEXITSTATUS(status));
    }
    assert(c->work[0] && c->work[1] && c->work[2]);
    printf("[eevdf-probe] BSP work=%llu AP contention=%llu:%llu\n",
           (unsigned long long)c->work[0],
           (unsigned long long)c->work[1], (unsigned long long)c->work[2]);
    fflush(stdout);
    assert(munmap(c, 4096) == 0);
}

int main(void)
{
    cpu_set_t available;
    CPU_ZERO(&available);
    assert(sched_getaffinity(0, sizeof(available), &available) == 0);
    unsigned cpus[64], count = 0;
    for (unsigned i = 0; i < 64; ++i) if (CPU_ISSET(i, &available)) cpus[count++] = i;
    assert(count);
    printf("[eevdf-probe] active_cpus=%u\n", count);
    assert(count >= 2 && CPU_ISSET(0, &available) && CPU_ISSET(1, &available));
    compete(0);
    compete(5);
    compete_ap();
    for (unsigned round = 0; round < 16; ++round) {
        pid_t child = fork(); assert(child >= 0);
        if (!child) {
            pin(cpus[round % count]);
            for (unsigned i = 0; i < 50; ++i) assert(sched_yield() == 0);
            _exit(0);
        }
        int status;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && !WEXITSTATUS(status));
    }
    puts("[apk-probe] DONE failures=0 (EEVDF fairness, nice, affinity, fork/yield/wait)");
    return 0;
}
