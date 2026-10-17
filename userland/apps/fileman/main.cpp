#include "fileman.h"

extern "C" {
#include <reliefos/launch_result.h>
}

#include "fltk_support.h"

#include <FL/Fl_Browser.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Multi_Browser.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Window.H>
#include <FL/fl_ask.H>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

enum { ACTION_ABOUT = 1000 };

Fl_Double_Window *main_window;
Fl_Menu_Bar *menu_bar;
Fl_Input *address;
Fl_Hold_Browser *folders;
Fl_Multi_Browser *files;
Fl_Output *status_output;
Fl_Output *operation_output;
Fl_Progress *progress;
Fl_Menu_Button *context_menu;
struct reliefos_ui_tree_item tree_items[FILEMAN_TREE_MAX_NODES];
uint32_t tree_count;
int updating;

void refresh_ui();
void perform_action(uint32_t action);
void show_context_menu();
void menu_action(Fl_Widget *, void *data);

int action_enabled(uint32_t action)
{
    switch (action) {
    case FILEMAN_ACTION_OPEN:
    case FILEMAN_ACTION_DETAILS:
    case FILEMAN_ACTION_TOGGLE_MARK:
        return selected_entry_valid();
    case FILEMAN_ACTION_OPEN_WITH:
    case FILEMAN_ACTION_DEFAULT_PROGRAM:
    case FILEMAN_ACTION_CREATE_SHORTCUT:
        return selected_entry_is_file();
    case FILEMAN_ACTION_RENAME:
        return selected_entry_is_mutable() && fileman_selected_count() <= 1;
    case FILEMAN_ACTION_COPY:
    case FILEMAN_ACTION_CUT:
    case FILEMAN_ACTION_DELETE:
        return selected_entry_is_mutable();
    case FILEMAN_ACTION_PASTE:
        return fileman_clipboard_available();
    case FILEMAN_ACTION_SELECT_ALL:
        return entry_count != 0;
    case FILEMAN_ACTION_CLEAR_SELECTION:
        return fileman_selected_count() != 0;
    case FILEMAN_ACTION_EXTRACT_TAR:
        return selected_entry_is_file() && ends_with(entries[file_list.selected].name, ".tar");
    case FILEMAN_ACTION_COMPRESS_TAR:
        return selected_entry_is_mutable();
    default:
        return 1;
    }
}

int x11_app_allowed(const char *program)
{
    const char *apps = RELIEFOS_LAYOUT_RELIEFOS_APPS "/";
    if (!program || strncmp(program, apps, strlen(apps))) return 1;
    static const char *const compatible_apps[] = {
        RELIEFOS_LAYOUT_RELIEFOS_APPS "/calc/calc.elf",
        RELIEFOS_LAYOUT_RELIEFOS_APPS "/doom/doom.elf",
        RELIEFOS_LAYOUT_RELIEFOS_APPS "/doomlauncher/doomlauncher.elf",
        RELIEFOS_LAYOUT_RELIEFOS_APPS "/fileman/fileman.elf",
        RELIEFOS_LAYOUT_RELIEFOS_APPS "/imageview/imageview.elf",
        RELIEFOS_LAYOUT_RELIEFOS_APPS "/installer/installer.elf",
        RELIEFOS_LAYOUT_RELIEFOS_APPS "/leonmmcoset/leonmmcoset.elf",
        RELIEFOS_LAYOUT_RELIEFOS_APPS "/minesweeper/minesweeper.elf",
        RELIEFOS_LAYOUT_RELIEFOS_APPS "/osver/osver.elf",
        RELIEFOS_LAYOUT_RELIEFOS_APPS "/paint/paint.elf",
        RELIEFOS_LAYOUT_RELIEFOS_APPS "/taskmgr/taskmgr.elf",
        RELIEFOS_LAYOUT_RELIEFOS_APPS "/xiaobai/xiaobai.elf",
    };
    for (unsigned i = 0; i < sizeof(compatible_apps) / sizeof(compatible_apps[0]); ++i)
        if (!strcmp(program, compatible_apps[i])) return 1;
    return 0;
}

int spawn_app(const char *program, const char *path)
{
    int errors[2];
    if (pipe2(errors, O_CLOEXEC) < 0) return -errno;
    pid_t child = fork();
    if (!child) {
        close(errors[0]);
        const char *argument = path;
        if (!argument && !strcmp(program, RELIEFOS_LAYOUT_RELIEFOS_APPS "/installer/installer.elf"))
            argument = "--graphical";
        char *args[] = {(char *)program, (char *)argument, NULL};
        execv(program, args);
        int error = errno;
        (void)write(errors[1], &error, sizeof(error));
        _exit(127);
    }
    int error = errno;
    close(errors[1]);
    if (child < 0) {
        close(errors[0]);
        return -error;
    }
    ssize_t got;
    do { got = read(errors[0], &error, sizeof(error)); } while (got < 0 && errno == EINTR);
    close(errors[0]);
    if (got != 0) {
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
        return got == (ssize_t)sizeof(error) ? -error : -EIO;
    }
    return (int)child;
}

int launch_path(const char *path, unsigned depth)
{
    if (depth > 8) return RELIEFOS_LAUNCH_ERR_SHORTCUT_LOOP;
    struct stat st;
    if (stat(path, &st) < 0) return -errno;
    if (S_ISDIR(st.st_mode)) return navigate_to_path(path);
    if (!S_ISREG(st.st_mode)) return -EINVAL;
    if (ends_with(path, ".lnk")) {
        FILE *file = fopen(path, "r");
        if (!file) return -errno;
        char line[512], target[RELIEFOS_FS_PATH_LEN] = {0};
        while (fgets(line, sizeof(line), file)) {
            if (strncmp(line, "target=", 7)) continue;
            line[strcspn(line, "\r\n")] = 0;
            if (strlen(line + 7) >= sizeof(target)) {
                fclose(file);
                return -ENAMETOOLONG;
            }
            copy_text(target, sizeof(target), line + 7);
            break;
        }
        fclose(file);
        return target[0] ? launch_path(target, depth + 1) : RELIEFOS_LAUNCH_ERR_INVALID_SHORTCUT;
    }
    const char *program = fileman_x11_default_app(path);
    if (!program) {
        if (!x11_app_allowed(path)) return -ENOTSUP;
        return spawn_app(path, NULL);
    }
    char extension[16], association[RELIEFOS_FS_PATH_LEN];
    if (reliefos_launch_get_extension_for_path(path, extension, sizeof(extension)) &&
        reliefos_launch_get_extension_association(extension, association, sizeof(association)) > 0 &&
        association[0] && x11_app_allowed(association)) program = association;
    return spawn_app(program, path);
}

struct InputDialogState {
    Fl_Window *window;
    Fl_Input *input;
    int answer;
};

void input_accept(Fl_Widget *, void *data)
{
    InputDialogState *state = (InputDialogState *)data;
    state->answer = 1;
    state->window->hide();
}

void input_cancel(Fl_Widget *, void *data)
{
    InputDialogState *state = (InputDialogState *)data;
    state->answer = 0;
    state->window->hide();
}

void close_dialog(Fl_Widget *widget, void *data)
{
    (void)widget;
    InputDialogState *state = (InputDialogState *)data;
    state->answer = 0;
    state->window->hide();
}

struct ConfirmState {
    Fl_Window *window;
    int answer;
};

void confirm_yes(Fl_Widget *, void *data)
{
    ConfirmState *state = (ConfirmState *)data;
    state->answer = 1;
    state->window->hide();
}

void confirm_no(Fl_Widget *, void *data)
{
    ConfirmState *state = (ConfirmState *)data;
    state->answer = 0;
    state->window->hide();
}

void confirm_close(Fl_Widget *, void *data)
{
    ConfirmState *state = (ConfirmState *)data;
    state->answer = 0;
    state->window->hide();
}

int input_dialog(const char *title, const char *prompt, char *value, uint32_t capacity)
{
    if (!value || !capacity) return 0;
    InputDialogState state;
    state.answer = 0;
    state.window = new Fl_Window(560, 146, title);
    Fl_Box *label = new Fl_Box(16, 12, 528, 28, prompt);
    label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    state.input = new Fl_Input(16, 43, 528, 30);
    state.input->value(value);
    Fl_Return_Button *ok = new Fl_Return_Button(352, 94, 88, 32, T("OK"));
    Fl_Button *cancel = new Fl_Button(448, 94, 96, 32, T("Cancel"));
    ok->callback(input_accept, &state);
    cancel->callback(input_cancel, &state);
    state.window->callback(close_dialog, &state);
    state.window->set_modal();
    state.window->end();
    reliefos_fltk::style(state.window);
    state.input->take_focus();
    state.window->show();
    while (state.window->shown()) Fl::wait();
    if (state.answer) {
        if (!reliefos_fltk::copy_input(value, capacity, state.input->value())) {
            fl_alert("%s", T("Input is too long; the value was not changed."));
            state.answer = 0;
        }
    }
    delete state.window;
    return state.answer;
}

int confirm_dialog(const char *title, const char *message, uint32_t default_yes)
{
    ConfirmState state;
    state.answer = 0;
    state.window = new Fl_Window(560, 154, title);
    Fl_Multiline_Output *body = new Fl_Multiline_Output(16, 14, 528, 78);
    body->value(message ? message : "");
    Fl_Button *yes = new Fl_Return_Button(344, 104, 92, 32, T("Yes"));
    Fl_Button *no = new Fl_Button(448, 104, 96, 32, T("No"));
    yes->callback(confirm_yes, &state);
    no->callback(confirm_no, &state);
    state.window->callback(confirm_close, &state);
    state.window->set_modal();
    state.window->end();
    reliefos_fltk::style(state.window);
    if (!default_yes) no->take_focus(); else yes->take_focus();
    state.window->show();
    while (state.window->shown()) Fl::wait();
    int answer = state.answer;
    delete state.window;
    return answer;
}

struct SettingsState {
    Fl_Window *window;
    Fl_Check_Button *show_hidden;
    int answer;
};

void settings_save(Fl_Widget *, void *data)
{
    SettingsState *state = (SettingsState *)data;
    state->answer = 1;
    state->window->hide();
}

void settings_cancel(Fl_Widget *, void *data)
{
    SettingsState *state = (SettingsState *)data;
    state->answer = 0;
    state->window->hide();
}

void settings_close(Fl_Widget *, void *data)
{
    SettingsState *state = (SettingsState *)data;
    state->answer = 0;
    state->window->hide();
}

void show_settings()
{
    fileman_settings_show_hidden = fileman_show_hidden;
    SettingsState state;
    state.answer = 0;
    state.window = new Fl_Window(500, 160, T("File Manager Settings"));
    state.show_hidden = new Fl_Check_Button(18, 24, 460, 32,
        T("Show files and folders starting with a dot"));
    state.show_hidden->value(fileman_settings_show_hidden != 0);
    Fl_Return_Button *save = new Fl_Return_Button(292, 104, 88, 32, T("Save"));
    Fl_Button *cancel = new Fl_Button(392, 104, 88, 32, T("Cancel"));
    save->callback(settings_save, &state);
    cancel->callback(settings_cancel, &state);
    state.window->callback(settings_close, &state);
    state.window->set_modal();
    state.window->end();
    reliefos_fltk::style(state.window);
    state.window->show();
    while (state.window->shown()) Fl::wait();
    if (state.answer) {
        fileman_settings_show_hidden = state.show_hidden->value() ? 1 : 0;
        fileman_apply_settings();
        refresh_ui();
    } else {
        fileman_cancel_settings();
    }
    delete state.window;
}

struct PropertiesState {
    Fl_Window *window;
    Fl_Input *mode;
    Fl_Input *uid;
    Fl_Input *gid;
    Fl_Multiline_Output *summary;
    Fl_Output *message;
    int requested;
};

void properties_save_request(Fl_Widget *, void *data)
{
    ((PropertiesState *)data)->requested = 1;
}

void properties_close_request(Fl_Widget *, void *data)
{
    ((PropertiesState *)data)->requested = 2;
    ((PropertiesState *)data)->window->hide();
}

void show_details()
{
    if (!selected_entry_valid()) return;
    char path[RELIEFOS_FS_PATH_LEN];
    build_child_path(path, sizeof(path), entries[file_list.selected].name);
    struct stat st;
    if (stat(path, &st) < 0) {
        set_status_error(T("Properties failed"), -1);
        return;
    }
    char size[80], contains[128] = {0}, summary[RELIEFOS_FS_PATH_LEN + RELIEFOS_FS_NAME_LEN + 320];
    struct folder_size_info info = {};
    uint64_t bytes = (uint64_t)st.st_size;
    if (S_ISDIR(st.st_mode)) {
        (void)accumulate_folder_size(path, &info, 0);
        bytes = info.bytes;
        format_contains_text(contains, sizeof(contains), &info);
    }
    format_size_text(size, sizeof(size), bytes);
    snprintf(summary, sizeof(summary), "%s: %s\n%s: %s\n%s: %s\n%s: %s\n%s",
             T("Name"), entries[file_list.selected].name,
             T("Type"), entry_type_name(&entries[file_list.selected]),
             T("Path"), path,
             T("Size"), size, contains);

    PropertiesState state;
    state.requested = 0;
    state.window = new Fl_Window(700, 420, T("Properties"));
    state.summary = new Fl_Multiline_Output(16, 14, 668, 190);
    state.summary->value(summary);
    Fl_Box *mode_label = new Fl_Box(16, 216, 150, 26, T("Mode (octal)"));
    mode_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    state.mode = new Fl_Input(168, 216, 148, 28);
    char value[32];
    snprintf(value, sizeof(value), "%04o", (unsigned)(st.st_mode & 07777));
    state.mode->value(value);
    Fl_Box *uid_label = new Fl_Box(328, 216, 62, 26, T("UID"));
    uid_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    state.uid = new Fl_Input(390, 216, 112, 28);
    snprintf(value, sizeof(value), "%lu", (unsigned long)st.st_uid);
    state.uid->value(value);
    Fl_Box *gid_label = new Fl_Box(512, 216, 62, 26, T("GID"));
    gid_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    state.gid = new Fl_Input(574, 216, 110, 28);
    snprintf(value, sizeof(value), "%lu", (unsigned long)st.st_gid);
    state.gid->value(value);
    state.message = new Fl_Output(16, 260, 668, 28);
    state.message->value(T("Permission changes use the current user's access rights."));
    Fl_Return_Button *save = new Fl_Return_Button(488, 362, 92, 34, T("Save"));
    Fl_Button *close = new Fl_Button(592, 362, 92, 34, T("Close"));
    save->callback(properties_save_request, &state);
    close->callback(properties_close_request, &state);
    state.window->callback(properties_close_request, &state);
    state.window->set_modal();
    state.window->end();
    reliefos_fltk::style(state.window);
    state.window->show();
    while (state.window->shown()) {
        Fl::wait();
        if (state.requested != 1) continue;
        state.requested = 0;
        char *end = NULL;
        errno = 0;
        unsigned long mode = strtoul(state.mode->value(), &end, 8);
        int valid = state.mode->value()[0] && *end == 0 && !errno && mode <= 07777;
        errno = 0;
        unsigned long uid = strtoul(state.uid->value(), &end, 10);
        if (!state.uid->value()[0] || *end || errno || uid > UINT32_MAX - 1U) valid = 0;
        errno = 0;
        unsigned long gid = strtoul(state.gid->value(), &end, 10);
        if (!state.gid->value()[0] || *end || errno || gid > UINT32_MAX - 1U) valid = 0;
        if (!valid) {
            state.message->value(T("Invalid mode, UID or GID"));
            continue;
        }
        int result = 0;
        if (st.st_uid != uid || st.st_gid != gid) result = chown(path, (uid_t)uid, (gid_t)gid);
        if (!result) result = chmod(path, (mode_t)mode);
        if (!result) result = stat(path, &st);
        if (result < 0) {
            state.message->value(strerror(errno));
            continue;
        }
        set_status(T("Permissions saved"));
        state.window->hide();
    }
    delete state.window;
}

void open_with_path_ui(const char *path, uint8_t set_default_only)
{
    char program[RELIEFOS_FS_PATH_LEN];
    copy_text(program, sizeof(program), fileman_x11_default_app(path));
    if (!program[0]) copy_text(program, sizeof(program), "/usr/bin/nedit");
    if (!input_dialog(set_default_only ? T("Default Program") : T("Open With"),
            T("X11 executable path (NEdit: /usr/bin/nedit, Dillo: /usr/bin/dillo):"),
            program, sizeof(program))) {
        set_status(T("Open With canceled"));
        return;
    }
    if (program[0] != '/' || !x11_app_allowed(program) || access(program, X_OK) < 0) {
        set_status(T("Choose an installed X11 executable using its absolute path"));
        return;
    }
    if (set_default_only || confirm_dialog(T("Default Program"),
            T("Remember this application for files with the same extension?"), 0)) {
        char extension[16];
        if (!reliefos_launch_get_extension_for_path(path, extension, sizeof(extension)) ||
            reliefos_launch_set_extension_association(extension, program) < 0) {
            set_status(T("Could not save the file association"));
            return;
        }
        if (set_default_only) {
            set_status(T("Default application saved"));
            return;
        }
    }
    int pid = spawn_app(program, path);
    printf("[fileman.elf] open-with path=%s app=%s pid=%d\n", path, program, pid);
    if (pid < 0) set_status_error(T("Open With failed"), pid);
    else set_status(T("Application launched"));
}

void add_menu_action(const char *group, const char *label, uint32_t action, int divider = 0)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", group, label);
    menu_bar->add(path, 0, menu_action, (void *)(uintptr_t)action,
                  divider ? FL_MENU_DIVIDER : 0);
}

void action_menu_callback(Fl_Widget *, void *data)
{
    uint32_t action = (uint32_t)(uintptr_t)data;
    if (action == ACTION_ABOUT) {
        (void)confirm_dialog(T("File Manager"),
            T("Browse files, manage folders and launch X11 applications."), 1);
    } else if (action == FILEMAN_ACTION_ROOT) {
        navigate_root();
    } else {
        execute_action(action);
    }
    refresh_ui();
}

void perform_action(uint32_t action)
{
    if (!action_enabled(action)) return;
    action_menu_callback(NULL, (void *)(uintptr_t)action);
}

void update_status()
{
    if (!status_output) return;
    status_output->value(status_text);
    if (fileman_operation_active) {
        operation_output->value(fileman_operation_text);
        if (progress->value() != (double)fileman_operation_percent)
            progress->value((double)fileman_operation_percent);
        progress->activate();
    } else {
        operation_output->value("");
        if (progress->value() != 0) progress->value(0);
        progress->deactivate();
    }
    if (main_window) {
        char title[RELIEFOS_FS_PATH_LEN + 64];
        snprintf(title, sizeof(title), "%s - %s", T("File Manager"), current_path);
        if (!main_window->label() || strcmp(main_window->label(), title) != 0)
            main_window->copy_label(title);
    }
}

void refresh_tree()
{
    if (!folders) return;
    tree_count = build_tree_items(tree_items, FILEMAN_TREE_MAX_NODES);
    updating = 1;
    folders->clear();
    folders->format_char(0);
    for (uint32_t i = 0; i < tree_count; ++i) {
        char line[RELIEFOS_FS_NAME_LEN + 512];
        const char *marker = tree_items[i].flags & RELIEFOS_UI_TREE_LEAF ? " " :
            (tree_items[i].flags & RELIEFOS_UI_TREE_EXPANDED ? "-" : "+");
        unsigned indent = tree_items[i].depth * 2U;
        if (indent > sizeof(line) - 4U) indent = sizeof(line) - 4U;
        memset(line, ' ', indent);
        snprintf(line + indent, sizeof(line) - indent, "%s %s", marker, tree_items[i].label);
        folders->add(line);
        if (text_eq(current_path, tree_path_for_id(tree_items[i].id)))
            folders->value((int)i + 1);
    }
    updating = 0;
}

void refresh_files()
{
    if (!files) return;
    updating = 1;
    int top = files->topline();
    files->clear();
    files->format_char(0);
    for (uint32_t i = 0; i < entry_count; ++i) {
        char line[RELIEFOS_FS_NAME_LEN + 32];
        snprintf(line, sizeof(line), "%s  %s", entry_type_name(&entries[i]), entries[i].name);
        files->add(line);
        if (fileman_entry_marked(i)) files->select((int)i + 1, 1);
    }
    if (!selected_mask && selected_entry_valid()) {
        files->value(file_list.selected + 1);
    } else if (selected_entry_valid()) {
        files->value(file_list.selected + 1);
    }
    if (top > 0 && (uint32_t)top <= entry_count) files->topline(top);
    updating = 0;
    refresh_tree();
}

void refresh_ui()
{
    if (address) address->value(current_path);
    refresh_files();
    update_status();
    if (menu_bar) menu_bar->redraw();
}

void sync_selection()
{
    if (updating || !files) return;
    int current = files->value();
    file_list.selected = current > 0 && (uint32_t)current <= entry_count ? current - 1 : -1;
    selected_mask = 0;
    for (uint32_t i = 0; i < entry_count && i < 64U; ++i) {
        if (files->selected((int)i + 1)) selected_mask |= 1ULL << i;
    }
    update_status();
}

void files_callback(Fl_Widget *, void *)
{
    sync_selection();
}

void address_callback(Fl_Widget *, void *)
{
    char path[RELIEFOS_FS_PATH_LEN];
    if (!reliefos_fltk::copy_input(path, sizeof(path), address->value())) {
        set_status(T("Path is too long"));
        refresh_ui();
        return;
    }
    if (navigate_to_path(path) < 0) set_status(T("Could not open the requested path"));
    refresh_ui();
    files->take_focus();
}

void tree_callback(Fl_Widget *, void *)
{
    if (updating || !folders) return;
    int row = folders->value();
    if (row < 1 || (uint32_t)row > tree_count) return;
    const char *selected_path = tree_path_for_id(tree_items[row - 1].id);
    char path[RELIEFOS_FS_PATH_LEN];
    if (!reliefos_fltk::copy_input(path, sizeof(path), selected_path)) return;
    if (Fl::event_clicks()) fileman_tree_toggle(tree_items[row - 1].id);
    if (navigate_to_path(path) < 0) set_status(T("Could not open the requested path"));
    refresh_ui();
}

void address_focus()
{
    address->take_focus();
    address->position(0, (int)strlen(address->value()));
    address->redraw();
}

void files_key(int key, int state)
{
    if (key == FL_F + 5) perform_action(FILEMAN_ACTION_REFRESH);
    else if ((state & FL_ALT) && key == FL_Up) perform_action(FILEMAN_ACTION_UP);
    else if ((state & FL_CTRL) && (key == 'l' || key == 'L')) address_focus();
    else if ((state & FL_CTRL) && (key == 'c' || key == 'C')) perform_action(FILEMAN_ACTION_COPY);
    else if ((state & FL_CTRL) && (key == 'x' || key == 'X')) perform_action(FILEMAN_ACTION_CUT);
    else if ((state & FL_CTRL) && (key == 'v' || key == 'V')) perform_action(FILEMAN_ACTION_PASTE);
    else if ((state & FL_CTRL) && (key == 'a' || key == 'A')) perform_action(FILEMAN_ACTION_SELECT_ALL);
    else if (key == FL_F + 2) perform_action(FILEMAN_ACTION_RENAME);
    else if (key == FL_Delete) perform_action(FILEMAN_ACTION_DELETE);
    else if (key == FL_Enter || key == FL_KP_Enter) perform_action(FILEMAN_ACTION_OPEN);
}

class FileBrowser : public Fl_Multi_Browser {
public:
    FileBrowser(int x, int y, int w, int h, const char *label = 0)
        : Fl_Multi_Browser(x, y, w, h, label) {}
    int handle(int event) override
    {
        if (event == FL_KEYDOWN) {
            files_key(Fl::event_key(), Fl::event_state());
            int key = Fl::event_key();
            if (key == FL_F + 5 || key == FL_F + 2 || key == FL_Delete ||
                key == FL_Enter || key == FL_KP_Enter ||
                (Fl::event_state() & FL_CTRL) ||
                ((Fl::event_state() & FL_ALT) && key == FL_Up)) return 1;
        }
        int result = Fl_Multi_Browser::handle(event);
        if (event == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE) {
            sync_selection();
            if (context_menu) {
                show_context_menu();
                const Fl_Menu_Item *picked = context_menu->popup();
                if (picked) perform_action((uint32_t)(uintptr_t)picked->user_data());
            }
            return 1;
        }
        return result;
    }
};

void show_context_menu()
{
    if (!context_menu) return;
    context_menu->clear();
    struct reliefos_ui_context_menu_item items[FILEMAN_CONTEXT_MENU_COUNT];
    build_context_menu_items(items, FILEMAN_CONTEXT_MENU_COUNT);
    for (uint32_t i = 0; i < FILEMAN_CONTEXT_MENU_COUNT; ++i) {
        if (items[i].flags & RELIEFOS_UI_MENU_SEPARATOR) {
            if (i) context_menu->add(" ", 0, NULL, NULL, FL_MENU_DIVIDER | FL_MENU_INACTIVE);
            continue;
        }
        int flags = items[i].flags & RELIEFOS_UI_MENU_DISABLED ? FL_MENU_INACTIVE : 0;
        context_menu->add(items[i].label, 0, NULL,
                          (void *)(uintptr_t)items[i].id, flags);
    }
}

void button_action(Fl_Widget *, void *data)
{
    perform_action((uint32_t)(uintptr_t)data);
}

void menu_action(Fl_Widget *, void *data)
{
    uint32_t action = (uint32_t)(uintptr_t)data;
    if (action == ACTION_ABOUT) {
        (void)confirm_dialog(T("File Manager"),
            T("Browse files, manage folders and launch X11 applications."), 1);
    } else if (action == FILEMAN_ACTION_ROOT) {
        navigate_root();
    } else if (action == FILEMAN_ACTION_SETTINGS) {
        fileman_open_settings();
        show_settings();
    } else {
        if (action_enabled(action)) execute_action(action);
    }
    refresh_ui();
}

void address_callback_wrapper(Fl_Widget *, void *) { address_callback(NULL, NULL); }

void reap_children(void *)
{
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    Fl::repeat_timeout(1.0, reap_children);
}

void build_ui(int argc, char **argv)
{
    (void)argc;
    main_window = new Fl_Double_Window(980, 650, T("File Manager"));
    main_window->size_range(760, 460);
    menu_bar = new Fl_Menu_Bar(0, 0, 980, 30);
    add_menu_action(T("File"), T("Open"), FILEMAN_ACTION_OPEN);
    add_menu_action(T("File"), T("Open With..."), FILEMAN_ACTION_OPEN_WITH);
    add_menu_action(T("File"), T("Default Program..."), FILEMAN_ACTION_DEFAULT_PROGRAM);
    add_menu_action(T("File"), T("Create Shortcut"), FILEMAN_ACTION_CREATE_SHORTCUT);
    add_menu_action(T("File"), T("Details"), FILEMAN_ACTION_DETAILS, 1);
    add_menu_action(T("File"), T("Rename"), FILEMAN_ACTION_RENAME);
    add_menu_action(T("File"), T("New Folder"), FILEMAN_ACTION_NEW_FOLDER);
    add_menu_action(T("File"), T("Delete"), FILEMAN_ACTION_DELETE);
    add_menu_action(T("Edit"), T("Copy"), FILEMAN_ACTION_COPY);
    add_menu_action(T("Edit"), T("Cut"), FILEMAN_ACTION_CUT);
    add_menu_action(T("Edit"), T("Paste"), FILEMAN_ACTION_PASTE);
    add_menu_action(T("Edit"), T("Mark for Batch"), FILEMAN_ACTION_TOGGLE_MARK, 1);
    add_menu_action(T("Edit"), T("Mark All"), FILEMAN_ACTION_SELECT_ALL);
    add_menu_action(T("Edit"), T("Clear Marks"), FILEMAN_ACTION_CLEAR_SELECTION);
    add_menu_action(T("Edit"), T("Delete"), FILEMAN_ACTION_DELETE);
    add_menu_action(T("View"), T("Up"), FILEMAN_ACTION_UP);
    add_menu_action(T("View"), T("Root"), FILEMAN_ACTION_ROOT);
    add_menu_action(T("View"), T("Refresh"), FILEMAN_ACTION_REFRESH);
    add_menu_action(T("View"), T("Settings..."), FILEMAN_ACTION_SETTINGS, 1);
    add_menu_action(T("View"), T("About"), ACTION_ABOUT);
    add_menu_action(T("Archive"), T("Compress to .tar"), FILEMAN_ACTION_COMPRESS_TAR);
    add_menu_action(T("Archive"), T("Extract tar"), FILEMAN_ACTION_EXTRACT_TAR);

    int toolbar_y = 32;
    int x = 8;
    struct ButtonSpec { const char *label; uint32_t action; int width; };
    static const ButtonSpec buttons[] = {
        {"Up", FILEMAN_ACTION_UP, 62}, {"Open", FILEMAN_ACTION_OPEN, 76},
        {"Refresh", FILEMAN_ACTION_REFRESH, 88}, {"New Folder", FILEMAN_ACTION_NEW_FOLDER, 104},
        {"Copy", FILEMAN_ACTION_COPY, 76}, {"Cut", FILEMAN_ACTION_CUT, 68},
        {"Paste", FILEMAN_ACTION_PASTE, 76}, {"Rename", FILEMAN_ACTION_RENAME, 88},
        {"Delete", FILEMAN_ACTION_DELETE, 82},
    };
    for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) {
        Fl_Button *button = new Fl_Button(x, toolbar_y, buttons[i].width, 30, T(buttons[i].label));
        button->callback(button_action, (void *)(uintptr_t)buttons[i].action);
        x += buttons[i].width + 4;
    }
    Fl_Box *address_label = new Fl_Box(8, 69, 62, 30, T("Address"));
    address_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    address = new Fl_Input(70, 69, 900, 30);
    address->when(FL_WHEN_ENTER_KEY);
    address->callback(address_callback_wrapper);

    Fl_Box *tree_heading = new Fl_Box(8, 106, 236, 24, T("Folders (double-click to expand)"));
    tree_heading->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    Fl_Box *files_heading = new Fl_Box(254, 106, 716, 24,
        T("Type / Name (Ctrl or Shift for multiple selection)"));
    files_heading->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    int content_bottom = 586;
    folders = new Fl_Hold_Browser(8, 130, 236, content_bottom - 130);
    folders->format_char(0);
    folders->when(FL_WHEN_CHANGED);
    folders->callback(tree_callback);
    files = new FileBrowser(254, 130, 716, content_bottom - 130);
    files->format_char(0);
    files->when(FL_WHEN_CHANGED);
    files->callback(files_callback);

    context_menu = new Fl_Menu_Button(0, 0, 0, 0);
    context_menu->type(Fl_Menu_Button::POPUP3);

    operation_output = new Fl_Output(8, 592, 430, 24);
    progress = new Fl_Progress(446, 592, 524, 24);
    progress->minimum(0.0);
    progress->maximum(100.0);
    progress->deactivate();
    status_output = new Fl_Output(8, 620, 962, 22);
    status_output->value(T("Ready"));
    main_window->resizable(files);
    main_window->end();
    reliefos_fltk::style(main_window);
    if (argc > 1 && argv[1]) address->value(argv[1]);
}

} // namespace

extern "C" int fileman_input_dialog(const char *title, const char *prompt,
                                      char *value, uint32_t capacity)
{
    return input_dialog(title, prompt, value, capacity);
}

extern "C" int fileman_confirm_dialog(const char *title, const char *message,
                                        uint32_t default_yes)
{
    return confirm_dialog(title, message, default_yes);
}

extern "C" int fileman_launch_path(const char *path)
{
    return launch_path(path, 0);
}

extern "C" void show_open_with_for_path(const char *path, uint8_t set_default_only)
{
    open_with_path_ui(path, set_default_only);
}

extern "C" void show_details_selected(void)
{
    show_details();
}

extern "C" void present_fileman(uint32_t window_id, struct reliefos_ui_surface *ui)
{
    (void)window_id;
    (void)ui;
    update_status();
    // Progress is called inside synchronous C file operations. Dispatching
    // input here could re-enter them and change their selection/path state.
    if (main_window) Fl::flush();
}

int main(int argc, char **argv)
{
    reliefos_fltk::init();
    refresh_home_path();
    fileman_settings_load();
    fileman_tree_reset();
    file_list.selected = -1;
    file_list.visible_rows = 24;
    fileman_window_id = 1;
    const char *initial = argc > 1 ? argv[1] : (home_path[0] ? home_path : "/");
    if (navigate_to_path(initial) < 0) (void)navigate_to_path("/");
    (void)fileman_tree_toggle(1);
    build_ui(argc, argv);
    refresh_ui();
    main_window->show(argc, argv);
    Fl::add_timeout(1.0, reap_children);
    puts("[fileman.elf] FLTK file manager ready");
    fflush(stdout);
    return Fl::run();
}
