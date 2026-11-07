#include "dialogs.h"
#include "ui_adapters.h"
#include "../fltk_support.h"
#include <FL/Fl_Button.H>
#include <FL/Fl_Box.H>
#include <FL/Fl.H>
#include <FL/Fl_Tabs.H>
#include <FL/fl_draw.H>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace rmail {
namespace {

constexpr int kAccountDialogWidth = 800;
constexpr int kAccountDialogHeight = 548;
constexpr int kAccountTabsX = 14;
constexpr int kAccountTabsY = 16;
constexpr int kAccountTabsWidth = 772;
constexpr int kAccountTabsHeight = 340;
constexpr int kAccountPageY = 40;
constexpr int kAccountPageHeight = 316;
constexpr int kAccountFooterNoteY = 374;
constexpr int kAccountFooterNoteHeight = 56;
constexpr int kAccountActionsY = 496;
constexpr int kAccountActionHeight = 32;

Fl_Input *add_input(Fl_Group *group, int x, int y, int width, int height,
    const char *value = nullptr) {
    Fl_Input *input = new Fl_Input(x, y, width, height);
    input->align(FL_ALIGN_LEFT);
    if (value) input->value(value);
    group->add(input);
    return input;
}

Fl_Secret_Input *add_secret(Fl_Group *group, int x, int y, int width, int height,
    const char * = nullptr) {
    Fl_Secret_Input *input = new Fl_Secret_Input(x, y, width, height);
    input->align(FL_ALIGN_LEFT);
    group->add(input);
    return input;
}

void add_field_label(Fl_Group *group, const Fl_Widget *field, int x, int y,
    int width, int height, const char *text) {
    Fl_Box *label = new Fl_Box(x, y, width, height, text);
    label->labelfont(field ? field->labelfont() : FL_HELVETICA);
    label->labelsize(field ? field->labelsize() : 14);
    label->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    label->box(FL_NO_BOX);
    group->add(label);
    if (!text || width <= 0) return;
    fl_font(label->labelfont(), label->labelsize());
    const int measured = static_cast<int>(fl_width(text) + 0.5);
    if (measured > width) {
        int size = label->labelsize();
        while (size > 10) {
            label->labelsize(--size);
            fl_font(label->labelfont(), size);
            if (fl_width(text) <= width) break;
        }
    }
}

void add_full_row_field_label(Fl_Group *group, const Fl_Widget *field, int x, int y,
    int width, int height, const char *text) {
    Fl_Box *label = new Fl_Box(x, y, width, height, text);
    label->labelfont(field ? field->labelfont() : FL_HELVETICA);
    label->labelsize(field ? field->labelsize() : 14);
    label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    label->box(FL_NO_BOX);
    group->add(label);
    if (!text || width <= 0) return;
    int size = label->labelsize();
    fl_font(label->labelfont(), size);
    while (size > 10 && fl_width(text) > width) {
        label->labelsize(--size);
        fl_font(label->labelfont(), size);
    }
}

bool parse_u32(const char *value, uint32_t minimum, uint32_t maximum, uint32_t &out) {
    if (!value || !*value) return false;
    char *end = nullptr;
    const unsigned long parsed = strtoul(value, &end, 10);
    if (!end || *end || parsed < minimum || parsed > maximum) return false;
    out = static_cast<uint32_t>(parsed);
    return true;
}

bool copy_text(Text &out, const Fl_Input *input) {
    return out.set(input && input->value() ? input->value() : "");
}

uint32_t security_value(const Fl_Choice *choice) {
    return choice && choice->value() == 1 ? static_cast<uint32_t>(TransportSecurity::StartTls) :
        static_cast<uint32_t>(TransportSecurity::ImplicitTls);
}

void set_input(Fl_Input *input, const Text &value) {
    if (input) input->value(value.c_str());
}

bool copy_secret(SecretBytes &secret, const Fl_Secret_Input *input) {
    const char *value = input && input->value() ? input->value() : "";
    return secret.set(value, strlen(value));
}

}

AccountSetupDialog::AccountSetupDialog(const Account *account,
    void (*callback)(void *, AccountSetupDialog &, AccountDialogAction), void *context,
    uint64_t profile_epoch)
    : Fl_Double_Window(kAccountDialogWidth, kAccountDialogHeight,
          account && account->id ? T("Edit mail account") : T("Add mail account")),
      original_(account ? *account : Account()), profile_epoch_(profile_epoch),
      callback_(callback), context_(context) {
    set_modal();
    begin();

    Fl_Tabs *tabs = new Fl_Tabs(kAccountTabsX, kAccountTabsY, kAccountTabsWidth,
        kAccountTabsHeight);
    Fl_Group *identity = new Fl_Group(kAccountTabsX, kAccountPageY, kAccountTabsWidth,
        kAccountPageHeight, T("Identity"));
    preset_ = new Fl_Choice(146, 88, 280, 28);
    add_field_label(identity, preset_, 24, 88, 114, 28, T("Provider"));
    preset_->add(T("Custom IMAP"));
    preset_->add(T("QQ Mail"));
    preset_->add(T("163 Mail"));
    preset_->value(0);
    preset_->callback(preset_cb, this);
    display_name_ = add_input(identity, 146, 132, 276, 28);
    add_field_label(identity, display_name_, 24, 132, 114, 28, T("Display name"));
    email_ = add_input(identity, 510, 132, 264, 28);
    add_field_label(identity, email_, 430, 132, 74, 28, T("Email address"));
    authentication_ = new Fl_Choice(146, 176, 276, 28);
    add_field_label(identity, authentication_, 24, 176, 114, 28, T("Authentication"));
    authentication_->add(T("Password"));
    authentication_->add(T("Application password / code"));
    authentication_->value(0);
    enabled_ = new Fl_Check_Button(510, 176, 112, 28, T("Enabled"));
    is_default_ = new Fl_Check_Button(638, 176, 138, 28, T("Default account"));
    Fl_Box *identity_note = new Fl_Box(28, 230, 740, 58,
        T("IMAP is required in this build. OAuth2-only login is not available. Use your provider's application password when required."));
    identity_note->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    identity->end();

    Fl_Group *incoming = new Fl_Group(kAccountTabsX, kAccountPageY, kAccountTabsWidth,
        kAccountPageHeight, T("Incoming"));
    incoming_host_ = add_input(incoming, 146, 86, 628, 28);
    add_field_label(incoming, incoming_host_, 24, 86, 114, 28, T("IMAP server"));
    incoming_port_ = add_input(incoming, 146, 126, 82, 28);
    add_field_label(incoming, incoming_port_, 24, 126, 114, 28, T("Port"));
    incoming_security_ = new Fl_Choice(146, 166, 190, 28);
    add_field_label(incoming, incoming_security_, 24, 166, 114, 28, T("Security"));
    incoming_security_->add(T("Implicit TLS"));
    incoming_security_->add(T("STARTTLS"));
    incoming_security_->value(0);
    username_ = add_input(incoming, 184, 206, 590, 28);
    add_full_row_field_label(incoming, username_, 24, 206, 154, 28, T("IMAP username"));
    incoming_password_ = add_secret(incoming, 184, 242, 590, 28);
    add_full_row_field_label(incoming, incoming_password_, 24, 242, 154, 28,
        T("IMAP password / app code"));
    reuse_incoming_ = new Fl_Check_Button(24, 278, 750, 26,
        T("Reuse session password if identity is unchanged"));
    Fl_Box *incoming_note = new Fl_Box(24, 314, 740, 28,
        T("Incoming and outgoing authentication are tested separately. Tests never send mail."));
    incoming_note->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    incoming->end();

    Fl_Group *outgoing = new Fl_Group(kAccountTabsX, kAccountPageY, kAccountTabsWidth,
        kAccountPageHeight, T("Outgoing"));
    outgoing_host_ = add_input(outgoing, 146, 86, 628, 28);
    add_field_label(outgoing, outgoing_host_, 24, 86, 114, 28, T("SMTP server"));
    outgoing_port_ = add_input(outgoing, 146, 126, 82, 28);
    add_field_label(outgoing, outgoing_port_, 24, 126, 114, 28, T("Port"));
    outgoing_security_ = new Fl_Choice(146, 166, 190, 28);
    add_field_label(outgoing, outgoing_security_, 24, 166, 114, 28, T("Security"));
    outgoing_security_->add(T("Implicit TLS"));
    outgoing_security_->add(T("STARTTLS"));
    outgoing_security_->value(1);
    smtp_username_ = add_input(outgoing, 184, 206, 590, 28);
    add_full_row_field_label(outgoing, smtp_username_, 24, 206, 154, 28, T("SMTP username"));
    outgoing_password_ = add_secret(outgoing, 184, 242, 590, 28);
    add_full_row_field_label(outgoing, outgoing_password_, 24, 242, 154, 28,
        T("SMTP password / app code"));
    reuse_outgoing_ = new Fl_Check_Button(24, 278, 750, 26,
        T("Reuse session password if identity is unchanged"));
    outgoing->end();

    Fl_Group *advanced = new Fl_Group(kAccountTabsX, kAccountPageY, kAccountTabsWidth,
        kAccountPageHeight, T("Advanced"));
    ca_file_ = add_input(advanced, 204, 94, 564, 28);
    add_field_label(advanced, ca_file_, 24, 94, 172, 28, T("Custom CA file (optional)"));
    Fl_Box *ca_note = new Fl_Box(204, 123, 564, 19,
        T("Leave blank to use the system certificates. Private CAs are provided by your administrator."));
    ca_note->labelsize(12);
    ca_note->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    connect_timeout_ = add_input(advanced, 204, 146, 154, 28);
    add_field_label(advanced, connect_timeout_, 24, 146, 172, 28, T("Connect timeout ms"));
    io_timeout_ = add_input(advanced, 614, 146, 154, 28);
    add_field_label(advanced, io_timeout_, 414, 146, 192, 28, T("I/O timeout ms"));
    poll_interval_ = add_input(advanced, 204, 194, 154, 28);
    add_field_label(advanced, poll_interval_, 24, 194, 172, 28, T("Poll minutes"));
    cache_days_ = add_input(advanced, 614, 194, 154, 28);
    add_field_label(advanced, cache_days_, 414, 194, 192, 28, T("Cache days"));

    Fl_Button *repair_ca = new Fl_Button(204, 240, 340, 30, T("Repair/update system certificates"));
    repair_ca->callback(action_cb,
        reinterpret_cast<void *>(static_cast<intptr_t>(AccountDialogAction::RepairCertificates) + 1));
    Fl_Box *repair_note = new Fl_Box(204, 280, 564, 50,
        T("ReliefAPK downloads the signed CA package and requests administrator authentication. Test the connection again after repair."));
    repair_note->labelsize(12);
    repair_note->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);

    advanced->end();

    Fl_Group *folders = new Fl_Group(kAccountTabsX, kAccountPageY, kAccountTabsWidth,
        kAccountPageHeight, T("Folders"));
    inbox_ = add_input(folders, 146, 86, 212, 28);
    add_field_label(folders, inbox_, 24, 86, 114, 28, T("Inbox folder"));
    sent_ = add_input(folders, 556, 86, 212, 28);
    add_field_label(folders, sent_, 414, 86, 134, 28, T("Sent folder"));
    drafts_ = add_input(folders, 146, 134, 212, 28);
    add_field_label(folders, drafts_, 24, 134, 114, 28, T("Drafts folder"));
    trash_ = add_input(folders, 556, 134, 212, 28);
    add_field_label(folders, trash_, 414, 134, 134, 28, T("Trash folder"));
    junk_ = add_input(folders, 146, 182, 212, 28);
    add_field_label(folders, junk_, 24, 182, 114, 28, T("Junk folder"));
    archive_ = add_input(folders, 556, 182, 212, 28);
    add_field_label(folders, archive_, 414, 182, 134, 28, T("Archive folder"));
    folders->end();
    tabs->end();

    Fl_Box *pass_note = new Fl_Box(18, kAccountFooterNoteY, 764,
        kAccountFooterNoteHeight,
        T("New accounts need both passwords. For an edit, leave a password blank only when reusing a matching session credential; a changed server or user needs a new password. Credentials stay in this session unless you explicitly save them to the encrypted vault."));
    pass_note->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    Fl_Button *test_in = new Fl_Button(18, kAccountActionsY, 132, kAccountActionHeight,
        T("Test incoming"));
    test_in->callback(action_cb, reinterpret_cast<void *>(static_cast<intptr_t>(AccountDialogAction::TestIncoming) + 1));
    Fl_Button *test_out = new Fl_Button(158, kAccountActionsY, 132, kAccountActionHeight,
        T("Test outgoing"));
    test_out->callback(action_cb, reinterpret_cast<void *>(static_cast<intptr_t>(AccountDialogAction::TestOutgoing) + 1));
    if (account && account->id) {
        Fl_Button *remove = new Fl_Button(298, kAccountActionsY, 112, kAccountActionHeight,
            T("Delete account"));
        remove->callback(action_cb, reinterpret_cast<void *>(static_cast<intptr_t>(AccountDialogAction::Delete) + 1));
    }
    Fl_Button *cancel = new Fl_Button(554, kAccountActionsY, 104, kAccountActionHeight,
        T("Cancel"));
    cancel->callback(action_cb, reinterpret_cast<void *>(static_cast<intptr_t>(AccountDialogAction::Cancel) + 1));
    Fl_Button *save = new Fl_Button(670, kAccountActionsY, 112, kAccountActionHeight,
        T("Save account"));
    save->callback(action_cb, reinterpret_cast<void *>(static_cast<intptr_t>(AccountDialogAction::Save) + 1));

    if (account) {
        set_input(display_name_, account->display_name);
        set_input(email_, account->email_address);
        set_input(incoming_host_, account->incoming_host);
        char number[32];
        snprintf(number, sizeof(number), "%u", account->incoming_port);
        incoming_port_->value(number);
        set_input(username_, account->username);
        incoming_security_->value(static_cast<int>(account->incoming_security));
        reuse_incoming_->value(1);
        set_input(outgoing_host_, account->outgoing_host);
        snprintf(number, sizeof(number), "%u", account->outgoing_port);
        outgoing_port_->value(number);
        set_input(smtp_username_, account->smtp_username);
        outgoing_security_->value(static_cast<int>(account->outgoing_security));
        reuse_outgoing_->value(1);
        authentication_->value(static_cast<int>(account->authentication));
        enabled_->value(account->enabled);
        is_default_->value(account->is_default);
        set_input(ca_file_, account->custom_ca_file);
        snprintf(number, sizeof(number), "%u", account->connect_timeout_ms);
        connect_timeout_->value(number);
        snprintf(number, sizeof(number), "%u", account->io_timeout_ms);
        io_timeout_->value(number);
        snprintf(number, sizeof(number), "%u", account->poll_interval_minutes);
        poll_interval_->value(number);
        snprintf(number, sizeof(number), "%u", account->cache_days);
        cache_days_->value(number);
        set_input(inbox_, account->inbox_folder);
        set_input(sent_, account->sent_folder);
        set_input(drafts_, account->drafts_folder);
        set_input(trash_, account->trash_folder);
        set_input(junk_, account->junk_folder);
        set_input(archive_, account->archive_folder);
    } else {
        enabled_->value(1);
        reuse_incoming_->value(0);
        reuse_outgoing_->value(0);
        connect_timeout_->value("15000");
        io_timeout_->value("30000");
        poll_interval_->value("0");
        cache_days_->value("30");
        inbox_->value("INBOX");
        sent_->value("Sent");
        drafts_->value("Drafts");
        trash_->value("Trash");
        junk_->value("Junk");
        archive_->value("Archive");
    }
    end();
    size_range(kAccountDialogWidth, kAccountDialogHeight);
    reliefos_fltk::style(this);
}

void AccountSetupDialog::action_cb(Fl_Widget *widget, void *data) {
    AccountSetupDialog *dialog = static_cast<AccountSetupDialog *>(widget->window());
    if (!dialog) return;
    const intptr_t value = reinterpret_cast<intptr_t>(data) - 1;
    if (dialog->callback_) dialog->callback_(dialog->context_, *dialog,
        static_cast<AccountDialogAction>(value));
}

void AccountSetupDialog::preset_cb(Fl_Widget *, void *context) {
    AccountSetupDialog *dialog = static_cast<AccountSetupDialog *>(context);
    dialog->apply_preset(dialog->preset_->value());
}

void AccountSetupDialog::apply_preset(int preset) {
    if (preset == 1) {
        incoming_host_->value("imap.qq.com"); incoming_port_->value("993"); incoming_security_->value(0);
        outgoing_host_->value("smtp.qq.com"); outgoing_port_->value("465"); outgoing_security_->value(0);
    } else if (preset == 2) {
        incoming_host_->value("imap.163.com"); incoming_port_->value("993"); incoming_security_->value(0);
        outgoing_host_->value("smtp.163.com"); outgoing_port_->value("465"); outgoing_security_->value(0);
    }
}

bool AccountSetupDialog::read(Account &account, SecretBytes &incoming_password,
    SecretBytes &outgoing_password, Text &error, bool require_both_passwords) const {
    Account next = original_;
    if (!copy_text(next.display_name, display_name_) || !copy_text(next.email_address, email_) ||
        !copy_text(next.incoming_host, incoming_host_) || !copy_text(next.username, username_) ||
        !copy_text(next.outgoing_host, outgoing_host_) || !copy_text(next.smtp_username, smtp_username_) ||
        !copy_text(next.custom_ca_file, ca_file_) || !copy_text(next.inbox_folder, inbox_) ||
        !copy_text(next.sent_folder, sent_) || !copy_text(next.drafts_folder, drafts_) ||
        !copy_text(next.trash_folder, trash_) || !copy_text(next.junk_folder, junk_) ||
        !copy_text(next.archive_folder, archive_)) {
        error.set(T("Not enough memory to read account settings."));
        return false;
    }
    uint32_t incoming_port = 0, outgoing_port = 0, connect_timeout = 0, io_timeout = 0;
    uint32_t poll = 0, cache = 0;
    if (!parse_u32(incoming_port_->value(), 1, 65535, incoming_port) ||
        !parse_u32(outgoing_port_->value(), 1, 65535, outgoing_port) ||
        !parse_u32(connect_timeout_->value(), 1, 300000, connect_timeout) ||
        !parse_u32(io_timeout_->value(), 1, 600000, io_timeout) ||
        !parse_u32(poll_interval_->value(), 0, 10080, poll) ||
        !parse_u32(cache_days_->value(), 0, 36500, cache)) {
        error.set(T("Check the port, timeout, poll interval, and cache days."));
        return false;
    }
    next.incoming_port = incoming_port;
    next.outgoing_port = outgoing_port;
    next.incoming_protocol = IncomingProtocol::Imap;
    next.incoming_security = static_cast<TransportSecurity>(security_value(incoming_security_));
    next.outgoing_security = static_cast<TransportSecurity>(security_value(outgoing_security_));
    next.authentication = authentication_->value() == 1 ? AuthenticationMethod::ApplicationPassword :
        AuthenticationMethod::Password;
    next.enabled = enabled_->value() != 0;
    next.is_default = is_default_->value() != 0;
    next.connect_timeout_ms = connect_timeout;
    next.io_timeout_ms = io_timeout;
    next.poll_interval_minutes = poll;
    next.cache_days = cache;
    if (!task6_validate_account_form(next, error)) return false;
    if (!copy_secret(incoming_password, incoming_password_) ||
        !copy_secret(outgoing_password, outgoing_password_)) {
        incoming_password.clear(); outgoing_password.clear();
        error.set(T("Not enough memory to handle account passwords."));
        return false;
    }
    if (require_both_passwords && (incoming_password.empty() || outgoing_password.empty())) {
        incoming_password.clear(); outgoing_password.clear();
        error.set(T("Enter both incoming and outgoing passwords or application codes."));
        return false;
    }
    account = static_cast<Account &&>(next);
    error.clear();
    return true;
}

void AccountSetupDialog::clear_passwords() {
    incoming_password_->value("");
    outgoing_password_->value("");
}

void AccountSetupDialog::show_modal() {
    show();
    while (shown()) Fl::wait();
}

MasterPasswordDialog::MasterPasswordDialog(const char *title, bool confirm)
    : Fl_Double_Window(460, confirm ? 188 : 142, title), confirm_(confirm) {
    set_modal();
    begin();
    first_ = new Fl_Secret_Input(154, 24, 286, 30, confirm ? T("New master password") : T("Master password"));
    if (confirm_) second_ = new Fl_Secret_Input(154, 64, 286, 30, T("Confirm password"));
    Fl_Button *cancel = new Fl_Button(214, confirm ? 130 : 84, 104, 30, T("Cancel"));
    cancel->callback(cancel_cb, this);
    Fl_Button *ok = new Fl_Button(330, confirm ? 130 : 84, 110, 30, T("Continue"));
    ok->callback(accept_cb, this);
    end();
}

void MasterPasswordDialog::accept_cb(Fl_Widget *, void *context) {
    MasterPasswordDialog *dialog = static_cast<MasterPasswordDialog *>(context);
    const char *first = dialog->first_->value();
    const char *second = dialog->confirm_ ? dialog->second_->value() : first;
    if (!first || !*first || !second || strcmp(first, second)) {
        fl_alert("%s", T("Enter a non-empty matching password."));
        return;
    }
    if (!dialog->accepted_first_.set(first, strlen(first)) ||
        !dialog->accepted_second_.set(dialog->confirm_ ? second : "",
            dialog->confirm_ ? strlen(second) : 0)) {
        dialog->accepted_first_.clear();
        dialog->accepted_second_.clear();
        fl_alert("%s", T("Not enough memory to handle the password."));
        return;
    }
    dialog->accepted_ = true;
    dialog->first_->value("");
    if (dialog->second_) dialog->second_->value("");
    dialog->hide();
}

void MasterPasswordDialog::cancel_cb(Fl_Widget *, void *context) {
    MasterPasswordDialog *dialog = static_cast<MasterPasswordDialog *>(context);
    dialog->accepted_ = false;
    dialog->first_->value("");
    if (dialog->second_) dialog->second_->value("");
    dialog->hide();
}

bool MasterPasswordDialog::run(SecretBytes &first, SecretBytes &second) {
    accepted_ = false;
    accepted_first_.clear();
    accepted_second_.clear();
    show();
    while (shown()) Fl::wait();
    if (!accepted_) return false;
    first = static_cast<SecretBytes &&>(accepted_first_);
    second = static_cast<SecretBytes &&>(accepted_second_);
    return true;
}

QueryFilterDialog::QueryFilterDialog(const QueryFilterSettings &settings)
    : Fl_Double_Window(520, 414, T("Search filters")) {
    set_modal();
    begin();
    summary_ = new Fl_Box(20, 7, 474, 20);
    summary_->labelsize(12);
    summary_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    summary_->box(FL_NO_BOX);
    search_field_ = new Fl_Choice(164, 32, 330, 28, T("Search field"));
    search_field_->add(T("All"));
    search_field_->add(T("From"));
    search_field_->add(T("Subject"));
    search_field_->add(T("Body"));
    search_field_->add(T("Message-ID"));
    search_field_->value(message_query_field_choice(settings.search.field));
    search_field_->callback(selection_cb, this);
    scope_ = new Fl_Choice(164, 68, 330, 28, T("Search scope"));
    scope_->add(T("Unified"));
    scope_->add(T("Account"));
    scope_->add(T("Folder"));
    scope_->add(T("All mail"));
    scope_->value(message_query_scope_choice(settings.search.scope));
    scope_->callback(selection_cb, this);
    sort_ = new Fl_Choice(164, 104, 330, 28, T("Sort whole query"));
    sort_->add(T("Newest received first"));
    sort_->add(T("Oldest received first"));
    sort_->add(T("Sender"));
    sort_->add(T("Subject"));
    sort_->value(static_cast<int>(settings.sort));
    attachments_ = new Fl_Choice(164, 140, 330, 28, T("Attachment state"));
    attachments_->add(T("Any (including unknown)"));
    attachments_->add(T("Has attachments"));
    attachments_->add(T("No attachments"));
    attachments_->add(T("Unknown only"));
    attachments_->value(static_cast<int>(settings.attachments));
    unread_ = new Fl_Check_Button(164, 176, 145, 26, T("Unread only"));
    unread_->value(settings.unread_only);
    starred_ = new Fl_Check_Button(320, 176, 174, 26, T("Starred only"));
    starred_->value(settings.starred_only);
    Fl_Box *after_label = new Fl_Box(20, 208, 220, 20, T("Received date start (UTC)"));
    after_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    Fl_Box *before_label = new Fl_Box(260, 208, 234, 20, T("Received date end (UTC)"));
    before_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    after_ = new Fl_Input(20, 228, 220, 28);
    before_ = new Fl_Input(260, 228, 234, 28);
    after_->value(settings.received_after.c_str());
    before_->value(settings.received_before.c_str());
    Fl_Box *date_note = new Fl_Box(20, 260, 474, 38,
        T("Dates use received time (IMAP INTERNALDATE) in UTC; both dates are inclusive, format YYYY-MM-DD."));
    date_note->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    server_ = new Fl_Check_Button(164, 306, 330, 26, T("Search selected IMAP folder on server"));
    server_->value(settings.server_search);
    Fl_Box *server_note = new Fl_Box(20, 336, 474, 34,
        T("Server search cannot apply local-only date, unread, starred, attachment, unified-inbox, or all-mail scope."));
    server_note->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    Fl_Button *cancel = new Fl_Button(272, 378, 104, 28, T("Cancel"));
    cancel->callback(cancel_cb, this);
    Fl_Button *apply = new Fl_Button(390, 378, 104, 28, T("Apply"));
    apply->callback(accept_cb, this);
    end();
    refresh_summary();
}

void QueryFilterDialog::selection_cb(Fl_Widget *, void *context) {
    static_cast<QueryFilterDialog *>(context)->refresh_summary();
}

void QueryFilterDialog::refresh_summary() {
    const char *field_labels[] = {T("All"), T("From"), T("Subject"), T("Body"), T("Message-ID")};
    const char *scope_labels[] = {T("Unified"), T("Account"), T("Folder"), T("All mail")};
    const QuerySearchSelection selection =
        search_selection_from_choices(search_field_->value(), scope_->value());
    char summary[256];
    if (!format_search_selection_summary(selection, T("Field: %s · Scope: %s"),
            field_labels, scope_labels, summary, sizeof(summary))) {
        summary_->copy_label("");
        return;
    }
    summary_->copy_label(summary);
}

void QueryFilterDialog::accept_cb(Fl_Widget *, void *context) {
    QueryFilterDialog *dialog = static_cast<QueryFilterDialog *>(context);
    dialog->accepted_ = true;
    dialog->hide();
}

void QueryFilterDialog::cancel_cb(Fl_Widget *, void *context) {
    QueryFilterDialog *dialog = static_cast<QueryFilterDialog *>(context);
    dialog->accepted_ = false;
    dialog->hide();
}

bool QueryFilterDialog::run(QueryFilterSettings &settings) {
    accepted_ = false;
    show();
    while (shown()) Fl::wait();
    if (!accepted_) return false;
    QueryFilterSettings next = settings;
    next.search = search_selection_from_choices(search_field_->value(), scope_->value());
    next.sort = static_cast<MessageQuerySort>(sort_->value());
    next.attachments = static_cast<AttachmentQueryFilter>(attachments_->value());
    next.unread_only = unread_->value() != 0;
    next.starred_only = starred_->value() != 0;
    next.server_search = server_->value() != 0;
    if (!next.received_after.set(after_->value() ? after_->value() : "") ||
        !next.received_before.set(before_->value() ? before_->value() : "")) return false;
    settings = static_cast<QueryFilterSettings &&>(next);
    return true;
}

}
