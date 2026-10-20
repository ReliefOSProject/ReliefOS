#ifndef RELIEFMATHPP_SCENE_H
#define RELIEFMATHPP_SCENE_H
#include "mesh.h"
namespace rmp {
struct Primitive {
  enum Type { Line, Fill, Dot, Label } type = Line;
  double x[3]{}, y[3]{};
  uint32_t rgb = 0;
  int width = 1;
  Text text;
};
struct Frame {
  int width = 0, height = 0;
  Array<Primitive> items;
};
uint32_t palette(int);
bool frame(const Document &, const Array<Mesh> &, int tab, int width,
           int height, Frame &);
bool svg(const Frame &, Bytes &, Text &error);
} // namespace rmp
#endif
