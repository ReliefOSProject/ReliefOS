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
#ifdef CONFIG_DESKTOP_BACKEND_XORG
    char message[160];
    snprintf(message, sizeof(message), "%s: %s", prefix, strerror(value == -1 ? errno : -value));
    set_status(message);
#else
    if (permission_error(value)) {
        set_status(T("Permission denied"));
    } else {
        set_status_code(prefix, value);
    }
#endif
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

#ifndef CONFIG_DESKTOP_BACKEND_XORG
static void draw_permissions_page(struct reliefos_ui_surface *ui,
                                  const struct stat *st, const char *message)
{
    char text[96];
    snprintf(text, sizeof(text), "UID: %lu    GID: %lu",
             (unsigned long)st->st_uid, (unsigned long)st->st_gid);
    reliefos_ui_text(ui, 24, 54, text, RELIEFOS_UI_BLACK, RELIEFOS_UI_GRAY);
    snprintf(text, sizeof(text), "%s %03o", T("Permissions:"), (unsigned)(st->st_mode & 07777));
    reliefos_ui_text(ui, 24, 80, text, RELIEFOS_UI_BLACK, RELIEFOS_UI_GRAY);
    const char *labels[] = {T("Owner"), T("Group"), T("Other Users")};
    const char *bits[] = {"R", "W", "X"};
    for (unsigned col = 0; col < 3; ++col)
        reliefos_ui_text(ui, 184 + col * 48, 110, bits[col], RELIEFOS_UI_BLACK, RELIEFOS_UI_GRAY);
    for (unsigned row = 0; row < 3; ++row) {
        reliefos_ui_text(ui, 24, 140 + row * 32, labels[row], RELIEFOS_UI_BLACK, RELIEFOS_UI_GRAY);
        for (unsigned col = 0; col < 3; ++col)
            reliefos_ui_checkbox(ui, 180 + col * 48, 138 + row * 32, "",
                               (st->st_mode & (0400u >> (row * 3 + col))) != 0, 0);
    }
    reliefos_ui_text_clipped(ui, 24, 258, FILEMAN_DETAILS_W - 48, message ? message : "",
                           RELIEFOS_UI_BLACK, RELIEFOS_UI_GRAY);
    reliefos_ui_button(ui, 24, FILEMAN_DETAILS_H - 38, 144, RELIEFOS_UI_BUTTON_H,
                     T("Owner / Group"), 0);
    reliefos_ui_button(ui, 180, FILEMAN_DETAILS_H - 38, 88, RELIEFOS_UI_BUTTON_H,
                     T("Mode Value"), 0);
    reliefos_ui_button(ui, 368, FILEMAN_DETAILS_H - 38, 82, RELIEFOS_UI_BUTTON_H,
                     T("Save"), 0);
}

static int permissions_hit(struct stat *st, int32_t x, int32_t y)
{
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned col = 0; col < 3; ++col)
            if (hit_rect_i(x, y, 180 + col * 48, 138 + row * 32, 18, 18)) {
                st->st_mode ^= 0400u >> (row * 3 + col);
                return 1;
            }
    return 0;
}

void show_details_selected(void)
{
    struct reliefos_ui_surface ui;
    struct reliefos_gui_app_event event;
    struct reliefos_stat st;
    struct stat permissions;
    char path[RELIEFOS_FS_PATH_LEN];
    char size_line[56];
    char contains_line[72];
    char acl_message[96];
    struct folder_size_info folder_info = {0};
    uint32_t active_tab = 0;
    struct reliefos_ui_tab_state details_tabs;
    int acl_loaded;
    int window_id;
    if (!selected_entry_valid()) {
        set_status(T("Select an item"));
        return;
    }
    build_child_path(path, sizeof(path), entries[file_list.selected].name);
    if (reliefos_stat_legacy(path, &st) < 0) {
        set_status("Details stat failed");
        return;
    }
    if (st.type == RELIEFOS_FS_TYPE_DIR) {
        (void)accumulate_folder_size(path, &folder_info, 0);
        format_size_text(size_line, sizeof(size_line), folder_info.bytes);
        format_contains_text(contains_line, sizeof(contains_line), &folder_info);
    } else {
        format_size_text(size_line, sizeof(size_line), st.size);
        contains_line[0] = 0;
    }
    permissions = (struct stat){0};
    acl_message[0] = 0;
    acl_loaded = stat(path, &permissions);
    if (acl_loaded < 0) {
        set_status_error("Permission load failed ", errno);
        copy_text(acl_message, sizeof(acl_message), T("Could not load permissions"));
    }

    window_id = reliefos_gui_create_app_window_ex(T("Properties"), path,
                                                FILEMAN_DETAILS_W, FILEMAN_DETAILS_H,
                                                RELIEFOS_GUI_WINDOW_NO_RESIZE);
    if (window_id <= 0) {
        set_status_code("Details failed ", window_id);
        return;
    }
    reliefos_ui_bind(&ui, details_pixels, FILEMAN_DETAILS_W, FILEMAN_DETAILS_H,
                   FILEMAN_DETAILS_W);
    reliefos_ui_tab_state_init(&details_tabs, active_tab);
    for (;;) {
        struct reliefos_ui_tab_item tabs[] = {
            {T("General"), 0, 0},
            {T("Security"), 1, 0},
        };
        struct reliefos_ui_property_item props[5];
        uint32_t prop_count = 4;
        props[0] = (struct reliefos_ui_property_item){T("Name:"), entries[file_list.selected].name, 0};
        props[1] = (struct reliefos_ui_property_item){T("Type:"), entry_type_name(&entries[file_list.selected]), 0};
        props[2] = (struct reliefos_ui_property_item){T("Path:"), path, 0};
        props[3] = (struct reliefos_ui_property_item){T("Size:"), size_line, 0};
        if (st.type == RELIEFOS_FS_TYPE_DIR) {
            props[prop_count++] = (struct reliefos_ui_property_item){T("Contains:"), contains_line, 0};
        }
        reliefos_ui_rect(&ui, 0, 0, FILEMAN_DETAILS_W, FILEMAN_DETAILS_H, RELIEFOS_UI_GRAY);
        details_tabs.selected_id = active_tab;
        reliefos_ui_tab_control(&ui, 16, 10, FILEMAN_DETAILS_W - 32, tabs, 2,
                              &details_tabs);
        reliefos_ui_tab_body(&ui, 16, 36, FILEMAN_DETAILS_W - 32, FILEMAN_DETAILS_H - 84);
        if (active_tab == 0) {
            reliefos_ui_property_grid(&ui, 28, 56, FILEMAN_DETAILS_W - 56,
                                    props, prop_count, 86, 24);
        } else if (acl_loaded == 0) {
            draw_permissions_page(&ui, &permissions, acl_message);
        } else {
            reliefos_ui_text(&ui, 28, 56, T("Permission information is unavailable."),
                           RELIEFOS_UI_BLACK, RELIEFOS_UI_GRAY);
            reliefos_ui_text_clipped(&ui, 28, 84, FILEMAN_DETAILS_W - 56,
                                   acl_message, RELIEFOS_UI_BLACK, RELIEFOS_UI_GRAY);
            reliefos_ui_button(&ui, 152, FILEMAN_DETAILS_H - 38, 88, RELIEFOS_UI_BUTTON_H,
                             T("Reload"), 0);
        }
        reliefos_ui_button(&ui, FILEMAN_DETAILS_W - 90, FILEMAN_DETAILS_H - 38,
                         72, RELIEFOS_UI_BUTTON_H, "OK", 0);
        reliefos_gui_present_window((uint32_t)window_id, FILEMAN_DETAILS_W,
                                  FILEMAN_DETAILS_H, FILEMAN_DETAILS_W,
                                  details_pixels);
        event.window_id = (uint32_t)window_id;
        if (reliefos_gui_wait_app_event(&event, RELIEFOS_GUI_IDLE_WAIT_MS) > 0) {
            if (event.type == RELIEFOS_GUI_APP_EVENT_CLOSE) {
                break;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_KEY_DOWN &&
                (event.keycode == RELIEFOS_KEY_ENTER || event.keycode == FILEMAN_KEY_ESCAPE)) {
                break;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_MOUSE_BUTTON && (event.buttons & 1u)) {
                if (reliefos_ui_tab_control_handle_mouse(&details_tabs, event.x, event.y,
                                                       16, 10, FILEMAN_DETAILS_W - 32,
                                                       tabs, 2)) {
                    active_tab = details_tabs.selected_id;
                    continue;
                }
                if (hit_rect_i(event.x, event.y, FILEMAN_DETAILS_W - 90,
                               FILEMAN_DETAILS_H - 38, 72,
                               (int32_t)RELIEFOS_UI_BUTTON_H)) {
                    break;
                }
                if (active_tab == 1) {
                    if (acl_loaded == 0 && permissions_hit(&permissions, event.x, event.y)) {
                        copy_text(acl_message, sizeof(acl_message),
                                  T("Unsaved changes"));
                        continue;
                    }
                    if (acl_loaded == 0 && hit_rect_i(event.x, event.y, 24, FILEMAN_DETAILS_H - 38,
                                   144, (int32_t)RELIEFOS_UI_BUTTON_H)) {
                        char value[48], *end;
                        snprintf(value, sizeof(value), "%lu:%lu", (unsigned long)permissions.st_uid,
                                 (unsigned long)permissions.st_gid);
                        if (reliefos_ui_show_input_dialog(T("Ownership"), "UID:GID", value, sizeof(value))) {
                            errno = 0;
                            unsigned long uid = strtoul(value, &end, 10);
                            if (end != value && *end == ':' && uid < UINT32_MAX && !errno) {
                                const char *group = end + 1;
                                unsigned long gid = strtoul(group, &end, 10);
                                if (end != group && !*end && gid < UINT32_MAX && !errno) {
                                    permissions.st_uid = (uid_t)uid;
                                    permissions.st_gid = (gid_t)gid;
                                    copy_text(acl_message, sizeof(acl_message), T("Unsaved changes"));
                                    continue;
                                }
                            }
                            copy_text(acl_message, sizeof(acl_message), T("Invalid UID or GID"));
                        }
                        continue;
                    }
                    if (acl_loaded == 0 && hit_rect_i(event.x, event.y, 180, FILEMAN_DETAILS_H - 38,
                                   88, (int32_t)RELIEFOS_UI_BUTTON_H)) {
                        char value[16], *end;
                        snprintf(value, sizeof(value), "%03o", (unsigned)(permissions.st_mode & 07777));
                        if (reliefos_ui_show_input_dialog(T("Permissions"), T("Mode Value"), value, sizeof(value))) {
                            errno = 0;
                            unsigned long mode = strtoul(value, &end, 8);
                            if (end != value && !*end && mode <= 07777 && !errno) {
                                permissions.st_mode = (permissions.st_mode & ~07777) | (mode_t)mode;
                                copy_text(acl_message, sizeof(acl_message), T("Unsaved changes"));
                            } else copy_text(acl_message, sizeof(acl_message), T("Invalid mode"));
                        }
                        continue;
                    }
                    if (acl_loaded < 0 && hit_rect_i(event.x, event.y, 152, FILEMAN_DETAILS_H - 38,
                                                     88, (int32_t)RELIEFOS_UI_BUTTON_H)) {
                        acl_loaded = stat(path, &permissions);
                        continue;
                    }
                    if (acl_loaded == 0 &&
                        hit_rect_i(event.x, event.y, 368, FILEMAN_DETAILS_H - 38,
                                   82, (int32_t)RELIEFOS_UI_BUTTON_H)) {
                        struct stat current;
                        int ret = stat(path, &current);
                        if (!ret && (current.st_uid != permissions.st_uid || current.st_gid != permissions.st_gid))
                            ret = chown(path, permissions.st_uid, permissions.st_gid);
                        if (!ret) ret = chmod(path, permissions.st_mode & 07777);
                        int error = errno;
                        acl_loaded = stat(path, &permissions);
                        if (!ret && acl_loaded < 0) error = errno;
                        if (!ret && !acl_loaded) copy_text(acl_message, sizeof(acl_message), T("Permissions saved"));
                        else snprintf(acl_message, sizeof(acl_message), "%s: %s", T("Save failed"), strerror(error));
                        continue;
                    }
                }
            }
        } else {
            sleep_ms(10);
        }
    }
    reliefos_gui_destroy_app_window((uint32_t)window_id);
}

void show_open_with_for_path(const char *path, uint8_t set_default_only)
{
    char program[RELIEFOS_FS_PATH_LEN];
    char extension[16];
    uint32_t remember = set_default_only ? 1 : 0;
    uint32_t flags = set_default_only ? RELIEFOS_UI_OPEN_WITH_SET_DEFAULT : 0;
    int ret;
    int pid;
    program[0] = 0;
    extension[0] = 0;
    menu_open = FILEMAN_MENU_NONE;
    context_menu_set_active(0);
    ret = reliefos_ui_show_open_with_dialog(set_default_only ? T("Default Program") : T("Open With"),
                                          path, program, sizeof(program),
                                          &remember, flags);
    if (ret < 0) {
        set_status_code("Open With failed ", ret);
        return;
    }
    if (ret == 0) {
        set_status(T("Open With canceled"));
        return;
    }
    if (set_default_only || remember) {
        int assoc_ret;
        if (!reliefos_launch_get_extension_for_path(path, extension, sizeof(extension))) {
            if (set_default_only) {
                set_status("This file has no extension to remember");
                return;
            }
        } else {
            assoc_ret = reliefos_launch_set_extension_association(extension, program);
            if (assoc_ret < 0) {
                set_status_code("Save association failed ", assoc_ret);
                return;
            }
            if (set_default_only) {
                char buf[96];
                uint32_t pos = 0;
                buf[0] = 0;
                append_text(buf, &pos, sizeof(buf), "Default app set for ");
                append_text(buf, &pos, sizeof(buf), extension);
                set_status(buf);
                return;
            }
        }
    }

    pid = reliefos_launch_file_with_app(path, program);
    printf("[fileman.elf] open-with path=%s app=%s pid=%d\n", path, program, pid);
    if (pid < 0) {
        set_status_code("Open With failed ", pid);
        return;
    }
    {
        char buf[96];
        uint32_t pos = 0;
        buf[0] = 0;
        append_text(buf, &pos, sizeof(buf), "Open With launched pid ");
        append_dec(buf, &pos, sizeof(buf), (uint32_t)pid);
        set_status(buf);
    }
}

#endif

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

/** Refresh the root tree so new /mnt and /media mounts become visible. */
static void fileman_tree_refresh_mounts(void)
{
#ifndef CONFIG_DESKTOP_BACKEND_XORG
    fileman_tree_reset();
#endif
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
    fileman_tree_refresh_mounts();
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
