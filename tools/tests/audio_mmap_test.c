#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include <sound/asound.h>

#include "audio_fixture.h"
#include "audio_fake_card.h"

/* The mmap fixture links the PCM core without the ALSA timer module. */
void audio_timer_period_elapsed(uint32_t card, uint32_t device,
                                enum audio_direction direction,
                                uint64_t frames, int error)
{
    (void)card; (void)device; (void)direction; (void)frames; (void)error;
}

#include "../../kernel/reliefnt/kernel/reliefnt/audio/core.c"
#include "../../kernel/reliefnt/kernel/reliefnt/audio/pcm.c"
#include "../../kernel/reliefnt/kernel/reliefnt/audio/alsa_pcm.c"

static void audio_test_hw_any(struct snd_pcm_hw_params *params)
{
    memset(params, 0, sizeof(*params));
    for (unsigned int i = 0;
         i <= SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK; ++i)
        for (unsigned int word = 0; word < 8; ++word)
            params->masks[i].bits[word] = UINT32_MAX;
    for (unsigned int i = 0;
         i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; ++i)
        params->intervals[i] = (struct snd_interval){.min = 0, .max = UINT_MAX};
    params->rmask = UINT_MAX;
}

static void audio_test_single(struct snd_pcm_hw_params *params,
                              unsigned int parameter, unsigned int value)
{
    params->intervals[parameter - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL] =
        (struct snd_interval){.min = value, .max = value, .integer = 1};
}

static void audio_test_select(struct snd_mask *mask, unsigned int value)
{
    memset(mask, 0, sizeof(*mask));
    mask->bits[value / 32u] = 1u << (value % 32u);
}

static void audio_test_configure(struct audio_pcm *pcm)
{
    struct snd_pcm_hw_params params;
    audio_test_hw_any(&params);
    assert(!audio_alsa_refine(pcm, &params));
    audio_test_select(&params.masks[SNDRV_PCM_HW_PARAM_ACCESS],
                      SNDRV_PCM_ACCESS_MMAP_INTERLEAVED);
    audio_test_select(&params.masks[SNDRV_PCM_HW_PARAM_FORMAT],
                      SNDRV_PCM_FORMAT_S16_LE);
    audio_test_select(&params.masks[SNDRV_PCM_HW_PARAM_SUBFORMAT],
                      SNDRV_PCM_SUBFORMAT_STD);
    audio_test_single(&params, SNDRV_PCM_HW_PARAM_RATE, 48000);
    audio_test_single(&params, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
    audio_test_single(&params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16);
    audio_test_single(&params, SNDRV_PCM_HW_PARAM_FRAME_BITS, 32);
    audio_test_single(&params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, 512);
    audio_test_single(&params, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 2048);
    audio_test_single(&params, SNDRV_PCM_HW_PARAM_PERIODS, 4);
    audio_test_single(&params, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, 2048);
    audio_test_single(&params, SNDRV_PCM_HW_PARAM_BUFFER_BYTES, 8192);
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_HW_PARAMS, &params));
}

static void mmap_contract(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    int error = 0;
    struct audio_pcm *pcm = audio_pcm_open(card.id, 0, AUDIO_PLAYBACK, &error);
    assert(pcm && !error);
    audio_test_configure(pcm);

    struct audio_pcm_mmap mapping;
    assert(!audio_pcm_mmap_acquire(pcm, SNDRV_PCM_MMAP_OFFSET_DATA,
                                   8192, PROT_READ | PROT_WRITE, &mapping));
    assert(mapping.region == AUDIO_PCM_MMAP_DATA);
    assert(mapping.length == 8192);
    assert(mapping.generation != 0);
    assert(audio_pcm_mmap_acquire(pcm, SNDRV_PCM_MMAP_OFFSET_DATA + 1,
                                  4096, PROT_READ | PROT_WRITE, &mapping) == -EINVAL);
    assert(audio_pcm_mmap_acquire(pcm, SNDRV_PCM_MMAP_OFFSET_DATA,
                                  UINT64_MAX, PROT_READ | PROT_WRITE, &mapping) == -EINVAL);
    assert(audio_pcm_mmap_acquire(pcm, SNDRV_PCM_MMAP_OFFSET_STATUS,
                                  4096, PROT_READ | PROT_WRITE, &mapping) == -EPERM);
    assert(!audio_pcm_mmap_acquire(pcm, SNDRV_PCM_MMAP_OFFSET_STATUS,
                                   4096, PROT_READ, &mapping));
    audio_pcm_mmap_release(pcm,AUDIO_PCM_MMAP_STATUS);
    assert(!audio_pcm_mmap_acquire(pcm, SNDRV_PCM_MMAP_OFFSET_CONTROL,
                                   4096, PROT_READ | PROT_WRITE, &mapping));
    audio_pcm_mmap_release(pcm,AUDIO_PCM_MMAP_CONTROL);

    struct audio_params selected = {
        .rate = 48000,
        .channels = 2,
        .sample_bits = 16,
        .frame_bytes = 4,
        .period_frames = 512,
        .buffer_frames = 2048,
    };
    assert(audio_pcm_hw_params(pcm, &selected) == -EBADFD);
    audio_pcm_mmap_release(pcm,AUDIO_PCM_MMAP_DATA);
    assert(!audio_pcm_hw_params(pcm, &selected));

    assert(!audio_pcm_mmap_acquire(pcm, SNDRV_PCM_MMAP_OFFSET_DATA,
                                   8192, PROT_READ | PROT_WRITE, &mapping));
    audio_pcm_release(pcm);
    error = 0;
    assert(!audio_pcm_open(card.id, 0, AUDIO_PLAYBACK, &error));
    assert(error == -EBUSY);
    audio_pcm_mmap_release(pcm,AUDIO_PCM_MMAP_DATA);
    pcm = audio_pcm_open(card.id, 0, AUDIO_PLAYBACK, &error);
    assert(pcm && !error);
    audio_pcm_release(pcm);
    assert(!audio_unregister_card(card.id, 0));
}

static void metadata_pages_and_lifetime(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    int error = 0;
    struct audio_pcm *pcm = audio_pcm_open(card.id,0,AUDIO_PLAYBACK,&error);
    assert(pcm && !error);
    struct audio_pcm_mmap status, control;
    assert(!audio_pcm_mmap_acquire(pcm,SNDRV_PCM_MMAP_OFFSET_STATUS,4096,PROT_READ,&status));
    assert(status.backing && !(status.backing & 4095u));
    struct __snd_pcm_mmap_status *st = (void *)(uintptr_t)status.backing;
    assert(st->state == SNDRV_PCM_STATE_OPEN);
    assert(!audio_pcm_mmap_acquire(pcm,SNDRV_PCM_MMAP_OFFSET_CONTROL,4096,PROT_READ,&control));
    assert(control.backing && !(control.backing & 4095u));
    audio_test_configure(pcm); /* metadata mappings do not pin the DMA config */
    assert(st->state == SNDRV_PCM_STATE_SETUP);
    assert(!audio_pcm_prepare(pcm));
    assert(st->state == SNDRV_PCM_STATE_PREPARED);
    struct __snd_pcm_mmap_control *ct = (void *)(uintptr_t)control.backing;
    ct->appl_ptr = 1024;
    ct->avail_min = 64;
    struct audio_pcm_status snap;
    assert(!audio_pcm_status(pcm,&snap));
    assert(snap.appl_ptr == 1024 && snap.avail_min == 64);
    assert(!audio_pcm_start(pcm));
    audio_test_advance(&card,512,0);
    assert(st->state == SNDRV_PCM_STATE_RUNNING && st->hw_ptr == 512);
    assert(!audio_pcm_drop(pcm));
    assert(!audio_alsa_ioctl(pcm,SNDRV_PCM_IOCTL_HW_FREE,NULL));
    assert(st->state == SNDRV_PCM_STATE_OPEN);
    audio_pcm_release(pcm);
    assert(st->state == SNDRV_PCM_STATE_OPEN);
    assert(!audio_pcm_open(card.id,0,AUDIO_PLAYBACK,&error) && error == -EBUSY);
    audio_pcm_mmap_release(pcm,AUDIO_PCM_MMAP_STATUS);
    assert(!audio_pcm_open(card.id,0,AUDIO_PLAYBACK,&error) && error == -EBUSY);
    audio_pcm_mmap_release(pcm,AUDIO_PCM_MMAP_CONTROL);
    pcm = audio_pcm_open(card.id,0,AUDIO_PLAYBACK,&error);
    assert(pcm && !error);
    audio_test_configure(pcm);
    assert(!audio_pcm_mmap_acquire(pcm,SNDRV_PCM_MMAP_OFFSET_STATUS_NEW,4096,PROT_READ,&status));
    st = (void *)(uintptr_t)status.backing;
    assert(!audio_unregister_card(card.id,1));
    assert(st->state == SNDRV_PCM_STATE_DISCONNECTED);
    audio_pcm_release(pcm);
    audio_pcm_mmap_release(pcm,AUDIO_PCM_MMAP_STATUS);
    puts("ALSA real metadata pages, IRQ publication, HW_FREE and disconnect PASS");
}

static void continuous_slave_control_pointer(void)
{
    for (unsigned direction = AUDIO_PLAYBACK; direction <= AUDIO_CAPTURE; ++direction) {
        struct audio_test_card card;
        audio_test_card_init(&card);
        int error = 0;
        struct audio_pcm *pcm = audio_pcm_open(card.id, 0, direction, &error);
        assert(pcm && !error);
        audio_test_configure(pcm);
        struct snd_pcm_sw_params sw = {.avail_min = 512, .start_threshold = 1,
            .stop_threshold = pcm->boundary};
        assert(!audio_pcm_sw_params(pcm, &sw));
        struct audio_pcm_mmap control;
        assert(!audio_pcm_mmap_acquire(pcm, SNDRV_PCM_MMAP_OFFSET_CONTROL,
                                       4096, PROT_READ | PROT_WRITE, &control));
        struct __snd_pcm_mmap_control *ct = (void *)(uintptr_t)control.backing;
        assert(!audio_pcm_prepare(pcm) && !audio_pcm_start(pcm));
        audio_test_advance(&card, 4096, 0);
        /* dmix/dsnoop slaves deliberately leave appl_ptr behind the hardware. */
        assert(!audio_pcm_hwsync(pcm));
        assert(pcm->state == AUDIO_PCM_RUNNING && ct->appl_ptr == 0);
        struct snd_pcm_sync_ptr sync = {.flags = SNDRV_PCM_SYNC_PTR_AVAIL_MIN};
        sync.c.control.appl_ptr = 1;
        assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_SYNC_PTR, &sync));
        assert(sync.s.status.hw_ptr == 4096 && sync.c.control.appl_ptr == 1);
        ct->appl_ptr = pcm->boundary;
        assert(audio_pcm_hwsync(pcm) == -EINVAL);
        assert(pcm->appl_ptr == 1 && pcm->state == AUDIO_PCM_RUNNING);
        ct->appl_ptr = 1;
        audio_pcm_mmap_release(pcm, AUDIO_PCM_MMAP_CONTROL);
        audio_pcm_release(pcm);
        assert(!audio_unregister_card(card.id, 0));
    }
    puts("ALSA continuous dmix/dsnoop slave pointer and boundary validation PASS");
}

int main(void)
{
    continuous_slave_control_pointer();
    metadata_pages_and_lifetime();
    mmap_contract();
    puts("ALSA PCM mmap contract PASS");
    return 0;
}
