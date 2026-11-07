#include "ui_widgets.h"
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <assert.h>

using namespace rmail;

namespace {
int activations = 0;
void activated(Fl_Widget *, void *) { ++activations; }
}

int main() {
    Fl_Double_Window window(240, 160);
    window.begin();
    NavigationButton inbox(0, 0, 160, 28, "Inbox");
    NavigationButton container(0, 30, 160, 28, "Projects (container)");
    NavigationButton sent(0, 60, 160, 28, "Sent");
    inbox.next(&container);
    container.previous(&inbox);
    container.next(&sent);
    sent.previous(&container);
    container.deactivate();
    sent.callback(activated);
    window.end();

    /* Exercise FLTK's actual widget focus contract, not a geometry model. */
    assert(inbox.visible_focus());
    assert(inbox.take_focus());
    assert(Fl::focus() == &inbox);

    Fl::e_keysym = FL_Down;
    assert(inbox.handle(FL_KEYDOWN));
    assert(Fl::focus() == &sent);
    Fl::e_keysym = FL_Up;
    assert(sent.handle(FL_KEYDOWN));
    assert(Fl::focus() == &inbox);

    Fl::e_keysym = FL_Enter;
    assert(inbox.handle(FL_KEYDOWN));
    assert(activations == 0);
    assert(sent.take_focus());
    assert(sent.handle(FL_KEYDOWN));
    assert(activations == 1);
    return 0;
}
