#include <reliefos/audio_test.h>
#include <alsa/asoundlib.h>
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

struct _snd_pcm { unsigned channels; };
static unsigned calls, waits, closes, drops, prepares, resumes;
static unsigned captured;
static int failed_wait;
int snd_pcm_open(snd_pcm_t **pcm, const char *name, snd_pcm_stream_t stream, int mode) {
    (void)name; assert(stream == SND_PCM_STREAM_PLAYBACK && mode & SND_PCM_NONBLOCK);
    *pcm = calloc(1, sizeof(**pcm)); return *pcm ? 0 : -ENOMEM;
}
int snd_pcm_set_params(snd_pcm_t *pcm, snd_pcm_format_t format, snd_pcm_access_t access,
                       unsigned channels, unsigned rate, int resample, unsigned latency) {
    assert(format == SND_PCM_FORMAT_S16_LE && access == SND_PCM_ACCESS_RW_INTERLEAVED);
    assert(rate == 48000 && resample && latency <= 50000); pcm->channels = channels; return 0;
}
int snd_pcm_hw_params_current(snd_pcm_t *pcm, snd_pcm_hw_params_t *params) { (void)pcm; (void)params; return 0; }
int snd_pcm_hw_params_get_rate(const snd_pcm_hw_params_t *params, unsigned *rate, int *dir) {
    (void)params; *rate = 47000; *dir = 0; return 0;
}
int snd_pcm_hw_params_get_channels(const snd_pcm_hw_params_t *params, unsigned *channels) {
    (void)params; *channels = 2; return 0;
}
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buffer, snd_pcm_uframes_t frames) {
    ++calls;
    if (calls == 1) return -EINTR;
    if (calls == 3) return -EAGAIN;
    if (calls == 4) return -EPIPE;
    if (calls == 5 || calls == 6) return -ESTRPIPE;
    unsigned n = frames > 7 ? 7 : (unsigned)frames;
    const int16_t *samples = buffer;
    assert(pcm->channels == 2);
    for (unsigned i = 0; i < n; ++i) {
        double phase = 6.2831853071795864769 * (captured + i) / 47000;
        assert(abs(samples[i * 2] - (int16_t)(sin(phase * 440) * 6000)) <= 1);
        assert(abs(samples[i * 2 + 1] - (int16_t)(sin(phase * 660) * 6000)) <= 1);
    }
    captured += n; return n;
}
int snd_pcm_wait(snd_pcm_t *pcm, int timeout) { (void)pcm; assert(timeout <= 50); ++waits; return failed_wait ? -ENODEV : 1; }
int snd_pcm_prepare(snd_pcm_t *pcm) { (void)pcm; ++prepares; return 0; }
int snd_pcm_resume(snd_pcm_t *pcm) { (void)pcm; return ++resumes == 1 ? -EAGAIN : 0; }
int snd_pcm_drain(snd_pcm_t *pcm) { (void)pcm; return 0; }
int snd_pcm_drop(snd_pcm_t *pcm) { (void)pcm; ++drops; return 0; }
int snd_pcm_close(snd_pcm_t *pcm) { free(pcm); ++closes; return 0; }
static int cancel(void *data) { return *(int *)data; }
int main(void) {
    assert(reliefos_audio_test_tone("default", 2, 1, NULL, NULL) == 0);
    assert(captured == 47000 && waits >= 3 && prepares == 1 && resumes == 2 && closes == 1 && drops == 1);
    int stop = 1;
    assert(reliefos_audio_test_tone("default", 2, 1, cancel, &stop) == -1 && errno == ECANCELED);
    assert(closes == 2 && captured == 47000);
    failed_wait = 1; calls = 0;
    assert(reliefos_audio_test_tone("default", 2, 1, NULL, NULL) == -1 && errno == ENODEV && closes == 3);
    assert(reliefos_audio_test_tone("default", 3, 1, NULL, NULL) == -1 && errno == EINVAL && closes == 3);
    puts("tone negotiated rate, distinct contiguous stereo, partial writes, EINTR/EAGAIN/XRUN/resume, cancellation and removal PASS");
    return 0;
}
