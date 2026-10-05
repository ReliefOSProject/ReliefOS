#ifndef RELIEFOS_DOOM_AUDIO_MIXER_H
#define RELIEFOS_DOOM_AUDIO_MIXER_H

#include <stdint.h>

#define DOOM_AUDIO_MIXER_MAX_CHANNELS 32U

struct doom_audio_mixer_sample {
    const int16_t *pcm;
    uint32_t frames;
    uint32_t source_rate;
};

struct doom_audio_mixer_channel {
    const struct doom_audio_mixer_sample *sample;
    uint64_t phase;
    uint64_t step;
    uint16_t volume;
    uint16_t separation;
    uint8_t active;
};

struct doom_audio_mixer {
    uint32_t output_rate;
    uint32_t channels;
    struct doom_audio_mixer_channel voices[DOOM_AUDIO_MIXER_MAX_CHANNELS];
};

int doom_audio_mixer_init(struct doom_audio_mixer *mixer, uint32_t output_rate,
                          uint32_t channels);
int doom_audio_mixer_start(struct doom_audio_mixer *mixer, uint32_t slot,
                           const struct doom_audio_mixer_sample *sample,
                           uint16_t volume, uint16_t separation);
void doom_audio_mixer_stop(struct doom_audio_mixer *mixer, uint32_t slot);
uint32_t doom_audio_mixer_render(struct doom_audio_mixer *mixer, int16_t *out,
                                 uint32_t frames);

#endif
