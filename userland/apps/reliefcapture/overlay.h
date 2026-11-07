#ifndef RELIEFCAPTURE_OVERLAY_H
#define RELIEFCAPTURE_OVERLAY_H
#include "capture.h"
#include "../fltk_support.h"
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_RGB_Image.H>
namespace rc {
class Overlay : public Fl_Double_Window {
  Snapshot snapshot_;
  Fl_Group *bar_ = nullptr;
  Fl_Button *region_ = nullptr, *window_ = nullptr;
  bool dragging_ = false, window_mode_ = false;
  Point start_;
  Rect selected_;
  Array<unsigned char> original_pixels_, dim_pixels_;
  Fl_RGB_Image *original_ = nullptr, *dim_ = nullptr;
  void (*finished_)(Rect, bool, void *) = nullptr;
  void *data_ = nullptr;
  void mode(bool);
  void hover(int, int);
  void finish(bool canceled);
  static void dim_line(void *, int, int, int, unsigned char *);
public:
  Overlay(Snapshot &&, bool window_mode, void (*)(Rect, bool, void *), void *);
  const Image &image() const { return snapshot_.image; }
  void present();
  void dismiss();
  void draw() override;
  int handle(int) override;
  ~Overlay() override;
};
}
#endif
