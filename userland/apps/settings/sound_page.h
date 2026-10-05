#ifndef RELIEFOS_SETTINGS_SOUND_PAGE_H
#define RELIEFOS_SETTINGS_SOUND_PAGE_H

#include <stdint.h>
#include <reliefos/ui.h>
#include <reliefos/audio_control.h>

struct settings_sound_model {
    struct reliefos_audio_control_desc controls[64];
    uint32_t count;
    uint32_t card;
    uint32_t generation;
    int64_t volume[2];
    uint32_t volume_count;
    uint8_t muted;
    uint8_t loading;
    uint8_t connected;
    uint8_t dirty;
    char route[64];
    char source[64];
    char error[96];
};

int settings_sound_init(void);
void settings_sound_shutdown(void);
void settings_sound_poll(struct settings_sound_model *snapshot);
int settings_sound_set_volume(int64_t value);
int settings_sound_set_mute(uint8_t muted);
int settings_sound_set_route(const char *label);
int settings_sound_set_source(const char *label);
void settings_sound_draw(struct reliefos_ui_surface *ui,
                         const struct settings_sound_model *model);
void settings_sound_click(int32_t x, int32_t y,
                          const struct settings_sound_model *model);

#endif
