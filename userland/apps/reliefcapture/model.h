#ifndef RELIEFCAPTURE_MODEL_H
#define RELIEFCAPTURE_MODEL_H
#include "../relieftodo/storage.h"
namespace rc {
using rt::Text;
template <class T> using Array = rt::Array<T>;
constexpr size_t MaxPixels = 16777216, MaxMarks = 512, MaxPoints = 8192;
constexpr int MaxSide = 8192;
struct Point { int x = 0, y = 0; };
struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
  bool valid() const { return w > 0 && h > 0; }
  bool contains(int px, int py) const;
};
Rect selection(int x0, int y0, int x1, int y1, int width, int height);
Rect fit_rect(int iw, int ih, Rect available, bool natural = false);
bool image_point(Rect view, int iw, int ih, int x, int y, Point &out);
bool valid_utf8(const char *, size_t);
struct Image {
  int width = 0, height = 0;
  Array<uint32_t> pixels;
  bool create(int, int);
  bool crop(Rect, Image &) const;
  bool valid() const;
};
enum class Tool : unsigned { Pen, Arrow, Rectangle, Label };
struct Mark {
  Tool tool = Tool::Pen;
  Point a, b;
  unsigned color = 0xe53935, width = 3, font = 24;
  Array<Point> points;
  Text text;
};
class Document {
  Image image_;
  Array<Mark> marks_;
  size_t visible_ = 0;
public:
  const Image &image() const { return image_; }
  const Array<Mark> &marks() const { return marks_; }
  size_t visible() const { return visible_; }
  bool can_undo() const { return visible_ != 0; }
  bool can_redo() const { return visible_ < marks_.size(); }
  void replace(Image &&);
  bool add(Mark &&);
  bool undo();
  bool redo();
};
}
#endif
