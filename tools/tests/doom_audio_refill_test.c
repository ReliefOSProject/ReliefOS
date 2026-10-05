#include <assert.h>
#include "../../userland/apps/doom/reliefos_audio.c"
#pragma GCC diagnostic push
/* Retained upstream config helper has an unused extern with SOUND disabled. */
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "../../third_party/doomgeneric/doomgeneric/i_sound.c"
#pragma GCC diagnostic pop
#define main doom_port_main
#include "../../userland/apps/doom/main.c"
#undef main

struct refill_hardware {
    uint32_t free_frames;
    uint32_t written_frames;
    uint32_t queries;
    uint32_t writes;
};

static long refill_available(void *opaque)
{
    struct refill_hardware *hardware = opaque;
    ++hardware->queries;
    return hardware->free_frames;
}

static long refill_write(void *opaque, const void *pcm, uint32_t bytes)
{
    struct refill_hardware *hardware = opaque;
    (void)pcm;
    assert(!(bytes % 4U) && bytes / 4U <= hardware->free_frames);
    assert(bytes <= 32768U);
    ++hardware->writes;
    hardware->free_frames -= bytes / 4U;
    hardware->written_frames += bytes / 4U;
    return bytes;
}

static struct refill_hardware *sleep_hardware;
static unsigned int sleeps;
unsigned long reliefos_uptime_ms(void) { return 1U; }
int reliefos_gui_poll_app_event(struct reliefos_gui_app_event *event)
{ (void)event; return 0; }
int reliefos_gui_set_mouse_visible(uint32_t id, uint32_t visible)
{ (void)id; (void)visible; return 0; }
int reliefos_gui_destroy_app_window(uint32_t id) { (void)id; return 0; }
int sleep_ms(unsigned long ms)
{
    assert(ms == 1U && sleep_hardware);
    if (sound_module) assert(!sleep_hardware->free_frames);
    ++sleeps;
    sleep_hardware->free_frames += 333U;
    return 0;
}

/* Music is inactive in this refill test; its own real OPL suite covers render. */
void doom_music_render(int16_t *stereo, uint32_t frames, uint32_t rate)
{ (void)stereo; (void)frames; (void)rate; assert(0); }
void doom_music_opl_generate(struct doom_music_opl *opl, int16_t *stereo,
                             uint32_t frames)
{ (void)opl; (void)stereo; (void)frames; assert(0); }

int main(void)
{
    /* After recovery or an odd hardware position, fewer than 256 frames can
     * remain before a full-ring startup watermark. They must be submitted. */
    const uint32_t spaces[] = {0U, 1U, 139U, 255U, 5120U + 139U, 8192U, 131072U};
    for (unsigned int i = 0; i < sizeof(spaces) / sizeof(spaces[0]); ++i) {
        struct refill_hardware hardware = {.free_frames = spaces[i]};
        struct doom_audio_io io = {.write_bytes = refill_write,
            .available_frames = refill_available, .opaque = &hardware};
        reliefos_doom_output = doom_audio_output_create(&io, 5120U, 4U);
        assert(reliefos_doom_output);
        reliefos_doom_audio_open_attempted = 1U;
        reliefos_doom_audio_available = 1U;
        reliefos_doom_update_sound();
        assert(hardware.written_frames == spaces[i] && !hardware.free_frames);
        assert(hardware.queries == 1U);
        assert(hardware.writes == (spaces[i] + 8191U) / 8192U);
        doom_audio_close(reliefos_doom_output);
    }
    struct refill_hardware hardware = {.free_frames = 139U};
    struct doom_audio_io io = {.write_bytes = refill_write,
        .available_frames = refill_available, .opaque = &hardware};
    reliefos_doom_output = doom_audio_output_create(&io, 5120U, 4U);
    assert(reliefos_doom_output);
    sleep_hardware = &hardware;
    sound_module_t dispatcher = {.Update = reliefos_doom_update_sound};
    sound_module = &dispatcher;
    /* Actual frame pacing must service audio before and after sleeping. */
    DG_SleepMs(1U);
    assert(sleeps == 1U && hardware.queries == 2U);
    assert(hardware.written_frames == 472U && !hardware.free_frames);
    doom_audio_close(reliefos_doom_output);
    sound_module = NULL;
    hardware = (struct refill_hardware){.free_frames = 50U};
    DG_SleepMs(1U);
    assert(sleeps == 2U && !hardware.queries && !hardware.written_frames);
    puts("PASS real Doom refill submits partial blocks through full-ring startup");
    return 0;
}
