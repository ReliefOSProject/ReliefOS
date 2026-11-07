/* Audio pacing is shared by the FLTK frontend and the headless game mode. */
#include <stdint.h>
#include <reliefos/system.h>
#include <reliefos/syscall.h>
#include "doomgeneric.h"
#include "i_sound.h"

void doom_pump_events(void);
uint32_t DG_GetTicksMs(void)
{
    return (uint32_t)reliefos_uptime_ms();
}
void DG_SleepMs(uint32_t milliseconds)
{
    I_UpdateSound();
    doom_pump_events();
    sleep_ms(milliseconds);
    I_UpdateSound();
}
