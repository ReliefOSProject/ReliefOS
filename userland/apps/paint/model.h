/* Paint canvas model: toolkit-independent drawing surface and BMP codec. */
#ifndef PAINT_MODEL_H
#define PAINT_MODEL_H

#include <stdint.h>

#define PAINT_MAX_PIXELS (1024U * 1024U)

enum paint_tool {
    PAINT_TOOL_PENCIL = 0,
    PAINT_TOOL_BRUSH,
    PAINT_TOOL_ERASER,
};

/* Decode a 24/32-bit uncompressed BMP into malloc'd 0x00RRGGBB pixels. */
int paint_bmp_decode(const uint8_t *data, uint32_t len, uint32_t **pixels,
                     uint32_t *width, uint32_t *height);

/* Encode pixels as a 24-bit BMP in a malloc'd buffer the caller frees. */
int paint_bmp_encode(const uint32_t *pixels, uint32_t width, uint32_t height,
                     uint8_t **data, uint32_t *len);

/* Stamp one brush dab; the eraser paints white. */
void paint_draw_point(uint32_t *pixels, uint32_t width, uint32_t height,
                      uint32_t x, uint32_t y, uint32_t color,
                      uint32_t brush_size, enum paint_tool tool);

/* Interpolate a stroke segment so fast motion leaves no gaps. */
void paint_draw_line(uint32_t *pixels, uint32_t width, uint32_t height,
                     uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                     uint32_t color, uint32_t brush_size, enum paint_tool tool);

#endif
