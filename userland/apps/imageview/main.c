#include "model.h"
#include <libintl.h>
#include <locale.h>
#include <reliefos/layout.h>
#include <reliefos/png.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <X11/keysym.h>
#include <Xm/DrawingA.h>
#include <Xm/FileSB.h>
#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/Protocols.h>
#include <Xm/PushB.h>
#include <Xm/Text.h>
#include <Xm/ToggleB.h>

#define IMAGEVIEW_ROWS_MAX 64U
#define T(s) gettext(s)

static uint32_t *image_pixels;
static uint32_t image_w;
static uint32_t image_h;
static uint32_t *view_pixels;
static uint64_t view_cap;
static enum imageview_zoom zoom_mode = IMAGEVIEW_ZOOM_FIT;
static char current_path[PATH_MAX];
static char current_dir[PATH_MAX];
static char status_text[160];
static char detail_text[192];
static char sibling_names[IMAGEVIEW_ROWS_MAX][NAME_MAX + 1U];
static uint32_t sibling_count;
static uint32_t sibling_index;
static XtAppContext app;
static Widget shell;
static Widget drawing_area;
static Widget prev_button;
static Widget next_button;
static Widget zoom_buttons[3];
static Widget path_label;
static Widget detail_label;
static Widget status_label;
static Display *display;
static GC gc;

static uint32_t text_len(const char *text)
{
    uint32_t n = 0;
    while (text && text[n]) ++n;
    return n;
}

static char ascii_tolower(char ch)
{
    if (ch >= 'A' && ch <= 'Z') return (char)(ch - 'A' + 'a');
    return ch;
}

static int ends_with_ignore_case(const char *text, const char *suffix)
{
    uint32_t text_n = text_len(text);
    uint32_t suffix_n = text_len(suffix);
    if (!text || !suffix || suffix_n > text_n) return 0;
    for (uint32_t i = 0; i < suffix_n; ++i) {
        if (ascii_tolower(text[text_n - suffix_n + i]) != suffix[i]) return 0;
    }
    return 1;
}

static int text_eq_ignore_case(const char *a, const char *b)
{
    uint32_t i = 0;
    if (!a || !b) return 0;
    while (a[i] && b[i] && ascii_tolower(a[i]) == ascii_tolower(b[i])) ++i;
    return a[i] == 0 && b[i] == 0;
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

static const char *path_basename(const char *path)
{
    const char *base = path ? path : "";
    for (uint32_t i = 0; path && path[i]; ++i) {
        if (path[i] == '/') base = path + i + 1U;
    }
    return base;
}

static void path_parent(char *dst, uint32_t cap, const char *path)
{
    uint32_t len;
    copy_text(dst, cap, path);
    len = text_len(dst);
    while (len > 1U && dst[len - 1U] != '/') dst[--len] = 0;
    if (len > 1U) dst[len - 1U] = 0;
}

static void build_child_path(char *dst, uint32_t cap, const char *dir,
                             const char *name)
{
    copy_text(dst, cap, dir);
    uint32_t pos = text_len(dst);
    if (pos && dst[pos - 1U] != '/') dst[pos++] = '/';
    copy_text(dst + pos, cap - pos, name);
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

static void free_image(void)
{
    reliefos_png_free(image_pixels);
    image_pixels = 0;
    image_w = 0;
    image_h = 0;
}

static void rebuild_detail(void)
{
    if (!image_pixels) {
        copy_text(detail_text, sizeof(detail_text), T("No image loaded."));
    } else {
        imageview_format_detail(detail_text, sizeof(detail_text), image_w,
                                image_h, zoom_mode, sibling_index, sibling_count);
    }
    set_label(detail_label, detail_text);
}

static void sync_toolbar(void)
{
    for (int i = 0; i < 3; ++i) {
        XtVaSetValues(zoom_buttons[i], XmNset, zoom_mode == (enum imageview_zoom)i,
                      NULL);
    }
    XtVaSetValues(prev_button, XmNsensitive, sibling_count > 1U, NULL);
    XtVaSetValues(next_button, XmNsensitive, sibling_count > 1U, NULL);
    set_label(path_label, current_path[0] ? current_path : T("No file"));
}

static void rebuild_siblings(void)
{
    DIR *directory;
    struct dirent *entry;
    const char *base = path_basename(current_path);
    sibling_count = 0;
    sibling_index = 0;
    path_parent(current_dir, sizeof(current_dir), current_path);
    directory = opendir(current_dir);
    if (!directory) return;
    while (sibling_count < IMAGEVIEW_ROWS_MAX &&
           (entry = readdir(directory)) != 0) {
        if ((entry->d_type == DT_REG || entry->d_type == DT_UNKNOWN) &&
            imageview_is_supported_path(entry->d_name)) {
            copy_text(sibling_names[sibling_count],
                      sizeof(sibling_names[0]), entry->d_name);
            if (text_eq_ignore_case(entry->d_name, base)) {
                sibling_index = sibling_count;
            }
            ++sibling_count;
        }
    }
    closedir(directory);
}

static int read_file_all(const char *path, uint8_t **out_data, uint32_t *out_len)
{
    struct stat st;
    uint8_t *data;
    uint32_t len = 0;
    int fd;
    if (!out_data || !out_len || stat(path, &st) < 0 ||
        !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (uint64_t)st.st_size > 8U * 1024U * 1024U) {
        return -1;
    }
    data = (uint8_t *)malloc((size_t)st.st_size);
    if (!data) return -1;
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        free(data);
        return fd;
    }
    while (len < (uint32_t)st.st_size) {
        long got = read(fd, data + len, (uint32_t)st.st_size - len);
        if (got < 0) {
            close(fd);
            free(data);
            return (int)got;
        }
        if (got == 0) break;
        len += (uint32_t)got;
    }
    close(fd);
    if (len != (uint32_t)st.st_size) {
        free(data);
        return -1;
    }
    *out_data = data;
    *out_len = len;
    return 0;
}

static int load_image_path(const char *path)
{
    uint8_t *data = 0;
    uint32_t *decoded = 0;
    uint32_t len = 0;
    uint32_t decoded_w = 0;
    uint32_t decoded_h = 0;
    int ret;
    if (!path || !path[0] || !imageview_is_supported_path(path)) {
        set_status(T("Unsupported image format. Use BMP, DIB, or PNG."));
        return -1;
    }
    if (ends_with_ignore_case(path, ".png")) {
        ret = reliefos_png_decode_file(path, &decoded, &decoded_w, &decoded_h);
        if (ret < 0) {
            set_status(T("Could not decode PNG (maximum 1024x1024)."));
            return ret;
        }
        free_image();
        image_pixels = decoded;
        image_w = decoded_w;
        image_h = decoded_h;
    } else {
        ret = read_file_all(path, &data, &len);
        if (ret < 0) {
            set_status(T("Could not read image."));
            return ret;
        }
        ret = imageview_bmp_decode(data, len, &decoded, &decoded_w, &decoded_h);
        free(data);
        if (ret < 0) {
            set_status(T("Unsupported BMP. Use uncompressed 24/32-bit BMP."));
            return ret;
        }
        free_image();
        image_pixels = decoded;
        image_w = decoded_w;
        image_h = decoded_h;
    }
    copy_text(current_path, sizeof(current_path), path);
    rebuild_siblings();
    rebuild_detail();
    sync_toolbar();
    set_status(T("Image loaded"));
    return 0;
}

/* Nearest-neighbor scale into a software back buffer, then hand it to X11 as
 * a temporary XImage wrapper around the buffer. */
static void draw_canvas(void)
{
    Dimension width = 0, height = 0;
    uint64_t need;
    uint32_t draw_w = 0, draw_h = 0, dst_x, dst_y;
    XImage *image;
    if (!drawing_area || !gc) return;
    XtVaGetValues(drawing_area, XmNwidth, &width, XmNheight, &height, NULL);
    if (!width || !height) return;
    need = (uint64_t)width * height;
    if (need > view_cap) {
        uint32_t *next = (uint32_t *)realloc(view_pixels, (size_t)need * sizeof(uint32_t));
        if (!next) return;
        view_pixels = next;
        view_cap = need;
    }
    for (uint64_t i = 0; i < need; ++i) view_pixels[i] = 0x00ffffffU;
    if (image_pixels && image_w && image_h) {
        imageview_zoom_dims(image_w, image_h, zoom_mode,
                            width > 12 ? width - 12 : width,
                            height > 12 ? height - 12 : height,
                            &draw_w, &draw_h);
        dst_x = width > draw_w ? (width - draw_w) / 2U : 0U;
        dst_y = height > draw_h ? (height - draw_h) / 2U : 0U;
        for (uint32_t y = 0; y < draw_h && dst_y + y < height; ++y) {
            uint32_t sy = (uint64_t)y * image_h / draw_h;
            for (uint32_t x = 0; x < draw_w && dst_x + x < width; ++x) {
                uint32_t sx = (uint64_t)x * image_w / draw_w;
                view_pixels[(dst_y + y) * (uint32_t)width + dst_x + x] =
                    image_pixels[sy * image_w + sx];
            }
        }
    }
    image = XCreateImage(display, DefaultVisual(display, DefaultScreen(display)),
                         DefaultDepth(display, DefaultScreen(display)),
                         ZPixmap, 0, (char *)view_pixels, width, height, 32, 0);
    if (!image) return;
    XPutImage(display, XtWindow(drawing_area), gc, image, 0, 0, 0, 0,
              width, height);
    /* The pixels belong to the back buffer; detach before freeing the wrapper. */
    image->data = NULL;
    XDestroyImage(image);
}

static void canvas_expose(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget;
    (void)data;
    (void)call;
    draw_canvas();
}

static void canvas_resize(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget;
    (void)data;
    (void)call;
    draw_canvas();
}

static void load_sibling_delta(int delta)
{
    char next_path[PATH_MAX];
    if (sibling_count <= 1U) return;
    sibling_index = imageview_next_index(sibling_index, sibling_count, delta);
    build_child_path(next_path, sizeof(next_path), current_dir,
                     sibling_names[sibling_index]);
    (void)load_image_path(next_path);
}

/* Modal Motif dialogs run a nested event loop until a callback settles them. */
struct dialog_result {
    int done;
    int outcome;
    char path[PATH_MAX];
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

static void dialog_cancel(Widget widget, XtPointer data, XtPointer call)
{
    struct dialog_result *result = (struct dialog_result *)data;
    (void)widget;
    (void)call;
    result->outcome = 0;
    result->done = 1;
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

static int run_file_dialog(const char *title, char *path, uint32_t cap)
{
    struct dialog_result result = {0, 0, {0}};
    Widget dialog = XmCreateFileSelectionDialog(shell, "fileDialog", NULL, 0);
    XmString title_text = XmStringCreateLocalized((char *)title);
    XtVaSetValues(dialog, XmNdialogTitle, title_text, XmNautoUnmanage, True, NULL);
    XmStringFree(title_text);
    XtUnmanageChild(XmFileSelectionBoxGetChild(dialog, XmDIALOG_HELP_BUTTON));
    XtAddCallback(dialog, XmNokCallback, file_ok, &result);
    XtAddCallback(dialog, XmNcancelCallback, dialog_cancel, &result);
    XtAddCallback(dialog, XmNmapCallback, raise_dialog, XtParent(dialog));
    XtManageChild(dialog);
    while (!result.done) {
        XEvent event;
        XtAppNextEvent(app, &event);
        XtDispatchEvent(&event);
    }
    XtUnmanageChild(dialog);
    XtDestroyWidget(dialog);
    if (result.outcome > 0 && result.path[0]) {
        copy_text(path, cap, result.path);
        return 1;
    }
    return 0;
}

static void open_image_via_dialog(void)
{
    char path[PATH_MAX] = {0};
    if (run_file_dialog(T("Open image"), path, sizeof(path)) > 0) {
        (void)load_image_path(path);
        draw_canvas();
    }
}

static void button_open(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget; (void)data; (void)call;
    open_image_via_dialog();
}

static void button_previous(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget; (void)data; (void)call;
    load_sibling_delta(-1);
    draw_canvas();
}

static void button_next(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget; (void)data; (void)call;
    load_sibling_delta(1);
    draw_canvas();
}

static void select_zoom(Widget widget, XtPointer data, XtPointer call)
{
    (void)call;
    if (!XmToggleButtonGetState(widget)) {
        XmToggleButtonSetState(widget, True, False);
        return;
    }
    zoom_mode = (enum imageview_zoom)(uintptr_t)data;
    rebuild_detail();
    sync_toolbar();
    draw_canvas();
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
    if (event->xkey.state & ControlMask) {
        if (symbol == XK_o || symbol == XK_O) {
            open_image_via_dialog();
            *dispatch = False;
        }
        return;
    }
    if (symbol == XK_Left) {
        load_sibling_delta(-1);
        draw_canvas();
        *dispatch = False;
    } else if (symbol == XK_Right) {
        load_sibling_delta(1);
        draw_canvas();
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
    shell = XtVaAppInitialize(&app, "ReliefOSImageView", NULL, 0, &argc, argv,
                              fallback, XtNtitle, T("Image Viewer"),
                              XtNwidth, 840, XtNheight, 640, NULL);
    {
        XmFontList fonts = app_font_list(shell);
        XtVaSetValues(shell, XmNlabelFontList, fonts,
                      XmNbuttonFontList, fonts, XmNtextFontList, fonts, NULL);
    }
    watch_keys(shell);
    Widget form = XtVaCreateWidget("imageview", xmFormWidgetClass, shell,
        XmNmarginWidth, 8, XmNmarginHeight, 8, NULL);
    Widget bar = XtVaCreateManagedWidget("toolbar", xmFormWidgetClass, form,
        XmNtopAttachment, XmATTACH_FORM, XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM, NULL);
    Widget previous_widget;
    {
        Widget button = XtVaCreateManagedWidget("open", xmPushButtonWidgetClass, bar,
            XmNtopAttachment, XmATTACH_FORM, XmNleftAttachment, XmATTACH_FORM, NULL);
        set_label(button, T("Open"));
        XtAddCallback(button, XmNactivateCallback, button_open, NULL);
        watch_keys(button);
        previous_widget = button;
    }
    {
        struct { char *name; const char *label; XtCallbackProc callback; Widget *slot; }
        entries[2] = {
            {"previous", T("Previous"), button_previous, &prev_button},
            {"next", T("Next Image"), button_next, &next_button},
        };
        for (int i = 0; i < 2; ++i) {
            Widget button = XtVaCreateManagedWidget(entries[i].name,
                xmPushButtonWidgetClass, bar,
                XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, previous_widget,
                XmNtopAttachment, XmATTACH_FORM, NULL);
            set_label(button, entries[i].label);
            XtAddCallback(button, XmNactivateCallback, entries[i].callback, NULL);
            watch_keys(button);
            *entries[i].slot = button;
            previous_widget = button;
        }
    }
    {
        static char *zoom_names[3] = {"fit", "zoom1x", "zoom2x"};
        const char *labels[3] = {"Fit", "1x", "2x"};
        for (int i = 0; i < 3; ++i) {
            zoom_buttons[i] = XtVaCreateManagedWidget(zoom_names[i],
                xmToggleButtonWidgetClass, bar,
                XmNindicatorOn, False, XmNrecomputeSize, False, XmNwidth, 40,
                XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, previous_widget,
                XmNtopAttachment, XmATTACH_FORM, NULL);
            set_label(zoom_buttons[i], labels[i]);
            XtAddCallback(zoom_buttons[i], XmNvalueChangedCallback, select_zoom,
                          (XtPointer)(uintptr_t)i);
            watch_keys(zoom_buttons[i]);
            previous_widget = zoom_buttons[i];
        }
    }
    path_label = XtVaCreateManagedWidget("path", xmLabelWidgetClass, bar,
        XmNalignment, XmALIGNMENT_END,
        XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, previous_widget,
        XmNrightAttachment, XmATTACH_FORM,
        XmNtopAttachment, XmATTACH_FORM, NULL);
    status_label = XtVaCreateManagedWidget("status", xmLabelWidgetClass, form,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM, NULL);
    detail_label = XtVaCreateManagedWidget("detail", xmLabelWidgetClass, form,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNbottomAttachment, XmATTACH_WIDGET, XmNbottomWidget, status_label,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM, NULL);
    drawing_area = XtVaCreateManagedWidget("canvas", xmDrawingAreaWidgetClass, form,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, bar,
        XmNbottomAttachment, XmATTACH_WIDGET, XmNbottomWidget, detail_label,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM, NULL);
    XtAddCallback(drawing_area, XmNexposeCallback, canvas_expose, NULL);
    XtAddCallback(drawing_area, XmNresizeCallback, canvas_resize, NULL);
    watch_keys(drawing_area);
    copy_text(status_text, sizeof(status_text), T("Use Open to choose a BMP or PNG image."));
    copy_text(detail_text, sizeof(detail_text), T("No image loaded."));
    set_label(status_label, status_text);
    set_label(detail_label, detail_text);
    if (argc > 1 && argv && argv[1] && argv[1][0]) {
        (void)load_image_path(argv[1]);
    }
    sync_toolbar();
    XtManageChild(form);
    XtRealizeWidget(shell);
    display = XtDisplay(shell);
    gc = XCreateGC(display, XtWindow(drawing_area), 0, NULL);
    Atom delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(shell, delete_window, close_window, NULL);
    puts("[imageview.elf] Motif image viewer ready");
    fflush(stdout);
    XtAppMainLoop(app);
    free_image();
    free(view_pixels);
    view_pixels = 0;
    view_cap = 0;
    XtDestroyWidget(shell);
    XtDestroyApplicationContext(app);
    return 0;
}
