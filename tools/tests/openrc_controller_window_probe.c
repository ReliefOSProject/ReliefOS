/* Independent X11 observer for the private QEMU acceptance harness. */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(void) {
  Display *display = XOpenDisplay(NULL);
  if (!display)
    return 1;
  Window root = DefaultRootWindow(display);
  Atom clients = XInternAtom(display, "_NET_CLIENT_LIST", False),
       owner = XInternAtom(display, "_NET_WM_PID", False),
       name = XInternAtom(display, "_NET_WM_NAME", False);
  char last[16384] = "";
  for (;;) {
    Atom type;
    int format;
    unsigned long count, left;
    unsigned char *data = NULL;
    char current[16384] = "";
    if (XGetWindowProperty(display, root, clients, 0, 256, False, XA_WINDOW,
                           &type, &format, &count, &left, &data) == Success &&
        data) {
      Window *windows = (Window *)data;
      for (unsigned long i = 0; i < count; ++i) {
        XWindowAttributes attr;
        int x, y;
        Window child;
        unsigned char *title = NULL, *pid_data = NULL;
        unsigned long n, remaining;
        if (!XGetWindowAttributes(display, windows[i], &attr) ||
            attr.map_state != IsViewable)
          continue;
        if (!XTranslateCoordinates(display, windows[i], root, 0, 0, &x, &y,
                                   &child))
          continue;
        XGetWindowProperty(display, windows[i], name, 0, 128, False,
                           AnyPropertyType, &type, &format, &n, &remaining,
                           &title);
        XGetWindowProperty(display, windows[i], owner, 0, 1, False, XA_CARDINAL,
                           &type, &format, &n, &remaining, &pid_data);
        char line[1024];
        snprintf(line, sizeof(line),
                 "OC_WINDOW id=%lu pid=%lu x=%d y=%d w=%d h=%d title=%s\n",
                 windows[i], pid_data && n ? *(unsigned long *)pid_data : 0, x,
                 y, attr.width, attr.height, title ? (char *)title : "");
        if (strlen(current) + strlen(line) < sizeof(current))
          strcat(current, line);
        if (title)
          XFree(title);
        if (pid_data)
          XFree(pid_data);
      }
      XFree(data);
    }
    if (strcmp(current, last)) {
      fputs(current, stdout);
      fflush(stdout);
      strcpy(last, current);
    }
    usleep(150000);
  }
}
