#include "music.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static int vlq(const uint8_t *data, uint32_t end, uint32_t *position,
               uint32_t *value)
{
    uint32_t result = 0;
    uint32_t count = 0;
    uint8_t byte;
    do {
        if (*position >= end || count++ == 4U) return -EINVAL;
        byte = data[(*position)++];
        result = (result << 7) | (byte & 0x7fU);
    } while (byte & 0x80U);
    *value = result;
    return 0;
}

static int event_append(struct doom_music_song *song, uint64_t tick, uint8_t status,
                        uint8_t a, uint8_t b, uint8_t track, uint8_t meta)
{
    struct doom_music_event *event;
    if (song->event_count >= DOOM_MUSIC_MAX_EVENTS) return -E2BIG;
    event = &song->events[song->event_count++];
    *event = (struct doom_music_event){.tick = tick, .status = status,
                                       .a = a, .b = b, .track = track,
                                       .meta = meta};
    return 0;
}

static int parse_midi_track(struct doom_music_song *song, const uint8_t *data,
                            uint32_t start, uint32_t end, uint8_t track)
{
    uint32_t position = start;
    uint64_t tick = 0;
    uint8_t running = 0;
    while (position < end) {
        uint32_t delta;
        uint8_t status;
        if (vlq(data, end, &position, &delta) < 0) return -EINVAL;
        tick += delta;
        if (position >= end) return -EINVAL;
        status = data[position++];
        if (status < 0x80U) {
            if (!running) return -EINVAL;
            --position;
            status = running;
        } else if (status < 0xf0U) {
            running = status;
        }
        if (status == 0xffU) {
            uint32_t size;
            uint8_t meta;
            if (position >= end) return -EINVAL;
            meta = data[position++];
            if (vlq(data, end, &position, &size) < 0 || size > end - position)
                return -EINVAL;
            if (meta == 0x51U && size == 3U) {
                if (event_append(song, tick, status, data[position],
                                 data[position + 1U], track, meta) < 0)
                    return -E2BIG;
                song->events[song->event_count - 1U].c = data[position + 2U];
            }
            if (meta == 0x2fU && event_append(song, tick, status, 0, 0, track, meta) < 0)
                return -E2BIG;
            position += size;
            continue;
        }
        if (status == 0xf0U || status == 0xf7U) {
            uint32_t size;
            if (vlq(data, end, &position, &size) < 0 || size > end - position)
                return -EINVAL;
            position += size;
            continue;
        }
        if (status >= 0xf1U) {
            uint32_t payload = status == 0xf2U ? 2U :
                               ((status == 0xf1U || status == 0xf3U) ? 1U : 0U);
            if (payload > end - position) return -EINVAL;
            position += payload;
            continue;
        }
        if ((status & 0xf0U) == 0xc0U || (status & 0xf0U) == 0xd0U) {
            if (position >= end) return -EINVAL;
            if (event_append(song, tick, status, data[position++], 0, track, 0) < 0)
                return -E2BIG;
        } else {
            if (end - position < 2U) return -EINVAL;
            if (event_append(song, tick, status, data[position], data[position + 1U],
                             track, 0) < 0) return -E2BIG;
            position += 2U;
        }
    }
    return 0;
}

static int event_compare(const struct doom_music_event *a,
                         const struct doom_music_event *b)
{
    if (a->tick < b->tick) return -1;
    if (a->tick > b->tick) return 1;
    return a->track < b->track ? -1 : (a->track > b->track ? 1 : 0);
}

static void sort_events(struct doom_music_song *song)
{
    uint32_t i;
    for (i = 1; i < song->event_count; ++i) {
        struct doom_music_event value = song->events[i];
        uint32_t j = i;
        while (j && event_compare(&value, &song->events[j - 1U]) < 0) {
            song->events[j] = song->events[j - 1U];
            --j;
        }
        song->events[j] = value;
    }
}

static void assign_frames(struct doom_music_song *song)
{
    uint64_t last_tick = 0;
    uint64_t frame = 0;
    uint64_t remainder = 0;
    uint32_t tempo = 500000U;
    uint32_t i;
    for (i = 0; i < song->event_count; ++i) {
        struct doom_music_event *event = &song->events[i];
        uint64_t ticks = event->tick - last_tick;
        uint64_t numerator = ticks * tempo * song->rate + remainder;
        uint64_t denominator = (uint64_t)song->division * 1000000ULL;
        frame += numerator / denominator;
        remainder = numerator % denominator;
        event->frame = frame;
        if (event->meta == 0x51U) {
            tempo = ((uint32_t)event->a << 16) | ((uint32_t)event->b << 8) |
                    song->events[i].c;
        }
        last_tick = event->tick;
    }
    song->tempo = tempo;
}

static int parse_midi(struct doom_music_song *song, const uint8_t *data, uint32_t length)
{
    uint32_t header;
    uint16_t format;
    uint16_t tracks;
    uint32_t position;
    uint16_t division;
    uint32_t index;
    if (length < 14U || memcmp(data, "MThd", 4) != 0) return -EINVAL;
    header = be32(data + 4U);
    if (header < 6U || header > length - 8U) return -EINVAL;
    format = be16(data + 8U);
    tracks = be16(data + 10U);
    division = be16(data + 12U);
    if (format > 1U || !tracks || tracks > DOOM_MUSIC_MAX_TRACKS || !division ||
        (division & 0x8000U)) return -ENOTSUP;
    song->division = division;
    position = 8U + header;
    for (index = 0; index < tracks; ++index) {
        uint32_t size;
        if (length - position < 8U || memcmp(data + position, "MTrk", 4) != 0)
            return -EINVAL;
        size = be32(data + position + 4U);
        position += 8U;
        if (size > length - position) return -EINVAL;
        if (parse_midi_track(song, data, position, position + size, (uint8_t)index) < 0)
            return -EINVAL;
        position += size;
    }
    sort_events(song);
    assign_frames(song);
    return 0;
}

static int parse_mus(struct doom_music_song *song, const uint8_t *data, uint32_t length)
{
    uint16_t score_start;
    uint16_t score_length;
    uint32_t position;
    uint64_t tick = 0;
    if (length < 16U || memcmp(data, "MUS\x1a", 4) != 0) return -EINVAL;
    score_length = (uint16_t)(data[4] | ((uint16_t)data[5] << 8));
    score_start = (uint16_t)(data[6] | ((uint16_t)data[7] << 8));
    if (score_start < 16U || score_start > length || score_length > length - score_start)
        return -EINVAL;
    song->division = 140U;
    position = score_start;
    while (position < (uint32_t)score_start + score_length) {
        uint8_t descriptor = data[position++];
        uint8_t type = (descriptor >> 4) & 7U;
        uint8_t channel = descriptor & 15U;
        uint8_t value;
        if (channel >= 16U) return -EINVAL;
        if (type == 0U || type == 1U) {
            if (position >= length) return -EINVAL;
            value = data[position++];
            if (event_append(song, tick, type == 1U ? 0x90U : 0x80U,
                             value & 0x7fU, 100U, channel, 0) < 0) return -E2BIG;
            if (value & 0x80U) {
                if (position >= length) return -EINVAL;
                song->events[song->event_count - 1U].b = data[position++];
            }
        } else if (type == 2U) {
            if (position >= length) return -EINVAL;
            ++position;
        } else if (type == 3U) {
            if (position >= length) return -EINVAL;
            ++position;
        } else if (type == 4U) {
            if (length - position < 2U) return -EINVAL;
            position += 2U;
        } else if (type != 6U) {
            return -EINVAL;
        }
        if (descriptor & 0x80U) {
            uint32_t delay = 0;
            do {
                if (position >= length) return -EINVAL;
                value = data[position++];
                delay = (delay << 7) | (value & 0x7fU);
            } while (value & 0x80U);
            tick += delay;
        }
        if (type == 6U) break;
    }
    sort_events(song);
    for (position = 0; position < song->event_count; ++position)
        song->events[position].frame = (song->events[position].tick * song->rate) / 140U;
    return 0;
}

int doom_music_parse(struct doom_music_song *song, const uint8_t *data,
                     uint32_t length, uint32_t output_rate)
{
    if (!song || !data || !length || !output_rate) return -EINVAL;
    memset(song, 0, sizeof(*song));
    song->rate = output_rate;
    if (length >= 4U && memcmp(data, "MUS\x1a", 4) == 0) {
        song->format = DOOM_MUSIC_MUS;
        return parse_mus(song, data, length);
    }
    if (length >= 4U && memcmp(data, "MThd", 4) == 0) {
        song->format = DOOM_MUSIC_MIDI;
        return parse_midi(song, data, length);
    }
    return -EINVAL;
}

int doom_music_genmidi_valid(const uint8_t *data, uint32_t length)
{
    if (!data || length < 8U || memcmp(data, "#OPL_II#", 8) != 0) return 0;
    return length >= 8U + 175U * 36U;
}

int doom_music_next(struct doom_music_song *song, struct doom_music_event *event)
{
    if (!song || !event || song->cursor >= song->event_count) return 0;
    *event = song->events[song->cursor++];
    return 1;
}

/* The player deliberately lives beside the parser so the event cursor and
 * the OPL clock cannot drift apart.  It is a small, deterministic OPL bridge;
 * malformed or missing GENMIDI falls back to one neutral instrument. */
#include "music_opl.h"

struct doom_music_player_voice {
    uint8_t active;
    uint8_t channel;
    uint8_t note;
    uint8_t percussion;
    uint32_t age;
    uint16_t fnum;
    uint8_t block;
};

struct doom_music_player {
    struct doom_music_song *song;
    struct doom_music_opl opl;
    const uint8_t *genmidi;
    uint32_t genmidi_length;
    uint64_t frame;
    uint32_t age;
    uint8_t active;
    uint8_t paused;
    uint8_t loop;
    uint8_t volume;
    uint8_t programs[16];
    uint8_t sustain[16];
    struct doom_music_player_voice voices[18];
};

static struct doom_music_player music_player;
static uint8_t *music_genmidi;

static void player_release_voice(struct doom_music_player_voice *voice)
{
    uint16_t reg;
    uint8_t channel;
    if (!voice->active) return;
    channel = voice->channel;
    reg = (uint16_t)(0xb0U + (channel % 9U));
    if (channel >= 9U) reg = (uint16_t)(reg + 0x100U);
    doom_music_opl_write(&music_player.opl, reg,
                         (uint8_t)((voice->block << 2) | ((voice->fnum >> 8) & 3U)));
    voice->active = 0;
}

static uint16_t player_fnum(uint8_t note, uint8_t *block)
{
    static const uint16_t base[12] =
        {0x157, 0x16b, 0x181, 0x198, 0x1b0, 0x1ca,
         0x1e5, 0x202, 0x220, 0x240, 0x261, 0x284};
    uint8_t octave = note / 12U;
    uint8_t semitone = note % 12U;
    *block = octave > 1U ? (uint8_t)(octave - 1U) : 0U;
    if (*block > 7U) *block = 7U;
    return base[semitone];
}

static const uint8_t opl_operator[9][2] = {
    {0, 3}, {1, 4}, {2, 5}, {6, 9}, {7, 10},
    {8, 11}, {12, 15}, {13, 16}, {14, 17}
};

static void player_program_voice(uint8_t channel, uint8_t program)
{
    const uint8_t *instrument = NULL;
    uint8_t voice[16] = {0};
    uint16_t base = channel >= 9U ? 0x100U : 0U;
    uint8_t local = channel % 9U;
    uint8_t op;
    if (music_player.genmidi && music_player.genmidi_length >= 8U + 175U * 36U) {
        uint32_t index = program < 128U ? program : 0U;
        instrument = music_player.genmidi + 8U + index * 36U;
        memcpy(voice, instrument + 4U, sizeof(voice));
    }
    for (op = 0; op < 2U; ++op) {
        uint8_t offset = opl_operator[local][op];
        uint8_t *v = &voice[op * 8U];
        doom_music_opl_write(&music_player.opl, (uint16_t)(base + 0x20U + offset), v[0]);
        doom_music_opl_write(&music_player.opl, (uint16_t)(base + 0x40U + offset),
                             (uint8_t)(v[1] | (op ? 0x00U : 0x3fU)));
        doom_music_opl_write(&music_player.opl, (uint16_t)(base + 0x60U + offset), v[2]);
        doom_music_opl_write(&music_player.opl, (uint16_t)(base + 0x80U + offset), v[3]);
        doom_music_opl_write(&music_player.opl, (uint16_t)(base + 0xe0U + offset),
                             (uint8_t)(v[4] & 3U));
    }
    doom_music_opl_write(&music_player.opl, (uint16_t)(base + 0xc0U + local),
                         (uint8_t)(voice[5] & 0x0fU));
}

static struct doom_music_player_voice *player_alloc_voice(uint8_t channel,
                                                            uint8_t note)
{
    struct doom_music_player_voice *candidate = NULL;
    uint8_t i;
    for (i = 0; i < 18U; ++i) {
        if (music_player.voices[i].active && music_player.voices[i].channel == channel &&
            music_player.voices[i].note == note) {
            player_release_voice(&music_player.voices[i]);
            return &music_player.voices[i];
        }
        if (!music_player.voices[i].active && !candidate) candidate = &music_player.voices[i];
    }
    if (!candidate) {
        candidate = &music_player.voices[0];
        for (i = 1; i < 18U; ++i)
            if (music_player.voices[i].age < candidate->age) candidate = &music_player.voices[i];
        player_release_voice(candidate);
    }
    return candidate;
}

static void player_note_on(uint8_t channel, uint8_t note, uint8_t velocity)
{
    struct doom_music_player_voice *voice;
    uint8_t block;
    if (channel >= 16U || note >= 128U || velocity == 0U) return;
    voice = player_alloc_voice(channel, note);
    memset(voice, 0, sizeof(*voice));
    voice->active = 1U;
    voice->channel = channel;
    voice->note = note;
    voice->percussion = channel == 9U;
    voice->age = ++music_player.age;
    voice->fnum = player_fnum(note, &block);
    voice->block = block;
    player_program_voice(channel, voice->percussion ? (uint8_t)(128U +
                       (note >= 35U && note <= 81U ? note - 35U : 0U)) :
                       music_player.programs[channel]);
    {
        uint16_t reg = (uint16_t)(0xa0U + (channel % 9U));
        uint16_t key = (uint16_t)(0xb0U + (channel % 9U));
        if (channel >= 9U) { reg += 0x100U; key += 0x100U; }
        doom_music_opl_write(&music_player.opl, reg, (uint8_t)voice->fnum);
        doom_music_opl_write(&music_player.opl, key,
                             (uint8_t)(0x20U | (block << 2) | ((voice->fnum >> 8) & 3U)));
    }
}

static void player_note_off(uint8_t channel, uint8_t note)
{
    uint8_t i;
    if (channel >= 16U) return;
    if (music_player.sustain[channel]) return;
    for (i = 0; i < 18U; ++i)
        if (music_player.voices[i].active && music_player.voices[i].channel == channel &&
            music_player.voices[i].note == note) player_release_voice(&music_player.voices[i]);
}

static void player_dispatch(const struct doom_music_event *event)
{
    uint8_t type = event->status & 0xf0U;
    uint8_t channel = event->status & 0x0fU;
    uint8_t i;
    if (event->meta == 0x2fU) {
        for (i = 0; i < 18U; ++i) player_release_voice(&music_player.voices[i]);
        return;
    }
    if (event->status == 0xffU) return;
    if (channel >= 16U) return;
    if (type == 0x90U) player_note_on(channel, event->a, event->b);
    else if (type == 0x80U) player_note_off(channel, event->a);
    else if (type == 0xc0U) music_player.programs[channel] = event->a & 127U;
    else if (type == 0xb0U) {
        if (event->a == 64U) music_player.sustain[channel] = event->b >= 64U;
        else if (event->a == 123U || event->a == 120U)
            for (i = 0; i < 18U; ++i)
                if (music_player.voices[i].active && music_player.voices[i].channel == channel)
                    player_release_voice(&music_player.voices[i]);
    }
}

struct doom_music_song *doom_music_register(const uint8_t *data, uint32_t length,
                                            uint32_t output_rate)
{
    struct doom_music_song *song;
    if (!data || !length || !output_rate) return NULL;
    song = (struct doom_music_song *)calloc(1, sizeof(*song));
    if (!song || doom_music_parse(song, data, length, output_rate) < 0) {
        free(song);
        return NULL;
    }
    return song;
}

void doom_music_unregister(struct doom_music_song *song)
{
    if (music_player.song == song) doom_music_stop();
    free(song);
}

int doom_music_play(struct doom_music_song *song, int loop)
{
    if (!song) return -EINVAL;
    music_player.song = song;
    music_player.frame = 0;
    music_player.age = 0;
    music_player.active = 1U;
    music_player.paused = 0U;
    music_player.loop = loop ? 1U : 0U;
    music_player.volume = music_player.volume ? music_player.volume : 127U;
    memset(music_player.programs, 0, sizeof(music_player.programs));
    memset(music_player.sustain, 0, sizeof(music_player.sustain));
    memset(music_player.voices, 0, sizeof(music_player.voices));
    if (doom_music_opl_init(&music_player.opl, song->rate) < 0) return -EINVAL;
    song->cursor = 0;
    return 0;
}

void doom_music_pause(int paused) { music_player.paused = paused ? 1U : 0U; }

void doom_music_set_volume(uint8_t volume) { music_player.volume = volume > 127U ? 127U : volume; }

void doom_music_stop(void)
{
    uint8_t i;
    for (i = 0; i < 18U; ++i) player_release_voice(&music_player.voices[i]);
    music_player.active = 0U;
    music_player.song = NULL;
}

int doom_music_playing(void) { return music_player.active && !music_player.paused; }

int doom_music_set_genmidi(const uint8_t *data, uint32_t length)
{
    uint8_t *copy;
    if (!doom_music_genmidi_valid(data, length)) return -EINVAL;
    copy = (uint8_t *)malloc(length);
    if (!copy) return -ENOMEM;
    memcpy(copy, data, length);
    free(music_genmidi);
    music_genmidi = copy;
    music_player.genmidi = music_genmidi;
    music_player.genmidi_length = length;
    return 0;
}

void doom_music_render(int16_t *stereo, uint32_t frames, uint32_t rate)
{
    uint32_t remaining = frames;
    int16_t *out = stereo;
    if (!stereo) return;
    if (!music_player.active || music_player.paused || !music_player.song ||
        rate != music_player.song->rate) {
        memset(stereo, 0, (size_t)frames * 2U * sizeof(*stereo));
        return;
    }
    while (remaining) {
        struct doom_music_song *song = music_player.song;
        uint64_t next = song->cursor < song->event_count ?
                        song->events[song->cursor].frame : UINT64_MAX;
        if (next <= music_player.frame) {
            uint32_t dispatched = 0;
            while (song->cursor < song->event_count &&
                   song->events[song->cursor].frame <= music_player.frame) {
                player_dispatch(&song->events[song->cursor++]);
                if (++dispatched == DOOM_MUSIC_MAX_DISPATCH) break;
            }
            if (song->cursor >= song->event_count) {
                if (!music_player.loop) {
                    doom_music_stop();
                    memset(out, 0, (size_t)remaining * 2U * sizeof(*out));
                    return;
                }
                song->cursor = 0;
                music_player.frame = 0;
                memset(music_player.voices, 0, sizeof(music_player.voices));
                doom_music_opl_init(&music_player.opl, song->rate);
            }
            continue;
        }
        {
            uint64_t until = next - music_player.frame;
            uint32_t chunk = until < remaining ? (uint32_t)until : remaining;
            doom_music_opl_generate(&music_player.opl, out, chunk);
            out += (size_t)chunk * 2U;
            remaining -= chunk;
            music_player.frame += chunk;
        }
    }
}
