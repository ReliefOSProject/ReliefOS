#include "window.h"
#include <stdio.h>
namespace {
struct Startup {
  rmp::MathWindow *window;
  const char *path;
};
void ready(void *p) {
  auto *s = static_cast<Startup *>(p);
  s->window->redraw();
  Fl::flush();
  if (s->path)
    s->window->load_initial(s->path);
  puts("[reliefmathpp.elf] FLTK geometry workspace ready");
  fflush(stdout);
}
} // namespace
int main(int argc, char **argv) {
  const char *path = nullptr;
  for (int n = 1; n < argc; ++n) {
    if (!strcmp(argv[n], "--help")) {
      puts("Usage: reliefmathpp [document.rmathpp]\nNative FLTK 2D/3D "
           "geometry, functions and numerical analysis.");
      return 0;
    }
    if (path || argv[n][0] == '-') {
      fputs("reliefmathpp: unsupported argument\n", stderr);
      return 2;
    }
    path = argv[n];
  }
  reliefos_fltk::init("reliefmathpp");
  setlocale(LC_NUMERIC, "C");
  rmp::MathWindow window;
  reliefos_fltk::style(&window);
  window.show();
  Startup startup{&window, path};
  Fl::add_timeout(.2, ready, &startup);
  return Fl::run();
}
