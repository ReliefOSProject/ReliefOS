#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <reliefos/fs.h>
#include <reliefos/stdio.h>
#include <reliefos/syscall.h>
#include <linux/soundcard.h>

static unsigned writes, sleeps;
static int first_error, sleep_error, sleep_interrupt;
static int tone_mode;
static uint32_t tone_frames;
static unsigned syncs, closes;
static unsigned negotiated_channels = 2, fragment_requests;
static int probe_open(const char *path, int flags, ...)
{
    assert(!strcmp(path, "/dev/dsp") && flags == (O_WRONLY | O_NONBLOCK));
    return 7;
}
static int probe_close(int fd)
{
    assert(fd == 7);
    ++closes;
    return 0;
}
static int probe_ioctl(int fd, unsigned long request, ...)
{
    assert(fd == 7);
    if (request == SNDCTL_DSP_SYNC) {
        if (++syncs == 1) { errno = EAGAIN; return -1; }
        return 0;
    }
    assert(request == SNDCTL_DSP_SETFMT || request == SNDCTL_DSP_CHANNELS ||
           request == SNDCTL_DSP_SPEED || request == SNDCTL_DSP_SETFRAGMENT);
    va_list args;
    va_start(args, request);
    int *value = va_arg(args, int *);
    if (request == SNDCTL_DSP_CHANNELS) *value = (int)negotiated_channels;
    if (request == SNDCTL_DSP_SETFRAGMENT) {
        ++fragment_requests;
        /* Eight periods of 512 frames at 8/16 channels, preserving duration. */
        assert(*value == ((8 << 16) | (negotiated_channels == 8 ? 13 : 14)));
    }
    va_end(args);
    return 0;
}
static ssize_t probe_write(int fd, const void *data, size_t length)
{
    assert(fd == 7 && data && length);
    ++writes;
    if (tone_mode) {
        const int16_t *samples = data;
        assert(!(length % (negotiated_channels * 2)));
        for (size_t frame = 0; frame < length / (negotiated_channels * 2); ++frame) {
            int16_t expected = tone_frames % 50 < 25 ? 6000 : -6000;
            assert(samples[negotiated_channels * frame] == expected &&
                   samples[negotiated_channels * frame + 1] == expected);
            for (unsigned channel = 2; channel < negotiated_channels; ++channel)
                assert(samples[negotiated_channels * frame + channel] == 0);
            ++tone_frames;
        }
        return (ssize_t)length;
    }
    if (writes == 1) { errno = first_error; return -1; }
    return (ssize_t)(length > 4 ? 4 : length);
}
static int probe_nanosleep(const struct timespec *delay, struct timespec *remaining)
{
    assert(delay->tv_sec == 0 && remaining);
    assert(delay->tv_nsec == (sleep_interrupt && sleeps ? 2000000 : 5000000));
    ++sleeps;
    if (sleep_interrupt && sleeps == 1) {
        *remaining = (struct timespec){.tv_nsec = 2000000};
        errno = EINTR;
        return -1;
    }
    if (sleep_error) { errno = sleep_error; return -1; }
    return 0;
}
#define write probe_write
#define nanosleep probe_nanosleep
#define open probe_open
#define close probe_close
#define ioctl probe_ioctl
#define main wavplay_main
#include "../../userland/apps/wavplay/main.c"
#undef main
#undef nanosleep
#undef write
#undef open
#undef close
#undef ioctl

int main(void)
{
    const uint8_t pcm[12] = {0};
    dsp_fd = 7;
    first_error = EAGAIN;
    assert(!write_audio_retry(pcm, sizeof pcm));
    assert(writes == 4 && sleeps == 1);
    writes = sleeps = 0;
    first_error = EINTR;
    assert(!write_audio_retry(pcm, sizeof pcm));
    assert(writes == 4 && sleeps == 0);
    writes = sleeps = 0;
    first_error = EIO;
    assert(write_audio_retry(pcm, sizeof pcm) < 0);
    assert(writes == 1 && sleeps == 0);
    writes = sleeps = 0;
    first_error = EAGAIN;
    sleep_error = EFAULT;
    assert(write_audio_retry(pcm, sizeof pcm) < 0);
    assert(writes == 1 && sleeps == 1);
    sleep_error = 0;
    sleeps = 0;
    sleep_interrupt = 1;
    assert(!wait_audio_ms(5));
    assert(sleeps == 2);
    writes = sleeps = 0;
    sleep_interrupt = 0;
    tone_mode = 1;
    /* A ready PCM queue determines the pace; a userspace frame delay would
     * round 21.33 ms to 30 ms on this kernel and repeatedly starve DMA. */
    assert(!play_square_note(50, 2048));
    assert(tone_frames == 2048 && sleeps == 0);
    tone_frames = 0;
    dsp_fd = -1;
    assert(!wavplay_main(0, NULL, NULL));
    assert(tone_frames == WAVPLAY_TEST_RATE * WAVPLAY_TEST_SECONDS);
    assert(syncs == 2 && sleeps == 1 && closes == 1 && dsp_fd == -1);
    for (unsigned channels = 8; channels <= 16; channels *= 2) {
        negotiated_channels = channels;
        tone_frames = 0;
        assert(!configure_audio(48000, NULL));
        assert(!play_square_note(50, 2048));
        assert(tone_frames == 2048);
    }
    assert(fragment_requests == 2);
    negotiated_channels = 17;
    assert(configure_audio(48000, NULL) < 0);
    puts("PASS wavplay: POSIX EAGAIN, EINTR, partial writes and terminal EIO");
    return 0;
}
