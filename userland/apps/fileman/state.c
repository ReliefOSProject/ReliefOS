#include "fileman.h"

struct reliefos_dir_entry entries[FILEMAN_MAX_ENTRIES];
char current_path[RELIEFOS_FS_PATH_LEN] = "/";
char home_path[RELIEFOS_AUTH_HOME_LEN];
char status_text[160] = "Ready";
uint32_t entry_count;
struct reliefos_ui_listview_state file_list;
int32_t last_click_index = -1;
unsigned long last_click_ms;
uint8_t menu_open;
uint8_t context_menu_active;
uint8_t context_menu_animating;
uint8_t context_menu_opening;
unsigned long context_menu_anim_start;
uint32_t context_menu_x;
uint32_t context_menu_y;
uint32_t view_w = FILEMAN_W;
uint32_t view_h = FILEMAN_H;
uint64_t selected_mask;
uint32_t fileman_window_id;
struct reliefos_ui_surface fileman_ui;
char address_input[RELIEFOS_FS_PATH_LEN];
struct reliefos_ui_edit_state address_edit;
uint8_t fileman_operation_active;
uint32_t fileman_operation_percent;
char fileman_operation_text[160];
struct fileman_tree_node fileman_tree_nodes[FILEMAN_TREE_MAX_NODES];
uint32_t fileman_tree_node_count;
uint32_t fileman_tree_next_id;
uint32_t fileman_tree_scroll;
uint8_t fileman_show_hidden;
uint8_t fileman_settings_open;
uint8_t fileman_settings_show_hidden;

struct fileman_layout current_layout(void)
{
    struct fileman_layout l;
    uint32_t content_h = view_h > LIST_Y + STATUS_H + 10 ? view_h - LIST_Y - STATUS_H - 10 : ROW_H * 2;
    struct reliefos_ui_split_pane_state split;
    if (view_w > 430) {
        reliefos_ui_split_pane_init(&split, RELIEFOS_UI_SPLIT_VERTICAL, TREE_W, 96, 220);
        split.splitter_size = 8;
        reliefos_ui_split_pane_layout(&split, 8, LIST_Y,
                                    view_w > 34 ? view_w - 34 : view_w,
                                    content_h + 4);
        l.tree_x = (uint32_t)split.first.x;
        l.tree_y = (uint32_t)split.first.y;
        l.tree_w = split.first.w;
        l.tree_h = split.first.h;
        l.list_x = (uint32_t)split.second.x;
        l.list_y = (uint32_t)split.second.y;
        l.list_w = split.second.w > 22 ? split.second.w - 22 : split.second.w;
        l.list_h = split.second.h;
    } else {
        l.tree_x = 8;
        l.tree_y = LIST_Y;
        l.tree_w = 0;
        l.tree_h = content_h + 4;
        l.list_x = 8;
        l.list_y = LIST_Y;
        l.list_w = view_w > l.list_x + 34 ? view_w - l.list_x - 26 : 220;
        l.list_h = content_h + 4;
    }
    l.rows_y = l.list_y + 30;
    l.rows_h = l.list_h > 34 ? l.list_h - 34 : ROW_H;
    l.visible_rows = l.rows_h / ROW_H;
    if (!l.visible_rows) {
        l.visible_rows = 1;
    }
    l.scrollbar_x = l.list_x + l.list_w + 2;
    l.scrollbar_h = l.list_h;
    return l;
}
