// Test-only UTF-8 clipboard owner; it never maps a window over the application.
#include "fltk_support.h"
#include <FL/x.H>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
int main() {
    reliefos_fltk::init();
    fl_open_display();
    const char text[]="中文输入框粘贴测试";
    Fl::copy(text,int(strlen(text)),1);
    puts("RX_CLIPBOARD_READY"); fflush(stdout);
    time_t until=time(nullptr)+600;
    while (time(nullptr)<until) { Fl::wait(.05); usleep(10000); }
    return 0;
}
