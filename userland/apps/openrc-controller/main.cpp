#include "window.h"
#include "worker.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--worker"))
    return oc::worker_main();
  if (argc != 1) {
    fputs("Usage: openrc-controller\n", stderr);
    return 2;
  }
  reliefos_fltk::init("openrc-controller");
  oc::Session session;
  if (!oc::authenticate(session))
    return 1;
  oc::App app(session);
  app.show();
  return Fl::run();
}
