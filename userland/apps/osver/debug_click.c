#include "debug_click.h"

int osver_debug_click(struct osver_debug_click *state, uint32_t now_ms,
                      int inside_logo)
{
    if (!inside_logo) {
        state->count = 0;
        return 0;
    }
    if (!state->count || now_ms - state->window_start_ms > 2000U) {
        state->count = 0;
        state->window_start_ms = now_ms;
    }
    if (++state->count < 5U) return 0;
    state->count = 0;
    return 1;
}
