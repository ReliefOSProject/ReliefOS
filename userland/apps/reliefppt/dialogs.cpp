#include "../fltk_support.h"
#include "canvas.h"
#include "render.h"
#include "window.h"
#include <FL/Fl_Color_Chooser.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Value_Input.H>
#include <math.h>
namespace rp {
namespace {
void wait_dialog(Fl_Window &w) {
    int X, Y, W, H;
    Fl::screen_work_area(X, Y, W, H);
    w.position(X + (W - w.w()) / 2, Y + (H - w.h()) / 2);
    reliefos_fltk::style(&w);
    w.set_modal();
    w.show();
    while (w.shown())
        Fl::wait();
}
void number(Fl_Value_Input &i, double v, double lo, double hi) {
    i.value(v);
    i.bounds(lo, hi);
    i.step(1);
    i.align(FL_ALIGN_TOP_LEFT);
}
bool valid(const Fl_Value_Input &i, double low, double high) {
    return isfinite(i.value()) && i.value() >= low && i.value() <= high;
}
} // namespace
void PptWindow::object_dialog() {
    if (editor.selected.size() != 1) {
        message(T("Select one object to edit its position and size"));
        return;
    }
    const Object *original = editor.deck.object(editor.current, editor.selected[0]);
    if (!original)
        return;
    Object next = *original;
    Fl_Window dialog(470, 430, T("Position and size"));
    Fl_Value_Input X(22, 45, 198, 29, T("X position")), Y(250, 45, 198, 29, T("Y position"));
    Fl_Value_Input W(22, 112, 198, 29, T("Width")), H(250, 112, 198, 29, T("Height"));
    number(X, next.rect.x, 0, editor.deck.width);
    number(Y, next.rect.y, 0, editor.deck.height);
    number(W, next.rect.w, 4, editor.deck.width);
    number(H, next.rect.h, 4, editor.deck.height);
    Fl_Check_Button filled(22, 166, 198, 28, T("Fill object")),
        outlined(250, 166, 198, 28, T("Draw outline"));
    filled.value(next.style.filled);
    outlined.value(next.style.outlined);
    Fl_Value_Input line(22, 226, 198, 29, T("Line width (0–20)"));
    number(line, next.style.line_width, 0, 20);
    line.step(.5);
    Fl_Check_Button aspect(250, 226, 198, 29, T("Keep aspect ratio"));
    aspect.value(next.kind == Kind::Image);
    if (next.kind == Kind::Image) {
        filled.deactivate();
        outlined.deactivate();
        line.deactivate();
    }
    if (next.kind == Kind::Line || next.kind == Kind::Arrow)
        filled.deactivate();
    struct Ratio {
        Fl_Value_Input *w, *h;
        Fl_Check_Button *enabled;
        double ratio;
        bool updating = false;
    } ratio{&W, &H, &aspect, next.rect.w / next.rect.h, false};
    auto dimension = [](Fl_Widget *widget, void *c) {
        auto *r = static_cast<Ratio *>(c);
        if (r->updating || !r->enabled->value())
            return;
        r->updating = true;
        if (widget == r->w && isfinite(r->w->value()))
            r->h->value(r->w->value() / r->ratio);
        else if (widget == r->h && isfinite(r->h->value()))
            r->w->value(r->h->value() * r->ratio);
        r->updating = false;
    };
    W.callback(dimension, &ratio);
    H.callback(dimension, &ratio);
    W.when(FL_WHEN_CHANGED);
    H.when(FL_WHEN_CHANGED);
    Fl_Box hint(22, 280, 426, 54,
                next.kind == Kind::Text && text_overflows(next)
                    ? T("Text exceeds this box. Increase its height or reduce the font size.")
                    : T("Positions and sizes are measured in slide units.\nThe object must remain inside the "
                        "slide."));
    hint.labelsize(13);
    hint.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    int accepted = 0;
    Fl_Button cancel(250, 380, 92, 30, T("Cancel"));
    cancel.callback([](Fl_Widget *w, void *) { w->window()->hide(); });
    Fl_Return_Button apply(356, 380, 92, 30, T("Apply"));
    apply.callback(
        [](Fl_Widget *w, void *c) {
            *static_cast<int *>(c) = 1;
            w->window()->hide();
        },
        &accepted);
    dialog.end();
    for (;;) {
        accepted = 0;
        wait_dialog(dialog);
        if (!accepted)
            break;
        if (!valid(X, 0, editor.deck.width) || !valid(Y, 0, editor.deck.height) ||
            !valid(W, 4, editor.deck.width) || !valid(H, 4, editor.deck.height) || !valid(line, 0, 20) ||
            X.value() + W.value() > editor.deck.width + .01 ||
            Y.value() + H.value() > editor.deck.height + .01) {
            fl_alert("%s", T("Enter finite values that keep the object inside the slide"));
            continue;
        }
        next.rect = {float(X.value()), float(Y.value()), float(W.value()), float(H.value())};
        next.style.filled = filled.value();
        next.style.outlined = outlined.value();
        next.style.line_width = float(line.value());
        Array<Object> objects;
        if (!objects.push(next)) {
            fl_alert("%s", T("Not enough memory"));
            continue;
        }
        if (!editor.update_objects(objects)) {
            fl_alert("%s", T(editor.error.c_str()));
            continue;
        }
        changed();
        break;
    }
    canvas->take_focus();
}
void PptWindow::color_dialog(Command c) {
    const Object *first =
        editor.selected.empty() ? nullptr : editor.deck.object(editor.current, editor.selected[0]);
    if (c != Command::Background && !first) {
        message(T("Select an object first"));
        return;
    }
    uint32_t value = c == Command::Background  ? editor.deck.slides[editor.current].background
                     : c == Command::TextColor ? first->style.text
                     : c == Command::FillColor ? first->style.fill
                                               : first->style.stroke;
    double r = (value >> 16) / 255., g = ((value >> 8) & 255) / 255., b = (value & 255) / 255.;
    const char *label = c == Command::Background  ? T("Slide background")
                        : c == Command::TextColor ? T("Text color")
                        : c == Command::FillColor ? T("Fill color")
                                                  : T("Line color");
    if (!fl_color_chooser(label, r, g, b)) {
        canvas->take_focus();
        return;
    }
    value = uint32_t(r * 255 + .5) << 16 | uint32_t(g * 255 + .5) << 8 | uint32_t(b * 255 + .5);
    if (c == Command::Background) {
        if (!editor.set_background(value))
            message(T(editor.error.c_str()));
        else
            changed();
        canvas->take_focus();
        return;
    }
    Array<Object> objects;
    const Slide &slide = editor.deck.slides[editor.current];
    for (size_t i = 0; i < slide.objects.size(); ++i) {
        const Object &original = slide.objects[i];
        bool selected = false;
        for (size_t j = 0; j < editor.selected.size(); ++j)
            if (editor.selected[j] == original.id)
                selected = true;
        if (!selected || original.kind == Kind::Image)
            continue;
        Object o = original;
        if (c == Command::TextColor) {
            if (o.kind != Kind::Text)
                continue;
            o.style.text = value;
        } else if (c == Command::FillColor) {
            o.style.fill = value;
            o.style.filled = true;
        } else {
            o.style.stroke = value;
            o.style.outlined = true;
        }
        if (!objects.push(o)) {
            message(T("Not enough memory"));
            return;
        }
    }
    if (!editor.update_objects(objects))
        message(T(editor.error.c_str()));
    else
        changed();
    canvas->take_focus();
}
} // namespace rp
