/* Image viewer model: toolkit-independent decode, scaling and navigation. */
#ifndef IMAGEVIEW_MODEL_H
#define IMAGEVIEW_MODEL_H

#include <stdint.h>

#define IMAGEVIEW_MAX_PIXELS (1024U * 1024U)

enum imageview_zoom {
    IMAGEVIEW_ZOOM_FIT = 0,
    IMAGEVIEW_ZOOM_1X = 1,
    IMAGEVIEW_ZOOM_2X = 2,
};

/* Decode a 24/32-bit uncompressed BMP into malloc'd 0x00RRGGBB pixels. */
int imageview_bmp_decode(const uint8_t *data, uint32_t len, uint32_t **pixels,
                         uint32_t *width, uint32_t *height);

/* Extension filter for directory navigation: .bmp, .dib or .png (any case). */
int imageview_is_supported_path(const char *path);

/* Compute the destination rectangle for a zoom mode inside a box. */
void imageview_zoom_dims(uint32_t image_w, uint32_t image_h,
                         enum imageview_zoom zoom, uint32_t box_w,
                         uint32_t box_h, uint32_t *draw_w, uint32_t *draw_h);

/* Step through the sibling list with wrap-around; returns the new index. */
uint32_t imageview_next_index(uint32_t index, uint32_t count, int delta);

/* Build the "<w>x<h>  <zoom>  <i>/<n>" detail line. */
void imageview_format_detail(char *buf, uint32_t cap, uint32_t image_w,
                             uint32_t image_h, enum imageview_zoom zoom,
                             uint32_t sibling_index, uint32_t sibling_count);

#endif
