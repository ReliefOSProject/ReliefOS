/* Exercise the actual ES1371 transport; only its port registers are simulated. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <reliefos/driver.h>
static uint32_t regs[64];
static uint32_t read_long(uint16_t port) { return regs[port / 4U]; }
static void write_long(uint16_t port, uint32_t value) { regs[port / 4U] = value; }
static void log_text(const char *text) { (void)text; }
#ifndef ES1371_QUEUE_SOURCE
#define ES1371_QUEUE_SOURCE "../../kernel/reliefnt/drivers/es1371/es1371.c"
#endif
#include ES1371_QUEUE_SOURCE
static void cursor(uint32_t offset) { regs[ES_REG_DAC1_SIZE / 4U] = offset << 14; }
static int all_zero(const uint8_t *p, unsigned n)
{ for(unsigned i=0;i<n;i++)if(p[i])return 0;return 1; }
int main(void)
{
    struct reliefos_driver_kernel_api api = {.inl=read_long, .outl=write_long,
        .console_write=log_text};
    uint8_t ring[ES1371_DMA_BYTES] = {0}, pcm[2048];
    memset(pcm, 0x5a, sizeof pcm);
    kernel_api = &api;es1371.dma_buffer=ring;es1371.dma_configured=1;
    es1371.active=es1371.present=1;es1371.sample_rate=48000;
    es1371.dma_last_cursor=ES1371_DMA_BYTES-2048U;
    es1371_recover_empty(es1371.dma_last_cursor);
    struct reliefos_audio_state state;
    es1371_get_state(&state);
    assert(state.queued_bytes==0); /* Idle lead is never application backlog. */
    assert(!es1371_queue_block(pcm,sizeof pcm));
    es1371_get_state(&state);assert(state.queued_bytes==sizeof pcm);
    es1371.dma_running=1;
    cursor(0);es1371_get_state(&state);assert(state.queued_bytes==sizeof pcm);
    cursor(3072);es1371_get_state(&state);assert(state.queued_bytes==1024);
    /* The consumed half must be silent when the cyclic DMA ring revisits it. */
    assert(all_zero(ring+2048,1024));
    assert(!all_zero(ring+3072,1024));
    cursor(4096);es1371_get_state(&state);assert(!state.queued_bytes);
    assert(all_zero(ring,sizeof ring));
    cursor(8192);es1371_get_state(&state);assert(!state.queued_bytes);
    assert(all_zero(ring,sizeof ring));
    puts("actual_ES1371_idle_lead_excluded_partial_wrap_drain_and_no_stale_PCM_replay PASS");
    return 0;
}
