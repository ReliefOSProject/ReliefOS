/* fbdev helpers; independent of window systems. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <reliefos/device.h>
#include <reliefos/fb.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int fb_fd(void)
{
    static int fd = -1;
    if (fd < 0) fd = open(RELIEFOS_DEV_FB0, O_RDWR, 0);
    return fd;
}

static void *fb_mapping;
static size_t fb_mapping_bytes;
static struct reliefos_fb_info fb_mapped_info;

static void fb_unmap(void)
{
    if (fb_mapping) munmap(fb_mapping, fb_mapping_bytes);
    fb_mapping = 0;
    fb_mapping_bytes = 0;
}

static void *fb_map(struct reliefos_fb_info *info)
{
    if (reliefos_fb_info(info) < 0) return 0;
    if (fb_mapping &&
        (info->width != fb_mapped_info.width || info->height != fb_mapped_info.height ||
         info->pitch != fb_mapped_info.pitch || info->bpp != fb_mapped_info.bpp))
        fb_unmap();
    if (fb_mapping) return fb_mapping;
    fb_mapping_bytes = (size_t)info->pitch * info->height;
    fb_mapping = mmap(0, fb_mapping_bytes,
                          PROT_READ, MAP_SHARED, fb_fd(), 0);
    /* mmap failure reports MAP_FAILED ((void*)-1), which is nonzero: a bare
     * truthiness check would hand memcpy an invalid destination. */
    if (fb_mapping == MAP_FAILED) fb_mapping = 0;
    fb_mapped_info = *info;
    return fb_mapping;
}

int reliefos_fb_info(struct reliefos_fb_info *info)
{
    struct fb_var_screeninfo variable;
    struct fb_fix_screeninfo fixed;
    int fd = fb_fd();
    if (!info || fd < 0) return -1;
    memset(&variable, 0, sizeof(variable));
    memset(&fixed, 0, sizeof(fixed));
    if (ioctl(fd, FBIOGET_VSCREENINFO, &variable) < 0) return -1;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fixed) < 0) return -1;
    info->width = variable.xres;
    info->height = variable.yres;
    info->pitch = fixed.line_length;
    info->bpp = (uint8_t)variable.bits_per_pixel;
    return 0;
}

int reliefos_fb_capabilities(struct reliefos_fb_capabilities *caps)
{
    int fd = fb_fd();
    if (!caps || fd < 0) return -1;
    memset(caps, 0, sizeof(*caps));
    return ioctl(fd, RELIEFOS_FBIOGET_CAPABILITIES, caps);
}

int reliefos_fb_set_mode(uint32_t width, uint32_t height)
{
    struct fb_var_screeninfo variable;
    int fd = fb_fd();
    if (fd < 0 || !width || !height) return -1;
    memset(&variable, 0, sizeof(variable));
    if (ioctl(fd, FBIOGET_VSCREENINFO, &variable) < 0) return -1;
    variable.xres = width;
    variable.yres = height;
    variable.xres_virtual = width;
    variable.yres_virtual = height;
    if (ioctl(fd, FBIOPUT_VSCREENINFO, &variable) < 0) return -1;
    /* The visible span and the device's framebuffer offset may both change. */
    fb_unmap();
    return 0;
}

int reliefos_fb_fill(uint32_t color)
{
    struct reliefos_fb_info info;
    if (reliefos_fb_info(&info) < 0) return -1;
    return reliefos_fb_rect(0, 0, info.width, info.height, color);
}

int reliefos_fb_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                   uint32_t color)
{
    struct reliefos_fb_present update = { .x = x, .y = y, .width = width,
        .height = height, .color = color };
    return ioctl(fb_fd(), RELIEFOS_FBIOBLIT, &update);
}

int reliefos_fb_text(uint32_t x, uint32_t y, const char *text, uint32_t fg, uint32_t bg)
{
    static const uint8_t glyph[][7] = {
        {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
        {0x00,0x00,0x5f,0x00,0x00,0x00,0x00},
        {0x00,0x07,0x00,0x07,0x00,0x00,0x00},
        {0x14,0x7f,0x14,0x7f,0x14,0x00,0x00},
        {0x24,0x2a,0x7f,0x2a,0x12,0x00,0x00},
        {0x23,0x13,0x08,0x64,0x62,0x00,0x00},
        {0x36,0x49,0x55,0x22,0x50,0x00,0x00},
        {0x00,0x05,0x03,0x00,0x00,0x00,0x00},
    };
    if (!text) return -1;
    for (uint32_t i = 0; text[i]; ++i) {
        uint8_t ch = (uint8_t)text[i];
        const uint8_t *rows = ch < sizeof(glyph) / sizeof(glyph[0]) ? glyph[ch] : glyph[0];
        for (uint32_t row = 0; row < 7u; ++row) {
            for (uint32_t bit = 0; bit < 5u; ++bit) {
                uint32_t color = (rows[row] & (1u << bit)) ? fg : bg;
                (void)reliefos_fb_rect(x + i * 6u + bit, y + row, 1, 1, color);
            }
        }
    }
    return 0;
}

uint32_t reliefos_fb_pixel(uint32_t x, uint32_t y)
{
    struct reliefos_fb_info info;
    void *mapping = fb_map(&info);
    if (!mapping) return 0;
    if (x >= info.width || y >= info.height) return 0;
    return *(uint32_t *)((uint8_t *)mapping + y * info.pitch + x * 4u);
}

int reliefos_fb_blit(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                   uint32_t stride, const uint32_t *pixels)
{
    if (!pixels || stride < width) { errno = EINVAL; return -1; }
    struct reliefos_fb_present update = { .x = x, .y = y, .width = width,
        .height = height, .stride = stride, .pixels = (uintptr_t)pixels };
    /* Kernel validation and copying share the VT switch transaction. */
    return ioctl(fb_fd(), RELIEFOS_FBIOBLIT, &update);
}

extern __typeof__(reliefos_fb_blit) leonos_fb_blit __attribute__((alias("reliefos_fb_blit")));
extern __typeof__(reliefos_fb_capabilities) leonos_fb_capabilities __attribute__((alias("reliefos_fb_capabilities")));
extern __typeof__(reliefos_fb_fill) leonos_fb_fill __attribute__((alias("reliefos_fb_fill")));
extern __typeof__(reliefos_fb_info) leonos_fb_info __attribute__((alias("reliefos_fb_info")));
extern __typeof__(reliefos_fb_pixel) leonos_fb_pixel __attribute__((alias("reliefos_fb_pixel")));
extern __typeof__(reliefos_fb_rect) leonos_fb_rect __attribute__((alias("reliefos_fb_rect")));
extern __typeof__(reliefos_fb_set_mode) leonos_fb_set_mode __attribute__((alias("reliefos_fb_set_mode")));
extern __typeof__(reliefos_fb_text) leonos_fb_text __attribute__((alias("reliefos_fb_text")));
