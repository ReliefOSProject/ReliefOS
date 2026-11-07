#ifndef RELIEFMAIL_COMPOSE_WINDOW_H
#define RELIEFMAIL_COMPOSE_WINDOW_H

#include "compose_state.h"
#include "compose_header_layout.h"
#include "editor.h"
#include "ui_widgets.h"
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Scroll.H>

namespace rmail {

class MailWindow;
class ComposeCanvas;
class AttachmentListView;
class ComposeRecipientInput;

class ComposeWindow : public Fl_Double_Window {
    friend class ComposeCanvas;
    friend class AttachmentListView;
    friend class ComposeRecipientInput;
    MailWindow *owner_ = nullptr;
    Draft draft_;
    MessageKey forward_source_;
    Account account_;
    AccountList accounts_;
    ComposeAccountChoiceMap from_choice_accounts_;
    MailEditor editor_;
    ComposeSaveGate save_gate_;
    WorkerDraftAttachmentRefs attachment_refs_;
    WorkerAttachmentMetadataList attachment_metadata_;
    WorkerComposeCatalog compose_catalog_;
    WorkerComposeContactSuggestions contact_suggestions_;
    size_t selected_attachment_ = 0;
    Fl_Group *top_ = nullptr;
    Fl_Scroll *ribbon_scroll_ = nullptr;
    Fl_Group *ribbon_group_ = nullptr;
    Fl_Group *ribbon_pages_[3] = {nullptr, nullptr, nullptr};
    Fl_Button *ribbon_tabs_[3] = {nullptr, nullptr, nullptr};
    Fl_Button *quick_undo_ = nullptr;
    Fl_Button *quick_redo_ = nullptr;
    Fl_Input *to_ = nullptr;
    Fl_Input *cc_ = nullptr;
    Fl_Input *bcc_ = nullptr;
    Fl_Button *cc_toggle_ = nullptr;
    Fl_Button *bcc_toggle_ = nullptr;
    Fl_Input *subject_ = nullptr;
    Fl_Input *active_recipient_field_ = nullptr;
    ComposeHeaderVisibility optional_recipient_visibility_;
    Fl_Box *from_caption_ = nullptr;
    Fl_Choice *from_choice_ = nullptr;
    Fl_Box *recipient_captions_[4] = {nullptr, nullptr, nullptr, nullptr};
    AttachmentListView *attachment_list_ = nullptr;
    PlainTextBox *status_ = nullptr;
    Fl_Scroll *body_scroll_ = nullptr;
    ComposeCanvas *canvas_ = nullptr;
    Fl_Choice *font_family_ = nullptr;
    Fl_Choice *font_size_ = nullptr;
    Fl_Choice *template_choice_ = nullptr;
    Fl_Choice *contact_choice_ = nullptr;
    Fl_Button *bold_ = nullptr;
    Fl_Button *italic_ = nullptr;
    Fl_Button *underline_ = nullptr;
    Fl_Button *clear_format_ = nullptr;
    Fl_Button *copy_button_ = nullptr;
    Fl_Button *cut_button_ = nullptr;
    Fl_Button *paste_button_ = nullptr;
    Fl_Button *align_left_ = nullptr;
    Fl_Button *align_center_ = nullptr;
    Fl_Button *align_right_ = nullptr;
    Fl_Button *bullet_list_ = nullptr;
    Fl_Button *number_list_ = nullptr;
    Fl_Button *outdent_ = nullptr;
    Fl_Button *indent_ = nullptr;
    Fl_Button *link_button_ = nullptr;
    Fl_Button *rich_mode_ = nullptr;
    Fl_Button *save_button_ = nullptr;
    Fl_Button *send_button_ = nullptr;
    Fl_Button *queue_button_ = nullptr;
    Fl_Button *retry_archive_button_ = nullptr;
    Fl_Button *signature_button_ = nullptr;
    Fl_Button *contacts_button_ = nullptr;
    Fl_Button *attach_button_ = nullptr;
    Fl_Button *remove_button_ = nullptr;
    uint64_t profile_epoch_ = 0;
    uint64_t operation_id_ = 0;
    uint64_t save_generation_ = 0;
    uint64_t compose_catalog_serial_ = 0;
    bool suppress_callbacks_ = false;
    bool close_after_save_ = false;
    bool application_close_after_save_ = false;
    bool send_after_save_ = false;
    bool session_stale_ = false;
    bool send_in_progress_ = false;
    bool queue_pending_ = false;
    bool copy_pending_ = false;
    bool archive_retry_pending_ = false;
    bool archive_retry_available_ = false;
    bool forward_source_valid_ = false;
    int active_ribbon_tab_ = 0;

    static void close_cb(Fl_Widget *, void *);
    static void action_cb(Fl_Widget *, void *);
    static void field_cb(Fl_Widget *, void *);
    static void optional_recipient_cb(Fl_Widget *, void *);
    static void account_cb(Fl_Widget *, void *);
    static void format_cb(Fl_Widget *, void *);
    static void template_cb(Fl_Widget *, void *);
    static void contact_cb(Fl_Widget *, void *);
    static void ribbon_tab_cb(Fl_Widget *, void *);
    static void autosave_cb(void *);
    void build_widgets();
    void layout_widgets();
    void update_from_label();
    void show_ribbon_tab(int index);
    void change_account(size_t index);
    void change_account_identity(uint64_t account_id, uint64_t account_generation);
    void update_format_controls();
    void update_optional_recipient_controls();
    void toggle_optional_recipient(bool cc);
    void set_interaction_enabled(bool enabled);
    void changed(bool body_change = false);
    bool sync_fields(Text &error);
    bool schedule_save(bool explicit_save);
    bool submit_save();
    bool submit_copy(const Account &target);
    bool submit_archive_retry();
    bool request_compose_catalog(const Text &query, uint64_t template_id = 0);
    void insert_contact_choice();
    bool submit_queue_or_send(bool transmit);
    bool submit_send(uint64_t operation_id);
    bool update_attachment_summary();
    bool current_account(Account &account, Text &error) const;
    void apply_font_family();
    void apply_font_size();
    void apply_style(unsigned mask, const rw::TextStyle &style);
    void apply_paragraph(rw::ParagraphStyle style, unsigned mask);
    void finish_close();

public:
    enum class Action : uint8_t {
        Save, Send, Queue, Attach, RemoveAttachment, Copy, Cut, Paste,
        Bold, Italic, Underline, ClearFormat,
        Color, AlignLeft, AlignCenter, AlignRight, BulletList, NumberList,
        Indent, Outdent, Link, Undo, Redo, Find, SelectAll, PlainMode, NextAccount,
        PreviousAccount, RetryArchive, InsertSignature, SearchContacts
    };

    explicit ComposeWindow(MailWindow *owner);
    ~ComposeWindow() override;
    bool open_new(const AccountList &accounts, uint64_t account_id, uint64_t profile_epoch);
    bool open_existing(const AccountList &accounts, const Account &account,
        WorkerDraftSnapshot snapshot, uint64_t profile_epoch);
    bool open_prefilled(const AccountList &accounts, const Account &account,
        Draft draft, uint64_t profile_epoch, const MessageKey *forward_source = nullptr);
    void resize(int, int, int, int) override;
        bool close_for_application(Text &reason);
    void profile_transition();
    void save_result(const ComposeSaveToken &token, bool succeeded,
        const WorkerDraftSnapshot *snapshot, const char *diagnostic);
    void copy_result(bool succeeded, WorkerDraftSnapshot *snapshot, const Account &target,
        const Account &source, uint64_t source_id, uint64_t source_revision, const char *diagnostic);
    void archive_retry_result(bool succeeded, const WorkerOperationResult *operation,
        const char *diagnostic);
    void compose_catalog_result(bool succeeded, WorkerComposeCatalog *catalog,
        const Account &account, uint64_t serial, uint64_t template_id, const Text &query,
        const char *diagnostic);
    void queue_result(bool succeeded, const WorkerOperationResult *operation,
        const char *diagnostic, bool transmit);
    void send_result(bool succeeded, const WorkerOperationResult *operation,
        const char *diagnostic);
    MailEditor &editor() { return editor_; }
    void canvas_changed();
    void canvas_status(const char *message);
    void paste_text(const char *text, size_t length);
    bool copy_selection(ComposeClipboardOperation operation);
    void recipient_focus(Fl_Input *input) { active_recipient_field_ = input; }
};

bool choose_compose_draft(const WorkerDraftList &drafts, WorkerDraftSummary &selected);

}

#endif
