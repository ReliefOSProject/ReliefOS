#include "music_opl.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

int doom_music_opl_init(struct doom_music_opl *opl, uint32_t rate)
{
    if (!opl || !rate) return -EINVAL;
    memset(opl, 0, sizeof(*opl));
    opl->rate = rate;
    OPL3_Reset(&opl->chip, rate);
    return 0;
}

void doom_music_opl_write(struct doom_music_opl *opl, uint16_t reg, uint8_t value)
{
    if (opl) OPL3_WriteRegBuffered(&opl->chip, reg, value);
}

void doom_music_opl_generate(struct doom_music_opl *opl, int16_t *stereo,
                             uint32_t frames)
{
    if (opl && stereo) OPL3_GenerateStream(&opl->chip, stereo, frames);
}
