#include "window.h"
#include <stdio.h>
namespace {
struct Startup { rt::TodoWindow *window; const char *path; };
void ready(void *p) {
    auto *s=static_cast<Startup *>(p);
    s->window->redraw(); Fl::flush();
    if (s->path) s->window->load_initial(s->path);
    puts("[relieftodo.elf] FLTK task manager ready"); fflush(stdout);
}
}
int main(int argc,char **argv) {
    const char *path=nullptr;
    for (int n=1;n<argc;++n) {
        if (!strcmp(argv[n],"--help")) { puts("Usage: relieftodo [list.rtodo]\nNative FLTK offline task manager with CSV and Markdown export."); return 0; }
        if (path || argv[n][0]=='-') { fputs("relieftodo: unsupported argument\n",stderr); return 2; }
        path=argv[n];
    }
    reliefos_fltk::init("relieftodo");
    rt::TodoWindow window; reliefos_fltk::style(&window); window.show();
    Startup startup{&window,path}; Fl::add_timeout(.2,ready,&startup);
    return Fl::run();
}
