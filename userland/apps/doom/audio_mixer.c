#include "audio_mixer.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

static int16_t doom_audio_mix_clamp(int64_t value)
{
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    return (int16_t)value;
}

int doom_audio_mixer_init(struct doom_audio_mixer *mixer, uint32_t output_rate,
                          uint32_t channels)
{
    if (!mixer || !output_rate || channels != 2U) {
        return -EINVAL;
    }
    memset(mixer, 0, sizeof(*mixer));
    mixer->output_rate = output_rate;
    mixer->channels = channels;
    return 0;
}

int doom_audio_mixer_start(struct doom_audio_mixer *mixer, uint32_t slot,
                           const struct doom_audio_mixer_sample *sample,
                           uint16_t volume, uint16_t separation)
{
    struct doom_audio_mixer_channel *voice;
    if (!mixer || slot >= DOOM_AUDIO_MIXER_MAX_CHANNELS || !sample ||
        !sample->pcm || !sample->frames || !sample->source_rate ||
        volume > 127U || separation > 254U) {
        return -EINVAL;
    }
    voice = &mixer->voices[slot];
    voice->sample = sample;
    voice->phase = 0;
    voice->step = ((uint64_t)sample->source_rate << 32) / mixer->output_rate;
    if (!voice->step) {
        voice->step = 1;
    }
    voice->volume = volume;
    voice->separation = separation;
    voice->active = 1U;
    return 0;
}

void doom_audio_mixer_stop(struct doom_audio_mixer *mixer, uint32_t slot)
{
    if (mixer && slot < DOOM_AUDIO_MIXER_MAX_CHANNELS) {
        mixer->voices[slot].active = 0;
    }
}

uint32_t doom_audio_mixer_render(struct doom_audio_mixer *mixer, int16_t *out,
                                 uint32_t frames)
{
    uint32_t frame;
    uint32_t slot;
    if (!mixer || !out || mixer->channels != 2U) {
        return 0;
    }
    memset(out, 0, (size_t)frames * 2U * sizeof(*out));
    for (frame = 0; frame < frames; ++frame) {
        int64_t left = 0;
        int64_t right = 0;
        for (slot = 0; slot < DOOM_AUDIO_MIXER_MAX_CHANNELS; ++slot) {
            struct doom_audio_mixer_channel *voice = &mixer->voices[slot];
            const struct doom_audio_mixer_sample *sample;
            uint32_t index;
            uint32_t fraction;
            int64_t a;
            int64_t b;
            int64_t value;
            if (!voice->active || !voice->sample) {
                continue;
            }
            sample = voice->sample;
            index = (uint32_t)(voice->phase >> 32);
            if (index >= sample->frames) {
                voice->active = 0;
                continue;
            }
            fraction = (uint32_t)voice->phase;
            a = sample->pcm[index];
            b = index + 1U < sample->frames ? sample->pcm[index + 1U] : a;
            value = a + ((b - a) * fraction >> 32);
            value = value * voice->volume;
            left += value * (254U - voice->separation);
            right += value * voice->separation;
            voice->phase += voice->step;
            if ((voice->phase >> 32) >= sample->frames) {
                voice->active = 0;
            }
        }
        out[frame * 2U] = doom_audio_mix_clamp(left / (127LL * 254LL));
        out[frame * 2U + 1U] = doom_audio_mix_clamp(right / (127LL * 254LL));
    }
    return frames;
}
