#ifndef RELIEFOS_FB_H
#define RELIEFOS_FB_H

#include <stdint.h>

#define RELIEFOS_FB_CAP_MODE_SET 0x0001U
#define RELIEFOS_FB_BACKEND_BOOT 0U
#define RELIEFOS_FB_BACKEND_BOCHS_VBE 1U
#define RELIEFOS_FB_BACKEND_VMWARE_SVGA 2U

/* /dev/fb0 hardware limits; fbdev's visible geometry is not a mode limit. */
#define RELIEFOS_FBIOGET_CAPABILITIES 0x46f0UL
#define RELIEFOS_FBIOUPDATE_REGION 0x46f1UL
#define RELIEFOS_FBIOBLIT 0x46f2UL

/* Atomic presentation by the active graphical controlling VT. Inactive callers
 * receive EAGAIN. pixels == 0 fills color; otherwise stride counts RGB32 pixels. */
struct reliefos_fb_present {
    uint32_t x, y, width, height, stride, color;
    uint64_t pixels;
};

struct reliefos_fb_capabilities {
    uint8_t bytes_per_pixel;
    uint8_t reserved;
    uint16_t capabilities;
    uint32_t max_width;
    uint32_t max_height;
    uint32_t max_bytes;
    uint32_t backend;
};

struct reliefos_fb_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t bpp;
};

struct reliefos_fb_mode {
    uint32_t width;
    uint32_t height;
};

struct reliefos_fb_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t color;
};

struct reliefos_fb_text {
    uint32_t x;
    uint32_t y;
    uint32_t fg;
    uint32_t bg;
    const char *text;
};

struct reliefos_fb_blit {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    const uint32_t *pixels;
};

int reliefos_fb_info(struct reliefos_fb_info *info);
int reliefos_fb_capabilities(struct reliefos_fb_capabilities *caps);
int reliefos_fb_set_mode(uint32_t width, uint32_t height);
int reliefos_fb_fill(uint32_t color);
int reliefos_fb_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color);
int reliefos_fb_text(uint32_t x, uint32_t y, const char *text, uint32_t fg, uint32_t bg);
uint32_t reliefos_fb_pixel(uint32_t x, uint32_t y);
int reliefos_fb_blit(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t stride, const uint32_t *pixels);

#endif
