#include "canvas.h"
#include "../fltk_support.h"
#include "render.h"
#include "window.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
namespace rp {
namespace {
class InlineInput : public Fl_Multiline_Input {
    Canvas *owner_;

  public:
    InlineInput(Canvas *o) : Fl_Multiline_Input(0, 0, 1, 1), owner_(o) {}
    int handle(int e) override {
        if ((e == FL_KEYDOWN || e == FL_SHORTCUT) && Fl::event_key() == FL_Escape) {
            owner_->finish_edit();
            return 1;
        }
        return Fl_Multiline_Input::handle(e);
    }
};
bool has(const Array<uint32_t> &ids, uint32_t id) {
    for (size_t i = 0; i < ids.size(); ++i)
        if (ids[i] == id)
            return true;
    return false;
}
void handle_position(Rect r, int handle, float &x, float &y) {
    static const float xs[] = {0, .5, 1, 1, 1, .5, 0, 0}, ys[] = {0, 0, 0, .5, 1, 1, 1, .5};
    x = r.x + r.w * xs[handle];
    y = r.y + r.h * ys[handle];
}
} // namespace
Canvas::Canvas(int X, int Y, int W, int H, PptWindow *owner) : Fl_Group(X, Y, W, H), owner_(owner) {
    begin();
    input_ = new InlineInput(this);
    input_->wrap(1);
    input_->maximum_size(MaxTextBytes);
    input_->when(FL_WHEN_CHANGED);
    input_->callback([](Fl_Widget *, void *c) { static_cast<Canvas *>(c)->input_changed(); }, this);
    input_->hide();
    vertical_ = new Fl_Scrollbar(X + W - 14, Y, 14, H - 14);
    vertical_->callback(scroll_cb, this);
    horizontal_ = new Fl_Scrollbar(X, Y + H - 14, W - 14, 14);
    horizontal_->type(FL_HORIZONTAL);
    horizontal_->callback(scroll_cb, this);
    end();
}
void Canvas::scroll_cb(Fl_Widget *, void *c) {
    auto *canvas = static_cast<Canvas *>(c);
    canvas->position_edit();
    canvas->redraw();
}
void Canvas::input_changed() {
    const Object *object = owner_->editor.deck.object(owner_->editor.current, editing_);
    if (object)
        input_->textfont(editing_font(object->style, input_->value(), strlen(input_->value())));
    owner_->pending_input();
}
void Canvas::position_edit() {
    if (!editing_)
        return;
    const Object *object = owner_->editor.deck.object(owner_->editor.current, editing_);
    if (!object)
        return;
    input_->resize(paper_x() + int(object->rect.x * scale()) + 1,
                   paper_y() + int(object->rect.y * scale()) + 1, int(fmax(1, object->rect.w * scale() - 2)),
                   int(fmax(1, object->rect.h * scale() - 2)));
    input_->textsize(int(fmax(6, object->style.font_size * scale())));
    input_->textfont(editing_font(object->style, input_->value(), strlen(input_->value())));
}
int Canvas::paper_x() const {
    int content = int(round(owner_->editor.deck.width * scale()));
    return x() + (w() - 14 > content + 40 ? (w() - 14 - content) / 2 : 20) - int(horizontal_->value());
}
int Canvas::paper_y() const {
    int content = int(round(owner_->editor.deck.height * scale()));
    return y() + (h() - 14 > content + 40 ? (h() - 14 - content) / 2 : 20) - int(vertical_->value());
}
void Canvas::bars() {
    int W = w() - 14, H = h() - 14, cw = int(round(owner_->editor.deck.width * scale())) + 40,
        ch = int(round(owner_->editor.deck.height * scale())) + 40;
    horizontal_->resize(x(), y() + h() - 14, W, 14);
    vertical_->resize(x() + w() - 14, y(), 14, H);
    horizontal_->value(int(fmin(horizontal_->value(), fmax(0, cw - W))), W, 0, cw > W ? cw : W);
    vertical_->value(int(fmin(vertical_->value(), fmax(0, ch - H))), H, 0, ch > H ? ch : H);
}
void Canvas::fit() {
    if (!finish_edit())
        return;
    fit_ = true;
    zoom_ = int(
        floor(fmin((w() - 54) / owner_->editor.deck.width, (h() - 54) / owner_->editor.deck.height) * 100));
    if (zoom_ < 25)
        zoom_ = 25;
    if (zoom_ > 200)
        zoom_ = 200;
    horizontal_->value(0);
    vertical_->value(0);
    bars();
    redraw();
    owner_->selected();
}
void Canvas::set_zoom(int z) {
    if (!finish_edit())
        return;
    fit_ = false;
    zoom_ = z < 25 ? 25 : z > 200 ? 200 : z;
    bars();
    redraw();
    owner_->selected();
}
void Canvas::resize(int X, int Y, int W, int H) {
    Fl_Group::resize(X, Y, W, H);
    if (!input_)
        return;
    if (fit_) {
        zoom_ = int(
            floor(fmin((W - 54) / owner_->editor.deck.width, (H - 54) / owner_->editor.deck.height) * 100));
        if (zoom_ < 25)
            zoom_ = 25;
        if (zoom_ > 200)
            zoom_ = 200;
    }
    bars();
    position_edit();
    redraw();
}
void Canvas::draw() {
    fl_push_clip(x(), y(), w() - 14, h() - 14);
    fl_color(fl_rgb_color(191, 198, 205));
    fl_rectf(x(), y(), w(), h());
    int X = paper_x(), Y = paper_y(), W = int(round(owner_->editor.deck.width * scale())),
        H = int(round(owner_->editor.deck.height * scale()));
    fl_color(fl_rgb_color(130, 140, 150));
    fl_rectf(X + 4, Y + 4, W, H);
    Text error;
    if (!draw_slide(owner_->editor.deck, owner_->editor.current, X, Y, scale(),
                    preview_.empty() ? nullptr : &preview_, &error))
        owner_->message(T(error.c_str()));
    if (owner_->show_grid()) {
        fl_color(fl_rgb_color(205, 214, 224));
        for (float px = 20; px < owner_->editor.deck.width; px += 20)
            for (float py = 20; py < owner_->editor.deck.height; py += 20)
                fl_point(X + int(px * scale()), Y + int(py * scale()));
    }
    const Slide &slide = owner_->editor.deck.slides[owner_->editor.current];
    if (slide.objects.empty()) {
        fl_color(FL_DARK3);
        fl_font(FL_HELVETICA, 16);
        fl_draw(T("Insert text, pictures or shapes to start your slide"), X + 20, Y + H / 2, W - 40, 40,
                FL_ALIGN_CENTER);
    }
    if (!editing_) {
        for (size_t i = 0; i < slide.objects.size(); ++i)
            if (has(owner_->editor.selected, slide.objects[i].id)) {
                const Object *o = &slide.objects[i];
                for (size_t p = 0; p < preview_.size(); ++p)
                    if (preview_[p].id == o->id) {
                        o = &preview_[p];
                        break;
                    }
                fl_color(fl_rgb_color(39, 107, 177));
                fl_line_style(FL_DASH, 1);
                fl_rect(X + int(o->rect.x * scale()), Y + int(o->rect.y * scale()), int(o->rect.w * scale()),
                        int(o->rect.h * scale()));
                fl_line_style(0);
            }
        Bounds b = selection_bounds(owner_->editor.deck, owner_->editor.current, owner_->editor.selected);
        if (b.valid) {
            if (!preview_.empty()) {
                float left = preview_[0].rect.x, top = preview_[0].rect.y, right = left + preview_[0].rect.w,
                      bottom = top + preview_[0].rect.h;
                for (size_t i = 1; i < preview_.size(); ++i) {
                    Rect r = preview_[i].rect;
                    left = fmin(left, r.x);
                    top = fmin(top, r.y);
                    right = fmax(right, r.x + r.w);
                    bottom = fmax(bottom, r.y + r.h);
                }
                b.rect = {left, top, right - left, bottom - top};
            }
            for (int i = 0; i < 8; ++i) {
                float px, py;
                handle_position(b.rect, i, px, py);
                int hx = X + int(round(px * scale())), hy = Y + int(round(py * scale()));
                fl_color(FL_WHITE);
                fl_rectf(hx - 4, hy - 4, 8, 8);
                fl_color(fl_rgb_color(39, 107, 177));
                fl_rect(hx - 4, hy - 4, 8, 8);
            }
        }
    }
    fl_pop_clip();
    draw_children();
}
int Canvas::hit_handle(int X, int Y) const {
    Bounds b = selection_bounds(owner_->editor.deck, owner_->editor.current, owner_->editor.selected);
    if (!b.valid)
        return -1;
    for (int i = 0; i < 8; ++i) {
        float px, py;
        handle_position(b.rect, i, px, py);
        if (abs(X - paper_x() - int(round(px * scale()))) <= 6 &&
            abs(Y - paper_y() - int(round(py * scale()))) <= 6)
            return i;
    }
    return -1;
}
bool Canvas::pending() const { return editing_ && strcmp(input_->value(), original_text_.c_str()); }
bool Canvas::snapshot(Deck &candidate, Text &error) const {
    if (!editing_)
        return true;
    Object *object = candidate.object(owner_->editor.current, editing_);
    if (!object || !valid_utf8(input_->value(), strlen(input_->value())) ||
        !object->text.set(input_->value())) {
        error.set("Could not capture the edited text");
        return false;
    }
    return true;
}
void Canvas::edit(uint32_t id) {
    if (!finish_edit())
        return;
    const Object *o = owner_->editor.deck.object(owner_->editor.current, id);
    if (!o || o->kind != Kind::Text)
        return;
    owner_->editor.select(id);
    original_text_ = o->text;
    editing_ = id;
    input_->textcolor(fl_rgb_color(o->style.text >> 16, (o->style.text >> 8) & 255, o->style.text & 255));
    input_->color(FL_WHITE);
    input_->value(o->text.c_str());
    position_edit();
    input_->show();
    input_->take_focus();
    input_->position(input_->size());
    owner_->selected();
    redraw();
}
bool Canvas::finish_edit() {
    if (!editing_)
        return true;
    uint32_t id = editing_;
    if (pending() && !owner_->editor.set_text(id, input_->value(), strlen(input_->value()))) {
        owner_->message(T(owner_->editor.error.c_str()));
        input_->take_focus();
        return false;
    }
    editing_ = 0;
    input_->hide();
    original_text_.clear();
    take_focus();
    owner_->changed();
    return true;
}
void Canvas::begin_drag() {
    originals_.clear();
    preview_.clear();
    original_bounds_ = selection_bounds(owner_->editor.deck, owner_->editor.current, owner_->editor.selected);
    const Slide &s = owner_->editor.deck.slides[owner_->editor.current];
    for (size_t i = 0; i < s.objects.size(); ++i)
        if (has(owner_->editor.selected, s.objects[i].id) && !originals_.push(s.objects[i])) {
            originals_.clear();
            owner_->message(T("Not enough memory"));
            return;
        }
    dragging_ = original_bounds_.valid && !originals_.empty();
}
void Canvas::cancel_drag() {
    dragging_ = false;
    preview_.clear();
    originals_.clear();
    handle_ = -2;
    redraw();
}
void Canvas::preview_drag() {
    float dx = (Fl::event_x() - down_x_) / scale(), dy = (Fl::event_y() - down_y_) / scale();
    Rect target = original_bounds_.rect;
    if (handle_ >= 0) {
        if (!resize_rect(target, handle_, dx, dy, Fl::event_state(FL_SHIFT), owner_->editor.deck.width,
                         owner_->editor.deck.height, target))
            return;
    } else {
        if (owner_->snap_grid() && !Fl::event_state(FL_ALT)) {
            dx = round((target.x + dx) / 10) * 10 - target.x;
            dy = round((target.y + dy) / 10) * 10 - target.y;
        }
        dx = fmax(-target.x, fmin(dx, owner_->editor.deck.width - target.x - target.w));
        dy = fmax(-target.y, fmin(dy, owner_->editor.deck.height - target.y - target.h));
        target.x += dx;
        target.y += dy;
    }
    Array<Object> next;
    for (size_t i = 0; i < originals_.size(); ++i) {
        Object o = originals_[i];
        float sx = target.w / original_bounds_.rect.w, sy = target.h / original_bounds_.rect.h;
        o.rect = {target.x + (o.rect.x - original_bounds_.rect.x) * sx,
                  target.y + (o.rect.y - original_bounds_.rect.y) * sy, o.rect.w * sx, o.rect.h * sy};
        if (o.rect.w < 4 || o.rect.h < 4)
            return;
        if (!next.push(o)) {
            owner_->message(T("Not enough memory"));
            return;
        }
    }
    preview_.swap(next);
    redraw();
}
void Canvas::move_selected(float dx, float dy) {
    Bounds b = selection_bounds(owner_->editor.deck, owner_->editor.current, owner_->editor.selected);
    if (!b.valid)
        return;
    dx = fmax(-b.rect.x, fmin(dx, owner_->editor.deck.width - b.rect.x - b.rect.w));
    dy = fmax(-b.rect.y, fmin(dy, owner_->editor.deck.height - b.rect.y - b.rect.h));
    Array<Object> next;
    const Slide &s = owner_->editor.deck.slides[owner_->editor.current];
    for (size_t i = 0; i < s.objects.size(); ++i)
        if (has(owner_->editor.selected, s.objects[i].id)) {
            Object o = s.objects[i];
            o.rect.x += dx;
            o.rect.y += dy;
            if (!next.push(o)) {
                owner_->message(T("Not enough memory"));
                return;
            }
        }
    if (!owner_->editor.update_objects(next))
        owner_->message(T(owner_->editor.error.c_str()));
    else
        owner_->changed();
}
int Canvas::handle(int e) {
    if (e == FL_FOCUS || e == FL_UNFOCUS)
        return 1;
    // The window manager may return focus to the canvas after resizing. Keep
    // text keys in the active input until the edit has actually finished.
    if (editing_ && (e == FL_KEYDOWN || e == FL_PASTE))
        return input_->handle(e);
    if (editing_ && Fl_Group::handle(e))
        return 1;
    if (e == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) {
        if (Fl::event_x() >= x() + w() - 14 || Fl::event_y() >= y() + h() - 14)
            return Fl_Group::handle(e);
        if (!owner_->commit_inputs())
            return 1;
        take_focus();
        down_x_ = Fl::event_x();
        down_y_ = Fl::event_y();
        handle_ = hit_handle(down_x_, down_y_);
        if (handle_ < 0) {
            uint32_t id =
                hit_test(owner_->editor.deck.slides[owner_->editor.current], (down_x_ - paper_x()) / scale(),
                         (down_y_ - paper_y()) / scale(), 5 / scale());
            if (!id) {
                owner_->editor.clear_selection();
                redraw();
                owner_->selected();
                return 1;
            }
            if (Fl::event_state(FL_SHIFT) || !has(owner_->editor.selected, id))
                owner_->editor.select(id, Fl::event_state(FL_SHIFT));
            if (Fl::event_clicks()) {
                edit(id);
                return 1;
            }
        }
        begin_drag();
        owner_->selected();
        redraw();
        return 1;
    }
    if (e == FL_DRAG && dragging_) {
        preview_drag();
        return 1;
    }
    if (e == FL_RELEASE && dragging_) {
        bool ok = true;
        if (!preview_.empty())
            ok = owner_->editor.update_objects(preview_);
        cancel_drag();
        if (!ok)
            owner_->message(T(owner_->editor.error.c_str()));
        else
            owner_->changed();
        return 1;
    }
    if (e == FL_MOUSEWHEEL) {
        if (!finish_edit())
            return 1;
        if (Fl::event_state(FL_CTRL))
            set_zoom(zoom_ - Fl::event_dy() * 10);
        else {
            vertical_->value(
                fmax(0, fmin(vertical_->value() + Fl::event_dy() * 35,
                             vertical_->maximum() -
                                 vertical_->slider_size() * (vertical_->maximum() - vertical_->minimum()))));
            redraw();
        }
        return 1;
    }
    if (e == FL_PASTE) {
        owner_->receive_text(Fl::event_text(), size_t(Fl::event_length()));
        return 1;
    }
    if (e == FL_KEYDOWN) {
        int key = Fl::event_key();
        if (key == FL_Escape) {
            cancel_drag();
            owner_->editor.clear_selection();
            owner_->selected();
            return 1;
        }
        if (key == FL_Enter || key == FL_F + 2) {
            if (owner_->editor.selected.size() == 1)
                edit(owner_->editor.selected[0]);
            return 1;
        }
        if (key == FL_Delete || key == FL_BackSpace) {
            owner_->act(Command::Delete);
            return 1;
        }
        float step = Fl::event_state(FL_SHIFT) ? 10 : 1;
        if (key == FL_Left || key == FL_Right || key == FL_Up || key == FL_Down) {
            move_selected(key == FL_Left    ? -step
                          : key == FL_Right ? step
                                            : 0,
                          key == FL_Up     ? -step
                          : key == FL_Down ? step
                                           : 0);
            return 1;
        }
        if (!Fl::event_state(FL_CTRL | FL_ALT) && Fl::event_length() > 0 &&
            owner_->editor.selected.size() == 1) {
            uint32_t id = owner_->editor.selected[0];
            const Object *o = owner_->editor.deck.object(owner_->editor.current, id);
            if (o && o->kind == Kind::Text) {
                edit(id);
                return input_->handle(e);
            }
        }
    }
    return Fl_Group::handle(e);
}
Thumbnails::Thumbnails(int X, int Y, int W, int H, PptWindow *owner) : Fl_Group(X, Y, W, H), owner_(owner) {
    begin();
    scroll_ = new Fl_Scrollbar(X + W - 14, Y, 14, H);
    scroll_->callback([](Fl_Widget *, void *c) { static_cast<Thumbnails *>(c)->redraw(); }, this);
    end();
}
int Thumbnails::item_height() const {
    return int((w() - 46) * owner_->editor.deck.height / owner_->editor.deck.width) + 38;
}
void Thumbnails::refresh() {
    int total = int(owner_->editor.deck.slides.size()) * item_height();
    scroll_->value(int(fmin(scroll_->value(), fmax(0, total - h()))), h(), 0, total > h() ? total : h());
    int top = int(owner_->editor.current) * item_height();
    if (top < scroll_->value())
        scroll_->value(top);
    if (top + item_height() > scroll_->value() + h())
        scroll_->value(top + item_height() - h());
    redraw();
}
void Thumbnails::resize(int X, int Y, int W, int H) {
    Fl_Group::resize(X, Y, W, H);
    scroll_->resize(X + W - 14, Y, 14, H);
    refresh();
}
void Thumbnails::draw() {
    fl_push_clip(x(), y(), w() - 14, h());
    fl_color(fl_rgb_color(232, 236, 240));
    fl_rectf(x(), y(), w(), h());
    int row = item_height(), offset = int(scroll_->value());
    for (size_t i = 0; i < owner_->editor.deck.slides.size(); ++i) {
        int Y = y() + int(i) * row - offset;
        if (Y + row < y() || Y > y() + h())
            continue;
        int X = x() + 28, W = w() - 46, H = int(W * owner_->editor.deck.height / owner_->editor.deck.width);
        if (i == owner_->editor.current) {
            fl_color(fl_rgb_color(212, 227, 243));
            fl_rectf(x() + 3, Y + 4, w() - 20, row - 8);
        }
        draw_slide(owner_->editor.deck, i, X, Y + 13, W / owner_->editor.deck.width);
        fl_color(i == owner_->editor.current ? fl_rgb_color(40, 107, 177) : FL_DARK3);
        fl_line_style(FL_SOLID, i == owner_->editor.current ? 2 : 1);
        fl_rect(X - 1, Y + 12, W + 2, H + 2);
        fl_line_style(0);
        char number[20];
        snprintf(number, sizeof(number), "%zu", i + 1);
        fl_font(FL_HELVETICA, 12);
        fl_draw(number, x() + 4, Y + 13, 21, 20, FL_ALIGN_CENTER);
    }
    fl_pop_clip();
    draw_children();
}
int Thumbnails::handle(int e) {
    if (e == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) {
        if (Fl::event_x() >= scroll_->x())
            return Fl_Group::handle(e);
        int index = (Fl::event_y() - y() + int(scroll_->value())) / item_height();
        if (index >= 0 && size_t(index) < owner_->editor.deck.slides.size())
            owner_->change_slide(size_t(index));
        take_focus();
        return 1;
    }
    if (e == FL_MOUSEWHEEL) {
        scroll_->value(
            fmax(0, fmin(scroll_->value() + Fl::event_dy() * item_height() / 2, scroll_->maximum() - h())));
        redraw();
        return 1;
    }
    if (e == FL_FOCUS || e == FL_UNFOCUS)
        return 1;
    if (e == FL_KEYDOWN) {
        int key = Fl::event_key();
        if (key == FL_Delete) {
            owner_->act(Command::DeleteSlide);
            return 1;
        }
        if (key == FL_Up || key == FL_Down) {
            int n = int(owner_->editor.current) + (key == FL_Up ? -1 : 1);
            if (n >= 0 && size_t(n) < owner_->editor.deck.slides.size())
                owner_->change_slide(size_t(n));
            return 1;
        }
    }
    return Fl_Group::handle(e);
}
} // namespace rp
