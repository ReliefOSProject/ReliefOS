#include "render.h"
#include "../fltk_pixels.h"
#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_Widget.H>
#include <math.h>
namespace rc {
void draw_mark(const Mark &m, Rect view, int iw, int ih) {
  if (!view.valid() || iw <= 0 || ih <= 0) return;
  double scale = double(view.w) / iw;
  auto sx = [&](int x) { return view.x + int(lround(double(x) * view.w / iw)); };
  auto sy = [&](int y) { return view.y + int(lround(double(y) * view.h / ih)); };
  fl_color(fl_rgb_color(m.color >> 16, m.color >> 8, m.color));
  int line = int(lround(m.width * scale)); if (line < 1) line = 1;
  fl_line_style(FL_SOLID | FL_CAP_ROUND | FL_JOIN_ROUND, line);
  int ax = sx(m.a.x), ay = sy(m.a.y), bx = sx(m.b.x), by = sy(m.b.y);
  if (m.tool == Tool::Pen) {
    if (m.points.size() == 1) fl_pie(sx(m.points[0].x) - line / 2, sy(m.points[0].y) - line / 2, line + 1, line + 1, 0, 360);
    for (size_t n = 1; n < m.points.size(); ++n)
      fl_line(sx(m.points[n - 1].x), sy(m.points[n - 1].y), sx(m.points[n].x), sy(m.points[n].y));
  } else if (m.tool == Tool::Rectangle) {
    fl_rect(ax < bx ? ax : bx, ay < by ? ay : by, abs(bx - ax) + 1, abs(by - ay) + 1);
  } else if (m.tool == Tool::Arrow) {
    fl_line(ax, ay, bx, by);
    double angle = atan2(double(by - ay), double(bx - ax));
    double length = fmax(12.0, m.width * 4.0) * scale;
    fl_polygon(bx, by, bx - int(lround(length * cos(angle - .45))), by - int(lround(length * sin(angle - .45))),
               bx - int(lround(length * cos(angle + .45))), by - int(lround(length * sin(angle + .45))));
  } else {
    int font = int(lround(m.font * scale)); if (font < 1) font = 1;
    fl_font(FL_HELVETICA, font);
    fl_draw(m.text.c_str(), ax, ay, view.x + view.w - ax, view.y + view.h - ay,
            FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE);
  }
  fl_line_style(0);
}
void draw_document(const Document &doc, Rect view, const Mark *pending) {
  const auto &image = doc.image();
  if (!image.valid() || !view.valid()) return;
  fl_push_clip(view.x, view.y, view.w, view.h);
  reliefos_fltk::draw_pixels(image.pixels.data(), image.width, image.height, view.x, view.y, view.w, view.h);
  for (size_t n = 0; n < doc.visible(); ++n) draw_mark(doc.marks()[n], view, image.width, image.height);
  if (pending) draw_mark(*pending, view, image.width, image.height);
  fl_pop_clip();
}
namespace {
class Picture : public Fl_Widget {
  const Document &doc_;
public:
  Picture(const Document &doc) : Fl_Widget(0, 0, doc.image().width, doc.image().height), doc_(doc) {}
  void draw() override { draw_document(doc_, {x(), y(), w(), h()}); }
};
}
bool render_document(const Document &doc, Image &out, Text &error) {
  if (!doc.image().valid()) { error.set("There is no screenshot to save"); return false; }
  if (!doc.visible()) return doc.image().crop({0, 0, doc.image().width, doc.image().height}, out);
  Fl_Group *previous = Fl_Group::current(); Fl_Group::current(nullptr);
  Picture picture(doc); Fl_Group::current(previous);
  Fl_Image_Surface surface(doc.image().width, doc.image().height);
  surface.set_current(); surface.draw(&picture); Fl_RGB_Image *rgb = surface.image();
  Fl_Display_Device::display_device()->set_current();
  bool ok = rgb && rgb->d() >= 3 && rgb->w() == doc.image().width && rgb->h() == doc.image().height &&
            out.create(rgb->w(), rgb->h());
  if (ok) {
    int stride = rgb->ld() ? rgb->ld() : rgb->w() * rgb->d();
    const auto *bytes = reinterpret_cast<const unsigned char *>(rgb->data()[0]);
    for (int y = 0; y < rgb->h(); ++y) for (int x = 0; x < rgb->w(); ++x) {
      const auto *p = bytes + size_t(y) * stride + size_t(x) * rgb->d();
      out.pixels[size_t(y) * rgb->w() + x] = (unsigned(p[0]) << 16) | (unsigned(p[1]) << 8) | p[2];
    }
  }
  delete rgb;
  if (!ok) error.set("Could not render the screenshot");
  return ok;
}
}
