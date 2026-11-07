#ifndef RELIEFDIAGRAM_SCENE_H
#define RELIEFDIAGRAM_SCENE_H
#include "model.h"
namespace rd {
struct Stroke {
  Point points[64]{};
  unsigned count = 0;
  bool closed = false, filled = false;
  uint32_t fill = 0xffffff, line = 0x65768c;
  double width = 2;
  uint64_t owner = 0;
};
struct Label {
  Point at;
  Text text;
  uint32_t color = 0x202d40;
  int font = 16;
  bool bold = false;
};
struct Frame {
  Array<Stroke> paths;
  Array<Label> labels;
  Array<uint64_t> visible_nodes;
  Rect bounds;
};
Stroke outline(const Node &);
Point anchor(const Node &, Point towards);
Stroke connection(const Node &, const Node &, Route);
bool inside(const Node &, Point);
double distance(const Stroke &, Point);
bool build_frame(const Document &, Frame &, Text &);
bool svg(const Frame &, Bytes &, Text &);
} // namespace rd
#endif
