#ifndef RELIEFMAIL_IMAP_H
#define RELIEFMAIL_IMAP_H

#include "transport.h"

namespace rmail {

constexpr size_t MaxImapUidBatch = MaxRowsPerPage;
constexpr size_t MaxImapFetchResponseBytes = MaxRawMessageBytes;

enum class ImapSearchMode : uint8_t { All, Unseen, Flagged };
enum class ImapTextSearchField : uint8_t { From, Subject, Body };
enum class ImapFolderOperation : uint8_t { Subscribe, Unsubscribe, Create, Rename, Delete };
enum class ImapAppendOutcome : uint8_t { Accepted, Rejected, Uncertain };

enum ImapSystemFlag : uint32_t {
    ImapFlagSeen = 1u << 0,
    ImapFlagAnswered = 1u << 1,
    ImapFlagFlagged = 1u << 2,
    ImapFlagDeleted = 1u << 3,
    ImapFlagDraft = 1u << 4
};

struct ImapMessage {
    MessageKey key;
    Bytes raw;
    uint32_t flags = 0;
    int64_t internal_date = 0; // Unix seconds, including dates before 1970.
    uint64_t size = 0;
    bool headers_only = false;
};

struct ImapCapabilities {
    bool starttls = false;
    bool auth_plain = false;
    bool auth_login = false;
    bool id = false;
    bool move = false;
    bool uidplus = false;
};

/* FolderKey::server_name is the exact IMAP wire name (modified UTF-7 for
 * IMAP4rev1). Folder::display_name is UTF-8. LIST has no UIDVALIDITY and must
 * return zero/unknown; only SELECT establishes a nonzero value. Consumers
 * refreshing from LIST must preserve any previously known validity. */
bool encode_modified_utf7(const char *utf8, size_t length, Text &wire_name, Text &error);

class ImapSession {
    Stream stream_;
    Account account_;
    void *protocol_state_ = nullptr;
    bool connected_ = false;
    bool authenticated_ = false;

public:
    ImapSession() = default;
    ~ImapSession();
    ImapSession(const ImapSession &) = delete;
    ImapSession &operator=(const ImapSession &) = delete;

    bool connect(const Account &account, const OperationContext &context, Text &error);
    bool login(SecretView password, const OperationContext &context, Text &error);
    void close();

    bool list(FolderList &folders, const OperationContext &context, Text &error);
    bool select(const FolderKey &folder, Folder &selected,
        const OperationContext &context, Text &error);
    bool search(const FolderKey &folder, ImapSearchMode mode, Array<MessageKey> &keys,
        const OperationContext &context, Text &error);
    bool search_message_id(const FolderKey &folder, const Text &message_id,
        Array<MessageKey> &keys, const OperationContext &context, Text &error);
    bool search_text(const FolderKey &folder, ImapTextSearchField field, const Text &text,
        Array<MessageKey> &keys, const OperationContext &context, Text &error);
    /* Returned literal payload across this call is capped at
     * MaxImapFetchResponseBytes; an oversized server batch fails closed. */
    bool fetch(const MessageKey *keys, size_t key_count, Array<ImapMessage> &messages,
        const OperationContext &context, Text &error);
    /* Header bytes are a bounded header section only and are marked as such;
     * total returned literal bytes are capped by MaxImapFetchResponseBytes. */
    bool fetch_headers(const MessageKey *keys, size_t key_count, Array<ImapMessage> &messages,
        const OperationContext &context, Text &error);
    bool store(const MessageKey &key, uint32_t add_flags, uint32_t remove_flags,
        const OperationContext &context, Text &error);
    bool delete_message(const MessageKey &key, const OperationContext &context, Text &error);
    bool copy(const MessageKey &key, const FolderKey &destination,
        const OperationContext &context, Text &error);
    bool move(const MessageKey &key, const FolderKey &destination,
        const OperationContext &context, Text &error);
    ImapAppendOutcome append(const FolderKey &folder, const MessageSource &message,
        const OperationContext &context, Text &error);
    bool folder_operation(ImapFolderOperation operation, const FolderKey &folder,
        const Text &new_display_name, const OperationContext &context, Text &error);
    bool capabilities(ImapCapabilities &snapshot) const;
    bool diagnostics(TransportDiagnostics &snapshot) const;
};

}

#endif
