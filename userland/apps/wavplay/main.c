#include <reliefos/fs.h>
#include <reliefos/stdio.h>
#include <reliefos/syscall.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <linux/soundcard.h>

#define WAVPLAY_BUFFER_BYTES 4096U
#define WAVPLAY_TONE_FRAMES 1024U
#define WAVPLAY_TEST_RATE 48000U
#define WAVPLAY_TEST_SECONDS 30U
#define WAVPLAY_MAX_CHANNELS 16U

static int16_t tone_samples[WAVPLAY_TONE_FRAMES * 2U];
static int dsp_fd = -1;
static uint32_t dsp_channels = 2U;
static uint8_t output_samples[WAVPLAY_TONE_FRAMES * WAVPLAY_MAX_CHANNELS * 2U];

struct wav_info {
    uint32_t sample_rate;
    uint32_t data_bytes;
};

static int audio_error(const char *operation)
{
    printf("wavplay: %s failed (errno=%d)\n", operation, errno);
    return -1;
}

static int configure_audio(uint32_t requested_rate, uint32_t *actual_rate)
{
    int format = AFMT_S16_LE;
    int channels = 2;
    int rate = (int)requested_rate;
    if (requested_rate < 8000U || requested_rate > 48000U) {
        printf("wavplay: unsupported sample rate %u\n", requested_rate);
        return -1;
    }
    if (dsp_fd < 0) {
        dsp_fd = open("/dev/dsp", O_WRONLY | O_NONBLOCK, 0);
    }
    if (dsp_fd < 0) return audio_error("open /dev/dsp");
    if (ioctl(dsp_fd, SNDCTL_DSP_SETFMT, &format) < 0)
        return audio_error("SNDCTL_DSP_SETFMT");
    if (ioctl(dsp_fd, SNDCTL_DSP_CHANNELS, &channels) < 0)
        return audio_error("SNDCTL_DSP_CHANNELS");
    if (ioctl(dsp_fd, SNDCTL_DSP_SPEED, &rate) < 0)
        return audio_error("SNDCTL_DSP_SPEED");
    if (format != AFMT_S16_LE || channels < 2 ||
        channels > (int)WAVPLAY_MAX_CHANNELS || rate <= 0) {
        printf("wavplay: unsupported negotiated format=%d channels=%d rate=%d\n",
               format, channels, rate);
        return -1;
    }
    if (channels > 2) {
        /* Retain the default stereo period duration when each hardware frame
         * contains more channels. OSS fragment sizes must be powers of two. */
        unsigned exponent = 11U;
        while ((1U << exponent) < 512U * (uint32_t)channels * 2U) ++exponent;
        int fragment = (8 << 16) | (int)exponent;
        if (ioctl(dsp_fd, SNDCTL_DSP_SETFRAGMENT, &fragment) < 0)
            return audio_error("SNDCTL_DSP_SETFRAGMENT");
    }
    dsp_channels = (uint32_t)channels;
    if (actual_rate) *actual_rate = (uint32_t)rate;
    return 0;
}

static int wait_audio_ms(uint32_t milliseconds)
{
    struct timespec delay = {
        .tv_sec = milliseconds / 1000U,
        .tv_nsec = (long)(milliseconds % 1000U) * 1000000L,
    };
    while (nanosleep(&delay, &delay) < 0) {
        if (errno != EINTR) return audio_error("nanosleep");
    }
    return 0;
}

static int write_audio_retry(const void *data, uint32_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t offset = 0;
    uint32_t waits = 0;
    while (offset < length) {
        long written = write(dsp_fd, bytes + offset, length - offset);
        if (written > 0 && (uint32_t)written <= length - offset) {
            offset += (uint32_t)written;
            waits = 0;
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        if ((written == 0 || (written < 0 && errno == EAGAIN)) && waits++ < 200U) {
            if (wait_audio_ms(5U) < 0) return -1;
            continue;
        }
        if (written >= 0) errno = written ? EIO : EAGAIN;
        return audio_error("PCM write");
    }
    return 0;
}

static int drain_audio(void)
{
    uint32_t waits = 0;
    while (ioctl(dsp_fd, SNDCTL_DSP_SYNC, (void *)0) < 0) {
        if (errno == EINTR) continue;
        if (errno != EAGAIN || waits++ >= 200U) return audio_error("SNDCTL_DSP_SYNC");
        if (wait_audio_ms(5U) < 0) return -1;
    }
    return 0;
}

/* Keep the source stereo pair on the front converter; other hardware channels
 * carry zero PCM. The hardware's negotiated interleave is never mislabelled. */
static int write_stereo_audio(const void *data, uint32_t length)
{
    if (length % 4U) { errno = EINVAL; return audio_error("stereo frame"); }
    if (dsp_channels == 2U) return write_audio_retry(data, length);
    const uint8_t *source = data;
    uint32_t frames = length / 4U;
    uint32_t frame_bytes = dsp_channels * 2U;
    while (frames) {
        uint32_t count = frames > WAVPLAY_TONE_FRAMES ? WAVPLAY_TONE_FRAMES : frames;
        for (uint32_t frame = 0; frame < count; ++frame) {
            memcpy(output_samples + frame * frame_bytes, source + frame * 4U, 4U);
            memset(output_samples + frame * frame_bytes + 4U, 0, frame_bytes - 4U);
        }
        if (write_audio_retry(output_samples, count * frame_bytes) < 0) return -1;
        source += count * 4U;
        frames -= count;
    }
    return 0;
}

static uint16_t read_le16(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static int text_eq_n(const uint8_t *left, const char *right, uint32_t length)
{
    for (uint32_t index = 0; index < length; ++index) {
        if (!right[index] || left[index] != (uint8_t)right[index]) {
            return 0;
        }
    }
    return right[length] == 0;
}

static int read_exact(int fd, void *buffer, uint32_t length)
{
    uint8_t *out = (uint8_t *)buffer;
    uint32_t offset = 0;
    while (offset < length) {
        long got = read(fd, out + offset, length - offset);
        if (got <= 0) {
            return -1;
        }
        offset += (uint32_t)got;
    }
    return 0;
}

static int skip_bytes(int fd, uint32_t length)
{
    return lseek(fd, (long)length, RELIEFOS_SEEK_CUR) < 0 ? -1 : 0;
}

static int parse_wav_header(int fd, struct wav_info *out)
{
    uint8_t header[12];
    uint8_t chunk[8];
    uint8_t format[16];
    uint8_t found_format = 0;
    if (!out || read_exact(fd, header, sizeof(header)) < 0 ||
        !text_eq_n(header, "RIFF", 4) || !text_eq_n(header + 8, "WAVE", 4)) {
        return -1;
    }
    *out = (struct wav_info){0};
    for (;;) {
        uint32_t chunk_size;
        if (read_exact(fd, chunk, sizeof(chunk)) < 0) {
            return -1;
        }
        chunk_size = read_le32(chunk + 4);
        if (text_eq_n(chunk, "fmt ", 4)) {
            if (chunk_size < sizeof(format) || read_exact(fd, format, sizeof(format)) < 0 ||
                read_le16(format) != 1U || read_le16(format + 2) != 2U ||
                read_le16(format + 14) != 16U ||
                skip_bytes(fd, chunk_size - sizeof(format)) < 0) {
                return -1;
            }
            out->sample_rate = read_le32(format + 4);
            found_format = 1;
        } else if (text_eq_n(chunk, "data", 4)) {
            if (!found_format) {
                return -1;
            }
            out->data_bytes = chunk_size;
            return 0;
        } else if (skip_bytes(fd, chunk_size) < 0) {
            return -1;
        }
        if (chunk_size & 1U && skip_bytes(fd, 1U) < 0) {
            return -1;
        }
    }
}

static int play_wav(const char *path)
{
    struct wav_info info;
    uint8_t *buffer;
    uint32_t remaining;
    int fd;
    int ret;
    fd = open(path, RELIEFOS_O_RDONLY, 0);
    if (fd < 0) {
        printf("wavplay: open failed %d\n", fd);
        return 1;
    }
    if (parse_wav_header(fd, &info) < 0) {
        puts("wavplay: supported files are PCM, stereo, 16-bit WAV");
        close(fd);
        return 1;
    }
    ret = configure_audio(info.sample_rate, &info.sample_rate);
    if (ret < 0) {
        printf("wavplay: audio format rejected %d\n", ret);
        close(fd);
        return 1;
    }
    printf("wavplay: %u Hz stereo PCM through /dev/dsp (%u output channels)\n",
           info.sample_rate, dsp_channels);
    buffer = malloc(WAVPLAY_BUFFER_BYTES + 4U);
    if (!buffer) {
        puts("wavplay: out of memory");
        close(fd);
        return 1;
    }
    remaining = info.data_bytes;
    while (remaining) {
        uint32_t wanted = remaining > WAVPLAY_BUFFER_BYTES
                              ? WAVPLAY_BUFFER_BYTES
                              : remaining;
        long got = read(fd, buffer, wanted);
        uint32_t padded;
        if (got <= 0) {
            puts("wavplay: truncated WAV data");
            free(buffer);
            close(fd);
            return 1;
        }
        padded = ((uint32_t)got + 3U) & ~3U;
        while ((uint32_t)got < padded) {
            buffer[got++] = 0;
        }
        if (write_stereo_audio(buffer, padded) < 0) {
            puts("wavplay: playback failed");
            free(buffer);
            close(fd);
            return 1;
        }
        remaining -= (uint32_t)got > remaining ? remaining : (uint32_t)got;
    }
    free(buffer);
    close(fd);
    if (drain_audio() < 0) return 1;
    puts("wavplay: complete");
    return 0;
}

static int play_square_note(uint32_t period, uint32_t frames)
{
    uint32_t phase = 0;
    while (frames) {
        uint32_t count = frames > WAVPLAY_TONE_FRAMES ? WAVPLAY_TONE_FRAMES : frames;
        for (uint32_t index = 0; index < count; ++index) {
            int16_t value = phase < period / 2U ? 6000 : -6000;
            tone_samples[index * 2U] = value;
            tone_samples[index * 2U + 1U] = value;
            phase = (phase + 1U) % period;
        }
        if (write_stereo_audio(tone_samples, count * 4U) < 0) {
            return -1;
        }
        frames -= count;
    }
    return 0;
}

static int play_test_melody(void)
{
    if (configure_audio(WAVPLAY_TEST_RATE, 0) < 0) {
        return 1;
    }
    printf("wavplay: playing 30-second 48000 Hz PCM tone (%u output channels)\n",
           dsp_channels);
    if (play_square_note(50U, WAVPLAY_TEST_RATE * WAVPLAY_TEST_SECONDS) < 0) {
        puts("wavplay: PCM tone failed");
        return 1;
    }
    if (drain_audio() < 0) return 1;
    puts("wavplay: PCM tone complete");
    return 0;
}

int main(int argc, char **argv, char **envp)
{
    int result;
    (void)envp;
    if (argc > 1 && argv && argv[1] && argv[1][0]) {
        result = play_wav(argv[1]);
    } else result = play_test_melody();
    if (dsp_fd >= 0) {
        close(dsp_fd);
        dsp_fd = -1;
    }
    return result;
}
