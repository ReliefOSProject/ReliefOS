#include <reliefos/audio_test.h>
#include <alsa/asoundlib.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <time.h>

static int64_t milliseconds(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time)) return -1;
    return (int64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

int reliefos_audio_test_tone(const char *device, uint32_t channels, uint32_t seconds,
                            int (*cancelled)(void *), void *context)
{
    snd_pcm_t *pcm = NULL;
    snd_pcm_hw_params_t *params = NULL;
    unsigned rate = 48000, actual_channels;
    int result, direction = 0;
    if (!device || !*device || channels < 1 || channels > 2 || !seconds || seconds > 30) {
        errno = EINVAL; return -1;
    }
    result = snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (result < 0) { errno = -result; return -1; }
    result = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                channels, rate, 1, 50000);
    if (result < 0) goto done;
    result = snd_pcm_hw_params_malloc(&params);
    if (result < 0) goto done;
    result = snd_pcm_hw_params_current(pcm, params);
    if (result >= 0) result = snd_pcm_hw_params_get_rate(params, &rate, &direction);
    if (result >= 0) result = snd_pcm_hw_params_get_channels(params, &actual_channels);
    snd_pcm_hw_params_free(params);
    if (result < 0) goto done;
    if (!rate || rate > 384000 || actual_channels != channels) { result = -EINVAL; goto done; }
    int64_t now = milliseconds();
    if (now < 0) { result = -errno; goto done; }
    int64_t deadline = now + (int64_t)seconds * 1000 + 3000;
    uint64_t frame = 0, total = (uint64_t)rate * seconds;
    int16_t samples[256 * 2];
    while (frame < total) {
        unsigned frames = total - frame < 256 ? (unsigned)(total - frame) : 256;
        for (unsigned i = 0; i < frames; ++i) {
            double phase = 6.2831853071795864769 * (double)(frame + i) / rate;
            samples[i * channels] = (int16_t)(sin(phase * 440) * 6000);
            if (channels == 2) samples[i * channels + 1] = (int16_t)(sin(phase * 660) * 6000);
        }
        unsigned offset = 0;
        while (offset < frames) {
            if (cancelled && cancelled(context)) { result = -ECANCELED; goto done; }
            now = milliseconds();
            if (now < 0) { result = -errno; goto done; }
            if (now >= deadline) { result = -ETIMEDOUT; goto done; }
            snd_pcm_sframes_t written = snd_pcm_writei(pcm, samples + offset * channels, frames - offset);
            if (written > 0) {
                if ((uint64_t)written > frames - offset) { result = -EIO; goto done; }
                offset += (unsigned)written;
            } else if (written == -EPIPE) {
                result = snd_pcm_prepare(pcm); if (result < 0) goto done;
            } else if (written == -ESTRPIPE) {
                result = snd_pcm_resume(pcm);
                if (result == -EAGAIN) { snd_pcm_wait(pcm, 20); }
                else if (result < 0) { result = snd_pcm_prepare(pcm); if (result < 0) goto done; }
            } else if (!written || written == -EAGAIN || written == -EINTR) {
                result = snd_pcm_wait(pcm, 20);
                if (result < 0 && result != -EINTR && result != -EPIPE && result != -ESTRPIPE) goto done;
            } else { result = (int)written; goto done; }
        }
        frame += frames;
    }
    do {
        if (cancelled && cancelled(context)) { result = -ECANCELED; goto done; }
        now = milliseconds();
        if (now < 0) { result = -errno; goto done; }
        if (now >= deadline) { result = -ETIMEDOUT; goto done; }
        result = snd_pcm_drain(pcm);
        if (result == -EAGAIN || result == -EINTR) snd_pcm_wait(pcm, 20);
    } while (result == -EAGAIN || result == -EINTR);
done:
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);
    if (result < 0) { errno = -result; return -1; }
    return 0;
}
