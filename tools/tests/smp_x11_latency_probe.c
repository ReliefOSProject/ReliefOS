/* SPDX-License-Identifier: Apache-2.0 */
/* Paced client repaint plus XSync acknowledgement; not physical input latency. */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static unsigned xerrors;
static int on_error(Display *display, XErrorEvent *event)
{
    (void)display;
    fprintf(stderr, "x11-latency: X error=%u request=%u\n", event->error_code, event->request_code);
    ++xerrors;
    return 0;
}

static uint64_t now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) { perror("clock_gettime"); exit(1); }
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static Window wm_property(Display *d, Window w, Atom atom)
{
    Atom type; int format; unsigned long count, left; unsigned char *value = NULL;
    Window result = None;
    if (XGetWindowProperty(d, w, atom, 0, 1, False, XA_WINDOW, &type, &format,
                           &count, &left, &value) == Success && type == XA_WINDOW &&
        format == 32 && count == 1 && value) result = *(unsigned long *)value;
    if (value) XFree(value);
    return result;
}

static int wm_ready(Display *d)
{
    Atom atom = XInternAtom(d, "_NET_SUPPORTING_WM_CHECK", True);
    if (atom == None) return 0;
    Window root = DefaultRootWindow(d), w = wm_property(d, root, atom);
    return w != None && w != root && wm_property(d, w, atom) == w;
}

static int compare(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static unsigned number(const char *text, unsigned max)
{
    char *end; errno = 0;
    unsigned long n = strtoul(text, &end, 10);
    if (errno || !*text || *end || !n || n > max) { fprintf(stderr, "x11-latency: invalid number\n"); exit(2); }
    return (unsigned)n;
}

int main(int argc, char **argv)
{
    unsigned limit = 30000, interval_ms = 20, timeout_seconds = 900;
    const char *ready = NULL, *stop = NULL;
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) return 2;
        if (!strcmp(argv[i], "--samples")) limit = number(argv[i + 1], 30000);
        else if (!strcmp(argv[i], "--interval-ms")) interval_ms = number(argv[i + 1], 1000);
        else if (!strcmp(argv[i], "--timeout-seconds")) timeout_seconds = number(argv[i + 1], 3600);
        else if (!strcmp(argv[i], "--ready-file")) ready = argv[i + 1];
        else if (!strcmp(argv[i], "--stop-file")) stop = argv[i + 1];
        else return 2;
    }
    uint64_t *values = calloc(limit, sizeof(*values)), *sorted = calloc(limit, sizeof(*sorted));
    if (!values || !sorted) { perror("calloc"); return 1; }
    alarm(timeout_seconds);
    XSetErrorHandler(on_error);
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "x11-latency: cannot open display\n"); return 1; }
    if (!wm_ready(d)) { fprintf(stderr, "x11-latency: window manager absent\n"); return 1; }
    int screen = DefaultScreen(d), depth = DefaultDepth(d, screen);
    if (depth != 24 && depth != 32) { fprintf(stderr, "x11-latency: unsupported depth\n"); return 1; }
    Window win = XCreateSimpleWindow(d, RootWindow(d, screen), 30, 30, 320, 180, 0, 0, 0);
    XStoreName(d, win, "SMP client repaint latency");
    XMapWindow(d, win);
    Pixmap pix = XCreatePixmap(d, win, 320, 180, (unsigned)depth);
    GC gc = XCreateGC(d, pix, 0, NULL);
    if (!gc) return 1;
    XSync(d, False);
    if (xerrors) return 1;
    /* A WM may receive MapRequest without ever mapping the client window. */
    uint64_t deadline = now_ns() + 10000000000ull;
    XWindowAttributes window_attributes;
    for (;;) {
        if (!XGetWindowAttributes(d, win, &window_attributes) || xerrors) return 1;
        if (window_attributes.map_state == IsViewable) break;
        if (now_ns() >= deadline) { fprintf(stderr, "x11-latency: window is not viewable\n"); return 1; }
        struct timespec delay = {0, 20000000L};
        while (nanosleep(&delay, &delay) && errno == EINTR) {}
    }
    if (ready) {
        int fd = open(ready, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0 || write(fd, "ready\n", 6) != 6 || close(fd)) { perror("ready file"); return 1; }
    }
    unsigned count = 0, color = 0;
    while (count < limit) {
        if (stop && access(stop, F_OK) == 0) break;
        if (stop && errno != ENOENT) { perror("stop file"); return 1; }
        color = 0x204000u + count % 256;
        uint64_t start = now_ns();
        XSetForeground(d, gc, color);
        XFillRectangle(d, pix, gc, 0, 0, 320, 180);
        XCopyArea(d, pix, win, gc, 0, 0, 320, 180, 0, 0);
        XSync(d, False);
        uint64_t end = now_ns();
        if (end < start || xerrors) return 1;
        values[count++] = end - start;
        struct timespec delay = {(time_t)(interval_ms / 1000), (long)(interval_ms % 1000) * 1000000L};
        while (nanosleep(&delay, &delay) && errno == EINTR) {}
    }
    if (!count || (stop && count == limit)) { fprintf(stderr, "x11-latency: sample limit reached or no samples\n"); return 1; }
    int viewable = XGetWindowAttributes(d, win, &window_attributes) &&
                   window_attributes.map_state == IsViewable;
    XImage *image = viewable ? XGetImage(d, win, 10, 10, 1, 1, AllPlanes, ZPixmap) : NULL;
    int pixels = image && (XGetPixel(image, 0, 0) & 0xffffffu) == color;
    if (image) XDestroyImage(image);
    int wm = wm_ready(d);
    XFreeGC(d, gc); XFreePixmap(d, pix); XDestroyWindow(d, win); XSync(d, False);
    XCloseDisplay(d);
    if (!pixels || !wm || xerrors) { fprintf(stderr, "x11-latency: pixel, WM or X error\n"); return 1; }
    memcpy(sorted, values, count * sizeof(*values));
    qsort(sorted, count, sizeof(*sorted), compare);
#define RANK(p) (((count * (p) + 99) / 100) - 1)
    printf("X11 count=%u p50_ns=%llu p95_ns=%llu p99_ns=%llu max_ns=%llu wm=1 pixels=1\n", count,
           (unsigned long long)sorted[RANK(50)], (unsigned long long)sorted[RANK(95)],
           (unsigned long long)sorted[RANK(99)], (unsigned long long)sorted[count - 1]);
#undef RANK
    for (unsigned i = 0; i < count; ++i) printf("X11_SAMPLE ns=%llu\n", (unsigned long long)values[i]);
    free(sorted); free(values);
    return 0;
}
