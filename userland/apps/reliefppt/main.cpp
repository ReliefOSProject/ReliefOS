#include "../fltk_support.h"
#include "canvas.h"
#include "window.h"
#include <stdio.h>
namespace {
struct Startup {
    rp::PptWindow *window;
    const char *path;
    bool usage;
};
void start(void *data) {
    auto *s = static_cast<Startup *>(data);
    if (s->path)
        s->window->load_initial(s->path);
    else if (s->usage)
        fl_alert("%s", T("Usage: reliefppt [presentation.rppt]"));
    if (!s->usage)
        s->window->offer_recovery();
    s->window->canvas->take_focus();
    s->window->redraw();
    Fl::flush();
    puts("[reliefppt.elf] FLTK presentation editor ready");
    fflush(stdout);
}
} // namespace
int main(int argc, char **argv) {
    reliefos_fltk::init("reliefppt");
    setlocale(LC_NUMERIC, "C");
    const char *families[] = {"SimSun", "Liberation Sans", "Liberation Mono"}, *faces[] = {"", "B", "I", "P"};
    for (int f = 0; f < 3; ++f)
        for (int face = 0; face < 4; ++face) {
            static char names[12][96];
            int i = f * 4 + face;
            snprintf(names[i], sizeof(names[i]), "%s%s", faces[face], families[f]);
            Fl::set_font(16 + i, names[i]);
        }
    rp::PptWindow window;
    if (window.editor.deck.slides.empty())
        return 1;
    reliefos_fltk::style(&window);
    window.show();
    Startup startup{&window, argc == 2 ? argv[1] : nullptr, argc > 2};
    Fl::add_timeout(.2, start, &startup);
    return Fl::run();
}
