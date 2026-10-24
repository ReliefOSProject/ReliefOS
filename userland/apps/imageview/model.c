#include "model.h"
#include <stdlib.h>
#include <string.h>

static uint32_t read_le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t read_le32s(const uint8_t *p)
{
    return (int32_t)read_le32(p);
}

int imageview_bmp_decode(const uint8_t *data, uint32_t len, uint32_t **pixels,
                         uint32_t *width, uint32_t *height)
{
    int32_t width_s;
    int32_t height_s;
    uint32_t w, h, offset, bpp, compression, stride;
    uint32_t *out;
    if (!data || !pixels || !width || !height || len < 54U ||
        data[0] != 'B' || data[1] != 'M') return -1;
    offset = read_le32(data + 10);
    if (read_le32(data + 14) < 40U || offset >= len) return -1;
    width_s = read_le32s(data + 18);
    height_s = read_le32s(data + 22);
    bpp = read_le16(data + 28);
    compression = read_le32(data + 30);
    if (width_s <= 0 || height_s == 0 || compression != 0U ||
        (bpp != 24U && bpp != 32U)) return -1;
    w = (uint32_t)width_s;
    /* Cast before negating: -INT32_MIN would overflow. */
    h = height_s < 0 ? (uint32_t)(-(int64_t)height_s) : (uint32_t)height_s;
    stride = ((w * bpp + 31U) / 32U) * 4U;
    if (!w || !h || w > 4096U || h > 4096U ||
        (uint64_t)w * h > IMAGEVIEW_MAX_PIXELS ||
        (uint64_t)offset + (uint64_t)stride * h > len) return -1;
    out = (uint32_t *)malloc((size_t)w * h * sizeof(uint32_t));
    if (!out) return -1;
    for (uint32_t y = 0; y < h; ++y) {
        uint32_t sy = height_s < 0 ? y : h - 1U - y;
        const uint8_t *row = data + offset + (uint64_t)sy * stride;
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t *px = row + x * (bpp / 8U);
            out[y * w + x] = ((uint32_t)px[2] << 16) |
                             ((uint32_t)px[1] << 8) | px[0];
        }
    }
    *pixels = out;
    *width = w;
    *height = h;
    return 0;
}

static uint32_t text_len(const char *text)
{
    uint32_t n = 0;
    while (text && text[n]) ++n;
    return n;
}

static char ascii_tolower(char ch)
{
    if (ch >= 'A' && ch <= 'Z') return (char)(ch - 'A' + 'a');
    return ch;
}

int imageview_is_supported_path(const char *path)
{
    static const char *suffixes[3] = {".bmp", ".dib", ".png"};
    uint32_t path_len = text_len(path);
    if (!path || !path_len) return 0;
    for (uint32_t i = 0; i < 3; ++i) {
        uint32_t suffix_len = text_len(suffixes[i]);
        if (suffix_len > path_len) continue;
        uint32_t j = 0;
        while (j < suffix_len &&
               ascii_tolower(path[path_len - suffix_len + j]) == suffixes[i][j]) {
            ++j;
        }
        if (j == suffix_len) return 1;
    }
    return 0;
}

void imageview_zoom_dims(uint32_t image_w, uint32_t image_h,
                         enum imageview_zoom zoom, uint32_t box_w,
                         uint32_t box_h, uint32_t *draw_w, uint32_t *draw_h)
{
    uint32_t w, h;
    if (!image_w || !image_h || !draw_w || !draw_h) return;
    if (zoom > IMAGEVIEW_ZOOM_2X) zoom = IMAGEVIEW_ZOOM_FIT;
    if (zoom == IMAGEVIEW_ZOOM_FIT) {
        w = box_w;
        h = (uint32_t)((uint64_t)w * image_h / image_w);
        if (h > box_h) {
            h = box_h;
            w = (uint32_t)((uint64_t)h * image_w / image_h);
        }
    } else {
        uint32_t scale = zoom == IMAGEVIEW_ZOOM_2X ? 2U : 1U;
        w = image_w * scale;
        h = image_h * scale;
    }
    if (!w) w = 1U;
    if (!h) h = 1U;
    *draw_w = w;
    *draw_h = h;
}

uint32_t imageview_next_index(uint32_t index, uint32_t count, int delta)
{
    if (count <= 1U) return 0;
    if (delta < 0) return index == 0 ? count - 1U : index - 1U;
    return (index + 1U) % count;
}

static void append_char(char *buf, uint32_t *pos, uint32_t cap, char ch)
{
    if (*pos + 1U < cap) buf[(*pos)++] = ch;
}

static void append_u32(char *buf, uint32_t *pos, uint32_t cap, uint32_t value)
{
    char tmp[12];
    uint32_t n = 0;
    if (value == 0) {
        append_char(buf, pos, cap, '0');
        return;
    }
    while (value && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (n) append_char(buf, pos, cap, tmp[--n]);
}

void imageview_format_detail(char *buf, uint32_t cap, uint32_t image_w,
                             uint32_t image_h, enum imageview_zoom zoom,
                             uint32_t sibling_index, uint32_t sibling_count)
{
    uint32_t pos = 0;
    static const char *zoom_names[3] = {"Fit", "1x", "2x"};
    if (!buf || !cap) return;
    buf[0] = 0;
    if (zoom > IMAGEVIEW_ZOOM_2X) zoom = IMAGEVIEW_ZOOM_FIT;
    if (!image_w || !image_h) return;
    append_u32(buf, &pos, cap, image_w);
    append_char(buf, &pos, cap, 'x');
    append_u32(buf, &pos, cap, image_h);
    append_char(buf, &pos, cap, ' ');
    append_char(buf, &pos, cap, ' ');
    for (const char *s = zoom_names[zoom]; *s; ++s) append_char(buf, &pos, cap, *s);
    if (sibling_count) {
        append_char(buf, &pos, cap, ' ');
        append_char(buf, &pos, cap, ' ');
        append_u32(buf, &pos, cap, sibling_index + 1U);
        append_char(buf, &pos, cap, '/');
        append_u32(buf, &pos, cap, sibling_count);
    }
    buf[pos] = 0;
}
