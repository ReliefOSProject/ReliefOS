#ifndef RELIEFOS_XIAOBAI_MODEL_H
#define RELIEFOS_XIAOBAI_MODEL_H

/*
 * Toolkit-independent easter egg behavior: fitting the decoded PNG into
 * the canvas with a margin.  The Motif frontend maps its drawing area
 * onto this; the host suite runs it bare.
 */
#include <stdint.h>

struct xiaobai_fit {
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
};

void xiaobai_fit_rect(struct xiaobai_fit *out,
                      uint32_t canvas_w, uint32_t canvas_h,
                      uint32_t margin,
                      uint32_t img_w, uint32_t img_h);

#endif
