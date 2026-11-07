#include "window.h"
#include <FL/Fl_Image_Surface.H>
#include <math.h>
#include <png.h>
namespace rmp {
Canvas::Canvas(int x, int y, int w, int h, MathWindow *owner, int tab)
    : Fl_Widget(x, y, w, h), owner_(owner), tab_(tab) {
  tooltip(tab ? "拖动旋转；滚轮缩放；Home 重置"
              : "拖动平移；滚轮缩放；几何工具拾点；Esc 取消");
}
bool Canvas::prepare() {
  if (valid_ && frame_.width == w() && frame_.height == h())
    return true;
  valid_ = frame(owner_->document, owner_->meshes, tab_, w(), h(), frame_);
  return valid_;
}
void Canvas::draw() {
  fl_push_clip(x(), y(), w(), h());
  fl_color(FL_WHITE);
  fl_rectf(x(), y(), w(), h());
  if (prepare())
    for (size_t n = 0; n < frame_.items.size(); ++n) {
      const Primitive &p = frame_.items[n];
      fl_color(
          fl_rgb_color((p.rgb >> 16) & 255, (p.rgb >> 8) & 255, p.rgb & 255));
      if (p.type == Primitive::Line) {
        fl_line_style(FL_SOLID, p.width);
        fl_line(x() + int(p.x[0]), y() + int(p.y[0]), x() + int(p.x[1]),
                y() + int(p.y[1]));
      } else if (p.type == Primitive::Fill) {
        fl_begin_polygon();
        for (int i = 0; i < 3; ++i)
          fl_vertex(x() + p.x[i], y() + p.y[i]);
        fl_end_polygon();
      } else if (p.type == Primitive::Dot)
        fl_pie(x() + int(p.x[0]) - 4, y() + int(p.y[0]) - 4, 8, 8, 0, 360);
      else {
        fl_font(FL_HELVETICA, 13);
        fl_draw(p.text.c_str(), x() + int(p.x[0]), y() + int(p.y[0]));
      }
    }
  else {
    fl_color(FL_FOREGROUND_COLOR);
    fl_draw("绘图内存不足，请减少对象或采样精度", x() + 20, y() + 30);
  }
  fl_line_style(0);
  fl_color(FL_DARK3);
  fl_rect(x(), y(), w(), h());
  fl_pop_clip();
}
int Canvas::handle(int event) {
  View &v = owner_->document.view;
  if (event == FL_ENTER || event == FL_MOVE) {
    if (!tab_) {
      double factor = w() / v.span;
      owner_->pointer(v.cx + (Fl::event_x() - x() - w() * .5) / factor,
                      v.cy - (Fl::event_y() - y() - h() * .5) / factor);
    }
    return 1;
  }
  if (event == FL_PUSH) {
    take_focus();
    last_x_ = Fl::event_x();
    last_y_ = Fl::event_y();
    if (!tab_ && owner_->picking()) {
      double s = w() / v.span;
      owner_->picked({v.cx + (last_x_ - x() - w() * .5) / s,
                      v.cy - (last_y_ - y() - h() * .5) / s, 0});
      return 1;
    }
    dragging_ = true;
    return 1;
  }
  if (event == FL_DRAG && dragging_) {
    int dx = Fl::event_x() - last_x_, dy = Fl::event_y() - last_y_;
    last_x_ = Fl::event_x();
    last_y_ = Fl::event_y();
    if (tab_) {
      v.yaw += dx * .008;
      v.yaw = remainder(v.yaw, 2 * acos(-1));
      v.pitch = fmax(-1.55, fmin(1.55, v.pitch + dy * .008));
    } else {
      v.cx = fmax(-1e8, fmin(1e8, v.cx - dx * v.span / w()));
      v.cy = fmax(-1e8, fmin(1e8, v.cy + dy * v.span / w()));
    }
    owner_->view_changed();
    return 1;
  }
  if (event == FL_RELEASE) {
    dragging_ = false;
    return 1;
  }
  if (event == FL_MOUSEWHEEL) {
    double factor = pow(1.16, Fl::event_dy());
    if (tab_)
      v.scale3 = fmax(.01, fmin(100, v.scale3 / factor));
    else {
      double old = v.span;
      v.span = fmax(1e-5, fmin(1e6, v.span * factor));
      double mx = (Fl::event_x() - x() - w() * .5) / w(),
             my = (Fl::event_y() - y() - h() * .5) / w();
      v.cx = fmax(-1e8, fmin(1e8, v.cx + mx * (old - v.span)));
      v.cy = fmax(-1e8, fmin(1e8, v.cy - my * (old - v.span)));
    }
    owner_->view_changed();
    return 1;
  }
  if (event == FL_KEYDOWN) {
    if (Fl::event_key() == FL_Home) {
      owner_->run(Command::Reset);
      return 1;
    }
    if (Fl::event_key() == FL_Escape) {
      owner_->run(Command::CancelPick);
      return 1;
    }
  }
  if (event == FL_FOCUS || event == FL_UNFOCUS)
    return 1;
  return Fl_Widget::handle(event);
}
bool Canvas::export_svg(Bytes &out, Text &err) {
  return prepare() && svg(frame_, out, err);
}
bool Canvas::export_png(Bytes &out, Text &err) {
  if (!prepare()) {
    err.set("不能生成绘图");
    return false;
  }
  Fl_Image_Surface surface(w(), h());
  surface.set_current();
  surface.draw(this);
  Fl_RGB_Image *rgb = surface.image();
  Fl_Display_Device::display_device()->set_current();
  if (!rgb) {
    err.set("不能获取画布图像");
    return false;
  }
  png_image image{};
  image.version = PNG_IMAGE_VERSION;
  image.width = rgb->w();
  image.height = rgb->h();
  image.format = PNG_FORMAT_RGB;
  png_alloc_size_t count = 0;
  bool ok = png_image_write_to_memory(&image, nullptr, &count, 0,
                                      rgb->data()[0], rgb->ld(), nullptr) != 0;
  if (ok)
    ok = count <= MaxFileBytes && out.resize(size_t(count));
  if (ok)
    ok = png_image_write_to_memory(&image, out.data(), &count, 0,
                                   rgb->data()[0], rgb->ld(), nullptr) != 0;
  png_image_free(&image);
  delete rgb;
  if (!ok) {
    err.set("PNG 编码失败或图像超出限制");
    return false;
  }
  out.resize(size_t(count));
  err.clear();
  return true;
}
} // namespace rmp
