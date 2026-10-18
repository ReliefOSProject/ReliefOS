#include "window.h"
#include "../fltk_support.h"
#include "canvas.h"
#include "render.h"
#include <FL/Fl_Menu_Button.H>
#include <FL/x.H>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
namespace rp {
namespace {
const int Zooms[] = {25, 50, 75, 100, 125, 150, 175, 200};
unsigned long selection_owner() {
    using AtomFn = Atom (*)(Display *, const char *, Bool);
    using OwnerFn = Window (*)(Display *, Atom);
    static AtomFn a = reinterpret_cast<AtomFn>(dlsym(RTLD_DEFAULT, "XInternAtom"));
    static OwnerFn o = reinterpret_cast<OwnerFn>(dlsym(RTLD_DEFAULT, "XGetSelectionOwner"));
    return a && o && fl_display ? o(fl_display, a(fl_display, "CLIPBOARD", False)) : 0;
}
void group_label(int X, int Y, int W, const char *t) {
    Fl_Box *b = new Fl_Box(X, Y, W, 15, t);
    b->labelsize(11);
    b->labelcolor(FL_DARK3);
}
bool selected_id(const Array<uint32_t> &ids, uint32_t id) {
    for (size_t i = 0; i < ids.size(); ++i)
        if (ids[i] == id)
            return true;
    return false;
}
} // namespace
PptWindow::PptWindow() : Fl_Double_Window(1180, 760, "ReliefPPT") {
    if (editor.deck.slides.empty())
        return;
    size_range(900, 600);
    callback(close_cb, this);
    saved_revision_ = editor.deck.revision;
    begin();
    top_ = new Fl_Group(0, 0, w(), 34);
    auto *file = new Fl_Menu_Button(7, 3, 64, 28, T("&File"));
    file->box(FL_THIN_UP_BOX);
    auto item = [file](const char *t, Command c, int shortcut = 0) {
        file->add(t, shortcut, dispatch, reinterpret_cast<void *>(intptr_t(c) + 1));
    };
    item(T("New"), Command::New, FL_CTRL + 'n');
    item(T("Open..."), Command::Open, FL_CTRL + 'o');
    item(T("Save"), Command::Save, FL_CTRL + 's');
    item(T("Save as..."), Command::SaveAs, FL_CTRL + FL_SHIFT + 's');
    item(T("Export current slide as PNG..."), Command::ExportPng);
    item(T("Export presentation as HTML..."), Command::ExportHtml);
    item(T("Example presentation"), Command::Example);
    item(T("About ReliefPPT"), Command::About);
    item(T("Exit"), Command::Quit);
    const char *names[] = {T("Home tab"), T("Insert"), T("Design"), T("Slide show")};
    for (int i = 0; i < 4; ++i) {
        tabs_[i] = new Fl_Button(80 + i * 88, 3, 84, 28, names[i]);
        tabs_[i]->callback(tab_cb, reinterpret_cast<void *>(intptr_t(i)));
        tabs_[i]->clear_visible_focus();
    }
    auto *stretch = new Fl_Box(434, 0, w() - 638, 34);
    const char *quick[] = {T("Save"), T("Undo"), T("Redo")};
    Command cmds[] = {Command::Save, Command::Undo, Command::Redo};
    for (int i = 0; i < 3; ++i) {
        quick_[i] = new Fl_Button(w() - 200 + i * 65, 4, 61, 26, quick[i]);
        quick_[i]->callback(dispatch, reinterpret_cast<void *>(intptr_t(cmds[i]) + 1));
        quick_[i]->clear_visible_focus();
    }
    top_->resizable(stretch);
    top_->end();
    ribbon_ = new Fl_Group(0, 34, w(), 100);
    ribbon_scroll_ = new Fl_Scroll(0, 34, w(), 100);
    ribbon_scroll_->type(Fl_Scroll::HORIZONTAL);
    ribbon_scroll_->scrollbar_size(10);
    build_ribbon();
    ribbon_scroll_->end();
    ribbon_->resizable(ribbon_scroll_);
    ribbon_->end();
    thumbs = new Thumbnails(0, 136, 190, h() - 164, this);
    canvas = new Canvas(192, 136, w() - 192, h() - 164, this);
    notes_group_ = new Fl_Group(192, h() - 108, w() - 192, 80);
    notes_ = new Fl_Multiline_Input(250, h() - 101, w() - 265, 65, T("Notes"));
    notes_->wrap(1);
    notes_->maximum_size(MaxNotesBytes);
    notes_->when(FL_WHEN_CHANGED);
    notes_->callback([](Fl_Widget *, void *c) { static_cast<PptWindow *>(c)->pending_input(); }, this);
    notes_group_->resizable(notes_);
    notes_group_->end();
    notes_group_->hide();
    status_ = new Fl_Box(8, h() - 27, w() / 2 - 16, 26, T("Ready"));
    status_->labelsize(13);
    status_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    summary_ = new Fl_Box(w() / 2, h() - 27, w() / 2 - 8, 26);
    summary_->labelsize(13);
    summary_->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    end();
    resizable(canvas);
    show_tab(0);
    canvas->fit();
    changed();
    int X, Y, W, H;
    Fl::screen_work_area(X, Y, W, H);
    int cw = w() < W - 24 ? w() : W - 24, ch = h() < H - 48 ? h() : H - 48;
    if (cw >= 900 && ch >= 600)
        resize(X + (W - cw) / 2, Y + (H - ch) / 2, cw, ch);
    Fl::add_timeout(60, recovery_tick, this);
}
PptWindow::~PptWindow() {
    Fl::remove_timeout(recovery_tick, this);
    unlock_recovery_path(recovery_fd_);
    unlock_recovery_path(recovered_fd_);
}
void PptWindow::build_ribbon() {
    auto button = [](int X, int Y, int W, const char *t, Command c, int H = 26, const char *tip = nullptr) {
        auto *b = new Fl_Button(X, Y, W, H, t);
        b->callback(dispatch, reinterpret_cast<void *>(intptr_t(c) + 1));
        b->clear_visible_focus();
        if (tip)
            b->tooltip(tip);
        return b;
    };
    panes_[0] = new Fl_Group(0, 34, 1160, 88);
    button(10, 42, 62, T("Paste"), Command::Paste, 54);
    button(78, 42, 58, T("Cut"), Command::Cut);
    button(78, 72, 58, T("Copy"), Command::Copy);
    group_label(10, 100, 126, T("Clipboard"));
    button(154, 42, 84, T("New slide"), Command::NewSlide);
    layout_ = new Fl_Choice(244, 42, 134, 26);
    layout_->add(T("Blank slide"));
    layout_->add(T("Title slide"));
    layout_->add(T("Title and content"));
    layout_->add(T("Two columns"));
    layout_->value(2);
    layout_->tooltip(T("Layout for the next new slide"));
    button(154, 72, 108, T("Duplicate slide"), Command::CopySlide);
    button(268, 72, 110, T("Delete slide"), Command::DeleteSlide);
    group_label(154, 100, 224, T("Slides"));
    font_ = new Fl_Choice(396, 42, 132, 26);
    font_->add(T("Chinese / serif"));
    font_->add(T("Sans serif"));
    font_->add(T("Monospace"));
    font_->callback(dispatch, reinterpret_cast<void *>(intptr_t(Command::Font) + 1));
    font_->tooltip(T("Font for the selected text boxes"));
    size_ = new Fl_Input_Choice(534, 42, 76, 26);
    const int sizes[] = {12, 16, 18, 20, 24, 28, 32, 36, 40, 44, 48, 54, 64, 72, 96, 120, 144};
    for (int n : sizes) {
        char t[12];
        snprintf(t, sizeof(t), "%d", n);
        size_->add(t);
    }
    size_->input()->when(FL_WHEN_ENTER_KEY_ALWAYS);
    size_->callback(dispatch, reinterpret_cast<void *>(intptr_t(Command::Size) + 1));
    size_->tooltip(T("Font size: 6–144 points; press Enter"));
    const char *style[] = {"B", "I", "U"};
    Command styles[] = {Command::Bold, Command::Italic, Command::Underline};
    const char *tips[] = {T("Bold (Ctrl+B)"), T("Italic (Ctrl+I)"), T("Underline (Ctrl+U)")};
    for (int i = 0; i < 3; ++i) {
        toggles_[i] = button(396 + i * 36, 72, 32, style[i], styles[i], 26, tips[i]);
        toggles_[i]->type(FL_TOGGLE_BUTTON);
    }
    toggles_[0]->labelfont(FL_HELVETICA_BOLD);
    toggles_[1]->labelfont(FL_HELVETICA_ITALIC);
    button(510, 72, 100, T("Text color"), Command::TextColor);
    group_label(396, 100, 214, T("Text"));
    button(628, 42, 62, T("Left"), Command::TextLeft);
    button(696, 42, 62, T("Center"), Command::TextCenter);
    button(764, 42, 62, T("Right"), Command::TextRight);
    button(628, 72, 92, T("Bullets"), Command::Bullets);
    button(726, 72, 100, T("Properties..."), Command::Properties);
    group_label(628, 100, 198, T("Text / object"));
    button(844, 42, 92, T("Text box"), Command::TextBox);
    button(844, 72, 92, T("Picture..."), Command::Image);
    button(944, 42, 92, T("Fill color"), Command::FillColor);
    button(944, 72, 92, T("Line color"), Command::LineColor);
    button(1044, 42, 100, T("Duplicate"), Command::Duplicate);
    button(1044, 72, 100, T("Delete"), Command::Delete);
    group_label(844, 100, 300, T("Objects"));
    panes_[0]->end();
    panes_[1] = new Fl_Group(0, 34, 1100, 88);
    button(10, 42, 100, T("Text box"), Command::TextBox, 54);
    button(118, 42, 100, T("Picture..."), Command::Image, 54);
    button(238, 42, 90, T("Rectangle"), Command::Rectangle);
    button(334, 42, 90, T("Ellipse"), Command::Ellipse);
    button(238, 72, 90, T("Line"), Command::Line);
    button(334, 72, 90, T("Arrow"), Command::Arrow);
    group_label(10, 100, 414, T("Insert objects"));
    button(446, 42, 84, T("Align left"), Command::AlignLeft);
    button(536, 42, 84, T("Center X"), Command::AlignCenter);
    button(626, 42, 84, T("Align right"), Command::AlignRight);
    button(446, 72, 84, T("Align top"), Command::AlignTop);
    button(536, 72, 84, T("Center Y"), Command::AlignMiddle);
    button(626, 72, 84, T("Align bottom"), Command::AlignBottom);
    group_label(446, 100, 264, T("Align selected objects"));
    button(732, 42, 88, T("To front"), Command::Top);
    button(826, 42, 88, T("Raise"), Command::Raise);
    button(732, 72, 88, T("To back"), Command::Bottom);
    button(826, 72, 88, T("Lower"), Command::Lower);
    group_label(732, 100, 182, T("Order"));
    button(936, 42, 142, T("Position and size..."), Command::Properties, 54);
    group_label(936, 100, 142, T("Object"));
    panes_[1]->end();
    panes_[2] = new Fl_Group(0, 34, 1100, 88);
    button(10, 42, 108, T("Widescreen 16:9"), Command::Wide, 54);
    button(126, 42, 108, T("Standard 4:3"), Command::Standard, 54);
    group_label(10, 100, 224, T("Slide size"));
    button(256, 42, 108, T("Background..."), Command::Background, 54);
    button(386, 42, 92, T("Blue theme"), Command::ThemeBlue, 54);
    button(486, 42, 92, T("Green theme"), Command::ThemeGreen, 54);
    button(586, 42, 92, T("Light theme"), Command::ThemeLight, 54);
    group_label(256, 100, 422, T("Current slide colors"));
    grid_ = new Fl_Check_Button(702, 42, 118, 26, T("Show grid"));
    grid_->callback(dispatch, reinterpret_cast<void *>(intptr_t(Command::Grid) + 1));
    snap_ = new Fl_Check_Button(702, 72, 118, 26, T("Snap to grid"));
    snap_->callback(dispatch, reinterpret_cast<void *>(intptr_t(Command::Snap) + 1));
    snap_->tooltip(T("Snap movement to 10 units; hold Alt to bypass"));
    group_label(702, 100, 118, T("Guides"));
    button(842, 42, 108, T("Move slide up"), Command::SlideUp);
    button(842, 72, 108, T("Move slide down"), Command::SlideDown);
    group_label(842, 100, 108, T("Slide order"));
    panes_[2]->end();
    panes_[3] = new Fl_Group(0, 34, 1100, 88);
    button(10, 42, 138, T("From beginning"), Command::Start, 54, T("Start slide show (F5)"));
    button(156, 42, 138, T("From current slide"), Command::StartCurrent, 54,
           T("Start from current slide (Shift+F5)"));
    group_label(10, 100, 284, T("Present"));
    zoom_ = new Fl_Choice(320, 42, 100, 26);
    for (int n : Zooms) {
        char t[16];
        snprintf(t, sizeof(t), "%d%%", n);
        zoom_->add(t);
    }
    zoom_->callback(dispatch, reinterpret_cast<void *>(intptr_t(Command::Zoom) + 1));
    button(320, 72, 100, T("Fit slide"), Command::Fit);
    notes_toggle_ = new Fl_Check_Button(438, 42, 130, 26, T("Speaker notes"));
    notes_toggle_->callback(dispatch, reinterpret_cast<void *>(intptr_t(Command::Notes) + 1));
    group_label(320, 100, 248, T("Editor view"));
    button(590, 42, 158, T("Export PNG..."), Command::ExportPng);
    button(590, 72, 158, T("Export HTML..."), Command::ExportHtml);
    group_label(590, 100, 158, T("Share"));
    auto *hint = new Fl_Box(774, 42, 306, 54, T("Arrow keys: change slide\nB: black screen   Esc: return"));
    hint->labelsize(13);
    hint->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    panes_[3]->end();
}
void PptWindow::dispatch(Fl_Widget *w, void *data) {
    auto *o = static_cast<PptWindow *>(w->window());
    if (o && !o->updating_)
        o->act(Command(intptr_t(data) - 1));
}
void PptWindow::tab_cb(Fl_Widget *w, void *data) {
    auto *o = static_cast<PptWindow *>(w->window());
    o->show_tab(int(intptr_t(data)));
}
void PptWindow::close_cb(Fl_Widget *, void *data) { static_cast<PptWindow *>(data)->act(Command::Quit); }
void PptWindow::show_tab(int index) {
    tab_ = index;
    for (int i = 0; i < 4; ++i) {
        if (i == index)
            panes_[i]->show();
        else
            panes_[i]->hide();
        tabs_[i]->value(i == index);
    }
    ribbon_scroll_->scroll_to(0, 0);
    ribbon_scroll_->redraw();
}
void PptWindow::message(const char *t) {
    if (status_)
        status_->copy_label(t && *t ? t : T("Ready"));
}
bool PptWindow::dirty() const {
    return editor.deck.revision != saved_revision_ || (canvas && canvas->pending()) ||
           (notes_ && notes_page_ == editor.current &&
            strcmp(notes_->value(), editor.deck.slides[editor.current].notes.c_str()));
}
void PptWindow::pending_input() {
    ++pending_epoch_;
    title();
}
void PptWindow::title() {
    char title[4300];
    snprintf(title, sizeof(title), "%s%s — ReliefPPT", dirty() ? "* " : "",
             path_.empty()                 ? T("Untitled presentation")
             : strrchr(path_.c_str(), '/') ? strrchr(path_.c_str(), '/') + 1
                                           : path_.c_str());
    copy_label(title);
}
bool PptWindow::commit_inputs() {
    if (canvas && !canvas->finish_edit())
        return false;
    if (notes_page_ == editor.current &&
        strcmp(notes_->value(), editor.deck.slides[editor.current].notes.c_str())) {
        if (!editor.set_notes(notes_->value(), strlen(notes_->value()))) {
            message(T(editor.error.c_str()));
            return false;
        }
    }
    return true;
}
void PptWindow::changed() {
    if (!canvas)
        return;
    thumbs->refresh();
    canvas->redraw();
    title();
    selected();
}
void PptWindow::selected() {
    if (updating_ || !canvas)
        return;
    updating_ = true;
    const Object *o =
        editor.selected.empty() ? nullptr : editor.deck.object(editor.current, editor.selected[0]);
    bool text = o && o->kind == Kind::Text;
    if (text) {
        font_->value(o->style.family);
        char t[32];
        snprintf(t, sizeof(t), "%g", double(o->style.font_size));
        size_->value(t);
        toggles_[0]->value(o->style.bold);
        toggles_[1]->value(o->style.italic);
        toggles_[2]->value(o->style.underline);
    }
    if (text) {
        font_->activate();
        size_->activate();
        for (auto *b : toggles_)
            b->activate();
    } else {
        font_->deactivate();
        size_->deactivate();
        for (auto *b : toggles_)
            b->deactivate();
    }
    if (editor.can_undo())
        quick_[1]->activate();
    else
        quick_[1]->deactivate();
    if (editor.can_redo())
        quick_[2]->activate();
    else
        quick_[2]->deactivate();
    if (notes_page_ != editor.current) {
        notes_->value(editor.deck.slides[editor.current].notes.c_str());
        notes_page_ = editor.current;
    }
    char summary[200];
    snprintf(summary, sizeof(summary), T("Slide %zu / %zu  ·  %zu selected  ·  %d%%"), editor.current + 1,
             editor.deck.slides.size(), editor.selected.size(), canvas->zoom());
    summary_->copy_label(summary);
    int z = -1;
    for (unsigned i = 0; i < sizeof(Zooms) / sizeof(Zooms[0]); ++i)
        if (Zooms[i] == canvas->zoom())
            z = int(i);
    zoom_->value(z);
    updating_ = false;
    title();
}
void PptWindow::change_slide(size_t index) {
    if (index >= editor.deck.slides.size() || !commit_inputs())
        return;
    canvas->cancel_drag();
    editor.current = index;
    editor.clear_selection();
    notes_page_ = SIZE_MAX;
    changed();
    canvas->take_focus();
}
void PptWindow::copy_selection(bool cut) {
    if (editor.selected.empty())
        return;
    if (!editor.copy_selected(clipboard_)) {
        message(T(editor.error.c_str()));
        return;
    }
    Array<char> text;
    for (size_t i = 0; i < clipboard_.slides[0].objects.size(); ++i) {
        const Object &o = clipboard_.slides[0].objects[i];
        if (o.kind != Kind::Text)
            continue;
        if (!text.empty() && !text.push('\n')) {
            message(T("Not enough memory"));
            return;
        }
        if (!text.reserve(text.size() + o.text.size())) {
            message(T("Not enough memory"));
            return;
        }
        for (size_t j = 0; j < o.text.size(); ++j)
            if (!text.push(o.text.c_str()[j])) {
                message(T("Not enough memory"));
                return;
            }
    }
    if (!clipboard_text_.set(text.empty() ? "ReliefPPT objects" : text.data(),
                             text.empty() ? 17 : text.size())) {
        message(T("Not enough memory"));
        return;
    }
    Fl::copy(clipboard_text_.c_str(), int(clipboard_text_.size()), 1);
    clipboard_owner_ = selection_owner();
    if (cut) {
        if (editor.delete_selected())
            changed();
        else
            message(T(editor.error.c_str()));
    } else
        message(T("Objects copied"));
}
void PptWindow::paste() {
    if (clipboard_owner_ && selection_owner() == clipboard_owner_ && !clipboard_.slides.empty()) {
        if (editor.paste_objects(clipboard_))
            changed();
        else
            message(T(editor.error.c_str()));
        return;
    }
    canvas->take_focus();
    Fl::paste(*canvas, 1);
}
void PptWindow::receive_text(const char *bytes, size_t length) {
    if (!length)
        return;
    Object o;
    o.kind = Kind::Text;
    o.rect = {64, 80, editor.deck.width - 128, 180};
    if (!o.text.set(bytes, length)) {
        message(T("Not enough memory"));
        return;
    }
    if (!editor.insert_object(o))
        message(T(editor.error.c_str()));
    else
        changed();
}
void PptWindow::insert(Kind kind) {
    if (kind == Kind::Image) {
        Text path;
        if (!choose_path(path, false, "image"))
            return;
        Asset a;
        Text error;
        if (!load_image(path.c_str(), a, error)) {
            fl_alert("%s", T(error.c_str()));
            return;
        }
        float width = fmin(420.f, editor.deck.width - 96), height = width * a.height / a.width;
        if (height > editor.deck.height - 96) {
            height = editor.deck.height - 96;
            width = height * a.width / a.height;
        }
        if (!editor.insert_image(
                a.width, a.height, a.rgba.c_str(), a.rgba.size(),
                {(editor.deck.width - width) / 2, (editor.deck.height - height) / 2, width, height}))
            message(T(editor.error.c_str()));
        else
            changed();
        return;
    }
    Object o;
    o.kind = kind;
    o.rect = {120, 110, kind == Kind::Text ? 480.f : 240.f, kind == Kind::Text ? 140.f : 140.f};
    if (kind == Kind::Rectangle || kind == Kind::Ellipse) {
        o.style.filled = true;
        o.style.outlined = true;
    } else if (kind == Kind::Line || kind == Kind::Arrow) {
        o.rect.h = 40;
        o.style.outlined = true;
    }
    uint32_t id = 0;
    if (!editor.insert_object(o, &id)) {
        message(T(editor.error.c_str()));
        return;
    }
    changed();
    if (kind == Kind::Text)
        canvas->edit(id);
    else
        canvas->take_focus();
}
bool PptWindow::style_selection(Command c) {
    Array<Object> replacements;
    const Slide &s = editor.deck.slides[editor.current];
    float size = 28;
    if (c == Command::Size) {
        char *end = nullptr;
        double v = strtod(size_->value(), &end);
        if (!end || *end || !isfinite(v) || v < 6 || v > 144) {
            message(T("Enter a font size from 6 to 144 points"));
            return false;
        }
        size = float(v);
    }
    const Object *first =
        editor.selected.empty() ? nullptr : editor.deck.object(editor.current, editor.selected[0]);
    bool on = first ? (c == Command::Bold        ? !first->style.bold
                       : c == Command::Italic    ? !first->style.italic
                       : c == Command::Underline ? !first->style.underline
                                                 : !first->style.bullets)
                    : false;
    for (size_t i = 0; i < s.objects.size(); ++i)
        if (selected_id(editor.selected, s.objects[i].id) && s.objects[i].kind == Kind::Text) {
            Object o = s.objects[i];
            switch (c) {
            case Command::Font:
                o.style.family = uint8_t(font_->value());
                break;
            case Command::Size:
                o.style.font_size = size;
                break;
            case Command::Bold:
                o.style.bold = on;
                break;
            case Command::Italic:
                o.style.italic = on;
                break;
            case Command::Underline:
                o.style.underline = on;
                break;
            case Command::Bullets:
                o.style.bullets = on;
                break;
            case Command::TextLeft:
                o.style.alignment = Alignment::Left;
                break;
            case Command::TextCenter:
                o.style.alignment = Alignment::Center;
                break;
            case Command::TextRight:
                o.style.alignment = Alignment::Right;
                break;
            default:
                break;
            }
            if (!replacements.push(o)) {
                message(T("Not enough memory"));
                return false;
            }
        }
    if (replacements.empty()) {
        message(T("Select a text box first"));
        return false;
    }
    return editor.update_objects(replacements);
}
void PptWindow::theme(unsigned index) {
    Deck next;
    if (!next.clone_from(editor.deck)) {
        message(T("Not enough memory"));
        return;
    }
    const uint32_t backgrounds[] = {0xeaf2fa, 0xeaf5ef, 0xffffff}, text[] = {0x173c63, 0x18513a, 0x30343b},
                   accent[] = {0x3478b8, 0x32835c, 0x687789};
    Slide &s = next.slides[editor.current];
    s.background = backgrounds[index];
    for (size_t i = 0; i < s.objects.size(); ++i) {
        Object &o = s.objects[i];
        o.style.text = text[index];
        o.style.stroke = accent[index];
        o.style.fill = accent[index];
    }
    if (!editor.commit(static_cast<Deck &&>(next)))
        message(T(editor.error.c_str()));
    else
        changed();
}
void PptWindow::act(Command c) {
    if (busy_)
        return;
    updating_ = true;
    bool committed = commit_inputs();
    updating_ = false;
    if (!committed)
        return;
    canvas->cancel_drag();
    Fl::compose_reset();
    bool ok = true;
    switch (c) {
    case Command::New:
        if (confirm_discard()) {
            Deck next;
            if (next.init() && editor.replace_deck(static_cast<Deck &&>(next))) {
                discard_recovery();
                path_.clear();
                stamp_ = {};
                saved_revision_ = editor.deck.revision;
                notes_page_ = SIZE_MAX;
                changed();
                canvas->fit();
            }
        }
        return;
    case Command::Open: {
        Text path;
        if (choose_path(path, false, "rppt") && confirm_discard())
            open_file(path.c_str());
        return;
    }
    case Command::Save:
        save(false);
        return;
    case Command::SaveAs:
        save(true);
        return;
    case Command::ExportPng:
        export_file(false);
        return;
    case Command::ExportHtml:
        export_file(true);
        return;
    case Command::Quit:
        if (confirm_discard()) {
            discard_recovery();
            hide();
        }
        return;
    case Command::Example:
        if (confirm_discard()) {
            Deck d;
            Text error;
            if (!example_deck(d, error) || !editor.replace_deck(static_cast<Deck &&>(d))) {
                message(T(error.empty() ? editor.error.c_str() : error.c_str()));
                return;
            }
            discard_recovery();
            path_.clear();
            stamp_ = {};
            saved_revision_ = 0;
            notes_page_ = SIZE_MAX;
            changed();
            canvas->fit();
        }
        return;
    case Command::About:
        fl_message("ReliefPPT\n%s", T("A native ReliefOS presentation editor.\nDouble-click text to edit. "
                                      "Drag handles to resize.\nF5: present · Ctrl+S: save · Esc: finish "
                                      "editing\nUse .rppt to keep editable slides and embedded pictures."));
        return;
    case Command::Undo:
        ok = editor.undo();
        notes_page_ = SIZE_MAX;
        break;
    case Command::Redo:
        ok = editor.redo();
        notes_page_ = SIZE_MAX;
        break;
    case Command::Copy:
        copy_selection(false);
        return;
    case Command::Cut:
        copy_selection(true);
        return;
    case Command::Paste:
        paste();
        return;
    case Command::Delete:
        ok = editor.delete_selected();
        break;
    case Command::Duplicate:
        ok = editor.duplicate_selected();
        break;
    case Command::SelectAll:
        editor.select_all();
        selected();
        canvas->redraw();
        return;
    case Command::NewSlide:
        ok = editor.add_slide(Layout(layout_->value()));
        notes_page_ = SIZE_MAX;
        break;
    case Command::CopySlide:
        ok = editor.duplicate_slide();
        notes_page_ = SIZE_MAX;
        break;
    case Command::DeleteSlide:
        ok = editor.delete_slide();
        notes_page_ = SIZE_MAX;
        break;
    case Command::SlideUp:
        ok = editor.move_slide(-1);
        notes_page_ = SIZE_MAX;
        break;
    case Command::SlideDown:
        ok = editor.move_slide(1);
        notes_page_ = SIZE_MAX;
        break;
    case Command::TextBox:
        insert(Kind::Text);
        return;
    case Command::Rectangle:
        insert(Kind::Rectangle);
        return;
    case Command::Ellipse:
        insert(Kind::Ellipse);
        return;
    case Command::Line:
        insert(Kind::Line);
        return;
    case Command::Arrow:
        insert(Kind::Arrow);
        return;
    case Command::Image:
        insert(Kind::Image);
        return;
    case Command::Font:
    case Command::Size:
    case Command::Bold:
    case Command::Italic:
    case Command::Underline:
    case Command::TextLeft:
    case Command::TextCenter:
    case Command::TextRight:
    case Command::Bullets:
        ok = style_selection(c);
        break;
    case Command::TextColor:
    case Command::FillColor:
    case Command::LineColor:
    case Command::Background:
        color_dialog(c);
        return;
    case Command::Properties:
        object_dialog();
        return;
    case Command::AlignLeft:
    case Command::AlignCenter:
    case Command::AlignRight:
    case Command::AlignTop:
    case Command::AlignMiddle:
    case Command::AlignBottom:
        ok = editor.align_selected(Align(int(c) - int(Command::AlignLeft)));
        break;
    case Command::Top:
        ok = editor.layer_selected(Layer::Top);
        break;
    case Command::Bottom:
        ok = editor.layer_selected(Layer::Bottom);
        break;
    case Command::Raise:
        ok = editor.layer_selected(Layer::Up);
        break;
    case Command::Lower:
        ok = editor.layer_selected(Layer::Down);
        break;
    case Command::Wide:
        ok = editor.resize_page(true);
        canvas->fit();
        break;
    case Command::Standard:
        ok = editor.resize_page(false);
        canvas->fit();
        break;
    case Command::ThemeBlue:
        theme(0);
        return;
    case Command::ThemeGreen:
        theme(1);
        return;
    case Command::ThemeLight:
        theme(2);
        return;
    case Command::Grid:
    case Command::Snap:
        canvas->redraw();
        return;
    case Command::Notes:
        notes_visible_ = notes_toggle_->value();
        if (notes_visible_)
            notes_group_->show();
        else
            notes_group_->hide();
        resize(x(), y(), w(), h());
        return;
    case Command::Fit:
        canvas->fit();
        return;
    case Command::Zoom:
        if (zoom_->value() >= 0 && zoom_->value() < 8)
            canvas->set_zoom(Zooms[zoom_->value()]);
        return;
    case Command::Start:
        slideshow(false);
        return;
    case Command::StartCurrent:
        slideshow(true);
        return;
    }
    if (ok) {
        changed();
        canvas->take_focus();
    } else if (!editor.error.empty())
        message(T(editor.error.c_str()));
    selected();
}
void PptWindow::resize(int X, int Y, int W, int H) {
    Fl_Double_Window::resize(X, Y, W, H);
    if (!canvas)
        return;
    top_->resize(0, 0, W, 34);
    for (int i = 0; i < 3; ++i)
        quick_[i]->resize(W - 200 + i * 65, 4, 61, 26);
    ribbon_->resize(0, 34, W, 100);
    ribbon_scroll_->resize(0, 34, W, 100);
    thumbs->resize(0, 136, 190, H - 164);
    canvas->resize(192, 136, W - 192, H - 164 - (notes_visible_ ? 80 : 0));
    notes_group_->resize(192, H - 108, W - 192, 80);
    notes_->resize(250, H - 101, W - 265, 65);
    status_->resize(8, H - 27, W / 2 - 16, 26);
    summary_->resize(W / 2, H - 27, W / 2 - 8, 26);
    selected();
}
int PptWindow::handle(int e) {
    if ((e == FL_KEYDOWN || e == FL_SHORTCUT) && !busy_) {
        int key = Fl::event_key();
        bool ctrl = Fl::event_state(FL_CTRL), shift = Fl::event_state(FL_SHIFT);
        if (key == FL_Escape) {
            if (canvas->editing())
                canvas->finish_edit();
            else {
                canvas->cancel_drag();
                editor.clear_selection();
                canvas->take_focus();
                changed();
            }
            return 1;
        }
        if (key == FL_F + 5) {
            act(shift ? Command::StartCurrent : Command::Start);
            return 1;
        }
        if (ctrl && !Fl::event_state(FL_ALT) && !Fl::compose_state) {
            bool input = canvas->editing() || Fl::focus() == notes_ || Fl::focus() == size_->input();
            if (input && (key == 'a' || key == 'c' || key == 'x' || key == 'v' || key == 'z' || key == 'y'))
                return Fl_Double_Window::handle(e);
            Command c;
            switch (key) {
            case 'n':
                c = Command::New;
                break;
            case 'o':
                c = Command::Open;
                break;
            case 's':
                c = shift ? Command::SaveAs : Command::Save;
                break;
            case 'z':
                c = shift ? Command::Redo : Command::Undo;
                break;
            case 'y':
                c = Command::Redo;
                break;
            case 'c':
                c = Command::Copy;
                break;
            case 'x':
                c = Command::Cut;
                break;
            case 'v':
                c = Command::Paste;
                break;
            case 'a':
                c = Command::SelectAll;
                break;
            case 'd':
                c = Command::Duplicate;
                break;
            case 'm':
                c = Command::NewSlide;
                break;
            case 'b':
                c = Command::Bold;
                break;
            case 'i':
                c = Command::Italic;
                break;
            case 'u':
                c = Command::Underline;
                break;
            default:
                return Fl_Double_Window::handle(e);
            }
            act(c);
            return 1;
        }
    }
    return Fl_Double_Window::handle(e);
}
} // namespace rp
