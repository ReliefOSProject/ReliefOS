#include "../fltk_support.h"
#include "window.h"
#include <stdio.h>
namespace {
struct Startup { rx::ExcelWindow *window; const char *path; bool usage; };
void start(void *context) {
    Startup *startup=static_cast<Startup *>(context);
    rx::ExcelWindow &window=*startup->window;
    window.redraw();
    Fl::flush();
    if (startup->path) window.load_initial(startup->path);
    else if (startup->usage) fl_alert("%s",T("Usage: reliefexcel [workbook.rexl | data.csv]"));
    else window.offer_recovery();
    window.redraw();
    Fl::flush();
    puts("[reliefexcel.elf] FLTK spreadsheet ready");
    fflush(stdout);
}
}
int main(int argc, char **argv)
{
    reliefos_fltk::init("reliefexcel");
    // Native formulas and CSV use a dot independently of the UI language.
    setlocale(LC_NUMERIC,"C");
    rx::ExcelWindow window;
    reliefos_fltk::style(&window);
    window.show();
    Startup startup{&window,argc==2?argv[1]:nullptr,argc>2};
    Fl::add_timeout(.2,start,&startup);
    return Fl::run();
}
