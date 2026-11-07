#ifndef RELIEFDIAGRAM_CANVAS_H
#define RELIEFDIAGRAM_CANVAS_H
#include "scene.h"
#include <FL/Fl_Widget.H>
namespace rd {
class DiagramWindow;
class Canvas : public Fl_Widget {
  DiagramWindow *owner_;
  Frame frame_;
  bool valid_ = false, gesture_ = false, started_ = false;
  enum class Drag { None, Move, Resize, Box, Pan } drag_ = Drag::None;
  Point start_, last_;
  int mouse_x_ = 0, mouse_y_ = 0;
  uint64_t source_ = 0, resize_id_ = 0;
  Array<Node> originals_;
  uint64_t hit(Point) const;
  bool node_visible(uint64_t) const;

public:
  Canvas(int, int, int, int, DiagramWindow *);
  void invalidate() {
    valid_ = false;
    redraw();
  }
  bool prepare();
  const Frame &frame() const { return frame_; }
  Point world(int, int) const;
  Point screen(Point) const;
  void fit();
  void zoom(double, int = -1, int = -1);
  void cancel();
  void draw() override;
  int handle(int) override;
};
} // namespace rd
#endif
