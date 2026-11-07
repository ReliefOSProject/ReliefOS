#include "window.h"
#include "action_policy.h"
#include "imap.h"
#include "../fltk_support.h"
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Double_Window.H>
#include <FL/fl_ask.H>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace rmail {
namespace {

const Account *find_account(const AccountList &accounts, uint64_t id) {
    for (size_t i = 0; i < accounts.size(); ++i)
        if (accounts[i].id == id) return &accounts[i];
    return nullptr;
}

const Folder *find_folder(const FolderList &folders, const FolderKey &key) {
    for (size_t i = 0; i < folders.size(); ++i)
        if (folders[i].key == key) return &folders[i];
    return nullptr;
}

bool append_bytes(Array<char> &output, const char *bytes, size_t size) {
    if ((!bytes && size) || size > 256u * 1024u - output.size()) return false;
    for (size_t i = 0; i < size; ++i)
        if (!output.push(bytes[i])) return false;
    return true;
}

bool message_identity_details(const BoundedArray<MessageQueryRow, MaxWorkerOutstanding> &rows,
    const Folder *destination, Text &details) {
    Array<char> output;
    char line[4608];
    for (size_t i = 0; i < rows.size(); ++i) {
        const MessageKey &key = rows[i].message.key;
        const int written = snprintf(line, sizeof(line), T("Account %llu · %s · UIDVALIDITY %llu · UID %u\n"),
            static_cast<unsigned long long>(key.folder.account_id), key.folder.server_name.c_str(),
            static_cast<unsigned long long>(key.uidvalidity), key.uid);
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(line) ||
            !append_bytes(output, line, static_cast<size_t>(written))) return false;
    }
    if (destination) {
        const int written = snprintf(line, sizeof(line), T("Destination account %llu · %s · folder generation %llu · UIDVALIDITY %llu\n"),
            static_cast<unsigned long long>(destination->key.account_id), destination->key.server_name.c_str(),
            static_cast<unsigned long long>(destination->generation),
            static_cast<unsigned long long>(destination->uidvalidity));
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(line) ||
            !append_bytes(output, line, static_cast<size_t>(written))) return false;
    }
    if (!output.push('\0')) return false;
    return details.set(output.data(), output.size() - 1);
}

const char *action_word(UiCommand command) {
    switch (command) {
    case UiCommand::ArchiveMessage: return "ARCHIVE";
    case UiCommand::JunkMessage: return "JUNK";
    case UiCommand::TrashMessage: return "TRASH";
    case UiCommand::MoveMessage: return "MOVE";
    case UiCommand::CopyMessage: return "COPY";
    case UiCommand::DeleteMessage: return "DELETE";
    default: return "ACTION";
    }
}

struct FolderChoiceState {
    Fl_Double_Window *window = nullptr;
    Fl_Choice *choice = nullptr;
    BoundedArray<FolderKey, MaxFoldersPerAccount> folders;
    bool accepted = false;
};

struct FolderChoiceButton {
    FolderChoiceState *state = nullptr;
    bool accept = false;
};

void folder_choice_cb(Fl_Widget *, void *context) {
    FolderChoiceButton *button = static_cast<FolderChoiceButton *>(context);
    if (!button || !button->state || !button->state->window) return;
    if (button->accept) {
        const int selected = button->state->choice->value();
        button->state->accepted = selected >= 0 &&
            static_cast<size_t>(selected) < button->state->folders.size();
    }
    button->state->window->hide();
}

void folder_choice_close_cb(Fl_Widget *, void *context) {
    FolderChoiceState *state = static_cast<FolderChoiceState *>(context);
    if (state) {
        state->accepted = false;
        if (state->window) state->window->hide();
    }
}

bool choose_destination(const FolderList &folders, uint64_t account_id, FolderKey &key) {
    FolderChoiceState state;
    Fl_Double_Window dialog(620, 142, T("Choose destination folder"));
    dialog.set_modal();
    state.window = &dialog;
    dialog.callback(folder_choice_close_cb, &state);
    dialog.begin();
    Fl_Box *folder_label = new Fl_Box(18, 14, 584, 18, T("Folder"));
    folder_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    state.choice = new Fl_Choice(18, 38, 584, 28);
    for (size_t i = 0; i < folders.size(); ++i) {
        const Folder &folder = folders[i];
        if (!folder.selectable || folder.key.account_id != account_id) continue;
        const char *display = folder.display_name.empty() ? folder.key.server_name.c_str() : folder.display_name.c_str();
        char label[8448];
        snprintf(label, sizeof(label), "%s   [%s]", display, folder.key.server_name.c_str());
        Text clean, safe;
        if (!task6_sanitize_single_line_display_text(label, strlen(label), clean) ||
            !task6_escape_fltk_label_symbols(clean.c_str(), clean.size(), safe) ||
            !state.folders.push(folder.key)) continue;
        state.choice->add(safe.c_str());
    }
    Fl_Button *cancel = new Fl_Button(344, 88, 118, 30, T("Cancel"));
    Fl_Button *accept = new Fl_Button(478, 88, 124, 30, T("Choose"));
    FolderChoiceButton cancel_action{&state, false};
    FolderChoiceButton accept_action{&state, true};
    cancel->callback(folder_choice_cb, &cancel_action);
    accept->callback(folder_choice_cb, &accept_action);
    dialog.end();
    if (state.folders.empty()) return false;
    state.choice->value(0);
    cancel->take_focus();
    dialog.show();
    while (dialog.shown()) Fl::wait();
    if (!state.accepted) return false;
    const int selected = state.choice->value();
    if (selected < 0 || static_cast<size_t>(selected) >= state.folders.size()) return false;
    key = state.folders[static_cast<size_t>(selected)];
    return true;
}

bool send_operation_details(const SendOperationSummary &operation, Text &details) {
    if (operation.sender.size() > 4096 || operation.recipients.size() > 220u * 1024u ||
        operation.last_diagnostic.size() > 4096 || operation.message_id.size() > 998)
        return false;
    Text sender, recipients, diagnostic;
    if (!task6_sanitize_display_text(operation.sender.c_str(), operation.sender.size(), sender) ||
        !task6_sanitize_display_text(operation.recipients.c_str(), operation.recipients.size(), recipients) ||
        !task6_sanitize_display_text(operation.last_diagnostic.c_str(), operation.last_diagnostic.size(), diagnostic))
        return false;
    char header[2048];
    const int written = snprintf(header, sizeof(header),
        T("Operation #%llu\nMessage-ID: %s\nStage %u · delivery count %u\nFrom: %s\nTo: "),
        static_cast<unsigned long long>(operation.id), operation.message_id.c_str(),
        static_cast<unsigned>(operation.stage), operation.delivery_count,
        sender.c_str());
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(header)) return false;
    Array<char> output;
    if (!append_bytes(output, header, static_cast<size_t>(written)) ||
        !append_bytes(output, recipients.c_str(), recipients.size()) ||
        !append_bytes(output, "\n", 1) ||
        !append_bytes(output, diagnostic.c_str(), diagnostic.size()) || !output.push('\0')) return false;
    return details.set(output.data(), output.size() - 1);
}

bool clone_startup_snapshot(const WorkerStartupSnapshot &source, WorkerStartupSnapshot &destination) {
    WorkerStartupSnapshot next;
    next.selected_account_id = source.selected_account_id;
    if (!next.accounts.clone_from(source.accounts) || !next.folders.clone_from(source.folders))
        return false;
    destination = static_cast<WorkerStartupSnapshot &&>(next);
    return true;
}

}

bool MailWindow::make_imap_credentials_for(const Account &account,
    SecretBytes &packet, Text &error) const {
    SecretBytes password;
    return make_live_credentials(packet, error) &&
        worker_find_credential(packet, account, CredentialPurpose::Imap, account.username,
            coordinator_.profile_epoch(), password, error);
}

bool MailWindow::make_smtp_credentials_for(const Account &account,
    SecretBytes &packet, Text &error) const {
    const Text &username = account.smtp_username.empty() ? account.username : account.smtp_username;
    SecretBytes password;
    return make_live_credentials(packet, error) &&
        worker_find_credential(packet, account, CredentialPurpose::Smtp, username,
            coordinator_.profile_epoch(), password, error);
}

void MailWindow::open_tools_window(int page) {
    if (!startup_complete_ || closing_) {
        set_status(T("The local profile is still loading."));
        return;
    }
    if (!tools_window_) {
        tools_window_ = new MailToolsWindow(this);
        reliefos_fltk::style(tools_window_);
    }
    tools_window_->refresh_snapshot(startup_, selected_account_id_);
    tools_window_->select_page(page);
    tools_window_->show();
    if (tools_window_->selected_account_id() &&
        tools_window_->selected_account_id() != selected_account_id_) {
        tools_account_changed(tools_window_->selected_account_id());
    } else if (page == 1 && tools_window_->selected_account_id() &&
        !tools_window_->queue_page_loaded()) {
        refresh_tool_queue(tools_window_->selected_account_id());
    }
}

void MailWindow::tools_account_changed(uint64_t account_id) {
    const Account *account = find_account(startup_.accounts, account_id);
    if (!tools_window_ || !account) {
        if (tools_window_) tools_window_->set_status(T("The selected account is no longer available."));
        return;
    }
    refresh_tool_queue(account_id);
    submit_tool_folder_operation(WorkerFolderAction::List, nullptr);
}

bool MailWindow::submit_tool_task(WorkerTaskKind kind, WorkerRequest request,
    WorkerTaskInput *input, SecretBytes secret, PendingUiRequest pending, Text &error) {
    pending.purpose = PendingPurpose::ToolTask;
    pending.tool_kind = kind;
    const bool tool_window_task = pending.tool_window_task;
    if (!submit_task(kind, request, input, static_cast<SecretBytes &&>(secret),
            static_cast<PendingUiRequest &&>(pending), error)) return false;
    if (tool_window_task) {
        ++tool_pending_count_;
        if (tools_window_) tools_window_->set_busy(true);
    }
    return true;
}

void MailWindow::set_action_continuation_status(const char *message) {
    const char *diagnostic = message && *message ? message :
        T("The original message selection changed while refreshing the destination.");
    action_refresh_diagnostic_.set(diagnostic);
    last_error_.set(diagnostic);
    set_status(diagnostic);
    for (size_t i = 0; i < pending_.size(); ++i) {
        PendingUiRequest &pending = pending_[i];
        if (pending.purpose == PendingPurpose::Query &&
            pending.token.profile_epoch == coordinator_.profile_epoch() &&
            pending.token.ui_generation == ui_generation_) {
            pending.tool_action_result_diagnostic.set(diagnostic);
            action_refresh_diagnostic_.clear();
        }
    }
}

void MailWindow::refresh_tool_queue(uint64_t account_id, uint64_t offset) {
    if (!tools_window_) return;
    const Account *account = find_account(startup_.accounts, account_id);
    if (!account) {
        tools_window_->set_status(T("The selected account is no longer available."));
        return;
    }
    WorkerTaskInput input;
    input.page_offset = offset;
    WorkerRequest request;
    request.account_id = account->id;
    request.account_generation = account->generation;
    PendingUiRequest pending;
    pending.account = *account;
    pending.tool_window_task = true;
    Text error;
    if (!submit_tool_task(WorkerTaskKind::ListSendOperations, request, &input,
            SecretBytes(), static_cast<PendingUiRequest &&>(pending), error)) {
        tools_window_->set_status(error.empty() ? T("Could not load send history.") : error.c_str());
        set_status(error.c_str());
    }
}

void MailWindow::submit_tool_folder_operation(WorkerFolderAction action,
    const Folder *folder, const char *query) {
    if (!tools_window_) return;
    const uint64_t account_id = tools_window_->selected_account_id();
    const Account *account = find_account(startup_.accounts, account_id);
    if (!account || !account->enabled) {
        tools_window_->set_status(T("Choose an enabled account before using its server folders."));
        return;
    }
    WorkerTaskInput input;
    input.folder_action = action;
    if (query && !input.query.set(query)) {
        tools_window_->set_status(T("Could not copy the folder name."));
        return;
    }
    WorkerRequest request;
    request.account_id = account->id;
    request.account_generation = account->generation;
    if (folder) {
        if (folder->key.account_id != account->id || !folder->generation) {
            tools_window_->set_status(T("The selected folder identity is stale."));
            return;
        }
        request.folder_server_name = folder->key.server_name;
        request.folder_generation = folder->generation;
        request.folder_uidvalidity = folder->uidvalidity;
        input.page_query.folder = folder->key;
    }
    SecretBytes secret;
    Text error;
    if (!make_imap_credentials_for(*account, secret, error)) {
        tools_window_->set_status(error.empty() ? T("Enter the current account's incoming password first.") : error.c_str());
        return;
    }
    PendingUiRequest pending;
    pending.account = *account;
    pending.folder = folder ? *folder : Folder();
    pending.tool_window_task = true;
    pending.tool_folder_action = action;
    pending.tool_folder_key = folder ? folder->key : FolderKey();
    if (!submit_tool_task(WorkerTaskKind::FolderOperation, request, &input,
            static_cast<SecretBytes &&>(secret), static_cast<PendingUiRequest &&>(pending), error)) {
        tools_window_->set_status(error.empty() ? T("Could not queue the folder operation.") : error.c_str());
        set_status(error.c_str());
    }
}

void MailWindow::submit_tool_send_review(WorkerTaskKind kind,
    const SendOperationSummary &operation, const Folder *target,
    WorkerSendReviewAction action) {
    if (!tools_window_) return;
    const Account *account = find_account(startup_.accounts, operation.account_id);
    if (!account || tools_window_->selected_account_id() != operation.account_id ||
        !account->enabled || !operation.account_generation) {
        tools_window_->set_status(T("The operation no longer matches the selected account."));
        return;
    }
    WorkerTaskInput input;
    input.item_id = operation.id;
    input.item_revision = operation.account_generation;
    input.send_review_action = action;
    input.review_confirmed = true;
    WorkerRequest request;
    request.account_id = account->id;
    request.account_generation = account->generation;
    if (kind == WorkerTaskKind::ReviewArchiveTarget) {
        if (!target || target->key.account_id != account->id || !target->selectable ||
            !target->generation || !target->uidvalidity) {
            tools_window_->set_status(T("Select and refresh a selectable folder in this same account."));
            return;
        }
        input.destination_folder = target->key;
        input.destination_folder_generation = target->generation;
        input.destination_folder_uidvalidity = target->uidvalidity;
    }
    if (kind == WorkerTaskKind::ResolveUncertainSend) {
        Text details;
        if (!send_operation_details(operation, details)) {
            tools_window_->set_status(T("Could not prepare the bounded delivery review."));
            return;
        }
        const bool delivered = action == WorkerSendReviewAction::ConfirmDelivered;
        char phrase[64];
        snprintf(phrase, sizeof(phrase), "%s %llu", delivered ? "DELIVERED" : "NOT DELIVERED",
            static_cast<unsigned long long>(operation.id));
        const char *prompt = delivered ?
            T("You are recording a user decision that delivery succeeded. This decision will not contact SMTP.") :
            T("You are recording a user decision that delivery did not succeed. This decision will not send; SMTP retry requires a second confirmation.");
        if (!literal_confirmation(T("Review uncertain delivery"), prompt, details.c_str(),
                T("Cancel"), T("Continue")) ||
            !literal_text_confirmation(T("Confirm delivery review"),
                T("Type the exact decision to record."), phrase, T("Cancel"), T("Record review"))) return;
    } else if (kind == WorkerTaskKind::ReviewArchiveTarget) {
        Text details;
        if (!send_operation_details(operation, details)) {
            tools_window_->set_status(T("Could not prepare the bounded archive review."));
            return;
        }
        const size_t details_size = details.size();
        char target_line[4608];
        const int written = snprintf(target_line, sizeof(target_line),
            T("\nCurrent account generation %llu\nExact IMAP destination: %s\nFolder generation %llu · UIDVALIDITY %llu\n"),
            static_cast<unsigned long long>(account->generation), target->key.server_name.c_str(),
            static_cast<unsigned long long>(target->generation),
            static_cast<unsigned long long>(target->uidvalidity));
        Array<char> extended;
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(target_line) ||
            !append_bytes(extended, details.c_str(), details_size) ||
            !append_bytes(extended, target_line, static_cast<size_t>(written)) || !extended.push('\0')) {
            tools_window_->set_status(T("Could not prepare the exact archive destination review."));
            return;
        }
        Text full_details;
        if (!full_details.set(extended.data(), extended.size() - 1)) {
            tools_window_->set_status(T("Could not prepare the exact archive destination review."));
            return;
        }
        char phrase[48];
        snprintf(phrase, sizeof(phrase), "REVIEW %llu", static_cast<unsigned long long>(operation.id));
        if (!literal_confirmation(T("Review Sent-folder destination"),
                T("SMTP delivery is already accepted or explicitly reviewed. This confirmation only binds a current IMAP folder; it never resends SMTP."),
                full_details.c_str(), T("Cancel"), T("Continue")) ||
            !literal_text_confirmation(T("Confirm archive destination"),
                T("Type the operation review phrase to save this exact IMAP destination."),
                phrase, T("Cancel"), T("Confirm folder"))) return;
    }
    Text error;
    PendingUiRequest pending;
    pending.account = *account;
    pending.tool_operation = operation;
    pending.tool_has_operation = true;
    pending.tool_window_task = true;
    if (!submit_tool_task(kind, request, &input, SecretBytes(),
            static_cast<PendingUiRequest &&>(pending), error)) {
        tools_window_->set_status(error.empty() ? T("Could not queue the delivery review.") : error.c_str());
        set_status(error.c_str());
    }
}

void MailWindow::perform_tool(MailToolsCommand command) {
    if (!tools_window_ || closing_) return;
    const uint64_t account_id = tools_window_->selected_account_id();
    const Account *account = find_account(startup_.accounts, account_id);
    if (!account || !tools_window_->selected_account() ||
        tools_window_->selected_account()->generation != account->generation) {
        tools_window_->set_status(T("Refresh the account list before using this account."));
        return;
    }
    const Folder *folder = tools_window_->selected_folder();
    switch (command) {
    case MailToolsCommand::RefreshFolders:
        submit_tool_folder_operation(WorkerFolderAction::List, nullptr);
        return;
    case MailToolsCommand::SubscribeFolder:
    case MailToolsCommand::UnsubscribeFolder:
        if (!folder) { tools_window_->set_status(T("Select a selectable server folder first.")); return; }
        submit_tool_folder_operation(command == MailToolsCommand::SubscribeFolder ?
            WorkerFolderAction::Subscribe : WorkerFolderAction::Unsubscribe, folder);
        return;
    case MailToolsCommand::CreateFolder: {
        const char *name = fl_input("%s", "", T("New server folder name"));
        if (!name || !*name) return;
        submit_tool_folder_operation(WorkerFolderAction::Create, nullptr, name);
        return;
    }
    case MailToolsCommand::RenameFolder: {
        if (!folder) { tools_window_->set_status(T("Select a selectable server folder first.")); return; }
        const char *name = fl_input("%s", folder->key.server_name.c_str(), T("New server folder name"));
        if (!name || !*name) return;
        char phrase[48];
        snprintf(phrase, sizeof(phrase), "RENAME %llu", static_cast<unsigned long long>(account->id));
        if (!literal_confirmation(T("Rename server folder"),
                T("Renaming may change server-side message locations. Review the wire name before continuing."),
                folder->key.server_name.c_str(), T("Cancel"), T("Continue")) ||
            !literal_text_confirmation(T("Confirm folder rename"),
                T("Type the account-bound rename phrase."), phrase, T("Cancel"), T("Rename"))) return;
        submit_tool_folder_operation(WorkerFolderAction::Rename, folder, name);
        return;
    }
    case MailToolsCommand::DeleteFolder: {
        if (!folder) { tools_window_->set_status(T("Select a selectable server folder first.")); return; }
        char phrase[48];
        snprintf(phrase, sizeof(phrase), "DELETE FOLDER %llu", static_cast<unsigned long long>(account->id));
        if (!literal_confirmation(T("Delete server folder"),
                T("This deletes the folder on the server. The special Inbox, Sent, Drafts, Trash, Junk, and Archive mappings cannot be deleted here."),
                folder->key.server_name.c_str(), T("Cancel"), T("Continue")) ||
            !literal_text_confirmation(T("Confirm folder deletion"),
                T("Type the account-bound delete phrase."), phrase, T("Cancel"), T("Delete"))) return;
        submit_tool_folder_operation(WorkerFolderAction::Delete, folder);
        return;
    }
    case MailToolsCommand::ClearTrash: {
        if (!folder || !folder->selectable ||
            (folder->role != FolderRole::Trash && folder->key.server_name != account->trash_folder)) {
            tools_window_->set_status(T("Select the configured, selectable Trash folder first."));
            return;
        }
        char phrase[48];
        snprintf(phrase, sizeof(phrase), "CLEAR TRASH %llu", static_cast<unsigned long long>(account->id));
        char detail[4608];
        snprintf(detail, sizeof(detail), T("Account %llu\nServer folder: %s\nFolder generation %llu · UIDVALIDITY %llu\nDeletes are targeted by exact UIDPLUS identity."),
            static_cast<unsigned long long>(account->id), folder->key.server_name.c_str(),
            static_cast<unsigned long long>(folder->generation),
            static_cast<unsigned long long>(folder->uidvalidity));
        if (!literal_confirmation(T("Clear Trash"),
                T("This permanently deletes messages in the configured Trash folder. It never issues an unrestricted EXPUNGE."),
                detail, T("Cancel"), T("Continue")) ||
            !literal_text_confirmation(T("Confirm Trash clear"),
                T("Type the account-bound Trash phrase."), phrase, T("Cancel"), T("Clear Trash"))) return;
        submit_tool_folder_operation(WorkerFolderAction::ClearTrash, folder);
        return;
    }
    case MailToolsCommand::RefreshQueue:
        refresh_tool_queue(account_id);
        return;
    case MailToolsCommand::PreviousQueuePage:
        if (tools_window_->queue_page_loaded() && tools_window_->queue_offset() >= MaxSendOperationPageSize)
            refresh_tool_queue(account_id, tools_window_->queue_offset() - MaxSendOperationPageSize);
        return;
    case MailToolsCommand::NextQueuePage:
        if (tools_window_->queue_page_loaded() && tools_window_->queue_has_more())
            refresh_tool_queue(account_id, tools_window_->queue_offset() + MaxSendOperationPageSize);
        return;
    case MailToolsCommand::ReviewArchiveTarget: {
        const SendOperationSummary *operation = tools_window_->selected_operation();
        const Folder *target = tools_window_->selected_archive_target();
        if (!operation || !target ||
            !task8a_archive_review_target_valid(*operation, account_id, target)) {
            tools_window_->set_status(T("Select a delivered operation and a selectable same-account destination."));
            return;
        }
        if (operation->account_generation == 0) {
            tools_window_->set_status(T("The operation has no durable source generation and cannot be reviewed."));
            return;
        }
        SecretBytes secret;
        Text error;
        if (!make_imap_credentials_for(*account, secret, error)) {
            tools_window_->set_status(error.empty() ? T("Enter this account's current incoming password before checking the destination.") : error.c_str());
            return;
        }
        WorkerTaskInput input;
        input.page_query.folder = target->key;
        WorkerRequest request;
        request.account_id = account->id;
        request.account_generation = account->generation;
        request.folder_server_name = target->key.server_name;
        request.folder_generation = target->generation;
        request.folder_uidvalidity = target->uidvalidity;
        PendingUiRequest pending;
        pending.account = *account;
        pending.folder = *target;
        pending.tool_operation = *operation;
        pending.tool_has_operation = true;
        pending.tool_folder_key = target->key;
        pending.tool_window_task = true;
        if (!submit_tool_task(WorkerTaskKind::FolderSelect, request, &input,
                static_cast<SecretBytes &&>(secret), static_cast<PendingUiRequest &&>(pending), error)) {
            tools_window_->set_status(error.empty() ? T("Could not refresh the selected archive folder.") : error.c_str());
            set_status(error.c_str());
        }
        return;
    }
    case MailToolsCommand::ConfirmDelivered:
    case MailToolsCommand::ConfirmNotDelivered: {
        const SendOperationSummary *operation = tools_window_->selected_operation();
        if (!operation || operation->account_id != account_id || operation->stage != SendStage::Uncertain) {
            tools_window_->set_status(T("Select a send operation whose SMTP delivery is uncertain."));
            return;
        }
        submit_tool_send_review(WorkerTaskKind::ResolveUncertainSend, *operation, nullptr,
            command == MailToolsCommand::ConfirmDelivered ?
                WorkerSendReviewAction::ConfirmDelivered : WorkerSendReviewAction::ConfirmNotDelivered);
        return;
    }
    case MailToolsCommand::RetryArchive: {
        const SendOperationSummary *operation = tools_window_->selected_operation();
        if (!operation || !task8a_archive_retryable(*operation, account_id, account->generation)) {
            tools_window_->set_status(T("Review and confirm a current Sent-folder destination first."));
            return;
        }
        Text details;
        if (!send_operation_details(*operation, details)) {
            tools_window_->set_status(T("Could not prepare the bounded archive retry confirmation."));
            return;
        }
        char phrase[48];
        snprintf(phrase, sizeof(phrase), "ARCHIVE %llu", static_cast<unsigned long long>(operation->id));
        if (!literal_confirmation(T("Retry Sent-folder archive"),
                T("This retries only IMAP append to the exact reviewed folder. SMTP will not be contacted."),
                details.c_str(), T("Cancel"), T("Continue")) ||
            !literal_text_confirmation(T("Confirm archive-only retry"),
                T("Type the operation-bound archive phrase."), phrase, T("Cancel"), T("Retry archive"))) return;
        SecretBytes secret;
        Text error;
        if (!make_imap_credentials_for(*account, secret, error)) {
            tools_window_->set_status(error.empty() ? T("Enter this account's current incoming password first.") : error.c_str());
            return;
        }
        WorkerTaskInput input;
        input.item_id = operation->id;
        WorkerRequest request;
        request.account_id = account->id;
        request.account_generation = account->generation;
        PendingUiRequest pending;
        pending.account = *account;
        pending.tool_operation = *operation;
        pending.tool_has_operation = true;
        pending.tool_window_task = true;
        if (!submit_tool_task(WorkerTaskKind::RetryArchive, request, &input,
                static_cast<SecretBytes &&>(secret), static_cast<PendingUiRequest &&>(pending), error))
            tools_window_->set_status(error.empty() ? T("Could not queue the IMAP archive retry.") : error.c_str());
        return;
    }
    case MailToolsCommand::SendQueued: {
        const SendOperationSummary *operation = tools_window_->selected_operation();
        if (!operation || operation->account_id != account_id || operation->stage != SendStage::Ready ||
            operation->account_generation != account->generation) {
            tools_window_->set_status(T("Only a ready operation under the current account generation can be sent."));
            return;
        }
        Text details;
        if (!send_operation_details(*operation, details)) {
            tools_window_->set_status(T("Could not prepare the bounded SMTP confirmation."));
            return;
        }
        char phrase[48];
        snprintf(phrase, sizeof(phrase), "SEND %llu", static_cast<unsigned long long>(operation->id));
        if (!literal_confirmation(T("Send queued message"),
                T("This starts a new SMTP attempt for this ready operation. Confirm the recipient and Message-ID carefully."),
                details.c_str(), T("Cancel"), T("Continue")) ||
            !literal_text_confirmation(T("Confirm SMTP attempt"),
                T("Type the operation-bound send phrase."), phrase, T("Cancel"), T("Send now"))) return;
        SecretBytes secret;
        Text error;
        if (!make_smtp_credentials_for(*account, secret, error)) {
            tools_window_->set_status(error.empty() ? T("Enter this account's current outgoing password first.") : error.c_str());
            return;
        }
        WorkerTaskInput input;
        input.item_id = operation->id;
        WorkerRequest request;
        request.account_id = account->id;
        request.account_generation = account->generation;
        PendingUiRequest pending;
        pending.account = *account;
        pending.tool_operation = *operation;
        pending.tool_has_operation = true;
        pending.tool_window_task = true;
        if (!submit_tool_task(WorkerTaskKind::SendQueued, request, &input,
                static_cast<SecretBytes &&>(secret), static_cast<PendingUiRequest &&>(pending), error))
            tools_window_->set_status(error.empty() ? T("Could not queue the SMTP attempt.") : error.c_str());
        return;
    }
    case MailToolsCommand::CancelQueuedSend: {
        const SendOperationSummary *operation = tools_window_->selected_operation();
        if (!operation || operation->account_id != account_id || operation->stage != SendStage::Ready) {
            tools_window_->set_status(T("Only a ready operation can be cancelled."));
            return;
        }
        Text details;
        if (!send_operation_details(*operation, details)) {
            tools_window_->set_status(T("Could not prepare the bounded cancellation confirmation."));
            return;
        }
        char phrase[48];
        snprintf(phrase, sizeof(phrase), "CANCEL %llu", static_cast<unsigned long long>(operation->id));
        if (!literal_confirmation(T("Cancel queued send"),
                T("This removes the ready operation from future SMTP submission. It does not affect a message already sent."),
                details.c_str(), T("Keep queued"), T("Continue")) ||
            !literal_text_confirmation(T("Confirm queued cancellation"),
                T("Type the operation-bound cancellation phrase."), phrase, T("Cancel"), T("Cancel queued"))) return;
        WorkerTaskInput input;
        input.item_id = operation->id;
        WorkerRequest request;
        request.account_id = account->id;
        request.account_generation = account->generation;
        PendingUiRequest pending;
        pending.account = *account;
        pending.tool_operation = *operation;
        pending.tool_has_operation = true;
        pending.tool_window_task = true;
        Text error;
        if (!submit_tool_task(WorkerTaskKind::CancelQueuedSend, request, &input,
                SecretBytes(), static_cast<PendingUiRequest &&>(pending), error))
            tools_window_->set_status(error.empty() ? T("Could not queue the cancellation.") : error.c_str());
        return;
    }
    }
}

void MailWindow::run_message_action(UiCommand command) {
    run_message_action(command, nullptr);
}

void MailWindow::run_message_action(UiCommand command, const FolderKey *preferred_target) {
    run_message_action(command, preferred_target, nullptr);
}

void MailWindow::run_message_action(UiCommand command, const FolderKey *preferred_target,
    const PendingUiRequest *continuation) {
    auto report_continuation_error = [&](const char *message) {
        if (continuation) set_action_continuation_status(message);
        else set_status(message);
    };
    if (action_batch_pending_) {
        report_continuation_error(T("Wait for the current message action batch to finish before starting another."));
        return;
    }
    uint64_t account_id = 0;
    if (continuation) {
        if (!continuation->tool_has_action_selection) {
            report_continuation_error(T("The original message selection could not be retained. Select the messages again."));
            return;
        }
        account_id = continuation->account.id;
    } else if (!checked_rows_.empty()) {
        const uint32_t index = checked_rows_[0];
        if (index < rows_.size()) account_id = rows_[index].message.key.folder.account_id;
    } else if (selected_row_valid_) account_id = selected_row_.message.key.folder.account_id;
    const Account *account = continuation ? find_account(action_snapshot_.accounts, account_id) :
        find_account(startup_.accounts, account_id);
    if (!account && continuation) account = find_account(startup_.accounts, account_id);
    if (!account || !account->enabled || account->incoming_protocol != IncomingProtocol::Imap) {
        report_continuation_error(T("Select messages from one enabled IMAP account before applying server actions."));
        return;
    }
    BoundedArray<MessageQueryRow, MaxWorkerOutstanding> selected;
    Text error;
    if (continuation) {
        if (account->generation != continuation->account.generation ||
            !task8a_retained_action_selection_valid(continuation->tool_action_rows,
                action_snapshot_.folders, continuation->tool_action_profile_epoch,
                continuation->tool_action_ui_generation, coordinator_.profile_epoch(),
                ui_generation_, account->id, account->generation, error) ||
            !selected.clone_from(continuation->tool_action_rows)) {
            report_continuation_error(T("The original message selection changed while refreshing the destination. Select it again."));
            return;
        }
    } else if (!task8a_collect_action_rows(rows_, checked_rows_, selected_row_valid_ ? &selected_row_ : nullptr,
            coordinator_.profile_epoch(), ui_generation_, account->id, account->generation,
            selected, error)) {
        set_status(error.c_str());
        return;
    }
    if (!continuation && !task8a_retained_action_selection_valid(selected, startup_.folders,
            selected[0].profile_epoch, selected[0].ui_generation,
            coordinator_.profile_epoch(), ui_generation_, account->id, account->generation, error)) {
        set_status(T("The selected folder identity is stale."));
        return;
    }

    auto retain_action_selection = [&](PendingUiRequest &pending) {
        if (selected.empty() || !pending.tool_action_rows.clone_from(selected)) return false;
        pending.tool_action_profile_epoch = selected[0].profile_epoch;
        pending.tool_action_ui_generation = selected[0].ui_generation;
        pending.tool_has_action_selection = true;
        return true;
    };

    WorkerTaskKind kind = WorkerTaskKind::SetFlags;
    WorkerTaskInput action_input;
    const Folder *destination = nullptr;
    FolderKey requested_target;
    switch (command) {
    case UiCommand::MarkRead:
        action_input.flags_add = ImapFlagSeen;
        kind = WorkerTaskKind::SetFlags;
        break;
    case UiCommand::MarkUnread:
        action_input.flags_remove = ImapFlagSeen;
        kind = WorkerTaskKind::SetFlags;
        break;
    case UiCommand::StarMessage:
        action_input.flags_add = ImapFlagFlagged;
        kind = WorkerTaskKind::SetFlags;
        break;
    case UiCommand::UnstarMessage:
        action_input.flags_remove = ImapFlagFlagged;
        kind = WorkerTaskKind::SetFlags;
        break;
    case UiCommand::ArchiveMessage:
        if (account->archive_folder.empty()) {
            set_status(T("Configure this account's Archive folder before archiving messages.")); return;
        }
        requested_target.account_id = account->id;
        requested_target.server_name = account->archive_folder;
        kind = WorkerTaskKind::MoveMessage;
        break;
    case UiCommand::JunkMessage:
        if (account->junk_folder.empty()) { set_status(T("Configure this account's Junk folder first.")); return; }
        requested_target.account_id = account->id;
        requested_target.server_name = account->junk_folder;
        kind = WorkerTaskKind::MoveMessage;
        break;
    case UiCommand::TrashMessage:
        if (account->trash_folder.empty()) { set_status(T("Configure this account's Trash folder first.")); return; }
        requested_target.account_id = account->id;
        requested_target.server_name = account->trash_folder;
        kind = WorkerTaskKind::MoveMessage;
        break;
    case UiCommand::MoveMessage:
    case UiCommand::CopyMessage:
        if (preferred_target) requested_target = *preferred_target;
        else if (!choose_destination(startup_.folders, account->id, requested_target)) return;
        kind = command == UiCommand::MoveMessage ? WorkerTaskKind::MoveMessage : WorkerTaskKind::CopyMessage;
        break;
    case UiCommand::DeleteMessage:
        kind = WorkerTaskKind::DeleteMessage;
        break;
    default:
        set_status(T("This message action is unavailable."));
        return;
    }

    if (kind == WorkerTaskKind::MoveMessage || kind == WorkerTaskKind::CopyMessage) {
        destination = find_folder(startup_.folders, requested_target);
        if (!destination && action_snapshot_.selected_account_id == account->id)
            destination = find_folder(action_snapshot_.folders, requested_target);
        if (!destination) {
            if (continuation) {
                report_continuation_error(T("The destination folder is no longer available; choose it again."));
                return;
            }
            SecretBytes secret;
            if (!make_imap_credentials_for(*account, secret, error)) {
                set_status(error.empty() ? T("Enter the account's incoming password before refreshing its folders.") : error.c_str());
                return;
            }
            WorkerTaskInput input;
            input.folder_action = WorkerFolderAction::List;
            WorkerRequest request;
            request.account_id = account->id;
            request.account_generation = account->generation;
            PendingUiRequest pending;
            pending.account = *account;
            pending.tool_ui_command = command;
            pending.tool_folder_key = requested_target;
            Text submit_error;
            if (!retain_action_selection(pending)) {
                set_status(T("The original message selection could not be retained. Select the messages again."));
                return;
            }
            pending.tool_folder_action = WorkerFolderAction::List;
            if (!submit_tool_task(WorkerTaskKind::FolderOperation, request, &input,
                    static_cast<SecretBytes &&>(secret), static_cast<PendingUiRequest &&>(pending), submit_error))
                set_status(submit_error.empty() ? T("Could not refresh destination folders.") : submit_error.c_str());
            return;
        }
        if (!destination->selectable || destination->key.account_id != account->id ||
            !destination->generation || !requested_target.server_name.size()) {
            report_continuation_error(T("Choose a selectable destination in this same account."));
            return;
        }
        if (!destination->uidvalidity) {
            SecretBytes secret;
            if (!make_imap_credentials_for(*account, secret, error)) {
                set_status(error.empty() ? T("Enter the account's incoming password before checking the destination.") : error.c_str());
                return;
            }
            WorkerTaskInput input;
            input.page_query.folder = destination->key;
            WorkerRequest request;
            request.account_id = account->id;
            request.account_generation = account->generation;
            request.folder_server_name = destination->key.server_name;
            request.folder_generation = destination->generation;
            request.folder_uidvalidity = destination->uidvalidity;
            PendingUiRequest pending;
            pending.account = *account;
            pending.folder = *destination;
            pending.tool_folder_key = requested_target;
            pending.tool_ui_command = command;
            pending.tool_folder_action = WorkerFolderAction::List;
            Text submit_error;
            if (!retain_action_selection(pending)) {
                set_status(T("The original message selection could not be retained. Select the messages again."));
                return;
            }
            if (!submit_tool_task(WorkerTaskKind::FolderSelect, request, &input,
                    static_cast<SecretBytes &&>(secret), static_cast<PendingUiRequest &&>(pending), submit_error))
                set_status(submit_error.empty() ? T("Could not refresh destination UIDVALIDITY.") : submit_error.c_str());
            return;
        }
        if (requested_target.account_id != account->id) {
            set_status(T("Cross-account message moves and copies are not available.")); return;
        }
    }

    const bool mutation = kind == WorkerTaskKind::MoveMessage || kind == WorkerTaskKind::CopyMessage ||
        kind == WorkerTaskKind::DeleteMessage;
    if (mutation) {
        Text details;
        if (!message_identity_details(selected, destination, details)) {
            set_status(T("Could not prepare the exact message identity confirmation.")); return;
        }
        char phrase[64];
        snprintf(phrase, sizeof(phrase), "%s %zu", action_word(command), selected.size());
        if (!literal_confirmation(T("Confirm message action"),
                T("This server action applies only to the exact account, folder, UIDVALIDITY, and UIDs shown below."),
                details.c_str(), T("Cancel"), T("Continue")) ||
            !literal_text_confirmation(T("Confirm exact message action"),
                T("Type the action and selected-message count."), phrase, T("Cancel"), T("Apply"))) return;
    }

    SecretBytes credential_check;
    if (!make_imap_credentials_for(*account, credential_check, error)) {
        set_status(error.empty() ? T("Enter the account's incoming password before changing server mail.") : error.c_str());
        return;
    }
    action_batch_pending_ = 0;
    action_batch_had_uncertain_ = false;
    action_batch_last_diagnostic_.clear();
    size_t queued = 0;
    for (size_t i = 0; i < selected.size(); ++i) {
        WorkerTaskInput input;
        input.flags_add = action_input.flags_add;
        input.flags_remove = action_input.flags_remove;
        input.message_key = selected[i].message.key;
        if (destination) {
            input.destination_folder = destination->key;
            input.destination_folder_generation = destination->generation;
            input.destination_folder_uidvalidity = destination->uidvalidity;
        }
        SecretBytes row_credentials;
        if (!make_imap_credentials_for(*account, row_credentials, error)) {
            if (action_batch_last_diagnostic_.empty()) action_batch_last_diagnostic_ = error;
            break;
        }
        PendingUiRequest pending;
        pending.account = *account;
        pending.tool_kind = kind;
        pending.tool_message_batch = true;
        pending.tool_ui_command = command;
        if (submit_row_task(kind, selected[i], input, static_cast<SecretBytes &&>(row_credentials),
                PendingPurpose::ToolTask, selection_generation_, error, static_cast<PendingUiRequest &&>(pending))) {
            ++action_batch_pending_;
            ++queued;
        } else {
            if (action_batch_last_diagnostic_.empty()) action_batch_last_diagnostic_ = error;
            break;
        }
    }
    if (queued != selected.size()) {
        char message[256];
        snprintf(message, sizeof(message), T("Queued %zu of %zu message actions; inspect the results before repeating any uncertain action."),
            queued, selected.size());
        set_status(message);
    } else if (queued) {
        char message[192];
        snprintf(message, sizeof(message), T("Queued %zu exact-identity message actions."), queued);
        set_status(message);
    }
}

void MailWindow::finish_message_action_batch() {
    if (action_batch_pending_) return;
    const bool uncertain = action_batch_had_uncertain_;
    const Text diagnostic = action_batch_last_diagnostic_;
    if (!diagnostic.empty()) {
        action_refresh_diagnostic_.set(diagnostic.c_str());
        last_error_.set(diagnostic.c_str());
    }
    schedule_query(true);
    if (!diagnostic.empty()) set_status(diagnostic.c_str());
    else set_status(uncertain ?
        T("At least one result is uncertain. Refresh the server folder before deciding whether to act again.") :
        T("Server actions completed; the local view is refreshing."));
    action_batch_had_uncertain_ = false;
    action_batch_last_diagnostic_.clear();
}

void MailWindow::handle_tool_result(PendingUiRequest &pending, WorkerResult &result) {
    if (!result.ok()) {
        const char *diagnostic = result.diagnostic.empty() ? T("Background mail action failed.") : result.diagnostic.c_str();
        if (pending.tool_message_batch) {
            if (result.status == WorkerStatus::Uncertain) action_batch_had_uncertain_ = true;
            action_batch_last_diagnostic_.set(diagnostic);
            if (action_batch_pending_) --action_batch_pending_;
            if (!action_batch_pending_) finish_message_action_batch();
        } else {
            last_error_.set(diagnostic);
            if (pending.tool_window_task && tools_window_ &&
                tools_window_->selected_account_id() == pending.account.id)
                tools_window_->set_status(diagnostic);
            set_status(diagnostic);
        }
        return;
    }

    if (pending.tool_message_batch) {
        if (!result.diagnostic.empty()) action_batch_last_diagnostic_.set(result.diagnostic.c_str());
        if (action_batch_pending_) --action_batch_pending_;
        if (!action_batch_pending_) finish_message_action_batch();
        return;
    }

    if (pending.tool_kind == WorkerTaskKind::FolderOperation &&
        pending.tool_folder_action != WorkerFolderAction::ClearTrash &&
        result.payload.empty() && !result.diagnostic.empty()) {
        if (tools_window_ && tools_window_->selected_account_id() == pending.account.id)
            tools_window_->set_status(result.diagnostic.c_str());
        set_status(result.diagnostic.c_str());
        /* The server may have accepted a folder mutation even though refreshing
         * its snapshot failed. Retry only the read-only listing, never the mutation. */
        if (pending.tool_folder_action != WorkerFolderAction::List && tools_window_ &&
            tools_window_->selected_account_id() == pending.account.id)
            submit_tool_folder_operation(WorkerFolderAction::List, nullptr);
        return;
    }

    if (pending.tool_kind == WorkerTaskKind::FolderSelect ||
        (pending.tool_kind == WorkerTaskKind::FolderOperation &&
         pending.tool_folder_action != WorkerFolderAction::ClearTrash)) {
        WorkerStartupSnapshot next;
        Text error;
        if (!worker_decode_startup_snapshot(result.payload.data(), result.payload.size(), next, error)) {
            const char *diagnostic = error.empty() ? T("Folder worker returned an invalid snapshot.") : error.c_str();
            if (pending.tool_window_task && tools_window_ &&
                tools_window_->selected_account_id() == pending.account.id) tools_window_->set_status(diagnostic);
            set_status(diagnostic);
            return;
        }
        const Account *current_account = find_account(startup_.accounts, pending.account.id);
        if (!current_account || current_account->generation != pending.account.generation) {
            const char *diagnostic = T("The account changed while destination folders were being refreshed. Review the current account and select messages again.");
            if (pending.tool_has_action_selection) set_action_continuation_status(diagnostic);
            else set_status(diagnostic);
            return;
        }
        if (!clone_startup_snapshot(next, action_snapshot_)) {
            const char *diagnostic = T("The refreshed folder identity could not be retained safely.");
            if (pending.tool_has_action_selection) set_action_continuation_status(diagnostic);
            else set_status(diagnostic);
            return;
        }
        if (pending.account.id == selected_account_id_) {
            if (!clone_startup_snapshot(next, startup_)) {
                const char *diagnostic = T("The refreshed folder identity could not be retained safely.");
                if (pending.tool_has_action_selection) set_action_continuation_status(diagnostic);
                else set_status(diagnostic);
                return;
            }
            selected_account_id_ = pending.account.id;
            const Folder *current = selected_folder_valid_ ? find_folder(startup_.folders, selected_folder_.key) : nullptr;
            if (current && current->selectable) selected_folder_ = *current;
            else choose_default_folder();
            render_accounts_and_folders();
        }
        if (pending.tool_window_task && tools_window_ &&
            tools_window_->selected_account_id() == pending.account.id) {
            tools_window_->refresh_snapshot(next, pending.account.id);
            tools_window_->set_status(result.diagnostic.empty() ? T("Folder identity refreshed through IMAP.") : result.diagnostic.c_str());
        }
        if (pending.tool_kind == WorkerTaskKind::FolderOperation &&
            pending.tool_folder_action == WorkerFolderAction::List &&
            pending.tool_ui_command != UiCommand::Count) {
            const bool account_current = selected_account_id_ == pending.account.id;
            const bool context_current = pending.tool_action_profile_epoch == coordinator_.profile_epoch() &&
                pending.tool_action_ui_generation == ui_generation_;
            (void)task8a_resume_folder_action(true, false, account_current, context_current,
                [&]() {
                    run_message_action(pending.tool_ui_command,
                        pending.tool_folder_key.server_name.empty() ? nullptr : &pending.tool_folder_key,
                        &pending);
                }, []() {});
            return;
        }
        if (pending.tool_kind == WorkerTaskKind::FolderSelect &&
            pending.tool_has_operation && pending.tool_window_task) {
            const Folder *target = find_folder(action_snapshot_.folders, pending.tool_folder_key);
            if (!target || !target->selectable || !target->generation || !target->uidvalidity ||
                !tools_window_ || tools_window_->selected_account_id() != pending.account.id) {
                if (tools_window_) tools_window_->set_status(T("The archive destination identity changed during review. Choose it again."));
                return;
            }
            if (selected_account_id_ == pending.account.id) schedule_query(true, true);
            submit_tool_send_review(WorkerTaskKind::ReviewArchiveTarget, pending.tool_operation,
                target, WorkerSendReviewAction::SetArchiveTarget);
            return;
        }
        if (pending.tool_ui_command != UiCommand::Count) {
            const bool account_current = selected_account_id_ == pending.account.id;
            const bool context_current = pending.tool_action_profile_epoch == coordinator_.profile_epoch() &&
                pending.tool_action_ui_generation == ui_generation_;
            (void)task8a_resume_folder_action(true,
                pending.tool_kind == WorkerTaskKind::FolderSelect,
                account_current, context_current,
                [&]() {
                    run_message_action(pending.tool_ui_command,
                        pending.tool_folder_key.server_name.empty() ? nullptr : &pending.tool_folder_key,
                        &pending);
                }, [&]() { schedule_query(true, true); });
            return;
        }
        if (pending.tool_kind == WorkerTaskKind::FolderSelect &&
            pending.account.id == selected_account_id_) schedule_query(true, true);
        if (pending.tool_window_task && tools_window_)
            tools_window_->set_status(T("Folder list refreshed from the server."));
        set_status(result.diagnostic.empty() ? T("Folder identity refreshed.") : result.diagnostic.c_str());
        return;
    }

    if (pending.tool_kind == WorkerTaskKind::ClearCache) return;
    if (pending.tool_kind == WorkerTaskKind::ListSendOperations) {
        WorkerSendOperationPage page;
        Text error;
        const Account *current = find_account(startup_.accounts, pending.account.id);
        if (!worker_decode_send_operation_page(result.payload.data(), result.payload.size(), page, error) ||
            page.account_id != pending.account.id || !current ||
            current->generation != pending.account.generation || !tools_window_ ||
            tools_window_->selected_account_id() != pending.account.id) {
            const char *diagnostic = error.empty() ? T("Send history no longer matches the selected account.") : error.c_str();
            if (tools_window_) tools_window_->set_status(diagnostic);
            set_status(diagnostic);
            return;
        }
        tools_window_->set_send_page(page);
        tools_window_->set_status(T("Durable send history loaded. SMTP and archive actions require separate explicit confirmation."));
        set_status(T("Delivery history loaded."));
        return;
    }

    if (pending.tool_kind == WorkerTaskKind::FolderOperation &&
        pending.tool_folder_action == WorkerFolderAction::ClearTrash) {
        if (!result.diagnostic.empty()) {
            if (tools_window_) tools_window_->set_status(result.diagnostic.c_str());
            set_status(result.diagnostic.c_str());
        } else {
            if (tools_window_) tools_window_->set_status(T("Trash deletion completed. Refreshing the server folder."));
            set_status(T("Trash deletion completed; refreshing the server folder."));
        }
        submit_tool_folder_operation(WorkerFolderAction::List, nullptr);
        return;
    }

    if (pending.tool_kind == WorkerTaskKind::ResolveUncertainSend ||
        pending.tool_kind == WorkerTaskKind::ReviewArchiveTarget ||
        pending.tool_kind == WorkerTaskKind::RetryArchive ||
        pending.tool_kind == WorkerTaskKind::SendQueued ||
        pending.tool_kind == WorkerTaskKind::CancelQueuedSend) {
        WorkerOperationResult operation;
        Text error;
        if (!worker_decode_operation_result(result.payload.data(), result.payload.size(), operation, error) ||
            !pending.tool_has_operation || operation.operation_id != pending.tool_operation.id) {
            const char *diagnostic = error.empty() ? T("The worker result does not match the reviewed operation.") : error.c_str();
            if (tools_window_) tools_window_->set_status(diagnostic);
            set_status(diagnostic);
            return;
        }
        if (!result.diagnostic.empty()) {
            if (tools_window_ && tools_window_->selected_account_id() == pending.account.id)
                tools_window_->set_status(result.diagnostic.c_str());
            set_status(result.diagnostic.c_str());
        }
        if (pending.tool_kind == WorkerTaskKind::RetryArchive) {
            if (task8a_archive_retry_completed(result.status, operation.stage)) {
                const char *message = T("Archive retry completed using IMAP only; SMTP was not contacted.");
                if (tools_window_ && tools_window_->selected_account_id() == pending.account.id)
                    tools_window_->set_status(message);
                set_status(message);
            } else if (result.diagnostic.empty()) {
                const char *message = T("Archive retry did not reach the archived state; SMTP was not retried.");
                if (tools_window_ && tools_window_->selected_account_id() == pending.account.id)
                    tools_window_->set_status(message);
                set_status(message);
            }
        }
        else if (pending.tool_kind == WorkerTaskKind::SendQueued && operation.stage == SendStage::Archived)
            set_status(T("SMTP delivery and Sent-folder archive completed."));
        else if (pending.tool_kind == WorkerTaskKind::SendQueued && operation.stage == SendStage::ArchivePending)
            set_status(T("SMTP delivery is confirmed; Sent-folder archive needs review. Do not resend."));
        else if (pending.tool_kind == WorkerTaskKind::SendQueued && operation.stage == SendStage::Uncertain)
            set_status(T("SMTP delivery is uncertain. No automatic retry was started."));
        else if (pending.tool_kind == WorkerTaskKind::CancelQueuedSend)
            set_status(T("The ready send operation was cancelled before SMTP submission."));
        else if (pending.tool_kind == WorkerTaskKind::ResolveUncertainSend)
            set_status(operation.stage == SendStage::ReviewedDelivered ?
                T("Delivery review recorded. No SMTP retry was started.") :
                T("The operation is ready only for a separately confirmed SMTP attempt."));
        refresh_tool_queue(pending.account.id, tools_window_ &&
            tools_window_->selected_account_id() == pending.account.id ? tools_window_->queue_offset() : 0);
        return;
    }

    const char *diagnostic = result.diagnostic.empty() ? T("The tool operation completed.") : result.diagnostic.c_str();
    if (tools_window_ && tools_window_->selected_account_id() == pending.account.id)
        tools_window_->set_status(diagnostic);
    set_status(diagnostic);
}

}
