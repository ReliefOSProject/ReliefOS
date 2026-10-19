#include "reader.h"
#include "mail_ui_layout.h"
#include "reader_presentation.h"
#include "reader_render.h"
#include "ui_adapters.h"
#include "../fltk_support.h"
#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace rmail {
namespace {
constexpr size_t ReaderDisplayLimit = MaxReaderPlainPreviewBytes;

const char *translate_reader_notice(const char *message) {
    return T(message);
}

bool ascii_equal(const char *left, const char *right, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        unsigned char a = static_cast<unsigned char>(left[i]);
        unsigned char b = static_cast<unsigned char>(right[i]);
        if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = static_cast<unsigned char>(b + ('a' - 'A'));
        if (a != b) return false;
    }
    return true;
}

class TextBuilder {
    char *data_ = static_cast<char *>(malloc(ReaderDisplayLimit + 1));
    size_t size_ = 0;

public:
    ~TextBuilder() { free(data_); }
    bool valid() const { return data_ != nullptr; }
    bool append(const char *bytes, size_t length) {
        if (!data_ || size_ > ReaderDisplayLimit || length > ReaderDisplayLimit - size_) return false;
        if (length) memcpy(data_ + size_, bytes, length);
        size_ += length;
        return true;
    }
    bool finish(Text &text) {
        if (!data_) return false;
        data_[size_] = 0;
        return text.set(data_, size_);
    }
    size_t size() const { return size_; }
    bool append_char(char value) { return append(&value, 1); }
    bool finish_with_prefix(Text &text, const char *prefix) {
        const size_t prefix_size = strlen(prefix);
        if (prefix_size > ReaderDisplayLimit - size_) return false;
        memmove(data_ + prefix_size, data_, size_);
        memcpy(data_, prefix, prefix_size);
        size_ += prefix_size;
        return finish(text);
    }
    bool append_text(const Text &text) { return append(text.c_str(), text.size()); }
    bool append_static(const char *text) { return append(text, strlen(text)); }
    bool append_text_bounded(const Text &text, size_t size) {
        if (size > text.size()) size = text.size();
        return append(text.c_str(), size);
    }
};

bool parse_opaque_index(const char *uri, const char *prefix, size_t count, size_t &index) {
    if (!uri || !prefix) return false;
    const size_t prefix_size = strlen(prefix);
    if (strncmp(uri, prefix, prefix_size)) return false;
    const char *digits = uri + prefix_size;
    if (!*digits) return false;
    size_t value = 0;
    for (const char *at = digits; *at; ++at) {
        if (*at < '0' || *at > '9') return false;
        const size_t digit = static_cast<size_t>(*at - '0');
        if (value > (SIZE_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    if (value >= count) return false;
    index = value;
    return true;
}

bool write_all(int fd, const char *bytes, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        const ssize_t count = write(fd, bytes + offset, size - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        offset += static_cast<size_t>(count);
    }
    return true;
}

int reader_label_slot_width() {
    const Fl_Font previous_font = fl_font();
    const int previous_size = fl_size();
    fl_font(FL_HELVETICA, 15);
    const char *labels[] = {T("View"), T("Attachment"), T("Link")};
    double widest = 0;
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i) {
        const double width = fl_width(labels[i]);
        if (width > widest) widest = width;
    }
    fl_font(previous_font, previous_size);
    return static_cast<int>(widest + 0.5) + 10;
}

void style_reader_field_label(PlainTextBox *label) {
    if (!label) return;
    label->labelfont(FL_HELVETICA);
    label->labelsize(12);
    label->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
}

bool html_text(const Text &html, Text &plain) {
    TextBuilder output;
    if (!output.valid()) return false;
    size_t at = 0;
    while (at < html.size() && output.size() < ReaderDisplayLimit) {
        if (html.c_str()[at] == '<') {
            size_t end = at + 1;
            while (end < html.size() && html.c_str()[end] != '>') ++end;
            if (end == html.size()) break;
            size_t name = at + 1;
            if (name < end && html.c_str()[name] == '/') ++name;
            size_t name_end = name;
            while (name_end < end && ((html.c_str()[name_end] >= 'a' && html.c_str()[name_end] <= 'z') ||
                (html.c_str()[name_end] >= 'A' && html.c_str()[name_end] <= 'Z'))) ++name_end;
            const size_t n = name_end - name;
            if ((n == 1 && ascii_equal(html.c_str() + name, "p", 1)) ||
                (n == 2 && (ascii_equal(html.c_str() + name, "br", 2) || ascii_equal(html.c_str() + name, "li", 2))) ||
                (n == 3 && ascii_equal(html.c_str() + name, "div", 3))) {
                const char newline = '\n';
                if (!output.append_char(newline)) return false;
            }
            at = end + 1;
            continue;
        }
        if (html.c_str()[at] == '&') {
            const struct Entity { const char *name; const char *value; size_t size; } entities[] = {
                {"&amp;", "&", 1}, {"&lt;", "<", 1}, {"&gt;", ">", 1},
                {"&quot;", "\"", 1}, {"&#39;", "'", 1}, {"&nbsp;", " ", 1}
            };
            bool decoded = false;
            for (size_t i = 0; i < sizeof(entities) / sizeof(entities[0]); ++i) {
                const size_t name_size = strlen(entities[i].name);
                if (name_size <= html.size() - at && !memcmp(html.c_str() + at, entities[i].name, name_size)) {
                    if (!output.append(entities[i].value, entities[i].size)) return false;
                    at += name_size;
                    decoded = true;
                    break;
                }
            }
            if (decoded) continue;
        }
        const char value = html.c_str()[at++];
        if (!output.append_char(value)) return false;
    }
    return output.finish(plain);
}

}

ReaderPane::ReaderPane(int x, int y, int width, int height, ReaderActions actions)
    : Fl_Group(x, y, width, height), actions_(actions) {
    box(FL_BORDER_BOX);
    color(FL_BACKGROUND_COLOR);
    begin();
    subject_ = new PlainTextBox(x + 16, y + 12, width - 32, 30);
    subject_->labelfont(FL_HELVETICA_BOLD);
    subject_->labelsize(20);
    subject_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    details_ = new PlainTextBox(x + 18, y + 44, width - 36, 38);
    details_->labelsize(12);
    details_->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    notice_ = new PlainTextBox(x + 18, y + 82, width - 36, 26);
    notice_->labelsize(11);
    notice_->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    empty_state_ = new PlainTextBox(x + 22, y + 180, width - 44, 72);
    empty_state_->labelsize(13);
    empty_state_->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    view_label_ = new PlainTextBox(x + 8, y + 116, reader_label_slot_width(), 27, T("View"));
    style_reader_field_label(view_label_);
    view_choice_ = new Fl_Choice(x + 52, y + 116, 126, 27);
    view_choice_->add(T("Plain text"));
    view_choice_->add(T("Safe HTML"));
    view_choice_->add(T("Headers"));
    view_choice_->add(T("Original preview"));
    view_choice_->value(0);
    view_choice_->callback(view_cb, this);
    fetch_button_ = new Fl_Button(x + 182, y + 116, 96, 27, T("Fetch body"));
    fetch_button_->callback(fetch_cb, this);
    source_button_ = new Fl_Button(x + 282, y + 116, 132, 27, T("Save original..."));
    source_button_->callback(source_cb, this);
    attachment_label_ = new PlainTextBox(x + 8, y + 148, reader_label_slot_width(), 27, T("Attachment"));
    style_reader_field_label(attachment_label_);
    attachment_choice_ = new Fl_Choice(x + 52, y + 148, 220, 27);
    attachment_choice_->callback(attachment_choice_cb, this);
    save_button_ = new Fl_Button(x + 278, y + 148, 128, 27, T("Save attachment..."));
    save_button_->callback(save_cb, this);
    attachment_name_ = new PlainTextBox(x + 18, y + 178, width - 36, 22);
    attachment_name_->labelsize(11);
    attachment_name_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    link_label_ = new PlainTextBox(x + 8, y + 204, reader_label_slot_width(), 27, T("Link"));
    style_reader_field_label(link_label_);
    link_choice_ = new Fl_Choice(x + 52, y + 204, 220, 27);
    open_link_button_ = new Fl_Button(x + 278, y + 204, 128, 27, T("Open link..."));
    open_link_button_->callback(link_cb, this);
    link_target_ = new PlainTextBox(x + 18, y + 234, width - 36, 24);
    link_target_->labelsize(11);
    link_target_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    body_ = new Fl_Text_Display(x + 12, y + 270, width - 24, height - 282);
    body_->buffer(&body_buffer_);
    body_->textfont(20); // Registered SimSun regular font.
    body_->textsize(14);
    body_->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
    body_->color(FL_BACKGROUND2_COLOR);
    html_body_ = new Fl_Help_View(x + 12, y + 270, width - 24, height - 282);
    html_body_->textfont(20);
    html_body_->textsize(14);
    html_body_->color(FL_BACKGROUND2_COLOR);
    html_body_->link(html_resource_cb);
    html_body_->hide();
    empty_state_->hide();
    notice_->hide();
    view_label_->hide();
    view_choice_->hide();
    fetch_button_->hide();
    source_button_->hide();
    attachment_label_->hide();
    attachment_choice_->hide();
    attachment_name_->hide();
    save_button_->hide();
    link_label_->hide();
    link_choice_->hide();
    link_target_->hide();
    open_link_button_->hide();
    end();
    layout_children();
    set_fetch_available(false);
    source_button_->deactivate();
    save_button_->deactivate();
    open_link_button_->deactivate();
    clear_message(T("Select a message to read its cached content."));
}

ReaderPane::~ReaderPane() {
    release_html_resources();
}

void ReaderPane::view_cb(Fl_Widget *, void *context) {
    ReaderPane *reader = static_cast<ReaderPane *>(context);
    if (reader->view_choice_->value() == 3 && reader->original_text_.empty() &&
        reader->actions_.preview_original) {
        reader->actions_.preview_original(reader->actions_.context);
        return;
    }
    reader->refresh_body();
}
void ReaderPane::fetch_cb(Fl_Widget *, void *context) {
    ReaderPane *reader = static_cast<ReaderPane *>(context);
    if (reader->actions_.fetch) reader->actions_.fetch(reader->actions_.context);
}
void ReaderPane::source_cb(Fl_Widget *, void *context) {
    ReaderPane *reader = static_cast<ReaderPane *>(context);
    if (reader->actions_.source) reader->actions_.source(reader->actions_.context);
}
void ReaderPane::save_cb(Fl_Widget *, void *context) {
    ReaderPane *reader = static_cast<ReaderPane *>(context);
    const int selected = reader->selected_attachment();
    if (selected >= 0 && reader->actions_.save_attachment)
        reader->actions_.save_attachment(reader->actions_.context, static_cast<uint32_t>(selected));
}
void ReaderPane::attachment_choice_cb(Fl_Widget *, void *context) {
    static_cast<ReaderPane *>(context)->show_attachment_name();
}
void ReaderPane::link_cb(Fl_Widget *, void *context) {
    ReaderPane *reader = static_cast<ReaderPane *>(context);
    const int selected = reader->selected_link();
    if (selected >= 0 && static_cast<size_t>(selected) < reader->safe_links_.size() && reader->actions_.open_link)
        reader->actions_.open_link(reader->actions_.context, reader->safe_links_[static_cast<size_t>(selected)].c_str(),
            reader->safe_links_[static_cast<size_t>(selected)].size());
}

const char *ReaderPane::html_resource_cb(Fl_Widget *widget, const char *uri) {
    if (!widget || !widget->parent()) return nullptr;
    ReaderPane *reader = static_cast<ReaderPane *>(widget->parent());
    size_t index = 0;
    if (parse_opaque_index(uri, "rmail-link://", reader->safe_links_.size(), index)) {
        if (reader->actions_.open_link)
            reader->actions_.open_link(reader->actions_.context,
                reader->safe_links_[index].c_str(), reader->safe_links_[index].size());
        return nullptr;
    }
    if (parse_opaque_index(uri, "rmail-cid://", reader->html_resource_paths_.size(), index))
        return reader->html_resource_paths_[index].c_str();
    return nullptr;
}

void ReaderPane::link_choice_cb(Fl_Widget *, void *context) {
    static_cast<ReaderPane *>(context)->show_link_target();
}

void ReaderPane::show_link_target() {
    const int selected = selected_link();
    if (selected >= 0 && static_cast<size_t>(selected) < safe_links_.size())
        link_target_->text(safe_links_[static_cast<size_t>(selected)].c_str());
    else link_target_->text("");
}

void ReaderPane::show_attachment_name() {
    const int selected = selected_attachment();
    if (selected < 0 || static_cast<size_t>(selected) >= message_.attachments.size()) {
        attachment_name_->text("");
        return;
    }
    const WorkerAttachmentMetadata &attachment = message_.attachments[static_cast<size_t>(selected)];
    Text filename;
    if (!task6_sanitize_single_line_display_text(attachment.filename.c_str(), attachment.filename.size(), filename)) {
        attachment_name_->text(T("Attachment name cannot be displayed safely."));
        return;
    }
    char label[4352];
    snprintf(label, sizeof(label), T("%s (%llu bytes)"), filename.c_str(),
        static_cast<unsigned long long>(attachment.size));
    attachment_name_->text(label);
}

void ReaderPane::release_html_resources() {
    if (html_body_) html_body_->value("");
    if (html_resource_directory_fd_ >= 0) {
        for (size_t i = 0; i < html_resource_names_.size(); ++i)
            (void)unlinkat(html_resource_directory_fd_, html_resource_names_[i].c_str(), 0);
        (void)close(html_resource_directory_fd_);
        html_resource_directory_fd_ = -1;
    }
    if (!html_resource_directory_.empty()) (void)rmdir(html_resource_directory_.c_str());
    html_resource_names_.clear();
    html_resource_paths_.clear();
    html_resource_directory_.clear();
}

bool ReaderPane::materialize_html_resources() {
    release_html_resources();
    html_resources_ready_ = true;
    if (message_.inline_images.empty()) return true;

    char directory[] = "/tmp/reliefmail-reader-XXXXXX";
    char *created = mkdtemp(directory);
    if (!created || !html_resource_directory_.set(created)) {
        if (created) (void)rmdir(created);
        html_resources_ready_ = false;
        return false;
    }
    html_resource_directory_fd_ = open(created, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (html_resource_directory_fd_ < 0) {
        (void)rmdir(created);
        html_resource_directory_.clear();
        html_resources_ready_ = false;
        return false;
    }
    for (size_t i = 0; i < message_.inline_images.size(); ++i) {
        const WorkerReaderInlineImage &image = message_.inline_images[i];
        Text validation_error;
        if (!validate_reader_xpm(image.xpm.c_str(), image.xpm.size(), image.width,
                image.height, validation_error)) {
            release_html_resources();
            html_resources_ready_ = false;
            return false;
        }
        char name[32], path[PATH_MAX];
        const int name_size = snprintf(name, sizeof(name), "image-%u.xpm", static_cast<unsigned>(i));
        const int path_size = snprintf(path, sizeof(path), "%s/%s", created, name);
        if (name_size <= 0 || static_cast<size_t>(name_size) >= sizeof(name) ||
            path_size <= 0 || static_cast<size_t>(path_size) >= sizeof(path)) {
            release_html_resources();
            html_resources_ready_ = false;
            return false;
        }
        const int fd = openat(html_resource_directory_fd_, name,
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) {
            release_html_resources();
            html_resources_ready_ = false;
            return false;
        }
        const bool written = write_all(fd, image.xpm.c_str(), image.xpm.size());
        const bool closed = close(fd) == 0;
        if (!written || !closed) {
            (void)unlinkat(html_resource_directory_fd_, name, 0);
            release_html_resources();
            html_resources_ready_ = false;
            return false;
        }
        Text stored_name, stored_path;
        if (!stored_name.set(name) || !stored_path.set(path) ||
            !html_resource_names_.push(static_cast<Text &&>(stored_name)) ||
            !html_resource_paths_.push(static_cast<Text &&>(stored_path))) {
            (void)unlinkat(html_resource_directory_fd_, name, 0);
            release_html_resources();
            html_resources_ready_ = false;
            return false;
        }
    }
    return true;
}

void ReaderPane::layout_children() {
    const int X = x(), Y = y(), W = w(), H = h();
    const int inner_width = W > 24 ? W - 24 : 1;
    const int gap = 6;
    const bool tight = W < 450;
    const int label_width = tight ? 46 : 66;
    const int controls_x = X + 12 + label_width + 6;
    const int row_height = 28;
    int cursor = 108;
    if (!has_message_) {
        empty_state_->resize(X + 28, Y + (H > 88 ? (H - 88) / 2 : 0),
            W > 56 ? W - 56 : 1, H > 88 ? 88 : H);
        return;
    }

    subject_->resize(X + 16, Y + 14, W > 32 ? W - 32 : 1, 30);
    details_->resize(X + 18, Y + 50, inner_width + 4, 54);
    if (notice_->visible()) {
        const int notice_height = plain_text_wrapped_height(notice_->text().c_str(),
            inner_width, 11, 66);
        notice_->resize(X + 18, Y + cursor, inner_width + 4, notice_height);
        cursor += notice_height + 6;
    }
    view_label_->resize(X + 12, Y + cursor, label_width, row_height);
    const int available = W - (controls_x - X) - 12;
    const int inline_view_width = tight ? 90 : (available > 410 ? 132 : 104);
    const int inline_fetch_width = fetch_button_->visible() ? (tight ? 76 : 84) : 0;
    const int inline_source_width = tight ? 100 : 112;
    const bool compact = !reader_action_row_fits_inline(W, controls_x - X,
        inline_view_width, inline_fetch_width, inline_source_width, gap, 12);
    const int view_width = compact ? 76 : inline_view_width;
    const int fetch_width = fetch_button_->visible() ? (compact ? 68 : inline_fetch_width) : 0;
    const int source_width = compact ? 88 : inline_source_width;
    view_choice_->resize(controls_x, Y + cursor, view_width, row_height);
    fetch_button_->resize(controls_x + view_width + gap, Y + cursor, fetch_width, row_height);
    if (compact) {
        cursor += row_height + 4;
        source_button_->resize(X + 12, Y + cursor, W - 24, row_height);
    } else {
        source_button_->resize(controls_x + view_width + gap + fetch_width + gap,
            Y + cursor, source_width, row_height);
    }
    cursor += row_height + 3;

    const int right = X + W - 12;
    const int attachment_control_x = X + 12 + label_width + 6;
    if (attachment_choice_->visible()) {
        const int save_width = compact ? 82 : (tight ? 92 : 112);
        const int attachment_choice_width = right - attachment_control_x - gap - save_width;
        attachment_label_->resize(X + 12, Y + cursor, label_width, row_height);
        attachment_choice_->resize(attachment_control_x, Y + cursor,
            attachment_choice_width > 64 ? attachment_choice_width : 64, row_height);
        save_button_->resize(attachment_control_x + attachment_choice_width + gap,
            Y + cursor, save_width, row_height);
        cursor += row_height + 3;
    }

    if (link_choice_->visible()) {
        const int open_width = compact ? 82 : (tight ? 92 : 112);
        const int link_choice_width = right - attachment_control_x - gap - open_width;
        link_label_->resize(X + 12, Y + cursor, label_width, row_height);
        link_choice_->resize(attachment_control_x, Y + cursor,
            link_choice_width > 64 ? link_choice_width : 64, row_height);
        open_link_button_->resize(attachment_control_x + link_choice_width + gap,
            Y + cursor, open_width, row_height);
        cursor += row_height + 5;
    }

    const int body_top = cursor;
    const int body_height = H > body_top + 4 ? H - body_top - 4 : 0;
    body_->resize(X + 8, Y + body_top, W > 16 ? W - 16 : 1, body_height);
    html_body_->resize(X + 8, Y + body_top, W > 16 ? W - 16 : 1, body_height);
    empty_state_->resize(X + 28, Y + body_top + 8,
        W > 56 ? W - 56 : 1, body_height > 16 ? body_height - 16 : 1);
}

void ReaderPane::resize(int x, int y, int width, int height) {
    Fl_Group::resize(x, y, width, height);
    if (subject_) layout_children();
}

bool ReaderPane::set_display_text(const char *data, size_t size) {
    if (size > ReaderDisplayLimit) size = ReaderDisplayLimit;
    Text safe;
    if (!task6_sanitize_display_text(data, size, safe)) return false;
    body_buffer_.text(safe.c_str());
    return true;
}

void ReaderPane::clear_message(const char *status) {
    release_html_resources();
    has_message_ = false;
    message_ = WorkerReaderResult();
    plain_text_.clear();
    html_text_.clear();
    original_text_.clear();
    html_resources_ready_ = true;
    source_incomplete_ = false;
    fetch_available_ = false;
    safe_links_.clear();
    subject_->text("");
    subject_->hide();
    details_->hide();
    details_->text("");
    notice_->text("");
    body_buffer_.text("");
    body_->hide();
    html_body_->hide();
    empty_state_->text(status ? status : T("Select a message to read its cached content."));
    empty_state_->show();
    notice_->hide();
    view_label_->hide();
    view_choice_->hide();
    fetch_button_->hide();
    source_button_->hide();
    attachment_label_->hide();
    attachment_choice_->clear();
    attachment_choice_->hide();
    attachment_name_->text("");
    attachment_name_->hide();
    save_button_->hide();
    link_label_->hide();
    link_choice_->clear();
    link_choice_->hide();
    link_target_->text("");
    link_target_->hide();
    open_link_button_->hide();
    fetch_button_->deactivate();
    source_button_->deactivate();
    save_button_->deactivate();
    open_link_button_->deactivate();
    layout_children();
    redraw();
}

void ReaderPane::update_notice() {
    char combined[4608];
    size_t used = 0;
    combined[0] = 0;
    auto append = [&](const char *value) {
        if (!value || !value[0] || used >= sizeof(combined) - 1) return;
        const size_t available = sizeof(combined) - used;
        const int count = snprintf(combined + used, available, "%s%s", used ? "  " : "", value);
        if (count < 0) return;
        if (static_cast<size_t>(count) >= available) used = sizeof(combined) - 1;
        else used += static_cast<size_t>(count);
    };
    if (message_.plain_text_truncated)
        append(T("Plain-text preview is truncated; save the original to view the complete message."));
    if (!message_.warning.empty())
        append(reader_notice_for_display(message_.warning.c_str(), translate_reader_notice));
    if (!html_resources_ready_)
        append(T("Inline image preview is unavailable; plain text remains available."));
    else if (message_.html_warning)
        append(T("Some HTML content uses a safe fallback."));
    notice_->text(combined);
    if (combined[0]) notice_->show();
    else notice_->hide();
    layout_children();
}

void ReaderPane::set_message(WorkerReaderResult &&message) {
    release_html_resources();
    message_ = static_cast<WorkerReaderResult &&>(message);
    has_message_ = true;
    plain_text_ = message_.plain_text;
    html_text_ = message_.html_text;
    Text safe_subject, safe_from, safe_to, safe_date;
    if (!task6_sanitize_single_line_display_text(message_.subject.c_str(), message_.subject.size(), safe_subject))
        safe_subject.set(T("Subject unavailable"));
    if (!task6_sanitize_single_line_display_text(message_.from.c_str(), message_.from.size(), safe_from))
        safe_from.set(T("Sender unavailable"));
    if (!task6_sanitize_single_line_display_text(message_.to.c_str(), message_.to.size(), safe_to))
        safe_to.set(T("Recipient unavailable"));
    if (!task6_sanitize_single_line_display_text(message_.date.c_str(), message_.date.size(), safe_date))
        safe_date.set(T("Date unavailable"));
    subject_->text(safe_subject.empty() ? T("(no subject)") : safe_subject.c_str());
    subject_->show();
    details_->show();
    char summary[4096];
    snprintf(summary, sizeof(summary), "%s: %s\n%s: %s\n%s: %s",
        T("From"), safe_from.c_str(), T("To"), safe_to.c_str(), T("Date"), safe_date.c_str());
    details_->text(summary);
    empty_state_->hide();
    view_label_->show();
    view_choice_->show();
    source_button_->show();
    if (fetch_available_) fetch_button_->show();
    if (!materialize_html_resources()) {
        message_.html_warning = true;
    }
    update_notice();
    source_incomplete_ = false;
    source_button_->copy_label(message_.headers_only ? T("Save headers...") : T("Save original..."));
    source_button_->tooltip(message_.headers_only ? T("Save the cached message headers") :
        T("Export the complete raw message without truncating the display preview"));
    source_button_->activate();
    attachment_choice_->clear();
    attachment_labels_.clear();
    for (size_t i = 0; i < message_.attachments.size(); ++i) {
        const WorkerAttachmentMetadata &attachment = message_.attachments[i];
        Text filename;
        char item[4352];
        if (task6_sanitize_single_line_display_text(attachment.filename.c_str(),
                attachment.filename.size(), filename) && !filename.empty()) {
            snprintf(item, sizeof(item), T("%s %u: %s (%llu bytes)"), T("Attachment"),
                static_cast<unsigned>(i + 1), filename.c_str(),
                static_cast<unsigned long long>(attachment.size));
        } else {
            snprintf(item, sizeof(item), T("%s %u"), T("Attachment"),
                static_cast<unsigned>(i + 1));
        }
        Text label, menu_label;
        if (!label.set(item) || !task6_escape_fltk_label_symbols(label.c_str(), label.size(), menu_label))
            continue;
        label = static_cast<Text &&>(menu_label);
        if (attachment_labels_.push(static_cast<Text &&>(label)))
            attachment_choice_->add(attachment_labels_[attachment_labels_.size() - 1].c_str());
    }
    if (message_.attachments.size()) {
        attachment_label_->show();
        attachment_choice_->show();
        attachment_name_->hide();
        save_button_->show();
        attachment_choice_->value(0);
        show_attachment_name();
        save_button_->activate();
    } else {
        attachment_label_->hide();
        attachment_choice_->hide();
        attachment_name_->text("");
        attachment_name_->hide();
        save_button_->hide();
        save_button_->deactivate();
    }
    safe_links_.clear();
    for (size_t i = 0; i < message_.safe_links.size(); ++i)
        (void)safe_links_.push(message_.safe_links[i]);
    link_labels_.clear();
    link_choice_->clear();
    link_choice_->callback(static_cast<Fl_Callback *>(nullptr));
    if (!safe_links_.empty()) {
        for (size_t i = 0; i < safe_links_.size(); ++i) {
            char item[40];
            snprintf(item, sizeof(item), "%s %u", T("Link"), static_cast<unsigned>(i + 1));
            Text label;
            if (label.set(item) && link_labels_.push(static_cast<Text &&>(label)))
                link_choice_->add(link_labels_[link_labels_.size() - 1].c_str());
        }
    }
    if (safe_links_.size()) {
        link_label_->show();
        link_choice_->show();
        link_target_->hide();
        open_link_button_->show();
        link_choice_->value(0);
        link_choice_->callback(link_choice_cb, this);
        show_link_target();
        open_link_button_->tooltip(T("Open the selected address after reviewing its destination"));
        open_link_button_->activate();
    } else {
        link_label_->hide();
        link_choice_->hide();
        link_target_->text("");
        link_target_->hide();
        open_link_button_->hide();
        open_link_button_->deactivate();
    }
    view_choice_->value(0);
    layout_children();
    refresh_body();
}

void ReaderPane::set_original(const unsigned char *bytes, size_t size, bool incomplete) {
    original_text_.clear();
    if (!original_text_.set(reinterpret_cast<const char *>(bytes),
            size > 64u * 1024u ? 64u * 1024u : size)) {
        notice_->text(T("Could not allocate the bounded original preview."));
        notice_->show();
        layout_children();
        return;
    }
    source_incomplete_ = incomplete;
    view_choice_->value(3);
    refresh_body();
}

void ReaderPane::set_fetch_available(bool available) {
    fetch_available_ = available;
    if (available && has_message_) {
        fetch_button_->show();
        fetch_button_->activate();
    } else {
        fetch_button_->hide();
        fetch_button_->deactivate();
    }
    layout_children();
}

int ReaderPane::selected_attachment() const { return attachment_choice_->value(); }
int ReaderPane::selected_link() const { return link_choice_->value(); }

void ReaderPane::refresh_body() {
    Text display;
    const int mode = view_choice_->value();
    if (mode == 1 && !html_text_.empty() && html_resources_ready_) {
        empty_state_->hide();
        body_->hide();
        html_body_->show();
        html_body_->value(html_text_.c_str());
        return;
    }
    html_body_->hide();
    const auto show_empty = [&](const char *message) {
        body_->hide();
        empty_state_->text(message);
        empty_state_->show();
        layout_children();
    };
    if (mode == 0 && plain_text_.empty() && html_text_.empty()) {
        show_empty(message_.headers_only ? T("This message is stored as headers only.\nFetch the complete message to read it.") :
            T("This message has no cached body text."));
        return;
    }
    if (mode == 1 && html_text_.empty()) {
        show_empty(T("No safe HTML preview is available. Choose Plain text to read this message."));
        return;
    }
    if (mode == 3 && original_text_.empty()) {
        show_empty(T("The bounded original preview is not loaded. Use Save original to export the complete message."));
        return;
    }
    empty_state_->hide();
    body_->show();
    if (mode == 0) {
        if (!plain_text_.empty()) display = plain_text_;
        else if (!html_text_.empty()) (void)html_text(html_text_, display);
    } else if (mode == 1) {
        if (!html_text_.empty()) (void)html_text(html_text_, display);
    } else if (mode == 2) {
        char headers[8192];
        snprintf(headers, sizeof(headers), "From: %s\nTo: %s\nCc: %s\nBcc: %s\nDate: %s\nSubject: %s\nMessage-ID: %s\nIn-Reply-To: %s\nReferences: %s\n\n%s",
            message_.from.c_str(), message_.to.c_str(), message_.cc.c_str(), message_.bcc.c_str(),
            message_.date.c_str(), message_.subject.c_str(), message_.message_id.c_str(),
            message_.in_reply_to.c_str(), message_.references.c_str(),
            message_.headers_only ? T("Headers only are cached; this is not a complete original message.") : "");
        display.set(headers);
    } else {
        display = original_text_;
        if (source_incomplete_) {
            TextBuilder prefixed;
            if (prefixed.valid() && prefixed.append_text_bounded(display, display.size()))
                (void)prefixed.finish_with_prefix(display,
                    T("Showing the first 64 KiB; use Save original to export the full message.\n\n"));
        }
    }
    if (display.size() > ReaderDisplayLimit) {
        TextBuilder truncated;
        if (truncated.valid() && truncated.append_text_bounded(display, ReaderDisplayLimit - 40) &&
            truncated.append_static("\n\n[Preview truncated at 256 KiB]"))
            (void)truncated.finish(display);
    }
    (void)set_display_text(display.c_str(), display.size());
}

}
