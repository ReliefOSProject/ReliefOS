#include "tray.h"
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <stdio.h>

Tray::~Tray()
{
    if (window_) XDestroyWindow(connection_.display(), window_);
}

void Tray::initialize()
{
    Display *display = connection_.display();
    char name[64];
    snprintf(name, sizeof(name), "_NET_SYSTEM_TRAY_S%d", DefaultScreen(display));
    tray_ = connection_.atom(name);
    XSelectInput(display, DefaultRootWindow(display), StructureNotifyMask);
    window_ = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 24, 24, 0, 0, 0xc8c8c8);
    XSelectInput(display, window_, ExposureMask | ButtonPressMask | StructureNotifyMask);
    XClassHint hint{const_cast<char *>("reliefclipboard"), const_cast<char *>("reliefos-reliefclipboard")};
    XSetClassHint(display, window_, &hint);
    XStoreName(display, window_, "ReliefClipboard");
    unsigned long embed[] = {0, 1};
    Atom info = connection_.atom("_XEMBED_INFO");
    XChangeProperty(display, window_, info, info, 32, PropModeReplace,
        reinterpret_cast<unsigned char *>(embed), 2);
    dock();
}

void Tray::dock()
{
    Display *display = connection_.display();
    Window owner = XGetSelectionOwner(display, tray_);
    if (!owner) return;
    XEvent request{};
    request.xclient.type = ClientMessage;
    request.xclient.window = owner;
    request.xclient.message_type = connection_.atom("_NET_SYSTEM_TRAY_OPCODE");
    request.xclient.format = 32;
    request.xclient.data.l[0] = CurrentTime;
    request.xclient.data.l[1] = 0;
    request.xclient.data.l[2] = window_;
    XSendEvent(display, owner, False, NoEventMask, &request);
    XFlush(display);
}

void Tray::draw()
{
    Display *display = connection_.display();
    XWindowAttributes attributes{};
    if (!XGetWindowAttributes(display, window_, &attributes)) return;
    GC gc = XCreateGC(display, window_, 0, nullptr);
    int x = (attributes.width - 16) / 2, y = (attributes.height - 18) / 2;
    XSetForeground(display, gc, 0x235a90);
    XFillRectangle(display, window_, gc, x + 1, y + 2, 14, 16);
    XSetForeground(display, gc, 0xffffff);
    XFillRectangle(display, window_, gc, x + 3, y + 4, 10, 12);
    XSetForeground(display, gc, 0x5c5c5c);
    XFillRectangle(display, window_, gc, x + 5, y, 6, 5);
    XDrawLine(display, window_, gc, x + 5, y + 8, x + 11, y + 8);
    XDrawLine(display, window_, gc, x + 5, y + 11, x + 11, y + 11);
    XDrawLine(display, window_, gc, x + 5, y + 14, x + 9, y + 14);
    XFreeGC(display, gc);
}

void Tray::event(const XEvent &event)
{
    if (event.type == ClientMessage && event.xclient.message_type == connection_.atom("MANAGER") &&
        Atom(event.xclient.data.l[1]) == tray_) dock();
    else if (event.type == Expose && event.xexpose.window == window_) draw();
    else if (event.type == ButtonPress && event.xbutton.window == window_ && clicked)
        clicked(event.xbutton.button != Button1, event.xbutton.x_root, event.xbutton.y_root, context);
    else if (event.type == DestroyNotify && event.xdestroywindow.window == window_) { window_ = None; initialize(); }
}
