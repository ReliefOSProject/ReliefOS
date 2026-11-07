#include "../fltk_support.h"
#include "canvas.h"
#include "render.h"
#include "window.h"
#include <math.h>
namespace rp {
namespace {
class SlideShow : public Fl_Double_Window {
    const Deck &deck_;
    size_t index_;
    bool black_ = false;
    void draw() override {
        fl_color(FL_BLACK);
        fl_rectf(0, 0, w(), h());
        if (black_)
            return;
        if (index_ >= deck_.slides.size()) {
            fl_color(FL_WHITE);
            fl_font(FL_HELVETICA, 22);
            fl_draw(T("End of presentation\nPress Esc to return, or Left to go back"), 40, 0, w() - 80, h(),
                    FL_ALIGN_CENTER);
            return;
        }
        float scale = float(fmin(w() / deck_.width, h() / deck_.height));
        int X = int((w() - deck_.width * scale) / 2), Y = int((h() - deck_.height * scale) / 2);
        Text error;
        if (!draw_slide(deck_, index_, X, Y, scale, nullptr, &error)) {
            fl_color(FL_RED);
            fl_font(FL_HELVETICA, 18);
            fl_draw(T(error.c_str()), 20, 30);
        }
    }
    void next() {
        black_ = false;
        if (index_ < deck_.slides.size())
            ++index_;
        redraw();
    }
    void previous() {
        black_ = false;
        if (index_)
            --index_;
        redraw();
    }

  public:
    SlideShow(const Deck &d, size_t start)
        : Fl_Double_Window(960, 540, "ReliefPPT"), deck_(d), index_(start) {
        end();
        callback([](Fl_Widget *w, void *) { w->hide(); });
    }
    size_t index() const { return index_; }
    int handle(int e) override {
        if (e == FL_KEYDOWN || e == FL_SHORTCUT) {
            switch (Fl::event_key()) {
            case FL_Escape:
                hide();
                return 1;
            case FL_Right:
            case FL_Down:
            case FL_Page_Down:
            case ' ':
            case FL_Enter:
                next();
                return 1;
            case FL_Left:
            case FL_Up:
            case FL_Page_Up:
            case FL_BackSpace:
                previous();
                return 1;
            case FL_Home:
                index_ = 0;
                black_ = false;
                redraw();
                return 1;
            case FL_End:
                index_ = deck_.slides.size() - 1;
                black_ = false;
                redraw();
                return 1;
            case 'b':
            case 'B':
                black_ = !black_;
                redraw();
                return 1;
            default:
                return 1;
            }
        }
        if (e == FL_PUSH) {
            if (Fl::event_button() == FL_RIGHT_MOUSE)
                previous();
            else
                next();
            return 1;
        }
        if (e == FL_FOCUS || e == FL_UNFOCUS)
            return 1;
        return Fl_Double_Window::handle(e);
    }
};
} // namespace
void PptWindow::slideshow(bool from_current) {
    if (!commit_inputs())
        return;
    busy_ = true;
    SlideShow show(editor.deck, from_current ? editor.current : 0);
    show.set_modal();
    show.show();
    show.fullscreen();
    show.take_focus();
    while (show.shown())
        Fl::wait();
    if (show.index() < editor.deck.slides.size()) {
        editor.current = show.index();
        editor.clear_selection();
        notes_page_ = SIZE_MAX;
    }
    busy_ = false;
    changed();
    canvas->take_focus();
}
} // namespace rp
