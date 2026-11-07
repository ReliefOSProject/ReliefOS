#include "scene.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
namespace rd {
namespace {
constexpr double Pi = 3.14159265358979323846;
double cross(Point a, Point b) { return a.x * b.y - a.y * b.x; }
Point minus(Point a, Point b) { return {a.x - b.x, a.y - b.y}; }
void point(Stroke &s, double x, double y) {
  if (s.count < 64)
    s.points[s.count++] = {x, y};
}
double glyph(unsigned char c, int font) { return font * (c < 128 ? .58 : 1.0); }
size_t char_size(unsigned char c) {
  return c < 128 ? 1 : c < 224 ? 2 : c < 240 ? 3 : 4;
}
double text_width(const Text &s, int font) {
  double w = 0;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = s.c_str()[i];
    w += glyph(c, font);
    i += char_size(c);
  }
  return w;
}
bool label_lines(Frame &f, const Node &n) {
  Array<Text> lines;
  size_t at = 0;
  double available = n.w - 24;
  if (n.shape == Shape::Diamond)
    available = n.w * .58;
  if (n.shape == Shape::Ellipse)
    available = n.w * .75;
  int maximum = int(fmax(1, floor((n.h - 16) / (n.style.font * 1.3))));
  if (n.shape == Shape::Diamond)
    maximum = int(fmax(1, floor(n.h * .56 / (n.style.font * 1.3))));
  while (at < n.text.size() && int(lines.size()) < maximum) {
    size_t start = at, space = size_t(-1);
    double width = 0;
    while (at < n.text.size()) {
      unsigned char c = n.text.c_str()[at];
      if (c == '\n')
        break;
      if (c == ' ' || c == '\t')
        space = at;
      double next = glyph(c, n.style.font);
      if (width + next > available && at > start)
        break;
      width += next;
      at += char_size(c);
    }
    if (at < n.text.size() && n.text.c_str()[at] != '\n' &&
        space != size_t(-1) && space > start)
      at = space;
    size_t end = at;
    while (end > start &&
           (n.text.c_str()[end - 1] == ' ' || n.text.c_str()[end - 1] == '\t'))
      --end;
    Text line;
    if (!line.set(n.text.c_str() + start, end - start) || !lines.push(line))
      return false;
    while (at < n.text.size() &&
           (n.text.c_str()[at] == ' ' || n.text.c_str()[at] == '\t'))
      ++at;
    if (at < n.text.size() && n.text.c_str()[at] == '\n')
      ++at;
  }
  if (at < n.text.size() && !lines.empty()) {
    Text &last = lines[lines.size() - 1];
    char buf[4100];
    size_t used = last.size();
    while (used &&
           (static_cast<unsigned char>(last.c_str()[used - 1]) & 0xc0) == 0x80)
      --used;
    if (used)
      --used;
    memcpy(buf, last.c_str(), used);
    memcpy(buf + used, "…", 3);
    if (!last.set(buf, used + 3))
      return false;
  }
  if (lines.empty())
    return true;
  double y = n.y + n.h / 2 - (lines.size() - 1) * n.style.font * .65 +
             n.style.font * .34;
  for (size_t i = 0; i < lines.size(); ++i) {
    Label l;
    l.text = lines[i];
    l.at = {n.x + n.w / 2 - text_width(l.text, n.style.font) / 2,
            y + i * n.style.font * 1.3};
    l.font = n.style.font;
    l.bold = n.style.bold;
    l.color = n.style.text;
    if (!f.labels.push(l))
      return false;
  }
  return true;
}
Point midpoint(const Stroke &s) {
  double length = 0;
  for (unsigned i = 1; i < s.count; ++i)
    length += hypot(s.points[i].x - s.points[i - 1].x,
                    s.points[i].y - s.points[i - 1].y);
  double walked = 0;
  for (unsigned i = 1; i < s.count; ++i) {
    Point a = s.points[i - 1], b = s.points[i];
    double step = hypot(b.x - a.x, b.y - a.y);
    if (walked + step >= length / 2 && step) {
      double t = (length / 2 - walked) / step;
      return {a.x + t * (b.x - a.x), a.y + t * (b.y - a.y)};
    }
    walked += step;
  }
  return s.count ? s.points[0] : Point{};
}
bool arrow(Frame &f, Point tip, Point before, uint32_t color, double width) {
  double angle = atan2(tip.y - before.y, tip.x - before.x), len = 10 + width;
  Stroke s;
  s.closed = s.filled = true;
  s.fill = s.line = color;
  s.width = 1;
  point(s, tip.x, tip.y);
  point(s, tip.x - len * cos(angle - .42), tip.y - len * sin(angle - .42));
  point(s, tip.x - len * cos(angle + .42), tip.y - len * sin(angle + .42));
  return f.paths.push(s);
}
struct Builder {
  Bytes b;
  bool ok = true;
  void raw(const char *p, size_t n) {
    size_t at = b.size();
    if (!ok)
      return;
    ok = n <= MaxFileBytes - at && b.resize(at + n);
    if (ok && n)
      memcpy(b.data() + at, p, n);
  }
  void text(const char *s) { raw(s, strlen(s)); }
  void format(const char *fmt, ...) {
    char buf[512];
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
  void xml(const Text &s) {
    for (size_t i = 0; i < s.size(); ++i) {
      char c = s.c_str()[i];
      if (c == '&')
        text("&amp;");
      else if (c == '<')
        text("&lt;");
      else if (c == '>')
        text("&gt;");
      else if (c == '\"')
        text("&quot;");
      else if (c == '\'')
        text("&apos;");
      else
        raw(&c, 1);
    }
  }
};
} // namespace
Stroke outline(const Node &n) {
  Stroke s;
  s.closed = s.filled = true;
  s.fill = n.style.fill;
  s.line = n.style.line;
  s.owner = n.id;
  double x = n.x, y = n.y, w = n.w, h = n.h;
  if (n.shape == Shape::Ellipse) {
    for (int i = 0; i < 48; ++i) {
      double a = i * 2 * Pi / 48;
      point(s, x + w / 2 + cos(a) * w / 2, y + h / 2 + sin(a) * h / 2);
    }
  } else if (n.shape == Shape::Rounded) {
    double r = fmin(12, fmin(w, h) / 4),
           cx[] = {x + w - r, x + w - r, x + r, x + r},
           cy[] = {y + r, y + h - r, y + h - r, y + r};
    for (int corner = 0; corner < 4; ++corner)
      for (int i = 0; i <= 6; ++i) {
        double a = (-1 + corner) * Pi / 2 + i * Pi / 12;
        point(s, cx[corner] + r * cos(a), cy[corner] + r * sin(a));
      }
  } else if (n.shape == Shape::Diamond) {
    point(s, x + w / 2, y);
    point(s, x + w, y + h / 2);
    point(s, x + w / 2, y + h);
    point(s, x, y + h / 2);
  } else if (n.shape == Shape::Parallelogram) {
    double t = fmin(20, w / 5);
    point(s, x + t, y);
    point(s, x + w, y);
    point(s, x + w - t, y + h);
    point(s, x, y + h);
  } else if (n.shape == Shape::Document) {
    point(s, x, y);
    point(s, x + w, y);
    point(s, x + w, y + h - 9);
    for (int i = 1; i <= 20; ++i) {
      double t = i / 20.0;
      point(s, x + w * (1 - t), y + h - 9 + sin(t * 2 * Pi) * 8);
    }
  } else if (n.shape == Shape::Database) {
    double r = fmin(12, h / 5);
    for (int i = 0; i <= 20; ++i) {
      double a = Pi + i * Pi / 20;
      point(s, x + w / 2 + cos(a) * w / 2, y + r + sin(a) * r);
    }
    for (int i = 0; i <= 20; ++i) {
      double a = i * Pi / 20;
      point(s, x + w / 2 + cos(a) * w / 2, y + h - r + sin(a) * r);
    }
  } else if (n.shape == Shape::Note) {
    point(s, x, y);
    point(s, x + w - 18, y);
    point(s, x + w, y + 18);
    point(s, x + w, y + h);
    point(s, x, y + h);
  } else {
    point(s, x, y);
    point(s, x + w, y);
    point(s, x + w, y + h);
    point(s, x, y + h);
  }
  return s;
}
Point anchor(const Node &n, Point towards) {
  Point center{n.x + n.w / 2, n.y + n.h / 2}, ray = minus(towards, center);
  if (!ray.x && !ray.y)
    ray.x = 1;
  Stroke s = outline(n);
  double best = 1e30;
  for (unsigned i = 0; i < s.count; ++i) {
    Point a = s.points[i], b = s.points[(i + 1) % s.count], v = minus(b, a),
          offset = minus(a, center);
    double det = cross(ray, v);
    if (fabs(det) < 1e-12)
      continue;
    double t = cross(offset, v) / det, u = cross(offset, ray) / det;
    if (t >= 0 && u >= -1e-9 && u <= 1 + 1e-9)
      best = fmin(best, t);
  }
  return best < 1e29 ? Point{center.x + best * ray.x, center.y + best * ray.y}
                     : center;
}
Stroke connection(const Node &a, const Node &b, Route route) {
  Stroke s;
  Point ac{a.x + a.w / 2, a.y + a.h / 2}, bc{b.x + b.w / 2, b.y + b.h / 2};
  if (route == Route::Orthogonal && bc.y < ac.y &&
      fabs(bc.x - ac.x) < fmax(a.w, b.w) * .5) {
    double lane = fmin(a.x, b.x) - 48;
    Point p = anchor(a, {lane, ac.y}), q = anchor(b, {lane, bc.y});
    point(s, p.x, p.y);
    point(s, lane, p.y);
    point(s, lane, q.y);
    point(s, q.x, q.y);
    return s;
  }
  if (route == Route::Straight) {
    Point x = anchor(a, bc), y = anchor(b, ac);
    point(s, x.x, x.y);
    point(s, y.x, y.y);
    return s;
  }
  bool horizontal = fabs(bc.x - ac.x) > fabs(bc.y - ac.y);
  Point p = anchor(a, horizontal ? Point{bc.x, ac.y} : Point{ac.x, bc.y});
  Point q = anchor(b, horizontal ? Point{ac.x, bc.y} : Point{bc.x, ac.y});
  if (route == Route::Orthogonal) {
    point(s, p.x, p.y);
    if (horizontal) {
      double x = (p.x + q.x) / 2;
      point(s, x, p.y);
      point(s, x, q.y);
    } else {
      double y = (p.y + q.y) / 2;
      point(s, p.x, y);
      point(s, q.x, y);
    }
    point(s, q.x, q.y);
  } else {
    Point c1 = p, c2 = q;
    if (horizontal) {
      double dx = fmax(48, fabs(q.x - p.x) * .5) * (bc.x >= ac.x ? 1 : -1);
      c1.x += dx;
      c2.x -= dx;
      if (fabs(p.y - q.y) < 1) {
        c1.y -= 40;
        c2.y -= 40;
      }
    } else {
      double dy = fmax(48, fabs(q.y - p.y) * .5) * (bc.y >= ac.y ? 1 : -1);
      c1.y += dy;
      c2.y -= dy;
      if (fabs(p.x - q.x) < 1) {
        c1.x += 40;
        c2.x += 40;
      }
    }
    for (int i = 0; i <= 24; ++i) {
      double t = i / 24.0, u = 1 - t;
      point(s,
            u * u * u * p.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x +
                t * t * t * q.x,
            u * u * u * p.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y +
                t * t * t * q.y);
    }
  }
  return s;
}
bool inside(const Node &n, Point p) {
  Stroke s = outline(n);
  bool hit = false;
  for (unsigned i = 0, j = s.count - 1; i < s.count; j = i++) {
    Point a = s.points[i], b = s.points[j];
    if ((a.y > p.y) != (b.y > p.y) &&
        p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
      hit = !hit;
  }
  return hit;
}
double distance(const Stroke &s, Point p) {
  double result = 1e30;
  for (unsigned i = 1; i < s.count; ++i) {
    Point a = s.points[i - 1], b = s.points[i], v = minus(b, a);
    double length = v.x * v.x + v.y * v.y;
    double t =
        length
            ? fmax(0, fmin(1, ((p.x - a.x) * v.x + (p.y - a.y) * v.y) / length))
            : 0;
    result = fmin(result, hypot(p.x - a.x - t * v.x, p.y - a.y - t * v.y));
  }
  return result;
}
bool build_frame(const Document &d, Frame &out, Text &e) {
  if (!validate(d, e))
    return false;
  Frame f;
  TreeIndex tree(d);
  bool ok = true;
  for (size_t i = 0; i < d.nodes.size(); ++i) {
    const Node &n = d.nodes[i];
    if (!n.parent || !tree.shown(i))
      continue;
    const Node *p = node(d, n.parent);
    if (!p)
      continue;
    Stroke s = connection(*p, n, Route::Curve);
    s.line = n.style.line;
    s.width = 2;
    ok = ok && f.paths.push(s);
  }
  for (size_t i = 0; i < d.edges.size(); ++i) {
    const Edge &a = d.edges[i];
    int fi = tree.index(a.from), ti = tree.index(a.to);
    if (fi < 0 || ti < 0 || !tree.shown(size_t(fi)) || !tree.shown(size_t(ti)))
      continue;
    const Node *from = &d.nodes[size_t(fi)], *to = &d.nodes[size_t(ti)];
    Stroke s = connection(*from, *to, a.route);
    s.line = a.color;
    s.width = a.width;
    s.owner = a.id;
    ok = ok && f.paths.push(s);
    if (a.arrows)
      ok = ok && arrow(f, s.points[s.count - 1], s.points[s.count - 2], a.color,
                       a.width);
    if (a.arrows == 2)
      ok = ok && arrow(f, s.points[0], s.points[1], a.color, a.width);
    if (!a.text.empty()) {
      Text shortened;
      size_t len = a.text.size(), at = 0;
      int chars = 0;
      while (at < len && chars++ < 60)
        at += char_size(static_cast<unsigned char>(a.text.c_str()[at]));
      ok = ok && shortened.set(a.text.c_str(), at);
      Point m = midpoint(s);
      double width = text_width(shortened, 13) + 12;
      Stroke bg;
      bg.filled = bg.closed = true;
      bg.width = 0;
      bg.fill = 0xffffff;
      point(bg, m.x - width / 2, m.y - 11);
      point(bg, m.x + width / 2, m.y - 11);
      point(bg, m.x + width / 2, m.y + 10);
      point(bg, m.x - width / 2, m.y + 10);
      Label l;
      l.at = {m.x - width / 2 + 6, m.y + 4};
      l.font = 13;
      l.color = 0x41516a;
      l.text = shortened;
      ok = ok && f.paths.push(bg) && f.labels.push(l);
    }
  }
  for (size_t i = 0; i < d.nodes.size(); ++i) {
    const Node &n = d.nodes[i];
    if (!tree.shown(i))
      continue;
    ok = ok && f.visible_nodes.push(n.id) && f.paths.push(outline(n)) &&
         label_lines(f, n);
    if (n.shape == Shape::Database) {
      Stroke s;
      s.line = n.style.line;
      double r = fmin(12, n.h / 5);
      for (int j = 0; j <= 24; ++j) {
        double t = j * 2 * Pi / 24;
        point(s, n.x + n.w / 2 + cos(t) * n.w / 2, n.y + r + sin(t) * r);
      }
      ok = ok && f.paths.push(s);
    } else if (n.shape == Shape::Note) {
      Stroke s;
      s.line = n.style.line;
      point(s, n.x + n.w - 18, n.y);
      point(s, n.x + n.w - 18, n.y + 18);
      point(s, n.x + n.w, n.y + 18);
      ok = ok && f.paths.push(s);
    }
    if (n.collapsed) {
      Label l;
      l.text.set("＋");
      l.font = 13;
      l.at = {n.x + n.w - 18, n.y + n.h - 6};
      l.color = n.style.line;
      ok = ok && f.labels.push(l);
    }
  }
  if (!ok) {
    e.set("绘图内存不足");
    return false;
  }
  bool first = true;
  double x = 0, y = 0, right = 0, bottom = 0;
  auto bounds = [&](double a, double b) {
    if (first) {
      x = right = a;
      y = bottom = b;
      first = false;
    } else {
      x = fmin(x, a);
      y = fmin(y, b);
      right = fmax(right, a);
      bottom = fmax(bottom, b);
    }
  };
  for (size_t i = 0; i < f.paths.size(); ++i)
    for (unsigned j = 0; j < f.paths[i].count; ++j)
      bounds(f.paths[i].points[j].x, f.paths[i].points[j].y);
  for (size_t i = 0; i < f.labels.size(); ++i) {
    const Label &l = f.labels[i];
    bounds(l.at.x, l.at.y - l.font);
    bounds(l.at.x + text_width(l.text, l.font), l.at.y + l.font * .3);
  }
  f.bounds = first ? Rect{-100, -80, 200, 160}
                   : Rect{x - 4, y - 4, fmax(1, right - x + 8),
                          fmax(1, bottom - y + 8)};
  out = static_cast<Frame &&>(f);
  e.clear();
  return true;
}
bool svg(const Frame &f, Bytes &out, Text &e) {
  Builder b;
  Rect r = f.bounds;
  r.x -= 24;
  r.y -= 24;
  r.w += 48;
  r.h += 48;
  b.text("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
  b.format("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"%.3f %.3f %.3f "
           "%.3f\" width=\"%.0f\" height=\"%.0f\">\n",
           r.x, r.y, r.w, r.h, ceil(r.w), ceil(r.h));
  for (size_t i = 0; i < f.paths.size(); ++i) {
    const Stroke &s = f.paths[i];
    if (!s.count)
      continue;
    b.text("<path d=\"");
    for (unsigned j = 0; j < s.count; ++j)
      b.format(j ? " L%.3f %.3f" : "M%.3f %.3f", s.points[j].x, s.points[j].y);
    if (s.closed)
      b.text(" Z");
    b.text("\" fill=\"");
    if (s.filled)
      b.format("#%06x", s.fill);
    else
      b.text("none");
    b.format("\" stroke=\"#%06x\" stroke-width=\"%.2f\" "
             "stroke-linejoin=\"round\" stroke-linecap=\"round\"/>\n",
             s.line, s.width);
  }
  for (size_t i = 0; i < f.labels.size(); ++i) {
    const Label &l = f.labels[i];
    b.format("<text x=\"%.3f\" y=\"%.3f\" fill=\"#%06x\" font-family=\"SimSun, "
             "sans-serif\" font-size=\"%d\" font-weight=\"%s\" "
             "xml:space=\"preserve\">",
             l.at.x, l.at.y, l.color, l.font, l.bold ? "bold" : "normal");
    b.xml(l.text);
    b.text("</text>\n");
  }
  b.text("</svg>\n");
  if (!b.ok) {
    e.set("SVG 超过限制或内存不足");
    return false;
  }
  out.swap(b.b);
  e.clear();
  return true;
}
} // namespace rd
