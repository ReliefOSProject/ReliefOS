#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../userland/apps/doom/audio_mixer.h"

static void test_linear_and_pan(void)
{
    static const int16_t source_data[] = {0, 1000, 2000, 3000};
    struct doom_audio_mixer_sample sample = {
        .pcm = source_data, .frames = 4, .source_rate = 24000
    };
    struct doom_audio_mixer mixer;
    int16_t out[8];
    assert(doom_audio_mixer_init(&mixer, 48000, 2) == 0);
    assert(doom_audio_mixer_start(&mixer, 0, &sample, 127, 0) == 0);
    assert(doom_audio_mixer_render(&mixer, out, 4) == 4);
    assert(out[0] == 0 && out[2] == 500 && out[4] == 1000 && out[6] == 1500);
    assert(out[1] == 0 && out[3] == 0 && out[5] == 0 && out[7] == 0);
}

static void test_long_phase_and_clamp(void)
{
    enum { sample_frames = 70000, render_frames = 65540 };
    int16_t *data = (int16_t *)malloc((size_t)sample_frames * sizeof(*data));
    int16_t *out = (int16_t *)malloc((size_t)render_frames * 2U * sizeof(*out));
    struct doom_audio_mixer_sample sample;
    struct doom_audio_mixer mixer;
    unsigned int i;
    assert(data && out);
    for (i = 0; i < sample_frames; ++i) data[i] = 1234;
    sample.pcm = data;
    sample.frames = sample_frames;
    sample.source_rate = 48000;
    assert(doom_audio_mixer_init(&mixer, 48000, 2) == 0);
    assert(doom_audio_mixer_start(&mixer, 0, &sample, 127, 254) == 0);
    assert(doom_audio_mixer_render(&mixer, out, render_frames) == render_frames);
    assert(out[(render_frames - 1U) * 2U] == 0);
    assert(out[(render_frames - 1U) * 2U + 1U] == 1234);
    doom_audio_mixer_stop(&mixer, 0);
    assert(doom_audio_mixer_start(&mixer, 1, &sample, 127, 254) == 0);
    assert(doom_audio_mixer_start(&mixer, 2, &sample, 127, 254) == 0);
    data[0] = 30000;
    data[1] = 30000;
    assert(doom_audio_mixer_render(&mixer, out, 1) == 1);
    assert(out[1] == 32767);
    free(out);
    free(data);
}

static void test_overlap(void)
{
    static const int16_t left_data[] = {10000, 10000};
    static const int16_t right_data[] = {-10000, -10000};
    struct doom_audio_mixer_sample left = {.pcm = left_data, .frames = 2,
                                           .source_rate = 44100};
    struct doom_audio_mixer_sample right = {.pcm = right_data, .frames = 2,
                                            .source_rate = 44100};
    struct doom_audio_mixer mixer;
    int16_t out[4];
    assert(doom_audio_mixer_init(&mixer, 48000, 2) == 0);
    assert(doom_audio_mixer_start(&mixer, 0, &left, 127, 0) == 0);
    assert(doom_audio_mixer_start(&mixer, 1, &right, 127, 254) == 0);
    assert(doom_audio_mixer_render(&mixer, out, 2) == 2);
    assert(out[0] == 10000 && out[1] == -10000);
}

int main(void)
{
    test_linear_and_pan();
    test_long_phase_and_clamp();
    test_overlap();
    puts("Doom mixer PASS");
    return 0;
}
