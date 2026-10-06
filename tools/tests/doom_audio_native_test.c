#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/soundcard.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int channels = 8, speed_error, blocked, recover_error;
static unsigned closes, resets, speed_calls;
static size_t received_bytes, first_short;
static uint8_t received[1024 * 1024];
static int allocation_failure;

static void *probe_realloc(void *pointer, size_t bytes)
{
    if (allocation_failure) { allocation_failure = 0; return NULL; }
    return realloc(pointer, bytes);
}

static int probe_open(const char *path, int flags, ...)
{
    assert(!strcmp(path, "/dev/dsp") && flags == O_WRONLY);
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
    if (request == SNDCTL_DSP_RESET) { ++resets; return 0; }
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    if (request == SNDCTL_DSP_CHANNELS) *(int *)arg = channels;
    else if (request == SNDCTL_DSP_SETFRAGMENT)
        assert(*(int *)arg == ((64 << 16) | 13));
    else if (request == SNDCTL_DSP_SPEED) {
        ++speed_calls;
        if (speed_error) { errno = EIO; return -1; }
        *(int *)arg = 48000;
    } else if (request == SNDCTL_DSP_GETOSPACE) {
        ((struct audio_buf_info *)arg)->bytes = 10 * channels * 2;
    } else if (request == SNDCTL_DSP_GETOPTR) {
        memset(arg, 0, sizeof(count_info));
    } else if (request == SNDCTL_DSP_GETODELAY) {
        *(int *)arg = 0;
    } else assert(request == SNDCTL_DSP_SETFMT);
    return 0;
}

static ssize_t probe_write(int fd, const void *data, size_t bytes)
{
    assert(fd == 7 && data && bytes);
    if (recover_error) { errno = recover_error; recover_error = 0; return -1; }
    if (blocked) { errno = EAGAIN; return -1; }
    if (first_short) { bytes = first_short; first_short = 0; blocked = 1; }
    assert(received_bytes + bytes <= sizeof(received));
    memcpy(received + received_bytes, data, bytes);
    received_bytes += bytes;
    return (ssize_t)bytes;
}

#define open probe_open
#define close probe_close
#define ioctl probe_ioctl
#define write probe_write
#define realloc probe_realloc
#include "../../userland/apps/doom/audio_output.c"
#undef open
#undef close
#undef ioctl
#undef write
#undef realloc

static void expect_pcm(const int16_t *stereo, unsigned frames, unsigned offset)
{
    for (unsigned frame = 0; frame < frames; ++frame) {
        int16_t values[16];
        memcpy(values, received + (offset + frame) * channels * 2, channels * 2);
        assert(values[0] == stereo[frame * 2] && values[1] == stereo[frame * 2 + 1]);
        for (int channel = 2; channel < channels; ++channel) assert(values[channel] == 0);
    }
}

int main(void)
{
    const int16_t first[] = {123, -321, 222, -444, -777, 999};
    const int16_t second[] = {231, -132, -888, 456};
    for (unsigned n = 0; n < 3; ++n) {
        channels = n == 0 ? 8 : n == 1 ? 16 : 2;
        struct doom_audio_output *output = NULL;
        uint32_t rate = 0, frames = 0;
        received_bytes = 0;
        assert(!doom_audio_open(&output, NULL, &rate) && output && rate == 48000);
        assert(!doom_audio_writable_frames(output, &frames) && frames == 10);
        if (channels > 2) {
            allocation_failure = 1;
            assert(doom_audio_submit(output, first, 3) == -ENOMEM);
            assert(received_bytes == 0 && !output->pending_bytes);
        }
        /* Odd byte writes must retain the expanded hardware interleave. */
        first_short = 3;
        assert(doom_audio_submit(output, first, 3) == -EAGAIN);
        assert(!doom_audio_writable_frames(output, &frames) && frames == 7);
        assert(doom_audio_submit(output, second, 2) == -EAGAIN);
        assert(!doom_audio_writable_frames(output, &frames) && frames == 5);
        blocked = 0;
        assert(!doom_audio_flush(output));
        assert(received_bytes == 5U * channels * 2);
        expect_pcm(first, 3, 0);
        expect_pcm(second, 2, 3);
        recover_error = EPIPE;
        assert(!doom_audio_submit(output, second, 2));
        expect_pcm(second, 2, 5);
        int16_t large[8192 * 2];
        for (unsigned i = 0; i < 8192 * 2; ++i) large[i] = (int16_t)i;
        if (channels > 2) {
            size_t old_bytes = received_bytes;
            allocation_failure = 1;
            assert(doom_audio_submit(output, large, 8192) == -ENOMEM);
            assert(received_bytes == old_bytes);
        }
        assert(!doom_audio_submit(output, large, 8192));
        expect_pcm(large, 8192, 7);
        struct doom_audio_stats stats;
        assert(!doom_audio_get_stats(output, &stats));
        assert(stats.written_bytes == received_bytes && stats.xrun_recoveries == 1);
        doom_audio_close(output);
    }
    assert(closes == 3 && resets == 3 && speed_calls == 3);
    for (unsigned n = 0; n < 3; ++n) {
        channels = n == 0 ? 1 : n == 1 ? 17 : 8;
        speed_error = n == 2;
        struct doom_audio_output *output = (void *)1;
        assert(doom_audio_open(&output, NULL, NULL) == -ENODEV && !output);
    }
    assert(closes == 6 && speed_calls == 4);
    puts("PASS Doom native OSS: 2/8/16 channels, stereo expansion, odd short writes, pending frames, XRUN and close/reopen");
    return 0;
}
