#include "window.h"
#include "worker.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--worker"))
    return nc::worker_main();
  if (argc != 1) {
    fputs("Usage: network-controller\n", stderr);
    return 2;
  }
  reliefos_fltk::init("network-controller");
  nc::Session session;
  if (!nc::authenticate(session))
    return 1;
  nc::App app(session);
  app.show();
  return Fl::run();
}
