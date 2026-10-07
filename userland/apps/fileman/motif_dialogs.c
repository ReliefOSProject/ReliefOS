#include "motif.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/MessageB.h>
#include <Xm/Protocols.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Xm/SelectioB.h>
#include <Xm/Text.h>
#include <Xm/TextF.h>
#include <Xm/ToggleB.h>

static int dialog_answer;

void fileman_motif_answer(Widget widget, XtPointer answer, XtPointer call)
{
    (void)widget; (void)call;
    dialog_answer = (int)(intptr_t)answer;
}

int fileman_motif_wait(Widget dialog)
{
    dialog_answer = 0;
    XtVaSetValues(dialog, XmNdialogStyle, XmDIALOG_FULL_APPLICATION_MODAL,
                  XmNautoUnmanage, False, NULL);
    XtVaSetValues(XtParent(dialog), XmNdeleteResponse, XmDO_NOTHING, NULL);
    Atom close = XInternAtom(XtDisplay(dialog), "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(XtParent(dialog), close, fileman_motif_answer, (XtPointer)-1);
    XtManageChild(dialog);
    while (!dialog_answer && !XtAppGetExitFlag(fileman_app))
        XtAppProcessEvent(fileman_app, XtIMAll);
    XtUnmanageChild(dialog);
    return dialog_answer == 1;
}

int fileman_confirm_dialog(const char *title, const char *message, uint32_t default_yes)
{
    Widget dialog = XmCreateQuestionDialog(fileman_shell, "confirmation", NULL, 0);
    XmString caption = XmStringCreateLocalized((char *)title);
    XmString body = XmStringCreateLocalized((char *)message);
    XtVaSetValues(dialog, XmNdialogTitle, caption, XmNmessageString, body,
        XmNdefaultButtonType, default_yes ? XmDIALOG_OK_BUTTON : XmDIALOG_CANCEL_BUTTON, NULL);
    XmStringFree(caption); XmStringFree(body);
    XtUnmanageChild(XmMessageBoxGetChild(dialog, XmDIALOG_HELP_BUTTON));
    XtAddCallback(dialog, XmNokCallback, fileman_motif_answer, (XtPointer)1);
    XtAddCallback(dialog, XmNcancelCallback, fileman_motif_answer, (XtPointer)-1);
    int accepted = fileman_motif_wait(dialog);
    XtDestroyWidget(XtParent(dialog));
    return accepted;
}

int fileman_input_dialog(const char *title, const char *prompt, char *value, uint32_t capacity)
{
    Widget dialog = XmCreatePromptDialog(fileman_shell, "input", NULL, 0);
    XmString caption = XmStringCreateLocalized((char *)title);
    XmString label = XmStringCreateLocalized((char *)prompt);
    XmString initial = XmStringCreateLocalized(value);
    XtVaSetValues(dialog, XmNdialogTitle, caption, XmNselectionLabelString, label,
                  XmNtextString, initial, NULL);
    XmStringFree(caption); XmStringFree(label); XmStringFree(initial);
    Widget text = XmSelectionBoxGetChild(dialog, XmDIALOG_TEXT);
    XtVaSetValues(text, XmNmaxLength, capacity - 1, NULL);
    XtUnmanageChild(XmSelectionBoxGetChild(dialog, XmDIALOG_HELP_BUTTON));
    XtAddCallback(dialog, XmNokCallback, fileman_motif_answer, (XtPointer)1);
    XtAddCallback(dialog, XmNcancelCallback, fileman_motif_answer, (XtPointer)-1);
    int accepted = fileman_motif_wait(dialog);
    if (accepted) {
        char *result = XmTextFieldGetString(text);
        copy_text(value, capacity, result);
        XtFree(result);
    }
    XtDestroyWidget(XtParent(dialog));
    return accepted;
}

static Widget dialog_form(const char *title)
{
    Widget form = XmCreateFormDialog(fileman_shell, "dialog", NULL, 0);
    XtVaSetValues(form, XmNmarginWidth, 12, XmNmarginHeight, 12, NULL);
    XmString caption = XmStringCreateLocalized((char *)title);
    XtVaSetValues(form, XmNdialogTitle, caption, NULL);
    XmStringFree(caption);
    return form;
}

static Widget dialog_button(Widget parent, const char *label, int answer)
{
    Widget button = XtVaCreateManagedWidget(label, xmPushButtonWidgetClass, parent, NULL);
    fileman_motif_label(button, T(label));
    XtAddCallback(button, XmNactivateCallback, fileman_motif_answer, (XtPointer)(intptr_t)answer);
    return button;
}

void fileman_motif_settings(void)
{
    Widget form = dialog_form(T("File Manager Settings"));
    Widget toggle = XtVaCreateManagedWidget("showHidden", xmToggleButtonWidgetClass, form,
        XmNset, fileman_show_hidden, XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM, NULL);
    fileman_motif_label(toggle, T("Show files and folders starting with a dot"));
    Widget buttons = XtVaCreateManagedWidget("buttons", xmRowColumnWidgetClass, form,
        XmNorientation, XmHORIZONTAL, XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, toggle,
        XmNleftAttachment, XmATTACH_FORM, NULL);
    dialog_button(buttons, "Save", 1);
    dialog_button(buttons, "Cancel", -1);
    if (fileman_motif_wait(form)) {
        fileman_settings_show_hidden = XmToggleButtonGetState(toggle);
        fileman_apply_settings();
    }
    XtDestroyWidget(XtParent(form));
}

static int spawn_app(const char *program, const char *path)
{
    int errors[2];
    if (pipe2(errors, O_CLOEXEC) < 0) return -errno;
    pid_t child = fork();
    if (!child) {
        close(errors[0]);
        char *args[] = {(char *)program, (char *)path, NULL};
        execv(program, args);
        int error = errno;
        (void)write(errors[1], &error, sizeof(error));
        _exit(127);
    }
    int error = errno;
    close(errors[1]);
    if (child < 0) { close(errors[0]); return -error; }
    ssize_t got;
    do { got = read(errors[0], &error, sizeof(error)); } while (got < 0 && errno == EINTR);
    close(errors[0]);
    if (got != 0) {
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
        return got == sizeof(error) ? -error : -EIO;
    }
    return (int)child;
}

static int x11_app_allowed(const char *program)
{
    const char *apps = RELIEFOS_LAYOUT_RELIEFOS_APPS "/";
    if (strncmp(program, apps, strlen(apps))) return 1;
    return !strcmp(program, RELIEFOS_LAYOUT_RELIEFOS_APPS "/calc/calc.elf") ||
           !strcmp(program, RELIEFOS_LAYOUT_RELIEFOS_APPS "/osver/osver.elf") ||
           !strcmp(program, RELIEFOS_LAYOUT_RELIEFOS_APPS "/fileman/fileman.elf");
}

static int launch_path(const char *path, unsigned depth)
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
            if (strlen(line + 7) >= sizeof(target)) { fclose(file); return -ENAMETOOLONG; }
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

int fileman_launch_path(const char *path)
{
    return launch_path(path, 0);
}

void show_open_with_for_path(const char *path, uint8_t set_default_only)
{
    char program[RELIEFOS_FS_PATH_LEN];
    copy_text(program, sizeof(program), fileman_x11_default_app(path));
    if (!program[0]) copy_text(program, sizeof(program), "/usr/bin/nedit");
    if (!fileman_input_dialog(set_default_only ? T("Default Program") : T("Open With"),
            T("X11 executable path (NEdit: /usr/bin/nedit, Dillo: /usr/bin/dillo):"),
            program, sizeof(program))) { set_status(T("Open With canceled")); return; }
    if (program[0] != '/' || !x11_app_allowed(program) || access(program, X_OK) < 0) {
        set_status(T("Choose an installed X11 executable using its absolute path"));
        return;
    }
    if (set_default_only || fileman_confirm_dialog(T("Default Program"),
            T("Remember this application for files with the same extension?"), 0)) {
        char extension[16];
        if (!reliefos_launch_get_extension_for_path(path, extension, sizeof(extension)) ||
            reliefos_launch_set_extension_association(extension, program) < 0) {
            set_status(T("Could not save the file association"));
            return;
        }
        if (set_default_only) { set_status(T("Default application saved")); return; }
    }
    int pid = spawn_app(program, path);
    printf("[fileman.elf] open-with path=%s app=%s pid=%d\n", path, program, pid);
    if (pid < 0) set_status_error(T("Open With failed"), pid);
    else set_status(T("Application launched"));
}

static Widget property_field(Widget parent, const char *name, const char *value)
{
    Widget label = XtVaCreateManagedWidget(name, xmLabelWidgetClass, parent, NULL);
    fileman_motif_label(label, T(name));
    return XtVaCreateManagedWidget(name, xmTextFieldWidgetClass, parent,
        XmNvalue, value, XmNcolumns, 10, XmNmaxLength, 16, NULL);
}

void show_details_selected(void)
{
    if (!selected_entry_valid()) return;
    char path[RELIEFOS_FS_PATH_LEN];
    build_child_path(path, sizeof(path), entries[file_list.selected].name);
    struct stat st;
    if (stat(path, &st) < 0) { set_status_error(T("Properties failed"), -1); return; }
    char size[80], contains[96] = {0}, summary[768];
    struct folder_size_info info = {0};
    uint64_t bytes = (uint64_t)st.st_size;
    if (S_ISDIR(st.st_mode)) {
        accumulate_folder_size(path, &info, 0);
        bytes = info.bytes;
        format_contains_text(contains, sizeof(contains), &info);
    }
    format_size_text(size, sizeof(size), bytes);
    snprintf(summary, sizeof(summary), "%s: %s\n%s: %s\n%s: %s\n%s: %s\n%s",
        T("Name"), entries[file_list.selected].name, T("Type"), entry_type_name(&entries[file_list.selected]),
        T("Path"), path, T("Size"), size, contains);
    Widget form = dialog_form(T("Properties"));
    Widget general = XtVaCreateManagedWidget("general", xmTextWidgetClass, form,
        XmNvalue, summary, XmNeditable, False, XmNcursorPositionVisible, False,
        XmNeditMode, XmMULTI_LINE_EDIT, XmNwordWrap, True, XmNcolumns, 70, XmNrows, 7,
        XmNtopAttachment, XmATTACH_FORM, XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM, NULL);
    Widget fields = XtVaCreateManagedWidget("permissions", xmRowColumnWidgetClass, form,
        XmNorientation, XmHORIZONTAL, XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, general,
        XmNleftAttachment, XmATTACH_FORM, NULL);
    char value[32];
    snprintf(value, sizeof(value), "%04o", (unsigned)(st.st_mode & 07777));
    Widget mode = property_field(fields, "Mode (octal)", value);
    snprintf(value, sizeof(value), "%lu", (unsigned long)st.st_uid);
    Widget uid = property_field(fields, "UID", value);
    snprintf(value, sizeof(value), "%lu", (unsigned long)st.st_gid);
    Widget gid = property_field(fields, "GID", value);
    Widget message = XtVaCreateManagedWidget("message", xmLabelWidgetClass, form,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, fields,
        XmNleftAttachment, XmATTACH_FORM, NULL);
    fileman_motif_label(message, T("Permission changes use the current user's access rights."));
    Widget buttons = XtVaCreateManagedWidget("buttons", xmRowColumnWidgetClass, form,
        XmNorientation, XmHORIZONTAL, XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, message,
        XmNleftAttachment, XmATTACH_FORM, NULL);
    dialog_button(buttons, "Save", 1);
    dialog_button(buttons, "Close", -1);
    while (fileman_motif_wait(form)) {
        Widget widgets[] = {mode, uid, gid};
        unsigned long values[3];
        int valid = 1;
        for (unsigned i = 0; i < 3; ++i) {
            char *text = XmTextFieldGetString(widgets[i]), *end;
            errno = 0;
            values[i] = strtoul(text, &end, i ? 10 : 8);
            if (!*text || *text == '-' || *end || errno || values[i] > (i ? UINT32_MAX - 1 : 07777)) valid = 0;
            XtFree(text);
        }
        if (!valid) { fileman_motif_label(message, T("Invalid mode, UID or GID")); continue; }
        int result = 0;
        if (st.st_uid != values[1] || st.st_gid != values[2]) result = chown(path, values[1], values[2]);
        if (!result) result = chmod(path, values[0]);
        if (!result) result = stat(path, &st);
        if (result < 0) fileman_motif_label(message, strerror(errno));
        else { set_status(T("Permissions saved")); break; }
    }
    XtDestroyWidget(XtParent(form));
}
