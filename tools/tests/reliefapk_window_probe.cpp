#include <FL/x.H>
#include <X11/Xatom.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    unsigned long pid = strtoul(argv[1], nullptr, 10);
    fl_open_display();
    Window root = DefaultRootWindow(fl_display);
    Atom active = XInternAtom(fl_display, "_NET_ACTIVE_WINDOW", False),
         owner = XInternAtom(fl_display, "_NET_WM_PID", False),
         name = XInternAtom(fl_display, "_NET_WM_NAME", False);
    char last[4096] = {};
    char last_auth[256] = {};
    char last_payload[128] = "";
    Atom clients = XInternAtom(fl_display, "_NET_CLIENT_LIST", False);
    while (kill(static_cast<pid_t>(pid), 0) == 0) {
        char payload[128] = "absent";
        FILE *payload_file = fopen("/usr/share/reliefapk-demo/reliefapk-demo.txt", "rb");
        if (payload_file) {
            if (!fgets(payload, sizeof(payload), payload_file))
                strcpy(payload, "empty");
            fclose(payload_file);
            payload[strcspn(payload, "\r\n")] = 0;
        }
        if (strcmp(payload, last_payload)) {
            printf("RA_PAYLOAD value=%s\n", payload);
            fflush(stdout);
            strcpy(last_payload, payload);
        }
        Atom type;
        int format;
        unsigned long count, left;
        unsigned char *data = nullptr;
        if (XGetWindowProperty(fl_display, root, active, 0, 1, False, XA_WINDOW, &type, &format, &count,
                               &left, &data) == Success &&
            data && count) {
            Window window = *reinterpret_cast<Window *>(data);
            XFree(data);
            data = nullptr;
            if (XGetWindowProperty(fl_display, window, owner, 0, 1, False, XA_CARDINAL, &type, &format,
                                   &count, &left, &data) == Success &&
                data && count && *reinterpret_cast<unsigned long *>(data) == pid) {
                XFree(data);
                data = nullptr;
                int x, y, rx, ry;
                unsigned w, h, b, d;
                Window child, geometry_root;
                if (XGetGeometry(fl_display, window, &geometry_root, &rx, &ry, &w, &h, &b, &d) &&
                    XTranslateCoordinates(fl_display, window, root, 0, 0, &x, &y, &child)) {
                    char title[2048] = {};
                    if (XGetWindowProperty(fl_display, window, name, 0, 500, False, AnyPropertyType, &type,
                                           &format, &count, &left, &data) == Success &&
                        data) {
                        size_t n = count < sizeof(title) - 1 ? count : sizeof(title) - 1;
                        memcpy(title, data, n);
                        XFree(data);
                        data = nullptr;
                    }
                    char line[4096];
                    snprintf(line, sizeof(line), "RA_ACTIVE id=%lu x=%d y=%d w=%u h=%u title=%s",
                             static_cast<unsigned long>(window), x, y, w, h, title);
                    if (strcmp(last, line)) {
                        puts(line);
                        fflush(stdout);
                        strcpy(last, line);
                    }
                }
            }
            if (data)
                XFree(data);
        } else if (data)
            XFree(data);
        // A focus-follows-mouse WM can keep _NET_ACTIVE_WINDOW on the main
        // window while a password dialog is mapped above it. Observe the
        // actual mapped client, rather than infer its position from focus.
        char auth[256] = "RA_AUTH_WINDOW visible=0";
        data = nullptr;
        if (XGetWindowProperty(fl_display, root, clients, 0, 4096, False, XA_WINDOW,
                               &type, &format, &count, &left, &data) == Success && data) {
            auto *windows = reinterpret_cast<Window *>(data);
            for (unsigned long i = 0; i < count; ++i) {
                unsigned char *pid_data = nullptr;
                unsigned long pid_count, pid_left;
                if (XGetWindowProperty(fl_display, windows[i], owner, 0, 1, False, XA_CARDINAL,
                                       &type, &format, &pid_count, &pid_left, &pid_data) != Success ||
                    !pid_data)
                    continue;
                bool owned = pid_count && *reinterpret_cast<unsigned long *>(pid_data) == pid;
                XFree(pid_data);
                if (!owned)
                    continue;
                XWindowAttributes attributes;
                if (!XGetWindowAttributes(fl_display, windows[i], &attributes) ||
                    attributes.map_state != IsViewable || attributes.width != 500 || attributes.height != 226)
                    continue;
                int x, y;
                Window child;
                if (XTranslateCoordinates(fl_display, windows[i], root, 0, 0, &x, &y, &child))
                    snprintf(auth, sizeof(auth), "RA_AUTH_WINDOW visible=1 x=%d y=%d w=500 h=226", x, y);
                break;
            }
            XFree(data);
        }
        if (strcmp(last_auth, auth)) {
            puts(auth);
            fflush(stdout);
            strcpy(last_auth, auth);
        }
        usleep(100000);
    }
}
