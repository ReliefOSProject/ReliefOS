#include <reliefos/fs.h>
#include <reliefos/gui.h>
#include <libintl.h>
#include <locale.h>
#include <reliefos/layout.h>
#include <reliefos/stdio.h>
#include <reliefos/syscall.h>
#include <reliefos/ui.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/soundcard.h>
#include <string.h>

#define T(s) gettext(s)

#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include <minimp3.h>

#define MP3PLAY_W 480U
#define MP3PLAY_H 208U
#define MP3PLAY_INPUT_BYTES 16384U
#define MP3PLAY_DECODE_LOOKAHEAD 4096U
#define MP3PLAY_OPEN_X 16U
#define MP3PLAY_STOP_X 112U
#define MP3PLAY_BUTTON_Y 8U
#define MP3PLAY_BUTTON_W 80U

struct mp3_player {
    mp3dec_t decoder;
    char path[RELIEFOS_FS_PATH_LEN];
    char status[96];
    char detail[96];
    uint8_t input[MP3PLAY_INPUT_BYTES];
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    uint32_t input_len;
    uint32_t progress;
    uint32_t sample_rate;
    uint32_t configured;
    uint32_t playing;
    uint32_t eof;
    uint32_t decoded_any;
    uint64_t file_size;
    uint64_t read_bytes;
    int fd;
    int dsp_fd;
};

static uint32_t pixels[MP3PLAY_W * MP3PLAY_H];

static void copy_text(char *out, uint32_t capacity, const char *text)
{
    uint32_t pos = 0;
    if (!out || !capacity) {
        return;
    }
    while (text && text[pos] && pos + 1U < capacity) {
        out[pos] = text[pos];
        ++pos;
    }
    out[pos] = 0;
}

static void append_text(char *out, uint32_t *pos, uint32_t capacity, const char *text)
{
    if (!out || !pos || !capacity) {
        return;
    }
    while (text && *text && *pos + 1U < capacity) {
        out[(*pos)++] = *text++;
    }
    out[*pos] = 0;
}

static void append_u32(char *out, uint32_t *pos, uint32_t capacity, uint32_t value)
{
    char digits[10];
    uint32_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value && count < sizeof(digits));
    while (count) {
        char digit[2];
        digit[0] = digits[--count];
        digit[1] = 0;
        append_text(out, pos, capacity, digit);
    }
}

static int hit_rect(int32_t px, int32_t py, uint32_t x, uint32_t y,
                    uint32_t w, uint32_t h)
{
    return px >= (int32_t)x && py >= (int32_t)y &&
           px < (int32_t)(x + w) && py < (int32_t)(y + h);
}

static void player_close_file(struct mp3_player *player)
{
    if (player->fd >= 0) {
        close(player->fd);
        player->fd = -1;
    }
    player->input_len = 0;
    player->eof = 1;
    player->playing = 0;
}

static void player_stop(struct mp3_player *player, const char *status)
{
    player_close_file(player);
    player->configured = 0;
    if (status) {
        copy_text(player->status, sizeof(player->status), status);
    }
}

static void player_set_detail(struct mp3_player *player,
                              const mp3dec_frame_info_t *info)
{
    uint32_t pos = 0;
    player->detail[0] = 0;
    append_u32(player->detail, &pos, sizeof(player->detail), (uint32_t)info->hz);
    append_text(player->detail, &pos, sizeof(player->detail), " Hz, ");
    append_u32(player->detail, &pos, sizeof(player->detail), (uint32_t)info->bitrate_kbps);
    append_text(player->detail, &pos, sizeof(player->detail), " kbps, ");
    append_text(player->detail, &pos, sizeof(player->detail),
                info->channels == 1 ? "mono source -> stereo PCM" : "stereo PCM");
}

static int player_update_progress(struct mp3_player *player)
{
    uint32_t progress;
    uint64_t consumed;
    if (!player->file_size) {
        return 0;
    }
    consumed = player->read_bytes - player->input_len;
    progress = (uint32_t)(consumed * 100U / player->file_size);
    if (progress > 100U) {
        progress = 100U;
    }
    if (progress == player->progress) {
        return 0;
    }
    player->progress = progress;
    return 1;
}

static void player_finish(struct mp3_player *player, const char *status)
{
    player_close_file(player);
    player->configured = 0;
    player->progress = 100U;
    copy_text(player->status, sizeof(player->status), status);
}

static int player_read_more(struct mp3_player *player)
{
    long got;
    uint32_t space;
    if (player->eof || player->input_len >= MP3PLAY_INPUT_BYTES) {
        return 0;
    }
    space = MP3PLAY_INPUT_BYTES - player->input_len;
    got = read(player->fd, player->input + player->input_len, space);
    if (got < 0) {
        player_finish(player, "MP3 file read failed");
        return -1;
    }
    if (got == 0) {
        player->eof = 1;
        return 0;
    }
    player->input_len += (uint32_t)got;
    player->read_bytes += (uint32_t)got;
    return 1;
}

static void player_discard_input(struct mp3_player *player, uint32_t bytes)
{
    if (bytes > player->input_len) {
        bytes = player->input_len;
    }
    if (bytes < player->input_len) {
        memmove(player->input, player->input + bytes, player->input_len - bytes);
    }
    player->input_len -= bytes;
}

static int player_configure_audio(struct mp3_player *player,
                                  const mp3dec_frame_info_t *info)
{
    int format = AFMT_S16_LE;
    int channels = 2;
    int rate = info->hz;
    if (info->hz <= 0 || info->hz > 48000 || (info->channels != 1 && info->channels != 2)) {
        player_finish(player, "Unsupported MP3 audio format");
        return -1;
    }
    if (player->configured && player->sample_rate == (uint32_t)info->hz) {
        return 0;
    }
    if (player->dsp_fd < 0) {
        player->dsp_fd = open("/dev/dsp", O_WRONLY | O_NONBLOCK, 0);
    }
    if (player->dsp_fd < 0 ||
        ioctl(player->dsp_fd, SNDCTL_DSP_SETFMT, &format) < 0 ||
        format != AFMT_S16_LE ||
        ioctl(player->dsp_fd, SNDCTL_DSP_CHANNELS, &channels) < 0 ||
        channels != 2 || ioctl(player->dsp_fd, SNDCTL_DSP_SPEED, &rate) < 0 ||
        rate <= 0) {
        player_finish(player, "Audio device rejected 16-bit stereo PCM");
        return -1;
    }
    player->configured = 1;
    player->sample_rate = (uint32_t)rate;
    player_set_detail(player, info);
    copy_text(player->status, sizeof(player->status), "Playing");
    return 1;
}

static int player_write_frame(struct mp3_player *player, int samples,
                              const mp3dec_frame_info_t *info)
{
    uint32_t bytes;
    uint32_t offset = 0;
    uint32_t waits = 0;
    if (info->channels == 1) {
        for (int index = samples - 1; index >= 0; --index) {
            player->pcm[index * 2] = player->pcm[index];
            player->pcm[index * 2 + 1] = player->pcm[index];
        }
    }
    bytes = (uint32_t)samples * 4U;
    while (offset < bytes) {
        long written = write(player->dsp_fd, (const uint8_t *)player->pcm + offset,
                             bytes - offset);
        if (written > 0 && (uint32_t)written <= bytes - offset) {
            offset += (uint32_t)written;
            waits = 0;
            continue;
        }
        if ((written == 0 || written == -EAGAIN) && waits++ < 20U) {
            sleep_ms(5U);
            continue;
        }
        player_finish(player, "PCM playback failed");
        return -1;
    }
    return 0;
}

/* Decoding and audio writes are bounded to one MP3 frame so queued UI events
 * can stop playback before the next frame begins. */
static int player_decode_step(struct mp3_player *player)
{
    mp3dec_frame_info_t info = {0};
    int configure_result;
    int samples;
    int changed = 0;
    while (!player->eof && player->input_len < MP3PLAY_DECODE_LOOKAHEAD) {
        int read_result = player_read_more(player);
        if (read_result < 0) {
            return 1;
        }
        if (!read_result) {
            break;
        }
    }
    if (!player->input_len && player->eof) {
        player_finish(player, player->decoded_any ? "Playback complete" : "No playable MP3 frames");
        return 1;
    }

    samples = mp3dec_decode_frame(&player->decoder, player->input,
                                  (int)player->input_len, player->pcm, &info);
    if (samples <= 0) {
        uint32_t discard = info.frame_bytes > 0 ? (uint32_t)info.frame_bytes : 0;
        if (discard >= player->input_len && !player->eof && player->input_len > 3U) {
            discard = player->input_len - 3U;
        }
        if (discard) {
            player_discard_input(player, discard);
            changed = player_update_progress(player);
        } else if (player->eof) {
            player_finish(player, player->decoded_any ? "Playback complete" : "No playable MP3 frames");
            return 1;
        } else if (player->input_len == MP3PLAY_INPUT_BYTES) {
            player_finish(player, "MP3 decoder made no progress");
            return 1;
        }
        return changed;
    }
    if (info.frame_bytes <= 0 || (uint32_t)info.frame_bytes > player->input_len ||
        samples > MINIMP3_MAX_SAMPLES_PER_FRAME / 2) {
        player_finish(player, "Invalid MP3 frame");
        return 1;
    }
    configure_result = player_configure_audio(player, &info);
    if (configure_result < 0) {
        return 1;
    }
    changed |= configure_result > 0;
    if (player_write_frame(player, samples, &info) < 0) {
        return 1;
    }
    player_discard_input(player, (uint32_t)info.frame_bytes);
    player->decoded_any = 1;
    return player_update_progress(player) || changed;
}

static int player_start(struct mp3_player *player, const char *path)
{
    struct reliefos_stat stat_info;
    int fd;
    player_stop(player, 0);
    copy_text(player->path, sizeof(player->path), path);
    fd = open(player->path, RELIEFOS_O_RDONLY, 0);
    if (fd < 0) {
        copy_text(player->status, sizeof(player->status), "Could not open MP3 file");
        return -1;
    }
    player->fd = fd;
    player->file_size = 0;
    if (reliefos_fstat_legacy(fd, &stat_info) == 0 && stat_info.type == RELIEFOS_FS_TYPE_FILE) {
        player->file_size = stat_info.size;
    }
    player->read_bytes = 0;
    player->progress = 0;
    player->decoded_any = 0;
    player->eof = 0;
    player->playing = 1;
    mp3dec_init(&player->decoder);
    copy_text(player->status, sizeof(player->status), "Decoding MP3...");
    copy_text(player->detail, sizeof(player->detail), "Waiting for an MPEG audio frame");
    return 0;
}

static int player_open_dialog(struct mp3_player *player)
{
    char path[RELIEFOS_FS_PATH_LEN];
    player_stop(player, "Playback stopped");
    path[0] = 0;
    if (reliefos_ui_show_open_dialog(T("Open MP3"), path, sizeof(path),
                                   T("MP3 audio (*.mp3)"),
                                   ".mp3") <= 0 || !path[0]) {
        if (!player->path[0]) {
            copy_text(player->status, sizeof(player->status), "Select an MP3 file to begin");
        }
        return 0;
    }
    return player_start(player, path) == 0;
}

static void present(int window_id, struct reliefos_ui_surface *ui,
                    const struct mp3_player *player)
{
    uint32_t stop_flags = player->playing ? 0U : RELIEFOS_UI_BUTTON_DISABLED;
    reliefos_ui_bind(ui, pixels, MP3PLAY_W, MP3PLAY_H, MP3PLAY_W);
    reliefos_ui_rect(ui, 0, 0, MP3PLAY_W, MP3PLAY_H, RELIEFOS_UI_WHITE);
    reliefos_ui_toolbar(ui, 0, 0, MP3PLAY_W, 40U);
    reliefos_ui_button(ui, MP3PLAY_OPEN_X, MP3PLAY_BUTTON_Y, MP3PLAY_BUTTON_W,
                     RELIEFOS_UI_BUTTON_H, "Open", 0);
    reliefos_ui_button(ui, MP3PLAY_STOP_X, MP3PLAY_BUTTON_Y, MP3PLAY_BUTTON_W,
                     RELIEFOS_UI_BUTTON_H, "Stop", stop_flags);
    reliefos_ui_panel(ui, 16U, 54U, MP3PLAY_W - 32U, 106U, RELIEFOS_UI_LIGHT);
    reliefos_ui_text(ui, 28U, 68U, "MP3 file", RELIEFOS_UI_DARK, RELIEFOS_UI_LIGHT);
    reliefos_ui_text_clipped(ui, 28U, 88U, MP3PLAY_W - 56U,
                           player->path[0] ? player->path : "No file selected",
                           RELIEFOS_UI_BLACK, RELIEFOS_UI_LIGHT);
    reliefos_ui_progress(ui, 28U, 112U, MP3PLAY_W - 56U, 18U,
                       player->progress, 100U);
    reliefos_ui_text_clipped(ui, 28U, 138U, MP3PLAY_W - 56U, player->detail,
                           RELIEFOS_UI_DARK, RELIEFOS_UI_LIGHT);
    reliefos_ui_statusbar(ui, MP3PLAY_H - 28U, 28U, player->status);
    reliefos_gui_present_window((uint32_t)window_id, MP3PLAY_W, MP3PLAY_H,
                              MP3PLAY_W, pixels);
}

static int handle_event(struct mp3_player *player,
                        const struct reliefos_gui_app_event *event, int *running)
{
    if (event->type == RELIEFOS_GUI_APP_EVENT_CLOSE ||
        (event->type == RELIEFOS_GUI_APP_EVENT_KEY_DOWN && event->pressed &&
         event->keycode == 1U)) {
        player_stop(player, "Playback stopped");
        *running = 0;
        return 0;
    }
    if (event->type == RELIEFOS_GUI_APP_EVENT_MOUSE_BUTTON && (event->buttons & 1U)) {
        if (hit_rect(event->x, event->y, MP3PLAY_OPEN_X, MP3PLAY_BUTTON_Y,
                     MP3PLAY_BUTTON_W, RELIEFOS_UI_BUTTON_H)) {
            player_open_dialog(player);
            return 1;
        }
        if (player->playing && hit_rect(event->x, event->y, MP3PLAY_STOP_X,
                                        MP3PLAY_BUTTON_Y, MP3PLAY_BUTTON_W,
                                        RELIEFOS_UI_BUTTON_H)) {
            player_stop(player, "Playback stopped");
            return 1;
        }
    }
    if (event->type == RELIEFOS_GUI_APP_EVENT_KEY_DOWN && event->pressed) {
        if (event->keycode == RELIEFOS_KEY_ENTER) {
            player_open_dialog(player);
            return 1;
        }
        if (event->keycode == RELIEFOS_KEY_SPACE && player->playing) {
            player_stop(player, "Playback stopped");
            return 1;
        }
    }
    return event->type == RELIEFOS_GUI_APP_EVENT_FOCUS ||
           event->type == RELIEFOS_GUI_APP_EVENT_THEME_CHANGED;
}

int main(int argc, char **argv, char **envp)
{
    setlocale(LC_ALL, "");
    bindtextdomain("mp3play", RELIEFOS_LAYOUT_LOCALE);
    textdomain("mp3play");
    struct reliefos_ui_surface ui;
    struct reliefos_gui_app_event event;
    struct mp3_player player = {.fd = -1, .dsp_fd = -1};
    int window_id;
    int running = 1;
    (void)envp;

    copy_text(player.status, sizeof(player.status), "Select an MP3 file to begin");
    copy_text(player.detail, sizeof(player.detail), "16-bit stereo PCM output");
    window_id = reliefos_gui_create_app_window_ex("MP3 Player", "MP3 audio playback",
                                                MP3PLAY_W, MP3PLAY_H,
                                                RELIEFOS_GUI_WINDOW_NO_RESIZE);
    if (window_id <= 0) {
        printf("[mp3play.elf] create window failed=%d\n", window_id);
        return 1;
    }

    if (argc > 1 && argv && argv[1] && argv[1][0]) {
        player_start(&player, argv[1]);
    } else {
        player_open_dialog(&player);
    }
    present(window_id, &ui, &player);

    while (running) {
        int changed = 0;
        event.window_id = (uint32_t)window_id;
        if (player.playing) {
            while (reliefos_gui_poll_app_event(&event) > 0) {
                changed |= handle_event(&player, &event, &running);
                if (!running || !player.playing) {
                    break;
                }
                event.window_id = (uint32_t)window_id;
            }
            if (running && player.playing) {
                changed |= player_decode_step(&player);
            }
        } else if (reliefos_gui_wait_app_event(&event, RELIEFOS_GUI_IDLE_WAIT_MS) > 0) {
            changed |= handle_event(&player, &event, &running);
        }
        if (running && changed) {
            present(window_id, &ui, &player);
        }
        if (running && !player.playing && !changed) {
            sleep_ms(10);
        }
    }

    player_stop(&player, 0);
    reliefos_gui_destroy_app_window((uint32_t)window_id);
    return 0;
}
