#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/poll.h>

#include <sound/asound.h>
#include <sound/tlv.h>
#include <reliefnt/sched.h>
#include <reliefnt/wait.h>

#define AUDIO_FIXTURE_REAL_WAIT
#define AUDIO_FIXTURE_NO_PTHREAD
#include "audio_fixture.h"

#define sched_block_current audio_test_real_sched_block_current
#define sched_signal_wait_current audio_test_real_sched_signal_wait_current
#include "../../kernel/reliefnt/kernel/reliefnt/sched/sched.c"
#include "sysv_shm_fixture_stubs.h"
#undef sched_block_current
#undef sched_signal_wait_current
#include "../../kernel/reliefnt/kernel/reliefnt/wait.c"
#include "../../kernel/reliefnt/kernel/reliefnt/audio/core.c"

static uint32_t injected_card;
static int inject_before_epoch, inject_before_sleep, inject_after_sleep;
static int inject_during_info;

/* Inject real core notifications around real scheduler state transitions. */
void sched_block_current(void)
{
    if (inject_before_sleep) {
        inject_before_sleep = 0;
        audio_control_changed(injected_card, 0);
    }
    audio_test_real_sched_block_current();
    if (inject_after_sleep) {
        inject_after_sleep = 0;
        audio_control_changed(injected_card, 0);
    }
}

void sched_signal_wait_current(uint64_t deadline)
{
    if (inject_before_sleep) {
        inject_before_sleep = 0;
        audio_control_changed(injected_card, 0);
    }
    audio_test_real_sched_signal_wait_current(deadline);
    if (inject_after_sleep) {
        inject_after_sleep = 0;
        audio_control_changed(injected_card, 0);
    }
}

static uint32_t control_test_epoch(void)
{
    if (inject_before_epoch) {
        inject_before_epoch = 0;
        audio_control_changed(injected_card, 0);
    }
    return audio_control_event_epoch();
}

#define audio_control_event_epoch control_test_epoch
#include "../../kernel/reliefnt/kernel/reliefnt/audio/alsa_control.c"
#undef audio_control_event_epoch

bool user_range_ok(uint64_t ptr, uint64_t len)
{
    /* Model the kernel's unmapped low page for nested usercopy checks. */
    return ptr >= 4096u && len <= UINT64_MAX - ptr;
}

bool user_range_writable(uint64_t ptr, uint64_t len)
{
    return user_range_ok(ptr, len);
}

struct audio_test_control_card {
    uint32_t id, controls;
    int64_t values[64][2];
};

static struct audio_test_control_card *control_card;

static int control_caps(void *opaque, uint32_t device, enum audio_direction direction,
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

static int control_open(void *opaque, uint32_t device, enum audio_direction direction,
                        struct audio_hw_stream *stream)
{
    (void)opaque; (void)device; (void)direction; stream->id = 1; return 0;
}

static int control_prepare(void *opaque, struct audio_hw_stream *stream,
                           const struct audio_params *params, const struct audio_dma *dma)
{
    (void)opaque; (void)stream; (void)params; (void)dma; return 0;
}

static int control_trigger(void *opaque, struct audio_hw_stream *stream,
                           enum audio_trigger trigger)
{
    (void)opaque; (void)stream; (void)trigger; return 0;
}

static int control_pointer(void *opaque, struct audio_hw_stream *stream, uint64_t *frames)
{
    (void)opaque; (void)stream; *frames = 0; return 0;
}

static void control_close(void *opaque, struct audio_hw_stream *stream)
{
    (void)opaque; (void)stream;
}

static uint32_t control_count(void *opaque)
{
    return ((struct audio_test_control_card *)opaque)->controls;
}

static int control_info(void *opaque, uint32_t control, struct audio_control_info *info)
{
    (void)opaque;
    if (inject_during_info && control == 0) {
        inject_during_info = 0;
        audio_control_changed_mask(control_card->id, 0, SNDRV_CTL_EVENT_MASK_VALUE);
    }
    memset(info, 0, sizeof(*info));
    info->id = control + 1;
    info->count = control == 2 ? 1 : 2;
    info->access = control == 2 ? AUDIO_CONTROL_READ : (AUDIO_CONTROL_READ | AUDIO_CONTROL_WRITE);
    if (control == 2) {
        info->type = AUDIO_CONTROL_ENUMERATED;
        info->items = 2;
        strcpy(info->name, "Route");
        strcpy(info->item_names[0], "Line");
        strcpy(info->item_names[1], "Headphones");
    } else {
        info->type = AUDIO_CONTROL_INTEGER;
        info->min = 0; info->max = 40; info->step = 1;
        info->db_min = -4000; info->db_step = 100;
        strcpy(info->name, control ? "Capture Volume" : "Playback Volume");
    }
    return 0;
}

static int control_read(void *opaque, uint32_t control, struct audio_control_value *value)
{
    (void)opaque; memset(value, 0, sizeof(*value));
    value->values[0] = control_card->values[control][0];
    value->values[1] = control_card->values[control][1];
    return 0;
}

static int control_write(void *opaque, uint32_t control,
                         const struct audio_control_value *value)
{
    (void)opaque;
    control_card->values[control][0] = value->values[0];
    control_card->values[control][1] = value->values[1];
    return 0;
}

static void init_card(struct audio_test_control_card *card, uint32_t controls)
{
    static struct audio_card_ops ops;
    control_card = card;
    memset(card, 0, sizeof(*card));
    card->controls = controls;
    ops = (struct audio_card_ops){
        .version = AUDIO_CARD_OPS_VERSION, .size = sizeof(ops),
        .pcm_caps = control_caps, .open = control_open, .prepare = control_prepare,
        .trigger = control_trigger, .pointer = control_pointer, .close = control_close,
        .control_count = control_count, .control_info = control_info,
        .control_read = control_read, .control_write = control_write,
    };
    struct audio_card_identity identity = {.id = "fake", .name = "control-test"};
    assert(!audio_register_card_owned(&identity, &ops, card, 0x91u, &card->id));
}

static void list_and_info(struct audio_control_file *file)
{
    struct snd_ctl_elem_id ids[2];
    struct snd_ctl_elem_list list = {.offset = 1, .space = 2, .pids = ids};
    assert(!audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_ELEM_LIST, &list));
    assert(list.count == 3 && list.used == 2 && ids[0].numid == 2);
    list.offset = 0; list.space = 1; list.pids = ids;
    assert(!audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_ELEM_LIST, &list));
    assert(list.count == 3 && list.used == 1 && ids[0].numid == 1);
    list.space = 1; list.pids = NULL;
    assert(audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_ELEM_LIST, &list) == -EFAULT);
    list.space = 0; list.used = 99;
    assert(!audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_ELEM_LIST, &list));
    assert(list.used == 0 && list.count == 3);
    struct snd_ctl_elem_info info = {0};
    info.id.numid = 1;
    assert(!audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_ELEM_INFO, &info));
    assert(info.type == SNDRV_CTL_ELEM_TYPE_INTEGER && info.count == 2);
    assert(info.value.integer.min == 0 && info.value.integer.max == 40);
    info.id.iface = SNDRV_CTL_ELEM_IFACE_PCM;
    assert(!audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_ELEM_INFO, &info));
    info.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    info.id.numid = 3; info.value.enumerated.item = 1;
    assert(!audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_ELEM_INFO, &info));
    assert(info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED &&
           !strcmp(info.value.enumerated.name, "Headphones"));
    info.value.enumerated.item = 2;
    assert(audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_ELEM_INFO, &info) == -EINVAL);
    info.id.numid = 99;
    assert(audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_ELEM_INFO, &info) == -ENOENT);
}

static void pcm_enumeration(struct audio_control_file *file)
{
    int device = -1;
    for (int i=0;i<8;++i) {
        assert(!audio_alsa_control_ioctl(file,SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE,&device));
        assert(device == i);
    }
    assert(!audio_alsa_control_ioctl(file,SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE,&device));
    assert(device == -1);
    /* Linux encodes NEXT_DEVICE as _IOR, but its integer is an input/output
     * cursor. Exercise the real usercopy dispatcher used by aplay/arecord. */
    struct task_file ofd = {.audio_control_file = file};
    device = -1;
    for (int i=0;i<8;++i) {
        assert(!audio_control_ioctl(NULL,&ofd,SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE,
                                   (uintptr_t)&device));
        assert(device == i);
    }
    assert(!audio_control_ioctl(NULL,&ofd,SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE,
                               (uintptr_t)&device));
    assert(device == -1);
    struct snd_pcm_info info = {.device=0,.stream=SNDRV_PCM_STREAM_CAPTURE};
    assert(!audio_alsa_control_ioctl(file,SNDRV_CTL_IOCTL_PCM_INFO,&info));
    assert(info.device == 0 && info.stream == SNDRV_PCM_STREAM_CAPTURE);
    assert(info.card == audio_card_index(file->card) && info.subdevices_count == 1);
    info.device=8;
    assert(audio_alsa_control_ioctl(file,SNDRV_CTL_IOCTL_PCM_INFO,&info) == -ENXIO);
    puts("ALSA control actual PCM enumeration/INFO PASS");
}

static void values_lock_events(uint32_t card_id, struct audio_control_file *file,
                               struct audio_control_file *other)
{
    struct snd_ctl_elem_value value = {0};
    value.id.numid = 1; value.value.integer.value[0] = 20; value.value.integer.value[1] = 20;
    assert(!audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_ELEM_WRITE, &value));
    value.value.integer.value[0] = 41;
    assert(audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) == -ERANGE);
    value.indirect = 1;
    assert(audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) == -EINVAL);
    value.indirect = 0;
    value.value.integer.value[0] = 20;
    struct snd_ctl_elem_value readback = {.id.numid = 1};
    assert(!audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_ELEM_READ, &readback));
    assert(readback.value.integer.value[0] == 20);

    struct snd_ctl_elem_id id = {.numid = 1};
    assert(!audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_ELEM_LOCK, &id));
    assert(audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_ELEM_LOCK, &id) == -EBUSY);
    assert(audio_alsa_control_ioctl(other, SNDRV_CTL_IOCTL_ELEM_LOCK, &id) == -EBUSY);
    value.value.integer.value[0] = 21; value.value.integer.value[1] = 21;
    assert(audio_alsa_control_ioctl(other, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) == -EPERM);
    assert(!audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_ELEM_UNLOCK, &id));
    assert(audio_alsa_control_ioctl(other, SNDRV_CTL_IOCTL_ELEM_UNLOCK, &id) == -EINVAL);

    struct snd_ctl_elem_value route = {.id.numid = 3};
    route.value.enumerated.item[0] = 1;
    assert(audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_ELEM_WRITE, &route) == -EACCES);
    route.value.enumerated.item[0] = 2;
    assert(audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_ELEM_WRITE, &route) == -EACCES);

    int subscribe = 1;
    assert(!audio_alsa_control_ioctl(other, SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &subscribe));
    value.value.integer.value[0] = 30; value.value.integer.value[1] = 30;
    assert(!audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_ELEM_WRITE, &value));
    audio_control_changed_mask(card_id, 0, SNDRV_CTL_EVENT_MASK_INFO);
    assert(audio_control_poll_file(other, POLLIN) & POLLIN);
    struct snd_ctl_event event;
    assert(audio_control_read_file(other, &event, sizeof(event)) == (int)sizeof(event));
    assert(event.type == SNDRV_CTL_EVENT_ELEM &&
           event.data.elem.mask == (SNDRV_CTL_EVENT_MASK_VALUE |
                                    SNDRV_CTL_EVENT_MASK_INFO) &&
           event.data.elem.id.numid == 1);
    assert(audio_control_read_file(other, &event, sizeof(event)) == -EAGAIN);
    audio_control_changed_mask(card_id, 0, SNDRV_CTL_EVENT_MASK_INFO);
    assert(audio_control_poll_file(other, POLLIN) & POLLIN);
    assert(audio_control_read_file(other, &event, sizeof(event)) == (int)sizeof(event));
    assert(event.type == SNDRV_CTL_EVENT_ELEM &&
           event.data.elem.mask == SNDRV_CTL_EVENT_MASK_INFO &&
           event.data.elem.id.numid == 1);
    audio_control_changed_mask(card_id, 0, SNDRV_CTL_EVENT_MASK_ADD);
    audio_control_changed_mask(card_id, 0, SNDRV_CTL_EVENT_MASK_REMOVE);
    assert(audio_control_poll_file(other, POLLIN) & POLLIN);
    assert(audio_control_read_file(other, &event, sizeof(event)) == (int)sizeof(event));
    assert(event.data.elem.mask == (SNDRV_CTL_EVENT_MASK_ADD |
                                    SNDRV_CTL_EVENT_MASK_REMOVE));
    subscribe = 0;
    assert(!audio_alsa_control_ioctl(other, SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &subscribe));
    assert(audio_control_poll_file(other, POLLIN) == 0);
}

/* Query/reenable must not consume events, and a disabled queue must be empty. */
static void subscription_and_order(uint32_t card)
{
    struct audio_control_file *file = NULL;
    assert(!audio_control_open_card(card, &file));
    int subscribed = -1;
    assert(!audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &subscribed));
    assert(subscribed == 0);
    subscribed = 7;
    assert(!audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &subscribed));
    subscribed = -99;
    assert(!audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &subscribed));
    assert(subscribed == 1);
    audio_control_changed_mask(card, 0, SNDRV_CTL_EVENT_MASK_VALUE);
    subscribed = 1;
    assert(!audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &subscribed));
    struct snd_ctl_event event;
    assert(audio_control_read_file(file, &event, sizeof(event)) == (int)sizeof(event));
    subscribed = 0;
    audio_control_changed_mask(card, 1, SNDRV_CTL_EVENT_MASK_INFO);
    assert(!audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &subscribed));
    assert(audio_control_read_file(file, &event, sizeof(event)) == -EBADFD);
    audio_control_changed(card, 0);
    subscribed = 1;
    assert(!audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &subscribed));
    assert(audio_control_read_file(file, &event, sizeof(event)) == -EAGAIN);
    audio_control_changed_mask(card, 2, SNDRV_CTL_EVENT_MASK_VALUE);
    audio_control_changed_mask(card, 0, SNDRV_CTL_EVENT_MASK_INFO);
    audio_control_changed_mask(card, 1, SNDRV_CTL_EVENT_MASK_ADD);
    audio_control_changed_mask(card, 2, SNDRV_CTL_EVENT_MASK_INFO);
    assert(audio_control_read_file(file, &event, sizeof(event) - 1) == -EINVAL);
    static const unsigned numids[] = {3, 1, 2};
    static const unsigned masks[] = {3, 2, 4};
    for (unsigned i = 0; i < 3; ++i) {
        assert(audio_control_read_file(file, &event, sizeof(event)) == (int)sizeof(event));
        assert(event.data.elem.id.numid == numids[i] && event.data.elem.mask == masks[i]);
    }
    assert(audio_control_poll_file(file, POLLIN) == 0);
    audio_control_changed(card, 0);
    assert(!audio_control_close_file(file));
    assert(!audio_control_open_card(card, &file));
    assert(audio_control_read_file(file, &event, sizeof(event)) == -EBADFD);
    assert(!audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &subscribed));
    assert(audio_control_read_file(file, &event, sizeof(event)) == -EAGAIN);
    assert(!audio_control_close_file(file));
    puts("ALSA control subscription/query/FIFO/reuse PASS");
}

/* Catch acknowledging a fresh notification while assembling the older ID. */
static void event_during_read(uint32_t card)
{
    struct audio_control_file *file = NULL;
    assert(!audio_control_open_card(card, &file));
    int subscribed = 1;
    assert(!audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &subscribed));
    audio_control_changed_mask(card, 0, SNDRV_CTL_EVENT_MASK_INFO);
    inject_during_info = 1;
    struct snd_ctl_event event;
    assert(audio_control_read_file(file, &event, sizeof(event)) == (int)sizeof(event));
    assert(event.data.elem.mask == SNDRV_CTL_EVENT_MASK_INFO);
    assert(audio_control_poll_file(file, POLLIN) & POLLIN);
    assert(audio_control_read_file(file, &event, sizeof(event)) == (int)sizeof(event));
    assert(event.data.elem.mask == SNDRV_CTL_EVENT_MASK_VALUE);
    assert(audio_control_read_file(file, &event, sizeof(event)) == -EAGAIN);
    assert(!audio_control_close_file(file));
    puts("ALSA control notification during event read PASS");
}

/* A slow subscriber must retain merged masks regardless of notification count. */
static void event_burst(uint32_t card)
{
    struct audio_control_file *slow = NULL, *fast = NULL;
    assert(!audio_control_open_card(card, &slow));
    assert(!audio_control_open_card(card, &fast));
    int subscribed = 1;
    assert(!audio_alsa_control_ioctl(slow, SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &subscribed));
    assert(!audio_alsa_control_ioctl(fast, SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &subscribed));
    struct snd_ctl_event event;
    for (unsigned i = 0; i < 1024; ++i) {
        uint32_t mask = i & 1 ? SNDRV_CTL_EVENT_MASK_INFO : SNDRV_CTL_EVENT_MASK_VALUE;
        for (unsigned control = 0; control < 3; ++control) {
            audio_control_changed_mask(card, control, mask);
            assert(audio_control_read_file(fast, &event, sizeof(event)) == (int)sizeof(event));
            assert(event.data.elem.id.numid == control + 1 && event.data.elem.mask == mask);
        }
    }
    for (unsigned control = 0; control < 3; ++control) {
        assert(audio_control_read_file(slow, &event, sizeof(event)) == (int)sizeof(event));
        assert(event.data.elem.id.numid == control + 1);
        assert(event.data.elem.mask == (SNDRV_CTL_EVENT_MASK_VALUE | SNDRV_CTL_EVENT_MASK_INFO));
    }
    assert(audio_control_read_file(slow, &event, sizeof(event)) == -EAGAIN);
    assert(audio_control_poll_file(slow, POLLIN) == 0);
    assert(!audio_control_close_file(slow));
    assert(!audio_control_close_file(fast));
    puts("ALSA control slow-subscriber burst merging PASS");
}

/* Cover every fixed element and release/reuse every registered subscriber. */
static void event_capacity(void)
{
    struct audio_test_control_card card;
    init_card(&card, 64);
    struct audio_control_file *readers[64], *extra = NULL;
    int subscribed = 1;
    for (unsigned i = 0; i < 64; ++i) {
        assert(!audio_control_open_card(card.id, &readers[i]));
        assert(!audio_alsa_control_ioctl(readers[i], SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &subscribed));
    }
    assert(audio_control_open_card(card.id, &extra) == -ENFILE);
    for (unsigned i = 64; i > 0; --i) {
        audio_control_changed_mask(card.id, i - 1, SNDRV_CTL_EVENT_MASK_VALUE);
        audio_control_changed_mask(card.id, i - 1, SNDRV_CTL_EVENT_MASK_INFO);
    }
    struct snd_ctl_event event;
    for (unsigned reader = 0; reader < 64; ++reader) {
        for (unsigned i = 64; i > 0; --i) {
            assert(audio_control_read_file(readers[reader], &event, sizeof(event)) == (int)sizeof(event));
            assert(event.data.elem.id.numid == i && event.data.elem.mask == 3);
        }
        assert(audio_control_read_file(readers[reader], &event, sizeof(event)) == -EAGAIN);
        assert(!audio_control_close_file(readers[reader]));
    }
    assert(!audio_control_open_card(card.id, &extra));
    assert(!audio_alsa_control_ioctl(extra, SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &subscribed));
    assert(audio_control_read_file(extra, &event, sizeof(event)) == -EAGAIN);
    assert(!audio_control_close_file(extra));
    assert(!audio_unregister_card(card.id, 0));
    puts("ALSA control full element/subscriber capacity and reuse PASS");
}

static void tlv_and_percent(struct audio_control_file *file)
{
    struct {
        struct snd_ctl_tlv header;
        uint32_t payload[4];
    } too_small = {.header = {.numid = 1, .length = 4}};
    assert(audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_TLV_READ,
                                    &too_small.header) == -EINVAL);
    assert(too_small.header.length == 4);
    struct {
        struct snd_ctl_tlv header;
        uint32_t payload[4];
    } tlv = {.header = {.numid = 1, .length = 16}};
    assert(!audio_alsa_control_ioctl(file, SNDRV_CTL_IOCTL_TLV_READ, &tlv.header));
    assert(tlv.payload[0] == SNDRV_CTL_TLVT_DB_SCALE &&
           tlv.payload[1] == 8 && (int32_t)tlv.payload[2] == -4000);
    assert(tlv.header.length == 16);
    struct task_file ofd = {.audio_control_file = file};
    memset(tlv.payload,0xa5,sizeof(tlv.payload));
    assert(!audio_control_ioctl(NULL,&ofd,SNDRV_CTL_IOCTL_TLV_READ,(uintptr_t)&tlv));
    assert(tlv.payload[0] == SNDRV_CTL_TLVT_DB_SCALE && tlv.payload[1] == 8);
    assert((int32_t)tlv.payload[2] == -4000 && tlv.header.length == 16);
    tlv.header.length = 8;
    memset(tlv.payload,0xa5,sizeof(tlv.payload));
    assert(audio_control_ioctl(NULL,&ofd,SNDRV_CTL_IOCTL_TLV_READ,(uintptr_t)&tlv) == -ENOMEM);
    assert(tlv.payload[0] == 0xa5a5a5a5u && tlv.header.length == 8);
    tlv.header.numid = 0;
    assert(audio_control_ioctl(NULL,&ofd,SNDRV_CTL_IOCTL_TLV_READ,(uintptr_t)&tlv) == -EINVAL);
    assert(((unsigned)(50u * 40u + 50u) / 100u) == 20u);
    puts("ALSA TLV checked flexible payload, capacity and numid PASS");
}

static void nested_usercopy(struct audio_control_file *file)
{
    struct task_file task_file = {
        .node = {.flags = STORAGE_NODE_FLAG_AUDIO_CONTROL},
        .audio_control_file = file,
    };
    assert(audio_control_ioctl(NULL, &task_file, _IOWR('U', 0x7f, int), 1) == -ENOTTY);
    struct snd_ctl_elem_id ids[2];
    struct snd_ctl_elem_list list = {.space = 2, .pids = ids};
    assert(!audio_control_ioctl(NULL, &task_file, SNDRV_CTL_IOCTL_ELEM_LIST,
                                (uint64_t)(uintptr_t)&list));
    assert(list.used == 2 && ids[0].numid == 1);
    list.pids = (struct snd_ctl_elem_id *)(uintptr_t)1;
    assert(audio_control_ioctl(NULL, &task_file, SNDRV_CTL_IOCTL_ELEM_LIST,
                               (uint64_t)(uintptr_t)&list) == -EFAULT);
}

static void public_value_usercopy(struct audio_control_file *file)
{
    struct task_file ofd = {.audio_control_file = file};
    struct {
        uint64_t before;
        struct snd_ctl_elem_value value;
        uint64_t after;
    } storage = {.before = 0x123456789abcdef0ULL, .after = 0xfedcba9876543210ULL,
                 .value.id.numid = 1};
    assert(sizeof(storage.value) == 1224);
    int ret = audio_control_ioctl(NULL, &ofd, SNDRV_CTL_IOCTL_ELEM_READ,
                                  (uintptr_t)&storage.value);
    if (ret) fprintf(stderr, "public ELEM_READ ret=%d size=%u\n", ret,
                     _IOC_SIZE(SNDRV_CTL_IOCTL_ELEM_READ));
    assert(!ret);
    struct snd_ctl_elem_value original = storage.value;
    storage.value.value.integer.value[0] = 30;
    storage.value.value.integer.value[1] = 40;
    assert(!audio_control_ioctl(NULL, &ofd, SNDRV_CTL_IOCTL_ELEM_WRITE,
                                (uintptr_t)&storage.value));
    storage.value.value.integer.value[0] = storage.value.value.integer.value[1] = -1;
    assert(!audio_control_ioctl(NULL, &ofd, SNDRV_CTL_IOCTL_ELEM_READ,
                                (uintptr_t)&storage.value));
    assert(storage.value.value.integer.value[0] == 30 &&
           storage.value.value.integer.value[1] == 40);
    assert(storage.before == 0x123456789abcdef0ULL && storage.after == 0xfedcba9876543210ULL);
    assert(audio_control_ioctl(NULL, &ofd, SNDRV_CTL_IOCTL_ELEM_READ, 1) == -EFAULT);
    assert(audio_control_ioctl(NULL, &ofd, SNDRV_CTL_IOCTL_ELEM_WRITE, 1) == -EFAULT);
    assert(!audio_control_ioctl(NULL, &ofd, SNDRV_CTL_IOCTL_ELEM_WRITE, (uintptr_t)&original));
    puts("ALSA control public 1224-byte value usercopy PASS");
}

static void owner_pid_via_task(uint32_t card_id)
{
    struct task task = {0};
    task.pid = 4242;
    struct task_file file = {0};
    file.node.flags = STORAGE_NODE_FLAG_AUDIO_CONTROL;
    file.node.volume_id = card_id;
    assert(!audio_control_open(&task, &file));
    struct snd_ctl_elem_id id = {.numid = 1};
    task.pid = 5151; /* inherited OFD retains the opener identity */
    assert(!audio_control_ioctl(&task, &file, SNDRV_CTL_IOCTL_ELEM_LOCK,
                                (uint64_t)(uintptr_t)&id));
    struct snd_ctl_elem_info info = {0};
    info.id.numid = 1;
    assert(!audio_control_ioctl(&task, &file, SNDRV_CTL_IOCTL_ELEM_INFO,
                                (uint64_t)(uintptr_t)&info));
    assert(info.owner == 4242);
    assert(!audio_control_close(&file));
}

static void blocking_events(uint32_t card_id)
{
    struct task first = {.pid = 4243, .state = TASK_RUNNING, .running_cpu = 0};
    struct task second = {.pid = 4244, .state = TASK_RUNNING, .running_cpu = 0};
    struct task *test_tasks[] = {&first, &second};
    tasks = test_tasks;
    task_count = 2;
    current_pid[0] = first.pid;
    injected_card = card_id;
    struct task_file file = {.node.flags = STORAGE_NODE_FLAG_AUDIO_CONTROL,
                              .node.volume_id = card_id};
    struct task_file other = file;
    assert(!audio_control_open(&first, &file));
    assert(!audio_control_open(&second, &other));
    struct snd_ctl_event event;
    assert(audio_control_read_file(file.audio_control_file, &event, sizeof(event)) ==
           -EBADFD);
    assert(audio_control_read_task(&first, &file, &event, sizeof(event)) == -EBADFD);
    assert(first.state == TASK_RUNNING && !first.waiting_queue);
    int subscribe = 1;
    assert(!audio_alsa_control_ioctl(file.audio_control_file,
                                    SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &subscribe));
    assert(!audio_alsa_control_ioctl(other.audio_control_file,
                                    SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &subscribe));
    /* A notification between an empty probe and epoch capture must not vanish. */
    inject_before_epoch = 1;
    assert(audio_control_read_task(&first, &file, &event, sizeof(event)) ==
           (int)sizeof(event));
    assert(first.state != TASK_BLOCKED && !first.waiting_queue);
    assert(audio_control_read_file(other.audio_control_file, &event, sizeof(event)) ==
           (int)sizeof(event));

    /* A wake before sleep publication must leave a runnable task with the event. */
    first.state = TASK_RUNNING;
    inject_before_sleep = 1;
    assert(audio_control_read_task(&first, &file, &event, sizeof(event)) ==
           (int)sizeof(event));
    assert(first.state == TASK_READY && !first.waiting_queue);
    assert(event.data.elem.id.numid == 1 && event.data.elem.mask == 1);
    assert(audio_control_read_file(other.audio_control_file, &event, sizeof(event)) ==
           (int)sizeof(event));

    first.state = TASK_RUNNING;
    inject_after_sleep = 1;
    assert(audio_control_read_task(&first, &file, &event, sizeof(event)) ==
           (int)sizeof(event));
    assert(first.state == TASK_READY && !first.waiting_queue);
    assert(audio_control_read_file(other.audio_control_file, &event, sizeof(event)) ==
           (int)sizeof(event));

    first.state = TASK_RUNNING;
    assert(audio_control_read_task(&first, &file, &event, sizeof(event)) ==
           KERNEL_SYSCALL_BLOCKED);
    assert(first.state == TASK_BLOCKED && first.waiting_queue);
    current_pid[0] = second.pid;
    assert(audio_control_read_task(&second, &other, &event, sizeof(event)) ==
           KERNEL_SYSCALL_BLOCKED);
    assert(second.state == TASK_BLOCKED && second.waiting_queue);
    audio_control_changed(card_id, 0);
    assert(first.state == TASK_READY && second.state == TASK_READY);
    assert(!first.waiting_queue && !second.waiting_queue);
    assert(audio_control_read_task(&second, &other, &event, sizeof(event)) ==
           (int)sizeof(event));
    current_pid[0] = first.pid;
    assert(audio_control_read_task(&first, &file, &event, sizeof(event)) ==
           (int)sizeof(event));

    /* A pending deliverable signal must not leave an uninterruptible sleeper. */
    first.state = TASK_RUNNING;
    first.pending_signals = 1ULL << (LINUX_SIGBUS - 1);
    assert(audio_control_read_task(&first, &file, &event, sizeof(event)) ==
           KERNEL_SYSCALL_BLOCKED);
    assert(first.state == TASK_READY && !first.waiting_queue);
    first.pending_signals = 0;

    /* A full bounded queue falls back to retry without parking an unregistered task. */
    struct task *fillers = calloc(KERNEL_WAIT_QUEUE_MAX, sizeof(*fillers));
    assert(fillers);
    for (uint32_t i = 0; i < KERNEL_WAIT_QUEUE_MAX; ++i) {
        fillers[i].pid = 5000 + i;
        assert(!kernel_wait_queue_add(&audio_control_waiters, &fillers[i]));
    }
    first.state = TASK_RUNNING;
    assert(audio_control_read_task(&first, &file, &event, sizeof(event)) == -EAGAIN);
    assert(first.state == TASK_RUNNING && !first.waiting_queue);
    for (uint32_t i = 0; i < KERNEL_WAIT_QUEUE_MAX; ++i)
        kernel_wait_queue_remove(&audio_control_waiters, &fillers[i]);
    free(fillers);

    first.state = TASK_RUNNING;
    assert(audio_control_read_task(&first, &file, &event, sizeof(event)) ==
           KERNEL_SYSCALL_BLOCKED);
    assert(!audio_unregister_card(card_id, 0));
    assert(first.state == TASK_READY && !first.waiting_queue);
    assert(audio_control_read_task(&first, &file, &event, sizeof(event)) == -ENODEV);
    assert(!audio_control_close(&file));
    assert(!audio_control_close(&other));
    tasks = NULL;
    task_count = 0;
    current_pid[0] = 0;
    puts("ALSA control real wait/scheduler notification races PASS");
}

int main(void)
{
    struct audio_test_control_card card;
    init_card(&card, 3);
    struct audio_control_file *file = NULL, *other = NULL;
    assert(!audio_control_open_card(card.id, &file));
    assert(!audio_control_open_card(card.id, &other));
    list_and_info(file);
    pcm_enumeration(file);
    values_lock_events(card.id, file, other);
    subscription_and_order(card.id);
    if (getenv("AUDIO_CONTROL_BURST_FIRST")) event_burst(card.id);
    event_during_read(card.id);
    if (!getenv("AUDIO_CONTROL_BURST_FIRST")) event_burst(card.id);
    tlv_and_percent(file);
    nested_usercopy(file);
    public_value_usercopy(file);
    owner_pid_via_task(card.id);
    assert(!audio_control_close_file(file));
    assert(!audio_control_close_file(other));
    blocking_events(card.id);
    event_capacity();
    puts("ALSA control/event/TLV PASS");
    return 0;
}
