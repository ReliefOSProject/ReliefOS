/* Production scheduler/wait queues and PCM; inject IRQs only at host edges. */
#define main audio_device_wait_fixture_main
#include "audio_device_test.c"
#undef main
#include <linux/soundcard.h>
#include <reliefnt/driver_manager.h>

static int oss_irq_after_snapshot;
static uint32_t oss_transfer_calls;
static long oss_test_pcm_transfer(struct audio_pcm *pcm, void *kernel_data,
                                  uint32_t frames)
{
    ++oss_transfer_calls;
    return audio_pcm_transfer(pcm, kernel_data, frames);
}
static int oss_test_status(struct audio_pcm *pcm, struct audio_pcm_status *status)
{
    int ret = audio_pcm_status(pcm, status);
    if (oss_irq_after_snapshot) {
        oss_irq_after_snapshot = 0;
        audio_test_advance(wait_card, 512, 0);
    }
    return ret;
}
#define audio_pcm_status(pcm, status) oss_test_status(pcm, status)
#define audio_pcm_transfer(pcm, data, frames) oss_test_pcm_transfer(pcm, data, frames)
#include "../../kernel/reliefnt/kernel/reliefnt/audio/oss.c"
#undef audio_pcm_status
#undef audio_pcm_transfer

void driver_manager_audio_get_state(struct reliefos_audio_state *state)
{ *state = (struct reliefos_audio_state){0}; }
int driver_manager_audio_configure(const struct reliefos_audio_format *format)
{ (void)format; abort(); }
long driver_manager_audio_write(const void *data, uint32_t count, uint32_t *status)
{ (void)data; (void)count; (void)status; abort(); }
int driver_manager_audio_acquire(uint32_t *token,struct reliefos_audio_state *state)
{ (void)token;*state=(struct reliefos_audio_state){0};return -ENODEV; }
void driver_manager_audio_release(uint32_t token) { (void)token;abort(); }
int driver_manager_audio_state_bound(uint32_t token,struct reliefos_audio_state *state)
{ (void)token;(void)state;abort(); }
int driver_manager_audio_configure_bound(uint32_t token,const struct reliefos_audio_format *format)
{ (void)token;(void)format;abort(); }
long driver_manager_audio_write_bound(uint32_t token,const void *data,uint32_t count,uint32_t *status)
{ (void)token;(void)data;(void)count;(void)status;abort(); }

static int u8_sparse_caps(void *opaque, uint32_t device, enum audio_direction direction,
                          struct audio_caps *caps)
{
    if (device != 2 || direction != AUDIO_PLAYBACK) return -ENODEV;
    audio_test_caps(opaque, device, direction, caps);
    caps->formats = AUDIO_FORMAT_U8;
    return 0;
}
static int odd_channels_caps(void *opaque, uint32_t device, enum audio_direction direction,
                              struct audio_caps *caps)
{
    int ret = audio_test_caps(opaque, device, direction, caps);
    caps->channels_max = 3;
    return ret;
}
static int full_prefill_caps(void *opaque, uint32_t device, enum audio_direction direction,
                             struct audio_caps *caps)
{
    int ret = audio_test_caps(opaque, device, direction, caps);
    if (!ret) {
        caps->period_bytes_max = 512 * 1024;
        caps->buffer_bytes_max = 1024 * 1024;
    }
    return ret;
}
static int u8_format_caps(void *opaque, uint32_t device, enum audio_direction direction,
                          struct audio_format_caps *caps)
{
    *caps = (struct audio_format_caps){0};
    int ret = u8_sparse_caps(opaque, device, direction, &caps->pcm);
    caps->format_bits[0] = AUDIO_FORMAT_U8;
    caps->subformats[0] = 1u << AUDIO_SUBFORMAT_STD;
    return ret;
}
static int u8_prepare(void *opaque, struct audio_hw_stream *stream,
                       const struct audio_params_ext *params, const struct audio_dma *dma)
{
    assert(params->format == AUDIO_FORMAT_U8 && params->pcm.sample_bits == 8);
    assert(((uint8_t *)dma->kernel)[0] == 0x80);
    return audio_test_prepare(opaque, stream, &params->pcm, dma);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    struct audio_test_card card;
    if (!strcmp(argv[1], "u8-sparse")) {
        card = (struct audio_test_card){0};
        struct audio_card_ops ops = audio_test_ops;
        ops.pcm_caps = u8_sparse_caps;
        ops.pcm_format_caps = u8_format_caps;
        ops.prepare_format = u8_prepare;
        struct audio_card_identity identity = {.id = "u8", .name = "u8-sparse"};
        assert(!audio_register_card_owned(&identity, &ops, &card, 0x7a, &card.id));
    } else if (!strcmp(argv[1], "odd-channels") || !strcmp(argv[1], "boundary")) {
        card = (struct audio_test_card){0};
        struct audio_card_ops ops = audio_test_ops;
        ops.pcm_caps = odd_channels_caps;
        struct audio_card_identity identity = {.id = "odd", .name = "odd-channels"};
        assert(!audio_register_card_owned(&identity, &ops, &card, 0x7b, &card.id));
    } else if (!strcmp(argv[1], "full-prefill") || !strcmp(argv[1], "grow-buffer")) {
        card = (struct audio_test_card){0};
        struct audio_card_ops ops = audio_test_ops;
        ops.pcm_caps = full_prefill_caps;
        struct audio_card_identity identity = {.id = "full", .name = "full-prefill"};
        assert(!audio_register_card_owned(&identity, &ops, &card, 0x7c, &card.id));
    } else audio_test_card_init(&card);
    struct task task = {.pid = 711, .state = TASK_RUNNING};
    struct task *entries[] = {&task};
    tasks = entries; task_count = 1; current_pid[0] = task.pid; wait_card = &card;
    struct task_file file = {.flags = !strcmp(argv[1], "read") || !strcmp(argv[1], "xrun")
        ? RELIEFOS_O_RDONLY : !strcmp(argv[1], "u8-sparse")
            ? RELIEFOS_O_RDWR : RELIEFOS_O_WRONLY};
    assert(!audio_oss_open(&task, &file));
    uint8_t data[16384] = {0};
    if (!strcmp(argv[1], "read")) {
        assert(!audio_oss_pcm_prepare(file.audio_oss_file, AUDIO_CAPTURE));
        oss_irq_after_snapshot = 1;
        assert(audio_oss_read(&task, &file, data, 2048) == 2048);
        assert(!task.waiting_queue && task.state != TASK_BLOCKED);
    } else if (!strcmp(argv[1], "trigger")) {
        assert(audio_oss_write(&task, &file, data, 2048) == 2048);
        int trigger = PCM_ENABLE_OUTPUT;
        assert(!audio_oss_ioctl(&task, &file, SNDCTL_DSP_SETTRIGGER,
                               (uintptr_t)&trigger));
        assert(card.trigger_count[AUDIO_START] == 1);
        trigger = 0;
        assert(!audio_oss_ioctl(&task, &file, SNDCTL_DSP_SETTRIGGER,
                               (uintptr_t)&trigger));
        assert(card.trigger_count[AUDIO_STOP] >= 1);
        assert(!card.trigger_count[AUDIO_PAUSE]);
        struct audio_pcm_status stopped;
        assert(!audio_pcm_status(file.audio_oss_file->playback_pcm, &stopped));
        assert(stopped.state == AUDIO_PCM_OPEN);
        trigger = PCM_ENABLE_OUTPUT;
        assert(!audio_oss_ioctl(&task, &file, SNDCTL_DSP_SETTRIGGER,
                               (uintptr_t)&trigger));
        assert(!audio_oss_queue(file.audio_oss_file));
    } else if (!strcmp(argv[1], "prefill")) {
        /* OSS playback must queue a startup watermark before starting the
         * hardware.  Starting an almost-empty HDA ring lets the device
         * underrun while a frame-at-a-time writer is still filling the first
         * mixed Doom block.  Leave one period free so the writer cannot block
         * before the stream has been started. */
        assert(audio_oss_write(&task, &file, data, 14336 - 4) == 14336 - 4);
        assert(card.trigger_count[AUDIO_START] == 0);
        assert(audio_oss_write(&task, &file, data, 4) == 4);
        assert(card.trigger_count[AUDIO_START] == 1);
    } else if (!strcmp(argv[1], "grow-buffer")) {
        int fragment = (64 << 16) | 11;
        assert(!audio_oss_ioctl(&task, &file, SNDCTL_DSP_SETFRAGMENT,
                                (uintptr_t)&fragment));
        assert(audio_oss_write(&task, &file, data, sizeof(data)) == (int)sizeof(data));
        assert(!audio_oss_ioctl(&task, &file, SNDCTL_DSP_RESET, 0));
        fragment = (64 << 16) | 13;
        assert(!audio_oss_ioctl(&task, &file, SNDCTL_DSP_SETFRAGMENT,
                                (uintptr_t)&fragment));
        assert(audio_oss_write(&task, &file, data, sizeof(data)) == (int)sizeof(data));
        assert(card.prepared_dma.bytes == 524288);
    } else if (!strcmp(argv[1], "full-prefill")) {
        /* A maximum-size batch ring is the Doom path.  Keep the stream
         * stopped until the complete ring is queued so a slow game tick does
         * not consume its only startup period before the next mix block. */
        int fragment = (64 << 16) | 13;
        assert(!audio_oss_ioctl(&task, &file, SNDCTL_DSP_SETFRAGMENT,
                                (uintptr_t)&fragment));
        for (unsigned i = 0; i < 31; ++i)
            assert(audio_oss_write(&task, &file, data, sizeof(data)) == (int)sizeof(data));
        assert(audio_oss_write(&task, &file, data, 16380) == 16380);
        assert(card.trigger_count[AUDIO_START] == 0);
        assert(audio_oss_write(&task, &file, data, 4) == 4);
        assert(card.trigger_count[AUDIO_START] == 1);
    } else if (!strcmp(argv[1], "bulk")) {
        /* A complete OSS write should reach the PCM ring in one bounded
         * transfer; the old frame-at-a-time path called into the core once
         * per stereo frame and consumed the scheduler budget. */
        assert(audio_oss_write(&task, &file, data, 4096) == 4096);
        assert(oss_transfer_calls == 1);
        assert(card.trigger_count[AUDIO_START] == 0);
    } else if (!strcmp(argv[1], "fragment")) {
        assert(audio_oss_write(&task, &file, data, 2048) == 2048);
        int value = (4 << 16) | 12;
        assert(!audio_oss_ioctl(&task, &file, SNDCTL_DSP_SETFRAGMENT,
                               (uintptr_t)&value));
        assert(audio_oss_write(&task, &file, data, 4096) == 4096);
        assert(card.prepared_params.period_frames == 1024);
        assert(card.prepared_params.buffer_frames == 4096);
    } else if (!strcmp(argv[1], "reset")) {
        assert(audio_oss_write(&task, &file, data, 14336) == 14336);
        audio_test_advance(&card, 512, 0);
        count_info pointer;
        assert(!audio_oss_ioctl(&task, &file, SNDCTL_DSP_GETOPTR, (uintptr_t)&pointer));
        assert(pointer.blocks == 1);
        assert(!audio_oss_ioctl(&task, &file, SNDCTL_DSP_RESET, 0));
        assert(audio_oss_write(&task, &file, data, 2048) == 2048);
        assert(!audio_oss_ioctl(&task, &file, SNDCTL_DSP_GETOPTR, (uintptr_t)&pointer));
        assert(pointer.bytes == 0 && pointer.blocks == 0 && pointer.ptr == 0);
    } else if (!strcmp(argv[1], "u8-sparse")) {
        int value = 0;
        assert(!audio_oss_ioctl(&task, &file, SNDCTL_DSP_GETCAPS, (uintptr_t)&value));
        assert(!(value & (DSP_CAP_DUPLEX | DSP_CAP_MMAP)));
        assert(!audio_oss_ioctl(&task, &file, SNDCTL_DSP_GETFMTS, (uintptr_t)&value));
        assert(value == AFMT_U8);
        assert(!audio_oss_ioctl(&task, &file, SNDCTL_DSP_GETTRIGGER, (uintptr_t)&value));
        assert(value == PCM_ENABLE_OUTPUT);
        assert(file.audio_oss_file->devices[AUDIO_PLAYBACK] == 2);
        assert(audio_oss_read(&task, &file, data, 4) == -EBADF);
        assert(audio_oss_write(&task, &file, data, 2) == 2);
        assert(card.prepared_params.frame_bytes == 2);
        assert(card.prepared_params.period_frames == 1024);
    } else if (!strcmp(argv[1], "odd-channels") || !strcmp(argv[1], "boundary")) {
        int value = 3;
        assert(!audio_oss_ioctl(&task, &file, SNDCTL_DSP_CHANNELS, (uintptr_t)&value));
        assert(value == 3);
        if (!strcmp(argv[1], "odd-channels")) {
            assert(audio_oss_write(&task, &file, data, 2046) == 2046);
            assert(card.prepared_params.period_frames == 341);
            assert(card.prepared_params.buffer_frames == 341 * 8);
            assert(!audio_oss_ioctl(&task, &file, SNDCTL_DSP_GETBLKSIZE, (uintptr_t)&value));
            assert(value == 2046);
        } else {
            /* Preserve the real non-power-of-two boundary and hardware snapshot. */
            file.audio_oss_file->fragment_bytes = 2046;
            assert(audio_oss_write(&task, &file, data, 2046) == 2046);
            struct audio_pcm *pcm = file.audio_oss_file->playback_pcm;
            struct audio_test_stream *stream = pcm->dma.kernel ?
                &card.streams[0] : NULL;
            assert(stream && pcm->boundary != (1ULL << 60));
            stream->frames = pcm->boundary - 341;
            pcm->appl_ptr = 40;
            audio_period_elapsed(card.id, stream->id, stream->frames, 0);
            assert(audio_oss_pcm_bytes(pcm, 0) == 381 * 6);
            file.audio_oss_file->last_output_hw_frames = pcm->boundary - 341;
            stream->frames = pcm->boundary + 1;
            audio_period_elapsed(card.id, stream->id, stream->frames, 0);
            count_info pointer;
            assert(!audio_oss_ioctl(&task, &file, SNDCTL_DSP_GETOPTR, (uintptr_t)&pointer));
            assert(pointer.blocks == 1 && pointer.ptr == 6);
        }
    } else if (!strcmp(argv[1], "xrun")) {
        assert(!audio_oss_pcm_prepare(file.audio_oss_file, AUDIO_CAPTURE));
        audio_test_advance(&card, 0, -EPIPE);
        assert(audio_oss_read(&task, &file, data, 4) == -EPIPE);
        assert(audio_oss_poll(&file, POLLIN | POLLOUT) == POLLERR);
        assert(!task.waiting_queue && task.state == TASK_RUNNING);
    } else if (!strcmp(argv[1], "lease")) {
        assert(audio_unregister_card(card.id, 0) == -EBUSY);
        assert(!audio_oss_ioctl(&task, &file, SNDCTL_DSP_SYNC, 0));
        struct task_file other = {.flags = RELIEFOS_O_WRONLY};
        assert(audio_oss_open(&task, &other) == -EBUSY && !other.audio_oss_file);
        assert(!audio_oss_ioctl(&task, &file, SNDCTL_DSP_RESET, 0));
        assert(audio_unregister_card(card.id, 0) == -EBUSY);
        assert(!(audio_oss_poll(&file, POLLIN) & POLLIN));
    } else if (!strcmp(argv[1], "negotiate")) {
        int value = 44100;
        assert(!audio_oss_ioctl(&task, &file, SNDCTL_DSP_SPEED, (uintptr_t)&value));
        assert(value == 48000);
        value = 1;
        assert(!audio_oss_ioctl(&task, &file, SNDCTL_DSP_CHANNELS, (uintptr_t)&value));
        assert(value == 2);
        value = AFMT_U8;
        assert(!audio_oss_ioctl(&task, &file, SNDCTL_DSP_SETFMT, (uintptr_t)&value));
        assert(value == AFMT_S16_LE);
        assert(audio_oss_write(&task, &file, data, 2048) == 2048);
        value = AFMT_QUERY;
        uint32_t prepare_count = card.prepare_count;
        assert(!audio_oss_ioctl(&task, &file, SNDCTL_DSP_SETFMT, (uintptr_t)&value));
        assert(card.prepare_count == prepare_count);
        assert(audio_oss_ioctl(&task, &file, SNDCTL_DSP_SETDUPLEX, 0) == -EIO);
    } else if (!strcmp(argv[1], "full-tail")) {
        assert(audio_oss_write(&task, &file, data, sizeof(data)) == sizeof(data));
        audio_test_advance(&card, 511, 0);
        assert(audio_oss_write(&task, &file, data, 2044) == 2044);
        assert(audio_oss_write(&task, &file, data, 3) == 3);
        assert(audio_oss_write(&task, &file, data, 1) == 1);
        assert(file.audio_oss_file->tail_bytes == 4);
        assert(audio_oss_ioctl(&task, &file, SNDCTL_DSP_SYNC, 0) == KERNEL_SYSCALL_BLOCKED);
        audio_test_advance(&card, 1, 0);
        task.state = TASK_RUNNING;
        assert(audio_oss_ioctl(&task, &file, SNDCTL_DSP_SYNC, 0) == KERNEL_SYSCALL_BLOCKED);
        assert(!file.audio_oss_file->tail_bytes);
        audio_test_advance(&card, 4096, 0);
        task.state = TASK_RUNNING;
        assert(!audio_oss_ioctl(&task, &file, SNDCTL_DSP_SYNC, 0));
    } else if (!strcmp(argv[1], "tail")) {
        assert(audio_oss_write(&task, &file, data, 3) == 3);
        assert(audio_oss_ioctl(&task, &file, SNDCTL_DSP_SYNC, 0) == KERNEL_SYSCALL_BLOCKED);
        assert(!file.audio_oss_file->tail_bytes);
        assert(card.prepared_params.period_frames == 512);
        assert(audio_oss_queue(file.audio_oss_file) == 2048);
        audio_test_advance(&card, 512, 0);
        task.state = TASK_RUNNING;
        assert(!audio_oss_ioctl(&task, &file, SNDCTL_DSP_SYNC, 0));
    } else {
        assert(audio_oss_write(&task, &file, data, sizeof(data)) == sizeof(data));
        assert(audio_oss_write(&task, &file, data, 4) == KERNEL_SYSCALL_BLOCKED);
        assert(task.waiting_queue && task.state == TASK_BLOCKED);
        audio_test_advance(&card, 512, 0);
        assert(!task.waiting_queue && task.state == TASK_READY);
        task.state = TASK_RUNNING;
        assert(audio_oss_write(&task, &file, data, 2052) == 2048);
        assert(!task.waiting_queue && task.state == TASK_RUNNING);
        task.pending_signals = 1ULL << 9;
        assert(audio_oss_write(&task, &file, data, 4) == KERNEL_SYSCALL_BLOCKED);
        assert(!task.waiting_queue && task.state == TASK_READY);
        task.pending_signals = 0; task.state = TASK_RUNNING;
        assert(audio_oss_write(&task, &file, data, 4) == KERNEL_SYSCALL_BLOCKED);
        assert(!audio_unregister_card(card.id, 1));
        assert(!task.waiting_queue && task.state == TASK_READY);
        assert(audio_oss_write(&task, &file, data, 4) == -ENODEV);
    }
    assert(!audio_oss_close(&file));
    if (strcmp(argv[1], "write")) assert(!audio_unregister_card(card.id, 0));
    tasks = NULL; task_count = 0; current_pid[0] = 0;
    printf("PASS OSS real wait: %s\n", argv[1]);
}
