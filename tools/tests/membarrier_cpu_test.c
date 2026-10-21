#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include "../../kernel/reliefnt/arch/x86_64/cpu/smp.c"

static _Thread_local uint32_t test_cpu;
static atomic_int stop_workers;
static atomic_uint sent[4];
static atomic_uint tlb_flushes[4];
void x86_64_flush_user_tlb(void)
{
    atomic_fetch_add(&tlb_flushes[test_cpu], 1);
}

uint32_t apic_id(void) { return test_cpu; }

void apic_send_ipi(uint32_t destination, uint8_t vector)
{
    assert(vector == SMP_MEMBARRIER_VECTOR && destination > 0 && destination < 3);
    atomic_fetch_add_explicit(&sent[destination], 1, memory_order_relaxed);
}

static void *remote_cpu(void *argument)
{
    test_cpu = (uintptr_t)argument;
    while (!atomic_load_explicit(&stop_workers, memory_order_relaxed)) {
        /* Model the lock-free handler also used with masked interrupts. */
        smp_membarrier_poll();
    }
    return NULL;
}

int main(void)
{
    /* Host TSC_AUX identifies a Linux CPU, not this fixture's logical CPU.
     * Use the mocked LAPIC identity for these simulated worker threads. */
    smp_rdtscp_supported = 0;
    cpu_count = 4;
    for (uint32_t i = 0; i < 4; ++i) {
        cpus[i].apic_id = i;
        cpus[i].online = i < 3;
    }
    pthread_t workers[2];
    for (uintptr_t i = 1; i <= 2; ++i) assert(pthread_create(&workers[i - 1], NULL, remote_cpu, (void *)i) == 0);
    for (uint64_t sequence = 1; sequence <= 2000; ++sequence) {
        if (sequence % 3 == 0) smp_flush_user_tlb();
        else smp_membarrier(sequence % 2);
        for (unsigned cpu = 1; cpu <= 2; ++cpu)
            assert(__atomic_load_n(&membarrier_ack[cpu], __ATOMIC_ACQUIRE) == sequence);
        assert(membarrier_request[0] == 0 && membarrier_request[3] == 0);
    }
    atomic_store(&stop_workers, 1);
    for (unsigned i = 0; i < 2; ++i) {
        assert(pthread_join(workers[i], NULL) == 0);
        assert(atomic_load(&sent[i + 1]) == 2000);
        assert(atomic_load(&tlb_flushes[i + 1]) == 2000 / 3);
    }
    puts("PASS membarrier CPU rendezvous: remote acknowledgement, offline CPUs, repeated normal/sync-core barriers");
}
