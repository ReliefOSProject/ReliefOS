/* Guest-only X11 observation/clipboard helper. It never opens a block device. */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static Window found;
static XWindowAttributes largest;

/** @brief Observe the largest viewable Diskmgr client using its standard WM_CLASS. */
static void find_window(Display *display, Window parent, unsigned depth)
{
    if (depth > 4) return;
    Window root, owner, *children = NULL;
    unsigned count = 0;
    if (!XQueryTree(display, parent, &root, &owner, &children, &count)) return;
    for (unsigned i = 0; i < count; ++i) {
        XClassHint hint = {0};
        XWindowAttributes attributes;
        if (XGetClassHint(display, children[i], &hint)) {
            int matches = (hint.res_class && !strcmp(hint.res_class, "reliefos-diskmgr")) ||
                          (hint.res_name && !strcmp(hint.res_name, "reliefos-diskmgr"));
            if (matches && XGetWindowAttributes(display, children[i], &attributes) &&
                attributes.map_state == IsViewable && attributes.width * attributes.height > largest.width * largest.height) {
                found = children[i]; largest = attributes;
            }
            if (hint.res_class) XFree(hint.res_class);
            if (hint.res_name) XFree(hint.res_name);
        }
        find_window(display, children[i], depth + 1);
    }
    if (children) XFree(children);
}

/** @brief Supply real UTF-8 clipboard data through ICCCM selection requests for two minutes. */
static int clipboard(Display *display)
{
    const char text[] = "中文资料盘";
    Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 1, 1, 0, 0, 0);
    Atom selection = XInternAtom(display, "CLIPBOARD", False);
    Atom utf8 = XInternAtom(display, "UTF8_STRING", False);
    Atom targets = XInternAtom(display, "TARGETS", False);
    XSetSelectionOwner(display, selection, window, CurrentTime);
    XSync(display, False);
    if (XGetSelectionOwner(display, selection) != window) return 1;
    puts("DM_CLIPBOARD_READY"); fflush(stdout);
    struct timespec start, now, delay = {0, 20000000};
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        while (XPending(display)) {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == SelectionClear) return 0;
            if (event.type != SelectionRequest) continue;
            XSelectionRequestEvent *request = &event.xselectionrequest;
            Atom property = request->property == None ? request->target : request->property;
            XEvent response = {0};
            response.xselection.type = SelectionNotify;
            response.xselection.display = display;
            response.xselection.requestor = request->requestor;
            response.xselection.selection = request->selection;
            response.xselection.target = request->target;
            response.xselection.time = request->time;
            response.xselection.property = property;
            if (request->target == targets) {
                Atom supported[] = {targets, utf8};
                XChangeProperty(display, request->requestor, property, XA_ATOM, 32, PropModeReplace,
                                (const unsigned char *)supported, 2);
            } else if (request->target == utf8) {
                XChangeProperty(display, request->requestor, property, utf8, 8, PropModeReplace,
                                (const unsigned char *)text, (int)strlen(text));
            } else response.xselection.property = None;
            XSendEvent(display, request->requestor, False, 0, &response);
            XFlush(display);
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - start.tv_sec > 120) break;
        nanosleep(&delay, NULL);
    }
    XDestroyWindow(display, window);
    return 0;
}

/** @brief Print observable client geometry or own a test clipboard, using only normal X11 APIs. */
int main(int argc, char **argv)
{
    Display *display = XOpenDisplay(NULL);
    if (!display) return 1;
    if (argc == 2 && !strcmp(argv[1], "--clipboard")) return clipboard(display);
    find_window(display, DefaultRootWindow(display), 0);
    if (!found) return 2;
    if (argc == 4 && !strcmp(argv[1], "--resize")) {
        int width = atoi(argv[2]), height = atoi(argv[3]);
        if (width < 920 || width > 1920 || height < 600 || height > 1080) return 3;
        XResizeWindow(display, found, (unsigned)width, (unsigned)height);
        XSync(display, False);
    }
    int x, y; Window child;
    XTranslateCoordinates(display, found, DefaultRootWindow(display), 0, 0, &x, &y, &child);
    printf("DM_GEOMETRY %lu %d %d %d %d\n", found, x, y, largest.width, largest.height);
    XCloseDisplay(display);
    return 0;
}
