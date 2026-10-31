#ifndef RELIEFOS_FLTK_PIXELS_H
#define RELIEFOS_FLTK_PIXELS_H
#include <FL/fl_draw.H>
#include <stdint.h>

namespace reliefos_fltk {
struct pixel_view {
    const uint32_t *pixels;
    unsigned width, height, draw_width, draw_height;
};
inline void pixel_line(void *data, int x, int y, int count, unsigned char *output)
{
    const pixel_view *view = static_cast<const pixel_view *>(data);
    unsigned sy = static_cast<unsigned>((uint64_t)y * view->height / view->draw_height);
    for (int index = 0; index < count; ++index) {
        unsigned sx = static_cast<unsigned>((uint64_t)(x + index) * view->width / view->draw_width);
        uint32_t rgb = view->pixels[(uint64_t)sy * view->width + sx];
        *output++ = static_cast<unsigned char>(rgb >> 16);
        *output++ = static_cast<unsigned char>(rgb >> 8);
        *output++ = static_cast<unsigned char>(rgb);
    }
}
inline void draw_pixels(const uint32_t *pixels, unsigned width, unsigned height,
                        int x, int y, unsigned draw_width, unsigned draw_height)
{
    if (!pixels || !width || !height || !draw_width || !draw_height) return;
    pixel_view view = {pixels, width, height, draw_width, draw_height};
    fl_draw_image(pixel_line, &view, x, y, draw_width, draw_height, 3);
}
}
#endif
