#include "ui_widgets.h"
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/x.H>
#include <X11/Xutil.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

using namespace rmail;

namespace {
constexpr int kMaxDetailHeight = 131072;

unsigned long visible_ink(Fl_Double_Window &window, const Fl_Scroll &scroll,
    int top, int height) {
    XImage *image = XGetImage(fl_display, fl_xid(&window), scroll.x() + 12,
        scroll.y() + top, scroll.w() - 36, height, AllPlanes, ZPixmap);
    assert(image);
    unsigned long ink = 0;
    const unsigned long white = WhitePixel(fl_display, DefaultScreen(fl_display));
    for (int y = 0; y < image->height; ++y)
        for (int x = 0; x < image->width; ++x)
            if (XGetPixel(image, x, y) != white) ++ink;
    XDestroyImage(image);
    return ink;
}

void flush_window() {
    Fl::check();
    Fl::flush();
    XSync(fl_display, False);
}

void append_line(char *buffer, size_t capacity, size_t &used, const char *line) {
    const size_t line_size = strlen(line);
    assert(used + line_size + 2 < capacity);
    memcpy(buffer + used, line, line_size);
    used += line_size;
    buffer[used++] = '\n';
    buffer[used] = 0;
}
}

int main() {
    Fl_Double_Window window(560, 360, "ReliefMail delivery detail regression");
    window.begin();
    Fl_Scroll scroll(16, 20, 528, 152);
    scroll.type(Fl_Scroll::VERTICAL);
    scroll.box(FL_DOWN_BOX);
    scroll.color(FL_WHITE);
    scroll.begin();
    PlainTextBox detail(scroll.x() + 4, scroll.y() + 4, scroll.w() - 28,
        1000, "");
    detail.box(FL_NO_BOX);
    detail.color(FL_WHITE);
    detail.labelsize(12);
    detail.labelcolor(FL_BLACK);
    detail.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    scroll.end();
    window.end();

    char short_text[2048] = {};
    size_t used = 0;
    append_line(short_text, sizeof(short_text), used, "Operation #1 · Archive pending · delivery count 1");
    append_line(short_text, sizeof(short_text), used, "Message-ID: <rich-native-fixture@example.test>");
    append_line(short_text, sizeof(short_text), used, "From: alice@example.test");
    append_line(short_text, sizeof(short_text), used, "To: bob@example.test");
    append_line(short_text, sizeof(short_text), used, "Archive target: Sent · account generation 22");
    append_line(short_text, sizeof(short_text), used, "last result: destination identity needs review");
    assert(detail.text(short_text));

    window.show();
    fit_plain_text_scroll(&scroll, &detail, kMaxDetailHeight);
    flush_window();

    const int short_height = plain_text_wrapped_height(short_text,
        detail.w() - 8, detail.labelsize(), kMaxDetailHeight);
    assert(short_height < 1000);
    assert(detail.h() == short_height);
    assert((detail.align() & FL_ALIGN_TOP) != 0);
    assert(visible_ink(window, scroll, 4, 30) > 20);

    char long_text[8192] = {};
    used = 0;
    for (int row = 0; row < 72; ++row) {
        char line[128];
        const int written = snprintf(line, sizeof(line),
            "Identity %02d account 22 folder generation 7 UIDVALIDITY 991547 UID %u result retained for review",
            row, static_cast<unsigned>(row + 1));
        assert(written > 0 && static_cast<size_t>(written) < sizeof(line));
        append_line(long_text, sizeof(long_text), used, line);
    }
    assert(detail.text(long_text));
    fit_plain_text_scroll(&scroll, &detail, kMaxDetailHeight);
    flush_window();
    const int wide_height = detail.h();
    assert(wide_height > scroll.h());
    assert(wide_height <= kMaxDetailHeight);
    assert(visible_ink(window, scroll, 4, 30) > 20);
    scroll.scroll_to(scroll.xposition(), 1000000);
    flush_window();
    assert(scroll.yposition() > 0);

    /* Match MailToolsWindow::resize: normalize the scroll before rebasing its
     * child to the resized container's coordinates. */
    scroll.scroll_to(0, 0);
    scroll.resize(scroll.x(), scroll.y(), 300, scroll.h());
    detail.resize(scroll.x() + 4, scroll.y() + 4, 272, detail.h());
    fit_plain_text_scroll(&scroll, &detail, kMaxDetailHeight);
    flush_window();
    assert(detail.h() > wide_height);
    assert(visible_ink(window, scroll, 4, 30) > 20);

    assert(detail.text(short_text));
    fit_plain_text_scroll(&scroll, &detail, kMaxDetailHeight);
    flush_window();
    assert(detail.h() < wide_height);
    assert(visible_ink(window, scroll, 4, 30) > 20);

    puts("ReliefMail detail widget layout suite passed");
    return 0;
}
