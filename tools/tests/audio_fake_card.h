#ifndef RELIEFOS_AUDIO_FAKE_CARD_H
#define RELIEFOS_AUDIO_FAKE_CARD_H

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <reliefnt/audio.h>

#define AUDIO_TEST_STREAMS 8u

struct audio_test_stream {
    uint32_t id;
    enum audio_direction direction;
    uint64_t frames;
    uint32_t live;
    uint32_t running;
    uint32_t prepared;
};

struct audio_test_card {
    uint32_t id;
    uint32_t next_stream;
    struct audio_test_stream streams[AUDIO_TEST_STREAMS];
    struct audio_params prepared_params;
    struct audio_dma prepared_dma;
    uint32_t prepare_count;
    uint32_t trigger_count[4];
    uint32_t open_count;
    uint32_t close_count;
};

static struct audio_test_stream *audio_test_find_stream(struct audio_test_card *card,
                                                         uint32_t id)
{
    for (uint32_t i = 0; i < AUDIO_TEST_STREAMS; ++i)
        if (card->streams[i].live && card->streams[i].id == id) return &card->streams[i];
    return NULL;
}

static int audio_test_caps(void *opaque, uint32_t device, enum audio_direction direction,
                           struct audio_caps *caps)
{
    (void)opaque;
    (void)device;
    (void)direction;
    *caps = (struct audio_caps){
        .formats = AUDIO_FORMAT_S16_LE,
        .rates = AUDIO_RATE_48000,
        .channels_min = 2,
        .channels_max = 2,
        .period_bytes_min = 4,
        .period_bytes_max = 65536,
        .buffer_bytes_max = 65536,
    };
    return 0;
}

static int audio_test_open(void *opaque, uint32_t device, enum audio_direction direction,
                           struct audio_hw_stream *stream)
{
    (void)device;
    struct audio_test_card *card = opaque;
    for (uint32_t i = 0; i < AUDIO_TEST_STREAMS; ++i) {
        if (card->streams[i].live) continue;
        struct audio_test_stream *test = &card->streams[i];
        *test = (struct audio_test_stream){
            .id = ++card->next_stream,
            .direction = direction,
            .live = 1,
        };
        stream->id = test->id;
        stream->driver = test;
        ++card->open_count;
        return 0;
    }
    return -ENOSPC;
}

static int audio_test_prepare(void *opaque, struct audio_hw_stream *stream,
                              const struct audio_params *params,
                              const struct audio_dma *dma)
{
    struct audio_test_card *card = opaque;
    struct audio_test_stream *test = audio_test_find_stream(card, stream->id);
    if (!test) return -ENODEV;
    card->prepared_params = *params;
    card->prepared_dma = *dma;
    test->prepared = 1;
    test->frames = 0;
    ++card->prepare_count;
    return 0;
}

static int audio_test_trigger(void *opaque, struct audio_hw_stream *stream,
                              enum audio_trigger trigger)
{
    struct audio_test_card *card = opaque;
    struct audio_test_stream *test = audio_test_find_stream(card, stream->id);
    if (!test) return -ENODEV;
    assert((uint32_t)trigger < 4u);
    ++card->trigger_count[trigger];
    if (trigger == AUDIO_START || trigger == AUDIO_UNPAUSE) test->running = 1;
    if (trigger == AUDIO_STOP || trigger == AUDIO_PAUSE) test->running = 0;
    return 0;
}

static int audio_test_pointer(void *opaque, struct audio_hw_stream *stream, uint64_t *frames)
{
    struct audio_test_card *card = opaque;
    struct audio_test_stream *test = audio_test_find_stream(card, stream->id);
    if (!test) return -ENODEV;
    *frames = test->frames;
    return 0;
}

static void audio_test_close(void *opaque, struct audio_hw_stream *stream)
{
    struct audio_test_card *card = opaque;
    struct audio_test_stream *test = audio_test_find_stream(card, stream->id);
    assert(test);
    test->live = 0;
    ++card->close_count;
}

static uint32_t audio_test_controls(void *opaque)
{
    (void)opaque;
    return 0;
}

static struct audio_card_ops audio_test_ops = {
    .version = AUDIO_CARD_OPS_VERSION,
    .size = sizeof(audio_test_ops),
    .pcm_caps = audio_test_caps,
    .open = audio_test_open,
    .prepare = audio_test_prepare,
    .trigger = audio_test_trigger,
    .pointer = audio_test_pointer,
    .close = audio_test_close,
    .control_count = audio_test_controls,
};

static void audio_test_card_init(struct audio_test_card *card)
{
    memset(card, 0, sizeof(*card));
    struct audio_card_identity identity = {.id = "fake", .name = "audio-test"};
    assert(!audio_register_card_owned(&identity, &audio_test_ops, card, 0x7au, &card->id));
}

static void audio_test_advance(struct audio_test_card *card, uint64_t frames, int error)
{
    for (uint32_t i = 0; i < AUDIO_TEST_STREAMS; ++i) {
        struct audio_test_stream *test = &card->streams[i];
        if (!test->live || !test->running) continue;
        test->frames += frames;
        audio_period_elapsed(card->id, test->id, test->frames, error);
    }
}

#endif
