#include "../fltk_support.h"
#include "window.h"
#include <stdio.h>
#include <string.h>

static void ready(void *) {
    Fl::flush(); puts("[calc.elf] FLTK calculator ready"); fflush(stdout);
}
int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--help")) {
        puts("Usage: calc\nNative FLTK calculator: standard, scientific, programmer, units, statistics and dates."); return 0;
    }
    if (argc!=1) { fputs("calc: unsupported argument\n",stderr); return 2; }
    reliefos_fltk::init("calc");
    calc::Window window; reliefos_fltk::style(&window); window.show();
    Fl::add_timeout(.2,ready);
    return Fl::run();
}
