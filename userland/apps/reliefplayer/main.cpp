#include "window.h"
#include <stdio.h>
namespace {
rp::PlayerWindow *active = nullptr;
Fl_Event_Dispatch previous = nullptr;
int dispatch(int event, Fl_Window *window) {
  if (active && window == active && !Fl::grab() && !Fl::modal() &&
      (event == FL_KEYDOWN || event == FL_SHORTCUT) && active->hotkey())
    return 1;
  return previous ? previous(event, window) : Fl::handle_(event, window);
}
struct Startup {
  rp::PlayerWindow *window;
  const char *path;
};
void ready(void *p) {
  auto &start = *static_cast<Startup *>(p);
  start.window->redraw();
  Fl::flush();
  puts("[reliefplayer.elf] FLTK audio player ready");
  fflush(stdout);
  if (start.path)
    start.window->open_path(start.path);
}
} // namespace
int main(int argc, char **argv) {
  const char *path = nullptr;
  bool literal = false;
  for (int n = 1; n < argc; ++n) {
    if (!literal && !strcmp(argv[n], "--help")) {
      puts("Usage: reliefplayer [--] [audio.mp3|audio.wav]\nNative FLTK audio "
           "player.");
      return 0;
    }
    if (!literal && !strcmp(argv[n], "--")) {
      literal = true;
      continue;
    }
    if (path || (!literal && argv[n][0] == '-')) {
      fputs("reliefplayer: unsupported argument\n", stderr);
      return 2;
    }
    path = argv[n];
  }
  reliefos_fltk::init("reliefplayer");
  setlocale(LC_NUMERIC, "C");
  rp::PlayerWindow window;
  reliefos_fltk::style(&window);
  window.show();
  Startup startup{&window, path};
  Fl::add_timeout(.15, ready, &startup);
  active = &window;
  previous = Fl::event_dispatch();
  Fl::event_dispatch(dispatch);
  int result = Fl::run();
  Fl::event_dispatch(previous);
  active = nullptr;
  Fl::remove_timeout(ready, &startup);
  return result;
}
