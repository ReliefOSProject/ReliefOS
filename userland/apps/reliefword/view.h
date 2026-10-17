#ifndef RELIEFWORD_VIEW_H
#define RELIEFWORD_VIEW_H
#include "layout.h"
#include <FL/Fl_Group.H>
#include <FL/Fl_Scrollbar.H>
namespace rw {
class WordWindow;
class PageView:public Fl_Group {
    WordWindow *owner_;
    Fl_Scrollbar *vertical_,*horizontal_;
    int zoom_=100;
    bool ruler_=true,blink_=true,dragging_=false,previous_affinity_=false;
    int drag_x_=0,drag_y_=0;
    float desired_x_=-1;
    uint64_t laid_out_=0;
    double scale() const;
    int canvas_width() const; int canvas_height() const; int ruler_height() const;
    double paper_left() const; double paper_top(unsigned page) const;
    double content_width() const; double content_height() const;
    bool layout_current() const;
    size_t point_to_offset(int X,int Y,size_t *line=nullptr) const;
    void update_bars(); void move_to(size_t offset,bool extend,bool previous=false);
    void move_vertical(int amount,bool extend);
    void drag_select();
    static void scroll_cb(Fl_Widget *,void *);
    static void blink_cb(void *);
    static void drag_cb(void *);
protected:
    void draw() override;
public:
    Layout layout;
    PageView(int X,int Y,int W,int H,WordWindow *owner);
    ~PageView() override;
    bool reflow(bool force=false);
    void ensure_caret();
    void set_zoom(int zoom); int zoom() const { return zoom_; }
    void fit_width();
    void show_ruler(bool enabled); bool ruler() const { return ruler_; }
    unsigned current_page() const;
    static Metrics measure(void *,uint32_t,const TextStyle &);
    static int font(const TextStyle &);
    void resize(int,int,int,int) override;
    int handle(int event) override;
};
}
#endif
