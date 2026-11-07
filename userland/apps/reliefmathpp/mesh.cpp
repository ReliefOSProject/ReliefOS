#include "mesh.h"
#include <math.h>
namespace rmp {
bool finite(Vec3 p) {
  return isfinite(p.x) && isfinite(p.y) && isfinite(p.z) && fabs(p.x) < 1e10 &&
         fabs(p.y) < 1e10 && fabs(p.z) < 1e10;
}
Projected project(Vec3 p, const View &v, int width, int height) {
  p = subtract(p, {v.tx, v.ty, v.tz});
  double cy = cos(v.yaw), sy = sin(v.yaw), cp = cos(v.pitch), sp = sin(v.pitch);
  double x = cy * p.x - sy * p.y, y = sy * p.x + cy * p.y;
  double factor = fmin(width, height) * v.scale3 / 14;
  return {width * .5 + x * factor, height * .53 + (sp * y - cp * p.z) * factor,
          cp * y + sp * p.z};
}
namespace {
Vec3 missing() { return {NAN, NAN, NAN}; }
bool smooth(Vec3 a, Vec3 b, Vec3 m) {
  if (!finite(a) || !finite(b) || !finite(m))
    return false;
  return norm(subtract(m, scale(add(a, b), .5))) <
         .15 * (1 + norm(a) + norm(b));
}
} // namespace
Vec3 Sampler::position(double u, double v) const {
  const Object &o = object_;
  const double *p = o.p;
  Variables vars = vars_;
  double pi = acos(-1);
  switch (o.kind) {
  case Kind::Function2:
    vars.x = u;
    return {u, expr_[0].eval(vars), 0};
  case Kind::Parametric2:
    vars.t = u;
    return {expr_[0].eval(vars), expr_[1].eval(vars), 0};
  case Kind::Polar2: {
    vars.t = u;
    double r = expr_[0].eval(vars);
    return {r * cos(u), r * sin(u), 0};
  }
  case Kind::Circle2:
    return {p[0] + p[2] * cos(u), p[1] + p[2] * sin(u), 0};
  case Kind::Ellipse2:
    return {p[0] + p[2] * cos(u), p[1] + p[3] * sin(u), 0};
  case Kind::Function3:
    vars.x = u;
    vars.y = v;
    return {u, v, expr_[0].eval(vars)};
  case Kind::Parametric3:
    vars.u = u;
    vars.v = v;
    return {expr_[0].eval(vars), expr_[1].eval(vars), expr_[2].eval(vars)};
  case Kind::Curve3:
    vars.t = u;
    return {expr_[0].eval(vars), expr_[1].eval(vars), expr_[2].eval(vars)};
  case Kind::Sphere3:
    return {p[0] + p[3] * cos(u) * cos(v), p[1] + p[3] * sin(u) * cos(v),
            p[2] + p[3] * sin(v)};
  case Kind::Cylinder3:
    return {p[0] + p[3] * cos(u), p[1] + p[3] * sin(u), p[2] + p[4] * v};
  case Kind::Cone3:
    return {p[0] + p[3] * (1 - v) * cos(u), p[1] + p[3] * (1 - v) * sin(u),
            p[2] + p[4] * v};
  case Kind::Plane3: {
    // Solve along the strongest normal component, supporting vertical planes.
    int k = fabs(p[0]) > fabs(p[1]) ? 0 : 1;
    if (fabs(p[2]) > fabs(p[k]))
      k = 2;
    if (k == 0)
      return {(p[3] - p[1] * u - p[2] * v) / p[0], u, v};
    if (k == 1)
      return {u, (p[3] - p[0] * u - p[2] * v) / p[1], v};
    return {u, v, (p[3] - p[0] * u - p[1] * v) / p[2]};
  }
  default:
    (void)pi;
    return missing();
  }
}
bool Sampler::line(Mesh &m, Vec3 a, Vec3 b) {
  if (!finite(a) || !finite(b))
    return true;
  if (!m.lines.push(Segment{a, b})) {
    m.error.set("采样内存不足");
    done_ = true;
    return false;
  }
  return true;
}
bool Sampler::face(Mesh &m, Vec3 a, Vec3 b, Vec3 c) {
  if (!finite(a) || !finite(b) || !finite(c) || norm(subtract(a, b)) > 1e5 ||
      norm(subtract(a, c)) > 1e5)
    return true;
  if (!m.faces.push(Triangle{a, b, c})) {
    m.error.set("曲面内存不足");
    done_ = true;
    return false;
  }
  return true;
}
bool Sampler::begin(const Object &o, const Variables &vars, Mesh &m) {
  m = Mesh{};
  object_ = o;
  vars_ = vars;
  row_ = 0;
  done_ = false;
  surface_ = implicit_ = false;
  total_ = 1;
  if (!valid_object(o, m.error)) {
    done_ = true;
    return false;
  }
  for (int n = 0; n < info(o.kind).formulas; ++n)
    if (!expr_[n].compile(o.formula[n].c_str(), m.error)) {
      done_ = true;
      return false;
    }
  if (o.kind == Kind::Implicit2) {
    implicit_ = true;
    total_ = o.resolution * 2;
  } else if (o.kind == Kind::Function3 || o.kind == Kind::Parametric3 ||
             o.kind == Kind::Plane3 || o.kind == Kind::Sphere3 ||
             o.kind == Kind::Cylinder3 || o.kind == Kind::Cone3) {
    surface_ = true;
    total_ = o.resolution;
  } else if (o.kind == Kind::Function2 || o.kind == Kind::Parametric2 ||
             o.kind == Kind::Polar2 || o.kind == Kind::Circle2 ||
             o.kind == Kind::Ellipse2 || o.kind == Kind::Curve3)
    total_ = o.resolution * 16;
  return true;
}
bool Sampler::step(Mesh &m, int budget) {
  if (done_)
    return true;
  const Object &o = object_;
  const double *p = o.p;
  double pi = acos(-1);
  if (implicit_) {
    for (int k = 0; k < budget && row_ < total_; ++k, ++row_) {
      for (int col = 0; col < total_; ++col) {
        double x0 = o.range[0] + (o.range[1] - o.range[0]) * col / total_,
               x1 = o.range[0] + (o.range[1] - o.range[0]) * (col + 1) / total_;
        double y0 = o.range[2] + (o.range[3] - o.range[2]) * row_ / total_,
               y1 =
                   o.range[2] + (o.range[3] - o.range[2]) * (row_ + 1) / total_;
        Vec3 corners[] = {{x0, y0, 0}, {x1, y0, 0}, {x1, y1, 0}, {x0, y1, 0}},
             hits[4];
        double val[4];
        int count = 0;
        for (int i = 0; i < 4; ++i) {
          Variables v = vars_;
          v.x = corners[i].x;
          v.y = corners[i].y;
          val[i] = expr_[0].eval(v);
        }
        for (int i = 0; i < 4; ++i) {
          int j = (i + 1) % 4;
          double a = val[i], b = val[j];
          if (!isfinite(a) || !isfinite(b) || ((a < 0) == (b < 0)))
            continue;
          Vec3 mid = scale(add(corners[i], corners[j]), .5);
          Variables v = vars_;
          v.x = mid.x;
          v.y = mid.y;
          double fm = expr_[0].eval(v);
          if (!isfinite(fm) ||
              fabs(fm - (a + b) * .5) > .8 * (1 + fabs(a) + fabs(b)))
            continue;
          double f = a / (a - b);
          if (isfinite(f))
            hits[count++] =
                add(corners[i], scale(subtract(corners[j], corners[i]), f));
        }
        if (count >= 2 && !line(m, hits[0], hits[1]))
          return true;
        if (count == 4 && !line(m, hits[2], hits[3]))
          return true;
      }
    }
  } else if (surface_) {
    double u0 = o.range[0], u1 = o.range[1], v0 = o.range[2], v1 = o.range[3];
    if (o.kind == Kind::Sphere3) {
      u0 = 0;
      u1 = 2 * pi;
      v0 = -pi / 2;
      v1 = pi / 2;
    }
    if (o.kind == Kind::Cylinder3 || o.kind == Kind::Cone3) {
      u0 = 0;
      u1 = 2 * pi;
      v0 = 0;
      v1 = 1;
    }
    for (int k = 0; k < budget && row_ < total_; ++k, ++row_) {
      double y0 = v0 + (v1 - v0) * row_ / total_,
             y1 = v0 + (v1 - v0) * (row_ + 1) / total_;
      for (int col = 0; col < total_; ++col) {
        double x0 = u0 + (u1 - u0) * col / total_,
               x1 = u0 + (u1 - u0) * (col + 1) / total_;
        Vec3 a = position(x0, y0), b = position(x1, y0), c = position(x1, y1),
             d = position(x0, y1), mid = position((x0 + x1) / 2, (y0 + y1) / 2);
        if (smooth(a, c, mid) && smooth(b, d, mid)) {
          if (!face(m, a, b, c) || !face(m, a, c, d))
            return true;
          if (!line(m, a, b) || !line(m, a, d))
            return true;
          if (row_ == total_ - 1 && !line(m, d, c))
            return true;
          if (col == total_ - 1 && !line(m, b, c))
            return true;
        }
      }
    }
    if (row_ == total_ &&
        (o.kind == Kind::Cylinder3 || o.kind == Kind::Cone3)) {
      Vec3 base{p[0], p[1], p[2]}, top{p[0], p[1], p[2] + p[4]};
      for (int n = 0; n < total_; ++n) {
        double t = 2 * pi * n / total_, s = 2 * pi * (n + 1) / total_;
        if (!face(m, base, position(s, 0), position(t, 0)))
          return true;
        if (o.kind == Kind::Cylinder3 &&
            !face(m, top, position(t, 1), position(s, 1)))
          return true;
      }
    }
  } else if (total_ > 1) {
    double lo = o.range[0], hi = o.range[1];
    if (o.kind == Kind::Circle2 || o.kind == Kind::Ellipse2) {
      lo = 0;
      hi = 2 * pi;
    }
    for (int k = 0; k < budget * 32 && row_ < total_; ++k, ++row_) {
      double t = lo + (hi - lo) * row_ / total_,
             s = lo + (hi - lo) * (row_ + 1) / total_;
      Vec3 a = position(t), b = position(s), mid = position((t + s) / 2);
      if (smooth(a, b, mid) && !line(m, a, b))
        return true;
    }
  } else {
    Vec3 a{}, b{}, c{};
    bool dim3 = info(o.kind).dim3;
    a = {p[0], p[1], dim3 ? p[2] : 0};
    switch (o.kind) {
    case Kind::Point2:
    case Kind::Point3:
      if (!m.points.push(a))
        m.error.set("内存不足");
      break;
    case Kind::Segment2:
    case Kind::Vector2:
    case Kind::Segment3:
    case Kind::Vector3:
      b = dim3 ? Vec3{p[3], p[4], p[5]} : Vec3{p[2], p[3], 0};
      if (o.kind == Kind::Vector2 || o.kind == Kind::Vector3)
        b = add(a, b);
      line(m, a, b);
      m.points.push(b);
      break;
    case Kind::Triangle2:
    case Kind::Triangle3:
      b = dim3 ? Vec3{p[3], p[4], p[5]} : Vec3{p[2], p[3], 0};
      c = dim3 ? Vec3{p[6], p[7], p[8]} : Vec3{p[4], p[5], 0};
      line(m, a, b);
      line(m, b, c);
      line(m, c, a);
      face(m, a, b, c);
      break;
    case Kind::Rectangle2:
      b = {p[0] + p[2], p[1], 0};
      c = {p[0] + p[2], p[1] + p[3], 0};
      line(m, a, b);
      line(m, b, c);
      line(m, c, {p[0], p[1] + p[3], 0});
      line(m, {p[0], p[1] + p[3], 0}, a);
      break;
    case Kind::Box3: {
      Vec3 q[8];
      for (int n = 0; n < 8; ++n)
        q[n] = {p[0] + (n & 1 ? p[3] : 0), p[1] + (n & 2 ? p[4] : 0),
                p[2] + (n & 4 ? p[5] : 0)};
      for (int n = 0; n < 8; ++n)
        for (int bit = 1; bit < 8; bit *= 2)
          if (!(n & bit))
            line(m, q[n], q[n | bit]);
      const int f[][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4},
                          {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
      for (const auto &quad : f) {
        face(m, q[quad[0]], q[quad[1]], q[quad[2]]);
        face(m, q[quad[0]], q[quad[2]], q[quad[3]]);
      }
      break;
    }
    default:
      break;
    }
    row_ = 1;
  }
  done_ = row_ >= total_;
  if (done_ && m.error.empty() && m.lines.empty() && m.faces.empty() &&
      m.points.empty())
    m.error.set("此范围内没有可绘制的有限值");
  return done_;
}
} // namespace rmp
