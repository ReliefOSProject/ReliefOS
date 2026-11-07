#ifndef RELIEFMATHPP_MESH_H
#define RELIEFMATHPP_MESH_H
#include "model.h"
namespace rmp {
struct Segment {
  Vec3 a, b;
};
struct Triangle {
  Vec3 a, b, c;
};
struct Mesh {
  Array<Segment> lines;
  Array<Triangle> faces;
  Array<Vec3> points;
  Text error;
};
class Sampler {
  Object object_;
  Variables vars_;
  Expression expr_[3];
  int row_ = 0, total_ = 0;
  bool surface_ = false, implicit_ = false, done_ = true;
  Vec3 position(double u, double v = 0) const;
  bool line(Mesh &, Vec3, Vec3);
  bool face(Mesh &, Vec3, Vec3, Vec3);

public:
  bool begin(const Object &, const Variables &, Mesh &);
  bool step(Mesh &, int budget = 2);
};
bool finite(Vec3);
struct Projected {
  double x, y, depth;
};
Projected project(Vec3, const View &, int width, int height);
} // namespace rmp
#endif
