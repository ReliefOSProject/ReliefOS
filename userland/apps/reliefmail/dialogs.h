#ifndef RELIEFMAIL_DIALOGS_H
#define RELIEFMAIL_DIALOGS_H

#include "model.h"
#include "mail_query_presentation.h"
#include <FL/Fl_Box.H>
#include "vault.h"
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Secret_Input.H>

namespace rmail {

enum class AccountDialogAction : uint8_t { Save, TestIncoming, TestOutgoing, Delete, Cancel, RepairCertificates };

class AccountSetupDialog : public Fl_Double_Window {
    Fl_Choice *preset_ = nullptr;
    Fl_Input *display_name_ = nullptr;
    Fl_Input *email_ = nullptr;
    Fl_Input *incoming_host_ = nullptr;
    Fl_Input *incoming_port_ = nullptr;
    Fl_Input *username_ = nullptr;
    Fl_Choice *incoming_security_ = nullptr;
    Fl_Secret_Input *incoming_password_ = nullptr;
    Fl_Check_Button *reuse_incoming_ = nullptr;
    Fl_Input *outgoing_host_ = nullptr;
    Fl_Input *outgoing_port_ = nullptr;
    Fl_Input *smtp_username_ = nullptr;
    Fl_Choice *outgoing_security_ = nullptr;
    Fl_Secret_Input *outgoing_password_ = nullptr;
    Fl_Check_Button *reuse_outgoing_ = nullptr;
    Fl_Choice *authentication_ = nullptr;
    Fl_Check_Button *enabled_ = nullptr;
    Fl_Check_Button *is_default_ = nullptr;
    Fl_Input *ca_file_ = nullptr;
    Fl_Input *connect_timeout_ = nullptr;
    Fl_Input *io_timeout_ = nullptr;
    Fl_Input *poll_interval_ = nullptr;
    Fl_Input *cache_days_ = nullptr;
    Fl_Input *inbox_ = nullptr;
    Fl_Input *sent_ = nullptr;
    Fl_Input *drafts_ = nullptr;
    Fl_Input *trash_ = nullptr;
    Fl_Input *junk_ = nullptr;
    Fl_Input *archive_ = nullptr;
    Account original_;
    uint64_t profile_epoch_ = 0;
    void (*callback_)(void *, AccountSetupDialog &, AccountDialogAction) = nullptr;
    void *context_ = nullptr;

    static void action_cb(Fl_Widget *, void *);
    static void preset_cb(Fl_Widget *, void *);
    void apply_preset(int preset);

public:
    AccountSetupDialog(const Account *account,
        void (*callback)(void *, AccountSetupDialog &, AccountDialogAction), void *context,
        uint64_t profile_epoch = 0);
    bool read(Account &account, SecretBytes &incoming_password,
        SecretBytes &outgoing_password, Text &error, bool require_both_passwords = true) const;
    void clear_passwords();
    bool reuse_incoming_password() const { return reuse_incoming_ && reuse_incoming_->value() != 0; }
    bool reuse_outgoing_password() const { return reuse_outgoing_ && reuse_outgoing_->value() != 0; }
    const Account &original_account() const { return original_; }
    uint64_t profile_epoch() const { return profile_epoch_; }
    void show_modal();
};

class MasterPasswordDialog : public Fl_Double_Window {
    Fl_Secret_Input *first_ = nullptr;
    Fl_Secret_Input *second_ = nullptr;
    SecretBytes accepted_first_;
    SecretBytes accepted_second_;
    bool confirm_ = false;
    bool accepted_ = false;
    static void accept_cb(Fl_Widget *, void *);
    static void cancel_cb(Fl_Widget *, void *);

public:
    MasterPasswordDialog(const char *title, bool confirm);
    bool run(SecretBytes &first, SecretBytes &second);
};

struct QueryFilterSettings {
    QuerySearchSelection search;
    MessageQuerySort sort = MessageQuerySort::Newest;
    AttachmentQueryFilter attachments = AttachmentQueryFilter::Any;
    bool unread_only = false;
    bool starred_only = false;
    bool server_search = false;
    Text received_after;
    Text received_before;
};

class QueryFilterDialog : public Fl_Double_Window {
    Fl_Box *summary_ = nullptr;
    Fl_Choice *search_field_ = nullptr;
    Fl_Choice *scope_ = nullptr;
    Fl_Choice *sort_ = nullptr;
    Fl_Choice *attachments_ = nullptr;
    Fl_Check_Button *unread_ = nullptr;
    Fl_Check_Button *starred_ = nullptr;
    Fl_Check_Button *server_ = nullptr;
    Fl_Input *after_ = nullptr;
    Fl_Input *before_ = nullptr;
    bool accepted_ = false;
    static void accept_cb(Fl_Widget *, void *);
    static void cancel_cb(Fl_Widget *, void *);
    static void selection_cb(Fl_Widget *, void *);
    void refresh_summary();

public:
    explicit QueryFilterDialog(const QueryFilterSettings &settings);
    bool run(QueryFilterSettings &settings);
};

}

#endif
