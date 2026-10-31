#ifndef RELIEFOS_OSVER_DEBUG_CLICK_H
#define RELIEFOS_OSVER_DEBUG_CLICK_H

#include <stdint.h>

struct osver_debug_click {
    uint32_t count;
    uint32_t window_start_ms;
};

int osver_debug_click(struct osver_debug_click *state, uint32_t now_ms,
                      int inside_logo);

#endif
