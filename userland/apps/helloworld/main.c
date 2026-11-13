#include <reliefos/gui.h>
#include <libintl.h>
#include <locale.h>
#include <reliefos/layout.h>
#include <reliefos/ui.h>

#define T(s) gettext(s)

#define WIN_W 320U
#define WIN_H 200U

static uint32_t pixels[WIN_W * WIN_H];

int main(void)
{
    setlocale(LC_ALL, "");
    bindtextdomain("helloworld", RELIEFOS_LAYOUT_LOCALE);
    textdomain("helloworld");
    struct reliefos_gui_app_event event;
    struct reliefos_ui_surface ui;
    int window_id;
    int done = 0;

    window_id = reliefos_gui_create_app_window_ex(
        T("Hello World"),
        T("helloworld"),
        WIN_W, WIN_H, RELIEFOS_GUI_WINDOW_NO_RESIZE);
    if (window_id <= 0) {
        return 1;
    }
    reliefos_ui_bind(&ui, pixels, WIN_W, WIN_H, WIN_W);

    while (!done) {
        reliefos_ui_rect(&ui, 0, 0, WIN_W, WIN_H, RELIEFOS_UI_WHITE);
        reliefos_ui_text(&ui, 80, 60,
                       T("Hello, World!"),
                       RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
        reliefos_ui_text(&ui, 50, 100,
                       T("Installed via API package"),
                       RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
        reliefos_ui_button(&ui, WIN_W / 2U - 36U, WIN_H - 52U, 72U,
                         RELIEFOS_UI_BUTTON_H,
                         T("OK"), 0);
        reliefos_gui_present_window((uint32_t)window_id, WIN_W, WIN_H,
                                  WIN_W, pixels);
        event.window_id = (uint32_t)window_id;
        if (reliefos_gui_wait_app_event(&event, RELIEFOS_GUI_IDLE_WAIT_MS) > 0) {
            if (event.type == RELIEFOS_GUI_APP_EVENT_CLOSE) {
                done = 1;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_MOUSE_BUTTON &&
                (event.buttons & 1U)) {
                if (event.x >= (int32_t)(WIN_W / 2U - 36U) &&
                    event.x < (int32_t)(WIN_W / 2U + 36U) &&
                    event.y >= (int32_t)(WIN_H - 52U) &&
                    event.y < (int32_t)(WIN_H - 52U + RELIEFOS_UI_BUTTON_H)) {
                    done = 1;
                }
            }
        }
    }
    reliefos_gui_destroy_app_window((uint32_t)window_id);
    return 0;
}
