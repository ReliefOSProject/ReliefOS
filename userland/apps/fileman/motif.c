#include "motif.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <X11/keysym.h>
#include <Xm/CascadeB.h>
#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/List.h>
#include <Xm/MessageB.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Xm/Scale.h>
#include <Xm/Separator.h>
#include <Xm/TextF.h>

static Widget address, files, folders, status, progress, popup;
static struct reliefos_ui_tree_item tree_items[FILEMAN_TREE_MAX_NODES];
static uint32_t tree_count;
static int updating;
static struct { Widget widget; uint32_t action; } controls[96];
static unsigned control_count;

void fileman_motif_label(Widget widget, const char *text)
{
    XmString label = XmStringCreateLocalized((char *)text);
    XtVaSetValues(widget, XmNlabelString, label, NULL);
    XmStringFree(label);
}

void fileman_motif_status(void)
{
    if (!status) return;
    fileman_motif_label(status, fileman_operation_active ? fileman_operation_text : status_text);
    XmScaleSetValue(progress, (int)fileman_operation_percent);
    XtSetSensitive(progress, fileman_operation_active);
    XmUpdateDisplay(fileman_shell);
}

void present_fileman(uint32_t window_id, struct reliefos_ui_surface *ui)
{
    (void)window_id; (void)ui;
    fileman_motif_status();
}

static int action_enabled(uint32_t action)
{
    switch (action) {
    case FILEMAN_ACTION_OPEN: case FILEMAN_ACTION_DETAILS:
    case FILEMAN_ACTION_TOGGLE_MARK: return selected_entry_valid();
    case FILEMAN_ACTION_OPEN_WITH: case FILEMAN_ACTION_DEFAULT_PROGRAM:
    case FILEMAN_ACTION_CREATE_SHORTCUT: return selected_entry_is_file();
    case FILEMAN_ACTION_RENAME: return selected_entry_is_mutable() && fileman_selected_count() <= 1;
    case FILEMAN_ACTION_COPY: case FILEMAN_ACTION_CUT:
    case FILEMAN_ACTION_DELETE: return selected_entry_is_mutable();
    case FILEMAN_ACTION_PASTE: return fileman_clipboard_available();
    case FILEMAN_ACTION_SELECT_ALL: return entry_count != 0;
    case FILEMAN_ACTION_CLEAR_SELECTION: return fileman_selected_count() != 0;
    case FILEMAN_ACTION_EXTRACT_TAR:
        return selected_entry_is_file() && ends_with(entries[file_list.selected].name, ".tar");
    case FILEMAN_ACTION_COMPRESS_TAR: return selected_entry_is_mutable();
    default: return 1;
    }
}

static void update_controls(void)
{
    for (unsigned i = 0; i < control_count; ++i)
        XtSetSensitive(controls[i].widget, action_enabled(controls[i].action));
}

static void refresh_tree(void)
{
    tree_count = build_tree_items(tree_items, FILEMAN_TREE_MAX_NODES);
    XmListDeleteAllItems(folders);
    for (uint32_t i = 0; i < tree_count; ++i) {
        char line[256];
        const char *marker = tree_items[i].flags & RELIEFOS_UI_TREE_LEAF ? " " :
            (tree_items[i].flags & RELIEFOS_UI_TREE_EXPANDED ? "-" : "+");
        snprintf(line, sizeof(line), "%*s%s %s", (int)(tree_items[i].depth * 2), "",
                 marker, tree_items[i].label);
        XmString label = XmStringCreateLocalized(line);
        XmListAddItemUnselected(folders, label, 0);
        XmStringFree(label);
        if (text_eq(current_path, tree_path_for_id(tree_items[i].id)))
            XmListSelectPos(folders, (int)i + 1, False);
    }
}

void fileman_motif_refresh(void)
{
    if (!files) return;
    updating = 1;
    int top = 1;
    XtVaGetValues(files, XmNtopItemPosition, &top, NULL);
    XmListDeleteAllItems(files);
    for (uint32_t i = 0; i < entry_count; ++i) {
        char line[192];
        snprintf(line, sizeof(line), "%-4s  %s", entry_type_name(&entries[i]), entries[i].name);
        XmString label = XmStringCreateLocalized(line);
        XmListAddItemUnselected(files, label, 0);
        XmStringFree(label);
        if (fileman_entry_marked(i)) XmListSelectPos(files, (int)i + 1, False);
    }
    if (!selected_mask && selected_entry_valid())
        XmListSelectPos(files, file_list.selected + 1, False);
    if (top <= (int)entry_count) XmListSetPos(files, top);
    refresh_tree();
    XmTextFieldSetString(address, current_path);
    char title[320];
    snprintf(title, sizeof(title), "%s - %s", T("File Manager"), current_path);
    XtVaSetValues(fileman_shell, XtNtitle, title, NULL);
    update_controls();
    fileman_motif_status();
    updating = 0;
}

void fileman_motif_action(uint32_t action)
{
    if (!action_enabled(action)) return;
    if (action == FILEMAN_ACTION_ROOT) navigate_root();
    else if (action == FILEMAN_ACTION_SETTINGS) fileman_motif_settings();
    else if (action == FILEMAN_ACTION_ABOUT) {
        fileman_confirm_dialog(T("File Manager"), T("Browse files, manage folders and launch X11 applications."), 1);
    } else execute_action(action);
    fileman_motif_refresh();
    printf("[fileman.elf] action=%u status=%s\n", action, status_text);
    fflush(stdout);
}

static void action_callback(Widget widget, XtPointer action, XtPointer call)
{
    (void)widget; (void)call;
    fileman_motif_action((uint32_t)(uintptr_t)action);
}

static Widget action_button(Widget parent, const char *label, uint32_t action)
{
    Widget button = XtVaCreateManagedWidget(label, xmPushButtonWidgetClass, parent, NULL);
    fileman_motif_label(button, T(label));
    XtAddCallback(button, XmNactivateCallback, action_callback, (XtPointer)(uintptr_t)action);
    if (control_count < sizeof(controls) / sizeof(controls[0])) {
        controls[control_count].widget = button;
        controls[control_count++].action = action;
    }
    return button;
}

static Widget menu(Widget bar, const char *name)
{
    Widget pane = XmCreatePulldownMenu(bar, (char *)name, NULL, 0);
    Widget cascade = XtVaCreateManagedWidget(name, xmCascadeButtonWidgetClass, bar,
                                             XmNsubMenuId, pane, NULL);
    fileman_motif_label(cascade, T(name));
    return pane;
}

static void menu_items(Widget pane, struct reliefos_ui_context_menu_item *items, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        if (items[i].flags & RELIEFOS_UI_MENU_SEPARATOR)
            XtVaCreateManagedWidget("separator", xmSeparatorWidgetClass, pane, NULL);
        else action_button(pane, items[i].label, items[i].id);
    }
}

static void select_files(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget; (void)data;
    if (updating) return;
    XmListCallbackStruct *selection = call;
    file_list.selected = selection->item_position - 1;
    selected_mask = 0;
    for (int i = 0; i < selection->selected_item_count; ++i) {
        int index = selection->selected_item_positions[i] - 1;
        if (index >= 0 && index < 64) selected_mask |= 1ULL << index;
    }
    update_controls();
}

static void open_file(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget; (void)data;
    XmListCallbackStruct *selection = call;
    file_list.selected = selection->item_position - 1;
    fileman_motif_action(FILEMAN_ACTION_OPEN);
}

static void select_folder(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget;
    if (updating) return;
    XmListCallbackStruct *selection = call;
    int index = selection->item_position - 1;
    if (index < 0 || (uint32_t)index >= tree_count) return;
    char path[RELIEFOS_FS_PATH_LEN];
    copy_text(path, sizeof(path), tree_path_for_id(tree_items[index].id));
    if (data) fileman_tree_toggle(tree_items[index].id);
    navigate_to_path(path);
    fileman_motif_refresh();
}

static void navigate_address(Widget widget, XtPointer data, XtPointer call)
{
    (void)data; (void)call;
    char *path = XmTextFieldGetString(widget);
    navigate_to_path(path);
    XtFree(path);
    fileman_motif_refresh();
    XmProcessTraversal(files, XmTRAVERSE_CURRENT);
}

static void key(Widget widget, XtPointer data, XEvent *event, Boolean *dispatch)
{
    (void)data;
    if (event->type != KeyPress) return;
    KeySym symbol = XLookupKeysym(&event->xkey, 0);
    unsigned modifiers = event->xkey.state;
    uint32_t action = 0;
    if ((modifiers & ControlMask) && symbol == XK_l) {
        XmProcessTraversal(address, XmTRAVERSE_CURRENT);
        XmTextFieldSetSelection(address, 0, strlen(current_path), event->xkey.time);
    } else if (symbol == XK_F5) action = FILEMAN_ACTION_REFRESH;
    else if ((modifiers & Mod1Mask) && symbol == XK_Up) action = FILEMAN_ACTION_UP;
    else if (widget != files) return;
    else if ((modifiers & ControlMask) && symbol == XK_c) action = FILEMAN_ACTION_COPY;
    else if ((modifiers & ControlMask) && symbol == XK_x) action = FILEMAN_ACTION_CUT;
    else if ((modifiers & ControlMask) && symbol == XK_v) action = FILEMAN_ACTION_PASTE;
    else if ((modifiers & ControlMask) && symbol == XK_a) action = FILEMAN_ACTION_SELECT_ALL;
    else if (symbol == XK_F2) action = FILEMAN_ACTION_RENAME;
    else if (symbol == XK_Delete) action = FILEMAN_ACTION_DELETE;
    else return;
    *dispatch = False;
    if (action) fileman_motif_action(action);
}

static void right_click(Widget widget, XtPointer data, XEvent *event, Boolean *dispatch)
{
    (void)data; (void)dispatch;
    if (event->type != ButtonPress || event->xbutton.button != Button3) return;
    int position = XmListYToPos(widget, event->xbutton.y);
    if (position > 0 && position <= (int)entry_count) {
        if (!fileman_entry_marked((uint32_t)position - 1)) {
            XmListDeselectAllItems(files);
            XmListSelectPos(files, position, False);
            selected_mask = 0;
        }
        file_list.selected = position - 1;
    } else {
        file_list.selected = -1;
        selected_mask = 0;
    }
    update_controls();
    XmMenuPosition(popup, &event->xbutton);
    XtManageChild(popup);
}

void fileman_motif_build(void)
{
    Widget form = XtVaCreateWidget("fileManager", xmFormWidgetClass, fileman_shell,
        XmNwidth, 900, XmNheight, 520, XmNresizePolicy, XmRESIZE_NONE,
        XmNmarginWidth, 8, XmNmarginHeight, 8, NULL);
    Widget bar = XmCreateMenuBar(form, "menuBar", NULL, 0);
    XtVaSetValues(bar, XmNtopAttachment, XmATTACH_FORM, XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM, NULL);
    struct reliefos_ui_context_menu_item items[FILEMAN_CONTEXT_MENU_COUNT];
    build_file_menu_items(items, FILEMAN_FILE_MENU_COUNT);
    menu_items(menu(bar, "File"), items, FILEMAN_FILE_MENU_COUNT);
    build_edit_menu_items(items, FILEMAN_EDIT_MENU_COUNT);
    menu_items(menu(bar, "Edit"), items, FILEMAN_EDIT_MENU_COUNT);
    Widget view = menu(bar, "View");
    action_button(view, "Up", FILEMAN_ACTION_UP);
    action_button(view, "Root", FILEMAN_ACTION_ROOT);
    action_button(view, "Refresh", FILEMAN_ACTION_REFRESH);
    action_button(view, "Settings...", FILEMAN_ACTION_SETTINGS);
    action_button(view, "About", FILEMAN_ACTION_ABOUT);
    Widget archive = menu(bar, "Archive");
    action_button(archive, "Compress to .tar", FILEMAN_ACTION_COMPRESS_TAR);
    action_button(archive, "Extract tar", FILEMAN_ACTION_EXTRACT_TAR);
    XtManageChild(bar);
    Widget toolbar = XtVaCreateManagedWidget("toolbar", xmRowColumnWidgetClass, form,
        XmNorientation, XmHORIZONTAL, XmNpacking, XmPACK_TIGHT,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, bar,
        XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_FORM, NULL);
    action_button(toolbar, "Up", FILEMAN_ACTION_UP);
    action_button(toolbar, "Open", FILEMAN_ACTION_OPEN);
    action_button(toolbar, "Refresh", FILEMAN_ACTION_REFRESH);
    action_button(toolbar, "New Folder", FILEMAN_ACTION_NEW_FOLDER);
    action_button(toolbar, "Copy", FILEMAN_ACTION_COPY);
    action_button(toolbar, "Cut", FILEMAN_ACTION_CUT);
    action_button(toolbar, "Paste", FILEMAN_ACTION_PASTE);
    action_button(toolbar, "Rename", FILEMAN_ACTION_RENAME);
    action_button(toolbar, "Delete", FILEMAN_ACTION_DELETE);
    address = XtVaCreateManagedWidget("address", xmTextFieldWidgetClass, form,
        XmNmaxLength, RELIEFOS_FS_PATH_LEN - 1, XmNheight, 32,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, toolbar,
        XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_FORM, NULL);
    XtAddCallback(address, XmNactivateCallback, navigate_address, NULL);
    XtInsertEventHandler(address, KeyPressMask, False, key, NULL, XtListHead);
    status = XtVaCreateManagedWidget("status", xmLabelWidgetClass, form,
        XmNalignment, XmALIGNMENT_BEGINNING, XmNrecomputeSize, False, XmNheight, 26,
        XmNbottomAttachment, XmATTACH_FORM, XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM, NULL);
    progress = XtVaCreateManagedWidget("progress", xmScaleWidgetClass, form,
        XmNorientation, XmHORIZONTAL, XmNminimum, 0, XmNmaximum, 100,
        XmNshowValue, False, XmNheight, 22, XmNtraversalOn, False,
        XmNbottomAttachment, XmATTACH_WIDGET, XmNbottomWidget, status,
        XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_FORM, NULL);
    Widget tree_heading = XtVaCreateManagedWidget("Folders", xmLabelWidgetClass, form,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, address,
        XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_POSITION,
        XmNrightPosition, 25, NULL);
    fileman_motif_label(tree_heading, T("Folders (double-click to expand)"));
    Widget list_heading = XtVaCreateManagedWidget("Type / Name", xmLabelWidgetClass, form,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, address,
        XmNleftAttachment, XmATTACH_POSITION, XmNleftPosition, 25,
        XmNrightAttachment, XmATTACH_FORM, NULL);
    fileman_motif_label(list_heading, T("Type / Name (Ctrl or Shift for multiple selection)"));
    folders = XmCreateScrolledList(form, "folders", NULL, 0);
    XtVaSetValues(folders, XmNselectionPolicy, XmBROWSE_SELECT, XmNvisibleItemCount, 18, NULL);
    XtVaSetValues(XtParent(folders), XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, tree_heading,
        XmNbottomAttachment, XmATTACH_WIDGET, XmNbottomWidget, progress,
        XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_POSITION,
        XmNrightPosition, 25, XmNrightOffset, 8, NULL);
    XtAddCallback(folders, XmNbrowseSelectionCallback, select_folder, NULL);
    XtAddCallback(folders, XmNdefaultActionCallback, select_folder, (XtPointer)1);
    XtManageChild(folders);
    files = XmCreateScrolledList(form, "files", NULL, 0);
    XtVaSetValues(files, XmNselectionPolicy, XmEXTENDED_SELECT, XmNvisibleItemCount, 18, NULL);
    XtVaSetValues(XtParent(files), XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, list_heading,
        XmNbottomAttachment, XmATTACH_WIDGET, XmNbottomWidget, progress,
        XmNleftAttachment, XmATTACH_POSITION, XmNleftPosition, 25,
        XmNrightAttachment, XmATTACH_FORM, NULL);
    XtAddCallback(files, XmNextendedSelectionCallback, select_files, NULL);
    XtAddCallback(files, XmNdefaultActionCallback, open_file, NULL);
    XtInsertEventHandler(files, KeyPressMask, False, key, NULL, XtListHead);
    XtAddEventHandler(files, ButtonPressMask, False, right_click, NULL);
    XtManageChild(files);
    popup = XmCreatePopupMenu(files, "context", NULL, 0);
    build_context_menu_items(items, FILEMAN_CONTEXT_MENU_COUNT);
    menu_items(popup, items, FILEMAN_CONTEXT_MENU_COUNT - 1);
    action_button(popup, "Compress to .tar", FILEMAN_ACTION_COMPRESS_TAR);
    action_button(popup, "Extract tar", FILEMAN_ACTION_EXTRACT_TAR);
    action_button(popup, "New Folder", FILEMAN_ACTION_NEW_FOLDER);
    fileman_tree_toggle(1);
    XtManageChild(form);
}
