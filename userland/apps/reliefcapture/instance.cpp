#include "instance.h"
#include <FL/Fl.H>
#include <FL/x.H>
#include <stdio.h>
#include <unistd.h>
namespace rc {
Instance::Result Instance::claim(bool select, Text &error) {
  fl_open_display();
  if (!fl_display) { error.set("Could not connect to the X11 display"); return Result::Failed; }
  char name[96]; snprintf(name, sizeof(name), "_RELIEFOS_CAPTURE_INSTANCE_%lu", (unsigned long)getuid());
  Atom selection = XInternAtom(fl_display, name, False);
  message_ = XInternAtom(fl_display, "_RELIEFOS_CAPTURE_REQUEST", False);
  XGrabServer(fl_display);
  ::Window existing = XGetSelectionOwner(fl_display, selection);
  if (existing) {
    XEvent event{}; event.xclient.type = ClientMessage; event.xclient.display = fl_display;
    event.xclient.window = existing; event.xclient.message_type = message_; event.xclient.format = 32;
    event.xclient.data.l[0] = select ? 1 : 0;
    int sent = XSendEvent(fl_display, existing, False, NoEventMask, &event);
    XUngrabServer(fl_display);
    // Complete the server round trip before this short-lived sender exits.
    XSync(fl_display, False);
    if (!sent) { error.set("Could not contact the existing screenshot window"); return Result::Failed; }
    return Result::Forwarded;
  }
  window_ = XCreateSimpleWindow(fl_display, DefaultRootWindow(fl_display), 0, 0, 1, 1, 0, 0, 0);
  XSetSelectionOwner(fl_display, selection, window_, CurrentTime);
  bool owner = XGetSelectionOwner(fl_display, selection) == window_;
  XUngrabServer(fl_display); XFlush(fl_display);
  if (!owner) { error.set("Could not register the screenshot window"); return Result::Failed; }
  Fl::add_system_handler(event, this); return Result::Primary;
}
int Instance::event(void *raw, void *data) {
  auto *self = static_cast<Instance *>(data); auto *event = static_cast<XEvent *>(raw);
  if (event->type == ClientMessage && event->xclient.window == self->window_ &&
      event->xclient.message_type == self->message_ && event->xclient.format == 32) {
    if (self->request_) self->request_(event->xclient.data.l[0] == 1, self->data_);
    return 1;
  }
  return 0;
}
Instance::~Instance() {
  Fl::remove_system_handler(event);
  if (window_ && fl_display) XDestroyWindow(fl_display, window_);
}
}
