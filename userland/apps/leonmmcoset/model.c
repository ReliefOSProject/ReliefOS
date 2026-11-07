#include "model.h"

void leonmmcoset_fit_rect(struct leonmmcoset_fit *out,
                          uint32_t canvas_w, uint32_t canvas_h,
                          uint32_t margin,
                          uint32_t img_w, uint32_t img_h)
{
    uint32_t avail_w;
    uint32_t avail_h;
    uint32_t w;
    uint32_t h;

    if (!out) return;
    out->x = 0;
    out->y = 0;
    out->w = 0;
    out->h = 0;
    if (!canvas_w || !canvas_h || !img_w || !img_h ||
        canvas_w <= margin * 2U || canvas_h <= margin * 2U) {
        return;
    }
    avail_w = canvas_w - margin * 2U;
    avail_h = canvas_h - margin * 2U;
    w = avail_w;
    h = (uint32_t)(((uint64_t)w * img_h) / img_w);
    if (h > avail_h) {
        h = avail_h;
        w = (uint32_t)(((uint64_t)h * img_w) / img_h);
    }
    if (!w || !h) {
        return;
    }
    out->w = w;
    out->h = h;
    out->x = (canvas_w - w) / 2U;
    out->y = (canvas_h - h) / 2U;
}
