#include "fileman.h"

int handle_menu_click(int32_t x, int32_t y)
{
    struct reliefos_ui_menubar_item menu_items[] = {
        {T("File"), FILEMAN_MENU_FILE, 54, 0},
        {T("View"), FILEMAN_MENU_VIEW, 54, 0},
        {T("Edit"), FILEMAN_MENU_EDIT, 54, 0},
    };
    uint32_t action = 0;
    if (reliefos_ui_menubar_hit(x, y, 0, 0, menu_items,
                              sizeof(menu_items) / sizeof(menu_items[0]),
                              &action)) {
        if (action) {
            menu_open = menu_open == action ? FILEMAN_MENU_NONE : (uint8_t)action;
            return 1;
        }
        menu_open = FILEMAN_MENU_NONE;
        return 1;
    }
    if (menu_open == FILEMAN_MENU_FILE) {
        struct reliefos_ui_context_menu_item items[FILEMAN_FILE_MENU_COUNT];
        struct reliefos_ui_rect r;
        build_file_menu_items(items, FILEMAN_FILE_MENU_COUNT);
        reliefos_ui_menubar_item_rect(0, 0, menu_items,
                                    sizeof(menu_items) / sizeof(menu_items[0]),
                                    FILEMAN_MENU_FILE, &r);
        if (reliefos_ui_menu_popup_hit(x, y, (uint32_t)r.x, MENU_BAR_H, 204,
                                     items, FILEMAN_FILE_MENU_COUNT, &action)) {
            menu_open = FILEMAN_MENU_NONE;
            if (action) {
                execute_action(action);
            }
            return 1;
        }
        menu_open = FILEMAN_MENU_NONE;
        return 1;
    }
    if (menu_open == FILEMAN_MENU_VIEW) {
        struct reliefos_ui_context_menu_item items[] = {
            {T("Refresh"), FILEMAN_ACTION_REFRESH, 0},
            {T("Root"), FILEMAN_ACTION_ROOT, 0},
            {T("Settings..."), FILEMAN_ACTION_SETTINGS, 0},
            {T("About"), FILEMAN_ACTION_ABOUT, 0},
        };
        struct reliefos_ui_rect r;
        reliefos_ui_menubar_item_rect(0, 0, menu_items,
                                    sizeof(menu_items) / sizeof(menu_items[0]),
                                    FILEMAN_MENU_VIEW, &r);
        if (reliefos_ui_menu_popup_hit(x, y, (uint32_t)r.x, MENU_BAR_H, 170,
                                     items, sizeof(items) / sizeof(items[0]),
                                     &action)) {
            menu_open = FILEMAN_MENU_NONE;
            if (action == FILEMAN_ACTION_REFRESH) {
                reload_dir();
            } else if (action == FILEMAN_ACTION_ROOT) {
                navigate_root();
            } else if (action == FILEMAN_ACTION_SETTINGS) {
                fileman_open_settings();
            } else if (action == FILEMAN_ACTION_ABOUT) {
                reliefos_ui_show_message_box(T("File Manager"), T("Browse files and launch apps."), "OK");
            }
            return 1;
        }
        menu_open = FILEMAN_MENU_NONE;
        return 1;
    }
    if (menu_open == FILEMAN_MENU_EDIT) {
        struct reliefos_ui_context_menu_item items[FILEMAN_EDIT_MENU_COUNT];
        struct reliefos_ui_rect r;
        build_edit_menu_items(items, FILEMAN_EDIT_MENU_COUNT);
        reliefos_ui_menubar_item_rect(0, 0, menu_items,
                                    sizeof(menu_items) / sizeof(menu_items[0]),
                                    FILEMAN_MENU_EDIT, &r);
        if (reliefos_ui_menu_popup_hit(x, y, (uint32_t)r.x, MENU_BAR_H, 190,
                                     items, FILEMAN_EDIT_MENU_COUNT, &action)) {
            menu_open = FILEMAN_MENU_NONE;
            if (action) {
                execute_action(action);
            }
            return 1;
        }
        menu_open = FILEMAN_MENU_NONE;
        return 1;
    }
    return 0;
}

int handle_context_menu_click(int32_t x, int32_t y)
{
    struct reliefos_ui_context_menu_item items[FILEMAN_CONTEXT_MENU_COUNT];
    uint32_t action = 0;
    if (!context_menu_active) {
        return 0;
    }
    build_context_menu_items(items, FILEMAN_CONTEXT_MENU_COUNT);
    if (reliefos_ui_context_menu_hit(x, y, context_menu_x, context_menu_y,
                                   FILEMAN_CONTEXT_MENU_W, items,
                                   FILEMAN_CONTEXT_MENU_COUNT, &action)) {
        context_menu_set_active(0);
        if (action) {
            execute_action(action);
        }
        return 1;
    }
    context_menu_set_active(0);
    return 0;
}

void show_context_menu_at(int32_t x, int32_t y, int32_t target)
{
    uint32_t menu_h = reliefos_ui_context_menu_height(FILEMAN_CONTEXT_MENU_COUNT);
    menu_open = FILEMAN_MENU_NONE;
    if (target >= 0 && (uint32_t)target < entry_count) {
        file_list.selected = target;
        if ((uint32_t)target < file_list.scroll) {
            file_list.scroll = (uint32_t)target;
        } else if ((uint32_t)target >= file_list.scroll + file_list.visible_rows) {
            file_list.scroll = (uint32_t)target - file_list.visible_rows + 1;
        }
    } else {
        file_list.selected = -1;
    }
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    context_menu_x = (uint32_t)x;
    context_menu_y = (uint32_t)y;
    if (context_menu_x + FILEMAN_CONTEXT_MENU_W > view_w) {
        context_menu_x = view_w > FILEMAN_CONTEXT_MENU_W ? view_w - FILEMAN_CONTEXT_MENU_W : 0;
    }
    if (context_menu_y + menu_h > view_h - STATUS_H) {
        context_menu_y = view_h - STATUS_H > menu_h
                             ? view_h - STATUS_H - menu_h
                             : 0;
    }
    context_menu_set_active(1);
}

void handle_right_click(int32_t x, int32_t y)
{
    int32_t index = list_index_at(x, y);
    if (index >= 0) {
        set_status(entries[index].name);
    } else {
        set_status("Folder actions");
    }
    show_context_menu_at(x, y, index);
}

void handle_click(int32_t x, int32_t y)
{
    struct fileman_layout l = current_layout();
    uint32_t address_w = view_w > 238 ? view_w - 238 :
                         (view_w > 230 ? view_w - 230 : 1);
    if (fileman_settings_open) {
        (void)fileman_handle_settings_click(x, y);
        return;
    }
    if (handle_context_menu_click(x, y)) {
        return;
    }
    if (handle_menu_click(x, y)) {
        context_menu_set_active(0);
        return;
    }
    menu_open = FILEMAN_MENU_NONE;
    context_menu_set_active(0);
    if (hit_rect_i(x, y, 230, TOOLBAR_Y, (int32_t)address_w,
                   (int32_t)(RELIEFOS_FONT_H + 8))) {
        (void)reliefos_ui_edit_state_handle_mouse(&address_edit, x, y,
                                                230, TOOLBAR_Y, address_w,
                                                1U);
        return;
    }
    if (address_edit.focused) {
        address_edit_sync_path();
    }
    if (x >= 8 && x < 62 && y >= TOOLBAR_Y && y < TOOLBAR_Y + (int32_t)RELIEFOS_UI_BUTTON_H) {
        navigate_up();
        return;
    }
    if (x >= 72 && x < 132 && y >= TOOLBAR_Y && y < TOOLBAR_Y + (int32_t)RELIEFOS_UI_BUTTON_H) {
        open_selected_entry();
        return;
    }
    if (x >= 142 && x < 218 && y >= TOOLBAR_Y && y < TOOLBAR_Y + (int32_t)RELIEFOS_UI_BUTTON_H) {
        reload_dir();
        return;
    }
    if (l.tree_w && hit_rect_i(x, y, (int32_t)l.tree_x, (int32_t)l.tree_y,
                               (int32_t)l.tree_w, (int32_t)l.tree_h)) {
        struct reliefos_ui_tree_item tree_items[FILEMAN_TREE_MAX_NODES];
        uint32_t tree_count = build_tree_items(tree_items, sizeof(tree_items) / sizeof(tree_items[0]));
        uint32_t tree_rows = fileman_tree_visible_rows(&l);
        uint32_t tree_first = fileman_tree_scroll;
        uint32_t tree_visible;
        uint32_t id = 0;
        if (tree_first > (tree_count > tree_rows ? tree_count - tree_rows : 0)) {
            tree_first = tree_count > tree_rows ? tree_count - tree_rows : 0;
            fileman_tree_scroll = tree_first;
        }
        if (x >= (int32_t)(l.tree_x + l.tree_w - 20)) {
            reliefos_ui_vscrollbar_handle_mouse(&fileman_tree_scroll,
                                              tree_count > tree_rows ? tree_count : tree_rows,
                                              tree_rows, l.tree_x + l.tree_w - 20,
                                              l.tree_y + 2, 18,
                                              l.tree_h > 4 ? l.tree_h - 4 : 1, x, y);
            return;
        }
        tree_visible = tree_count > tree_first ? tree_count - tree_first : 0;
        if (tree_visible > tree_rows) {
            tree_visible = tree_rows;
        }
        if (reliefos_ui_tree_hit(x, y, l.tree_x + 2, l.tree_y + 4,
                               l.tree_w > 22 ? l.tree_w - 22 : 1,
                               tree_items + tree_first, tree_visible,
                               TREE_ROW_H, &id)) {
            const char *path = tree_path_for_id(id);
            uint32_t index = ((uint32_t)y - (l.tree_y + 4)) / TREE_ROW_H;
            uint32_t indent = index < tree_visible ? tree_items[tree_first + index].depth * 14U : 0;
            uint32_t glyph_x = l.tree_x + 6U + indent;
            if (index < tree_visible && x >= (int32_t)glyph_x && x < (int32_t)(glyph_x + 12U)) {
                (void)fileman_tree_toggle(id);
            } else if (path) {
                navigate_to_path(path);
            }
        }
        return;
    }
    if (x >= (int32_t)l.scrollbar_x && y >= (int32_t)(l.list_y + 2) &&
        y < (int32_t)(l.list_y + l.scrollbar_h)) {
        reliefos_ui_vscrollbar_handle_mouse(&file_list.scroll,
                                          entry_count > l.visible_rows ? entry_count : l.visible_rows,
                                          l.visible_rows,
                                          l.scrollbar_x, l.list_y + 2, 18,
                                          l.scrollbar_h - 4,
                                          x, y);
        return;
    }
    {
        uint32_t activate = 0;
        int32_t before = file_list.selected;
        unsigned long now = reliefos_uptime_ms();
        int32_t row = (y - (int32_t)l.rows_y) / (int32_t)ROW_H;
        uint32_t index;
        if (row < 0) {
            return;
        }
        index = file_list.scroll + (uint32_t)row;
        if (index >= entry_count) {
            last_click_index = -1;
            last_click_ms = 0;
            return;
        }
        if (!reliefos_ui_listview_state_handle_mouse(&file_list, x, y, l.list_x + 2,
                                                   l.rows_y, l.list_w, &activate)) {
            return;
        }
        activate = before == file_list.selected &&
                   file_list.selected >= 0 &&
                   last_click_index == file_list.selected &&
                   now - last_click_ms <= 500;
        last_click_index = file_list.selected;
        last_click_ms = now;
        if (activate) {
            open_selected_entry();
        } else if (file_list.selected >= 0 && (uint32_t)file_list.selected < entry_count) {
            set_status(entries[file_list.selected].name);
        }
    }
}

void handle_key(uint8_t keycode, uint8_t pressed)
{
    uint32_t activate = 0;
    if (fileman_settings_open) {
        if (pressed) {
            (void)fileman_handle_settings_key(keycode);
        }
        return;
    }
    if (address_edit.focused) {
        if (pressed && keycode == FILEMAN_KEY_ESCAPE) {
            address_edit.focused = 0;
            address_edit_sync_path();
            return;
        }
        if (pressed && keycode == RELIEFOS_KEY_ENTER) {
            char path[RELIEFOS_FS_PATH_LEN];
            copy_text(path, sizeof(path), address_input);
            (void)navigate_to_path(path);
            return;
        }
        (void)reliefos_ui_edit_state_handle_key(&address_edit, keycode, pressed);
        return;
    }
    if (!pressed) {
        return;
    }
    if (keycode == RELIEFOS_KEY_DELETE) {
        execute_action(FILEMAN_ACTION_DELETE);
        return;
    }
    if (reliefos_ui_listview_state_handle_key(&file_list, keycode, &activate)) {
        if (activate) {
            open_selected_entry();
        } else if (file_list.selected >= 0 && (uint32_t)file_list.selected < entry_count) {
            set_status(entries[file_list.selected].name);
        }
    }
}

int handle_wheel(int32_t x, int32_t y, int32_t wheel)
{
    struct fileman_layout l = current_layout();
    if (l.tree_w && hit_rect_i(x, y, (int32_t)l.tree_x, (int32_t)l.tree_y,
                               (int32_t)l.tree_w, (int32_t)l.tree_h)) {
        struct reliefos_ui_tree_item tree_items[FILEMAN_TREE_MAX_NODES];
        uint32_t tree_count = build_tree_items(tree_items, sizeof(tree_items) / sizeof(tree_items[0]));
        return reliefos_ui_vscrollbar_handle_wheel(&fileman_tree_scroll,
                                                 tree_count,
                                                 fileman_tree_visible_rows(&l), wheel);
    }
    if (hit_rect_i(x, y, (int32_t)l.list_x, (int32_t)l.list_y,
                   (int32_t)(l.list_w + 24), (int32_t)l.list_h)) {
        return reliefos_ui_listview_state_handle_wheel(&file_list, wheel);
    }
    return 0;
}
