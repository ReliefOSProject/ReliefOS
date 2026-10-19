#ifndef RELIEFMAIL_WINDOW_H
#define RELIEFMAIL_WINDOW_H

#include "dialogs.h"
#include "compose_state.h"
#include "reader.h"
#include "tasks.h"
#include "tools.h"
#include "ui_adapters.h"
#include "ui_widgets.h"
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Choice.H>
#include <sys/types.h>

namespace rmail {

class PaneDivider;
class ComposeWindow;

enum class UiCommand : uint8_t {
    AddAccount, ManageAccounts, RefreshFolders, SyncMessages, Cancel,
    TogglePreview, HorizontalPreview, VerticalPreview, About, OpenTools,
    Vault, Search, More, Compose, Drafts, Reply, ReplyAll, Forward,
    ForwardAsAttachment, MarkRead, MarkUnread, StarMessage, UnstarMessage,
    ArchiveMessage, JunkMessage, TrashMessage, MoveMessage, CopyMessage,
    DeleteMessage, Quit, Count
};

/* Typed, narrow seams owned by later task slices. A missing handler is
 * reported as unavailable instead of pretending the action succeeded. */
struct UiExtensionHooks {
    void (*compose)(void *context) = nullptr;
    void (*open_tools)(void *context) = nullptr;
    void *context = nullptr;
    Task6ApplicationCloseGuard prepare_close = nullptr;
};

class MailWindow : public Fl_Double_Window {
    friend class PaneDivider;
    friend class ComposeWindow;
    friend class MailToolsWindow;
    enum class PendingPurpose : uint8_t {
        Initialize, VaultStatus, AccountSave, AccountDelete, AccountTestImap,
        AccountTestSmtp, AccountList, FolderList, HeaderSync, Query,
        ParseMessage, FetchMessage, SourcePreview, ExportChunk, UnlockVault,
        SaveVault, ChangeVaultPassword, ComposeSave, ComposeCopy, ComposeQueue, ComposeSend,
        ComposeArchiveRetry, ComposeCatalog,
        ComposeDraftList, ComposeDraftLoad, ToolTask
    };

    struct PendingUiRequest {
        Task6PendingRequest token;
        PendingPurpose purpose = PendingPurpose::Initialize;
        uint64_t selection_generation = 0;
        MessageQueryRow row;
        Account account;
        Folder folder;
        SecretBytes incoming_password;
        SecretBytes outgoing_password;
        SecretBytes master_password;
        uint64_t chunk_offset = 0;
        bool remember_after_unlock = false;
        ComposeWindow *compose = nullptr;
        ComposeSaveToken compose_save_token;
        Account compose_copy_source_account;
        uint64_t compose_copy_source_id = 0;
        uint64_t compose_copy_source_revision = 0;
        uint64_t compose_catalog_serial = 0;
        uint64_t compose_template_id = 0;
        Text compose_catalog_query;
        bool compose_transmit = false;
        uint64_t draft_id = 0;
        uint64_t draft_revision = 0;
        WorkerTaskKind tool_kind = WorkerTaskKind::Initialize;
        WorkerFolderAction tool_folder_action = WorkerFolderAction::List;
        FolderKey tool_folder_key;
        SendOperationSummary tool_operation;
        bool tool_has_operation = false;
        UiCommand tool_ui_command = UiCommand::Count;
        bool tool_window_task = false;
        bool tool_message_batch = false;
        BoundedArray<MessageQueryRow, MaxWorkerOutstanding> tool_action_rows;
        uint64_t tool_action_profile_epoch = 0;
        uint64_t tool_action_ui_generation = 0;
        bool tool_has_action_selection = false;
        Text tool_action_result_diagnostic;
    };

    struct LiveCredential {
        Account account;
        CredentialPurpose purpose = CredentialPurpose::Imap;
        SecretBytes password;
    };

    struct ExportState {
        int fd = -1;
        Text path;
        Text temp_path;
        MessageQueryRow row;
        uint64_t offset = 0;
        uint64_t total_size = 0;
        dev_t target_device = 0;
        ino_t target_inode = 0;
        uint32_t attachment_index = UINT32_MAX;
        bool active = false;
        bool replace_existing = false;
    };

    Fl_Group *top_ = nullptr;
    Fl_Group *ribbon_ = nullptr;
    Fl_Scroll *ribbon_scroll_ = nullptr;
    Fl_Group *search_strip_ = nullptr;
    Fl_Group *ribbon_pages_[4]{};
    Fl_Button *tabs_[4]{};
    Fl_Button *quick_compose_ = nullptr;
    Fl_Button *quick_sync_ = nullptr;
    Fl_Button *compose_button_ = nullptr;
    Fl_Button *reply_button_ = nullptr;
    Fl_Button *reply_all_button_ = nullptr;
    Fl_Button *forward_button_ = nullptr;
    Fl_Button *forward_attachment_button_ = nullptr;
    Fl_Button *drafts_button_ = nullptr;
    Fl_Button *refresh_folders_button_ = nullptr;
    Fl_Button *sync_messages_button_ = nullptr;
    Fl_Button *cancel_button_ = nullptr;
    Fl_Button *account_add_button_ = nullptr;
    Fl_Button *account_manage_button_ = nullptr;
    Fl_Button *vault_button_ = nullptr;
    Fl_Button *open_tools_button_ = nullptr;
    Fl_Button *preview_button_ = nullptr;
    Fl_Button *message_action_buttons_[10]{};
    Fl_Group *left_pane_ = nullptr;
    Fl_Group *list_pane_ = nullptr;
    PlainTextBox *list_heading_ = nullptr;
    NavigationButton *unified_button_ = nullptr;
    NavigationButton *all_mail_button_ = nullptr;
    ReaderPane *reader_pane_ = nullptr;
    PaneDivider *left_divider_ = nullptr;
    PaneDivider *preview_divider_ = nullptr;
    Fl_Group *bottom_ = nullptr;
    Fl_Scroll *account_folder_scroll_ = nullptr;
    Fl_Scroll *message_scroll_ = nullptr;
    PlainTextBox *status_ = nullptr;
    PlainTextBox *selection_status_ = nullptr;
    Fl_Button *search_button_ = nullptr;
    Fl_Input *search_input_ = nullptr;
    Fl_Choice *search_field_ = nullptr;
    Fl_Choice *scope_choice_ = nullptr;
    Fl_Button *filter_button_ = nullptr;
    Fl_Button *more_button_ = nullptr;
    Fl_Check_Button *row_checks_[MaxRowsPerPage]{};
    MailMessageRowButton *row_buttons_[MaxRowsPerPage]{};
    PlainTextBox *empty_rows_ = nullptr;
    size_t rendered_row_count_ = 0;

    WorkerCoordinator coordinator_;
    ComposeWindow *compose_window_ = nullptr;
    MailToolsWindow *tools_window_ = nullptr;
    ProfilePaths paths_;
    WorkerStartupSnapshot startup_;
    WorkerStartupSnapshot action_snapshot_;
    UiExtensionHooks hooks_{};
    Text last_error_;
    BoundedArray<PendingUiRequest, MaxWorkerOutstanding> pending_;
    BoundedArray<LiveCredential, MaxAccounts * 2> credentials_;
    BoundedArray<MessageQueryRow, MaxRowsPerPage> rows_;
    BoundedArray<uint32_t, MaxRowsPerPage> checked_rows_;
    QueryFilterSettings filter_settings_;
    MessageQuerySpec current_query_;
    MessageQueryRow selected_row_;
    WorkerReaderResult selected_message_;
    Folder selected_folder_;
    bool selected_row_valid_ = false;
    bool selected_message_valid_ = false;
    bool selected_folder_valid_ = false;
    ExportState export_;

    uint64_t next_request_id_ = 1;
    uint64_t active_request_id_ = 0;
    uint64_t selected_account_id_ = 0;
    uint64_t selection_generation_ = 1;
    uint64_t ui_generation_ = 1;
    int active_tab_ = 0;
    int left_pane_width_ = 208;
    int list_pane_width_ = 0;
    int vertical_list_height_ = 0;
    bool coordinator_open_ = false;
    bool startup_complete_ = false;
    bool startup_query_waiting_for_vault_ = false;
    bool vault_exists_ = false;
    bool preview_visible_ = true;
    bool vertical_preview_ = false;
    bool all_mail_navigation_selected_ = false;
    bool closing_ = false;
    bool close_complete_ = false;
    bool preserve_selection_on_query_ = false;
    size_t action_batch_pending_ = 0;
    size_t tool_pending_count_ = 0;
    bool action_batch_had_uncertain_ = false;
    Text action_batch_last_diagnostic_;
    Text action_refresh_diagnostic_;

    static void close_cb(Fl_Widget *, void *);
    static void tick_cb(void *);
    static void action_cb(Fl_Widget *, void *);
    static void tab_cb(Fl_Widget *, void *);
    static void navigation_scope_cb(Fl_Widget *, void *);
    static void account_cb(Fl_Widget *, void *);
    static void folder_cb(Fl_Widget *, void *);
    static void message_cb(Fl_Widget *, void *);
    static void checked_cb(Fl_Widget *, void *);
    static void query_cb(Fl_Widget *, void *);
    static void filter_cb(Fl_Widget *, void *);
    static void search_timeout_cb(void *);
    static void account_dialog_cb(void *, AccountSetupDialog &, AccountDialogAction);
    static void reader_fetch_cb(void *);
    static void reader_preview_cb(void *);
    static void reader_export_cb(void *);
    static void reader_attachment_cb(void *, uint32_t);
    static void reader_link_cb(void *, const char *, size_t);

    void build_ribbon();
    void show_tab(int);
    void update_ribbon_state();
    void update_navigation_selection();
    void select_navigation_scope(int);
    void layout_content();
    void layout_message_rows();
    void resize_left_pane(int delta);
    void resize_preview_split(int delta);
    void pump_once();
    void handle_result(WorkerResult &);
    void close_step();
    void perform(UiCommand);
    void perform_tool(MailToolsCommand);
    void tools_account_changed(uint64_t account_id);
    void open_tools_window(int page = 0);
    void run_message_action(UiCommand command);
    void run_message_action(UiCommand command, const FolderKey *preferred_target);
    void run_message_action(UiCommand command, const FolderKey *preferred_target,
        const PendingUiRequest *continuation);
    void finish_message_action_batch();
    void handle_tool_result(PendingUiRequest &pending, WorkerResult &result);
    bool submit_tool_task(WorkerTaskKind kind, WorkerRequest request,
        WorkerTaskInput *input, SecretBytes secret, PendingUiRequest pending, Text &error);
    void refresh_tool_queue(uint64_t account_id, uint64_t offset = 0);
    void submit_tool_folder_operation(WorkerFolderAction action, const Folder *folder,
        const char *query = nullptr);
    void submit_tool_send_review(WorkerTaskKind kind, const SendOperationSummary &operation,
        const Folder *target, WorkerSendReviewAction action);
    void open_compose_new();
    void open_compose_from_selected(bool reply_all, bool forward, bool attach_original);
    void open_draft_list();
    void open_draft(uint64_t id, uint64_t revision, const Account &account);
    void set_status(const char *);
    void set_action_continuation_status(const char *);
    void render_accounts_and_folders();
    void render_message_rows();
    bool submit_initialize();
    bool submit_task(WorkerTaskKind kind, WorkerRequest request, WorkerTaskInput *input,
        SecretBytes secret, PendingUiRequest pending, Text &error);
    PendingUiRequest *find_pending(uint64_t request_id, size_t *index = nullptr);
    void remove_pending(size_t index);
    bool make_live_credentials(SecretBytes &packet, Text &error) const;
    bool make_imap_credentials_for(const Account &account, SecretBytes &packet, Text &error) const;
    bool make_smtp_credentials_for(const Account &account, SecretBytes &packet, Text &error) const;
    bool install_credentials(const Account &account, SecretBytes incoming, SecretBytes outgoing);
    void remove_credentials(uint64_t account_id);
    bool set_unlocked_credentials(SecretBytes packet, Text &error);
    void open_account_dialog(bool add_new = false);
    void handle_account_dialog(AccountSetupDialog &, AccountDialogAction);
    void start_account_test(AccountSetupDialog &, bool smtp);
    void start_account_save(AccountSetupDialog &);
    void refresh_after_account_mutation(uint64_t deleted_account_id = 0);
    void select_account(size_t index);
    void select_folder(size_t index);
    void choose_default_folder();
    void refresh_account_list();
    void refresh_folders();
    void sync_selected_folder();
    void schedule_query(bool immediate = false, bool preserve_selection = false);
    void submit_query(bool more = false);
    bool build_query(MessageQuerySpec &query, Text &error) const;
    void open_filter_dialog();
    void select_message(size_t index);
    bool submit_row_task(WorkerTaskKind kind, const MessageQueryRow &row,
        WorkerTaskInput &input, SecretBytes secret, PendingPurpose purpose,
        uint64_t selection_generation, Text &error, PendingUiRequest extra);
    void fetch_selected_message();
    void preview_original();
    void export_original(uint32_t attachment_index);
    void open_safe_link(const char *url, size_t length);
    void start_vault_flow();
    void handle_vault_status(bool exists);
    void prompt_unlock(bool remember_after_unlock = false, SecretBytes master = SecretBytes());
    void save_vault(SecretBytes master);
    void change_vault_password();
    void abort_export(const char *message);
    bool write_export_chunk(const WorkerChunkResult &chunk, Text &error);
    void finish_export();

public:
    explicit MailWindow(const char *worker_executable, UiExtensionHooks hooks = {});
    ~MailWindow() override;
    bool start();
    void resize(int, int, int, int) override;
    void set_extension_hooks(UiExtensionHooks hooks) { hooks_ = hooks; update_ribbon_state(); }
    const WorkerStartupSnapshot &startup_snapshot() const { return startup_; }
    bool preview_visible() const { return preview_visible_; }
    bool vertical_preview() const { return vertical_preview_; }
};

}

#endif
