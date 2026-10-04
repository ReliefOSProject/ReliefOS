/* Exercise the actual guest probe's transfer and restoration paths without
 * opening a host sound card. Only standard libc I/O/time calls are scripted. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <time.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sound/asound.h>

static ssize_t scripted_write(int, const void *, size_t);
static ssize_t scripted_pwrite(int, const void *, size_t, off_t);
static int scripted_poll(struct pollfd *, nfds_t, int);
static int scripted_ioctl(int, unsigned long, ...);
static int scripted_clock(clockid_t, struct timespec *);
static int scripted_open(const char *, int, ...);
static int scripted_close(int);
static int scripted_fsync(int);
static int scripted_fstat(int, struct stat *);
#define open scripted_open
#define close scripted_close
#define fsync scripted_fsync
#define fstat scripted_fstat
#define write scripted_write
#define pwrite scripted_pwrite
#define poll scripted_poll
#define ioctl scripted_ioctl
#define clock_gettime scripted_clock
#define main audio_guest_entry
#include "audio_guest_test.c"
#undef main
#undef write
#undef pwrite
#undef poll
#undef ioctl
#undef clock_gettime
#undef open
#undef close
#undef fsync
#undef fstat

static const unsigned char bytes[] = {1, 2, 3, 4, 5, 6, 7};
static int steps[16], step_count, cursor, polls, expired, restoring, capture_direction;
static unsigned restored_mask, unlocked_mask;
static size_t consumed;
static const void *frame_base;
static int capture_scenario, capture_running, capture_dropped;
static unsigned capture_produced, capture_consumed;
static size_t capture_file_bytes;
static int load_scenario, load_calls;
static int drain_scenario, drain_calls;
static uint64_t load_bytes;

static int scripted_open(const char *path, int flags, ...)
{
    if (load_scenario) {
        assert(!strcmp(path, "load.raw") && (flags & O_EXCL));
        return 102;
    }
    assert(capture_scenario);
    if (!strcmp(path, "/dev/snd/pcmC0D0c")) return 99;
    assert(!strcmp(path, "fixture.raw") && (flags & O_EXCL));
    return 101;
}

static int scripted_close(int fd)
{
    if (load_scenario) { assert(fd == 102); return 0; }
    assert(fd == 99 || (capture_scenario && fd == 101));
    return 0;
}

static ssize_t scripted_pwrite(int fd, const void *buffer, size_t length, off_t offset)
{
    assert(load_scenario && fd == 102);
    assert((uint64_t)offset == load_bytes % 262144U);
    assert(length && length <= 4096U && (uint64_t)offset + length <= 262144U);
    if (++load_calls == 1) { errno = EINTR; return -1; }
    if (load_scenario == 2) return 0;
    size_t accepted = length > 733U ? 733U : length;
    for (size_t i = 0; i < accepted; ++i) assert(!((const unsigned char *)buffer)[i]);
    load_bytes += accepted;
    return (ssize_t)accepted;
}

static int scripted_fsync(int fd)
{
    assert(capture_scenario && fd == 101 && capture_dropped);
    return 0;
}

static int scripted_fstat(int fd, struct stat *st)
{
    assert(capture_scenario && fd == 101);
    *st = (struct stat){.st_size = (off_t)capture_file_bytes};
    return 0;
}

static int scripted_clock(clockid_t id, struct timespec *out)
{
    assert(id == CLOCK_MONOTONIC);
    *out = (struct timespec){.tv_sec = expired ? 11 : 0};
    return 0;
}

static ssize_t scripted_write(int fd, const void *data, size_t length)
{
    if (capture_scenario) {
        assert(fd == 101 && !(length % (CHANNELS * sizeof(int16_t))));
        const int16_t *samples = data;
        for (size_t i = 0; i < length / sizeof(*samples); ++i)
            assert(samples[i] == (int16_t)((capture_file_bytes / sizeof(*samples) + i) % 30000));
        capture_file_bytes += length;
        /* A 100ms disk write lets the 48kHz ADC overrun a live small ring. */
        if (capture_running) capture_produced += 4800;
        return (ssize_t)length;
    }
    assert(fd == 99 && cursor < step_count && !restoring);
    assert(data == bytes + consumed && length == sizeof(bytes) - consumed);
    int n = steps[cursor++];
    if (n < 0) { errno = -n; return -1; }
    consumed += (unsigned)n;
    return n;
}

static int scripted_poll(struct pollfd *p, nfds_t count, int timeout)
{
    assert(count == 1 && p->fd == 99 && timeout > 0 && timeout <= 1000);
    ++polls;
    if (drain_scenario) {
        p->revents = drain_scenario == 3 ? POLLNVAL : POLLOUT | POLLERR;
        return 1;
    }
    p->revents = p->events;
    return 1;
}

static int scripted_ioctl(int fd, unsigned long request, ...)
{
    assert(fd == 99);
    va_list args; va_start(args, request); void *argument = va_arg(args, void *); va_end(args);
    if (drain_scenario) {
        if (request == SNDRV_PCM_IOCTL_STATUS) {
            struct snd_pcm_status *status = argument;
            status->state = drain_scenario == 2 ? SNDRV_PCM_STATE_XRUN : SNDRV_PCM_STATE_SETUP;
            return 0;
        }
        assert(request == SNDRV_PCM_IOCTL_DRAIN && !argument);
        ++drain_calls;
        errno = EAGAIN;
        return -1;
    }
    if (capture_scenario) {
        if (request == SNDRV_PCM_IOCTL_START) { capture_running = 1; return 0; }
        if (request == SNDRV_PCM_IOCTL_DROP) {
            capture_running = 0; capture_dropped = 1; return 0;
        }
        if (request == SNDRV_PCM_IOCTL_READI_FRAMES) {
            assert(capture_running);
            if (capture_produced - capture_consumed > BUFFER) {
                errno = EPIPE; return -1;
            }
            struct snd_xferi *x = argument;
            int16_t *samples = x->buf;
            for (unsigned long i = 0; i < x->frames * CHANNELS; ++i)
                samples[i] = (int16_t)((capture_consumed * CHANNELS + i) % 30000);
            capture_consumed += (unsigned)x->frames;
            capture_produced += (unsigned)x->frames;
            x->result = (snd_pcm_sframes_t)x->frames;
            return 0;
        }
        assert(request == SNDRV_PCM_IOCTL_HW_REFINE || request == SNDRV_PCM_IOCTL_HW_PARAMS ||
               request == SNDRV_PCM_IOCTL_SW_PARAMS || request == SNDRV_PCM_IOCTL_PREPARE);
        return 0;
    }
    if (restoring) {
        if (request == SNDRV_CTL_IOCTL_ELEM_UNLOCK) {
            unsigned id = ((struct snd_ctl_elem_id *)argument)->numid;
            unlocked_mask |= 1U << id; return 0;
        }
        struct snd_ctl_elem_value *v = argument;
        unsigned id = v->id.numid;
        if (request == SNDRV_CTL_IOCTL_ELEM_WRITE) {
            restored_mask |= 1U << id;
            if (id == 3) { errno = EIO; return -1; }
            assert(v->value.integer.value[0] == (long)id);
        } else {
            assert(request == SNDRV_CTL_IOCTL_ELEM_READ);
            v->value.integer.value[0] = id;
        }
        return 0;
    }
    assert(request == (capture_direction ? SNDRV_PCM_IOCTL_READI_FRAMES :
                      SNDRV_PCM_IOCTL_WRITEI_FRAMES) && cursor < step_count);
    struct snd_xferi *x = argument;
    assert(x->buf == (const char *)frame_base + consumed * CHANNELS * sizeof(int16_t));
    assert(x->frames == 7 - consumed);
    int n = steps[cursor++];
    if (n < -1000) { errno = -n - 1000; return -1; }
    /* ALSA can return the negative transfer result inside the structure. */
    x->result = n;
    if (n > 0) consumed += (unsigned)n;
    return 0;
}

static void script(const int *values, int count)
{
    memcpy(steps, values, (size_t)count * sizeof(*values));
    step_count = count; cursor = polls = expired = restoring = capture_direction = 0; consumed = 0;
    interrupted = 0; deadline = (struct timespec){.tv_sec = 10};
}

int main(void)
{
    const int short_bytes[] = {1, -EINTR, -EAGAIN, 3, 3};
    script(short_bytes, 5);
    assert(!write_all_nonblocking(99, bytes, sizeof(bytes)));
    assert(consumed == 7 && cursor == 5 && polls == 1);
    const int zero[] = {0}; script(zero, 1);
    assert(write_all_nonblocking(99, bytes, sizeof(bytes)) == -1 && errno == EIO && cursor == 1);
    int16_t samples[14]; frame_base = samples;
    const int short_frames[] = {2, -EINTR, -1000 - EINTR, -EAGAIN, 1, 4}; script(short_frames, 6);
    assert(!transfer(99, samples, 7, 0));
    assert(consumed == 7 && cursor == 6 && polls == 1);
    script(short_frames, 6); capture_direction = 1;
    assert(!transfer(99, samples, 7, 1));
    assert(consumed == 7 && cursor == 6 && polls == 1);
    script(zero, 1);
    assert(transfer(99, samples, 7, 0) == -1 && errno == EIO && cursor == 1);
    script(zero, 1); expired = 1;
    assert(write_all_nonblocking(99, bytes, 7) == -1 && errno == ETIMEDOUT && !cursor);
    expired = 0; interrupted = SIGTERM;
    assert(transfer(99, samples, 7, 0) == -1 && errno == EINTR && !cursor);
    saved_controls = calloc(3, sizeof(*saved_controls)); assert(saved_controls);
    saved_count = 3; control_fd = 99; restoring = 1;
    for (unsigned i = 0; i < 3; ++i) {
        struct saved_control *s = &saved_controls[i];
        s->locked = s->dirty = 1;
        s->info.id.numid = s->value.id.numid = i + 1;
        s->info.type = SNDRV_CTL_ELEM_TYPE_INTEGER; s->info.count = 1;
        s->value.value.integer.value[0] = i + 1;
    }
    /* Cancellation and failure restoring one element cannot skip the rest. */
    assert(restore_controls() == -1);
    assert(restored_mask == 14 && unlocked_mask == 14);
    assert(!saved_controls && !saved_count && control_fd == -1);
    script(zero, 1);
    capture_scenario = 1;
    assert(!capture(0, 0, 4096, "fixture.raw"));
    assert(capture_consumed == 4096 && capture_file_bytes == 16384 && capture_dropped);
    capture_scenario = 0;
    interrupted = 0;
    load_scenario = 1;
    assert(!io_load("load.raw", 2));
    assert(load_bytes == 524288U && load_calls > 128);
    load_scenario = 2;
    load_bytes = 0;
    assert(io_load("load.raw", 1) == -1 && errno == EIO);
    load_scenario = 0;
    for (drain_scenario = 1; drain_scenario <= 3; ++drain_scenario) {
        interrupted = expired = drain_calls = polls = 0;
        deadline = (struct timespec){.tv_sec = 10};
        int result = finish_playback(99, 0);
        if (drain_scenario == 1) assert(!result && drain_calls == 1 && polls == 1);
        if (drain_scenario == 2) assert(result == -1 && errno == EPIPE && drain_calls == 1);
        if (drain_scenario == 3) assert(result == -1 && errno == EIO && drain_calls == 1);
    }
    drain_scenario = 0;
    puts("PASS nonblocking DRAIN: SETUP/POLLERR completion, real XRUN and invalid fd distinguished");
    puts("PASS persistent I/O worker preserves short writes, EINTR and bounded extent; rejects zero progress");
    puts("PASS capture burst survives slow disk output with all samples preserved");
    puts("PASS guest probe byte/frame short transfers, EINTR/EAGAIN, zero, deadline, signal and all-control restoration attempts");
    return 0;
}
