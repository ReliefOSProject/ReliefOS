#include "../fltk_support.h"
#include "window.h"
#include "view.h"
#include <stdio.h>
namespace {
struct Startup { rw::WordWindow *window; const char *path; bool usage; };
void start(void *context) {
    Startup *startup=static_cast<Startup *>(context);
    if(startup->path) startup->window->load_initial(startup->path);
    else if(startup->usage) fl_alert("%s",T("Usage: reliefword [document.rword | text.txt | page.html]"));
    else startup->window->offer_recovery();
    startup->window->view->take_focus(); startup->window->redraw(); Fl::flush();
    puts("[reliefword.elf] FLTK word processor ready"); fflush(stdout);
}
}
int main(int argc,char **argv) {
    reliefos_fltk::init("reliefword"); setlocale(LC_NUMERIC,"C");
    const char *families[]={"Liberation Sans","SimSun","Liberation Mono"};
    const char *prefixes[]={"","B","I","P"};
    for(int family=0;family<3;++family) for(int face=0;face<4;++face) {
        // FLTK retains font-name pointers; static storage lives for the app.
        static char names[12][96]; int index=family*4+face;
        snprintf(names[index],sizeof(names[index]),"%s%s",prefixes[face],families[family]); Fl::set_font(16+index,names[index]);
    }
    rw::WordWindow window; if(window.editor.doc.paragraphs.empty()) return 1;
    reliefos_fltk::style(&window); window.show();
    Startup startup{&window,argc==2?argv[1]:nullptr,argc>2}; Fl::add_timeout(.2,start,&startup);
    return Fl::run();
}
