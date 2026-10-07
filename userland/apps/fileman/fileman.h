#ifndef RELIEFOS_FILEMAN_H
#define RELIEFOS_FILEMAN_H

#include <reliefos/auth.h>
#include <reliefos/fs.h>
#include <reliefos/gui.h>
#include <libintl.h>
#include <locale.h>
#include <reliefos/layout.h>
#include <reliefos/launch.h>
#include <reliefos/psf_font.h>
#include <reliefos/stdio.h>
#include <reliefos/syscall.h>
#include <reliefos/ui.h>
#include <sys/stat.h>
#include "boundary.h"

int fileman_input_dialog(const char *title, const char *prompt, char *value, uint32_t capacity);
int fileman_confirm_dialog(const char *title, const char *message, uint32_t default_yes);
int fileman_launch_path(const char *path);
#define reliefos_ui_show_input_dialog fileman_input_dialog
#define reliefos_ui_show_confirm_dialog fileman_confirm_dialog

#define FILEMAN_W 560
#define FILEMAN_H 360
#define FILEMAN_MAX_W RELIEFOS_GUI_MAX_WINDOW_WIDTH
#define FILEMAN_MAX_H RELIEFOS_GUI_MAX_WINDOW_HEIGHT
#define FILEMAN_MAX_ENTRIES RELIEFOS_FS_MAX_ENTRIES
#define TOOLBAR_Y 40
#define LIST_X 8
#define LIST_Y 82
#define ROW_H (RELIEFOS_FONT_H + 8)
#define STATUS_H 28
#define TREE_W 132
#define TREE_ROW_H 24
#define FILEMAN_TREE_MAX_NODES 128
#define MENU_BAR_H 28
#define MENU_ITEM_H (RELIEFOS_FONT_H + 8)
#define FILEMAN_KEY_ESCAPE 1U
#define FILEMAN_KEY_UP 72U
#define FILEMAN_KEY_DOWN 80U
#define FILEMAN_CONTEXT_MENU_W 206
#define FILEMAN_CONTEXT_MENU_COUNT 13
#define FILEMAN_FILE_MENU_COUNT 9
#define FILEMAN_EDIT_MENU_COUNT 8
#define FILEMAN_DETAILS_W 560
#define FILEMAN_DETAILS_H 360
#define FILEMAN_FOLDER_SIZE_MAX_DEPTH 16
#define FILEMAN_FOLDER_SIZE_MAX_ITEMS 2048
#define FILEMAN_SETTINGS_DIALOG_W 340
#define FILEMAN_SETTINGS_DIALOG_H 166
#define T(s) gettext(s)

enum {
    FILEMAN_MENU_NONE = 0,
    FILEMAN_MENU_FILE = 1,
    FILEMAN_MENU_VIEW = 2,
    FILEMAN_MENU_EDIT = 3,
};

enum {
    FILEMAN_ACTION_OPEN = 1,
    FILEMAN_ACTION_OPEN_WITH = 2,
    FILEMAN_ACTION_DEFAULT_PROGRAM = 3,
    FILEMAN_ACTION_DETAILS = 4,
    FILEMAN_ACTION_RENAME = 5,
    FILEMAN_ACTION_DELETE = 6,
    FILEMAN_ACTION_NEW_FOLDER = 7,
    FILEMAN_ACTION_UP = 8,
    FILEMAN_ACTION_REFRESH = 9,
    FILEMAN_ACTION_CREATE_SHORTCUT = 10,
    FILEMAN_ACTION_ROOT = 11,
    FILEMAN_ACTION_ABOUT = 12,
    FILEMAN_ACTION_COPY = 13,
    FILEMAN_ACTION_CUT = 14,
    FILEMAN_ACTION_PASTE = 15,
    FILEMAN_ACTION_TOGGLE_MARK = 16,
    FILEMAN_ACTION_SELECT_ALL = 17,
    FILEMAN_ACTION_CLEAR_SELECTION = 18,
    FILEMAN_ACTION_EXTRACT_TAR = 23,
    FILEMAN_ACTION_COMPRESS_TAR = 24,
    FILEMAN_ACTION_COMPRESS_SELECTED = 25,
    FILEMAN_ACTION_SETTINGS = 26,
};

struct fileman_layout {
    uint32_t tree_x;
    uint32_t tree_y;
    uint32_t tree_w;
    uint32_t tree_h;
    uint32_t list_x;
    uint32_t list_y;
    uint32_t list_w;
    uint32_t list_h;
    uint32_t rows_y;
    uint32_t rows_h;
    uint32_t visible_rows;
    uint32_t scrollbar_x;
    uint32_t scrollbar_h;
};

struct folder_size_info {
    uint64_t bytes;
    uint32_t files;
    uint32_t folders;
    uint32_t visited;
    uint8_t partial;
};

struct fileman_tree_node {
    char path[RELIEFOS_FS_PATH_LEN];
    char label[RELIEFOS_FS_NAME_LEN];
    uint32_t id;
    uint32_t parent_id;
    uint8_t used;
    uint8_t expanded;
    uint8_t loaded;
    uint8_t has_children;
};

extern struct reliefos_dir_entry entries[FILEMAN_MAX_ENTRIES];
extern char current_path[RELIEFOS_FS_PATH_LEN];
extern char home_path[RELIEFOS_AUTH_HOME_LEN];
extern char status_text[160];
extern uint32_t entry_count;
extern struct reliefos_ui_listview_state file_list;
extern int32_t last_click_index;
extern unsigned long last_click_ms;
extern uint8_t menu_open;
extern uint8_t context_menu_active;
extern uint8_t context_menu_animating;
extern uint8_t context_menu_opening;
extern unsigned long context_menu_anim_start;
extern uint32_t context_menu_x;
extern uint32_t context_menu_y;
extern uint32_t view_w;
extern uint32_t view_h;
extern uint64_t selected_mask;
extern uint32_t fileman_window_id;
extern struct reliefos_ui_surface fileman_ui;
extern char address_input[RELIEFOS_FS_PATH_LEN];
extern struct reliefos_ui_edit_state address_edit;
extern uint8_t fileman_operation_active;
extern uint32_t fileman_operation_percent;
extern char fileman_operation_text[160];
extern struct fileman_tree_node fileman_tree_nodes[FILEMAN_TREE_MAX_NODES];
extern uint32_t fileman_tree_node_count;
extern uint32_t fileman_tree_next_id;
extern uint32_t fileman_tree_scroll;
extern uint8_t fileman_show_hidden;
extern uint8_t fileman_settings_open;
extern uint8_t fileman_settings_show_hidden;
extern char fileman_elevated_path[RELIEFOS_FS_PATH_LEN];

struct fileman_layout current_layout(void);
void copy_text(char *dst, uint32_t dst_len, const char *src);
uint32_t text_len(const char *text);
int text_eq(const char *a, const char *b);
int hit_rect_i(int32_t x, int32_t y, int32_t rx, int32_t ry, int32_t rw, int32_t rh);
int ends_with(const char *text, const char *suffix);
void append_char(char *buf, uint32_t *pos, uint32_t cap, char ch);
void append_text(char *buf, uint32_t *pos, uint32_t cap, const char *text);
void append_dec(char *buf, uint32_t *pos, uint32_t cap, uint32_t value);
void append_size(char *buf, uint32_t *pos, uint32_t cap, uint64_t value);
int is_root_path(const char *path);
int selected_entry_valid(void);
int fileman_entry_is_device(uint32_t index);
int fileman_entry_device(const struct reliefos_dir_entry *entry);
int selected_entry_is_file(void);
int selected_entry_is_mutable(void);
int fileman_entry_marked(uint32_t index);
uint32_t fileman_selected_count(void);
void fileman_toggle_selected(void);
void fileman_select_all(void);
void fileman_clear_selection(void);
int fileman_entry_is_hidden(const struct reliefos_dir_entry *entry);
int list_index_at(int32_t x, int32_t y);
void format_size_text(char *buf, uint32_t cap, uint64_t bytes);
void set_status(const char *text);
void set_status_code(const char *prefix, int value);
int permission_error(int value);
void set_status_error(const char *prefix, int value);
int refresh_home_path(void);
void context_menu_set_active(uint8_t active);
void build_child_path(char *dst, uint32_t dst_len, const char *name);
void build_path_join(char *dst, uint32_t dst_len, const char *parent, const char *name);
void build_parent_path(char *dst, uint32_t dst_len);
const char *path_basename(const char *path);
const char *entry_type_name(const struct reliefos_dir_entry *entry);
void build_context_menu_items(struct reliefos_ui_context_menu_item *items, uint32_t count);
void build_file_menu_items(struct reliefos_ui_context_menu_item *items, uint32_t count);
void build_edit_menu_items(struct reliefos_ui_context_menu_item *items, uint32_t count);
void format_contains_text(char *buf, uint32_t cap, const struct folder_size_info *info);
int accumulate_folder_size(const char *path, struct folder_size_info *info, uint32_t depth);
void show_details_selected(void);
void show_open_with_for_path(const char *path, uint8_t set_default_only);
void show_open_with_selected(void);
void show_default_program_for_selected(void);
int reload_dir(void);
/* Elevation for protected directories. These never prompt unless a password is
 * actually required, and they set the status bar themselves on failure. */
int fileman_prompt_elevation(const char *path);
int fileman_elevation_applies(const char *path);
/* Present an already-loaded entry list as the current directory contents.
 * Shared so the ordinary and elevated paths behave identically. */
void present_directory(uint32_t count, const char *status_prefix,
                       const char *status_suffix);
int fileman_list_elevated(const char *path, struct reliefos_dir_entry *out,
                          uint32_t capacity, uint32_t *out_count);
int fileman_mkdir_elevated(const char *parent, const char *name,
                           struct reliefos_dir_entry *out, uint32_t capacity,
                           uint32_t *out_count);
int fileman_rename_elevated(const char *from, const char *to,
                            struct reliefos_dir_entry *out, uint32_t capacity,
                            uint32_t *out_count);
int fileman_delete_elevated(const char *path, uint8_t is_dir,
                            struct reliefos_dir_entry *out, uint32_t capacity,
                            uint32_t *out_count);
void fileman_settings_load(void);
void fileman_open_settings(void);
void fileman_cancel_settings(void);
void fileman_apply_settings(void);
void fileman_settings_dialog_rect(struct reliefos_ui_rect *out);
int fileman_handle_settings_click(int32_t x, int32_t y);
int fileman_handle_settings_key(uint8_t keycode);
uint32_t build_tree_items(struct reliefos_ui_tree_item *items, uint32_t cap);
const char *tree_path_for_id(uint32_t id);
int fileman_tree_toggle(uint32_t id);
void fileman_tree_reset(void);
uint32_t fileman_tree_visible_rows(const struct fileman_layout *layout);
int navigate_to_path(const char *path);
void address_edit_sync_path(void);
void draw_fileman(struct reliefos_ui_surface *ui);
void draw_fileman_settings_dialog(struct reliefos_ui_surface *ui);
void open_selected_entry(void);
void navigate_up(void);
void navigate_root(void);
void create_new_folder(void);
void create_shortcut_for_selected(void);
void rename_selected_entry(void);
void delete_selected_entries(void);
void copy_selected_entries(uint8_t cut);
void paste_clipboard(void);
int fileman_clipboard_available(void);
void extract_tar_selected(void);
void compress_selected_to_tar(void);
void extract_tar_with_path(const char *tar_path);
void fileman_present_progress(void);
void execute_action(uint32_t action);
int handle_menu_click(int32_t x, int32_t y);
int handle_context_menu_click(int32_t x, int32_t y);
void show_context_menu_at(int32_t x, int32_t y, int32_t target);
void handle_right_click(int32_t x, int32_t y);
void handle_click(int32_t x, int32_t y);
void handle_key(uint8_t keycode, uint8_t pressed);
int handle_wheel(int32_t x, int32_t y, int32_t wheel);
void present_fileman(uint32_t window_id, struct reliefos_ui_surface *ui);

#endif
