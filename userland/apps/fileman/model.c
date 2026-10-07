#include "fileman.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

int is_root_path(const char *path)
{
    return path && path[0] == '/' && path[1] == 0;
}

int selected_entry_valid(void)
{
    return file_list.selected >= 0 && (uint32_t)file_list.selected < entry_count;
}

int selected_entry_is_file(void)
{
    return selected_entry_valid() && entries[file_list.selected].type == RELIEFOS_FS_TYPE_FILE;
}

int fileman_entry_is_device(uint32_t index)
{
    char path[RELIEFOS_FS_PATH_LEN];
    struct stat st;
    if (index >= entry_count) {
        return 0;
    }
    build_child_path(path, sizeof(path), entries[index].name);
    return stat(path, &st) == 0 && S_ISCHR(st.st_mode);
}

int fileman_entry_device(const struct reliefos_dir_entry *entry)
{
    char path[RELIEFOS_FS_PATH_LEN];
    struct stat st;
    if (!entry) {
        return 0;
    }
    build_child_path(path, sizeof(path), entry->name);
    return stat(path, &st) == 0 && S_ISCHR(st.st_mode);
}

int selected_entry_is_mutable(void)
{
    return selected_entry_valid() &&
           !fileman_entry_is_device((uint32_t)file_list.selected);
}

int fileman_entry_marked(uint32_t index)
{
    return index < 64U && (selected_mask & (1ULL << index)) != 0;
}

uint32_t fileman_selected_count(void)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < entry_count && i < 64U; ++i) {
        if (fileman_entry_marked(i)) {
            ++count;
        }
    }
    return count;
}

void fileman_toggle_selected(void)
{
    if (!selected_entry_valid() || file_list.selected >= 64) {
        set_status(T("Select an item"));
        return;
    }
    selected_mask ^= 1ULL << (uint32_t)file_list.selected;
    set_status(fileman_entry_marked((uint32_t)file_list.selected)
                   ? T("Item marked")
                   : T("Item unmarked"));
}

void fileman_select_all(void)
{
    selected_mask = entry_count >= 64U ? UINT64_MAX :
                    (entry_count ? (1ULL << entry_count) - 1ULL : 0);
    set_status(T("All items marked"));
}

void fileman_clear_selection(void)
{
    selected_mask = 0;
    set_status(T("Marks cleared"));
}

int fileman_entry_is_hidden(const struct reliefos_dir_entry *entry)
{
    return entry && entry->name[0] == '.';
}

static void fileman_settings_path(char *path, uint32_t capacity)
{
    struct reliefos_user_info user;
    if (!path || capacity == 0) {
        return;
    }
    path[0] = 0;
    if (reliefos_auth_current(&user) == 0 && user.uid && user.home[0]) {
        build_path_join(path, capacity, user.home, ".fileman.conf");
        return;
    }
    (void)mkdir("/var", 0777);
    build_path_join(path, capacity, "/var", ".fileman.conf");
}

static uint8_t fileman_settings_show_hidden_value(const char *config)
{
    static const char key[] = "show_hidden=";
    uint32_t pos = 0;
    while (config && config[pos]) {
        uint32_t start = pos;
        uint32_t key_pos = 0;
        while (config[pos] && config[pos] != '\n' && config[pos] != '\r') {
            ++pos;
        }
        while (key[key_pos] && start + key_pos < pos &&
               config[start + key_pos] == key[key_pos]) {
            ++key_pos;
        }
        if (!key[key_pos] && start + key_pos < pos) {
            char value = config[start + key_pos];
            return value == '1' || value == 'y' || value == 'Y' ||
                   value == 't' || value == 'T';
        }
        while (config[pos] == '\n' || config[pos] == '\r') {
            ++pos;
        }
    }
    return 0;
}

void fileman_settings_load(void)
{
    char path[RELIEFOS_FS_PATH_LEN];
    char config[128];
    uint32_t length = 0;
    int fd;
    fileman_show_hidden = 0;
    fileman_settings_path(path, sizeof(path));
    fd = open(path, RELIEFOS_O_RDONLY, 0);
    if (fd < 0) {
        return;
    }
    while (length + 1u < sizeof(config)) {
        long got = read(fd, config + length, sizeof(config) - length - 1u);
        if (got <= 0) {
            break;
        }
        length += (uint32_t)got;
    }
    close(fd);
    config[length] = 0;
    fileman_show_hidden = fileman_settings_show_hidden_value(config);
}

static int fileman_settings_save(uint8_t show_hidden)
{
    char path[RELIEFOS_FS_PATH_LEN];
    static const char enabled[] = "version=1\nshow_hidden=1\n";
    static const char disabled[] = "version=1\nshow_hidden=0\n";
    const char *config = show_hidden ? enabled : disabled;
    uint32_t length = text_len(config);
    int fd;
    fileman_settings_path(path, sizeof(path));
    fd = open(path, RELIEFOS_O_WRONLY | RELIEFOS_O_CREAT | RELIEFOS_O_TRUNC, 0666);
    if (fd < 0) {
        return fd;
    }
    if (write(fd, config, length) != (long)length) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

void fileman_open_settings(void)
{
    fileman_settings_show_hidden = fileman_show_hidden;
    fileman_settings_open = 1;
    menu_open = FILEMAN_MENU_NONE;
    context_menu_set_active(0);
}

void fileman_cancel_settings(void)
{
    fileman_settings_open = 0;
}

void fileman_apply_settings(void)
{
    int ret = fileman_settings_save(fileman_settings_show_hidden);
    if (ret < 0) {
        set_status_error(T("Could not save file manager settings "), ret);
        return;
    }
    fileman_show_hidden = fileman_settings_show_hidden;
    fileman_settings_open = 0;
    fileman_tree_reset();
    (void)reload_dir();
    set_status(T("File Manager settings saved"));
}

void fileman_settings_dialog_rect(struct reliefos_ui_rect *out)
{
    uint32_t width = view_w > 16u ? view_w - 16u : view_w;
    uint32_t height = view_h > 16u ? view_h - 16u : view_h;
    if (!out) {
        return;
    }
    if (width > FILEMAN_SETTINGS_DIALOG_W) {
        width = FILEMAN_SETTINGS_DIALOG_W;
    }
    if (height > FILEMAN_SETTINGS_DIALOG_H) {
        height = FILEMAN_SETTINGS_DIALOG_H;
    }
    out->w = width;
    out->h = height;
    out->x = (int32_t)(view_w > width ? (view_w - width) / 2u : 0u);
    out->y = (int32_t)(view_h > height ? (view_h - height) / 2u : 0u);
}

int fileman_handle_settings_click(int32_t x, int32_t y)
{
    struct reliefos_ui_rect rect;
    uint32_t button_y;
    if (!fileman_settings_open) {
        return 0;
    }
    fileman_settings_dialog_rect(&rect);
    button_y = (uint32_t)rect.y + rect.h - 34u;
    if (hit_rect_i(x, y, rect.x + 18, rect.y + 52,
                   (int32_t)(rect.w > 36u ? rect.w - 36u : rect.w), 30)) {
        fileman_settings_show_hidden = fileman_settings_show_hidden ? 0 : 1;
        return 1;
    }
    if (hit_rect_i(x, y, rect.x + (int32_t)rect.w - 168, (int32_t)button_y,
                   72, RELIEFOS_UI_BUTTON_H)) {
        fileman_apply_settings();
        return 1;
    }
    if (hit_rect_i(x, y, rect.x + (int32_t)rect.w - 88, (int32_t)button_y,
                   72, RELIEFOS_UI_BUTTON_H)) {
        fileman_cancel_settings();
        return 1;
    }
    return 1;
}

int fileman_handle_settings_key(uint8_t keycode)
{
    if (!fileman_settings_open) {
        return 0;
    }
    if (keycode == FILEMAN_KEY_ESCAPE) {
        fileman_cancel_settings();
    } else if (keycode == RELIEFOS_KEY_ENTER) {
        fileman_apply_settings();
    }
    return 1;
}

int list_index_at(int32_t x, int32_t y)
{
    struct fileman_layout l = current_layout();
    int32_t row;
    uint32_t index;
    if (!hit_rect_i(x, y, (int32_t)(l.list_x + 2), (int32_t)l.rows_y,
                    (int32_t)l.list_w, (int32_t)(l.visible_rows * ROW_H))) {
        return -1;
    }
    row = (y - (int32_t)l.rows_y) / (int32_t)ROW_H;
    if (row < 0) {
        return -1;
    }
    index = file_list.scroll + (uint32_t)row;
    if (index >= entry_count) {
        return -1;
    }
    return (int)index;
}

void format_size_text(char *buf, uint32_t cap, uint64_t bytes)
{
    static const char *units[] = {"Byte", "KB", "MB", "GB", "TB"};
    uint64_t whole = bytes;
    uint64_t frac = 0;
    uint32_t unit = 0;
    uint32_t pos = 0;
    while (whole >= 1024 && unit + 1 < sizeof(units) / sizeof(units[0])) {
        frac = ((whole % 1024) * 100) / 1024;
        whole /= 1024;
        ++unit;
    }
    buf[0] = 0;
    append_size(buf, &pos, cap, whole);
    if (unit > 0 && frac > 0) {
        append_char(buf, &pos, cap, '.');
        append_char(buf, &pos, cap, (char)('0' + frac / 10));
        append_char(buf, &pos, cap, (char)('0' + frac % 10));
    }
    append_char(buf, &pos, cap, ' ');
    append_text(buf, &pos, cap, units[unit]);
    if (unit == 0 && bytes != 1) {
        append_char(buf, &pos, cap, 's');
    }
    if (unit > 0) {
        append_text(buf, &pos, cap, " (");
        append_size(buf, &pos, cap, bytes);
        append_text(buf, &pos, cap, " bytes)");
    }
}

void set_status(const char *text)
{
    copy_text(status_text, sizeof(status_text), text);
}

void set_status_code(const char *prefix, int value)
{
    char buf[96];
    uint32_t pos = 0;
    buf[0] = 0;
    append_text(buf, &pos, sizeof(buf), prefix);
    if (value < 0) {
        append_char(buf, &pos, sizeof(buf), '-');
        value = -value;
    }
    append_dec(buf, &pos, sizeof(buf), (uint32_t)value);
    set_status(buf);
}

int permission_error(int value)
{
    return value == -RELIEFOS_EPERM || value == -RELIEFOS_EACCES;
}

void set_status_error(const char *prefix, int value)
{
    char message[160];
    snprintf(message, sizeof(message), "%s: %s", prefix, strerror(value == -1 ? errno : -value));
    set_status(message);
}

int refresh_home_path(void)
{
    struct reliefos_user_info user;
    home_path[0] = 0;
    if (reliefos_auth_current(&user) == 0 && user.uid && user.home[0]) {
        copy_text(home_path, sizeof(home_path), user.home);
        return 1;
    }
    return 0;
}

void context_menu_set_active(uint8_t active)
{
    if (context_menu_active == active && !context_menu_animating) {
        return;
    }
    context_menu_active = active;
    context_menu_opening = active;
    context_menu_animating = 1;
    context_menu_anim_start = reliefos_uptime_ms();
}

void build_child_path(char *dst, uint32_t dst_len, const char *name)
{
    uint32_t pos = 0;
    dst[0] = 0;
    append_text(dst, &pos, dst_len, current_path);
    if (!is_root_path(current_path)) {
        append_char(dst, &pos, dst_len, '/');
    }
    append_text(dst, &pos, dst_len, name);
}

void build_path_join(char *dst, uint32_t dst_len, const char *parent, const char *name)
{
    uint32_t pos = 0;
    dst[0] = 0;
    append_text(dst, &pos, dst_len, parent);
    if (!is_root_path(parent)) {
        append_char(dst, &pos, dst_len, '/');
    }
    append_text(dst, &pos, dst_len, name);
}

void build_parent_path(char *dst, uint32_t dst_len)
{
    uint32_t len;
    copy_text(dst, dst_len, current_path);
    if (is_root_path(dst)) {
        return;
    }
    len = text_len(dst);
    while (len > 1 && dst[len - 1] != '/') {
        dst[--len] = 0;
    }
    if (len > 1) {
        dst[len - 1] = 0;
    }
}

const char *path_basename(const char *path)
{
    const char *base = path;
    if (!path) {
        return "";
    }
    for (uint32_t i = 0; path[i]; ++i) {
        if (path[i] == '/') {
            base = path + i + 1;
        }
    }
    return base ? base : "";
}

const char *entry_type_name(const struct reliefos_dir_entry *entry)
{
    if (!entry) {
        return "FILE";
    }
    if (entry->type == RELIEFOS_FS_TYPE_DIR) {
        return "DIR ";
    }
    if (fileman_entry_device(entry)) {
        return "DEV ";
    }
    if (ends_with(entry->name, ".elf")) {
        return "ELF ";
    }
    if (ends_with(entry->name, ".lnk")) {
        return "LNK ";
    }
    return "FILE";
}

void build_context_menu_items(struct reliefos_ui_context_menu_item *items,
                                      uint32_t count)
{
    uint32_t has_item = selected_entry_valid();
    uint32_t has_file = selected_entry_is_file();
    uint32_t has_mutable = selected_entry_is_mutable();
    if (!items || count < FILEMAN_CONTEXT_MENU_COUNT) {
        return;
    }
    items[0] = (struct reliefos_ui_context_menu_item){
        T("Open"), FILEMAN_ACTION_OPEN, has_item ? 0 : RELIEFOS_UI_MENU_DISABLED};
    items[1] = (struct reliefos_ui_context_menu_item){
        T("Open With..."), FILEMAN_ACTION_OPEN_WITH, has_file ? 0 : RELIEFOS_UI_MENU_DISABLED};
    items[2] = (struct reliefos_ui_context_menu_item){
        T("Copy"), FILEMAN_ACTION_COPY, has_mutable ? 0 : RELIEFOS_UI_MENU_DISABLED};
    items[3] = (struct reliefos_ui_context_menu_item){
        T("Cut"), FILEMAN_ACTION_CUT, has_mutable ? 0 : RELIEFOS_UI_MENU_DISABLED};
    items[4] = (struct reliefos_ui_context_menu_item){
        T("Paste"), FILEMAN_ACTION_PASTE,
        fileman_clipboard_available() ? 0 : RELIEFOS_UI_MENU_DISABLED};
    items[5] = (struct reliefos_ui_context_menu_item){
        fileman_entry_marked((uint32_t)(file_list.selected < 0 ? 0 : file_list.selected))
            ? T("Unmark") : T("Mark for Batch"),
        FILEMAN_ACTION_TOGGLE_MARK, has_item ? 0 : RELIEFOS_UI_MENU_DISABLED};
    items[6] = (struct reliefos_ui_context_menu_item){"", 0, RELIEFOS_UI_MENU_SEPARATOR};
    items[7] = (struct reliefos_ui_context_menu_item){
        T("Rename"), FILEMAN_ACTION_RENAME,
        has_mutable && fileman_selected_count() <= 1U ? 0 : RELIEFOS_UI_MENU_DISABLED};
    items[8] = (struct reliefos_ui_context_menu_item){
        T("Delete"), FILEMAN_ACTION_DELETE,
        has_mutable ? 0 : RELIEFOS_UI_MENU_DISABLED};
    items[9] = (struct reliefos_ui_context_menu_item){
        T("Details"), FILEMAN_ACTION_DETAILS,
        has_item ? 0 : RELIEFOS_UI_MENU_DISABLED};
    items[10] = (struct reliefos_ui_context_menu_item){
        T("Refresh"), FILEMAN_ACTION_REFRESH, 0};
    items[11] = (struct reliefos_ui_context_menu_item){"", 0, RELIEFOS_UI_MENU_SEPARATOR};
    items[12] = (struct reliefos_ui_context_menu_item){
        has_file && ends_with(entries[file_list.selected].name, ".tar")
            ? T("Extract tar")
            : T("Compress to .tar"),
        has_file && ends_with(entries[file_list.selected].name, ".tar")
            ? FILEMAN_ACTION_EXTRACT_TAR
            : FILEMAN_ACTION_COMPRESS_TAR,
        has_item ? 0 : RELIEFOS_UI_MENU_DISABLED};
}

void build_file_menu_items(struct reliefos_ui_context_menu_item *items, uint32_t count)
{
    uint32_t has_item = selected_entry_valid();
    uint32_t has_file = selected_entry_is_file();
    uint32_t has_mutable = selected_entry_is_mutable();
    if (!items || count < FILEMAN_FILE_MENU_COUNT) {
        return;
    }
    items[0] = (struct reliefos_ui_context_menu_item){T("Open"), FILEMAN_ACTION_OPEN,
                                                     has_item ? 0 : RELIEFOS_UI_MENU_DISABLED};
    items[1] = (struct reliefos_ui_context_menu_item){T("Open With..."), FILEMAN_ACTION_OPEN_WITH,
                                                     has_file ? 0 : RELIEFOS_UI_MENU_DISABLED};
    items[2] = (struct reliefos_ui_context_menu_item){T("Default Program..."), FILEMAN_ACTION_DEFAULT_PROGRAM,
                                                     has_file ? 0 : RELIEFOS_UI_MENU_DISABLED};
    items[3] = (struct reliefos_ui_context_menu_item){T("Create Shortcut"), FILEMAN_ACTION_CREATE_SHORTCUT,
                                                     has_file ? 0 : RELIEFOS_UI_MENU_DISABLED};
    items[4] = (struct reliefos_ui_context_menu_item){T("Details"), FILEMAN_ACTION_DETAILS,
                                                     has_item ? 0 : RELIEFOS_UI_MENU_DISABLED};
    items[5] = (struct reliefos_ui_context_menu_item){"", 0, RELIEFOS_UI_MENU_SEPARATOR};
    items[6] = (struct reliefos_ui_context_menu_item){T("Rename"), FILEMAN_ACTION_RENAME,
                                                     has_mutable && fileman_selected_count() <= 1U ? 0 : RELIEFOS_UI_MENU_DISABLED};
    items[7] = (struct reliefos_ui_context_menu_item){T("New Folder"), FILEMAN_ACTION_NEW_FOLDER, 0};
    items[8] = (struct reliefos_ui_context_menu_item){T("Refresh"), FILEMAN_ACTION_REFRESH, 0};
}

void build_edit_menu_items(struct reliefos_ui_context_menu_item *items, uint32_t count)
{
    uint32_t has_mutable = selected_entry_is_mutable();
    if (!items || count < FILEMAN_EDIT_MENU_COUNT) {
        return;
    }
    items[0] = (struct reliefos_ui_context_menu_item){T("Copy"), FILEMAN_ACTION_COPY,
                                                     has_mutable ? 0 : RELIEFOS_UI_MENU_DISABLED};
    items[1] = (struct reliefos_ui_context_menu_item){T("Cut"), FILEMAN_ACTION_CUT,
                                                     has_mutable ? 0 : RELIEFOS_UI_MENU_DISABLED};
    items[2] = (struct reliefos_ui_context_menu_item){T("Paste"), FILEMAN_ACTION_PASTE,
                                                     fileman_clipboard_available() ? 0 : RELIEFOS_UI_MENU_DISABLED};
    items[3] = (struct reliefos_ui_context_menu_item){"", 0, RELIEFOS_UI_MENU_SEPARATOR};
    items[4] = (struct reliefos_ui_context_menu_item){
        fileman_entry_marked((uint32_t)(file_list.selected < 0 ? 0 : file_list.selected))
            ? T("Unmark") : T("Mark for Batch"),
        FILEMAN_ACTION_TOGGLE_MARK, selected_entry_valid() ? 0 : RELIEFOS_UI_MENU_DISABLED};
    items[5] = (struct reliefos_ui_context_menu_item){T("Mark All"), FILEMAN_ACTION_SELECT_ALL,
                                                     entry_count ? 0 : RELIEFOS_UI_MENU_DISABLED};
    items[6] = (struct reliefos_ui_context_menu_item){T("Clear Marks"), FILEMAN_ACTION_CLEAR_SELECTION,
                                                     fileman_selected_count() ? 0 : RELIEFOS_UI_MENU_DISABLED};
    items[7] = (struct reliefos_ui_context_menu_item){T("Delete"), FILEMAN_ACTION_DELETE,
                                                     has_mutable ? 0 : RELIEFOS_UI_MENU_DISABLED};
}

void format_contains_text(char *buf, uint32_t cap, const struct folder_size_info *info)
{
    uint32_t pos = 0;
    buf[0] = 0;
    append_dec(buf, &pos, cap, info ? info->files : 0);
    append_text(buf, &pos, cap, T(" files, "));
    append_dec(buf, &pos, cap, info ? info->folders : 0);
    append_text(buf, &pos, cap, T(" folders"));
    if (info && info->partial) {
        append_text(buf, &pos, cap, T(" (partial)"));
    }
}

int accumulate_folder_size(const char *path, struct folder_size_info *info, uint32_t depth)
{
    struct reliefos_dir_entry entry;
    int fd;
    int ret;
    if (!path || !info) {
        return -1;
    }
    if (depth >= FILEMAN_FOLDER_SIZE_MAX_DEPTH ||
        info->visited >= FILEMAN_FOLDER_SIZE_MAX_ITEMS) {
        info->partial = 1;
        return 0;
    }
    fd = open(path, RELIEFOS_O_RDONLY, 0);
    if (fd < 0) {
        info->partial = 1;
        return fd;
    }
    for (;;) {
        char child[RELIEFOS_FS_PATH_LEN];
        struct reliefos_stat st;
        ret = reliefos_readdir(fd, &entry);
        if (ret < 0) {
            info->partial = 1;
            close(fd);
            return ret;
        }
        if (ret == 0) {
            break;
        }
        if (info->visited >= FILEMAN_FOLDER_SIZE_MAX_ITEMS) {
            info->partial = 1;
            break;
        }
        ++info->visited;
        build_path_join(child, sizeof(child), path, entry.name);
        if (reliefos_stat_legacy(child, &st) < 0) {
            info->partial = 1;
            continue;
        }
        if (st.type == RELIEFOS_FS_TYPE_DIR) {
            ++info->folders;
            (void)accumulate_folder_size(child, info, depth + 1);
        } else if (st.type == RELIEFOS_FS_TYPE_FILE) {
            ++info->files;
            info->bytes += st.size;
        }
    }
    close(fd);
    return 0;
}


void show_open_with_selected(void)
{
    char path[RELIEFOS_FS_PATH_LEN];
    if (file_list.selected < 0 || (uint32_t)file_list.selected >= entry_count) {
        set_status(T("Select a file"));
        return;
    }
    if (entries[file_list.selected].type != RELIEFOS_FS_TYPE_FILE) {
        set_status(T("Open With is for files"));
        return;
    }
    build_child_path(path, sizeof(path), entries[file_list.selected].name);
    show_open_with_for_path(path, 0);
}

void show_default_program_for_selected(void)
{
    char path[RELIEFOS_FS_PATH_LEN];
    if (file_list.selected < 0 || (uint32_t)file_list.selected >= entry_count) {
        set_status(T("Select a file"));
        return;
    }
    if (entries[file_list.selected].type != RELIEFOS_FS_TYPE_FILE) {
        set_status(T("Default program is for files"));
        return;
    }
    build_child_path(path, sizeof(path), entries[file_list.selected].name);
    show_open_with_for_path(path, 1);
}

static char sort_fold_ascii(char ch)
{
    return ch >= 'A' && ch <= 'Z' ? (char)(ch + ('a' - 'A')) : ch;
}

static int compare_entry_names(const char *left, const char *right)
{
    uint32_t index = 0;
    while (left[index] && right[index]) {
        char a = sort_fold_ascii(left[index]);
        char b = sort_fold_ascii(right[index]);
        if (a != b) {
            return (uint8_t)a < (uint8_t)b ? -1 : 1;
        }
        ++index;
    }
    if (left[index]) {
        return 1;
    }
    if (right[index]) {
        return -1;
    }
    return 0;
}

static int compare_entries(const struct reliefos_dir_entry *left,
                           const struct reliefos_dir_entry *right)
{
    uint32_t left_is_dir = left->type == RELIEFOS_FS_TYPE_DIR;
    uint32_t right_is_dir = right->type == RELIEFOS_FS_TYPE_DIR;
    if (left_is_dir != right_is_dir) {
        return left_is_dir ? -1 : 1;
    }
    return compare_entry_names(left->name, right->name);
}

static void sort_directory_entries(uint32_t count)
{
    for (uint32_t i = 1; i < count; ++i) {
        struct reliefos_dir_entry entry = entries[i];
        uint32_t slot = i;
        while (slot && compare_entries(&entry, &entries[slot - 1U]) < 0) {
            entries[slot] = entries[slot - 1U];
            --slot;
        }
        entries[slot] = entry;
    }
}

/* Should this entry appear in the list? The privileged broker returns raw
 * directory contents, so the same filters apply to both sources. */
static int entry_passes_filters(const struct reliefos_dir_entry *entry)
{
    if (!fileman_show_hidden && fileman_entry_is_hidden(entry)) {
        return 0;
    }
    return 1;
}

/* Present a loaded entry list and report it in the status bar. Shared by the
 * ordinary open/readdir path and the elevated broker path so both behave
 * identically apart from how the entries were obtained. */
void present_directory(uint32_t count, const char *status_prefix,
                             const char *status_suffix)
{
    sort_directory_entries(count);
    entry_count = count;
    selected_mask = 0;
    reliefos_ui_listview_state_set_count(&file_list, entry_count);
    file_list.selected = entry_count ? 0 : -1;
    file_list.scroll = 0;
    context_menu_set_active(0);
    last_click_index = -1;
    last_click_ms = 0;
    char buf[128];
    uint32_t pos = 0;
    buf[0] = 0;
    append_text(buf, &pos, sizeof(buf), status_prefix);
    append_dec(buf, &pos, sizeof(buf), entry_count);
    append_text(buf, &pos, sizeof(buf), status_suffix);
    append_text(buf, &pos, sizeof(buf), current_path);
    set_status(buf);
    printf("[fileman.elf] list path=%s count=%d\n", current_path, (int)count);
}

int reload_dir(void)
{
    int fd = open(current_path, 0, 0);
    int ret = 0;
    uint32_t count = 0;
    if (fd < 0) {
        if (permission_error(fd) &&
            fileman_list_elevated(current_path, entries, FILEMAN_MAX_ENTRIES,
                                  &count) == 0) {
            /* The administrator verified; the broker enumerated the directory
             * because this process cannot. */
            present_directory(count, T("Elevated - items "),
                              " in ");
            return 0;
        }
        entry_count = 0;
        reliefos_ui_listview_state_set_count(&file_list, 0);
        if (ret == 0) {
            set_status_error("Open dir failed ", fd);
        }
        printf("[fileman.elf] list path=%s open=%d\n", current_path, fd);
        return fd;
    }
    reliefos_ui_listview_state_set_count(&file_list, 0);
    while (count < FILEMAN_MAX_ENTRIES) {
        struct reliefos_dir_entry entry;
        ret = reliefos_readdir(fd, &entry);
        if (ret < 0) {
            close(fd);
            entry_count = 0;
            reliefos_ui_listview_state_set_count(&file_list, 0);
            set_status_error("Read dir failed ", ret);
            printf("[fileman.elf] list path=%s readdir=%d\n", current_path, ret);
            return ret;
        }
        if (ret == 0) {
            break;
        }
        if (!entry_passes_filters(&entry)) {
            continue;
        }
        entries[count] = entry;
        ++count;
    }
    close(fd);
    present_directory(count, T("Items "), " in ");
    return 0;
}

static int tree_compare_nodes(const struct fileman_tree_node *left,
                              const struct fileman_tree_node *right)
{
    return compare_entry_names(left->label, right->label);
}

static int tree_node_index_for_id(uint32_t id)
{
    for (uint32_t i = 0; i < fileman_tree_node_count; ++i) {
        if (fileman_tree_nodes[i].used && fileman_tree_nodes[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

static int tree_node_index_for_path(const char *path)
{
    for (uint32_t i = 0; i < fileman_tree_node_count; ++i) {
        if (fileman_tree_nodes[i].used && text_eq(fileman_tree_nodes[i].path, path)) {
            return (int)i;
        }
    }
    return -1;
}

static int tree_dir_has_children(const char *path)
{
    struct reliefos_dir_entry entry;
    int fd;
    int ret;
    if (!path) {
        return 0;
    }
    fd = open(path, RELIEFOS_O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    for (;;) {
        ret = reliefos_readdir(fd, &entry);
        if (ret <= 0) {
            break;
        }
        if (entry.type == RELIEFOS_FS_TYPE_DIR &&
            (fileman_show_hidden || !fileman_entry_is_hidden(&entry))) {
            close(fd);
            return 1;
        }
    }
    close(fd);
    return 0;
}

static void tree_add_node(const char *path, const char *label, uint32_t id,
                          uint32_t parent_id)
{
    struct fileman_tree_node *node;
    if (fileman_tree_node_count >= FILEMAN_TREE_MAX_NODES) {
        return;
    }
    node = &fileman_tree_nodes[fileman_tree_node_count++];
    node->used = 1;
    node->id = id;
    node->parent_id = parent_id;
    copy_text(node->path, sizeof(node->path), path);
    copy_text(node->label, sizeof(node->label), label);
    node->has_children = tree_dir_has_children(path) ? 1 : 0;
}

void fileman_tree_reset(void)
{
    for (uint32_t i = 0; i < FILEMAN_TREE_MAX_NODES; ++i) {
        fileman_tree_nodes[i] = (struct fileman_tree_node){0};
    }
    fileman_tree_node_count = 0;
    fileman_tree_next_id = 11;
    fileman_tree_scroll = 0;
    tree_add_node("/", "/", 1, 0);
}

static void tree_init_if_needed(void)
{
    if (fileman_tree_node_count == 0) {
        fileman_tree_reset();
    }
}

static void tree_load_children(uint32_t node_index)
{
    struct fileman_tree_node *node;
    struct reliefos_dir_entry entry;
    uint32_t first_child;
    uint32_t added;
    int fd;
    int ret;
    if (node_index >= fileman_tree_node_count) {
        return;
    }
    node = &fileman_tree_nodes[node_index];
    if (!node->used || node->loaded) {
        return;
    }
    node->loaded = 1;
    first_child = fileman_tree_node_count;
    fd = open(node->path, RELIEFOS_O_RDONLY, 0);
    if (fd < 0) {
        node->has_children = 0;
        return;
    }
    while (fileman_tree_node_count < FILEMAN_TREE_MAX_NODES) {
        char child_path[RELIEFOS_FS_PATH_LEN];
        ret = reliefos_readdir(fd, &entry);
        if (ret <= 0) {
            break;
        }
        if (entry.type != RELIEFOS_FS_TYPE_DIR) {
            continue;
        }
        if (!fileman_show_hidden && fileman_entry_is_hidden(&entry)) {
            continue;
        }
        build_path_join(child_path, sizeof(child_path), node->path, entry.name);
        tree_add_node(child_path, entry.name, fileman_tree_next_id++, node->id);
    }
    close(fd);
    added = fileman_tree_node_count - first_child;
    if (added == 0) {
        node->has_children = 0;
        return;
    }
    for (uint32_t i = first_child + 1; i < fileman_tree_node_count; ++i) {
        struct fileman_tree_node child = fileman_tree_nodes[i];
        uint32_t slot = i;
        while (slot > first_child &&
               tree_compare_nodes(&child, &fileman_tree_nodes[slot - 1]) < 0) {
            fileman_tree_nodes[slot] = fileman_tree_nodes[slot - 1];
            --slot;
        }
        fileman_tree_nodes[slot] = child;
    }
}

static int tree_ensure_path(const char *path)
{
    char partial[RELIEFOS_FS_PATH_LEN];
    uint32_t pos;
    int index;
    if (!path || path[0] != '/') {
        return -1;
    }
    partial[0] = '/';
    partial[1] = 0;
    index = tree_node_index_for_path(partial);
    if (index < 0) {
        return -1;
    }
    pos = 1;
    while (path[pos]) {
        uint32_t end = pos;
        uint32_t partial_len;
        while (path[end] && path[end] != '/') {
            ++end;
        }
        if (end == pos) {
            pos = path[end] == '/' ? end + 1 : end;
            continue;
        }
        partial_len = text_len(partial);
        if (!is_root_path(partial)) {
            append_char(partial, &partial_len, sizeof(partial), '/');
        }
        while (pos < end) {
            append_char(partial, &partial_len, sizeof(partial), path[pos++]);
        }
        tree_load_children((uint32_t)index);
        index = tree_node_index_for_path(partial);
        if (index < 0) {
            return -1;
        }
        pos = path[end] == '/' ? end + 1 : end;
    }
    return index;
}

static void tree_expand_path(const char *path)
{
    int index;
    tree_init_if_needed();
    index = tree_ensure_path(path);
    while (index >= 0) {
        struct fileman_tree_node *node = &fileman_tree_nodes[index];
        if (node->has_children) {
            tree_load_children((uint32_t)index);
            if (node->has_children) {
                node->expanded = 1;
            }
        }
        index = node->parent_id ? tree_node_index_for_id(node->parent_id) : -1;
    }
}

static void tree_collapse_path(const char *path)
{
    int index;
    tree_init_if_needed();
    index = tree_ensure_path(path);
    if (index >= 0) {
        fileman_tree_nodes[index].expanded = 0;
    }
}

static void tree_scroll_to_path(const char *path)
{
    struct reliefos_ui_tree_item items[FILEMAN_TREE_MAX_NODES];
    struct fileman_layout layout;
    uint32_t count;
    uint32_t visible_rows;
    if (!path) {
        return;
    }
    count = build_tree_items(items, sizeof(items) / sizeof(items[0]));
    layout = current_layout();
    visible_rows = fileman_tree_visible_rows(&layout);
    for (uint32_t i = 0; i < count; ++i) {
        const char *item_path = tree_path_for_id(items[i].id);
        if (!text_eq(path, item_path)) {
            continue;
        }
        if (i < fileman_tree_scroll) {
            fileman_tree_scroll = i;
        } else if (i >= fileman_tree_scroll + visible_rows) {
            fileman_tree_scroll = i - visible_rows + 1;
        }
        return;
    }
}

static int tree_path_is_direct_child(const char *parent, const char *child)
{
    uint32_t parent_len;
    uint32_t child_pos;
    if (!parent || !child || !parent[0] || !child[0] || text_eq(parent, child)) {
        return 0;
    }
    parent_len = text_len(parent);
    for (uint32_t i = 0; i < parent_len; ++i) {
        if (parent[i] != child[i]) {
            return 0;
        }
    }
    child_pos = parent_len;
    if (!is_root_path(parent)) {
        if (child[child_pos++] != '/') {
            return 0;
        }
    }
    if (!child[child_pos]) {
        return 0;
    }
    while (child[child_pos]) {
        if (child[child_pos++] == '/') {
            return 0;
        }
    }
    return 1;
}

static void fileman_tree_sync_navigation(const char *old_path, const char *new_path)
{
    if (tree_path_is_direct_child(old_path, new_path)) {
        tree_expand_path(new_path);
        tree_scroll_to_path(new_path);
    } else if (tree_path_is_direct_child(new_path, old_path)) {
        tree_collapse_path(old_path);
        tree_scroll_to_path(new_path);
    }
}

static void tree_append_visible(struct reliefos_ui_tree_item *items, uint32_t cap,
                                uint32_t *count, uint32_t node_index,
                                uint32_t depth)
{
    struct fileman_tree_node *node;
    if (!items || !count || node_index >= fileman_tree_node_count || *count >= cap) {
        return;
    }
    node = &fileman_tree_nodes[node_index];
    if (!node->used) {
        return;
    }
    items[(*count)++] = (struct reliefos_ui_tree_item){
        node->label, node->id, depth,
        node->has_children ? (node->expanded ? RELIEFOS_UI_TREE_EXPANDED : 0)
                           : RELIEFOS_UI_TREE_LEAF};
    if (!node->expanded) {
        return;
    }
    for (uint32_t i = 0; i < fileman_tree_node_count && *count < cap; ++i) {
        if (fileman_tree_nodes[i].used && fileman_tree_nodes[i].parent_id == node->id) {
            tree_append_visible(items, cap, count, i, depth + 1);
        }
    }
}

uint32_t build_tree_items(struct reliefos_ui_tree_item *items, uint32_t cap)
{
    uint32_t count = 0;
    tree_init_if_needed();
    if (!items || cap == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < fileman_tree_node_count && count < cap; ++i) {
        if (fileman_tree_nodes[i].used && fileman_tree_nodes[i].parent_id == 0) {
            tree_append_visible(items, cap, &count, i, 0);
        }
    }
    return count;
}

const char *tree_path_for_id(uint32_t id)
{
    int index;
    tree_init_if_needed();
    index = tree_node_index_for_id(id);
    return index >= 0 ? fileman_tree_nodes[index].path : 0;
}

int fileman_tree_toggle(uint32_t id)
{
    int index;
    tree_init_if_needed();
    index = tree_node_index_for_id(id);
    if (index < 0 || !fileman_tree_nodes[index].has_children) {
        return 0;
    }
    if (!fileman_tree_nodes[index].expanded) {
        tree_load_children((uint32_t)index);
        if (!fileman_tree_nodes[index].has_children) {
            return 0;
        }
    }
    fileman_tree_nodes[index].expanded = fileman_tree_nodes[index].expanded ? 0 : 1;
    return 1;
}

uint32_t fileman_tree_visible_rows(const struct fileman_layout *layout)
{
    uint32_t height;
    if (!layout || layout->tree_h <= 8) {
        return 1;
    }
    height = layout->tree_h - 8;
    return height / TREE_ROW_H ? height / TREE_ROW_H : 1;
}

int navigate_to_path(const char *path)
{
    char old_path[RELIEFOS_FS_PATH_LEN];
    char target[RELIEFOS_FS_PATH_LEN];
    int ret;
    if (!path || !path[0]) {
        return -1;
    }
    copy_text(old_path, sizeof(old_path), current_path);
    copy_text(target, sizeof(target), path);
    ret = chdir(target);
    if (ret < 0) {
        /* A protected directory is browsable only through the privileged
         * broker, which verifies an administrator first. `chdir` itself can
         * never succeed for this process, so verify and then adopt the path:
         * the subsequent reload enumerates it through the broker. */
        if (permission_error(ret) && fileman_prompt_elevation(target) == 0) {
            ret = 0;
        }
        if (ret < 0) {
            copy_text(current_path, sizeof(current_path), old_path);
            address_edit_sync_path();
            set_status_error("Open dir failed ", ret);
            return ret;
        }
    }
    copy_text(current_path, sizeof(current_path), target);
    getcwd(current_path, sizeof(current_path));
    ret = reload_dir();
    if (ret == 0) {
        fileman_tree_sync_navigation(old_path, current_path);
    }
    address_edit_sync_path();
    return ret;
}

void address_edit_sync_path(void)
{
    if (!address_edit.buffer) {
        return;
    }
    copy_text(address_input, sizeof(address_input), current_path);
    reliefos_ui_edit_state_sync(&address_edit);
    address_edit.focused = 0;
    address_edit.selecting = 0;
}
