#include "compose_window.h"
#include "window.h"
#include "../fltk_support.h"
#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Color_Chooser.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/fl_draw.H>
#include <stdio.h>
#include <string.h>

namespace rmail {
namespace {

constexpr int kComposeTop = 34;
constexpr int kComposeRibbon = 100;
constexpr int kComposeStatus = 28;
constexpr int kComposeFieldsTop = kComposeTop + kComposeRibbon + 4;
constexpr int kFieldHeight = 28;
constexpr int kFieldGap = 3;
constexpr int kComposeRibbonContentWidth = 1100;

bool set_error(Text &error, const char *message) {
    if (!error.set(message ? message : T("The operation failed.")))
        error.set(T("The operation failed."));
    return false;
}

Fl_Font font_for(const rw::TextStyle &style) {
    const unsigned face = style.bold ? (style.italic ? 3u : 1u) : (style.italic ? 2u : 0u);
    return static_cast<Fl_Font>(16 + static_cast<unsigned>(style.family) * 4u + face);
}

rw::Metrics measure_mail_glyph(void *, uint32_t codepoint, const rw::TextStyle &style) {
    char bytes[5] = {};
    const unsigned count = rw::encode_utf8(codepoint, bytes);
    fl_font(font_for(style), style.size);
    rw::Metrics result;
    result.advance = static_cast<float>(fl_width(bytes, static_cast<int>(count)));
    result.ascent = static_cast<float>(fl_height() - fl_descent());
    result.descent = static_cast<float>(fl_descent());
    return result;
}

bool render_addresses(const Array<Address> &addresses, Text &output) {
    Array<char> bytes;
    Text error;
    for (size_t i = 0; i < addresses.size(); ++i) {
        Text mailbox;
        if (!serialize_address(addresses[i], mailbox, error)) return false;
        if (i && (!bytes.push(',') || !bytes.push(' '))) return false;
        for (size_t j = 0; j < mailbox.size(); ++j)
            if (!bytes.push(mailbox.c_str()[j])) return false;
    }
    return output.set(bytes.data(), bytes.size());
}

class ComposeClipboard {
public:
    rw::Document document;
    Array<MailLinkRange> links;
    Text plain;
    bool valid = false;
};

ComposeClipboard &compose_clipboard() {
    static ComposeClipboard clipboard;
    return clipboard;
}

void format_size(uint64_t size, char *output, size_t capacity) {
    if (size < 1024) snprintf(output, capacity, "%llu B", static_cast<unsigned long long>(size));
    else if (size < 1024u * 1024u)
        snprintf(output, capacity, "%.1f KiB", static_cast<double>(size) / 1024.0);
    else snprintf(output, capacity, "%.1f MiB", static_cast<double>(size) / (1024.0 * 1024.0));
}

const char *content_type_for_path(const char *path) {
    const char *dot = strrchr(path ? path : "", '.');
    if (!dot) return "application/octet-stream";
    if (!strcasecmp(dot, ".txt") || !strcasecmp(dot, ".md") || !strcasecmp(dot, ".csv")) return "text/plain";
    if (!strcasecmp(dot, ".html") || !strcasecmp(dot, ".htm")) return "text/html";
    if (!strcasecmp(dot, ".pdf")) return "application/pdf";
    if (!strcasecmp(dot, ".png")) return "image/png";
    if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(dot, ".gif")) return "image/gif";
    if (!strcasecmp(dot, ".eml")) return "application/octet-stream";
    return "application/octet-stream";
}

bool contact_choice_label(const WorkerComposeContactSuggestion &contact, Text &label) {
    Array<char> output;
    auto append_text = [&](const Text &text) {
        for (size_t i = 0; i < text.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(text.c_str()[i]);
            const char *replacement = nullptr;
            if (c == '@') replacement = "＠";
            else if (c == '/') replacement = "／";
            else if (c == '&') replacement = "＆";
            if (replacement) {
                for (size_t j = 0; replacement[j]; ++j)
                    if (!output.push(replacement[j])) return false;
            } else if (c < 0x20 || c == 0x7f) {
                if (!output.push(' ')) return false;
            } else if (!output.push(static_cast<char>(c))) return false;
        }
        return true;
    };
    return append_text(contact.display_name) && output.push(' ') && output.push('-') && output.push(' ') &&
        append_text(contact.address) && label.set(output.data(), output.size());
}

void request_fltk_paste(void *, void *target, int clipboard_buffer) {
    Fl::paste(*static_cast<Fl_Widget *>(target), clipboard_buffer);
}

}

class ComposeCanvas : public Fl_Widget {
    ComposeWindow *owner_;
    rw::Layout layout_;
    uint64_t layout_revision_ = 0;
    int layout_width_ = 0;
    int page_height_ = 0;
    int viewport_height_ = 0;
    bool dragging_ = false;
    int drag_x_ = 0;
    int drag_y_ = 0;
    float desired_x_ = -1;
    bool previous_affinity_ = false;

    bool rebuild() {
        rw::Document &document = owner_->editor().word_editor().doc;
        const int width = w() > 2000 ? 2000 : w();
        const int available_height = page_height_;
        if (width < 144 || available_height < 144) return false;
        if (document.revision == layout_revision_ && layout_width_ == width &&
            page_height_ == available_height) return true;
        rw::PageSettings page = document.page;
        page.width = static_cast<uint16_t>(width);
        page.height = static_cast<uint16_t>(available_height);
        page.left = page.right = 18;
        page.top = page.bottom = 12;
        page.header.clear(); page.footer.clear(); page.page_numbers = false;
        document.page = page;
        if (!layout_.build(document, measure_mail_glyph, nullptr)) {
            owner_->canvas_status(layout_.error.c_str());
            return false;
        }
        layout_revision_ = document.revision;
        layout_width_ = width;
        return true;
    }

    size_t hit_test(int local_x, int local_y, size_t *line_out = nullptr) {
        if (!rebuild() || page_height_ <= 0) return 0;
        unsigned page = static_cast<unsigned>(local_y / page_height_);
        if (page >= layout_.pages) page = layout_.pages ? layout_.pages - 1 : 0;
        const float y = static_cast<float>(local_y - static_cast<int>(page) * page_height_);
        const size_t offset = layout_.hit_test(page, static_cast<float>(local_x), y);
        if (line_out) *line_out = layout_.line_for(offset, false);
        return offset;
    }

    void move_caret(size_t offset, bool extend, bool previous = false) {
        rw::Editor &editor = owner_->editor().word_editor();
        const size_t anchor = extend ? editor.anchor : offset;
        editor.select(anchor, offset);
        previous_affinity_ = previous;
        desired_x_ = -1;
        redraw();
    }

    void draw_run(size_t start, size_t end, const rw::Line &line, unsigned page) {
        rw::Document &document = owner_->editor().word_editor().doc;
        rw::Editor &editor = owner_->editor().word_editor();
        const size_t selection_start = editor.selection_start();
        const size_t selection_end = editor.selection_end();
        for (size_t i = start; i < end; ++i) {
            const rw::GlyphBox &box = layout_.boxes[i];
            const int page_y = y() + static_cast<int>(page) * page_height_;
            const int gx = x() + static_cast<int>(box.x + 0.5f);
            const int gy = page_y + static_cast<int>(box.y + 0.5f);
            const int baseline = page_y + static_cast<int>(line.baseline + 0.5f);
            const float width = box.width;
            const bool selected = selection_start < selection_end && i >= selection_start && i < selection_end;
            const rw::TextStyle &style = document.style_at(i);
            Fl_Color foreground = fl_rgb_color(static_cast<uchar>((style.foreground >> 16) & 255),
                static_cast<uchar>((style.foreground >> 8) & 255),
                static_cast<uchar>(style.foreground & 255));
            if (selected) {
                fl_color(FL_SELECTION_COLOR);
                fl_rectf(gx, gy, static_cast<int>(width + 1.0f), static_cast<int>(line.height + 0.5f));
                foreground = fl_contrast(foreground, FL_SELECTION_COLOR);
            } else if (style.highlight) {
                fl_color(fl_rgb_color(255, 245, 170));
                fl_rectf(gx, gy, static_cast<int>(width + 1.0f), static_cast<int>(line.height + 0.5f));
            }
            char utf8[5] = {};
            const unsigned count = rw::encode_utf8(document.chars[i].codepoint, utf8);
            fl_font(font_for(style), style.size);
            fl_color(foreground);
            fl_draw(utf8, count, gx, baseline);
            if (style.underline || style.strike) {
                fl_color(foreground);
                if (style.underline) fl_line(gx, baseline + 1, gx + static_cast<int>(width), baseline + 1);
                if (style.strike) fl_line(gx, baseline - static_cast<int>(style.size / 3),
                    gx + static_cast<int>(width), baseline - static_cast<int>(style.size / 3));
            }
            for (size_t link = 0; link < owner_->editor().links().size(); ++link) {
                const MailLinkRange &range = owner_->editor().links()[link];
                if (i >= range.start && i < range.end) {
                    fl_color(foreground);
                    fl_line(gx, baseline + 2, gx + static_cast<int>(width), baseline + 2);
                    break;
                }
            }
        }
    }

    static void finish_typing_cb(void *context) {
        static_cast<ComposeCanvas *>(context)->owner_->editor().end_typing_group();
    }

public:
    ComposeCanvas(int x, int y, int width, int height, ComposeWindow *owner)
        : Fl_Widget(x, y, width, height), owner_(owner) {
        box(FL_DOWN_BOX);
        color(FL_BACKGROUND2_COLOR);
    }

    ~ComposeCanvas() override { Fl::remove_timeout(finish_typing_cb, this); }

    void resize(int x, int y, int width, int height) override {
        Fl_Widget::resize(x, y, width, height);
        layout_revision_ = 0;
        redraw();
    }

    void refresh_layout() {
        rw::Document &document = owner_->editor().word_editor().doc;
        const int page_width = w() > 2000 ? 2000 : w();
        const int page_height = viewport_height_ > 2000 ? 2000 : viewport_height_;
        if (page_width < 144 || page_height < 144) return;
        rw::PageSettings page = document.page;
        page.width = static_cast<uint16_t>(page_width);
        page.height = static_cast<uint16_t>(page_height);
        page.left = page.right = 18;
        page.top = page.bottom = 12;
        page.header.clear(); page.footer.clear(); page.page_numbers = false;
        document.page = page;
        if (!layout_.build(document, measure_mail_glyph, nullptr)) {
            owner_->canvas_status(layout_.error.c_str());
            return;
        }
        layout_revision_ = document.revision;
        layout_width_ = page_width;
        page_height_ = page_height;
        const uint64_t desired = static_cast<uint64_t>(page_height_) * layout_.pages;
        const int content_height = desired > 1500000u ? 1500000 :
            (desired < static_cast<uint64_t>(page_height_) ? page_height_ : static_cast<int>(desired));
        if (h() != content_height) Fl_Widget::resize(x(), y(), w(), content_height);
        redraw();
    }

    void refresh_layout(int viewport_height) {
        viewport_height_ = viewport_height;
        layout_revision_ = 0;
        refresh_layout();
    }

    void draw() override {
        fl_push_clip(x(), y(), w(), h());
        draw_box();
        if (!rebuild()) { fl_pop_clip(); return; }
        rw::Document &document = owner_->editor().word_editor().doc;
        for (size_t line_index = 0; line_index < layout_.lines.size(); ++line_index) {
            const rw::Line &line = layout_.lines[line_index];
            const int page_y = y() + static_cast<int>(line.page) * page_height_;
            const int baseline = page_y + static_cast<int>(line.baseline + 0.5f);
            if (baseline < y() - 8 || baseline > y() + h() + 8) continue;
            const rw::ParagraphStyle &paragraph = document.paragraphs[line.paragraph].style;
            if (line.paragraph_start && paragraph.list != rw::ListKind::Plain) {
                const rw::TextStyle &style = document.style_at(line.start < document.chars.size() ? line.start : 0);
                fl_font(font_for(style), style.size);
                fl_color(fl_rgb_color(static_cast<uchar>((style.foreground >> 16) & 255),
                    static_cast<uchar>((style.foreground >> 8) & 255),
                    static_cast<uchar>(style.foreground & 255)));
                char marker[24];
                if (paragraph.list == rw::ListKind::Bullet) snprintf(marker, sizeof(marker), "•");
                else snprintf(marker, sizeof(marker), "%u.", line.list_number);
                fl_draw(marker, x() + static_cast<int>(line.x) - 18, baseline);
            }
            draw_run(line.start, line.end, line, line.page);
        }
        rw::Editor &editor = owner_->editor().word_editor();
        if (Fl::focus() == this && editor.anchor == editor.caret && layout_.boxes.size()) {
            const size_t caret = editor.caret < layout_.boxes.size() ? editor.caret : layout_.boxes.size() - 1;
            const rw::GlyphBox &box = layout_.boxes[caret];
            const unsigned page = layout_.lines.empty() ? 0 : layout_.lines[box.line].page;
            const int left = x() + static_cast<int>(box.x + 0.5f);
            const int top = y() + static_cast<int>(page) * page_height_ + static_cast<int>(box.y + 0.5f);
            fl_color(FL_FOREGROUND_COLOR);
            fl_line(left, top, left, top + static_cast<int>(layout_.lines[box.line].height + 0.5f));
        }
        fl_pop_clip();
    }

    int handle(int event) override {
        if (event == FL_FOCUS || event == FL_UNFOCUS) { redraw(); return 1; }
        if (event == FL_ENTER) { fl_cursor(FL_CURSOR_INSERT); return 1; }
        if (event == FL_LEAVE) { fl_cursor(FL_CURSOR_DEFAULT); return 1; }
        if (event == FL_PASTE) {
            owner_->paste_text(Fl::event_text(), static_cast<size_t>(Fl::event_length()));
            return 1;
        }
        if (event == FL_PUSH) {
            take_focus(); Fl::compose_reset(); desired_x_ = -1;
            size_t line = 0;
            const size_t offset = hit_test(Fl::event_x() - x(), Fl::event_y() - y(), &line);
            move_caret(offset, Fl::event_state(FL_SHIFT),
                line < layout_.lines.size() && layout_.lines[line].end == offset);
            owner_->update_format_controls();
            if (Fl::event_button() == FL_MIDDLE_MOUSE) {
                (void)compose_request_paste(this, ComposeClipboardOperation::MiddleMousePaste,
                    request_fltk_paste, nullptr);
                return 1;
            }
            dragging_ = true; drag_x_ = Fl::event_x(); drag_y_ = Fl::event_y();
            return 1;
        }
        if (event == FL_DRAG && dragging_) {
            drag_x_ = Fl::event_x(); drag_y_ = Fl::event_y();
            size_t line = 0; const size_t offset = hit_test(drag_x_ - x(), drag_y_ - y(), &line);
            move_caret(offset, true, line < layout_.lines.size() && layout_.lines[line].end == offset);
            owner_->update_format_controls();
            return 1;
        }
        if (event == FL_RELEASE && dragging_) {
            dragging_ = false;
            (void)owner_->copy_selection(ComposeClipboardOperation::MouseSelectionCopy);
            return 1;
        }
        if (event != FL_KEYDOWN) return Fl_Widget::handle(event);

        rw::Editor &editor = owner_->editor().word_editor();
        const int key = Fl::event_key();
        const bool ctrl = Fl::event_state(FL_CTRL) != 0;
        const bool shift = Fl::event_state(FL_SHIFT) != 0;
        if (ctrl && (key == 'a' || key == 'A')) { (void)owner_->editor().select_all(); redraw(); return 1; }
        if (ctrl && (key == 'c' || key == 'C')) {
            (void)owner_->copy_selection(ComposeClipboardOperation::KeyboardCopy); return 1;
        }
        if (ctrl && (key == 'x' || key == 'X')) {
            if (owner_->copy_selection(ComposeClipboardOperation::KeyboardCut) && owner_->editor().erase(false)) owner_->canvas_changed();
            else owner_->canvas_status(owner_->editor().error().c_str());
            return 1;
        }
        if (ctrl && (key == 'v' || key == 'V')) {
            take_focus();
            (void)compose_request_paste(this, ComposeClipboardOperation::KeyboardPaste,
                request_fltk_paste, nullptr);
            return 1;
        }
        if (ctrl && (key == 'z' || key == 'Z')) {
            if (owner_->editor().undo()) owner_->canvas_changed();
            else owner_->canvas_status(owner_->editor().error().c_str());
            return 1;
        }
        if (ctrl && (key == 'y' || key == 'Y')) {
            if (owner_->editor().redo()) owner_->canvas_changed();
            else owner_->canvas_status(owner_->editor().error().c_str());
            return 1;
        }
        if (ctrl && (key == 'b' || key == 'B')) {
            rw::TextStyle style = editor.typing_style; style.bold = !style.bold;
            owner_->apply_style(rw::Bold, style); redraw(); return 1;
        }
        if (ctrl && (key == 'i' || key == 'I')) {
            rw::TextStyle style = editor.typing_style; style.italic = !style.italic;
            owner_->apply_style(rw::Italic, style); redraw(); return 1;
        }
        if (ctrl && (key == 'u' || key == 'U')) {
            rw::TextStyle style = editor.typing_style; style.underline = !style.underline;
            owner_->apply_style(rw::Underline, style); redraw(); return 1;
        }
        if (ctrl && (key == 'f' || key == 'F')) {
            ComposeWindow::action_cb(this, reinterpret_cast<void *>(static_cast<intptr_t>(ComposeWindow::Action::Find) + 1));
            return 1;
        }
        if (Fl::compose_state == 0 && !Fl::event_state(FL_ALT)) {
            size_t next = editor.caret;
            if (key == FL_Left || key == FL_Right) {
                const size_t low = editor.selection_start(), high = editor.selection_end();
                if (!shift && low != high) next = key == FL_Left ? low : high;
                else if (key == FL_Left) next = rw::previous_cluster(editor.doc, next);
                else next = rw::next_cluster(editor.doc, next);
                move_caret(next, shift); return 1;
            }
            if (key == FL_Home || key == FL_End) {
                if (ctrl) next = key == FL_Home ? 0 : editor.doc.chars.size();
                else if (rebuild() && !layout_.lines.empty()) {
                    const size_t line = layout_.line_for(editor.caret, previous_affinity_);
                    if (line < layout_.lines.size()) next = key == FL_Home ? layout_.lines[line].start : layout_.lines[line].end;
                }
                move_caret(next, shift, key == FL_End); return 1;
            }
            if (key == FL_BackSpace || key == FL_Delete) {
                if (owner_->editor().erase(key == FL_BackSpace)) owner_->canvas_changed();
                else owner_->canvas_status(owner_->editor().error().c_str());
                return 1;
            }
            if (key == FL_Enter || key == FL_KP_Enter || key == FL_Tab) {
                if (key == FL_Tab && ctrl) return 0;
                const char *insert = key == FL_Tab ? "\t" : "\n";
                if (owner_->editor().insert_utf8(insert, 1, false)) owner_->canvas_changed();
                else owner_->canvas_status(owner_->editor().error().c_str());
                return 1;
            }
        }
        int remove_bytes = 0;
        if (Fl::compose(remove_bytes)) {
            const size_t anchor = editor.anchor, caret = editor.caret;
            size_t start = caret;
            while (remove_bytes > 0 && start) {
                const uint32_t cp = editor.doc.chars[--start].codepoint;
                remove_bytes -= cp <= 0x7f ? 1 : cp <= 0x7ff ? 2 : cp <= 0xffff ? 3 : 4;
            }
            if (start != caret) editor.select(start, caret);
            if (Fl::event_length() || start != caret) {
                if (owner_->editor().insert_utf8(Fl::event_text(), static_cast<size_t>(Fl::event_length()), true)) {
                    Fl::remove_timeout(finish_typing_cb, this);
                    Fl::add_timeout(.8, finish_typing_cb, this);
                    owner_->canvas_changed();
                } else {
                    editor.select(anchor, caret);
                    owner_->canvas_status(owner_->editor().error().c_str());
                }
            }
            return 1;
        }
        return Fl_Widget::handle(event);
    }
};

class AttachmentListView : public Fl_Widget {
    ComposeWindow *owner_;
    size_t first_visible_ = 0;
    static constexpr int kRowHeight = 18;

    size_t visible_rows() const {
        const int available = h() - 4;
        return available > 0 ? static_cast<size_t>(available / kRowHeight) : 1;
    }

public:
    AttachmentListView(int x, int y, int width, int height, ComposeWindow *owner)
        : Fl_Widget(x, y, width, height), owner_(owner) {
        box(FL_DOWN_BOX);
        color(FL_BACKGROUND2_COLOR);
        align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    }

    void draw() override {
        draw_box();
        fl_push_clip(x() + 2, y() + 2, w() - 4, h() - 4);
        const WorkerDraftAttachmentRefs &attachments = owner_->attachment_refs_;
        fl_font(labelfont(), labelsize());
        if (attachments.empty()) {
            fl_color(FL_FOREGROUND_COLOR);
            fl_draw(T("Attachments: none"), x() + 7, y() + 17);
            fl_pop_clip();
            return;
        }
        const size_t rows = visible_rows();
        if (owner_->selected_attachment_ < first_visible_)
            first_visible_ = owner_->selected_attachment_;
        else if (owner_->selected_attachment_ >= first_visible_ + rows)
            first_visible_ = owner_->selected_attachment_ - rows + 1;
        if (first_visible_ + rows > attachments.size())
            first_visible_ = attachments.size() > rows ? attachments.size() - rows : 0;
        for (size_t row = 0; row < rows && first_visible_ + row < attachments.size(); ++row) {
            const size_t index = first_visible_ + row;
            const int row_y = y() + 2 + static_cast<int>(row) * kRowHeight;
            if (index == owner_->selected_attachment_) {
                fl_color(FL_SELECTION_COLOR);
                fl_rectf(x() + 2, row_y, w() - 4, kRowHeight);
            }
            const WorkerDraftAttachmentRef &ref = attachments[index];
            const char *name = ref.filename.c_str();
            if (!*name && ref.kind == WorkerAttachmentRefKind::SelectedFile) {
                const char *base = strrchr(ref.path.c_str(), '/');
                name = base ? base + 1 : ref.path.c_str();
            }
            char text[2048];
            size_t used = 0;
            for (; name[used] && used + 1 < sizeof(text); ++used) {
                const unsigned char c = static_cast<unsigned char>(name[used]);
                text[used] = c < 0x20 || c == 0x7f ? ' ' : static_cast<char>(c);
            }
            text[used] = 0;
            char suffix[64];
            if (index < owner_->attachment_metadata_.size()) {
                char size_text[32];
                format_size(owner_->attachment_metadata_[index].size, size_text, sizeof(size_text));
                snprintf(suffix, sizeof(suffix), " (%s)", size_text);
            } else {
                snprintf(suffix, sizeof(suffix), " (%s)", T("size checked on save"));
            }
            const size_t available = sizeof(text) - used - 1;
            size_t suffix_len = strlen(suffix);
            if (suffix_len > available) suffix_len = available;
            memcpy(text + used, suffix, suffix_len);
            text[used + suffix_len] = 0;
            fl_color(index == owner_->selected_attachment_ ?
                fl_contrast(FL_FOREGROUND_COLOR, FL_SELECTION_COLOR) : FL_FOREGROUND_COLOR);
            fl_draw(text, x() + 7, row_y + 13);
        }
        fl_pop_clip();
    }

    int handle(int event) override {
        const size_t rows = visible_rows();
        if (event == FL_PUSH && Fl::event_inside(this)) {
            const int row = (Fl::event_y() - y() - 2) / kRowHeight;
            const size_t index = first_visible_ + static_cast<size_t>(row < 0 ? 0 : row);
            if (index < owner_->attachment_refs_.size()) {
                owner_->selected_attachment_ = index;
                redraw();
            }
            return 1;
        }
        if (event == FL_MOUSEWHEEL && !owner_->attachment_refs_.empty()) {
            const int delta = Fl::event_dy();
            const size_t max_first = owner_->attachment_refs_.size() > rows ?
                owner_->attachment_refs_.size() - rows : 0;
            if (delta > 0 && first_visible_ < max_first) ++first_visible_;
            else if (delta < 0 && first_visible_ > 0) --first_visible_;
            redraw();
            return 1;
        }
        return Fl_Widget::handle(event);
    }
};

class ComposeRecipientInput : public Fl_Input {
    ComposeWindow *owner_;
public:
    ComposeRecipientInput(int x, int y, int width, int height, ComposeWindow *owner)
        : Fl_Input(x, y, width, height), owner_(owner) {}

    int handle(int event) override {
        if (event == FL_FOCUS || event == FL_PUSH) owner_->recipient_focus(this);
        return Fl_Input::handle(event);
    }
};

namespace {
class DraftPicker : public Fl_Double_Window {
    int selected_ = -1;
    static void row_cb(Fl_Widget *widget, void *data) {
        DraftPicker *picker = static_cast<DraftPicker *>(widget->window());
        picker->selected_ = static_cast<int>(reinterpret_cast<intptr_t>(data) - 1);
        picker->hide();
    }
    static void close_cb(Fl_Widget *widget, void *) { widget->window()->hide(); }

public:
    explicit DraftPicker(const WorkerDraftList &drafts)
        : Fl_Double_Window(600, 500, T("Open draft")) {
        size_range(480, 320);
        begin();
        Fl_Scroll *scroll = new Fl_Scroll(8, 8, 584, 438);
        scroll->type(Fl_Scroll::VERTICAL);
        scroll->box(FL_BORDER_BOX);
        scroll->begin();
        Fl_Group *rows = new Fl_Group(8, 8, 560,
            static_cast<int>(drafts.drafts.size() ? drafts.drafts.size() * 56 : 56));
        for (size_t i = 0; i < drafts.drafts.size(); ++i) {
            Text subject, recipient;
            const WorkerDraftSummary &draft = drafts.drafts[i];
            if (!task6_sanitize_display_text(draft.subject.c_str(), draft.subject.size(), subject) ||
                !task6_sanitize_display_text(draft.to.c_str(), draft.to.size(), recipient)) continue;
            char label[8192];
            snprintf(label, sizeof(label), "#%llu  %s\n%s",
                static_cast<unsigned long long>(draft.id),
                subject.empty() ? T("(no subject)") : subject.c_str(), recipient.c_str());
            char literal_label[sizeof(label) * 2];
            size_t literal_size = 0;
            for (size_t j = 0; label[j] && literal_size + 2 < sizeof(literal_label); ++j) {
                if (label[j] == '@') literal_label[literal_size++] = '@';
                literal_label[literal_size++] = label[j];
            }
            literal_label[literal_size] = 0;
            Fl_Button *row = new Fl_Button(10, 8 + static_cast<int>(i) * 56, 556, 50);
            row->copy_label(literal_label);
            row->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
            row->callback(row_cb, reinterpret_cast<void *>(static_cast<intptr_t>(i) + 1));
        }
        rows->end();
        scroll->end();
        Fl_Button *cancel = new Fl_Button(490, 456, 100, 30, T("Cancel"));
        cancel->callback(close_cb, nullptr);
        end();
        reliefos_fltk::style(this);
    }

    int run() {
        set_modal(); show();
        while (shown()) Fl::wait();
        set_non_modal();
        return selected_;
    }
};
}

bool choose_compose_draft(const WorkerDraftList &drafts, WorkerDraftSummary &selected) {
    if (drafts.drafts.empty()) return false;
    DraftPicker picker(drafts);
    const int index = picker.run();
    if (index < 0 || static_cast<size_t>(index) >= drafts.drafts.size()) return false;
    selected = drafts.drafts[static_cast<size_t>(index)];
    return true;
}

ComposeWindow::ComposeWindow(MailWindow *owner)
    : Fl_Double_Window(1160, 740, T("New message")), owner_(owner) {
    size_range(860, 540);
    callback(close_cb, this);
    build_widgets();
    layout_widgets();
    reliefos_fltk::style(this);
    int screen_x = 0, screen_y = 0, screen_w = 0, screen_h = 0;
    Fl::screen_work_area(screen_x, screen_y, screen_w, screen_h);
    const int width = w() < screen_w - 24 ? w() : screen_w - 24;
    const int height = h() < screen_h - 48 ? h() : screen_h - 48;
    if (width >= 860 && height >= 540)
        resize(screen_x + (screen_w - width) / 2, screen_y + (screen_h - height) / 2,
            width, height);
}

ComposeWindow::~ComposeWindow() {
    Fl::remove_timeout(autosave_cb, this);
}

void ComposeWindow::build_widgets() {
    const int ribbon_content_height = kComposeRibbon - Fl::scrollbar_size();
    begin();
    top_ = new Fl_Group(0, 0, w(), kComposeTop);
    Fl_Menu_Button *file = new Fl_Menu_Button(7, 3, 64, 28, T("&File"));
    file->add(T("Save draft"), FL_CTRL + 's', action_cb,
        reinterpret_cast<void *>(static_cast<intptr_t>(Action::Save) + 1));
    file->add(T("Close"), 0, close_cb, this);
    const char *tab_names[] = {T("Message"), T("Edit"), T("Insert")};
    const int tab_x[] = {80, 164, 248};
    for (int i = 0; i < 3; ++i) {
        ribbon_tabs_[i] = new Fl_Button(tab_x[i], 3, 80, 28, tab_names[i]);
        ribbon_tabs_[i]->type(FL_TOGGLE_BUTTON);
        ribbon_tabs_[i]->callback(ribbon_tab_cb, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
    }
    Fl_Box *stretch = new Fl_Box(356, 0, w() - 560, kComposeTop);
    save_button_ = new Fl_Button(w() - 204, 4, 59, 26, T("Save"));
    save_button_->callback(action_cb, reinterpret_cast<void *>(static_cast<intptr_t>(Action::Save) + 1));
    save_button_->tooltip(T("Save draft (Ctrl+S)"));
    quick_undo_ = new Fl_Button(w() - 140, 4, 63, 26, T("Undo"));
    quick_undo_->callback(action_cb, reinterpret_cast<void *>(static_cast<intptr_t>(Action::Undo) + 1));
    quick_redo_ = new Fl_Button(w() - 72, 4, 63, 26, T("Redo"));
    quick_redo_->callback(action_cb, reinterpret_cast<void *>(static_cast<intptr_t>(Action::Redo) + 1));
    top_->resizable(stretch);
    top_->end();

    ribbon_scroll_ = new Fl_Scroll(0, kComposeTop, w(), kComposeRibbon);
    ribbon_scroll_->type(Fl_Scroll::HORIZONTAL);
    ribbon_scroll_->begin();
    ribbon_group_ = new Fl_Group(0, kComposeTop, kComposeRibbonContentWidth, ribbon_content_height);
    auto add_group = [&](int x, int width, const char *caption) {
        Fl_Group *group = new Fl_Group(x, kComposeTop, width, ribbon_content_height);
        Fl_Box *label = new Fl_Box(x + 2, kComposeTop + ribbon_content_height - 20,
            width - 8, 16, caption);
        label->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
        Fl_Box *divider = new Fl_Box(x + width - 2, kComposeTop + 9, 2, 58);
        divider->box(FL_ENGRAVED_FRAME);
        group->end();
        return group;
    };
    auto action_button = [&](int x, int y, int width, int height, const char *label, Action action) {
        Fl_Button *button = new Fl_Button(x, y, width, height, label);
        button->callback(action_cb, reinterpret_cast<void *>(static_cast<intptr_t>(action) + 1));
        return button;
    };
    auto add_choice_caption = [&](int x, int width, const char *caption) {
        Fl_Box *label = new Fl_Box(x, kComposeTop + 8, width, 14, caption);
        label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
        return new Fl_Choice(x, kComposeTop + 28, width, 26);
    };

    ribbon_pages_[0] = new Fl_Group(0, kComposeTop, kComposeRibbonContentWidth, ribbon_content_height);
    Fl_Group *send_group = add_group(6, 174, T("Send"));
    auto command_button = [&](int x, int y, int width, int height, const char *label, Action action) {
        Fl_Button *button = new Fl_Button(x, y, width, height, label);
        button->callback(action_cb, reinterpret_cast<void *>(static_cast<intptr_t>(action) + 1));
        return button;
    };
    send_button_ = command_button(14, kComposeTop + 8, 72, 54,
        T("Send now"), Action::Send);
    queue_button_ = action_button(92, kComposeTop + 8, 76, 54, T("Queue"), Action::Queue);
    send_group->end();
    Fl_Group *clipboard_group = add_group(188, 154, T("Clipboard"));
    paste_button_ = action_button(196, kComposeTop + 8, 52, 54, T("Paste"), Action::Paste);
    copy_button_ = action_button(256, kComposeTop + 8, 78, 25, T("Copy"), Action::Copy);
    cut_button_ = action_button(256, kComposeTop + 37, 78, 25, T("Cut"), Action::Cut);
    clipboard_group->end();
    Fl_Group *font_group = add_group(354, 252, T("Font"));
    font_family_ = new Fl_Choice(362, kComposeTop + 8, 150, 26);
    font_family_->add(T("Sans")); font_family_->add(T("SimSun")); font_family_->add(T("Mono"));
    font_family_->value(1); font_family_->callback(format_cb, this);
    font_size_ = new Fl_Choice(518, kComposeTop + 8, 72, 26);
    font_size_->add("9"); font_size_->add("10"); font_size_->add("11"); font_size_->add("12");
    font_size_->add("14"); font_size_->add("16"); font_size_->add("18"); font_size_->add("24"); font_size_->add("36");
    font_size_->value(3); font_size_->callback(format_cb, this);
    bold_ = action_button(362, kComposeTop + 38, 28, 26, T("B"), Action::Bold);
    bold_->type(FL_TOGGLE_BUTTON); bold_->labelfont(FL_HELVETICA_BOLD);
    italic_ = action_button(394, kComposeTop + 38, 28, 26, T("I"), Action::Italic);
    italic_->type(FL_TOGGLE_BUTTON); italic_->labelfont(FL_HELVETICA_ITALIC);
    underline_ = action_button(426, kComposeTop + 38, 28, 26, T("U"), Action::Underline);
    underline_->type(FL_TOGGLE_BUTTON);
    action_button(458, kComposeTop + 38, 52, 26, T("Color"), Action::Color);
    clear_format_ = action_button(516, kComposeTop + 38, 82, 26, T("Clear format"), Action::ClearFormat);
    font_group->end();
    Fl_Group *paragraph_group = add_group(618, 312, T("Paragraph"));
    align_left_ = action_button(626, kComposeTop + 8, 63, 26, T("Left"), Action::AlignLeft);
    align_center_ = action_button(693, kComposeTop + 8, 69, 26, T("Center"), Action::AlignCenter);
    align_right_ = action_button(766, kComposeTop + 8, 67, 26, T("Right"), Action::AlignRight);
    bullet_list_ = action_button(626, kComposeTop + 38, 66, 26, T("Bullets"), Action::BulletList);
    number_list_ = action_button(696, kComposeTop + 38, 67, 26, T("Number"), Action::NumberList);
    outdent_ = action_button(767, kComposeTop + 38, 66, 26, T("Outdent"), Action::Outdent);
    indent_ = action_button(837, kComposeTop + 38, 70, 26, T("Indent"), Action::Indent);
    Fl_Button *paragraph_buttons[] = {align_left_, align_center_, align_right_, bullet_list_, number_list_};
    for (Fl_Button *button : paragraph_buttons)
        button->type(FL_TOGGLE_BUTTON);
    paragraph_group->end();
    ribbon_pages_[0]->end();

    ribbon_pages_[1] = new Fl_Group(0, kComposeTop, kComposeRibbonContentWidth, ribbon_content_height);
    Fl_Group *format_mode_group = add_group(6, 172, T("Message format"));
    rich_mode_ = action_button(14, kComposeTop + 8, 156, 54,
        T("Rich text"), Action::PlainMode);
    rich_mode_->type(FL_TOGGLE_BUTTON);
    format_mode_group->end();
    Fl_Group *editing_group = add_group(186, 150, T("Editing"));
    action_button(194, kComposeTop + 8, 134, 25, T("Find"), Action::Find);
    action_button(194, kComposeTop + 37, 134, 25, T("Select all"), Action::SelectAll);
    editing_group->end();
    Fl_Group *delivery_group = add_group(344, 152, T("Delivery"));
    retry_archive_button_ = action_button(352, kComposeTop + 8, 136, 54,
        T("Retry archive"), Action::RetryArchive);
    retry_archive_button_->deactivate();
    delivery_group->end();
    ribbon_pages_[1]->end();

    ribbon_pages_[2] = new Fl_Group(0, kComposeTop, kComposeRibbonContentWidth, ribbon_content_height);
    Fl_Group *attachment_group = add_group(6, 154, T("Attachments"));
    attach_button_ = command_button(14, kComposeTop + 8, 64, 54,
        T("Attach"), Action::Attach);
    remove_button_ = command_button(84, kComposeTop + 8, 68, 54,
        T("Remove"), Action::RemoveAttachment);
    attachment_group->end();
    Fl_Group *link_group = add_group(166, 148, T("Links"));
    link_button_ = command_button(174, kComposeTop + 8, 126, 54,
        T("Link"), Action::Link);
    link_group->end();
    Fl_Group *signature_group = add_group(324, 218, T("Saved content"));
    signature_button_ = command_button(332, kComposeTop + 8, 92, 54,
        T("Signature"), Action::InsertSignature);
    template_choice_ = add_choice_caption(431, 103, T("Template"));
    template_choice_->callback(template_cb, this);
    template_choice_->deactivate();
    signature_group->end();
    Fl_Group *contacts_group = add_group(552, 270, T("Contacts"));
    contacts_button_ = action_button(560, kComposeTop + 8, 118, 54,
        T("Find contact"), Action::SearchContacts);
    contact_choice_ = add_choice_caption(684, 130, T("Matches"));
    contact_choice_->callback(contact_cb, this);
    contact_choice_->deactivate();
    contacts_group->end();
    ribbon_pages_[2]->end();
    ribbon_group_->end();
    ribbon_scroll_->end();
    show_ribbon_tab(0);

    const int field_x = 91;
    const int field_w = w() - field_x - 14;
    from_caption_ = new Fl_Box(8, kComposeFieldsTop, field_x - 16, kFieldHeight, T("From:"));
    from_caption_->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
    from_choice_ = new Fl_Choice(field_x, kComposeFieldsTop, field_w, kFieldHeight);
    from_choice_->callback(account_cb, this);
    const int row_top = kComposeFieldsTop + kFieldHeight + kFieldGap;
    auto add_field = [&](int slot, int y, const char *label, Fl_Input *&input, bool recipient = false) {
        recipient_captions_[slot] = new Fl_Box(8, y, field_x - 16, kFieldHeight, label);
        recipient_captions_[slot]->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        input = recipient ? static_cast<Fl_Input *>(new ComposeRecipientInput(field_x, y, field_w, kFieldHeight, this)) :
            new Fl_Input(field_x, y, field_w, kFieldHeight);
        input->when(FL_WHEN_CHANGED);
        input->callback(field_cb, this);
    };
    add_field(0, row_top, T("To:"), to_, true);
    add_field(1, row_top + kFieldHeight + kFieldGap, T("Cc:"), cc_, true);
    add_field(2, row_top + (kFieldHeight + kFieldGap) * 2, T("Bcc:"), bcc_, true);
    add_field(3, row_top + (kFieldHeight + kFieldGap) * 3, T("Subject:"), subject_);
    cc_toggle_ = new Fl_Button(0, row_top, 72, kFieldHeight, T("Cc +"));
    bcc_toggle_ = new Fl_Button(0, row_top, 72, kFieldHeight, T("Bcc +"));
    cc_toggle_->callback(optional_recipient_cb, this);
    bcc_toggle_->callback(optional_recipient_cb, this);
    to_->tooltip(T("Raw To field is preserved exactly and validated at send time"));
    cc_->tooltip(T("Raw Cc field is preserved exactly and validated at send time"));
    bcc_->tooltip(T("Raw Bcc field is preserved exactly and validated at send time"));
    active_recipient_field_ = to_;
    template_choice_->callback(template_cb, this);
    template_choice_->deactivate();

    const int attachments_y = row_top + (kFieldHeight + kFieldGap) * 4 + 4;
    attachment_list_ = new AttachmentListView(8, attachments_y, w() - 16, 42, this);
    attachment_list_->hide();
    const int canvas_y = attachments_y;
    const int status_y = h() - kComposeStatus;
    body_scroll_ = new Fl_Scroll(8, canvas_y, w() - 16, status_y - canvas_y - 4);
    body_scroll_->type(Fl_Scroll::VERTICAL);
    body_scroll_->begin();
    canvas_ = new ComposeCanvas(body_scroll_->x(), body_scroll_->y(),
        body_scroll_->w() - Fl::scrollbar_size() - 2,
        body_scroll_->h(), this);
    body_scroll_->end();
    status_ = new PlainTextBox(8, h() - kComposeStatus + 1, w() - 16, kComposeStatus - 2,
        T("Ready to compose"));
    status_->labelsize(13);
    status_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    end();
    // Keep screen-work-area adaptation from turning the compose into a fixed-size window.
    resizable(body_scroll_);
}

void ComposeWindow::layout_widgets() {
    top_->resize(0, 0, w(), kComposeTop);
    save_button_->resize(w() - 204, 4, 59, 26);
    quick_undo_->resize(w() - 140, 4, 63, 26);
    quick_redo_->resize(w() - 72, 4, 63, 26);
    ribbon_scroll_->resize(0, kComposeTop, w(), kComposeRibbon);
    const int field_x = 91;
    const int field_w = w() - field_x - 14;
    const int to_w = field_w - 2 * 72 - 12;
    const ComposeHeaderLayout header = compose_header_layout(kComposeFieldsTop, kFieldHeight,
        kFieldGap, optional_recipient_visibility_, !attachment_refs_.empty(), h(), kComposeStatus);
    from_caption_->resize(8, header.from_y, field_x - 16, kFieldHeight);
    from_choice_->resize(field_x, header.from_y, field_w, kFieldHeight);
    recipient_captions_[0]->resize(8, header.to_y, field_x - 16, kFieldHeight);
    to_->resize(field_x, header.to_y, to_w, kFieldHeight);
    cc_toggle_->resize(field_x + to_w + 6, header.to_y, 72, kFieldHeight);
    bcc_toggle_->resize(field_x + to_w + 84, header.to_y, 72, kFieldHeight);
    if (header.cc_y >= 0) {
        recipient_captions_[1]->resize(8, header.cc_y, field_x - 16, kFieldHeight);
        cc_->resize(field_x, header.cc_y, field_w, kFieldHeight);
        recipient_captions_[1]->show();
        cc_->show();
    } else {
        recipient_captions_[1]->hide();
        cc_->hide();
    }
    if (header.bcc_y >= 0) {
        recipient_captions_[2]->resize(8, header.bcc_y, field_x - 16, kFieldHeight);
        bcc_->resize(field_x, header.bcc_y, field_w, kFieldHeight);
        recipient_captions_[2]->show();
        bcc_->show();
    } else {
        recipient_captions_[2]->hide();
        bcc_->hide();
    }
    recipient_captions_[3]->resize(8, header.subject_y, field_x - 16, kFieldHeight);
    subject_->resize(field_x, header.subject_y, field_w, kFieldHeight);
    attachment_list_->resize(8, header.attachment_y, w() - 16, 42);
    const int canvas_y = header.body_y;
    const int status_y = h() - kComposeStatus;
    body_scroll_->resize(8, canvas_y, w() - 16, status_y - canvas_y - 4);
    canvas_->resize(body_scroll_->x(), body_scroll_->y(),
        body_scroll_->w() - Fl::scrollbar_size() - 2,
        body_scroll_->h());
    status_->resize(8, status_y + 1, w() - 16, kComposeStatus - 2);
    if (canvas_) canvas_->refresh_layout(body_scroll_->h());
}

void ComposeWindow::show_ribbon_tab(int index) {
    if (index < 0 || index >= 3) return;
    active_ribbon_tab_ = index;
    for (int i = 0; i < 3; ++i) {
        if (ribbon_pages_[i]) {
            if (i == index) ribbon_pages_[i]->show();
            else ribbon_pages_[i]->hide();
        }
        if (ribbon_tabs_[i]) ribbon_tabs_[i]->value(i == index);
    }
    if (ribbon_scroll_) ribbon_scroll_->scroll_to(0, 0);
    if (ribbon_group_) ribbon_group_->redraw();
}

void ComposeWindow::resize(int x, int y, int width, int height) {
    Fl_Double_Window::resize(x, y, width, height);
    if (top_) layout_widgets();
}

bool ComposeWindow::open_new(const AccountList &accounts, uint64_t account_id,
    uint64_t profile_epoch) {
    if (save_gate_.pending() || send_in_progress_ || queue_pending_ || copy_pending_ || archive_retry_pending_) return false;
    accounts_.clear();
    for (size_t i = 0; i < accounts.size(); ++i) (void)accounts_.push(accounts[i]);
    if (!accounts_.size()) {
        owner_->set_status(T("Add an account before composing a message."));
        return false;
    }
    size_t selected = 0;
    for (size_t i = 0; i < accounts_.size(); ++i)
        if (accounts_[i].id == account_id) { selected = i; break; }
    account_ = accounts_[selected];
    profile_epoch_ = profile_epoch;
    session_stale_ = false;
    draft_ = Draft();
    draft_.account_id = account_.id;
    draft_.rich_format = true;
    Text error;
    if (!set_raw_recipient_inputs(draft_, "", 0, "", 0, "", 0, error)) return false;
    attachment_refs_.clear(); attachment_metadata_.clear(); operation_id_ = 0;
    selected_attachment_ = 0;
    compose_catalog_ = WorkerComposeCatalog();
    contact_suggestions_.clear();
    template_choice_->clear(); template_choice_->value(-1);
    contact_choice_->clear(); contact_choice_->value(-1);
    forward_source_valid_ = false;
    EditorStateLoad state;
    if (!editor_.load_draft(draft_, state, error)) return set_error(error, error.c_str());
    save_gate_.reset_for_open(draft_);
    (void)save_gate_.note_edit(); // A new blank draft can be explicitly saved.
    suppress_callbacks_ = true;
    to_->value(""); cc_->value(""); bcc_->value(""); subject_->value("");
    suppress_callbacks_ = false;
    compose_header_visibility_new(optional_recipient_visibility_);
    active_recipient_field_ = to_;
    send_after_save_ = close_after_save_ = application_close_after_save_ = false;
    send_in_progress_ = false;
    queue_pending_ = false;
    archive_retry_pending_ = archive_retry_available_ = false;
    session_stale_ = false;
    update_attachment_summary();
    update_from_label();
    update_format_controls();
    size_range(860, 540);
    set_modal();
    show();
    canvas_->take_focus();
    status_->text(T("New draft — changes save automatically"));
    (void)request_compose_catalog(Text());
    Fl::remove_timeout(autosave_cb, this);
    return true;
}

bool ComposeWindow::open_existing(const AccountList &accounts, const Account &account,
    WorkerDraftSnapshot snapshot, uint64_t profile_epoch) {
    if (save_gate_.pending() || send_in_progress_ || queue_pending_ || copy_pending_ || archive_retry_pending_ ||
        snapshot.draft.account_id != account.id ||
        snapshot.account_generation != account.generation || !snapshot.draft.id || !snapshot.draft.revision)
        return false;
    accounts_.clear();
    for (size_t i = 0; i < accounts.size(); ++i) (void)accounts_.push(accounts[i]);
    account_ = account;
    profile_epoch_ = profile_epoch;
    session_stale_ = false;
    forward_source_valid_ = false;
    draft_ = static_cast<Draft &&>(snapshot.draft);
    attachment_refs_.clear(); attachment_metadata_ = static_cast<WorkerAttachmentMetadataList &&>(snapshot.attachments);
    selected_attachment_ = 0;
    compose_catalog_ = WorkerComposeCatalog();
    contact_suggestions_.clear();
    template_choice_->clear(); template_choice_->value(-1);
    contact_choice_->clear(); contact_choice_->value(-1);
    for (size_t i = 0; i < attachment_metadata_.size(); ++i) {
        WorkerDraftAttachmentRef ref;
        ref.kind = WorkerAttachmentRefKind::ExistingDraftPart;
        ref.source_draft_id = draft_.id;
        ref.source_revision = draft_.revision;
        ref.source_position = static_cast<uint32_t>(i);
        ref.filename = attachment_metadata_[i].filename;
        ref.content_type = attachment_metadata_[i].content_type;
        ref.content_id = attachment_metadata_[i].content_id;
        ref.inline_part = attachment_metadata_[i].inline_part;
        if (!attachment_refs_.push(static_cast<WorkerDraftAttachmentRef &&>(ref)))
            return false;
    }
    EditorStateLoad state;
    Text error;
    if (!editor_.load_draft(draft_, state, error)) return false;
    save_gate_.reset_for_open(draft_);
    bool converted_plain = false;
    if (state == EditorStateLoad::Unsupported || state == EditorStateLoad::Corrupt) {
        const char *detail = state == EditorStateLoad::Unsupported ?
            T("This draft uses a rich-editor format this version cannot read.") :
            T("This draft's rich-editor state is damaged.");
        const int convert = literal_confirmation(T("Open draft safely"),
            T("Convert a plain-text copy for editing? The saved draft remains unchanged until you save."),
            detail, T("Keep read-only"), T("Convert to plain text"));
        if (compose_confirmation_accepted(convert)) {
            if (!editor_.accept_plain_fallback(draft_, error)) return false;
            converted_plain = true;
        }
    }
    if (converted_plain && !save_gate_.note_edit()) return false;
    suppress_callbacks_ = true;
    to_->value(draft_.raw_recipient_input ? draft_.to_input.c_str() : "");
    cc_->value(draft_.raw_recipient_input ? draft_.cc_input.c_str() : "");
    bcc_->value(draft_.raw_recipient_input ? draft_.bcc_input.c_str() : "");
    if (!draft_.raw_recipient_input) {
        Text value;
        if (render_addresses(draft_.to, value)) to_->value(value.c_str());
        if (render_addresses(draft_.cc, value)) cc_->value(value.c_str());
        if (render_addresses(draft_.bcc, value)) bcc_->value(value.c_str());
    }
    subject_->value(draft_.subject.c_str());
    suppress_callbacks_ = false;
    compose_header_visibility_restore(optional_recipient_visibility_, cc_->value(), bcc_->value());
    active_recipient_field_ = to_;
    close_after_save_ = send_after_save_ = application_close_after_save_ = false;
    send_in_progress_ = false;
    queue_pending_ = false;
    archive_retry_pending_ = archive_retry_available_ = false;
    operation_id_ = 0;
    update_attachment_summary(); update_from_label(); update_format_controls();
    set_modal(); show(); canvas_->take_focus();
    status_->text(T("Draft reopened — saves use its exact current revision"));
    (void)request_compose_catalog(Text());
    Fl::remove_timeout(autosave_cb, this);
    if (save_gate_.dirty()) Fl::add_timeout(1.2, autosave_cb, this);
    return true;
}

bool ComposeWindow::open_prefilled(const AccountList &accounts, const Account &account,
    Draft draft, uint64_t profile_epoch, const MessageKey *forward_source) {
    if (!draft.account_id) draft.account_id = account.id;
    if (draft.account_id != account.id || draft.id || draft.revision ||
        (forward_source && (forward_source->folder.account_id != account.id ||
            forward_source->folder.server_name.empty() || !forward_source->uidvalidity ||
            !forward_source->uid))) return false;
    draft.rich_format = true;
    if (!open_new(accounts, account.id, profile_epoch)) return false;
    draft_ = static_cast<Draft &&>(draft);
    draft_.account_id = account.id;
    forward_source_valid_ = forward_source != nullptr;
    if (forward_source) forward_source_ = *forward_source;
    EditorStateLoad state;
    Text error;
    if (!editor_.load_draft(draft_, state, error) || state != EditorStateLoad::PlainDraft) {
        forward_source_valid_ = false;
        finish_close();
        status_->text(error.empty() ? T("The new message body could not be opened safely.") : error.c_str());
        return false;
    }
    save_gate_.reset_for_open(draft_);
    if (!save_gate_.note_edit()) {
        forward_source_valid_ = false;
        finish_close();
        status_->text(T("The draft edit counter reached its limit."));
        return false;
    }
    suppress_callbacks_ = true;
    to_->value(draft_.raw_recipient_input ? draft_.to_input.c_str() : "");
    cc_->value(draft_.raw_recipient_input ? draft_.cc_input.c_str() : "");
    bcc_->value(draft_.raw_recipient_input ? draft_.bcc_input.c_str() : "");
    subject_->value(draft_.subject.c_str());
    suppress_callbacks_ = false;
    compose_header_visibility_restore(optional_recipient_visibility_, cc_->value(), bcc_->value());
    update_attachment_summary();
    update_from_label();
    update_format_controls();
    canvas_->refresh_layout();
    canvas_->take_focus();
    status_->text(forward_source_valid_ ?
        T("Forward draft — the complete original will be attached on first save.") :
        T("Reply draft — the message is quoted and will save locally."));
    return true;
}

void ComposeWindow::update_from_label() {
    if (!from_choice_) return;
    from_choice_->clear();
    from_choice_accounts_.clear();
    int selected = -1;
    for (size_t i = 0; i < accounts_.size(); ++i) {
        if (!accounts_[i].id || !accounts_[i].generation) continue;
        Text safe_name, safe_email, escaped;
        const Text &name = accounts_[i].display_name.empty() ? accounts_[i].email_address : accounts_[i].display_name;
        if (!task6_sanitize_single_line_display_text(name.c_str(), name.size(), safe_name) ||
            !task6_sanitize_single_line_display_text(accounts_[i].email_address.c_str(),
                accounts_[i].email_address.size(), safe_email)) continue;
        char line[8193];
        const int count = snprintf(line, sizeof(line), "%s <%s>%s", safe_name.c_str(), safe_email.c_str(),
            accounts_[i].is_default ? T("  · Default") : "");
        if (count < 0 || static_cast<size_t>(count) >= sizeof(line) ||
            !compose_account_choice_menu_label(line, static_cast<size_t>(count), escaped)) continue;
        const int menu_index = from_choice_->add("", 0, nullptr, nullptr, 0);
        if (menu_index < 0) continue;
        if (!compose_account_choice_record(from_choice_accounts_, menu_index,
                accounts_[i].id, accounts_[i].generation)) {
            from_choice_->remove(menu_index);
            continue;
        }
        /* replace(index, label) stores one literal item; add(label) would
         * reinterpret '/' as a submenu path and the one-argument overload
         * also treats '|' as an item separator. */
        from_choice_->replace(menu_index, escaped.c_str());
        if (accounts_[i].id == account_.id && accounts_[i].generation == account_.generation)
            selected = menu_index;
    }
    from_choice_->value(selected);
}

void ComposeWindow::change_account(size_t index) {
    if (index >= accounts_.size()) { update_from_label(); return; }
    const Account &target = accounts_[index];
    if (target.id == account_.id && target.generation == account_.generation) {
        update_from_label();
        return;
    }
    if (session_stale_ || copy_pending_ || send_in_progress_ || queue_pending_ || archive_retry_pending_) {
        status_->text(T("This sender cannot change while the current operation is unresolved."));
        update_from_label();
        return;
    }
    if (save_gate_.pending()) {
        status_->text(T("Wait for the current save before changing the sender account."));
        update_from_label();
        return;
    }
    char detail[2048];
    snprintf(detail, sizeof(detail), "%s <%s>", target.display_name.c_str(), target.email_address.c_str());
    if (draft_.id) {
        if (!compose_confirmation_accepted(literal_confirmation(T("Copy draft to another account"),
                T("A new draft will be created under this sender. Current edits and attachments are copied; the original stays unchanged."),
                detail, T("Keep current"), T("Copy draft")))) {
            update_from_label();
            return;
        }
        (void)submit_copy(target);
        update_from_label();
        return;
    }
    if (!compose_confirmation_accepted(literal_confirmation(T("Change sender account"),
            T("This new draft will use the following identity."), detail,
            T("Keep current"), T("Change account")))) {
        update_from_label();
        return;
    }
    account_ = target;
    draft_.account_id = account_.id;
    compose_catalog_ = WorkerComposeCatalog();
    contact_suggestions_.clear();
    template_choice_->clear(); template_choice_->value(-1);
    contact_choice_->clear(); contact_choice_->value(-1);
    update_from_label();
    changed();
    update_format_controls();
    (void)request_compose_catalog(Text());
}

void ComposeWindow::change_account_identity(uint64_t account_id, uint64_t account_generation) {
    for (size_t i = 0; i < accounts_.size(); ++i) {
        if (accounts_[i].id == account_id && accounts_[i].generation == account_generation) {
            change_account(i);
            return;
        }
    }
    status_->text(T("The selected sender account is no longer available."));
    update_from_label();
}

void ComposeWindow::update_format_controls() {
    const rw::Editor &word = editor_.word_editor();
    const rw::TextStyle &style = word.caret && word.caret <= word.doc.chars.size() ?
        word.doc.style_at(word.caret - 1) : word.typing_style;
    const int family = style.family == rw::FontFamily::Sans ? 0 :
        style.family == rw::FontFamily::Serif ? 1 : 2;
    font_family_->value(family);
    const int sizes[] = {9, 10, 11, 12, 14, 16, 18, 24, 36};
    int selected = 3;
    for (int i = 0; i < 9; ++i) if (sizes[i] == style.size) selected = i;
    font_size_->value(selected);
    bold_->value(style.bold); italic_->value(style.italic); underline_->value(style.underline);
    const rw::ParagraphStyle &paragraph = word.doc.paragraphs[
        word.doc.paragraph_at(word.caret)].style;
    align_left_->value(paragraph.alignment == rw::Alignment::Left);
    align_center_->value(paragraph.alignment == rw::Alignment::Center);
    align_right_->value(paragraph.alignment == rw::Alignment::Right);
    bullet_list_->value(paragraph.list == rw::ListKind::Bullet);
    number_list_->value(paragraph.list == rw::ListKind::Number);
    const bool enabled = editor_.editing_allowed() && editor_.rich_format() && !session_stale_ &&
        !copy_pending_ && !send_in_progress_ && !queue_pending_ && !archive_retry_pending_;
    Fl_Button *format_buttons[] = {bold_, italic_, underline_, clear_format_, align_left_,
        align_center_, align_right_, bullet_list_, number_list_, outdent_, indent_, link_button_};
    if (enabled) {
        font_family_->activate(); font_size_->activate();
        for (Fl_Button *button : format_buttons) if (button) button->activate();
    } else {
        font_family_->deactivate(); font_size_->deactivate();
        for (Fl_Button *button : format_buttons) if (button) button->deactivate();
    }
    if (rich_mode_) rich_mode_->value(editor_.rich_format());
    if (!editor_.rich_format()) {
        font_family_->deactivate(); font_size_->deactivate();
        bold_->deactivate(); italic_->deactivate(); underline_->deactivate();
        clear_format_->deactivate(); align_left_->deactivate(); align_center_->deactivate();
        align_right_->deactivate(); bullet_list_->deactivate(); number_list_->deactivate();
        outdent_->deactivate(); indent_->deactivate(); link_button_->deactivate();
    }
    const bool editable = editor_.editing_allowed() && !session_stale_ &&
        !send_in_progress_ && !queue_pending_ && !copy_pending_ && !archive_retry_pending_;
    if (rich_mode_) {
        if (editable) rich_mode_->activate();
        else rich_mode_->deactivate();
    }
    if (editable && !save_gate_.pending() && accounts_.size() > 1) from_choice_->activate();
    else from_choice_->deactivate();
    const bool can_save = editable && save_gate_.dirty() && !save_gate_.pending();
    const bool can_change_attachments = editable && !save_gate_.pending();
    if (editable) {
        send_button_->activate(); queue_button_->activate();
    } else {
        send_button_->deactivate(); queue_button_->deactivate();
    }
    if (can_save) save_button_->activate(); else save_button_->deactivate();
    if (can_change_attachments) { attach_button_->activate(); remove_button_->activate(); }
    else { attach_button_->deactivate(); remove_button_->deactivate(); }
    const bool has_selection = word.selection_start() < word.selection_end();
    if (editable && has_selection) { copy_button_->activate(); cut_button_->activate(); }
    else { copy_button_->deactivate(); cut_button_->deactivate(); }
    if (editable) paste_button_->activate(); else paste_button_->deactivate();
    if (editable && editor_.can_undo()) quick_undo_->activate(); else quick_undo_->deactivate();
    if (editable && editor_.can_redo()) quick_redo_->activate(); else quick_redo_->deactivate();
    if (archive_retry_available_ && !archive_retry_pending_ && !session_stale_ &&
        !copy_pending_ && !send_in_progress_ && !queue_pending_)
        retry_archive_button_->activate();
    else retry_archive_button_->deactivate();
    if (editable && compose_catalog_.has_signature) signature_button_->activate();
    else signature_button_->deactivate();
    if (editable && compose_catalog_.templates.size()) template_choice_->activate();
    else template_choice_->deactivate();
    if (editable) contacts_button_->activate();
    else contacts_button_->deactivate();
    if (editable && contact_suggestions_.size()) contact_choice_->activate();
    else contact_choice_->deactivate();
    update_optional_recipient_controls();
}

void ComposeWindow::update_optional_recipient_controls() {
    if (!cc_toggle_ || !bcc_toggle_) return;
    const bool cc_has_value = cc_->value() && cc_->value()[0] != '\0';
    const bool bcc_has_value = bcc_->value() && bcc_->value()[0] != '\0';
    cc_toggle_->copy_label(optional_recipient_visibility_.cc_visible ? T("Hide Cc") : T("Cc +"));
    bcc_toggle_->copy_label(optional_recipient_visibility_.bcc_visible ? T("Hide Bcc") : T("Bcc +"));
    cc_toggle_->tooltip(optional_recipient_visibility_.cc_visible && cc_has_value ?
        T("Clear the Cc field before hiding it.") : T("Show or hide the Cc field."));
    bcc_toggle_->tooltip(optional_recipient_visibility_.bcc_visible && bcc_has_value ?
        T("Clear the Bcc field before hiding it.") : T("Show or hide the Bcc field."));
    if (optional_recipient_visibility_.cc_visible && cc_has_value) cc_toggle_->deactivate();
    else cc_toggle_->activate();
    if (optional_recipient_visibility_.bcc_visible && bcc_has_value) bcc_toggle_->deactivate();
    else bcc_toggle_->activate();
    cc_toggle_->redraw();
    bcc_toggle_->redraw();
}

void ComposeWindow::toggle_optional_recipient(bool cc) {
    Fl_Input *field = cc ? cc_ : bcc_;
    bool &visible = cc ? optional_recipient_visibility_.cc_visible :
        optional_recipient_visibility_.bcc_visible;
    const bool changed_visibility = cc ?
        compose_header_toggle_cc(optional_recipient_visibility_, field->value()) :
        compose_header_toggle_bcc(optional_recipient_visibility_, field->value());
    if (!changed_visibility) {
        update_optional_recipient_controls();
        return;
    }
    if (!visible && active_recipient_field_ == field) {
        active_recipient_field_ = to_;
        to_->take_focus();
    }
    layout_widgets();
    update_optional_recipient_controls();
    if (visible) {
        active_recipient_field_ = field;
        field->take_focus();
    }
    redraw();
}

void ComposeWindow::set_interaction_enabled(bool enabled) {
    const bool editable = enabled && editor_.editing_allowed() && !session_stale_ && !copy_pending_ &&
        !archive_retry_pending_;
    Fl_Input *fields[] = {to_, cc_, bcc_, subject_};
    for (Fl_Input *field : fields) {
        if (editable) field->activate(); else field->deactivate();
    }
    if (editable && !save_gate_.pending() && accounts_.size() > 1) from_choice_->activate();
    else from_choice_->deactivate();
    if (editable) canvas_->activate(); else canvas_->deactivate();
    if (editable) {
        save_button_->activate(); send_button_->activate(); queue_button_->activate();
        attach_button_->activate(); remove_button_->activate();
    } else {
        save_button_->deactivate(); send_button_->deactivate(); queue_button_->deactivate();
        attach_button_->deactivate(); remove_button_->deactivate();
    }
    update_format_controls();
}

bool ComposeWindow::current_account(Account &account, Text &error) const {
    error.clear();
    if (session_stale_ || !owner_ || owner_->coordinator_.profile_epoch() != profile_epoch_)
        return set_error(error, T("The local profile changed; this compose window cannot save or send."));
    for (size_t i = 0; i < owner_->startup_.accounts.size(); ++i) {
        if (owner_->startup_.accounts[i].id != account_.id) continue;
        if (owner_->startup_.accounts[i].generation != account_.generation)
            return set_error(error, T("This account changed while the compose window was open."));
        account = owner_->startup_.accounts[i];
        return true;
    }
    return set_error(error, T("The sending account is no longer available."));
}

bool ComposeWindow::sync_fields(Text &error) {
    error.clear();
    Account current;
    if (!current_account(current, error)) return false;
    if (draft_.account_id != current.id)
        return set_error(error, T("Draft sender identity changed; reopen or copy the draft first."));
    const char *to = to_->value() ? to_->value() : "";
    const char *cc = cc_->value() ? cc_->value() : "";
    const char *bcc = bcc_->value() ? bcc_->value() : "";
    if (!set_raw_recipient_inputs(draft_, to, strlen(to), cc, strlen(cc), bcc, strlen(bcc), error)) return false;
    const char *subject = subject_->value() ? subject_->value() : "";
    if (strlen(subject) > MaxEditableTextBytes || !valid_utf8(subject, strlen(subject)) ||
        !draft_.subject.set(subject, strlen(subject)))
        return set_error(error, T("Subject is invalid or too long."));
    if (!editor_.save_into_draft(draft_, error)) return false;
    return true;
}

void ComposeWindow::changed(bool body_change) {
    if (suppress_callbacks_) return;
    if (session_stale_) { status_->text(T("Profile changed; this draft is read-only until reopened.")); return; }
    if (body_change) canvas_->refresh_layout();
    if (!save_gate_.note_edit()) { status_->text(T("Draft edit counter reached its limit.")); return; }
    Fl::remove_timeout(autosave_cb, this);
    if (shown()) Fl::add_timeout(1.2, autosave_cb, this);
    status_->text(T("Unsaved changes — autosave scheduled"));
    update_format_controls();
}

bool ComposeWindow::submit_save() {
    if (session_stale_ || copy_pending_ || archive_retry_pending_) { status_->text(T("Profile or a send operation is in progress; saving is blocked.")); return false; }
    if (save_gate_.pending()) return true;
    if (!save_gate_.dirty()) return true;
    Text error;
    if (!sync_fields(error)) { status_->text(error.c_str()); return false; }
    Account current;
    if (!current_account(current, error)) { status_->text(error.c_str()); return false; }
    WorkerTaskInput input;
    ComposeSaveToken token;
    const WorkerTaskKind create_kind = !draft_.id && forward_source_valid_ ?
        WorkerTaskKind::SaveForwardDraft : WorkerTaskKind::SaveDraft;
    if (!save_gate_.begin(draft_, current.generation, attachment_refs_, input, token, error,
            create_kind, create_kind == WorkerTaskKind::SaveForwardDraft ? &forward_source_ : nullptr)) {
        status_->text(error.c_str()); return false;
    }
    update_format_controls();
    WorkerRequest request;
    request.account_id = current.id;
    request.account_generation = current.generation;
    MailWindow::PendingUiRequest pending;
    pending.purpose = MailWindow::PendingPurpose::ComposeSave;
    pending.account = current;
    pending.compose = this;
    pending.compose_save_token = token;
    if (!owner_->submit_task(token.kind, request, &input, SecretBytes(),
            static_cast<MailWindow::PendingUiRequest &&>(pending), error)) {
        bool save_again = false;
        Text ignored;
        (void)save_gate_.complete(token, false, nullptr, draft_, save_again, ignored);
        update_format_controls();
        status_->text(error.empty() ? T("Draft save could not start; edits remain here.") : error.c_str());
        return false;
    }
    save_generation_ = token.edit_generation;
    status_->text(token.kind == WorkerTaskKind::SaveDraft ?
        T("Saving the first draft revision…") : T("Saving this exact draft revision…"));
    return true;
}

bool ComposeWindow::submit_copy(const Account &target) {
    if (session_stale_ || copy_pending_ || archive_retry_pending_ || save_gate_.pending() || send_in_progress_ || queue_pending_) {
        status_->text(T("Wait for the current compose operation before copying this draft."));
        return false;
    }
    if (!draft_.id || !draft_.revision || target.id == account_.id) {
        status_->text(T("Save this draft first and choose a different account to copy it."));
        return false;
    }
    Text error;
    if (!sync_fields(error)) { status_->text(error.c_str()); return false; }
    Account source;
    if (!current_account(source, error)) { status_->text(error.c_str()); return false; }

    WorkerTaskInput input;
    input.item_id = draft_.id;
    input.item_revision = draft_.revision;
    input.account = source;
    input.has_draft = true;
    if (!clone_draft(draft_, input.draft, error)) { status_->text(error.c_str()); return false; }
    input.draft.id = 0;
    input.draft.revision = 0;
    input.draft.account_id = target.id;
    input.draft.attachments.clear();
    for (size_t i = 0; i < attachment_refs_.size(); ++i) {
        const WorkerDraftAttachmentRef &ref = attachment_refs_[i];
        if (ref.kind == WorkerAttachmentRefKind::ExistingDraftPart &&
            (ref.source_draft_id != draft_.id || ref.source_revision != draft_.revision)) {
            status_->text(T("An attachment belongs to an older draft revision; reload the draft before copying."));
            return false;
        }
        if (!input.attachment_refs.push(ref)) {
            status_->text(T("The attachment list exceeds its safe copy limit."));
            return false;
        }
    }

    WorkerRequest request;
    request.account_id = target.id;
    request.account_generation = target.generation;
    MailWindow::PendingUiRequest pending;
    pending.purpose = MailWindow::PendingPurpose::ComposeCopy;
    pending.account = target;
    pending.compose = this;
    pending.compose_copy_source_account = source;
    pending.compose_copy_source_id = draft_.id;
    pending.compose_copy_source_revision = draft_.revision;
    copy_pending_ = true;
    Fl::remove_timeout(autosave_cb, this);
    set_interaction_enabled(false);
    if (!owner_->submit_task(WorkerTaskKind::SaveDraftCopy, request, &input, SecretBytes(),
            static_cast<MailWindow::PendingUiRequest &&>(pending), error)) {
        copy_pending_ = false;
        set_interaction_enabled(true);
        status_->text(error.empty() ? T("Draft copy could not start; the original remains open.") : error.c_str());
        if (save_gate_.dirty() && shown()) Fl::add_timeout(1.2, autosave_cb, this);
        return false;
    }
    status_->text(T("Copying this exact draft revision under the selected account…"));
    return true;
}

bool ComposeWindow::schedule_save(bool explicit_save) {
    if (!save_gate_.dirty()) {
        if (explicit_save) status_->text(T("Draft is already saved."));
        return true;
    }
    return submit_save();
}

bool ComposeWindow::submit_queue_or_send(bool transmit) {
    if (session_stale_ || send_in_progress_ || queue_pending_ || archive_retry_pending_ || copy_pending_) return false;
    Text error;
    if (!sync_fields(error)) { status_->text(error.c_str()); return false; }
    send_after_save_ = transmit;
    if (save_gate_.pending()) {
        status_->text(T("Waiting for the current save before finalizing this message…"));
        return true;
    }
    if (save_gate_.dirty()) return submit_save();
    if (!draft_.id || !draft_.revision) {
        status_->text(T("Save the draft before queueing this revision."));
        return false;
    }
    Text confirmation;
    if (!compose_confirmation_text(account_, draft_, attachment_refs_.size(), confirmation, error)) {
        status_->text(error.c_str()); send_after_save_ = false; return false;
    }
    if (!compose_confirmation_accepted(literal_confirmation(transmit ? T("Confirm sending") : T("Confirm queueing"),
            transmit ? T("Review the full message identity and recipients before SMTP submission.") :
                T("This saves a finalized local send operation. No SMTP connection will be started."),
            confirmation.c_str(), T("Cancel"), transmit ? T("Send now") : T("Queue locally")))) {
        send_after_save_ = false;
        return true;
    }
    Account current;
    if (!current_account(current, error)) { status_->text(error.c_str()); send_after_save_ = false; return false; }
    WorkerTaskInput input;
    input.item_id = draft_.id;
    input.item_revision = draft_.revision;
    WorkerRequest request;
    request.account_id = current.id;
    request.account_generation = current.generation;
    MailWindow::PendingUiRequest pending;
    pending.purpose = MailWindow::PendingPurpose::ComposeQueue;
    pending.account = current;
    pending.compose = this;
    pending.compose_transmit = transmit;
    queue_pending_ = true;
    set_interaction_enabled(false);
    if (!owner_->submit_task(WorkerTaskKind::QueueSend, request, &input, SecretBytes(),
            static_cast<MailWindow::PendingUiRequest &&>(pending), error)) {
        queue_pending_ = false;
        set_interaction_enabled(true);
        status_->text(error.c_str()); send_after_save_ = false; return false;
    }
    status_->text(transmit ? T("Finalizing the selected draft revision before SMTP…") :
        T("Creating a local queued operation; no message will be sent now…"));
    return true;
}

bool ComposeWindow::submit_send(uint64_t operation_id) {
    Text error;
    Account current;
    if (!operation_id || !current_account(current, error)) { status_->text(error.c_str()); return false; }
    SecretBytes credentials;
    if (!owner_->make_live_credentials(credentials, error)) { status_->text(error.c_str()); return false; }
    const Text &smtp_user = current.smtp_username.empty() ? current.username : current.smtp_username;
    SecretBytes matched;
    if (!worker_find_credential(credentials, current, CredentialPurpose::Smtp, smtp_user,
            profile_epoch_, matched, error)) {
        status_->text(T("No current SMTP credential is available. Unlock the vault or enter the account password first."));
        return false;
    }
    matched.clear();
    WorkerTaskInput input;
    input.item_id = operation_id;
    WorkerRequest request;
    request.account_id = current.id;
    request.account_generation = current.generation;
    MailWindow::PendingUiRequest pending;
    pending.purpose = MailWindow::PendingPurpose::ComposeSend;
    pending.account = current;
    pending.compose = this;
    send_in_progress_ = true;
    set_interaction_enabled(false);
    if (!owner_->submit_task(WorkerTaskKind::SendQueued, request, &input,
            static_cast<SecretBytes &&>(credentials), static_cast<MailWindow::PendingUiRequest &&>(pending), error)) {
        send_in_progress_ = false;
        set_interaction_enabled(true);
        status_->text(error.c_str()); return false;
    }
    status_->text(T("SMTP submission is running; closing is blocked until its result is known."));
    return true;
}

bool ComposeWindow::submit_archive_retry() {
    if (session_stale_ || !archive_retry_available_ || archive_retry_pending_ ||
        send_in_progress_ || queue_pending_ || copy_pending_) return false;
    Text error;
    Account current;
    if (!operation_id_ || !current_account(current, error)) {
        status_->text(error.empty() ? T("No confirmed delivery is available for archive retry.") : error.c_str());
        return false;
    }
    SecretBytes credentials;
    if (!owner_->make_live_credentials(credentials, error)) {
        status_->text(error.c_str());
        return false;
    }
    SecretBytes imap_credential;
    if (!worker_find_credential(credentials, current, CredentialPurpose::Imap, current.username,
            profile_epoch_, imap_credential, error)) {
        status_->text(T("No current IMAP credential is available to retry Sent-folder archiving."));
        return false;
    }
    imap_credential.clear();
    WorkerTaskInput input;
    input.item_id = operation_id_;
    WorkerRequest request;
    request.account_id = current.id;
    request.account_generation = current.generation;
    MailWindow::PendingUiRequest pending;
    pending.purpose = MailWindow::PendingPurpose::ComposeArchiveRetry;
    pending.account = current;
    pending.compose = this;
    archive_retry_pending_ = true;
    set_interaction_enabled(false);
    if (!owner_->submit_task(WorkerTaskKind::RetryArchive, request, &input,
            static_cast<SecretBytes &&>(credentials), static_cast<MailWindow::PendingUiRequest &&>(pending), error)) {
        archive_retry_pending_ = false;
        set_interaction_enabled(true);
        status_->text(error.empty() ? T("Sent-folder archive retry could not start.") : error.c_str());
        return false;
    }
    status_->text(T("Retrying Sent-folder archiving only; SMTP will not be contacted."));
    return true;
}

bool ComposeWindow::request_compose_catalog(const Text &query, uint64_t template_id) {
    if (session_stale_ || send_in_progress_ || queue_pending_ || copy_pending_ || archive_retry_pending_)
        return false;
    if (query.size() > 256 || !valid_utf8(query.c_str(), query.size()) ||
        (template_id && !query.empty())) {
        status_->text(T("Contact search or template request is invalid."));
        return false;
    }
    if (compose_catalog_serial_ == UINT64_MAX) {
        status_->text(T("Compose catalog request limit reached; reopen this window."));
        return false;
    }
    Text error;
    Account current;
    if (!current_account(current, error)) { status_->text(error.c_str()); return false; }
    WorkerTaskInput input;
    input.item_id = template_id;
    input.query = query;
    WorkerRequest request;
    request.account_id = current.id;
    request.account_generation = current.generation;
    MailWindow::PendingUiRequest pending;
    pending.purpose = MailWindow::PendingPurpose::ComposeCatalog;
    pending.account = current;
    pending.compose = this;
    pending.compose_catalog_serial = ++compose_catalog_serial_;
    pending.compose_template_id = template_id;
    pending.compose_catalog_query = query;
    if (!query.empty()) {
        contact_suggestions_.clear();
        contact_choice_->clear(); contact_choice_->value(-1); contact_choice_->deactivate();
    }
    if (!owner_->submit_task(WorkerTaskKind::ComposeCatalog, request, &input, SecretBytes(),
            static_cast<MailWindow::PendingUiRequest &&>(pending), error)) {
        status_->text(error.empty() ? T("Compose catalog request could not start.") : error.c_str());
        return false;
    }
    if (!query.empty()) status_->text(T("Searching saved contacts…"));
    else if (template_id) status_->text(T("Loading the selected template…"));
    return true;
}

bool ComposeWindow::update_attachment_summary() {
    if (attachment_refs_.empty()) selected_attachment_ = 0;
    else if (selected_attachment_ >= attachment_refs_.size()) selected_attachment_ = attachment_refs_.size() - 1;
    if (attachment_refs_.empty()) attachment_list_->hide();
    else attachment_list_->show();
    if (top_) layout_widgets();
    attachment_list_->redraw();
    return true;
}

void ComposeWindow::apply_style(unsigned mask, const rw::TextStyle &style) {
    const uint64_t prior = editor_.word_editor().doc.revision;
    if (editor_.apply_character_style(style, mask)) {
        if (editor_.word_editor().doc.revision != prior) changed(true);
        update_format_controls(); canvas_->redraw();
    } else status_->text(editor_.error().c_str());
}

void ComposeWindow::apply_paragraph(rw::ParagraphStyle style, unsigned mask) {
    const uint64_t prior = editor_.word_editor().doc.revision;
    if (editor_.apply_paragraph_style(style, mask)) {
        if (editor_.word_editor().doc.revision != prior) changed(true);
        update_format_controls();
        canvas_->redraw();
    } else status_->text(editor_.error().c_str());
}

void ComposeWindow::apply_font_family() {
    rw::TextStyle style = editor_.word_editor().typing_style;
    style.family = font_family_->value() == 0 ? rw::FontFamily::Sans :
        font_family_->value() == 1 ? rw::FontFamily::Serif : rw::FontFamily::Mono;
    apply_style(rw::Family, style);
}

void ComposeWindow::apply_font_size() {
    static const uint16_t sizes[] = {9, 10, 11, 12, 14, 16, 18, 24, 36};
    const int selected = font_size_->value();
    if (selected < 0 || selected >= static_cast<int>(sizeof(sizes) / sizeof(sizes[0]))) return;
    rw::TextStyle style = editor_.word_editor().typing_style;
    style.size = sizes[selected];
    apply_style(rw::Size, style);
}

bool ComposeWindow::copy_selection(ComposeClipboardOperation operation) {
    ComposeClipboard &clipboard = compose_clipboard();
    rw::Document next;
    Array<MailLinkRange> links;
    if (!editor_.copy_selection(next, links)) return false;
    rw::Text plain;
    if (!rw::document_text(next, 0, next.chars.size(), plain) || plain.size() > static_cast<size_t>(INT32_MAX))
        return false;
    if (!clipboard.document.clone_from(next)) return false;
    clipboard.links = static_cast<Array<MailLinkRange> &&>(links);
    if (!clipboard.plain.set(plain.c_str(), plain.size())) return false;
    clipboard.valid = true;
    const int buffer = compose_clipboard_buffer(operation);
    if (buffer < 0) return false;
    Fl::copy(plain.c_str(), static_cast<int>(plain.size()), buffer);
    return true;
}

void ComposeWindow::paste_text(const char *text, size_t length) {
    if (!text || !length) return;
    ComposeClipboard &clipboard = compose_clipboard();
    bool pasted_rich = false;
    if (clipboard.valid && clipboard.plain.size() == length && !memcmp(clipboard.plain.c_str(), text, length))
        pasted_rich = editor_.paste_document(clipboard.document, clipboard.links);
    if (!pasted_rich && !editor_.insert_utf8(text, length, false)) {
        status_->text(editor_.error().c_str()); return;
    }
    canvas_changed();
}

void ComposeWindow::canvas_changed() {
    changed(true);
    update_format_controls();
    if (canvas_) { canvas_->refresh_layout(); canvas_->redraw(); }
}

void ComposeWindow::canvas_status(const char *message) {
    if (message && *message) status_->text(message);
}

void ComposeWindow::save_result(const ComposeSaveToken &token, bool succeeded,
    const WorkerDraftSnapshot *snapshot, const char *diagnostic) {
    bool save_again = false;
    Text error;
    const bool saved = save_gate_.complete(token, succeeded, snapshot, draft_, save_again, error);
    update_format_controls();
    if (!saved) {
        status_->text(diagnostic && *diagnostic ? diagnostic : error.c_str());
        close_after_save_ = false;
        application_close_after_save_ = false;
        send_after_save_ = false;
        if (save_again) { Fl::remove_timeout(autosave_cb, this); Fl::add_timeout(1.2, autosave_cb, this); }
        return;
    }
    if (snapshot) {
        attachment_metadata_.clear();
        for (size_t i = 0; i < snapshot->attachments.size(); ++i)
            if (!attachment_metadata_.push(snapshot->attachments[i])) {
                status_->text(T("Draft saved, but attachment metadata could not be refreshed."));
                return;
            }
        attachment_refs_.clear();
        for (size_t i = 0; i < snapshot->attachments.size(); ++i) {
            WorkerDraftAttachmentRef ref;
            ref.kind = WorkerAttachmentRefKind::ExistingDraftPart;
            ref.source_draft_id = draft_.id;
            ref.source_revision = draft_.revision;
            ref.source_position = static_cast<uint32_t>(i);
            ref.filename = snapshot->attachments[i].filename;
            ref.content_type = snapshot->attachments[i].content_type;
            ref.content_id = snapshot->attachments[i].content_id;
            ref.inline_part = snapshot->attachments[i].inline_part;
            if (!attachment_refs_.push(static_cast<WorkerDraftAttachmentRef &&>(ref))) {
                status_->text(T("Draft saved, but attachment references could not be refreshed."));
                return;
            }
        }
        update_attachment_summary();
    }
    if (token.kind == WorkerTaskKind::SaveForwardDraft) forward_source_valid_ = false;
    status_->text(T("Draft saved safely."));
    if (save_again) {
        Fl::remove_timeout(autosave_cb, this);
        Fl::add_timeout(.05, autosave_cb, this);
        return;
    }
    if (close_after_save_) {
        close_after_save_ = false;
        const bool close_application = application_close_after_save_;
        application_close_after_save_ = false;
        finish_close();
        if (close_application && owner_) owner_->close_step();
        return;
    }
    if (send_after_save_) {
        const bool transmit = send_after_save_;
        send_after_save_ = false;
        (void)submit_queue_or_send(transmit);
    }
}

void ComposeWindow::copy_result(bool succeeded, WorkerDraftSnapshot *snapshot, const Account &target,
    const Account &source, uint64_t source_id, uint64_t source_revision, const char *diagnostic) {
    copy_pending_ = false;
    if (!succeeded || !snapshot) {
        status_->text(diagnostic && *diagnostic ? diagnostic : T("Draft copy failed; the original remains open."));
        set_interaction_enabled(true);
        if (!session_stale_ && save_gate_.dirty() && shown()) Fl::add_timeout(1.2, autosave_cb, this);
        return;
    }
    if (!owner_) {
        status_->text(T("The copy completed, but this compose window is no longer available."));
        return;
    }
    bool target_current = false;
    for (size_t i = 0; i < owner_->startup_.accounts.size(); ++i)
        if (owner_->startup_.accounts[i].id == target.id &&
            owner_->startup_.accounts[i].generation == target.generation) target_current = true;
    Account current_source;
    Text error;
    if (owner_->coordinator_.profile_epoch() != profile_epoch_ || session_stale_ ||
        !target_current || !current_account(current_source, error) ||
        current_source.id != source.id || current_source.generation != source.generation ||
        account_.id != source.id || account_.generation != source.generation ||
        draft_.id != source_id || draft_.revision != source_revision ||
        snapshot->account_generation != target.generation || snapshot->draft.account_id != target.id ||
        !snapshot->draft.id || !snapshot->draft.revision || snapshot->draft.id == source_id) {
        status_->text(T("The copy completed, but this editor or account changed. Reopen the target account's draft list."));
        set_interaction_enabled(true);
        return;
    }
    if (!open_existing(owner_->startup_.accounts, target,
            static_cast<WorkerDraftSnapshot &&>(*snapshot), profile_epoch_)) {
        status_->text(T("The copy was saved, but could not replace this editor. The original draft remains on its account."));
        set_interaction_enabled(true);
        return;
    }
    status_->text(T("Draft copied under the selected account. The original draft remains unchanged."));
}

void ComposeWindow::queue_result(bool succeeded, const WorkerOperationResult *operation,
    const char *diagnostic, bool transmit) {
    queue_pending_ = false;
    if (!succeeded || !operation) {
        status_->text(diagnostic && *diagnostic ? diagnostic : T("The finalized send operation could not be created."));
        send_after_save_ = false;
        set_interaction_enabled(true);
        return;
    }
    operation_id_ = operation->operation_id;
    if (operation->stage != SendStage::Ready) {
        status_->text(operation->stage == SendStage::Uncertain ?
            T("Delivery is uncertain. This message will not be sent again automatically.") :
            T("This draft already has a finalized send operation; no new payload was created."));
        send_after_save_ = false;
        set_interaction_enabled(true);
        return;
    }
    if (!transmit) {
        status_->text(T("Queued locally. No SMTP connection was started."));
        send_after_save_ = false;
        set_interaction_enabled(true);
        return;
    }
    if (!submit_send(operation->operation_id)) {
        ComposeQueueHandoffState handoff;
        handoff.operation_id = operation_id_;
        handoff.controls_enabled = false;
        handoff.send_after_save = send_after_save_;
        handoff.send_in_progress = send_in_progress_;
        compose_queue_send_preflight_failed(handoff);
        operation_id_ = handoff.operation_id;
        send_after_save_ = handoff.send_after_save;
        send_in_progress_ = handoff.send_in_progress;
        set_interaction_enabled(handoff.controls_enabled);
    }
}

void ComposeWindow::send_result(bool succeeded, const WorkerOperationResult *operation,
    const char *diagnostic) {
    send_in_progress_ = false;
    if (!operation) {
        archive_retry_available_ = false;
        status_->text(diagnostic && *diagnostic ? diagnostic : T("SMTP submission failed."));
        set_interaction_enabled(true);
        return;
    }
    operation_id_ = operation->operation_id;
    switch (operation->stage) {
    case SendStage::Archived:
    case SendStage::Sent:
        archive_retry_available_ = false;
        status_->text(T("SMTP delivery is confirmed and the Sent-folder archive completed."));
        break;
    case SendStage::ArchivePending:
    case SendStage::ArchiveFailed:
    case SendStage::Accepted:
        archive_retry_available_ = operation->delivery_count > 0 && operation_id_ != 0;
        status_->text(T("SMTP delivery is confirmed; Sent-folder archiving needs attention. SMTP will not be retried."));
        break;
    case SendStage::Uncertain:
    case SendStage::AwaitingFinalResponse:
        archive_retry_available_ = false;
        status_->text(T("SMTP delivery is uncertain. Do not retry this message."));
        break;
    case SendStage::Ready:
    case SendStage::Failed:
        archive_retry_available_ = false;
        status_->text(diagnostic && *diagnostic ? diagnostic : T("SMTP did not accept this message. You may review the draft and retry explicitly."));
        break;
    default:
        status_->text(succeeded ? T("Send operation completed.") :
            (diagnostic && *diagnostic ? diagnostic : T("Send operation needs attention.")));
        break;
    }
    set_interaction_enabled(true);
}

void ComposeWindow::archive_retry_result(bool succeeded, const WorkerOperationResult *operation,
    const char *diagnostic) {
    archive_retry_pending_ = false;
    if (!operation || !operation->operation_id || operation->operation_id != operation_id_) {
        status_->text(diagnostic && *diagnostic ? diagnostic : T("Archive retry returned no matching operation."));
        set_interaction_enabled(true);
        return;
    }
    if (operation->stage == SendStage::Archived || operation->stage == SendStage::Sent) {
        archive_retry_available_ = false;
        status_->text(T("Sent-folder archive completed. SMTP was not retried."));
    } else if (operation->delivery_count > 0 &&
            (operation->stage == SendStage::ArchiveFailed || operation->stage == SendStage::ArchivePending ||
             operation->stage == SendStage::Accepted)) {
        archive_retry_available_ = true;
        status_->text(diagnostic && *diagnostic ? diagnostic : T("Archiving is still pending; SMTP was not retried."));
    } else {
        archive_retry_available_ = false;
        status_->text(succeeded ? T("Archive retry completed without an archived result.") :
            (diagnostic && *diagnostic ? diagnostic : T("Archive retry failed; SMTP was not retried.")));
    }
    set_interaction_enabled(true);
}

void ComposeWindow::compose_catalog_result(bool succeeded, WorkerComposeCatalog *catalog,
    const Account &account, uint64_t serial, uint64_t template_id, const Text &query,
    const char *diagnostic) {
    if (serial != compose_catalog_serial_) return;
    if (!succeeded || !catalog) {
        status_->text(diagnostic && *diagnostic ? diagnostic : T("Compose catalog could not be loaded."));
        return;
    }
    if (!owner_ || session_stale_ || owner_->coordinator_.profile_epoch() != profile_epoch_ ||
        account.id != account_.id || account.generation != account_.generation ||
        catalog->account_generation != account.generation) {
        status_->text(T("Contact, signature, or template data changed accounts; retry from the current draft."));
        return;
    }
    Account current;
    Text error;
    if (!current_account(current, error) || current.id != account.id || current.generation != account.generation) {
        status_->text(error.empty() ? T("The compose account changed before catalog results arrived.") : error.c_str());
        return;
    }
    if (catalog->has_signature) {
        compose_catalog_.has_signature = true;
        compose_catalog_.signature_id = catalog->signature_id;
        compose_catalog_.signature_body = catalog->signature_body;
    } else {
        compose_catalog_.has_signature = false;
        compose_catalog_.signature_id = 0;
        compose_catalog_.signature_body.clear();
    }
    if (template_id) {
        if (catalog->selected_template_id != template_id) {
            status_->text(T("The selected template changed before it could be inserted."));
            return;
        }
        if (!catalog->selected_template_body.empty() &&
            !editor_.insert_utf8(catalog->selected_template_body.c_str(),
                catalog->selected_template_body.size(), false)) {
            status_->text(editor_.error().c_str());
            return;
        }
        if (!catalog->selected_template_body.empty()) canvas_changed();
        if (!catalog->selected_template_subject.empty() && !*subject_->value()) {
            subject_->value(catalog->selected_template_subject.c_str());
            changed();
        }
        template_choice_->value(-1);
        status_->text(T("Template content inserted at the current caret; the subject was filled only if empty."));
        update_format_controls();
        return;
    }

    compose_catalog_ = static_cast<WorkerComposeCatalog &&>(*catalog);
    contact_suggestions_.clear();
    template_choice_->clear();
    for (size_t i = 0; i < compose_catalog_.templates.size(); ++i) {
        WorkerComposeContactSuggestion display;
        display.display_name = compose_catalog_.templates[i].name;
        display.address = compose_catalog_.templates[i].subject.empty() ?
            compose_catalog_.templates[i].name : compose_catalog_.templates[i].subject;
        Text label;
        if (!contact_choice_label(display, label) && !label.set(T("Template")))
            label.set("Template");
        template_choice_->add(label.c_str());
    }
    template_choice_->value(-1);
    contact_choice_->clear();
    for (size_t i = 0; i < compose_catalog_.contacts.size(); ++i) {
        Text label;
        if (!contact_choice_label(compose_catalog_.contacts[i], label)) {
            status_->text(T("Contact suggestions could not be retained safely."));
            continue;
        }
        if (!contact_suggestions_.push(compose_catalog_.contacts[i])) {
            status_->text(T("Contact suggestions could not be retained safely."));
            break;
        }
        contact_choice_->add(label.c_str());
    }
    contact_choice_->value(-1);
    update_format_controls();
    if (!query.empty()) {
        if (contact_suggestions_.empty()) status_->text(T("No saved contacts matched that search."));
        else if (compose_catalog_.contacts_truncated) status_->text(T("Showing the first 32 matching contact addresses."));
        else status_->text(T("Choose a matching contact to append its address to the active recipient field."));
    } else if (compose_catalog_.templates_truncated) {
        status_->text(T("Showing the first 64 templates; manage the catalog in Tools."));
    }
}

void ComposeWindow::insert_contact_choice() {
    const int selected = contact_choice_->value();
    if (selected < 0 || static_cast<size_t>(selected) >= contact_suggestions_.size()) return;
    Fl_Input *field = active_recipient_field_;
    if (field != to_ && field != cc_ && field != bcc_) field = to_;
    const char *current = field->value() ? field->value() : "";
    const size_t current_length = strlen(current);
    const Text &address = contact_suggestions_[static_cast<size_t>(selected)].address;
    const bool add_separator = current_length != 0;
    if (current_length > MaxEditableTextBytes || address.size() > MaxEditableTextBytes - current_length - (add_separator ? 2 : 0)) {
        status_->text(T("The recipient field is too long to add this contact."));
        contact_choice_->value(-1);
        return;
    }
    Array<char> combined;
    for (size_t i = 0; i < current_length; ++i)
        if (!combined.push(current[i])) { status_->text(T("Could not add this contact.")); return; }
    if (add_separator && (!combined.push(',') || !combined.push(' '))) {
        status_->text(T("Could not add this contact.")); return;
    }
    for (size_t i = 0; i < address.size(); ++i)
        if (!combined.push(address.c_str()[i])) { status_->text(T("Could not add this contact.")); return; }
    if (!valid_utf8(combined.data(), combined.size())) {
        status_->text(T("The contact address is not valid UTF-8.")); return;
    }
    field->value(combined.data());
    changed();
    contact_choice_->value(-1);
    status_->text(T("Contact address appended. The complete recipient field remains the send-time authority."));
}

void ComposeWindow::profile_transition() {
    session_stale_ = true;
    Fl::remove_timeout(autosave_cb, this);
    status_->text(T("Profile changed. This editor is preserved locally and cannot save or send until reopened."));
    to_->deactivate(); cc_->deactivate(); bcc_->deactivate(); subject_->deactivate();
    if (canvas_) canvas_->deactivate();
    update_format_controls();
}

bool ComposeWindow::close_for_application(Text &reason) {
    if (!shown()) return true;
    if (save_gate_.pending() || send_in_progress_ || queue_pending_ || copy_pending_ || archive_retry_pending_)
        return set_error(reason, T("Wait for the open compose operation to finish before closing ReliefMail."));
    if (save_gate_.dirty()) {
        const int choice = fl_choice(T("Save changes to this draft before closing ReliefMail?"),
            T("Cancel"), T("Discard and close"), T("Save and close"));
        if (choice == 0)
            return set_error(reason, T("The unsaved draft is still open."));
        if (choice == 1) {
            application_close_after_save_ = false;
            finish_close();
            return true;
        }
        application_close_after_save_ = true;
        close_after_save_ = true;
        if (!submit_save()) {
            application_close_after_save_ = false;
            close_after_save_ = false;
            return set_error(reason, T("The draft could not be saved; ReliefMail remains open."));
        }
        return set_error(reason, T("Saving the draft before closing ReliefMail."));
    }
    finish_close();
    return true;
}

void ComposeWindow::finish_close() {
    Fl::remove_timeout(autosave_cb, this);
    set_non_modal();
    hide();
}

void ComposeWindow::close_cb(Fl_Widget *, void *context) {
    ComposeWindow *window = static_cast<ComposeWindow *>(context);
    if (window->save_gate_.pending() || window->send_in_progress_ || window->queue_pending_ ||
        window->copy_pending_ || window->archive_retry_pending_) {
        window->status_->text(T("Wait for the current save or send result before closing."));
        return;
    }
    if (!window->save_gate_.dirty()) { window->finish_close(); return; }
    const int choice = fl_choice(T("Save changes to this draft before closing?"),
        T("Cancel"), T("Discard edits"), T("Save"));
    if (choice == 1) window->finish_close();
    else if (choice == 2) {
        window->close_after_save_ = true;
        if (!window->submit_save()) window->close_after_save_ = false;
    }
}

void ComposeWindow::action_cb(Fl_Widget *widget, void *data) {
    ComposeWindow *window = static_cast<ComposeWindow *>(widget->window());
    if (!window) return;
    const Action action = static_cast<Action>(reinterpret_cast<intptr_t>(data) - 1);
    if (window->session_stale_ || window->copy_pending_ || window->send_in_progress_ ||
        window->queue_pending_ || window->archive_retry_pending_) {
        window->status_->text(T("This compose action is unavailable while the profile or current operation is unresolved."));
        return;
    }
    switch (action) {
    case Action::Save: (void)window->schedule_save(true); return;
    case Action::Send: (void)window->submit_queue_or_send(true); return;
    case Action::Queue: (void)window->submit_queue_or_send(false); return;
    case Action::RetryArchive: (void)window->submit_archive_retry(); return;
    case Action::Attach: {
        if (window->save_gate_.pending()) { window->status_->text(T("Wait for the current save before changing attachments.")); return; }
        char *path = fl_file_chooser(T("Attach a file"), "*", nullptr, 1);
        if (!path || !*path) return;
        if (window->attachment_refs_.size() >= MaxMimeParts) { window->status_->text(T("Attachment limit reached.")); return; }
        WorkerDraftAttachmentRef ref;
        ref.kind = WorkerAttachmentRefKind::SelectedFile;
        if (!ref.path.set(path)) { window->status_->text(T("Could not store the selected path.")); return; }
        const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
        if (!ref.filename.set(base) || !ref.content_type.set(content_type_for_path(path)) ||
            !window->attachment_refs_.push(static_cast<WorkerDraftAttachmentRef &&>(ref))) {
            window->status_->text(T("Could not add this attachment.")); return;
        }
        window->selected_attachment_ = window->attachment_refs_.size() - 1;
        window->changed(); window->update_attachment_summary();
        return;
    }
    case Action::RemoveAttachment:
        if (window->save_gate_.pending()) { window->status_->text(T("Wait for the current save before changing attachments.")); return; }
        if (!window->attachment_refs_.empty() &&
            window->selected_attachment_ < window->attachment_refs_.size()) {
            const size_t removed = window->selected_attachment_;
            window->attachment_refs_.erase(removed);
            if (removed < window->attachment_metadata_.size()) window->attachment_metadata_.erase(removed);
            if (window->selected_attachment_ >= window->attachment_refs_.size() &&
                !window->attachment_refs_.empty()) window->selected_attachment_ = window->attachment_refs_.size() - 1;
            window->changed(); window->update_attachment_summary();
        }
        return;
    case Action::Copy:
        if (!window->copy_selection(ComposeClipboardOperation::KeyboardCopy))
            window->status_->text(T("Select text before copying."));
        return;
    case Action::Cut:
        if (window->copy_selection(ComposeClipboardOperation::KeyboardCut)) {
            if (window->editor_.erase(false)) window->canvas_changed();
            else window->status_->text(window->editor_.error().c_str());
        } else window->status_->text(T("Select text before cutting."));
        return;
    case Action::Paste:
        window->canvas_->take_focus();
        (void)compose_request_paste(window->canvas_, ComposeClipboardOperation::KeyboardPaste,
            request_fltk_paste, nullptr);
        return;
    case Action::Bold:
    case Action::Italic:
    case Action::Underline: {
        rw::TextStyle style = window->editor_.word_editor().typing_style;
        unsigned mask = 0;
        if (action == Action::Bold) { style.bold = !style.bold; mask = rw::Bold; }
        else if (action == Action::Italic) { style.italic = !style.italic; mask = rw::Italic; }
        else { style.underline = !style.underline; mask = rw::Underline; }
        window->apply_style(mask, style); return;
    }
    case Action::ClearFormat: {
        const rw::TextStyle style;
        window->apply_style(rw::AllStyle, style);
        return;
    }
    case Action::Color: {
        rw::TextStyle style = window->editor_.word_editor().typing_style;
        double red = ((style.foreground >> 16) & 255) / 255.0;
        double green = ((style.foreground >> 8) & 255) / 255.0;
        double blue = (style.foreground & 255) / 255.0;
        if (!fl_color_chooser(T("Text color"), red, green, blue)) return;
        style.foreground = (static_cast<uint32_t>(red * 255.0 + 0.5) << 16) |
            (static_cast<uint32_t>(green * 255.0 + 0.5) << 8) |
            static_cast<uint32_t>(blue * 255.0 + 0.5);
        window->apply_style(rw::Foreground, style); return;
    }
    case Action::AlignLeft:
    case Action::AlignCenter:
    case Action::AlignRight: {
        rw::ParagraphStyle style = window->editor_.word_editor().doc.paragraphs[
            window->editor_.word_editor().doc.paragraph_at(window->editor_.word_editor().caret)].style;
        style.alignment = action == Action::AlignLeft ? rw::Alignment::Left :
            action == Action::AlignCenter ? rw::Alignment::Center : rw::Alignment::Right;
        window->apply_paragraph(style, rw::ParaAlignment); return;
    }
    case Action::BulletList:
    case Action::NumberList: {
        rw::ParagraphStyle style = window->editor_.word_editor().doc.paragraphs[
            window->editor_.word_editor().doc.paragraph_at(window->editor_.word_editor().caret)].style;
        const rw::ListKind wanted = action == Action::BulletList ? rw::ListKind::Bullet : rw::ListKind::Number;
        style.list = style.list == wanted ? rw::ListKind::Plain : wanted;
        window->apply_paragraph(style, rw::ParaList); return;
    }
    case Action::Indent: {
        rw::ParagraphStyle style = window->editor_.word_editor().doc.paragraphs[
            window->editor_.word_editor().doc.paragraph_at(window->editor_.word_editor().caret)].style;
        style.left_indent = static_cast<int16_t>(style.left_indent <= 680 ? style.left_indent + 18 : 720);
        window->apply_paragraph(style, rw::ParaIndent); return;
    }
    case Action::Outdent: {
        rw::ParagraphStyle style = window->editor_.word_editor().doc.paragraphs[
            window->editor_.word_editor().doc.paragraph_at(window->editor_.word_editor().caret)].style;
        style.left_indent = static_cast<int16_t>(style.left_indent >= 18 ? style.left_indent - 18 : 0);
        window->apply_paragraph(style, rw::ParaIndent); return;
    }
    case Action::Link: {
        const char *target = fl_input(T("Link target (http, https, or mailto)"), "https://");
        if (!target) return;
        if (!window->editor_.set_link(target, strlen(target))) window->status_->text(window->editor_.error().c_str());
        else window->canvas_changed();
        return;
    }
    case Action::Undo:
        if (window->editor_.undo()) window->canvas_changed();
        else window->status_->text(window->editor_.error().c_str());
        return;
    case Action::Redo:
        if (window->editor_.redo()) window->canvas_changed();
        else window->status_->text(window->editor_.error().c_str());
        return;
    case Action::Find: {
        const char *query = fl_input(T("Find text"), "");
        if (!query || !*query) return;
        size_t start = 0, end = 0;
        rw::Editor &word = window->editor_.word_editor();
        if (!window->editor_.find(query, false, word.caret, start, end) &&
            !window->editor_.find(query, false, 0, start, end)) {
            window->status_->text(T("Text not found.")); return;
        }
        word.select(start, end); window->canvas_->redraw();
        window->status_->text(T("Matching text selected.")); return;
    }
    case Action::SelectAll:
        (void)window->editor_.select_all(); window->canvas_->redraw(); return;
    case Action::PlainMode: {
        const bool rich = !window->editor_.rich_format();
        if (!rich && !compose_confirmation_accepted(literal_confirmation(T("Use plain-text format"),
                T("Formatting and hyperlinks will be omitted from the outgoing message. The saved rich document remains available if you switch back before sending."),
                "", T("Keep rich formatting"), T("Use plain text")))) {
            window->rich_mode_->value(window->editor_.rich_format());
            return;
        }
        if (!window->editor_.set_rich_format(rich)) window->status_->text(window->editor_.error().c_str());
        else { window->changed(); window->update_format_controls(); }
        return;
    }
    case Action::NextAccount:
    case Action::PreviousAccount: {
        if (window->accounts_.size() < 2) return;
        size_t index = 0;
        for (; index < window->accounts_.size(); ++index) if (window->accounts_[index].id == window->account_.id) break;
        if (index >= window->accounts_.size()) return;
        index = action == Action::NextAccount ? (index + 1) % window->accounts_.size() :
            (index + window->accounts_.size() - 1) % window->accounts_.size();
        window->change_account(index);
        return;
    }
    case Action::InsertSignature: {
        if (!window->compose_catalog_.has_signature) {
            window->status_->text(T("No enabled signature is available for this account."));
            return;
        }
        const bool has_body = !window->editor_.word_editor().doc.chars.empty();
        Array<char> insertion;
        if (has_body && (!insertion.push('\n') || !insertion.push('\n'))) {
            window->status_->text(T("Could not prepare the signature text.")); return;
        }
        for (size_t i = 0; i < window->compose_catalog_.signature_body.size(); ++i) {
            if (!insertion.push(window->compose_catalog_.signature_body.c_str()[i])) {
                window->status_->text(T("Could not prepare the signature text.")); return;
            }
        }
        if (!window->editor_.insert_utf8(insertion.data(), insertion.size(), false)) {
            window->status_->text(window->editor_.error().c_str()); return;
        }
        window->canvas_changed();
        window->status_->text(T("Signature inserted at the current caret."));
        return;
    }
    case Action::SearchContacts: {
        const char *query = fl_input(T("Find contacts by name or address"), "");
        if (!query) return;
        Text search;
        if (!search.set(query, strlen(query))) {
            window->status_->text(T("Contact search could not be retained safely.")); return;
        }
        (void)window->request_compose_catalog(search);
        return;
    }
    }
}

void ComposeWindow::field_cb(Fl_Widget *, void *context) {
    static_cast<ComposeWindow *>(context)->changed();
}

void ComposeWindow::optional_recipient_cb(Fl_Widget *widget, void *context) {
    ComposeWindow *window = static_cast<ComposeWindow *>(context);
    if (!window) return;
    window->toggle_optional_recipient(widget == window->cc_toggle_);
}

void ComposeWindow::format_cb(Fl_Widget *widget, void *context) {
    ComposeWindow *window = static_cast<ComposeWindow *>(context);
    if (window->session_stale_ || window->copy_pending_ || window->send_in_progress_ ||
        window->queue_pending_ || window->archive_retry_pending_) return;
    if (widget == window->font_family_) window->apply_font_family();
    else if (widget == window->font_size_) window->apply_font_size();
}

void ComposeWindow::account_cb(Fl_Widget *, void *context) {
    ComposeWindow *window = static_cast<ComposeWindow *>(context);
    if (!window || !window->from_choice_) return;
    const int selected = window->from_choice_->value();
    if (selected < 0) { window->update_from_label(); return; }
    size_t live_index = SIZE_MAX;
    if (!window->owner_ || !compose_account_choice_live_index(window->from_choice_accounts_,
            selected, window->owner_->startup_.accounts, live_index)) {
        if (window->owner_ && !window->accounts_.clone_from(window->owner_->startup_.accounts)) {
            window->from_choice_->deactivate();
            window->status_->text(T("The sender account list could not be refreshed."));
            return;
        }
        window->status_->text(T("The selected sender account is no longer available."));
        window->update_from_label();
        return;
    }
    uint64_t account_id = 0;
    uint64_t account_generation = 0;
    if (!compose_account_choice_identity(window->from_choice_accounts_, selected,
            account_id, account_generation)) {
        window->status_->text(T("The selected sender account is no longer available."));
        window->update_from_label();
        return;
    }
    if (!window->accounts_.clone_from(window->owner_->startup_.accounts)) {
        window->from_choice_->deactivate();
        window->status_->text(T("The sender account list could not be refreshed."));
        return;
    }
    if (live_index >= window->accounts_.size() ||
        window->accounts_[live_index].id != account_id ||
        window->accounts_[live_index].generation != account_generation) {
        window->status_->text(T("The selected sender account is no longer available."));
        window->update_from_label();
        return;
    }
    window->update_from_label();
    window->change_account_identity(account_id, account_generation);
}

void ComposeWindow::ribbon_tab_cb(Fl_Widget *widget, void *data) {
    ComposeWindow *window = static_cast<ComposeWindow *>(widget->window());
    if (!window) return;
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(data));
    window->show_ribbon_tab(index);
}

void ComposeWindow::template_cb(Fl_Widget *, void *context) {
    ComposeWindow *window = static_cast<ComposeWindow *>(context);
    if (!window || window->session_stale_ || window->copy_pending_ || window->send_in_progress_ ||
        window->queue_pending_ || window->archive_retry_pending_) return;
    const int selected = window->template_choice_->value();
    if (selected < 0 || static_cast<size_t>(selected) >= window->compose_catalog_.templates.size()) return;
    const uint64_t id = window->compose_catalog_.templates[static_cast<size_t>(selected)].id;
    (void)window->request_compose_catalog(Text(), id);
}

void ComposeWindow::contact_cb(Fl_Widget *, void *context) {
    ComposeWindow *window = static_cast<ComposeWindow *>(context);
    if (!window || window->session_stale_ || window->copy_pending_ || window->send_in_progress_ ||
        window->queue_pending_ || window->archive_retry_pending_) return;
    window->insert_contact_choice();
}

void ComposeWindow::autosave_cb(void *context) {
    ComposeWindow *window = static_cast<ComposeWindow *>(context);
    if (!window->shown() || window->session_stale_ || window->copy_pending_ ||
        window->archive_retry_pending_ || !window->save_gate_.dirty()) return;
    if (window->save_gate_.pending()) return;
    (void)window->submit_save();
}

}
