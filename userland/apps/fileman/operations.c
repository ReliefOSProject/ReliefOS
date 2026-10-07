#include "fileman.h"
#include "filesystem.h"
#include <errno.h>

#define FILEMAN_CLIPBOARD_MAX FILEMAN_MAX_ENTRIES
#define FILEMAN_COPY_BUFFER_SIZE 1024U
#define FILEMAN_COPY_MAX_DEPTH 16U

static char clipboard_paths[FILEMAN_CLIPBOARD_MAX][RELIEFOS_FS_PATH_LEN];
static uint32_t clipboard_count;
static uint8_t clipboard_cut;

static void operation_set(uint32_t percent, const char *text)
{
    fileman_operation_active = 1;
    fileman_operation_percent = percent > 100U ? 100U : percent;
    copy_text(fileman_operation_text, sizeof(fileman_operation_text), text);
    fileman_present_progress();
}

void fileman_present_progress(void)
{
    if (fileman_window_id) {
        present_fileman(fileman_window_id, &fileman_ui);
    }
}

static void operation_finish(const char *text)
{
    fileman_operation_percent = 100;
    copy_text(fileman_operation_text, sizeof(fileman_operation_text), text);
    fileman_present_progress();
    fileman_operation_active = 0;
    set_status(text);
}

static void build_path_in_dir(char *dst, uint32_t cap, const char *dir,
                              const char *name)
{
    build_path_join(dst, cap, dir, name);
}

static void parent_path_of(char *dst, uint32_t cap, const char *path)
{
    uint32_t len;
    copy_text(dst, cap, path);
    len = text_len(dst);
    while (len > 1U && dst[len - 1U] != '/') {
        dst[--len] = 0;
    }
    if (len > 1U) {
        dst[len - 1U] = 0;
    }
}

static int path_is_same_or_child(const char *parent, const char *path)
{
    uint32_t i = 0;
    while (parent[i] && path[i] && parent[i] == path[i]) {
        ++i;
    }
    return !parent[i] && (!path[i] || path[i] == '/');
}

static void build_copy_name(char *dst, uint32_t cap, const char *name,
                            uint32_t serial)
{
    uint32_t pos = 0;
    uint32_t dot = 0;
    uint32_t len = text_len(name);
    for (uint32_t i = 0; i < len; ++i) {
        if (name[i] == '.') {
            dot = i;
        }
    }
    if (!dot) {
        dot = len;
    }
    for (uint32_t i = 0; i < dot; ++i) {
        append_char(dst, &pos, cap, name[i]);
    }
    append_text(dst, &pos, cap, "-copy-");
    append_dec(dst, &pos, cap, serial);
    for (uint32_t i = dot; i < len; ++i) {
        append_char(dst, &pos, cap, name[i]);
    }
}

static int choose_target_path(const char *dir, const char *name,
                              char *dst, uint32_t cap)
{
    struct reliefos_stat st;
    char candidate[RELIEFOS_FS_NAME_LEN];
    build_path_in_dir(dst, cap, dir, name);
    if (reliefos_stat_legacy(dst, &st) < 0) {
        return 0;
    }
    if (reliefos_ui_show_confirm_dialog(T("File Conflict"),
                                      T("The destination exists. Replace it? Choose No to save with another name."),
                                      0)) {
        return 1;
    }
    copy_text(candidate, sizeof(candidate), name);
    for (uint32_t serial = 2; serial < 100U; ++serial) {
        build_copy_name(candidate, sizeof(candidate), name, serial);
        build_path_in_dir(dst, cap, dir, candidate);
        if (reliefos_stat_legacy(dst, &st) < 0) {
            return 0;
        }
    }
    return -1;
}

static int choose_free_target_path(const char *dir, const char *name,
                                   char *dst, uint32_t cap)
{
    struct reliefos_stat st;
    char candidate[RELIEFOS_FS_NAME_LEN];
    build_path_in_dir(dst, cap, dir, name);
    if (reliefos_stat_legacy(dst, &st) < 0) {
        return 0;
    }
    for (uint32_t serial = 2; serial < 100U; ++serial) {
        build_copy_name(candidate, sizeof(candidate), name, serial);
        build_path_in_dir(dst, cap, dir, candidate);
        if (reliefos_stat_legacy(dst, &st) < 0) {
            return 0;
        }
    }
    return -1;
}

static int copy_file(const char *src, const char *dst, uint64_t total,
                     uint64_t *done, uint32_t base_percent,
                     uint32_t span_percent)
{
    char buffer[FILEMAN_COPY_BUFFER_SIZE];
    int in = open(src, RELIEFOS_O_RDONLY, 0);
    int out;
    if (in < 0) {
        return in;
    }
    struct stat source;
    if (fstat(in, &source) < 0) { close(in); return -1; }
    out = open(dst, RELIEFOS_O_WRONLY | RELIEFOS_O_CREAT | RELIEFOS_O_TRUNC, source.st_mode & 0777);
    if (out < 0) {
        close(in);
        return out;
    }
    for (;;) {
        long got = read(in, buffer, sizeof(buffer));
        if (got < 0) {
            close(in);
            close(out);
            return (int)got;
        }
        if (got == 0) {
            break;
        }
        if (write(out, buffer, (uint32_t)got) != got) {
            close(in);
            close(out);
            return -1;
        }
        *done += (uint32_t)got;
        if (total) {
            uint32_t progress = base_percent +
                (uint32_t)((*done * span_percent) / total);
            operation_set(progress, T("Copying files..."));
        }
    }
    close(in);
    close(out);
    return 0;
}

static int copy_tree(const char *src, const char *dst, uint64_t total,
                     uint64_t *done, uint32_t base_percent,
                     uint32_t span_percent, uint32_t depth)
{
    struct reliefos_stat st;
    int ret;
    if (depth > FILEMAN_COPY_MAX_DEPTH || reliefos_stat_legacy(src, &st) < 0) {
        return -1;
    }
    if (st.type != RELIEFOS_FS_TYPE_DIR) {
        return copy_file(src, dst, total, done, base_percent, span_percent);
    }
    ret = mkdir(dst, 0777);
    if (ret < 0 && reliefos_stat_legacy(dst, &(struct reliefos_stat){0}) < 0) {
        return ret;
    }
    {
        int fd = open(src, RELIEFOS_O_RDONLY, 0);
        if (fd < 0) {
            return fd;
        }
        for (;;) {
            struct reliefos_dir_entry entry;
            char child_src[RELIEFOS_FS_PATH_LEN];
            char child_dst[RELIEFOS_FS_PATH_LEN];
            ret = reliefos_readdir(fd, &entry);
            if (ret <= 0) {
                break;
            }
            build_path_in_dir(child_src, sizeof(child_src), src, entry.name);
            build_path_in_dir(child_dst, sizeof(child_dst), dst, entry.name);
            ret = copy_tree(child_src, child_dst, total, done, base_percent,
                            span_percent, depth + 1U);
            if (ret < 0) {
                close(fd);
                return ret;
            }
        }
        close(fd);
    }
    return ret < 0 ? ret : 0;
}

static uint64_t path_bytes(const char *path)
{
    struct reliefos_stat st;
    struct folder_size_info info = {0};
    if (reliefos_stat_legacy(path, &st) < 0) {
        return 0;
    }
    if (st.type == RELIEFOS_FS_TYPE_DIR) {
        (void)accumulate_folder_size(path, &info, 0);
        return info.bytes;
    }
    return st.size;
}

int fileman_clipboard_available(void)
{
    return clipboard_count != 0;
}

void copy_selected_entries(uint8_t cut)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < entry_count && i < FILEMAN_CLIPBOARD_MAX; ++i) {
        if (fileman_entry_marked(i) ||
            (!fileman_selected_count() && i == (uint32_t)(file_list.selected < 0 ? entry_count : file_list.selected))) {
            if (fileman_entry_is_device(i)) {
                continue;
            }
            build_child_path(clipboard_paths[count], sizeof(clipboard_paths[count]),
                             entries[i].name);
            ++count;
        }
    }
    clipboard_count = count;
    clipboard_cut = cut;
    set_status(count ? (cut ? T("Items cut. Open a folder and paste.")
                            : T("Items copied. Open a folder and paste."))
                     : T("Select an item"));
}

void paste_clipboard(void)
{
    uint64_t total = 0;
    uint64_t done = 0;
    uint32_t completed = 0;
    uint32_t failed = 0;
    uint8_t was_cut = clipboard_cut;
    if (!clipboard_count) {
        set_status(T("Clipboard is empty"));
        return;
    }
    for (uint32_t i = 0; i < clipboard_count; ++i) {
        total += path_bytes(clipboard_paths[i]);
    }
    operation_set(0, was_cut ? T("Moving files...")
                             : T("Copying files..."));
    for (uint32_t i = 0; i < clipboard_count; ++i) {
        char target[RELIEFOS_FS_PATH_LEN];
        char source_parent[RELIEFOS_FS_PATH_LEN];
        const char *name = path_basename(clipboard_paths[i]);
        int conflict;
        int ret;
        parent_path_of(source_parent, sizeof(source_parent), clipboard_paths[i]);
        if (text_eq(source_parent, current_path)) {
            if (was_cut || choose_free_target_path(current_path, name, target,
                                                   sizeof(target)) != 0) {
                ++failed;
                continue;
            }
            conflict = 0;
        } else {
            if (path_is_same_or_child(clipboard_paths[i], current_path)) {
                ++failed;
                continue;
            }
            conflict = choose_target_path(current_path, name, target, sizeof(target));
        }
        if (conflict < 0) {
            ++failed;
            continue;
        }
        if (conflict > 0) {
            ret = fileman_remove_tree(target);
            if (ret < 0) {
                ++failed;
                continue;
            }
        }
        if (was_cut) {
            ret = rename(clipboard_paths[i], target);
            if (ret == 0) {
                done += path_bytes(target);
            } else {
                ret = copy_tree(clipboard_paths[i], target, total, &done, 0, 100, 0);
                if (ret == 0) {
                    ret = fileman_remove_tree(clipboard_paths[i]);
                }
            }
        } else {
            ret = copy_tree(clipboard_paths[i], target, total, &done, 0, 100, 0);
        }
        if (ret < 0) {
            ++failed;
        } else {
            ++completed;
        }
        operation_set(total ? (uint32_t)((done * 100ULL) / total)
                            : ((i + 1U) * 100U) / clipboard_count,
                      was_cut ? T("Moving files...")
                              : T("Copying files..."));
    }
    if (was_cut && !failed) {
        clipboard_count = 0;
        clipboard_cut = 0;
    }
    reload_dir();
    selected_mask = 0;
    if (failed) {
        char status[160];
        uint32_t pos = 0;
        status[0] = 0;
        append_dec(status, &pos, sizeof(status), completed);
        append_text(status, &pos, sizeof(status), T(" item(s) completed; "));
        append_dec(status, &pos, sizeof(status), failed);
        append_text(status, &pos, sizeof(status), T(" failed"));
        operation_finish(status);
    } else {
        operation_finish(was_cut ? T("Move complete")
                                 : T("Copy complete"));
    }
}

void delete_selected_entries(void)
{
    char paths[FILEMAN_MAX_ENTRIES][RELIEFOS_FS_PATH_LEN];
    uint32_t count = 0, removed = 0, failed = 0;
    uint32_t marked = fileman_selected_count();
    int error = 0;
    for (uint32_t i = 0; i < entry_count && i < FILEMAN_MAX_ENTRIES; ++i) {
        if (!(marked ? fileman_entry_marked(i) : i == (uint32_t)file_list.selected) ||
            fileman_entry_is_device(i)) continue;
        if (fileman_join_path(paths[count], sizeof(paths[count]), current_path, entries[i].name) < 0) {
            set_status(T("Invalid file name or path too long"));
            return;
        }
        ++count;
    }
    if (!count) { set_status(T("Select an item")); return; }
    if (!reliefos_ui_show_confirm_dialog(T("Delete"),
            T("Permanently delete the selected items and all folder contents? This cannot be undone."), 0)) {
        set_status(T("Delete canceled"));
        return;
    }
    operation_set(0, T("Deleting items..."));
    for (uint32_t i = 0; i < count; ++i) {
        int result = fileman_remove_tree(paths[i]);
        if (permission_error(result)) {
            struct stat status;
            uint32_t elevated_count = 0;
            if (lstat(paths[i], &status) == 0 &&
                fileman_delete_elevated(paths[i], S_ISDIR(status.st_mode), entries,
                    FILEMAN_MAX_ENTRIES, &elevated_count) == 0) result = 0;
        }
        if (!result) ++removed;
        else { ++failed; error = result; }
        operation_set((i + 1U) * 100U / count, T("Deleting items..."));
    }
    reload_dir();
    selected_mask = 0;
    if (failed) {
        char message[160];
        snprintf(message, sizeof(message), "%u %s; %u %s: %s", removed,
            T("item(s) deleted"), failed, T("failed"), strerror(-error));
        operation_finish(message);
    } else operation_finish(T("Items deleted"));
}
