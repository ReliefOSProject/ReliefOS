#include "scene.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
namespace rmp {
uint32_t palette(int index) {
  const uint32_t colors[] = {0x2563bc, 0xd04b38, 0x16866e, 0x8a4bbb,
                             0xc48315, 0x218fa5, 0xc64b82, 0x454f5f};
  return colors[index & 7];
}
namespace {
struct Painter {
  Frame &f;
  bool ok = true;
  void emit(Primitive p) {
    if (ok)
      ok = f.items.push(static_cast<Primitive &&>(p));
  }
  bool clip(double &x, double &y, double &u, double &v) {
    if (!isfinite(x) || !isfinite(y) || !isfinite(u) || !isfinite(v))
      return false;
    double dx = u - x, dy = v - y, lo = 0, hi = 1;
    double p[] = {-dx, dx, -dy, dy}, q[] = {x, f.width - x, y, f.height - y};
    for (int i = 0; i < 4; ++i) {
      if (!p[i]) {
        if (q[i] < 0)
          return false;
        continue;
      }
      double r = q[i] / p[i];
      if (p[i] < 0)
        lo = fmax(lo, r);
      else
        hi = fmin(hi, r);
    }
    if (lo > hi)
      return false;
    u = x + hi * dx;
    v = y + hi * dy;
    x += lo * dx;
    y += lo * dy;
    return true;
  }
  void line(double x, double y, double u, double v, uint32_t c, int width = 1) {
    if (!clip(x, y, u, v))
      return;
    Primitive p;
    p.x[0] = x;
    p.y[0] = y;
    p.x[1] = u;
    p.y[1] = v;
    p.rgb = c;
    p.width = width;
    emit(p);
  }
  void label(double x, double y, const char *s, uint32_t c = 0x596274) {
    if (x < 0 || y < 0 || x > f.width || y > f.height)
      return;
    Primitive p;
    p.type = Primitive::Label;
    p.x[0] = x;
    p.y[0] = y;
    p.rgb = c;
    if (!p.text.set(s)) {
      ok = false;
      return;
    }
    emit(p);
  }
  void dot(double x, double y, uint32_t c) {
    if (x < 0 || y < 0 || x > f.width || y > f.height)
      return;
    Primitive p;
    p.type = Primitive::Dot;
    p.x[0] = x;
    p.y[0] = y;
    p.rgb = c;
    emit(p);
  }
  void triangle(Projected a, Projected b, Projected c, uint32_t color) {
    Projected buf[2][12];
    buf[0][0] = a;
    buf[0][1] = b;
    buf[0][2] = c;
    int size = 3, src = 0;
    for (int edge = 0; edge < 4; ++edge) {
      int count = 0;
      auto distance = [&](Projected q) {
        return edge == 0   ? q.x
               : edge == 1 ? f.width - q.x
               : edge == 2 ? q.y
                           : f.height - q.y;
      };
      for (int i = 0; i < size; ++i) {
        Projected p = buf[src][i], q = buf[src][(i + 1) % size];
        double da = distance(p), db = distance(q);
        if (!isfinite(da) || !isfinite(db))
          return;
        if (da >= 0)
          buf[1 - src][count++] = p;
        if ((da >= 0) != (db >= 0)) {
          double t = da / (da - db);
          buf[1 - src][count++] = {p.x + t * (q.x - p.x), p.y + t * (q.y - p.y),
                                   0};
        }
      }
      size = count;
      src = 1 - src;
      if (!size)
        return;
    }
    for (int i = 1; i < size - 1; ++i) {
      Primitive p;
      p.type = Primitive::Fill;
      p.rgb = color;
      Projected v[] = {buf[src][0], buf[src][i], buf[src][i + 1]};
      for (int j = 0; j < 3; ++j) {
        p.x[j] = v[j].x;
        p.y[j] = v[j].y;
      }
      emit(p);
    }
  }
};
struct Face {
  Projected a, b, c;
  double depth;
  uint32_t color;
};
int compare_faces(const void *p, const void *q) {
  double d =
      static_cast<const Face *>(p)->depth - static_cast<const Face *>(q)->depth;
  return d > 0 ? -1 : d < 0 ? 1 : 0;
}
uint32_t shade(uint32_t rgb, double k) {
  unsigned r = unsigned(((rgb >> 16) & 255) * k),
           g = unsigned(((rgb >> 8) & 255) * k), b = unsigned((rgb & 255) * k);
  return (r << 16) | (g << 8) | b;
}
struct Builder {
  Bytes &b;
  bool ok = true;
  void raw(const char *p, size_t n) {
    size_t pos = b.size();
    if (!ok)
      return;
    ok = n <= MaxFileBytes - pos && b.resize(pos + n);
    if (ok && n)
      memcpy(b.data() + pos, p, n);
  }
  void add(const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0 || size_t(n) >= sizeof(buf)) {
      ok = false;
      return;
    }
    raw(buf, size_t(n));
  }
  void xml(const char *p) {
    for (; *p; ++p) {
      if (static_cast<unsigned char>(*p) < 32 && *p != '\t' && *p != '\n' &&
          *p != '\r')
        continue;
      if (*p == '&')
        raw("&amp;", 5);
      else if (*p == '<')
        raw("&lt;", 4);
      else if (*p == '>')
        raw("&gt;", 4);
      else
        raw(p, 1);
    }
  }
};
} // namespace
bool frame(const Document &d, const Array<Mesh> &meshes, int tab, int width,
           int height, Frame &out) {
  if (width < 1 || height < 1 || width > 8192 || height > 8192)
    return false;
  Frame f;
  f.width = width;
  f.height = height;
  Painter p{f};
  const View &view = d.view;
  auto screen = [&](Vec3 v) {
    if (tab)
      return project(v, view, width, height);
    double s = width / view.span;
    return Projected{width * .5 + (v.x - view.cx) * s,
                     height * .5 - (v.y - view.cy) * s, 0};
  };
  auto worldline = [&](Vec3 a, Vec3 b, uint32_t color, int thick = 1) {
    Projected x = screen(a), y = screen(b);
    p.line(x.x, x.y, y.x, y.y, color, thick);
  };
  if (!tab) {
    double s = width / view.span, raw = view.span / 10,
           unit = pow(10, floor(log10(raw))), ratio = raw / unit;
    double step = (ratio > 5 ? 10 : ratio > 2 ? 5 : ratio > 1 ? 2 : 1) * unit;
    double xmin = view.cx - view.span / 2, xmax = view.cx + view.span / 2,
           half = height / (2 * s), ymin = view.cy - half,
           ymax = view.cy + half;
    double xaxis = fmin(height - 24, fmax(18, height * .5 + view.cy * s)),
           yaxis = fmin(width - 36, fmax(18, width * .5 - view.cx * s));
    for (double x = ceil(xmin / step) * step; x <= xmax && p.ok; x += step) {
      Projected t = screen({x, 0, 0});
      if (view.grid)
        p.line(t.x, 0, t.x, height, 0xe8ecf0);
      if (view.axes) {
        char label[32];
        snprintf(label, sizeof(label), "%.5g", fabs(x) < step * 1e-7 ? 0 : x);
        p.label(t.x + 3, xaxis + 16, label);
      }
    }
    for (double y = ceil(ymin / step) * step; y <= ymax && p.ok; y += step) {
      Projected t = screen({0, y, 0});
      if (view.grid)
        p.line(0, t.y, width, t.y, 0xe8ecf0);
      if (view.axes && fabs(y) > step * 1e-7) {
        char label[32];
        snprintf(label, sizeof(label), "%.5g", y);
        p.label(yaxis + 5, t.y - 4, label);
      }
    }
    if (view.axes) {
      worldline({xmin, 0, 0}, {xmax, 0, 0}, 0x8794a5);
      worldline({0, ymin, 0}, {0, ymax, 0}, 0x8794a5);
      p.label(width - 20, xaxis - 7, "x");
      p.label(yaxis + 8, 15, "y");
    }
  } else {
    if (view.grid)
      for (int i = -5; i <= 5; ++i) {
        worldline({double(i), -5, 0}, {double(i), 5, 0}, 0xe5e9ed);
        worldline({-5, double(i), 0}, {5, double(i), 0}, 0xe5e9ed);
      }
    if (view.axes) {
      const Vec3 end[] = {{6, 0, 0}, {0, 6, 0}, {0, 0, 6}};
      const char *labels[] = {"X", "Y", "Z"};
      const uint32_t colors[] = {0xc14d48, 0x288a61, 0x3b72b2};
      for (int n = 0; n < 3; ++n) {
        worldline(scale(end[n], -1), end[n], colors[n]);
        Projected a = screen(end[n]);
        p.label(a.x + 5, a.y, labels[n], colors[n]);
      }
    }
  }
  Array<Face> faces;
  if (tab && !view.wire) {
    for (size_t n = 0; n < d.objects.size() && n < meshes.size(); ++n) {
      const Object &o = d.objects[n];
      if (!o.visible || !info(o.kind).dim3)
        continue;
      for (size_t i = 0; i < meshes[n].faces.size(); ++i) {
        const Triangle &t = meshes[n].faces[i];
        Vec3 normal = cross_product(subtract(t.b, t.a), subtract(t.c, t.a));
        double length = norm(normal);
        double light =
            length ? fabs(dot(scale(normal, 1 / length), {.3, -.4, .866})) : .5;
        Projected a = screen(t.a), b = screen(t.b), c = screen(t.c);
        Face face{a, b, c, (a.depth + b.depth + c.depth) / 3,
                  shade(palette(o.color), .58 + .42 * light)};
        if (faces.size() >= 150000 || !faces.push(face))
          return false;
      }
    }
    qsort(faces.data(), faces.size(), sizeof(Face), compare_faces);
    for (size_t n = 0; n < faces.size(); ++n)
      p.triangle(faces[n].a, faces[n].b, faces[n].c, faces[n].color);
  }
  for (size_t n = 0; n < d.objects.size() && n < meshes.size(); ++n) {
    const Object &o = d.objects[n];
    if (!o.visible || info(o.kind).dim3 != bool(tab))
      continue;
    const Mesh &m = meshes[n];
    uint32_t color = palette(o.color);
    // Filled surfaces use a restrained wire grid; line objects stay fully
    // visible.
    size_t stride = 1;
    for (size_t i = 0;
         (!tab || view.wire || m.faces.empty()) && i < m.lines.size();
         i += stride)
      worldline(m.lines[i].a, m.lines[i].b,
                tab && !m.faces.empty() ? shade(color, .72) : color,
                tab ? 1 : 2);
    for (size_t i = 0; i < m.points.size(); ++i) {
      Projected a = screen(m.points[i]);
      p.dot(a.x, a.y, color);
    }
    if (o.kind == Kind::Vector2 || o.kind == Kind::Vector3) {
      if (!m.lines.empty()) {
        Projected a = screen(m.lines[0].a), b = screen(m.lines[0].b);
        double angle = atan2(b.y - a.y, b.x - a.x);
        p.line(b.x, b.y, b.x - 11 * cos(angle - .4), b.y - 11 * sin(angle - .4),
               color, 2);
        p.line(b.x, b.y, b.x - 11 * cos(angle + .4), b.y - 11 * sin(angle + .4),
               color, 2);
      }
    }
  }
  // Legends share the same labels and colors in the viewport and exports.
  int row = 0;
  for (size_t n = 0; n < d.objects.size() && row < 8; ++n) {
    const Object &o = d.objects[n];
    if (!o.visible || info(o.kind).dim3 != bool(tab))
      continue;
    p.line(17, 22 + row * 22, 35, 22 + row * 22, palette(o.color), 3);
    p.label(43, 27 + row * 22, o.name.c_str(), palette(o.color));
    ++row;
  }
  if (!row)
    p.label(24, 36,
            tab ? "三维视图：使用工具添加曲面或几何体"
                : "二维视图：添加函数，或选择几何工具在画布上拾点");
  if (!p.ok)
    return false;
  out = static_cast<Frame &&>(f);
  return true;
}
bool svg(const Frame &f, Bytes &out, Text &err) {
  Bytes data;
  Builder b{data};
  b.add("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" "
        "viewBox=\"0 0 %d %d\">\n<rect width=\"100%%\" height=\"100%%\" "
        "fill=\"white\"/>\n",
        f.width, f.height, f.width, f.height);
  for (size_t n = 0; n < f.items.size(); ++n) {
    const Primitive &p = f.items[n];
    if (p.type == Primitive::Line)
      b.add("<path d=\"M%.3f %.3fL%.3f %.3f\" fill=\"none\" stroke=\"#%06x\" "
            "stroke-width=\"%d\"/>\n",
            p.x[0], p.y[0], p.x[1], p.y[1], p.rgb, p.width);
    else if (p.type == Primitive::Fill)
      b.add("<path d=\"M%.3f %.3fL%.3f %.3fL%.3f %.3fZ\" fill=\"#%06x\"/>\n",
            p.x[0], p.y[0], p.x[1], p.y[1], p.x[2], p.y[2], p.rgb);
    else if (p.type == Primitive::Dot)
      b.add("<circle cx=\"%.3f\" cy=\"%.3f\" r=\"4\" fill=\"#%06x\"/>\n",
            p.x[0], p.y[0], p.rgb);
    else {
      b.add("<text x=\"%.3f\" y=\"%.3f\" fill=\"#%06x\" "
            "font-family=\"SimSun,serif\" font-size=\"13\">",
            p.x[0], p.y[0], p.rgb);
      b.xml(p.text.c_str());
      b.add("</text>\n");
    }
  }
  b.add("</svg>\n");
  if (!b.ok) {
    err.set("SVG 超出大小限制或内存不足");
    return false;
  }
  out.swap(data);
  err.clear();
  return true;
}
} // namespace rmp
