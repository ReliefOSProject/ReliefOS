/* POSIX process/time probe; Linux affinity pins two compute tasks per vCPU. */
#define _GNU_SOURCE 1
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_CPUS 8
struct sample { uint64_t total[MAX_CPUS], busy[MAX_CPUS], irq[MAX_CPUS]; unsigned cpus; int has_irq; };
struct worker_result { unsigned cpu; uint64_t work, ticks, usec; };
static volatile sig_atomic_t stop_worker;
static void stop_handler(int signo) { (void)signo; stop_worker = 1; }
static void fail(const char *message) { perror(message); exit(1); }
static void read_exact(int fd, void *buffer, size_t bytes)
{
    char *p = buffer;
    while (bytes) {
        ssize_t n = read(fd, p, bytes);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) fail("pipe read");
        p += n; bytes -= (size_t)n;
    }
}
static struct sample snapshot(void)
{
    struct sample result = {0};
    FILE *file = fopen("/proc/stat", "r");
    if (!file) fail("/proc/stat");
    char line[2048];
    uint64_t aggregate = 0, aggregate_busy = 0;
    while (fgets(line, sizeof(line), file)) {
        char name[32]; unsigned long long v[10] = {0};
        int n = sscanf(line, "%31s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
            name, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
        if (n >= 2 && !strcmp(name, "timer_events")) {
            result.has_irq = 1;
            for (unsigned i = 0; i < MAX_CPUS; ++i) result.irq[i] = v[i];
            continue;
        }
        if (n < 5) continue;
        uint64_t total = 0;
        for (unsigned i = 0; i < 8; ++i) total += v[i];
        if (!strcmp(name, "cpu")) { aggregate = total; aggregate_busy = total - v[3]; }
        else if (!strncmp(name, "cpu", 3)) {
            unsigned cpu = (unsigned)strtoul(name + 3, NULL, 10);
            if (cpu >= MAX_CPUS) fail("too many CPUs");
            result.total[cpu] = total; result.busy[cpu] = total - v[3];
            if (result.cpus <= cpu) result.cpus = cpu + 1;
        }
    }
    fclose(file);
    uint64_t sum = 0, busy = 0;
    for (unsigned i = 0; i < result.cpus; ++i) { sum += result.total[i]; busy += result.busy[i]; }
    if (!result.cpus || sum != aggregate || busy != aggregate_busy) {
        fprintf(stderr, "[smp-time] FAIL incoherent aggregate\n"); exit(1);
    }
    return result;
}
static void worker(unsigned cpu, int ready, int report)
{
    cpu_set_t affinity; CPU_ZERO(&affinity); CPU_SET(cpu, &affinity);
    if (sched_setaffinity(0, sizeof(affinity), &affinity)) _exit(10);
    struct sigaction action = {0}; action.sa_handler = stop_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL)) _exit(11);
    char token = 'R';
    if (write(ready, &token, 1) != 1) _exit(12);
    volatile uint64_t work = 1;
    while (!stop_worker) {
        /* The loop makes no syscalls; both pinned workers require preemption. */
        ++work;
    }
    struct tms usage; struct rusage resource;
    if (times(&usage) == (clock_t)-1 || getrusage(RUSAGE_SELF, &resource)) _exit(13);
    struct worker_result result = {.cpu = cpu, .work = work,
        .ticks = (uint64_t)(usage.tms_utime + usage.tms_stime),
        .usec = (uint64_t)resource.ru_utime.tv_sec * 1000000 + resource.ru_utime.tv_usec +
                (uint64_t)resource.ru_stime.tv_sec * 1000000 + resource.ru_stime.tv_usec};
    if (write(report, &result, sizeof(result)) != sizeof(result)) _exit(14);
    _exit(0);
}
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    long hz = sysconf(_SC_CLK_TCK);
    if (hz != 100) { fprintf(stderr, "[smp-time] FAIL USER_HZ=%ld\n", hz); return 1; }
    struct sample initial = snapshot();
    unsigned workers = initial.cpus * 2;
    pid_t pids[MAX_CPUS * 2]; int ready[2], report[2];
    if (pipe(ready) || pipe(report)) fail("pipe");
    printf("[smp-time] BEGIN cpus=%u USER_HZ=%ld workers=%u\n", initial.cpus, hz, workers);
    for (unsigned i = 0; i < workers; ++i) {
        pids[i] = fork();
        if (pids[i] < 0) fail("fork");
        if (!pids[i]) worker(i / 2, ready[1], report[1]);
    }
    close(ready[1]); close(report[1]);
    char tokens[MAX_CPUS * 2]; read_exact(ready[0], tokens, workers); close(ready[0]);
    struct timespec start, end, duration = {.tv_sec = 2};
    clock_gettime(CLOCK_MONOTONIC, &start);
    struct sample before = snapshot();
    while (nanosleep(&duration, &duration) && errno == EINTR) {}
    struct sample after = snapshot();
    clock_gettime(CLOCK_MONOTONIC, &end);
    uint64_t wall_us = (uint64_t)(end.tv_sec - start.tv_sec) * 1000000 +
        (int64_t)(end.tv_nsec - start.tv_nsec) / 1000;
    unsigned failures = 0;
    for (unsigned cpu = 0; cpu < before.cpus; ++cpu) {
        uint64_t total = after.total[cpu] - before.total[cpu];
        uint64_t busy = after.busy[cpu] - before.busy[cpu];
        printf("[smp-time] CPU cpu=%u total=%llu busy=%llu wall_us=%llu",
            cpu, (unsigned long long)total, (unsigned long long)busy, (unsigned long long)wall_us);
        uint64_t expected = wall_us / 10000;
        if (total + 5 < expected || total > expected + 5 || busy * 100 < total * 65) ++failures;
        if (before.has_irq && after.has_irq) {
            uint64_t irqs = after.irq[cpu] - before.irq[cpu];
            printf(" raw_timer_events=%llu", (unsigned long long)irqs);
            if (irqs * 100 < expected * 85 || irqs * 100 > expected * 115) ++failures;
        }
        putchar('\n');
    }
    for (unsigned i = 0; i < workers; ++i) if (kill(pids[i], SIGUSR1)) fail("kill worker");
    for (unsigned i = 0; i < workers; ++i) {
        struct worker_result result; read_exact(report[0], &result, sizeof(result));
        uint64_t converted = result.ticks * 10000;
        uint64_t difference = result.usec > converted ? result.usec - converted : converted - result.usec;
        printf("[smp-time] WORKER cpu=%u work=%llu ticks=%llu rusage_us=%llu\n", result.cpu,
            (unsigned long long)result.work, (unsigned long long)result.ticks, (unsigned long long)result.usec);
        if (result.work < 100 || result.ticks < 5 || difference > 10000) ++failures;
    }
    close(report[0]);
    for (unsigned i = 0; i < workers; ++i) {
        int status;
        if (waitpid(pids[i], &status, 0) != pids[i] || !WIFEXITED(status) || WEXITSTATUS(status)) ++failures;
    }
    printf("[smp-time] DONE failures=%u raw_irq_probe=%d\n", failures, before.has_irq && after.has_irq);
    return failures ? 1 : 0;
}
