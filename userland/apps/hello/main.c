#include <reliefos/gui.h>
#include <libintl.h>
#include <locale.h>
#include <reliefos/layout.h>
#include <reliefos/psf_font.h>
#include <reliefos/stdio.h>
#include <reliefos/syscall.h>
#include <reliefos/ui.h>

#define HELLO_W 300
#define HELLO_H 150
#define T(s) gettext(s)

static uint32_t pixels[HELLO_W * HELLO_H];

static void draw_hello(struct reliefos_ui_surface *ui, uint32_t hover)
{
    reliefos_ui_rect(ui, 0, 0, HELLO_W, HELLO_H, RELIEFOS_UI_WHITE);
    reliefos_ui_panel(ui, 10, 10, HELLO_W - 20, HELLO_H - 20, RELIEFOS_UI_LIGHT);
    reliefos_ui_text(ui, 22, 26, T("Hello from hello.elf"), RELIEFOS_UI_BLACK, RELIEFOS_UI_LIGHT);
    reliefos_ui_text(ui, 22, 50, T("This window is rendered by the app"), RELIEFOS_UI_DARK, RELIEFOS_UI_LIGHT);
    reliefos_ui_button(ui, 22, 88, 88, RELIEFOS_UI_BUTTON_H, T("Hello"),
                     hover ? RELIEFOS_UI_BUTTON_PRESSED : 0);
    reliefos_ui_text(ui, 124, 92, hover ? T("Button pressed") : T("Window server v2"), RELIEFOS_UI_BLACK, RELIEFOS_UI_LIGHT);
}

int main(void)
{
    setlocale(LC_ALL, "");
    bindtextdomain("hello", RELIEFOS_LAYOUT_LOCALE);
    textdomain("hello");
    struct reliefos_ui_surface ui;
    struct reliefos_gui_app_event event;
    int window_id;
    uint32_t hover = 0;
    puts("[hello.elf] hello from ReliefOS Ring-3 ELF");
    printf("[hello.elf] pid=%d creating GUI window\n", getpid());
    window_id = reliefos_gui_create_app_window_ex(T("Hello"), T("Hello from hello.elf process"),
                                                HELLO_W, HELLO_H, RELIEFOS_GUI_WINDOW_NO_RESIZE);
    if (window_id <= 0) {
        printf("[hello.elf] create window failed=%d\n", window_id);
        return 1;
    }
    reliefos_ui_bind(&ui, pixels, HELLO_W, HELLO_H, HELLO_W);
    draw_hello(&ui, hover);
    reliefos_gui_present_window((uint32_t)window_id, HELLO_W, HELLO_H, HELLO_W, pixels);
    for (;;) {
        event.window_id = (uint32_t)window_id;
        if (reliefos_gui_wait_app_event(&event, RELIEFOS_GUI_IDLE_WAIT_MS) > 0) {
            if (event.type == 1) {
                break;
            }
            if (event.type == 6) {
                hover = event.x >= 22 && event.x < 110 && event.y >= 88 && event.y < 112;
                draw_hello(&ui, hover);
                reliefos_gui_present_window((uint32_t)window_id, HELLO_W, HELLO_H, HELLO_W, pixels);
            }
            if (event.type == 4) {
                draw_hello(&ui, hover);
                reliefos_gui_present_window((uint32_t)window_id, HELLO_W, HELLO_H, HELLO_W, pixels);
            }
        } else {
            sleep_ms(10);
        }
    }
    return 0;
}
