#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <linux/poll.h>
#include <linux/soundcard.h>

#include "audio_fixture.h"

#include "../../kernel/reliefnt/kernel/reliefnt/audio/core.c"
#include "../../kernel/reliefnt/kernel/reliefnt/audio/alsa_control.c"
#include "../../kernel/reliefnt/kernel/reliefnt/audio/mixer.c"

bool user_range_ok(uint64_t ptr, uint64_t len)
{
    return ptr >= 4096u && len <= UINT64_MAX - ptr;
}

bool user_range_writable(uint64_t ptr, uint64_t len)
{
    return user_range_ok(ptr, len);
}

struct mixer_test_card {
    uint32_t id;
    int64_t values[4][2];
};

static struct mixer_test_card *mixer_card;

static int mixer_caps(void *opaque, uint32_t device, enum audio_direction direction,
                      struct audio_caps *caps)
{
    (void)opaque; (void)device; (void)direction;
    *caps = (struct audio_caps){
        .formats = AUDIO_FORMAT_S16_LE, .rates = AUDIO_RATE_48000,
        .channels_min = 2, .channels_max = 2, .period_bytes_min = 4,
        .period_bytes_max = 65536, .buffer_bytes_max = 65536,
    };
    return 0;
}

static int mixer_open(void *opaque, uint32_t device, enum audio_direction direction,
                      struct audio_hw_stream *stream)
{
    (void)opaque; (void)device; (void)direction; stream->id = 1; return 0;
}

static int mixer_prepare(void *opaque, struct audio_hw_stream *stream,
                         const struct audio_params *params, const struct audio_dma *dma)
{
    (void)opaque; (void)stream; (void)params; (void)dma; return 0;
}

static int mixer_trigger(void *opaque, struct audio_hw_stream *stream,
                         enum audio_trigger trigger)
{
    (void)opaque; (void)stream; (void)trigger; return 0;
}

static int mixer_pointer(void *opaque, struct audio_hw_stream *stream, uint64_t *frames)
{
    (void)opaque; (void)stream; *frames = 0; return 0;
}

static void mixer_close(void *opaque, struct audio_hw_stream *stream)
{
    (void)opaque; (void)stream;
}

static uint32_t mixer_count(void *opaque)
{
    (void)opaque; return 4;
}

static int mixer_control_info(void *opaque, uint32_t control, struct audio_control_info *info)
{
    (void)opaque;
    memset(info, 0, sizeof(*info));
    info->id = control + 1;
    info->access = AUDIO_CONTROL_READ | AUDIO_CONTROL_WRITE;
    if (control == 0) {
        info->type = AUDIO_CONTROL_INTEGER; info->count = 2;
        info->min = 0; info->max = 40; info->step = 1;
        strcpy(info->name, "Playback Volume");
    } else if (control == 1) {
        info->type = AUDIO_CONTROL_INTEGER; info->count = 2;
        info->min = 0; info->max = 100; info->step = 1;
        strcpy(info->name, "Capture Volume");
    } else if (control == 2) {
        info->type = AUDIO_CONTROL_BOOLEAN; info->count = 1;
        info->min = 0; info->max = 1; info->step = 1;
        strcpy(info->name, "Playback Mute");
    } else {
        info->type = AUDIO_CONTROL_ENUMERATED; info->count = 1; info->items = 2;
        strcpy(info->name, "Capture Source");
        strcpy(info->item_names[0], "Line");
        strcpy(info->item_names[1], "Mic");
    }
    return 0;
}

static int mixer_read(void *opaque, uint32_t control, struct audio_control_value *value)
{
    (void)opaque;
    memset(value, 0, sizeof(*value));
    value->values[0] = mixer_card->values[control][0];
    value->values[1] = mixer_card->values[control][1];
    return 0;
}

static int mixer_write(void *opaque, uint32_t control,
                       const struct audio_control_value *value)
{
    (void)opaque;
    mixer_card->values[control][0] = value->values[0];
    mixer_card->values[control][1] = value->values[1];
    return 0;
}

static void init_card(struct mixer_test_card *card)
{
    static struct audio_card_ops ops;
    mixer_card = card;
    memset(card, 0, sizeof(*card));
    ops = (struct audio_card_ops){
        .version = AUDIO_CARD_OPS_VERSION, .size = sizeof(ops),
        .pcm_caps = mixer_caps, .open = mixer_open, .prepare = mixer_prepare,
        .trigger = mixer_trigger, .pointer = mixer_pointer, .close = mixer_close,
        .control_count = mixer_count, .control_info = mixer_control_info,
        .control_read = mixer_read, .control_write = mixer_write,
    };
    struct audio_card_identity identity = {.id = "fake", .name = "mixer-test"};
    assert(!audio_register_card_owned(&identity, &ops, card, 0x92u, &card->id));
}

static void mixer_masks_and_shared_values(struct audio_mixer_file *mixer,
                                          struct audio_control_file *control)
{
    int value = 0;
    assert(!audio_mixer_ioctl_file(mixer, SOUND_MIXER_READ_DEVMASK, &value));
    assert((uint32_t)value == ((uint32_t)SOUND_MASK_VOLUME | (uint32_t)SOUND_MASK_PCM |
                     (uint32_t)SOUND_MASK_SPEAKER | ((uint32_t)1u << SOUND_MIXER_MUTE) |
                     (uint32_t)SOUND_MASK_MIC | (uint32_t)SOUND_MASK_LINE));
    assert(!audio_mixer_ioctl_file(mixer, SOUND_MIXER_READ_RECMASK, &value));
    assert(value == (SOUND_MASK_MIC | SOUND_MASK_LINE));
    assert(!audio_mixer_ioctl_file(mixer, SOUND_MIXER_READ_STEREODEVS, &value));
    assert(value == (SOUND_MASK_VOLUME | SOUND_MASK_PCM | SOUND_MASK_SPEAKER |
                     SOUND_MASK_MIC | SOUND_MASK_LINE));

    value = 50 | (25 << 8);
    assert(!audio_mixer_ioctl_file(mixer, SOUND_MIXER_WRITE_VOLUME, &value));
    assert(mixer_card->values[0][0] == 20 && mixer_card->values[0][1] == 10);
    struct snd_ctl_elem_value alsa = {.id = {.numid = 1}};
    assert(!audio_alsa_control_ioctl(control, SNDRV_CTL_IOCTL_ELEM_READ, &alsa));
    assert(alsa.value.integer.value[0] == 20 && alsa.value.integer.value[1] == 10);

    struct snd_ctl_elem_id lock_id = {.numid = 1};
    assert(!audio_alsa_control_ioctl(control, SNDRV_CTL_IOCTL_ELEM_LOCK, &lock_id));
    value = 60 | (60 << 8);
    assert(audio_mixer_ioctl_file(mixer, SOUND_MIXER_WRITE_VOLUME, &value) == -EPERM);
    assert(!audio_alsa_control_ioctl(control, SNDRV_CTL_IOCTL_ELEM_UNLOCK, &lock_id));

    int subscribe = 1;
    assert(!audio_alsa_control_ioctl(control, SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &subscribe));
    value = 70 | (70 << 8);
    assert(!audio_mixer_ioctl_file(mixer, SOUND_MIXER_WRITE_PCM, &value));
    assert(audio_control_poll_file(control, POLLIN) & POLLIN);
    struct snd_ctl_event event;
    assert(audio_control_read_file(control, &event, sizeof(event)) == (int)sizeof(event));
    assert(event.data.elem.id.numid == 1);

    value = 0;
    assert(!audio_mixer_ioctl_file(mixer, SOUND_MIXER_READ_VOLUME, &value));
    assert((value & 0xff) == 70 && ((value >> 8) & 0xff) == 70);
    value = 100;
    assert(!audio_mixer_ioctl_file(mixer, SOUND_MIXER_WRITE_MUTE, &value));
    value = 0;
    assert(!audio_mixer_ioctl_file(mixer, SOUND_MIXER_READ_MUTE, &value));
    assert((value & 0xff) == 100 && ((value >> 8) & 0xff) == 100);
}

int main(void)
{
    struct mixer_test_card card;
    init_card(&card);
    struct audio_mixer_file *mixer = NULL;
    struct audio_control_file *control = NULL;
    assert(!audio_mixer_open_card(card.id, &mixer));
    assert(!audio_control_open_card(card.id, &control));
    mixer_masks_and_shared_values(mixer, control);
    struct task_file task_file = {
        .node = {.flags = STORAGE_NODE_FLAG_AUDIO_MIXER},
        .audio_mixer_file = mixer,
    };
    int task_value = 0;
    assert(audio_mixer_ioctl(NULL, &task_file, SOUND_MIXER_READ_VOLUME, 1) == -EFAULT);
    assert(!audio_mixer_ioctl(NULL, &task_file, SOUND_MIXER_READ_VOLUME,
                              (uint64_t)(uintptr_t)&task_value));
    assert((task_value & 0xff) == 70 && ((task_value >> 8) & 0xff) == 70);
    int value = SOUND_MASK_MIC;
    assert(!audio_mixer_ioctl_file(mixer, SOUND_MIXER_WRITE_RECSRC, &value));
    assert(card.values[3][0] == 1);
    assert(audio_mixer_ioctl_file(mixer, SOUND_MIXER_WRITE_CD, &value) == -EOPNOTSUPP);
    assert(audio_mixer_ioctl_file(mixer, _IO('M', 127), &value) == -ENOTTY);
    assert(!audio_mixer_close_file(mixer));
    assert(!audio_control_close_file(control));
    assert(!audio_unregister_card(card.id, 0));
    puts("OSS mixer/shared ALSA control PASS");
    return 0;
}
