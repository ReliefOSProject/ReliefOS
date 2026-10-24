#include "model.h"
namespace rc {
namespace {
int clamp(int v, int low, int high) { return v < low ? low : v > high ? high : v; }
bool dimensions(int w, int h) {
  return w > 0 && h > 0 && w <= MaxSide && h <= MaxSide &&
         uint64_t(w) * unsigned(h) <= MaxPixels;
}
}
bool Rect::contains(int px, int py) const {
  return valid() && int64_t(px) >= x && int64_t(py) >= y &&
         int64_t(px) < int64_t(x) + w && int64_t(py) < int64_t(y) + h;
}
Rect selection(int x0, int y0, int x1, int y1, int width, int height) {
  if (width <= 0 || height <= 0) return {};
  x0 = clamp(x0, 0, width); x1 = clamp(x1, 0, width);
  y0 = clamp(y0, 0, height); y1 = clamp(y1, 0, height);
  return {x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1,
          x0 < x1 ? x1 - x0 : x0 - x1, y0 < y1 ? y1 - y0 : y0 - y1};
}
Rect fit_rect(int iw, int ih, Rect available, bool natural) {
  if (!dimensions(iw, ih) || !available.valid()) return {};
  int w = iw, h = ih;
  if (!natural && (w > available.w || h > available.h)) {
    if (int64_t(available.w) * ih <= int64_t(available.h) * iw) {
      w = available.w; h = int(int64_t(ih) * w / iw);
    } else { h = available.h; w = int(int64_t(iw) * h / ih); }
  }
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  return {available.x + (available.w > w ? (available.w - w) / 2 : 0),
          available.y + (available.h > h ? (available.h - h) / 2 : 0), w, h};
}
bool image_point(Rect view, int iw, int ih, int x, int y, Point &out) {
  if (!dimensions(iw, ih) || !view.contains(x, y)) return false;
  out = {int((int64_t(x) - view.x) * iw / view.w),
         int((int64_t(y) - view.y) * ih / view.h)};
  return true;
}
bool valid_utf8(const char *s, size_t n) {
  if (!s) return false;
  for (size_t i = 0; i < n;) {
    unsigned a = (unsigned char)s[i++], value, need, minimum;
    if (!a) return false;
    if (a < 128) continue;
    if (a >= 0xc2 && a <= 0xdf) { value = a & 31; need = 1; minimum = 128; }
    else if (a >= 0xe0 && a <= 0xef) { value = a & 15; need = 2; minimum = 2048; }
    else if (a >= 0xf0 && a <= 0xf4) { value = a & 7; need = 3; minimum = 65536; }
    else return false;
    if (need > n - i) return false;
    while (need--) {
      unsigned b = (unsigned char)s[i++];
      if ((b & 0xc0) != 0x80) return false;
      value = (value << 6) | (b & 63);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
  }
  return true;
}
bool Image::create(int w, int h) {
  if (!dimensions(w, h)) return false;
  Array<uint32_t> next;
  if (!next.resize(size_t(w) * h)) return false;
  pixels = static_cast<Array<uint32_t> &&>(next); width = w; height = h;
  return true;
}
bool Image::valid() const {
  return dimensions(width, height) && pixels.size() == size_t(width) * height;
}
bool Image::crop(Rect r, Image &out) const {
  if (!valid() || !r.valid() || r.x < 0 || r.y < 0 ||
      int64_t(r.x) + r.w > width || int64_t(r.y) + r.h > height) return false;
  Image next;
  if (!next.create(r.w, r.h)) return false;
  for (int y = 0; y < r.h; ++y)
    memcpy(next.pixels.data() + size_t(y) * r.w,
           pixels.data() + size_t(y + r.y) * width + r.x, size_t(r.w) * 4);
  out = static_cast<Image &&>(next); return true;
}
void Document::replace(Image &&image) {
  image_ = static_cast<Image &&>(image); marks_.reset(); visible_ = 0;
}
bool Document::add(Mark &&mark) {
  const auto &im = image_;
  auto point_ok = [&](Point p) { return p.x >= 0 && p.y >= 0 && p.x < im.width && p.y < im.height; };
  if (!im.valid() || visible_ >= MaxMarks || !point_ok(mark.a) || !point_ok(mark.b) ||
      !mark.width || mark.width > 32 || mark.color > 0xffffff ||
      mark.font < 10 || mark.font > 96 || unsigned(mark.tool) > unsigned(Tool::Label)) return false;
  if (mark.tool == Tool::Pen) {
    if (mark.points.empty() || mark.points.size() > MaxPoints) return false;
    for (size_t i = 0; i < mark.points.size(); ++i) if (!point_ok(mark.points[i])) return false;
  } else if (mark.tool == Tool::Label) {
    if (mark.text.empty() || mark.text.size() > 1024 ||
        !valid_utf8(mark.text.c_str(), mark.text.size())) return false;
  } else if (mark.a.x == mark.b.x && mark.a.y == mark.b.y) return false;
  if (!marks_.reserve(visible_ + 1)) return false;
  marks_.erase(visible_, marks_.size() - visible_);
  if (!marks_.push(static_cast<Mark &&>(mark))) return false;
  ++visible_; return true;
}
bool Document::undo() { if (!visible_) return false; --visible_; return true; }
bool Document::redo() { if (visible_ >= marks_.size()) return false; ++visible_; return true; }
}
