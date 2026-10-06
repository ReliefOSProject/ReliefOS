/* Same source on Linux and ReliefOS: libc + public Linux sound UAPI only. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define AUDIO_ABI_NO_MAIN
#include "audio_abi_layout_test.c"

#ifndef AUDIO_GUEST_PERIOD_FRAMES
#define AUDIO_GUEST_PERIOD_FRAMES 512
#endif
#ifndef AUDIO_GUEST_BUFFER_FRAMES
#define AUDIO_GUEST_BUFFER_FRAMES 2048
#endif
enum { RATE = 48000, CHANNELS = 2,
       PERIOD = AUDIO_GUEST_PERIOD_FRAMES, BUFFER = AUDIO_GUEST_BUFFER_FRAMES };
static volatile sig_atomic_t interrupted;
static struct timespec deadline;

static void on_signal(int number) { interrupted = number; }

static int error(const char *operation)
{
    int saved = errno;
    fprintf(stderr, "FAIL operation=%s errno=%d (%s)\n", operation, saved,
            strerror(saved));
    errno = saved;
    return -1;
}

static int live(void)
{
    struct timespec now;
    if (interrupted) { errno = EINTR; return 0; }
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    if (now.tv_sec > deadline.tv_sec ||
        (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
        errno = ETIMEDOUT;
        return 0;
    }
    return 1;
}

static int wait_ready(int fd, short events)
{
    while (live()) {
        struct pollfd p = {.fd = fd, .events = events};
        int n = poll(&p, 1, 100);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = EIO;
            return -1;
        }
        if (n && (p.revents & events)) return 0;
    }
    return -1;
}

static int call_ioctl(int fd, unsigned long request, void *argument,
                      const char *operation)
{
    while (live()) {
        int n = ioctl(fd, request, argument);
        if (n >= 0) return 0;
        if (errno != EINTR) return error(operation);
    }
    return error(operation);
}

/* Byte transfers preserve arbitrary OSS short-write tails. Zero is failure. */
static int write_all_nonblocking(int fd, const void *bytes, size_t length)
{
    size_t done = 0;
    while (done < length && live()) {
        ssize_t n = write(fd, (const char *)bytes + done, length - done);
        if (n > 0 && (size_t)n <= length - done) { done += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN && !wait_ready(fd, POLLOUT)) continue;
        if (n >= 0) errno = EIO;
        return -1;
    }
    return done == length ? 0 : -1;
}

static int open_pcm(unsigned card, unsigned device, int capture)
{
    char path[80];
    snprintf(path, sizeof(path), "/dev/snd/pcmC%uD%u%c", card, device,
             capture ? 'c' : 'p');
    /* ALSA opens an OFD read/write in either direction; mmap metadata needs
     * readable access even for playback. The node chooses the stream. */
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) error(path);
    else printf("OPEN path=%s\n", path);
    return fd;
}

static void exact(struct snd_pcm_hw_params *p, unsigned parameter, unsigned value)
{
    struct snd_interval *i = &p->intervals[parameter - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    *i = (struct snd_interval){.min = value, .max = value, .integer = 1};
}

static void mask_one(struct snd_pcm_hw_params *p, unsigned parameter, unsigned value)
{
    struct snd_mask *m = &p->masks[parameter - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    memset(m, 0, sizeof(*m));
    m->bits[value / 32] = 1U << (value % 32);
}

static int configure(int fd, int mmap_access)
{
    struct snd_pcm_hw_params hw = {0};
    for (size_t i = 0; i < sizeof(hw.masks) / sizeof(hw.masks[0]); ++i)
        memset(&hw.masks[i], 0xff, sizeof(hw.masks[i]));
    for (size_t i = 0; i < sizeof(hw.intervals) / sizeof(hw.intervals[0]); ++i)
        hw.intervals[i].max = UINT_MAX;
    mask_one(&hw, SNDRV_PCM_HW_PARAM_ACCESS, mmap_access ?
             SNDRV_PCM_ACCESS_MMAP_INTERLEAVED : SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    mask_one(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    mask_one(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
    exact(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, CHANNELS);
    exact(&hw, SNDRV_PCM_HW_PARAM_RATE, RATE);
    exact(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, PERIOD);
    exact(&hw, SNDRV_PCM_HW_PARAM_PERIODS, BUFFER / PERIOD);
    exact(&hw, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, BUFFER);
    hw.rmask = UINT_MAX;
    if (call_ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &hw, "HW_REFINE") ||
        call_ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw, "HW_PARAMS")) return -1;
    struct snd_pcm_sw_params sw = {
        .tstamp_mode = SNDRV_PCM_TSTAMP_ENABLE, .period_step = 1,
        .avail_min = PERIOD, .xfer_align = 1, .start_threshold = PERIOD,
        .stop_threshold = BUFFER, .boundary = BUFFER,
    };
    while (sw.boundary <= (LONG_MAX - BUFFER) / 2) sw.boundary *= 2;
    if (call_ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw, "SW_PARAMS") ||
        call_ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, NULL, "PREPARE")) return -1;
    printf("PARAMS format=S16_LE rate=%d channels=%d period=%d buffer=%d boundary=%lu\n",
           RATE, CHANNELS, PERIOD, BUFFER, (unsigned long)sw.boundary);
    return 0;
}

static void tone(int16_t *out, unsigned first, unsigned frames)
{
    const double tau = 6.2831853071795864769;
    for (unsigned i = 0; i < frames; ++i) {
        out[i * 2] = (int16_t)(12000 * sin(tau * ((first + i) % RATE) * 1000 / RATE));
        out[i * 2 + 1] = (int16_t)(12000 * sin(tau * ((first + i) % RATE) * 2000 / RATE));
    }
}

/* READI/WRITEI results are frames, unlike read/write results in bytes. */
static int transfer(int fd, int16_t *samples, unsigned frames, int capture)
{
    unsigned done = 0;
    while (done < frames && live()) {
        struct snd_xferi x = {.buf = samples + done * CHANNELS, .frames = frames - done};
        int n = ioctl(fd, capture ? SNDRV_PCM_IOCTL_READI_FRAMES :
                      SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x);
        if (n >= 0 && x.result > 0 && (unsigned long)x.result <= frames - done) {
            done += (unsigned)x.result;
            continue;
        }
        if (n >= 0) errno = x.result < 0 ? (int)-x.result : EIO;
        if (errno == EINTR) continue;
        if (errno == EAGAIN && !wait_ready(fd, capture ? POLLIN : POLLOUT)) continue;
        return error(capture ? "READI_FRAMES" : "WRITEI_FRAMES");
    }
    return done == frames ? 0 : error("transfer deadline");
}

static int finish_playback(int fd, int oss)
{
    while (live()) {
        if (ioctl(fd, oss ? SNDCTL_DSP_SYNC : SNDRV_PCM_IOCTL_DRAIN, NULL) >= 0) return 0;
        if (errno == EINTR) continue;
        if (errno == EAGAIN) {
            struct pollfd p = {.fd = fd, .events = POLLOUT};
            int n = poll(&p, 1, 100);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) return error("drain poll");
            if (p.revents & (POLLHUP | POLLNVAL)) {
                errno = EIO;
                return error("drain disconnected fd");
            }
            if (!oss) {
                struct snd_pcm_status status = {0};
                if (call_ioctl(fd, SNDRV_PCM_IOCTL_STATUS, &status, "drain status")) return -1;
                /* Linux keeps nonblocking DRAIN at EAGAIN even in SETUP.
                 * Observe its documented completion state; XRUN still fails. */
                if (status.state == SNDRV_PCM_STATE_SETUP) return 0;
                if (status.state == SNDRV_PCM_STATE_XRUN) {
                    errno = EPIPE;
                    return error("drain XRUN");
                }
                if (status.state == SNDRV_PCM_STATE_DISCONNECTED) {
                    errno = ENODEV;
                    return error("drain disconnected");
                }
            }
            continue;
        }
        return error(oss ? "OSS_SYNC" : "DRAIN");
    }
    return error("drain deadline");
}

static int play(unsigned card, unsigned device, int oss, unsigned total_frames)
{
    int fd = oss ? open("/dev/dsp", O_WRONLY | O_NONBLOCK | O_CLOEXEC) :
                   open_pcm(card, device, 0);
    if (fd < 0) return oss ? error("open /dev/dsp (first PCM card)") : -1;
    int result = -1;
    if (oss) {
        int fragment = (4 << 16) | 11, format = AFMT_S16_LE, channels = CHANNELS, rate = RATE;
        printf("OSS mapping=first-PCM-card requested-ALSA-card=%u\n", card);
        int formats=0,caps=0;
        if(call_ioctl(fd,SNDCTL_DSP_GETFMTS,&formats,"OSS_GETFMTS") ||
           call_ioctl(fd,SNDCTL_DSP_GETCAPS,&caps,"OSS_GETCAPS"))goto done;
        if(!(formats&AFMT_S16_LE)){errno=EINVAL;error("OSS missing S16 format");goto done;}
        printf("OSS formats=0x%x capabilities=0x%x\n",formats,caps);
        if (call_ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &fragment, "OSS_SETFRAGMENT") ||
            call_ioctl(fd, SNDCTL_DSP_SETFMT, &format, "OSS_SETFMT") ||
            call_ioctl(fd, SNDCTL_DSP_CHANNELS, &channels, "OSS_CHANNELS") ||
            call_ioctl(fd, SNDCTL_DSP_SPEED, &rate, "OSS_SPEED")) goto done;
        if (format != AFMT_S16_LE || channels != CHANNELS || rate != RATE) {
            errno = EINVAL; error("OSS negotiated parameters"); goto done;
        }
    } else if (configure(fd, 0)) goto done;
    for (unsigned first = 0; first < total_frames; first += PERIOD) {
        unsigned frames = total_frames - first < PERIOD ? total_frames - first : PERIOD;
        int16_t samples[PERIOD * CHANNELS];
        tone(samples, first, frames);
        if (oss ? write_all_nonblocking(fd, samples, frames * CHANNELS * sizeof(int16_t)) :
                  transfer(fd, samples, frames, 0)) {
            if (oss) error("OSS write");
            goto done;
        }
    }
    result = finish_playback(fd, oss);
    if (!result) printf("PASS backend=%s submitted_frames=%u tone_left=1000 tone_right=2000 seconds=%.3f\n",
                        oss ? "OSS" : "ALSA", total_frames, (double)total_frames / RATE);
done:
    if (result) ioctl(fd, oss ? SNDCTL_DSP_RESET : SNDRV_PCM_IOCTL_DROP, NULL);
    close(fd);
    return result;
}

static int capture(unsigned card, unsigned device, unsigned frames, const char *output)
{
    int fd = open_pcm(card, device, 1);
    if (fd < 0) return -1;
    int result = -1, file = -1, started = 0;
    size_t bytes = (size_t)frames * CHANNELS * sizeof(int16_t);
    int16_t *samples = NULL;
    if (configure(fd, 0)) goto done;
    samples = malloc(bytes);
    if (!samples) { errno = ENOMEM; error("capture sample allocation"); goto done; }
    /* Initialize sample storage before START. The bounded capture burst must not
     * wait for disk I/O while a 2048-frame hardware ring is producing data. */
    memset(samples, 0, bytes);
    file = open(output, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (file < 0) { error("create raw PCM (exclusive)"); goto done; }
    if (call_ioctl(fd, SNDRV_PCM_IOCTL_START, NULL, "capture START")) goto done;
    started = 1;
    unsigned done_frames = 0;
    while (done_frames < frames) {
        unsigned count = frames - done_frames < PERIOD ? frames - done_frames : PERIOD;
        if (transfer(fd, samples + done_frames * CHANNELS, count, 1)) goto done;
        done_frames += count;
    }
    result = 0;
done:
    if (started && call_ioctl(fd, SNDRV_PCM_IOCTL_DROP, NULL, "capture DROP") && !result)
        result = -1;
    if (file >= 0 && !result) {
        struct stat st;
        if (write_all_nonblocking(file, samples, bytes)) result = error("capture file write");
        else if (fsync(file) || fstat(file, &st)) result = error("sync/stat raw PCM");
        else if (st.st_size != (off_t)((uint64_t)frames * CHANNELS * sizeof(int16_t))) {
            errno = EIO; result = error("raw PCM size");
        }
    }
    if (file >= 0 && close(file) && !result) result = error("close raw PCM");
    close(fd);
    free(samples);
    if (!result) printf("PASS capture_frames=%u bytes=%u output=%s format=S16_LE rate=%d channels=%d\n",
           frames, frames * CHANNELS * (unsigned)sizeof(int16_t), output, RATE, CHANNELS);
    return result;
}

static int timer(unsigned card, unsigned device)
{
    int fd = open("/dev/snd/timer", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return error("open /dev/snd/timer");
    int result = -1;
    int version = 0;
    struct snd_timer_select select = {
        .id = {
            .dev_class = SNDRV_TIMER_CLASS_PCM,
            .dev_sclass = SNDRV_TIMER_SCLASS_NONE,
            .card = (int)card,
            .device = (int)device,
            .subdevice = 0,
        },
    };
    struct snd_timer_info info = {0};
    struct snd_timer_params params = {
        .flags = SNDRV_TIMER_PSFLG_AUTO,
        .ticks = 1,
        .queue_size = 64,
    };
    struct snd_timer_status status = {0};
    struct snd_timer_read events[8] = {0};
    if (call_ioctl(fd, SNDRV_TIMER_IOCTL_PVERSION, &version, "TIMER_PVERSION") ||
        version != SNDRV_TIMER_VERSION) {
        errno = EPROTO;
        error("TIMER_PVERSION value");
        goto done;
    }
    if (call_ioctl(fd, SNDRV_TIMER_IOCTL_SELECT, &select, "TIMER_SELECT") ||
        call_ioctl(fd, SNDRV_TIMER_IOCTL_INFO, &info, "TIMER_INFO") ||
        call_ioctl(fd, SNDRV_TIMER_IOCTL_PARAMS, &params, "TIMER_PARAMS") ||
        call_ioctl(fd, SNDRV_TIMER_IOCTL_STATUS, &status, "TIMER_STATUS") ||
        call_ioctl(fd, SNDRV_TIMER_IOCTL_START, NULL, "TIMER_START")) goto done;
    if (play(card, device, 0, RATE)) goto done;
    if (wait_ready(fd, POLLIN)) goto done;
    ssize_t bytes = read(fd, events, sizeof(events));
    if (bytes < (ssize_t)sizeof(events[0]) || bytes % (ssize_t)sizeof(events[0]) ||
        events[0].ticks == 0) {
        errno = EIO;
        error("TIMER_READ events");
        goto done;
    }
    if (call_ioctl(fd, SNDRV_TIMER_IOCTL_STATUS, &status, "TIMER_STATUS after tick") ||
        call_ioctl(fd, SNDRV_TIMER_IOCTL_STOP, NULL, "TIMER_STOP")) goto done;
    printf("PASS timer pversion=0x%x events=%zd resolution=%u queue=%u lost=%u\n",
           version, bytes / (ssize_t)sizeof(events[0]), status.resolution,
           status.queue, status.lost);
    result = 0;
done:
    if (result) ioctl(fd, SNDRV_TIMER_IOCTL_STOP, NULL);
    close(fd);
    return result;
}

static int lifetime(unsigned card, unsigned device)
{
    int fd = open_pcm(card, device, 0), result = -1;
    if (fd < 0) return -1;
    void *data = MAP_FAILED, *status = MAP_FAILED, *control = MAP_FAILED;
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) { errno = EINVAL; error("page size"); goto done; }
    if (configure(fd, 1)) goto done;
    data = mmap(NULL, BUFFER * CHANNELS * sizeof(int16_t), PROT_READ | PROT_WRITE,
                MAP_SHARED, fd, SNDRV_PCM_MMAP_OFFSET_DATA);
    status = mmap(NULL, (size_t)page, PROT_READ, MAP_SHARED, fd, SNDRV_PCM_MMAP_OFFSET_STATUS);
    control = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                   SNDRV_PCM_MMAP_OFFSET_CONTROL);
    if (data == MAP_FAILED || status == MAP_FAILED || control == MAP_FAILED) {
        error("PCM mmap"); goto done;
    }
    if (!mprotect(status, (size_t)page, PROT_READ | PROT_WRITE) || errno != EACCES) {
        errno = EPROTO; error("status mprotect expected EACCES"); goto done;
    }
    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_FREE, NULL) != -1 || errno != EBADFD) {
        errno = EPROTO; error("mapped HW_FREE expected EBADFD"); goto done;
    }
    int duplicate = dup(fd);
    if (duplicate < 0) { error("dup PCM"); goto done; }
    close(fd); fd = duplicate;
    snd_pcm_state_t state = ((volatile struct snd_pcm_mmap_status *)status)->state;
    pid_t child = fork();
    if (child < 0) { error("fork PCM mapping"); goto done; }
    if (!child) {
        _exit(((volatile struct snd_pcm_mmap_status *)status)->state == state ? 0 : 1);
    }
    int child_status = 0;
    while (live()) {
        pid_t found = waitpid(child, &child_status, WNOHANG);
        if (found == child) break;
        if (found < 0 && errno != EINTR) { error("waitpid"); goto child_failure; }
        struct timespec pause = {.tv_nsec = 1000000};
        nanosleep(&pause, NULL);
    }
    if (!live()) { error("fork deadline"); goto child_failure; }
    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status)) {
        errno = EPROTO; error("fork status mapping"); goto done;
    }
    ((volatile int16_t *)data)[0] = 123;
    close(fd); fd = -1;
    /* Closing can change the published state; only storage lifetime and a
     * valid public state are promised after the final OFD is released. */
    if (((volatile struct snd_pcm_mmap_status *)status)->state > SNDRV_PCM_STATE_DISCONNECTED) {
        errno = EPROTO; error("mapping after last close"); goto done;
    }
    if (((volatile int16_t *)data)[0] != 123) {
        errno = EPROTO; error("data after last close"); goto done;
    }
    result = 0;
    goto done;
child_failure:
    kill(child, SIGKILL);
    (void)waitpid(child, NULL, WNOHANG);
done:
    if (data != MAP_FAILED && munmap(data, BUFFER * CHANNELS * sizeof(int16_t))) result = error("unmap data");
    if (status != MAP_FAILED && munmap(status, (size_t)page)) result = error("unmap status");
    if (control != MAP_FAILED && munmap(control, (size_t)page)) result = error("unmap control");
    if (fd >= 0) { ioctl(fd, SNDRV_PCM_IOCTL_DROP, NULL); close(fd); }
    if (!result) puts("PASS lifetime=status-protection,mapped-HW_FREE,dup,fork,last-close,last-unmap");
    return result;
}

struct saved_control {
    struct snd_ctl_elem_info info;
    struct snd_ctl_elem_value value;
    int locked, dirty;
};
static struct saved_control *saved_controls;
static unsigned saved_count;
static int control_fd = -1;

static int equal_value(const struct saved_control *saved, const struct snd_ctl_elem_value *v)
{
    for (unsigned i = 0; i < saved->info.count; ++i) {
        if (saved->info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
            if (v->value.enumerated.item[i] != saved->value.value.enumerated.item[i]) return 0;
        } else if (v->value.integer.value[i] != saved->value.value.integer.value[i]) return 0;
    }
    return 1;
}

/* Cleanup ignores cancellation/deadline so every saved value gets an attempt.
 * The fd is nonblocking; EINTR retries are finite. SIGKILL cannot be recovered. */
static int cleanup_ioctl(unsigned long request, void *argument)
{
    for (unsigned i = 0; i < 8; ++i) {
        if (ioctl(control_fd, request, argument) >= 0) return 0;
        if (errno != EINTR) break;
    }
    return -1;
}

static int restore_controls(void)
{
    int result = 0;
    for (unsigned n = saved_count; n; --n) {
        struct saved_control *s = &saved_controls[n - 1];
        if (s->dirty) {
            struct snd_ctl_elem_value original = s->value, check = {.id = s->value.id};
            if (cleanup_ioctl(SNDRV_CTL_IOCTL_ELEM_WRITE, &original) ||
                cleanup_ioctl(SNDRV_CTL_IOCTL_ELEM_READ, &check) || !equal_value(s, &check)) {
                if (!errno) errno = EPROTO;
                error("restore control"); result = -1;
            } else {
                s->dirty = 0;
                printf("RESTORED numid=%u\n", s->info.id.numid);
            }
        }
        if (s->locked) {
            if (cleanup_ioctl(SNDRV_CTL_IOCTL_ELEM_UNLOCK, &s->info.id)) {
                error("unlock control"); result = -1;
            }
            s->locked = 0;
        }
    }
    free(saved_controls); saved_controls = NULL; saved_count = 0;
    if (control_fd >= 0) close(control_fd);
    control_fd = -1;
    return result;
}

static int controls(unsigned card)
{
    char path[80];
    snprintf(path, sizeof(path), "/dev/snd/controlC%u", card);
    control_fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (control_fd < 0) return error("open control");
    int result = -1;
    struct snd_ctl_elem_id *ids = NULL;
    struct snd_ctl_elem_list list = {0};
    if (call_ioctl(control_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list, "ELEM_LIST count")) goto done;
    if (!list.count || list.count > 4096) { errno = E2BIG; error("control count"); goto done; }
    ids = calloc(list.count, sizeof(*ids));
    saved_controls = calloc(list.count, sizeof(*saved_controls));
    if (!ids || !saved_controls) { errno = ENOMEM; error("snapshot allocation"); goto done; }
    list.space = list.count; list.pids = ids;
    if (call_ioctl(control_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list, "ELEM_LIST ids")) goto done;
    if (list.used > list.space) { errno = EPROTO; error("control list used"); goto done; }
    /* Capture every candidate before the first value mutation, under locks. */
    for (unsigned i = 0; i < list.used; ++i) {
        struct saved_control *s = &saved_controls[saved_count];
        s->info.id = ids[i];
        if (call_ioctl(control_fd, SNDRV_CTL_IOCTL_ELEM_INFO, &s->info, "ELEM_INFO")) goto done;
        unsigned access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_WRITE;
        if ((s->info.access & access) != access || !s->info.count || s->info.count > 128 ||
            (s->info.access & (SNDRV_CTL_ELEM_ACCESS_INACTIVE | SNDRV_CTL_ELEM_ACCESS_LOCK)) ||
            (s->info.type != SNDRV_CTL_ELEM_TYPE_BOOLEAN &&
             s->info.type != SNDRV_CTL_ELEM_TYPE_INTEGER &&
             s->info.type != SNDRV_CTL_ELEM_TYPE_ENUMERATED)) continue;
        if (call_ioctl(control_fd, SNDRV_CTL_IOCTL_ELEM_LOCK, &s->info.id, "ELEM_LOCK")) goto done;
        s->locked = 1; ++saved_count;
        s->value.id = s->info.id;
        if (call_ioctl(control_fd, SNDRV_CTL_IOCTL_ELEM_READ, &s->value, "snapshot ELEM_READ")) goto done;
        printf("SNAPSHOT numid=%u type=%u count=%u name=%.44s\n", s->info.id.numid,
               s->info.type, s->info.count, s->info.id.name);
    }
    int subscribe = 1;
    if (call_ioctl(control_fd, SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &subscribe, "SUBSCRIBE_EVENTS")) goto done;
    unsigned changed[4] = {0};
    for (unsigned i = 0; i < saved_count; ++i) {
        struct saved_control *s = &saved_controls[i];
        struct snd_ctl_elem_value next = s->value;
        if (s->info.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN) {
            for (unsigned j = 0; j < s->info.count; ++j)
                next.value.integer.value[j] = !next.value.integer.value[j];
        } else if (s->info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
            unsigned items = s->info.value.enumerated.items;
            if (items < 2) continue;
            for (unsigned j = 0; j < s->info.count; ++j)
                next.value.enumerated.item[j] = (next.value.enumerated.item[j] + 1U) % items;
        } else {
            long min = s->info.value.integer.min, max = s->info.value.integer.max;
            long step = s->info.value.integer.step > 0 ? s->info.value.integer.step : 1;
            if (min >= max || min > LONG_MAX - step || min + step > max) continue;
            for (unsigned j = 0; j < s->info.count; ++j)
                next.value.integer.value[j] = s->value.value.integer.value[j] == min ? min + step : min;
        }
        s->dirty = 1; /* Also restore after a partially applied failed write. */
        if (call_ioctl(control_fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &next, "test ELEM_WRITE")) goto done;
        struct snd_ctl_elem_value check = {.id = s->info.id};
        if (call_ioctl(control_fd, SNDRV_CTL_IOCTL_ELEM_READ, &check, "test ELEM_READ")) goto done;
        struct saved_control expected = *s; expected.value = next;
        if (!equal_value(&expected, &check)) { errno = EPROTO; error("control readback"); goto done; }
        int found = 0;
        while (!found && live()) {
            struct snd_ctl_event event;
            if (wait_ready(control_fd, POLLIN)) { error("control event poll"); goto done; }
            ssize_t n = read(control_fd, &event, sizeof(event));
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (n != sizeof(event)) { if (n >= 0) errno = EPROTO; error("control event read"); goto done; }
            found = event.type == SNDRV_CTL_EVENT_ELEM &&
                    event.data.elem.id.numid == s->info.id.numid &&
                    (event.data.elem.mask & SNDRV_CTL_EVENT_MASK_VALUE);
        }
        if (!found) { error("control event deadline"); goto done; }
        ++changed[s->info.type];
        printf("TESTED numid=%u type=%u readback=match event=VALUE\n", s->info.id.numid, s->info.type);
    }
    if (!(changed[1] + changed[2] + changed[3])) { errno = ENOTSUP; error("no mutable controls"); goto done; }
    printf("CONTROL coverage mute=%u gain=%u route=%u (zero means unavailable, not passed)\n",
           changed[1], changed[2], changed[3]);
    result = 0;
done:
    free(ids);
    if (restore_controls()) result = -1;
    if (!result) puts("PASS controls readback/events/restoration");
    return result;
}

static int number(const char *text, unsigned max, unsigned *out)
{
    char *end;
    errno = 0;
    unsigned long n = strtoul(text, &end, 10);
    if (errno || !*text || *end || text[0] == '-' || n > max) return -1;
    *out = (unsigned)n;
    return 0;
}

/* Continuous bounded I/O pressure without repeatedly faulting a new executable.
 * A finite iteration count also verifies this worker on native Linux. */
static int io_load(const char *path, unsigned iterations)
{
    static const unsigned char zeros[4096];
    const uint32_t span = 262144U;
    uint64_t bytes = 0, cycles = 0;
    uint32_t offset = 0;
    struct sigaction action = {.sa_handler = on_signal};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL))
        return error("load signal setup");
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return error("load exclusive open");
    int result = 0;
    while (!interrupted && (!iterations || cycles < iterations)) {
        size_t length = span - offset < sizeof(zeros) ? span - offset : sizeof(zeros);
        ssize_t written = pwrite(fd, zeros, length, offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            result = error("load pwrite");
            break;
        }
        if (!written || written > (ssize_t)length) {
            errno = EIO;
            result = error("load short progress");
            break;
        }
        bytes += (uint64_t)written;
        offset += (uint32_t)written;
        /* Preserve a short write's uncompleted suffix before wrapping. */
        if (offset >= span) {
            offset = 0;
            ++cycles;
            if (cycles == 1) {
                printf("[audio-load] I/O active bytes=%u\n", span);
                fflush(stdout);
            }
        }
    }
    if (close(fd) && !result) result = error("load close");
    if (!result) {
        printf("PASS audio-io-load bytes=%llu cycles=%llu\n",
               (unsigned long long)bytes, (unsigned long long)cycles);
        fflush(stdout);
    }
    return result;
}

int main(int argc, char **argv)
{
    unsigned card = 0, device = 0, frames = 4096;int frames_set=0;
    unsigned iterations = 0;
    const char *mode = NULL, *backend = "both", *output = "audio-capture.raw";
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) goto usage;
        if (!strcmp(argv[i], "--card")) { if (number(argv[i + 1], 31, &card)) goto usage; }
        else if (!strcmp(argv[i], "--device")) { if (number(argv[i + 1], 7, &device)) goto usage; }
        else if (!strcmp(argv[i], "--frames")) { if (number(argv[i + 1], RATE * 5, &frames) || !frames) goto usage;frames_set=1; }
        else if (!strcmp(argv[i], "--mode")) mode = argv[i + 1];
        else if (!strcmp(argv[i], "--backend")) backend = argv[i + 1];
        else if (!strcmp(argv[i], "--output")) output = argv[i + 1];
        else if (!strcmp(argv[i], "--iterations")) { if (number(argv[i + 1], 1000000U, &iterations)) goto usage; }
        else goto usage;
    }
    if (!mode || (strcmp(backend, "both") && strcmp(backend, "alsa") && strcmp(backend, "oss"))) goto usage;
    if (!strcmp(mode, "abi")) { print_layout(); return 0; }
    if (!strcmp(mode, "io-load")) return io_load(output, iterations) ? 1 : 0;
    if (strcmp(mode, "play") && strcmp(mode, "capture") && strcmp(mode, "lifetime") &&
        strcmp(mode, "controls") && strcmp(mode, "timer")) goto usage;
    struct sigaction action = {.sa_handler = on_signal};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL) ||
        sigaction(SIGHUP, &action, NULL) || sigaction(SIGALRM, &action, NULL) ||
        clock_gettime(CLOCK_MONOTONIC, &deadline)) return error("deadline/signal setup") != 0;
    deadline.tv_sec += 10;
    alarm(10);
    int result;
    if (!strcmp(mode, "play")) {
        if(!frames_set)frames=RATE*2;
        result = strcmp(backend, "oss") ? play(card, device, 0, frames) : 0;
        if (!result && strcmp(backend, "alsa")) result = play(card, device, 1, frames);
    } else if (!strcmp(mode, "capture")) result = capture(card, device, frames, output);
    else if (!strcmp(mode, "lifetime")) result = lifetime(card, device);
    else if (!strcmp(mode, "controls")) result = controls(card);
    else result = timer(card, device);
    alarm(0);
    if (!result && interrupted) { errno = EINTR; result = error("signal cancellation"); }
    return result ? 1 : 0;
usage:
    fprintf(stderr, "usage: audio-guest --card N --mode abi|play|capture|lifetime|controls|timer|io-load "
            "[--device N] [--backend both|alsa|oss] [--frames N] [--output new.raw] "
            "[--iterations N (io-load; 0 means until signalled)]\n");
    return 2;
}
