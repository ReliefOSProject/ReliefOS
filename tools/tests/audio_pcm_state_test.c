#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_fixture.h"
#include "audio_fake_card.h"

#include "../../kernel/reliefnt/kernel/reliefnt/audio/core.c"
#include "../../kernel/reliefnt/kernel/reliefnt/audio/pcm.c"

/* The PCM fixture links the core directly and does not include timer.c. */
void audio_timer_period_elapsed(uint32_t card, uint32_t device,
                                enum audio_direction direction,
                                uint64_t frames, int error)
{
    (void)card; (void)device; (void)direction; (void)frames; (void)error;
}

static void playback_state_and_xrun(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    int error = 0;
    struct audio_pcm *pcm = audio_pcm_open(card.id, 0, AUDIO_PLAYBACK, &error);
    assert(pcm && error == 0);
    assert(audio_pcm_start(pcm) == -EBADFD);

    struct audio_params params = {48000, 2, 16, 4, 512, 2048};
    assert(!audio_pcm_hw_params(pcm, &params));
    assert(!audio_pcm_prepare(pcm));
    int16_t samples[4096] = {0};
    assert(audio_pcm_transfer(pcm, samples, 2048) == 2048);
    assert(audio_pcm_transfer(pcm, samples, 1) == -EAGAIN);
    assert(!audio_pcm_start(pcm));
    audio_test_advance(&card, 512, 0);
    assert(audio_pcm_transfer(pcm, samples, 512) == 512);
    audio_test_advance(&card, 4096, -EPIPE);
    assert(audio_pcm_transfer(pcm, samples, 1) == -EPIPE);
    assert(!audio_pcm_prepare(pcm));
    audio_pcm_release(pcm);
    assert(card.close_count == 1);
    assert(card.trigger_count[AUDIO_START] == 1);
    assert(card.trigger_count[AUDIO_STOP] >= 1);
    assert(!audio_unregister_card(card.id, 0));
}

static void capture_empty_and_overrun(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    int error = 0;
    struct audio_pcm *pcm = audio_pcm_open(card.id, 0, AUDIO_CAPTURE, &error);
    assert(pcm && !error);
    struct audio_params params = {48000, 2, 16, 4, 512, 2048};
    assert(!audio_pcm_hw_params(pcm, &params) && !audio_pcm_prepare(pcm));
    int16_t samples[4096] = {0};
    assert(audio_pcm_transfer(pcm, samples, 1) == -EAGAIN);
    assert(!audio_pcm_start(pcm));
    audio_test_advance(&card, 512, 0);
    assert(audio_pcm_transfer(pcm, samples, 512) == 512);
    audio_test_advance(&card, 4096, -EPIPE);
    assert(audio_pcm_transfer(pcm, samples, 1) == -EPIPE);
    audio_pcm_release(pcm);
    assert(!audio_unregister_card(card.id, 0));
}

static void state_guards_and_disconnect(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    int error = 0;
    struct audio_pcm *first = audio_pcm_open(card.id, 0, AUDIO_PLAYBACK, &error);
    assert(first && !error);
    struct audio_pcm *duplicate = audio_pcm_open(card.id, 0, AUDIO_PLAYBACK, &error);
    assert(!duplicate && error == -EBUSY);
    struct audio_params params = {48000, 2, 16, 4, 512, 2048};
    assert(!audio_pcm_hw_params(first, &params) && !audio_pcm_prepare(first));
    assert(!audio_pcm_start(first));
    assert(audio_pcm_start(first) == -EBUSY);
    assert(!audio_pcm_pause(first, true));
    assert(card.trigger_count[AUDIO_PAUSE] == 1);
    assert(!audio_pcm_pause(first, false));
    assert(card.trigger_count[AUDIO_UNPAUSE] == 1);
    assert(!audio_unregister_card(card.id, 1));
    int16_t sample = 0;
    assert(audio_pcm_transfer(first, &sample, 1) == -ENODEV);
    audio_pcm_release(first);
}

static void drain_link_overflow_and_boundary(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    int error = 0;
    struct audio_pcm *playback = audio_pcm_open(card.id, 0, AUDIO_PLAYBACK, &error);
    assert(playback && !error);
    struct audio_pcm *capture = audio_pcm_open(card.id, 0, AUDIO_CAPTURE, &error);
    assert(capture && !error);
    struct audio_params params = {48000, 2, 16, 4, 512, 2048};
    assert(!audio_pcm_hw_params(playback, &params) && !audio_pcm_prepare(playback));
    assert(!audio_pcm_hw_params(capture, &params) && !audio_pcm_prepare(capture));
    assert(!audio_pcm_link(playback, capture));
    assert(!audio_pcm_start(playback));
    assert(card.trigger_count[AUDIO_START] == 2);

    int16_t samples[4096] = {0};
    assert(audio_pcm_transfer(playback, samples, 512) == 512);
    assert(audio_pcm_drain(playback) == -EAGAIN);
    audio_test_advance(&card, 512, 0);
    assert(!audio_pcm_drain(playback));
    assert(card.trigger_count[AUDIO_STOP] >= 1);
    assert(!audio_pcm_unlink(playback));

    struct audio_params overflow = {48000, UINT32_MAX, 16, 0, 512, 2048};
    assert(audio_pcm_hw_params(playback, &overflow) == -EINVAL);
    audio_pcm_release(capture);
    audio_pcm_release(playback);
    assert(!audio_unregister_card(card.id, 0));

    audio_test_card_init(&card);
    struct audio_pcm *wrapped = audio_pcm_open(card.id, 0, AUDIO_PLAYBACK, &error);
    assert(wrapped && !error);
    assert(!audio_pcm_hw_params(wrapped, &params) && !audio_pcm_prepare(wrapped));
    assert(audio_pcm_transfer(wrapped, samples, 2048) == 2048);
    assert(!audio_pcm_start(wrapped));
    audio_test_advance(&card, AUDIO_PCM_BOUNDARY_BASE + 512, 0);
    assert(audio_pcm_transfer(wrapped, samples, 512) == 512);
    audio_pcm_release(wrapped);
    assert(!audio_unregister_card(card.id, 0));
}

static void silence_preserves_application_accounting(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    int error = 0;
    struct audio_pcm *pcm = audio_pcm_open(card.id, 0, AUDIO_PLAYBACK, &error);
    struct audio_params params = {48000, 2, 16, 4, 512, 2048};
    assert(pcm && !audio_pcm_hw_params(pcm, &params) && !audio_pcm_prepare(pcm));
    struct snd_pcm_sw_params sw = {.avail_min = 1, .start_threshold = 2048,
        .stop_threshold = pcm->boundary, .silence_threshold = 768, .silence_size = 256};
    assert(!audio_pcm_sw_params(pcm, &sw));
    int16_t samples[2048];
    memset(samples, 0x5a, sizeof(samples));
    memset(pcm->buffer, 0x7b, pcm->dma.bytes);
    assert(audio_pcm_transfer(pcm, samples, 512) == 512);
    assert(!audio_pcm_start(pcm));
    struct audio_pcm_status status;
    assert(!audio_pcm_status(pcm, &status));
    assert(status.appl_ptr == 512 && status.hw_ptr == 0);
    assert(pcm->buffer[511 * 4] == 0x5a);
    assert(pcm->buffer[512 * 4] == 0 && pcm->buffer[767 * 4] == 0);
    assert(pcm->buffer[768 * 4] == 0x7b);
    assert(!audio_pcm_status(pcm, &status));
    assert(status.appl_ptr == 512 && pcm->buffer[768 * 4] == 0x7b);
    audio_test_advance(&card, 128, 0);
    assert(!audio_pcm_status(pcm, &status) && status.appl_ptr == 512);
    assert(pcm->buffer[895 * 4] == 0 && pcm->buffer[896 * 4] == 0x7b);
    struct audio_pcm_mmap control;
    assert(!audio_pcm_mmap_acquire(pcm, AUDIO_PCM_MMAP_OFFSET_CONTROL_NEW, 4096, 3, &control));
    memset(pcm->buffer + 512 * 4, 0x5a, 512 * 4);
    struct __snd_pcm_mmap_control *ct = (void *)(uintptr_t)control.backing;
    __atomic_store_n(&ct->appl_ptr, 1024, __ATOMIC_RELEASE);
    audio_test_advance(&card, 128, 0);
    assert(!audio_pcm_status(pcm, &status) && status.appl_ptr == 1024);
    assert(pcm->buffer[1023 * 4] == 0x5a);
    audio_pcm_mmap_release(pcm, AUDIO_PCM_MMAP_CONTROL);
    assert(!audio_pcm_drop(pcm) && !audio_pcm_prepare(pcm));
    sw.silence_threshold = 0; sw.silence_size = pcm->boundary;
    assert(!audio_pcm_sw_params(pcm, &sw));
    assert(audio_pcm_transfer(pcm, samples, 512) == 512);
    memset(pcm->buffer + 512 * 4, 0x7b, (2048 - 512) * 4);
    assert(!audio_pcm_start(pcm));
    assert(!audio_pcm_status(pcm, &status) && status.appl_ptr == 512);
    assert(pcm->buffer[511 * 4] == 0x5a && pcm->buffer[512 * 4] == 0);
    audio_test_advance(&card, 128, 0);
    assert(pcm->buffer[127 * 4] == 0 && pcm->buffer[128 * 4] == 0x5a);
    audio_pcm_release(pcm);
    assert(!audio_unregister_card(card.id, 0));
}

static void drain_partial_period_completion(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    int error = 0;
    struct audio_pcm *pcm = audio_pcm_open(card.id, 0, AUDIO_PLAYBACK, &error);
    struct audio_params params = {48000, 2, 16, 4, 512, 2048};
    assert(pcm && !audio_pcm_hw_params(pcm, &params) && !audio_pcm_prepare(pcm));
    int16_t samples[1536]; memset(samples, 0x5a, sizeof(samples));
    memset(pcm->buffer, 0x7b, pcm->dma.bytes);
    assert(audio_pcm_transfer(pcm, samples, 768) == 768);
    assert(!audio_pcm_start(pcm));
    assert(audio_pcm_drain(pcm) == -EAGAIN);
    audio_test_advance(&card, 1024, 0);
    assert(!audio_pcm_drain(pcm));
    assert(pcm->state == AUDIO_PCM_SETUP && !pcm->error);
    assert(pcm->buffer[767*4] == 0x5a);
    assert(pcm->buffer[768*4] == 0 && pcm->buffer[1023*4] == 0);
    assert(pcm->buffer[1024*4] == 0 && pcm->buffer[2047*4] == 0);
    audio_pcm_release(pcm);
    assert(!audio_unregister_card(card.id, 0));
}
static void drain_full_ring_preserves_unplayed_samples(void)
{
    struct audio_test_card card;audio_test_card_init(&card);int error=0;
    struct audio_pcm *pcm=audio_pcm_open(card.id,0,AUDIO_PLAYBACK,&error);
    struct audio_params params={48000,2,16,4,512,2048};
    assert(pcm && !audio_pcm_hw_params(pcm,&params) && !audio_pcm_prepare(pcm));
    int16_t samples[4096];memset(samples,0x5a,sizeof samples);
    assert(audio_pcm_transfer(pcm,samples,768)==768 && !audio_pcm_start(pcm));
    audio_test_advance(&card,256,0);
    assert(audio_pcm_transfer(pcm,samples,1536)==1536);
    assert(audio_pcm_drain(pcm)==-EAGAIN);
    assert(pcm->buffer[256*4]==0x5a && pcm->buffer[511*4]==0x5a);
    audio_test_advance(&card,512,0);
    assert(audio_pcm_drain(pcm)==-EAGAIN);
    assert(pcm->buffer[256*4]==0 && pcm->buffer[511*4]==0);
    assert(pcm->buffer[768*4]==0x5a);
    audio_test_advance(&card,1792,0);assert(!audio_pcm_drain(pcm));
    audio_pcm_release(pcm);assert(!audio_unregister_card(card.id,0));
}
/** @brief Drain silences the entire unsubmitted ring, retaining queued frames.
 * @return None; asserts partial/aligned ends and wrapping without overwriting queued data.
 */
static void drain_unsubmitted_ring_is_silent(void)
{
    for (unsigned mode = 0; mode < 3; ++mode) {
        struct audio_test_card card;
        audio_test_card_init(&card);
        int error = 0;
        struct audio_pcm *pcm = audio_pcm_open(card.id, 0, AUDIO_PLAYBACK, &error);
        struct audio_params params = {48000, 2, 16, 4, 512, 2048};
        assert(pcm && !error && !audio_pcm_hw_params(pcm, &params));
        assert(!audio_pcm_prepare(pcm));
        uint8_t samples[8192];
        memset(samples, 0x5a, sizeof(samples));
        memset(audio_pcm_buffer(pcm), 0x7b, pcm->dma.bytes);
        uint32_t written = mode == 1 ? 1024 : mode == 2 ? 2048 : 768;
        assert(audio_pcm_transfer(pcm, samples, written) == written);
        assert(!audio_pcm_start(pcm));
        if (mode == 2) {
            audio_test_advance(&card, 1536, 0);
            assert(audio_pcm_transfer(pcm, samples, 1000) == 1000);
        }
        uint64_t submitted = pcm->appl_ptr;
        assert(audio_pcm_drain(pcm) == -EAGAIN);
        assert(pcm->appl_ptr == submitted);
        uint32_t begin = mode == 2 ? 1000 : written;
        uint32_t end = mode == 2 ? 1536 : 2048;
        for (uint32_t frame = 0; frame < 2048; ++frame) {
            uint8_t expected = frame >= begin && frame < end
                ? 0 : 0x5a;
            for (uint32_t byte = 0; byte < params.frame_bytes; ++byte)
                assert(audio_pcm_buffer(pcm)[frame * params.frame_bytes + byte] == expected);
        }
        audio_pcm_release(pcm);
        assert(!audio_unregister_card(card.id, 0));
    }
    puts("DRAIN partial/aligned/wrapped: all unsubmitted frames silent, queued data retained PASS");
}

static int drain_stop_failure;
static struct audio_pcm *fault_during_close;
static enum audio_pcm_state fault_notification_state;
static int drain_trigger(void *opaque,struct audio_hw_stream *stream,enum audio_trigger trigger)
{
    if(trigger==AUDIO_STOP && fault_during_close){
        struct audio_pcm *pcm=fault_during_close;fault_during_close=NULL;
        assert(find_stream(pcm->stream)->detached);
        assert(!audio_request_controller_disconnect(((struct audio_test_card *)opaque)->id));
        fault_notification_state=pcm->state;
        return -EIO;
    }
    if(trigger==AUDIO_STOP && drain_stop_failure){--drain_stop_failure;return -EIO;}
    return audio_test_trigger(opaque,stream,trigger);
}
static void fatal_during_failed_close_preserves_disconnected_mapping(void)
{
    struct audio_test_card card;audio_test_card_init(&card);int error=0;
    cards[card.id&SLOT_MASK].ops.trigger=drain_trigger;
    struct audio_pcm *pcm=audio_pcm_open(card.id,0,AUDIO_PLAYBACK,&error);
    struct audio_params params={48000,2,16,4,512,2048};
    assert(pcm && !audio_pcm_hw_params(pcm,&params) && !audio_pcm_prepare(pcm));
    struct audio_pcm_mmap mapping;
    assert(!audio_pcm_mmap_acquire(pcm,0,8192,AUDIO_PCM_PROT_READ|AUDIO_PCM_PROT_WRITE,&mapping));
    fault_during_close=pcm;audio_pcm_release(pcm);
    assert(fault_notification_state==AUDIO_PCM_DISCONNECTED);
    assert(pcm->state==AUDIO_PCM_DISCONNECTED);
    assert(pcm->error==-ENODEV);
    assert(((struct __snd_pcm_mmap_status *)(void *)pcm->status_page)->state==SNDRV_PCM_STATE_DISCONNECTED);
    /* A period notifier may have captured its generation before the IRQ
     * disconnect closed admission, then arrive after the fatal notifier. */
    uint64_t stopped_pointer=pcm->hw_ptr;
    audio_pcm_notify(pcm->stream,64,-EPIPE);
    assert(pcm->state==AUDIO_PCM_DISCONNECTED && pcm->hw_ptr==stopped_pointer);
    audio_pcm_notify(pcm->stream,128,0);
    assert(pcm->state==AUDIO_PCM_DISCONNECTED && pcm->hw_ptr==stopped_pointer);
    assert(pcm->live && pcm->close_pending && !pcm->file_refs);
    assert(find_stream(pcm->stream) && !find_stream(pcm->stream)->detached);
    assert(audio_process_pending_disconnects(16)==1);
    assert(audio_pcm_retry_releases(32)==1);
    uint64_t flags;kernel_execution_lock_irqsave(&flags);
    audio_pcm_finish_releases();kernel_execution_unlock_irqrestore(flags);
    assert(pcm->live && !pcm->close_pending && pcm->state==AUDIO_PCM_DISCONNECTED);
    audio_pcm_mmap_release(pcm,AUDIO_PCM_MMAP_DATA);
    assert(!pcm->live && !audio_pcm_release_work_pending() && !audio_disconnect_work_pending());
    puts("fatal_during_failed_close_keeps_DISCONNECTED_mmap_and_DMA_lease PASS");
}
static uint32_t fault_on_callback;
static int fault_callback_result;
static int fault_prepare(void *opaque,struct audio_hw_stream *stream,
                         const struct audio_params *params,const struct audio_dma *dma)
{
    int ret=audio_test_prepare(opaque,stream,params,dma);
    if(fault_on_callback){fault_on_callback=0;
        assert(!audio_request_controller_disconnect(((struct audio_test_card *)opaque)->id));
        return fault_callback_result;}
    return ret;
}
static int fault_trigger(void *opaque,struct audio_hw_stream *stream,enum audio_trigger trigger)
{
    int ret=audio_test_trigger(opaque,stream,trigger);
    if(fault_on_callback){fault_on_callback=0;
        assert(!audio_request_controller_disconnect(((struct audio_test_card *)opaque)->id));
        return fault_callback_result;}
    return ret;
}
static void fatal_callback_cannot_resurrect_pcm_state(void)
{
    const char *names[]={"prepare","start","pause","unpause","drop","hw_free","drain","xrun","auto_start_transfer"};
    unsigned failures=0;
    for(unsigned mode=0;mode<2;mode++)for(unsigned op=0;op<9;op++){
        fault_callback_result=0;
        struct audio_test_card card;audio_test_card_init(&card);int error=0;
        cards[card.id&SLOT_MASK].ops.prepare=fault_prepare;
        cards[card.id&SLOT_MASK].ops.trigger=fault_trigger;
        struct audio_pcm *pcm=audio_pcm_open(card.id,0,AUDIO_PLAYBACK,&error);
        struct audio_params params={48000,2,16,4,512,2048};
        assert(pcm && !audio_pcm_hw_params(pcm,&params));
        if(op)assert(!audio_pcm_prepare(pcm));
        if(op==2 || op==3 || op==4 || op==6)assert(!audio_pcm_start(pcm));
        if(op==3)assert(!audio_pcm_pause(pcm,true));
        struct audio_pcm_mmap status;
        assert(!audio_pcm_mmap_acquire(pcm,AUDIO_PCM_MMAP_OFFSET_STATUS_NEW,4096,AUDIO_PCM_PROT_READ,&status));
        fault_on_callback=op!=7 || mode;
        fault_callback_result=mode?-EIO:0;
        if(op==7 && !mode)assert(!audio_request_controller_disconnect(card.id));
        if(op==8){pcm->auto_start=true;pcm->start_threshold=1;}
        int ret;
        switch(op){
        case 0:ret=audio_pcm_prepare(pcm);break;
        case 1:ret=audio_pcm_start(pcm);break;
        case 2:ret=audio_pcm_pause(pcm,true);break;
        case 3:ret=audio_pcm_pause(pcm,false);break;
        case 4:ret=audio_pcm_drop(pcm);break;
        case 5:ret=audio_pcm_hw_free(pcm);break;
        case 6:ret=audio_pcm_drain(pcm);break;
        case 7:ret=audio_pcm_xrun(pcm);break;
        default:{int16_t samples[2]={1,2};ret=audio_pcm_transfer(pcm,samples,1);break;}
        }
        int pass=ret==(op==8?1:-ENODEV) && pcm->state==AUDIO_PCM_DISCONNECTED && pcm->error==-ENODEV &&
            ((struct __snd_pcm_mmap_status *)(uintptr_t)status.backing)->state==SNDRV_PCM_STATE_DISCONNECTED;
        printf("fatal callback %s mode=%u: ret=%d state=%u error=%d %s\n",names[op],mode,ret,pcm->state,pcm->error,pass?"PASS":"FAIL");
        failures+=!pass;
        assert(!fault_on_callback && audio_process_pending_disconnects(16)==1);
        audio_pcm_release(pcm);audio_pcm_mmap_release(pcm,AUDIO_PCM_MMAP_STATUS);
        assert(!pcm->live && !audio_disconnect_work_pending() && !audio_pcm_release_work_pending());
    }
    fflush(stdout);assert(!failures);
}
static unsigned link_fault_mode;
static int link_fault_trigger(void *opaque,struct audio_hw_stream *stream,enum audio_trigger trigger)
{
    struct audio_test_stream *test=stream->driver;
    if(trigger==AUDIO_START && test->direction==AUDIO_CAPTURE)return -EIO;
    if(trigger==AUDIO_STOP && test->direction==AUDIO_PLAYBACK && link_fault_mode){
        unsigned mode=link_fault_mode;link_fault_mode=0;
        if(mode==1){
            assert(!audio_request_controller_disconnect(((struct audio_test_card *)opaque)->id));
            return audio_test_trigger(opaque,stream,trigger);
        }
        return -EIO;
    }
    return audio_test_trigger(opaque,stream,trigger);
}
static void linked_start_rollback_keeps_fatal_or_failed_stop_state(void)
{
    for(unsigned mode=1;mode<=2;mode++){
        struct audio_test_card card;audio_test_card_init(&card);int error=0;
        cards[card.id&SLOT_MASK].ops.trigger=link_fault_trigger;
        struct audio_pcm *pcm=audio_pcm_open(card.id,0,AUDIO_PLAYBACK,&error);
        struct audio_pcm *peer=audio_pcm_open(card.id,0,AUDIO_CAPTURE,&error);
        struct audio_params params={48000,2,16,4,512,2048};
        assert(pcm && peer && !audio_pcm_hw_params(pcm,&params) && !audio_pcm_hw_params(peer,&params));
        assert(!audio_pcm_prepare(pcm) && !audio_pcm_prepare(peer) && !audio_pcm_link(pcm,peer));
        link_fault_mode=mode;int ret=audio_pcm_start(pcm);
        if(mode==1){
            assert(ret==-ENODEV && pcm->state==AUDIO_PCM_DISCONNECTED && peer->state==AUDIO_PCM_DISCONNECTED);
            assert(audio_process_pending_disconnects(16)==1);
        }else{
            assert(ret==-EIO && pcm->state==AUDIO_PCM_XRUN && pcm->error==-EIO);
            assert(((struct audio_test_stream *)find_stream(pcm->stream)->hw.driver)->running);
            assert(pcm->dma.kernel && audio_pcm_hw_params(pcm,&params)==-EBADFD);
            assert(!audio_pcm_drop(pcm));
        }
        audio_pcm_release(peer);audio_pcm_release(pcm);
        if(mode==2)assert(!audio_unregister_card(card.id,0));
    }
    puts("linked_start_rollback_disconnect_and_STOP_failure_retain_safe_state PASS");
}
static void drain_stop_retry_and_real_underflow(void)
{
    for(unsigned mode=0;mode<3;mode++){
        struct audio_test_card card;audio_test_card_init(&card);int error=0;
        cards[card.id&SLOT_MASK].ops.trigger=drain_trigger;
        struct audio_pcm *pcm=audio_pcm_open(card.id,0,AUDIO_PLAYBACK,&error);
        struct audio_params params={48000,2,16,4,512,2048};
        assert(pcm && !audio_pcm_hw_params(pcm,&params) && !audio_pcm_prepare(pcm));
        int16_t samples[1536]={0};
        assert(audio_pcm_transfer(pcm,samples,768)==768 && !audio_pcm_start(pcm));
        if(mode!=1)assert(audio_pcm_drain(pcm)==-EAGAIN);
        audio_test_advance(&card,mode==2?4096:1024,0);
        if(!mode){
            drain_stop_failure=1;
            assert(audio_pcm_drain(pcm)==-EIO && pcm->state==AUDIO_PCM_DRAINING);
            assert(pcm->dma.kernel && pcm->buffer);
            assert(!audio_pcm_drain(pcm) && pcm->state==AUDIO_PCM_SETUP);
        }else assert(audio_pcm_drain(pcm)==-EPIPE && pcm->state==AUDIO_PCM_XRUN);
        audio_pcm_release(pcm);assert(!audio_unregister_card(card.id,0));
    }
}
static void failed_final_close_keeps_dma_backing_after_last_munmap(void)
{
    struct audio_test_card card;audio_test_card_init(&card);int error=0;
    cards[card.id&SLOT_MASK].ops.trigger=drain_trigger;
    struct audio_pcm *pcm=audio_pcm_open(card.id,0,AUDIO_PLAYBACK,&error);
    struct audio_params params={48000,2,16,4,512,2048};
    assert(pcm && !audio_pcm_hw_params(pcm,&params) && !audio_pcm_prepare(pcm));
    assert(!audio_pcm_start(pcm));
    struct audio_pcm_mmap mapping;
    assert(!audio_pcm_mmap_acquire(pcm,0,8192,AUDIO_PCM_PROT_READ|AUDIO_PCM_PROT_WRITE,&mapping));
    uint32_t stream=pcm->stream;pcm->buffer[0]=0x5a;
    drain_stop_failure=2;audio_pcm_release(pcm);
    assert(find_stream(stream)->live && !find_stream(stream)->detached);
    audio_pcm_mmap_release(pcm,AUDIO_PCM_MMAP_DATA);
    assert(pcm->live && !pcm->file_refs && !pcm->mapping_refs && pcm->buffer[0]==0x5a);
    assert(!audio_pcm_open(card.id,0,AUDIO_PLAYBACK,&error) && error==-EBUSY);
    assert(audio_pcm_release_work_pending());
    assert(!audio_pcm_retry_releases(0));
    drain_stop_failure=1;
    assert(audio_pcm_retry_releases(1)==1 && pcm->live && pcm->close_pending);
    assert(card.close_count==0 && pcm->buffer[0]==0x5a);
    drain_stop_failure=0;
    assert(audio_pcm_retry_releases(1)==1 && pcm->live && !find_stream(stream));
    assert(pcm->close_pending==3 && audio_pcm_release_work_pending());
    assert(!audio_pcm_open(card.id,0,AUDIO_PLAYBACK,&error) && error==-EBUSY);
    uint64_t flags;kernel_execution_lock_irqsave(&flags);
    audio_pcm_finish_releases();kernel_execution_unlock_irqrestore(flags);
    assert(!pcm->live);
    assert(!audio_pcm_release_work_pending() && card.close_count==1);
    assert(!audio_unregister_card(card.id,0));
    puts("failed_final_close_retains_DMA_after_last_munmap PASS");
}

static unsigned recovery_stop_fault;
static int recovery_trigger(void *opaque, struct audio_hw_stream *stream,
                             enum audio_trigger trigger)
{
    if (trigger==AUDIO_STOP && recovery_stop_fault) {
        if (recovery_stop_fault==2)
            assert(!audio_request_controller_disconnect(((struct audio_test_card *)opaque)->id));
        return -EIO;
    }
    return audio_test_trigger(opaque,stream,trigger);
}

static void failed_recovery_stop_keeps_old_dma(void)
{
    for (unsigned fault=1;fault<=2;++fault) {
        struct audio_test_card card;
        audio_test_ops.trigger=recovery_trigger; audio_test_card_init(&card);
        audio_test_ops.trigger=audio_test_trigger;
        int error; struct audio_pcm *pcm=audio_pcm_open(card.id,0,AUDIO_CAPTURE,&error);
        assert(pcm && !error);
        struct audio_params params={48000,2,16,4,512,2048};
        assert(!audio_pcm_hw_params(pcm,&params) && !audio_pcm_prepare(pcm));
        assert(!audio_pcm_start(pcm));
        audio_test_advance(&card,512,-EPIPE);
        audio_pcm_buffer(pcm)[0]=0x5a;
        unsigned prepared=card.prepare_count;
        recovery_stop_fault=fault;
        assert(audio_pcm_prepare(pcm)==(fault==2 ? -ENODEV : -EIO));
        assert(card.prepare_count==prepared && card.streams[0].running &&
               pcm->hw_ptr==512 && audio_pcm_buffer(pcm)[0]==0x5a &&
               pcm->state==(fault==2 ? AUDIO_PCM_DISCONNECTED : AUDIO_PCM_XRUN));
        recovery_stop_fault=0;
        if (fault==2)assert(audio_process_pending_disconnects(16)==1);
        audio_pcm_release(pcm);
        if (fault==1)assert(!audio_unregister_card(card.id,0));
    }
    puts("failed XRUN recovery STOP retains DMA/pointers; fatal disconnect dominates PASS");
}

static void irq_xrun_and_stale_period_reprepare(void)
{
    unsigned failures = 0;
    for (unsigned fault = 0; fault < 2; ++fault) {
        struct audio_test_card card; audio_test_card_init(&card);
        int error; struct audio_pcm *pcm=audio_pcm_open(card.id,0,AUDIO_CAPTURE,&error);
        assert(pcm && !error);
        struct audio_params params={48000,2,16,4,512,2048};
        assert(!audio_pcm_hw_params(pcm,&params) && !audio_pcm_prepare(pcm));
        assert(!audio_pcm_start(pcm));
        /* The actual IRQ notifier updates PCM immediately. Leave its core
         * coalesced event unconsumed across DROP/PREPARE, or deliver XRUN and
         * recover directly as ALSA does. Old-run frames/errors must not leak. */
        audio_test_advance(&card,8192,fault ? -EPIPE : 0);
        if (!fault) assert(!audio_pcm_drop(pcm));
        int ret=audio_pcm_prepare(pcm);
        struct audio_pcm_status status; assert(!audio_pcm_status(pcm,&status));
        int pass=!ret && !card.streams[0].running && status.state==AUDIO_PCM_PREPARED &&
                 !status.hw_ptr && !status.appl_ptr && !status.error;
        printf("reprepare %s: ret=%d hardware_running=%u state=%u hw=%llu appl=%llu error=%d %s\n",
               fault ? "IRQ-XRUN" : "DROP-old-period",ret,card.streams[0].running,status.state,
               (unsigned long long)status.hw_ptr,(unsigned long long)status.appl_ptr,status.error,
               pass ? "PASS" : "FAIL");
        failures+=!pass;
        audio_pcm_release(pcm);assert(!audio_unregister_card(card.id,0));
    }
    fflush(stdout);assert(!failures);
}

static int hwsync_pointer_fault;
static int failing_hwsync_pointer(void *opaque, struct audio_hw_stream *stream,
                                 uint64_t *frames)
{
    struct audio_test_card *card=opaque;
    (void)stream;(void)frames;
    if(hwsync_pointer_fault==2) assert(!audio_request_controller_disconnect(card->id));
    return -EIO;
}

static void hwsync_failure_quiesces_without_revival(void)
{
    for(hwsync_pointer_fault=1;hwsync_pointer_fault<=2;++hwsync_pointer_fault) {
        struct audio_test_card card={0};
        struct audio_card_ops ops=audio_test_ops;
        ops.pointer=failing_hwsync_pointer;
        struct audio_card_identity identity={.id="sync-fault",.name="sync-fault"};
        assert(!audio_register_card_owned(&identity,&ops,&card,0x7au,&card.id));
        int error=0;
        struct audio_pcm *pcm=audio_pcm_open(card.id,0,AUDIO_CAPTURE,&error);
        struct audio_params params={48000,2,16,4,512,2048};
        assert(pcm && !error && !audio_pcm_hw_params(pcm,&params));
        assert(!audio_pcm_prepare(pcm) && !audio_pcm_start(pcm));
        ((unsigned char*)pcm->dma.kernel)[0]=0x5a;
        assert(audio_pcm_hwsync(pcm)==(hwsync_pointer_fault==2 ? -ENODEV : -EIO));
        assert(((unsigned char*)pcm->dma.kernel)[0]==0x5a);
        assert(pcm->state==(hwsync_pointer_fault==2 ? AUDIO_PCM_DISCONNECTED : AUDIO_PCM_XRUN));
        if(hwsync_pointer_fault==1) assert(card.trigger_count[AUDIO_STOP]==1);
        else assert(audio_process_pending_disconnects(16)==1);
        audio_pcm_release(pcm);
        if(hwsync_pointer_fault==1) assert(!audio_unregister_card(card.id,0));
    }
    puts("HWSYNC pointer failure retains DMA, stops XRUN and preserves fatal disconnect PASS");
}

static void hwsync_reads_progress_between_irqs(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    int error=0;
    struct audio_pcm *pcm=audio_pcm_open(card.id,0,AUDIO_CAPTURE,&error);
    assert(pcm && !error);
    struct audio_params params={48000,2,16,4,512,2048};
    assert(!audio_pcm_hw_params(pcm,&params) && !audio_pcm_prepare(pcm));
    assert(!audio_pcm_start(pcm));
    struct audio_test_stream *hw=audio_test_find_stream(&card,find_stream(pcm->stream)->hw.id);
    assert(hw);
    hw->frames=240; /* Real pointer advances without a period IRQ. */
    assert(!audio_pcm_hwsync(pcm));
    struct audio_pcm_status st;
    assert(!audio_pcm_status(pcm,&st) && st.hw_ptr==240);
    /* A coalesced older IRQ must not undo the fresh pointer observation. */
    audio_period_elapsed(card.id,pcm->stream,128,0);
    assert(!audio_pcm_status(pcm,&st) && st.hw_ptr==240);
    hw->frames=480;
    assert(!audio_pcm_sync(pcm,SNDRV_PCM_SYNC_PTR_HWSYNC |
                           SNDRV_PCM_SYNC_PTR_APPL | SNDRV_PCM_SYNC_PTR_AVAIL_MIN,0,0,&st));
    assert(st.hw_ptr==480);
    /* Driver frames remain cumulative while the public ALSA pointer wraps. */
    hw->frames=pcm->boundary+240;
    assert(!audio_pcm_hwsync(pcm));
    assert(!audio_pcm_status(pcm,&st) && st.hw_ptr==240);
    audio_period_elapsed(card.id,pcm->stream,pcm->boundary-128,0);
    assert(!audio_pcm_status(pcm,&st) && st.hw_ptr==240);
    audio_pcm_release(pcm);
    assert(!audio_unregister_card(card.id,0));
    puts("HWSYNC/SYNC_PTR fresh sub-period progress and older IRQ monotonicity PASS");
}

int main(void)
{
    drain_unsubmitted_ring_is_silent();
    hwsync_failure_quiesces_without_revival();
    hwsync_reads_progress_between_irqs();
    irq_xrun_and_stale_period_reprepare();
    failed_recovery_stop_keeps_old_dma();
    fatal_callback_cannot_resurrect_pcm_state();
    linked_start_rollback_keeps_fatal_or_failed_stop_state();
    fatal_during_failed_close_preserves_disconnected_mapping();
    failed_final_close_keeps_dma_backing_after_last_munmap();
    drain_stop_retry_and_real_underflow();
    drain_full_ring_preserves_unplayed_samples();drain_partial_period_completion();
    silence_preserves_application_accounting();
    playback_state_and_xrun();
    capture_empty_and_overrun();
    state_guards_and_disconnect();
    drain_link_overflow_and_boundary();
    puts("PCM state, transfer, XRUN, drain, LINK, lifetime PASS");
    return 0;
}
