#ifndef RELIEFPPT_CANVAS_H
#define RELIEFPPT_CANVAS_H
#include "geometry.h"
#include <FL/Fl_Group.H>
#include <FL/Fl_Multiline_Input.H>
#include <FL/Fl_Scrollbar.H>
namespace rp {
class PptWindow;
class Canvas : public Fl_Group {
    PptWindow *owner_;
    Fl_Scrollbar *vertical_, *horizontal_;
    Fl_Multiline_Input *input_;
    int zoom_ = 75, handle_ = -2;
    bool fit_ = true, dragging_ = false;
    int down_x_ = 0, down_y_ = 0;
    Bounds original_bounds_;
    Array<Object> originals_, preview_;
    uint32_t editing_ = 0;
    Text original_text_;
    void bars();
    void position_edit();
    void input_changed();
    int hit_handle(int, int) const;
    void begin_drag();
    void preview_drag();
    static void scroll_cb(Fl_Widget *, void *);

  protected:
    void draw() override;

  public:
    Canvas(int, int, int, int, PptWindow *);
    float scale() const { return zoom_ / 100.f; }
    int paper_x() const;
    int paper_y() const;
    int zoom() const { return zoom_; }
    void fit();
    void set_zoom(int);
    void edit(uint32_t);
    bool finish_edit();
    bool editing() const { return editing_ != 0; }
    bool pending() const;
    bool snapshot(Deck &, Text &error) const;
    void move_selected(float, float);
    void cancel_drag();
    int handle(int) override;
    void resize(int, int, int, int) override;
};
class Thumbnails : public Fl_Group {
    PptWindow *owner_;
    Fl_Scrollbar *scroll_;
    int item_height() const;
    void draw() override;

  public:
    Thumbnails(int, int, int, int, PptWindow *);
    void refresh();
    int handle(int) override;
    void resize(int, int, int, int) override;
};
} // namespace rp
#endif
