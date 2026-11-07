#include "window.h"
#include "../fltk_support.h"
#include <FL/Fl.H>
#include <FL/fl_ask.H>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>

namespace rmail {
namespace {

bool set_error(Text &error, const char *message) {
    error.set(message ? message : T("The operation failed."));
    return false;
}

}

void MailWindow::render_accounts_and_folders() {
    NavigationButton *old_focus = static_cast<NavigationButton *>(Fl::focus());
    enum class FocusKind { Untracked, Unified, AllMail, Account, Folder };
    FocusKind focus_kind = FocusKind::Untracked;
    uint64_t focus_account_id = 0;
    FolderKey focus_folder_key;
    if (old_focus && account_folder_scroll_->contains(old_focus)) {
        if (old_focus == unified_button_) focus_kind = FocusKind::Unified;
        else if (old_focus == all_mail_button_) focus_kind = FocusKind::AllMail;
        else if (old_focus->callback() == account_cb) {
            const long row = old_focus->argument() - 1;
            if (row >= 0 && static_cast<size_t>(row) < startup_.accounts.size()) {
                focus_kind = FocusKind::Account;
                focus_account_id = startup_.accounts[static_cast<size_t>(row)].id;
            }
        } else if (old_focus->callback() == folder_cb) {
            const long row = old_focus->argument() - 1;
            if (row >= 0 && static_cast<size_t>(row) < startup_.folders.size()) {
                focus_kind = FocusKind::Folder;
                focus_folder_key = startup_.folders[static_cast<size_t>(row)].key;
            }
        }
        /* clear() destroys the focused widget; clear the global focus first. */
        Fl::focus(nullptr);
    }
    account_folder_scroll_->clear();
    account_folder_scroll_->begin();
    int y = account_folder_scroll_->y() + 4;
    NavigationButton *previous = nullptr;
    auto connect_row = [&](NavigationButton *row) {
        row->previous(previous);
        if (previous) previous->next(row);
        previous = row;
    };

    unified_button_ = new NavigationButton(8, y, account_folder_scroll_->w() - 16, 28,
        T("Unified inbox"));
    unified_button_->type(FL_TOGGLE_BUTTON);
    unified_button_->labelsize(13);
    unified_button_->callback(navigation_scope_cb, reinterpret_cast<void *>(intptr_t(1)));
    unified_button_->value(scope_choice_ && scope_choice_->value() == 0);
    connect_row(unified_button_);
    y += 30;

    all_mail_button_ = new NavigationButton(8, y, account_folder_scroll_->w() - 16, 28,
        T("All mail"));
    all_mail_button_->type(FL_TOGGLE_BUTTON);
    all_mail_button_->labelsize(13);
    all_mail_button_->callback(navigation_scope_cb, reinterpret_cast<void *>(intptr_t(4)));
    all_mail_button_->value(scope_choice_ && scope_choice_->value() == 3);
    connect_row(all_mail_button_);
    y += 35;

    PlainTextBox *accounts_heading = new PlainTextBox(12, y, account_folder_scroll_->w() - 24, 20,
        T("Accounts"));
    accounts_heading->labelsize(11);
    y += 23;
    auto render_selected_folders = [&]() {
    for (size_t index = 0; index < startup_.folders.size(); ++index) {
        const Folder &folder = startup_.folders[index];
        if (folder.key.account_id != selected_account_id_) continue;
        const char *display = folder.display_name.empty() ? folder.key.server_name.c_str() : folder.display_name.c_str();
        switch (folder.role) {
        case FolderRole::Inbox: display = T("Inbox"); break;
        case FolderRole::Sent: display = T("Sent"); break;
        case FolderRole::Drafts: display = T("Drafts"); break;
        case FolderRole::Trash: display = T("Trash"); break;
        case FolderRole::Junk: display = T("Junk"); break;
        case FolderRole::Archive: display = T("Archive"); break;
        case static_cast<FolderRole>(0): break; // X11 defines None as a macro.
        }
        char label[2048];
        snprintf(label, sizeof(label), "%s%s", display, folder.selectable ? "" : T("  (container)"));
        NavigationButton *row = new NavigationButton(22, y, account_folder_scroll_->w() - 30, 28, label);
        row->type(FL_TOGGLE_BUTTON);
        row->alignment(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
        row->labelsize(12);
        if (!folder.selectable) row->deactivate();
        else row->value(selected_folder_valid_ && folder.key == selected_folder_.key);
        row->callback(folder_cb, reinterpret_cast<void *>(static_cast<intptr_t>(index) + 1));
        connect_row(row);
        y += 29;
    }
    };
    for (size_t index = 0; index < startup_.accounts.size(); ++index) {
        const Account &account = startup_.accounts[index];
        char label[2048];
        const char *name = account.display_name.empty() ? account.email_address.c_str() : account.display_name.c_str();
        snprintf(label, sizeof(label), "%s\n%s%s%s", name, account.email_address.c_str(),
            account.is_default ? T("  · Default") : "",
            account.enabled ? "" : T("  · Disabled"));
        NavigationButton *row = new NavigationButton(8, y, account_folder_scroll_->w() - 16, 44, label);
        row->type(FL_TOGGLE_BUTTON);
        row->alignment(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
        row->labelsize(12);
        row->value(account.id == selected_account_id_);
        row->callback(account_cb, reinterpret_cast<void *>(static_cast<intptr_t>(index) + 1));
        connect_row(row);
        y += 48;
        if (account.id == selected_account_id_) {
            render_selected_folders();
            y += 10;
        }
    }
    if (startup_.accounts.empty()) {
        PlainTextBox *empty = new PlainTextBox(14, y, account_folder_scroll_->w() - 28, 46,
            T("No account is configured. Add one from the Send/Receive tab."));
        empty->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
        empty->labelsize(12);
        y += 50;
    }
    account_folder_scroll_->end();
    account_folder_scroll_->redraw();
    NavigationButton *restore_focus = nullptr;
    if (focus_kind == FocusKind::Unified) restore_focus = unified_button_;
    else if (focus_kind == FocusKind::AllMail) restore_focus = all_mail_button_;
    else if (focus_kind == FocusKind::Account) {
        for (int child = 0; child < account_folder_scroll_->children(); ++child) {
            Fl_Widget *widget = account_folder_scroll_->child(child);
            if (widget && widget->callback() == account_cb) {
                const long row = widget->argument() - 1;
                if (row >= 0 && static_cast<size_t>(row) < startup_.accounts.size() &&
                    startup_.accounts[static_cast<size_t>(row)].id == focus_account_id) {
                    restore_focus = static_cast<NavigationButton *>(widget);
                    break;
                }
            }
        }
    } else if (focus_kind == FocusKind::Folder) {
        for (int child = 0; child < account_folder_scroll_->children(); ++child) {
            Fl_Widget *widget = account_folder_scroll_->child(child);
            if (widget && widget->callback() == folder_cb) {
                const long row = widget->argument() - 1;
                if (row >= 0 && static_cast<size_t>(row) < startup_.folders.size() &&
                    startup_.folders[static_cast<size_t>(row)].key == focus_folder_key &&
                    widget->active()) {
                    restore_focus = static_cast<NavigationButton *>(widget);
                    break;
                }
            }
        }
    }
    if (restore_focus && restore_focus->take_focus()) {
        const int visible_top = account_folder_scroll_->yposition();
        const int visible_bottom = visible_top + account_folder_scroll_->h();
        const int content_top = restore_focus->y() - account_folder_scroll_->y();
        if (content_top < visible_top)
            account_folder_scroll_->scroll_to(0, content_top);
        else if (content_top + restore_focus->h() > visible_bottom)
            account_folder_scroll_->scroll_to(0, content_top + restore_focus->h() -
                account_folder_scroll_->h());
    }
    update_navigation_selection();
    update_ribbon_state();
}

void MailWindow::account_cb(Fl_Widget *widget, void *data) {
    MailWindow *window = static_cast<MailWindow *>(widget->window());
    const intptr_t index = reinterpret_cast<intptr_t>(data) - 1;
    if (index >= 0) window->select_account(static_cast<size_t>(index));
}

void MailWindow::folder_cb(Fl_Widget *widget, void *data) {
    MailWindow *window = static_cast<MailWindow *>(widget->window());
    const intptr_t index = reinterpret_cast<intptr_t>(data) - 1;
    if (index >= 0) window->select_folder(static_cast<size_t>(index));
}

void MailWindow::choose_default_folder() {
    selected_folder_valid_ = false;
    Account *account = nullptr;
    for (size_t i = 0; i < startup_.accounts.size(); ++i)
        if (startup_.accounts[i].id == selected_account_id_) { account = &startup_.accounts[i]; break; }
    if (account && !account->inbox_folder.empty()) {
        for (size_t i = 0; i < startup_.folders.size(); ++i) {
            const Folder &folder = startup_.folders[i];
            if (folder.selectable && folder.key.account_id == selected_account_id_ &&
                folder.key.server_name == account->inbox_folder) {
                selected_folder_ = folder;
                selected_folder_valid_ = true;
                break;
            }
        }
    }
    if (!selected_folder_valid_) {
        for (size_t i = 0; i < startup_.folders.size(); ++i) {
            const Folder &folder = startup_.folders[i];
            if (folder.selectable && folder.key.account_id == selected_account_id_ &&
                folder.role == FolderRole::Inbox) {
                selected_folder_ = folder;
                selected_folder_valid_ = true;
                break;
            }
        }
    }
    if (!selected_folder_valid_) {
        for (size_t i = 0; i < startup_.folders.size(); ++i) {
            const Folder &folder = startup_.folders[i];
            if (folder.selectable && folder.key.account_id == selected_account_id_) {
                selected_folder_ = folder;
                selected_folder_valid_ = true;
                break;
            }
        }
    }
    render_accounts_and_folders();
}

void MailWindow::select_account(size_t index) {
    if (index >= startup_.accounts.size()) return;
    selected_account_id_ = startup_.accounts[index].id;
    selected_folder_valid_ = false;
    if (scope_choice_) scope_choice_->value(1);
    filter_settings_.search.scope = MessageQueryScope::Account;
    all_mail_navigation_selected_ = false;
    update_navigation_selection();
    ++ui_generation_;
    ++selection_generation_;
    selected_row_valid_ = false;
    rows_.clear();
    checked_rows_.clear();
    reader_pane_->clear_message(T("Choose a folder or search this account's cached mail."));
    render_message_rows();
    render_accounts_and_folders();
    refresh_account_list();
}

void MailWindow::select_folder(size_t index) {
    if (index >= startup_.folders.size() || !startup_.folders[index].selectable) return;
    selected_folder_ = startup_.folders[index];
    selected_folder_valid_ = true;
    if (scope_choice_) scope_choice_->value(2);
    filter_settings_.search.scope = MessageQueryScope::Folder;
    all_mail_navigation_selected_ = false;
    update_navigation_selection();
    ++ui_generation_;
    ++selection_generation_;
    selected_row_valid_ = false;
    checked_rows_.clear();
    rows_.clear();
    reader_pane_->clear_message(T("Select a cached message in this folder."));
    render_accounts_and_folders();
    render_message_rows();
    schedule_query(true);
}

void MailWindow::refresh_account_list() {
    if (!startup_complete_) return;
    WorkerRequest request;
    PendingUiRequest pending;
    pending.purpose = PendingPurpose::AccountList;
    if (selected_account_id_) {
        for (size_t i = 0; i < startup_.accounts.size(); ++i)
            if (startup_.accounts[i].id == selected_account_id_) {
                request.account_id = selected_account_id_;
                request.account_generation = startup_.accounts[i].generation;
                break;
            }
    }
    Text error;
    if (!submit_task(WorkerTaskKind::AccountList, request, nullptr, SecretBytes(),
            static_cast<PendingUiRequest &&>(pending), error)) set_status(error.c_str());
}

void MailWindow::refresh_after_account_mutation(uint64_t deleted_account_id) {
    if (deleted_account_id && selected_account_id_ == deleted_account_id)
        selected_account_id_ = 0;
    selected_folder_valid_ = false;
    selected_row_valid_ = false;
    preserve_selection_on_query_ = false;
    ++ui_generation_;
    ++selection_generation_;
    rows_.clear();
    checked_rows_.clear();
    reader_pane_->clear_message(T("Account settings changed. Refreshing the local view."));
    render_accounts_and_folders();
    render_message_rows();
    if (selected_account_id_) refresh_account_list();
    else if (!submit_initialize()) set_status(T("Account changed, but the local view could not be refreshed."));
}

void MailWindow::refresh_folders() {
    if (!selected_account_id_) {
        set_status(T("Select an account before synchronizing folders.")); return;
    }
    const Account *selected = nullptr;
    for (size_t i = 0; i < startup_.accounts.size(); ++i)
        if (startup_.accounts[i].id == selected_account_id_) selected = &startup_.accounts[i];
    if (!selected) { set_status(T("The selected account is no longer in the local profile.")); return; }
    SecretBytes packet, check;
    Text error;
    if (!make_live_credentials(packet, error) ||
        !worker_find_credential(packet, *selected, CredentialPurpose::Imap,
            selected->username, coordinator_.profile_epoch(), check, error)) {
        set_status(T("Unlock the password vault or add this account's incoming password before syncing.")); return;
    }
    WorkerRequest request;
    request.account_id = selected->id;
    request.account_generation = selected->generation;
    PendingUiRequest pending;
    pending.purpose = PendingPurpose::FolderList;
    ++ui_generation_;
    ++selection_generation_;
    rows_.clear();
    selected_row_valid_ = false;
    Text submit_error;
    if (!submit_task(WorkerTaskKind::FolderList, request, nullptr,
            static_cast<SecretBytes &&>(packet), static_cast<PendingUiRequest &&>(pending), submit_error))
        set_status(submit_error.c_str());
    else render_message_rows();
}

void MailWindow::open_account_dialog(bool add_new) {
    if (!startup_complete_) { set_status(T("Wait for the local account list to load.")); return; }
    Account *account = nullptr;
    if (!add_new) {
        for (size_t i = 0; i < startup_.accounts.size(); ++i)
            if (startup_.accounts[i].id == selected_account_id_) { account = &startup_.accounts[i]; break; }
    }
    AccountSetupDialog dialog(account, account_dialog_cb, this, coordinator_.profile_epoch());
    dialog.show_modal();
}

void MailWindow::account_dialog_cb(void *context, AccountSetupDialog &dialog, AccountDialogAction action) {
    static_cast<MailWindow *>(context)->handle_account_dialog(dialog, action);
}

void MailWindow::handle_account_dialog(AccountSetupDialog &dialog, AccountDialogAction action) {
    if (action == AccountDialogAction::Cancel) { dialog.hide(); return; }
    if (dialog.profile_epoch() != coordinator_.profile_epoch()) {
        dialog.hide();
        set_status(T("The profile changed while account settings were open. Reopen the account dialog."));
        return;
    }
    if (action == AccountDialogAction::Save) { start_account_save(dialog); return; }
    if (action == AccountDialogAction::TestIncoming) { start_account_test(dialog, false); return; }
    if (action == AccountDialogAction::TestOutgoing) { start_account_test(dialog, true); return; }
    if (action == AccountDialogAction::RepairCertificates) { repair_system_certificates(); return; }
    if (action == AccountDialogAction::Delete) {
        Account account;
        SecretBytes imap_password, smtp_password;
        Text error;
        if (!dialog.read(account, imap_password, smtp_password, error, false)) {
            fl_alert("%s", error.c_str()); return;
        }
        if (!account.id || !account.generation) { fl_alert("%s", T("Select a saved account to remove.")); return; }
        if (!literal_confirmation(T("Remove account"),
                T("Remove the selected account? Its local cached messages will be kept."),
                account.email_address.c_str(), T("Cancel"), T("Remove account"))) return;
        WorkerTaskInput input;
        WorkerRequest request;
        request.account_id = account.id;
        request.account_generation = account.generation;
        PendingUiRequest pending;
        pending.purpose = PendingPurpose::AccountDelete;
        pending.account = account;
        ++ui_generation_;
        ++selection_generation_;
        rows_.clear();
        selected_row_valid_ = false;
        Text submit_error;
        if (!submit_task(WorkerTaskKind::AccountDelete, request, &input, SecretBytes(),
                static_cast<PendingUiRequest &&>(pending), submit_error)) {
            --ui_generation_; --selection_generation_;
            fl_alert("%s", submit_error.c_str()); return;
        }
        reader_pane_->clear_message(T("Account removal is running in the background."));
        dialog.hide();
        return;
    }
}

void MailWindow::start_account_test(AccountSetupDialog &dialog, bool smtp) {
    Account candidate;
    SecretBytes incoming_password, outgoing_password;
    Text error;
    if (!dialog.read(candidate, incoming_password, outgoing_password, error, false)) {
        fl_alert("%s", error.c_str()); return;
    }
    SecretBytes password = smtp ? static_cast<SecretBytes &&>(outgoing_password) :
        static_cast<SecretBytes &&>(incoming_password);
    if (password.empty()) {
        fl_alert("%s", smtp ? T("Enter the SMTP password before testing outgoing authentication.") :
            T("Enter the IMAP password before testing incoming authentication."));
        return;
    }
    candidate.id = 0;
    candidate.generation = 0;
    WorkerTaskInput input;
    input.account = candidate;
    SecretBytes credential;
    if (!worker_encode_account_credential(candidate, smtp ? CredentialPurpose::Smtp : CredentialPurpose::Imap,
            coordinator_.profile_epoch(), static_cast<SecretBytes &&>(password), credential, error)) {
        fl_alert("%s", error.c_str()); return;
    }
    WorkerRequest request;
    PendingUiRequest pending;
    pending.purpose = smtp ? PendingPurpose::AccountTestSmtp : PendingPurpose::AccountTestImap;
    pending.account = candidate;
    if (!submit_task(smtp ? WorkerTaskKind::AccountTestSmtp : WorkerTaskKind::AccountTestImap,
            request, &input, static_cast<SecretBytes &&>(credential),
            static_cast<PendingUiRequest &&>(pending), error)) {
        fl_alert("%s", error.c_str()); return;
    }
    dialog.clear_passwords();
}

void MailWindow::account_test_failed(const PendingUiRequest &pending, const Text &diagnostic) {
    const TransportProblem problem = transport_problem(diagnostic);
    const char *hint = T("Connection test failed. Review the diagnostic below.");
    switch (problem) {
    case TransportProblem::CaUnavailable:
    case TransportProblem::CaInvalid:
        hint = pending.account.custom_ca_file.empty() ?
            T("The system CA certificate bundle could not be loaded. Repair the system certificates, then test again.") :
            T("The custom CA file could not be loaded. Check its path and contents, or leave it blank to use system certificates.");
        break;
    case TransportProblem::UntrustedCertificate:
        hint = T("The server certificate chain is not trusted. Check the server address and certificate chain; the system CA bundle may need updating.");
        break;
    case TransportProblem::CertificateTime:
        hint = T("The certificate is expired or not yet valid. Check the system date and time, then the server certificate.");
        break;
    case TransportProblem::HostnameMismatch:
        hint = T("The certificate does not match the server hostname. Check the IMAP/SMTP server address; updating CAs will not fix a hostname mismatch.");
        break;
    case TransportProblem::Handshake:
        hint = T("TLS negotiation failed before verification completed. Check the port, TLS/STARTTLS setting and network connection.");
        break;
    case TransportProblem::Other: break;
    }
    last_error_ = diagnostic;
    set_status(hint);
    const bool repairable = pending.account.custom_ca_file.empty() &&
        (problem == TransportProblem::CaUnavailable || problem == TransportProblem::CaInvalid ||
            problem == TransportProblem::UntrustedCertificate);
    if (repairable) {
        if (fl_choice("%s\n\n%s", T("Later"), T("Repair certificates"), nullptr,
                hint, diagnostic.c_str()) == 1) repair_system_certificates();
    } else fl_alert("%s\n\n%s", hint, diagnostic.c_str());
}

void MailWindow::repair_system_certificates() {
    if (closing_ || close_complete_) return;
    if (certificate_repair_pid_ > 0) {
        set_status(T("Certificate repair is already open in ReliefAPK."));
        return;
    }
    if (access("/usr/bin/reliefapk", X_OK) != 0) {
        fl_alert("%s", T("ReliefAPK is not installed. Install the signed ca-certificates-bundle package with your system package manager."));
        return;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        fl_alert("%s", T("Could not start the certificate repair tool."));
        return;
    }
    if (pid == 0) {
        execl("/usr/bin/reliefapk", "reliefapk", "--repair-ca", static_cast<char *>(nullptr));
        _exit(127);
    }
    certificate_repair_pid_ = static_cast<long>(pid);
    set_status(T("Review certificate repair in ReliefAPK. After completion, test the mail connection again."));
}

void MailWindow::start_account_save(AccountSetupDialog &dialog) {
    Account account;
    SecretBytes incoming_password, outgoing_password;
    Text error;
    if (!dialog.read(account, incoming_password, outgoing_password, error, false)) {
        fl_alert("%s", error.c_str()); return;
    }
    if (!account.id && (incoming_password.empty() || outgoing_password.empty())) {
        fl_alert("%s", T("Enter both incoming and outgoing passwords or application codes."));
        return;
    }
    if (account.id) {
        const Account &original = dialog.original_account();
        SecretBytes packet;
        bool packet_ready = false;
        if (incoming_password.empty()) {
            if (!dialog.reuse_incoming_password() ||
                !task6_account_secret_identity_unchanged(original, account, CredentialPurpose::Imap)) {
                fl_alert("%s", T("Enter a new incoming password when the IMAP server or user changes.")); return;
            }
            if (!packet_ready && !(packet_ready = make_live_credentials(packet, error))) {
                fl_alert("%s", T("Unlock the vault or enter the incoming password before reusing it.")); return;
            }
            if (!worker_find_credential(packet, original, CredentialPurpose::Imap, original.username,
                    coordinator_.profile_epoch(), incoming_password, error)) {
                fl_alert("%s", T("Unlock the vault or enter the incoming password before reusing it.")); return;
            }
        }
        if (outgoing_password.empty()) {
            if (!dialog.reuse_outgoing_password() ||
                !task6_account_secret_identity_unchanged(original, account, CredentialPurpose::Smtp)) {
                fl_alert("%s", T("Enter a new outgoing password when the SMTP server or user changes.")); return;
            }
            if (!packet_ready && !(packet_ready = make_live_credentials(packet, error))) {
                fl_alert("%s", T("Unlock the vault or enter the outgoing password before reusing it.")); return;
            }
            const Text &smtp_user = original.smtp_username.empty() ? original.username : original.smtp_username;
            if (!worker_find_credential(packet, original, CredentialPurpose::Smtp, smtp_user,
                    coordinator_.profile_epoch(), outgoing_password, error)) {
                fl_alert("%s", T("Unlock the vault or enter the outgoing password before reusing it.")); return;
            }
        }
    }
    const WorkerTaskKind kind = account.id ? WorkerTaskKind::AccountUpdate : WorkerTaskKind::AccountCreate;
    WorkerRequest request;
    request.account_id = account.id;
    request.account_generation = account.generation;
    WorkerTaskInput input;
    input.account = account;
    PendingUiRequest pending;
    pending.purpose = PendingPurpose::AccountSave;
    pending.account = account;
    pending.incoming_password = static_cast<SecretBytes &&>(incoming_password);
    pending.outgoing_password = static_cast<SecretBytes &&>(outgoing_password);
    ++ui_generation_;
    ++selection_generation_;
    rows_.clear();
    checked_rows_.clear();
    selected_row_valid_ = false;
    reader_pane_->clear_message(T("Account settings are being saved."));
    if (!submit_task(kind, request, &input, SecretBytes(), static_cast<PendingUiRequest &&>(pending), error)) {
        --ui_generation_; --selection_generation_;
        fl_alert("%s", error.c_str());
        schedule_query(true);
        return;
    }
    dialog.clear_passwords();
    dialog.hide();
}

bool MailWindow::make_live_credentials(SecretBytes &packet, Text &error) const {
    CredentialInput records[MaxAccounts * 2];
    for (size_t i = 0; i < credentials_.size(); ++i) {
        records[i].configuration = credentials_[i].account;
        records[i].purpose = credentials_[i].purpose;
        records[i].username = credentials_[i].purpose == CredentialPurpose::Smtp &&
            !credentials_[i].account.smtp_username.empty() ? credentials_[i].account.smtp_username :
                credentials_[i].account.username;
        records[i].secret = credentials_[i].password.data();
        records[i].secret_size = credentials_[i].password.size();
    }
    return worker_encode_credentials(records, credentials_.size(), coordinator_.profile_epoch(), packet, error);
}

bool MailWindow::install_credentials(const Account &account, SecretBytes incoming, SecretBytes outgoing) {
    if (credentials_.size() + 2 > credentials_.limit() || incoming.empty() || outgoing.empty()) return false;
    LiveCredential imap;
    imap.account = account;
    imap.purpose = CredentialPurpose::Imap;
    imap.password = static_cast<SecretBytes &&>(incoming);
    LiveCredential smtp;
    smtp.account = account;
    smtp.purpose = CredentialPurpose::Smtp;
    smtp.password = static_cast<SecretBytes &&>(outgoing);
    return credentials_.push(static_cast<LiveCredential &&>(imap)) &&
        credentials_.push(static_cast<LiveCredential &&>(smtp));
}

void MailWindow::remove_credentials(uint64_t account_id) {
    for (size_t i = credentials_.size(); i > 0; --i)
        if (credentials_[i - 1].account.id == account_id) credentials_.erase(i - 1);
}

bool MailWindow::set_unlocked_credentials(SecretBytes packet, Text &error) {
    BoundedArray<LiveCredential, MaxAccounts * 2> next;
    for (size_t i = 0; i < startup_.accounts.size(); ++i) {
        const Account &account = startup_.accounts[i];
        SecretBytes imap_password;
        Text ignored;
        if (worker_find_credential(packet, account, CredentialPurpose::Imap, account.username,
                coordinator_.profile_epoch(), imap_password, ignored)) {
            LiveCredential item;
            item.account = account;
            item.purpose = CredentialPurpose::Imap;
            item.password = static_cast<SecretBytes &&>(imap_password);
            if (!next.push(static_cast<LiveCredential &&>(item))) return set_error(error, T("Too many saved credentials."));
        }
        const Text &smtp_user = account.smtp_username.empty() ? account.username : account.smtp_username;
        SecretBytes smtp_password;
        if (worker_find_credential(packet, account, CredentialPurpose::Smtp, smtp_user,
                coordinator_.profile_epoch(), smtp_password, ignored)) {
            LiveCredential item;
            item.account = account;
            item.purpose = CredentialPurpose::Smtp;
            item.password = static_cast<SecretBytes &&>(smtp_password);
            if (!next.push(static_cast<LiveCredential &&>(item))) return set_error(error, T("Too many saved credentials."));
        }
    }
    if (next.empty()) return set_error(error, T("No saved account credential matches the current account settings."));
    credentials_ = static_cast<BoundedArray<LiveCredential, MaxAccounts * 2> &&>(next);
    return true;
}

}
