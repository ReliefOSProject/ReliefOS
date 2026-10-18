#include "fileman.h"
#include "search.h"

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

enum { ACTION_ABOUT = 1000, ACTION_SEARCH };

Fl_Window *main_window;
Fl_Menu_Bar *menu_bar;
Fl_Input *address;
Fl_Hold_Browser *folders;
Fl_Multi_Browser *files;
Fl_Output *status_output;
Fl_Output *operation_output;
Fl_Progress *progress;
Fl_Menu_Button *context_menu;
Fl_Box *address_label;
Fl_Box *folders_heading;
Fl_Box *name_heading;
Fl_Box *type_heading;
Fl_Output *item_summary;
char selected_path_tip[RELIEFOS_FS_PATH_LEN];
Fl_Button *up_button;
Fl_Button *root_button;
Fl_Button *search_button;
Fl_Button *refresh_button;
Fl_Box *action_separators[2];
int file_columns[2];
struct ToolbarAction { Fl_Button *button; uint32_t action; };
ToolbarAction toolbar_actions[8];
unsigned toolbar_action_count;
struct reliefos_ui_tree_item tree_items[FILEMAN_TREE_MAX_NODES];
uint32_t tree_count;
int updating;

void refresh_ui();
void perform_action(uint32_t action);
void show_context_menu();
void menu_action(Fl_Widget *, void *data);
void show_search();
void layout_main();

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
        RELIEFOS_LAYOUT_RELIEFOS_APPS "/reliefexcel/reliefexcel.elf",
        RELIEFOS_LAYOUT_RELIEFOS_APPS "/reliefword/reliefword.elf",
        RELIEFOS_LAYOUT_RELIEFOS_APPS "/reliefppt/reliefppt.elf",
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

struct SearchState {
    FileSearch search;
    Fl_Window *window;
    Fl_Input *root;
    Fl_Input *query;
    Fl_Check_Button *recursive;
    Fl_Check_Button *ignore_case;
    Fl_Check_Button *hidden;
    Fl_Button *start;
    Fl_Button *stop;
    Fl_Button *open;
    Fl_Button *locate;
    Fl_Hold_Browser *results;
    Fl_Output *selected;
    Fl_Output *status;
    unsigned rendered;
    int action;
    char chosen[RELIEFOS_FS_PATH_LEN];
};

void search_tick(void *data);

void search_selection(Fl_Widget *, void *data)
{
    SearchState *state = static_cast<SearchState *>(data);
    int row = state->results->value();
    const FileSearchResult *result = row > 0 ? state->search.result((unsigned)row - 1) : NULL;
    state->selected->value(result ? result->path : "");
    if (result) {
        state->open->activate();
        state->locate->activate();
    } else {
        state->open->deactivate();
        state->locate->deactivate();
    }
}

void search_status(SearchState *state)
{
    const char *phase = state->search.running() ? T("Searching") :
        state->search.canceled() ? T("Search stopped") :
        state->search.limited() ? T("Search stopped at limit") :
        state->search.skipped() ? T("Search complete (partial)") :
        state->search.result_count() ? T("Search complete") : T("No matches found");
    char message[320];
    snprintf(message, sizeof(message), T("%s — %lu checked, %u matches, %lu skipped"),
             phase, state->search.scanned(), state->search.result_count(), state->search.skipped());
    state->status->value(message);
    Fl_Widget *controls[] = {state->root, state->query, state->recursive,
                             state->ignore_case, state->hidden, state->start};
    for (unsigned i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i) {
        if (state->search.running()) controls[i]->deactivate();
        else controls[i]->activate();
    }
    if (state->search.running()) state->stop->activate();
    else state->stop->deactivate();
}

void search_stop(Fl_Widget *, void *data)
{
    SearchState *state = static_cast<SearchState *>(data);
    Fl::remove_timeout(search_tick, state);
    state->search.cancel();
    search_status(state);
}

void search_close(Fl_Widget *, void *data)
{
    SearchState *state = static_cast<SearchState *>(data);
    Fl::remove_timeout(search_tick, state);
    state->search.cancel();
    state->window->hide();
}

void search_choose(SearchState *state, int action)
{
    int row = state->results->value();
    const FileSearchResult *result = row > 0 ? state->search.result((unsigned)row - 1) : NULL;
    if (!result || !reliefos_fltk::copy_input(state->chosen, sizeof(state->chosen), result->path)) return;
    state->action = action;
    search_close(NULL, state);
}

void search_open(Fl_Widget *, void *data) { search_choose(static_cast<SearchState *>(data), 1); }
void search_locate(Fl_Widget *, void *data) { search_choose(static_cast<SearchState *>(data), 2); }

class SearchBrowser : public Fl_Hold_Browser {
    SearchState *state_;
public:
    SearchBrowser(int x, int y, int w, int h, SearchState *state)
        : Fl_Hold_Browser(x, y, w, h), state_(state) {}
    int handle(int event) override
    {
        if (event == FL_KEYDOWN && (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter)) {
            search_choose(state_, 1);
            return 1;
        }
        int handled = Fl_Hold_Browser::handle(event);
        if (event == FL_RELEASE && Fl::event_button() == FL_LEFT_MOUSE && Fl::event_clicks())
            search_choose(state_, 1);
        return handled;
    }
};

void search_tick(void *data)
{
    SearchState *state = static_cast<SearchState *>(data);
    state->search.step(32);
    while (state->rendered < state->search.result_count()) {
        const FileSearchResult *result = state->search.result(state->rendered++);
        /* Escape control bytes only for display. The selected/opened path
         * remains byte-for-byte intact, including UTF-8 and literal '@'. */
        char line[RELIEFOS_FS_PATH_LEN * 4 + 1];
        unsigned at = 0;
        for (const unsigned char *p = (const unsigned char *)result->path; *p; ++p) {
            if (*p < 32 || *p == 127) {
                snprintf(line + at, sizeof(line) - at, "\\x%02x", (unsigned)*p);
                at += 4;
            } else line[at++] = (char)*p;
        }
        line[at] = 0;
        state->results->add(line);
    }
    search_status(state);
    if (state->search.running()) Fl::repeat_timeout(.01, search_tick, state);
}

void search_start(Fl_Widget *, void *data)
{
    SearchState *state = static_cast<SearchState *>(data);
    if (!state->query->value()[0]) {
        fl_alert("%s", T("Enter part of a file or folder name."));
        state->query->take_focus();
        return;
    }
    FileSearchOptions options;
    options.recursive = state->recursive->value() != 0;
    options.show_hidden = state->hidden->value() != 0;
    options.ignore_ascii_case = state->ignore_case->value() != 0;
    Fl::remove_timeout(search_tick, state);
    int error = state->search.start(state->root->value(), state->query->value(), options);
    state->results->clear();
    state->rendered = 0;
    search_selection(NULL, state);
    search_status(state);
    if (error < 0) {
        char message[320];
        snprintf(message, sizeof(message), "%s: %s", T("Could not start search"), strerror(-error));
        state->status->value(message);
        return;
    }
    Fl::add_timeout(.01, search_tick, state);
}

void show_search()
{
    SearchState state{};
    state.window = new Fl_Double_Window(main_window->x() + 40, main_window->y() + 45,
                                         900, 560, T("Search Files"));
    state.window->set_modal();
    state.window->callback(search_close, &state);
    Fl_Box *root_label = new Fl_Box(16, 14, 98, 30, T("Search in:"));
    root_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    state.root = new Fl_Input(114, 14, 770, 30);
    state.root->value(current_path);
    Fl_Box *query_label = new Fl_Box(16, 54, 98, 30, T("File name:"));
    query_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    state.query = new Fl_Input(114, 54, 646, 30);
    state.query->when(FL_WHEN_ENTER_KEY_ALWAYS);
    state.query->callback(search_start, &state);
    state.start = new Fl_Button(772, 54, 112, 30, T("Search"));
    state.start->callback(search_start, &state);
    state.recursive = new Fl_Check_Button(16, 94, 205, 28, T("Include subfolders"));
    state.recursive->value(1);
    state.ignore_case = new Fl_Check_Button(230, 94, 280, 28, T("Ignore English letter case"));
    state.ignore_case->value(1);
    state.hidden = new Fl_Check_Button(525, 94, 230, 28, T("Show hidden files"));
    state.hidden->value(fileman_show_hidden != 0);
    Fl_Box *hint = new Fl_Box(16, 125, 868, 24, T("Matches part of a file or folder name."));
    hint->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    state.results = new SearchBrowser(16, 154, 868, 300, &state);
    state.results->format_char(0);
    state.results->when(FL_WHEN_CHANGED);
    state.results->callback(search_selection, &state);
    state.selected = new Fl_Output(16, 462, 868, 28);
    state.status = new Fl_Output(16, 496, 868, 26);
    state.status->value(T("Enter part of a file or folder name."));
    state.open = new Fl_Button(16, 526, 110, 30, T("Open"));
    state.open->deactivate();
    state.open->callback(search_open, &state);
    state.locate = new Fl_Button(138, 526, 240, 30, T("Open Containing Folder"));
    state.locate->deactivate();
    state.locate->callback(search_locate, &state);
    state.stop = new Fl_Button(628, 526, 112, 30, T("Stop"));
    state.stop->deactivate();
    state.stop->callback(search_stop, &state);
    Fl_Button *close = new Fl_Button(752, 526, 132, 30, T("Close"));
    close->callback(search_close, &state);
    state.window->end();
    reliefos_fltk::style(state.window);
    state.window->show();
    state.query->take_focus();
    while (state.window->shown()) Fl::wait();
    Fl::remove_timeout(search_tick, &state);
    state.search.cancel();
    delete state.window;
    files->take_focus();
    int located_row = 0;
    if (state.action == 1) {
        int result = fileman_launch_path(state.chosen);
        if (result < 0) set_status_error(T("Could not open the search result"), result);
    } else if (state.action == 2) {
        char *base = strrchr(state.chosen, '/');
        if (base) {
            char name[RELIEFOS_FS_PATH_LEN];
            reliefos_fltk::copy_input(name, sizeof(name), base + 1);
            if (base == state.chosen) base[1] = 0; else *base = 0;
            if (navigate_to_path(state.chosen) == 0) {
                for (uint32_t i = 0; i < entry_count; ++i) {
                    if (!strcmp(entries[i].name, name)) {
                        file_list.selected = (int)i;
                        located_row = (int)i + 1;
                        break;
                    }
                }
            }
        }
    }
    refresh_ui();
    if (located_row) {
        files->middleline(located_row);
    }
}

void add_menu_action(const char *group, const char *label, uint32_t action, int divider = 0,
                     int shortcut = 0)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", group, label);
    menu_bar->add(path, shortcut, menu_action, (void *)(uintptr_t)action,
                  divider ? FL_MENU_DIVIDER : 0);
}

void action_menu_callback(Fl_Widget *, void *data)
{
    uint32_t action = (uint32_t)(uintptr_t)data;
    if (action == ACTION_ABOUT) {
        (void)confirm_dialog(T("File Manager"),
            T("Browse files, manage folders and launch X11 applications."), 1);
    } else if (action == ACTION_SEARCH) {
        show_search();
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
    const char *directory_prefix = T("Items ");
    size_t prefix_length = strlen(directory_prefix);
    bool directory_status = !strncmp(status_text, directory_prefix, prefix_length) &&
                            status_text[prefix_length] >= '0' &&
                            status_text[prefix_length] <= '9';
    status_output->value(directory_status ? T("Ready") : status_text);
    for (unsigned i = 0; i < toolbar_action_count; ++i) {
        if (action_enabled(toolbar_actions[i].action)) toolbar_actions[i].button->activate();
        else toolbar_actions[i].button->deactivate();
    }
    selected_path_tip[0] = 0;
    if (selected_entry_valid() &&
        fileman_join_path(selected_path_tip, sizeof(selected_path_tip), current_path,
                          entries[file_list.selected].name) < 0)
        selected_path_tip[0] = 0;
    status_output->tooltip(selected_path_tip[0] ? selected_path_tip : NULL);
    if (item_summary) {
        unsigned selected = fileman_selected_count();
        if (!selected && selected_entry_valid()) selected = 1;
        char summary[160];
        snprintf(summary, sizeof(summary), T("%u items, %u selected"), entry_count, selected);
        item_summary->value(summary);
    }
    if (fileman_operation_active) {
        status_output->hide();
        item_summary->hide();
        operation_output->show();
        progress->show();
        operation_output->value(fileman_operation_text);
        if (progress->value() != (double)fileman_operation_percent)
            progress->value((double)fileman_operation_percent);
        progress->activate();
    } else {
        operation_output->value("");
        if (progress->value() != 0) progress->value(0);
        progress->deactivate();
        operation_output->hide();
        progress->hide();
        status_output->show();
        item_summary->show();
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
        // Columns and labels are display-only: keep the original filename
        // bytes in entries for every open, rename and batch operation.
        char display_name[RELIEFOS_FS_NAME_LEN * 4 + 1];
        unsigned at = 0;
        for (const unsigned char *p = (const unsigned char *)entries[i].name; *p; ++p) {
            if (*p < 32 || *p == 127) {
                snprintf(display_name + at, sizeof(display_name) - at, "\\x%02x", (unsigned)*p);
                at += 4;
            } else display_name[at++] = (char)*p;
        }
        display_name[at] = 0;
        const char *kind = entries[i].type == RELIEFOS_FS_TYPE_DIR ? T("Folder") :
            fileman_entry_is_device(i) ? T("Device") :
            ends_with(entries[i].name, ".elf") ? T("Application") :
            ends_with(entries[i].name, ".lnk") ? T("Shortcut") : T("File");
        char line[sizeof(display_name) + 128];
        snprintf(line, sizeof(line), "%s\t%s", display_name, kind);
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
    if (Fl::event() == FL_RELEASE && Fl::event_button() == FL_LEFT_MOUSE &&
        Fl::event_clicks()) fileman_tree_toggle(tree_items[row - 1].id);
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

bool main_shortcut(int event)
{
    if (event != FL_KEYDOWN && event != FL_SHORTCUT) return false;
    int key = Fl::event_key(), modifiers = Fl::event_state();
    if ((modifiers & FL_CTRL) && (key == 'f' || key == 'F')) show_search();
    else if ((modifiers & FL_CTRL) && (key == 'l' || key == 'L')) address_focus();
    else if (key == FL_F + 5) perform_action(FILEMAN_ACTION_REFRESH);
    else if ((modifiers & FL_ALT) && key == FL_Up) perform_action(FILEMAN_ACTION_UP);
    else return false;
    return true;
}

template <typename Base>
class SearchShortcutWidget : public Base {
public:
    SearchShortcutWidget(int x, int y, int w, int h, const char *label = 0)
        : Base(x, y, w, h, label) {}
    int handle(int event) override
    {
        if (main_shortcut(event)) return 1;
        return Base::handle(event);
    }
};

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
        if (event == FL_RELEASE && Fl::event_button() == FL_LEFT_MOUSE && Fl::event_clicks() &&
            !(Fl::event_state() & (FL_CTRL | FL_SHIFT))) {
            sync_selection();
            perform_action(FILEMAN_ACTION_OPEN);
            return 1;
        }
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
    } else if (action == ACTION_SEARCH) {
        show_search();
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

class FilemanWindow : public Fl_Window {
public:
    FilemanWindow(int w, int h, const char *label) : Fl_Window(w, h, label) {}
    void resize(int x, int y, int w, int h) override
    {
        Fl_Window::resize(x, y, w, h);
        if (files) layout_main();
    }
    int handle(int event) override
    {
        if (main_shortcut(event)) return 1;
        return Fl_Window::handle(event);
    }
};

void layout_main()
{
    if (!main_window || !item_summary) return;
    int w = main_window->w(), h = main_window->h();
    menu_bar->resize(0, 0, w, 28);
    address->resize(204, 36, w - 416, 30);
    search_button->resize(w - 204, 36, 92, 30);
    refresh_button->resize(w - 104, 36, 92, 30);
    int list_width = w - 224;
    int list_height = h - 182;
    folders->resize(12, 136, 190, list_height);
    files->resize(212, 136, list_width, list_height);
    // Reserve a native column for the type, while giving filenames all
    // remaining width. Column storage must outlive the FLTK browser.
    file_columns[0] = list_width - 120;
    file_columns[1] = 0;
    name_heading->resize(212, 112, file_columns[0], 24);
    type_heading->resize(212 + file_columns[0], 112, 120, 24);
    operation_output->resize(12, h - 34, w / 2 - 20, 24);
    progress->resize(w / 2, h - 34, w / 2 - 12, 24);
    status_output->resize(12, h - 34, w - 284, 24);
    item_summary->resize(w - 264, h - 34, 252, 24);
    files->redraw();
    main_window->redraw();
}

void build_ui(int argc, char **argv)
{
    (void)argc;
    main_window = new FilemanWindow(960, 600, T("File Manager"));
    main_window->size_range(800, 480);
    menu_bar = new Fl_Menu_Bar(0, 0, 960, 28);
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
    add_menu_action(T("Edit"), T("Search Files..."), ACTION_SEARCH, 0, FL_CTRL + 'f');
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

    up_button = new Fl_Button(12, 36, 60, 30, T("Up"));
    up_button->callback(button_action, (void *)(uintptr_t)FILEMAN_ACTION_UP);
    up_button->tooltip(T("Up one folder (Alt+Up)"));
    root_button = new Fl_Button(80, 36, 64, 30, T("Root"));
    root_button->callback(button_action, (void *)(uintptr_t)FILEMAN_ACTION_ROOT);
    address_label = new Fl_Box(156, 36, 48, 30, T("Address"));
    address_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    address = new SearchShortcutWidget<Fl_Input>(204, 36, 544, 30);
    address->when(FL_WHEN_ENTER_KEY);
    address->callback(address_callback_wrapper);
    address->tooltip(T("Enter a folder path, then press Enter (Ctrl+L)"));
    search_button = new Fl_Button(756, 36, 92, 30, T("Search"));
    search_button->callback(button_action, (void *)(uintptr_t)ACTION_SEARCH);
    search_button->tooltip(T("Search this folder and subfolders (Ctrl+F)"));
    refresh_button = new Fl_Button(856, 36, 92, 30, T("Refresh"));
    refresh_button->callback(button_action, (void *)(uintptr_t)FILEMAN_ACTION_REFRESH);
    refresh_button->tooltip(T("Refresh (F5)"));

    struct ButtonSpec { const char *label; uint32_t action; int x, width; const char *tip; };
    const ButtonSpec buttons[] = {
        {"New Folder", FILEMAN_ACTION_NEW_FOLDER, 12, 108, T("Create a folder in the current location")},
        {"Open", FILEMAN_ACTION_OPEN, 128, 70, T("Open the selected item (Enter)")},
        {"Details", FILEMAN_ACTION_DETAILS, 206, 88, T("View properties and permissions")},
        {"Copy", FILEMAN_ACTION_COPY, 314, 66, T("Copy selected items (Ctrl+C)")},
        {"Cut", FILEMAN_ACTION_CUT, 388, 60, T("Move selected items using Cut and Paste (Ctrl+X)")},
        {"Paste", FILEMAN_ACTION_PASTE, 456, 66, T("Paste into the current folder (Ctrl+V)")},
        {"Rename", FILEMAN_ACTION_RENAME, 542, 82, T("Rename the selected item (F2)")},
        {"Delete", FILEMAN_ACTION_DELETE, 632, 82, T("Delete selected items after confirmation")},
    };
    toolbar_action_count = 0;
    for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) {
        Fl_Button *button = new Fl_Button(buttons[i].x, 74, buttons[i].width, 28,
                                          T(buttons[i].label));
        button->callback(button_action, (void *)(uintptr_t)buttons[i].action);
        button->tooltip(buttons[i].tip);
        toolbar_actions[toolbar_action_count++] = {button, buttons[i].action};
    }
    action_separators[0] = new Fl_Box(FL_ENGRAVED_BOX, 302, 78, 2, 20, NULL);
    action_separators[1] = new Fl_Box(FL_ENGRAVED_BOX, 530, 78, 2, 20, NULL);

    folders_heading = new Fl_Box(FL_THIN_UP_BOX, 12, 112, 190, 24, T("Folders"));
    name_heading = new Fl_Box(FL_THIN_UP_BOX, 212, 112, 616, 24, T("Name"));
    type_heading = new Fl_Box(FL_THIN_UP_BOX, 828, 112, 120, 24, T("Type"));
    Fl_Box *column_headings[] = {folders_heading, name_heading, type_heading};
    for (unsigned i = 0; i < 3; ++i) {
        column_headings[i]->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        column_headings[i]->labelfont(FL_HELVETICA_BOLD);
    }
    folders = new SearchShortcutWidget<Fl_Hold_Browser>(12, 136, 190, 418);
    folders->format_char(0);
    folders->textsize(15);
    folders->when(FL_WHEN_CHANGED | FL_WHEN_ENTER_KEY);
    folders->callback(tree_callback);
    folders->tooltip(T("Double-click a folder to expand or collapse its tree"));
    files = new SearchShortcutWidget<FileBrowser>(212, 136, 736, 418);
    files->format_char(0);
    files->column_char('\t');
    files->column_widths(file_columns);
    files->textsize(15);
    files->when(FL_WHEN_CHANGED);
    files->callback(files_callback);
    files->tooltip(T("Double-click to open. Ctrl/Shift selects multiple items."));

    context_menu = new Fl_Menu_Button(0, 0, 0, 0);
    context_menu->type(Fl_Menu_Button::POPUP3);

    operation_output = new SearchShortcutWidget<Fl_Output>(12, 566, 460, 24);
    progress = new Fl_Progress(480, 566, 468, 24);
    progress->minimum(0.0);
    progress->maximum(100.0);
    progress->deactivate();
    operation_output->hide();
    progress->hide();
    status_output = new SearchShortcutWidget<Fl_Output>(12, 566, 676, 24);
    status_output->value(T("Ready"));
    item_summary = new SearchShortcutWidget<Fl_Output>(696, 566, 252, 24);
    main_window->resizable(NULL);
    main_window->end();
    reliefos_fltk::style(main_window);
    layout_main();
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
    reliefos_fltk::init("fileman");
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
