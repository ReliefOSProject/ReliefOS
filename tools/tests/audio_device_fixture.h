#ifndef RELIEFOS_AUDIO_DEVICE_FIXTURE_H
#define RELIEFOS_AUDIO_DEVICE_FIXTURE_H
#include <limits.h>
#include <sound/asound.h>
#include <reliefnt/sched.h>
#include <reliefnt/audio.h>

/* Select the fake card's documented format through the production ioctl
 * adapter. Opening an ALSA node alone must never configure or start DMA. */
static void audio_test_configure_file(struct task_file *file)
{
    struct snd_pcm_hw_params params = {0};
    params.masks[SNDRV_PCM_HW_PARAM_ACCESS].bits[0] =
        1u << SNDRV_PCM_ACCESS_RW_INTERLEAVED;
    params.masks[SNDRV_PCM_HW_PARAM_FORMAT].bits[0] =
        1u << SNDRV_PCM_FORMAT_S16_LE;
    params.masks[SNDRV_PCM_HW_PARAM_SUBFORMAT].bits[0] = 1u;
    for (unsigned i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL -
                              SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; ++i)
        params.intervals[i] = (struct snd_interval){.max = UINT_MAX};
    params.intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE -
                     SNDRV_PCM_HW_PARAM_FIRST_INTERVAL] =
        (struct snd_interval){.min = 512, .max = 512, .integer = 1};
    params.intervals[SNDRV_PCM_HW_PARAM_BUFFER_SIZE -
                     SNDRV_PCM_HW_PARAM_FIRST_INTERVAL] =
        (struct snd_interval){.min = 2048, .max = 2048, .integer = 1};
    assert(!audio_device_ioctl(NULL, file, SNDRV_PCM_IOCTL_HW_PARAMS,
                               (uintptr_t)&params));
    assert(!audio_device_ioctl(NULL, file, SNDRV_PCM_IOCTL_PREPARE, 0));
    assert(!audio_device_ioctl(NULL, file, SNDRV_PCM_IOCTL_START, 0));
}
#endif
