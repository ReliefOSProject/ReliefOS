#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <reliefnt/console.h>
#include <reliefnt/smp.h>
#include <reliefnt/vdso_data.h>
#include "../../kernel/reliefnt/kernel/time/time.c"

static unsigned held;
static uint64_t fake_tsc;
static uint32_t fake_online_cpus = 1;
static uint32_t fake_cpu_count = 1;
static bool fake_tsc_safe = true;
static struct reliefnt_vdso_data *concurrent_data;
static _Atomic bool snapshot_reader_running;
static _Atomic uint32_t snapshot_reader_samples;
static int64_t expected_realtime_minus_monotonic_ns;

uint64_t time_test_rdtsc(void) { return fake_tsc; }
bool time_test_tsc_safe(void) { return fake_tsc_safe; }

void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{
    (void)lock;
    assert(held++ == 0);
    *flags = 0;
}
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{
    (void)lock;
    (void)flags;
    assert(held-- == 1);
}
void audio_service_tick(void) { assert(!held); }
void usb_poll(void) { assert(!held); }
void sched_on_tick(void) { assert(!held); }
void console_display_tick(void) { assert(!held); }
void console_printf(const char *format, ...) { (void)format; }
void console_log(enum console_log_level level, const char *format, ...)
{
    (void)level;
    (void)format;
}
uint32_t smp_cpu_count(void) { return fake_cpu_count; }
bool smp_cpu_online(uint32_t cpu) { return cpu < fake_online_cpus; }

static bool timespec_at_least(const struct linux_timespec *a,
                              const struct linux_timespec *b)
{
    return a->tv_sec > b->tv_sec ||
           (a->tv_sec == b->tv_sec && a->tv_nsec >= b->tv_nsec);
}

static bool timespec_equal(const struct linux_timespec *a,
                           const struct linux_timespec *b)
{
    return a->tv_sec == b->tv_sec && a->tv_nsec == b->tv_nsec;
}

static int64_t timespec_delta_ns(const struct linux_timespec *start,
                                 const struct linux_timespec *end)
{
    return (end->tv_sec - start->tv_sec) * 1000000000LL +
           end->tv_nsec - start->tv_nsec;
}

static void *snapshot_reader(void *unused)
{
    (void)unused;
    while (atomic_load_explicit(&snapshot_reader_running, memory_order_acquire)) {
        uint32_t before = __atomic_load_n(&concurrent_data->seq, __ATOMIC_ACQUIRE);
        if (before & 1u) continue;
        uint32_t mode = __atomic_load_n(&concurrent_data->clock_mode, __ATOMIC_RELAXED);
        uint32_t clock_mask = __atomic_load_n(&concurrent_data->clock_mask, __ATOMIC_RELAXED);
        uint64_t rt_sec = (uint64_t)__atomic_load_n(&concurrent_data->basetime[0].sec,
                                                    __ATOMIC_RELAXED);
        uint64_t rt_nsec = __atomic_load_n(&concurrent_data->basetime[0].nsec,
                                           __ATOMIC_RELAXED);
        uint64_t mono_sec = (uint64_t)__atomic_load_n(&concurrent_data->basetime[1].sec,
                                                      __ATOMIC_RELAXED);
        uint64_t mono_nsec = __atomic_load_n(&concurrent_data->basetime[1].nsec,
                                             __ATOMIC_RELAXED);
        uint32_t after = __atomic_load_n(&concurrent_data->seq, __ATOMIC_ACQUIRE);
        if (before != after || (after & 1u)) continue;
        assert(mode == RELIEFNT_VDSO_MODE_TSC);
        assert(clock_mask == ((1u << 0) | (1u << 1) | (1u << 5) | (1u << 6)));
        assert(rt_nsec < 1000000000ULL && mono_nsec < 1000000000ULL);
        int64_t delta = ((int64_t)rt_sec - (int64_t)mono_sec) * 1000000000LL +
                        (int64_t)rt_nsec - (int64_t)mono_nsec;
        assert(delta == expected_realtime_minus_monotonic_ns);
        atomic_fetch_add_explicit(&snapshot_reader_samples, 1, memory_order_relaxed);
    }
    return NULL;
}

int main(void)
{
    struct reliefnt_vdso_data data;
    memset(&data, 0xa5, sizeof(data));
    fake_tsc = 1000000;
    tsc_ready = 1;
    tsc_per_us = 2000; /* 2 GHz test TSC */
    tsc_calib_span = 8;
    tsc_anchor = fake_tsc;
    wall_clock_valid = 1;
    wall_unix_seconds = 1700000000;
    wall_fraction_ns = 250000000;
    monotonic_seconds = 3;
    monotonic_ns = 400000000;
    time_discipline_clear();

    /* A discovered AP may run user code before a later tick observes it.
     * Do not publish user-TSC clocks while SMP is configured, even if only
     * the BSP is online at this instant. */
    fake_cpu_count = 2;
    fake_online_cpus = 1;
    time_vdso_bind(&data);
    assert(data.clock_mode == RELIEFNT_VDSO_MODE_NONE);
    assert(data.clock_mask == ((1u << 5) | (1u << 6)));
    fake_cpu_count = 1;
    time_vdso_bind(&data);
    assert(!(data.seq & 1u));
    assert(data.clock_mode == RELIEFNT_VDSO_MODE_TSC);
    assert(data.clock_mask == ((1u << 0) | (1u << 1) | (1u << 5) | (1u << 6)));
    assert(data.hres_mask == ((1u << 0) | (1u << 1)));
    assert(data.cycle_last == fake_tsc);
    assert(data.mult != 0 && data.max_cycles != 0 && data.resolution_ns != 0);
    assert(data.basetime[0].sec == 1700000000 && data.basetime[0].nsec == 250000000);
    assert(data.basetime[1].sec == 3 && data.basetime[1].nsec == 400000000);
    assert(data.basetime[5].sec == 1700000000 && data.basetime[5].nsec == 250000000);
    assert(data.basetime[6].sec == 3 && data.basetime[6].nsec == 400000000);
    struct linux_timespec resolution;
    assert(time_clock_getres(LINUX_CLOCK_MONOTONIC, &resolution) == 0);
    assert(resolution.tv_sec == 0 && (uint64_t)resolution.tv_nsec == data.resolution_ns);
    assert(time_clock_getres(LINUX_CLOCK_MONOTONIC_COARSE, &resolution) == 0);
    assert(resolution.tv_nsec == 10000000);
    assert(time_clock_getres(LINUX_CLOCK_MONOTONIC, NULL) == 0);
    assert(time_clock_getres(99, NULL) == -LINUX_EINVAL);

    /* An adjustment during a tick settles the old rate, then the next
     * interval uses the new rate. The boundary must not double-credit a tick. */
    fake_tsc += tsc_per_us * 5000ULL;
    struct linux_timex adjustment = {.modes = ADJ_TICK, .tick = 9000};
    assert(time_adjust(&adjustment, true) >= 0);
    struct linux_timespec before_tick, after_tick, next_tick;
    assert(time_clock_get(LINUX_CLOCK_MONOTONIC, &before_tick) == 0);
    fake_tsc += tsc_per_us * 5000ULL;
    assert(time_clock_get(LINUX_CLOCK_MONOTONIC, &before_tick) == 0);
    assert(before_tick.tv_sec == 3);
    assert(before_tick.tv_nsec >= 409499998 && before_tick.tv_nsec <= 409500002);
    time_on_tick();
    assert(time_clock_get(LINUX_CLOCK_MONOTONIC, &after_tick) == 0);
    assert(timespec_equal(&after_tick, &before_tick));
    assert(!(data.seq & 1u));
    assert(data.basetime[1].sec > 0 && data.basetime[1].nsec < 1000000000ULL);

    fake_tsc += tsc_per_us * 10000ULL;
    assert(time_clock_get(LINUX_CLOCK_MONOTONIC, &before_tick) == 0);
    assert(before_tick.tv_sec == 3);
    assert(before_tick.tv_nsec >= 418499998 && before_tick.tv_nsec <= 418500002);
    time_on_tick();
    assert(time_clock_get(LINUX_CLOCK_MONOTONIC, &next_tick) == 0);
    assert(timespec_equal(&next_tick, &before_tick));

    /* Read vvar locklessly while ticks republish it. Each accepted sequence
     * must pair realtime and monotonic bases from the same writer pass. */
    concurrent_data = &data;
    expected_realtime_minus_monotonic_ns =
        (data.basetime[0].sec - data.basetime[1].sec) * 1000000000LL +
        (int64_t)data.basetime[0].nsec - (int64_t)data.basetime[1].nsec;
    atomic_store(&snapshot_reader_samples, 0);
    atomic_store_explicit(&snapshot_reader_running, true, memory_order_release);
    pthread_t reader;
    assert(pthread_create(&reader, NULL, snapshot_reader, NULL) == 0);
    for (unsigned i = 0; i < 1000; ++i) {
        fake_tsc += tsc_per_us * 10000ULL;
        time_on_tick();
    }
    while (atomic_load_explicit(&snapshot_reader_samples, memory_order_relaxed) == 0) { }
    atomic_store_explicit(&snapshot_reader_running, false, memory_order_release);
    assert(pthread_join(reader, NULL) == 0);

    /* A wall-clock step publishes the new realtime anchor immediately and
     * leaves monotonic time continuous. */
    fake_tsc += tsc_per_us * 1000ULL;
    struct linux_timespec monotonic_before_set, monotonic_after_set, realtime;
    assert(time_clock_get(LINUX_CLOCK_MONOTONIC, &monotonic_before_set) == 0);
    assert(time_set_wall_clock_ns(1800000000, 123456789) == 0);
    assert(time_clock_get(LINUX_CLOCK_MONOTONIC, &monotonic_after_set) == 0);
    assert(timespec_at_least(&monotonic_after_set, &monotonic_before_set));
    assert(time_clock_get(LINUX_CLOCK_REALTIME, &realtime) == 0);
    assert(realtime.tv_sec == 1800000000 && realtime.tv_nsec == 123456789);
    assert(data.basetime[0].sec == 1800000000 && data.basetime[0].nsec == 123456789);

    /* COARSE clocks are copied from the actual tick bases. */
    struct linux_timespec coarse;
    assert(time_clock_get(LINUX_CLOCK_MONOTONIC_COARSE, &coarse) == 0);
    assert(data.basetime[6].sec == coarse.tv_sec && data.basetime[6].nsec == coarse.tv_nsec);
    assert(time_clock_get(LINUX_CLOCK_REALTIME_COARSE, &coarse) == 0);
    assert(data.basetime[5].sec == coarse.tv_sec && data.basetime[5].nsec == coarse.tv_nsec);

    /* Insertion and deletion leap corrections must update the high-resolution
     * realtime base with the coarse wall clock. TAI must remain continuous as
     * its UTC offset changes by the opposite second. */
    assert(time_set_wall_clock_ns(86398, 990000000) == 0);
    struct linux_timex reset_tick = {.modes = ADJ_TICK, .tick = 10000};
    assert(time_adjust(&reset_tick, true) >= 0);
    struct linux_timex leap = {.modes = ADJ_STATUS, .status = STA_INS};
    assert(time_adjust(&leap, true) >= 0);
    struct linux_timespec tai_before, tai_after, realtime_hres;
    for (unsigned i = 0; i < 101; ++i) {
        if (i == 100) assert(time_clock_get(11, &tai_before) == 0);
        fake_tsc += tsc_per_us * 10000ULL;
        time_on_tick();
    }
    assert(time_clock_get(LINUX_CLOCK_REALTIME, &realtime_hres) == 0);
    assert(time_clock_get(LINUX_CLOCK_REALTIME_COARSE, &coarse) == 0);
    assert(timespec_equal(&realtime_hres, &coarse));
    assert(realtime_hres.tv_sec == 86399 && realtime_hres.tv_nsec == 0);
    assert(time_clock_get(11, &tai_after) == 0);
    assert(tai_before.tv_sec == 86399 && tai_before.tv_nsec == 990000000);
    assert(tai_after.tv_sec == 86400 && tai_after.tv_nsec == 0);

    assert(time_set_wall_clock_ns(172797, 990000000) == 0);
    reset_tick = (struct linux_timex){.modes = ADJ_TICK, .tick = 10000};
    assert(time_adjust(&reset_tick, true) >= 0);
    leap = (struct linux_timex){.modes = ADJ_STATUS, .status = STA_DEL};
    assert(time_adjust(&leap, true) >= 0);
    for (unsigned i = 0; i < 101; ++i) {
        if (i == 100) assert(time_clock_get(11, &tai_before) == 0);
        fake_tsc += tsc_per_us * 10000ULL;
        time_on_tick();
    }
    assert(time_clock_get(LINUX_CLOCK_REALTIME, &realtime_hres) == 0);
    assert(time_clock_get(LINUX_CLOCK_REALTIME_COARSE, &coarse) == 0);
    assert(timespec_equal(&realtime_hres, &coarse));
    assert(realtime_hres.tv_sec == 172800 && realtime_hres.tv_nsec == 0);
    assert(time_clock_get(11, &tai_after) == 0);
    assert(tai_before.tv_sec == 172799 && tai_before.tv_nsec == 990000000);
    assert(tai_after.tv_sec == 172800 && tai_after.tv_nsec == 0);

    /* Syscall normalization must handle a stale multi-second interval even
     * though vDSO readers fall back when it exceeds max_cycles. */
    struct linux_timespec before_long_delta, after_long_delta;
    assert(time_clock_get(LINUX_CLOCK_MONOTONIC, &before_long_delta) == 0);
    fake_tsc += tsc_per_us * 2000000ULL;
    assert(time_clock_get(LINUX_CLOCK_MONOTONIC, &after_long_delta) == 0);
    assert(after_long_delta.tv_sec == before_long_delta.tv_sec + 2);
    assert(after_long_delta.tv_nsec == before_long_delta.tv_nsec);
    assert(after_long_delta.tv_nsec < 1000000000);

    fake_tsc_safe = false;
    time_on_tick();
    assert(data.clock_mode == RELIEFNT_VDSO_MODE_NONE && data.hres_mask == 0);
    assert(data.clock_mask == ((1u << 5) | (1u << 6)));
    assert(time_clock_getres(LINUX_CLOCK_MONOTONIC, &resolution) == 0);
    assert(resolution.tv_nsec == 1000);
    fake_tsc_safe = true;
    fake_cpu_count = 2;
    fake_online_cpus = 2;
    time_on_tick();
    assert(data.clock_mode == RELIEFNT_VDSO_MODE_NONE && data.hres_mask == 0);
    assert(data.clock_mask == ((1u << 5) | (1u << 6)));

    /* The legacy syscall path also promises nondecreasing MONOTONIC when an
     * adjusted 9ms tick follows a 10ms subtick read. The floor must not add
     * that missing millisecond to the next tick: it should advance by 18ms. */
    struct linux_timespec fallback_start, fallback_before_tick;
    struct linux_timespec fallback_after_tick, fallback_after_next_tick;
    assert(time_clock_get(LINUX_CLOCK_MONOTONIC, &fallback_start) == 0);
    struct linux_timex fallback_adjust = {.modes = ADJ_TICK, .tick = 9000};
    assert(time_adjust(&fallback_adjust, true) >= 0);
    fake_tsc += tsc_per_us * 10000ULL;
    assert(time_clock_get(LINUX_CLOCK_MONOTONIC, &fallback_before_tick) == 0);
    time_on_tick();
    assert(time_clock_get(LINUX_CLOCK_MONOTONIC, &fallback_after_tick) == 0);
    assert(timespec_at_least(&fallback_after_tick, &fallback_before_tick));
    fake_tsc += tsc_per_us * 10000ULL;
    time_on_tick();
    assert(time_clock_get(LINUX_CLOCK_MONOTONIC, &fallback_after_next_tick) == 0);
    struct linux_timespec expected_fallback = fallback_start;
    expected_fallback.tv_nsec += 18000000;
    if (expected_fallback.tv_nsec >= 1000000000) {
        expected_fallback.tv_nsec -= 1000000000;
        ++expected_fallback.tv_sec;
    }
    assert(timespec_equal(&fallback_after_next_tick, &expected_fallback));

    wall_clock_valid = 0;
    time_vdso_bind(&data);
    assert(data.clock_mode == RELIEFNT_VDSO_MODE_NONE);
    assert(data.clock_mask == (1u << 6));
    assert(!(data.seq & 1u));

    /* At 2.4 GHz, Q32 cannot represent the 5/12 ns-per-cycle scale exactly.
     * A nominal full PIT tick is still exactly 10ms in the TSC clock domain;
     * one thousand settles must match that independent literal total. */
    fake_cpu_count = 1;
    fake_online_cpus = 1;
    fake_tsc_safe = true;
    tsc_per_us = 2400;
    tsc_ready = 1;
    tsc_anchor = fake_tsc;
    uptime_tsc_anchor = fake_tsc;
    monotonic_seconds = 100000;
    monotonic_ns = 0;
    monotonic_floor = (struct reliefnt_vdso_timestamp){100000, 0};
    monotonic_floor_valid = 1;
    tsc_trusted = 0;
    time_discipline_clear();
    time_vdso_bind(&data);
    struct linux_timex tick_rate = {.modes = ADJ_TICK, .tick = 10000};
    assert(time_adjust(&tick_rate, true) >= 0);
    struct linux_timespec rate_start, rate_end;
    assert(time_clock_get(LINUX_CLOCK_MONOTONIC, &rate_start) == 0);
    for (unsigned i = 0; i < 1000; ++i) {
        fake_tsc += tsc_per_us * 10000ULL;
        time_on_tick();
    }
    assert(time_clock_get(LINUX_CLOCK_MONOTONIC, &rate_end) == 0);
    assert(timespec_delta_ns(&rate_start, &rate_end) == 10000000000LL);

    /* Repeat at both discipline bounds with literal elapsed-time totals. */
    tick_rate = (struct linux_timex){.modes = ADJ_TICK, .tick = 9000};
    assert(time_adjust(&tick_rate, true) >= 0);
    assert(time_clock_get(LINUX_CLOCK_MONOTONIC, &rate_start) == 0);
    for (unsigned i = 0; i < 1000; ++i) {
        fake_tsc += tsc_per_us * 10000ULL;
        time_on_tick();
    }
    assert(time_clock_get(LINUX_CLOCK_MONOTONIC, &rate_end) == 0);
    assert(timespec_delta_ns(&rate_start, &rate_end) == 9000000000LL);

    tick_rate = (struct linux_timex){.modes = ADJ_TICK, .tick = 11000};
    assert(time_adjust(&tick_rate, true) >= 0);
    assert(time_clock_get(LINUX_CLOCK_MONOTONIC, &rate_start) == 0);
    for (unsigned i = 0; i < 1000; ++i) {
        fake_tsc += tsc_per_us * 10000ULL;
        time_on_tick();
    }
    assert(time_clock_get(LINUX_CLOCK_MONOTONIC, &rate_end) == 0);
    assert(timespec_delta_ns(&rate_start, &rate_end) == 11000000000LL);

    /* A mid-tick 11ms->9ms change settles the first half at the old rate,
     * then the second half at the new rate without a boundary double-credit. */
    tick_rate = (struct linux_timex){.modes = ADJ_TICK, .tick = 9000};
    assert(time_clock_get(LINUX_CLOCK_MONOTONIC, &rate_start) == 0);
    fake_tsc += tsc_per_us * 5000ULL;
    assert(time_adjust(&tick_rate, true) >= 0);
    assert(time_clock_get(LINUX_CLOCK_MONOTONIC, &rate_end) == 0);
    assert(timespec_delta_ns(&rate_start, &rate_end) == 5500000LL);
    fake_tsc += tsc_per_us * 5000ULL;
    assert(time_clock_get(LINUX_CLOCK_MONOTONIC, &rate_end) == 0);
    assert(timespec_delta_ns(&rate_start, &rate_end) == 10000000LL);
    time_on_tick();
    assert(time_clock_get(LINUX_CLOCK_MONOTONIC, &rate_end) == 0);
    assert(timespec_delta_ns(&rate_start, &rate_end) == 10000000LL);
    fake_tsc += tsc_per_us * 10000ULL;
    time_on_tick();
    assert(time_clock_get(LINUX_CLOCK_MONOTONIC, &rate_end) == 0);
    assert(timespec_delta_ns(&rate_start, &rate_end) == 19000000LL);

    /* A plausible average must not bless a disturbed PIT calibration window.
     * Six 2.4GHz intervals plus one half-length burst still average into the
     * old broad 100..10000 cycles/us range, but the window is not stable. */
    fake_tsc_safe = false;
    fake_cpu_count = 2;
    fake_online_cpus = 2;
    tsc_trusted = 0;
    tsc_ready = 0;
    tsc_per_us = 0;
    tsc_calib_span = 0;
    tsc_calib_first = 0;
    tsc_calib_confirm_rate = 0;
    time_on_tick();
    for (unsigned i = 0; i < 7; ++i) {
        fake_tsc += i == 3 ? 12000000ULL : 24000000ULL;
        time_on_tick();
    }
    assert(tsc_ready == 0);
    fake_tsc += 0;
    time_on_tick();
    assert(tsc_calib_span == 1);
    --fake_tsc;
    time_on_tick();
    assert(tsc_calib_span == 1);
    fake_tsc += 100000001ULL;
    time_on_tick();
    assert(tsc_calib_span == 1);

    /* Rejection restarts at the latest sample. One stable window only records
     * a candidate rate; a following 2.6GHz window must fail confirmation. */
    for (unsigned i = 0; i < 7; ++i) {
        fake_tsc += 24000000ULL;
        time_on_tick();
    }
    assert(tsc_ready == 0);
    for (unsigned i = 0; i < 7; ++i) {
        fake_tsc += 26000000ULL;
        time_on_tick();
    }
    assert(tsc_ready == 0);

    /* Two consecutive stable windows at 2.4GHz establish the rate. */
    for (unsigned i = 0; i < 7; ++i) {
        fake_tsc += 24000000ULL;
        time_on_tick();
    }
    assert(tsc_ready == 0);
    for (unsigned i = 0; i < 7; ++i) {
        fake_tsc += 24000000ULL;
        time_on_tick();
    }
    assert(tsc_ready == 1);
    assert(tsc_per_us == 2400);

    assert(!held);
    puts("PASS real timekeeper/vvar: continuous tick and adjtime, settime, coarse data, and unsafe fallback");
    return 0;
}
