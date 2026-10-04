#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sound/asound.h>

#include "audio_fixture.h"
#include "audio_fake_card.h"

#include "../../kernel/reliefnt/kernel/reliefnt/audio/core.c"
#include "../../kernel/reliefnt/kernel/reliefnt/audio/pcm.c"
#ifndef AUDIO_ALSA_IOCTL_RED
#include "../../kernel/reliefnt/kernel/reliefnt/audio/alsa_pcm.c"
#else
int audio_alsa_ioctl(struct audio_pcm *pcm, uint64_t request, void *argument);
#endif

static void audio_test_hw_any(struct snd_pcm_hw_params *params)
{
    memset(params, 0, sizeof(*params));
    for (unsigned int i = 0;
         i <= SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK; ++i)
        for (unsigned int word = 0; word < 8; ++word)
            params->masks[i].bits[word] = UINT32_MAX;
    for (unsigned int i = 0;
         i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; ++i)
        params->intervals[i] = (struct snd_interval){.min = 0, .max = UINT32_MAX};
    params->rmask = UINT32_MAX;
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

static void protocol_and_hw_params(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    int error = 0;
    struct audio_pcm *pcm = audio_pcm_open(card.id, 0, AUDIO_PLAYBACK, &error);
    assert(pcm && !error);

    struct snd_pcm_info info = {0};
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_INFO, &info));
    assert(info.device == 0 && info.stream == SNDRV_PCM_STREAM_PLAYBACK);
    assert(info.subdevices_count == 1 && info.subdevices_avail == 0);
    assert(audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_HWSYNC, NULL) == -EBADFD);
    struct snd_pcm_sw_params sw = {.avail_min = 64};
    assert(audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) == -EBADFD);
    int version = 0;
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_PVERSION, &version));
    assert(version == SNDRV_PCM_VERSION);
    int user_version = SNDRV_PCM_VERSION;
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_USER_PVERSION, &user_version));
    user_version = 0x020011;
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_USER_PVERSION, &user_version));
    int legacy_tstamp = 1234;
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_TSTAMP, &legacy_tstamp));

    struct snd_pcm_hw_params params;
    audio_test_hw_any(&params);
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_HW_REFINE, &params));
    audio_test_select(&params.masks[SNDRV_PCM_HW_PARAM_ACCESS],
                      SNDRV_PCM_ACCESS_RW_INTERLEAVED);
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
    assert(params.rate_num == 48000 && params.rate_den == 1 && params.msbits == 16);
    sw.proto = SNDRV_PCM_VERSION;
    sw.tstamp_mode = SNDRV_PCM_TSTAMP_ENABLE;
    sw.tstamp_type = SNDRV_PCM_TSTAMP_TYPE_MONOTONIC_RAW;
    sw.start_threshold = 2048;
    sw.stop_threshold = 2048;
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_SW_PARAMS, &sw));
    assert(sw.boundary >= 2048 && sw.boundary % 2048 == 0);
    sw.avail_min = 0;
    assert(audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) == -EINVAL);
    sw.avail_min = 64;
    struct snd_pcm_channel_info channel = {.channel = 1};
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_CHANNEL_INFO, &channel));
    assert(channel.offset == 0 && channel.first == 16 && channel.step == 32);
    channel.channel = 2;
    assert(audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_CHANNEL_INFO, &channel) == -EINVAL);
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_PREPARE, NULL));
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_START, NULL));

    struct snd_pcm_status status;
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_STATUS, &status));
    assert(status.state == SNDRV_PCM_STATE_RUNNING && status.appl_ptr == 0);
    snd_pcm_sframes_t delay = -1;
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_DELAY, &delay) && delay == 0);

    uint8_t samples[16] = {0};
    struct snd_xferi transfer = {.buf = samples, .frames = 4};
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &transfer));
    assert(transfer.result == 4);
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_STATUS, &status));
    assert(status.appl_ptr == 4 && status.delay == 4);
    assert(status.tstamp.tv_sec == 104 && status.tstamp.tv_nsec == 12345);
    assert(status.trigger_tstamp.tv_sec == 104 && status.trigger_tstamp.tv_nsec == 12345);
    snd_pcm_uframes_t move = 99;
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_REWIND, &move) && move == 4);
    move = 9999;
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_FORWARD, &move) && move == 2048);
    assert(audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_READI_FRAMES, &transfer) == -EBADF);
    assert(audio_alsa_ioctl(pcm, 0xdeadbeef, NULL) == -ENOTTY);

    audio_pcm_release(pcm);
    assert(!audio_unregister_card(card.id, 0));
}

static void reset_xrun_and_sync(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    int error = 0;
    struct audio_pcm *pcm = audio_pcm_open(card.id, 0, AUDIO_PLAYBACK, &error);
    assert(pcm && !error);

    struct snd_pcm_hw_params params;
    audio_test_hw_any(&params);
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_HW_REFINE, &params));
    audio_test_select(&params.masks[SNDRV_PCM_HW_PARAM_ACCESS],
                      SNDRV_PCM_ACCESS_RW_INTERLEAVED);
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
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_PREPARE, NULL));
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_START, NULL));

    uint8_t samples[16] = {0};
    struct snd_xferi transfer = {.buf = samples, .frames = 4};
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &transfer));
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_RESET, NULL));
    struct snd_pcm_status status;
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_STATUS, &status));
    assert(status.state == SNDRV_PCM_STATE_RUNNING && status.appl_ptr == 0 &&
           status.hw_ptr == 0);

    struct snd_pcm_sync_ptr sync;
    memset(&sync, 0, sizeof(sync));
    sync.flags = 0;
    sync.c.control.appl_ptr = 2;
    sync.c.control.avail_min = 3;
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_SYNC_PTR, &sync));
    assert(sync.s.status.hw_ptr == 0 && sync.c.control.appl_ptr == 2);
    memset(&sync, 0, sizeof(sync));
    sync.flags = SNDRV_PCM_SYNC_PTR_APPL | SNDRV_PCM_SYNC_PTR_AVAIL_MIN;
    sync.c.control.appl_ptr = UINT64_MAX;
    sync.c.control.avail_min = UINT64_MAX;
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_SYNC_PTR, &sync));
    assert(sync.c.control.appl_ptr == 2 && sync.c.control.avail_min == 3);
    struct snd_pcm_sync_ptr bad = {0};
    bad.c.control.appl_ptr = UINT64_MAX;
    bad.c.control.avail_min = 0;
    assert(audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_SYNC_PTR, &bad) == -EINVAL);
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_SYNC_PTR, &sync));
    assert(sync.c.control.appl_ptr == 2 && sync.c.control.avail_min == 3);
    assert(audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_HW_FREE, NULL) == -EBADFD);

    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_XRUN, NULL));
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_STATUS, &status));
    assert(status.state == SNDRV_PCM_STATE_XRUN);
    assert(audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_HW_PARAMS, &params) == -EBADFD);
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_PREPARE, NULL));
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_HW_PARAMS, &params));
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_HW_FREE, NULL));
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_STATUS, &status));
    assert(status.state == SNDRV_PCM_STATE_OPEN);
    assert(audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_PREPARE, NULL) == -EBADFD);
    assert(audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_HW_FREE, NULL) == -EBADFD);

    audio_pcm_release(pcm);
    assert(!audio_unregister_card(card.id, 0));
}

/** @brief Verify public status after underrun and wrapped/continuous progress. */
static void status_preserves_ring_availability(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    int error = 0;
    struct audio_pcm *pcm = audio_pcm_open(card.id, 0, AUDIO_PLAYBACK, &error);
    assert(pcm && !error);
    const struct audio_params params = {48000, 2, 16, 4, 512, 2048};
    assert(!audio_pcm_hw_params(pcm, &params));
    assert(!audio_pcm_prepare(pcm));
    assert(!audio_pcm_start(pcm));
    /* An IRQ has advanced 49 frames beyond the last application submission. */
    pcm->appl_ptr = 73774;
    audio_pcm_notify(pcm->stream, 73823, -EPIPE);
    struct snd_pcm_status status;
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_STATUS, &status));
    assert(status.state == SNDRV_PCM_STATE_XRUN);
    assert(status.hw_ptr == 73823 && status.appl_ptr == 73774);
    assert(status.avail == 2048 + 49 && status.delay == 0);
    snd_pcm_sframes_t delay = 123;
    assert(audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_DELAY, &delay) == -EPIPE);
    assert(delay == 123);
    /* Direct dmix slaves keep running across an empty ring. Preserve the
     * signed deficit and availability so callers can detect it themselves. */
    assert(!audio_pcm_prepare(pcm));
    assert(!audio_pcm_start(pcm));
    pcm->appl_ptr = 0;
    audio_pcm_notify(pcm->stream, 49, 0);
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_STATUS, &status));
    assert(status.state == SNDRV_PCM_STATE_RUNNING);
    assert(status.avail == 2097 && status.delay == -49);
    /* Application/hardware wrap at the advertised boundary, not ring size. */
    assert(!audio_pcm_drop(pcm));
    assert(!audio_pcm_prepare(pcm));
    assert(!audio_pcm_start(pcm));
    pcm->appl_ptr = 25;
    audio_pcm_notify(pcm->stream, pcm->boundary - 75, 0);
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_STATUS, &status));
    assert(status.avail == 1948 && status.delay == 100);
    audio_pcm_release(pcm);
    pcm = audio_pcm_open(card.id, 0, AUDIO_CAPTURE, &error);
    assert(pcm && !error);
    assert(!audio_pcm_hw_params(pcm, &params));
    assert(!audio_pcm_prepare(pcm));
    assert(!audio_pcm_start(pcm));
    audio_pcm_notify(pcm->stream, 2097, -EPIPE);
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_STATUS, &status));
    assert(status.state == SNDRV_PCM_STATE_XRUN);
    assert(status.avail == 2097 && status.delay == 0);
    audio_pcm_release(pcm);
    assert(!audio_unregister_card(card.id, 0));
}

static int explicit_caps(void *opaque, uint32_t device, enum audio_direction direction,
                         struct audio_format_caps *caps)
{
    memset(caps, 0, sizeof(*caps));
    audio_test_caps(opaque, device, direction, &caps->pcm);
    caps->pcm.formats = AUDIO_FORMAT_S32_LE | AUDIO_FORMAT_U8;
    caps->format_step_bytes = 128;
    caps->period_count_min = 2;
    caps->period_count_max = 8;
    caps->format_bits[0] = AUDIO_FORMAT_S32_LE;
    caps->subformats[0] = 1u << AUDIO_SUBFORMAT_MSBITS_24;
    caps->format_bits[1] = AUDIO_FORMAT_U8;
    caps->subformats[1] = 1u;
    return 0;
}
static int explicit_legacy_caps(void *opaque, uint32_t device,
                                enum audio_direction direction, struct audio_caps *caps)
{
    struct audio_format_caps ext;
    explicit_caps(opaque, device, direction, &ext);
    *caps = ext.pcm;
    return 0;
}
static struct audio_params_ext explicit_prepared;
static int explicit_prepare(void *opaque, struct audio_hw_stream *stream,
                            const struct audio_params_ext *params, const struct audio_dma *dma)
{
    explicit_prepared = *params;
    return audio_test_prepare(opaque, stream, &params->pcm, dma);
}
static void selected_subformat_and_geometry(void)
{
    struct audio_test_card card = {0};
    struct audio_card_ops ops = audio_test_ops;
    ops.pcm_caps = explicit_legacy_caps;
    ops.pcm_format_caps = explicit_caps;
    ops.prepare_format = explicit_prepare;
    struct audio_card_identity identity = {.id = "explicit", .name = "explicit"};
    assert(!audio_register_card_owned(&identity, &ops, &card, 0x7a, &card.id));
    int error;
    struct audio_pcm *pcm = audio_pcm_open(card.id, 0, AUDIO_PLAYBACK, &error);
    assert(pcm && !error);
    struct snd_pcm_hw_params h;
    audio_test_hw_any(&h);
    audio_test_select(&h.masks[SNDRV_PCM_HW_PARAM_ACCESS], SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    audio_test_select(&h.masks[SNDRV_PCM_HW_PARAM_FORMAT], SNDRV_PCM_FORMAT_S32_LE);
    audio_test_select(&h.masks[SNDRV_PCM_HW_PARAM_SUBFORMAT], SNDRV_PCM_SUBFORMAT_MSBITS_24);
    audio_test_single(&h, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, 512);
    audio_test_single(&h, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, 2048);
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_HW_PARAMS, &h));
    assert(h.msbits == 24);
    assert(!audio_pcm_prepare(pcm));
    assert(explicit_prepared.format == AUDIO_FORMAT_S32_LE);
    assert(explicit_prepared.subformat == AUDIO_SUBFORMAT_MSBITS_24);
    assert(explicit_prepared.significant_bits == 24);
    assert(explicit_prepared.pcm.sample_bits == 32 && explicit_prepared.pcm.frame_bytes == 8);
    audio_pcm_release(pcm);
    pcm = audio_pcm_open(card.id, 0, AUDIO_PLAYBACK, &error);
    assert(pcm);
    audio_test_hw_any(&h);
    audio_test_select(&h.masks[SNDRV_PCM_HW_PARAM_FORMAT], SNDRV_PCM_FORMAT_S32_LE);
    audio_test_select(&h.masks[SNDRV_PCM_HW_PARAM_SUBFORMAT], SNDRV_PCM_SUBFORMAT_STD);
    assert(audio_alsa_refine(pcm, &h) == -EINVAL);
    audio_test_hw_any(&h);
    audio_test_select(&h.masks[SNDRV_PCM_HW_PARAM_ACCESS], SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    audio_test_select(&h.masks[SNDRV_PCM_HW_PARAM_FORMAT], SNDRV_PCM_FORMAT_U8);
    audio_test_select(&h.masks[SNDRV_PCM_HW_PARAM_SUBFORMAT], SNDRV_PCM_SUBFORMAT_STD);
    audio_test_single(&h, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, 512);
    audio_test_single(&h, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, 2048);
    assert(!audio_alsa_ioctl(pcm, SNDRV_PCM_IOCTL_HW_PARAMS, &h));
    assert(!audio_pcm_prepare(pcm));
    assert(explicit_prepared.format == AUDIO_FORMAT_U8 && h.msbits == 8);
    audio_pcm_release(pcm);
    assert(!audio_unregister_card(card.id, 0));
    puts("PASS ALSA selected subformat, U8 signedness, explicit hardware precision");
}

static void software_start_threshold(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    int error;
    struct audio_pcm *pcm = audio_pcm_open(card.id, 0, AUDIO_PLAYBACK, &error);
    struct audio_params params = {48000, 2, 16, 4, 512, 2048};
    assert(pcm && !audio_pcm_hw_params(pcm, &params));
    struct snd_pcm_sw_params sw = {.proto = SNDRV_PCM_VERSION, .avail_min = 1,
        .start_threshold = 4, .stop_threshold = 2048};
    assert(!audio_pcm_sw_params(pcm, &sw) && !audio_pcm_prepare(pcm));
    uint8_t data[16] = {0};
    assert(audio_pcm_transfer(pcm, data, 3) == 3);
    assert(!card.trigger_count[AUDIO_START]);
    assert(audio_pcm_transfer(pcm, data, 1) == 1);
    assert(card.trigger_count[AUDIO_START] == 1);
    audio_pcm_release(pcm);
    assert(!audio_unregister_card(card.id, 0));
}

int main(void)
{
    status_preserves_ring_availability();
    software_start_threshold();
    selected_subformat_and_geometry();
    protocol_and_hw_params();
    reset_xrun_and_sync();
    puts("ALSA PCM ioctl protocol/state PASS");
    return 0;
}
