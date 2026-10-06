#include "sound_page.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef RELIEFOS_INSTALLER

int settings_sound_init(void) { return 0; }
void settings_sound_shutdown(void) {}
void settings_sound_poll(struct settings_sound_model *snapshot) { if (snapshot) memset(snapshot, 0, sizeof(*snapshot)); }
int settings_sound_set_volume(int64_t value) { (void)value; errno = ENOTSUP; return -1; }
int settings_sound_set_mute(uint8_t muted) { (void)muted; errno = ENOTSUP; return -1; }
int settings_sound_set_route(const char *label) { (void)label; errno = ENOTSUP; return -1; }
int settings_sound_set_source(const char *label) { (void)label; errno = ENOTSUP; return -1; }
void settings_sound_draw(struct reliefos_ui_surface *ui, const struct settings_sound_model *model) { (void)ui; (void)model; }
void settings_sound_click(int32_t x, int32_t y, const struct settings_sound_model *model) { (void)x; (void)y; (void)model; }

#else

struct sound_worker {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    struct settings_sound_model model;
    struct reliefos_audio_control *control;
    uint8_t stop;
    uint8_t started;
};
static struct sound_worker worker;

static void error_text(struct settings_sound_model *m, int error, const char *what)
{
    snprintf(m->error, sizeof(m->error), "%s: %s", what, strerror(error));
    m->connected = 0; m->loading = 0;
}
static int find_name(const struct settings_sound_model *m, const char *name)
{
    for (uint32_t i = 0; i < m->count; ++i)
        if (!strcmp(m->controls[i].name, name)) return (int)i;
    return -1;
}
static void refresh_locked(struct sound_worker *w)
{
    struct settings_sound_model *m = &w->model;
    uint32_t count = 0;
    m->loading = 1; m->error[0] = 0;
    if (!w->control) {
        int error = errno;
        if (reliefos_audio_control_open(m->card, &w->control)) { error = errno; error_text(m, error, "open audio"); return; }
    }
    if (reliefos_audio_control_list(w->control, m->controls, 64, &count)) {
        int error = errno; error_text(m, error, "list controls"); return;
    }
    m->count = count; m->connected = 1; m->loading = 0;
    int volume = find_name(m, "Playback Volume");
    if (volume < 0) volume = find_name(m, "Master Playback Volume");
    if (volume >= 0 && m->controls[volume].count <= 2) {
        m->volume_count = m->controls[volume].count;
        if (reliefos_audio_control_get(w->control, m->controls[volume].numid, m->volume, 2))
            error_text(m, errno, "read volume");
    }
    int mute = find_name(m, "Playback Mute");
    if (mute < 0) mute = find_name(m, "Master Playback Switch");
    if (mute >= 0) { int64_t value; if (!reliefos_audio_control_get(w->control, m->controls[mute].numid, &value, 1)) m->muted = value == 0; }
    int route = find_name(m, "Output Source");
    if (route >= 0) { int64_t value; if (!reliefos_audio_control_get(w->control, m->controls[route].numid, &value, 1) && !reliefos_audio_control_item_name(w->control, m->controls[route].numid, value, m->route, sizeof(m->route))) {} }
    int source = find_name(m, "Input Source");
    if (source >= 0) { int64_t value; if (!reliefos_audio_control_get(w->control, m->controls[source].numid, &value, 1) && !reliefos_audio_control_item_name(w->control, m->controls[source].numid, value, m->source, sizeof(m->source))) {} }
    ++m->generation;
}
static void *sound_worker_main(void *context)
{
    struct sound_worker *w = context;
    pthread_mutex_lock(&w->lock); refresh_locked(w); pthread_mutex_unlock(&w->lock);
    while (1) {
        uint32_t id, mask;
        pthread_mutex_lock(&w->lock); int done = w->stop; struct reliefos_audio_control *c = w->control; pthread_mutex_unlock(&w->lock);
        if (done) break;
        if (!c) {
            struct timespec delay = { .tv_sec = 0, .tv_nsec = 50000000L };
            nanosleep(&delay, NULL);
            continue;
        }
        int result = reliefos_audio_control_wait(c, 100, &id, &mask);
        pthread_mutex_lock(&w->lock);
        if (result > 0 || (result < 0 && errno == ENODEV)) refresh_locked(w);
        pthread_mutex_unlock(&w->lock);
    }
    return NULL;
}
int settings_sound_init(void)
{
    memset(&worker, 0, sizeof(worker)); worker.model.card = 0; worker.model.loading = 1;
    if (pthread_mutex_init(&worker.lock, NULL) || pthread_cond_init(&worker.wake, NULL)) { errno = ENOMEM; return -1; }
    worker.started = pthread_create(&worker.thread, NULL, sound_worker_main, &worker) == 0;
    if (!worker.started) { pthread_cond_destroy(&worker.wake); pthread_mutex_destroy(&worker.lock); errno = EAGAIN; return -1; }
    return 0;
}
void settings_sound_shutdown(void)
{
    if (!worker.started) return;
    pthread_mutex_lock(&worker.lock); worker.stop = 1; pthread_mutex_unlock(&worker.lock);
    pthread_join(worker.thread, NULL);
    pthread_mutex_lock(&worker.lock); if (worker.control) reliefos_audio_control_close(worker.control); worker.control = NULL; pthread_mutex_unlock(&worker.lock);
    pthread_cond_destroy(&worker.wake); pthread_mutex_destroy(&worker.lock); worker.started = 0;
}
void settings_sound_poll(struct settings_sound_model *snapshot)
{
    if (!snapshot || !worker.started) return;
    pthread_mutex_lock(&worker.lock); *snapshot = worker.model; pthread_mutex_unlock(&worker.lock);
}
static int submit_value(const char *name, const int64_t *values, uint32_t count)
{
    int result = -1; pthread_mutex_lock(&worker.lock);
    int index = find_name(&worker.model, name);
    if (index < 0) { errno = ENOTSUP; }
    else if (!worker.control) { errno = ENODEV; }
    else result = reliefos_audio_control_set(worker.control, worker.model.controls[index].numid, values, count);
    if (result) { int error = errno; error_text(&worker.model, error, "set audio"); }
    else worker.model.dirty = 1;
    pthread_mutex_unlock(&worker.lock); return result;
}
int settings_sound_set_volume(int64_t value)
{
    char control_name[64] = {0};
    pthread_mutex_lock(&worker.lock);
    int index = find_name(&worker.model, "Playback Volume");
    if (index < 0) index = find_name(&worker.model, "Master Playback Volume");
    uint32_t count = index >= 0 ? worker.model.controls[index].count : 0;
    if (index >= 0) snprintf(control_name, sizeof(control_name), "%s", worker.model.controls[index].name);
    pthread_mutex_unlock(&worker.lock);
    int64_t values[2] = {value, value};
    return count ? submit_value(control_name, values, 1) : (errno = ENOTSUP, -1);
}
int settings_sound_set_mute(uint8_t muted)
{ int64_t value = muted ? 0 : 1; int r = submit_value("Playback Mute", &value, 1); if (r && errno == ENOTSUP) r = submit_value("Master Playback Switch", &value, 1); return r; }
static int set_enum(const char *control, const char *label)
{
    pthread_mutex_lock(&worker.lock); int index = find_name(&worker.model, control); int64_t value = -1;
    if (index >= 0 && worker.control) { for (uint32_t i = 0; i < worker.model.controls[index].items; ++i) { char name[64]; if (!reliefos_audio_control_item_name(worker.control, worker.model.controls[index].numid, i, name, sizeof(name)) && !strcmp(name, label)) { value = i; break; } } }
    pthread_mutex_unlock(&worker.lock); if (value < 0) { errno = ENOTSUP; return -1; } return submit_value(control, &value, 1);
}
int settings_sound_set_route(const char *label) { return set_enum("Output Source", label); }
int settings_sound_set_source(const char *label) { return set_enum("Input Source", label); }
void settings_sound_draw(struct reliefos_ui_surface *ui, const struct settings_sound_model *m)
{
    reliefos_ui_text(ui, 38, 66, "Sound", RELIEFOS_UI_BLACK, RELIEFOS_UI_GRAY);
    if (m->loading) { reliefos_ui_text(ui, 38, 100, "Loading audio controls...", RELIEFOS_UI_DARK, RELIEFOS_UI_GRAY); return; }
    if (!m->connected) { reliefos_ui_text(ui, 38, 100, m->error[0] ? m->error : "No audio device", RELIEFOS_UI_DARK, RELIEFOS_UI_GRAY); return; }
    char text[128]; snprintf(text, sizeof(text), "Card %u  %s", m->card, m->route[0] ? m->route : "Output route unavailable"); reliefos_ui_text(ui, 38, 94, text, RELIEFOS_UI_BLACK, RELIEFOS_UI_GRAY);
    snprintf(text, sizeof(text), "Volume: %lld", (long long)m->volume[0]); reliefos_ui_text(ui, 38, 128, text, RELIEFOS_UI_BLACK, RELIEFOS_UI_GRAY);
    reliefos_ui_button(ui, 38, 152, 150, 24, m->muted ? "Unmute" : "Mute", 0);
    reliefos_ui_button(ui, 200, 152, 150, 24, "Volume +", 0);
    reliefos_ui_button(ui, 362, 152, 150, 24, "Volume -", 0);
    reliefos_ui_text(ui, 38, 196, m->source[0] ? m->source : "Capture source unavailable", RELIEFOS_UI_DARK, RELIEFOS_UI_GRAY);
    if (m->error[0]) reliefos_ui_text(ui, 38, 226, m->error, RELIEFOS_UI_DARK, RELIEFOS_UI_GRAY);
}
void settings_sound_click(int32_t x, int32_t y, const struct settings_sound_model *m)
{
    if (!m || y < 145 || y >= 184) return;
    if (x >= 38 && x < 188) (void)settings_sound_set_mute(!m->muted);
    else if (x >= 200 && x < 350) (void)settings_sound_set_volume(m->volume[0] + 1);
    else if (x >= 362 && x < 524) (void)settings_sound_set_volume(m->volume[0] - 1);
}

#endif
