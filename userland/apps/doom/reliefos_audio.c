#include <reliefos/syscall.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/soundcard.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "doomgeneric.h"
#include "i_sound.h"
#include "m_argv.h"
#include "w_wad.h"
#include "z_zone.h"
#include "audio_output.h"
#include "music.h"
#include "music_opl.h"

#define RELIEFOS_DOOM_AUDIO_RATE 48000U
#define RELIEFOS_DOOM_AUDIO_CHANNELS 2U
#define RELIEFOS_DOOM_AUDIO_BITS 16U
#define RELIEFOS_DOOM_AUDIO_TICK_HZ 35U
#define RELIEFOS_DOOM_AUDIO_FRAMES (RELIEFOS_DOOM_AUDIO_RATE / RELIEFOS_DOOM_AUDIO_TICK_HZ)
/* One bounded 32 KiB PCM write; reduces catch-up syscall scheduling overhead. */
#define RELIEFOS_DOOM_AUDIO_MAX_MIX_FRAMES 8192U
#define RELIEFOS_DOOM_AUDIO_MIX_CHANNELS 16U
#define RELIEFOS_DOOM_AUDIO_MAX_SAMPLE_FRAMES (RELIEFOS_DOOM_AUDIO_RATE * 8U)
#define RELIEFOS_DOOM_AUDIO_MUS_CHANNELS 16U
#define RELIEFOS_DOOM_AUDIO_MUS_TICKS_PER_TICK 4U
#define RELIEFOS_DOOM_AUDIO_MUS_VOICES 16U
#define RELIEFOS_DOOM_AUDIO_SAMPLE_CACHE 256U
#define RELIEFOS_DOOM_MIDI_MAX_TRACKS 32U

struct reliefos_doom_sample {
    int16_t *pcm;
    uint32_t frames;
    uint32_t source_rate;
};

struct reliefos_doom_channel {
    struct reliefos_doom_sample *sample;
    uint64_t position;
    uint64_t step;
    uint16_t volume;
    uint16_t separation;
    uint8_t active;
};

struct reliefos_doom_music_handle {
    const uint8_t *data;
    uint32_t length;
    struct doom_music_song *player_song;
};

struct reliefos_doom_music_voice {
    uint32_t phase;
    uint32_t step;
    uint16_t volume;
    uint8_t active;
    uint8_t percussion;
    uint8_t program;
    uint8_t channel;
    uint8_t note;
    uint8_t releasing;
    uint8_t envelope_stage;
    uint16_t envelope;
};

struct reliefos_doom_midi_track {
    uint32_t start;
    uint32_t end;
    uint32_t position;
    uint32_t delay;
    uint8_t running_status;
    uint8_t ended;
};

struct reliefos_doom_music_state {
    struct reliefos_doom_music_handle *handle;
    uint32_t position;
    uint32_t score_start;
    uint32_t delay;
    uint8_t looping;
    uint8_t paused;
    uint8_t active;
    uint8_t volume;
    uint8_t format;
    uint16_t division;
    uint32_t tempo;
    uint64_t tick_remainder;
    uint8_t track_count;
    uint8_t programs[RELIEFOS_DOOM_AUDIO_MUS_CHANNELS];
    struct reliefos_doom_midi_track tracks[RELIEFOS_DOOM_MIDI_MAX_TRACKS];
    struct reliefos_doom_music_voice voices[RELIEFOS_DOOM_AUDIO_MUS_VOICES];
};

static struct doom_audio_output *reliefos_doom_output;
static uint8_t reliefos_doom_audio_available;
static uint8_t reliefos_doom_audio_open_attempted;
static uint8_t reliefos_doom_audio_reported;
static uint8_t reliefos_doom_missing_sfx_reported;
static uint8_t reliefos_doom_sound_initialized;
static uint8_t reliefos_doom_music_only;
static uint8_t reliefos_doom_use_sfx_prefix;
static uint8_t reliefos_doom_mix_reported;
static uint8_t reliefos_doom_music_reported;
static uint8_t reliefos_doom_song_reported;
static uint8_t reliefos_doom_submit_reported;
static uint8_t reliefos_doom_midi_note_reported;
static uint8_t reliefos_doom_midi_activity_reported;
static int16_t reliefos_doom_mix[RELIEFOS_DOOM_AUDIO_MAX_MIX_FRAMES * 2U];
static int32_t reliefos_doom_mix_left[RELIEFOS_DOOM_AUDIO_MAX_MIX_FRAMES];
static int32_t reliefos_doom_mix_right[RELIEFOS_DOOM_AUDIO_MAX_MIX_FRAMES];
static struct reliefos_doom_channel reliefos_doom_channels[RELIEFOS_DOOM_AUDIO_MIX_CHANNELS];
static struct reliefos_doom_sample *reliefos_doom_sample_cache[RELIEFOS_DOOM_AUDIO_SAMPLE_CACHE];
static uint32_t reliefos_doom_sample_cache_count;
static struct reliefos_doom_music_state reliefos_doom_music;
static struct doom_music_opl reliefos_doom_opl;
static uint8_t reliefos_doom_opl_ready;
static uint32_t reliefos_doom_last_audio_ms;
static uint32_t reliefos_doom_audio_frame_remainder;
static uint32_t reliefos_doom_music_starts;
static uint32_t reliefos_doom_sfx_starts;
static uint32_t reliefos_doom_attack_starts;
static uint32_t reliefos_doom_max_sfx_voices;

/* Kept for the shared DOOM configuration ABI; ReliefOS uses nearest-neighbour
 * conversion in this backend and never links libsamplerate. */
int use_libsamplerate;
float libsamplerate_scale = 0.65f;

static void reliefos_doom_audio_log_unavailable(void)
{
    if (!reliefos_doom_audio_reported) {
        printf("[doom] no PCM audio device; continuing silently\n");
        reliefos_doom_audio_reported = 1;
    }
}

static void reliefos_doom_audio_log_once(uint8_t *flag, const char *message)
{
    if (!*flag) {
        printf("%s\n", message);
        fflush(stdout);
        *flag = 1U;
    }
}

static int reliefos_doom_configure_audio(void)
{
    uint32_t rate = 0;
    if (reliefos_doom_output) {
        return 0;
    }
    reliefos_doom_audio_open_attempted = 1U;
    if (doom_audio_open(&reliefos_doom_output, "/dev/dsp", &rate) < 0 ||
        rate != RELIEFOS_DOOM_AUDIO_RATE) {
        doom_audio_close(reliefos_doom_output);
        reliefos_doom_output = NULL;
        return -1;
    }
    return 0;
}

static int16_t reliefos_doom_clamp16(int32_t value)
{
    if (value < -32768) {
        return -32768;
    }
    if (value > 32767) {
        return 32767;
    }
    return (int16_t)value;
}

static void reliefos_doom_sample_free(struct reliefos_doom_sample *sample)
{
    if (!sample) {
        return;
    }
    free(sample->pcm);
    free(sample);
}

static struct reliefos_doom_sample *reliefos_doom_sample_load(sfxinfo_t *sfx)
{
    uint8_t *data;
    uint32_t lump_length;
    uint32_t source_rate;
    uint32_t source_frames;
    uint32_t output_frames;
    struct reliefos_doom_sample *sample;
    uint32_t i;

    if (!sfx || sfx->lumpnum < 0) {
        return NULL;
    }
    data = (uint8_t *)W_CacheLumpNum((unsigned int)sfx->lumpnum, PU_STATIC);
    lump_length = (uint32_t)W_LumpLength((unsigned int)sfx->lumpnum);
    if (!data || lump_length < 40U || data[0] != 3U || data[1] != 0U) {
        if (data) {
            W_ReleaseLumpNum((unsigned int)sfx->lumpnum);
        }
        return NULL;
    }
    source_rate = (uint32_t)data[2] | ((uint32_t)data[3] << 8);
    source_frames = (uint32_t)data[4] |
                    ((uint32_t)data[5] << 8) |
                    ((uint32_t)data[6] << 16) |
                    ((uint32_t)data[7] << 24);
    if (!source_rate || source_frames <= 32U || source_frames > lump_length - 8U) {
        W_ReleaseLumpNum((unsigned int)sfx->lumpnum);
        return NULL;
    }
    source_frames -= 32U;
    if (source_frames > RELIEFOS_DOOM_AUDIO_MAX_SAMPLE_FRAMES) {
        source_frames = RELIEFOS_DOOM_AUDIO_MAX_SAMPLE_FRAMES;
    }
    output_frames = (uint32_t)(((uint64_t)source_frames * RELIEFOS_DOOM_AUDIO_RATE) /
                               source_rate);
    if (!output_frames || output_frames > RELIEFOS_DOOM_AUDIO_MAX_SAMPLE_FRAMES) {
        W_ReleaseLumpNum((unsigned int)sfx->lumpnum);
        return NULL;
    }
    sample = (struct reliefos_doom_sample *)calloc(1, sizeof(*sample));
    if (!sample) {
        W_ReleaseLumpNum((unsigned int)sfx->lumpnum);
        return NULL;
    }
    sample->pcm = (int16_t *)malloc((size_t)output_frames * sizeof(*sample->pcm));
    if (!sample->pcm) {
        free(sample);
        W_ReleaseLumpNum((unsigned int)sfx->lumpnum);
        return NULL;
    }
    sample->frames = output_frames;
    sample->source_rate = source_rate;
    /* DMX leaves a 16-byte lead-in and a trailing 16-byte guard area. */
    data += 24U;
    for (i = 0; i < output_frames; ++i) {
        uint32_t source = (uint32_t)(((uint64_t)i * source_frames) / output_frames);
        int32_t value = ((int32_t)data[source] - 128) * 256;
        sample->pcm[i] = (int16_t)value;
    }
    if (reliefos_doom_sample_cache_count < RELIEFOS_DOOM_AUDIO_SAMPLE_CACHE) {
        reliefos_doom_sample_cache[reliefos_doom_sample_cache_count++] = sample;
    }
    W_ReleaseLumpNum((unsigned int)sfx->lumpnum);
    return sample;
}

static void reliefos_doom_sfx_name(sfxinfo_t *sfx, char *name, size_t capacity)
{
    const char *source;
    if (sfx && sfx->link) {
        sfx = sfx->link;
    }
    source = sfx ? sfx->name : "";
    if (reliefos_doom_use_sfx_prefix) {
        snprintf(name, capacity, "ds%s", source);
    } else {
        snprintf(name, capacity, "%s", source);
    }
}

static int reliefos_doom_get_sfx_lump(sfxinfo_t *sfx)
{
    char name[16];
    int lump;
    reliefos_doom_sfx_name(sfx, name, sizeof(name));
    lump = W_CheckNumForName(name);
    if (lump < 0) {
        reliefos_doom_audio_log_once(&reliefos_doom_missing_sfx_reported,
                                   "[doom] missing SFX lump skipped");
    }
    return lump;
}

static boolean reliefos_doom_init_sound(boolean use_sfx_prefix)
{
    reliefos_doom_use_sfx_prefix = use_sfx_prefix ? 1U : 0U;
    reliefos_doom_sound_initialized = 1U;
    reliefos_doom_music_only = 0U;
    reliefos_doom_music_starts = 0U;
    reliefos_doom_sfx_starts = 0U;
    reliefos_doom_attack_starts = 0U;
    reliefos_doom_max_sfx_voices = 0U;
    /* Open on the first mixed block, after WAD/level setup, so the hardware
     * stream cannot underrun while Doom is still initializing. */
    reliefos_doom_audio_available = 0;
    reliefos_doom_audio_open_attempted = 0;
    return true;
}

/** @brief Report actual audio scenes and release cached voices/output. */
static void reliefos_doom_shutdown_sound(void)
{
    uint32_t i;
    printf("[doom-audio] scenes music_starts=%u sfx_starts=%u attack_starts=%u max_sfx_voices=%u\n",
           reliefos_doom_music_starts, reliefos_doom_sfx_starts,
           reliefos_doom_attack_starts, reliefos_doom_max_sfx_voices);
    for (i = 0; i < RELIEFOS_DOOM_AUDIO_MIX_CHANNELS; ++i) {
        reliefos_doom_channels[i].sample = NULL;
        reliefos_doom_channels[i].active = 0;
    }
    for (i = 0; i < reliefos_doom_sample_cache_count; ++i) {
        reliefos_doom_sample_free(reliefos_doom_sample_cache[i]);
        reliefos_doom_sample_cache[i] = NULL;
    }
    reliefos_doom_sample_cache_count = 0;
    reliefos_doom_sound_initialized = 0;
    reliefos_doom_audio_available = 0;
    reliefos_doom_audio_open_attempted = 0;
    doom_audio_close(reliefos_doom_output);
    reliefos_doom_output = NULL;
    reliefos_doom_opl_ready = 0;
    reliefos_doom_last_audio_ms = 0;
    reliefos_doom_audio_frame_remainder = 0;
}

static void reliefos_doom_cache_sounds(sfxinfo_t *sounds, int count)
{
    (void)sounds;
    (void)count;
}

static int reliefos_doom_start_sound(sfxinfo_t *sfx, int channel, int volume, int separation)
{
    struct reliefos_doom_sample *sample;
    struct reliefos_doom_channel *out;
    if (!sfx || channel < 0 || channel >= (int)RELIEFOS_DOOM_AUDIO_MIX_CHANNELS) {
        return 0;
    }
    if (sfx->lumpnum < 0) {
        sfx->lumpnum = reliefos_doom_get_sfx_lump(sfx);
    }
    sample = (struct reliefos_doom_sample *)sfx->driver_data;
    if (!sample) {
        sample = reliefos_doom_sample_load(sfx);
        if (!sample) {
            return 0;
        }
        sfx->driver_data = sample;
    }
    out = &reliefos_doom_channels[channel];
    out->sample = sample;
    out->position = 0;
    out->step = 1ULL << 32;
    out->volume = (uint16_t)(volume < 0 ? 0 : volume > 127 ? 127 : volume);
    out->separation = (uint16_t)(separation < 0 ? 0 : separation > 254 ? 254 : separation);
    out->active = 1;
    ++reliefos_doom_sfx_starts;
    if (!strcmp(sfx->name, "pistol") || !strcmp(sfx->name, "shotgn") ||
        !strcmp(sfx->name, "dshtgn") || !strcmp(sfx->name, "plasma") ||
        !strcmp(sfx->name, "bfg") || !strcmp(sfx->name, "rlaunc") ||
        !strcmp(sfx->name, "sawful")) ++reliefos_doom_attack_starts;
    uint32_t active = 0;
    for (uint32_t i = 0; i < RELIEFOS_DOOM_AUDIO_MIX_CHANNELS; ++i)
        active += reliefos_doom_channels[i].active != 0;
    if (active > reliefos_doom_max_sfx_voices)
        reliefos_doom_max_sfx_voices = active;
    return channel;
}

static void reliefos_doom_stop_sound(int channel)
{
    if (channel >= 0 && channel < (int)RELIEFOS_DOOM_AUDIO_MIX_CHANNELS) {
        reliefos_doom_channels[channel].active = 0;
    }
}

static boolean reliefos_doom_sound_playing(int channel)
{
    return channel >= 0 && channel < (int)RELIEFOS_DOOM_AUDIO_MIX_CHANNELS &&
           reliefos_doom_channels[channel].active;
}

static void reliefos_doom_update_sound_params(int channel, int volume, int separation)
{
    if (channel < 0 || channel >= (int)RELIEFOS_DOOM_AUDIO_MIX_CHANNELS) {
        return;
    }
    reliefos_doom_channels[channel].volume =
        (uint16_t)(volume < 0 ? 0 : volume > 127 ? 127 : volume);
    reliefos_doom_channels[channel].separation =
        (uint16_t)(separation < 0 ? 0 : separation > 254 ? 254 : separation);
}

static uint32_t reliefos_doom_note_step(uint8_t note)
{
    static const uint16_t octave4[12] = {
        262U, 277U, 294U, 311U, 330U, 349U, 370U, 392U, 415U, 440U, 466U, 494U
    };
    uint32_t frequency = octave4[note % 12U];
    int octave = (int)(note / 12U) - 4;
    if (octave > 0) {
        while (octave--) frequency <<= 1;
    } else {
        while (octave++) frequency >>= 1;
    }
    if (!frequency) frequency = 1;
    return (uint32_t)(((uint64_t)frequency << 32) / RELIEFOS_DOOM_AUDIO_RATE);
}

static int32_t reliefos_doom_wave(uint32_t phase, uint8_t percussion, uint8_t program)
{
    uint32_t value;
    if (percussion) {
        value = phase * 1103515245U + 12345U;
        return (int32_t)((value >> 16) & 0xffffU) - 32768;
    }
    if ((program % 3U) == 1U) {
        value = phase < 0x80000000U ? phase : 0xffffffffU - phase;
        return (int32_t)(value >> 15) - 16384;
    }
    if ((program % 3U) == 2U) {
        return phase < 0x80000000U ? 24000 : -24000;
    }
    if (phase & 0x80000000U) {
        return 32767 - (int32_t)((phase - 0x80000000U) >> 15);
    }
    return (int32_t)(phase >> 15) - 32768;
}

static void reliefos_doom_music_stop_voices(void)
{
    memset(reliefos_doom_music.voices, 0, sizeof(reliefos_doom_music.voices));
}

static int reliefos_doom_music_valid(const struct reliefos_doom_music_handle *handle)
{
    if (!handle || !handle->data || handle->length < 4U) {
        return 0;
    }
    return (handle->length >= 16U && memcmp(handle->data, "MUS\x1a", 4) == 0) ||
           (handle->length >= 14U && memcmp(handle->data, "MThd", 4) == 0);
}

static uint16_t reliefos_doom_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t reliefos_doom_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static int reliefos_doom_midi_vlq(const uint8_t *data, uint32_t end,
                                uint32_t *position, uint32_t *value)
{
    uint32_t result = 0;
    uint32_t count = 0;
    uint8_t byte;
    if (!data || !position || !value) return -1;
    do {
        if (*position >= end || count++ == 4U) return -1;
        byte = data[(*position)++];
        result = (result << 7) | (byte & 0x7fU);
    } while (byte & 0x80U);
    *value = result;
    return 0;
}

static int reliefos_doom_midi_prepare_track(struct reliefos_doom_music_state *music,
                                          struct reliefos_doom_midi_track *track)
{
    uint32_t delta;
    track->position = track->start;
    track->running_status = 0;
    track->ended = 0;
    if (reliefos_doom_midi_vlq(music->handle->data, track->end,
                             &track->position, &delta) < 0) return -1;
    track->delay = delta;
    return 0;
}

static int reliefos_doom_midi_next_delta(struct reliefos_doom_music_state *music,
                                       struct reliefos_doom_midi_track *track)
{
    uint32_t delta;
    if (track->position >= track->end ||
        reliefos_doom_midi_vlq(music->handle->data, track->end,
                             &track->position, &delta) < 0) {
        track->ended = 1;
        return -1;
    }
    track->delay = delta;
    return 0;
}

static void reliefos_doom_midi_note_off(uint8_t channel, uint8_t note)
{
    uint32_t index;
    for (index = 0; index < RELIEFOS_DOOM_AUDIO_MUS_VOICES; ++index) {
        struct reliefos_doom_music_voice *voice = &reliefos_doom_music.voices[index];
        if (voice->active && voice->channel == channel && voice->note == note) {
            voice->releasing = 1U;
            voice->envelope_stage = 2U;
        }
    }
}

static void reliefos_doom_midi_note_on(uint8_t channel, uint8_t note, uint8_t velocity)
{
    struct reliefos_doom_music_voice *voice = NULL;
    uint32_t index;
    if (!velocity) {
        reliefos_doom_midi_note_off(channel, note);
        return;
    }
    for (index = 0; index < RELIEFOS_DOOM_AUDIO_MUS_VOICES; ++index) {
        if (reliefos_doom_music.voices[index].active &&
            reliefos_doom_music.voices[index].channel == channel &&
            reliefos_doom_music.voices[index].note == note) {
            voice = &reliefos_doom_music.voices[index];
            break;
        }
    }
    if (!voice) {
        for (index = 0; index < RELIEFOS_DOOM_AUDIO_MUS_VOICES; ++index) {
            if (!reliefos_doom_music.voices[index].active) {
                voice = &reliefos_doom_music.voices[index];
                break;
            }
        }
    }
    if (!voice) {
        /* Steal the first releasing voice, then the oldest slot. */
        for (index = 0; index < RELIEFOS_DOOM_AUDIO_MUS_VOICES; ++index) {
            if (reliefos_doom_music.voices[index].releasing) {
                voice = &reliefos_doom_music.voices[index];
                break;
            }
        }
    }
    if (!voice) {
        voice = &reliefos_doom_music.voices[channel % RELIEFOS_DOOM_AUDIO_MUS_VOICES];
    }
    voice->step = reliefos_doom_note_step(note);
    voice->phase = 0;
    voice->volume = velocity;
    voice->channel = channel;
    voice->note = note;
    voice->program = reliefos_doom_music.programs[channel];
    voice->envelope = 0U;
    voice->releasing = 0;
    voice->envelope_stage = 0U;
    voice->active = 1;
    voice->percussion = channel == 9U;
    reliefos_doom_audio_log_once(&reliefos_doom_midi_note_reported,
                               "[doom] MIDI first note-on");
}

static int reliefos_doom_midi_event(struct reliefos_doom_music_state *music,
                                  struct reliefos_doom_midi_track *track)
{
    const uint8_t *data = music->handle->data;
    uint8_t status;
    uint8_t channel;
    uint8_t type;
    uint32_t p = track->position;
    if (p >= track->end) return -1;
    status = data[p++];
    if (status < 0x80U) {
        if (!track->running_status) return -1;
        status = track->running_status;
        --p;
    } else if (status < 0xf0U) {
        track->running_status = status;
    }
    if (status == 0xffU) {
        uint8_t meta;
        uint32_t size;
        if (p >= track->end) return -1;
        meta = data[p++];
        if (reliefos_doom_midi_vlq(data, track->end, &p, &size) < 0 ||
            size > track->end - p) return -1;
        if (meta == 0x51U && size == 3U) {
            music->tempo = ((uint32_t)data[p] << 16) |
                           ((uint32_t)data[p + 1U] << 8) | data[p + 2U];
            if (!music->tempo) music->tempo = 500000U;
        }
        p += size;
        track->position = p;
        if (meta == 0x2fU) track->ended = 1U;
        return track->ended ? 1 : 0;
    }
    if (status == 0xf0U || status == 0xf7U) {
        uint32_t size;
        if (reliefos_doom_midi_vlq(data, track->end, &p, &size) < 0 ||
            size > track->end - p) return -1;
        track->position = p + size;
        return 0;
    }
    if (status >= 0xf1U && status <= 0xfeU) {
        /* System-common/realtime messages do not carry a MIDI channel.  We
         * do not synthesize them, but consume their exact payload so a clock
         * or tune-request event cannot desynchronise the following notes. */
        uint32_t payload = (status == 0xf1U || status == 0xf3U) ? 1U :
                           (status == 0xf2U ? 2U : 0U);
        if (payload > track->end - p) return -1;
        track->position = p + payload;
        return 0;
    }
    channel = status & 0x0fU;
    type = status & 0xf0U;
    if (channel >= RELIEFOS_DOOM_AUDIO_MUS_CHANNELS) return -1;
    if (type == 0x80U || type == 0x90U) {
        if (p + 1U >= track->end) return -1;
        if (type == 0x80U) reliefos_doom_midi_note_off(channel, data[p]);
        else reliefos_doom_midi_note_on(channel, data[p], data[p + 1U]);
        p += 2U;
    } else if (type == 0xc0U || type == 0xd0U) {
        if (p >= track->end) return -1;
        if (type == 0xc0U) {
            reliefos_doom_music.programs[channel] = data[p];
        }
        ++p;
    } else {
        if (p + 1U >= track->end) return -1;
        p += 2U;
    }
    track->position = p;
    return 0;
}

static void reliefos_doom_midi_process_ready(void)
{
    uint32_t index;
    for (index = 0; index < reliefos_doom_music.track_count; ++index) {
        struct reliefos_doom_midi_track *track = &reliefos_doom_music.tracks[index];
        uint32_t guard = 0;
        while (!track->ended && track->delay == 0U && guard++ < 128U) {
            int ret = reliefos_doom_midi_event(&reliefos_doom_music, track);
            if (ret < 0) {
                track->ended = 1U;
                break;
            }
            if (!track->ended) reliefos_doom_midi_next_delta(&reliefos_doom_music, track);
        }
    }
}

static int reliefos_doom_midi_reset(void)
{
    const uint8_t *data = reliefos_doom_music.handle->data;
    uint32_t header_size;
    uint16_t tracks;
    uint32_t position = 14U;
    uint32_t index;
    if (reliefos_doom_music.handle->length < 14U || memcmp(data, "MThd", 4) != 0)
        return -1;
    header_size = reliefos_doom_be32(data + 4U);
    if (header_size < 6U || header_size > reliefos_doom_music.handle->length - 8U)
        return -1;
    tracks = reliefos_doom_be16(data + 10U);
    reliefos_doom_music.division = reliefos_doom_be16(data + 12U);
    if (!tracks || !reliefos_doom_music.division ||
        (reliefos_doom_music.division & 0x8000U) ||
        tracks > RELIEFOS_DOOM_MIDI_MAX_TRACKS)
        return -1;
    position = 8U + header_size;
    reliefos_doom_music.track_count = 0;
    for (index = 0; index < tracks; ++index) {
        uint32_t size;
        struct reliefos_doom_midi_track *track;
        if (position + 8U > reliefos_doom_music.handle->length ||
            memcmp(data + position, "MTrk", 4) != 0) return -1;
        size = reliefos_doom_be32(data + position + 4U);
        position += 8U;
        if (size > reliefos_doom_music.handle->length - position) return -1;
        track = &reliefos_doom_music.tracks[reliefos_doom_music.track_count++];
        track->start = position;
        track->end = position + size;
        if (reliefos_doom_midi_prepare_track(&reliefos_doom_music, track) < 0) return -1;
        position += size;
    }
    reliefos_doom_music.tempo = 500000U;
    reliefos_doom_music.tick_remainder = 0;
    memset(reliefos_doom_music.programs, 0, sizeof(reliefos_doom_music.programs));
    reliefos_doom_music_stop_voices();
    printf("[doom] MIDI tracks=%u PPQN=%u\n", tracks,
           reliefos_doom_music.division);
    reliefos_doom_midi_process_ready();
    return 0;
}

static void reliefos_doom_music_reset(void)
{
    const uint8_t *data;
    if (reliefos_doom_music.handle && reliefos_doom_music.handle->length >= 4U &&
        memcmp(reliefos_doom_music.handle->data, "MThd", 4) == 0) {
        reliefos_doom_music.active = 0;
        reliefos_doom_music.format = 2U;
        if (reliefos_doom_midi_reset() < 0) {
            reliefos_doom_audio_log_once(&reliefos_doom_music_reported,
                                       "[doom] MIDI song rejected");
            return;
        }
        reliefos_doom_music.active = 1U;
        reliefos_doom_music.paused = 0;
        reliefos_doom_audio_log_once(&reliefos_doom_music_reported,
                                   "[doom] MIDI song accepted and synthesizer active");
        return;
    }
    if (!reliefos_doom_music_valid(reliefos_doom_music.handle)) {
        reliefos_doom_music.active = 0;
        reliefos_doom_audio_log_once(&reliefos_doom_music_reported,
                                   "[doom] MUS song rejected");
        return;
    }
    data = reliefos_doom_music.handle->data;
    reliefos_doom_music.score_start = (uint32_t)data[6] | ((uint32_t)data[7] << 8);
    if (reliefos_doom_music.score_start < 16U ||
        reliefos_doom_music.score_start >= reliefos_doom_music.handle->length) {
        reliefos_doom_music.active = 0;
        reliefos_doom_audio_log_once(&reliefos_doom_music_reported,
                                   "[doom] MUS score offset invalid");
        return;
    }
    reliefos_doom_music.position = reliefos_doom_music.score_start;
    reliefos_doom_music.delay = 0;
    reliefos_doom_music.tick_remainder = 0;
    reliefos_doom_music.active = 1;
    reliefos_doom_music.format = 1U;
    reliefos_doom_music.paused = 0;
    reliefos_doom_music.volume = 127U;
    reliefos_doom_music_stop_voices();
    reliefos_doom_audio_log_once(&reliefos_doom_music_reported,
                               "[doom] MUS song accepted and synthesizer active");
}

static void reliefos_doom_music_event(uint8_t descriptor)
{
    uint8_t channel = descriptor & 0x0fU;
    uint8_t type = (descriptor >> 4) & 0x07U;
    uint8_t note;
    uint8_t velocity;
    struct reliefos_doom_music_voice *voice;
    if (channel >= RELIEFOS_DOOM_AUDIO_MUS_CHANNELS ||
        !reliefos_doom_music.handle ||
        reliefos_doom_music.position >= reliefos_doom_music.handle->length) {
        return;
    }
    voice = &reliefos_doom_music.voices[channel];
    if (type == 0U) {
        /* Release-note carries the note number even though this lightweight
         * synth tracks one active voice per MUS channel. */
        if (reliefos_doom_music.position < reliefos_doom_music.handle->length) {
            ++reliefos_doom_music.position;
        }
        voice->active = 0;
    } else if (type == 1U) {
        note = reliefos_doom_music.handle->data[reliefos_doom_music.position++];
        velocity = (uint8_t)(voice->volume ? voice->volume : 100U);
        if (note & 0x80U) {
            note &= 0x7fU;
            if (reliefos_doom_music.position < reliefos_doom_music.handle->length) {
                velocity = reliefos_doom_music.handle->data[reliefos_doom_music.position++];
            }
        }
        voice->step = reliefos_doom_note_step(note);
        voice->volume = velocity;
        voice->phase = 0;
        voice->active = 1;
        voice->percussion = channel == 9U;
        voice->releasing = 0;
        voice->envelope_stage = 1U;
        voice->envelope = 65535U;
    } else if (type == 2U) {
        if (reliefos_doom_music.position < reliefos_doom_music.handle->length) {
            ++reliefos_doom_music.position;
        }
    } else if (type == 3U) {
        /* System event has one controller byte. */
        if (reliefos_doom_music.position < reliefos_doom_music.handle->length) {
            ++reliefos_doom_music.position;
        }
    } else if (type == 4U) {
        if (reliefos_doom_music.position + 1U < reliefos_doom_music.handle->length) {
            reliefos_doom_music.position += 2U;
        }
    }
}

static void reliefos_doom_music_next_group(void)
{
    uint8_t descriptor;
    uint32_t delay = 0;
    uint8_t value;
    if (!reliefos_doom_music.handle ||
        reliefos_doom_music.position >= reliefos_doom_music.handle->length) {
        if (reliefos_doom_music.looping) {
            reliefos_doom_music_reset();
        } else {
            reliefos_doom_music.active = 0;
        }
        return;
    }
    do {
        descriptor = reliefos_doom_music.handle->data[reliefos_doom_music.position++];
        reliefos_doom_music_event(descriptor);
    } while (!(descriptor & 0x80U) &&
             reliefos_doom_music.position < reliefos_doom_music.handle->length);
    if (descriptor & 0x80U) {
        do {
            if (reliefos_doom_music.position >= reliefos_doom_music.handle->length) {
                break;
            }
            value = reliefos_doom_music.handle->data[reliefos_doom_music.position++];
            delay = (delay << 7) | (value & 0x7fU);
        } while (value & 0x80U);
    }
    reliefos_doom_music.delay = delay;
}

static void reliefos_doom_music_advance(uint32_t frames)
{
    uint32_t i;
    if (!reliefos_doom_music.active || reliefos_doom_music.paused) {
        return;
    }
    if (reliefos_doom_music.format == 2U) {
        reliefos_doom_music.tick_remainder +=
            1000000ULL * (uint64_t)reliefos_doom_music.division * frames;
        for (;;) {
            uint64_t threshold = (uint64_t)(reliefos_doom_music.tempo ?
                                            reliefos_doom_music.tempo : 500000U) *
                                 RELIEFOS_DOOM_AUDIO_RATE;
            if (reliefos_doom_music.tick_remainder < threshold) {
                break;
            }
            reliefos_doom_music.tick_remainder -= threshold;
            reliefos_doom_midi_process_ready();
            for (i = 0; i < reliefos_doom_music.track_count; ++i) {
                if (reliefos_doom_music.tracks[i].delay) {
                    --reliefos_doom_music.tracks[i].delay;
                }
            }
        }
        reliefos_doom_midi_process_ready();
        for (i = 0; i < reliefos_doom_music.track_count; ++i) {
            if (!reliefos_doom_music.tracks[i].ended) break;
        }
        if (i == reliefos_doom_music.track_count) {
            if (reliefos_doom_music.looping) {
                reliefos_doom_music_reset();
            } else {
                reliefos_doom_music.active = 0;
            }
        }
        return;
    }
    reliefos_doom_music.tick_remainder +=
        (uint64_t)frames * RELIEFOS_DOOM_AUDIO_MUS_TICKS_PER_TICK *
        RELIEFOS_DOOM_AUDIO_TICK_HZ;
    while (reliefos_doom_music.tick_remainder >= RELIEFOS_DOOM_AUDIO_RATE) {
        reliefos_doom_music.tick_remainder -= RELIEFOS_DOOM_AUDIO_RATE;
        if (!reliefos_doom_music.delay) {
            reliefos_doom_music_next_group();
        }
        if (reliefos_doom_music.delay) {
            --reliefos_doom_music.delay;
        }
    }
}

static void reliefos_doom_mix_music(int32_t *left, int32_t *right, uint32_t frames)
{
    uint32_t i;
    uint32_t active_voices = 0;
    for (i = 0; i < RELIEFOS_DOOM_AUDIO_MUS_VOICES; ++i) {
        struct reliefos_doom_music_voice *voice = &reliefos_doom_music.voices[i];
        uint32_t frame;
        if (!voice->active) continue;
        ++active_voices;
        for (frame = 0; frame < frames; ++frame) {
            int32_t value = reliefos_doom_wave(voice->phase, voice->percussion,
                                              voice->program);
            value = (value * voice->volume * reliefos_doom_music.volume * voice->envelope) /
                    (127LL * 127LL * 3LL * 65535LL);
            left[frame] += value;
            right[frame] += value;
            voice->phase += voice->step;
            if (voice->envelope_stage == 0U) {
                if (voice->envelope < 62000U) voice->envelope += 3500U;
                else {
                    voice->envelope = 65535U;
                    voice->envelope_stage = 1U;
                }
            } else if (voice->releasing) {
                if (voice->envelope > 1200U) voice->envelope -= 1200U;
                else {
                    voice->envelope = 0;
                    voice->active = 0;
                }
            }
        }
    }
    if (active_voices && !reliefos_doom_midi_activity_reported) {
        printf("[doom] MIDI active voices=%u\n", active_voices);
        reliefos_doom_midi_activity_reported = 1U;
    }
}

static void reliefos_doom_mix_frames(uint32_t frames)
{
    int32_t *left = reliefos_doom_mix_left;
    int32_t *right = reliefos_doom_mix_right;
    uint32_t channel;
    uint32_t frame;
    int submit;
    uint8_t mixed = 0;
    memset(left, 0, sizeof(reliefos_doom_mix_left));
    memset(right, 0, sizeof(reliefos_doom_mix_right));
    reliefos_doom_music_advance(frames);
    for (channel = 0; channel < RELIEFOS_DOOM_AUDIO_MIX_CHANNELS; ++channel) {
        struct reliefos_doom_channel *source = &reliefos_doom_channels[channel];
        if (!source->active || !source->sample) continue;
        for (frame = 0; frame < frames; ++frame) {
            uint32_t index = (uint32_t)(source->position >> 32);
            uint32_t fraction = (uint32_t)source->position;
            int64_t value;
            int64_t next;
            if (index >= source->sample->frames) {
                source->active = 0;
                break;
            }
            next = index + 1U < source->sample->frames ?
                source->sample->pcm[index + 1U] : source->sample->pcm[index];
            value = source->sample->pcm[index] +
                    ((next - source->sample->pcm[index]) * fraction >> 32);
            value *= source->volume;
            left[frame] += (int32_t)(value * (254U - source->separation) /
                                     (127LL * 254LL));
            right[frame] += (int32_t)(value * source->separation /
                                      (127LL * 254LL));
            source->position += source->step;
        }
    }
    if (reliefos_doom_music.handle && reliefos_doom_music.handle->player_song) {
        doom_music_render(reliefos_doom_mix, frames, RELIEFOS_DOOM_AUDIO_RATE);
        for (frame = 0; frame < frames; ++frame) {
            left[frame] += reliefos_doom_mix[frame * 2U] / 2;
            right[frame] += reliefos_doom_mix[frame * 2U + 1U] / 2;
        }
    } else {
        reliefos_doom_mix_music(left, right, frames);
        if (reliefos_doom_opl_ready) {
            uint32_t opl_frame;
            doom_music_opl_generate(&reliefos_doom_opl, reliefos_doom_mix, frames);
            for (opl_frame = 0; opl_frame < frames; ++opl_frame) {
                left[opl_frame] += reliefos_doom_mix[opl_frame * 2U] / 2;
                right[opl_frame] += reliefos_doom_mix[opl_frame * 2U + 1U] / 2;
            }
        }
    }
    for (frame = 0; frame < frames; ++frame) {
        if (left[frame] || right[frame]) {
            mixed = 1U;
        }
        reliefos_doom_mix[frame * 2U] = reliefos_doom_clamp16(left[frame]);
        reliefos_doom_mix[frame * 2U + 1U] = reliefos_doom_clamp16(right[frame]);
    }
    if (mixed) {
        reliefos_doom_audio_log_once(&reliefos_doom_mix_reported,
                                   "[doom] first nonzero mixed PCM block");
    }
    if (reliefos_doom_audio_available) {
        submit = doom_audio_submit(reliefos_doom_output, reliefos_doom_mix, frames);
        if (submit != 0 && submit != -EAGAIN) {
            reliefos_doom_audio_available = 0;
            if (!reliefos_doom_submit_reported) {
                printf("[doom] PCM submission failed ret=%d\n", submit);
                reliefos_doom_submit_reported = 1U;
            }
            reliefos_doom_audio_log_unavailable();
        }
    }
}

static void reliefos_doom_update_sound(void)
{
    if (!reliefos_doom_audio_open_attempted && reliefos_doom_sound_initialized) {
        if (reliefos_doom_configure_audio() == 0) {
            reliefos_doom_audio_available = 1U;
            printf("[doom] PCM audio enabled: 48000 Hz stereo signed-16\n");
            fflush(stdout);
        } else {
            reliefos_doom_audio_log_unavailable();
        }
    }
    if (reliefos_doom_audio_available) {
        uint32_t frames = 0;
        int ret = doom_audio_flush(reliefos_doom_output);
        if (ret == 0 || ret == -EAGAIN)
            ret = doom_audio_writable_frames(reliefos_doom_output, &frames);
        if (ret) {
            reliefos_doom_audio_available = 0;
            printf("[doom] PCM space query failed ret=%d\n", ret);
            reliefos_doom_audio_log_unavailable();
            return;
        }
        /* PIT uptime can lose ticks under VM/CPU load.  Refill the hardware's
         * actual consumed frames instead of accumulating that clock drift.
         * Snapshot once so this update has bounded work even as DMA advances.
         * Include partial blocks: a sub-256-frame gap must not strand a
         * PREPARED ring below its full startup watermark. */
        while (frames && reliefos_doom_audio_available) {
            uint32_t block = frames > RELIEFOS_DOOM_AUDIO_MAX_MIX_FRAMES
                ? RELIEFOS_DOOM_AUDIO_MAX_MIX_FRAMES : frames;
            reliefos_doom_mix_frames(block);
            frames -= block;
        }
        return;
    }
    /* Preserve the silent backend's music progression without hardware. */
    uint32_t now = DG_GetTicksMs();
    uint32_t frames = RELIEFOS_DOOM_AUDIO_FRAMES * 2U;
    if (reliefos_doom_last_audio_ms) {
        uint32_t elapsed = (uint32_t)(now - reliefos_doom_last_audio_ms);
        if (elapsed > 100U) elapsed = 100U;
        uint64_t frame_time = (uint64_t)elapsed * RELIEFOS_DOOM_AUDIO_RATE +
                              reliefos_doom_audio_frame_remainder;
        frames = (uint32_t)(frame_time / 1000ULL);
        if (frames < 256U) return;
        reliefos_doom_audio_frame_remainder = (uint32_t)(frame_time % 1000ULL);
    }
    reliefos_doom_last_audio_ms = now;
    reliefos_doom_mix_frames(frames);
}

static boolean reliefos_doom_music_init(void)
{
    reliefos_doom_music.volume = 127U;
    if (doom_music_opl_init(&reliefos_doom_opl, RELIEFOS_DOOM_AUDIO_RATE) == 0) {
        reliefos_doom_opl_ready = 1U;
    }
    if (M_CheckParm("-nosound") > 0) {
        reliefos_doom_music_only = 0;
        return true;
    }
    if (!reliefos_doom_sound_initialized && !reliefos_doom_audio_available) {
        if (reliefos_doom_configure_audio() == 0) {
            reliefos_doom_audio_available = 1;
        } else {
            reliefos_doom_audio_log_unavailable();
        }
    }
    reliefos_doom_music_only = reliefos_doom_sound_initialized ? 0U : 1U;
    return true;
}

static void reliefos_doom_music_shutdown(void)
{
    reliefos_doom_music.active = 0;
    reliefos_doom_music_stop_voices();
    doom_music_stop();
    reliefos_doom_opl_ready = 0;
}

static void reliefos_doom_music_set_volume(int volume)
{
    reliefos_doom_music.volume = (uint8_t)(volume < 0 ? 0 : volume > 127 ? 127 : volume);
    doom_music_set_volume(reliefos_doom_music.volume);
}

static void reliefos_doom_music_pause(void)
{
    reliefos_doom_music.paused = 1;
    doom_music_pause(1);
}

static void reliefos_doom_music_resume(void)
{
    reliefos_doom_music.paused = 0;
    doom_music_pause(0);
}

static void *reliefos_doom_music_register(void *data, int length)
{
    struct reliefos_doom_music_handle *handle;
    int valid_type;
    if (!data || length <= 0) return NULL;
    handle = (struct reliefos_doom_music_handle *)calloc(1, sizeof(*handle));
    if (!handle) return NULL;
    handle->data = (const uint8_t *)data;
    handle->length = (uint32_t)length;
    handle->player_song = doom_music_register(handle->data, handle->length,
                                              RELIEFOS_DOOM_AUDIO_RATE);
    if (!handle->player_song) {
        free(handle);
        reliefos_doom_audio_log_once(&reliefos_doom_song_reported,
                                   "[doom] registered song rejected");
        return NULL;
    }
    {
        int genmidi_lump = W_CheckNumForName("GENMIDI");
        if (genmidi_lump >= 0) {
            const uint8_t *bank = (const uint8_t *)W_CacheLumpNum((unsigned int)genmidi_lump,
                                                                   PU_STATIC);
            uint32_t bank_length = (uint32_t)W_LumpLength((unsigned int)genmidi_lump);
            if (bank) {
                (void)doom_music_set_genmidi(bank, bank_length);
                W_ReleaseLumpNum((unsigned int)genmidi_lump);
            }
        }
    }
    valid_type = reliefos_doom_music_valid(handle) ?
                 (memcmp(handle->data, "MThd", 4) == 0 ? 2 : 1) : 0;
    if (!valid_type) {
        reliefos_doom_audio_log_once(&reliefos_doom_song_reported,
                                   "[doom] registered song rejected");
    } else if (valid_type == 2) {
        reliefos_doom_audio_log_once(&reliefos_doom_song_reported,
                                   "[doom] MIDI song registered");
    } else {
        reliefos_doom_audio_log_once(&reliefos_doom_song_reported,
                                   "[doom] MUS lump registered");
    }
    return handle;
}

static void reliefos_doom_music_unregister(void *opaque)
{
    struct reliefos_doom_music_handle *handle = (struct reliefos_doom_music_handle *)opaque;
    if (reliefos_doom_music.handle == handle) {
        reliefos_doom_music.active = 0;
        reliefos_doom_music.handle = NULL;
        doom_music_stop();
    }
    doom_music_unregister(handle->player_song);
    free(handle);
}

static void reliefos_doom_music_play(void *opaque, boolean looping)
{
    reliefos_doom_music.handle = (struct reliefos_doom_music_handle *)opaque;
    reliefos_doom_music.looping = looping ? 1U : 0U;
    if (!reliefos_doom_music.handle || !reliefos_doom_music.handle->player_song ||
        doom_music_play(reliefos_doom_music.handle->player_song, looping ? 1 : 0) < 0) {
        reliefos_doom_music_reset();
    } else {
        reliefos_doom_music.active = 1U;
        reliefos_doom_music.paused = 0U;
        ++reliefos_doom_music_starts;
    }
    reliefos_doom_audio_log_once(&reliefos_doom_song_reported,
                               "[doom] song playback requested");
}

static void reliefos_doom_music_stop(void)
{
    reliefos_doom_music.active = 0;
    reliefos_doom_music_stop_voices();
    doom_music_stop();
}

static boolean reliefos_doom_music_playing(void)
{
    if (reliefos_doom_music.handle && reliefos_doom_music.handle->player_song)
        return doom_music_playing() ? true : false;
    return reliefos_doom_music.active && !reliefos_doom_music.paused;
}

static void reliefos_doom_music_poll(void)
{
    if (reliefos_doom_music_only) {
        reliefos_doom_update_sound();
    }
}

static snddevice_t reliefos_doom_devices[] = {SNDDEVICE_SB, SNDDEVICE_GENMIDI};

sound_module_t DG_sound_module = {
    reliefos_doom_devices,
    (int)(sizeof(reliefos_doom_devices) / sizeof(reliefos_doom_devices[0])),
    reliefos_doom_init_sound,
    reliefos_doom_shutdown_sound,
    reliefos_doom_get_sfx_lump,
    reliefos_doom_update_sound,
    reliefos_doom_update_sound_params,
    reliefos_doom_start_sound,
    reliefos_doom_stop_sound,
    reliefos_doom_sound_playing,
    reliefos_doom_cache_sounds,
};

music_module_t DG_music_module = {
    reliefos_doom_devices,
    (int)(sizeof(reliefos_doom_devices) / sizeof(reliefos_doom_devices[0])),
    reliefos_doom_music_init,
    reliefos_doom_music_shutdown,
    reliefos_doom_music_set_volume,
    reliefos_doom_music_pause,
    reliefos_doom_music_resume,
    reliefos_doom_music_register,
    reliefos_doom_music_unregister,
    reliefos_doom_music_play,
    reliefos_doom_music_stop,
    reliefos_doom_music_playing,
    reliefos_doom_music_poll,
};
