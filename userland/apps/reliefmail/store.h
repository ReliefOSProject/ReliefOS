#ifndef RELIEFMAIL_STORE_H
#define RELIEFMAIL_STORE_H

#include "model.h"

namespace rmail {

using Id = uint64_t;

class SecretBytes;

struct SendOperation {
    uint64_t id = 0;
    uint64_t account_id = 0;
    uint64_t account_generation = 0;
    uint64_t draft_id = 0;
    uint64_t draft_revision = 0;
    Text message_id;
    Text sender;
    Text recipients;
    Text spool_name;
    uint64_t spool_size = 0;
    SendStage stage = SendStage::Ready;
    uint32_t delivery_count = 0;
    Text archive_folder;
    uint64_t archive_review_generation = 0;
    uint64_t archive_folder_generation = 0;
    uint64_t archive_folder_uidvalidity = 0;
    Text last_diagnostic;
};

struct SendOperationSummary {
    uint64_t id = 0;
    uint64_t account_id = 0;
    uint64_t account_generation = 0;
    uint64_t draft_id = 0;
    uint64_t draft_revision = 0;
    Text message_id;
    Text sender;
    Text recipients;
    SendStage stage = SendStage::Ready;
    uint32_t delivery_count = 0;
    Text archive_folder;
    uint64_t archive_review_generation = 0;
    uint64_t archive_folder_generation = 0;
    uint64_t archive_folder_uidvalidity = 0;
    Text last_diagnostic;
};
constexpr size_t MaxSendOperationPageSize = 25;
using SendOperationSummaries = BoundedArray<SendOperationSummary, MaxSendOperationPageSize>;

struct SendOperationPage {
    uint64_t account_id = 0;
    uint64_t offset = 0;
    bool has_more = false;
    SendOperationSummaries operations;
};

enum class UncertainSendDecision : uint8_t { ConfirmDelivered = 1, ConfirmNotDelivered = 2 };

enum class RestoreResult : uint8_t { Failed, Restored, RestoredCleanupPending, RecoveryRequired };

struct ProfilePaths {
    Text config_dir;
    Text data_dir;
    Text cache_dir;
};

/* Resolve HOME/XDG paths without creating or mutating the selected profile. */
bool profile_paths(ProfilePaths &paths, Text &error);
bool profile_paths_from_env(const char *home, const char *xdg_config_home,
    const char *xdg_data_home, const char *xdg_cache_home,
    ProfilePaths &paths, Text &error);

enum class StoreOpenMode : uint8_t { InstanceOwner, Worker, CoordinatedOwner };

class Store {
    void *database_ = nullptr;
    int config_fd_ = -1;
    int data_fd_ = -1;
    int cache_fd_ = -1;
    int lock_fd_ = -1;
    StoreOpenMode mode_ = StoreOpenMode::Worker;
    Text config_dir_path_;
    Text data_dir_path_;
    Text cache_dir_path_;

    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;
    bool restore_profile_impl(const char *source_path, SecretBytes backup_master_password,
        Text &error, bool &cleanup_pending, bool &recovery_required);

public:
    Store() = default;
    Store(Store &&other) noexcept;
    Store &operator=(Store &&other) noexcept;
    ~Store();

    bool open(const ProfilePaths &paths, StoreOpenMode mode, Text &error);
    void close();
    bool is_open() const { return database_ != nullptr; }

    bool save_account(Account &account, Text &error);
    bool save_account_guarded(Account &account, uint64_t expected_generation, Text &error);
    bool get_account(Id id, Account &account, Text &error);
    bool list_accounts(AccountList &accounts, Text &error);
    bool delete_account(Id id, Text &error);
    bool delete_account_guarded(Id id, uint64_t expected_generation, Text &error);

    bool save_folder(const Folder &folder, Text &error);
    bool save_folder_metadata(const Folder &folder, Text &error);
    bool save_folder_metadata_guarded(const Folder &folder, uint64_t account_generation,
        uint64_t expected_folder_generation, Text &error);
    /* Call after SELECT: atomically persist current UIDVALIDITY and purge stale mappings, even for an empty mailbox. */
    bool update_folder_uidvalidity(const FolderKey &key, uint64_t uidvalidity, Text &error);
    bool update_folder_uidvalidity_guarded(const FolderKey &key, uint64_t uidvalidity,
        uint64_t account_generation, uint64_t folder_generation,
        uint64_t expected_current_uidvalidity, Text &error);
    bool get_folder(const FolderKey &key, Folder &folder, Text &error);
    bool list_folders(Id account_id, FolderList &folders, Text &error);
    bool delete_folder(const FolderKey &key, Text &error);
    bool delete_folder_guarded(const FolderKey &key, uint64_t account_generation,
        uint64_t folder_generation, Text &error);

    bool save_message(const MessageSummary &summary, const Bytes &raw_message, Text &error);
    bool save_message_guarded(const MessageSummary &summary, const Bytes &raw_message,
        uint64_t account_generation, uint64_t folder_generation, Text &error);
    bool save_message_guarded(const MessageSummary &summary, const Bytes &raw_message,
        uint64_t account_generation, uint64_t folder_generation,
        AttachmentPresence attachment_presence, Text &error);
    bool set_message_attachment_presence_guarded(const MessageKey &key,
        AttachmentPresence attachment_presence, uint64_t account_generation,
        uint64_t folder_generation, Text &error);
    bool save_header_message_guarded(const MessageSummary &summary, const Bytes &header_bytes,
        uint64_t account_generation, uint64_t folder_generation, Text &error);
    bool load_message(const MessageKey &key, MessageSummary &summary, Bytes &raw_message, Text &error);
    bool list_messages(const FolderKey &folder, size_t offset, MessagePage &messages, Text &error);
    bool search_messages(const FolderKey &folder, const char *query, size_t offset,
        MessagePage &messages, Text &error);
    bool account_message_count(Id account_id, size_t &count, Text &error);
    bool query_message_page(const MessageQuerySpec &query, MessageQueryPage &page, Text &error);
    size_t message_count(const FolderKey &folder, Text &error);
    bool delete_message(const MessageKey &key, Text &error);
    bool delete_message_guarded(const MessageKey &key, uint64_t account_generation,
        uint64_t folder_generation, Text &error);
    bool update_message_flags_guarded(const MessageKey &key, uint32_t add_flags,
        uint32_t remove_flags, uint64_t account_generation, uint64_t folder_generation, Text &error);

    /* Saves incomplete raw recipient input and materializes source attachments before commit. */
    bool save_draft(Draft &draft, Text &error);
    bool save_draft_guarded(Draft &draft, uint64_t account_generation,
        uint64_t expected_revision, Text &error);
    bool get_draft(Id id, Draft &draft, Text &error);
    bool list_drafts(Id account_id, DraftList &drafts, Text &error);
    bool delete_draft(Id id, Text &error);
    bool delete_draft_guarded(Id id, uint64_t expected_account_id,
        uint64_t account_generation, uint64_t expected_revision, Text &error);
    void release_draft_sources(Draft &draft);
    bool set_send_stage(Id id, SendStage stage, Text &error);
    bool recover_interrupted_sends(Text &error);
    bool create_send_operation(uint64_t account_id, uint64_t account_generation,
        uint64_t draft_id, uint64_t draft_revision, const Text &message_id,
        const Text &sender, const Text &recipients, const Text &archive_folder,
        const Bytes &spool, SendOperation &operation, Text &error);
    bool find_send_operation_for_draft(uint64_t account_id, uint64_t draft_id,
        uint64_t draft_revision, bool &found, SendOperation &operation, Bytes &spool, Text &error);
    bool get_send_operation(Id id, SendOperation &operation, Bytes &spool, Text &error);
    bool get_send_operation_summary(Id id, SendOperationSummary &operation, Text &error);
    bool list_send_operations(Id account_id, uint64_t offset, SendOperationPage &page, Text &error);
    bool review_send_archive_target(Id operation_id, uint64_t account_id,
        uint64_t expected_operation_generation, uint64_t current_account_generation,
        const FolderKey &target_folder, uint64_t target_folder_generation,
        uint64_t target_folder_uidvalidity, Text &error);
    bool resolve_uncertain_send(Id operation_id, uint64_t account_id,
        uint64_t expected_operation_generation, uint64_t current_account_generation,
        UncertainSendDecision decision, Text &error);
    bool set_send_operation_diagnostic(Id operation_id, uint64_t account_generation,
        const Text &diagnostic, Text &error);
    bool advance_send_operation(Id id, uint64_t account_generation, SendStage stage,
        bool count_delivery, Text &error);

    bool save_contact(Contact &contact, Text &error);
    bool get_contact(Id id, Contact &contact, Text &error);
    bool list_contacts(ContactList &contacts, Text &error);
    bool delete_contact(Id id, Text &error);

    bool save_signature(Signature &signature, Text &error);
    bool get_signature(Id id, Signature &signature, Text &error);
    bool list_signatures(SignatureList &signatures, Text &error);
    bool delete_signature(Id id, Text &error);

    bool save_template(Template &message_template, Text &error);
    bool get_template(Id id, Template &message_template, Text &error);
    bool list_templates(TemplateList &templates, Text &error);
    bool delete_template(Id id, Text &error);

    bool save_rule(Rule &rule, Text &error);
    bool get_rule(Id id, Rule &rule, Text &error);
    bool list_rules(RuleList &rules, Text &error);
    bool delete_rule(Id id, Text &error);

    bool clear_cache(Text &error);
    /* Export is a SQLite package with a fixed manifest and fixed cache/vault components. */
    bool backup_profile(const char *destination_path, Text &error);
    /* Vault-bearing packages require the backup master for authentication before replacement. */
    bool restore_profile(const char *source_path, Text &error);
    bool restore_profile(const char *source_path, SecretBytes backup_master_password, Text &error);
    RestoreResult restore_profile_with_status(const char *source_path,
        SecretBytes backup_master_password, Text &error);
};

#ifdef RELIEFMAIL_TESTING
enum class StoreFaultPoint : uint8_t { None, CacheWrite, CacheRename, DatabaseCommit };
enum class StoreRestoreFault : uint32_t {
    None = 0,
    CrashAfterVaultInstall = 1u << 0,
    CrashAfterDatabaseInstall = 1u << 1,
    CrashAfterCommitPersisted = 1u << 2,
    CrashDuringCommittedCleanup = 1u << 3,
    CrashDuringRollbackCleanup = 1u << 4,
    FailDatabaseInstall = 1u << 5,
    FailRollbackSync = 1u << 6
};
void store_test_fail_next(StoreFaultPoint point);
void store_test_set_backup_hook(void (*hook)(void *), void *context);
void store_test_set_restore_faults(uint32_t mask);
#endif

}

#endif
