#ifndef RELIEFMAIL_MODEL_H
#define RELIEFMAIL_MODEL_H

#include "storage.h"

namespace rmail {

/*
 * Mail model values are process-local. GUI/worker communication must transfer
 * serialized bytes and reconstruct model values at the receiving endpoint.
 */

constexpr size_t MaxAccounts = 16;
constexpr size_t MaxFoldersPerAccount = 4096;
constexpr size_t MaxIndexedMessagesPerFolder = 100000;
constexpr size_t MaxRowsPerPage = 200;
constexpr size_t MaxRawMessageBytes = 64u * 1024u * 1024u;
constexpr size_t MaxEditableTextBytes = 1u * 1024u * 1024u;
constexpr size_t MaxEditorStateBytes = 16u * 1024u * 1024u;
constexpr size_t MaxMimeDepth = 20;
constexpr size_t MaxMimeParts = 512;
constexpr size_t MaxHeaderBytes = 256u * 1024u;
constexpr size_t DefaultMaxAttachmentBytes = 25u * 1024u * 1024u;
constexpr size_t MaxContactAddresses = 4096;
constexpr size_t MaxStoredCatalogEntries = 100000;

enum class ResultCode : uint8_t { Ok, InvalidInput, Unsupported, LimitExceeded, OutOfMemory };
enum class ErrorCode : uint8_t {
    None,
    InvalidInput,
    InvalidUtf8,
    UnsupportedInternationalizedAddress,
    EmptyRecipient,
    HeaderInjection,
    InvalidPort,
    InvalidAccount,
    InvalidMailto,
    LimitExceeded,
    OutOfMemory
};

struct Error {
    ErrorCode code = ErrorCode::None;
    Text message;
};

struct Result {
    ResultCode code = ResultCode::Ok;
    Error error;
    bool ok() const { return code == ResultCode::Ok; }
};

enum class IncomingProtocol : uint8_t { Imap, Pop3 };
enum class TransportSecurity : uint8_t { ImplicitTls, StartTls };
enum class AuthenticationMethod : uint8_t { Password, ApplicationPassword };

struct Account {
    uint64_t id = 0;
    /* Durable configuration identity. Incremented on every committed edit. */
    uint64_t generation = 0;
    Text display_name;
    Text email_address;
    Text username;
    Text smtp_username;
    Text incoming_host;
    Text outgoing_host;
    uint32_t incoming_port = 993;
    uint32_t outgoing_port = 587;
    IncomingProtocol incoming_protocol = IncomingProtocol::Imap;
    TransportSecurity incoming_security = TransportSecurity::ImplicitTls;
    TransportSecurity outgoing_security = TransportSecurity::StartTls;
    AuthenticationMethod authentication = AuthenticationMethod::Password;
    bool enabled = true;
    bool is_default = false;
    uint32_t connect_timeout_ms = 15000;
    uint32_t io_timeout_ms = 30000;
    uint32_t poll_interval_minutes = 0;
    uint32_t cache_days = 30;
    Text custom_ca_file;
    Text inbox_folder;
    Text sent_folder;
    Text drafts_folder;
    Text trash_folder;
    Text junk_folder;
    Text archive_folder;
    uint64_t signature_id = 0;
};

using AccountList = BoundedArray<Account, MaxAccounts>;

struct FolderKey {
    uint64_t account_id = 0;
    Text server_name;
    bool operator==(const FolderKey &other) const {
        return account_id == other.account_id && server_name == other.server_name;
    }
    bool operator!=(const FolderKey &other) const { return !(*this == other); }
};

struct MessageKey {
    FolderKey folder;
    uint64_t uidvalidity = 0;
    uint32_t uid = 0;
    bool operator==(const MessageKey &other) const {
        return folder == other.folder && uidvalidity == other.uidvalidity && uid == other.uid;
    }
    bool operator!=(const MessageKey &other) const { return !(*this == other); }
};

struct Address {
    Text display_name;
    Text local_part;
    Text domain;
    bool display_name_quoted = false;
    bool local_part_quoted = false;
};

enum class FolderRole : uint8_t { None, Inbox, Sent, Drafts, Trash, Junk, Archive };

struct Folder {
    FolderKey key;
    Text display_name;
    FolderRole role = FolderRole::None;
    uint64_t uidvalidity = 0;
    uint64_t generation = 0;
    uint32_t message_count = 0;
    uint32_t unread_count = 0;
    bool selectable = true;
};

using FolderList = BoundedArray<Folder, MaxFoldersPerAccount>;

struct MessageSummary {
    MessageKey key;
    Text from;
    Text subject;
    int64_t internal_date = 0;
    uint64_t size = 0;
    uint32_t flags = 0;
    bool headers_only = false;
};

using MessageIndex = BoundedArray<MessageSummary, MaxIndexedMessagesPerFolder>;
using MessagePage = BoundedArray<MessageSummary, MaxRowsPerPage>;

enum class MessageQueryScope : uint8_t { Folder, Account, Unified, AllMail };
enum class MessageQuerySort : uint8_t { Newest, Oldest, Sender, Subject };
enum class MessageQueryField : uint8_t { All, From, Subject, Body, MessageId };
enum class AttachmentPresence : uint8_t { Unknown, None, Some };
enum class AttachmentQueryFilter : uint8_t { Any, HasAttachments, NoAttachments, Unknown };

struct MessageQuerySpec {
    MessageQueryScope scope = MessageQueryScope::Folder;
    MessageQuerySort sort = MessageQuerySort::Newest;
    MessageQueryField search_field = MessageQueryField::All;
    uint64_t account_id = 0;
    FolderKey folder;
    uint64_t offset = 0;
    uint32_t limit = MaxRowsPerPage;
    uint32_t required_flags = 0;
    uint32_t excluded_flags = 0;
    /* Inclusive bounds on server INTERNALDATE, in signed Unix seconds. */
    bool has_received_after = false;
    int64_t received_after = 0;
    bool has_received_before = false;
    int64_t received_before = 0;
    AttachmentQueryFilter attachment_filter = AttachmentQueryFilter::Any;
    Text query;
};

struct MessageQueryRow {
    MessageSummary message;
    Text folder_display_name;
    uint64_t profile_epoch = 0;
    uint64_t ui_generation = 0;
    uint64_t account_generation = 0;
    uint64_t folder_generation = 0;
    bool attachment_presence_known = false;
    bool has_attachments = false;
};
using MessageQueryRows = BoundedArray<MessageQueryRow, MaxRowsPerPage>;

struct MessageQueryPage {
    MessageQuerySpec query;
    /* Folder-scoped results carry the durable identity observed while being
     * produced, including the empty-page case. Other scopes leave both 0. */
    uint64_t folder_generation = 0;
    uint64_t folder_uidvalidity = 0;
    MessageQueryRows rows;
    uint64_t total_matches = 0;
    bool has_more = false;
    /* Coverage of every scope/flag-eligible indexed message before text matching
     * and pagination; these counts are not the page's match composition. */
    uint64_t downloaded_body_count = 0;
    uint64_t headers_only_count = 0;
    Text diagnostic;
};

using AttachmentReadCallback = bool (*)(void *context, size_t offset, unsigned char *buffer,
    size_t capacity, size_t *read_size);
using AttachmentSourceRetainCallback = void (*)(void *owner_context);
using AttachmentSourceReleaseCallback = void (*)(void *owner_context);

struct AttachmentSource {
    void *context = nullptr;
    AttachmentReadCallback read = nullptr;
    size_t size = 0;

private:
    void *owner_context_ = nullptr;
    AttachmentSourceRetainCallback retain_owner_ = nullptr;
    AttachmentSourceReleaseCallback release_owner_ = nullptr;

    void retain_owner() {
        if (owner_context_ && retain_owner_) retain_owner_(owner_context_);
    }

public:
    AttachmentSource() = default;
    AttachmentSource(const AttachmentSource &other)
        : context(other.context), read(other.read), size(other.size), owner_context_(other.owner_context_),
          retain_owner_(other.retain_owner_), release_owner_(other.release_owner_) { retain_owner(); }
    AttachmentSource(AttachmentSource &&other) noexcept
        : context(other.context), read(other.read), size(other.size), owner_context_(other.owner_context_),
          retain_owner_(other.retain_owner_), release_owner_(other.release_owner_) {
        other.context = nullptr;
        other.read = nullptr;
        other.size = 0;
        other.owner_context_ = nullptr;
        other.retain_owner_ = nullptr;
        other.release_owner_ = nullptr;
    }
    AttachmentSource &operator=(const AttachmentSource &other) {
        if (this == &other) return *this;
        AttachmentSource next(other);
        *this = static_cast<AttachmentSource &&>(next);
        return *this;
    }
    AttachmentSource &operator=(AttachmentSource &&other) noexcept {
        if (this == &other) return *this;
        reset();
        context = other.context;
        read = other.read;
        size = other.size;
        owner_context_ = other.owner_context_;
        retain_owner_ = other.retain_owner_;
        release_owner_ = other.release_owner_;
        other.context = nullptr;
        other.read = nullptr;
        other.size = 0;
        other.owner_context_ = nullptr;
        other.retain_owner_ = nullptr;
        other.release_owner_ = nullptr;
        return *this;
    }
    ~AttachmentSource() { reset(); }

    void set_owned(void *source_context, AttachmentReadCallback callback, size_t source_size,
        AttachmentSourceRetainCallback retain_callback,
        AttachmentSourceReleaseCallback release_callback) {
        reset();
        context = source_context;
        read = callback;
        size = source_size;
        owner_context_ = source_context;
        retain_owner_ = retain_callback;
        release_owner_ = release_callback;
    }
    bool is_owned() const { return owner_context_ && release_owner_; }
    void reset() {
        void *owner = owner_context_;
        AttachmentSourceReleaseCallback release = release_owner_;
        context = nullptr;
        read = nullptr;
        size = 0;
        owner_context_ = nullptr;
        retain_owner_ = nullptr;
        release_owner_ = nullptr;
        if (owner && release) release(owner);
    }
};

struct Attachment {
    Text filename;
    Text content_type;
    Text content_id;
    Bytes data;
    AttachmentSource source;
    bool inline_part = false;
};

using AttachmentList = BoundedArray<Attachment, MaxMimeParts>;

struct Contact {
    uint64_t id = 0;
    Text display_name;
    Text organization;
    Text notes;
    Array<Address> addresses;
};

struct Signature {
    uint64_t id = 0;
    Text name;
    Text body;
    bool enabled = true;
};

struct Template {
    uint64_t id = 0;
    Text name;
    Text subject;
    Text body;
};

enum class RuleField : uint8_t { From, To, Subject, Body };
enum class RuleOperator : uint8_t { Contains, Equals, StartsWith };
enum class RuleAction : uint8_t { MarkRead, Star, Move, Delete };

struct Rule {
    uint64_t id = 0;
    bool enabled = true;
    RuleField field = RuleField::From;
    RuleOperator operation = RuleOperator::Contains;
    Text value;
    RuleAction action = RuleAction::MarkRead;
    FolderKey destination;
};

using ContactList = Array<Contact>;
using SignatureList = Array<Signature>;
using TemplateList = Array<Template>;
using RuleList = Array<Rule>;

enum class SendStage : uint8_t {
    Draft,
    Ready,
    Connecting,
    Authenticating,
    SendingEnvelope,
    SendingData,
    AwaitingFinalResponse,
    Sent,
    Uncertain,
    Failed,
    Accepted,
    ArchivePending,
    ArchiveFailed,
    Archived,
    ReviewedDelivered
};

/* User-reviewed delivery may proceed through the IMAP-only archive stages
 * without fabricating an SMTP acceptance count. The exact archive target is
 * the durable evidence that this zero-count operation was explicitly reviewed. */
inline bool send_operation_delivery_state_valid(SendStage stage, uint32_t delivery_count,
    uint64_t archive_review_generation) {
    if (delivery_count > 1) return false;
    switch (stage) {
    case SendStage::Accepted:
    case SendStage::Sent:
    case SendStage::ArchivePending:
    case SendStage::ArchiveFailed:
    case SendStage::Archived:
        return delivery_count == 1 || (delivery_count == 0 && archive_review_generation != 0 &&
            (stage == SendStage::ArchivePending || stage == SendStage::ArchiveFailed ||
             stage == SendStage::Archived));
    case SendStage::Uncertain:
        return delivery_count == 0 && archive_review_generation == 0;
    case SendStage::ReviewedDelivered:
        return delivery_count == 0;
    default:
        return delivery_count == 0 && archive_review_generation == 0;
    }
}

struct Draft {
    uint64_t id = 0;
    uint64_t revision = 0;
    uint64_t account_id = 0;
    Array<Address> to;
    Array<Address> cc;
    Array<Address> bcc;
    Text to_input;
    Text cc_input;
    Text bcc_input;
    bool raw_recipient_input = false;
    Text subject;
    Text body;
    Text html_body;
    /* Opaque, versioned editor payload; storage preserves bytes, MIME ignores it. */
    Bytes editor_state;
    bool rich_format = false;
    Text message_id;
    Text in_reply_to;
    Text references;
    AttachmentList attachments;
    SendStage send_stage = SendStage::Draft;
};

using DraftList = BoundedArray<Draft, MaxRowsPerPage>;

bool valid_utf8(const char *bytes, size_t length);

/* The sized overloads reject embedded NUL bytes; C-string forms end at NUL. */
/* Parsers replace their output only after successful validation. */
bool parse_addresses(const char *input, Array<Address> &output, Text &error);
bool parse_addresses(const char *input, size_t length, Array<Address> &output, Text &error);
bool parse_addresses(const char *input, Array<Address> &output, Error &error);
bool parse_addresses(const char *input, size_t length, Array<Address> &output, Error &error);
bool serialize_address(const Address &address, Text &output, Text &error);
bool serialize_address(const Address &address, Text &output, Error &error);

bool parse_mailto(const char *uri, Draft &draft, Text &error);
bool parse_mailto(const char *uri, size_t length, Draft &draft, Text &error);
bool parse_mailto(const char *uri, Draft &draft, Error &error);
bool parse_mailto(const char *uri, size_t length, Draft &draft, Error &error);

bool validate_account(const Account &account, Text &error);
bool validate_account(const Account &account, Error &error);
bool validate_draft(const Draft &draft, Text &error);
bool validate_draft(const Draft &draft, Error &error);
/* Failure-aware process-local value copy; retained AttachmentSource owners are shared safely. */
bool clone_draft(const Draft &source, Draft &destination, Text &error);
bool set_raw_recipient_inputs(Draft &draft,
    const char *to, size_t to_length,
    const char *cc, size_t cc_length,
    const char *bcc, size_t bcc_length,
    Text &error);

}
#endif
