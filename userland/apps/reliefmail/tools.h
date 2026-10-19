#ifndef RELIEFMAIL_TOOLS_H
#define RELIEFMAIL_TOOLS_H

#include "tasks.h"
#include "ui_widgets.h"
#include <FL/Fl_Box.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Scroll.H>

namespace rmail {

class MailWindow;

enum class MailToolsCommand : uint8_t {
    RefreshFolders, SubscribeFolder, UnsubscribeFolder, CreateFolder, RenameFolder,
    DeleteFolder, ClearTrash, RefreshQueue, PreviousQueuePage, NextQueuePage,
    RetryArchive, ReviewArchiveTarget, ConfirmDelivered, ConfirmNotDelivered,
    SendQueued, CancelQueuedSend
};

/* Native Office-style folder and delivery-review surface. All mutations are
 * routed through MailWindow's typed worker requests; this class holds only
 * bounded display snapshots and stable row identities. */
class MailToolsWindow : public Fl_Double_Window {
    MailWindow *owner_ = nullptr;
    Fl_Button *tabs_[2] = {nullptr, nullptr};
    Fl_Group *pages_[2] = {nullptr, nullptr};
    Fl_Scroll *ribbon_scroll_ = nullptr;
    Fl_Box *account_label_ = nullptr;
    Fl_Box *archive_target_label_ = nullptr;
    Fl_Choice *account_choice_ = nullptr;
    Fl_Choice *archive_target_choice_ = nullptr;
    Fl_Scroll *folder_scroll_ = nullptr;
    Fl_Scroll *queue_scroll_ = nullptr;
    Fl_Scroll *detail_scroll_ = nullptr;
    PlainTextBox *detail_ = nullptr;
    PlainTextBox *status_ = nullptr;
    Fl_Button *folder_refresh_ = nullptr;
    Fl_Button *subscribe_ = nullptr;
    Fl_Button *unsubscribe_ = nullptr;
    Fl_Button *create_folder_ = nullptr;
    Fl_Button *rename_folder_ = nullptr;
    Fl_Button *delete_folder_ = nullptr;
    Fl_Button *clear_trash_ = nullptr;
    Fl_Button *queue_refresh_ = nullptr;
    Fl_Button *queue_previous_ = nullptr;
    Fl_Button *queue_next_ = nullptr;
    Fl_Button *archive_retry_ = nullptr;
    Fl_Button *archive_review_ = nullptr;
    Fl_Button *delivered_ = nullptr;
    Fl_Button *not_delivered_ = nullptr;
    Fl_Button *send_queued_ = nullptr;
    Fl_Button *cancel_queued_ = nullptr;
    AccountList accounts_;
    BoundedArray<uint64_t, MaxAccounts> account_choice_ids_;
    FolderList folders_;
    SendOperationPage send_page_;
    BoundedArray<NavigationButton *, MaxFoldersPerAccount> folder_rows_;
    BoundedArray<PlainTextButton *, MaxSendOperationPageSize> queue_rows_;
    BoundedArray<FolderKey, MaxFoldersPerAccount> archive_targets_;
    FolderKey selected_folder_key_;
    uint64_t selected_account_id_ = 0;
    uint64_t selected_operation_id_ = 0;
    uint64_t queue_offset_ = 0;
    bool queue_page_loaded_ = false;
    bool busy_ = false;
    int active_page_ = 0;

    static void tab_cb(Fl_Widget *, void *);
    static void command_cb(Fl_Widget *, void *);
    static void account_cb(Fl_Widget *, void *);
    static void folder_cb(Fl_Widget *, void *);
    static void operation_cb(Fl_Widget *, void *);
    void show_page(int page);
    void render_folders();
    void render_queue();
    void render_detail();
    void update_controls();

public:
    explicit MailToolsWindow(MailWindow *owner);
    void resize(int x, int y, int width, int height) override;
    void refresh_snapshot(const WorkerStartupSnapshot &snapshot, uint64_t selected_account_id);
    void set_send_page(const WorkerSendOperationPage &page);
    void set_status(const char *text);
    void set_busy(bool busy);
    void select_page(int page) { show_page(page); }

    uint64_t selected_account_id() const { return selected_account_id_; }
    uint64_t selected_operation_id() const { return selected_operation_id_; }
    uint64_t queue_offset() const { return queue_offset_; }
    bool queue_page_loaded() const { return queue_page_loaded_; }
    bool queue_has_more() const { return send_page_.has_more; }
    const Folder *selected_folder() const;
    const Folder *selected_archive_target() const;
    const SendOperationSummary *selected_operation() const;
    const Account *selected_account() const;
};

}

#endif
