#ifndef RELIEFMAIL_READER_H
#define RELIEFMAIL_READER_H

#include "tasks.h"
#include "ui_widgets.h"
#include <FL/Fl_Choice.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Help_View.H>

namespace rmail {

struct ReaderActions {
    void (*fetch)(void *context) = nullptr;
    void (*source)(void *context) = nullptr;
    void (*preview_original)(void *context) = nullptr;
    void (*save_attachment)(void *context, uint32_t index) = nullptr;
    void (*open_link)(void *context, const char *url, size_t length) = nullptr;
    void *context = nullptr;
};

class ReaderPane : public Fl_Group {
    PlainTextBox *subject_ = nullptr;
    PlainTextBox *details_ = nullptr;
    PlainTextBox *notice_ = nullptr;
    PlainTextBox *empty_state_ = nullptr;
    PlainTextBox *link_target_ = nullptr;
    PlainTextBox *attachment_name_ = nullptr;
    PlainTextBox *view_label_ = nullptr;
    PlainTextBox *attachment_label_ = nullptr;
    PlainTextBox *link_label_ = nullptr;
    Fl_Choice *view_choice_ = nullptr;
    Fl_Choice *attachment_choice_ = nullptr;
    Fl_Choice *link_choice_ = nullptr;
    Fl_Text_Display *body_ = nullptr;
    Fl_Help_View *html_body_ = nullptr;
    Fl_Text_Buffer body_buffer_;
    Fl_Button *fetch_button_ = nullptr;
    Fl_Button *source_button_ = nullptr;
    Fl_Button *save_button_ = nullptr;
    Fl_Button *open_link_button_ = nullptr;
    ReaderActions actions_{};
    WorkerReaderResult message_;
    BoundedArray<Text, 64> safe_links_;
    BoundedArray<Text, 64> link_labels_;
    BoundedArray<Text, MaxMimeParts> attachment_labels_;
    BoundedArray<Text, MaxReaderInlineImages> html_resource_names_;
    BoundedArray<Text, MaxReaderInlineImages> html_resource_paths_;
    Text html_resource_directory_;
    int html_resource_directory_fd_ = -1;
    Text plain_text_;
    Text html_text_;
    Text original_text_;
    bool has_message_ = false;
    bool fetch_available_ = false;
    bool source_incomplete_ = false;
    bool html_resources_ready_ = true;

    static void view_cb(Fl_Widget *, void *);
    static void fetch_cb(Fl_Widget *, void *);
    static void source_cb(Fl_Widget *, void *);
    static void save_cb(Fl_Widget *, void *);
    static void attachment_choice_cb(Fl_Widget *, void *);
    static void link_choice_cb(Fl_Widget *, void *);
    static void link_cb(Fl_Widget *, void *);
    static const char *html_resource_cb(Fl_Widget *, const char *);
    void update_notice();
    void refresh_body();
    void show_link_target();
    void show_attachment_name();
    void layout_children();
    void release_html_resources();
    bool materialize_html_resources();
    bool set_display_text(const char *data, size_t size);

public:
    ReaderPane(int x, int y, int width, int height, ReaderActions actions = {});
    ~ReaderPane() override;
    void resize(int x, int y, int width, int height) override;
    void clear_message(const char *status);
    void set_message(WorkerReaderResult &&message);
    void set_original(const unsigned char *bytes, size_t size, bool incomplete);
    void set_fetch_available(bool available);
    bool has_message() const { return has_message_; }
    const WorkerReaderResult &message() const { return message_; }
    int selected_attachment() const;
    int selected_link() const;
};

}

#endif
