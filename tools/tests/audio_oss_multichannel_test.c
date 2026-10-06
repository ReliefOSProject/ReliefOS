/* Reuse the real OSS/core/PCM fixture; replace only hardware capabilities. */
#define main oss_stereo_fixture_main
#include "audio_oss_test.c"
#undef main

static uint32_t hardware_channels;

static int multichannel_caps(void *opaque, uint32_t device,
                             enum audio_direction direction,
                             struct audio_caps *caps)
{
    int ret = audio_test_caps(opaque, device, direction, caps);
    caps->channels_min = caps->channels_max = hardware_channels;
    return ret;
}

static void check_multichannel(uint32_t channels)
{
    struct audio_test_card card = {0};
    struct audio_card_ops ops = audio_test_ops;
    ops.pcm_caps = multichannel_caps;
    hardware_channels = channels;
    struct audio_card_identity identity = {.id = "multi", .name = "HDA-multi"};
    assert(!audio_register_card_owned(&identity, &ops, &card, 0x7au, &card.id));
    for (unsigned run = 0; run < 2; ++run) {
        struct task_file file = {.flags = RELIEFOS_O_NONBLOCK | RELIEFOS_O_WRONLY};
        int ret = audio_oss_open(NULL, &file);
        printf("OSS %u-channel open: %d\n", channels, ret);
        fflush(stdout);
        assert(!ret);
        int negotiated = 2;
        assert(!audio_oss_ioctl(NULL, &file, SNDCTL_DSP_CHANNELS, (uintptr_t)&negotiated));
        assert(negotiated == (int)channels);
        uint8_t frame[32];
        for (unsigned i = 0; i < sizeof frame; ++i) frame[i] = (uint8_t)(i + 1);
        /* Exercise the partial-frame copy as well as the aligned batch path. */
        assert(audio_oss_write(NULL, &file, frame, 1) == 1);
        assert(audio_oss_write(NULL, &file, frame + 1, channels * 2 - 1) ==
               (int)(channels * 2 - 1));
        assert(card.prepared_params.channels == channels);
        assert(!memcmp(card.prepared_dma.kernel, frame, channels * 2));
        assert(audio_oss_write(NULL, &file, frame, channels * 2) == (int)(channels * 2));
        assert(!memcmp((uint8_t *)card.prepared_dma.kernel + channels * 2,
                       frame, channels * 2));
        assert(!audio_oss_close(&file));
        assert(!file.audio_oss_file);
    }
    assert(card.open_count == 2 && card.close_count == 2);
    struct task_file input = {.flags = RELIEFOS_O_NONBLOCK | RELIEFOS_O_RDONLY};
    assert(!audio_oss_open(NULL, &input));
    uint8_t captured[32] = {0}, expected[32];
    assert(audio_oss_read(NULL, &input, captured, 1) == -EAGAIN);
    for (unsigned i = 0; i < sizeof expected; ++i) expected[i] = (uint8_t)(i + 3);
    memcpy(card.prepared_dma.kernel, expected, channels * 2);
    audio_test_advance(&card, 1, 0);
    assert(audio_oss_read(NULL, &input, captured, 1) == 1);
    assert(audio_oss_read(NULL, &input, captured + 1, channels * 2 - 1) ==
           (int)(channels * 2 - 1));
    assert(!memcmp(captured, expected, channels * 2));
    assert(!audio_oss_close(&input));
    assert(!audio_unregister_card(card.id, 0));
}

int main(void)
{
    assert(!oss_stereo_fixture_main());
    check_multichannel(8);
    check_multichannel(16);
    struct audio_test_card card = {0};
    struct audio_card_ops ops = audio_test_ops;
    ops.pcm_caps = multichannel_caps;
    hardware_channels = 17;
    struct audio_card_identity identity = {.id = "limit", .name = "oversized"};
    assert(!audio_register_card_owned(&identity, &ops, &card, 0x7au, &card.id));
    struct task_file file = {.flags = RELIEFOS_O_NONBLOCK | RELIEFOS_O_WRONLY};
    assert(audio_oss_open(NULL, &file) == -EINVAL);
    assert(!file.audio_oss_file && card.open_count == card.close_count);
    assert(!audio_unregister_card(card.id, 0));
    puts("OSS multichannel open/negotiation/partial write/close-reopen PASS");
    return 0;
}
