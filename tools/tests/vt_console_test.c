#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/reliefnt/drivers/bootstrap/console.c"

void *kernel_malloc(size_t size) { return malloc(size); }
void kernel_free(void *ptr) { free(ptr); }

static uint32_t pixels[80 * 48];
static const struct framebuffer display = {
    .pixels = pixels, .width = 80, .height = 48, .pitch = 80 * 4,
    .bpp = 32, .bytes_per_pixel = 4, .available = true,
};
const struct framebuffer *framebuffer_get(void) { return &display; }
void framebuffer_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c)
{
    for (uint32_t row = y; row < y + h && row < 48; ++row)
        for (uint32_t col = x; col < x + w && col < 80; ++col)
            pixels[row * 80 + col] = c;
}
void framebuffer_clear(uint32_t c) { framebuffer_rect(0, 0, 80, 48, c); }
void framebuffer_present(void) {}
void framebuffer_present_region(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{ (void)x; (void)y; (void)w; (void)h; }
uint32_t framebuffer_codepoint_width(uint32_t c) { (void)c; return 1; }
void framebuffer_codepoint(uint32_t x, uint32_t y, uint32_t c, uint32_t fg, uint32_t bg)
{ (void)c; (void)bg; framebuffer_rect(x, y, 8, 16, fg); }
void serial_write(const char *text) { (void)text; }
void vga_putc(char c) { (void)c; }
uint64_t time_uptime_us(void) { return 0; }

int main(void)
{
    fb_console_enabled = true;
    console_vt_activate(2, false);
    assert(fb_console_tty_cursor_visible && fb_console_tty_cursor_x == 0);
    console_vt_write(2, "a", 1);
    assert(fb_console_tty_cursor_x == 8);
    console_vt_write(2, "b", 1);
    assert(fb_console_tty_cursor_x == 16);
    uint32_t before[80 * 48];
    memcpy(before, pixels, sizeof(pixels));
    console_write("background kernel log\n");
    assert(memcmp(before, pixels, sizeof(pixels)) == 0);
    console_vt_write(3, "another terminal", 16);
    assert(memcmp(before, pixels, sizeof(pixels)) == 0);
    puts("VT text cursor, kernel-log isolation and background output passed");
}
