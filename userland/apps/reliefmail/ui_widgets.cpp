#include "ui_widgets.h"
#include "../fltk_support.h"
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/fl_draw.H>
#include <string.h>

namespace rmail {

namespace {

struct LiteralConfirmationState {
    Fl_Window *window = nullptr;
    int result = 0;
};

struct LiteralConfirmationButton {
    LiteralConfirmationState *state = nullptr;
    int result = 0;
};

struct LiteralTextConfirmationState {
    Fl_Window *window = nullptr;
    Fl_Input *input = nullptr;
    const char *required = nullptr;
    bool accepted = false;
};

struct LiteralTextConfirmationButton {
    LiteralTextConfirmationState *state = nullptr;
    bool confirm = false;
};

void literal_confirmation_button_cb(Fl_Widget *, void *context) {
    LiteralConfirmationButton *button = static_cast<LiteralConfirmationButton *>(context);
    if (!button || !button->state) return;
    button->state->result = button->result;
    if (button->state->window) button->state->window->hide();
}

void literal_confirmation_close_cb(Fl_Widget *, void *context) {
    LiteralConfirmationState *state = static_cast<LiteralConfirmationState *>(context);
    if (state) {
        state->result = 0;
        if (state->window) state->window->hide();
    }
}

void literal_text_confirmation_button_cb(Fl_Widget *, void *context) {
    LiteralTextConfirmationButton *button = static_cast<LiteralTextConfirmationButton *>(context);
    if (!button || !button->state) return;
    LiteralTextConfirmationState *state = button->state;
    if (!button->confirm) {
        if (state->window) state->window->hide();
        return;
    }
    const char *entered = state->input ? state->input->value() : nullptr;
    state->accepted = entered && state->required && !strcmp(entered, state->required);
    if (state->window && state->accepted) state->window->hide();
}

void literal_text_confirmation_close_cb(Fl_Widget *, void *context) {
    LiteralTextConfirmationState *state = static_cast<LiteralTextConfirmationState *>(context);
    if (state) {
        state->accepted = false;
        if (state->window) state->window->hide();
    }
}

size_t utf8_sequence_size(const unsigned char *text, size_t remaining) {
    if (!remaining || text[0] < 0x80) return 1;
    if ((text[0] & 0xe0) == 0xc0 && remaining >= 2) return 2;
    if ((text[0] & 0xf0) == 0xe0 && remaining >= 3) return 3;
    if ((text[0] & 0xf8) == 0xf0 && remaining >= 4) return 4;
    return 1;
}

void draw_literal_line(const char *text, size_t size, int x, int &baseline,
    int width, int bottom, int line_height, Fl_Align alignment) {
    if (!size) {
        baseline += line_height;
        return;
    }
    size_t offset = 0;
    while (offset < size && baseline + fl_descent() <= bottom) {
        size_t end = offset;
        size_t last_space = SIZE_MAX;
        size_t after_space = SIZE_MAX;
        double measured = 0;
        while (end < size && end - offset < 4095) {
            const size_t character_size = utf8_sequence_size(
                reinterpret_cast<const unsigned char *>(text + end), size - end);
            const double character_width = fl_width(text + end, static_cast<int>(character_size));
            if (end > offset && measured + character_width > width) break;
            if (character_size == 1 && (text[end] == ' ' || text[end] == '\t')) {
                last_space = end;
                after_space = end + 1;
            }
            measured += character_width;
            end += character_size;
        }
        if (end == offset) {
            end += utf8_sequence_size(reinterpret_cast<const unsigned char *>(text + end), size - end);
        }
        size_t draw_end = end;
        size_t next = end;
        if (end < size && last_space != SIZE_MAX && last_space > offset) {
            draw_end = last_space;
            next = after_space;
        }
        char line[4096];
        const size_t line_size = draw_end - offset;
        memcpy(line, text + offset, line_size);
        line[line_size] = 0;
        const double line_width = fl_width(line);
        int draw_x = x;
        if ((alignment & FL_ALIGN_RIGHT) != 0) draw_x += width - static_cast<int>(line_width + 0.5);
        else if ((alignment & FL_ALIGN_LEFT) == 0) draw_x += (width - static_cast<int>(line_width + 0.5)) / 2;
        fl_draw(line, draw_x, baseline);
        baseline += line_height;
        offset = next;
        while (offset < size && (text[offset] == ' ' || text[offset] == '\t')) ++offset;
    }
}

void draw_literal_text(const char *text, int x, int y, int width, int height,
    Fl_Align alignment) {
    const int line_height = fl_height();
    if (line_height <= 0 || width <= 0 || height <= 0) return;
    int baseline = y + line_height - fl_descent();
    const int bottom = y + height;
    const size_t size = strlen(text);
    size_t start = 0;
    for (size_t at = 0; at <= size; ++at) {
        if (at != size && text[at] != '\n' && text[at] != '\r') continue;
        draw_literal_line(text + start, at - start, x, baseline, width,
            bottom, line_height, alignment);
        if (at < size && text[at] == '\r' && at + 1 < size && text[at + 1] == '\n') ++at;
        start = at + 1;
        if (baseline + fl_descent() > bottom) break;
    }
}

size_t wrapped_literal_line_count(const char *text, size_t size, int width) {
    if (!size || width <= 0) return 1;
    size_t lines = 0;
    size_t offset = 0;
    while (offset < size) {
        size_t end = offset;
        size_t last_space = SIZE_MAX;
        size_t after_space = SIZE_MAX;
        double measured = 0;
        while (end < size && end - offset < 4095) {
            const size_t character_size = utf8_sequence_size(
                reinterpret_cast<const unsigned char *>(text + end), size - end);
            const double character_width = fl_width(text + end, static_cast<int>(character_size));
            if (end > offset && measured + character_width > width) break;
            if (character_size == 1 && (text[end] == ' ' || text[end] == '\t')) {
                last_space = end;
                after_space = end + 1;
            }
            measured += character_width;
            end += character_size;
        }
        if (end == offset) {
            end += utf8_sequence_size(reinterpret_cast<const unsigned char *>(text + end),
                size - end);
        }
        size_t next = end;
        if (end < size && last_space != SIZE_MAX && last_space > offset)
            next = after_space;
        ++lines;
        offset = next;
        while (offset < size && (text[offset] == ' ' || text[offset] == '\t')) ++offset;
    }
    return lines;
}

int wrapped_literal_text_height(const char *value, int width, Fl_Font font,
    int font_size, int padding, int minimum, int maximum) {
    const char *text = value ? value : "";
    const size_t size = strlen(text);
    fl_font(font, font_size);
    size_t lines = 0;
    size_t start = 0;
    for (size_t at = 0; at <= size; ++at) {
        if (at != size && text[at] != '\n' && text[at] != '\r') continue;
        lines += wrapped_literal_line_count(text + start, at - start, width);
        if (at < size && text[at] == '\r' && at + 1 < size && text[at + 1] == '\n') ++at;
        start = at + 1;
    }
    const size_t line_height = static_cast<size_t>(fl_height() > 0 ? fl_height() : 1);
    const size_t measured = lines * line_height + static_cast<size_t>(padding);
    if (measured < static_cast<size_t>(minimum)) return minimum;
    if (measured > static_cast<size_t>(maximum)) return maximum;
    return static_cast<int>(measured);
}

class LiteralPromptBox : public PlainTextBox {
public:
    LiteralPromptBox(int x, int y, int width, int height, const char *text)
        : PlainTextBox(x, y, width, height, text) {
        box(FL_NO_BOX);
        labelfont(FL_HELVETICA);
        labelsize(15);
        labelcolor(FL_FOREGROUND_COLOR);
    }

    void draw() override {
        fl_color(color());
        fl_rectf(x(), y(), w(), h());
        fl_font(labelfont(), labelsize());
        fl_color(active_r() ? labelcolor() : fl_inactive(labelcolor()));
        fl_push_clip(x(), y(), w(), h());
        draw_literal_text(text().c_str(), x() + 4, y() + 2, w() - 8, h() - 4,
            FL_ALIGN_LEFT);
        fl_pop_clip();
    }
};

}

int plain_text_wrapped_height(const char *text, int width, int font_size,
    int max_height) {
    return wrapped_literal_text_height(text, width, FL_HELVETICA, font_size,
        8, 20, max_height);
}

void fit_plain_text_scroll(Fl_Scroll *scroll, PlainTextBox *content,
    int max_height) {
    if (!scroll || !content || max_height <= 0) return;
    content->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    const int text_width = content->w() > 8 ? content->w() - 8 : 1;
    const int natural_height = plain_text_wrapped_height(content->text().c_str(),
        text_width, content->labelsize(), max_height);
    scroll->scroll_to(0, 0);
    content->resize(content->x(), content->y(), content->w(), natural_height);
    scroll->redraw();
    content->redraw();
}

int literal_confirmation(const char *title, const char *prompt, const char *literal,
    const char *cancel_label, const char *confirm_label) {
    Fl_Text_Buffer literal_buffer;
    const bool has_literal = literal && *literal;
    if (has_literal) literal_buffer.text(literal);
    constexpr int dialog_width = 600;
    constexpr int content_width = dialog_width - 36;
    constexpr int prompt_y = 14;
    constexpr int prompt_font_size = 15;
    const int prompt_height = wrapped_literal_text_height(prompt, content_width - 8,
        FL_HELVETICA, prompt_font_size, 8, 36, 180);
    const int details_height = has_literal ? wrapped_literal_text_height(literal,
        content_width - 24, static_cast<Fl_Font>(20), 14, 8, 36, 204) : 0;
    const int details_y = prompt_y + prompt_height + 12;
    const int buttons_y = has_literal ? details_y + details_height + 16 :
        prompt_y + prompt_height + 20;
    const int dialog_height = buttons_y + 30 + 16;
    Fl_Double_Window dialog(dialog_width, dialog_height, title ? title : "");
    dialog.set_modal();
    LiteralConfirmationState state;
    state.window = &dialog;
    dialog.callback(literal_confirmation_close_cb, &state);
    dialog.begin();
    new LiteralPromptBox(18, prompt_y, content_width, prompt_height, prompt ? prompt : "");
    if (has_literal) {
        Fl_Text_Display *literal_view = new Fl_Text_Display(18, details_y, content_width,
            details_height);
        literal_view->box(FL_DOWN_BOX);
        literal_view->buffer(&literal_buffer);
        literal_view->textfont(20);
        literal_view->textsize(14);
        literal_view->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
        literal_view->hide_cursor();
    }
    Fl_Button *cancel = new Fl_Button(300, buttons_y, 128, 30,
        cancel_label ? cancel_label : "");
    Fl_Button *confirm = new Fl_Button(444, buttons_y, 138, 30,
        confirm_label ? confirm_label : "");
    LiteralConfirmationButton cancel_action{&state, 0};
    LiteralConfirmationButton confirm_action{&state, 1};
    cancel->callback(literal_confirmation_button_cb, &cancel_action);
    confirm->callback(literal_confirmation_button_cb, &confirm_action);
    dialog.end();
    cancel->take_focus();
    dialog.show();
    while (dialog.shown()) Fl::wait();
    return state.result;
}

bool literal_text_confirmation(const char *title, const char *prompt, const char *required_text,
    const char *cancel_label, const char *confirm_label) {
    if (!required_text || !*required_text || strlen(required_text) > 128) return false;
    constexpr int dialog_width = 600;
    constexpr int content_width = dialog_width - 36;
    constexpr int prompt_y = 14;
    const int prompt_height = wrapped_literal_text_height(prompt ? prompt : "",
        content_width - 8, FL_HELVETICA, 15, 8, 36, 150);
    const int required_height = wrapped_literal_text_height(required_text,
        content_width - 24, FL_HELVETICA, 14, 8, 36, 104);
    const int input_label_y = prompt_y + prompt_height + required_height + 26;
    const int input_y = input_label_y + 18;
    const int buttons_y = input_y + 36;
    Fl_Double_Window dialog(dialog_width, buttons_y + 46, title ? title : "");
    dialog.set_modal();
    LiteralTextConfirmationState state;
    state.window = &dialog;
    state.required = required_text;
    dialog.callback(literal_text_confirmation_close_cb, &state);
    dialog.begin();
    new LiteralPromptBox(18, prompt_y, content_width, prompt_height, prompt ? prompt : "");
    new LiteralPromptBox(30, prompt_y + prompt_height + 4,
        content_width - 24, required_height, required_text);
    Fl_Box *input_label = new Fl_Box(18, input_label_y, content_width, 18, T("Type the text above"));
    input_label->align(FL_ALIGN_TOP_LEFT | FL_ALIGN_INSIDE);
    input_label->labelsize(13);
    state.input = new Fl_Input(18, input_y, content_width, 28);
    state.input->when(FL_WHEN_ENTER_KEY);
    Fl_Button *cancel = new Fl_Button(300, buttons_y, 128, 30,
        cancel_label ? cancel_label : "");
    Fl_Button *confirm = new Fl_Button(444, buttons_y, 138, 30,
        confirm_label ? confirm_label : "");
    LiteralTextConfirmationButton cancel_action{&state, false};
    LiteralTextConfirmationButton confirm_action{&state, true};
    cancel->callback(literal_text_confirmation_button_cb, &cancel_action);
    confirm->callback(literal_text_confirmation_button_cb, &confirm_action);
    dialog.end();
    state.input->take_focus();
    dialog.show();
    while (dialog.shown()) Fl::wait();
    return state.accepted;
}

PlainTextBox::PlainTextBox(int x, int y, int width, int height, const char *text)
    : Fl_Box(x, y, width, height, "") {
    /* FL_NO_BOX widgets paint their own background; inherit the actual pane
     * surface instead of FLTK's unrelated global gray. */
    color(parent() ? parent()->color() : FL_BACKGROUND_COLOR);
    if (text) this->text(text);
}

bool PlainTextBox::text(const char *value) {
    if (!text_.set(value ? value : "")) return false;
    redraw();
    return true;
}

void PlainTextBox::draw() {
    if (box() == FL_NO_BOX) {
        /* A no-box Fl_Box does not repaint its client area; replacing a long
         * status string would otherwise leave the old glyphs behind. */
        fl_color(color());
        fl_rectf(x(), y(), w(), h());
    } else {
        Fl_Box::draw();
    }
    fl_font(labelfont(), labelsize());
    fl_color(active_r() ? labelcolor() : fl_inactive(labelcolor()));
    fl_push_clip(x(), y(), w(), h());
    /* Disable FLTK's @symbol parsing for literal user-visible text. */
    fl_draw(text_.c_str(), x() + 4, y() + 2, w() - 8, h() - 4, align(), nullptr, 0);
    fl_pop_clip();
}

PlainTextButton::PlainTextButton(int x, int y, int width, int height, const char *text)
    : Fl_Button(x, y, width, height, "") {
    if (text) this->text(text);
}

bool PlainTextButton::text(const char *value) {
    if (!text_.set(value ? value : "")) return false;
    redraw();
    return true;
}

void PlainTextButton::draw() {
    Fl_Button::draw();
    fl_font(labelfont(), labelsize());
    const Fl_Color background = value() ? selection_color() : color();
    const Fl_Color text_color = active_r() ? fl_contrast(labelcolor(), background) :
        fl_inactive(fl_contrast(labelcolor(), background));
    fl_color(text_color);
    fl_push_clip(x() + 6, y() + 2, w() - 12, h() - 4);
    /* Keep the stored text literal; it may contain strings such as @B/@C1. */
    draw_literal_text(text_.c_str(), x() + 6, y() + 2, w() - 12, h() - 4, alignment_);
    fl_pop_clip();
}

NavigationButton::NavigationButton(int x, int y, int width, int height, const char *text)
    : PlainTextButton(x, y, width, height, text) {
    /* Folder navigation is a keyboard surface; keep FLTK's native focus ring. */
    set_visible_focus();
}

int NavigationButton::handle(int event) {
    if (event == FL_KEYDOWN) {
        bool forward = false;
        bool navigation_key = false;
        if (Fl::event_key() == FL_Up) navigation_key = true;
        else if (Fl::event_key() == FL_Down) { forward = true; navigation_key = true; }
        if (navigation_key) {
            NavigationButton *target = navigation_next_active(forward ? next_ : previous_, forward);
            if (target && target->take_focus()) return 1;
        }
        if (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter) {
            do_callback();
            return 1;
        }
    }
    return PlainTextButton::handle(event);
}

void NavigationButton::draw() {
    Fl_Button::draw();
    const bool selected = value() != 0;
    const char *line = text().c_str();
    const char *split = strchr(line, '\n');
    const Fl_Color background = selected ? selection_color() : color();
    const Fl_Color text_color = active_r() ? fl_contrast(labelcolor(), background) :
        fl_inactive(fl_contrast(labelcolor(), background));
    fl_color(text_color);
    fl_push_clip(x() + 9, y() + 1, w() - 18, h() - 2);
    fl_font(selected || split ? FL_HELVETICA_BOLD : FL_HELVETICA, 13);
    if (split) {
        fl_draw(line, static_cast<int>(split - line), x() + 10, y() + 16);
        fl_font(FL_HELVETICA, 11);
        fl_draw(split + 1, x() + 10, y() + 33);
    } else {
        fl_draw(line, x() + 10, y(), w() - 20, h(), FL_ALIGN_LEFT | FL_ALIGN_CLIP, nullptr, 0);
    }
    fl_pop_clip();
}

MailMessageRowButton::MailMessageRowButton(int x, int y, int width, int height)
    : Fl_Button(x, y, width, height, "") {
}

bool MailMessageRowButton::message(const char *sender, const char *subject,
    const char *context, const char *received, const char *attachment_state,
    bool unread, bool flagged) {
    Text next_sender, next_subject, next_context, next_received, next_attachment;
    if (!next_sender.set(sender ? sender : "") || !next_subject.set(subject ? subject : "") ||
        !next_context.set(context ? context : "") || !next_received.set(received ? received : "") ||
        !next_attachment.set(attachment_state ? attachment_state : "")) return false;
    sender_ = static_cast<Text &&>(next_sender);
    subject_ = static_cast<Text &&>(next_subject);
    context_ = static_cast<Text &&>(next_context);
    received_ = static_cast<Text &&>(next_received);
    attachment_state_ = static_cast<Text &&>(next_attachment);
    unread_ = unread;
    flagged_ = flagged;
    redraw();
    return true;
}

void MailMessageRowButton::draw() {
    Fl_Button::draw();
    const Fl_Color background = value() ? selection_color() : color();
    const Fl_Color text_color = active_r() ? fl_contrast(labelcolor(), background) :
        fl_inactive(fl_contrast(labelcolor(), background));
    const int left = x() + 12;
    const int inner_width = w() - 24;
    fl_push_clip(x() + 7, y() + 2, w() - 14, h() - 4);
    fl_font(unread_ ? FL_HELVETICA_BOLD : FL_HELVETICA, 14);
    fl_color(text_color);
    const int metadata_width = flagged_ ? 16 : 0;
    fl_draw(sender_.c_str(), left, y() + 5, inner_width - 86 - metadata_width, 18,
        FL_ALIGN_LEFT | FL_ALIGN_CLIP, nullptr, 0);
    if (flagged_) {
        fl_font(FL_HELVETICA, 12);
        fl_draw("*", x() + w() - 22, y() + 5, 14, 18,
            FL_ALIGN_RIGHT | FL_ALIGN_CLIP, nullptr, 0);
    }
    fl_font(FL_HELVETICA, 12);
    fl_color(text_color);
    const int received_width = flagged_ ? 56 : 74;
    fl_draw(received_.c_str(), x() + w() - 84, y() + 5, received_width, 18,
        FL_ALIGN_RIGHT | FL_ALIGN_CLIP, nullptr, 0);
    fl_font(unread_ ? FL_HELVETICA_BOLD : FL_HELVETICA, 14);
    fl_color(text_color);
    fl_draw(subject_.c_str(), left, y() + 27, inner_width, 18,
        FL_ALIGN_LEFT | FL_ALIGN_CLIP, nullptr, 0);
    fl_font(FL_HELVETICA, 11);
    fl_color(text_color);
    const int tag_width = attachment_state_.empty() ? 0 : 74;
    fl_draw(context_.c_str(), left, y() + 49, inner_width - tag_width - 8, 16,
        FL_ALIGN_LEFT | FL_ALIGN_CLIP, nullptr, 0);
    if (tag_width) {
        const int attachment_x = x() + w() - tag_width - 8;
        fl_draw(attachment_state_.c_str(), attachment_x, y() + 49, tag_width, 16,
            FL_ALIGN_LEFT | FL_ALIGN_CLIP, nullptr, 0);
    }
    fl_pop_clip();
}

}
