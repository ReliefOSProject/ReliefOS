#ifndef RELIEFMATHPP_MODEL_H
#define RELIEFMATHPP_MODEL_H
#include "expression.h"
namespace rmp {
constexpr size_t MaxObjects = 64, MaxFileBytes = 16U * 1024U * 1024U;
enum class Kind {
  Function2,
  Parametric2,
  Polar2,
  Implicit2,
  Point2,
  Segment2,
  Vector2,
  Circle2,
  Ellipse2,
  Triangle2,
  Rectangle2,
  Function3,
  Parametric3,
  Curve3,
  Point3,
  Segment3,
  Vector3,
  Triangle3,
  Plane3,
  Sphere3,
  Box3,
  Cylinder3,
  Cone3,
  Count
};
struct KindInfo {
  const char *name, *fields, *help;
  int formulas, values;
  bool dim3;
};
const KindInfo &info(Kind);
struct Vec3 {
  double x = 0, y = 0, z = 0;
};
Vec3 add(Vec3, Vec3);
Vec3 subtract(Vec3, Vec3);
Vec3 scale(Vec3, double);
double dot(Vec3, Vec3);
Vec3 cross_product(Vec3, Vec3);
double norm(Vec3);
struct Object {
  uint64_t id = 0;
  Kind kind = Kind::Function2;
  Text name, formula[3];
  double p[12]{}, range[4]{-5, 5, -5, 5};
  int color = 0;
  bool visible = true;
  int resolution = 48;
};
struct View {
  double cx = 0, cy = 0, span = 14, yaw = .65, pitch = .55, scale3 = 1, tx = 0,
         ty = 0, tz = 0;
  bool grid = true, axes = true, wire = false;
  int tab = 0;
};
struct Document {
  Array<Object> objects;
  uint64_t next_id = 1;
  Variables vars;
  View view;
  bool clone_from(const Document &);
};
Object make_object(Kind);
bool valid_object(const Object &, Text &error);
bool validate(const Document &, Text &error);
bool append(Document &, Object, uint64_t &id);
Object *find(Document &, uint64_t);
bool example(Document &, int index);
struct Measures {
  double length = 0, area = 0, volume = 0;
  bool any = false;
};
Measures measure(const Object &);
class History {
  Array<Document> past_, future_;

public:
  bool checkpoint(const Document &);
  bool undo(Document &);
  bool redo(Document &);
  void clear() {
    past_.clear();
    future_.clear();
  }
};
} // namespace rmp
#endif
