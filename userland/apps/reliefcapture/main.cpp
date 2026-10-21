#include "window.h"
#include "instance.h"
#include <stdio.h>
int main(int argc, char **argv) {
  bool select = false;
  if (argc == 2 && !strcmp(argv[1], "--help")) {
    puts("Usage: reliefcapture [--select]\nNative FLTK screen capture and annotation.\n--select opens region selection directly."); return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--select")) select = true;
  else if (argc != 1) { fputs("reliefcapture: unsupported argument\n", stderr); return 2; }
  reliefos_fltk::init("reliefcapture"); setlocale(LC_NUMERIC, "C");
  rc::Instance instance; rc::Text error;
  auto result = instance.claim(select, error);
  if (result == rc::Instance::Result::Forwarded) return 0;
  if (result == rc::Instance::Result::Failed) { fl_alert("%s", T(error.c_str())); return 1; }
  rc::CaptureWindow window(select);
  instance.callback([](bool select, void *p) { static_cast<rc::CaptureWindow *>(p)->request(select); }, &window);
  if (!select) window.show();
  Fl::flush(); puts("[reliefcapture.elf] FLTK screen capture ready"); fflush(stdout);
  if (select) window.request(true);
  // A capture worker or countdown can run while both GUI windows are hidden.
  while (window.running()) Fl::wait(.05);
  return 0;
}
