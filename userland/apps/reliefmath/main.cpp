#include "window.h"
#include <locale.h>
#include <stdio.h>
#include <string.h>
namespace {
struct Startup { rm::MathWindow *window; const char *path; };
void finish_startup(void *context) {
    Startup *startup=static_cast<Startup *>(context);
    rm::MathWindow &window=*startup->window;
    window.redraw();
    Fl::flush();
    if (startup->path) window.load_initial(startup->path);
    window.redraw();
    Fl::flush();
    puts("[reliefmath.elf] FLTK formula editor ready");
    fflush(stdout);
}
}
int main(int argc,char **argv){
    reliefos_fltk::init("reliefmath");setlocale(LC_NUMERIC,"C");
    const char *path=nullptr;
    for(int n=1;n<argc;n++){
        if(!strcmp(argv[n],"--help")){puts("Usage: reliefmath [document.rmath]\nOffline mathematical formula editor with SVG, transparent PNG and LaTeX export.");return 0;}
        if(path||argv[n][0]=='-'){fprintf(stderr,"reliefmath: unsupported argument\n");return 2;}path=argv[n];
    }
    rm::MathWindow window;reliefos_fltk::style(&window);window.show();
    Startup startup{&window,path};
    Fl::add_timeout(.2,finish_startup,&startup);
    return Fl::run();
}
