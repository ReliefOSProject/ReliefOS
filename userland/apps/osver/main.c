#include "debug_click.h"
#include <generated/build_info.h>
#include <libintl.h>
#include <locale.h>
#include <reliefos/layout.h>
#include <reliefos/png.h>
#include <reliefos/system.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/MessageB.h>
#include <Xm/Protocols.h>
#include <Xm/PushB.h>
#include <Xm/Separator.h>
#include <Xm/TextF.h>
#include <Xm/Text.h>

#define T(s) gettext(s)
#define LOGO_SIZE 180U

static XtAppContext app;
static Widget status_label, shell, logo;
static struct osver_debug_click clicks;
static Pixmap logo_pixmap;
static unsigned logo_x, logo_y, logo_w, logo_h;

static void set_label(Widget widget, const char *text)
{
    XmString value = XmStringCreateLocalized((char *)text);
    XtVaSetValues(widget, XmNlabelString, value, NULL);
    XmStringFree(value);
}

static unsigned long color_component(unsigned value, unsigned long mask)
{
    unsigned shift = 0;
    if (!mask) return 0;
    while (!(mask & 1UL)) {
        mask >>= 1;
        ++shift;
    }
    return ((value * mask + 127UL) / 255UL) << shift;
}

static Pixmap load_logo(Display *display)
{
    uint32_t *pixels = NULL, width = 0, height = 0;
    if (reliefos_png_decode_file(RELIEFOS_PATH_LOGO_PNG, &pixels, &width, &height) < 0 ||
        !pixels || !width || !height) {
        reliefos_png_free(pixels);
        return None;
    }
    logo_w = LOGO_SIZE;
    logo_h = (unsigned)((uint64_t)LOGO_SIZE * height / width);
    if (logo_h > LOGO_SIZE) {
        logo_h = LOGO_SIZE;
        logo_w = (unsigned)((uint64_t)LOGO_SIZE * width / height);
    }
    if (!logo_w || !logo_h) {
        reliefos_png_free(pixels);
        return None;
    }
    logo_x = (LOGO_SIZE - logo_w) / 2;
    logo_y = (LOGO_SIZE - logo_h) / 2;
    int screen = DefaultScreen(display);
    Visual *visual = DefaultVisual(display, screen);
    if (visual->class != TrueColor) {
        reliefos_png_free(pixels);
        return None;
    }
    XImage *image = XCreateImage(display, visual, DefaultDepth(display, screen),
                                ZPixmap, 0, NULL, LOGO_SIZE, LOGO_SIZE, 32, 0);
    if (!image) {
        reliefos_png_free(pixels);
        return None;
    }
    image->data = calloc(LOGO_SIZE, image->bytes_per_line);
    if (!image->data) {
        XDestroyImage(image);
        reliefos_png_free(pixels);
        return None;
    }
    for (unsigned y = 0; y < LOGO_SIZE; ++y) {
        for (unsigned x = 0; x < LOGO_SIZE; ++x) {
            uint32_t rgb = 0xffffff;
            if (x >= logo_x && x < logo_x + logo_w && y >= logo_y && y < logo_y + logo_h) {
                unsigned sx = (unsigned)((uint64_t)(x - logo_x) * width / logo_w);
                unsigned sy = (unsigned)((uint64_t)(y - logo_y) * height / logo_h);
                rgb = pixels[sy * width + sx];
            }
            unsigned long pixel = color_component((rgb >> 16) & 255, visual->red_mask) |
                                  color_component((rgb >> 8) & 255, visual->green_mask) |
                                  color_component(rgb & 255, visual->blue_mask);
            XPutPixel(image, x, y, pixel);
        }
    }
    reliefos_png_free(pixels);
    Pixmap pixmap = XCreatePixmap(display, RootWindow(display, screen), LOGO_SIZE,
                                  LOGO_SIZE, DefaultDepth(display, screen));
    GC gc = XCreateGC(display, pixmap, 0, NULL);
    XPutImage(display, pixmap, gc, image, 0, 0, 0, 0, LOGO_SIZE, LOGO_SIZE);
    XFreeGC(display, gc);
    XDestroyImage(image);
    return pixmap;
}

static void enable_debug(void)
{
    uint32_t flags = 0;
    if (reliefos_kernel_debug_get_state(&flags) == 0 &&
        (flags & RELIEFOS_KERNEL_DEBUG_STATE_ENABLED)) return;
    int success = reliefos_kernel_debug_set_enabled(1) == 0;
    if (success) set_label(status_label, T("Kernel debug mode enabled"));
    Widget dialog = XmCreateInformationDialog(shell, "kernelDebug", NULL, 0);
    set_label(XmMessageBoxGetChild(dialog, XmDIALOG_OK_BUTTON), T("OK"));
    XmString message = XmStringCreateLocalized(success ?
        T("Kernel debug mode enabled. Reboot to enter it.") :
        T("Could not persist kernel debug mode."));
    XmString title = XmStringCreateLocalized(T("Kernel debug mode"));
    XtVaSetValues(dialog, XmNmessageString, message, XmNdialogTitle, title, NULL);
    XmStringFree(message);
    XmStringFree(title);
    XtUnmanageChild(XmMessageBoxGetChild(dialog, XmDIALOG_CANCEL_BUTTON));
    XtUnmanageChild(XmMessageBoxGetChild(dialog, XmDIALOG_HELP_BUTTON));
    XtManageChild(dialog);
}

static void logo_click(Widget widget, XtPointer data, XEvent *event, Boolean *dispatch)
{
    (void)data;
    (void)dispatch;
    if (event->type != ButtonPress || event->xbutton.button != Button1) return;
    Dimension width, height;
    XtVaGetValues(widget, XmNwidth, &width, XmNheight, &height, NULL);
    int x = event->xbutton.x - ((int)width - (int)LOGO_SIZE) / 2;
    int y = event->xbutton.y - ((int)height - (int)LOGO_SIZE) / 2;
    int inside = logo_pixmap && x >= (int)logo_x && y >= (int)logo_y &&
        x < (int)(logo_x + logo_w) && y < (int)(logo_y + logo_h);
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return;
    uint32_t milliseconds = (uint32_t)((uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
    if (osver_debug_click(&clicks, milliseconds, inside)) enable_debug();
}

static void close_window(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget;
    (void)data;
    (void)call;
    XtAppSetExitFlag(app);
}

static Widget label(Widget parent, const char *name, const char *text,
                    Widget above, int height)
{
    XmString value = XmStringCreateLocalized((char *)text);
    Widget widget = XtVaCreateManagedWidget(name, xmLabelWidgetClass, parent,
        XmNlabelString, value, XmNalignment, XmALIGNMENT_BEGINNING,
        XmNheight, height, XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNtopAttachment, above ? XmATTACH_WIDGET : XmATTACH_FORM,
        XmNtopWidget, above, NULL);
    XmStringFree(value);
    return widget;
}

/* The XmNfontList string resource can only describe core fonts, which have no
 * CJK glyphs; route widgets to a CJK-capable Xft rendition instead. */
static XmFontList app_font_list(Widget shell)
{
    Arg args[3];
    XmRendition rendition;
    XtSetArg(args[0], XmNfontName, "SimSun");
    XtSetArg(args[1], XmNfontType, XmFONT_IS_XFT);
    XtSetArg(args[2], XmNloadModel, XmLOAD_IMMEDIATE);
    rendition = XmRenditionCreate(shell, XmFONTLIST_DEFAULT_TAG, args, 3);
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
        "*TextField.background: white", "*highlightColor: #3b62a6", NULL
    };
    shell = XtVaAppInitialize(&app, "ReliefOSSystemInformation", NULL, 0,
        &argc, argv, fallback, XtNtitle, T("About ReliefOS"),
        XtNwidth, 760, XtNheight, 380, NULL);
    {
        XmFontList fonts = app_font_list(shell);
        XtVaSetValues(shell, XmNlabelFontList, fonts,
                      XmNbuttonFontList, fonts, XmNtextFontList, fonts, NULL);
    }
    struct reliefos_system_info info = {0};
    const char *status = T("System version information");
    if (reliefos_system_info(&info) < 0) {
        snprintf(info.kernel_name, sizeof(info.kernel_name), "%s", "unknown");
        snprintf(info.kernel_version, sizeof(info.kernel_version), "%s", "unknown");
        snprintf(info.build_time, sizeof(info.build_time), "%s", "unknown");
        status = T("Could not read system version information");
    }
    if (!info.build_time[0]) {
        struct utsname uts;
        if (uname(&uts) == 0) {
            snprintf(info.build_time, sizeof(info.build_time), "%.*s",
                     (int)sizeof(info.build_time) - 1, uts.version);
        }
        if (!info.build_time[0]) snprintf(info.build_time, sizeof(info.build_time), "%s", T("Unavailable"));
    }
    if (!info.copyright[0]) {
        snprintf(info.copyright, sizeof(info.copyright), "%s", RELIEFOS_COPYRIGHT);
    }
    info.kernel_name[sizeof(info.kernel_name) - 1] = 0;
    info.kernel_version[sizeof(info.kernel_version) - 1] = 0;
    info.build_time[sizeof(info.build_time) - 1] = 0;
    info.copyright[sizeof(info.copyright) - 1] = 0;
    uint32_t flags = 0;
    if (reliefos_kernel_debug_get_state(&flags) == 0 &&
        (flags & RELIEFOS_KERNEL_DEBUG_STATE_ENABLED)) status = T("Kernel debug mode enabled");
    Widget form = XtVaCreateWidget("about", xmFormWidgetClass, shell,
        XmNwidth, 760, XmNheight, 380, XmNresizePolicy, XmRESIZE_NONE,
        XmNmarginWidth, 16, XmNmarginHeight, 12, NULL);
    Widget heading = label(form, "heading", "ReliefOS", NULL, 30);
    Widget subtitle = label(form, "subtitle", T("About this operating system"), heading, 24);
    Widget separator = XtVaCreateManagedWidget("separator", xmSeparatorWidgetClass, form,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, subtitle,
        XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_FORM, NULL);
    Widget close = XtVaCreateManagedWidget("close", xmPushButtonWidgetClass, form,
        XmNbottomAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_FORM,
        XmNwidth, 90, XmNheight, 32, XmNrecomputeSize, False, NULL);
    set_label(close, T("Close"));
    XtAddCallback(close, XmNactivateCallback, close_window, NULL);
    status_label = XtVaCreateManagedWidget("status", xmLabelWidgetClass, form,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNbottomAttachment, XmATTACH_FORM, XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_WIDGET, XmNrightWidget, close, XmNheight, 32, NULL);
    set_label(status_label, status);
    logo_pixmap = load_logo(XtDisplay(shell));
    logo = XtVaCreateManagedWidget("logo", xmLabelWidgetClass, form,
        XmNwidth, LOGO_SIZE, XmNheight, LOGO_SIZE,
        XmNmarginWidth, 0, XmNmarginHeight, 0, XmNhighlightThickness, 0,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, separator, XmNtopOffset, 20,
        XmNleftAttachment, XmATTACH_FORM, NULL);
    if (logo_pixmap) XtVaSetValues(logo, XmNlabelType, XmPIXMAP, XmNlabelPixmap, logo_pixmap, NULL);
    else set_label(logo, T("Logo unavailable"));
    Widget details = XtVaCreateManagedWidget("details", xmFormWidgetClass, form,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, separator, XmNtopOffset, 16,
        XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, logo, XmNleftOffset, 20,
        XmNrightAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_WIDGET, XmNbottomWidget, close, XmNbottomOffset, 16, NULL);
    const char *names[] = {T("Kernel"), T("Kernel version"), T("Build time"), T("Copyright")};
    const char *values[] = {info.kernel_name, info.kernel_version, info.build_time, info.copyright};
    Widget previous = NULL;
    for (unsigned i = 0; i < 4; ++i) {
        Widget row = XtVaCreateWidget("row", xmFormWidgetClass, details,
            XmNheight, i == 3 ? 52 : 36, XmNresizePolicy, XmRESIZE_NONE,
            XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_FORM,
            XmNtopAttachment, previous ? XmATTACH_WIDGET : XmATTACH_FORM,
            XmNtopWidget, previous, XmNtopOffset, 4, NULL);
        Widget name = XtVaCreateManagedWidget("name", xmLabelWidgetClass, row,
            XmNalignment, XmALIGNMENT_BEGINNING, XmNleftAttachment, XmATTACH_FORM,
            XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
            XmNrightAttachment, XmATTACH_POSITION, XmNrightPosition, 27, NULL);
        set_label(name, names[i]);
        Widget value = XtVaCreateManagedWidget("value",
            i == 3 ? xmTextWidgetClass : xmTextFieldWidgetClass, row,
            XmNvalue, values[i], XmNeditable, False, XmNcursorPositionVisible, False,
            XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_POSITION, XmNleftPosition, 28,
            XmNrightAttachment, XmATTACH_FORM, NULL);
        if (i == 3) XtVaSetValues(value, XmNeditMode, XmMULTI_LINE_EDIT, XmNwordWrap, True, NULL);
        XtManageChild(row);
        previous = row;
    }
    label(details, "note", T("Build and runtime components"), previous, 30);
    XtAddEventHandler(logo, ButtonPressMask, False, logo_click, NULL);
    XtManageChild(form);
    XtRealizeWidget(shell);
    Atom delete_window = XInternAtom(XtDisplay(shell), "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(shell, delete_window, close_window, NULL);
    puts("[osver.elf] Motif system information ready");
    printf("[osver.elf] kernel=%s version=%s build=%s\n", info.kernel_name, info.kernel_version, info.build_time);
    fflush(stdout);
    while (!XtAppGetExitFlag(app)) {
        XEvent event;
        XtAppNextEvent(app, &event);
        if (event.type == ButtonPress && event.xbutton.button == Button1 &&
            event.xbutton.window != XtWindow(logo)) {
            (void)osver_debug_click(&clicks, 0, 0);
        }
        if (event.type == KeyPress && XLookupKeysym(&event.xkey, 0) == XK_Escape) XtAppSetExitFlag(app);
        XtDispatchEvent(&event);
    }
    if (logo_pixmap) XFreePixmap(XtDisplay(shell), logo_pixmap);
    XtDestroyWidget(shell);
    XtDestroyApplicationContext(app);
    return 0;
}
