#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <linux/poll.h>
#include <linux/soundcard.h>

#include <reliefos/audio_abi.h>
#include <reliefnt/driver_manager.h>
#include <reliefnt/sched.h>
#include <reliefnt/wait.h>

#include "audio_fake_card.h"

static struct reliefos_audio_state fake_audio_state;
static uint32_t fake_audio_generation=1,fake_audio_lease;
static unsigned fake_audio_write_calls;

int driver_manager_audio_configure(const struct reliefos_audio_format *format)
{
    if (!format) return -EINVAL;
    fake_audio_state.sample_rate = format->sample_rate;
    fake_audio_state.channels = format->channels;
    fake_audio_state.bits_per_sample = format->bits_per_sample;
    return 0;
}

long driver_manager_audio_write(const void *data, uint32_t length,
                                uint32_t *out_status)
{
    (void)data;
    ++fake_audio_write_calls;
    if (out_status) *out_status = RELIEFOS_AUDIO_STATUS_OK;
    fake_audio_state.queued_bytes += length;
    return (long)length;
}

void driver_manager_audio_get_state(struct reliefos_audio_state *out)
{
    if (out) *out = fake_audio_state;
}
int driver_manager_audio_acquire(uint32_t *token,struct reliefos_audio_state *out)
{
    if(!fake_audio_state.present || !fake_audio_state.active)return -ENODEV;
    if(fake_audio_lease==fake_audio_generation)return -EBUSY;
    *token=fake_audio_lease=fake_audio_generation;*out=fake_audio_state;return 0;
}
void driver_manager_audio_release(uint32_t token)
{ if(fake_audio_lease==token)fake_audio_lease=0; }
int driver_manager_audio_state_bound(uint32_t token,struct reliefos_audio_state *out)
{
    *out=(struct reliefos_audio_state){0};
    if(token!=fake_audio_generation || token!=fake_audio_lease || !fake_audio_state.active)return -ENODEV;
    *out=fake_audio_state;return 0;
}
int driver_manager_audio_configure_bound(uint32_t token,const struct reliefos_audio_format *format)
{
    struct reliefos_audio_state out;
    if(driver_manager_audio_state_bound(token,&out))return -ENODEV;
    if(format->bits_per_sample!=16 || format->channels!=2)return -EINVAL;
    return driver_manager_audio_configure(format);
}
long driver_manager_audio_write_bound(uint32_t token,const void *data,uint32_t bytes,uint32_t *status)
{
    struct reliefos_audio_state out;
    if(driver_manager_audio_state_bound(token,&out))return -ENODEV;
    return driver_manager_audio_write(data,bytes,status);
}

/* The OSS fixture links the PCM core without the ALSA timer module. */
void audio_timer_period_elapsed(uint32_t card, uint32_t device,
                                enum audio_direction direction,
                                uint64_t frames, int error)
{
    (void)card; (void)device; (void)direction; (void)frames; (void)error;
}

bool user_range_ok(uint64_t ptr, uint64_t len)
{
    return ptr >= 4096u && len <= UINT64_MAX - ptr;
}

bool user_range_writable(uint64_t ptr, uint64_t len)
{
    return user_range_ok(ptr, len);
}

/* The fixture exercises nonblocking OSS paths; provide the scheduler edge
 * symbols so the production wait/retry branches remain link-checked. */
struct task *sched_current_task(void) { return NULL; }
void sched_signal_wait_current(uint64_t deadline) { (void)deadline; }
void sched_wake_interruptible(struct task *task) { (void)task; }
int kernel_wait_queue_add(struct kernel_wait_queue *queue, struct task *task)
{
    (void)queue; (void)task; return 0;
}
void kernel_wait_queue_remove(struct kernel_wait_queue *queue, struct task *task)
{
    (void)queue; (void)task;
}

#include "audio_fixture.h"
#include "../../kernel/reliefnt/kernel/reliefnt/audio/core.c"
#include "../../kernel/reliefnt/kernel/reliefnt/audio/pcm.c"
#include "../../kernel/reliefnt/kernel/reliefnt/audio/oss.c"

static void check_ioctl_state(struct task_file *file)
{
    int value = 0;
    assert(audio_oss_ioctl(NULL, file, SNDCTL_DSP_GETBLKSIZE, 1) == -EFAULT);
    assert(!audio_oss_ioctl(NULL, file, SNDCTL_DSP_GETBLKSIZE, (uint64_t)(uintptr_t)&value));
    assert(value == 2048);
    value = (4 << 16) | 12;
    assert(!audio_oss_ioctl(NULL, file, SNDCTL_DSP_SETFRAGMENT, (uint64_t)(uintptr_t)&value));
    assert(!audio_oss_ioctl(NULL, file, SNDCTL_DSP_GETBLKSIZE, (uint64_t)(uintptr_t)&value));
    assert(value == 4096);
    value = AFMT_U8;
    assert(!audio_oss_ioctl(NULL, file, SNDCTL_DSP_SETFMT, (uint64_t)(uintptr_t)&value));
    assert(value == AFMT_S16_LE);
    value = 1;
    assert(!audio_oss_ioctl(NULL, file, SNDCTL_DSP_STEREO, (uint64_t)(uintptr_t)&value));
    value = 1;
    assert(!audio_oss_ioctl(NULL, file, SNDCTL_DSP_CHANNELS, (uint64_t)(uintptr_t)&value));
    assert(value==2);
    value = PCM_ENABLE_OUTPUT;
    assert(!audio_oss_ioctl(NULL, file, SNDCTL_DSP_SETTRIGGER, (uint64_t)(uintptr_t)&value));
    value = 0;
    assert(!audio_oss_ioctl(NULL, file, SNDCTL_DSP_GETTRIGGER, (uint64_t)(uintptr_t)&value));
    assert(value == PCM_ENABLE_OUTPUT);
    value=0;
    assert(!audio_oss_ioctl(NULL,file,SNDCTL_DSP_GETCAPS,(uintptr_t)&value) && value==0);
    assert(audio_oss_ioctl(NULL,file,SNDCTL_DSP_SETTRIGGER,(uintptr_t)&value)==-ENOTTY);
    assert(audio_oss_ioctl(NULL,file,SNDCTL_DSP_GETISPACE,0)==-ENOTTY);
    value = 0;
    assert(audio_oss_ioctl(NULL, file, SNDCTL_DSP_SETDUPLEX, 0) == -EIO);
    struct audio_buf_info space = {0};
    assert(!audio_oss_ioctl(NULL, file, SNDCTL_DSP_GETOSPACE,
                            (uint64_t)(uintptr_t)&space));
    assert(space.fragsize == 4096 && space.fragstotal == 4);
    assert(audio_oss_ioctl(NULL, file, _IO('P', 127), 0) == -ENOTTY);
}

static void check_hardware_clock_space(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    struct task_file file = {.flags = RELIEFOS_O_NONBLOCK | RELIEFOS_O_WRONLY};
    assert(!audio_oss_open(NULL, &file));
    int fragment = (64 << 16) | 10;
    assert(!audio_oss_ioctl(NULL, &file, SNDCTL_DSP_SETFRAGMENT, (uintptr_t)&fragment));
    struct audio_buf_info space;
    assert(!audio_oss_ioctl(NULL, &file, SNDCTL_DSP_GETOSPACE, (uintptr_t)&space));
    assert(space.bytes == 65536);
    uint8_t pcm[65536] = {0};
    for (unsigned i = 0; i < 3; ++i)
        assert(audio_oss_write(NULL, &file, pcm, 20480) == 20480);
    assert(!card.trigger_count[AUDIO_START]);
    assert(audio_oss_write(NULL, &file, pcm, 4096) == 4096);
    assert(card.trigger_count[AUDIO_START] == 1);
    /* A latched hardware error before the first completion must also release
     * the query's apparent full queue so a polling writer sees EPIPE. */
    audio_test_advance(&card, 0, -EPIPE);
    assert(!audio_oss_ioctl(NULL, &file, SNDCTL_DSP_GETOSPACE, (uintptr_t)&space));
    assert(space.bytes == 65536);
    assert(audio_oss_write(NULL, &file, pcm, 4) == -EPIPE);
    assert(!audio_oss_ioctl(NULL, &file, SNDCTL_DSP_RESET, 0));
    assert(audio_oss_write(NULL, &file, pcm, sizeof(pcm)) == (int)sizeof(pcm));
    audio_test_advance(&card, 256, 0);
    assert(!audio_oss_ioctl(NULL, &file, SNDCTL_DSP_GETOSPACE, (uintptr_t)&space));
    assert(space.bytes == 1024);
    /* Hardware has passed all submitted samples: the queue is empty, and
     * the next write must expose EPIPE instead of a permanently full ring. */
    audio_test_advance(&card, 16384, 0);
    assert(!audio_oss_ioctl(NULL, &file, SNDCTL_DSP_GETOSPACE, (uintptr_t)&space));
    assert(space.bytes == 65536);
    int delay = 99;
    assert(!audio_oss_ioctl(NULL, &file, SNDCTL_DSP_GETODELAY, (uintptr_t)&delay));
    assert(delay == 0);
    assert(audio_oss_write(NULL, &file, pcm, 4) == -EPIPE);
    assert(!audio_oss_ioctl(NULL, &file, SNDCTL_DSP_RESET, 0));
    assert(!audio_oss_close(&file));
    assert(!audio_unregister_card(card.id, 0));
}

int main(void)
{
    fake_audio_state = (struct reliefos_audio_state){.present = 1, .active = 1,
        .sample_rate = 48000, .channels = 2, .bits_per_sample = 16};
    struct task_file file = {.flags = RELIEFOS_O_NONBLOCK | RELIEFOS_O_WRONLY};
    assert(!audio_oss_open(NULL, &file));
    int formats=0;
    assert(!audio_oss_ioctl(NULL,&file,SNDCTL_DSP_GETFMTS,(uintptr_t)&formats));
    assert(formats==AFMT_S16_LE);
    check_ioctl_state(&file);
    uint8_t samples[128] = {0};
    int value = AFMT_S16_LE;
    assert(!audio_oss_ioctl(NULL, &file, SNDCTL_DSP_SETFMT,
                            (uint64_t)(uintptr_t)&value));
    value = 2;
    assert(!audio_oss_ioctl(NULL, &file, SNDCTL_DSP_CHANNELS,
                            (uint64_t)(uintptr_t)&value));
    assert(audio_oss_write(NULL, &file, samples, 2) == 2);
    assert(fake_audio_state.queued_bytes == 0);
    assert(audio_oss_write(NULL, &file, samples + 2, 2) == 2);
    assert(fake_audio_state.queued_bytes == 4);
    assert(audio_oss_write(NULL, &file, samples + 4, sizeof(samples) - 4) ==
           (int)sizeof(samples) - 4);
    assert(fake_audio_write_calls==2); /* One tail frame, one bounded PCM batch. */
    struct count_info pointer = {0};
    assert(audio_oss_ioctl(NULL, &file, SNDCTL_DSP_GETOPTR,
                            (uint64_t)(uintptr_t)&pointer)==-ENOTTY);
    assert(!audio_oss_ioctl(NULL,&file,SNDCTL_DSP_GETODELAY,(uintptr_t)&value));
    assert(value==(int)sizeof(samples));
    assert(audio_oss_poll(&file, POLLOUT) & POLLOUT);
    assert(audio_oss_ioctl(NULL, &file, SNDCTL_DSP_RESET, 0)==-ENOTTY);
    struct task_file duplicate={.flags=RELIEFOS_O_WRONLY};
    assert(audio_oss_open(NULL,&duplicate)==-EBUSY);
    struct task_file input={.flags=RELIEFOS_O_RDONLY};
    assert(!audio_oss_close(&file));
    assert(audio_oss_open(NULL,&input)==-ENODEV);
    assert(!audio_oss_open(NULL,&file));
    /* Reopening after a short-lived player retains the v1 hardware backlog.
     * Negotiating its existing rate must not stop it or fail with EBUSY. */
    assert(fake_audio_state.queued_bytes == sizeof(samples));
    value = 48000;
    assert(!audio_oss_ioctl(NULL, &file, SNDCTL_DSP_SPEED, (uintptr_t)&value));
    assert(value == 48000 && fake_audio_state.queued_bytes == sizeof(samples));
    value = 44100;
    assert(audio_oss_ioctl(NULL, &file, SNDCTL_DSP_SPEED, (uintptr_t)&value) == -EBUSY);
    ++fake_audio_generation; /* A replacement backend cannot consume old fd data. */
    assert(audio_oss_write(NULL,&file,samples,4)==-ENODEV);
    assert(audio_oss_poll(&file,POLLOUT)&POLLERR);
    assert(!audio_oss_open(NULL,&duplicate));
    assert(!audio_oss_close(&file));
    assert(fake_audio_lease==fake_audio_generation);
    assert(!audio_oss_close(&duplicate));

    fake_audio_state = (struct reliefos_audio_state){0};
    struct audio_test_card card;
    audio_test_card_init(&card);
    struct task_file capture_only = {
        .flags = RELIEFOS_O_NONBLOCK | RELIEFOS_O_RDONLY,
    };
    assert(!audio_oss_open(NULL, &capture_only));
    uint8_t capture_probe[4] = {0};
    assert(audio_oss_read(NULL, &capture_only, capture_probe,
                          sizeof(capture_probe)) == -EAGAIN);
    audio_test_advance(&card, 512, 0);
    uint8_t capture_frame[2048] = {0};
    assert(audio_oss_read(NULL, &capture_only, capture_frame,
                          sizeof(capture_frame)) == (int)sizeof(capture_frame));
    assert(!audio_oss_close(&capture_only));

    struct task_file blocking_capture = {.flags = RELIEFOS_O_RDWR};
    assert(!audio_oss_open(NULL, &blocking_capture));
    value = PCM_ENABLE_INPUT;
    assert(!audio_oss_ioctl(NULL, &blocking_capture, SNDCTL_DSP_SETTRIGGER,
                            (uint64_t)(uintptr_t)&value));
    uint8_t blocking_probe[4] = {0};
    assert(audio_oss_read(NULL, &blocking_capture, blocking_probe,
                          sizeof(blocking_probe)) == -EAGAIN);
    assert(!audio_oss_close(&blocking_capture));

    struct task_file duplex = {.flags = RELIEFOS_O_NONBLOCK | RELIEFOS_O_RDWR};
    assert(!audio_oss_open(NULL, &duplex));
    value = 0;
    assert(!audio_oss_ioctl(NULL, &duplex, SNDCTL_DSP_SETDUPLEX, 0));
    value = 0;
    assert(!audio_oss_ioctl(NULL, &duplex, SNDCTL_DSP_SETTRIGGER,
                            (uint64_t)(uintptr_t)&value));
    uint8_t trigger_probe[4] = {0};
    assert(audio_oss_read(NULL, &duplex, trigger_probe, sizeof(trigger_probe)) == -EAGAIN);
    value = PCM_ENABLE_INPUT;
    assert(!audio_oss_ioctl(NULL, &duplex, SNDCTL_DSP_SETTRIGGER,
                            (uint64_t)(uintptr_t)&value));
    value = 48000;
    assert(!audio_oss_ioctl(NULL, &duplex, SNDCTL_DSP_SPEED,
                            (uint64_t)(uintptr_t)&value));
    assert(value == 48000);
    value = 44100;
    assert(!audio_oss_ioctl(NULL, &duplex, SNDCTL_DSP_SPEED,
                           (uint64_t)(uintptr_t)&value));
    assert(value == 48000);
    value = 0;
    assert(!audio_oss_ioctl(NULL, &duplex, SNDCTL_DSP_GETFMTS,
                            (uint64_t)(uintptr_t)&value));
    assert(value == AFMT_S16_LE);
    value = AFMT_U8;
    assert(!audio_oss_ioctl(NULL, &duplex, SNDCTL_DSP_SETFMT,
                           (uint64_t)(uintptr_t)&value));
    assert(value == AFMT_S16_LE);
    struct audio_buf_info input_space = {0};
    assert(!audio_oss_ioctl(NULL, &duplex, SNDCTL_DSP_GETISPACE,
                            (uint64_t)(uintptr_t)&input_space));
    assert(input_space.bytes == 0);
    audio_test_advance(&card, 512, 0);
    assert(!audio_oss_ioctl(NULL, &duplex, SNDCTL_DSP_GETISPACE,
                            (uint64_t)(uintptr_t)&input_space));
    assert(input_space.bytes == 2048);
    uint8_t captured[2048] = {0};
    assert(audio_oss_read(NULL, &duplex, captured, sizeof(captured)) == (int)sizeof(captured));
    struct count_info input_pointer = {0};
    assert(!audio_oss_ioctl(NULL, &duplex, SNDCTL_DSP_GETIPTR,
                            (uint64_t)(uintptr_t)&input_pointer));
    assert(input_pointer.bytes == (int)sizeof(captured));
    assert(!(audio_oss_poll(&duplex, POLLIN) & POLLIN));
    audio_test_advance(&card, 512, 0);
    assert(audio_oss_poll(&duplex, POLLIN) & POLLIN);
    assert(!audio_oss_close(&duplex));
    assert(!audio_unregister_card(card.id, 0));

    struct audio_test_card playback_card;
    audio_test_card_init(&playback_card);
    struct task_file playback = {.flags = RELIEFOS_O_NONBLOCK | RELIEFOS_O_WRONLY};
    assert(!audio_oss_open(NULL, &playback));
    fake_audio_state=(struct reliefos_audio_state){.present=1,.active=1,
        .sample_rate=48000,.channels=2,.bits_per_sample=16};
    value=AFMT_U8;
    assert(!audio_oss_ioctl(NULL,&playback,SNDCTL_DSP_SETFMT,(uintptr_t)&value));
    assert(value==AFMT_S16_LE && !playback.audio_oss_file->legacy_generation);
    fake_audio_state=(struct reliefos_audio_state){0};
    uint8_t frame[4] = {0};
    assert(audio_oss_write(NULL, &playback, frame, 2) == 2);
    assert(audio_oss_write(NULL, &playback, frame + 2, 2) == 2);
    assert(!audio_oss_ioctl(NULL, &playback, SNDCTL_DSP_RESET, 0));
    value = PCM_ENABLE_OUTPUT;
    assert(!audio_oss_ioctl(NULL, &playback, SNDCTL_DSP_SETTRIGGER,
                            (uint64_t)(uintptr_t)&value));
    uint8_t output[16384] = {0};
    assert(audio_oss_write(NULL, &playback, output, sizeof(output)) ==
           (int)sizeof(output));
    assert(audio_oss_write(NULL, &playback, frame, sizeof(frame)) == -EAGAIN);
    struct audio_buf_info output_space = {0};
    assert(!audio_oss_ioctl(NULL, &playback, SNDCTL_DSP_GETOSPACE,
                            (uint64_t)(uintptr_t)&output_space));
    assert(output_space.bytes == 0 && output_space.fragments == 0);
    int delay = 0;
    assert(!audio_oss_ioctl(NULL, &playback, SNDCTL_DSP_GETODELAY,
                            (uint64_t)(uintptr_t)&delay));
    assert(delay == (int)sizeof(output));
    struct count_info output_pointer = {0};
    assert(!audio_oss_ioctl(NULL, &playback, SNDCTL_DSP_GETOPTR,
                            (uint64_t)(uintptr_t)&output_pointer));
    assert(output_pointer.bytes == 0);
    audio_test_advance(&playback_card, 512, 0);
    assert(!audio_oss_ioctl(NULL, &playback, SNDCTL_DSP_GETOSPACE,
                            (uint64_t)(uintptr_t)&output_space));
    assert(output_space.bytes == 2048);
    assert(audio_oss_ioctl(NULL, &playback, SNDCTL_DSP_SYNC, 0) == -EAGAIN);
    audio_test_advance(&playback_card, 3584, 0);
    assert(!audio_oss_ioctl(NULL, &playback, SNDCTL_DSP_SYNC, 0));
    assert(!audio_oss_ioctl(NULL, &playback, SNDCTL_DSP_GETODELAY,
                            (uint64_t)(uintptr_t)&delay));
    assert(delay == 0);
    assert(!audio_oss_close(&playback));
    assert(!audio_unregister_card(playback_card.id, 0));
    check_hardware_clock_space();
    puts("OSS control/queue/fragment PASS");
    return 0;
}
