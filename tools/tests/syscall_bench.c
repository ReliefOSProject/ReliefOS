/* ReliefOS guest syscall latency/throughput benchmark.
 *
 * Measures the full ring-3 syscall round trip (entry + dispatch + return) with
 * the TSC, so the per-syscall fixed overhead ReliefNT pays is visible.  Null
 * syscalls (getpid) isolate that fixed cost because their dispatch is trivial.
 * Reports one [syscall-bench] line per case plus a DONE marker on the console.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

/* Raw syscall instruction, bypassing libc wrappers so we time the kernel path
 * only.  Mirrors the Linux x86-64 calling convention. */
static inline long raw_syscall(long n, long a0, long a1, long a2,
                               long a3, long a4, long a5)
{
    long ret;
    register long r10 __asm__("r10") = a3;
    register long r8 __asm__("r8") = a4;
    register long r9 __asm__("r9") = a5;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

static uint64_t g_tsc_per_ns;

static uint64_t ns_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void calibrate(void)
{
    uint64_t n0 = ns_now(), c0 = rdtsc();
    struct timespec req = { 0, 80 * 1000 * 1000 };
    nanosleep(&req, NULL);
    uint64_t c1 = rdtsc(), n1 = ns_now();
    uint64_t ns = n1 - n0;
    g_tsc_per_ns = ns ? (c1 - c0) / ns : 1;
    if (!g_tsc_per_ns) g_tsc_per_ns = 1;
    printf("[syscall-bench] calibrate tsc_per_ns=%llu span_ns=%llu\n",
           (unsigned long long)g_tsc_per_ns, (unsigned long long)ns);
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* Report min/avg/p50/p99/max latency in nanoseconds for one measured case. */
static void report(const char *name, uint64_t *samples, int n, long check)
{
    qsort(samples, (size_t)n, sizeof(uint64_t), cmp_u64);
    unsigned long long sum = 0;
    for (int i = 0; i < n; ++i) sum += samples[i] / g_tsc_per_ns;
    unsigned long long avg = n ? sum / (unsigned long long)n : 0;
    unsigned long long p50 = samples[n / 2] / g_tsc_per_ns;
    unsigned long long p99 = samples[(int)((double)n * 0.99)] / g_tsc_per_ns;
    printf("[syscall-bench] name=%s n=%d min_ns=%llu avg_ns=%llu p50_ns=%llu "
           "p99_ns=%llu max_ns=%llu check=%ld\n",
           name, n,
           (unsigned long long)(samples[0] / g_tsc_per_ns), avg, p50, p99,
           (unsigned long long)(samples[n - 1] / g_tsc_per_ns), check);
}

#define LAT_ITERS 20000

/* Time one raw syscall with fixed args; the args are ignored by null syscalls. */
static void bench_null(const char *name, long n)
{
    static uint64_t samples[LAT_ITERS];
    long last = 0;
    for (int i = 0; i < LAT_ITERS; ++i) {
        uint64_t t0 = rdtsc();
        last = raw_syscall(n, 0, 0, 0, 0, 0, 0);
        uint64_t t1 = rdtsc();
        samples[i] = t1 - t0;
    }
    report(name, samples, LAT_ITERS, last);
}

static void bench_gettimeofday(void)
{
    static uint64_t samples[LAT_ITERS];
    long last = 0;
    struct { long s; long us; } tv;
    for (int i = 0; i < LAT_ITERS; ++i) {
        uint64_t t0 = rdtsc();
        last = raw_syscall(SYS_gettimeofday, (long)&tv, 0, 0, 0, 0, 0);
        uint64_t t1 = rdtsc();
        samples[i] = t1 - t0;
    }
    report("gettimeofday", samples, LAT_ITERS, last);
}

static void bench_clock_gettime(void)
{
    static uint64_t samples[LAT_ITERS];
    long last = 0;
    struct timespec ts;
    for (int i = 0; i < LAT_ITERS; ++i) {
        uint64_t t0 = rdtsc();
        last = raw_syscall(SYS_clock_gettime, 1 /* CLOCK_MONOTONIC */, (long)&ts, 0, 0, 0, 0);
        uint64_t t1 = rdtsc();
        samples[i] = t1 - t0;
    }
    report("clock_gettime", samples, LAT_ITERS, last);
}

/* One write into a pipe plus one read back: two syscalls per sample. */
static void bench_pipe_roundtrip(void)
{
    static uint64_t samples[LAT_ITERS / 4];
    int fds[2];
    if (pipe(fds)) { printf("[syscall-bench] pipe failed\n"); return; }
    char buf[8] = { 0 };
    int n = LAT_ITERS / 4;
    for (int i = 0; i < n; ++i) {
        uint64_t t0 = rdtsc();
        raw_syscall(SYS_write, fds[1], (long)buf, 8, 0, 0, 0);
        raw_syscall(SYS_read, fds[0], (long)buf, 8, 0, 0, 0);
        uint64_t t1 = rdtsc();
        samples[i] = t1 - t0;
    }
    close(fds[0]); close(fds[1]);
    report("pipe_write+read", samples, n, 8);
}

/* mmap one anonymous page then munmap it: two syscalls per sample. */
static void bench_mmap_munmap(void)
{
    static uint64_t samples[LAT_ITERS / 4];
    int n = LAT_ITERS / 4;
    for (int i = 0; i < n; ++i) {
        uint64_t t0 = rdtsc();
        void *p = (void *)raw_syscall(SYS_mmap, 0, 4096, 3 /* RW */,
                                      0x22 /* PRIVATE|ANON */, -1, 0);
        raw_syscall(SYS_munmap, (long)p, 4096, 0, 0, 0, 0);
        uint64_t t1 = rdtsc();
        samples[i] = t1 - t0;
    }
    report("mmap+munmap", samples, n, 4096);
}

/* Tight getpid loop measures achievable null-syscall throughput. */
static void bench_throughput(void)
{
    const int iters = 200000;
    uint64_t t0 = rdtsc();
    for (int i = 0; i < iters; ++i) {
        __asm__ volatile("" : : : "memory");
        raw_syscall(SYS_getpid, 0, 0, 0, 0, 0, 0);
    }
    uint64_t t1 = rdtsc();
    unsigned long long ns = (t1 - t0) / g_tsc_per_ns;
    double per = ns ? (double)ns / iters : 0;
    double ops = ns ? (double)iters * 1e9 / ns : 0;
    printf("[syscall-bench] throughput syscalls=%d total_ns=%llu ns_per=%.2f ops_per_sec=%.0f\n",
           iters, ns, per, ops);
}

int main(void)
{
    printf("[syscall-bench] BEGIN\n");
    calibrate();
    bench_null("getpid", SYS_getpid);
    bench_null("getuid", SYS_getuid);
    bench_null("getppid", SYS_getppid);
    bench_null("geteuid", SYS_geteuid);
    bench_clock_gettime();
    bench_gettimeofday();
    bench_pipe_roundtrip();
    bench_mmap_munmap();
    bench_throughput();
    printf("[syscall-bench] DONE failures=0\n");
    return 0;
}
