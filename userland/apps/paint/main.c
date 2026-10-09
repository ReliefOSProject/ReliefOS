#include "model.h"
#include <libintl.h>
#include <locale.h>
#include <reliefos/layout.h>
#include <reliefos/png.h>
#include <fcntl.h>
#include <limits.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <X11/keysym.h>
#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/Protocols.h>
#include <Xm/PushB.h>
#include <Xm/ToggleB.h>
#include <Xm/DrawingA.h>
#include <Xm/FileSB.h>
#include <Xm/MessageB.h>
#include <Xm/Text.h>

#define PAINT_PATH_CAP PATH_MAX
#define T(s) gettext(s)

static uint32_t *canvas;
static uint32_t canvas_w;
static uint32_t canvas_h;
static uint32_t color = 0x00000000U;
static uint32_t brush_size = 4U;
static enum paint_tool tool = PAINT_TOOL_BRUSH;
static uint8_t drawing;
static uint8_t dirty;
static uint32_t stroke_x, stroke_y;
static char current_path[PAINT_PATH_CAP];
static XtAppContext app;
static Widget shell;
static Widget drawing_area;
static Widget status_label;
static Widget tool_buttons[3];
static Widget size_buttons[3];
static Widget swatch_buttons[6];
static Display *display;
static GC gc;

static const uint32_t swatch_colors[6] = {
    0x00000000U, 0x00ff0000U, 0x000080ffU,
    0x0000aa00U, 0x00ffff00U, 0x00ffffffU
};
static const uint32_t brush_sizes[3] = {2U, 6U, 14U};

static uint32_t text_len(const char *s)
{
    uint32_t n = 0;
    while (s && s[n]) ++n;
    return n;
}

static void copy_text(char *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0;
    if (!dst || cap == 0) return;
    while (src && src[i] && i + 1U < cap) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

static int ends_ci(const char *path, const char *suffix)
{
    uint32_t path_len = text_len(path);
    uint32_t suffix_len = text_len(suffix);
    if (suffix_len > path_len) return 0;
    for (uint32_t i = 0; i < suffix_len; ++i) {
        char a = path[path_len - suffix_len + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

static void set_label(Widget widget, const char *text)
{
    XmString value = XmStringCreateLocalized((char *)text);
    XtVaSetValues(widget, XmNlabelString, value, NULL);
    XmStringFree(value);
}

static void set_status(const char *text)
{
    set_label(status_label, text);
}

static void free_canvas(void)
{
    free(canvas);
    canvas = 0;
    canvas_w = 0;
    canvas_h = 0;
}

static void refresh_canvas(void)
{
    XImage *image;
    if (!canvas || !canvas_w || !canvas_h) return;
    image = XCreateImage(display, DefaultVisual(display, DefaultScreen(display)),
                         DefaultDepth(display, DefaultScreen(display)),
                         ZPixmap, 0, (char *)canvas, canvas_w, canvas_h, 32, 0);
    if (!image) return;
    XPutImage(display, XtWindow(drawing_area), gc, image, 0, 0, 0, 0,
              canvas_w, canvas_h);
    /* The pixels belong to the canvas; detach before freeing the wrapper. */
    image->data = NULL;
    XDestroyImage(image);
}

static void canvas_expose(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget;
    (void)data;
    (void)call;
    refresh_canvas();
}

static int new_canvas(uint32_t width, uint32_t height)
{
    uint64_t pixels = (uint64_t)width * height;
    uint32_t *next;
    if (!width || !height || pixels > PAINT_MAX_PIXELS) return -1;
    next = (uint32_t *)malloc((size_t)pixels * sizeof(uint32_t));
    if (!next) return -1;
    for (uint64_t i = 0; i < pixels; ++i) next[i] = 0x00ffffffU;
    free_canvas();
    canvas = next;
    canvas_w = width;
    canvas_h = height;
    dirty = 0;
    current_path[0] = 0;
    XtVaSetValues(drawing_area, XmNwidth, (int)width, XmNheight, (int)height, NULL);
    return 0;
}

static int read_file(const char *path, uint8_t **out, uint32_t *out_len)
{
    struct stat st;
    uint8_t *data;
    uint32_t offset = 0;
    int fd;
    if (!path || !out || !out_len || stat(path, &st) < 0 ||
        !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (uint64_t)st.st_size > RELIEFOS_PNG_MAX_FILE_BYTES) {
        return -1;
    }
    data = (uint8_t *)malloc((size_t)st.st_size);
    if (!data) return -1;
    fd = open(path, O_RDONLY);
    if (fd < 0) { free(data); return fd; }
    while (offset < (uint32_t)st.st_size) {
        long got = read(fd, data + offset, (uint32_t)st.st_size - offset);
        if (got <= 0) { close(fd); free(data); return -1; }
        offset += (uint32_t)got;
    }
    close(fd);
    *out = data;
    *out_len = offset;
    return 0;
}

static int load_image(const char *path)
{
    uint32_t *pixels = 0;
    uint32_t width = 0, height = 0;
    uint8_t *data = 0;
    uint32_t len = 0;
    int ret;
    if (ends_ci(path, ".png")) {
        ret = reliefos_png_decode_file(path, &pixels, &width, &height);
    } else {
        ret = read_file(path, &data, &len);
        if (ret == 0) ret = paint_bmp_decode(data, len, &pixels, &width, &height);
        free(data);
    }
    if (ret < 0 || !pixels || width == 0 || height == 0) {
        free(pixels);
        set_status(T("Could not open image"));
        return -1;
    }
    free_canvas();
    canvas = pixels;
    canvas_w = width;
    canvas_h = height;
    copy_text(current_path, sizeof(current_path), path);
    dirty = 0;
    XtVaSetValues(drawing_area, XmNwidth, (int)width, XmNheight, (int)height, NULL);
    set_status(T("Image opened"));
    return 0;
}

static int write_all(int fd, const void *buffer, uint32_t length)
{
    const uint8_t *data = (const uint8_t *)buffer;
    while (length) {
        long wrote = write(fd, data, length);
        if (wrote <= 0) return -1;
        data += wrote;
        length -= (uint32_t)wrote;
    }
    return 0;
}

static int save_bmp(const char *path)
{
    uint8_t *data = 0;
    uint32_t len = 0;
    int fd;
    int ret;
    if (paint_bmp_encode(canvas, canvas_w, canvas_h, &data, &len) < 0) return -1;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { free(data); return fd; }
    ret = write_all(fd, data, len);
    free(data);
    close(fd);
    return ret;
}

static int save_png(const char *path)
{
    png_image image;
    uint8_t *rgb;
    uint64_t bytes = (uint64_t)canvas_w * canvas_h * 3U;
    int ret;
    if (bytes > 0xffffffffULL) return -1;
    rgb = (uint8_t *)malloc((size_t)bytes);
    if (!rgb) return -1;
    for (uint64_t i = 0; i < (uint64_t)canvas_w * canvas_h; ++i) {
        uint32_t pixel = canvas[i];
        rgb[i * 3U] = (uint8_t)(pixel >> 16);
        rgb[i * 3U + 1U] = (uint8_t)(pixel >> 8);
        rgb[i * 3U + 2U] = (uint8_t)pixel;
    }
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    image.width = canvas_w;
    image.height = canvas_h;
    image.format = PNG_FORMAT_RGB;
    ret = png_image_write_to_file(&image, path, 0, rgb, 0, 0) ? 0 : -1;
    png_image_free(&image);
    free(rgb);
    return ret;
}

static int save_image(const char *path)
{
    int ret;
    if (!path || !path[0] || !canvas) return -1;
    ret = ends_ci(path, ".png") ? save_png(path) : save_bmp(path);
    if (ret < 0) {
        set_status(T("Could not save image"));
        return ret;
    }
    copy_text(current_path, sizeof(current_path), path);
    dirty = 0;
    set_status(T("Image saved"));
    return 0;
}

/* Modal Motif dialogs run a nested event loop until a callback settles them. */
struct dialog_result {
    int done;
    int outcome;
    char path[PAINT_PATH_CAP];
};

static void file_ok(Widget widget, XtPointer data, XtPointer call)
{
    struct dialog_result *result = (struct dialog_result *)data;
    XmFileSelectionBoxCallbackStruct *cb =
        (XmFileSelectionBoxCallbackStruct *)call;
    char *name = 0;
    if (cb->value) {
        XmStringGetLtoR(cb->value, XmFONTLIST_DEFAULT_TAG, &name);
    }
    if (!name) {
        name = XmTextGetString(XmFileSelectionBoxGetChild(widget, XmDIALOG_TEXT));
    }
    if (name) {
        copy_text(result->path, sizeof(result->path), name);
        XtFree(name);
    }
    result->outcome = 1;
    result->done = 1;
}

static void dialog_settle(Widget widget, XtPointer data, XtPointer call)
{
    struct dialog_result *result = (struct dialog_result *)data;
    (void)widget;
    (void)call;
    result->done = 1;
}

static void dialog_ok(Widget widget, XtPointer data, XtPointer call)
{
    struct dialog_result *result = (struct dialog_result *)data;
    result->outcome = 1;
    dialog_settle(widget, data, call);
}

static void dialog_cancel(Widget widget, XtPointer data, XtPointer call)
{
    struct dialog_result *result = (struct dialog_result *)data;
    result->outcome = 0;
    dialog_settle(widget, data, call);
}

static void dialog_help(Widget widget, XtPointer data, XtPointer call)
{
    struct dialog_result *result = (struct dialog_result *)data;
    result->outcome = -1;
    dialog_settle(widget, data, call);
}

static void pump_until_done(struct dialog_result *result)
{
    while (!result->done) {
        XEvent event;
        XtAppNextEvent(app, &event);
        XtDispatchEvent(&event);
    }
}

/* A modal dialog can map below its transient parent under some window
 * managers, leaving the app blocked on an invisible question; raise the
 * dialog shell as soon as it maps. */
static void raise_dialog(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget;
    (void)call;
    Widget shell = (Widget)data;
    Window window = XtWindow(shell);
    if (window)
        XRaiseWindow(XtDisplay(shell), window);
}

static int run_file_dialog(const char *title, const char *initial, char *path,
                           uint32_t cap)
{
    struct dialog_result result = {0, 0, {0}};
    Widget dialog = XmCreateFileSelectionDialog(shell, "fileDialog", NULL, 0);
    XmString title_text = XmStringCreateLocalized((char *)title);
    XmString initial_text = XmStringCreateLocalized((char *)initial);
    XtVaSetValues(dialog, XmNdialogTitle, title_text, XmNdirSpec, initial_text,
                  XmNautoUnmanage, True, NULL);
    XmStringFree(title_text);
    XmStringFree(initial_text);
    XtUnmanageChild(XmFileSelectionBoxGetChild(dialog, XmDIALOG_HELP_BUTTON));
    XtAddCallback(dialog, XmNokCallback, file_ok, &result);
    XtAddCallback(dialog, XmNcancelCallback, dialog_cancel, &result);
    XtAddCallback(dialog, XmNmapCallback, raise_dialog, XtParent(dialog));
    XtManageChild(dialog);
    pump_until_done(&result);
    XtUnmanageChild(dialog);
    XtDestroyWidget(dialog);
    if (result.outcome > 0 && result.path[0]) {
        copy_text(path, cap, result.path);
        return 1;
    }
    return result.outcome;
}

/* OK=1, Cancel=0, Help/abort=-1; pass help_label=NULL for a two-way dialog. */
static int run_question(const char *title, const char *text,
                        const char *ok_label, const char *cancel_label,
                        const char *help_label)
{
    struct dialog_result result = {0, 0, {0}};
    Widget dialog = XmCreateQuestionDialog(shell, "question", NULL, 0);
    XmString title_text = XmStringCreateLocalized((char *)title);
    XmString message = XmStringCreateLocalized((char *)text);
    XtVaSetValues(dialog,
                  XmNdialogTitle, title_text,
                  XmNmessageString, message,
                  XmNdialogStyle, XmDIALOG_FULL_APPLICATION_MODAL,
                  XmNautoUnmanage, True, NULL);
    XmStringFree(title_text);
    XmStringFree(message);
    set_label(XmMessageBoxGetChild(dialog, XmDIALOG_OK_BUTTON), ok_label);
    set_label(XmMessageBoxGetChild(dialog, XmDIALOG_CANCEL_BUTTON), cancel_label);
    if (help_label) {
        set_label(XmMessageBoxGetChild(dialog, XmDIALOG_HELP_BUTTON), help_label);
        XtAddCallback(dialog, XmNhelpCallback, dialog_help, &result);
    } else {
        XtUnmanageChild(XmMessageBoxGetChild(dialog, XmDIALOG_HELP_BUTTON));
    }
    XtAddCallback(dialog, XmNokCallback, dialog_ok, &result);
    XtAddCallback(dialog, XmNcancelCallback, dialog_cancel, &result);
    XtAddCallback(dialog, XmNmapCallback, raise_dialog, XtParent(dialog));
    XtManageChild(dialog);
    pump_until_done(&result);
    XtUnmanageChild(dialog);
    XtDestroyWidget(dialog);
    return result.outcome;
}

static void open_dialog(void)
{
    char path[PAINT_PATH_CAP] = {0};
    if (run_file_dialog(T("Open image"), "", path, sizeof(path)) > 0) {
        (void)load_image(path);
    }
}

static void save_as_dialog(void)
{
    char path[PAINT_PATH_CAP];
    copy_text(path, sizeof(path), current_path[0] ? current_path : "/untitled.bmp");
    if (run_file_dialog(T("Save image"), path, path, sizeof(path)) > 0) {
        (void)save_image(path);
    }
}

static void save_current(void)
{
    if (current_path[0]) {
        (void)save_image(current_path);
    } else {
        save_as_dialog();
    }
}

static void sync_toolbar(void)
{
    XtVaSetValues(tool_buttons[0], XmNset, tool == PAINT_TOOL_PENCIL, NULL);
    XtVaSetValues(tool_buttons[1], XmNset, tool == PAINT_TOOL_BRUSH, NULL);
    XtVaSetValues(tool_buttons[2], XmNset, tool == PAINT_TOOL_ERASER, NULL);
    for (int i = 0; i < 3; ++i) {
        XtVaSetValues(size_buttons[i], XmNset, brush_size == brush_sizes[i], NULL);
    }
    for (int i = 0; i < 6; ++i) {
        XtVaSetValues(swatch_buttons[i], XmNset, color == swatch_colors[i], NULL);
    }
}

static void new_image(void)
{
    if (dirty && run_question(T("Discard changes?"),
                              T("The current drawing has not been saved."),
                              T("Discard"), T("Keep"), 0) <= 0) {
        return;
    }
    if (new_canvas(800U, 520U) < 0) {
        set_status(T("Could not create canvas"));
    } else {
        set_status(T("New canvas"));
        refresh_canvas();
    }
}

static void close_window(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget;
    (void)data;
    (void)call;
    if (dirty) {
        int choice = run_question(T("Save changes?"),
                                  T("Save the current drawing before closing?"),
                                  T("Save"), T("Discard"), T("Cancel"));
        if (choice > 0) {
            save_current();
            if (dirty) return;
        } else if (choice < 0) {
            return;
        }
    }
    XtAppSetExitFlag(app);
}

static void button_new(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget; (void)data; (void)call;
    new_image();
}

static void button_open(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget; (void)data; (void)call;
    open_dialog();
    refresh_canvas();
}

static void button_save(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget; (void)data; (void)call;
    save_current();
}

static void button_save_as(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget; (void)data; (void)call;
    save_as_dialog();
}

static void select_tool(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget; (void)call;
    if (!XmToggleButtonGetState(widget)) {
        XmToggleButtonSetState(widget, True, False);
        return;
    }
    tool = (enum paint_tool)(uintptr_t)data;
    sync_toolbar();
}

static void select_size(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget; (void)call;
    if (!XmToggleButtonGetState(widget)) {
        XmToggleButtonSetState(widget, True, False);
        return;
    }
    brush_size = brush_sizes[(uintptr_t)data];
    sync_toolbar();
}

static void select_color(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget; (void)call;
    if (!XmToggleButtonGetState(widget)) {
        XmToggleButtonSetState(widget, True, False);
        return;
    }
    color = swatch_colors[(uintptr_t)data];
    sync_toolbar();
}

/* XmNinputCallback never reports MotionNotify, and dragging the brush is all
 * motion events, so the drawing interaction is an event handler instead. */
static void canvas_input(Widget widget, XtPointer data, XEvent *event,
                         Boolean *dispatch)
{
    (void)widget;
    (void)data;
    (void)dispatch;
    if (!canvas || !canvas_w || !canvas_h) return;
    if (event->type == ButtonPress && event->xbutton.button == Button1) {
        uint32_t x = (uint32_t)event->xbutton.x;
        uint32_t y = (uint32_t)event->xbutton.y;
        if (x >= canvas_w || y >= canvas_h) return;
        drawing = 1;
        stroke_x = x;
        stroke_y = y;
        paint_draw_point(canvas, canvas_w, canvas_h, x, y, color, brush_size, tool);
        dirty = 1;
        refresh_canvas();
    } else if (event->type == MotionNotify && drawing) {
        uint32_t x = (uint32_t)event->xmotion.x;
        uint32_t y = (uint32_t)event->xmotion.y;
        if (x >= canvas_w) x = canvas_w - 1U;
        if (y >= canvas_h) y = canvas_h - 1U;
        paint_draw_line(canvas, canvas_w, canvas_h, stroke_x, stroke_y, x, y,
                        color, brush_size, tool);
        stroke_x = x;
        stroke_y = y;
        dirty = 1;
        refresh_canvas();
    } else if (event->type == ButtonRelease && event->xbutton.button == Button1) {
        drawing = 0;
    }
}

static void key(Widget widget, XtPointer data, XEvent *event, Boolean *dispatch)
{
    KeySym symbol;
    (void)widget;
    (void)data;
    if (event->type != KeyPress) return;
    symbol = XLookupKeysym(&event->xkey, 0);
    if (symbol == XK_Escape) {
        close_window(widget, data, 0);
        *dispatch = False;
        return;
    }
    if (!(event->xkey.state & ControlMask)) return;
    if (symbol == XK_n || symbol == XK_N) new_image();
    else if (symbol == XK_o || symbol == XK_O) open_dialog();
    else if (symbol == XK_s || symbol == XK_S) save_current();
    else if (symbol == XK_a || symbol == XK_A) save_as_dialog();
    else return;
    refresh_canvas();
    *dispatch = False;
}

/* Xt event handlers do not bubble to ancestors, so every widget that can hold
 * keyboard focus needs its own registration (fileman/taskmgr do the same).
 * The shell is included because the window manager focuses the top-level
 * window and the canvas never takes X input focus on its own. */
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
    static char *tool_names[3] = {"pencil", "brush", "eraser"};
    static char *size_names[3] = {"sizeS", "sizeM", "sizeL"};
    setlocale(LC_ALL, "");
    bindtextdomain("leonos", RELIEFOS_LAYOUT_LOCALE);
    textdomain("leonos");
    XtSetLanguageProc(NULL, NULL, NULL);
    char *fallback[] = {
        "*background: #eceef4", "*foreground: #22242e",
        "*highlightColor: #3b62a6", NULL
    };
    shell = XtVaAppInitialize(&app, "ReliefOSPaint", NULL, 0, &argc, argv,
                              fallback, XtNtitle, T("Paint"),
                              XtNwidth, 848, XtNheight, 660, NULL);
    {
        XmFontList fonts = app_font_list(shell);
        XtVaSetValues(shell, XmNlabelFontList, fonts,
                      XmNbuttonFontList, fonts, XmNtextFontList, fonts, NULL);
    }
    watch_keys(shell);
    Widget form = XtVaCreateWidget("paint", xmFormWidgetClass, shell,
        XmNresizePolicy, XmRESIZE_NONE, XmNmarginWidth, 8, XmNmarginHeight, 8, NULL);
    Widget bar = XtVaCreateManagedWidget("toolbar", xmFormWidgetClass, form,
        XmNtopAttachment, XmATTACH_FORM, XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM, NULL);
    Widget previous;
    {
        Widget button = XtVaCreateManagedWidget("new", xmPushButtonWidgetClass, bar,
            XmNtopAttachment, XmATTACH_FORM, XmNleftAttachment, XmATTACH_FORM, NULL);
        set_label(button, T("New"));
        XtAddCallback(button, XmNactivateCallback, button_new, NULL);
        watch_keys(button);
        previous = button;
    }
    {
        struct { char *name; const char *label; XtCallbackProc callback; }
        entries[3] = {
            {"open", T("Open"), button_open},
            {"save", T("Save"), button_save},
            {"saveAs", T("Save as"), button_save_as},
        };
        for (int i = 0; i < 3; ++i) {
            Widget button = XtVaCreateManagedWidget(entries[i].name,
                xmPushButtonWidgetClass, bar,
                XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, previous,
                XmNtopAttachment, XmATTACH_FORM, NULL);
            set_label(button, entries[i].label);
            XtAddCallback(button, XmNactivateCallback, entries[i].callback, NULL);
            watch_keys(button);
            previous = button;
        }
    }
    {
        const char *labels[3];
        labels[0] = T("Pencil");
        labels[1] = T("Brush");
        labels[2] = T("Eraser");
        for (int i = 0; i < 3; ++i) {
            tool_buttons[i] = XtVaCreateManagedWidget(tool_names[i],
                xmToggleButtonWidgetClass, bar,
                XmNindicatorOn, False,
                XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, previous,
                XmNtopAttachment, XmATTACH_FORM, NULL);
            set_label(tool_buttons[i], labels[i]);
            XtAddCallback(tool_buttons[i], XmNvalueChangedCallback, select_tool,
                          (XtPointer)(uintptr_t)i);
            watch_keys(tool_buttons[i]);
            previous = tool_buttons[i];
        }
    }
    for (int i = 0; i < 3; ++i) {
        size_buttons[i] = XtVaCreateManagedWidget(size_names[i],
            xmToggleButtonWidgetClass, bar,
            XmNindicatorOn, False, XmNrecomputeSize, False, XmNwidth, 28,
            XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, previous,
            XmNtopAttachment, XmATTACH_FORM, NULL);
        set_label(size_buttons[i], i == 0 ? "S" : (i == 1 ? "M" : "L"));
        XtAddCallback(size_buttons[i], XmNvalueChangedCallback, select_size,
                      (XtPointer)(uintptr_t)i);
        watch_keys(size_buttons[i]);
        previous = size_buttons[i];
    }
    for (int i = 0; i < 6; ++i) {
        char name[16];
        snprintf(name, sizeof(name), "swatch%d", i);
        swatch_buttons[i] = XtVaCreateManagedWidget(name,
            xmToggleButtonWidgetClass, bar,
            XmNindicatorOn, False, XmNrecomputeSize, False, XmNwidth, 26,
            XmNbackground, (unsigned long)swatch_colors[i],
            XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, previous,
            XmNtopAttachment, XmATTACH_FORM, NULL);
        /* Motif would otherwise render the squeezed widget name on the chip. */
        set_label(swatch_buttons[i], "");
        XtAddCallback(swatch_buttons[i], XmNvalueChangedCallback, select_color,
                      (XtPointer)(uintptr_t)i);
        watch_keys(swatch_buttons[i]);
        previous = swatch_buttons[i];
    }
    drawing_area = XtVaCreateManagedWidget("canvas", xmDrawingAreaWidgetClass, form,
        XmNwidth, 800, XmNheight, 520,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, bar,
        XmNleftAttachment, XmATTACH_FORM, NULL);
    XtAddCallback(drawing_area, XmNexposeCallback, canvas_expose, NULL);
    XtAddEventHandler(drawing_area,
                      ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                      False, canvas_input, NULL);
    XtInsertEventHandler(drawing_area, KeyPressMask, False, key, NULL, XtListHead);
    status_label = XtVaCreateManagedWidget("status", xmLabelWidgetClass, form,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM, NULL);
    set_status(T("Ready"));
    if (new_canvas(800U, 520U) < 0) {
        puts("[paint.elf] canvas allocation failed");
        return 1;
    }
    if (argc > 1 && argv && argv[1] && argv[1][0]) (void)load_image(argv[1]);
    XtManageChild(form);
    XtRealizeWidget(shell);
    display = XtDisplay(shell);
    gc = XCreateGC(display, XtWindow(drawing_area), 0, NULL);
    Atom delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(shell, delete_window, close_window, NULL);
    sync_toolbar();
    puts("[paint.elf] Motif paint ready");
    fflush(stdout);
    XtAppMainLoop(app);
    free_canvas();
    XtDestroyWidget(shell);
    XtDestroyApplicationContext(app);
    return 0;
}
