#include "overlay.h"
#include "../fltk_pixels.h"
#include <FL/x.H>
#include <stdio.h>
namespace rc {
Overlay::Overlay(Snapshot &&snapshot, bool winmode, void (*finished)(Rect, bool, void *), void *data)
    : Fl_Double_Window(0, 0, snapshot.image.width, snapshot.image.height),
      snapshot_(static_cast<Snapshot &&>(snapshot)), finished_(finished), data_(data) {
  set_override(); begin();
  int width = w() < 610 ? w() - 16 : 594;
  bar_ = new Fl_Group((w() - width) / 2, 12, width, 82);
  bar_->box(FL_UP_BOX); bar_->begin();
  int bx = bar_->x() + 12, by = bar_->y() + 10, bw = (width - 48) / 4;
  region_ = new Fl_Button(bx, by, bw, 30, T("Region")); region_->type(FL_TOGGLE_BUTTON);
  region_->callback([](Fl_Widget *, void *p) { static_cast<Overlay *>(p)->mode(false); }, this);
  window_ = new Fl_Button(bx + bw + 8, by, bw, 30, T("Window")); window_->type(FL_TOGGLE_BUTTON);
  window_->callback([](Fl_Widget *, void *p) { static_cast<Overlay *>(p)->mode(true); }, this);
  auto *full = new Fl_Button(bx + (bw + 8) * 2, by, bw, 30, T("Full screen"));
  full->callback([](Fl_Widget *, void *p) {
    auto *self = static_cast<Overlay *>(p); self->selected_ = {0, 0, self->w(), self->h()}; self->finish(false);
  }, this);
  auto *cancel = new Fl_Button(bx + (bw + 8) * 3, by, bw, 30, T("Cancel"));
  cancel->callback([](Fl_Widget *, void *p) { static_cast<Overlay *>(p)->finish(true); }, this);
  auto *hint = new Fl_Box(bar_->x() + 8, by + 36, width - 16, 22,
                         T("Drag a region · R: region · W: window · F: full screen · Esc: cancel"));
  hint->labelsize(12);
  bar_->end(); end();
  callback([](Fl_Widget *, void *p) { static_cast<Overlay *>(p)->finish(true); }, this);
  size_t pixels = snapshot_.image.pixels.size();
  if (original_pixels_.resize(pixels * 3) && dim_pixels_.resize(pixels * 3)) {
    for (size_t n = 0; n < pixels; ++n) {
      uint32_t c = snapshot_.image.pixels[n];
      for (unsigned channel = 0; channel < 3; ++channel) {
        unsigned char value = (c >> (16 - channel * 8)) & 255;
        original_pixels_[n * 3 + channel] = value; dim_pixels_[n * 3 + channel] = value * 4 / 10;
      }
    }
    original_ = new Fl_RGB_Image(original_pixels_.data(), w(), h(), 3);
    dim_ = new Fl_RGB_Image(dim_pixels_.data(), w(), h(), 3);
  }
  mode(winmode); reliefos_fltk::style(this);
}
void Overlay::mode(bool window_mode) {
  window_mode_ = window_mode; dragging_ = false; selected_ = {};
  region_->value(!window_mode); window_->value(window_mode); redraw();
}
void Overlay::present() {
  show(); Fl::flush();
  XRaiseWindow(fl_display, fl_xid(this)); Fl::grab(this); cursor(FL_CURSOR_CROSS);
  printf("[reliefcapture.elf] selection-ready mode=%s size=%dx%d\n", window_mode_ ? "window" : "region", w(), h());
  fflush(stdout);
}
void Overlay::dismiss() { if (Fl::grab() == this) Fl::grab(nullptr); hide(); }
Overlay::~Overlay() {
  if (Fl::grab() == this) Fl::grab(nullptr);
  delete original_; delete dim_;
}
void Overlay::finish(bool canceled) { if (finished_) finished_(selected_, canceled, data_); }
void Overlay::hover(int x, int y) {
  selected_ = {};
  for (size_t n = 0; n < snapshot_.windows.size(); ++n) {
    if (snapshot_.windows[n].bounds.contains(x, y)) { selected_ = snapshot_.windows[n].bounds; break; }
  }
}
void Overlay::dim_line(void *p, int x, int y, int count, unsigned char *out) {
  auto *self = static_cast<Overlay *>(p);
  const auto &image = self->snapshot_.image;
  for (int n = 0; n < count; ++n) {
    uint32_t c = image.pixels[size_t(y) * image.width + x + n];
    *out++ = ((c >> 16) & 255) * 4 / 10;
    *out++ = ((c >> 8) & 255) * 4 / 10;
    *out++ = (c & 255) * 4 / 10;
  }
}
void Overlay::draw() {
  if (dim_) dim_->draw(0, 0); else fl_draw_image(dim_line, this, 0, 0, w(), h(), 3);
  if (selected_.valid()) {
    auto r = selected_;
    fl_push_clip(r.x, r.y, r.w, r.h);
    if (original_) original_->draw(0, 0);
    else reliefos_fltk::draw_pixels(snapshot_.image.pixels.data(), w(), h(), 0, 0, w(), h());
    fl_pop_clip();
    fl_color(FL_WHITE); fl_line_style(FL_SOLID, 2); fl_rect(r.x, r.y, r.w, r.h); fl_line_style(0);
    char dimensions[64]; snprintf(dimensions, sizeof(dimensions), "%d × %d", r.w, r.h);
    int dx = r.x, dy = r.y + r.h + 4;
    if (dy > h() - 28) dy = r.y > 30 ? r.y - 28 : 98;
    if (dx > w() - 154) dx = w() - 154;
    if (dx < 0) dx = 0;
    fl_color(FL_BACKGROUND_COLOR); fl_rectf(dx, dy, 150, 24);
    fl_color(FL_FOREGROUND_COLOR); fl_font(FL_HELVETICA, 14); fl_draw(dimensions, dx + 8, dy + 17);
  }
  draw_children();
}
int Overlay::handle(int event) {
  int x = Fl::event_x(), y = Fl::event_y();
  if (event == FL_KEYDOWN || event == FL_SHORTCUT) {
    int key = Fl::event_key();
    if (key == FL_Escape) { finish(true); return 1; }
    if (key == 'r' || key == 'R') { mode(false); return 1; }
    if (key == 'w' || key == 'W') { mode(true); return 1; }
    if (key == 'f' || key == 'F') { selected_ = {0, 0, w(), h()}; finish(false); return 1; }
    if (key == FL_Enter && selected_.valid()) { finish(false); return 1; }
  }
  if (event == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE) { finish(true); return 1; }
  if (!dragging_ && Rect{bar_->x(), bar_->y(), bar_->w(), bar_->h()}.contains(x, y))
    return Fl_Double_Window::handle(event);
  if (event == FL_MOVE && window_mode_) { hover(x, y); redraw(); return 1; }
  if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) {
    if (window_mode_) { hover(x, y); if (selected_.valid()) finish(false); return 1; }
    start_ = {x, y}; dragging_ = true; selected_ = {}; redraw(); return 1;
  }
  if (event == FL_DRAG && dragging_) {
    selected_ = selection(start_.x, start_.y, x, y, w(), h()); redraw(); return 1;
  }
  if (event == FL_RELEASE && dragging_) {
    dragging_ = false; selected_ = selection(start_.x, start_.y, x, y, w(), h());
    if (selected_.valid()) finish(false); else redraw(); return 1;
  }
  return Fl_Double_Window::handle(event);
}
}
