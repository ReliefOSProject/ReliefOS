#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../userland/apps/doom/music.h"
#include "../../userland/apps/doom/music_opl.h"

static void put_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static void put_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static uint32_t make_midi(uint8_t *data, int running_status)
{
    uint8_t track[] = {
        0x00, 0xff, 0x51, 0x03, 0x07, 0xa1, 0x20,
        0x00, 0x90, 0x3c, 0x64,
        0x60, running_status ? 0x3c : 0x80, 0x00,
        0x00, 0xff, 0x2f, 0x00
    };
    memcpy(data, "MThd", 4);
    put_be32(data + 4, 6);
    put_be16(data + 8, 1);
    put_be16(data + 10, 1);
    put_be16(data + 12, 96);
    memcpy(data + 14, "MTrk", 4);
    put_be32(data + 18, sizeof(track));
    memcpy(data + 22, track, sizeof(track));
    return 22U + sizeof(track);
}

static void test_midi_timing_and_running_status(void)
{
    uint8_t data[128];
    struct doom_music_song song;
    struct doom_music_event event;
    uint32_t length = make_midi(data, 1);
    assert(doom_music_parse(&song, data, length, 48000) == 0);
    assert(song.format == DOOM_MUSIC_MIDI && song.division == 96);
    assert(doom_music_next(&song, &event) == 1);
    assert(event.status == 0xff && event.meta == 0x51 && event.frame == 0);
    assert(doom_music_next(&song, &event) == 1);
    assert(event.status == 0x90 && event.frame == 0);
    assert(doom_music_next(&song, &event) == 1);
    assert(event.status == 0x90 && event.a == 0x3c && event.frame == 24000);
}

static void test_mus_and_genmidi(void)
{
    uint8_t mus[32] = {0};
    uint8_t genmidi[8 + 175 * 36];
    struct doom_music_song song;
    struct doom_music_event event;
    memcpy(mus, "MUS\x1a", 4);
    mus[4] = 6;
    mus[6] = 16;
    mus[16] = 0x91;
    mus[17] = 60;
    mus[18] = 1;
    mus[19] = 0x00;
    mus[20] = 60;
    mus[21] = 0x60;
    assert(doom_music_parse(&song, mus, 22, 48000) == 0);
    assert(song.format == DOOM_MUSIC_MUS);
    assert(doom_music_next(&song, &event) == 1 && event.frame == 0);
    assert(doom_music_next(&song, &event) == 1 && event.frame == 342);
    memcpy(genmidi, "#OPL_II#", 8);
    assert(doom_music_genmidi_valid(genmidi, sizeof(genmidi)) == 1);
    genmidi[0] = 'X';
    assert(doom_music_genmidi_valid(genmidi, sizeof(genmidi)) == 0);
}

static void test_opl3_determinism(void)
{
    struct doom_music_opl opl;
    int16_t first[128] = {0};
    int16_t second[128] = {0};
    unsigned int i;
    int nonzero = 0;
    assert(doom_music_opl_init(&opl, 48000) == 0);
    doom_music_opl_write(&opl, 0x20, 0x01);
    doom_music_opl_write(&opl, 0x23, 0x01);
    doom_music_opl_write(&opl, 0x40, 0x00);
    doom_music_opl_write(&opl, 0x43, 0x00);
    doom_music_opl_write(&opl, 0x60, 0xf0);
    doom_music_opl_write(&opl, 0x63, 0xf0);
    doom_music_opl_write(&opl, 0x80, 0x77);
    doom_music_opl_write(&opl, 0x83, 0x77);
    doom_music_opl_write(&opl, 0xa0, 0x98);
    doom_music_opl_write(&opl, 0xb0, 0x31);
    doom_music_opl_write(&opl, 0xc0, 0x01);
    doom_music_opl_generate(&opl, first, 64);
    for (i = 0; i < 128; ++i) if (first[i]) nonzero = 1;
    assert(nonzero);
    doom_music_opl_init(&opl, 48000);
    doom_music_opl_write(&opl, 0x20, 0x01);
    doom_music_opl_write(&opl, 0x23, 0x01);
    doom_music_opl_write(&opl, 0x40, 0x00);
    doom_music_opl_write(&opl, 0x43, 0x00);
    doom_music_opl_write(&opl, 0x60, 0xf0);
    doom_music_opl_write(&opl, 0x63, 0xf0);
    doom_music_opl_write(&opl, 0x80, 0x77);
    doom_music_opl_write(&opl, 0x83, 0x77);
    doom_music_opl_write(&opl, 0xa0, 0x98);
    doom_music_opl_write(&opl, 0xb0, 0x31);
    doom_music_opl_write(&opl, 0xc0, 0x01);
    doom_music_opl_generate(&opl, second, 64);
    assert(memcmp(first, second, sizeof(first)) == 0);
}

static void test_sample_clock_player(void)
{
    uint8_t data[128];
    uint8_t genmidi[8 + 175 * 36] = {0};
    struct doom_music_song *song;
    int16_t whole[8192] = {0};
    int16_t chunks[8192] = {0};
    uint32_t length = make_midi(data, 1);
    memcpy(genmidi, "#OPL_II#", 8);
    /* A minimal carrier voice with a finite level and envelope. */
    genmidi[8U + 4U + 8U + 1U] = 0x00;
    genmidi[8U + 4U + 8U + 2U] = 0xf0;
    genmidi[8U + 4U + 8U + 3U] = 0x77;
    assert(doom_music_set_genmidi(genmidi, sizeof(genmidi)) == 0);
    song = doom_music_register(data, length, 48000);
    assert(song != NULL);
    assert(doom_music_play(song, 0) == 0);
    doom_music_render(whole, 4096, 48000);
    doom_music_stop();
    assert(doom_music_play(song, 0) == 0);
    doom_music_render(chunks, 512, 48000);
    for (unsigned int i = 1; i < 8; ++i)
        doom_music_render(chunks + i * 1024U, 512, 48000);
    assert(memcmp(whole, chunks, sizeof(whole)) == 0);
    doom_music_pause(1);
    memset(chunks, 0x7f, sizeof(chunks));
    doom_music_render(chunks, 64, 48000);
    for (unsigned int i = 0; i < 128; ++i) assert(chunks[i] == 0);
    doom_music_pause(0);
    doom_music_unregister(song);
}

int main(void)
{
    test_midi_timing_and_running_status();
    test_mus_and_genmidi();
    test_opl3_determinism();
    test_sample_clock_player();
    puts("Doom music parser/OPL3 PASS");
    return 0;
}
