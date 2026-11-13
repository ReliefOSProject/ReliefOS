#include "browser.h"

#include <reliefos/text_input.h>

static void browser_update_inputm_context(void)
{
    text_input_context_t context = {
        .window_id = (uint32_t)window_id,
        .flags = 0,
    };
    if (address_edit.focused || browser_form_focus_active) {
        context.flags |= TEXT_INPUT_CONTEXT_FOCUSED;
    }
    if (browser_form_focus_active &&
        browser_form_focus_control < browser_form_control_count &&
        browser_form_controls[browser_form_focus_control].kind ==
            BROWSER_FORM_CONTROL_PASSWORD) {
        context.flags |= TEXT_INPUT_CONTEXT_SECURE;
    }
    (void)text_input_set_context(&context);
}

int main(int argc, char **argv, char **envp)
{
    setlocale(LC_ALL, "");
    bindtextdomain("browser", RELIEFOS_LAYOUT_LOCALE);
    textdomain("browser");
    struct reliefos_gui_app_event event;
    const char *initial = "about:reliefos";
    (void)envp;
    puts("[browser.elf] browser starting");
    if (argc > 1 && argv && argv[1] && argv[1][0]) {
        initial = argv[1];
    }
    window_id = reliefos_gui_create_app_window_ex(T("ReliefOS Browser"),
                                                T("Classic Web Browser"),
                                                view_w, view_h, 0);
    if (window_id <= 0) {
        printf("[browser.elf] create window failed=%d\n", window_id);
        return 1;
    }
    reliefos_ui_set_font_path(BROWSER_FONT_PATH);
    reliefos_ui_set_font_fallback_path(BROWSER_FONT_FALLBACK_PATH);
    reliefos_ui_bind(&ui, pixels, view_w, view_h, BROWSER_MAX_W);
    reliefos_ui_edit_state_init(&address_edit, address_input, sizeof(address_input));
    address_edit.focused = 1;
    browser_bookmarks_load();
    load_about();
    if (!text_eq(initial, "about:reliefos")) {
        navigate_to(initial, 1);
    } else {
        push_history(current_location);
    }
    present_browser();
    for (;;) {
        browser_update_inputm_context();
        event.window_id = (uint32_t)window_id;
        if (reliefos_gui_wait_app_event(&event,
                                      browser_toast.active ? 20U : RELIEFOS_GUI_IDLE_WAIT_MS) > 0) {
            if (event.type == RELIEFOS_GUI_APP_EVENT_CLOSE) {
                return 0;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_MOUSE_BUTTON) {
                handle_mouse_button(&event);
                if (browser_should_exit) {
                    return 0;
                }
                continue;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_MOUSE_WHEEL) {
                if (reliefos_ui_vscrollbar_handle_wheel(&scroll_line,
                                                      line_count ? line_count : 1U,
                                                      visible_rows(), event.dy)) {
                    present_browser();
                }
                continue;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_KEY_DOWN ||
                event.type == RELIEFOS_GUI_APP_EVENT_KEY_UP) {
                if (event.pressed && event.keycode == 1) {
                    return 0;
                }
                handle_key(&event);
                continue;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_RESIZE ||
                event.type == RELIEFOS_GUI_APP_EVENT_FOCUS) {
                if (event.width) {
                    view_w = event.width > BROWSER_MAX_W ? BROWSER_MAX_W : event.width;
                    if (view_w < BROWSER_MIN_W) {
                        view_w = BROWSER_MIN_W;
                    }
                }
                if (event.height) {
                    view_h = event.height > BROWSER_MAX_H ? BROWSER_MAX_H : event.height;
                    if (view_h < BROWSER_MIN_H) {
                        view_h = BROWSER_MIN_H;
                    }
                }
                rerender_page();
                present_browser();
                continue;
            }
        } else if (browser_toast.active) {
            present_browser();
            sleep_ms(10);
        } else {
            sleep_ms(10);
        }
    }
}
