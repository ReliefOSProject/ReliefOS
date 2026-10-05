#ifndef RELIEFOS_DOOM_MUSIC_H
#define RELIEFOS_DOOM_MUSIC_H

#include <stdint.h>

#define DOOM_MUSIC_MAX_EVENTS 16384U
#define DOOM_MUSIC_MAX_DISPATCH 4096U
#define DOOM_MUSIC_MAX_TRACKS 32U

enum doom_music_format {
    DOOM_MUSIC_MUS = 1,
    DOOM_MUSIC_MIDI = 2
};

struct doom_music_event {
    uint64_t frame;
    uint64_t tick;
    uint8_t status;
    uint8_t a;
    uint8_t b;
    uint8_t c;
    uint8_t track;
    uint8_t meta;
};

struct doom_music_song {
    enum doom_music_format format;
    uint32_t rate;
    uint16_t division;
    uint32_t tempo;
    uint32_t event_count;
    uint32_t cursor;
    struct doom_music_event events[DOOM_MUSIC_MAX_EVENTS];
};

/* Runtime player API.  The parser remains usable as a pure, allocation-free
 * value object through doom_music_parse(); these helpers own a parsed song
 * and advance it in output-frame time. */
struct doom_music_song *doom_music_register(const uint8_t *data,
                                            uint32_t length,
                                            uint32_t output_rate);
void doom_music_unregister(struct doom_music_song *song);
int doom_music_play(struct doom_music_song *song, int loop);
void doom_music_pause(int paused);
void doom_music_set_volume(uint8_t volume);
void doom_music_stop(void);
int doom_music_playing(void);
void doom_music_render(int16_t *stereo, uint32_t frames, uint32_t rate);
int doom_music_set_genmidi(const uint8_t *data, uint32_t length);

int doom_music_parse(struct doom_music_song *song, const uint8_t *data,
                     uint32_t length, uint32_t output_rate);
int doom_music_genmidi_valid(const uint8_t *data, uint32_t length);
int doom_music_next(struct doom_music_song *song, struct doom_music_event *event);

#endif
