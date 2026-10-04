/* Actual legacy driver queue accounting; only port I/O is simulated. */
#define AC97_TESTING 1
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <reliefos/driver.h>
static uint8_t regs[256];
static int hardware_model;
static unsigned run_writes;
static void fetch_descriptor(void);
static uint16_t ac97_test_inw(uint16_t port)
{ uint16_t value;memcpy(&value,regs+port,sizeof value);return value; }
static void ac97_test_outw(uint16_t port,uint16_t value)
{
    if(port==0x16){uint16_t old=ac97_test_inw(port);value=old&~value;}
    memcpy(regs+port,&value,sizeof value);
}
static uint8_t read_byte(uint16_t port) { return regs[port]; }
static void write_byte(uint16_t port,uint8_t value)
{
    regs[port]=value;
    if(!hardware_model)return;
    /* QEMU AC97 nabm_writeb: LVI restarts a halted RUN engine, while
     * writing CR.RUN fetches PIV even if LVI already restarted it. */
    if(port==0x15 && (regs[0x1b]&1) && (regs[0x16]&1)){
        regs[0x16]&=~5u;regs[0x14]=regs[0x1a];regs[0x1a]=(regs[0x1a]+1)%32;
        fetch_descriptor();
    }
    if(port==0x1b && (value&1)){
        ++run_writes;regs[0x14]=regs[0x1a];regs[0x1a]=(regs[0x1a]+1)%32;
        regs[0x16]&=~1u;fetch_descriptor();
    }
}
static void write_long(uint16_t port,uint32_t value) { memcpy(regs+port,&value,sizeof value); }
static void log_text(const char *text) { (void)text; }
#ifndef AC97_QUEUE_SOURCE
#define AC97_QUEUE_SOURCE "../../kernel/reliefnt/drivers/ac97/ac97.c"
#endif
#include AC97_QUEUE_SOURCE
static void fetch_descriptor(void)
{ uint16_t picb=ac97.bdl[regs[0x14]].samples;memcpy(regs+0x18,&picb,2); }
static void terminal_lvi(void)
{
    regs[0x14]=regs[0x15];regs[0x1a]=(regs[0x14]+1)%32;
    regs[0x16]=1;regs[0x18]=regs[0x19]=0;
}
int main(void)
{
    struct reliefos_driver_kernel_api api={.inb=read_byte,.outb=write_byte,
        .outl=write_long,.console_write=log_text};
    struct ac97_buffer_desc bdl[32];uint8_t buffers[65536];uint8_t samples[2048];
    memset(samples,0x5a,sizeof samples);
    kernel_api=&api;ac97.bdl=bdl;ac97.buffers=buffers;ac97.buffers_phys=0x200000;
    ac97_reset_stream_state();
    assert(!ac97_queue_descriptor_locked(NULL,2048));
    assert(!ac97_queue_descriptor_locked(samples,2048));
    struct reliefos_audio_state state;
    ac97_get_state(&state);assert(state.queued_bytes==2048);
    assert(ac97.queued_descriptors==2 && ac97.queued_bytes==4096);
    ac97.stream_started=ac97.running=1;ac97.last_hw_civ=0;ac97.last_hw_picb=1024;
    /* A query may first observe the next PCM descriptor already half played.
     * Both its consumed prefix and the prior silent descriptor must retire. */
    regs[0x14]=1;uint16_t picb=512;memcpy(regs+0x18,&picb,2);
    ac97_get_state(&state);assert(state.queued_bytes==1024);
    picb=256;memcpy(regs+0x18,&picb,2);
    ac97_get_state(&state);assert(state.queued_bytes==512);
    picb=0;memcpy(regs+0x18,&picb,2);uint16_t status=1;memcpy(regs+0x16,&status,2);
    ac97_get_state(&state);assert(!state.queued_bytes);
    /* Refilled idle silence may occupy DMA but never prolongs user drain. */
    assert(ac97.queued_descriptors && ac97.queued_bytes);
    ac97_reset_stream_state();ac97_get_state(&state);assert(!state.queued_bytes);
    puts("actual_AC97_application_queue_excludes_idle_silence_partial_and_terminal_progress PASS");
    memset(regs,0,sizeof regs);hardware_model=1;regs[0x16]=1;
    ac97_reset_stream_state();
    for(unsigned i=0;i<4;i++)assert(!ac97_queue_descriptor_locked(samples,2048));
    assert(!ac97_start_locked());assert(regs[0x14]==0 && run_writes==1);
    for(unsigned cycle=0;cycle<12;cycle++){
        terminal_lvi();unsigned next=regs[0x1a];
        ac97_get_state(&state);
        assert(!state.queued_bytes && ac97.queued_descriptors==4);
        assert(regs[0x14]==next && ac97.last_hw_civ==next);
        assert(run_writes==1 && ac97.last_hw_picb==1024);
    }
    puts("actual_AC97_LVI_restart_never_rewrites_RUN_or_skips_descriptor_through_wrap PASS");
    hardware_model=0;memset(regs,0,sizeof regs);ac97_reset_stream_state();
    ac97.write_index=30;
    for(unsigned i=0;i<3;i++)assert(!ac97_queue_descriptor_locked(samples,2048));
    ac97.stream_started=ac97.running=1;ac97.last_hw_civ=30;ac97.last_hw_picb=1024;
    regs[0x14]=31;picb=0;memcpy(regs+0x18,&picb,2);
    ac97_get_state(&state);assert(state.queued_bytes==2048);
    regs[0x14]=0;picb=512;memcpy(regs+0x18,&picb,2);
    ac97_get_state(&state);assert(state.queued_bytes==1024 && ac97.queued_descriptors==1);
    picb=256;memcpy(regs+0x18,&picb,2);
    ac97_get_state(&state);assert(state.queued_bytes==512);
    picb=0;memcpy(regs+0x18,&picb,2);status=1;memcpy(regs+0x16,&status,2);
    ac97_get_state(&state);assert(!state.queued_bytes);
    puts("actual_AC97_partial_CIV_transition_and_zero_PICB_account_exactly_once_through_wrap PASS");
    return 0;
}
