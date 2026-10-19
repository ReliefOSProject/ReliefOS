#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/reliefnt/drivers/console/console.c"

void *kernel_malloc(size_t size) { return malloc(size); }
void kernel_free(void *ptr) { free(ptr); }

static uint32_t pixels[80 * 48];
static uint32_t full_clear_count;
static uint32_t text_present_count;
static char serial_output[16384];
static size_t serial_length;
static const struct framebuffer display = {
    .pixels = pixels, .width = 80, .height = 48, .pitch = 80 * 4,
    .bpp = 32, .bytes_per_pixel = 4, .available = true,
};
const struct framebuffer *framebuffer_get(void) { return &display; }
void framebuffer_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c)
{
    if (!x && !y && w == display.width && h == display.height) ++full_clear_count;
    for (uint32_t row = y; row < y + h && row < 48; ++row)
        for (uint32_t col = x; col < x + w && col < 80; ++col)
            pixels[row * 80 + col] = c;
}
void framebuffer_clear(uint32_t c) { framebuffer_rect(0, 0, 80, 48, c); }
void framebuffer_present(void) {}
void framebuffer_present_region(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{ (void)x; (void)y; (void)w; (void)h; ++text_present_count; }
uint32_t framebuffer_codepoint_width(uint32_t c) { (void)c; return 1; }
void framebuffer_codepoint(uint32_t x, uint32_t y, uint32_t c, uint32_t fg, uint32_t bg)
{ (void)c; (void)bg; framebuffer_rect(x, y, 8, 16, fg); }
void serial_write(const char *text)
{
    size_t length = strlen(text);
    assert(length < sizeof(serial_output) - serial_length);
    memcpy(serial_output + serial_length, text, length + 1u);
    serial_length += length;
}
void vga_putc(char c) { (void)c; }
uint64_t time_uptime_us(void) { return 0; }
uint64_t kernel_irq_save(void) { return 0; }
void kernel_irq_restore(uint64_t flags) { (void)flags; }
uint32_t smp_current_cpu(void) { return 0; }
void smp_membarrier_poll(void) {}

int main(void)
{
    console_log_configure("logcolor=0");
    console_enable_framebuffer();
    console_info("boot ready\n");
    uint32_t boot_col = fb_col;
    uint32_t boot_row = fb_row;
    uint32_t boot_clear_count = full_clear_count;
    size_t boot_history_length = log_len - console_log_replay_start();
    console_vt_activate(1, false);
    assert(full_clear_count == boot_clear_count);
    assert(fb_col == boot_col && fb_row == boot_row);
    assert(vt_text[0].length == boot_history_length);
    assert(strstr(vt_text[0].bytes, "[INFO] boot ready\n") != NULL);
    console_vt_write(1, "init\n", 5);
    assert(full_clear_count == boot_clear_count);
    assert(vt_text[0].length == boot_history_length + 5u);
    assert(memcmp(vt_text[0].bytes + boot_history_length, "init\n", 5) == 0);
    static const char openrc_output[] = "\033[1;32mOpenRC\033[0m starting\n";
    size_t before_openrc = vt_text[0].length;
    uint32_t before_openrc_present = text_present_count;
    console_vt_write(1, openrc_output, sizeof(openrc_output) - 1u);
    size_t before_serial_log = log_len;
    console_serial_write_len(openrc_output, sizeof(openrc_output) - 1u);
    assert(log_len == before_serial_log);
    size_t startup_history_length = before_openrc + sizeof(openrc_output) - 1u;
    assert(vt_text[0].length == startup_history_length);
    assert(memcmp(vt_text[0].bytes + before_openrc, openrc_output,
                  sizeof(openrc_output) - 1u) == 0);
    assert(strstr(serial_output, openrc_output) != NULL);
    assert(text_present_count == before_openrc_present + 1u);
    assert(fb_col == 0 && fb_console_fg_color == console_fg());
    assert(full_clear_count == boot_clear_count);
    uint32_t init_col = fb_col;
    uint32_t init_row = fb_row;
    console_vt_activate(1, false);
    assert(full_clear_count == boot_clear_count);
    assert(fb_col == init_col && fb_row == init_row);
    console_vt_activate(2, false);
    console_vt_activate(1, false);
    assert(vt_text[0].length == startup_history_length);
    assert(strstr(vt_text[0].bytes, "[INFO] boot ready\ninit\n") != NULL);
    assert(strstr(vt_text[0].bytes, openrc_output) != NULL);
    console_vt_activate(2, false);
    assert(fb_console_tty_cursor_visible && fb_console_tty_cursor_x == 0);
    console_vt_write(2, "a", 1);
    assert(fb_console_tty_cursor_x == 8);
    console_vt_write(2, "b", 1);
    assert(fb_console_tty_cursor_x == 16);
    uint32_t before[80 * 48];
    memcpy(before, pixels, sizeof(pixels));
    console_write("background kernel log\n");
    console_write_len("kmsg diagnostic\n", sizeof("kmsg diagnostic\n") - 1u);
    assert(memcmp(before, pixels, sizeof(pixels)) == 0);
    assert(vt_text[0].length == startup_history_length);
    assert(strstr(serial_output, "kmsg diagnostic\n") != NULL);
    static const char service_output[] = " * Starting reliefos-dhcp ... [ ok ]\n";
    console_vt_write(1, service_output, sizeof(service_output) - 1u);
    console_serial_write_len(service_output, sizeof(service_output) - 1u);
    assert(memcmp(before, pixels, sizeof(pixels)) == 0);
    assert(vt_text[1].length == 2u);
    assert(memcmp(vt_text[0].bytes + startup_history_length, service_output,
                  sizeof(service_output) - 1u) == 0);
    assert(strstr(serial_output, service_output) != NULL);
    console_vt_write(3, "another terminal", 16);
    assert(memcmp(before, pixels, sizeof(pixels)) == 0);
    console_vt_activate(1, true);
    memcpy(before, pixels, sizeof(pixels));
    static const char graphical_output[] = "service still running\n";
    console_vt_write(1, graphical_output, sizeof(graphical_output) - 1u);
    assert(memcmp(before, pixels, sizeof(pixels)) == 0);
    assert(strstr(vt_text[0].bytes, graphical_output) != NULL);
    console_vt_activate(1, false);
    assert(strstr(vt_text[0].bytes, openrc_output) != NULL);
    assert(strstr(vt_text[0].bytes, service_output) != NULL);
    assert(strstr(vt_text[0].bytes, graphical_output) != NULL);
    puts("VT boot continuity, terminal/diagnostic separation, serial mirror, graphics isolation and text cursor passed");
}
