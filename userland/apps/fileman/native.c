#include "fileman.h"

int main(int argc, char **argv, char **envp)
{
    setlocale(LC_ALL, "");
    bindtextdomain("leonos", RELIEFOS_LAYOUT_LOCALE);
    textdomain("leonos");
    struct reliefos_gui_app_event event;
    int window_id;
    (void)envp;

    puts("[fileman.elf] file manager starting");
    printf("[fileman.elf] pid=%d creating GUI window\n", getpid());
    window_id = reliefos_gui_create_app_window_ex(T("File Manager"), T("ReliefOS file browser"),
                                                FILEMAN_W, FILEMAN_H, 0);
    if (window_id <= 0) {
        printf("[fileman.elf] create window failed=%d\n", window_id);
        return 1;
    }

    fileman_window_id = (uint32_t)window_id;
    reliefos_ui_bind(&fileman_ui, pixels, view_w, view_h, FILEMAN_MAX_W);
    reliefos_ui_listview_state_init(&file_list, current_layout().visible_rows, ROW_H);
    file_list.focused = 1;
    refresh_home_path();
    fileman_settings_load();
    fileman_tree_reset();
    if (argc > 1 && argv && argv[1] && argv[1][0]) {
        copy_text(current_path, sizeof(current_path), argv[1]);
    } else if (home_path[0]) {
        copy_text(current_path, sizeof(current_path), home_path);
    }
    copy_text(address_input, sizeof(address_input), current_path);
    reliefos_ui_edit_state_init(&address_edit, address_input, sizeof(address_input));
    if (navigate_to_path(current_path) < 0 && !text_eq(current_path, "/")) {
        navigate_to_path("/");
    }
    present_fileman(fileman_window_id, &fileman_ui);

    for (;;) {
        event.window_id = (uint32_t)window_id;
        if (reliefos_gui_wait_app_event(&event,
                                      context_menu_animating ? 20U : RELIEFOS_GUI_IDLE_WAIT_MS) > 0) {
            if (event.type == RELIEFOS_GUI_APP_EVENT_CLOSE) {
                return 0;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_MOUSE_BUTTON && (event.buttons & 3u)) {
                if (event.buttons & 2u) {
                    handle_right_click(event.x, event.y);
                } else {
                    handle_click(event.x, event.y);
                }
                file_list.focused = 1;
                present_fileman(fileman_window_id, &fileman_ui);
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_KEY_DOWN ||
                event.type == RELIEFOS_GUI_APP_EVENT_KEY_UP) {
                handle_key(event.keycode, event.pressed);
                present_fileman(fileman_window_id, &fileman_ui);
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_MOUSE_WHEEL) {
                if (handle_wheel(event.x, event.y, event.dy)) {
                    present_fileman(fileman_window_id, &fileman_ui);
                }
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_RESIZE ||
                event.type == RELIEFOS_GUI_APP_EVENT_FOCUS) {
                if (event.width >= 320) {
                    view_w = event.width > FILEMAN_MAX_W ? FILEMAN_MAX_W : event.width;
                }
                if (event.height >= 240) {
                    view_h = event.height > FILEMAN_MAX_H ? FILEMAN_MAX_H : event.height;
                }
                file_list.visible_rows = current_layout().visible_rows;
                reliefos_ui_listview_state_set_count(&file_list, entry_count);
                present_fileman(fileman_window_id, &fileman_ui);
            }
        } else if (context_menu_animating) {
            present_fileman(fileman_window_id, &fileman_ui);
            sleep_ms(10);
            continue;
        } else {
            sleep_ms(10);
        }
    }
}
