#ifndef RELIEFMATHPP_CANVAS_H
#define RELIEFMATHPP_CANVAS_H
#include "../fltk_support.h"
#include "scene.h"
namespace rmp {
class MathWindow;
class Canvas : public Fl_Widget {
  MathWindow *owner_;
  int tab_, last_x_ = 0, last_y_ = 0;
  bool dragging_ = false, valid_ = false;
  Frame frame_;
  void draw() override;

public:
  Canvas(int, int, int, int, MathWindow *, int);
  int handle(int) override;
  void invalidate() {
    valid_ = false;
    redraw();
  }
  bool prepare();
  bool export_svg(Bytes &, Text &);
  bool export_png(Bytes &, Text &);
};
} // namespace rmp
#endif
