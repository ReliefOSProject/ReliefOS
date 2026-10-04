#include "audio_output.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/soundcard.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct doom_audio_output {
    struct doom_audio_io io;
    uint8_t *pending;
    uint32_t capacity;
    uint32_t pending_bytes;
    uint32_t frame_bytes;
    uint8_t *interleaved;
    uint32_t interleaved_capacity;
    uint32_t output_channels;
    int fd;
    struct doom_audio_stats stats;
    uint64_t last_fd_write_cycles;
    uint64_t epoch_fd_written_bytes;
};

static int doom_audio_error(long result)
{
    if (result == -1L) {
        return errno ? errno : EIO;
    }
    return result < 0L ? (int)-result : 0;
}

static int doom_audio_recover(struct doom_audio_output *output, int error)
{
    int result;
    if (error == EPIPE) {
        if (output->stats.written_bytes) ++output->stats.xrun_recoveries;
        else ++output->stats.startup_recoveries;
    } else if (error == ESTRPIPE) {
        ++output->stats.suspend_recoveries;
    }
    if (!output->io.recover) {
        return -error;
    }
    result = output->io.recover(output->io.opaque, error);
    return result < 0 ? result : 0;
}

static int doom_audio_append(struct doom_audio_output *output,
                             const uint8_t *data, uint32_t bytes)
{
    if (bytes > output->capacity - output->pending_bytes) {
        return -ENOSPC;
    }
    memcpy(output->pending + output->pending_bytes, data, bytes);
    output->pending_bytes += bytes;
    return 0;
}

int doom_audio_flush(struct doom_audio_output *output)
{
    unsigned int interrupted = 0;
    unsigned int recovered = 0;
    if (!output || !output->io.write_bytes) {
        return -EINVAL;
    }
    while (output->pending_bytes) {
        long result = output->io.write_bytes(output->io.opaque, output->pending,
                                             output->pending_bytes);
        int error = doom_audio_error(result);
        if (error) {
            if (error == EINTR && interrupted++ < 8U) {
                continue;
            }
            if (error == EPIPE || error == ESTRPIPE) {
                if (recovered++ >= 8U) return -error;
                int recovery = doom_audio_recover(output, error);
                if (!recovery) {
                    interrupted = 0;
                    continue;
                }
                return recovery;
            }
            return error == EAGAIN || error == EWOULDBLOCK ? -EAGAIN : -error;
        }
        if (result == 0L || (uint32_t)result > output->pending_bytes) {
            return -EAGAIN;
        }
        memmove(output->pending, output->pending + result,
                output->pending_bytes - (uint32_t)result);
        output->pending_bytes -= (uint32_t)result;
        output->stats.written_bytes += (uint32_t)result;
        interrupted = 0;
        recovered = 0;
    }
    return 0;
}

int doom_audio_writable_frames(struct doom_audio_output *output, uint32_t *frames)
{
    if (!output || !frames) return -EINVAL;
    if (!output->io.available_frames) return -EOPNOTSUPP;
    long available = output->io.available_frames(output->io.opaque);
    int error = doom_audio_error(available);
    if (error) return -error;
    if ((unsigned long)available > UINT32_MAX) return -EOVERFLOW;
    uint64_t pending = ((uint64_t)output->pending_bytes + output->frame_bytes - 1U) /
                       output->frame_bytes;
    *frames = (uint64_t)available > pending ? (uint32_t)(available - pending) : 0U;
    return 0;
}

struct doom_audio_output *doom_audio_output_create(const struct doom_audio_io *io,
                                                   uint32_t buffer_frames,
                                                   uint32_t frame_bytes)
{
    struct doom_audio_output *output;
    uint64_t capacity;
    if (!io || !io->write_bytes || !buffer_frames || !frame_bytes) {
        errno = EINVAL;
        return NULL;
    }
    capacity = (uint64_t)buffer_frames * frame_bytes;
    if (capacity > UINT32_MAX) {
        errno = EOVERFLOW;
        return NULL;
    }
    output = (struct doom_audio_output *)calloc(1, sizeof(*output));
    if (!output) {
        return NULL;
    }
    output->pending = (uint8_t *)malloc((size_t)capacity);
    if (!output->pending) {
        free(output);
        return NULL;
    }
    output->io = *io;
    output->capacity = (uint32_t)capacity;
    output->frame_bytes = frame_bytes;
    output->fd = -1;
    return output;
}

int doom_audio_submit(struct doom_audio_output *output, const int16_t *pcm,
                      uint32_t frames)
{
    const uint8_t *bytes = (const uint8_t *)pcm;
    uint32_t total;
    uint32_t offset = 0;
    unsigned int interrupted = 0;
    unsigned int recovered = 0;
    if (!output || (!pcm && frames)) {
        return -EINVAL;
    }
    if (!frames) {
        return 0;
    }
    if ((uint64_t)frames * output->frame_bytes > UINT32_MAX) {
        return -EOVERFLOW;
    }
    total = frames * output->frame_bytes;
    /* The mixer supplies stereo frames; native HDA may negotiate a wider
     * interleave. Convert before queuing bytes so even an odd short write
     * retains the device layout and GETOSPACE uses hardware frame sizes. */
    if (output->output_channels > 2U) {
        if (total > output->interleaved_capacity) {
            uint8_t *interleaved = (uint8_t *)realloc(output->interleaved, total);
            if (!interleaved) return -ENOMEM;
            output->interleaved = interleaved;
            output->interleaved_capacity = total;
        }
        for (uint32_t frame = 0; frame < frames; ++frame) {
            uint8_t *destination = output->interleaved + frame * output->frame_bytes;
            memcpy(destination, pcm + frame * 2U, 4U);
            memset(destination + 4U, 0, output->frame_bytes - 4U);
        }
        bytes = output->interleaved;
    }
    if (output->pending_bytes) {
        int result = doom_audio_flush(output);
        if (result && result != -EAGAIN) {
            return result;
        }
        if (output->pending_bytes) {
            return doom_audio_append(output, bytes, total) ? -ENOSPC : -EAGAIN;
        }
    }
    while (offset < total) {
        long result = output->io.write_bytes(output->io.opaque, bytes + offset,
                                             total - offset);
        int error = doom_audio_error(result);
        if (error) {
            if (error == EINTR && interrupted++ < 8U) {
                continue;
            }
            if (error == EPIPE || error == ESTRPIPE) {
                if (recovered++ >= 8U) return -error;
                int recovery = doom_audio_recover(output, error);
                if (!recovery) {
                    interrupted = 0;
                    continue;
                }
                return recovery;
            }
            if (error == EAGAIN || error == EWOULDBLOCK) {
                if (doom_audio_append(output, bytes + offset, total - offset)) {
                    return -ENOSPC;
                }
                return -EAGAIN;
            }
            return -error;
        }
        if (result == 0L || (uint32_t)result > total - offset) {
            if (doom_audio_append(output, bytes + offset, total - offset)) {
                return -ENOSPC;
            }
            return -EAGAIN;
        }
        offset += (uint32_t)result;
        output->stats.written_bytes += (uint32_t)result;
        interrupted = 0;
        recovered = 0;
    }
    return 0;
}

/** @brief Read serialized x86 cycle time for failure-only PCM diagnostics. */
static uint64_t doom_audio_cycles(void)
{
    uint32_t low, high;
    __asm__ volatile("lfence; rdtsc" : "=a"(low), "=d"(high) :: "memory");
    return ((uint64_t)high << 32) | low;
}

/** @brief Submit native bytes, retaining timing/epoch evidence on real EPIPE. */
static long doom_audio_fd_write(void *opaque, const void *pcm, uint32_t bytes)
{
    struct doom_audio_output *output = (struct doom_audio_output *)opaque;
    uint64_t begin = doom_audio_cycles();
    long result = write(output->fd, pcm, bytes);
    int error = errno;
    uint64_t after = doom_audio_cycles();
    if (result > 0) {
        output->epoch_fd_written_bytes += (uint32_t)result;
        output->last_fd_write_cycles = after;
    } else if (result < 0 && error == EPIPE) {
        count_info position = {0};
        int delay = -1;
        int position_result = ioctl(output->fd, SNDCTL_DSP_GETOPTR, &position);
        int delay_result = ioctl(output->fd, SNDCTL_DSP_GETODELAY, &delay);
        fprintf(stderr, "[doom-audio] EPIPE epoch_submitted_bytes=%llu hw_bytes=%d ptr=%d "
                "position_rc=%d delay_bytes=%d delay_rc=%d since_write_cycles=%llu io_cycles=%llu\n",
                (unsigned long long)output->epoch_fd_written_bytes, position.bytes,
                position.ptr, position_result, delay, delay_result,
                (unsigned long long)(output->last_fd_write_cycles
                    ? after - output->last_fd_write_cycles : 0),
                (unsigned long long)(after - begin));
    }
    errno = error;
    return result;
}

static long doom_audio_fd_available(void *opaque)
{
    struct doom_audio_output *output = (struct doom_audio_output *)opaque;
    struct audio_buf_info space;
    if (ioctl(output->fd, SNDCTL_DSP_GETOSPACE, &space) < 0) return -errno;
    if (space.bytes < 0) return -EIO;
    return (long)((uint32_t)space.bytes / output->frame_bytes);
}

static int doom_audio_fd_recover(void *opaque, int error)
{
    struct doom_audio_output *output = (struct doom_audio_output *)opaque;
    if (!output || (error != EPIPE && error != ESTRPIPE)) {
        return -error;
    }
    if (ioctl(output->fd, SNDCTL_DSP_RESET, 0) < 0) return -errno;
    output->epoch_fd_written_bytes = 0;
    output->last_fd_write_cycles = 0;
    return 0;
}

int doom_audio_open(struct doom_audio_output **out, const char *pcm,
                    uint32_t *actual_rate)
{
    struct doom_audio_output *output;
    struct doom_audio_io io;
    int format = AFMT_S16_LE;
    int channels = 2;
    int rate = 48000;
    const char *device = pcm && pcm[0] ? pcm : "/dev/dsp";
    if (!out) {
        return -EINVAL;
    }
    *out = NULL;
    output = (struct doom_audio_output *)calloc(1, sizeof(*output));
    if (!output) {
        return -ENOMEM;
    }
    /* Doom mixes a complete periodic PCM block on each tick.  A blocking OSS
     * descriptor lets the kernel drain that block instead of dropping audio
     * when the small nonblocking pending queue fills.  Expand the kernel ring
     * before format negotiation so HDA has several scheduler periods of
     * slack when the game thread is delayed. */
    output->fd = open(device, O_WRONLY, 0);
    int fragment = (int)DOOM_AUDIO_FRAGMENT_CONFIG;
    if (output->fd < 0 || ioctl(output->fd, SNDCTL_DSP_SETFRAGMENT, &fragment) < 0 ||
        ioctl(output->fd, SNDCTL_DSP_SETFMT, &format) < 0 ||
        format != AFMT_S16_LE || ioctl(output->fd, SNDCTL_DSP_CHANNELS, &channels) < 0 ||
        channels < 2 || channels > 16 || ioctl(output->fd, SNDCTL_DSP_SPEED, &rate) < 0) {
        if (output->fd >= 0) {
            close(output->fd);
        }
        free(output);
        return -ENODEV;
    }
    io.write_bytes = doom_audio_fd_write;
    io.recover = doom_audio_fd_recover;
    io.available_frames = doom_audio_fd_available;
    io.opaque = output;
    output->output_channels = (uint32_t)channels;
    output->frame_bytes = (uint32_t)channels * sizeof(int16_t);
    output->pending = (uint8_t *)malloc(512U * output->frame_bytes);
    if (!output->pending) {
        close(output->fd);
        free(output);
        return -ENOMEM;
    }
    output->io = io;
    output->capacity = 512U * output->frame_bytes;
    fprintf(stderr, "[doom-audio] PCM configured rate=%d channels=%d frame_bytes=%u\n",
            rate, channels, output->frame_bytes);
    if (actual_rate) {
        *actual_rate = (uint32_t)rate;
    }
    *out = output;
    return 0;
}

int doom_audio_get_stats(const struct doom_audio_output *output,
                         struct doom_audio_stats *stats)
{
    if (!output || !stats) return -EINVAL;
    *stats = output->stats;
    return 0;
}

void doom_audio_close(struct doom_audio_output *output)
{
    if (!output) {
        return;
    }
    if (output->fd >= 0) {
        fprintf(stderr, "[doom-audio] stats written_bytes=%llu startup_recoveries=%llu "
                "xrun_recoveries=%llu suspend_recoveries=%llu\n",
                (unsigned long long)output->stats.written_bytes,
                (unsigned long long)output->stats.startup_recoveries,
                (unsigned long long)output->stats.xrun_recoveries,
                (unsigned long long)output->stats.suspend_recoveries);
        close(output->fd);
    }
    free(output->pending);
    free(output->interleaved);
    free(output);
}
