#include <assert.h>
#include <stdio.h>
#ifndef AUDIO_FIXTURE_NO_PTHREAD
#include <pthread.h>
#endif
#include <reliefnt/lock.h>
#include <reliefnt/wait.h>
#ifndef AUDIO_FIXTURE_REAL_WAIT
/* These PCM-only fixtures have no control readers registered for wakeup. */
uint32_t kernel_wait_queue_wake_all(struct kernel_wait_queue *queue)
{
    assert(queue && !queue->count);
    return 0;
}
#endif
static __thread uint64_t audio_fixture_irq_flags = 1ULL << 9;
uint32_t smp_current_cpu(void) { return 0; }
void smp_membarrier_poll(void) { }
uint64_t kernel_irq_save(void) {
    uint64_t saved = audio_fixture_irq_flags;
    audio_fixture_irq_flags &= ~(1ULL << 9);
    return saved;
}
void kernel_irq_restore(uint64_t flags) { audio_fixture_irq_flags = flags & (1ULL << 9); }
#include "../../kernel/reliefnt/kernel/reliefnt/lock.c"

#include <linux/time.h>
/* Hardware clock boundary only; all PCM state and ABI logic stays production. */
int time_clock_get(int32_t clock, struct linux_timespec *value)
{
    *value = (struct linux_timespec){.tv_sec = 100 + clock, .tv_nsec = 12345};
    return 0;
}
