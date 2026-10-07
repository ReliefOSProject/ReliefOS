#include "fileman.h"

void draw_fileman(struct reliefos_ui_surface *ui)
{
    struct fileman_layout l = current_layout();
    struct reliefos_ui_menubar_item menu_items[] = {
        {T("File"), FILEMAN_MENU_FILE, 54, 0},
        {T("View"), FILEMAN_MENU_VIEW, 54, 0},
        {T("Edit"), FILEMAN_MENU_EDIT, 54, 0},
    };
    struct reliefos_ui_list_column cols[] = {
        {T("Type"), 58},
        {T("Name"), l.list_w > 58 ? l.list_w - 58 : 120},
    };
    struct reliefos_ui_tree_item tree_items[FILEMAN_TREE_MAX_NODES];
    uint32_t tree_count = build_tree_items(tree_items, sizeof(tree_items) / sizeof(tree_items[0]));
    uint32_t tree_rows = fileman_tree_visible_rows(&l);
    uint32_t tree_first = fileman_tree_scroll;
    uint32_t tree_visible;
    for (uint32_t i = 0; i < tree_count; ++i) {
        const char *path = tree_path_for_id(tree_items[i].id);
        if (text_eq(current_path, path)) {
            tree_items[i].flags |= RELIEFOS_UI_TREE_SELECTED;
        }
    }
    file_list.visible_rows = l.visible_rows;
    reliefos_ui_listview_state_set_count(&file_list, entry_count);
    reliefos_ui_rect(ui, 0, 0, view_w, view_h, RELIEFOS_UI_WHITE);
    reliefos_ui_menubar_draw(ui, 0, 0, view_w, menu_items,
                           sizeof(menu_items) / sizeof(menu_items[0]),
                           menu_open);

    reliefos_ui_toolbar(ui, 0, 30, view_w, 42);
    reliefos_ui_toolbar_button(ui, 8, TOOLBAR_Y, 54, T("Up"), 0);
    reliefos_ui_toolbar_button(ui, 72, TOOLBAR_Y, 60, T("Open"), 0);
    reliefos_ui_toolbar_button(ui, 142, TOOLBAR_Y, 76, T("Refresh"), 0);
    reliefos_ui_edit_state_draw(ui, 230, TOOLBAR_Y,
                              view_w > 238 ? view_w - 238 :
                              (view_w > 230 ? view_w - 230 : 1),
                              &address_edit, 0);

    if (tree_first > (tree_count > tree_rows ? tree_count - tree_rows : 0)) {
        tree_first = tree_count > tree_rows ? tree_count - tree_rows : 0;
        fileman_tree_scroll = tree_first;
    }
    tree_visible = tree_count > tree_first ? tree_count - tree_first : 0;
    if (tree_visible > tree_rows) {
        tree_visible = tree_rows;
    }

    if (l.tree_w) {
        reliefos_ui_scroll_view_frame(ui, l.tree_x, l.tree_y, l.tree_w, l.tree_h);
        reliefos_ui_tree(ui, l.tree_x + 2, l.tree_y + 4, l.tree_w > 22 ? l.tree_w - 22 : 1,
                       tree_items + tree_first, tree_visible, TREE_ROW_H);
        reliefos_ui_vscrollbar(ui, l.tree_x + l.tree_w - 20, l.tree_y + 2, 18,
                             l.tree_h > 4 ? l.tree_h - 4 : 1,
                             fileman_tree_scroll,
                             tree_count > tree_rows ? tree_count : tree_rows,
                             tree_rows,
                             tree_count <= tree_rows ? RELIEFOS_UI_SCROLLBAR_DISABLED : 0);
        reliefos_ui_splitter(ui, l.tree_x + l.tree_w, l.tree_y,
                           l.list_x > l.tree_x + l.tree_w ? l.list_x - l.tree_x - l.tree_w : 8,
                           l.tree_h, RELIEFOS_UI_SPLIT_VERTICAL);
    }
    reliefos_ui_scroll_view_frame(ui, l.list_x, l.list_y, l.list_w + 22, l.list_h);
    reliefos_ui_listview_header(ui, l.list_x + 2, l.list_y + 2, l.list_w, cols, 2);
    for (uint32_t row = 0; row < file_list.visible_rows; ++row) {
        uint32_t i = file_list.scroll + row;
        const char *cells[2];
        if (i >= entry_count) {
            break;
        }
        cells[0] = entry_type_name(&entries[i]);
        cells[1] = entries[i].name;
        reliefos_ui_listview_row(ui, l.list_x + 2, l.rows_y + row * ROW_H, l.list_w, cols, cells, 2,
                               file_list.selected == (int32_t)i ? RELIEFOS_UI_MENU_SELECTED : 0);
        if (fileman_entry_marked(i)) {
            reliefos_ui_text(ui, l.list_x + l.list_w - 14, l.rows_y + row * ROW_H + 4,
                           "*", 0x00007000U,
                           file_list.selected == (int32_t)i ? RELIEFOS_UI_ACTIVE_TITLE : RELIEFOS_UI_WHITE);
        }
    }
    reliefos_ui_vscrollbar(ui, l.scrollbar_x, l.list_y + 2, 18, l.scrollbar_h - 4,
                         file_list.scroll, entry_count > l.visible_rows ? entry_count : l.visible_rows,
                         l.visible_rows,
                         entry_count <= l.visible_rows ? RELIEFOS_UI_SCROLLBAR_DISABLED : 0);

    reliefos_ui_statusbar(ui, view_h - STATUS_H, STATUS_H, status_text);
    if (fileman_operation_active) {
        reliefos_ui_progress(ui, 12, view_h - STATUS_H - 24, view_w > 24 ? view_w - 24 : 1,
                           16, fileman_operation_percent, 100);
        reliefos_ui_text_clipped(ui, 16, view_h - STATUS_H - 46,
                               view_w > 32 ? view_w - 32 : 1,
                               fileman_operation_text, RELIEFOS_UI_DARK,
                               RELIEFOS_UI_WHITE);
    }

    if (menu_open == FILEMAN_MENU_FILE) {
        struct reliefos_ui_context_menu_item items[FILEMAN_FILE_MENU_COUNT];
        struct reliefos_ui_rect r;
        build_file_menu_items(items, FILEMAN_FILE_MENU_COUNT);
        reliefos_ui_menubar_item_rect(0, 0, menu_items,
                                    sizeof(menu_items) / sizeof(menu_items[0]),
                                    FILEMAN_MENU_FILE, &r);
        reliefos_ui_menu_popup(ui, (uint32_t)r.x, MENU_BAR_H, 204,
                             items, FILEMAN_FILE_MENU_COUNT, 0);
    } else if (menu_open == FILEMAN_MENU_VIEW) {
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
        reliefos_ui_menu_popup(ui, (uint32_t)r.x, MENU_BAR_H, 170,
                             items, sizeof(items) / sizeof(items[0]), 0);
    } else if (menu_open == FILEMAN_MENU_EDIT) {
        struct reliefos_ui_context_menu_item items[FILEMAN_EDIT_MENU_COUNT];
        struct reliefos_ui_rect r;
        build_edit_menu_items(items, FILEMAN_EDIT_MENU_COUNT);
        reliefos_ui_menubar_item_rect(0, 0, menu_items,
                                    sizeof(menu_items) / sizeof(menu_items[0]),
                                    FILEMAN_MENU_EDIT, &r);
        reliefos_ui_menu_popup(ui, (uint32_t)r.x, MENU_BAR_H, 190,
                             items, FILEMAN_EDIT_MENU_COUNT, 0);
    }
    if (context_menu_active || context_menu_animating) {
        struct reliefos_ui_context_menu_item items[FILEMAN_CONTEXT_MENU_COUNT];
        build_context_menu_items(items, FILEMAN_CONTEXT_MENU_COUNT);
        uint32_t progress = context_menu_animating
                                ? reliefos_ui_anim_progress(reliefos_uptime_ms(), context_menu_anim_start, 120)
                                : 1000;
        if (progress >= 1000) {
            context_menu_animating = 0;
            progress = context_menu_active ? 1000 : 0;
        } else if (!context_menu_opening) {
            progress = 1000 - progress;
        }
        reliefos_ui_context_menu_animated(ui, context_menu_x, context_menu_y, FILEMAN_CONTEXT_MENU_W,
                                        items, FILEMAN_CONTEXT_MENU_COUNT, progress);
    }
    if (fileman_settings_open) {
        draw_fileman_settings_dialog(ui);
    }
}

void draw_fileman_settings_dialog(struct reliefos_ui_surface *ui)
{
    struct reliefos_ui_rect rect;
    uint32_t button_y;
    if (!ui || !fileman_settings_open) {
        return;
    }
    fileman_settings_dialog_rect(&rect);
    button_y = (uint32_t)rect.y + rect.h - 34u;
    reliefos_ui_dialog(ui, (uint32_t)rect.x, (uint32_t)rect.y, rect.w, rect.h,
                     T("File Manager Settings"));
    reliefos_ui_text_clipped(ui, (uint32_t)rect.x + 18u, (uint32_t)rect.y + 36u,
                           rect.w > 36u ? rect.w - 36u : rect.w,
                           T("Display"), RELIEFOS_UI_BLACK, RELIEFOS_UI_GRAY);
    reliefos_ui_checkbox(ui, (uint32_t)rect.x + 18u, (uint32_t)rect.y + 56u, "",
                       fileman_settings_show_hidden, 0);
    reliefos_ui_text_clipped(ui, (uint32_t)rect.x + 40u, (uint32_t)rect.y + 58u,
                           rect.w > 58u ? rect.w - 58u : rect.w,
                           T("Show files and folders starting with a dot"),
                           RELIEFOS_UI_BLACK, RELIEFOS_UI_GRAY);
    reliefos_ui_text_clipped(ui, (uint32_t)rect.x + 18u, (uint32_t)rect.y + 90u,
                           rect.w > 36u ? rect.w - 36u : rect.w,
                           T("This preference is saved for the current user."),
                           RELIEFOS_UI_DARK, RELIEFOS_UI_GRAY);
    reliefos_ui_button(ui, (uint32_t)rect.x + rect.w - 168u, button_y,
                     72, RELIEFOS_UI_BUTTON_H, T("Save"), 0);
    reliefos_ui_button(ui, (uint32_t)rect.x + rect.w - 88u, button_y,
                     72, RELIEFOS_UI_BUTTON_H, T("Cancel"), 0);
}


void present_fileman(uint32_t window_id, struct reliefos_ui_surface *ui)
{
    reliefos_ui_bind(ui, pixels, view_w, view_h, FILEMAN_MAX_W);
    draw_fileman(ui);
    reliefos_gui_present_window(window_id, view_w, view_h, FILEMAN_MAX_W, pixels);
}
