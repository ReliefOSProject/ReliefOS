#include <assert.h>
#include <stdio.h>
#include <string.h>

#define RELIEFNT_APIC_TEST 1
#define APIC_TIMER_CALIBRATION_POLL_LIMIT 4096u
#include "../../kernel/reliefnt/arch/x86_64/apic.c"

static uint64_t fixture_us, armed_us;
static uint32_t programmed_count, programmed_lvt, programmed_divide;
static unsigned barrier_polls, writes, tick_reads;
static bool execution_locked;
static bool stalled_reference, stopped_counter, exhausted_counter;
static bool reversed_counter, overshot_reference, unavailable_reference, slow_snapshot;
static uint64_t fixture_rate;
static uint64_t pause_at_us;
static bool pause_done;
static uint32_t reference_mask, reference_offset;

uint32_t apic_test_read(uint32_t reg)
{
    if (reg == 0x20) return 2u << 24;
    if (reg == 0x390) {
        if (exhausted_counter) return 0;
        if (stopped_counter) return UINT32_MAX;
        uint64_t elapsed = fixture_us - armed_us;
        if (reversed_counter) return (uint32_t)(1000 + elapsed);
        uint64_t delta = elapsed * fixture_rate / 1000000;
        return delta >= UINT32_MAX ? 0 : UINT32_MAX - (uint32_t)delta;
    }
    return 0;
}
void apic_test_write(uint32_t reg, uint32_t value)
{
    ++writes;
    if (reg == 0x380) {
        if (value == UINT32_MAX) {
            assert(programmed_lvt == (0x40u | (1u << 16)));
            assert(programmed_divide == 3);
        }
        programmed_count = value; armed_us = fixture_us;
    }
    if (reg == 0x320) programmed_lvt = value;
    if (reg == 0x3e0) programmed_divide = value;
}
bool power_acpi_pm_timer_read(uint32_t *count, uint32_t *mask)
{
    assert(!execution_locked);
    if (unavailable_reference) return false;
    fixture_us += slow_snapshot ? 1000 : 50;
    if (pause_at_us && !pause_done && fixture_us >= pause_at_us) {
        /* Deschedule this virtual CPU for one complete PM counter period.
         * Its modulo PM timestamp then looks normal while the LAPIC has
         * advanced by almost five seconds. */
        fixture_us += ((uint64_t)reference_mask + 1u) * 1000000u / ACPI_PM_TIMER_HZ;
        pause_done = true;
    }
    if (overshot_reference && fixture_us > 50000) fixture_us += 200000;
    *mask = reference_mask;
    *count = stalled_reference ? reference_offset :
        (reference_offset + (uint32_t)(fixture_us * ACPI_PM_TIMER_HZ / 1000000)) & reference_mask;
    return true;
}
/* A startup IRQ catch-up burst is deliberately unrelated to physical time.
 * Calibration must use the free-running PM clock, never these event ticks. */
uint64_t time_ticks(void) { ++tick_reads; return fixture_us * 1000; }
void smp_membarrier_poll(void) { assert(!execution_locked); ++barrier_polls; }
void kernel_execution_lock_irqsave(uint64_t *flags)
{ assert(!execution_locked); execution_locked = true; *flags = 0; }
void kernel_execution_unlock_irqrestore(uint64_t flags)
{ assert(execution_locked && !flags); execution_locked = false; }
void console_log(enum console_log_level level, const char *format, ...)
{ assert(execution_locked); (void)level; (void)format; }

static void reset(void)
{
    fixture_us = armed_us = programmed_count = programmed_lvt = programmed_divide = 0;
    barrier_polls = writes = tick_reads = 0;
    execution_locked = false;
    stalled_reference = stopped_counter = exhausted_counter = false;
    reversed_counter = overshot_reference = unavailable_reference = slow_snapshot = false;
    fixture_rate = 4200000;
    pause_at_us = 0;
    pause_done = false;
    reference_mask = 0x00ffffff;
    reference_offset = 0;
    local_apic_enabled = true;
    local_apic_base = 1; /* all MMIO goes through the host hooks */
}
static void assert_periodic(uint32_t expected)
{
    assert(programmed_count == expected);
    assert(programmed_lvt == (0x40u | (1u << 17)));
    assert(programmed_divide == 3);
}
static void test_measured_period_and_wrap(void)
{
    const uint64_t rates[] = {4200000, 62500000};
    const uint32_t masks[] = {0x00ffffff, UINT32_MAX};
    for (unsigned i = 0; i < 2; ++i) for (unsigned j = 0; j < 2; ++j) {
        reset(); fixture_rate = rates[i]; reference_mask = masks[j];
        reference_offset = masks[j] - 10000; /* cross a real 24/32-bit wrap */
        apic_timer_init(0x40, 1000000);
        uint32_t expected = (uint32_t)(fixture_rate / 100);
        assert(programmed_count >= expected - 1 && programmed_count <= expected + 1);
        assert(programmed_lvt == (0x40u | (1u << 17)) && programmed_divide == 3);
        assert(barrier_polls && barrier_polls <= APIC_TIMER_CALIBRATION_POLL_LIMIT);
        assert(!tick_reads);
    }
}
static void test_bounded_failures(void)
{
    for (unsigned fault = 0; fault < 8; ++fault) {
        reset();
        switch (fault) {
        case 0: stalled_reference = true; break;
        case 1: stopped_counter = true; break;
        case 2: exhausted_counter = true; break;
        case 3: reversed_counter = true; break;
        case 4: overshot_reference = true; break;
        case 5: unavailable_reference = true; break;
        case 6: fixture_rate = 1; break;
        case 7: slow_snapshot = true; break;
        }
        apic_timer_init(0x40, 1000000);
        assert_periodic(1000000);
        assert(barrier_polls <= APIC_TIMER_CALIBRATION_POLL_LIMIT);
        if (fault == 0 || fault == 7) assert(barrier_polls == APIC_TIMER_CALIBRATION_POLL_LIMIT);
    }
    reset(); fixture_rate = 60000000000ULL; /* one-shot expires within the window */
    apic_timer_init(0x40, 1000000); assert_periodic(1000000);
    reset(); stalled_reference = true;
    apic_timer_init(0x40, 0); assert_periodic(1000000);
    reset(); local_apic_enabled = false;
    apic_timer_init(0x40, 1000000); assert(!writes && !barrier_polls);
}
static void test_single_pm_wrap_pause(void)
{
    const uint64_t pauses[] = {25000, 75000};
    for (unsigned i = 0; i < sizeof(pauses) / sizeof(pauses[0]); ++i) {
        reset(); pause_at_us = pauses[i];
        apic_timer_init(0x40, 1000000);
        assert(pause_done);
        assert_periodic(1000000);
    }
}
int main(void)
{
    test_measured_period_and_wrap();
    test_bounded_failures();
    test_single_pm_wrap_pause();
    puts("PASS APIC timer: physical PM clock, IRQ bursts, 24/32-bit wrap, bounded faults and fallback");
    return 0;
}
