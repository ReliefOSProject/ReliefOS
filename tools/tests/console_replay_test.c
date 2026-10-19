#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <reliefnt/port.h>
#include <reliefnt/svga.h>

#include "../../kernel/reliefnt/drivers/video/framebuffer/framebuffer.c"
#include "../../kernel/reliefnt/drivers/console/console.c"

#define FB_GUARD_BYTES 32u
#define FB_MAX_BYTES (32u * 1024u)
#define FB_WIDTH 96u
#define FB_HEIGHT_24 53u
#define FB_PITCH_24 (FB_WIDTH * 3u + 13u)
#define FB_HEIGHT_32 59u
#define FB_PITCH_32 (FB_WIDTH * 4u + 17u)

struct svga_device svga;

static uint8_t display_a[FB_GUARD_BYTES + FB_MAX_BYTES + FB_GUARD_BYTES];
static uint8_t display_b[FB_GUARD_BYTES + FB_MAX_BYTES + FB_GUARD_BYTES];
static uint32_t allocation_calls;
static uint32_t allocation_failures;
static bool fail_replay_allocations;

uint64_t svga_lock(void) { return 0; }
bool svga_try_lock_irqsave(uint64_t *flags)
{
    if (flags) *flags = 0;
    return true;
}
void svga_unlock(uint64_t flags) { (void)flags; }
int svga_update_try_locked(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    (void)x; (void)y; (void)width; (void)height;
    return 0;
}
int svga_update_locked(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    (void)x; (void)y; (void)width; (void)height;
    return 0;
}
bool svga_restore_traces_locked(void) { return false; }
void x86_64_outl(uint32_t value, uint16_t port) { (void)value; (void)port; }
uint32_t x86_64_inl(uint16_t port) { (void)port; return 0; }
void x86_64_outw(uint16_t value, uint16_t port) { (void)value; (void)port; }
uint16_t x86_64_inw(uint16_t port) { (void)port; return 0; }

void *kernel_malloc(size_t size)
{
    ++allocation_calls;
    if (fail_replay_allocations &&
        (allocation_calls == 2u || allocation_calls == 4u)) {
        ++allocation_failures;
        return NULL;
    }
    return malloc(size);
}

void kernel_free(void *ptr) { free(ptr); }
void serial_write(const char *text) { (void)text; }
void vga_putc(char ch) { (void)ch; }
uint64_t time_uptime_us(void) { return 123456789ULL; }
uint64_t kernel_irq_save(void) { return 0; }
void kernel_irq_restore(uint64_t flags) { (void)flags; }
uint32_t smp_current_cpu(void) { return 0; }
void smp_membarrier_poll(void) {}

static void set_default_rgb_format(struct framebuffer *target)
{
    target->type = MULTIBOOT2_FRAMEBUFFER_TYPE_RGB;
    target->red_field_position = 16u;
    target->red_mask_size = 8u;
    target->green_field_position = 8u;
    target->green_mask_size = 8u;
    target->blue_field_position = 0u;
    target->blue_mask_size = 8u;
}

static void initialize_console_state(void)
{
    memset(&console_utf8, 0, sizeof(console_utf8));
    memset(vt_text, 0, sizeof(vt_text));
    memset(log_buffer, 0, sizeof(log_buffer));
    memset(fb_ansi_params, 0, sizeof(fb_ansi_params));
    memset(fb_ansi_param_colon, 0, sizeof(fb_ansi_param_colon));
    memset(fb_console_tty_cursor_saved, 0, sizeof(fb_console_tty_cursor_saved));
    console_active_vt = 0;
    console_vt_graphical = false;
    log_len = 0;
    log_start = 0;
    fb_console_enabled = false;
    vga_console_enabled = false;
    fb_x = fb_y = fb_cols = fb_rows = fb_col = fb_row = 0;
    console_ui_theme = 1u;
    console_line_start = true;
    console_service_logs_only = false;
    console_presenting = false;
    console_log_color_enabled = false;
    vga_ansi_state = 0;
    console_log_owner = UINT32_MAX;
    fb_saved_col = fb_saved_row = 0;
    fb_console_fg_color = fb_console_bg_color = 0;
    fb_console_bold = false;
    fb_console_underline = false;
    fb_console_reverse = false;
    fb_console_underline_color = 0;
    fb_console_underline_color_set = false;
    fb_scroll_top = fb_scroll_bottom = 0;
    fb_console_tty_cursor_active = false;
    fb_console_tty_cursor_visible = false;
    fb_console_tty_cursor_x = fb_console_tty_cursor_y = 0;
    fb_console_tty_cursor_bpp = 0;
    fb_console_reset_ansi();
    if (console_graphic_saved) kernel_free(console_graphic_saved);
    console_graphic_saved = NULL;
    console_graphic_saved_bytes = 0;
    console_graphic_saved_vt = 0;
#ifndef CONSOLE_REPLAY_REFERENCE
    fb_console_render_target = NULL;
#endif
}

static void configure_display(struct framebuffer *target, uint8_t *storage,
                              uint32_t width, uint32_t height, uint32_t pitch,
                              uint8_t bpp, uint8_t bytes_per_pixel)
{
    memset(storage, 0xa5, FB_GUARD_BYTES + FB_MAX_BYTES + FB_GUARD_BYTES);
    memset(target, 0, sizeof(*target));
    target->pixels = (uint32_t *)(void *)(storage + FB_GUARD_BYTES);
    target->width = width;
    target->height = height;
    target->pitch = pitch;
    target->bpp = bpp;
    target->bytes_per_pixel = bytes_per_pixel;
    target->backend = FRAMEBUFFER_BACKEND_BOOT;
    target->available = true;
    set_default_rgb_format(target);
}

static void fill_graphical_pattern(struct framebuffer *target)
{
    uint8_t *pixels = (uint8_t *)target->pixels;
    uint32_t visible_bytes = target->width * target->bytes_per_pixel;
    for (uint32_t y = 0; y < target->height; ++y) {
        for (uint32_t x = 0; x < visible_bytes; ++x)
            pixels[(size_t)y * target->pitch + x] =
                (uint8_t)(0x31u + (x * 5u + y * 11u) % 191u);
        for (uint32_t x = visible_bytes; x < target->pitch; ++x)
            pixels[(size_t)y * target->pitch + x] = 0xa5u;
    }
}

static void assert_guards_and_padding(const struct framebuffer *target,
                                     const uint8_t *storage)
{
    const uint8_t *pixels = (const uint8_t *)target->pixels;
    uint32_t visible_bytes = target->width * target->bytes_per_pixel;
    for (uint32_t i = 0; i < FB_GUARD_BYTES; ++i) {
        assert(storage[i] == 0xa5u);
        assert(pixels[(size_t)target->pitch * target->height + i] == 0xa5u);
    }
    for (uint32_t y = 0; y < target->height; ++y)
        for (uint32_t x = visible_bytes; x < target->pitch; ++x)
            assert(pixels[(size_t)y * target->pitch + x] == 0xa5u);
}

static void assert_remainder_strip_cleared(const struct framebuffer *target)
{
    const uint8_t *pixels = (const uint8_t *)target->pixels;
    uint32_t first_remainder_row = fb_rows * RELIEFOS_FONT_H;
    uint32_t visible_bytes = target->width * target->bytes_per_pixel;
    for (uint32_t y = first_remainder_row; y < target->height; ++y)
        for (uint32_t x = 0; x < visible_bytes; ++x)
            assert(pixels[(size_t)y * target->pitch + x] == 0u);
}

static void write_u32(FILE *output, uint32_t value)
{
    assert(fwrite(&value, sizeof(value), 1u, output) == 1u);
}

static void write_u64(FILE *output, uint64_t value)
{
    assert(fwrite(&value, sizeof(value), 1u, output) == 1u);
}

static void write_state(FILE *output)
{
#define WRITE_U32(name) write_u32(output, (uint32_t)(name))
    WRITE_U32(console_active_vt);
    WRITE_U32(console_vt_graphical);
    WRITE_U32(fb_cols); WRITE_U32(fb_rows); WRITE_U32(fb_col); WRITE_U32(fb_row);
    WRITE_U32(fb_saved_col); WRITE_U32(fb_saved_row);
    WRITE_U32(fb_scroll_top); WRITE_U32(fb_scroll_bottom);
    WRITE_U32(fb_console_enabled); WRITE_U32(fb_console_bold);
    WRITE_U32(fb_console_underline); WRITE_U32(fb_console_reverse);
    WRITE_U32(fb_console_underline_color_set);
    WRITE_U32(fb_console_fg_color); WRITE_U32(fb_console_bg_color);
    WRITE_U32(fb_console_underline_color);
    WRITE_U32(fb_console_tty_cursor_active); WRITE_U32(fb_console_tty_cursor_visible);
    WRITE_U32(fb_console_tty_cursor_x); WRITE_U32(fb_console_tty_cursor_y);
    WRITE_U32(fb_console_tty_cursor_bpp);
    WRITE_U32(fb_ansi_state); WRITE_U32(fb_ansi_param_count);
    WRITE_U32(fb_ansi_param_value); WRITE_U32(fb_ansi_param_active);
    WRITE_U32(fb_ansi_private);
    assert(fwrite(fb_ansi_params, sizeof(fb_ansi_params), 1u, output) == 1u);
    assert(fwrite(fb_ansi_param_colon, sizeof(fb_ansi_param_colon), 1u, output) == 1u);
    assert(fwrite(&console_utf8, sizeof(console_utf8), 1u, output) == 1u);
    assert(fwrite(fb_console_tty_cursor_saved,
                  sizeof(fb_console_tty_cursor_saved), 1u, output) == 1u);
    write_u64(output, (uint64_t)vt_text[1].length);
    assert(fwrite(vt_text[1].bytes, vt_text[1].length, 1u, output) == 1u);
#undef WRITE_U32
}

static void write_frame(FILE *output, uint32_t scenario,
                        const struct framebuffer *target, const uint8_t *storage)
{
    uint32_t visible_bytes = target->width * target->bytes_per_pixel;
    write_u32(output, scenario);
    write_u32(output, target->width);
    write_u32(output, target->height);
    write_u32(output, target->pitch);
    write_u32(output, target->bpp);
    write_u32(output, target->bytes_per_pixel);
    write_u32(output, visible_bytes);
    write_state(output);
    assert(fwrite(storage, FB_GUARD_BYTES, 1u, output) == 1u);
    assert(fwrite(target->pixels, (size_t)target->pitch * target->height,
                  1u, output) == 1u);
    assert(fwrite((const uint8_t *)target->pixels +
                  (size_t)target->pitch * target->height,
                  FB_GUARD_BYTES, 1u, output) == 1u);
}

static void write_long_history(void)
{
    static const char line[] = "\033[38;5;39mreplay-row-0123456789\033[0m\n";
    for (uint32_t i = 0; i < 5000u; ++i)
        console_vt_write(2u, line, sizeof(line) - 1u);
    while (vt_text[1].length < CONSOLE_VT_TEXT_CAP)
        console_vt_write(2u, "x", 1u);
    assert(vt_text[1].length == CONSOLE_VT_TEXT_CAP);
    console_vt_write(2u, "z", 1u);
    assert(vt_text[1].length < CONSOLE_VT_TEXT_CAP);
    while (vt_text[1].length < CONSOLE_VT_TEXT_CAP)
        console_vt_write(2u, "x", 1u);
    assert(vt_text[1].length == CONSOLE_VT_TEXT_CAP);
}

static void run_replay_fixture(FILE *output)
{
    static const char semantic_sequence[] =
        "login: \033[38;2;23;123;231mRGB\033[0m \033[1;4;7mBUI\033[0m "
        "\xe2\x98\x83\r\n"
        "\033[2;3rregion-top\nregion-bottom\nscroll\033[S"
        "\033[1;1H\033[sreplace\033[8;1Hrestore\033[u"
        "\033[2K\033[?25l\033[?25hend\n";
    uint8_t *active_storage = display_a;
    struct framebuffer display;

    initialize_console_state();
    configure_display(&display, active_storage, FB_WIDTH, FB_HEIGHT_24,
                      FB_PITCH_24, 24u, 3u);
    fb = display;
    console_enable_framebuffer();
    console_vt_activate(2u, true);
    console_vt_write(2u, semantic_sequence, sizeof(semantic_sequence) - 1u);
    fill_graphical_pattern(&fb);
    console_vt_activate(2u, false);
    assert_remainder_strip_cleared(&fb);
    assert_guards_and_padding(&fb, active_storage);
    assert(fb_console_tty_cursor_active && fb_console_tty_cursor_visible);
    assert(vt_text[1].length == sizeof(semantic_sequence) - 1u);
    write_frame(output, 1u, &fb, active_storage);

    console_vt_activate(2u, true);
    configure_display(&display, display_b, FB_WIDTH, FB_HEIGHT_32,
                      FB_PITCH_32, 32u, 4u);
    fb = display;
    fill_graphical_pattern(&fb);
    write_long_history();
    assert(vt_text[1].length == CONSOLE_VT_TEXT_CAP);
    console_vt_activate(2u, false);
    assert_remainder_strip_cleared(&fb);
    assert_guards_and_padding(&fb, display_b);
    assert(fb_console_tty_cursor_active && fb_console_tty_cursor_visible);
    assert(vt_text[1].length == CONSOLE_VT_TEXT_CAP);
    write_frame(output, 2u, &fb, display_b);
}

#ifndef CONSOLE_REPLAY_REFERENCE
static void test_explicit_framebuffer_target(void)
{
    uint8_t target_storage[FB_GUARD_BYTES + FB_MAX_BYTES + FB_GUARD_BYTES];
    uint8_t global_storage[FB_GUARD_BYTES + FB_MAX_BYTES + FB_GUARD_BYTES];
    uint8_t global_before[32u * 16u * 4u];
    struct framebuffer target;
    uint8_t *target_pixels;
    uint8_t *global_pixels;

    configure_display(&fb, global_storage, 32u, 16u, 32u * 4u,
                      32u, 4u);
    memset(target_storage, 0xa5, sizeof(target_storage));
    memset(&target, 0, sizeof(target));
    target.pixels = (uint32_t *)(void *)(target_storage + FB_GUARD_BYTES);
    target.width = 32u;
    target.height = 16u;
    target.pitch = 32u * 3u + 7u;
    target.bpp = 24u;
    target.bytes_per_pixel = 3u;
    target.available = true;
    set_default_rgb_format(&target);
    target_pixels = (uint8_t *)target.pixels;
    global_pixels = (uint8_t *)fb.pixels;
    memcpy(global_before, global_pixels, sizeof(global_before));

    framebuffer_rect_to(&target, 1u, 1u, 2u, 2u, 0x00112233u);
    assert(target_pixels[target.pitch + 3u] == 0x33u);
    assert(target_pixels[target.pitch + 4u] == 0x22u);
    assert(target_pixels[target.pitch + 5u] == 0x11u);
    framebuffer_codepoint_to(&target, 8u, 0u, 'A', 0x00ffffffu, 0u);
    assert(memcmp(global_pixels, global_before, sizeof(global_before)) == 0);
    for (uint32_t y = 0; y < target.height; ++y)
        for (uint32_t x = target.width * target.bytes_per_pixel;
             x < target.pitch; ++x)
            assert(target_pixels[(size_t)y * target.pitch + x] == 0xa5u);
    for (uint32_t i = 0; i < FB_GUARD_BYTES; ++i) {
        assert(target_storage[i] == 0xa5u);
        assert(target_pixels[(size_t)target.pitch * target.height + i] == 0xa5u);
    }

    struct framebuffer invalid = target;
    invalid.pitch = invalid.width * invalid.bytes_per_pixel - 1u;
    framebuffer_rect_to(&invalid, 0u, 0u, 8u, 8u, 0x00ffffffu);
    assert(target_pixels[0] == 0xa5u);

    struct framebuffer active = fb;
    struct framebuffer replay_target;
    uint8_t *replay_pixels = (uint8_t *)(uintptr_t)1u;
    uint32_t calls_before = allocation_calls;
    fb.pitch = fb.width * fb.bytes_per_pixel - 1u;
    assert(!fb_console_allocate_replay_target(&replay_target, &replay_pixels));
    assert(replay_pixels == NULL);
    assert(allocation_calls == calls_before);
    fb = active;
}
#endif

int main(int argc, char **argv)
{
    const char *output_path;
    FILE *output;
#ifndef CONSOLE_REPLAY_REFERENCE
    if (argc != 3) {
        fprintf(stderr, "usage: %s OUTPUT shadow|fallback\n", argv[0]);
        return 2;
    }
    fail_replay_allocations = strcmp(argv[2], "fallback") == 0;
    assert(fail_replay_allocations || strcmp(argv[2], "shadow") == 0);
    test_explicit_framebuffer_target();
#else
    if (argc != 2) {
        fprintf(stderr, "usage: %s OUTPUT\n", argv[0]);
        return 2;
    }
#endif
    output_path = argv[1];
    output = fopen(output_path, "wb");
    assert(output);
    assert(fwrite("C6RP", 4u, 1u, output) == 1u);
    run_replay_fixture(output);
    assert(fclose(output) == 0);
#ifndef CONSOLE_REPLAY_REFERENCE
    if (fail_replay_allocations)
        assert(allocation_failures == 2u);
    else
        assert(allocation_failures == 0u);
#else
    assert(allocation_failures == 0u);
#endif
    puts("console-replay: semantic framebuffer and VT state passed");
    return 0;
}
