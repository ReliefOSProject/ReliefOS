// Test-only independent X11 clipboard consumer. No application debug API.
#include "fltk_support.h"
#include <FL/Fl_Window.H>
#include <FL/x.H>
#include <stdio.h>
#include <string.h>
#include <time.h>

class Receiver : public Fl_Widget {
public:
    bool pending=false;
    Receiver() : Fl_Widget(0,0,1,1) {}
    void draw() override {}
    int handle(int event) override {
        if (event!=FL_PASTE) return 0;
        pending=false;
        if (Fl::event_clipboard_type()!=Fl::clipboard_plain_text) return 1;
        int length=Fl::event_length();
        if (length<0 || length>2047) return 1;
        char hex[4096]; const char *text=Fl::event_text();
        for (int n=0;n<length;++n) {
            unsigned c=static_cast<unsigned char>(text[n]);
            hex[2*n]="0123456789abcdef"[c>>4]; hex[2*n+1]="0123456789abcdef"[c&15];
        }
        hex[2*length]=0; printf("CALC_CLIPBOARD_HEX %s\n",hex); fflush(stdout); return 1;
    }
};
int main() {
    reliefos_fltk::init(); fl_open_display();
    Fl_Window anchor(0,0,1,1); anchor.set_override(); Receiver receiver;
    anchor.end(); anchor.show(); Fl::check();
    Fl::copy("probe",5,1);
    Window initial=XGetSelectionOwner(fl_display,XInternAtom(fl_display,"CLIPBOARD",False));
    puts("CALC_CLIPBOARD_READY"); fflush(stdout);
    unsigned polls=0; time_t until=time(nullptr)+1800;
    while (time(nullptr)<until) {
        Fl::wait(.1);
        Window owner=XGetSelectionOwner(fl_display,XInternAtom(fl_display,"CLIPBOARD",False));
        if (++polls%5==0 && owner && owner!=initial && !receiver.pending && Fl::clipboard_contains(Fl::clipboard_plain_text)) {
            receiver.pending=true; Fl::paste(receiver,1,Fl::clipboard_plain_text);
        }
    }
    return 0;
}
