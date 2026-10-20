#include "model.h"
#include <math.h>
namespace rmp {
namespace {
const KindInfo types[] = {
    {"二维函数", "", "y = f(x)，变量 x、a、b、c、d", 1, 0, false},
    {"二维参数曲线", "", "x(t)、y(t)，变量 t、a、b、c、d", 2, 0, false},
    {"极坐标曲线", "", "r(t)，t 是弧度", 1, 0, false},
    {"隐式曲线", "", "F(x,y) = 0（例如 x^2+y^2-9）", 1, 0, false},
    {"二维点", "x,y", "点的坐标", 0, 2, false},
    {"二维线段", "x1,y1,x2,y2", "两个端点", 0, 4, false},
    {"二维向量", "x,y,dx,dy", "起点和向量分量", 0, 4, false},
    {"圆", "cx,cy,r", "圆心与正半径", 0, 3, false},
    {"椭圆", "cx,cy,rx,ry", "中心与两个正半轴", 0, 4, false},
    {"二维三角形", "x1,y1,x2,y2,x3,y3", "三个顶点", 0, 6, false},
    {"矩形", "x,y,width,height", "左下角与正宽高", 0, 4, false},
    {"三维函数曲面", "", "z = f(x,y)，变量 x、y、a、b、c、d", 1, 0, true},
    {"三维参数曲面", "", "x(u,v)、y(u,v)、z(u,v)", 3, 0, true},
    {"空间参数曲线", "", "x(t)、y(t)、z(t)", 3, 0, true},
    {"三维点", "x,y,z", "点的坐标", 0, 3, true},
    {"三维线段", "x1,y1,z1,x2,y2,z2", "两个端点", 0, 6, true},
    {"三维向量", "x,y,z,dx,dy,dz", "起点和向量分量", 0, 6, true},
    {"三维三角形", "x1,y1,z1,x2,y2,z2,x3,y3,z3", "三个顶点", 0, 9, true},
    {"平面", "nx,ny,nz,d", "nx*x+ny*y+nz*z=d；范围决定显示大小", 0, 4, true},
    {"球体", "cx,cy,cz,r", "球心与正半径", 0, 4, true},
    {"长方体", "x,y,z,width,depth,height", "最小角与正长宽高", 0, 6, true},
    {"圆柱", "cx,cy,z,r,height", "沿 Z 轴，正半径和正高度", 0, 5, true},
    {"圆锥", "cx,cy,z,r,height", "沿 Z 轴，底面中心、正半径和正高度", 0, 5,
     true}};
}
const KindInfo &info(Kind k) {
  int n = int(k);
  return types[n >= 0 && n < int(Kind::Count) ? n : 0];
}
Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 subtract(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 scale(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross_product(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double norm(Vec3 a) { return hypot(hypot(a.x, a.y), a.z); }
bool Document::clone_from(const Document &other) {
  if (!objects.clone_from(other.objects))
    return false;
  next_id = other.next_id;
  vars = other.vars;
  view = other.view;
  return true;
}
Object make_object(Kind kind) {
  Object o;
  o.kind = kind;
  o.name.set(info(kind).name);
  double pi = acos(-1);
  switch (kind) {
  case Kind::Function2:
    o.formula[0].set("a*sin(x)");
    break;
  case Kind::Parametric2:
    o.formula[0].set("3*cos(t)");
    o.formula[1].set("2*sin(t)");
    o.range[0] = 0;
    o.range[1] = 2 * pi;
    break;
  case Kind::Polar2:
    o.formula[0].set("3*cos(3*t)");
    o.range[0] = 0;
    o.range[1] = 2 * pi;
    break;
  case Kind::Implicit2:
    o.formula[0].set("x^2+y^2-9");
    break;
  case Kind::Point2:
    o.p[0] = 1;
    o.p[1] = 2;
    break;
  case Kind::Segment2:
    o.p[0] = -2;
    o.p[1] = -1;
    o.p[2] = 2;
    o.p[3] = 2;
    break;
  case Kind::Vector2:
    o.p[2] = 3;
    o.p[3] = 2;
    break;
  case Kind::Circle2:
    o.p[2] = 3;
    break;
  case Kind::Ellipse2:
    o.p[2] = 4;
    o.p[3] = 2;
    break;
  case Kind::Triangle2:
    o.p[0] = -3;
    o.p[1] = -2;
    o.p[2] = 3;
    o.p[3] = -2;
    o.p[5] = 3;
    break;
  case Kind::Rectangle2:
    o.p[0] = -2;
    o.p[1] = -1;
    o.p[2] = 4;
    o.p[3] = 2;
    break;
  case Kind::Function3:
    o.formula[0].set("a*sin(sqrt(x^2+y^2))");
    break;
  case Kind::Parametric3:
    o.formula[0].set("(3+cos(v))*cos(u)");
    o.formula[1].set("(3+cos(v))*sin(u)");
    o.formula[2].set("sin(v)");
    o.range[0] = o.range[2] = 0;
    o.range[1] = o.range[3] = 2 * pi;
    break;
  case Kind::Curve3:
    o.formula[0].set("2*cos(t)");
    o.formula[1].set("2*sin(t)");
    o.formula[2].set("t/3");
    o.range[0] = -2 * pi;
    o.range[1] = 2 * pi;
    break;
  case Kind::Point3:
    o.p[0] = 1;
    o.p[1] = 2;
    o.p[2] = 3;
    break;
  case Kind::Segment3:
    o.p[3] = 2;
    o.p[4] = 3;
    o.p[5] = 4;
    break;
  case Kind::Vector3:
    o.p[3] = 3;
    o.p[4] = 2;
    o.p[5] = 2;
    break;
  case Kind::Triangle3:
    o.p[0] = -3;
    o.p[1] = -2;
    o.p[3] = 3;
    o.p[4] = -2;
    o.p[7] = 3;
    o.p[8] = 2;
    break;
  case Kind::Plane3:
    o.p[2] = 1;
    break;
  case Kind::Sphere3:
    o.p[3] = 3;
    break;
  case Kind::Box3:
    o.p[0] = o.p[1] = o.p[2] = -1.5;
    o.p[3] = o.p[4] = o.p[5] = 3;
    break;
  case Kind::Cylinder3:
  case Kind::Cone3:
    o.p[2] = -2;
    o.p[3] = 2;
    o.p[4] = 4;
    break;
  case Kind::Count:
    break;
  }
  return o;
}
bool valid_object(const Object &o, Text &err) {
  if (int(o.kind) < 0 || o.kind >= Kind::Count || o.name.empty() ||
      o.name.size() > 128 || o.color < 0 || o.color > 7 || o.resolution < 16 ||
      o.resolution > 96) {
    err.set("对象名称、类型、颜色或采样精度无效");
    return false;
  }
  for (double n : o.p)
    if (!isfinite(n) || fabs(n) > 1e8) {
      err.set("坐标必须为有限数值，绝对值不超过 1e8");
      return false;
    }
  for (double n : o.range)
    if (!isfinite(n) || fabs(n) > 1e6) {
      err.set("范围必须为有限数值，绝对值不超过 1e6");
      return false;
    }
  if (o.range[0] >= o.range[1] || o.range[2] >= o.range[3]) {
    err.set("范围下限必须小于上限");
    return false;
  }
  bool bad = false;
  switch (o.kind) {
  case Kind::Circle2:
    bad = o.p[2] <= 0;
    break;
  case Kind::Ellipse2:
  case Kind::Rectangle2:
    bad = o.p[2] <= 0 || o.p[3] <= 0;
    break;
  case Kind::Sphere3:
    bad = o.p[3] <= 0;
    break;
  case Kind::Cylinder3:
  case Kind::Cone3:
    bad = o.p[3] <= 0 || o.p[4] <= 0;
    break;
  case Kind::Box3:
    bad = o.p[3] <= 0 || o.p[4] <= 0 || o.p[5] <= 0;
    break;
  case Kind::Plane3:
    bad = hypot(hypot(o.p[0], o.p[1]), o.p[2]) < 1e-12;
    break;
  default:
    break;
  }
  if (bad) {
    err.set("半径、长宽高必须为正，平面法向量不能为零");
    return false;
  }
  unsigned mask = 960;
  switch (o.kind) {
  case Kind::Function2:
    mask |= 1;
    break;
  case Kind::Parametric2:
  case Kind::Polar2:
  case Kind::Curve3:
    mask |= 8;
    break;
  case Kind::Implicit2:
  case Kind::Function3:
    mask |= 3;
    break;
  case Kind::Parametric3:
    mask |= 48;
    break;
  default:
    break;
  }
  Expression expr;
  for (int n = 0; n < info(o.kind).formulas; ++n)
    if (!expr.compile(o.formula[n].c_str(), err, mask))
      return false;
  err.clear();
  return true;
}
bool validate(const Document &d, Text &err) {
  if (d.objects.size() > MaxObjects || !d.next_id) {
    err.set("文档对象过多或标识无效");
    return false;
  }
  for (size_t i = 0; i < d.objects.size(); ++i) {
    const Object &o = d.objects[i];
    if (!o.id || o.id >= d.next_id || !valid_object(o, err))
      return false;
    for (size_t j = 0; j < i; ++j)
      if (d.objects[j].id == o.id) {
        err.set("文档含重复对象标识");
        return false;
      }
  }
  for (int n = 0; n < 10; ++n)
    if (!isfinite(d.vars.get(n)) || fabs(d.vars.get(n)) > 1e8) {
      err.set("参数无效");
      return false;
    }
  const View &v = d.view;
  if (!isfinite(v.cx) || !isfinite(v.cy) || !isfinite(v.span) ||
      !isfinite(v.yaw) || !isfinite(v.pitch) || !isfinite(v.scale3) ||
      !isfinite(v.tx) || !isfinite(v.ty) || !isfinite(v.tz) ||
      fabs(v.tx) > 1e8 || fabs(v.ty) > 1e8 || fabs(v.tz) > 1e8 ||
      fabs(v.cx) > 1e8 || fabs(v.cy) > 1e8 || v.span < 1e-5 || v.span > 1e6 ||
      fabs(v.yaw) > 1e4 || fabs(v.pitch) > 1.57 || v.scale3 < .01 ||
      v.scale3 > 100 || v.tab < 0 || v.tab > 1) {
    err.set("视图设置超出范围");
    return false;
  }
  err.clear();
  return true;
}
bool append(Document &d, Object o, uint64_t &id) {
  Text err;
  if (d.objects.size() >= MaxObjects || d.next_id == UINT64_MAX ||
      !valid_object(o, err))
    return false;
  o.id = d.next_id;
  if (!d.objects.push(o))
    return false;
  id = d.next_id++;
  return true;
}
Object *find(Document &d, uint64_t id) {
  for (size_t n = 0; n < d.objects.size(); ++n)
    if (d.objects[n].id == id)
      return &d.objects[n];
  return nullptr;
}
Measures measure(const Object &o) {
  Measures m;
  double pi = acos(-1);
  const double *p = o.p;
  Vec3 a{}, b{}, c{};
  switch (o.kind) {
  case Kind::Segment2:
  case Kind::Vector2:
    m.length = hypot(o.kind == Kind::Segment2 ? p[2] - p[0] : p[2],
                     o.kind == Kind::Segment2 ? p[3] - p[1] : p[3]);
    break;
  case Kind::Circle2:
    m.length = 2 * pi * p[2];
    m.area = pi * p[2] * p[2];
    break;
  case Kind::Ellipse2:
    m.area = pi * p[2] * p[3];
    m.length =
        pi * (3 * (p[2] + p[3]) - sqrt((3 * p[2] + p[3]) * (p[2] + 3 * p[3])));
    break;
  case Kind::Rectangle2:
    m.area = p[2] * p[3];
    m.length = 2 * (p[2] + p[3]);
    break;
  case Kind::Triangle2:
    a = {p[0], p[1], 0};
    b = {p[2], p[3], 0};
    c = {p[4], p[5], 0};
    m.area = norm(cross_product(subtract(b, a), subtract(c, a))) / 2;
    m.length =
        norm(subtract(a, b)) + norm(subtract(b, c)) + norm(subtract(c, a));
    break;
  case Kind::Segment3:
  case Kind::Vector3:
    a = {p[0], p[1], p[2]};
    b = {p[3], p[4], p[5]};
    m.length = norm(o.kind == Kind::Segment3 ? subtract(b, a) : b);
    break;
  case Kind::Triangle3:
    a = {p[0], p[1], p[2]};
    b = {p[3], p[4], p[5]};
    c = {p[6], p[7], p[8]};
    m.area = norm(cross_product(subtract(b, a), subtract(c, a))) / 2;
    m.length =
        norm(subtract(a, b)) + norm(subtract(b, c)) + norm(subtract(c, a));
    break;
  case Kind::Sphere3:
    m.area = 4 * pi * p[3] * p[3];
    m.volume = m.area * p[3] / 3;
    break;
  case Kind::Box3:
    m.area = 2 * (p[3] * p[4] + p[4] * p[5] + p[3] * p[5]);
    m.volume = p[3] * p[4] * p[5];
    break;
  case Kind::Cylinder3:
    m.area = 2 * pi * p[3] * (p[3] + p[4]);
    m.volume = pi * p[3] * p[3] * p[4];
    break;
  case Kind::Cone3:
    m.area = pi * p[3] * (p[3] + hypot(p[3], p[4]));
    m.volume = pi * p[3] * p[3] * p[4] / 3;
    break;
  default:
    return m;
  }
  m.any = true;
  return m;
}
bool example(Document &out, int index) {
  Document d;
  uint64_t id;
  auto add_object = [&](Kind k, int color, const char *name,
                        const char *formula = nullptr) {
    Object o = make_object(k);
    o.color = color;
    o.name.set(name);
    if (formula)
      o.formula[0].set(formula);
    return append(d, o, id);
  };
  bool ok = false;
  if (index == 0) {
    ok = add_object(Kind::Function2, 0, "正弦函数  y = a·sin(x)") &&
         add_object(Kind::Function2, 1, "余弦函数  y = cos(x)", "cos(x)") &&
         add_object(Kind::Circle2, 2, "圆  x²+y² = 9");
    d.view.span = 16;
  } else if (index == 1) {
    ok = add_object(Kind::Function3, 0, "波纹曲面  z = a·sin(r)") &&
         add_object(Kind::Curve3, 3, "空间螺旋线");
    d.view.tab = 1;
  } else if (index == 2) {
    ok = add_object(Kind::Parametric3, 2, "环面") &&
         add_object(Kind::Vector3, 1, "空间向量");
    d.view.tab = 1;
  } else {
    ok = add_object(Kind::Polar2, 3, "玫瑰线  r = 3cos(3t)") &&
         add_object(Kind::Parametric2, 0, "参数椭圆");
  }
  if (!ok)
    return false;
  out = static_cast<Document &&>(d);
  return true;
}
bool History::checkpoint(const Document &d) {
  Document copy;
  if (!copy.clone_from(d) || !past_.push(static_cast<Document &&>(copy)))
    return false;
  if (past_.size() > 32)
    past_.erase(0);
  future_.clear();
  return true;
}
bool History::undo(Document &d) {
  if (past_.empty())
    return false;
  Document copy;
  if (!copy.clone_from(d) || !future_.push(static_cast<Document &&>(copy)))
    return false;
  d = static_cast<Document &&>(past_[past_.size() - 1]);
  past_.erase(past_.size() - 1);
  return true;
}
bool History::redo(Document &d) {
  if (future_.empty())
    return false;
  Document copy;
  if (!copy.clone_from(d) || !past_.push(static_cast<Document &&>(copy)))
    return false;
  d = static_cast<Document &&>(future_[future_.size() - 1]);
  future_.erase(future_.size() - 1);
  return true;
}
} // namespace rmp
