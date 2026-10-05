#ifndef RELIEFOS_DOOM_MUSIC_OPL_H
#define RELIEFOS_DOOM_MUSIC_OPL_H

#include <stdint.h>

#include <opl3.h>

struct doom_music_opl {
    opl3_chip chip;
    uint32_t rate;
};

int doom_music_opl_init(struct doom_music_opl *opl, uint32_t rate);
void doom_music_opl_write(struct doom_music_opl *opl, uint16_t reg, uint8_t value);
void doom_music_opl_generate(struct doom_music_opl *opl, int16_t *stereo,
                             uint32_t frames);

#endif
