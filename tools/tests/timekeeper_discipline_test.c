#include <reliefnt/console.h>
#include <assert.h>
#include <stdio.h>
#include "../../kernel/reliefnt/kernel/reliefnt/time.c"
static unsigned held;
static unsigned display_ticks;
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ (void)lock; assert(held++ == 0); *flags=0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)lock; (void)flags; assert(held-- == 1); }
void usb_poll(void) { assert(!held); }
void sched_on_tick(void) { assert(!held); }
void audio_service_tick(void) { assert(!held); }
void console_display_tick(void) { assert(!held); ++display_ticks; }
/* This existing test covers the PIT discipline; the dedicated vDSO fixture
 * exercises trusted TSC interpolation with a controlled counter. */
uint32_t smp_cpu_count(void) { return 2; }
bool smp_cpu_online(uint32_t cpu) { return cpu < 2; }
void console_printf(const char *format, ...) { (void)format; }

void console_log(enum console_log_level level, const char *format, ...)
{
    (void)level; (void)format;
}
int main(void)
{
    wall_clock_valid=1;
    wall_unix_seconds=1700000000;
    struct linux_timex tx={.modes=ADJ_FREQUENCY,.freq=32768000};
    assert(time_adjust(&tx,true)==TIME_ERROR);
    for(unsigned i=0;i<100;i++) time_on_tick();
    assert(display_ticks == 50);
    struct linux_timespec wall, mono, raw;
    assert(time_clock_get(LINUX_CLOCK_REALTIME,&wall)==0);
    assert(time_clock_get(LINUX_CLOCK_MONOTONIC,&mono)==0);
    assert(time_clock_get(LINUX_CLOCK_MONOTONIC_RAW,&raw)==0);
    assert(wall.tv_sec==1700000001 && wall.tv_nsec==500000);
    assert(mono.tv_sec==1 && mono.tv_nsec==500000);
    assert(raw.tv_sec==1 && raw.tv_nsec==0);
    assert(time_set_wall_clock_ns(1700000100,999999999)==0);
    assert(time_clock_get(LINUX_CLOCK_MONOTONIC,&raw)==0);
    assert(raw.tv_sec==mono.tv_sec && raw.tv_nsec==mono.tv_nsec);
    time_on_tick();
    assert(display_ticks == 50);
    assert(time_clock_get(LINUX_CLOCK_REALTIME,&wall)==0);
    assert(wall.tv_sec==1700000101 && wall.tv_nsec==10004999);
    assert(time_set_wall_clock_ns(1700000100,1000000000)==-1);
    assert(!held);
    puts("PASS real timekeeper: disciplined realtime/monotonic, unchanged RAW, atomic steps and lock boundaries");
}
