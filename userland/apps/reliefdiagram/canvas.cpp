#include "canvas.h"
#include "window.h"
#include <math.h>
#include <stdio.h>
namespace rd {
Canvas::Canvas(int x, int y, int w, int h, DiagramWindow *owner)
    : Fl_Widget(x, y, w, h), owner_(owner) {
  tooltip("拖动图形移动；右下角手柄调整尺寸；Shift "
          "多选；空白框选；中键平移；滚轮缩放");
}
bool Canvas::prepare() {
  if (valid_)
    return true;
  Text error;
  valid_ = build_frame(owner_->document, frame_, error);
  if (!valid_)
    owner_->update_status(error.c_str());
  return valid_;
}
Point Canvas::world(int sx, int sy) const {
  const View &v = owner_->document.view;
  return {v.cx + (sx - x() - w() / 2.0) / v.zoom,
          v.cy + (sy - y() - h() / 2.0) / v.zoom};
}
Point Canvas::screen(Point p) const {
  const View &v = owner_->document.view;
  return {x() + w() / 2.0 + (p.x - v.cx) * v.zoom,
          y() + h() / 2.0 + (p.y - v.cy) * v.zoom};
}
void Canvas::fit() {
  if (!prepare())
    return;
  const Rect &r = frame_.bounds;
  View &v = owner_->document.view;
  v.cx = r.x + r.w / 2;
  v.cy = r.y + r.h / 2;
  v.zoom = fmax(.15, fmin(1.5, fmin((w() - 64) / r.w, (h() - 64) / r.h)));
  redraw();
  owner_->update_status();
}
void Canvas::zoom(double factor, int px, int py) {
  if (px < 0) {
    px = x() + w() / 2;
    py = y() + h() / 2;
  }
  Point before = world(px, py);
  View &v = owner_->document.view;
  v.zoom = fmax(.15, fmin(4, v.zoom * factor));
  Point after = world(px, py);
  v.cx = fmax(-100000, fmin(100000, v.cx + before.x - after.x));
  v.cy = fmax(-100000, fmin(100000, v.cy + before.y - after.y));
  redraw();
  owner_->update_status();
}
uint64_t Canvas::hit(Point p) const {
  const Document &d = owner_->document;
  for (size_t i = frame_.visible_nodes.size(); i > 0; --i) {
    const Node *n = node(d, frame_.visible_nodes[i - 1]);
    if (n && rd::inside(*n, p))
      return n->id;
  }
  for (size_t i = frame_.paths.size(); i > 0; --i) {
    const Stroke &s = frame_.paths[i - 1];
    if (s.owner && !node(d, s.owner) && distance(s, p) < 7 / d.view.zoom)
      return s.owner;
  }
  return 0;
}
bool Canvas::node_visible(uint64_t id) const {
  for (size_t i = 0; i < frame_.visible_nodes.size(); ++i)
    if (frame_.visible_nodes[i] == id)
      return true;
  return false;
}
void Canvas::draw() {
  fl_push_clip(x(), y(), w(), h());
  fl_color(FL_WHITE);
  fl_rectf(x(), y(), w(), h());
  View &v = owner_->document.view;
  if (v.grid) {
    double step = 20 * v.zoom;
    while (step < 12)
      step *= 2;
    Point origin = screen({0, 0});
    double xx = x() + fmod(origin.x - x(), step),
           yy = y() + fmod(origin.y - y(), step);
    fl_color(fl_rgb_color(228, 232, 237));
    for (double a = xx; a < x() + w(); a += step)
      for (double b = yy; b < y() + h(); b += step)
        fl_point(int(a), int(b));
  }
  if (prepare()) {
    Point origin = screen({0, 0});
    paint_frame(frame_, origin.x, origin.y, v.zoom);
    for (size_t i = 0; i < owner_->selection.size(); ++i) {
      uint64_t id = owner_->selection[i];
      fl_color(FL_SELECTION_COLOR);
      fl_line_style(FL_DASH, 1);
      if (const Node *n = node(owner_->document, id)) {
        if (!node_visible(n->id))
          continue;
        Point p = screen({n->x, n->y}), q = screen({n->x + n->w, n->y + n->h});
        if (q.x < x() - 8 || p.x > x() + w() + 8 || q.y < y() - 8 ||
            p.y > y() + h() + 8)
          continue;
        fl_rect(int(p.x) - 4, int(p.y) - 4, int(q.x - p.x) + 8,
                int(q.y - p.y) + 8);
        fl_line_style(0);
        fl_rectf(int(q.x) - 4, int(q.y) - 4, 8, 8);
      } else
        for (size_t j = 0; j < frame_.paths.size(); ++j)
          if (frame_.paths[j].owner == id) {
            const Stroke &s = frame_.paths[j];
            fl_begin_line();
            for (unsigned k = 0; k < s.count; ++k) {
              Point p = screen(s.points[k]);
              fl_vertex(p.x, p.y);
            }
            fl_end_line();
          }
    }
  }
  if (drag_ == Drag::Box) {
    Point p = screen(start_), q = screen(last_);
    fl_color(FL_SELECTION_COLOR);
    fl_line_style(FL_DASH, 1);
    fl_rect(int(fmin(p.x, q.x)), int(fmin(p.y, q.y)), int(fabs(q.x - p.x)),
            int(fabs(q.y - p.y)));
  }
  if (source_)
    if (const Node *n = node(owner_->document, source_)) {
      Point p = screen(anchor(*n, world(Fl::event_x(), Fl::event_y())));
      fl_color(FL_SELECTION_COLOR);
      fl_line_style(FL_DASH, 1);
      fl_line(int(p.x), int(p.y), Fl::event_x(), Fl::event_y());
    }
  fl_line_style(0);
  fl_color(FL_DARK3);
  fl_rect(x(), y(), w(), h());
  fl_pop_clip();
}
void Canvas::cancel() {
  if (gesture_) {
    for (size_t i = 0; i < originals_.size(); ++i)
      if (Node *n = node(owner_->document, originals_[i].id))
        *n = originals_[i];
    owner_->finish_gesture();
  }
  gesture_ = false;
  started_ = false;
  drag_ = Drag::None;
  source_ = 0;
  originals_.clear();
  invalidate();
}
int Canvas::handle(int event) {
  if (owner_->busy())
    return 0;
  if (event == FL_FOCUS || event == FL_UNFOCUS)
    return 1;
  if (event == FL_ENTER)
    return 1;
  if (event == FL_MOVE) {
    if (source_)
      redraw();
    return 1;
  }
  if (event == FL_PUSH) {
    if (!owner_->apply_fields() || !prepare())
      return 1;
    take_focus();
    start_ = last_ = world(Fl::event_x(), Fl::event_y());
    mouse_x_ = Fl::event_x();
    mouse_y_ = Fl::event_y();
    started_ = false;
    gesture_ = false;
    originals_.clear();
    if (Fl::event_button() == 2 || Fl::event_button() == 3 ||
        owner_->mode() == 2) {
      drag_ = Drag::Pan;
      return 1;
    }
    uint64_t id = hit(start_);
    bool on_handle = false;
    for (size_t i = 0; i < owner_->selection.size(); ++i) {
      const Node *n = node(owner_->document, owner_->selection[i]);
      if (!n || !node_visible(n->id))
        continue;
      Point corner = screen({n->x + n->w, n->y + n->h});
      if (fabs(corner.x - Fl::event_x()) <= 9 &&
          fabs(corner.y - Fl::event_y()) <= 9) {
        id = n->id;
        on_handle = true;
        break;
      }
    }
    if (owner_->mode() == 1) {
      if (!node(owner_->document, id)) {
        source_ = 0;
        owner_->update_status("请选择图形作为连线端点");
        return 1;
      }
      if (!source_) {
        source_ = id;
        owner_->select(id);
        owner_->update_status("已选择起点，点击另一个图形完成连线");
      } else {
        owner_->connect(source_, id);
        source_ = 0;
      }
      redraw();
      return 1;
    }
    if (id && Fl::event_clicks() && !on_handle) {
      owner_->select(id);
      owner_->edit_label();
      return 1;
    }
    if (id) {
      if (Fl::event_state(FL_SHIFT)) {
        owner_->select(id, true);
        return 1;
      }
      if (!owner_->selected(id) && !owner_->select(id))
        return 1;
      if (Node *n = node(owner_->document, id)) {
        Point corner = screen({n->x + n->w, n->y + n->h});
        drag_ = fabs(corner.x - Fl::event_x()) <= 9 &&
                        fabs(corner.y - Fl::event_y()) <= 9
                    ? Drag::Resize
                    : Drag::Move;
        resize_id_ = id;
        TreeIndex tree(owner_->document);
        for (size_t i = 0; i < owner_->document.nodes.size(); ++i) {
          const Node &a = owner_->document.nodes[i];
          bool take = owner_->selected(a.id);
          if (drag_ == Drag::Resize)
            take = a.id == id;
          else
            for (size_t j = 0; !take && j < owner_->selection.size(); ++j)
              take = tree.descendant(i, owner_->selection[j]);
          if (take && !originals_.push(a)) {
            originals_.clear();
            drag_ = Drag::None;
            owner_->update_status("移动内存不足");
            return 1;
          }
        }
      } else
        drag_ = Drag::None;
    } else {
      if (!Fl::event_state(FL_SHIFT))
        owner_->select(0);
      drag_ = Drag::Box;
    }
    return 1;
  }
  if (event == FL_DRAG && drag_ != Drag::None) {
    if (drag_ == Drag::Pan) {
      View &v = owner_->document.view;
      v.cx = fmax(-100000,
                  fmin(100000, v.cx - (Fl::event_x() - mouse_x_) / v.zoom));
      v.cy = fmax(-100000,
                  fmin(100000, v.cy - (Fl::event_y() - mouse_y_) / v.zoom));
      mouse_x_ = Fl::event_x();
      mouse_y_ = Fl::event_y();
      redraw();
      return 1;
    }
    last_ = world(Fl::event_x(), Fl::event_y());
    if (!started_)
      started_ = hypot(Fl::event_x() - mouse_x_, Fl::event_y() - mouse_y_) > 3;
    if (drag_ == Drag::Box) {
      redraw();
      return 1;
    }
    if (!started_ || originals_.empty())
      return 1;
    if (!gesture_) {
      if (!owner_->begin_gesture()) {
        drag_ = Drag::None;
        return 1;
      }
      gesture_ = true;
    }
    double dx = last_.x - start_.x, dy = last_.y - start_.y;
    if (owner_->document.view.snap) {
      double ox = drag_ == Drag::Resize ? originals_[0].w : originals_[0].x,
             oy = drag_ == Drag::Resize ? originals_[0].h : originals_[0].y;
      dx = round((ox + dx) / 20) * 20 - ox;
      dy = round((oy + dy) / 20) * 20 - oy;
    }
    for (size_t i = 0; i < originals_.size(); ++i)
      if (Node *n = node(owner_->document, originals_[i].id)) {
        const Node &a = originals_[i];
        if (drag_ == Drag::Resize) {
          n->w = fmax(40, fmin(2000, a.w + dx));
          n->h = fmax(28, fmin(2000, a.h + dy));
        } else {
          n->x = fmax(-100000, fmin(100000, a.x + dx));
          n->y = fmax(-100000, fmin(100000, a.y + dy));
        }
      }
    invalidate();
    return 1;
  }
  if (event == FL_RELEASE) {
    if (drag_ == Drag::Box && started_) {
      double left = fmin(start_.x, last_.x), right = fmax(start_.x, last_.x),
             top = fmin(start_.y, last_.y), bottom = fmax(start_.y, last_.y);
      for (size_t i = 0; i < owner_->document.nodes.size(); ++i) {
        const Node &n = owner_->document.nodes[i];
        if (node_visible(n.id) && n.x <= right && n.x + n.w >= left &&
            n.y <= bottom && n.y + n.h >= top && !owner_->selected(n.id))
          owner_->selection.push(n.id);
      }
      owner_->selection_changed();
    }
    if (gesture_) {
      printf("[reliefdiagram.elf] gesture %s\n",
             drag_ == Drag::Resize ? "resize" : "move");
      fflush(stdout);
      owner_->finish_gesture();
    }
    gesture_ = false;
    drag_ = Drag::None;
    originals_.clear();
    redraw();
    return 1;
  }
  if (event == FL_MOUSEWHEEL) {
    zoom(pow(1.12, -Fl::event_dy()), Fl::event_x(), Fl::event_y());
    return 1;
  }
  if (event == FL_KEYDOWN) {
    int key = Fl::event_key();
    if (key == FL_Escape) {
      cancel();
      owner_->run(Command::SelectMode);
      return 1;
    }
    if (key == FL_Delete || key == FL_BackSpace) {
      owner_->run(Command::Delete);
      return 1;
    }
    if (key == FL_Tab && !Fl::event_state(FL_CTRL)) {
      owner_->run(Command::Child);
      return 1;
    }
    if (key == FL_Enter && !Fl::event_state(FL_CTRL)) {
      owner_->run(Command::Sibling);
      return 1;
    }
    if (key == FL_F + 2) {
      owner_->edit_label();
      return 1;
    }
    if (key == FL_Home) {
      fit();
      return 1;
    }
    if (key == FL_Left || key == FL_Right || key == FL_Up || key == FL_Down) {
      double step = Fl::event_state(FL_SHIFT) ? 20 : 1;
      owner_->nudge(key == FL_Left    ? -step
                    : key == FL_Right ? step
                                      : 0,
                    key == FL_Up     ? -step
                    : key == FL_Down ? step
                                     : 0);
      return 1;
    }
  }
  return Fl_Widget::handle(event);
}
} // namespace rd
