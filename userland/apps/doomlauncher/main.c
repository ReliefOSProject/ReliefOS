#include "model.h"
#include <libintl.h>
#include <locale.h>
#include <reliefos/gui.h>
#include <reliefos/launch.h>
#include <reliefos/launch_result.h>
#include <reliefos/layout.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <X11/keysym.h>
#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/Protocols.h>
#include <Xm/PushB.h>
#include <Xm/Text.h>
#include <Xm/TextF.h>
#include <Xm/ToggleB.h>

#define DOOM_PATH RELIEFOS_LAYOUT_RELIEFOS_APPS "/doom/doom.elf"
#define DEFAULT_IWAD RELIEFOS_LAYOUT_RELIEFOS_APPS "/doom/freedoom1.wad"
#define TASK_STATE_EXITED 3U
#define DOOM_POLL_MS 250U
#define T(s) gettext(s)

static char status_text[160];
static uint32_t doom_pid;
static XtAppContext app;
static Widget shell;
static Widget iwad_field;
static Widget args_field;
static Widget sound_check;
static Widget fullscreen_check;
static Widget launch_button;
static Widget status_label;

static void copy_text(char *dst, uint32_t capacity, const char *src)
{
    uint32_t i = 0;
    if (!dst || !capacity) return;
    while (src && src[i] && i + 1U < capacity) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

static void set_label(Widget widget, const char *text)
{
    XmString value = XmStringCreateLocalized((char *)text);
    XtVaSetValues(widget, XmNlabelString, value, NULL);
    XmStringFree(value);
}

static void set_status(const char *text)
{
    copy_text(status_text, sizeof(status_text), text);
    set_label(status_label, status_text);
}

static void set_status_code(const char *prefix, int code)
{
    uint32_t pos = 0;
    char digits[16];
    uint32_t count = 0;
    if (!prefix) {
        prefix = T("Status ");
    }
    while (prefix[pos] && pos + 1U < sizeof(status_text)) {
        status_text[pos] = prefix[pos];
        ++pos;
    }
    if (code < 0 && pos + 1U < sizeof(status_text)) {
        status_text[pos++] = '-';
        code = -code;
    }
    if (code == 0) {
        digits[count++] = '0';
    }
    while (code > 0 && count < sizeof(digits)) {
        digits[count++] = (char)('0' + code % 10);
        code /= 10;
    }
    while (count && pos + 1U < sizeof(status_text)) {
        status_text[pos++] = digits[--count];
    }
    status_text[pos] = 0;
    set_label(status_label, status_text);
}

static const char *launcher_error_text(int code)
{
    switch (code) {
    case LAUNCH_RESULT_EMPTY:
        return T("Arguments are empty");
    case LAUNCH_RESULT_TOO_MANY_ARGS:
        return T("Too many arguments");
    case LAUNCH_RESULT_UNCLOSED_QUOTE:
        return T("Missing closing quote");
    default:
        return reliefos_launch_error_text(code);
    }
}

static void sync_controls(void)
{
    XtVaSetValues(launch_button, XmNsensitive, doom_pid == 0, NULL);
}

static void reset_settings(void)
{
    struct doomlauncher_options options;
    doomlauncher_defaults(&options, DEFAULT_IWAD);
    XmTextFieldSetString(iwad_field, options.iwad);
    XmTextFieldSetString(args_field, "");
    XmToggleButtonSetState(sound_check, options.disable_sound != 0, False);
    XmToggleButtonSetState(fullscreen_check, options.fullscreen != 0, False);
    set_status(T("Settings reset"));
}

static void launch_doom(void)
{
    struct doomlauncher_options options;
    char *argv[DOOMLAUNCHER_MAX_ARGS];
    char *extra_argv[DOOMLAUNCHER_MAX_ARGS];
    char extra_copy[DOOMLAUNCHER_EXTRA_CAP];
    char *iwad;
    char *extra;
    int extra_count = 0;
    int built;
    int pid;
    if (doom_pid) {
        return;
    }
    iwad = XmTextGetString(iwad_field);
    extra = XmTextGetString(args_field);
    memset(&options, 0, sizeof(options));
    copy_text(options.iwad, sizeof(options.iwad), iwad ? iwad : "");
    copy_text(options.extra, sizeof(options.extra), extra ? extra : "");
    options.disable_sound = XmToggleButtonGetState(sound_check) ? 1U : 0U;
    options.fullscreen = XmToggleButtonGetState(fullscreen_check) ? 1U : 0U;
    if (iwad) XtFree(iwad);
    if (extra) XtFree(extra);
    if (!options.iwad[0]) {
        set_status(T("An IWAD path is required"));
        return;
    }
    copy_text(extra_copy, sizeof(extra_copy), options.extra);
    if (options.extra[0]) {
        extra_count = reliefos_cmdline_split(extra_copy, extra_argv,
                                             DOOMLAUNCHER_MAX_ARGS);
        if (extra_count < 0) {
            set_status(launcher_error_text(extra_count));
            return;
        }
    }
    built = doomlauncher_build_argv(DOOM_PATH, &options, extra_argv,
                                    extra_count, argv, DOOMLAUNCHER_MAX_ARGS);
    if (built == DOOMLAUNCHER_ERR_NO_IWAD) {
        set_status(T("An IWAD path is required"));
        return;
    }
    if (built < 0) {
        set_status(T("Too many arguments"));
        return;
    }
    pid = reliefos_spawn_argv(DOOM_PATH, argv);
    if (pid < 0) {
        set_status_code(T("Launch failed: "), pid);
        return;
    }
    doom_pid = (uint32_t)pid;
    sync_controls();
    set_status(T("Starting DOOM: the game window shows loading progress"));
}

static void update_doom_status(void)
{
    struct reliefos_task_info tasks[RELIEFOS_TASK_MAX];
    uint64_t tick;
    int snapshot_count;
    if (!doom_pid) {
        return;
    }
    snapshot_count = reliefos_task_snapshot(tasks, RELIEFOS_TASK_MAX, &tick);
    if (snapshot_count < 0) {
        return;
    }
    for (uint32_t i = 0; i < (uint32_t)snapshot_count; ++i) {
        if (tasks[i].pid != doom_pid) {
            continue;
        }
        if (tasks[i].state == TASK_STATE_EXITED) {
            int exit_status = 0;
            int reaped = wait4((int)doom_pid, &exit_status, 0, 0);
            int code = doomlauncher_exit_code(reaped, exit_status);
            doom_pid = 0;
            set_status_code(T("DOOM exited with code "), code);
            sync_controls();
        }
        return;
    }
    doom_pid = 0;
    sync_controls();
    set_status(T("DOOM is no longer running"));
}

static void doom_poll_tick(XtPointer data, XtIntervalId *id)
{
    (void)data;
    (void)id;
    update_doom_status();
    XtAppAddTimeOut(app, DOOM_POLL_MS, doom_poll_tick, NULL);
}

static void field_activate(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget;
    (void)data;
    (void)call;
    launch_doom();
}

static void button_launch(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget;
    (void)data;
    (void)call;
    launch_doom();
}

static void button_reset(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget;
    (void)data;
    (void)call;
    reset_settings();
}

static void close_window(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget;
    (void)data;
    (void)call;
    XtAppSetExitFlag(app);
}

static void key(Widget widget, XtPointer data, XEvent *event, Boolean *dispatch)
{
    (void)widget;
    (void)data;
    if (event->type != KeyPress) return;
    if (XLookupKeysym(&event->xkey, 0) == XK_Escape) {
        close_window(widget, data, 0);
        *dispatch = False;
    }
}

/* Xt event handlers do not bubble to ancestors, so every widget that can hold
 * keyboard focus needs its own registration (fileman/taskmgr do the same). */
static void watch_keys(Widget widget)
{
    XtInsertEventHandler(widget, KeyPressMask, False, key, NULL, XtListHead);
}

/* The XmNfontList string resource can only describe core fonts, which have no
 * CJK glyphs; route widgets to a CJK-capable Xft rendition instead. */
static XmFontList app_font_list(Widget widget_shell)
{
    Arg args[3];
    XmRendition rendition;
    XtSetArg(args[0], XmNfontName, "SimSun");
    XtSetArg(args[1], XmNfontType, XmFONT_IS_XFT);
    XtSetArg(args[2], XmNloadModel, XmLOAD_IMMEDIATE);
    rendition = XmRenditionCreate(widget_shell, XmFONTLIST_DEFAULT_TAG, args, 3);
    XmFontList list = XmRenderTableAddRenditions(NULL, &rendition, 1, XmDUPLICATE);
    XmRenditionFree(rendition);
    return list;
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    bindtextdomain("leonos", RELIEFOS_LAYOUT_LOCALE);
    textdomain("leonos");
    XtSetLanguageProc(NULL, NULL, NULL);
    char *fallback[] = {
        "*background: #eceef4", "*foreground: #22242e",
        "*highlightColor: #3b62a6", NULL
    };
    shell = XtVaAppInitialize(&app, "ReliefOSDoomLauncher", NULL, 0, &argc, argv,
                              fallback, XtNtitle, T("DOOM Launcher"),
                              XtNwidth, 640, XtNheight, 330, NULL);
    {
        XmFontList fonts = app_font_list(shell);
        XtVaSetValues(shell, XmNlabelFontList, fonts,
                      XmNbuttonFontList, fonts, XmNtextFontList, fonts, NULL);
    }
    watch_keys(shell);
    Widget form = XtVaCreateWidget("doomlauncher", xmFormWidgetClass, shell,
        XmNresizePolicy, XmRESIZE_NONE, XmNmarginWidth, 12, XmNmarginHeight, 12, NULL);
    Widget iwad_label = XtVaCreateManagedWidget("iwadLabel", xmLabelWidgetClass, form,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM, NULL);
    set_label(iwad_label, T("IWAD path"));
    iwad_field = XtVaCreateManagedWidget("iwadField", xmTextFieldWidgetClass, form,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, iwad_label,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM, NULL);
    XtAddCallback(iwad_field, XmNactivateCallback, field_activate, NULL);
    watch_keys(iwad_field);
    Widget args_label = XtVaCreateManagedWidget("argsLabel", xmLabelWidgetClass, form,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, iwad_field,
        XmNleftAttachment, XmATTACH_FORM, NULL);
    set_label(args_label, T("Extra DOOM arguments"));
    args_field = XtVaCreateManagedWidget("argsField", xmTextFieldWidgetClass, form,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, args_label,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM, NULL);
    XtAddCallback(args_field, XmNactivateCallback, field_activate, NULL);
    watch_keys(args_field);
    sound_check = XtVaCreateManagedWidget("disableSound", xmToggleButtonWidgetClass, form,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, args_field,
        XmNleftAttachment, XmATTACH_FORM, NULL);
    set_label(sound_check, T("Disable sound"));
    fullscreen_check = XtVaCreateManagedWidget("fullscreen", xmToggleButtonWidgetClass, form,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, args_field,
        XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, sound_check, NULL);
    set_label(fullscreen_check, T("Fullscreen"));
    launch_button = XtVaCreateManagedWidget("launch", xmPushButtonWidgetClass, form,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, sound_check,
        XmNleftAttachment, XmATTACH_FORM, NULL);
    set_label(launch_button, T("Launch"));
    XtAddCallback(launch_button, XmNactivateCallback, button_launch, NULL);
    watch_keys(launch_button);
    Widget reset_button = XtVaCreateManagedWidget("reset", xmPushButtonWidgetClass, form,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, sound_check,
        XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, launch_button, NULL);
    set_label(reset_button, T("Reset"));
    XtAddCallback(reset_button, XmNactivateCallback, button_reset, NULL);
    watch_keys(reset_button);
    watch_keys(sound_check);
    watch_keys(fullscreen_check);
    status_label = XtVaCreateManagedWidget("status", xmLabelWidgetClass, form,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM, NULL);
    reset_settings();
    set_status(T("Ready"));
    XtManageChild(form);
    XtRealizeWidget(shell);
    Atom delete_window = XInternAtom(XtDisplay(shell), "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(shell, delete_window, close_window, NULL);
    XtAppAddTimeOut(app, DOOM_POLL_MS, doom_poll_tick, NULL);
    puts("[doomlauncher.elf] Motif DOOM launcher ready");
    fflush(stdout);
    XtAppMainLoop(app);
    XtDestroyWidget(shell);
    XtDestroyApplicationContext(app);
    return 0;
}
