#ifndef RELIEFMAIL_UI_WIDGETS_H
#define RELIEFMAIL_UI_WIDGETS_H

#include "model.h"
#include "mail_ui_layout.h"
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Text_Display.H>

namespace rmail {

/* User supplied mail strings can contain FLTK label metacharacters. These
 * widgets draw their owned UTF-8 text directly instead of parsing labels. */
class PlainTextBox : public Fl_Box {
    Text text_;

public:
    PlainTextBox(int x, int y, int width, int height, const char *text = nullptr);
    bool text(const char *value);
    const Text &text() const { return text_; }
    void draw() override;
};

/* Layout a literal text box as scroll content using the same UTF-8 wrapping
 * metrics as its draw path. The content remains bounded by max_height. */
int plain_text_wrapped_height(const char *text, int width, int font_size,
    int max_height);
void fit_plain_text_scroll(Fl_Scroll *scroll, PlainTextBox *content,
    int max_height);

class PlainTextButton : public Fl_Button {
    Text text_;
    int alignment_ = FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP;

public:
    PlainTextButton(int x, int y, int width, int height, const char *text = nullptr);
    bool text(const char *value);
    const Text &text() const { return text_; }
    void alignment(int value) { alignment_ = value; redraw(); }
    void draw() override;
};

class NavigationButton : public PlainTextButton {
    NavigationButton *previous_ = nullptr;
    NavigationButton *next_ = nullptr;

public:
    NavigationButton(int x, int y, int width, int height, const char *text = nullptr);
    void neighbors(NavigationButton *previous, NavigationButton *next) {
        previous_ = previous;
        next_ = next;
    }
    void previous(NavigationButton *widget) { previous_ = widget; }
    void next(NavigationButton *widget) { next_ = widget; }
    NavigationButton *navigation_neighbor(bool forward) const {
        return forward ? next_ : previous_;
    }
    int handle(int event) override;
    void draw() override;
};

/* Compact message row. All message/account text is drawn as
 * literal UTF-8 and remains separate from FLTK's label-symbol parser. */
class MailMessageRowButton : public Fl_Button {
    Text sender_;
    Text subject_;
    Text context_;
    Text received_;
    Text attachment_state_;
    bool unread_ = false;
    bool flagged_ = false;

public:
    MailMessageRowButton(int x, int y, int width, int height);
    bool message(const char *sender, const char *subject, const char *context,
        const char *received, const char *attachment_state, bool unread, bool flagged);
    void draw() override;
};

/* A reusable modal confirmation that places untrusted/literal text in a
 * read-only text view, never in FLTK's @-aware message-label parser. */
int literal_confirmation(const char *title, const char *prompt, const char *literal,
    const char *cancel_label, const char *confirm_label);
bool literal_text_confirmation(const char *title, const char *prompt, const char *required_text,
    const char *cancel_label, const char *confirm_label);

}

#endif
