#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../userland/apps/doom/audio_output.h"

static void test_fragment_geometry_contract(void)
{
    assert(DOOM_AUDIO_FRAGMENT_EXPONENT == 13U);
    assert(DOOM_AUDIO_FRAGMENT_COUNT == 64U);
    assert(DOOM_AUDIO_FRAGMENT_CONFIG == ((64U << 16) | 13U));
    assert((1U << DOOM_AUDIO_FRAGMENT_EXPONENT) * DOOM_AUDIO_FRAGMENT_COUNT ==
           524288U);
}

struct fake_io {
    int steps[16];
    unsigned int step_count;
    unsigned int step;
    uint8_t received[256];
    uint32_t received_bytes;
    unsigned int recoveries;
    long writable_frames;
};

static long fake_write(void *opaque, const void *data, uint32_t bytes);

static long fake_available(void *opaque)
{
    return ((struct fake_io *)opaque)->writable_frames;
}

static void test_hardware_clock_and_pending_frames(void)
{
    static const int16_t pcm[] = {1, 2, 3, 4};
    struct fake_io fake = {.steps = {3, -EAGAIN}, .step_count = 2,
                           .writable_frames = 8};
    struct doom_audio_io io = {.write_bytes = fake_write,
        .available_frames = fake_available, .opaque = &fake};
    struct doom_audio_output *output = doom_audio_output_create(&io, 8, 2);
    uint32_t frames = 99;
    assert(output && !doom_audio_writable_frames(output, &frames) && frames == 8);
    assert(doom_audio_submit(output, pcm, 4) == -EAGAIN);
    /* A short byte write leaves 5 bytes pending: reserve 3 complete frames. */
    assert(!doom_audio_writable_frames(output, &frames) && frames == 5);
    fake.writable_frames = 2;
    assert(!doom_audio_writable_frames(output, &frames) && frames == 0);
    assert(!doom_audio_flush(output));
    assert(!doom_audio_writable_frames(output, &frames) && frames == 2);
    fake.writable_frames = -ENODEV;
    frames = 99;
    assert(doom_audio_writable_frames(output, &frames) == -ENODEV && frames == 99);
    fake.writable_frames = -1;
    errno = EIO;
    assert(doom_audio_writable_frames(output, &frames) == -EIO && frames == 99);
    doom_audio_close(output);
    io.available_frames = NULL;
    output = doom_audio_output_create(&io, 8, 2);
    assert(output && doom_audio_writable_frames(output, &frames) == -EOPNOTSUPP);
    doom_audio_close(output);
}

static long fake_write(void *opaque, const void *data, uint32_t bytes)
{
    struct fake_io *fake = (struct fake_io *)opaque;
    int action = fake->step < fake->step_count ? fake->steps[fake->step++] : 0;
    if (action == -EAGAIN) {
        errno = EAGAIN;
        return -1;
    }
    if (action == -EINTR) {
        errno = EINTR;
        return -1;
    }
    if (action == -EPIPE || action == -ESTRPIPE) {
        return action;
    }
    if (action <= 0 || (uint32_t)action > bytes) {
        action = (int)bytes;
    }
    assert(fake->received_bytes + (uint32_t)action <= sizeof(fake->received));
    memcpy(fake->received + fake->received_bytes, data, (size_t)action);
    fake->received_bytes += (uint32_t)action;
    return action;
}

static int fake_recover(void *opaque, int error)
{
    struct fake_io *fake = (struct fake_io *)opaque;
    assert(error == EPIPE || error == ESTRPIPE);
    ++fake->recoveries;
    return 0;
}

static void test_short_eagain_and_flush(void)
{
    static const int16_t pcm[] = {0x0102, 0x0304, 0x0506, 0x0708};
    struct fake_io fake = {.steps = {3, -EAGAIN, 2, 2, 1}, .step_count = 5};
    struct doom_audio_io io = {.write_bytes = fake_write, .opaque = &fake};
    struct doom_audio_output *output = doom_audio_output_create(&io, 4, 2);
    assert(output);
    assert(doom_audio_submit(output, pcm, 4) == -EAGAIN);
    assert(fake.received_bytes == 3);
    assert(doom_audio_flush(output) == 0);
    assert(fake.received_bytes == sizeof(pcm));
    assert(memcmp(fake.received, pcm, sizeof(pcm)) == 0);
    struct doom_audio_stats stats;
    assert(!doom_audio_get_stats(output, &stats));
    assert(stats.written_bytes == sizeof(pcm));
    doom_audio_close(output);
}

static void test_eintr_and_recovery(void)
{
    static const int16_t pcm[] = {11, 22};
    struct fake_io fake = {.steps = {-EINTR, -EPIPE, 4}, .step_count = 3};
    struct doom_audio_io io = {.write_bytes = fake_write, .recover = fake_recover,
                               .opaque = &fake};
    struct doom_audio_output *output = doom_audio_output_create(&io, 2, 2);
    assert(output);
    assert(doom_audio_submit(output, pcm, 2) == 0);
    assert(fake.recoveries == 1);
    assert(fake.received_bytes == sizeof(pcm));
    assert(memcmp(fake.received, pcm, sizeof(pcm)) == 0);
    struct doom_audio_stats stats;
    assert(!doom_audio_get_stats(output, &stats));
    assert(stats.written_bytes == 4 && stats.startup_recoveries == 1 &&
           !stats.xrun_recoveries && !stats.suspend_recoveries);
    fake.steps[0] = -EPIPE; fake.steps[1] = 4; fake.step_count = 2; fake.step = 0;
    assert(!doom_audio_submit(output, pcm, 2));
    assert(!doom_audio_get_stats(output, &stats));
    assert(stats.written_bytes == 8 && stats.startup_recoveries == 1 &&
           stats.xrun_recoveries == 1 && !stats.suspend_recoveries);
    fake.steps[0] = -ESTRPIPE; fake.step = 0;
    assert(!doom_audio_submit(output, pcm, 2));
    assert(!doom_audio_get_stats(output, &stats));
    assert(stats.written_bytes == 12 && stats.startup_recoveries == 1 &&
           stats.xrun_recoveries == 1 && stats.suspend_recoveries == 1);
    doom_audio_close(output);
}

static void test_queue_bound(void)
{
    static const int16_t pcm[] = {1, 2, 3, 4};
    struct fake_io fake = {.steps = {-EAGAIN}, .step_count = 1};
    struct doom_audio_io io = {.write_bytes = fake_write, .opaque = &fake};
    struct doom_audio_output *output = doom_audio_output_create(&io, 2, 2);
    assert(output);
    assert(doom_audio_submit(output, pcm, 4) == -ENOSPC);
    assert(doom_audio_flush(output) == 0);
    doom_audio_close(output);
}

static void test_repeated_recovery_is_bounded(void)
{
    static const int16_t pcm[] = {11, 22};
    struct fake_io fake = {.step_count = 10};
    for (unsigned int i = 0; i < fake.step_count; ++i) fake.steps[i] = -EPIPE;
    struct doom_audio_io io = {.write_bytes = fake_write, .recover = fake_recover,
                               .opaque = &fake};
    struct doom_audio_output *output = doom_audio_output_create(&io, 2, 2);
    assert(output);
    assert(doom_audio_submit(output, pcm, 2) == -EPIPE);
    assert(fake.step < fake.step_count && !fake.received_bytes);
    doom_audio_close(output);

    fake = (struct fake_io){.steps = {-EAGAIN}, .step_count = 11};
    for (unsigned int i = 1; i < fake.step_count; ++i) fake.steps[i] = -EPIPE;
    output = doom_audio_output_create(&io, 2, 2);
    assert(output && doom_audio_submit(output, pcm, 2) == -EAGAIN);
    assert(doom_audio_flush(output) == -EPIPE);
    assert(fake.step < fake.step_count && !fake.received_bytes);
    doom_audio_close(output);
}

int main(void)
{
    test_fragment_geometry_contract();
    test_hardware_clock_and_pending_frames();
    test_short_eagain_and_flush();
    test_eintr_and_recovery();
    test_queue_bound();
    test_repeated_recovery_is_bounded();
    puts("Doom output queue PASS");
    return 0;
}
