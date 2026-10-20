#include "window.h"
#include <stdio.h>
namespace {
rd::DiagramWindow *active_window = nullptr;
Fl_Event_Dispatch previous_dispatch = nullptr;
int dispatch(int event, Fl_Window *window) {
  // FLTK normally offers keys to the focused input before its parent window.
  // Apply must run before a multiline input turns Ctrl+Enter into a newline.
  if (active_window && window == active_window && !Fl::modal() && !Fl::grab() &&
      (event == FL_KEYDOWN || event == FL_SHORTCUT) &&
      Fl::event_key() == FL_Enter && Fl::event_state(FL_CTRL)) {
    active_window->run(rd::Command::Apply);
    return 1;
  }
  return previous_dispatch ? previous_dispatch(event, window)
                           : Fl::handle_(event, window);
}
struct Startup {
  rd::DiagramWindow *window;
  const char *path;
};
void ready(void *p) {
  auto *s = static_cast<Startup *>(p);
  s->window->redraw();
  Fl::flush();
  if (s->path)
    s->window->load_initial(s->path);
  puts("[reliefdiagram.elf] FLTK diagram workspace ready");
  fflush(stdout);
}
} // namespace
int main(int argc, char **argv) {
  const char *path = nullptr;
  for (int n = 1; n < argc; ++n) {
    if (!strcmp(argv[n], "--help")) {
      puts("Usage: reliefdiagram [document.rdiagram]\nNative FLTK mind maps, "
           "flowcharts and logic diagrams.");
      return 0;
    }
    if (path || argv[n][0] == '-') {
      fputs("reliefdiagram: unsupported argument\n", stderr);
      return 2;
    }
    path = argv[n];
  }
  reliefos_fltk::init("reliefdiagram");
  setlocale(LC_NUMERIC, "C");
  rd::DiagramWindow window;
  reliefos_fltk::style(&window);
  active_window = &window;
  previous_dispatch = Fl::event_dispatch();
  Fl::event_dispatch(dispatch);
  window.show();
  Startup startup{&window, path};
  Fl::add_timeout(.2, ready, &startup);
  int status = Fl::run();
  Fl::event_dispatch(previous_dispatch);
  active_window = nullptr;
  return status;
}
