#include "model.h"
#include <reliefos/fs.h>
#include <reliefos/layout.h>
#include <reliefos/png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/keysym.h>
#include <Xm/DrawingA.h>
#include <Xm/Form.h>
#include <Xm/Protocols.h>

#define CANVAS_W 760
#define CANVAS_H 760
#define IMAGE_MARGIN 8
#define APP_NAME "xiaobai"
#define APP_PNG APP_NAME ".png"

static uint32_t *image_pixels;
static uint32_t image_width;
static uint32_t image_height;
static Widget canvas;
static XtAppContext app;
static Display *display;
static Window canvas_window;
static GC gc;

static void copy_text(char *dst, uint32_t capacity, const char *src)
{
    uint32_t index = 0;
    if (!dst || capacity == 0) {
        return;
    }
    while (src && src[index] && index + 1U < capacity) {
        dst[index] = src[index];
        ++index;
    }
    dst[index] = 0;
}

static int load_image(const char *argv0)
{
    char path[RELIEFOS_FS_PATH_LEN];

    snprintf(path, sizeof(path), RELIEFOS_LAYOUT_RELIEFOS_APPS "/%s/" APP_PNG, APP_NAME);
    if (reliefos_png_decode_file(path, &image_pixels, &image_width, &image_height) == 0) {
        printf("[xiaobai.elf] PNG open path=%s size=%dx%d\n",
               path, (int)image_width, (int)image_height);
        return 0;
    }
    if (argv0 && argv0[0]) {
        uint32_t length = 0;
        uint32_t last_separator = 0;
        while (argv0[length]) {
            if (argv0[length] == '/') {
                last_separator = length;
            }
            ++length;
        }
        if (last_separator && last_separator + sizeof(APP_PNG) < sizeof(path)) {
            copy_text(path, sizeof(path), argv0);
            path[last_separator + 1U] = 0;
            copy_text(path + last_separator + 1U,
                      sizeof(path) - last_separator - 1U, APP_PNG);
            if (reliefos_png_decode_file(path, &image_pixels, &image_width, &image_height) == 0) {
                printf("[xiaobai.elf] PNG open path=%s size=%dx%d\n",
                       path, (int)image_width, (int)image_height);
                return 0;
            }
        }
    }
    if (reliefos_png_decode_file(APP_PNG, &image_pixels, &image_width, &image_height) == 0) {
        printf("[xiaobai.elf] PNG open path=%s size=%dx%d\n",
               APP_PNG, (int)image_width, (int)image_height);
        return 0;
    }
    puts("[xiaobai.elf] PNG open failed path=" APP_PNG);
    return -1;
}

static void draw_image(void)
{
    Dimension width = CANVAS_W;
    Dimension height = CANVAS_H;
    struct xiaobai_fit fit;
    uint32_t *scaled;
    XImage *image;

    if (!display || !canvas_window || !gc) {
        return;
    }
    XtVaGetValues(canvas, XmNwidth, &width, XmNheight, &height, NULL);
    XSetForeground(display, gc, BlackPixel(display, DefaultScreen(display)));
    XFillRectangle(display, canvas_window, gc, 0, 0, width, height);
    if (!image_pixels || !image_width || !image_height) {
        XSetForeground(display, gc, WhitePixel(display, DefaultScreen(display)));
        XDrawString(display, canvas_window, gc, 24, (int)height / 2 - 16,
                    "Could not decode " APP_PNG, (int)strlen("Could not decode " APP_PNG));
        return;
    }
    xiaobai_fit_rect(&fit, width, height, IMAGE_MARGIN, image_width, image_height);
    if (!fit.w || !fit.h) {
        return;
    }
    scaled = (uint32_t *)malloc((size_t)fit.w * fit.h * sizeof(uint32_t));
    if (!scaled) {
        return;
    }
    for (uint32_t y = 0; y < fit.h; ++y) {
        uint32_t source_y = (uint32_t)(((uint64_t)y * image_height) / fit.h);
        for (uint32_t x = 0; x < fit.w; ++x) {
            uint32_t source_x = (uint32_t)(((uint64_t)x * image_width) / fit.w);
            scaled[y * fit.w + x] = image_pixels[source_y * image_width + source_x];
        }
    }
    image = XCreateImage(display, DefaultVisual(display, DefaultScreen(display)),
                         DefaultDepth(display, DefaultScreen(display)),
                         ZPixmap, 0, (char *)scaled, fit.w, fit.h, 32, fit.w * 4);
    if (!image) {
        free(scaled);
        return;
    }
    XPutImage(display, canvas_window, gc, image, 0, 0, (int)fit.x, (int)fit.y, fit.w, fit.h);
    image->data = NULL;
    XDestroyImage(image);
    free(scaled);
}

static void canvas_expose(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget;
    (void)data;
    (void)call;
    draw_image();
}

static void canvas_resize(Widget widget, XtPointer data, XtPointer call)
{
    Window window = XtWindow(widget);
    (void)data;
    (void)call;
    /* The resize callback can fire while the shell is still realizing,
     * before the globals below exist; use widget-local state only. */
    if (window) {
        XClearArea(XtDisplay(widget), window, 0, 0, 0, 0, True);
    }
}

static void canvas_key(Widget widget, XtPointer data, XEvent *event, Boolean *dispatch)
{
    (void)widget;
    (void)data;
    if (event->type != KeyPress) {
        return;
    }
    if (XLookupKeysym(&event->xkey, 0) == XK_Escape) {
        XtAppSetExitFlag(app);
        *dispatch = False;
    }
}

static void close_window(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget;
    (void)data;
    (void)call;
    XtAppSetExitFlag(app);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    puts("[xiaobai.elf] step enter");
    char *fallback[] = {
        "*background: #000000", "*foreground: #ffffff", NULL
    };
    Widget shell = XtVaAppInitialize(&app, "ReliefOSXiaobai", NULL, 0,
                                     &argc, argv, fallback,
                                     XtNtitle, "Xiaobai",
                                     XtNwidth, CANVAS_W, XtNheight, CANVAS_H, NULL);
    puts("[xiaobai.elf] step initialize ok");
    Widget form = XtVaCreateWidget("xiaobai", xmFormWidgetClass, shell, NULL);
    canvas = XtVaCreateManagedWidget("canvas", xmDrawingAreaWidgetClass, form,
        XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_FORM, NULL);
    XtAddCallback(canvas, XmNexposeCallback, canvas_expose, NULL);
    XtAddCallback(canvas, XmNresizeCallback, canvas_resize, NULL);
    XtInsertEventHandler(canvas, KeyPressMask, False, canvas_key, NULL, XtListHead);
    XtManageChild(form);
    XtRealizeWidget(shell);
    puts("[xiaobai.elf] step realize ok");

    display = XtDisplay(shell);
    canvas_window = XtWindow(canvas);
    gc = XCreateGC(display, canvas_window, 0, NULL);
    puts("[xiaobai.elf] step gc ok");
    load_image(argc > 0 ? argv[0] : 0);
    puts("[xiaobai.elf] step image ok");
    Atom delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(shell, delete_window, close_window, NULL);
    draw_image();
    puts("[xiaobai.elf] step draw ok");
    puts("[xiaobai.elf] Motif xiaobai ready");
    fflush(stdout);
    XtAppMainLoop(app);
    XtDestroyWidget(shell);
    XtDestroyApplicationContext(app);
    reliefos_png_free(image_pixels);
    return 0;
}
