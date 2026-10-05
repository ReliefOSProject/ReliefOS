#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sound/asound.h>

#include "audio_fixture.h"
#include "audio_fake_card.h"

#include "../../kernel/reliefnt/kernel/reliefnt/audio/core.c"
#include "../../kernel/reliefnt/kernel/reliefnt/audio/pcm.c"
#include "../../kernel/reliefnt/kernel/reliefnt/audio/alsa_pcm.c"

int audio_alsa_refine(struct audio_pcm *pcm, struct snd_pcm_hw_params *params);

static void audio_test_hw_any(struct snd_pcm_hw_params *params)
{
    memset(params, 0, sizeof(*params));
    for (unsigned int i = 0;
         i <= SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK; ++i)
        for (unsigned int word = 0; word < 8; ++word)
            params->masks[i].bits[word] = UINT32_MAX;
    for (unsigned int i = 0;
         i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; ++i)
        params->intervals[i] = (struct snd_interval){
            .min = 0,
            .max = UINT_MAX,
        };
    params->rmask = UINT_MAX;
}

static int audio_test_mask_has(const struct snd_mask *mask, unsigned int bit)
{
    return bit < SNDRV_MASK_MAX &&
           (mask->bits[bit / 32u] & (1u << (bit % 32u))) != 0;
}

static struct snd_interval *audio_test_interval(struct snd_pcm_hw_params *params,
                                                 unsigned int parameter)
{
    return &params->intervals[parameter - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

static void refine_supported_constraints(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    int error = 0;
    struct audio_pcm *pcm = audio_pcm_open(card.id, 0, AUDIO_PLAYBACK, &error);
    assert(pcm && error == 0);

    struct snd_pcm_hw_params params;
    audio_test_hw_any(&params);
    assert(!audio_alsa_refine(pcm, &params));

    assert(audio_test_mask_has(&params.masks[SNDRV_PCM_HW_PARAM_ACCESS],
                               SNDRV_PCM_ACCESS_RW_INTERLEAVED));
    assert(audio_test_mask_has(&params.masks[SNDRV_PCM_HW_PARAM_ACCESS],
                               SNDRV_PCM_ACCESS_MMAP_INTERLEAVED));
    assert(!audio_test_mask_has(&params.masks[SNDRV_PCM_HW_PARAM_ACCESS],
                                SNDRV_PCM_ACCESS_RW_NONINTERLEAVED));
    assert(audio_test_mask_has(&params.masks[SNDRV_PCM_HW_PARAM_FORMAT],
                               SNDRV_PCM_FORMAT_S16_LE));
    assert(!audio_test_mask_has(&params.masks[SNDRV_PCM_HW_PARAM_FORMAT],
                                SNDRV_PCM_FORMAT_S32_LE));
    assert(audio_test_interval(&params, SNDRV_PCM_HW_PARAM_RATE)->min == 48000);
    assert(audio_test_interval(&params, SNDRV_PCM_HW_PARAM_RATE)->max == 48000);
    assert(audio_test_interval(&params, SNDRV_PCM_HW_PARAM_CHANNELS)->min == 2);
    assert(audio_test_interval(&params, SNDRV_PCM_HW_PARAM_CHANNELS)->max == 2);
    assert(audio_test_interval(&params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS)->min == 16);
    assert(audio_test_interval(&params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS)->max == 16);
    assert(audio_test_interval(&params, SNDRV_PCM_HW_PARAM_FRAME_BITS)->min == 32);
    assert(audio_test_interval(&params, SNDRV_PCM_HW_PARAM_FRAME_BITS)->max == 32);
    assert(params.cmask & (1u << SNDRV_PCM_HW_PARAM_FORMAT));
    assert(params.cmask & (1u << SNDRV_PCM_HW_PARAM_RATE));

    struct snd_interval *period_bytes =
        audio_test_interval(&params, SNDRV_PCM_HW_PARAM_PERIOD_BYTES);
    assert(period_bytes->min == 4 && period_bytes->max == 65536);
    struct snd_interval *buffer_bytes =
        audio_test_interval(&params, SNDRV_PCM_HW_PARAM_BUFFER_BYTES);
    assert(buffer_bytes->min == 4 && buffer_bytes->max == 65536);

    struct snd_pcm_hw_params empty = params;
    audio_test_interval(&empty, SNDRV_PCM_HW_PARAM_RATE)->min = 44100;
    audio_test_interval(&empty, SNDRV_PCM_HW_PARAM_RATE)->max = 44100;
    assert(audio_alsa_refine(pcm, &empty) == -EINVAL);

    audio_pcm_release(pcm);
    assert(!audio_unregister_card(card.id, 0));
}

static void refine_period_buffer_propagation(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    int error = 0;
    struct audio_pcm *pcm = audio_pcm_open(card.id, 0, AUDIO_CAPTURE, &error);
    assert(pcm && error == 0);

    struct snd_pcm_hw_params params;
    audio_test_hw_any(&params);
    audio_test_interval(&params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min = 512;
    audio_test_interval(&params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->max = 512;
    audio_test_interval(&params, SNDRV_PCM_HW_PARAM_PERIODS)->min = 4;
    audio_test_interval(&params, SNDRV_PCM_HW_PARAM_PERIODS)->max = 4;
    assert(!audio_alsa_refine(pcm, &params));
    assert(audio_test_interval(&params, SNDRV_PCM_HW_PARAM_PERIOD_BYTES)->min == 2048);
    assert(audio_test_interval(&params, SNDRV_PCM_HW_PARAM_PERIOD_BYTES)->max == 2048);
    assert(audio_test_interval(&params, SNDRV_PCM_HW_PARAM_BUFFER_SIZE)->min == 2048);
    assert(audio_test_interval(&params, SNDRV_PCM_HW_PARAM_BUFFER_SIZE)->max == 2048);
    assert(audio_test_interval(&params, SNDRV_PCM_HW_PARAM_BUFFER_BYTES)->min == 8192);
    assert(audio_test_interval(&params, SNDRV_PCM_HW_PARAM_BUFFER_BYTES)->max == 8192);

    audio_pcm_release(pcm);
    assert(!audio_unregister_card(card.id, 0));
}

static void refine_time_geometry(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    int error = 0;
    struct audio_pcm *pcm = audio_pcm_open(card.id, 0, AUDIO_PLAYBACK, &error);
    assert(pcm && !error);

    struct snd_pcm_hw_params params;
    audio_test_hw_any(&params);
    *audio_test_interval(&params, SNDRV_PCM_HW_PARAM_RATE) =
        (struct snd_interval){.min=48000, .max=48000, .integer=1};
    *audio_test_interval(&params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE) =
        (struct snd_interval){.min=512, .max=512, .integer=1};
    assert(!audio_alsa_refine(pcm, &params));
    struct snd_interval *period_time =
        audio_test_interval(&params, SNDRV_PCM_HW_PARAM_PERIOD_TIME);
    assert(period_time->min == 10666 && period_time->max == 10667);
    assert(!period_time->integer && period_time->openmin && period_time->openmax);

    /* A direct plugin copies this fractional interval into its shared slave
     * constraints. Advertising integer microseconds makes ALSA's soft refine
     * reject an otherwise valid 1024-frame / 48 kHz configuration. */
    audio_test_hw_any(&params);
    *audio_test_interval(&params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE) =
        (struct snd_interval){.min=1024, .max=1024, .integer=1};
    *audio_test_interval(&params, SNDRV_PCM_HW_PARAM_BUFFER_SIZE) =
        (struct snd_interval){.min=12288, .max=12288, .integer=1};
    assert(!audio_alsa_refine(pcm, &params));
    period_time = audio_test_interval(&params, SNDRV_PCM_HW_PARAM_PERIOD_TIME);
    assert(period_time->min == 21333 && period_time->max == 21334);
    assert(!period_time->integer && period_time->openmin && period_time->openmax);
    struct snd_interval *buffer_time =
        audio_test_interval(&params, SNDRV_PCM_HW_PARAM_BUFFER_TIME);
    assert(buffer_time->min == 256000 && buffer_time->max == 256000);
    assert(buffer_time->integer && !buffer_time->openmin && !buffer_time->openmax);
    struct snd_pcm_hw_params stable = params;
    assert(!audio_alsa_refine(pcm, &params));
    assert(!memcmp(stable.intervals, params.intervals, sizeof(params.intervals)));
    assert(params.cmask == 0);

    audio_test_hw_any(&params);
    *audio_test_interval(&params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE) =
        (struct snd_interval){.min=1024, .max=1024, .integer=1};
    *audio_test_interval(&params, SNDRV_PCM_HW_PARAM_PERIOD_TIME) =
        (struct snd_interval){.min=21333, .max=21334, .integer=1};
    assert(audio_alsa_refine(pcm, &params) == -EINVAL);

    audio_test_hw_any(&params);
    *audio_test_interval(&params, SNDRV_PCM_HW_PARAM_PERIOD_TIME) =
        (struct snd_interval){.min=21333, .max=21334, .openmin=1, .openmax=1};
    assert(!audio_alsa_refine(pcm, &params));
    assert(audio_test_interval(&params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min == 1024);
    assert(audio_test_interval(&params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->max == 1024);

    audio_test_hw_any(&params);
    *audio_test_interval(&params, SNDRV_PCM_HW_PARAM_RATE) =
        (struct snd_interval){.min=48000, .max=48000, .integer=1};
    *audio_test_interval(&params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE) =
        (struct snd_interval){.min=512, .max=512, .integer=1};
    *audio_test_interval(&params, SNDRV_PCM_HW_PARAM_PERIOD_TIME) =
        (struct snd_interval){.min=5001, .max=5001, .integer=1};
    assert(audio_alsa_refine(pcm, &params) == -EINVAL);

    audio_pcm_release(pcm);
    assert(!audio_unregister_card(card.id, 0));
    puts("ALSA period/buffer time geometry PASS");
}

static void refine_fixed_point_and_open_bounds(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    int error = 0;
    struct audio_pcm *pcm = audio_pcm_open(card.id, 0, AUDIO_PLAYBACK, &error);
    assert(pcm && !error);
    struct snd_pcm_hw_params h;
    audio_test_hw_any(&h);
    *audio_test_interval(&h, SNDRV_PCM_HW_PARAM_PERIOD_BYTES) =
        (struct snd_interval){.min=2048, .max=2048, .integer=1};
    *audio_test_interval(&h, SNDRV_PCM_HW_PARAM_BUFFER_BYTES) =
        (struct snd_interval){.min=8192, .max=8192, .integer=1};
    assert(!audio_alsa_refine(pcm, &h));
    assert(audio_test_interval(&h, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min == 512);
    assert(audio_test_interval(&h, SNDRV_PCM_HW_PARAM_PERIODS)->min == 4);
    assert(audio_test_interval(&h, SNDRV_PCM_HW_PARAM_PERIODS)->max == 4);
    struct snd_pcm_hw_params stable = h;
    assert(!audio_alsa_refine(pcm, &h));
    assert(h.cmask == 0);
    assert(!memcmp(stable.intervals,h.intervals,sizeof(h.intervals)));
    audio_test_hw_any(&h);
    *audio_test_interval(&h, SNDRV_PCM_HW_PARAM_RATE) =
        (struct snd_interval){.min=48000,.max=48000,.openmin=1};
    assert(audio_alsa_refine(pcm, &h) == -EINVAL);
    audio_test_hw_any(&h);
    *audio_test_interval(&h, SNDRV_PCM_HW_PARAM_PERIOD_BYTES) =
        (struct snd_interval){.min=2049,.max=2049,.integer=1};
    assert(audio_alsa_refine(pcm, &h) == -EINVAL);
    audio_pcm_release(pcm);
    assert(!audio_unregister_card(card.id,0));
    puts("ALSA fixed-point, exclusive interval, byte/frame contradiction PASS");
}

int main(void)
{
    refine_fixed_point_and_open_bounds();
    refine_supported_constraints();
    refine_period_buffer_propagation();
    refine_time_geometry();
    puts("ALSA PCM parameter refine PASS");
    return 0;
}
