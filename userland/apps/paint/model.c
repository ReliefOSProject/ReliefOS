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

static void write_le16(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

int paint_bmp_decode(const uint8_t *data, uint32_t len, uint32_t **pixels,
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
    if (!w || !h || (uint64_t)w * h > PAINT_MAX_PIXELS ||
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

int paint_bmp_encode(const uint32_t *pixels, uint32_t width, uint32_t height,
                     uint8_t **data, uint32_t *len)
{
    uint32_t stride, file_size, pos = 54U;
    uint8_t *out;
    if (!pixels || !data || !len || !width || !height ||
        (uint64_t)width * height > PAINT_MAX_PIXELS) return -1;
    stride = ((width * 24U + 31U) / 32U) * 4U;
    file_size = 54U + stride * height;
    out = (uint8_t *)calloc(1, file_size);
    if (!out) return -1;
    out[0] = 'B';
    out[1] = 'M';
    write_le32(out + 2, file_size);
    write_le32(out + 10, 54U);
    write_le32(out + 14, 40U);
    write_le32(out + 18, width);
    write_le32(out + 22, height);
    write_le16(out + 26, 1U);
    write_le16(out + 28, 24U);
    write_le32(out + 34, stride * height);
    for (uint32_t y = 0; y < height; ++y) {
        uint32_t sy = height - 1U - y;
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t pixel = pixels[sy * width + x];
            out[pos + x * 3U] = (uint8_t)pixel;
            out[pos + x * 3U + 1U] = (uint8_t)(pixel >> 8);
            out[pos + x * 3U + 2U] = (uint8_t)(pixel >> 16);
        }
        pos += stride;
    }
    *data = out;
    *len = file_size;
    return 0;
}

void paint_draw_point(uint32_t *pixels, uint32_t width, uint32_t height,
                      uint32_t x, uint32_t y, uint32_t color,
                      uint32_t brush_size, enum paint_tool tool)
{
    int32_t radius = (int32_t)(brush_size / 2U);
    uint32_t paint_color = tool == PAINT_TOOL_ERASER ? 0x00ffffffU : color;
    if (!pixels || !width || !height) return;
    if (radius < 1) radius = 1;
    for (int32_t dy = -radius; dy <= radius; ++dy) {
        for (int32_t dx = -radius; dx <= radius; ++dx) {
            int32_t xx = (int32_t)x + dx;
            int32_t yy = (int32_t)y + dy;
            if (tool == PAINT_TOOL_BRUSH && dx * dx + dy * dy > radius * radius)
                continue;
            if (xx >= 0 && yy >= 0 && (uint32_t)xx < width && (uint32_t)yy < height)
                pixels[(uint32_t)yy * width + (uint32_t)xx] = paint_color;
        }
    }
}

void paint_draw_line(uint32_t *pixels, uint32_t width, uint32_t height,
                     uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                     uint32_t color, uint32_t brush_size, enum paint_tool tool)
{
    int64_t dx = (int64_t)x1 - (int64_t)x0;
    int64_t dy = (int64_t)y1 - (int64_t)y0;
    int64_t steps = dx < 0 ? -dx : dx;
    int64_t abs_dy = dy < 0 ? -dy : dy;
    if (abs_dy > steps) steps = abs_dy;
    if (steps == 0) {
        paint_draw_point(pixels, width, height, x0, y0, color, brush_size, tool);
        return;
    }
    for (int64_t i = 0; i <= steps; ++i) {
        paint_draw_point(pixels, width, height,
                         (uint32_t)((int64_t)x0 + dx * i / steps),
                         (uint32_t)((int64_t)y0 + dy * i / steps),
                         color, brush_size, tool);
    }
}
