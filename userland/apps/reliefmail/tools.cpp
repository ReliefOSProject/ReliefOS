#include "tools.h"
#include "action_policy.h"
#include "window.h"
#include "../fltk_support.h"
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Scroll.H>
#include <FL/fl_draw.H>
#include <stdio.h>
#include <string.h>

namespace rmail {
namespace {
constexpr int kOperationDetailMaxHeight = 131072;
constexpr int kToolTabHeight = 34;
constexpr int kToolRibbonHeight = 100;
constexpr int kToolStatusHeight = 28;

Fl_Button *tool_button(int x, int y, int width, const char *label,
    MailToolsCommand command, Fl_Callback *callback, const char *tip = nullptr) {
    Fl_Button *button = new Fl_Button(x, y, width, 26, label);
    button->callback(callback, reinterpret_cast<void *>(static_cast<intptr_t>(command) + 1));
    if (tip) button->tooltip(tip);
    return button;
}

Fl_Button *tool_primary(int x, int y, int width, const char *label,
    MailToolsCommand command, Fl_Callback *callback,
    const char *tip = nullptr) {
    Fl_Button *button = new Fl_Button(x, y, width, 58, label);
    button->callback(callback, reinterpret_cast<void *>(static_cast<intptr_t>(command) + 1));
    if (tip) button->tooltip(tip);
    return button;
}

void tool_group(int x, int width, const char *label) {
    Fl_Box *caption = new Fl_Box(x, 105, width, 15, label);
    caption->labelsize(11);
    Fl_Box *separator = new Fl_Box(x + width - 3, 40, 2, 58);
    separator->box(FL_ENGRAVED_FRAME);
}

bool safe_choice_label(const char *source, Text &label) {
    Text clean;
    if (!task6_sanitize_single_line_display_text(source, strlen(source), clean)) return false;
    return task6_escape_fltk_label_symbols(clean.c_str(), clean.size(), label);
}

size_t bounded_utf8_prefix(const char *text, size_t size, size_t limit) {
    if (size <= limit) return size;
    size_t end = limit;
    while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
    return end;
}

bool preview_text(const Text &source, size_t limit, bool single_line, Text &output) {
    const size_t prefix_size = bounded_utf8_prefix(source.c_str(), source.size(), limit);
    Text clean;
    const bool sanitized = single_line ?
        task6_sanitize_single_line_display_text(source.c_str(), prefix_size, clean) :
        task6_sanitize_display_text(source.c_str(), prefix_size, clean);
    if (!sanitized) return false;
    if (prefix_size == source.size()) {
        output = static_cast<Text &&>(clean);
        return true;
    }
    Array<char> bounded;
    for (size_t i = 0; i < clean.size(); ++i)
        if (!bounded.push(clean.c_str()[i])) return false;
    static const char suffix[] = "…";
    for (size_t i = 0; i < sizeof(suffix) - 1; ++i)
        if (!bounded.push(suffix[i])) return false;
    return output.set(bounded.data(), bounded.size());
}

const char *stage_text(SendStage stage) {
    switch (stage) {
    case SendStage::Draft: return T("Draft");
    case SendStage::Ready: return T("Queued; not sent");
    case SendStage::Connecting: return T("Connecting");
    case SendStage::Authenticating: return T("Authenticating");
    case SendStage::SendingEnvelope: return T("Sending envelope");
    case SendStage::SendingData: return T("Sending message data");
    case SendStage::AwaitingFinalResponse: return T("Waiting for SMTP result");
    case SendStage::Sent: return T("Delivered; archive pending");
    case SendStage::Uncertain: return T("Delivery needs review");
    case SendStage::Failed: return T("Failed before delivery");
    case SendStage::Accepted: return T("Delivered; archive pending");
    case SendStage::ArchivePending: return T("Archive pending");
    case SendStage::ArchiveFailed: return T("Archive failed");
    case SendStage::Archived: return T("Archived in Sent");
    case SendStage::ReviewedDelivered: return T("Reviewed as delivered");
    }
    return T("Unknown delivery state");
}
}

MailToolsWindow::MailToolsWindow(MailWindow *owner)
    : Fl_Double_Window(960, 650, T("Mail tools")), owner_(owner) {
    size_range(820, 540);
    begin();
    tabs_[0] = new Fl_Button(8, 3, 104, 28, T("Folders"));
    tabs_[0]->type(FL_TOGGLE_BUTTON);
    tabs_[0]->callback(tab_cb, reinterpret_cast<void *>(static_cast<intptr_t>(0)));
    tabs_[1] = new Fl_Button(116, 3, 148, 28, T("Delivery queue"));
    tabs_[1]->type(FL_TOGGLE_BUTTON);
    tabs_[1]->callback(tab_cb, reinterpret_cast<void *>(static_cast<intptr_t>(1)));

    Fl_Group *ribbon = new Fl_Group(0, kToolTabHeight, w(), kToolRibbonHeight);
    ribbon_scroll_ = new Fl_Scroll(0, kToolTabHeight, w(), kToolRibbonHeight);
    ribbon_scroll_->type(Fl_Scroll::HORIZONTAL);
    pages_[0] = new Fl_Group(0, kToolTabHeight, 456, kToolRibbonHeight);
    folder_refresh_ = tool_primary(8, 42, 104, T("Refresh folders"), MailToolsCommand::RefreshFolders,
        command_cb, T("Read the selected account's current server folder list"));
    tool_group(4, 112, T("Server folders"));
    subscribe_ = tool_button(124, 42, 94, T("Subscribe"), MailToolsCommand::SubscribeFolder, command_cb);
    unsubscribe_ = tool_button(124, 71, 94, T("Unsubscribe"), MailToolsCommand::UnsubscribeFolder, command_cb);
    tool_group(120, 102, T("Membership"));
    create_folder_ = tool_button(234, 42, 98, T("New folder"), MailToolsCommand::CreateFolder, command_cb);
    rename_folder_ = tool_button(234, 71, 98, T("Rename"), MailToolsCommand::RenameFolder, command_cb);
    tool_group(230, 106, T("Edit folder"));
    delete_folder_ = tool_button(348, 42, 96, T("Delete folder"), MailToolsCommand::DeleteFolder, command_cb);
    clear_trash_ = tool_button(348, 71, 96, T("Clear Trash"), MailToolsCommand::ClearTrash, command_cb);
    tool_group(344, 104, T("Destructive actions"));
    pages_[0]->end();

    pages_[1] = new Fl_Group(0, kToolTabHeight, 586, kToolRibbonHeight);
    queue_refresh_ = tool_primary(8, 42, 100, T("Refresh queue"), MailToolsCommand::RefreshQueue, command_cb);
    queue_previous_ = tool_button(116, 42, 78, T("Previous"), MailToolsCommand::PreviousQueuePage, command_cb);
    queue_next_ = tool_button(116, 71, 78, T("Next"), MailToolsCommand::NextQueuePage, command_cb);
    tool_group(4, 196, T("Send history"));
    archive_retry_ = tool_button(206, 42, 116, T("Retry archive"), MailToolsCommand::RetryArchive,
        command_cb, T("Append the accepted message to its reviewed Sent folder over IMAP; never resends SMTP"));
    archive_review_ = tool_button(206, 71, 116, T("Choose archive target"), MailToolsCommand::ReviewArchiveTarget,
        command_cb, T("Review the current account generation and exact IMAP archive target"));
    tool_group(202, 124, T("Archive (IMAP only)"));
    delivered_ = tool_button(334, 42, 126, T("Confirm delivered"), MailToolsCommand::ConfirmDelivered, command_cb);
    not_delivered_ = tool_button(334, 71, 126, T("Confirm not delivered"), MailToolsCommand::ConfirmNotDelivered, command_cb);
    tool_group(330, 134, T("Uncertain delivery"));
    send_queued_ = tool_button(472, 42, 102, T("Send queued"), MailToolsCommand::SendQueued, command_cb,
        T("Submit this ready operation through SMTP only after explicit confirmation"));
    cancel_queued_ = tool_button(472, 71, 102, T("Cancel queued"), MailToolsCommand::CancelQueuedSend, command_cb);
    tool_group(468, 110, T("Ready operations"));
    pages_[1]->end();
    ribbon_scroll_->end();
    ribbon->resizable(ribbon_scroll_);
    ribbon->end();

    account_label_ = new Fl_Box(10, 136, 260, 16, T("Account"));
    account_label_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    account_choice_ = new Fl_Choice(10, 154, 260, 28);
    account_choice_->callback(account_cb, this);
    archive_target_label_ = new Fl_Box(286, 136, 664, 16, T("Archive destination"));
    archive_target_label_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    archive_target_choice_ = new Fl_Choice(286, 154, 664, 28);
    archive_target_choice_->tooltip(T("Choose a selectable folder in this same account"));
    folder_scroll_ = new Fl_Scroll(8, 190, w() - 16, h() - 218);
    folder_scroll_->type(Fl_Scroll::VERTICAL);
    folder_scroll_->box(FL_DOWN_BOX);
    folder_scroll_->end();
    queue_scroll_ = new Fl_Scroll(8, 190, w() - 16, h() - 342);
    queue_scroll_->type(Fl_Scroll::VERTICAL);
    queue_scroll_->box(FL_DOWN_BOX);
    queue_scroll_->end();
    detail_scroll_ = new Fl_Scroll(8, h() - 152, w() - 16, 124);
    detail_scroll_->type(Fl_Scroll::VERTICAL);
    detail_scroll_->box(FL_DOWN_BOX);
    detail_scroll_->begin();
    detail_ = new PlainTextBox(14, h() - 148, w() - 40, 1000,
        T("Select an operation to review its exact identity and last result."));
    detail_->box(FL_NO_BOX);
    detail_->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    detail_->labelsize(12);
    detail_scroll_->end();
    fit_plain_text_scroll(detail_scroll_, detail_, kOperationDetailMaxHeight);
    status_ = new PlainTextBox(8, h() - kToolStatusHeight, w() - 16, kToolStatusHeight - 2, T("Ready"));
    status_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    status_->labelsize(13);
    end();
    show_page(0);
}

void MailToolsWindow::show_page(int page) {
    if (page < 0 || page > 1) return;
    active_page_ = page;
    auto show = [](Fl_Widget *widget, bool visible) {
        if (!widget) return;
        if (visible) widget->show(); else widget->hide();
    };
    for (int i = 0; i < 2; ++i) {
        show(pages_[i], i == page);
        if (tabs_[i]) tabs_[i]->value(i == page);
    }
    show(folder_scroll_, page == 0);
    show(queue_scroll_, page == 1);
    show(detail_scroll_, page == 1);
    show(archive_target_choice_, page == 1);
    if (account_label_) account_label_->show();
    if (archive_target_label_) show(archive_target_label_, page == 1);
    if (account_choice_) account_choice_->show();
    update_controls();
    redraw();
}

void MailToolsWindow::resize(int x, int y, int width, int height) {
    if (detail_scroll_) detail_scroll_->scroll_to(0, 0);
    Fl_Double_Window::resize(x, y, width, height);
    if (!account_choice_) return;
    if (ribbon_scroll_) ribbon_scroll_->resize(0, kToolTabHeight, width, kToolRibbonHeight);
    const int account_width = width > 820 ? 260 : 220;
    const int target_x = 10 + account_width + 16;
    const int target_width = width - target_x - 10;
    account_label_->resize(10, 136, account_width, 16);
    account_choice_->resize(10, 154, account_width, 28);
    archive_target_label_->resize(target_x, 136, target_width, 16);
    archive_target_choice_->resize(target_x, 154, target_width, 28);
    folder_scroll_->resize(8, 190, width - 16, height - 218);
    queue_scroll_->resize(8, 190, width - 16, height - 342);
    detail_scroll_->resize(8, height - 152, width - 16, 124);
    detail_->resize(detail_scroll_->x() + 4, detail_scroll_->y() + 4,
        width - 40, detail_->h());
    fit_plain_text_scroll(detail_scroll_, detail_, kOperationDetailMaxHeight);
    status_->resize(8, height - kToolStatusHeight, width - 16, kToolStatusHeight - 2);
    render_folders();
    render_queue();
}

void MailToolsWindow::tab_cb(Fl_Widget *widget, void *data) {
    MailToolsWindow *window = static_cast<MailToolsWindow *>(widget->window());
    window->show_page(static_cast<int>(reinterpret_cast<intptr_t>(data)));
}

void MailToolsWindow::command_cb(Fl_Widget *widget, void *data) {
    MailToolsWindow *window = static_cast<MailToolsWindow *>(widget->window());
    const intptr_t encoded = reinterpret_cast<intptr_t>(data);
    if (window && window->owner_ && encoded > 0)
        window->owner_->perform_tool(static_cast<MailToolsCommand>(encoded - 1));
}

void MailToolsWindow::account_cb(Fl_Widget *widget, void *context) {
    MailToolsWindow *window = static_cast<MailToolsWindow *>(context);
    const int index = static_cast<Fl_Choice *>(widget)->value();
    if (!window || index < 0 || static_cast<size_t>(index) >= window->account_choice_ids_.size()) return;
    window->selected_account_id_ = window->account_choice_ids_[static_cast<size_t>(index)];
    window->selected_operation_id_ = 0;
    window->queue_page_loaded_ = false;
    window->send_page_.operations.clear();
    window->send_page_.account_id = window->selected_account_id_;
    window->send_page_.offset = 0;
    window->send_page_.has_more = false;
    window->render_folders();
    window->render_queue();
    window->update_controls();
    if (window->owner_) window->owner_->tools_account_changed(window->selected_account_id_);
}

void MailToolsWindow::folder_cb(Fl_Widget *widget, void *data) {
    MailToolsWindow *window = static_cast<MailToolsWindow *>(widget->window());
    const intptr_t index = reinterpret_cast<intptr_t>(data) - 1;
    if (!window || index < 0 || static_cast<size_t>(index) >= window->folders_.size()) return;
    const Folder &folder = window->folders_[static_cast<size_t>(index)];
    if (!folder.selectable) return;
    window->selected_folder_key_ = folder.key;
    for (size_t i = 0; i < window->folder_rows_.size(); ++i)
        if (window->folder_rows_[i]) window->folder_rows_[i]->value(window->folder_rows_[i] == widget);
    window->render_detail();
    window->update_controls();
}

void MailToolsWindow::operation_cb(Fl_Widget *widget, void *data) {
    MailToolsWindow *window = static_cast<MailToolsWindow *>(widget->window());
    const intptr_t index = reinterpret_cast<intptr_t>(data) - 1;
    if (!window || index < 0 || static_cast<size_t>(index) >= window->send_page_.operations.size()) return;
    window->selected_operation_id_ = window->send_page_.operations[static_cast<size_t>(index)].id;
    for (size_t i = 0; i < window->queue_rows_.size(); ++i)
        if (window->queue_rows_[i]) window->queue_rows_[i]->value(window->queue_rows_[i] == widget);
    window->render_detail();
    window->update_controls();
}

void MailToolsWindow::refresh_snapshot(const WorkerStartupSnapshot &snapshot,
    uint64_t selected_account_id) {
    AccountList accounts;
    FolderList folders;
    for (size_t i = 0; i < snapshot.accounts.size(); ++i)
        if (!accounts.push(snapshot.accounts[i])) { set_status(T("Account list is too large to show safely.")); return; }
    for (size_t i = 0; i < snapshot.folders.size(); ++i)
        if (!folders.push(snapshot.folders[i])) { set_status(T("Folder list is too large to show safely.")); return; }
    accounts_ = static_cast<AccountList &&>(accounts);
    folders_ = static_cast<FolderList &&>(folders);
    bool selected_found = false;
    for (size_t i = 0; i < accounts_.size(); ++i)
        if (accounts_[i].id == selected_account_id_) selected_found = true;
    if (!selected_found) selected_account_id_ = selected_account_id;
    account_choice_->clear();
    account_choice_ids_.clear();
    int selected_index = -1;
    for (size_t i = 0; i < accounts_.size(); ++i) {
        const Account &account = accounts_[i];
        const char *name = account.display_name.empty() ? account.email_address.c_str() : account.display_name.c_str();
        char label[1200];
        snprintf(label, sizeof(label), "%s <%s>", name, account.email_address.c_str());
        Text safe;
        if (!safe_choice_label(label, safe)) {
            snprintf(label, sizeof(label), "%s #%llu", T("Account"),
                static_cast<unsigned long long>(account.id));
            if (!safe_choice_label(label, safe)) continue;
        }
        account_choice_->add(safe.c_str());
        if (!account_choice_ids_.push(account.id)) {
            account_choice_->remove(account_choice_->size() - 1);
            continue;
        }
        if (account.id == selected_account_id_) selected_index = static_cast<int>(account_choice_ids_.size() - 1);
    }
    if (selected_index < 0 && accounts_.size()) {
        selected_index = account_choice_ids_.empty() ? -1 : 0;
        if (!account_choice_ids_.empty()) selected_account_id_ = account_choice_ids_[0];
    }
    account_choice_->value(selected_index);
    bool folder_still_exists = false;
    for (size_t i = 0; i < folders_.size(); ++i)
        if (folders_[i].key == selected_folder_key_ && folders_[i].selectable)
            folder_still_exists = true;
    if (!folder_still_exists) selected_folder_key_ = FolderKey();
    render_folders();
    render_queue();
    update_controls();
}

void MailToolsWindow::set_send_page(const WorkerSendOperationPage &page) {
    if (page.account_id != selected_account_id_) return;
    send_page_.operations.clear();
    send_page_.account_id = page.account_id;
    send_page_.offset = page.offset;
    send_page_.has_more = page.has_more;
    for (size_t i = 0; i < page.operations.size(); ++i)
        if (!send_page_.operations.push(page.operations[i])) break;
    queue_offset_ = page.offset;
    queue_page_loaded_ = true;
    bool selected_found = false;
    for (size_t i = 0; i < page.operations.size(); ++i)
        if (page.operations[i].id == selected_operation_id_) selected_found = true;
    if (!selected_found) selected_operation_id_ = page.operations.empty() ? 0 : page.operations[0].id;
    render_queue();
    render_detail();
    update_controls();
}

void MailToolsWindow::set_status(const char *text) {
    if (status_) status_->text(text && *text ? text : T("Ready"));
}

void MailToolsWindow::set_busy(bool busy) {
    busy_ = busy;
    update_controls();
}

void MailToolsWindow::render_folders() {
    FolderKey restore_focus_key;
    NavigationButton *focused = static_cast<NavigationButton *>(Fl::focus());
    bool restore_folder_focus = false;
    if (focused && folder_scroll_->contains(focused)) {
        for (size_t i = 0; i < folder_rows_.size(); ++i) {
            if (folder_rows_[i] != focused) continue;
            const intptr_t source_index = reinterpret_cast<intptr_t>(focused->argument()) - 1;
            if (source_index >= 0 && static_cast<size_t>(source_index) < folders_.size()) {
                restore_focus_key = folders_[static_cast<size_t>(source_index)].key;
                restore_folder_focus = true;
            }
            break;
        }
        Fl::focus(nullptr);
    }
    folder_rows_.clear();
    folder_scroll_->clear();
    folder_scroll_->begin();
    int y = folder_scroll_->y() + 4;
    NavigationButton *previous = nullptr;
    for (size_t i = 0; i < folders_.size(); ++i) {
        const Folder &folder = folders_[i];
        if (folder.key.account_id != selected_account_id_) continue;
        const char *display = folder.display_name.empty() ? folder.key.server_name.c_str() : folder.display_name.c_str();
        switch (folder.role) {
        case FolderRole::Inbox: display = T("Inbox"); break;
        case FolderRole::Sent: display = T("Sent"); break;
        case FolderRole::Drafts: display = T("Drafts"); break;
        case FolderRole::Trash: display = T("Trash"); break;
        case FolderRole::Junk: display = T("Junk"); break;
        case FolderRole::Archive: display = T("Archive"); break;
        case static_cast<FolderRole>(0): break;
        }
        char label[4352];
        snprintf(label, sizeof(label), "%s%s   ·   %s", display,
            folder.selectable ? "" : T("  (container)"), folder.key.server_name.c_str());
        NavigationButton *row = new NavigationButton(8, y, folder_scroll_->w() - 20, 28, label);
        row->labelsize(13);
        row->alignment(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
        if (!folder.selectable) row->deactivate();
        row->value(folder.key == selected_folder_key_);
        row->callback(folder_cb, reinterpret_cast<void *>(static_cast<intptr_t>(i) + 1));
        if (previous) { previous->next(row); row->previous(previous); }
        previous = row;
        if (!folder_rows_.push(row)) { delete row; break; }
        y += 31;
    }
    folder_scroll_->end();
    if (restore_folder_focus) {
        for (size_t i = 0; i < folder_rows_.size(); ++i) {
            NavigationButton *row = folder_rows_[i];
            const intptr_t source_index = reinterpret_cast<intptr_t>(row->argument()) - 1;
            if (source_index < 0 || static_cast<size_t>(source_index) >= folders_.size() ||
                folders_[static_cast<size_t>(source_index)].key != restore_focus_key) continue;
            if (row->take_focus()) {
                const int top = folder_scroll_->yposition();
                const int bottom = top + folder_scroll_->h();
                const int row_top = row->y() - folder_scroll_->y();
                if (row_top < top) folder_scroll_->scroll_to(0, row_top);
                else if (row_top + row->h() > bottom)
                    folder_scroll_->scroll_to(0, row_top + row->h() - folder_scroll_->h());
            }
            break;
        }
    }
    archive_target_choice_->clear();
    archive_targets_.clear();
    int target_index = -1;
    const Account *account = selected_account();
    for (size_t i = 0; i < folders_.size(); ++i) {
        const Folder &folder = folders_[i];
        if (!folder.selectable || folder.key.account_id != selected_account_id_) continue;
        const char *display = folder.display_name.empty() ? folder.key.server_name.c_str() : folder.display_name.c_str();
        char label[4352];
        snprintf(label, sizeof(label), "%s   [%s]", display, folder.key.server_name.c_str());
        Text safe;
        if (!safe_choice_label(label, safe) || !archive_targets_.push(folder.key)) continue;
        archive_target_choice_->add(safe.c_str());
        if (account && folder.key.server_name == account->sent_folder)
            target_index = static_cast<int>(archive_targets_.size() - 1);
        else if (target_index < 0 && folder.role == FolderRole::Sent)
            target_index = static_cast<int>(archive_targets_.size() - 1);
    }
    /* Never preselect an arbitrary folder for an accepted-message rebind. If
     * there is no current Sent mapping, the user must choose the exact target. */
    archive_target_choice_->value(target_index);
    render_detail();
    folder_scroll_->redraw();
}

void MailToolsWindow::render_queue() {
    queue_rows_.clear();
    queue_scroll_->clear();
    queue_scroll_->begin();
    int y = queue_scroll_->y() + 4;
    for (size_t i = 0; i < send_page_.operations.size(); ++i) {
        const SendOperationSummary &operation = send_page_.operations[i];
        Text sender, recipients;
        (void)preview_text(operation.sender, 256, true, sender);
        (void)preview_text(operation.recipients, 1024, true, recipients);
        char label[8192];
        snprintf(label, sizeof(label), "#%llu  %s  ·  %s  ·  %s → %s",
            static_cast<unsigned long long>(operation.id), stage_text(operation.stage),
            operation.message_id.c_str(), sender.c_str(), recipients.c_str());
        PlainTextButton *row = new PlainTextButton(8, y, queue_scroll_->w() - 20, 30, label);
        row->labelsize(12);
        row->callback(operation_cb, reinterpret_cast<void *>(static_cast<intptr_t>(i) + 1));
        row->value(operation.id == selected_operation_id_);
        if (!queue_rows_.push(row)) { delete row; break; }
        y += 32;
    }
    if (send_page_.operations.empty()) {
        PlainTextBox *empty = new PlainTextBox(16, y + 16, queue_scroll_->w() - 32, 48,
            queue_page_loaded_ ? T("No durable send operations are recorded for this account.") :
                T("Refresh the queue to load durable send history."));
        empty->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
        empty->labelsize(13);
    }
    queue_scroll_->end();
    queue_scroll_->redraw();
}

void MailToolsWindow::render_detail() {
    if (!detail_) return;
    if (active_page_ == 0) {
        const Folder *folder = selected_folder();
        if (!folder) {
            detail_->text(T("Select a server folder. Containers cannot be selected for server actions."));
        } else {
            char text[4608];
            snprintf(text, sizeof(text), "%s\n%s · generation %llu · UIDVALIDITY %llu · %u messages",
                folder->key.server_name.c_str(), folder->selectable ? T("Selectable") : T("Container"),
                static_cast<unsigned long long>(folder->generation),
                static_cast<unsigned long long>(folder->uidvalidity), folder->message_count);
            detail_->text(text);
        }
        detail_->hide();
        return;
    }
    detail_->show();
    const SendOperationSummary *operation = selected_operation();
    if (!operation) {
        detail_->text(T("Select an operation to review its exact identity and last result."));
        fit_plain_text_scroll(detail_scroll_, detail_, kOperationDetailMaxHeight);
        return;
    }
    Text sender, recipients, diagnostic, archive;
    (void)preview_text(operation->sender, 512, false, sender);
    (void)preview_text(operation->recipients, 2048, false, recipients);
    (void)preview_text(operation->last_diagnostic, 1024, false, diagnostic);
    (void)preview_text(operation->archive_folder, 512, true, archive);
    char text[8192];
    snprintf(text, sizeof(text), T("Operation #%llu · %s · delivery count %u\nMessage-ID: %s\nFrom: %s\nTo: %s\nArchive target: %s · account generation %llu · folder generation %llu · UIDVALIDITY %llu\n%s"),
        static_cast<unsigned long long>(operation->id), stage_text(operation->stage), operation->delivery_count,
        operation->message_id.c_str(), sender.c_str(), recipients.c_str(),
        archive.empty() ? T("not configured") : archive.c_str(),
        static_cast<unsigned long long>(operation->archive_review_generation),
        static_cast<unsigned long long>(operation->archive_folder_generation),
        static_cast<unsigned long long>(operation->archive_folder_uidvalidity), diagnostic.c_str());
    detail_->text(text);
    fit_plain_text_scroll(detail_scroll_, detail_, kOperationDetailMaxHeight);
}

void MailToolsWindow::update_controls() {
    const Account *account = selected_account();
    const Folder *folder = selected_folder();
    const SendOperationSummary *operation = selected_operation();
    const bool account_ready = account && account->enabled && !busy_;
    auto set = [](Fl_Widget *widget, bool enabled) {
        if (!widget) return;
        if (enabled) widget->activate(); else widget->deactivate();
    };
    set(folder_refresh_, account_ready);
    set(create_folder_, account_ready);
    set(subscribe_, account_ready && folder && folder->selectable);
    set(unsubscribe_, account_ready && folder && folder->selectable);
    set(rename_folder_, account_ready && folder && folder->selectable && folder->role == static_cast<FolderRole>(0));
    set(delete_folder_, account_ready && folder && folder->selectable && folder->role == static_cast<FolderRole>(0));
    set(clear_trash_, account_ready && folder && folder->selectable && folder->role == FolderRole::Trash);
    set(queue_refresh_, account_ready);
    set(queue_previous_, account_ready && queue_page_loaded_ && queue_offset_ >= MaxSendOperationPageSize);
    set(queue_next_, account_ready && queue_page_loaded_ && send_page_.has_more);
    const Folder *target = selected_archive_target();
    set(archive_review_, account_ready && operation &&
        task8a_archive_review_target_valid(*operation, account->id, target));
    set(archive_retry_, account_ready && operation &&
        task8a_archive_retryable(*operation, account->id, account->generation));
    set(delivered_, account_ready && operation && operation->stage == SendStage::Uncertain);
    set(not_delivered_, account_ready && operation && operation->stage == SendStage::Uncertain);
    set(send_queued_, account_ready && operation && operation->stage == SendStage::Ready &&
        operation->account_generation == account->generation);
    set(cancel_queued_, account_ready && operation && operation->stage == SendStage::Ready);
}

const Folder *MailToolsWindow::selected_folder() const {
    for (size_t i = 0; i < folders_.size(); ++i)
        if (folders_[i].key == selected_folder_key_ && folders_[i].key.account_id == selected_account_id_)
            return &folders_[i];
    return nullptr;
}

const Folder *MailToolsWindow::selected_archive_target() const {
    const int index = archive_target_choice_ ? archive_target_choice_->value() : -1;
    if (index < 0 || static_cast<size_t>(index) >= archive_targets_.size()) return nullptr;
    for (size_t i = 0; i < folders_.size(); ++i)
        if (folders_[i].key == archive_targets_[static_cast<size_t>(index)] && folders_[i].selectable)
            return &folders_[i];
    return nullptr;
}

const SendOperationSummary *MailToolsWindow::selected_operation() const {
    for (size_t i = 0; i < send_page_.operations.size(); ++i)
        if (send_page_.operations[i].id == selected_operation_id_) return &send_page_.operations[i];
    return nullptr;
}

const Account *MailToolsWindow::selected_account() const {
    for (size_t i = 0; i < accounts_.size(); ++i)
        if (accounts_[i].id == selected_account_id_) return &accounts_[i];
    return nullptr;
}

}
