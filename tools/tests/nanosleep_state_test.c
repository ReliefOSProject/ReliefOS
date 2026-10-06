#include <assert.h>
#include <stdio.h>
#include "../../kernel/reliefnt/kernel/reliefnt/syscall_time.c"

static struct task current;
static uint64_t ticks = 100;
static uint64_t wall_offset = 100000;
static int64_t monotonic_offset;
static unsigned blocked;
struct task *sched_current_task(void) { return &current; }
struct task *sched_find(uint32_t pid) { (void)pid; return &current; }
int sched_timer_create(struct task *task, int32_t clock, int32_t notify, int32_t signo,
                       int32_t target_pid, uint64_t value, int32_t *timerid)
{ (void)task; (void)clock; (void)notify; (void)signo; (void)target_pid; (void)value; (void)timerid; return -LINUX_ENOSYS; }
int sched_timer_delete(struct task *task, int32_t timerid) { (void)task; (void)timerid; return -LINUX_ENOSYS; }
int sched_timer_settime(struct task *task, int32_t timerid, uint32_t flags, const struct linux_itimerspec *value, struct linux_itimerspec *old) { (void)task; (void)timerid; (void)flags; (void)value; (void)old; return -LINUX_ENOSYS; }
int sched_timer_gettime(struct task *task, int timerid, struct linux_itimerspec *value) { (void)task; (void)timerid; (void)value; return -LINUX_ENOSYS; }
int sched_timer_getoverrun(struct task *task, int timerid) { (void)task; (void)timerid; return -LINUX_ENOSYS; }
void sched_block_current(void) { ++blocked; }
uint64_t time_ticks(void) { return ticks; }
bool user_range_ok(uint64_t address, uint64_t size)
{ return address >= 4096 && address + size >= address; }
bool user_range_writable(uint64_t address, uint64_t size) { return user_range_ok(address, size); }
void sched_itimer_task(struct task *task, const struct linux_itimerval *value, struct linux_itimerval *old)
{ (void)task; (void)value; (void)old; assert(0); }
void sched_sleep_current_until(uint64_t until)
{ current.wake_tick = until; ++blocked; }
int time_clock_get(int32_t clock, struct linux_timespec *value)
{
    uint64_t now = clock == LINUX_CLOCK_REALTIME ? ticks + wall_offset :
        (uint64_t)((int64_t)ticks + monotonic_offset);
    *value = (struct linux_timespec){now / RELIEFNT_TICK_HZ,
                                   (now % RELIEFNT_TICK_HZ) * (1000000000 / RELIEFNT_TICK_HZ)};
    return 0;
}
static long call(int clock, int flags, struct linux_timespec *request, void *remaining)
{ return syscall_nanosleep(clock, flags, (uintptr_t)request, (uintptr_t)remaining); }

int main(void)
{
    struct linux_timespec value = {0, 0}, remaining = {77, 88};
    assert(call(1, 0, NULL, NULL) == -LINUX_EFAULT);
    value.tv_sec = -1;
    assert(call(1, 0, &value, NULL) == -LINUX_EINVAL);
    value = (struct linux_timespec){0, 1000000000};
    assert(call(1, 0, &value, NULL) == -LINUX_EINVAL);
    value = (struct linux_timespec){0, 0};
    assert(call(1, 0, &value, &remaining) == 0 && blocked == 0);
    assert(remaining.tv_sec == 77 && remaining.tv_nsec == 88);
    assert(call(0, LINUX_TIMER_ABSTIME, &value, NULL) == 0);
    value.tv_nsec = 15000000;
    assert(call(1, 0, &value, &remaining) == KERNEL_SYSCALL_BLOCKED);
    assert(current.wake_tick == 102 && current.nanosleep_deadline == 102);
    value.tv_sec = 500;
    ++ticks;
    assert(call(1, 0, &value, &remaining) == KERNEL_SYSCALL_BLOCKED);
    assert(current.nanosleep_deadline == 102);
    ++ticks;
    assert(call(1, 0, &value, &remaining) == 0 && !current.nanosleep_deadline);
    assert(remaining.tv_sec == 77 && remaining.tv_nsec == 88);
    value = (struct linux_timespec){1001, 40000000};
    assert(call(0, LINUX_TIMER_ABSTIME, &value, &remaining) == KERNEL_SYSCALL_BLOCKED);
    assert(current.wake_tick == 104 && !current.nanosleep_remaining);
    ticks = 104;
    assert(call(0, LINUX_TIMER_ABSTIME, &value, &remaining) == 0);
    value = (struct linux_timespec){INT64_MAX, 999999999};
    assert(call(1, 0, &value, NULL) == KERNEL_SYSCALL_BLOCKED);
    assert(current.nanosleep_deadline > ticks && current.nanosleep_deadline < UINT64_MAX / 2);
    /* NTP disciplines CLOCK_MONOTONIC independently of raw scheduler ticks.
     * A relative wait must neither vanish nor gain that accumulated offset. */
    current.nanosleep_deadline = 0;
    ticks = 4000;
    monotonic_offset = 4;
    value = (struct linux_timespec){0, 5000000};
    assert(call(1, 0, &value, NULL) == KERNEL_SYSCALL_BLOCKED);
    assert(current.wake_tick == ticks + 1);
    ++ticks;
    assert(call(1, 0, &value, NULL) == 0);
    monotonic_offset = -4;
    assert(call(1, 0, &value, NULL) == KERNEL_SYSCALL_BLOCKED);
    assert(current.wake_tick == ticks + 1);
    ++ticks;
    assert(call(1, 0, &value, NULL) == 0);
    puts("PASS nanosleep: raw pointers, timespec validation, zero, rounding, retained deadline, realtime absolute, overflow");
}
