#define HDA_TESTING 1
#define HDA_STREAM_TESTING 1
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <reliefos/driver.h>
#include "audio_fixture.h"

#define FAKE_REG_BYTES 0x4000u
#define FAKE_DMA_MAX 32u
struct fake_dma { uint64_t phys; uint8_t *memory; uint32_t pages; uint8_t live; };
static struct {
    uint8_t regs[FAKE_REG_BYTES];
    struct fake_dma dma[FAKE_DMA_MAX];
    uint32_t dma_count;
    uint64_t next_phys;
    uint32_t stop_stuck;
    uint32_t position_stuck;
    uint32_t mmio_write_count;
} fake;
static uint64_t fake_tick_count;
static uint64_t fake_ticks(void) { return fake_tick_count; }
static int route_bind_result;
static unsigned route_bind_calls;

static uint8_t hda_test_mmio_read8(void *base, uint32_t offset)
{ return ((uint8_t *)base)[offset]; }
static uint16_t hda_test_mmio_read16(void *base, uint32_t offset)
{ uint16_t value; memcpy(&value, (uint8_t *)base + offset, sizeof(value)); return value; }
static uint32_t hda_test_mmio_read32(void *base, uint32_t offset)
{ uint32_t value; memcpy(&value, (uint8_t *)base + offset, sizeof(value)); return value; }
static void hda_test_mmio_write8(void *base, uint32_t offset, uint8_t value)
{
    ++fake.mmio_write_count;
    uint8_t *reg = (uint8_t *)base + offset;
    if (offset >= 0x80u && (offset & 0x1fu) == 3u) *reg &= (uint8_t)~value;
    else *reg = value;
}
static void hda_test_mmio_write16(void *base, uint32_t offset, uint16_t value)
{ ++fake.mmio_write_count; memcpy((uint8_t *)base + offset, &value, sizeof(value)); }
static void hda_test_mmio_write32(void *base, uint32_t offset, uint32_t value)
{
    ++fake.mmio_write_count;
    if (fake.position_stuck && offset == 0x70u && !(value & 1u)) return;
    if (fake.stop_stuck && offset == 0x80u) {
        uint32_t old = hda_test_mmio_read32(base, offset);
        if ((old & (1u << 1)) && !(value & (1u << 1))) value |= (1u << 1);
    }
    memcpy((uint8_t *)base + offset, &value, sizeof(value));
}
static void *hda_test_dma_map(uint64_t phys)
{
    for (uint32_t i = 0; i < fake.dma_count; ++i)
        if (fake.dma[i].live && fake.dma[i].phys == phys) return fake.dma[i].memory;
    return NULL;
}
static uint64_t hda_test_irq_save(void) { return 0; }
static void hda_test_irq_restore(uint64_t flags) { (void)flags; }
struct hda_controller;
void hda_test_before_sleep(const struct hda_controller *c) { (void)c; }

#include "../../kernel/reliefnt/kernel/reliefnt/audio/core.c"
#include "../../kernel/reliefnt/drivers/hda/controller.c"
#include "../../kernel/reliefnt/drivers/hda/stream.c"

/* The stream fixture links the PCM core without the ALSA timer module. */
void audio_timer_period_elapsed(uint32_t card, uint32_t device,
                                enum audio_direction direction,
                                uint64_t frames, int error)
{
    (void)card; (void)device; (void)direction; (void)frames; (void)error;
}

/* stream.c reports periods through the hda.c wrapper; this fixture links the
 * PCM core directly, so forward to it the way the pre-rename call resolved. */
void hda_audio_period_elapsed(uint32_t card, uint32_t stream,
                              uint64_t frames, int error)
{
    audio_period_elapsed(card, stream, frames, error);
}

int hda_stream_test_route_bind(struct hda_route_group *group, uint8_t tag,
                               uint16_t format)
{
    (void)group;
    (void)tag;
    (void)format;
    ++route_bind_calls;
    return route_bind_result;
}

static uint64_t fake_alloc_dma(uint32_t pages, uint64_t mask)
{
    if (!pages || fake.dma_count == FAKE_DMA_MAX) return 0;
    uint64_t phys = (fake.next_phys + 127u) & ~127ull;
    uint64_t end = phys + (uint64_t)pages * 4096u - 1u;
    if (end > mask) return 0;
    struct fake_dma *d = &fake.dma[fake.dma_count++];
    d->memory = calloc(pages, 4096u);
    if (!d->memory) return 0;
    d->phys = phys; d->pages = pages; d->live = 1u; fake.next_phys = end + 1u;
    return phys;
}
static void fake_free_dma(uint64_t phys, uint32_t pages)
{
    for (uint32_t i = 0; i < fake.dma_count; ++i)
        if (fake.dma[i].live && fake.dma[i].phys == phys) {
            assert(fake.dma[i].pages == pages);
            free(fake.dma[i].memory); fake.dma[i].live = 0u; return;
        }
    assert(0 && "free of unknown DMA run");
}
static int caps(void *opaque, uint32_t device, enum audio_direction direction,
                struct audio_caps *out)
{ (void)opaque; (void)device; (void)direction; memset(out, 0, sizeof(*out)); return 0; }
static int open_cb(void *opaque, uint32_t device, enum audio_direction direction,
                   struct audio_hw_stream *stream)
{ return hda_pcm_open(opaque, device, direction, stream); }
static int prepare_cb(void *opaque, struct audio_hw_stream *stream,
                      const struct audio_params *params, const struct audio_dma *dma)
{ return hda_pcm_prepare(opaque, stream, params, dma); }
static int trigger_cb(void *opaque, struct audio_hw_stream *stream, enum audio_trigger trigger)
{ return hda_pcm_trigger(opaque, stream, trigger); }
static int pointer_cb(void *opaque, struct audio_hw_stream *stream, uint64_t *frames)
{ return hda_pcm_pointer(opaque, stream, frames); }
static void close_cb(void *opaque, struct audio_hw_stream *stream)
{ hda_pcm_close(opaque, stream); }
static uint32_t control_count_cb(void *opaque) { (void)opaque; return 0; }
static struct audio_card_ops card_ops = {
    .version = AUDIO_CARD_OPS_VERSION, .size = sizeof(card_ops), .pcm_caps = caps,
    .open = open_cb, .prepare = prepare_cb, .trigger = trigger_cb,
    .pointer = pointer_cb, .close = close_cb, .control_count = control_count_cb,
    .pcm_format_caps = hda_pcm_format_caps,
    .prepare_format = hda_pcm_prepare_format,
};

static void fake_stream_irq(struct hda_stream *stream, uint32_t position,
                            uint8_t status)
{
    uint32_t base = 0x80u + stream->index * 0x20u;
    *stream->position = position;
    fake.regs[0x24u] |= (uint8_t)(1u << stream->index);
    fake.regs[base + 3u] |= status;
}

int main(void)
{
    uint16_t encoded;
    assert(!hda_format_encode(48000u, 16u, 2u, &encoded) && encoded == 0x11u);
    assert(!hda_format_encode(44100u, 16u, 2u, &encoded) && encoded == 0x4011u);
    assert(hda_format_encode(48000u, 16u, 0u, &encoded) == -EINVAL);
    memset(&fake, 0, sizeof(fake)); fake.next_phys = 0x200000u;
    struct reliefos_driver_kernel_api api = {.alloc_dma = fake_alloc_dma, .free_dma = fake_free_dma};
    struct hda_controller controller = {.api = &api, .mmio = fake.regs,
                                        .gcap = 0u, .initialized = 1u};
    struct hda_stream streams[1];
    assert(!hda_stream_controller_init(&controller, streams, 1u));
    assert((hda_test_mmio_read32(fake.regs, 0x70u) & 1u) != 0u);
    assert((hda_test_mmio_read32(fake.regs, 0x70u) & ~127u) ==
           (uint32_t)(controller.position_phys & ~127ull));
    assert(streams[0].bdl_dma.bus < 0x100000000ull);
    struct audio_card_identity identity = {.id = "hda", .name = "hda-test"};
    uint32_t card;
    assert(!audio_register_card_owned(&identity, &card_ops, &controller, 1u, &card));
    controller.card_id = card;
    struct audio_format_caps format_caps;
    assert(!audio_stream_format_caps(card, 0u, AUDIO_PLAYBACK, &format_caps));
    assert(format_caps.format_step_bytes == 128u && format_caps.period_count_min == 2u);
    assert(format_caps.format_bits[1] == AUDIO_FORMAT_S32_LE);
    assert(format_caps.subformats[1] & (1u << AUDIO_SUBFORMAT_MSBITS_24));
    uint32_t stream_id;
    assert(!audio_stream_open(card, 0u, AUDIO_PLAYBACK, &stream_id));
    struct hda_route route = {.channel_start = 0u, .channel_count = 2u};
    struct hda_route_group group = {.members = &route, .member_count = 1u,
                                    .association = 15u, .total_channels = 2u, .active = 1u};
    assert(!hda_stream_bind_group(&streams[0], &group));
    struct audio_dma pcm = {.kernel = calloc(1u, 4096u), .bus = 0x400000u, .bytes = 4096u};
    struct audio_params_ext params = {
        .pcm = {.rate = 48000u, .channels = 2u, .sample_bits = 16u,
                .frame_bytes = 4u, .period_frames = 32u, .buffer_frames = 128u},
        .format = AUDIO_FORMAT_S16_LE, .subformat = AUDIO_SUBFORMAT_STD,
        .significant_bits = 16u,
    };
    assert(!audio_stream_prepare_format(stream_id, &params, &pcm));
    struct audio_params_ext bad_params = params;
    bad_params.subformat = AUDIO_SUBFORMAT_MSBITS_24;
    bad_params.significant_bits = 24u;
    assert(audio_stream_prepare_format(stream_id, &bad_params, &pcm) == -EINVAL);
    struct hda_stream *s = &streams[0];
    assert(route.stream_bound && route.stream_tag == s->tag && route.stream_format == 0x11u);
    assert(s->bdl[0].address == pcm.bus && s->bdl[1].address == pcm.bus + 128u);
    assert(s->bdl[2].address == pcm.bus + 256u && s->bdl[3].address == pcm.bus + 384u);
    for (uint32_t i = 0; i < 4u; ++i)
        assert(s->bdl[i].length == 128u && s->bdl[i].flags == HDA_BDL_IOC);
    assert(!(hda_mmio_read32(&controller, 0x80u) & (1u << 1)));
    uint32_t *samples = pcm.kernel;
    samples[0] = 0x11112222u; samples[1] = 0x33334444u;
    assert(samples[0] == 0x11112222u && samples[1] == 0x33334444u);
    assert(!audio_stream_trigger(stream_id, AUDIO_START));
    assert(hda_mmio_read32(&controller, 0x80u) & (1u << 1));
    fake_stream_irq(s, 128u, HDA_SDSTS_BCIS);
    assert(hda_stream_service_locked(&controller, 1u) == 1u);
    assert(fake.regs[0x83u] == 0u);
    uint64_t completed_frames; int completion_error;
    assert(audio_stream_period_poll(stream_id, &completed_frames, &completion_error) == 1);
    assert(completed_frames == 32u && completion_error == 0);
    fake_stream_irq(s, 256u, HDA_SDSTS_BCIS);
    assert(hda_stream_service_locked(&controller, 1u) == 1u);
    assert(fake.regs[0x83u] == 0u);
    assert(audio_stream_period_poll(stream_id, &completed_frames, &completion_error) == 1);
    assert(completed_frames == 64u && completion_error == 0);
    fake_stream_irq(s, 384u, HDA_SDSTS_BCIS);
    assert(hda_stream_service_locked(&controller, 1u) == 1u);
    assert(fake.regs[0x83u] == 0u);
    fake_stream_irq(s, 0u, HDA_SDSTS_BCIS);
    assert(hda_stream_service_locked(&controller, 1u) == 1u);
    assert(fake.regs[0x83u] == 0u);
    fake_stream_irq(s, 128u, HDA_SDSTS_BCIS);
    assert(hda_stream_service_locked(&controller, 1u) == 1u);
    assert(fake.regs[0x83u] == 0u);
    assert(audio_card_periods(card) == 5u);
    assert(!audio_stream_trigger(stream_id, AUDIO_PAUSE));
    uint64_t frame_before; assert(!audio_stream_pointer(stream_id, &frame_before));
    assert(frame_before == 160u);
    assert(!audio_stream_trigger(stream_id, AUDIO_UNPAUSE));
    fake.stop_stuck = 1u; assert(audio_stream_trigger(stream_id, AUDIO_STOP) == -EIO);
    fake.stop_stuck = 0u; assert(!audio_stream_trigger(stream_id, AUDIO_STOP));
    assert(!(hda_mmio_read32(&controller, 0x20u) & 1u));
    uint64_t periods_after_stop = audio_card_periods(card);
    fake_stream_irq(s, 128u, HDA_SDSTS_BCIS);
    assert(hda_stream_service_locked(&controller, 1u) == 0u);
    assert(audio_card_periods(card) == periods_after_stop);
    assert(!audio_stream_close(stream_id));
    assert(!route.stream_bound && route.stream_tag == 0u && route.stream_format == 0u);

    /* Route capability failure must happen before BDL publication or SD MMIO. */
    uint32_t preflight_id;
    assert(!audio_stream_open(card, 0u, AUDIO_PLAYBACK, &preflight_id));
    assert(!hda_stream_bind_group(&streams[0], &group));
    struct hda_bdl_entry bdl_before = streams[0].bdl[0];
    uint8_t sd_before[0x20u];
    memcpy(sd_before, fake.regs + 0x80u, sizeof(sd_before));
    unsigned bind_calls_before = route_bind_calls;
    uint32_t mmio_writes_before = fake.mmio_write_count;
    struct audio_dma preflight_pcm = pcm;
    preflight_pcm.bus = 0x500000u;
    route_bind_result = -EOPNOTSUPP;
    assert(audio_stream_prepare_format(preflight_id, &params, &preflight_pcm) == -EOPNOTSUPP);
    assert(route_bind_calls == bind_calls_before + 1u);
    assert(!memcmp(&bdl_before, &streams[0].bdl[0], sizeof(bdl_before)));
    assert(!memcmp(sd_before, fake.regs + 0x80u, sizeof(sd_before)));
    assert(fake.mmio_write_count == mmio_writes_before);
    route_bind_result = 0;
    assert(!audio_stream_close(preflight_id));

    uint32_t capture_id;
    assert(!audio_stream_open(card, 0u, AUDIO_CAPTURE, &capture_id));
    assert(capture_id != stream_id);
    struct audio_params_ext capture_params = params;
    assert(!audio_stream_prepare_format(capture_id, &capture_params, &pcm));
    assert(hda_mmio_read32(&controller, 0x80u) & (1u << 19));
    struct hda_stream *capture = &streams[0];
    uint32_t *capture_samples = pcm.kernel + 256u / sizeof(*capture_samples);
    capture_samples[0] = 0xaaaabbbbu; capture_samples[1] = 0xccccddddu;
    assert(capture_samples[0] == 0xaaaabbbbu && capture_samples[1] == 0xccccddddu);
    assert(!audio_stream_trigger(capture_id, AUDIO_START));
    fake_stream_irq(capture, 256u, HDA_SDSTS_FIFOE);
    assert(hda_stream_service_locked(&controller, 1u) == 1u);
    assert(fake.regs[0x83u] == 0u);
    assert(capture->state == HDA_STREAM_XRUN);
    assert(!audio_stream_close(capture_id));

    uint32_t descriptor_id;
    assert(!audio_stream_open(card, 0u, AUDIO_PLAYBACK, &descriptor_id));
    assert(descriptor_id != capture_id);
    struct audio_params_ext descriptor_params = params;
    descriptor_params.pcm.sample_bits = 24u;
    descriptor_params.pcm.frame_bytes = 8u;
    descriptor_params.format = AUDIO_FORMAT_S32_LE;
    descriptor_params.subformat = AUDIO_SUBFORMAT_MSBITS_24;
    descriptor_params.significant_bits = 24u;
    assert(!audio_stream_prepare_format(descriptor_id, &descriptor_params, &pcm));
    assert(hda_mmio_read16(&controller, 0x92u) == 0x31u);
    struct hda_stream *descriptor = &streams[0];
    assert(!audio_stream_trigger(descriptor_id, AUDIO_START));
    fake_stream_irq(descriptor, 128u, HDA_SDSTS_DESE);
    assert(hda_stream_service_locked(&controller, 1u) == 1u);
    assert(fake.regs[0x83u] == 0u);
    assert(descriptor->state == HDA_STREAM_XRUN);
    uint64_t descriptor_frames;
    assert(audio_stream_pointer(descriptor_id, &descriptor_frames) == -EPIPE);
    assert(!audio_stream_close(descriptor_id));

    uint32_t repeat_id;
    assert(!audio_stream_open(card, 0u, AUDIO_PLAYBACK, &repeat_id));
    assert(!audio_stream_prepare_format(repeat_id, &params, &pcm));
    struct hda_stream *repeat = &streams[0];
    assert(!audio_stream_trigger(repeat_id, AUDIO_START));
    fake_stream_irq(repeat, 128u, HDA_SDSTS_BCIS);
    uint64_t polled;
    assert(!audio_stream_pointer(repeat_id, &polled) && polled == 32u);
    assert(hda_stream_service_locked(&controller, 1u) == 1u);
    assert(fake.regs[0x83u] == 0u);
    uint64_t periods_before_repeat = audio_card_periods(card);
    fake_stream_irq(repeat, 128u, HDA_SDSTS_BCIS);
    assert(hda_stream_service_locked(&controller, 1u) == 0u);
    assert(fake.regs[0x83u] == 0u);
    assert(repeat->state == HDA_STREAM_RUNNING);
    assert(audio_card_periods(card) == periods_before_repeat);
    assert(!audio_stream_close(repeat_id));
    /* A full tiny ring can pass within one 10 ms tick with unchanged LPIB. */
    uint32_t tiny_id;
    assert(!audio_stream_open(card, 0u, AUDIO_PLAYBACK, &tiny_id));
    assert(!audio_stream_prepare_format(tiny_id, &params, &pcm));
    hda_test_mmio_write32(fake.regs, 0x30u, 0u);
    assert(!audio_stream_trigger(tiny_id, AUDIO_START));
    struct hda_stream *tiny = &streams[0];
    hda_test_mmio_write32(fake.regs, 0x30u, 6000u); /* 250 us. */
    fake_stream_irq(tiny, 128u, HDA_SDSTS_BCIS);
    assert(!audio_stream_pointer(tiny_id, &polled) && polled == 32u);
    hda_test_mmio_write32(fake.regs, 0x30u, 72000u); /* Another 2.75 ms. */
    assert(audio_stream_pointer(tiny_id, &polled) == -EPIPE);
    assert(tiny->frame_position == 32u);
    /* A position rejection must quiesce DMA before publishing XRUN. If RUN
     * cannot be cleared, keep the live lease and retry at the next IRQ. */
    fake.stop_stuck = 1u;
    assert(hda_stream_service_locked(&controller, 1u) == 0u);
    assert(tiny->state == HDA_STREAM_RUNNING);
    assert(hda_mmio_read32(&controller, 0x80u) & HDA_SDCTL_RUN);
    assert(audio_stream_period_poll(tiny_id, &completed_frames, &completion_error) == 0);
    fake.stop_stuck = 0u;
    fake_stream_irq(tiny, 128u, HDA_SDSTS_BCIS);
    assert(hda_stream_service_locked(&controller, 1u) == 1u);
    assert(tiny->state == HDA_STREAM_XRUN);
    assert(!(hda_mmio_read32(&controller, 0x80u) & HDA_SDCTL_RUN));
    assert(audio_stream_period_poll(tiny_id, &completed_frames, &completion_error) == 1);
    assert(completion_error == -EPIPE && completed_frames == 32u);
    assert(!audio_stream_close(tiny_id));

    /* Guest timer delivery can bunch up while the controller's link clock
     * and DMA advance by less than a ring. Coarse ticks are not link samples;
     * they only disambiguate intervals spanning the 32-bit WALLCLK range. */
    uint32_t drift_id;
    struct audio_params_ext drift_params=params;
    drift_params.pcm.period_frames=512;drift_params.pcm.buffer_frames=2048;
    struct audio_dma drift_dma={.kernel=calloc(1,8192),.bus=0x800000u,.bytes=8192};
    assert(drift_dma.kernel);
    api.ticks=fake_ticks;fake_tick_count=0;
    hda_test_mmio_write32(fake.regs,0x30u,0u);
    assert(!audio_stream_open(card,0,AUDIO_PLAYBACK,&drift_id));
    assert(!audio_stream_prepare_format(drift_id,&drift_params,&drift_dma));
    assert(!audio_stream_trigger(drift_id,AUDIO_START));
    struct hda_stream *drift=&streams[0];
    fake_tick_count=9;hda_test_mmio_write32(fake.regs,0x30u,240000u);
    *drift->position=1920u;
    assert(!audio_stream_pointer(drift_id,&polled) && polled==480u);
    fake_tick_count=18;hda_test_mmio_write32(fake.regs,0x30u,480000u);
    *drift->position=3840u;
    assert(!audio_stream_pointer(drift_id,&polled) && polled==960u);
    fake_tick_count=18018;hda_test_mmio_write32(fake.regs,0x30u,480001u);
    assert(audio_stream_pointer(drift_id,&polled)==-EPIPE);
    assert(!audio_stream_close(drift_id));free(drift_dma.kernel);
    api.ticks=NULL;fake_tick_count=0;
    puts("controller link clock/timer drift and full-WALLCLK-alias guard PASS");

    /* A healthy observation across WALLCLK wrap and after PAUSE stays monotonic. */
    uint32_t clock_id;
    assert(!audio_stream_open(card, 0u, AUDIO_PLAYBACK, &clock_id));
    assert(!audio_stream_prepare_format(clock_id, &params, &pcm));
    hda_test_mmio_write32(fake.regs, 0x30u, 0xfffff000u);
    assert(!audio_stream_trigger(clock_id, AUDIO_START));
    struct hda_stream *clock_stream = &streams[0];
    hda_test_mmio_write32(fake.regs, 0x30u, 0x2000u);
    fake_stream_irq(clock_stream, 128u, HDA_SDSTS_BCIS);
    assert(!audio_stream_pointer(clock_id, &polled) && polled == 32u);
    hda_test_mmio_write32(fake.regs, 0x30u, 0x4000u);
    assert(!audio_stream_pointer(clock_id, &polled) && polled == 32u);
    assert(!audio_stream_trigger(clock_id, AUDIO_PAUSE));
    hda_test_mmio_write32(fake.regs, 0x30u, 2000000u);
    assert(!audio_stream_pointer(clock_id, &polled) && polled == 32u);
    assert(!audio_stream_trigger(clock_id, AUDIO_UNPAUSE));
    hda_test_mmio_write32(fake.regs, 0x30u, 2024000u);
    fake_stream_irq(clock_stream, 256u, HDA_SDSTS_BCIS);
    assert(!audio_stream_pointer(clock_id, &polled) && polled == 64u);
    /* Coarse ticks reject a long gap which aliases the 32-bit WALLCLK. */
    api.ticks = fake_ticks;
    fake_tick_count = 18000u;
    hda_test_mmio_write32(fake.regs, 0x30u, 2025000u);
    assert(audio_stream_pointer(clock_id, &polled) == -EPIPE);
    assert(clock_stream->frame_position == 64u);
    assert(!audio_stream_close(clock_id));
    api.ticks = NULL;
    fake_tick_count = 0u;

    /* A complete ring with identical LPIB cannot be silently counted as zero. */
    uint32_t full_clock_id;
    assert(!audio_stream_open(card, 0u, AUDIO_PLAYBACK, &full_clock_id));
    assert(!audio_stream_prepare_format(full_clock_id, &params, &pcm));
    hda_test_mmio_write32(fake.regs, 0x30u, 0u);
    assert(!audio_stream_trigger(full_clock_id, AUDIO_START));
    hda_test_mmio_write32(fake.regs, 0x30u, 64000u); /* 128 frames at 48 kHz. */
    assert(audio_stream_pointer(full_clock_id, &polled) == -EPIPE);
    assert(!audio_stream_close(full_clock_id));

    free(pcm.kernel);
    fake.position_stuck=1u;
    assert(hda_stream_controller_destroy(&controller)==-EIO);
    assert(controller.position_phys && controller.streams);
    fake.position_stuck=0u;
    assert(!hda_stream_controller_destroy(&controller));
    assert(!(hda_test_mmio_read32(fake.regs,0x70u)&1u));
    assert(!audio_unregister_card(card, 0u));
    return 0;
}
