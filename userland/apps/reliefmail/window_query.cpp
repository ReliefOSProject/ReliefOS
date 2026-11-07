#include "window.h"
#include "action_policy.h"
#include "imap.h"
#include "../fltk_support.h"
#include <FL/Fl.H>
#include <limits.h>

namespace rmail {
namespace {
constexpr uint32_t kPageSize = 50;

bool set_error(Text &error, const char *message) {
    error.set(message ? message : T("The query is invalid."));
    return false;
}
}

void MailWindow::schedule_query(bool immediate, bool preserve_selection) {
    Fl::remove_timeout(search_timeout_cb, this);
    preserve_selection_on_query_ = preserve_selection;
    if (immediate) submit_query();
    else Fl::add_timeout(.30, search_timeout_cb, this);
}

bool MailWindow::build_query(MessageQuerySpec &query, Text &error) const {
    MessageQuerySpec next;
    apply_search_selection(next, filter_settings_.search);
    next.sort = filter_settings_.sort;
    next.limit = kPageSize;
    next.required_flags = filter_settings_.starred_only ? ImapFlagFlagged : 0;
    next.excluded_flags = filter_settings_.unread_only ? ImapFlagSeen : 0;
    next.attachment_filter = filter_settings_.attachments;
    if (next.scope == MessageQueryScope::Folder) {
        if (!selected_folder_valid_ || selected_folder_.key.account_id != selected_account_id_)
            return set_error(error, T("Choose a real folder before using folder scope."));
        next.folder = selected_folder_.key;
        next.account_id = selected_folder_.key.account_id;
    } else if (next.scope == MessageQueryScope::Account) {
        if (!selected_account_id_) return set_error(error, T("Choose an account before using account scope."));
        next.account_id = selected_account_id_;
    }
    if (filter_settings_.received_after.size()) {
        if (!task6_parse_received_date(filter_settings_.received_after.c_str(), next.received_after, error)) return false;
        next.has_received_after = true;
    }
    if (filter_settings_.received_before.size()) {
        if (!task6_parse_received_date(filter_settings_.received_before.c_str(), next.received_before, error)) return false;
        if (next.received_before > INT64_MAX - 86399) return set_error(error, T("The received date is out of range."));
        next.received_before += 86399;
        next.has_received_before = true;
    }
    if (next.has_received_after && next.has_received_before && next.received_after > next.received_before)
        return set_error(error, T("Received-after date must not be later than received-before."));
    if (search_input_ && search_input_->value() && !next.query.set(search_input_->value()))
        return set_error(error, T("Not enough memory to copy the search text."));
    query = static_cast<MessageQuerySpec &&>(next);
    error.clear();
    return true;
}

void MailWindow::submit_query(bool more) {
    if (!startup_complete_ || closing_) return;
    MessageQuerySpec query;
    Text error;
    if (more) {
        if (rows_.empty() || rows_.size() >= MaxRowsPerPage) {
            set_status(T("No more cached rows can be added to this view.")); return;
        }
        query = current_query_;
        query.offset = rows_.size();
        query.limit = kPageSize;
    } else {
        if (!build_query(query, error)) { set_status(error.c_str()); return; }
        query.offset = 0;
        const bool preserve = preserve_selection_on_query_ && selected_row_valid_;
        if (!preserve) {
            ++ui_generation_;
            ++selection_generation_;
            abort_export(T("Export stopped because the search view changed."));
        }
        checked_rows_.clear();
        if (!preserve) {
            selected_row_valid_ = false;
            reader_pane_->clear_message(T("Select a cached message to read it."));
            selection_status_->text(T("No message selected"));
        }
        rows_.clear();
        render_message_rows();
        current_query_ = query;
        more_button_->deactivate();
    }
    WorkerTaskInput input;
    input.page_query = query;
    WorkerRequest request;
    if (query.scope == MessageQueryScope::Folder) {
        request.account_id = query.account_id;
        request.folder_server_name = query.folder.server_name;
        for (size_t i = 0; i < startup_.accounts.size(); ++i)
            if (startup_.accounts[i].id == query.account_id)
                request.account_generation = startup_.accounts[i].generation;
        for (size_t i = 0; i < startup_.folders.size(); ++i)
            if (startup_.folders[i].key == query.folder) {
                request.folder_generation = startup_.folders[i].generation;
                request.folder_uidvalidity = startup_.folders[i].uidvalidity;
                break;
            }
    } else if (query.scope == MessageQueryScope::Account) {
        request.account_id = query.account_id;
        for (size_t i = 0; i < startup_.accounts.size(); ++i)
            if (startup_.accounts[i].id == query.account_id) request.account_generation = startup_.accounts[i].generation;
    }

    WorkerTaskKind kind = query.query.empty() ? WorkerTaskKind::MessagePage : WorkerTaskKind::MessageSearch;
    SecretBytes secret;
    if (filter_settings_.server_search) {
        if (!task6_validate_server_search_query(query, error)) { set_status(error.c_str()); return; }
        const Account *account = nullptr;
        for (size_t i = 0; i < startup_.accounts.size(); ++i)
            if (startup_.accounts[i].id == query.account_id) account = &startup_.accounts[i];
        if (!account) { set_status(T("Select an account with a real folder before server search.")); return; }
        if (!make_live_credentials(secret, error)) { set_status(error.c_str()); return; }
        SecretBytes check;
        if (!worker_find_credential(secret, *account, CredentialPurpose::Imap, account->username,
                coordinator_.profile_epoch(), check, error)) {
            set_status(T("Unlock or enter the selected account's incoming password before server search.")); return;
        }
        kind = WorkerTaskKind::ServerSearch;
    }
    PendingUiRequest pending;
    pending.purpose = PendingPurpose::Query;
    const bool carry_action_diagnostic = !more && !action_refresh_diagnostic_.empty();
    if (carry_action_diagnostic)
        pending.tool_action_result_diagnostic.set(action_refresh_diagnostic_.c_str());
    if (!submit_task(kind, request, &input, static_cast<SecretBytes &&>(secret),
            static_cast<PendingUiRequest &&>(pending), error)) {
        if (carry_action_diagnostic)
            set_status(task8a_action_refresh_status(action_refresh_diagnostic_, error));
        else set_status(error.empty() ? T("Could not queue this search") : error.c_str());
    } else if (carry_action_diagnostic) {
        action_refresh_diagnostic_.clear();
    }
}

void MailWindow::open_filter_dialog() {
    QueryFilterDialog dialog(filter_settings_);
    QueryFilterSettings next = filter_settings_;
    if (dialog.run(next)) {
        if (next.search.scope == MessageQueryScope::Folder &&
            (!selected_folder_valid_ || selected_folder_.key.account_id != selected_account_id_)) {
            set_status(T("Select a real folder before using folder scope."));
            return;
        }
        filter_settings_ = static_cast<QueryFilterSettings &&>(next);
        search_field_->value(message_query_field_choice(filter_settings_.search.field));
        scope_choice_->value(message_query_scope_choice(filter_settings_.search.scope));
        all_mail_navigation_selected_ = filter_settings_.search.scope == MessageQueryScope::AllMail;
        update_navigation_selection();
        schedule_query(true);
    }
}

void MailWindow::sync_selected_folder() {
    if (!selected_account_id_ || !selected_folder_valid_) {
        set_status(T("Select an account and a selectable folder before synchronizing headers.")); return;
    }
    const Account *account = nullptr;
    for (size_t i = 0; i < startup_.accounts.size(); ++i)
        if (startup_.accounts[i].id == selected_account_id_) account = &startup_.accounts[i];
    if (!account) { set_status(T("The selected account is no longer available.")); return; }
    SecretBytes packet, check;
    Text error;
    if (!make_live_credentials(packet, error) ||
        !worker_find_credential(packet, *account, CredentialPurpose::Imap,
            account->username, coordinator_.profile_epoch(), check, error)) {
        set_status(T("Unlock the password vault or add this account's incoming password before syncing.")); return;
    }
    WorkerTaskInput input;
    input.page_query.scope = MessageQueryScope::Folder;
    input.page_query.account_id = selected_account_id_;
    input.page_query.folder = selected_folder_.key;
    input.page_query.limit = kPageSize;
    WorkerRequest request;
    request.account_id = selected_account_id_;
    request.account_generation = account->generation;
    request.folder_server_name = selected_folder_.key.server_name;
    request.folder_generation = selected_folder_.generation;
    request.folder_uidvalidity = selected_folder_.uidvalidity;
    PendingUiRequest pending;
    pending.purpose = PendingPurpose::HeaderSync;
    pending.account = *account;
    pending.folder = selected_folder_;
    ++ui_generation_;
    ++selection_generation_;
    rows_.clear();
    selected_row_valid_ = false;
    checked_rows_.clear();
    reader_pane_->clear_message(T("Synchronizing headers. Message bodies are not fetched yet."));
    render_message_rows();
    if (!submit_task(WorkerTaskKind::HeaderSync, request, &input,
            static_cast<SecretBytes &&>(packet), static_cast<PendingUiRequest &&>(pending), error))
        set_status(error.c_str());
}

bool MailWindow::submit_row_task(WorkerTaskKind kind, const MessageQueryRow &row,
    WorkerTaskInput &input, SecretBytes secret, PendingPurpose purpose,
    uint64_t selection_generation, Text &error, PendingUiRequest extra) {
    WorkerRequest request;
    const uint64_t request_id = next_request_id_++;
    if (!task6_build_message_request(row, request_id, kind, coordinator_.profile_epoch(),
            ui_generation_, request, error)) return false;
    request.folder_server_name = row.message.key.folder.server_name;
    request.folder_generation = row.folder_generation;
    request.folder_uidvalidity = row.message.key.uidvalidity;
    request.message_uid = row.message.key.uid;
    extra.purpose = purpose;
    extra.row = row;
    extra.selection_generation = selection_generation;
    return submit_task(kind, request, &input, static_cast<SecretBytes &&>(secret),
        static_cast<PendingUiRequest &&>(extra), error);
}

void MailWindow::select_message(size_t index) {
    if (index >= rows_.size()) return;
    selected_row_ = rows_[index];
    selected_row_valid_ = true;
    selected_message_valid_ = false;
    for (size_t i = 0; i < rendered_row_count_; ++i) {
        if (!row_buttons_[i]) continue;
        row_buttons_[i]->value(i == index);
        row_buttons_[i]->redraw();
    }
    ++selection_generation_;
    reader_pane_->clear_message(T("Loading this message from the local cache..."));
    selection_status_->text(selected_row_.message.subject.c_str());
    update_ribbon_state();
    WorkerTaskInput input;
    input.message_key = selected_row_.message.key;
    Text error;
    if (!submit_row_task(WorkerTaskKind::ParseMessage, selected_row_, input, SecretBytes(),
            PendingPurpose::ParseMessage, selection_generation_, error, PendingUiRequest())) {
        reader_pane_->clear_message(error.c_str());
        set_status(error.c_str());
    }
}

}
