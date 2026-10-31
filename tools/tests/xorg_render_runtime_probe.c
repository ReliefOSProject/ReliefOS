/* SPDX-License-Identifier: Apache-2.0 */
/* Run inside the guest on an isolated X server; timing is diagnostic, while
 * X errors, incorrect pixels, failed allocation and process failures fail. */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static unsigned errors;

static int xerror(Display *display, XErrorEvent *event)
{
    char message[256];
    XGetErrorText(display, event->error_code, message, sizeof(message));
    fprintf(stderr, "[xorg-probe] X error=%s request=%u minor=%u resource=%lu\n",
            message, event->request_code, event->minor_code, event->resourceid);
    ++errors;
    return 0;
}

static uint64_t clock_us(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value)) exit(2);
    return (uint64_t)value.tv_sec * 1000000u + (uint64_t)value.tv_nsec / 1000u;
}

static void result(const char *name, uint64_t started, unsigned count)
{
    uint64_t elapsed = clock_us() - started;
    printf("[xorg-probe] case=%s count=%u elapsed_us=%llu per_op_us=%llu\n",
           name, count, (unsigned long long)elapsed,
           (unsigned long long)(elapsed / count));
}

static int pixel_matches(Display *display, Drawable drawable, int x, int y,
                         unsigned long expected, const char *stage)
{
    XImage *image = XGetImage(display, drawable, x, y, 1, 1, AllPlanes, ZPixmap);
    unsigned long actual = image ? XGetPixel(image, 0, 0) & 0xffffffu : ~0ul;
    if (image) XDestroyImage(image);
    if (actual == expected) return 1;
    fprintf(stderr, "[xorg-probe] stage=%s pixel=%d,%d expected=%lx actual=%lx\n",
            stage, x, y, expected, actual);
    return 0;
}

static Window wm_window_property(Display *display, Window window, Atom property)
{
    Atom type = None;
    int format = 0;
    unsigned long count = 0, remaining = 0;
    unsigned char *value = NULL;
    Window result = None;
    if (XGetWindowProperty(display, window, property,
        0, 1, False, XA_WINDOW, &type, &format, &count, &remaining, &value) ==
        Success && type == XA_WINDOW && format == 32 && count == 1 && value)
        result = *(const unsigned long *)value;
    if (value) XFree(value);
    return result;
}

static int wm_query_error(Display *display, XErrorEvent *event)
{
    /* The WM may have exited since publishing the root property. */
    return event->error_code == BadWindow ? 0 : xerror(display, event);
}

static int wm_ready(Display *display)
{
    Atom property = XInternAtom(display, "_NET_SUPPORTING_WM_CHECK", True);
    if (property == None) return 0;
    XErrorHandler previous = XSetErrorHandler(wm_query_error);
    Window root = DefaultRootWindow(display);
    Window window = wm_window_property(display, root, property);
    int ready = window != None && window != root &&
                wm_window_property(display, window, property) == window;
    XSetErrorHandler(previous);
    return ready;
}

static int render_round(Display *display, int offscreen)
{
    int status = 1;
    int screen = DefaultScreen(display);
    unsigned depth = (unsigned)DefaultDepth(display, screen);
    if (depth != 24 && depth != 32) {
        fprintf(stderr, "[xorg-probe] unsupported depth=%u\n", depth);
        return 2;
    }
    Window window = XCreateSimpleWindow(display, RootWindow(display, screen),
        20, 20, 960, 600, 0, 0, 0);
    XSetWindowAttributes attributes = {.override_redirect = True};
    XChangeWindowAttributes(display, window, CWOverrideRedirect, &attributes);
    XStoreName(display, window, "ReliefOS Xorg render regression");
    XMapRaised(display, window);
    /* XGetImage(window) is undefined for obscured regions without backing
     * store. Concurrent desktop stress uses an owned pixmap for assertions. */
    Pixmap backing = offscreen ? XCreatePixmap(display, window, 960, 600, depth) : None;
    Drawable target = offscreen ? backing : window;
    GC gc = XCreateGC(display, target, 0, NULL);
    if (!gc) goto close;
    XSetForeground(display, gc, 0x00d04a32u);
    XFillRectangle(display, target, gc, 0, 0, 960, 600);
    if (offscreen) XCopyArea(display, target, window, gc, 0, 0, 960, 600, 0, 0);
    XSync(display, False);
    if (!pixel_matches(display, target, 40, 40, 0xd04a32u, "initial-fill") ||
        !pixel_matches(display, target, 959, 599, 0xd04a32u, "initial-edge"))
        goto close;

    uint64_t started = clock_us();
    for (unsigned i = 0; i < 1000; ++i) {
        XSetForeground(display, gc, 0x00204080u + i);
        XFillRectangle(display, target, gc, i % 700, i % 400, 240, 160);
        XSync(display, False);
    }
    result("fill-sync", started, 1000);
    if (!pixel_matches(display, target, 300, 200, 0x204080u + 999u, "fill") ||
        !pixel_matches(display, target, 959, 599, 0xd04a32u, "fill-edge"))
        goto close;

    /* A normal application batches X requests. Include a readback in this
     * timing so queued GPU work has completed before reporting throughput. */
    started = clock_us();
    for (unsigned i = 0; i < 4096; ++i) {
        XSetForeground(display, gc, 0x00103050u + i);
        XFillRectangle(display, target, gc, i % 700, i % 400, 240, 160);
    }
    if (!pixel_matches(display, target, 600, 100, 0x103050u + 4095u, "batch-fill"))
        goto close;
    result("fill-batch", started, 4096);

    XSetForeground(display, gc, 0x00102030u);
    XFillRectangle(display, target, gc, 0, 0, 960, 600);
    XSetForeground(display, gc, 0x00804020u);
    XFillRectangle(display, target, gc, 0, 0, 32, 600);
    XSync(display, False);
    started = clock_us();
    for (unsigned i = 0; i < 500; ++i) {
        XCopyArea(display, target, target, gc, 0, 0, 900, 550, 4, 4);
        XSync(display, False);
    }
    result("scroll-sync", started, 500);
    if (!pixel_matches(display, target, 400, 400, 0x804020u, "scroll-overlap") ||
        !pixel_matches(display, target, 800, 400, 0x102030u, "scroll-background"))
        goto close;

    started = clock_us();
    for (unsigned i = 0; i < 500; ++i) {
        XMoveWindow(display, window, 20 + i % 400, 20 + i % 200);
        XSync(display, False);
    }
    result("move-sync", started, 500);
    Window child = None;
    int root_x = 0, root_y = 0;
    if (!XTranslateCoordinates(display, window, RootWindow(display, screen),
                                0, 0, &root_x, &root_y, &child) ||
        root_x != 119 || root_y != 119 ||
        !pixel_matches(display, target, 400, 400, 0x804020u, "move"))
        goto close;

    started = clock_us();
    for (unsigned i = 0; i < 256; ++i) {
        Pixmap pixmap = XCreatePixmap(display, window, 1024, 768, depth);
        XSetForeground(display, gc, 0x00406080u + i);
        XFillRectangle(display, pixmap, gc, 0, 0, 1024, 768);
        XCopyArea(display, pixmap, target, gc, 0, 0, 900, 550, 0, 0);
        XFreePixmap(display, pixmap);
        XSync(display, False);
        if (i % 32 == 31 &&
            !pixel_matches(display, target, 899, 549, 0x406080u + i, "pixmap-copy"))
            goto close;
    }
    result("pixmap-churn", started, 256);
    XSetForeground(display, gc, 0x005ba342u);
    XFillRectangle(display, target, gc, 0, 0, 960, 600);
    if (offscreen) XCopyArea(display, target, window, gc, 0, 0, 960, 600, 0, 0);
    XSync(display, False);
    if (!pixel_matches(display, target, 40, 40, 0x5ba342u, "final-fill"))
        goto close;
    status = errors ? 1 : 0;
close:
    if (gc) XFreeGC(display, gc);
    if (backing) XFreePixmap(display, backing);
    XDestroyWindow(display, window);
    XSync(display, False);
    return errors ? 1 : status;
}

int main(int argc, char **argv)
{
    const char *program = argv[0];
    int offscreen = argc > 1 && !strcmp(argv[1], "--offscreen");
    if (offscreen) { --argc; ++argv; }
    int wait_only = argc == 2 && !strcmp(argv[1], "--wait");
    int wait_for_wm = argc == 2 && !strcmp(argv[1], "--wait-wm");
    if (offscreen && (wait_only || wait_for_wm)) return 2;
    unsigned long rounds = 1;
    if (argc == 3 && !strcmp(argv[1], "--repeat")) {
        char *end;
        errno = 0;
        rounds = strtoul(argv[2], &end, 10);
        if (errno || !*argv[2] || *end || !rounds || rounds > 1000) return 2;
    } else if (argc != 1 && !wait_only && !wait_for_wm) {
        fprintf(stderr, "usage: %s [--offscreen] [--wait|--wait-wm|--repeat 1..1000]\n", program);
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    pid_t parent = getpid();
    pid_t heartbeat = wait_only || wait_for_wm ? -1 : fork();
    if (!wait_only && !wait_for_wm && heartbeat < 0) return 2;
    if (!heartbeat) {
        while (getppid() == parent) {
            printf("[xorg-probe] heartbeat_us=%llu\n", (unsigned long long)clock_us());
            struct timespec delay = {2, 0};
            while (nanosleep(&delay, &delay) && errno == EINTR) {}
        }
        _exit(0);
    }
    XSetErrorHandler(xerror);
    Display *display = NULL;
    for (unsigned retry = 0; retry < 300 && !display; ++retry) {
        display = XOpenDisplay(NULL);
        if (!display) {
            struct timespec delay = {0, 100000000};
            nanosleep(&delay, NULL);
        }
    }
    int status = 1;
    if (!display) {
        fprintf(stderr, "[xorg-probe] cannot connect to X server\n");
        goto done;
    }
    if (wait_only) status = 0;
    else if (wait_for_wm) {
        for (unsigned retry = 0; retry < 300; ++retry) {
            if (wm_ready(display)) {
                printf("[xorg-probe] wm_ready=1\n");
                status = 0;
                break;
            }
            struct timespec delay = {0, 100000000};
            nanosleep(&delay, NULL);
        }
        if (status) fprintf(stderr, "[xorg-probe] window manager not ready\n");
    } else for (unsigned long round = 0; round < rounds; ++round) {
        printf("[xorg-probe] round=%lu/%lu\n", round + 1, rounds);
        status = render_round(display, offscreen);
        if (status) break;
    }
    XCloseDisplay(display);
done:
    if (heartbeat > 0) {
        kill(heartbeat, SIGTERM);
        while (waitpid(heartbeat, NULL, 0) < 0 && errno == EINTR) {}
    }
    if (errors) status = 1;
    printf("[xorg-probe] result=%d errors=%u\n", status, errors);
    return status;
}
