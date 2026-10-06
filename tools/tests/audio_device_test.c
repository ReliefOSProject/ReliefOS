#include <assert.h>
#include <errno.h>
#include <linux/poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <reliefnt/sched.h>

#define AUDIO_FIXTURE_REAL_WAIT
#define AUDIO_FIXTURE_NO_PTHREAD
#include "audio_fixture.h"
#include "audio_fake_card.h"

#define sched_signal_wait_current device_test_real_sched_signal_wait_current
#include "../../kernel/reliefnt/kernel/reliefnt/sched/sched.c"
#undef sched_signal_wait_current
#include "sysv_shm_fixture_stubs.h"
#include "../../kernel/reliefnt/kernel/reliefnt/wait.c"

#include "../../kernel/reliefnt/kernel/reliefnt/audio/core.c"
#include "../../kernel/reliefnt/kernel/reliefnt/audio/pcm.c"
#include "../../kernel/reliefnt/kernel/reliefnt/audio/device.c"
#include "../../kernel/reliefnt/kernel/reliefnt/audio/alsa_pcm.c"
#include "audio_device_fixture.h"

/* Host allocator boundary; descriptor growth stays in the real scheduler. */
void *kernel_malloc(uint64_t size) { return malloc(size); }
void kernel_free(void *pointer) { free(pointer); }

static struct audio_test_card *wait_card;
static int wake_before_sleep, wake_after_sleep;
void sched_signal_wait_current(uint64_t deadline)
{
    if (wake_before_sleep) {
        wake_before_sleep = 0;
        audio_test_advance(wait_card, 512, 0);
    }
    device_test_real_sched_signal_wait_current(deadline);
    if (wake_after_sleep) {
        wake_after_sleep = 0;
        audio_test_advance(wait_card, 512, 0);
    }
}

bool user_range_ok(uint64_t address, uint64_t length)
{ return address >= 4096 && length <= UINT64_MAX - address; }
bool user_range_writable(uint64_t address, uint64_t length)
{ return user_range_ok(address, length); }

/* Device/OSS wait fixtures link the PCM core without the ALSA timer module. */
void audio_timer_period_elapsed(uint32_t card, uint32_t device,
                                enum audio_direction direction,
                                uint64_t frames, int error)
{
    (void)card; (void)device; (void)direction; (void)frames; (void)error;
}

static void open_leaves_pcm_unconfigured(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    struct task_file file = {.used = 1,
        .flags = RELIEFOS_O_WRONLY | TASK_FILE_FLAG_DEV_NODE,
        .node = {.first_cluster = STORAGE_DEV_KIND_AUDIO,
                 .volume_id = AUDIO_DEVICE_VOLUME_ID(card.id, 0, AUDIO_PLAYBACK)}};
    assert(!audio_device_open(NULL, &file));
    struct snd_pcm_status status;
    assert(!audio_device_ioctl(NULL, &file, SNDRV_PCM_IOCTL_STATUS, (uintptr_t)&status));
    assert(status.state == SNDRV_PCM_STATE_OPEN);
    assert(audio_device_ioctl(NULL, &file, _IOWR('A', 0x7f, int), 1) == -ENOTTY);
    assert(card.prepare_count == 0 && card.trigger_count[AUDIO_START] == 0);
    assert(!audio_device_poll(&file, POLLOUT));
    uint8_t frame[4] = {0};
    assert(audio_device_write(NULL, &file, frame, sizeof(frame)) == -EBADFD);
    assert(!audio_device_close(&file));
    assert(!audio_unregister_card(card.id, 0));
}

static struct task_file audio_test_file(uint32_t card, uint32_t device,
                                        enum audio_direction direction,
                                        uint32_t flags)
{
    return (struct task_file){
        .used = 1,
        .flags = flags | TASK_FILE_FLAG_DEV_NODE,
        .node = {
            .type = RELIEFOS_FS_TYPE_DEVICE,
            .flags = STORAGE_NODE_FLAG_DEV_NODE,
            .first_cluster = STORAGE_DEV_KIND_AUDIO,
            .volume_id = AUDIO_DEVICE_VOLUME_ID(card, device, direction),
        },
    };
}

static void node_names_and_seek(void)
{
    uint32_t card, device;
    enum audio_direction direction;
    assert(!audio_device_parse_name("pcmC0D7p", &card, &device, &direction));
    assert(card == 0 && device == 7 && direction == AUDIO_PLAYBACK);
    assert(!audio_device_parse_name("pcmC12D0c", &card, &device, &direction));
    assert(card == 12 && device == 0 && direction == AUDIO_CAPTURE);
    assert(audio_device_parse_name("pcmC0D8p", &card, &device, &direction) == -EINVAL);
    assert(audio_device_parse_name("pcmC0D0p.extra", &card, &device, &direction) == -EINVAL);
    assert(audio_device_parse_name("pcmC16777216D0p", &card, &device, &direction) == -EINVAL);
    assert(audio_device_parse_name("pcmC0D00c", &card, &device, &direction) == 0);
    assert(AUDIO_DEVICE_CARD(AUDIO_DEVICE_VOLUME_ID(0x123456u, 7u,
                                                    AUDIO_CAPTURE)) == 0x123456u);
    assert(AUDIO_DEVICE_INDEX(AUDIO_DEVICE_VOLUME_ID(0x123456u, 7u,
                                                     AUDIO_CAPTURE)) == 7u);
    assert(AUDIO_DEVICE_DIRECTION(AUDIO_DEVICE_VOLUME_ID(0x123456u, 7u,
                                                         AUDIO_CAPTURE)) == AUDIO_CAPTURE);
}

static void live_card_ordinals_and_tokens(void)
{
    struct audio_test_card first, second;
    uint32_t first_token = 0, second_token = 0;
    struct audio_card_identity identity;
    audio_test_card_init(&first);
    audio_test_card_init(&second);
    assert(!audio_card_snapshot(0, &first_token, &identity));
    assert(!audio_card_snapshot(1, &second_token, NULL));
    assert(first_token == first.id && second_token == second.id);
    assert(!audio_card_index(first.id));
    assert(audio_card_index(second.id) == 1);
    assert(identity.id && identity.name);
    assert(!audio_unregister_card(first.id, 0));
    assert(audio_card_index(first.id) == -2);
    assert(!audio_card_snapshot(0, &first_token, NULL));
    assert(first_token == second.id);
    assert(!audio_unregister_card(second.id, 0));
    assert(audio_card_snapshot(0, NULL, NULL) == -2);
}

static void ofd_transfer_and_nonblock(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    struct task_file file = audio_test_file(card.id, 0, AUDIO_PLAYBACK, RELIEFOS_O_WRONLY);
    assert(!audio_device_open(NULL, &file));

    audio_test_configure_file(&file);

    /* CHANNEL_INFO is historically encoded _IOR but selects an input channel.
     * Test the usercopy adapter, not only the already-covered direct PCM API. */
    struct snd_pcm_channel_info channel = {.channel = 1};
    assert(!audio_device_ioctl(NULL, &file, SNDRV_PCM_IOCTL_CHANNEL_INFO,
                               (uintptr_t)&channel));
    assert(channel.channel == 1 && channel.first == 16 && channel.step == 32);
    channel.channel = 2;
    assert(audio_device_ioctl(NULL, &file, SNDRV_PCM_IOCTL_CHANNEL_INFO,
                              (uintptr_t)&channel) == -EINVAL);

    uint8_t data[8192] = {0};
    assert(audio_device_write(NULL, &file, data, 6) == 4);
    assert(audio_device_write(NULL, &file, data, 8192) == 8188);
    assert(audio_device_write(NULL, &file, data, 4) == -EAGAIN);
    file.flags |= RELIEFOS_O_NONBLOCK;
    assert(audio_device_write(NULL, &file, data, 4) == -EAGAIN);
    assert((audio_device_poll(&file, POLLOUT) & POLLOUT) == 0);
    assert(audio_device_seek(&file, 0, 0, NULL) == -ESPIPE);

    assert(!audio_device_close(&file));
    assert(!audio_unregister_card(card.id, 0));
}

static void capture_readiness_and_disconnect(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    struct task_file file = audio_test_file(card.id, 0, AUDIO_CAPTURE, RELIEFOS_O_RDONLY);
    assert(!audio_device_open(NULL, &file));
    audio_test_configure_file(&file);
    uint8_t data[2048] = {0};
    assert(audio_device_read(NULL, &file, data, sizeof(data)) == -EAGAIN);
    assert((audio_device_poll(&file, POLLIN) & POLLIN) == 0);
    audio_test_advance(&card, 512, 0);
    assert((audio_device_poll(&file, POLLIN) & POLLIN) == POLLIN);
    assert(audio_device_read(NULL, &file, data, sizeof(data)) == sizeof(data));
    assert(!audio_unregister_card(card.id, 1));
    assert(audio_device_read(NULL, &file, data, sizeof(data)) == -ENODEV);
    assert(audio_device_poll(&file, POLLIN) & (POLLERR | POLLHUP));
    assert(!audio_device_close(&file));
}

static void data_mmap_lifetime(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    struct task_file file = audio_test_file(card.id, 0, AUDIO_PLAYBACK, RELIEFOS_O_RDWR);
    assert(!audio_device_open(NULL, &file));
    audio_test_configure_file(&file);
    struct audio_pcm_mmap mapping;
    assert(!audio_device_mmap(NULL, &file, AUDIO_PCM_MMAP_OFFSET_DATA,
                              8192, PROT_READ | PROT_WRITE, &mapping));
    assert(mapping.pcm && mapping.region == AUDIO_PCM_MMAP_DATA);
    struct audio_pcm_mmap metadata;
    assert(!audio_device_mmap(NULL, &file, AUDIO_PCM_MMAP_OFFSET_STATUS_NEW,
                             4096, PROT_READ, &metadata));
    audio_pcm_mmap_release(metadata.pcm, metadata.region);
    assert(!audio_device_close(&file));

    struct task_file blocked = audio_test_file(card.id, 0, AUDIO_PLAYBACK,
                                               RELIEFOS_O_RDWR);
    assert(audio_device_open(NULL, &blocked) == -EBUSY);
    audio_pcm_mmap_release(mapping.pcm,mapping.region);
    assert(!audio_device_open(NULL, &blocked));
    assert(!audio_device_close(&blocked));
    assert(!audio_unregister_card(card.id, 0));
}

static void checked_frame_ioctl_and_link(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    struct task task = {.pid = 702, .state = TASK_RUNNING};
    struct task *entries[] = {&task};
    tasks = entries; task_count = 1; current_pid[0] = task.pid;
    struct task_file *play = sched_task_file_at(&task, 0);
    struct task_file *capture = sched_task_file_at(&task, 1);
    *play = audio_test_file(card.id, 0, AUDIO_PLAYBACK, RELIEFOS_O_RDWR);
    *capture = audio_test_file(card.id, 0, AUDIO_CAPTURE, RELIEFOS_O_RDWR);
    assert(!audio_device_open(&task, play));
    assert(!audio_device_open(&task, capture));
    assert(!audio_device_ioctl(&task, play, SNDRV_PCM_IOCTL_TSTAMP, 1));
    struct snd_xferi x = {.frames = 1, .buf = (void *)1};
    assert(audio_device_ioctl(&task, play, SNDRV_PCM_IOCTL_WRITEI_FRAMES,
                              (uintptr_t)&x) == -EBADFD);
    audio_test_configure_file(play); audio_test_configure_file(capture);
    x.frames = 0; x.buf = NULL; x.result = -99;
    assert(!audio_device_ioctl(&task, play, SNDRV_PCM_IOCTL_WRITEI_FRAMES,
                               (uintptr_t)&x) && !x.result);
    x.frames = 1; x.buf = (void *)1; x.result = -99;
    assert(audio_device_ioctl(&task, play, SNDRV_PCM_IOCTL_WRITEI_FRAMES,
                              (uintptr_t)&x) == -EFAULT && x.result == -99);
    uint8_t data[12288] = {0}; x.buf = data; x.frames = 3072;
    assert(!audio_device_ioctl(&task, play, SNDRV_PCM_IOCTL_WRITEI_FRAMES,
                               (uintptr_t)&x) && x.result == 2048);
    x.frames = 1; x.result = -99;
    assert(audio_device_ioctl(&task, capture, SNDRV_PCM_IOCTL_READI_FRAMES,
                              (uintptr_t)&x) == KERNEL_SYSCALL_BLOCKED);
    assert(x.result == -99 && task.state == TASK_BLOCKED);
    audio_test_advance(&card, 512, 0); task.state = TASK_RUNNING;
    assert(!audio_device_ioctl(&task, capture, SNDRV_PCM_IOCTL_READI_FRAMES,
                               (uintptr_t)&x) && x.result == 1);
    assert(!audio_device_ioctl(&task, play, SNDRV_PCM_IOCTL_LINK, 4));
    assert(audio_device_ioctl(&task, play, SNDRV_PCM_IOCTL_LINK, 999) == -EBADF);
    assert(!audio_device_ioctl(&task, play, SNDRV_PCM_IOCTL_UNLINK, 0));
    assert(!audio_device_close(play)); assert(!audio_device_close(capture));
    sched_task_file_release(&task);
    assert(!audio_unregister_card(card.id, 0));
    tasks = NULL; task_count = 0; current_pid[0] = 0;
}

static void interruptible_pcm_wait(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    struct task task = {.pid = 701, .state = TASK_RUNNING};
    struct task *entries[] = {&task};
    tasks = entries;
    task_count = 1;
    current_pid[0] = task.pid;
    wait_card = &card;
    struct task_file file = audio_test_file(card.id, 0, AUDIO_CAPTURE,
                                            RELIEFOS_O_RDONLY | RELIEFOS_O_NONBLOCK);
    assert(!audio_device_open(&task, &file));
    audio_test_configure_file(&file);
    uint8_t data[2048] = {0};
    assert(audio_device_read(&task, &file, data, sizeof(data)) == -EAGAIN);
    assert(task.state == TASK_RUNNING && !task.waiting_queue);
    file.flags &= ~RELIEFOS_O_NONBLOCK;
    assert(audio_device_read(&task, &file, data, sizeof(data)) == KERNEL_SYSCALL_BLOCKED);
    assert(task.state == TASK_BLOCKED && task.waiting_queue);
    audio_test_advance(&card, 512, 0);
    assert(task.state == TASK_READY && !task.waiting_queue);
    task.state = TASK_RUNNING;
    assert(audio_device_read(&task, &file, data, sizeof(data)) == sizeof(data));
    for (int timing = 0; timing < 2; ++timing) {
        wake_before_sleep = timing == 0;
        wake_after_sleep = timing == 1;
        assert(audio_device_read(&task, &file, data, sizeof(data)) == sizeof(data));
        assert(task.state == TASK_READY && !task.waiting_queue);
        task.state = TASK_RUNNING;
    }
    /* An unmasked pending signal must never be overwritten by sleep. */
    task.pending_signals = 1ULL << 9;
    assert(audio_device_read(&task, &file, data, sizeof(data)) == KERNEL_SYSCALL_BLOCKED);
    assert(task.state == TASK_READY && !task.waiting_queue);
    task.pending_signals = 0;
    task.state = TASK_RUNNING;
    assert(audio_device_read(&task, &file, data, sizeof(data)) == KERNEL_SYSCALL_BLOCKED);
    assert(!audio_unregister_card(card.id, 1));
    assert(task.state == TASK_READY && !task.waiting_queue);
    assert(audio_device_read(&task, &file, data, sizeof(data)) == -ENODEV);
    assert(!audio_device_close(&file));
    tasks = NULL;
    task_count = 0;
    current_pid[0] = 0;
    puts("PCM actual scheduler/wait: nonblocking, IRQ races, signal, disconnect PASS");
}

int main(void)
{
    node_names_and_seek();
    open_leaves_pcm_unconfigured();
    ofd_transfer_and_nonblock();
    capture_readiness_and_disconnect();
    data_mmap_lifetime();
    live_card_ordinals_and_tokens();
    interruptible_pcm_wait();
    checked_frame_ioctl_and_link();
    puts("audio device node/OFD/read-write/poll PASS");
    return 0;
}
