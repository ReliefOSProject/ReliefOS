#ifndef RELIEFMAIL_TASKS_H
#define RELIEFMAIL_TASKS_H

#include "worker.h"
#include "transport.h"

namespace rmail {

using WorkerSearchField = MessageQueryField;
enum class WorkerFolderAction : uint8_t { List, Subscribe, Unsubscribe, Create, Rename, Delete, ClearTrash };
enum class WorkerSendReviewAction : uint8_t { None, SetArchiveTarget, ConfirmDelivered, ConfirmNotDelivered };
enum class WorkerAttachmentRefKind : uint8_t { SelectedFile, ExistingDraftPart };
using WorkerPageQuery = MessageQuerySpec;
using WorkerPageRow = MessageQueryRow;
using WorkerPageRows = MessageQueryRows;
using WorkerMessagePage = MessageQueryPage;

struct WorkerAttachmentMetadata {
    Text filename;
    Text content_type;
    Text content_id;
    uint64_t size = 0;
    bool inline_part = false;
};
using WorkerAttachmentMetadataList = BoundedArray<WorkerAttachmentMetadata, MaxMimeParts>;

constexpr size_t MaxReaderSafeLinks = 64;
constexpr size_t MaxReaderInlineImages = 4;
constexpr size_t MaxReaderPlainPreviewBytes = 256u * 1024u;
constexpr size_t MaxReaderInlinePngBytes = 256u * 1024u;
constexpr size_t MaxReaderInlineImagePixels = 16u * 1024u;
constexpr uint32_t MaxReaderInlineImageDimension = 128;
constexpr size_t MaxReaderXpmBytes = 512u * 1024u;
constexpr size_t MaxReaderXpmTotalBytes = 1u * 1024u * 1024u;

struct WorkerReaderInlineImage {
    uint16_t width = 0;
    uint16_t height = 0;
    Text xpm;
};
using WorkerReaderInlineImages = BoundedArray<WorkerReaderInlineImage, MaxReaderInlineImages>;
using WorkerReaderSafeLinks = BoundedArray<Text, MaxReaderSafeLinks>;

/* FetchMessage returns this DTO after saving the exact full IMAP literal to the
 * guarded local cache. plain_text is a UTF-8 display preview only and is never
 * a complete-body editing/export source; consumers must honor
 * plain_text_truncated. It never includes raw EML or decoded attachment bytes;
 * export the original/parts through ReadMessageChunk in bounded slices. */
struct WorkerReaderResult {
    MessageKey key;
    bool headers_only = false;
    Text from;
    Text reply_to;
    Text to;
    Text cc;
    Text bcc;
    Text subject;
    Text date;
    Text message_id;
    Text in_reply_to;
    Text references;
    Text warning;
    Text plain_text;
    bool plain_text_truncated = false;
    Text html_text;
    bool html_warning = false;
    WorkerReaderSafeLinks safe_links;
    WorkerReaderInlineImages inline_images;
    WorkerAttachmentMetadataList attachments;
};

struct WorkerDraftAttachmentRef {
    WorkerAttachmentRefKind kind = WorkerAttachmentRefKind::SelectedFile;
    Text path;                 // SelectedFile only; opened by the worker with O_NOFOLLOW.
    uint64_t source_draft_id = 0;
    uint64_t source_revision = 0;
    uint32_t source_position = 0;
    Text filename;
    Text content_type;
    Text content_id;
    bool inline_part = false;
};
using WorkerDraftAttachmentRefs = BoundedArray<WorkerDraftAttachmentRef, MaxMimeParts>;

/* Draft fields and attachment metadata can cross IPC; source callbacks and
 * materialized bytes remain process-local. Existing parts are referenced by
 * draft revision and position, selected files by path and worker-opened. */
struct WorkerDraftSnapshot {
    Draft draft;
    uint64_t account_generation = 0;
    WorkerAttachmentMetadataList attachments;
};

struct WorkerDraftSummary {
    uint64_t id = 0;
    uint64_t revision = 0;
    Text to;
    Text subject;
    Text body_preview;
};
using WorkerDraftSummaries = BoundedArray<WorkerDraftSummary, MaxRowsPerPage>;
struct WorkerDraftList {
    uint64_t account_generation = 0;
    WorkerDraftSummaries drafts;
};

constexpr size_t MaxComposeContactSuggestions = 32;
constexpr size_t MaxComposeTemplateChoices = 64;

struct WorkerComposeTemplateChoice {
    uint64_t id = 0;
    Text name;
    Text subject;
};
using WorkerComposeTemplateChoices = BoundedArray<WorkerComposeTemplateChoice, MaxComposeTemplateChoices>;

struct WorkerComposeContactSuggestion {
    Text display_name;
    Text address;
};
using WorkerComposeContactSuggestions = BoundedArray<WorkerComposeContactSuggestion, MaxComposeContactSuggestions>;

/* Read-only, account-generation-bound data for compose UI. Template bodies are
 * fetched by ID on selection; contact results contain only chosen-address
 * material, never contact notes or stored credentials. */
struct WorkerComposeCatalog {
    uint64_t account_generation = 0;
    uint64_t signature_id = 0;
    bool has_signature = false;
    Text signature_body;
    WorkerComposeTemplateChoices templates;
    bool templates_truncated = false;
    WorkerComposeContactSuggestions contacts;
    bool contacts_truncated = false;
    uint64_t selected_template_id = 0;
    Text selected_template_subject;
    Text selected_template_body;
};

struct WorkerStartupSnapshot {
    AccountList accounts;
    uint64_t selected_account_id = 0;
    FolderList folders;
};

struct WorkerVaultStatus {
    bool exists = false;
};

struct WorkerAccountResult {
    Account account;
    bool deleted = false;
};

struct WorkerChunkResult {
    MessageKey key;
    uint32_t attachment_index = UINT32_MAX;
    bool headers_only = false;
    uint64_t offset = 0;
    uint64_t total_size = 0;
    Bytes bytes;
};

struct WorkerOperationResult {
    uint64_t operation_id = 0;
    SendStage stage = SendStage::Draft;
    uint32_t delivery_count = 0;
    Text message_id;
    Text diagnostic;
};

using WorkerSendOperationPage = SendOperationPage;

struct WorkerTaskInput {
    uint64_t item_id = 0;
    uint64_t item_revision = 0;
    uint64_t page_offset = 0;
    uint32_t flags_add = 0;
    uint32_t flags_remove = 0;
    uint8_t search_mode = 0;
    WorkerSearchField search_field = WorkerSearchField::All;
    WorkerFolderAction folder_action = WorkerFolderAction::List;
    WorkerSendReviewAction send_review_action = WorkerSendReviewAction::None;
    bool review_confirmed = false;
    Account account;
    WorkerPageQuery page_query;
    MessageKey message_key;
    uint64_t chunk_offset = 0;
    uint32_t chunk_size = 0;
    uint32_t attachment_index = UINT32_MAX; // UINT32_MAX means original message.
    FolderKey destination_folder;
    uint64_t destination_folder_generation = 0;
    uint64_t destination_folder_uidvalidity = 0;
    Text query;
    Text path;
    Draft draft;
    WorkerDraftAttachmentRefs attachment_refs;
    bool has_draft = false;
#ifdef RELIEFMAIL_TESTING
    bool test_pause_after_durable_commit = false;
    uint8_t test_vault_fault = 0;
    Text test_commit_marker_path;
#endif
};

/* Task values are versioned, bounded and exact-length. No undocumented byte
 * offsets are exposed to UI consumers. */
bool worker_encode_task_input(WorkerTaskKind kind, const WorkerTaskInput &input,
    Bytes &payload, Text &error);
bool worker_decode_task_input(WorkerTaskKind kind, const unsigned char *payload,
    size_t size, WorkerTaskInput &input, Text &error);
bool worker_encode_draft(const Draft &draft, Bytes &payload, Text &error);
bool worker_decode_draft(const unsigned char *payload, size_t size, Draft &draft, Text &error);
bool worker_encode_draft_snapshot(const WorkerDraftSnapshot &draft, Bytes &payload, Text &error);
bool worker_decode_draft_snapshot(const unsigned char *payload, size_t size,
    WorkerDraftSnapshot &draft, Text &error);
bool worker_encode_draft_list(const WorkerDraftList &drafts, Bytes &payload, Text &error);
bool worker_decode_draft_list(const unsigned char *payload, size_t size,
    WorkerDraftList &drafts, Text &error);
bool worker_encode_compose_catalog(const WorkerComposeCatalog &catalog, Bytes &payload, Text &error);
bool worker_decode_compose_catalog(const unsigned char *payload, size_t size,
    WorkerComposeCatalog &catalog, Text &error);
bool worker_encode_startup_snapshot(const WorkerStartupSnapshot &snapshot, Bytes &payload, Text &error);
bool worker_decode_startup_snapshot(const unsigned char *payload, size_t size,
    WorkerStartupSnapshot &snapshot, Text &error);
bool worker_encode_vault_status(const WorkerVaultStatus &status, Bytes &payload, Text &error);
bool worker_decode_vault_status(const unsigned char *payload, size_t size,
    WorkerVaultStatus &status, Text &error);
bool worker_encode_account_result(const WorkerAccountResult &account, Bytes &payload, Text &error);
bool worker_decode_account_result(const unsigned char *payload, size_t size,
    WorkerAccountResult &account, Text &error);
bool worker_encode_chunk_result(const WorkerChunkResult &chunk, Bytes &payload, Text &error);
bool worker_decode_chunk_result(const unsigned char *payload, size_t size,
    WorkerChunkResult &chunk, Text &error);
bool worker_encode_operation_result(const WorkerOperationResult &operation, Bytes &payload, Text &error);
bool worker_decode_operation_result(const unsigned char *payload, size_t size,
    WorkerOperationResult &operation, Text &error);
bool worker_encode_send_operation_page(const WorkerSendOperationPage &page, Bytes &payload, Text &error);
bool worker_decode_send_operation_page(const unsigned char *payload, size_t size,
    WorkerSendOperationPage &page, Text &error);
bool worker_encode_message_page(const WorkerMessagePage &page, Bytes &payload, Text &error);
bool worker_decode_message_page(const unsigned char *payload, size_t size,
    WorkerMessagePage &page, Text &error);
bool worker_encode_reader_result(const WorkerReaderResult &reader, Bytes &payload, Text &error);
bool worker_decode_reader_result(const unsigned char *payload, size_t size,
    WorkerReaderResult &reader, Text &error);
bool worker_clone_reader_result(const WorkerReaderResult &source, WorkerReaderResult &destination);

/* These encoders produce live RMCL packets bound to a required nonzero
 * coordinator profile epoch. They are valid only in the matching session.
 * Vault unlock is the only supported path that rebinds remembered records. */
enum class CredentialPurpose : uint8_t { Imap = 1, Smtp = 2, Shared = 3 };
struct CredentialInput {
    Account configuration;
    CredentialPurpose purpose = CredentialPurpose::Imap;
    Text username;
    const unsigned char *secret = nullptr;
    size_t secret_size = 0;
};
bool worker_encode_credentials(const CredentialInput *records, size_t count,
    uint64_t current_profile_epoch, SecretBytes &packet, Text &error);
bool worker_encode_account_credential(const Account &configuration, CredentialPurpose purpose,
    uint64_t current_profile_epoch, SecretBytes password, SecretBytes &packet, Text &error);
bool worker_find_credential(const SecretBytes &packet, const Account &configuration,
    CredentialPurpose purpose, const Text &username, uint64_t current_profile_epoch,
    SecretBytes &secret, Text &error);

enum class WorkerSecretPurpose : uint8_t { ImapPassword, SmtpPassword, VaultMaster,
    VaultSave, VaultPasswordChange, BackupMaster };
/* Live account packets and vault/master secrets remain wipeable and travel
 * only on the dedicated anonymous secret pipe, never WorkerRequest.payload. */
bool worker_encode_secret(WorkerSecretPurpose purpose, SecretBytes first,
    SecretBytes second, SecretBytes &packet, Text &error);
bool worker_decode_secret(SecretBytes packet, WorkerSecretPurpose expected,
    SecretBytes &first, SecretBytes &second, Text &error);

/* Executes exactly one fixed task kind in the child process. */
bool worker_execute_task(const WorkerRequest &request, SecretBytes secret,
    const OperationContext &context, WorkerResult &result);

}

#endif
