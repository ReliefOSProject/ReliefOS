#include "fileman.h"
#include "archive.h"
#include <errno.h>
#include <reliefos/launch_result.h>

/* Land the cursor on `name` after a refresh, scrolling it into view. */
static void select_entry_by_name(const char *name, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        if (text_eq(entries[i].name, name)) {
            file_list.selected = (int32_t)i;
            if (i >= file_list.scroll + file_list.visible_rows) {
                file_list.scroll = (int32_t)(i - file_list.visible_rows + 1);
            }
            return;
        }
    }
}

void open_selected_entry(void)
{
    char path[RELIEFOS_FS_PATH_LEN];
    int pid;
    if (file_list.selected < 0 || (uint32_t)file_list.selected >= entry_count) {
        set_status(T("Select an item"));
        return;
    }
    build_child_path(path, sizeof(path), entries[file_list.selected].name);
    if (entries[file_list.selected].type == RELIEFOS_FS_TYPE_DIR) {
        navigate_to_path(path);
        return;
    }
    if (ends_with(entries[file_list.selected].name, ".tar")) {
        extract_tar_with_path(path);
        return;
    }
#ifdef CONFIG_DESKTOP_BACKEND_XORG
    pid = fileman_launch_path(path);
#else
    {
        char *argv[] = {path, 0};
        pid = reliefos_launch_argv(argv);
    }
#endif
    if (pid < 0) {
        if (pid == LAUNCH_RESULT_NO_ASSOCIATION) {
            show_open_with_for_path(path, 0);
        } else if (reliefos_launch_is_error(pid)) {
            set_status(reliefos_launch_error_text(pid));
        } else {
            set_status_code("Launch failed ", pid);
        }
    } else {
        char buf[96];
        uint32_t pos = 0;
        buf[0] = 0;
        append_text(buf, &pos, sizeof(buf), "Launched pid ");
        append_dec(buf, &pos, sizeof(buf), (uint32_t)pid);
        append_text(buf, &pos, sizeof(buf), " from ");
        append_text(buf, &pos, sizeof(buf), entries[file_list.selected].name);
        set_status(buf);
    }
    printf("[fileman.elf] open path=%s pid=%d\n", path, pid);
}

void navigate_up(void)
{
    char path[RELIEFOS_FS_PATH_LEN];
    if (is_root_path(current_path)) {
        set_status("Already at root");
        return;
    }
    build_parent_path(path, sizeof(path));
    navigate_to_path(path);
}

void navigate_root(void)
{
    navigate_to_path("/");
}

void create_new_folder(void)
{
    char name[RELIEFOS_FS_NAME_LEN] = "New Folder";
    char path[RELIEFOS_FS_PATH_LEN];
    int ret;
    if (!reliefos_ui_show_input_dialog(T("New Folder"), T("Folder name:"), name, sizeof(name))) {
        set_status(T("New folder canceled"));
        return;
    }
    if (!name[0]) {
        set_status(T("Folder name is empty"));
        return;
    }
#ifdef CONFIG_DESKTOP_BACKEND_XORG
    if (!fileman_name_valid(name) || fileman_join_path(path, sizeof(path), current_path, name) < 0) {
        set_status(T("Invalid folder name or path too long"));
        return;
    }
#else
    build_child_path(path, sizeof(path), name);
#endif
    ret = mkdir(path, 0777);
    if (ret < 0) {
        uint32_t elevated_count = 0;
        /* A protected parent directory: create it through the broker, which
         * also hands back the refreshed listing. */
        if (permission_error(ret) &&
            fileman_mkdir_elevated(current_path, name, entries,
                                   FILEMAN_MAX_ENTRIES,
                                   &elevated_count) == 0) {
            select_entry_by_name(name, elevated_count);
            set_status(T("Folder created (elevated)"));
            return;
        }
        set_status_error("Create folder failed ", ret);
        return;
    }
    reload_dir();
    for (uint32_t i = 0; i < entry_count; ++i) {
        if (text_eq(entries[i].name, name)) {
            file_list.selected = (int32_t)i;
            if (i >= file_list.visible_rows) {
                file_list.scroll = i - file_list.visible_rows + 1;
            }
            break;
        }
    }
    set_status(T("Folder created"));
}

void create_shortcut_for_selected(void)
{
    char target_path[RELIEFOS_FS_PATH_LEN];
    char dest_dir[RELIEFOS_FS_PATH_LEN];
    char shortcut_path[RELIEFOS_FS_PATH_LEN];
    const char *created_name;
    int to_desktop;
    int ret;
    if (!selected_entry_is_file()) {
        set_status(T("Select a file"));
        return;
    }
    build_child_path(target_path, sizeof(target_path), entries[file_list.selected].name);
    to_desktop = reliefos_ui_show_confirm_dialog(
        T("Create Shortcut"),
        T("Place shortcut on Desktop? No creates it here."),
        1);
    if (to_desktop) {
        if (!home_path[0]) {
            refresh_home_path();
        }
        if (!home_path[0]) {
            set_status(T("Desktop folder unavailable"));
            return;
        }
        build_path_join(dest_dir, sizeof(dest_dir), home_path, "desktop");
    } else {
        copy_text(dest_dir, sizeof(dest_dir), current_path);
    }
    ret = reliefos_launch_create_shortcut_in_dir(dest_dir, target_path,
                                               shortcut_path, sizeof(shortcut_path));
    if (ret < 0) {
        if (reliefos_launch_is_error(ret)) {
            set_status(reliefos_launch_error_text(ret));
        } else {
            set_status_error("Create shortcut failed ", ret);
        }
        return;
    }
    if (text_eq(dest_dir, current_path)) {
        reload_dir();
        created_name = path_basename(shortcut_path);
        for (uint32_t i = 0; i < entry_count; ++i) {
            if (text_eq(entries[i].name, created_name)) {
                file_list.selected = (int32_t)i;
                if (i < file_list.scroll) {
                    file_list.scroll = i;
                } else if (i >= file_list.scroll + file_list.visible_rows) {
                    file_list.scroll = i - file_list.visible_rows + 1;
                }
                break;
            }
        }
    }
    set_status(T("Shortcut created"));
}

void rename_selected_entry(void)
{
    char old_path[RELIEFOS_FS_PATH_LEN];
    char new_path[RELIEFOS_FS_PATH_LEN];
    char name[RELIEFOS_FS_NAME_LEN];
    int ret;
    if (!selected_entry_valid()) {
        set_status(T("Select an item"));
        return;
    }
    copy_text(name, sizeof(name), entries[file_list.selected].name);
    if (!reliefos_ui_show_input_dialog(T("Rename"), T("New name:"), name, sizeof(name))) {
        set_status(T("Rename canceled"));
        return;
    }
    if (!name[0]) {
        set_status(T("New name is empty"));
        return;
    }
    build_child_path(old_path, sizeof(old_path), entries[file_list.selected].name);
#ifdef CONFIG_DESKTOP_BACKEND_XORG
    if (!fileman_name_valid(name) || fileman_join_path(new_path, sizeof(new_path), current_path, name) < 0) {
        set_status(T("Invalid file name or path too long"));
        return;
    }
    struct stat existing;
    if (!text_eq(old_path, new_path) && lstat(new_path, &existing) == 0 &&
        !fileman_confirm_dialog(T("Rename Conflict"), T("The destination exists. Replace it?"), 0)) {
        set_status(T("Rename canceled"));
        return;
    }
#else
    build_child_path(new_path, sizeof(new_path), name);
#endif
    ret = rename(old_path, new_path);
    if (ret < 0) {
        uint32_t elevated_count = 0;
        if (permission_error(ret) &&
            fileman_rename_elevated(old_path, new_path, entries,
                                    FILEMAN_MAX_ENTRIES,
                                    &elevated_count) == 0) {
            select_entry_by_name(name, elevated_count);
            set_status(T("Renamed (elevated)"));
            return;
        }
        set_status_error("Rename failed ", ret);
        return;
    }
    reload_dir();
    for (uint32_t i = 0; i < entry_count; ++i) {
        if (text_eq(entries[i].name, name)) {
            file_list.selected = (int32_t)i;
            if (i >= file_list.scroll + file_list.visible_rows) {
                file_list.scroll = i - file_list.visible_rows + 1;
            }
            break;
        }
    }
    set_status(T("Renamed"));
}

void execute_action(uint32_t action)
{
    context_menu_set_active(0);
    switch (action) {
    case FILEMAN_ACTION_OPEN:
        open_selected_entry();
        break;
    case FILEMAN_ACTION_OPEN_WITH:
        show_open_with_selected();
        break;
    case FILEMAN_ACTION_CREATE_SHORTCUT:
        create_shortcut_for_selected();
        break;
    case FILEMAN_ACTION_DEFAULT_PROGRAM:
        show_default_program_for_selected();
        break;
    case FILEMAN_ACTION_DETAILS:
        show_details_selected();
        break;
    case FILEMAN_ACTION_RENAME:
        rename_selected_entry();
        break;
    case FILEMAN_ACTION_DELETE:
        delete_selected_entries();
        break;
    case FILEMAN_ACTION_COPY:
        copy_selected_entries(0);
        break;
    case FILEMAN_ACTION_CUT:
        copy_selected_entries(1);
        break;
    case FILEMAN_ACTION_PASTE:
        paste_clipboard();
        break;
    case FILEMAN_ACTION_TOGGLE_MARK:
        fileman_toggle_selected();
        break;
    case FILEMAN_ACTION_SELECT_ALL:
        fileman_select_all();
        break;
    case FILEMAN_ACTION_CLEAR_SELECTION:
        fileman_clear_selection();
        break;
    case FILEMAN_ACTION_EXTRACT_TAR:
        extract_tar_selected();
        break;
    case FILEMAN_ACTION_COMPRESS_TAR:
        compress_selected_to_tar();
        break;
    case FILEMAN_ACTION_NEW_FOLDER:
        create_new_folder();
        break;
    case FILEMAN_ACTION_UP:
        navigate_up();
        break;
    case FILEMAN_ACTION_REFRESH:
        reload_dir();
        break;
    case FILEMAN_ACTION_SETTINGS:
        fileman_open_settings();
        break;
    default:
        break;
    }
}

void extract_tar_with_path(const char *tar_path)
{
    char dest_dir[RELIEFOS_FS_PATH_LEN];
    if (!tar_path || !ends_with(tar_path, ".tar") || text_len(tar_path) >= sizeof(dest_dir)) {
        set_status(T("Select a tar file"));
        return;
    }
    copy_text(dest_dir, sizeof(dest_dir), tar_path);
    dest_dir[text_len(dest_dir) - 4U] = 0;
    if (!fileman_name_valid(path_basename(dest_dir))) {
        set_status(T("Invalid extraction folder name"));
        return;
    }
    int created = mkdir(dest_dir, 0777) == 0;
    if (!created) {
        struct stat status;
        if (lstat(dest_dir, &status) < 0 || !S_ISDIR(status.st_mode)) {
            set_status(T("Cannot create extraction folder"));
            return;
        }
        if (!reliefos_ui_show_confirm_dialog(T("Extract tar"),
                T("The extraction folder exists. Existing files may be replaced. Continue?"), 0)) {
            set_status(T("Extraction canceled"));
            return;
        }
    }
    fileman_operation_active = 1;
    fileman_operation_percent = 0;
    copy_text(fileman_operation_text, sizeof(fileman_operation_text), T("Extracting tar..."));
    fileman_present_progress();
    int result = fileman_tar_extract(tar_path, dest_dir);
    if (result < 0 && created) (void)rmdir(dest_dir);
    fileman_operation_active = 0;
    fileman_operation_percent = result ? 0 : 100;
    reload_dir();
    set_status(result ? T("Tar extraction failed; check the archive and destination permissions") :
                        T("Tar extracted successfully"));
}

void extract_tar_selected(void)
{
    char tar_path[RELIEFOS_FS_PATH_LEN];
    if (!selected_entry_is_file() || !ends_with(entries[file_list.selected].name, ".tar")) {
        set_status(T("Select a tar file"));
        return;
    }
    build_child_path(tar_path, sizeof(tar_path), entries[file_list.selected].name);
    extract_tar_with_path(tar_path);
}

void compress_selected_to_tar(void)
{
    const char *members[FILEMAN_MAX_ENTRIES];
    uint32_t indices[FILEMAN_MAX_ENTRIES], count = 0;
    uint32_t marked = fileman_selected_count();
    for (uint32_t i = 0; i < entry_count && i < FILEMAN_MAX_ENTRIES; ++i) {
        if (!(marked ? fileman_entry_marked(i) : i == (uint32_t)file_list.selected) ||
            fileman_entry_is_device(i)) continue;
        indices[count] = i;
        members[count++] = entries[i].name;
    }
    if (!count) { set_status(T("Select an item")); return; }
    char name[RELIEFOS_FS_NAME_LEN + 5], tar_path[RELIEFOS_FS_PATH_LEN];
    char source[RELIEFOS_FS_PATH_LEN];
    snprintf(name, sizeof(name), "%s.tar", count == 1 ? members[0] : "archive");
    if (fileman_join_path(tar_path, sizeof(tar_path), current_path, name) < 0) {
        set_status(T("Path too long"));
        return;
    }
    struct stat existing;
    if (lstat(tar_path, &existing) == 0 &&
        !reliefos_ui_show_confirm_dialog(T("Archive Conflict"), T("The archive exists. Replace it?"), 0)) {
        set_status(T("Compression canceled"));
        return;
    }
    copy_text(source, sizeof(source), current_path);
    if (count == 1 && entries[indices[0]].type == RELIEFOS_FS_TYPE_DIR) {
        struct stat status;
        if (fileman_join_path(source, sizeof(source), current_path, members[0]) < 0) {
            set_status(T("Path too long")); return;
        }
        if (lstat(source, &status) == 0 && S_ISDIR(status.st_mode)) members[0] = ".";
        else copy_text(source, sizeof(source), current_path);
    }
    fileman_operation_active = 1;
    fileman_operation_percent = 0;
    copy_text(fileman_operation_text, sizeof(fileman_operation_text), T("Compressing to tar..."));
    fileman_present_progress();
    int result = fileman_tar_create(tar_path, source, members, count);
    fileman_operation_active = 0;
    fileman_operation_percent = result ? 0 : 100;
    reload_dir();
    if (result < 0) set_status_error(T("Tar creation failed"), result);
    else set_status(T("Tar created successfully"));
}
