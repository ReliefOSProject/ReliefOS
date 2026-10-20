#include "window.h"
#include "compose_window.h"
#include "action_policy.h"
#include "mail_ui_layout.h"
#include "imap.h"
#include "../fltk_support.h"
#include <FL/Fl.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/fl_draw.H>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace rmail {

namespace {
constexpr int kDividerWidth = 6;
}

class PaneDivider : public Fl_Box {
    MailWindow *owner_;
    bool left_;
    bool horizontal_drag_ = true;
    bool dragging_ = false;
    int last_coordinate_ = 0;

    int coordinate() const {
        return horizontal_drag_ ? Fl::event_x_root() : Fl::event_y_root();
    }

public:
    PaneDivider(MailWindow *owner, bool left)
        : Fl_Box(0, 0, kDividerWidth, 100), owner_(owner), left_(left) {
        box(FL_UP_BOX);
        tooltip(T("Drag to resize panes"));
        clear_visible_focus();
        axis(true);
    }

    void axis(bool horizontal_drag) {
        horizontal_drag_ = horizontal_drag;
        if (window()) window()->cursor(horizontal_drag_ ? FL_CURSOR_WE : FL_CURSOR_NS);
    }

    int handle(int event) override {
        if (event == FL_ENTER) {
            if (window()) window()->cursor(horizontal_drag_ ? FL_CURSOR_WE : FL_CURSOR_NS);
            return 1;
        }
        if (event == FL_LEAVE) {
            if (!dragging_ && window()) window()->default_cursor(FL_CURSOR_DEFAULT);
            return 1;
        }
        if (event == FL_PUSH) {
            dragging_ = true;
            last_coordinate_ = coordinate();
            box(FL_DOWN_BOX);
            redraw();
            return 1;
        }
        if (event == FL_DRAG && dragging_) {
            const int current = coordinate();
            const int delta = current - last_coordinate_;
            last_coordinate_ = current;
            if (delta) {
                if (left_) owner_->resize_left_pane(delta);
                else owner_->resize_preview_split(delta);
            }
            return 1;
        }
        if (event == FL_RELEASE && dragging_) {
            dragging_ = false;
            box(FL_UP_BOX);
            redraw();
            return 1;
        }
        return Fl_Box::handle(event);
    }
};

namespace {
constexpr int kTopHeight = 34;
constexpr int kRibbonHeight = 100;
constexpr int kStatusHeight = 28;
constexpr int kNavigationTop = kTopHeight + kRibbonHeight;
constexpr int kSearchStripHeight = 36;
constexpr int kContentTop = kNavigationTop + kSearchStripHeight;
constexpr int kLeftWidth = 208;
constexpr int kLeftMinWidth = 180;
constexpr int kListMinWidth = 260;
constexpr int kReaderMinWidth = 260;
constexpr int kMessageRowHeight = 72;
constexpr int kMessageRowGap = 2;
constexpr int kVerticalReaderMinHeight = 234;
constexpr int kVerticalListMinHeight = 130;
bool set_error(Text &error, const char *message) {
    error.set(message ? message : T("The operation failed."));
    return false;
}

void group_label(int x, int y, int width, const char *label) {
    Fl_Box *box = new Fl_Box(x, y, width, 15, label);
    box->labelsize(11);
}

Fl_Button *ribbon_button(int x, int y, int width, const char *label,
    UiCommand command, Fl_Callback *callback, const char *tip = nullptr,
    int height = 26) {
    Fl_Button *button = new Fl_Button(x, y, width, height, label);
    button->callback(callback, reinterpret_cast<void *>(static_cast<intptr_t>(command) + 1));
    if (tip) button->tooltip(tip);
    return button;
}

void format_row_date(int64_t seconds, char *buffer, size_t capacity) {
    const time_t value = static_cast<time_t>(seconds);
    struct tm utc;
    if (static_cast<int64_t>(value) == seconds && gmtime_r(&value, &utc) &&
        strftime(buffer, capacity, "%m-%d %H:%M", &utc)) return;
    snprintf(buffer, capacity, "%s", T("Date unavailable"));
}

const char *task_label(WorkerTaskKind kind) {
    switch (kind) {
    case WorkerTaskKind::Initialize: return T("Loading local profile");
    case WorkerTaskKind::AccountTestImap: return T("Testing incoming sign-in");
    case WorkerTaskKind::AccountTestSmtp: return T("Testing outgoing sign-in");
    case WorkerTaskKind::FolderList: return T("Synchronizing folder names");
    case WorkerTaskKind::HeaderSync: return T("Synchronizing message headers");
    case WorkerTaskKind::MessagePage:
    case WorkerTaskKind::MessageSearch: return T("Searching local mail");
    case WorkerTaskKind::ServerSearch: return T("Searching the selected IMAP folder");
    case WorkerTaskKind::ParseMessage: return T("Reading cached message");
    case WorkerTaskKind::FetchMessage: return T("Fetching complete message");
    case WorkerTaskKind::ReadMessageChunk: return T("Reading a bounded message section");
    case WorkerTaskKind::SaveForwardDraft: return T("Saving the complete original as a forwarded attachment");
    case WorkerTaskKind::AccountCreate:
    case WorkerTaskKind::AccountUpdate: return T("Saving account settings");
    case WorkerTaskKind::AccountDelete: return T("Removing account settings");
    case WorkerTaskKind::VaultStatus: return T("Checking encrypted password storage");
    case WorkerTaskKind::UnlockVault: return T("Unlocking encrypted password storage");
    case WorkerTaskKind::SaveVault: return T("Saving encrypted password storage");
    case WorkerTaskKind::ChangeVaultPassword: return T("Changing encrypted storage password");
    default: return T("Working in the background");
    }
}

}

MailWindow::MailWindow(const char *worker_executable, UiExtensionHooks hooks)
    : Fl_Double_Window(1160, 740, T("ReliefMail")), hooks_(hooks) {
    size_range(860, 540);
    callback(close_cb, this);
    (void)pending_.reserve(MaxWorkerOutstanding);
    (void)credentials_.reserve(MaxAccounts * 2);
    (void)rows_.reserve(MaxRowsPerPage);
    begin();

    top_ = new Fl_Group(0, 0, w(), kTopHeight);
    Fl_Menu_Button *file = new Fl_Menu_Button(7, 3, 64, 28, T("File"));
    file->add(T("New message"), FL_CTRL + 'n', action_cb,
        reinterpret_cast<void *>(static_cast<intptr_t>(UiCommand::Compose) + 1));
    file->add(T("Drafts..."), 0, action_cb,
        reinterpret_cast<void *>(static_cast<intptr_t>(UiCommand::Drafts) + 1));
    file->add(T("Manage accounts..."), 0, action_cb,
        reinterpret_cast<void *>(static_cast<intptr_t>(UiCommand::ManageAccounts) + 1));
    file->add(T("About ReliefMail"), 0, action_cb,
        reinterpret_cast<void *>(static_cast<intptr_t>(UiCommand::About) + 1));
    file->add(T("Exit"), 0, action_cb,
        reinterpret_cast<void *>(static_cast<intptr_t>(UiCommand::Quit) + 1));

    const char *tab_names[] = {T("Home"), T("Send/Receive"), T("Tools"), T("View")};
    const int tab_x[] = {80, 162, 278, 360};
    const int tab_w[] = {76, 108, 76, 70};
    for (int index = 0; index < 4; ++index) {
        tabs_[index] = new Fl_Button(tab_x[index], 3, tab_w[index], 28, tab_names[index]);
        tabs_[index]->type(FL_TOGGLE_BUTTON);
        tabs_[index]->callback(tab_cb, reinterpret_cast<void *>(static_cast<intptr_t>(index)));
    }
    Fl_Box *stretch = new Fl_Box(612, 0, w() - 800, kTopHeight);
    quick_compose_ = new Fl_Button(w() - 180, 4, 82, 26, T("New message"));
    quick_compose_->callback(action_cb, reinterpret_cast<void *>(static_cast<intptr_t>(UiCommand::Compose) + 1));
    quick_sync_ = new Fl_Button(w() - 92, 4, 84, 26, T("Sync"));
    quick_sync_->callback(action_cb, reinterpret_cast<void *>(static_cast<intptr_t>(UiCommand::SyncMessages) + 1));
    top_->resizable(stretch);
    top_->end();

    ribbon_ = new Fl_Group(0, kTopHeight, w(), kRibbonHeight);
    ribbon_scroll_ = new Fl_Scroll(0, kTopHeight, w(), kRibbonHeight);
    ribbon_scroll_->type(Fl_Scroll::HORIZONTAL);
    build_ribbon();
    ribbon_scroll_->end();
    ribbon_->resizable(ribbon_scroll_);
    ribbon_->end();

    left_pane_ = new Fl_Group(0, kNavigationTop, kLeftWidth, h() - kNavigationTop - kStatusHeight);
    left_pane_->box(FL_BORDER_BOX);
    PlainTextBox *account_heading = new PlainTextBox(12, kNavigationTop + 7, kLeftWidth - 24, 24,
        T("Mail navigation"));
    account_heading->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    account_heading->labelsize(14);
    account_folder_scroll_ = new Fl_Scroll(4, kNavigationTop + 34, kLeftWidth - 8,
        h() - kNavigationTop - kStatusHeight - 38);
    account_folder_scroll_->type(Fl_Scroll::VERTICAL);
    account_folder_scroll_->box(FL_DOWN_BOX);
    account_folder_scroll_->end();
    left_pane_->end();

    search_strip_ = new Fl_Group(kLeftWidth + kDividerWidth, kNavigationTop,
        w() - kLeftWidth - kDividerWidth - 6, kSearchStripHeight);
    search_input_ = new Fl_Input(kLeftWidth + kDividerWidth + 8, kNavigationTop + 5, 300, 26);
    search_input_->when(FL_WHEN_CHANGED);
    search_input_->callback(query_cb, this);
    search_input_->tooltip(T("Search cached sender, subject, body coverage, or message ID"));
    search_field_ = new Fl_Choice(kLeftWidth + kDividerWidth + 314, kNavigationTop + 5, 78, 26);
    search_field_->add(T("All"));
    search_field_->add(T("From"));
    search_field_->add(T("Subject"));
    search_field_->add(T("Body"));
    search_field_->add(T("Message-ID"));
    search_field_->value(0);
    search_field_->callback(query_cb, this);
    scope_choice_ = new Fl_Choice(kLeftWidth + kDividerWidth + 398, kNavigationTop + 5, 92, 26);
    scope_choice_->add(T("Unified"));
    scope_choice_->add(T("Account"));
    scope_choice_->add(T("Folder"));
    scope_choice_->add(T("All mail"));
    scope_choice_->value(0);
    scope_choice_->callback(query_cb, this);
    search_button_ = new Fl_Button(kLeftWidth + kDividerWidth + 496, kNavigationTop + 5, 66, 26, T("Search"));
    search_button_->callback(action_cb, reinterpret_cast<void *>(static_cast<intptr_t>(UiCommand::Search) + 1));
    filter_button_ = new Fl_Button(kLeftWidth + kDividerWidth + 568, kNavigationTop + 5, 82, 26, T("Filters..."));
    filter_button_->callback(filter_cb, this);
    more_button_ = new Fl_Button(kLeftWidth + kDividerWidth + 656, kNavigationTop + 5, 92, 26, T("More"));
    more_button_->callback(action_cb, reinterpret_cast<void *>(static_cast<intptr_t>(UiCommand::More) + 1));
    more_button_->deactivate();
    search_strip_->end();

    const int main_x = kLeftWidth + kDividerWidth;
    const int initial_list_width = 352;
    list_pane_ = new Fl_Group(main_x, kContentTop, initial_list_width,
        h() - kContentTop - kStatusHeight);
    list_pane_->box(FL_BORDER_BOX);
    list_heading_ = new PlainTextBox(main_x + 10, kContentTop + 5, initial_list_width - 20, 24,
        T("Messages"));
    list_heading_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    list_heading_->labelfont(FL_HELVETICA_BOLD);
    list_heading_->labelsize(14);
    message_scroll_ = new Fl_Scroll(main_x + 4, kContentTop + 31, initial_list_width - 8,
        h() - kContentTop - kStatusHeight - 35);
    message_scroll_->type(Fl_Scroll::VERTICAL);
    message_scroll_->box(FL_DOWN_BOX);
    message_scroll_->end();
    list_pane_->end();

    ReaderActions reader_actions;
    reader_actions.fetch = reader_fetch_cb;
    reader_actions.source = reader_export_cb;
    reader_actions.preview_original = reader_preview_cb;
    reader_actions.save_attachment = reader_attachment_cb;
    reader_actions.open_link = reader_link_cb;
    reader_actions.context = this;
    reader_pane_ = new ReaderPane(main_x + initial_list_width + kDividerWidth, kContentTop,
        w() - main_x - initial_list_width - kDividerWidth - 6,
        h() - kContentTop - kStatusHeight, reader_actions);
    left_divider_ = new PaneDivider(this, true);
    preview_divider_ = new PaneDivider(this, false);

    bottom_ = new Fl_Group(0, h() - kStatusHeight, w(), kStatusHeight);
    bottom_->box(FL_THIN_UP_BOX);
    status_ = new PlainTextBox(8, h() - kStatusHeight + 1, w() / 2 - 12, kStatusHeight - 2, T("Ready"));
    status_->labelsize(13);
    status_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    selection_status_ = new PlainTextBox(w() / 2, h() - kStatusHeight + 1,
        w() / 2 - 8, kStatusHeight - 2, T("No message selected"));
    selection_status_->labelsize(13);
    selection_status_->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    bottom_->end();
    end();
    resizable(reader_pane_);
    show_tab(0);
    render_accounts_and_folders();
    render_message_rows();
    layout_content();
    update_ribbon_state();
    int screen_x = 0, screen_y = 0, screen_w = 0, screen_h = 0;
    Fl::screen_work_area(screen_x, screen_y, screen_w, screen_h);
    const int width = w() < screen_w - 24 ? w() : screen_w - 24;
    const int height = h() < screen_h - 48 ? h() : screen_h - 48;
    if (width >= 860 && height >= 540)
        resize(screen_x + (screen_w - width) / 2, screen_y + (screen_h - height) / 2, width, height);

    Text error;
    if (profile_paths(paths_, error) && coordinator_.open(worker_executable, paths_, error)) {
        coordinator_open_ = true;
        set_status(T("Starting local profile..."));
    } else {
        last_error_.set(error.c_str());
        set_status(error.empty() ? T("Could not open the local profile") : error.c_str());
    }
}

MailWindow::~MailWindow() {
    Fl::remove_timeout(tick_cb, this);
    Fl::remove_timeout(search_timeout_cb, this);
    if (export_.fd >= 0) close(export_.fd);
    delete compose_window_;
    compose_window_ = nullptr;
    delete tools_window_;
    tools_window_ = nullptr;
    coordinator_.close();
}

bool MailWindow::start() {
    if (!coordinator_open_ || !submit_initialize()) return false;
    Fl::add_timeout(.02, tick_cb, this);
    return true;
}

bool MailWindow::submit_initialize() {
    startup_query_waiting_for_vault_ = false;
    WorkerRequest request;
    PendingUiRequest pending;
    pending.purpose = PendingPurpose::Initialize;
    Text error;
    if (!submit_task(WorkerTaskKind::Initialize, request, nullptr, SecretBytes(),
            static_cast<PendingUiRequest &&>(pending), error)) {
        last_error_.set(error.c_str());
        set_status(error.empty() ? T("Could not start profile initialization") : error.c_str());
        return false;
    }
    set_status(T("Loading local accounts and folders..."));
    return true;
}

bool MailWindow::submit_task(WorkerTaskKind kind, WorkerRequest request, WorkerTaskInput *input,
    SecretBytes secret, PendingUiRequest pending, Text &error) {
    if (!coordinator_open_ || closing_) return set_error(error, "background worker is not available");
    if (!request.request_id) request.request_id = next_request_id_++;
    if (!request.profile_epoch) request.profile_epoch = coordinator_.profile_epoch();
    if (!request.ui_generation) request.ui_generation = ui_generation_;
    request.kind = kind;
    request.profile_paths = paths_;
    if (input && !worker_encode_task_input(kind, *input, request.payload, error)) return false;
    pending.token.request_id = request.request_id;
    pending.token.kind = kind;
    pending.token.profile_epoch = request.profile_epoch;
    pending.token.ui_generation = request.ui_generation;
    if (!coordinator_.submit(request, static_cast<SecretBytes &&>(secret), error)) return false;
    if (!pending_.push(static_cast<PendingUiRequest &&>(pending))) {
        Text cancel_error;
        (void)coordinator_.cancel(request.request_id, cancel_error);
        return set_error(error, "pending task table is full");
    }
    active_request_id_ = request.request_id;
    set_status(task_label(kind));
    return true;
}

MailWindow::PendingUiRequest *MailWindow::find_pending(uint64_t request_id, size_t *index) {
    for (size_t i = 0; i < pending_.size(); ++i) {
        if (pending_[i].token.request_id == request_id) {
            if (index) *index = i;
            return &pending_[i];
        }
    }
    return nullptr;
}

void MailWindow::remove_pending(size_t index) {
    if (index < pending_.size()) pending_.erase(index);
}

void MailWindow::close_cb(Fl_Widget *, void *context) {
    MailWindow *window = static_cast<MailWindow *>(context);
    if (!window->coordinator_open_ || window->close_complete_) {
        window->hide();
        return;
    }
    window->close_step();
}

void MailWindow::close_step() {
    if (closing_) return;
    Text close_reason;
    if (compose_window_ && !compose_window_->close_for_application(close_reason)) {
        set_status(close_reason.c_str());
        return;
    }
    if (!task6_extension_allows_application_close(hooks_.prepare_close,
            hooks_.context, close_reason)) {
        set_status(close_reason.empty() ? T("An open editor must be resolved before closing.") :
            close_reason.c_str());
        return;
    }
    closing_ = true;
    Fl::remove_timeout(search_timeout_cb, this);
    abort_export(T("Export stopped because the window is closing."));
    Text error;
    if (!coordinator_.begin_shutdown(error)) {
        closing_ = false;
        set_status(error.empty() ? T("Could not begin safe shutdown") : error.c_str());
        return;
    }
    set_status(T("Stopping background work safely..."));
}

void MailWindow::tick_cb(void *context) {
    MailWindow *window = static_cast<MailWindow *>(context);
    window->pump_once();
    if (!window->close_complete_) Fl::repeat_timeout(.02, tick_cb, context);
}

void MailWindow::pump_once() {
    if (!coordinator_open_ || close_complete_) return;
    Text error;
    if (!coordinator_.pump(error) && !error.empty()) {
        last_error_.set(error.c_str());
        set_status(error.c_str());
    }
    WorkerCoordinatorStatus worker_status;
    if (coordinator_.status(worker_status)) active_request_id_ = worker_status.current_request_id;
    update_ribbon_state();
    WorkerResult result;
    /* One result per short tick bounds all parent-thread UI and decoding work. */
    if (coordinator_.take_result(result)) handle_result(result);
    if (closing_) {
        WorkerCoordinatorStatus state;
        if (coordinator_.status(state) && state.active + state.queued == 0 &&
            coordinator_.shutdown_complete()) {
            Text finish_error;
            if (coordinator_.finish_shutdown(finish_error)) {
                close_complete_ = true;
                coordinator_open_ = false;
                hide();
            } else {
                closing_ = false;
                set_status(finish_error.empty() ? T("Safe shutdown could not finish") : finish_error.c_str());
            }
        }
    }
}

void MailWindow::handle_result(WorkerResult &result) {
    size_t index = 0;
    PendingUiRequest *found = find_pending(result.request_id, &index);
    if (!found) return;
    PendingUiRequest pending = static_cast<PendingUiRequest &&>(*found);
    remove_pending(index);

    if (pending.tool_window_task) {
        if (tool_pending_count_) --tool_pending_count_;
        if (tools_window_) tools_window_->set_busy(tool_pending_count_ != 0);
    }

    const Task6ResultDisposition disposition = task6_classify_result(pending.token, result,
        coordinator_.profile_epoch(), ui_generation_);
    if (disposition == Task6ResultDisposition::ProfileTransition) {
        action_batch_pending_ = 0;
        action_batch_had_uncertain_ = false;
        action_batch_last_diagnostic_.clear();
        credentials_.clear();
        rows_.clear();
        selected_row_valid_ = false;
        selected_message_valid_ = false;
        ++selection_generation_;
        ++ui_generation_;
        abort_export(T("Export stopped because the local profile changed."));
        if (compose_window_) compose_window_->profile_transition();
        reader_pane_->clear_message(T("The local profile changed. Reloading the current profile."));
        startup_complete_ = false;
        Text error;
        if (submit_initialize()) return;
        set_status(error.empty() ? T("Profile recovery needs attention before mail can be read") : error.c_str());
        return;
    }
    if (pending.purpose != PendingPurpose::ExportChunk &&
        disposition != Task6ResultDisposition::Apply) return;
    if (pending.purpose == PendingPurpose::ExportChunk &&
        (result.request_id != pending.token.request_id || result.kind != pending.token.kind ||
         result.profile_epoch != coordinator_.profile_epoch())) {
        abort_export(T("Export stopped because its profile context is no longer current."));
        return;
    }
    if (pending.purpose == PendingPurpose::ParseMessage || pending.purpose == PendingPurpose::FetchMessage ||
        pending.purpose == PendingPurpose::SourcePreview) {
        if (!task6_selection_result_is_current(pending.selection_generation, selection_generation_)) return;
    }
    if (pending.purpose == PendingPurpose::ToolTask) {
        handle_tool_result(pending, result);
        return;
    }
    Text error;
    if (!result.ok()) {
        if (pending.purpose == PendingPurpose::VaultStatus && startup_query_waiting_for_vault_) {
            startup_query_waiting_for_vault_ = false;
            last_error_.set(result.diagnostic.c_str());
            set_status(T("The password vault could not be checked; cached mail will still load."));
            schedule_query(true);
            return;
        }
        if (pending.purpose == PendingPurpose::ExportChunk) abort_export(result.diagnostic.c_str());
        else if (pending.compose && pending.purpose == PendingPurpose::ComposeSave) {
            pending.compose->save_result(pending.compose_save_token, false, nullptr,
                result.diagnostic.empty() ? T("Draft save failed; local edits are preserved.") : result.diagnostic.c_str());
        } else if (pending.compose && pending.purpose == PendingPurpose::ComposeCopy) {
            pending.compose->copy_result(false, nullptr, pending.account, pending.compose_copy_source_account,
                pending.compose_copy_source_id, pending.compose_copy_source_revision, result.diagnostic.c_str());
        } else if (pending.compose && pending.purpose == PendingPurpose::ComposeQueue) {
            WorkerOperationResult operation;
            WorkerOperationResult *decoded = worker_decode_operation_result(
                result.payload.data(), result.payload.size(), operation, error) ? &operation : nullptr;
            pending.compose->queue_result(false, decoded, result.diagnostic.c_str(), pending.compose_transmit);
        } else if (pending.compose && pending.purpose == PendingPurpose::ComposeSend) {
            WorkerOperationResult operation;
            WorkerOperationResult *decoded = worker_decode_operation_result(
                result.payload.data(), result.payload.size(), operation, error) ? &operation : nullptr;
            pending.compose->send_result(false, decoded, result.diagnostic.c_str());
        } else if (pending.compose && pending.purpose == PendingPurpose::ComposeArchiveRetry) {
            WorkerOperationResult operation;
            WorkerOperationResult *decoded = worker_decode_operation_result(
                result.payload.data(), result.payload.size(), operation, error) ? &operation : nullptr;
            pending.compose->archive_retry_result(false, decoded, result.diagnostic.c_str());
        } else if (pending.compose && pending.purpose == PendingPurpose::ComposeCatalog) {
            pending.compose->compose_catalog_result(false, nullptr, pending.account,
                pending.compose_catalog_serial, pending.compose_template_id,
                pending.compose_catalog_query, result.diagnostic.c_str());
        } else {
            last_error_.set(result.diagnostic.c_str());
            if (pending.purpose == PendingPurpose::Query &&
                !pending.tool_action_result_diagnostic.empty())
                set_status(pending.tool_action_result_diagnostic.c_str());
            else set_status(result.diagnostic.empty() ? T("Background operation failed") : result.diagnostic.c_str());
        }
        return;
    }

    switch (pending.purpose) {
    case PendingPurpose::Initialize: {
        WorkerStartupSnapshot next;
        if (!worker_decode_startup_snapshot(result.payload.data(), result.payload.size(), next, error)) {
            set_status(error.c_str()); return;
        }
        startup_ = static_cast<WorkerStartupSnapshot &&>(next);
        startup_complete_ = true;
        selected_account_id_ = startup_.selected_account_id;
        if (tools_window_) {
            const uint64_t tools_account = tools_window_->selected_account_id();
            tools_window_->refresh_snapshot(startup_, selected_account_id_);
            if (tools_account && tools_account != selected_account_id_) tools_account_changed(tools_account);
        }
        render_accounts_and_folders();
        choose_default_folder();
        WorkerRequest status_request;
        PendingUiRequest status_pending;
        status_pending.purpose = PendingPurpose::VaultStatus;
        const bool vault_status_submitted = submit_task(WorkerTaskKind::VaultStatus,
            status_request, nullptr, SecretBytes(),
            static_cast<PendingUiRequest &&>(status_pending), error);
        const Task6StartupQueryAction query_action = task6_startup_query_action(
            vault_status_submitted ? Task6StartupVaultCheck::Pending :
                Task6StartupVaultCheck::NotStarted);
        startup_query_waiting_for_vault_ =
            query_action == Task6StartupQueryAction::WaitForVaultCheck;
        if (query_action == Task6StartupQueryAction::SubmitQueryNow) {
            last_error_.set(error.c_str());
            set_status(T("The password vault could not be checked; cached mail will still load."));
            schedule_query(true);
        }
        return;
    }
    case PendingPurpose::VaultStatus: {
        WorkerVaultStatus status;
        if (!worker_decode_vault_status(result.payload.data(), result.payload.size(), status, error)) {
            last_error_.set(error.c_str());
            set_status(T("The password vault could not be checked; cached mail will still load."));
            if (startup_query_waiting_for_vault_) {
                startup_query_waiting_for_vault_ = false;
                schedule_query(true);
            }
            return;
        }
        handle_vault_status(status.exists);
        if (startup_query_waiting_for_vault_) {
            startup_query_waiting_for_vault_ = false;
            schedule_query(true);
        }
        return;
    }
    case PendingPurpose::AccountSave: {
        WorkerAccountResult saved;
        if (!worker_decode_account_result(result.payload.data(), result.payload.size(), saved, error) || saved.deleted) {
            set_status(error.empty() ? T("Account save returned an invalid result") : error.c_str()); return;
        }
        remove_credentials(saved.account.id);
        if (!install_credentials(saved.account, static_cast<SecretBytes &&>(pending.incoming_password),
                static_cast<SecretBytes &&>(pending.outgoing_password))) {
            set_status(T("Account saved, but session credentials could not be retained; enter them again before connecting."));
        }
        bool found_account = false;
        for (size_t i = 0; i < startup_.accounts.size(); ++i) {
            if (startup_.accounts[i].id == saved.account.id) {
                startup_.accounts[i] = saved.account;
                found_account = true;
                break;
            }
        }
        if (!found_account) (void)startup_.accounts.push(saved.account);
        refresh_after_account_mutation();
        return;
    }
    case PendingPurpose::AccountDelete: {
        WorkerAccountResult deleted;
        if (!worker_decode_account_result(result.payload.data(), result.payload.size(), deleted, error) ||
            !deleted.deleted || deleted.account.id != pending.account.id) {
            set_status(error.empty() ? T("Account removal returned an invalid result") : error.c_str());
            return;
        }
        remove_credentials(deleted.account.id);
        for (size_t i = startup_.accounts.size(); i > 0; --i)
            if (startup_.accounts[i - 1].id == deleted.account.id) startup_.accounts.erase(i - 1);
        for (size_t i = startup_.folders.size(); i > 0; --i)
            if (startup_.folders[i - 1].key.account_id == deleted.account.id) startup_.folders.erase(i - 1);
        refresh_after_account_mutation(deleted.account.id);
        return;
    }
    case PendingPurpose::AccountTestImap:
        set_status(T("Incoming authentication succeeded; no messages were sent."));
        return;
    case PendingPurpose::AccountTestSmtp:
        set_status(T("Outgoing authentication succeeded; no messages were sent."));
        return;
    case PendingPurpose::AccountList:
    case PendingPurpose::FolderList: {
        WorkerStartupSnapshot next;
        if (!worker_decode_startup_snapshot(result.payload.data(), result.payload.size(), next, error)) {
            set_status(error.c_str()); return;
        }
        startup_ = static_cast<WorkerStartupSnapshot &&>(next);
        selected_account_id_ = startup_.selected_account_id;
        if (tools_window_) {
            const uint64_t tools_account = tools_window_->selected_account_id();
            tools_window_->refresh_snapshot(startup_, selected_account_id_);
            if (tools_account && tools_account != selected_account_id_) tools_account_changed(tools_account);
        }
        choose_default_folder();
        render_accounts_and_folders();
        schedule_query(true);
        set_status(pending.purpose == PendingPurpose::FolderList ?
            T("Folder list synchronized; cached messages remain available offline.") : T("Account folders loaded from the local profile."));
        return;
    }
    case PendingPurpose::HeaderSync: {
        WorkerMessagePage page;
        if (!worker_decode_message_page(result.payload.data(), result.payload.size(), page, error)) {
            set_status(error.c_str()); return;
        }
        if (!task6_apply_folder_sync_identity(startup_, selected_folder_, selected_folder_valid_,
                pending.folder, pending.account.generation, result.account_generation, page, error)) {
            set_status(error.c_str()); return;
        }
        rows_.clear();
        for (size_t i = 0; i < page.rows.size(); ++i) (void)rows_.push(static_cast<MessageQueryRow &&>(page.rows[i]));
        current_query_ = page.query;
        render_message_rows();
        set_status(T("Message headers synchronized; bodies are fetched only when you open a message."));
        return;
    }
    case PendingPurpose::Query: {
        WorkerMessagePage page;
        if (!worker_decode_message_page(result.payload.data(), result.payload.size(), page, error)) {
            set_status(error.c_str()); return;
        }
        if (!page.query.offset) rows_.clear();
        for (size_t i = 0; i < page.rows.size(); ++i) {
            if (!rows_.push(static_cast<MessageQueryRow &&>(page.rows[i]))) break;
        }
        current_query_ = page.query;
        more_button_->activate();
        if (!page.has_more || rows_.size() >= MaxRowsPerPage) more_button_->deactivate();
        render_message_rows();
        if (preserve_selection_on_query_ && selected_row_valid_) {
            for (size_t i = 0; i < rows_.size(); ++i) {
                if (rows_[i].message.key == selected_row_.message.key) {
                    selected_row_ = rows_[i];
                    break;
                }
            }
            preserve_selection_on_query_ = false;
        }
        Text refresh_status;
        if (!page.diagnostic.empty()) refresh_status.set(page.diagnostic.c_str());
        else {
            const char *scope = page.query.scope == MessageQueryScope::Folder ? T("Current folder") :
                page.query.scope == MessageQueryScope::Account ? T("Current account") :
                page.query.scope == MessageQueryScope::AllMail ? T("All mail") : T("Unified inbox");
            const char *field = T("Messages");
            if (!page.query.query.empty()) {
                switch (page.query.search_field) {
                case MessageQueryField::All: field = T("All fields"); break;
                case MessageQueryField::From: field = T("Sender"); break;
                case MessageQueryField::Subject: field = T("Subject"); break;
                case MessageQueryField::Body: field = T("Body"); break;
                case MessageQueryField::MessageId: field = T("Message-ID"); break;
                }
            }
            char summary[192];
            snprintf(summary, sizeof(summary), T("%s · %s · %llu messages"), scope, field,
                static_cast<unsigned long long>(page.total_matches));
            refresh_status.set(summary);
        }
        set_status(task8a_action_refresh_status(pending.tool_action_result_diagnostic, refresh_status));
        return;
    }
    case PendingPurpose::ParseMessage:
    case PendingPurpose::FetchMessage: {
        WorkerReaderResult message;
        if (!worker_decode_reader_result(result.payload.data(), result.payload.size(), message, error)) {
            reader_pane_->clear_message(error.c_str()); set_status(error.c_str()); return;
        }
        if (!selected_row_valid_ || selected_row_.message.key != pending.row.message.key ||
            !task6_apply_reader_result_to_row(message, pending.row)) {
            set_status(T("The reader result no longer matches the selected message."));
            return;
        }
        const bool attachment_coverage_changed =
            !selected_row_.attachment_presence_known && !message.headers_only;
        selected_row_ = pending.row;
        for (size_t i = 0; i < rows_.size(); ++i) {
            if (rows_[i].message.key == pending.row.message.key &&
                rows_[i].profile_epoch == pending.row.profile_epoch &&
                rows_[i].account_generation == pending.row.account_generation &&
                rows_[i].folder_generation == pending.row.folder_generation) {
                rows_[i] = pending.row;
                break;
            }
        }
        selected_message_valid_ = worker_clone_reader_result(message, selected_message_);
        reader_pane_->set_message(static_cast<WorkerReaderResult &&>(message));
        reader_pane_->set_fetch_available(reader_pane_->has_message() && selected_row_.message.headers_only);
        selection_status_->text(selected_row_.message.subject.c_str());
        update_ribbon_state();
        set_status(selected_row_.message.headers_only ? T("Headers are cached. Fetch the complete message to read its body.") : T("Message loaded from the local cache."));
        if (attachment_coverage_changed) {
            preserve_selection_on_query_ = true;
            schedule_query(true, true);
        }
        return;
    }
    case PendingPurpose::SourcePreview: {
        WorkerChunkResult chunk;
        if (!worker_decode_chunk_result(result.payload.data(), result.payload.size(), chunk, error)) {
            set_status(error.c_str()); return;
        }
        reader_pane_->set_original(chunk.bytes.data(), chunk.bytes.size(), chunk.total_size > chunk.bytes.size());
        set_status(chunk.headers_only ? T("This source contains headers only, not a complete original message.") : T("Showing a bounded original preview."));
        return;
    }
    case PendingPurpose::ExportChunk: {
        WorkerChunkResult chunk;
        if (!worker_decode_chunk_result(result.payload.data(), result.payload.size(), chunk, error) ||
            !write_export_chunk(chunk, error)) {
            abort_export(error.empty() ? T("Could not write the selected message data") : error.c_str());
            return;
        }
        export_.total_size = chunk.total_size;
        export_.offset += chunk.bytes.size();
        if (export_.offset >= export_.total_size) finish_export();
        else {
            WorkerTaskInput input;
            input.message_key = export_.row.message.key;
            input.chunk_offset = export_.offset;
            input.chunk_size = 256u * 1024u;
            input.attachment_index = export_.attachment_index;
            Text submit_error;
            (void)submit_row_task(WorkerTaskKind::ReadMessageChunk, export_.row, input,
                SecretBytes(), PendingPurpose::ExportChunk, selection_generation_, submit_error,
                PendingUiRequest());
        }
        return;
    }
    case PendingPurpose::UnlockVault: {
        if (result.secret_payload.empty() || !set_unlocked_credentials(static_cast<SecretBytes &&>(result.secret_payload), error)) {
            set_status(error.empty() ? T("The unlocked vault contained no usable account credentials.") : error.c_str());
            return;
        }
        set_status(T("Encrypted credentials unlocked for this session."));
        if (pending.remember_after_unlock)
            save_vault(static_cast<SecretBytes &&>(pending.master_password));
        return;
    }
    case PendingPurpose::SaveVault:
        vault_exists_ = true;
        set_status(result.status == WorkerStatus::CommittedWarning ?
            (result.diagnostic.empty() ? T("Password vault saved with cleanup pending.") : result.diagnostic.c_str()) :
            T("Passwords saved in the encrypted vault."));
        return;
    case PendingPurpose::ChangeVaultPassword:
        set_status(result.status == WorkerStatus::CommittedWarning ?
            (result.diagnostic.empty() ? T("Vault password changed; cleanup is pending.") : result.diagnostic.c_str()) :
            T("Vault password changed."));
        return;
    case PendingPurpose::ComposeSave: {
        WorkerDraftSnapshot snapshot;
        if (!worker_decode_draft_snapshot(result.payload.data(), result.payload.size(), snapshot, error)) {
            pending.compose->save_result(pending.compose_save_token, false, nullptr, error.c_str());
            return;
        }
        pending.compose->save_result(pending.compose_save_token, result.ok(), &snapshot,
            result.diagnostic.c_str());
        return;
    }
    case PendingPurpose::ComposeCopy: {
        WorkerDraftSnapshot snapshot;
        if (!worker_decode_draft_snapshot(result.payload.data(), result.payload.size(), snapshot, error)) {
            pending.compose->copy_result(false, nullptr, pending.account, pending.compose_copy_source_account,
                pending.compose_copy_source_id, pending.compose_copy_source_revision, error.c_str());
            return;
        }
        pending.compose->copy_result(result.ok(), &snapshot, pending.account,
            pending.compose_copy_source_account, pending.compose_copy_source_id,
            pending.compose_copy_source_revision, result.diagnostic.c_str());
        return;
    }
    case PendingPurpose::ComposeQueue:
    case PendingPurpose::ComposeSend: {
        WorkerOperationResult operation;
        const bool decoded = worker_decode_operation_result(result.payload.data(), result.payload.size(),
            operation, error);
        if (pending.purpose == PendingPurpose::ComposeQueue) {
            pending.compose->queue_result(result.ok() && decoded, decoded ? &operation : nullptr,
                decoded ? operation.diagnostic.c_str() : error.c_str(), pending.compose_transmit);
        } else {
            pending.compose->send_result(result.ok() && decoded, decoded ? &operation : nullptr,
                decoded ? operation.diagnostic.c_str() : error.c_str());
        }
        return;
    }
    case PendingPurpose::ComposeArchiveRetry: {
        WorkerOperationResult operation;
        const bool decoded = worker_decode_operation_result(result.payload.data(), result.payload.size(),
            operation, error);
        pending.compose->archive_retry_result(result.ok() && decoded, decoded ? &operation : nullptr,
            decoded ? operation.diagnostic.c_str() : error.c_str());
        return;
    }
    case PendingPurpose::ComposeCatalog: {
        WorkerComposeCatalog catalog;
        if (!worker_decode_compose_catalog(result.payload.data(), result.payload.size(), catalog, error)) {
            pending.compose->compose_catalog_result(false, nullptr, pending.account,
                pending.compose_catalog_serial, pending.compose_template_id,
                pending.compose_catalog_query, error.c_str());
            return;
        }
        pending.compose->compose_catalog_result(true, &catalog, pending.account,
            pending.compose_catalog_serial, pending.compose_template_id,
            pending.compose_catalog_query, result.diagnostic.c_str());
        return;
    }
    case PendingPurpose::ComposeDraftList: {
        WorkerDraftList drafts;
        if (!worker_decode_draft_list(result.payload.data(), result.payload.size(), drafts, error) ||
            drafts.account_generation != pending.account.generation) {
            set_status(error.empty() ? T("Draft list no longer matches this account configuration.") : error.c_str());
            return;
        }
        WorkerDraftSummary selected;
        if (!choose_compose_draft(drafts, selected)) return;
        open_draft(selected.id, selected.revision, pending.account);
        return;
    }
    case PendingPurpose::ComposeDraftLoad: {
        WorkerDraftSnapshot snapshot;
        if (!worker_decode_draft_snapshot(result.payload.data(), result.payload.size(), snapshot, error) ||
            snapshot.account_generation != pending.account.generation ||
            snapshot.draft.id != pending.draft_id || snapshot.draft.revision != pending.draft_revision) {
            set_status(error.empty() ? T("The draft changed or its account identity no longer matches. Reload the draft list.") : error.c_str());
            return;
        }
        if (!compose_window_) compose_window_ = new ComposeWindow(this);
        if (!compose_window_->open_existing(startup_.accounts, pending.account,
                static_cast<WorkerDraftSnapshot &&>(snapshot), coordinator_.profile_epoch()))
            set_status(T("The selected draft could not be opened safely."));
        return;
    }
    case PendingPurpose::ToolTask:
        return; // Dispatched above so tool failures retain their typed context.
    }
}

void MailWindow::build_ribbon() {
    const int content_height = kRibbonHeight - Fl::scrollbar_size();
    auto button = [](int x, int y, int width, const char *label, UiCommand command,
                         const char *tip = nullptr, int height = 26) {
        return ribbon_button(x, y, width, label, command, action_cb, tip, height);
    };
    auto group = [content_height](int x, int width, const char *label) {
        group_label(x, kTopHeight + content_height - 18, width, label);
        Fl_Box *separator = new Fl_Box(x + width - 3, 40, 2, 58);
        separator->box(FL_ENGRAVED_FRAME);
    };
    const int y0 = kTopHeight + 8;
    const int y1 = y0 + 29;

    ribbon_pages_[0] = new Fl_Group(0, kTopHeight, 1028, content_height);
    compose_button_ = button(8, y0, 86, T("New message"), UiCommand::Compose,
        T("Open a new rich-text draft"), 54);
    drafts_button_ = button(100, y0, 82, T("Drafts..."), UiCommand::Drafts,
        T("Open a saved draft using its exact current revision"), 54);
    group(4, 186, T("New"));
    reply_button_ = button(200, y0, 88, T("Reply"), UiCommand::Reply);
    reply_all_button_ = button(200, y1, 88, T("Reply all"), UiCommand::ReplyAll);
    forward_button_ = button(294, y0, 98, T("Forward mail"), UiCommand::Forward);
    forward_attachment_button_ = button(294, y1, 98, T("Forward as .eml"), UiCommand::ForwardAsAttachment);
    group(196, 204, T("Reply and forward"));
    message_action_buttons_[4] = button(410, y0, 84, T("Archive"), UiCommand::ArchiveMessage);
    message_action_buttons_[5] = button(410, y1, 84, T("Junk"), UiCommand::JunkMessage);
    message_action_buttons_[6] = button(500, y0, 84, T("Trash"), UiCommand::TrashMessage);
    message_action_buttons_[7] = button(500, y1, 84, T("Move..."), UiCommand::MoveMessage);
    group(406, 186, T("Organize"));
    message_action_buttons_[0] = button(602, y0, 86, T("Mark read"), UiCommand::MarkRead);
    message_action_buttons_[1] = button(602, y1, 86, T("Mark unread"), UiCommand::MarkUnread);
    message_action_buttons_[2] = button(694, y0, 82, T("Star"), UiCommand::StarMessage);
    message_action_buttons_[3] = button(694, y1, 82, T("Unstar"), UiCommand::UnstarMessage);
    group(598, 186, T("Message flags"));
    message_action_buttons_[8] = button(794, y0, 88, T("Copy..."), UiCommand::CopyMessage, nullptr, 54);
    group(790, 100, T("Copy"));
    message_action_buttons_[9] = button(900, y0, 116, T("Delete permanently"), UiCommand::DeleteMessage,
        T("Permanently delete only these exact UIDs with the server's UIDPLUS capability"), 54);
    group(896, 128, T("Permanent deletion"));
    ribbon_pages_[0]->end();

    ribbon_pages_[1] = new Fl_Group(0, kTopHeight, 802, content_height);
    sync_messages_button_ = button(8, y0, 112, T("Sync headers"), UiCommand::SyncMessages,
        T("Synchronize selected folder headers"), 54);
    refresh_folders_button_ = button(126, y0, 112, T("Sync folders"), UiCommand::RefreshFolders,
        T("Refresh the selected account's folder list"), 54);
    group(4, 242, T("Send/Receive"));
    cancel_button_ = button(256, y0, 112, T("Cancel"), UiCommand::Cancel, nullptr, 54);
    cancel_button_->deactivate();
    group(252, 124, T("Background work"));
    account_add_button_ = button(386, y0, 116, T("Add account"), UiCommand::AddAccount, nullptr, 54);
    account_manage_button_ = button(506, y0, 126, T("Manage accounts"), UiCommand::ManageAccounts, nullptr, 54);
    group(382, 260, T("Account settings"));
    vault_button_ = button(654, y0, 138, T("Passwords..."), UiCommand::Vault,
        T("Unlock or save the optional encrypted password vault"), 54);
    group(650, 150, T("Credentials"));
    ribbon_pages_[1]->end();

    ribbon_pages_[2] = new Fl_Group(0, kTopHeight, 158, content_height);
    open_tools_button_ = button(8, y0, 134, T("Mail tools"), UiCommand::OpenTools,
        T("Open additional mail tools"), 54);
    group(4, 150, T("Operations"));
    ribbon_pages_[2]->end();

    ribbon_pages_[3] = new Fl_Group(0, kTopHeight, 264, content_height);
    preview_button_ = button(8, y0, 112, T("Hide preview"), UiCommand::TogglePreview);
    group(4, 124, T("Reading pane"));
    button(138, y0, 112, T("Side by side"), UiCommand::HorizontalPreview);
    button(138, y1, 112, T("Stacked"), UiCommand::VerticalPreview);
    group(134, 124, T("Layout"));
    ribbon_pages_[3]->end();
}

void MailWindow::show_tab(int tab) {
    active_tab_ = tab;
    for (int index = 0; index < 4; ++index) {
        if (index == tab) ribbon_pages_[index]->show();
        else ribbon_pages_[index]->hide();
        tabs_[index]->value(index == tab);
    }
    ribbon_scroll_->scroll_to(0, 0);
    ribbon_->redraw();
}

void MailWindow::update_ribbon_state() {
    const bool ready = startup_complete_ && !closing_ && !close_complete_;
    const bool has_account = selected_account_id_ != 0;
    const bool has_folder = selected_folder_valid_ && selected_folder_.key.account_id == selected_account_id_;
    const bool can_reply = ready && selected_row_valid_ && selected_message_valid_ &&
        !selected_message_.headers_only && selected_row_.profile_epoch == coordinator_.profile_epoch() &&
        selected_row_.message.key == selected_message_.key;
    if (compose_button_) { if (ready && !startup_.accounts.empty()) compose_button_->activate(); else compose_button_->deactivate(); }
    if (quick_compose_) { if (ready && !startup_.accounts.empty()) quick_compose_->activate(); else quick_compose_->deactivate(); }
    if (drafts_button_) { if (ready && has_account) drafts_button_->activate(); else drafts_button_->deactivate(); }
    if (reply_button_) { if (can_reply) reply_button_->activate(); else reply_button_->deactivate(); }
    if (reply_all_button_) { if (can_reply) reply_all_button_->activate(); else reply_all_button_->deactivate(); }
    if (forward_button_) { if (can_reply) forward_button_->activate(); else forward_button_->deactivate(); }
    if (forward_attachment_button_) { if (can_reply) forward_attachment_button_->activate(); else forward_attachment_button_->deactivate(); }
    if (quick_sync_) { if (ready && has_folder) quick_sync_->activate(); else quick_sync_->deactivate(); }
    if (sync_messages_button_) { if (ready && has_folder) sync_messages_button_->activate(); else sync_messages_button_->deactivate(); }
    if (refresh_folders_button_) { if (ready && has_account) refresh_folders_button_->activate(); else refresh_folders_button_->deactivate(); }
    if (cancel_button_) { if (active_request_id_) cancel_button_->activate(); else cancel_button_->deactivate(); }
    if (account_add_button_) { if (ready) account_add_button_->activate(); else account_add_button_->deactivate(); }
    if (account_manage_button_) { if (ready) account_manage_button_->activate(); else account_manage_button_->deactivate(); }
    if (vault_button_) { if (ready) vault_button_->activate(); else vault_button_->deactivate(); }
    if (open_tools_button_) { if (ready) open_tools_button_->activate(); else open_tools_button_->deactivate(); }
    bool action_selection_valid = false;
    if (ready && !checked_rows_.empty()) {
        const uint32_t index = checked_rows_[0];
        if (index < rows_.size()) {
            const MessageQueryRow &row = rows_[index];
            const Account *account = nullptr;
            for (size_t i = 0; i < startup_.accounts.size(); ++i)
                if (startup_.accounts[i].id == row.message.key.folder.account_id) account = &startup_.accounts[i];
            if (account) {
                BoundedArray<MessageQueryRow, MaxWorkerOutstanding> selected;
                Text error;
                action_selection_valid = task8a_collect_action_rows(rows_, checked_rows_, nullptr,
                    coordinator_.profile_epoch(), ui_generation_, account->id, account->generation,
                    selected, error);
            }
        }
    } else if (ready && selected_row_valid_) {
        const Account *account = nullptr;
        for (size_t i = 0; i < startup_.accounts.size(); ++i)
            if (startup_.accounts[i].id == selected_row_.message.key.folder.account_id) account = &startup_.accounts[i];
        if (account) {
            BoundedArray<MessageQueryRow, MaxWorkerOutstanding> selected;
            Text error;
            action_selection_valid = task8a_collect_action_rows(rows_, checked_rows_, &selected_row_,
                coordinator_.profile_epoch(), ui_generation_, account->id, account->generation,
                selected, error);
        }
    }
    for (size_t i = 0; i < sizeof(message_action_buttons_) / sizeof(message_action_buttons_[0]); ++i)
        if (message_action_buttons_[i]) {
            if (action_selection_valid) message_action_buttons_[i]->activate();
            else message_action_buttons_[i]->deactivate();
        }
    if (preview_button_) preview_button_->copy_label(preview_visible_ ? T("Hide preview") : T("Show preview"));
}

void MailWindow::update_navigation_selection() {
    if (!scope_choice_) return;
    const int scope = scope_choice_->value();
    if (unified_button_) unified_button_->value(scope == 0);
    if (all_mail_button_) all_mail_button_->value(scope == 3);
    if (unified_button_) unified_button_->redraw();
    if (all_mail_button_) all_mail_button_->redraw();
}

void MailWindow::select_navigation_scope(int scope) {
    if (!scope_choice_ || (scope != 0 && scope != 3)) return;
    scope_choice_->value(scope);
    filter_settings_.search.scope = message_query_scope_from_choice(scope);
    all_mail_navigation_selected_ = scope == 3;
    update_navigation_selection();
    schedule_query(true);
}

void MailWindow::action_cb(Fl_Widget *widget, void *data) {
    MailWindow *window = static_cast<MailWindow *>(widget->window());
    const intptr_t value = reinterpret_cast<intptr_t>(data) - 1;
    window->perform(static_cast<UiCommand>(value));
}

void MailWindow::tab_cb(Fl_Widget *widget, void *data) {
    static_cast<MailWindow *>(widget->window())->show_tab(static_cast<int>(reinterpret_cast<intptr_t>(data)));
}

void MailWindow::navigation_scope_cb(Fl_Widget *widget, void *data) {
    MailWindow *window = static_cast<MailWindow *>(widget->window());
    window->select_navigation_scope(static_cast<int>(reinterpret_cast<intptr_t>(data) - 1));
}

void MailWindow::query_cb(Fl_Widget *widget, void *context) {
    MailWindow *window = static_cast<MailWindow *>(context);
    if (widget == window->scope_choice_ && window->scope_choice_->value() == 2 && !window->selected_folder_valid_) {
        window->set_status(T("Select a real folder before using folder scope."));
        window->scope_choice_->value(message_query_scope_choice(window->filter_settings_.search.scope));
        window->all_mail_navigation_selected_ =
            window->filter_settings_.search.scope == MessageQueryScope::AllMail;
        window->update_navigation_selection();
        return;
    }
    if (widget == window->scope_choice_ || widget == window->search_field_) {
        window->filter_settings_.search = search_selection_from_choices(
            window->search_field_->value(), window->scope_choice_->value());
    }
    if (widget == window->scope_choice_) {
        window->all_mail_navigation_selected_ = window->scope_choice_->value() == 3;
        window->update_navigation_selection();
    }
    window->schedule_query();
}

void MailWindow::filter_cb(Fl_Widget *, void *context) { static_cast<MailWindow *>(context)->open_filter_dialog(); }

void MailWindow::search_timeout_cb(void *context) { static_cast<MailWindow *>(context)->submit_query(); }

void MailWindow::perform(UiCommand command) {
    if (closing_) return;
    switch (command) {
    case UiCommand::AddAccount: open_account_dialog(true); return;
    case UiCommand::ManageAccounts: open_account_dialog(false); return;
    case UiCommand::RefreshFolders: refresh_folders(); return;
    case UiCommand::SyncMessages: sync_selected_folder(); return;
    case UiCommand::Cancel: {
        if (!active_request_id_ || !coordinator_open_) {
            set_status(T("No background task is running")); return;
        }
        Text error;
        if (coordinator_.cancel(active_request_id_, error)) set_status(T("Cancelling background work..."));
        else set_status(error.empty() ? T("Could not cancel the current task") : error.c_str());
        return;
    }
    case UiCommand::TogglePreview:
        preview_visible_ = !preview_visible_;
        preview_button_->copy_label(preview_visible_ ? T("Hide preview") : T("Show preview"));
        if (preview_visible_) reader_pane_->show(); else reader_pane_->hide();
        layout_content(); update_ribbon_state(); return;
    case UiCommand::HorizontalPreview:
        vertical_preview_ = false; layout_content();
        set_status(T("Reading pane arranged beside the message list")); return;
    case UiCommand::VerticalPreview:
        vertical_preview_ = true; layout_content();
        set_status(T("Reading pane arranged below the message list")); return;
    case UiCommand::About: fl_message(T("ReliefMail\nNative mail reader and account client")); return;
    case UiCommand::OpenTools:
        if (hooks_.open_tools) hooks_.open_tools(hooks_.context);
        else open_tools_window();
        return;
    case UiCommand::Vault: start_vault_flow(); return;
    case UiCommand::Search: schedule_query(true); return;
    case UiCommand::More: submit_query(true); return;
    case UiCommand::Compose:
        if (hooks_.compose) hooks_.compose(hooks_.context);
        else open_compose_new();
        return;
    case UiCommand::Drafts:
        open_draft_list();
        return;
    case UiCommand::Reply:
        open_compose_from_selected(false, false, false);
        return;
    case UiCommand::ReplyAll:
        open_compose_from_selected(true, false, false);
        return;
    case UiCommand::Forward:
        open_compose_from_selected(false, true, false);
        return;
    case UiCommand::ForwardAsAttachment:
        open_compose_from_selected(false, true, true);
        return;
    case UiCommand::MarkRead:
    case UiCommand::MarkUnread:
    case UiCommand::StarMessage:
    case UiCommand::UnstarMessage:
    case UiCommand::ArchiveMessage:
    case UiCommand::JunkMessage:
    case UiCommand::TrashMessage:
    case UiCommand::MoveMessage:
    case UiCommand::CopyMessage:
    case UiCommand::DeleteMessage:
        run_message_action(command);
        return;
    case UiCommand::Quit: close_step(); return;
    case UiCommand::Count: return;
    }
}

void MailWindow::set_status(const char *text) {
    status_->text(text && *text ? text : T("Ready"));
    status_->redraw();
}

void MailWindow::open_compose_new() {
    if (!startup_complete_ || !coordinator_open_) {
        set_status(T("The local profile is still loading."));
        return;
    }
    if (!compose_window_) compose_window_ = new ComposeWindow(this);
    if (!compose_window_->open_new(startup_.accounts, selected_account_id_, coordinator_.profile_epoch()))
        set_status(T("The compose window could not be opened."));
}

void MailWindow::open_compose_from_selected(bool reply_all, bool forward,
    bool attach_original) {
    if (!selected_row_valid_ || !selected_message_valid_ ||
        selected_row_.message.key != selected_message_.key ||
        selected_row_.profile_epoch != coordinator_.profile_epoch()) {
        set_status(T("Load a complete selected message before creating a reply or forward."));
        return;
    }
    if (selected_message_.headers_only) {
        set_status(T("Fetch the complete message before replying or forwarding it."));
        return;
    }
    Account source_account;
    bool found = false;
    for (size_t i = 0; i < startup_.accounts.size(); ++i) {
        if (startup_.accounts[i].id == selected_message_.key.folder.account_id &&
            startup_.accounts[i].generation == selected_row_.account_generation) {
            source_account = startup_.accounts[i];
            found = true;
            break;
        }
    }
    if (!found) {
        set_status(T("The source message account changed. Reload it before composing."));
        return;
    }
    Draft prepared;
    Draft empty_source;
    Text error;
    bool ok = false;
    if (forward) {
        ok = compose_forward_draft(empty_source, selected_message_.from,
            selected_message_.to, selected_message_.cc, selected_message_.subject,
            selected_message_.plain_text, selected_message_.plain_text_truncated,
            prepared, error);
        prepared.account_id = source_account.id;
    } else {
        ok = compose_reply_draft(prepared, source_account, selected_message_.reply_to,
            selected_message_.from, selected_message_.to, selected_message_.cc,
            selected_message_.subject, selected_message_.message_id,
            selected_message_.references, selected_message_.plain_text,
            selected_message_.plain_text_truncated, reply_all, prepared, error);
    }
    if (!ok) {
        set_status(error.empty() ? T("The selected message cannot be used to create this draft.") : error.c_str());
        return;
    }
    if (!compose_window_) compose_window_ = new ComposeWindow(this);
    const MessageKey *original = attach_original ? &selected_message_.key : nullptr;
    if (!compose_window_->open_prefilled(startup_.accounts, source_account,
            static_cast<Draft &&>(prepared), coordinator_.profile_epoch(), original))
        set_status(T("The reply or forward window could not be opened safely."));
}

void MailWindow::open_draft_list() {
    if (!startup_complete_ || !coordinator_open_) { set_status(T("The local profile is still loading.")); return; }
    Account account;
    bool found_account = false;
    for (size_t i = 0; i < startup_.accounts.size(); ++i)
        if (startup_.accounts[i].id == selected_account_id_) { account = startup_.accounts[i]; found_account = true; break; }
    if (!found_account) { set_status(T("Select an account before opening its drafts.")); return; }
    WorkerRequest request;
    request.account_id = account.id;
    request.account_generation = account.generation;
    PendingUiRequest pending;
    pending.purpose = PendingPurpose::ComposeDraftList;
    pending.account = account;
    Text error;
    if (!submit_task(WorkerTaskKind::ListDrafts, request, nullptr, SecretBytes(),
            static_cast<PendingUiRequest &&>(pending), error))
        set_status(error.empty() ? T("Could not load saved drafts.") : error.c_str());
}

void MailWindow::open_draft(uint64_t id, uint64_t revision, const Account &account) {
    WorkerTaskInput input;
    input.item_id = id;
    input.item_revision = revision;
    WorkerRequest request;
    request.account_id = account.id;
    request.account_generation = account.generation;
    PendingUiRequest pending;
    pending.purpose = PendingPurpose::ComposeDraftLoad;
    pending.account = account;
    pending.draft_id = id;
    pending.draft_revision = revision;
    Text error;
    if (!submit_task(WorkerTaskKind::LoadDraft, request, &input, SecretBytes(),
            static_cast<PendingUiRequest &&>(pending), error))
        set_status(error.empty() ? T("Could not reopen the selected draft.") : error.c_str());
}

void MailWindow::render_message_rows() {
    for (size_t i = 0; i < rendered_row_count_; ++i) {
        row_checks_[i] = nullptr;
        row_buttons_[i] = nullptr;
    }
    rendered_row_count_ = 0;
    empty_rows_ = nullptr;
    message_scroll_->clear();
    message_scroll_->begin();
    int y = message_scroll_->y() + 4;
    for (size_t index = 0; index < rows_.size(); ++index) {
        Fl_Check_Button *check = new Fl_Check_Button(message_scroll_->x() + 3, y + 7, 21, 21);
        check->callback(checked_cb, reinterpret_cast<void *>(static_cast<intptr_t>(index) + 1));
        bool checked = false;
        for (size_t i = 0; i < checked_rows_.size(); ++i) if (checked_rows_[i] == index) checked = true;
        check->value(checked);
        const MessageQueryRow &row = rows_[index];
        const char *attachment = row.attachment_presence_known && row.has_attachments ? T("Has attachments") : "";
        char received_date[40];
        Text sender, subject, folder;
        const Text &folder_source = row.folder_display_name.empty() ?
            row.message.key.folder.server_name : row.folder_display_name;
        (void)task6_sanitize_single_line_display_text(row.message.from.c_str(), row.message.from.size(), sender);
        (void)task6_sanitize_single_line_display_text(row.message.subject.empty() ? T("(no subject)") :
            row.message.subject.c_str(), row.message.subject.empty() ? strlen(T("(no subject)")) : row.message.subject.size(), subject);
        (void)task6_sanitize_single_line_display_text(folder_source.c_str(), folder_source.size(), folder);
        format_row_date(row.message.internal_date, received_date, sizeof(received_date));
        char context[4608];
        snprintf(context, sizeof(context), "%s%s%s", folder.c_str(),
            row.message.headers_only ? "  ·  " : "",
            row.message.headers_only ? T("Headers only") : "");
        MailMessageRowButton *open = new MailMessageRowButton(message_scroll_->x() + 28, y,
            message_scroll_->w() - 32, kMessageRowHeight);
        if (!open->message(sender.c_str(), subject.c_str(), context, received_date, attachment,
                (row.message.flags & ImapFlagSeen) == 0,
                (row.message.flags & ImapFlagFlagged) != 0)) {
            (void)open->message(T("Message text unavailable"), T(""), "", received_date, "", false, false);
        }
        open->callback(message_cb, reinterpret_cast<void *>(static_cast<intptr_t>(index) + 1));
        if (row.message.key == selected_row_.message.key && selected_row_valid_) open->value(1);
        row_checks_[index] = check;
        row_buttons_[index] = open;
        ++rendered_row_count_;
        y += kMessageRowHeight + kMessageRowGap;
    }
    if (rows_.empty()) {
        empty_rows_ = new PlainTextBox(message_scroll_->x() + 18, y + 32, message_scroll_->w() - 36, 64,
            startup_complete_ ? T("No messages match this view.\nSynchronize headers or change filters.") :
                T("Loading the local profile..."));
        empty_rows_->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
        empty_rows_->labelsize(13);
    }
    message_scroll_->end();
    message_scroll_->redraw();
}

void MailWindow::layout_message_rows() {
    const int x = message_scroll_->x();
    const int width = message_scroll_->w();
    int y = message_scroll_->y() + 4;
    const size_t count = rendered_row_count_ < rows_.size() ? rendered_row_count_ : rows_.size();
    for (size_t i = 0; i < count; ++i) {
        if (row_checks_[i]) row_checks_[i]->resize(x + 3, y + 7, 21, 21);
        if (row_buttons_[i]) row_buttons_[i]->resize(x + 28, y, width - 32, kMessageRowHeight);
        y += kMessageRowHeight + kMessageRowGap;
    }
    if (empty_rows_) empty_rows_->resize(x + 18, y + 32, width - 36, 64);
}

void MailWindow::message_cb(Fl_Widget *widget, void *data) {
    MailWindow *window = static_cast<MailWindow *>(widget->window());
    const intptr_t index = reinterpret_cast<intptr_t>(data) - 1;
    if (index >= 0) window->select_message(static_cast<size_t>(index));
}

void MailWindow::checked_cb(Fl_Widget *widget, void *data) {
    MailWindow *window = static_cast<MailWindow *>(widget->window());
    const uint32_t index = static_cast<uint32_t>(reinterpret_cast<intptr_t>(data) - 1);
    const bool selected = static_cast<Fl_Check_Button *>(widget)->value() != 0;
    for (size_t i = 0; i < window->checked_rows_.size(); ++i) {
        if (window->checked_rows_[i] == index) {
            if (!selected) window->checked_rows_.erase(i);
            return;
        }
    }
    if (selected) (void)window->checked_rows_.push(index);
    window->selection_status_->text(selected ? T("Message added to the selection") : T("Message removed from the selection"));
}

void MailWindow::layout_content() {
    const int nav_top = kNavigationTop;
    const int nav_height = h() - nav_top - kStatusHeight;
    const int top = kContentTop;
    const int height = h() - top - kStatusHeight;
    const int width = w();
    const int main_x = left_pane_width_ + kDividerWidth;
    const int main_width = width - main_x - 6;
    left_pane_->resize(0, nav_top, left_pane_width_, nav_height);
    left_divider_->resize(left_pane_width_, nav_top, kDividerWidth, nav_height);
    left_divider_->show();
    account_folder_scroll_->resize(4, nav_top + 34, left_pane_width_ - 8,
        nav_height > 38 ? nav_height - 38 : 0);
    search_strip_->resize(main_x, nav_top, main_width, kSearchStripHeight);

    const int search_x = main_x + 8;
    const int search_y = nav_top + 5;
    const SearchStripLayout layout = search_strip_layout(main_width);
    search_input_->resize(search_x, search_y, layout.input_width, 26);
    int control_x = search_x + layout.input_width + 4;
    if (layout.show_field) {
        search_field_->show(); search_field_->resize(control_x, search_y, 72, 26); control_x += 76;
    } else search_field_->hide();
    if (layout.show_scope) {
        scope_choice_->show(); scope_choice_->resize(control_x, search_y, 84, 26); control_x += 88;
    } else scope_choice_->hide();
    search_button_->resize(control_x, search_y, 52, 26); control_x += 56;
    filter_button_->resize(control_x, search_y, 58, 26); control_x += 62;
    more_button_->resize(control_x, search_y, 58, 26);

    if (vertical_preview_) {
        int list_height = height;
        if (preview_visible_) {
            const int maximum_list_height = height - kDividerWidth - kVerticalReaderMinHeight;
            const int minimum_list_height = maximum_list_height < kVerticalListMinHeight ?
                maximum_list_height : kVerticalListMinHeight;
            if (!vertical_list_height_) vertical_list_height_ = (height - kDividerWidth) / 2;
            list_height = vertical_list_height_;
            if (list_height < minimum_list_height) list_height = minimum_list_height;
            if (list_height > maximum_list_height) list_height = maximum_list_height;
            vertical_list_height_ = list_height;
        }
        list_pane_->resize(main_x, top, main_width, list_height);
        if (preview_visible_) {
            preview_divider_->axis(false);
            preview_divider_->resize(main_x, top + list_height, main_width, kDividerWidth);
            preview_divider_->show();
            reader_pane_->resize(main_x, top + list_height + kDividerWidth, main_width,
                height - list_height - kDividerWidth);
        } else {
            preview_divider_->hide();
            reader_pane_->resize(main_x, top + height, main_width, 0);
        }
    } else {
        int list_width = main_width;
        if (preview_visible_) {
            const int maximum_list_width = main_width - kDividerWidth - kReaderMinWidth;
            if (!list_pane_width_) list_pane_width_ = maximum_list_width < 352 ? maximum_list_width : 352;
            list_width = list_pane_width_;
            const int minimum_list_width = maximum_list_width < kListMinWidth ? maximum_list_width : kListMinWidth;
            if (list_width < minimum_list_width) list_width = minimum_list_width;
            if (list_width > maximum_list_width) list_width = maximum_list_width;
            list_pane_width_ = list_width;
        }
        list_pane_->resize(main_x, top, list_width, height);
        if (preview_visible_) {
            preview_divider_->axis(true);
            preview_divider_->resize(main_x + list_width, top, kDividerWidth, height);
            preview_divider_->show();
            reader_pane_->resize(main_x + list_width + kDividerWidth, top,
                main_width - list_width - kDividerWidth, height);
        } else {
            preview_divider_->hide();
            reader_pane_->resize(width - 6, top, 0, height);
        }
    }
    const int list_height = vertical_preview_ && preview_visible_ ? list_pane_->h() : height;
    list_heading_->resize(list_pane_->x() + 10, list_pane_->y() + 5,
        list_pane_->w() - 20, 24);
    message_scroll_->resize(list_pane_->x() + 4, list_pane_->y() + 31,
        list_pane_->w() - 8, list_height - 35);
    layout_message_rows();
    if (preview_visible_) reader_pane_->show(); else reader_pane_->hide();
    bottom_->resize(0, h() - kStatusHeight, width, kStatusHeight);
    status_->resize(8, h() - kStatusHeight + 1, width / 2 - 12, kStatusHeight - 2);
    selection_status_->resize(width / 2, h() - kStatusHeight + 1, width / 2 - 8, kStatusHeight - 2);
    redraw();
}

void MailWindow::resize_left_pane(int delta) {
    const int main_minimum = preview_visible_ && !vertical_preview_ ?
        kListMinWidth + kDividerWidth + kReaderMinWidth : kListMinWidth;
    const int maximum = w() - 6 - kDividerWidth - main_minimum;
    int next = left_pane_width_ + delta;
    if (next < kLeftMinWidth) next = kLeftMinWidth;
    if (next > maximum) next = maximum;
    if (next == left_pane_width_) return;
    left_pane_width_ = next;
    layout_content();
}

void MailWindow::resize_preview_split(int delta) {
    if (!preview_visible_) return;
    if (vertical_preview_) {
        const int height = h() - kContentTop - kStatusHeight;
        const int maximum = height - kDividerWidth - kVerticalReaderMinHeight;
        int next = vertical_list_height_ ? vertical_list_height_ + delta :
            (height - kDividerWidth) / 2 + delta;
        if (next < kVerticalListMinHeight) next = kVerticalListMinHeight;
        if (next > maximum) next = maximum;
        if (next == vertical_list_height_) return;
        vertical_list_height_ = next;
    } else {
        const int main_width = w() - left_pane_width_ - kDividerWidth - 6;
        const int maximum = main_width - kDividerWidth - kReaderMinWidth;
        int next = list_pane_width_ + delta;
        if (next < kListMinWidth) next = kListMinWidth;
        if (next > maximum) next = maximum;
        if (next == list_pane_width_) return;
        list_pane_width_ = next;
    }
    layout_content();
}

void MailWindow::resize(int x, int y, int width, int height) {
    Fl_Double_Window::resize(x, y, width, height);
    if (!left_pane_ || !list_pane_ || !reader_pane_) return;
    top_->resize(0, 0, width, kTopHeight);
    quick_compose_->resize(width - 180, 4, 82, 26);
    quick_sync_->resize(width - 92, 4, 84, 26);
    ribbon_->resize(0, kTopHeight, width, kRibbonHeight);
    ribbon_scroll_->resize(0, kTopHeight, width, kRibbonHeight);
    layout_content();
}

}
