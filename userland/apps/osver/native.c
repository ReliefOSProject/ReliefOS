#include <reliefos/gui.h>
#include <libintl.h>
#include <locale.h>
#include <reliefos/layout.h>
#include <reliefos/png.h>
#include <reliefos/psf_font.h>
#include <reliefos/stdio.h>
#include <reliefos/system.h>
#include <reliefos/syscall.h>
#include <reliefos/ui.h>
#include <reliefos/layout.h>

#define OSVER_W 720
#define OSVER_H 460
#define OSVER_LOGO_PATH RELIEFOS_PATH_LOGO_PNG
#define OSVER_LOGO_BOX 196U
#define T(s) gettext(s)

static uint32_t pixels[OSVER_W * OSVER_H];
static struct reliefos_system_info info;
static char status_text[96];
static uint32_t *logo_pixels;
static uint32_t logo_width;
static uint32_t logo_height;
static uint32_t logo_clicks;
static unsigned long logo_click_window_ms;

static void copy_text(char *dst, uint32_t cap, const char *src);

static int osver_logo_rect(uint32_t *left, uint32_t *top,
                           uint32_t *width, uint32_t *height)
{
    const uint32_t box_x = 16U + 35U;
    const uint32_t box_y = 88U + 58U;
    const uint32_t box_size = OSVER_LOGO_BOX - 16U;
    uint32_t draw_w;
    uint32_t draw_h;
    if (!logo_pixels || !logo_width || !logo_height) return 0;
    draw_w = box_size;
    draw_h = (uint32_t)(((uint64_t)box_size * logo_height) / logo_width);
    if (draw_h > box_size) {
        draw_h = box_size;
        draw_w = (uint32_t)(((uint64_t)box_size * logo_width) / logo_height);
    }
    if (!draw_w || !draw_h) return 0;
    if (left) *left = box_x + (box_size - draw_w) / 2U;
    if (top) *top = box_y + (box_size - draw_h) / 2U;
    if (width) *width = draw_w;
    if (height) *height = draw_h;
    return 1;
}

static void osver_kernel_debug_click(uint32_t x, uint32_t y)
{
    uint32_t left;
    uint32_t top;
    uint32_t width;
    uint32_t height;
    const uint32_t now = (uint32_t)reliefos_uptime_ms();
    uint32_t flags = 0;
    if (!osver_logo_rect(&left, &top, &width, &height) ||
        x < left || y < top || x >= left + width || y >= top + height) {
        logo_clicks = 0;
        logo_click_window_ms = now;
        return;
    }
    if (logo_clicks == 0U || now - logo_click_window_ms > 2000U) {
        logo_clicks = 0U;
        logo_click_window_ms = now;
    }
    ++logo_clicks;
    if (logo_clicks < 5U) return;
    logo_clicks = 0U;
    if (reliefos_kernel_debug_get_state(&flags) < 0 ||
        (flags & RELIEFOS_KERNEL_DEBUG_STATE_ENABLED) == 0U) {
        if (reliefos_kernel_debug_set_enabled(1) == 0) {
            copy_text(status_text, sizeof(status_text),
                      T("Kernel debug mode enabled"));
            (void)reliefos_ui_show_message_box(
                T("Kernel debug mode"),
                T("Kernel debug mode enabled. Use Start > Power to reboot into it."),
                T("OK"));
        } else {
            (void)reliefos_ui_show_message_box(
                T("Kernel debug mode"),
                T("Could not persist kernel debug mode."),
                T("OK"));
        }
    }
}

static void copy_text(char *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0;
    if (!dst || cap == 0) {
        return;
    }
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

static void draw_logo(struct reliefos_ui_surface *ui, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h)
{
    uint32_t draw_w;
    uint32_t draw_h;
    uint32_t draw_x;
    uint32_t draw_y;

    if (!logo_pixels || !logo_width || !logo_height || !w || !h) {
        reliefos_ui_rect(ui, x, y, w, h, RELIEFOS_UI_WHITE);
        reliefos_ui_text_resized_clipped(ui, x + 12, y + h / 2U - 8U,
                                       w > 24U ? w - 24U : w,
                                       T("Logo unavailable"),
                                       RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE,
                                       RELIEFOS_FONT_W, RELIEFOS_FONT_H);
        return;
    }

    draw_w = w;
    draw_h = (uint32_t)(((uint64_t)w * logo_height) / logo_width);
    if (draw_h > h) {
        draw_h = h;
        draw_w = (uint32_t)(((uint64_t)h * logo_width) / logo_height);
    }
    if (!draw_w || !draw_h) {
        return;
    }
    draw_x = x + (w - draw_w) / 2U;
    draw_y = y + (h - draw_h) / 2U;
    for (uint32_t dy = 0; dy < draw_h; ++dy) {
        uint32_t sy = (uint32_t)(((uint64_t)dy * logo_height) / draw_h);
        if (sy >= logo_height) {
            sy = logo_height - 1U;
        }
        for (uint32_t dx = 0; dx < draw_w; ++dx) {
            uint32_t sx = (uint32_t)(((uint64_t)dx * logo_width) / draw_w);
            if (sx >= logo_width) {
                sx = logo_width - 1U;
            }
            if (draw_x + dx < ui->width && draw_y + dy < ui->height) {
                ui->pixels[(draw_y + dy) * ui->stride + draw_x + dx] =
                    logo_pixels[sy * logo_width + sx];
            }
        }
    }
}

static void draw_osver(struct reliefos_ui_surface *ui)
{
    struct reliefos_ui_property_item props[] = {
        {T("Kernel"), info.kernel_name, 0},
        {T("Kernel version"), info.kernel_version, 0},
        {T("Build time"), info.build_time, 0},
        {T("Copyright"), info.copyright, 0},
    };
    const uint32_t hero_x = 16U;
    const uint32_t hero_y = 88U;
    const uint32_t hero_w = 250U;
    const uint32_t content_h = 328U;
    const uint32_t info_x = 282U;
    const uint32_t info_w = OSVER_W - info_x - 16U;

    reliefos_ui_rect(ui, 0, 0, OSVER_W, OSVER_H, RELIEFOS_UI_GRAY);
    reliefos_ui_toolbar(ui, 0, 0, OSVER_W, 68U);
    reliefos_ui_rect(ui, 0, 0, 8U, 68U, RELIEFOS_UI_ACTIVE_TITLE);
    reliefos_ui_text_resized_clipped(ui, 28U, 12U, 300U, "ReliefOS",
                                   RELIEFOS_UI_BLACK, RELIEFOS_UI_GRAY, 12U, 24U);
    reliefos_ui_text(ui, 29U, 43U,
                   T("About this operating system"),
                   RELIEFOS_UI_DARK, RELIEFOS_UI_GRAY);

    reliefos_ui_panel(ui, hero_x, hero_y, hero_w, content_h, RELIEFOS_UI_LIGHT);
    reliefos_ui_text(ui, hero_x + 16U, hero_y + 14U,
                   T("ReliefOS"), RELIEFOS_UI_BLACK, RELIEFOS_UI_LIGHT);
    reliefos_ui_rect(ui, hero_x + 16U, hero_y + 38U, hero_w - 32U, 1U,
                   RELIEFOS_UI_WHITE);
    reliefos_ui_panel(ui, hero_x + 27U, hero_y + 50U, OSVER_LOGO_BOX,
                    OSVER_LOGO_BOX, RELIEFOS_UI_WHITE);
    draw_logo(ui, hero_x + 35U, hero_y + 58U, OSVER_LOGO_BOX - 16U,
              OSVER_LOGO_BOX - 16U);
    reliefos_ui_text_resized_clipped(ui, hero_x + 16U, hero_y + 260U,
                                   hero_w - 32U, "ReliefOS", RELIEFOS_UI_BLACK,
                                   RELIEFOS_UI_LIGHT, 10U, 20U);
    reliefos_ui_text(ui, hero_x + 16U, hero_y + 287U,
                   T("A compact desktop OS"),
                   RELIEFOS_UI_DARK, RELIEFOS_UI_LIGHT);

    reliefos_ui_panel(ui, info_x, hero_y, info_w, content_h, RELIEFOS_UI_LIGHT);
    reliefos_ui_rect(ui, info_x + 1U, hero_y + 1U, info_w - 2U, 54U,
                   RELIEFOS_UI_WHITE);
    reliefos_ui_text(ui, info_x + 16U, hero_y + 10U,
                   T("System information"), RELIEFOS_UI_BLACK,
                   RELIEFOS_UI_WHITE);
    reliefos_ui_text(ui, info_x + 16U, hero_y + 31U,
                   T("Build and runtime components"),
                   RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
    reliefos_ui_property_grid(ui, info_x + 12U, hero_y + 68U, info_w - 24U,
                            props, sizeof(props) / sizeof(props[0]), 122U, 28U);
    reliefos_ui_text(ui, info_x + 16U, hero_y + 260U,
                   T("This window reports the version embedded in the running kernel."),
                   RELIEFOS_UI_DARK, RELIEFOS_UI_LIGHT);
    reliefos_ui_text(ui, info_x + 16U, hero_y + 282U,
                   T("ReliefOS is free software for learning and experimentation."),
                   RELIEFOS_UI_DARK, RELIEFOS_UI_LIGHT);
    reliefos_ui_statusbar(ui, OSVER_H - 28, 28, status_text);
}

int main(void)
{
    setlocale(LC_ALL, "");
    bindtextdomain("leonos", RELIEFOS_LAYOUT_LOCALE);
    textdomain("leonos");
    struct reliefos_ui_surface ui;
    struct reliefos_gui_app_event event;
    int window_id;
    int ret;

    puts("[osver.elf] system version viewer starting");
    (void)reliefos_png_decode_file(OSVER_LOGO_PATH, &logo_pixels, &logo_width,
                                 &logo_height);
    if (!logo_pixels) {
        copy_text(status_text, sizeof(status_text),
                  T("System information (logo unavailable)"));
    }
    ret = reliefos_system_info(&info);
    if (ret < 0) {
        copy_text(status_text, sizeof(status_text), T("Could not read system version information"));
        copy_text(info.kernel_name, sizeof(info.kernel_name), "unknown");
        copy_text(info.kernel_version, sizeof(info.kernel_version), "0.0.0-0000");
        copy_text(info.build_time, sizeof(info.build_time), "unknown");
        copy_text(info.copyright, sizeof(info.copyright),
                  "Copyright LeonMMcoset 2021-2026. All rights reserved.");
    }

    if (!status_text[0]) {
        copy_text(status_text, sizeof(status_text), T("System version information"));
    }
    {
        uint32_t debug_flags = 0;
        if (reliefos_kernel_debug_get_state(&debug_flags) == 0 &&
            (debug_flags & RELIEFOS_KERNEL_DEBUG_STATE_ENABLED) != 0U) {
            copy_text(status_text, sizeof(status_text),
                      T("Kernel debug mode enabled"));
        }
    }
    window_id = reliefos_gui_create_app_window_ex(T("About ReliefOS"), T("System version"),
                                                OSVER_W, OSVER_H, RELIEFOS_GUI_WINDOW_NO_RESIZE);
    if (window_id <= 0) {
        printf("[osver.elf] create window failed=%d\n", window_id);
        return 1;
    }

    reliefos_ui_bind(&ui, pixels, OSVER_W, OSVER_H, OSVER_W);
    draw_osver(&ui);
    reliefos_gui_present_window((uint32_t)window_id, OSVER_W, OSVER_H, OSVER_W, pixels);

    for (;;) {
        event.window_id = (uint32_t)window_id;
        while (reliefos_gui_wait_app_event(&event, RELIEFOS_GUI_IDLE_WAIT_MS) > 0) {
            if (event.type == RELIEFOS_GUI_APP_EVENT_CLOSE) {
                reliefos_png_free(logo_pixels);
                return 0;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_KEY_DOWN && event.pressed && event.keycode == 1) {
                reliefos_png_free(logo_pixels);
                return 0;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_MOUSE_BUTTON && (event.buttons & 1U)) {
                osver_kernel_debug_click((uint32_t)event.x, (uint32_t)event.y);
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_RESIZE ||
                event.type == RELIEFOS_GUI_APP_EVENT_FOCUS) {
                draw_osver(&ui);
                reliefos_gui_present_window((uint32_t)window_id, OSVER_W, OSVER_H, OSVER_W, pixels);
            }
        }
        sleep_ms(20);
    }
}
