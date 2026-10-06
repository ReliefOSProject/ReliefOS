#ifndef RELIEFOS_DOOM_AUDIO_OUTPUT_H
#define RELIEFOS_DOOM_AUDIO_OUTPUT_H

#include <stdint.h>

struct doom_audio_output;

/* Hardware-clocked PCM keeps scheduler/IO stalls within the 512 KiB ring. */
#define DOOM_AUDIO_FRAGMENT_EXPONENT 13U
#define DOOM_AUDIO_FRAGMENT_COUNT 64U
#define DOOM_AUDIO_FRAGMENT_CONFIG \
    ((DOOM_AUDIO_FRAGMENT_COUNT << 16) | DOOM_AUDIO_FRAGMENT_EXPONENT)

struct doom_audio_stats {
    uint64_t written_bytes;
    uint64_t startup_recoveries;
    uint64_t xrun_recoveries;
    uint64_t suspend_recoveries;
};

struct doom_audio_io {
    long (*write_bytes)(void *opaque, const void *pcm, uint32_t bytes);
    int (*recover)(void *opaque, int error);
    long (*available_frames)(void *opaque);
    void *opaque;
};

int doom_audio_open(struct doom_audio_output **out, const char *pcm,
                    uint32_t *actual_rate);
struct doom_audio_output *doom_audio_output_create(const struct doom_audio_io *io,
                                                   uint32_t buffer_frames,
                                                   uint32_t frame_bytes);
/* Native outputs accept stereo S16 mixes and expand to negotiated channels.
 * Outputs created with custom IO accept the caller's frame_bytes layout. */
int doom_audio_submit(struct doom_audio_output *output, const int16_t *pcm,
                      uint32_t frames);
int doom_audio_flush(struct doom_audio_output *output);
int doom_audio_writable_frames(struct doom_audio_output *output, uint32_t *frames);
int doom_audio_get_stats(const struct doom_audio_output *output,
                         struct doom_audio_stats *stats);
void doom_audio_close(struct doom_audio_output *output);

#endif
