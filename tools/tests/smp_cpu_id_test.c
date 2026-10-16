#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../kernel/reliefnt/arch/x86_64/smp.c"

static uint32_t fixture_apic_id_value;

uint32_t apic_id(void)
{
    return fixture_apic_id_value;
}

/* Only smp_init() logs; its privileged startup section is discarded by GC. */
void console_log(enum console_log_level level, const char *format, ...)
{
    (void)level;
    (void)format;
}

struct cpuid_result {
    uint32_t eax, ebx, ecx, edx;
};

static struct cpuid_result host_cpuid(uint32_t leaf, uint32_t subleaf)
{
    struct cpuid_result result;
    __asm__ volatile("cpuid"
                     : "=a"(result.eax), "=b"(result.ebx),
                       "=c"(result.ecx), "=d"(result.edx)
                     : "a"(leaf), "c"(subleaf));
    return result;
}

static bool host_rdtscp_supported(void)
{
    struct cpuid_result maximum = host_cpuid(0x80000000u, 0);
    if (maximum.eax < 0x80000001u) return false;
    return (host_cpuid(0x80000001u, 0).edx & (1u << 27)) != 0;
}

static bool host_rdpid_supported(void)
{
    if (host_cpuid(0, 0).eax < 7u) return false;
    return (host_cpuid(7, 0).ecx & (1u << 22)) != 0;
}

static uint32_t host_rdtscp_aux(void)
{
    uint32_t low, high, aux;
    __asm__ volatile("rdtscp" : "=a"(low), "=d"(high), "=c"(aux) : : "memory");
    (void)low;
    (void)high;
    return aux;
}

static uint32_t host_rdpid_aux(void)
{
    uint64_t aux;
    __asm__ volatile("rdpid %0" : "=r"(aux) : : "memory");
    return (uint32_t)aux;
}

static void configure_fixture_topology(void)
{
    memset(cpus, 0, sizeof(cpus));
    cpu_count = 3;
    cpus[0].apic_id = 0x11;
    cpus[1].apic_id = 0x22;
    cpus[2].apic_id = 0x33;
    fixture_apic_id_value = 0x22;
}

static uint32_t fixture_apic_index(void)
{
    for (uint32_t i = 0; i < cpu_count; ++i) {
        if (cpus[i].apic_id == fixture_apic_id_value) return i;
    }
    return 0;
}

/* TSC_AUX stores the CPU index plus one; zero and out-of-range values fall
 * back to the APIC topology lookup used by smp_current_cpu(). */
static uint32_t expected_index_for_aux(uint32_t aux)
{
    if (aux != 0 && aux <= SMP_MAX_CPUS) return aux - 1u;
    return fixture_apic_index();
}

static int expected_probe_method(void)
{
    if (host_rdpid_supported()) return 2;
    if (host_rdtscp_supported()) return 1;
    return 0;
}

static uint32_t host_aux_for_method(int method)
{
    if (method == 2) return host_rdpid_aux();
    if (method == 1) return host_rdtscp_aux();
    return 0;
}

static void test_cached_cpuid_probe(void)
{
    configure_fixture_topology();
    smp_cpu_aux_method = -1;
    int expected_method = expected_probe_method();
    uint32_t result = smp_current_cpu();
    assert(smp_cpu_aux_method == expected_method);
    if (expected_method == 0) {
        assert(result == fixture_apic_index());
    } else {
        uint32_t aux = host_aux_for_method(expected_method);
        assert(result == expected_index_for_aux(aux));
    }
    /* The selected source is cached, so later queries reuse that method. */
    assert(smp_current_cpu() == result);
    assert(smp_cpu_aux_method == expected_method);
}

static void test_forced_apic_fallback(void)
{
    configure_fixture_topology();
    smp_cpu_aux_method = 0;
    assert(smp_current_cpu() == 1);
    fixture_apic_id_value = 0x99;
    assert(smp_current_cpu() == 0);
}

static void test_real_aux_instructions_and_mapping(void)
{
    configure_fixture_topology();
    bool have_rdtscp = host_rdtscp_supported();
    bool have_rdpid = host_rdpid_supported();
    uint32_t rdtscp_aux = 0, rdpid_aux = 0;

    if (have_rdtscp) rdtscp_aux = host_rdtscp_aux();
    if (have_rdpid) rdpid_aux = host_rdpid_aux();
    if (have_rdtscp && have_rdpid) assert(rdtscp_aux == rdpid_aux);

    if (have_rdtscp) {
        smp_cpu_aux_method = 1;
        assert(smp_current_cpu() == expected_index_for_aux(rdtscp_aux));
    }
    if (have_rdpid) {
        smp_cpu_aux_method = 2;
        assert(smp_current_cpu() == expected_index_for_aux(rdpid_aux));
    }
}

int main(void)
{
    test_cached_cpuid_probe();
    test_forced_apic_fallback();
    test_real_aux_instructions_and_mapping();
    int method = expected_probe_method();
    uint32_t aux = host_aux_for_method(method);
    printf("PASS SMP CPU id probe/fallback; method=%d RDTSCP=%u RDPID=%u AUX=%u map=%s\n",
           method, (unsigned)host_rdtscp_supported(), (unsigned)host_rdpid_supported(), aux,
           aux != 0 && aux <= SMP_MAX_CPUS ? "aux" : "APIC fallback");
    return 0;
}
