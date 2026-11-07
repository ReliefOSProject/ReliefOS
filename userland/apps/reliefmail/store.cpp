#include "store.h"
#include "profile_io.h"
#include "vault.h"
#include "mime.h"
#include "sqlite3.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef RELIEFMAIL_TESTING
#include <signal.h>
#endif

namespace rmail {
namespace {

constexpr int CurrentSchemaVersion = 6;
constexpr size_t MaxStoredText = MaxEditableTextBytes;

#ifdef RELIEFMAIL_TESTING
StoreFaultPoint next_store_fault = StoreFaultPoint::None;
void (*next_backup_hook)(void *) = nullptr;
void *next_backup_hook_context = nullptr;
uint32_t restore_fault_mask = 0;

bool take_restore_fault(StoreRestoreFault point) {
    const uint32_t bit = static_cast<uint32_t>(point);
    if (!(restore_fault_mask & bit)) return false;
    restore_fault_mask &= ~bit;
    return true;
}

void crash_restore_test_process(StoreRestoreFault point) {
    if (take_restore_fault(point)) raise(SIGKILL);
}

bool consume_store_fault(StoreFaultPoint point) {
    if (next_store_fault != point) return false;
    next_store_fault = StoreFaultPoint::None;
    return true;
}
#endif

bool set_error(Text &error, const char *message) {
    error.clear();
    error.set(message);
    return false;
}

bool restore_journal_exists(int config_fd, bool &exists, Text &error);
bool recover_restore_journal(int config_fd, int data_fd, int cache_fd, Text &error);

class RestoreGate {
    int fd_ = -1;
public:
    ~RestoreGate() { if (fd_ >= 0) ::close(fd_); }
    bool lock(int config_fd, bool exclusive, Text &error) {
        if (!detail::open_private_file_at(config_fd, ".restore.lock", O_RDWR | O_CREAT, fd_, error))
            return false;
        const int operation = (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB;
        if (flock(fd_, operation) < 0) return set_error(error, "profile restore is active; retry later");
        return true;
    }
};

bool sqlite_ok(int code, Text &error, const char *message) {
    if (code == SQLITE_OK || code == SQLITE_DONE || code == SQLITE_ROW) return true;
    return set_error(error, message);
}

bool exec_sql(sqlite3 *db, const char *sql, Text &error, const char *message) {
    char *detail = nullptr;
    const int code = sqlite3_exec(db, sql, nullptr, nullptr, &detail);
    if (detail) sqlite3_free(detail);
    if (code == SQLITE_OK) return true;
    return set_error(error, message);
}

struct Statement {
    sqlite3_stmt *value = nullptr;
    ~Statement() { if (value) sqlite3_finalize(value); }
};

bool prepare(sqlite3 *db, Statement &statement, const char *sql, Text &error) {
    return sqlite_ok(sqlite3_prepare_v2(db, sql, -1, &statement.value, nullptr),
        error, "cannot prepare profile query");
}

bool bind_text(sqlite3_stmt *statement, int index, const Text &value, Text &error) {
    if (value.size() > static_cast<size_t>(INT_MAX)) return set_error(error, "profile text is too large");
    return sqlite_ok(sqlite3_bind_text(statement, index, value.c_str(),
        static_cast<int>(value.size()), SQLITE_TRANSIENT), error, "cannot bind profile text");
}

bool bind_blob(sqlite3_stmt *statement, int index, const Bytes &value, Text &error) {
    if (value.size() > static_cast<size_t>(INT_MAX)) return set_error(error, "profile binary value is too large");
    const int code = value.empty() ? sqlite3_bind_zeroblob(statement, index, 0) :
        sqlite3_bind_blob(statement, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    return sqlite_ok(code, error, "cannot bind profile binary value");
}

bool bind_i64(sqlite3_stmt *statement, int index, uint64_t value, Text &error) {
    if (value > static_cast<uint64_t>(INT64_MAX)) return set_error(error, "profile integer is out of range");
    return sqlite_ok(sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(value)),
        error, "cannot bind profile integer");
}

bool bind_signed(sqlite3_stmt *statement, int index, int64_t value, Text &error) {
    return sqlite_ok(sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(value)),
        error, "cannot bind signed profile integer");
}

bool next_identity_generation(sqlite3 *db, uint64_t &generation, Text &error) {
    Statement query;
    if (!prepare(db, query, "SELECT next_value FROM identity_sequence WHERE id=1", error)) return false;
    const int code = sqlite3_step(query.value);
    sqlite3_int64 current = 0;
    if (code != SQLITE_ROW || sqlite3_column_type(query.value, 0) != SQLITE_INTEGER ||
        (current = sqlite3_column_int64(query.value, 0)) <= 0 || current >= INT64_MAX)
        return set_error(error, "profile identity generation is invalid or exhausted");
    Statement update;
    if (!prepare(db, update, "UPDATE identity_sequence SET next_value=? WHERE id=1", error) ||
        !bind_i64(update.value, 1, static_cast<uint64_t>(current + 1), error) ||
        sqlite3_step(update.value) != SQLITE_DONE || sqlite3_changes(db) != 1)
        return set_error(error, "cannot advance profile identity generation");
    generation = static_cast<uint64_t>(current);
    return true;
}

bool account_generation_matches(sqlite3 *db, uint64_t account_id, uint64_t expected, Text &error) {
    if (!expected) return true;
    Statement query;
    if (!prepare(db, query, "SELECT config_generation FROM accounts WHERE id=?", error) ||
        !bind_i64(query.value, 1, account_id, error)) return false;
    const int code = sqlite3_step(query.value);
    if (code != SQLITE_ROW || sqlite3_column_type(query.value, 0) != SQLITE_INTEGER ||
        sqlite3_column_int64(query.value, 0) <= 0 ||
        static_cast<uint64_t>(sqlite3_column_int64(query.value, 0)) != expected)
        return set_error(error, "account configuration generation changed before durable write");
    return true;
}

bool folder_generation_matches(sqlite3 *db, const FolderKey &key, uint64_t expected,
    uint64_t uidvalidity, Text &error) {
    if (!expected && !uidvalidity) return true;
    Statement query;
    if (!prepare(db, query, "SELECT generation,uidvalidity FROM folders WHERE account_id=? AND server_name=?", error) ||
        !bind_i64(query.value, 1, key.account_id, error) || !bind_text(query.value, 2, key.server_name, error)) return false;
    const int code = sqlite3_step(query.value);
    if (code != SQLITE_ROW) return set_error(error, "folder was removed before durable write");
    const sqlite3_int64 generation = sqlite3_column_int64(query.value, 0);
    const sqlite3_int64 current_uidvalidity = sqlite3_column_int64(query.value, 1);
    if (generation <= 0 || current_uidvalidity < 0 ||
        (expected && static_cast<uint64_t>(generation) != expected) ||
        (uidvalidity && static_cast<uint64_t>(current_uidvalidity) != uidvalidity))
        return set_error(error, "folder identity or UIDVALIDITY changed before durable write");
    return true;
}

bool send_transition_allowed(SendStage from, SendStage to) {
    switch (from) {
    case SendStage::Ready:
        return to == SendStage::Connecting || to == SendStage::Failed;
    case SendStage::Connecting:
        return to == SendStage::Authenticating || to == SendStage::Ready || to == SendStage::Failed;
    case SendStage::Authenticating:
        return to == SendStage::SendingEnvelope || to == SendStage::Ready || to == SendStage::Failed;
    case SendStage::SendingEnvelope:
        return to == SendStage::SendingData || to == SendStage::Ready || to == SendStage::Failed;
    case SendStage::SendingData:
        return to == SendStage::AwaitingFinalResponse || to == SendStage::Uncertain ||
            to == SendStage::Ready || to == SendStage::Failed;
    case SendStage::AwaitingFinalResponse:
        return to == SendStage::Accepted || to == SendStage::Uncertain || to == SendStage::Failed;
    case SendStage::Sent:
        return to == SendStage::ArchivePending;
    case SendStage::Accepted:
        return to == SendStage::ArchivePending;
    case SendStage::ArchivePending:
        return to == SendStage::Archived || to == SendStage::ArchiveFailed;
    case SendStage::ArchiveFailed:
        return to == SendStage::ArchivePending || to == SendStage::Archived || to == SendStage::ArchiveFailed;
    case SendStage::ReviewedDelivered:
        return to == SendStage::ArchivePending;
    case SendStage::Draft:
    case SendStage::Uncertain:
    case SendStage::Failed:
    case SendStage::Archived:
        return false;
    }
    return false;
}

bool bind_i32(sqlite3_stmt *statement, int index, uint32_t value, Text &error) {
    return sqlite_ok(sqlite3_bind_int(statement, index, static_cast<int>(value)),
        error, "cannot bind profile integer");
}

bool bind_bool(sqlite3_stmt *statement, int index, bool value, Text &error) {
    return sqlite_ok(sqlite3_bind_int(statement, index, value ? 1 : 0),
        error, "cannot bind profile boolean");
}

bool column_text(sqlite3_stmt *statement, int index, Text &output) {
    if (sqlite3_column_type(statement, index) != SQLITE_TEXT) return false;
    const unsigned char *value = sqlite3_column_text(statement, index);
    const int size = sqlite3_column_bytes(statement, index);
    if (!value || size < 0) return output.set("", 0);
    return output.set(reinterpret_cast<const char *>(value), static_cast<size_t>(size));
}

bool column_blob(sqlite3_stmt *statement, int index, Bytes &output) {
    if (sqlite3_column_type(statement, index) != SQLITE_BLOB) return false;
    const void *value = sqlite3_column_blob(statement, index);
    const int size = sqlite3_column_bytes(statement, index);
    if (size < 0 || (size && !value) || static_cast<size_t>(size) > MaxEditorStateBytes) return false;
    return output.set(value, static_cast<size_t>(size));
}

bool column_bool(sqlite3_stmt *statement, int index, bool &output) {
    if (sqlite3_column_type(statement, index) != SQLITE_INTEGER) return false;
    const sqlite3_int64 value = sqlite3_column_int64(statement, index);
    if (value != 0 && value != 1) return false;
    output = value != 0;
    return true;
}

bool column_i64(sqlite3_stmt *statement, int index, sqlite3_int64 &output) {
    if (sqlite3_column_type(statement, index) != SQLITE_INTEGER) return false;
    output = sqlite3_column_int64(statement, index);
    return true;
}

bool column_u64_checked(sqlite3_stmt *statement, int index, uint64_t &output) {
    sqlite3_int64 value = 0;
    if (!column_i64(statement, index, value) || value < 0) return false;
    output = static_cast<uint64_t>(value);
    return true;
}

bool column_u32_checked(sqlite3_stmt *statement, int index, uint32_t &output) {
    uint64_t value = 0;
    if (!column_u64_checked(statement, index, value) || value > UINT32_MAX) return false;
    output = static_cast<uint32_t>(value);
    return true;
}

uint64_t column_u64(sqlite3_stmt *statement, int index) {
    const sqlite3_int64 value = sqlite3_column_int64(statement, index);
    return value < 0 ? 0 : static_cast<uint64_t>(value);
}

bool transaction_begin(sqlite3 *db, Text &error) {
    return exec_sql(db, "BEGIN IMMEDIATE", error, "profile is busy; retry the operation");
}

bool transaction_commit(sqlite3 *db, Text &error) {
#ifdef RELIEFMAIL_TESTING
    if (consume_store_fault(StoreFaultPoint::DatabaseCommit)) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        return set_error(error, "injected profile commit failure");
    }
#endif
    if (exec_sql(db, "COMMIT", error, "profile transaction could not commit")) return true;
    sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    return false;
}

void transaction_rollback(sqlite3 *db) {
    sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
}

bool valid_text(const Text &value, size_t maximum = MaxStoredText) {
    if (value.size() > maximum || !valid_utf8(value.c_str(), value.size())) return false;
    for (size_t i = 0; i < value.size(); ++i)
        if (!value.c_str()[i]) return false;
    return true;
}

bool text_contains_ascii_case_insensitive(const Text &value, const Text &needle) {
    if (needle.empty()) return true;
    if (needle.size() > value.size()) return false;
    for (size_t start = 0; start + needle.size() <= value.size(); ++start) {
        size_t i = 0;
        for (; i < needle.size(); ++i) {
            unsigned char left = static_cast<unsigned char>(value.c_str()[start + i]);
            unsigned char right = static_cast<unsigned char>(needle.c_str()[i]);
            if (left >= 'A' && left <= 'Z') left = static_cast<unsigned char>(left + ('a' - 'A'));
            if (right >= 'A' && right <= 'Z') right = static_cast<unsigned char>(right + ('a' - 'A'));
            if (left != right) break;
        }
        if (i == needle.size()) return true;
    }
    return false;
}

bool valid_folder_key(const FolderKey &key) {
    return key.account_id && !key.server_name.empty() && valid_text(key.server_name, 4096);
}

bool bind_folder_key(sqlite3_stmt *statement, int first, const FolderKey &key, Text &error) {
    return bind_i64(statement, first, key.account_id, error) &&
        bind_text(statement, first + 1, key.server_name, error);
}

bool bind_message_key(sqlite3_stmt *statement, int first, const MessageKey &key, Text &error) {
    return bind_folder_key(statement, first, key.folder, error) &&
        bind_i64(statement, first + 2, key.uidvalidity, error) &&
        bind_i32(statement, first + 3, key.uid, error);
}

bool bind_account_fields(sqlite3_stmt *statement, int first, const Account &account, Text &error) {
    return bind_text(statement, first, account.display_name, error) &&
        bind_text(statement, first + 1, account.email_address, error) &&
        bind_text(statement, first + 2, account.username, error) &&
        bind_text(statement, first + 3, account.smtp_username, error) &&
        bind_text(statement, first + 4, account.incoming_host, error) &&
        bind_text(statement, first + 5, account.outgoing_host, error) &&
        bind_i32(statement, first + 6, account.incoming_port, error) &&
        bind_i32(statement, first + 7, account.outgoing_port, error) &&
        bind_i32(statement, first + 8, static_cast<uint32_t>(account.incoming_protocol), error) &&
        bind_i32(statement, first + 9, static_cast<uint32_t>(account.incoming_security), error) &&
        bind_i32(statement, first + 10, static_cast<uint32_t>(account.outgoing_security), error) &&
        bind_i32(statement, first + 11, static_cast<uint32_t>(account.authentication), error) &&
        bind_bool(statement, first + 12, account.enabled, error) &&
        bind_bool(statement, first + 13, account.is_default, error) &&
        bind_i32(statement, first + 14, account.connect_timeout_ms, error) &&
        bind_i32(statement, first + 15, account.io_timeout_ms, error) &&
        bind_i32(statement, first + 16, account.poll_interval_minutes, error) &&
        bind_i32(statement, first + 17, account.cache_days, error) &&
        bind_text(statement, first + 18, account.custom_ca_file, error) &&
        bind_text(statement, first + 19, account.inbox_folder, error) &&
        bind_text(statement, first + 20, account.sent_folder, error) &&
        bind_text(statement, first + 21, account.drafts_folder, error) &&
        bind_text(statement, first + 22, account.trash_folder, error) &&
        bind_text(statement, first + 23, account.junk_folder, error) &&
        bind_text(statement, first + 24, account.archive_folder, error) &&
        bind_i64(statement, first + 25, account.signature_id, error) &&
        bind_i64(statement, first + 26, account.generation, error);
}

bool read_account(sqlite3_stmt *statement, Account &account) {
    Account next;
    bool enabled = false;
    bool is_default = false;
    if (!column_u64_checked(statement, 0, next.id) || !next.id) return false;
    if (!column_text(statement, 1, next.display_name) ||
        !column_text(statement, 2, next.email_address) ||
        !column_text(statement, 3, next.username) ||
        !column_text(statement, 4, next.smtp_username) ||
        !column_text(statement, 5, next.incoming_host) ||
        !column_text(statement, 6, next.outgoing_host)) return false;
    uint32_t incoming_protocol = 0, incoming_security = 0, outgoing_security = 0, authentication = 0;
    if (!column_u32_checked(statement, 7, next.incoming_port) ||
        !column_u32_checked(statement, 8, next.outgoing_port) ||
        !column_u32_checked(statement, 9, incoming_protocol) ||
        !column_u32_checked(statement, 10, incoming_security) ||
        !column_u32_checked(statement, 11, outgoing_security) ||
        !column_u32_checked(statement, 12, authentication)) return false;
    next.incoming_protocol = static_cast<IncomingProtocol>(incoming_protocol);
    next.incoming_security = static_cast<TransportSecurity>(incoming_security);
    next.outgoing_security = static_cast<TransportSecurity>(outgoing_security);
    next.authentication = static_cast<AuthenticationMethod>(authentication);
    if (!column_bool(statement, 13, enabled) || !column_bool(statement, 14, is_default)) return false;
    next.enabled = enabled;
    next.is_default = is_default;
    if (!column_u32_checked(statement, 15, next.connect_timeout_ms) ||
        !column_u32_checked(statement, 16, next.io_timeout_ms) ||
        !column_u32_checked(statement, 17, next.poll_interval_minutes) ||
        !column_u32_checked(statement, 18, next.cache_days)) return false;
    if (!column_text(statement, 19, next.custom_ca_file) ||
        !column_text(statement, 20, next.inbox_folder) ||
        !column_text(statement, 21, next.sent_folder) ||
        !column_text(statement, 22, next.drafts_folder) ||
        !column_text(statement, 23, next.trash_folder) ||
        !column_text(statement, 24, next.junk_folder) ||
        !column_text(statement, 25, next.archive_folder)) return false;
    if (!column_u64_checked(statement, 26, next.signature_id)) return false;
    if (!column_u64_checked(statement, 27, next.generation) || !next.generation) return false;
    account = static_cast<Account &&>(next);
    return true;
}

const char *AccountSelect =
    "SELECT id,display_name,email_address,username,smtp_username,incoming_host,outgoing_host,"
    "incoming_port,outgoing_port,incoming_protocol,incoming_security,outgoing_security,authentication,"
    "enabled,is_default,connect_timeout_ms,io_timeout_ms,poll_interval_minutes,cache_days,custom_ca_file,"
    "inbox_folder,sent_folder,drafts_folder,trash_folder,junk_folder,archive_folder,signature_id,"
    "config_generation FROM accounts";

bool get_user_version(sqlite3 *db, int &version, Text &error) {
    Statement query;
    if (!prepare(db, query, "PRAGMA user_version", error)) return false;
    const int code = sqlite3_step(query.value);
    if (code != SQLITE_ROW) return set_error(error, "cannot read profile schema version");
    version = sqlite3_column_int(query.value, 0);
    return true;
}

bool integrity_ok(sqlite3 *db, Text &error) {
    Statement query;
    if (!prepare(db, query, "PRAGMA integrity_check", error)) return false;
    if (sqlite3_step(query.value) != SQLITE_ROW) return set_error(error, "profile integrity check failed");
    const unsigned char *result = sqlite3_column_text(query.value, 0);
    const int length = sqlite3_column_bytes(query.value, 0);
    if (!result || length != 2 || memcmp(result, "ok", 2))
        return set_error(error, "profile integrity check failed");
    return sqlite3_step(query.value) == SQLITE_DONE || set_error(error, "profile integrity check failed");
}

bool make_random_name(const char *prefix, const char *suffix, Text &output, Text &error) {
    unsigned char random[16];
    if (!detail::random_bytes(random, sizeof(random), error)) return false;
    static const char digits[] = "0123456789abcdef";
    char name[96];
    size_t at = strlen(prefix);
    if (at + sizeof(random) * 2 + strlen(suffix) + 1 > sizeof(name)) {
        detail::wipe_memory(random, sizeof(random));
        return set_error(error, "generated cache name is too long");
    }
    memcpy(name, prefix, at);
    for (size_t i = 0; i < sizeof(random); ++i) {
        name[at++] = digits[random[i] >> 4];
        name[at++] = digits[random[i] & 15];
    }
    const size_t suffix_size = strlen(suffix);
    memcpy(name + at, suffix, suffix_size);
    at += suffix_size;
    name[at] = 0;
    detail::wipe_memory(random, sizeof(random));
    if (!output.set(name, at)) return set_error(error, "out of memory");
    return true;
}

bool anchored_child_path(int directory_fd, const char *name, char *path, size_t capacity) {
    const int count = snprintf(path, capacity, "/proc/self/fd/%d/%s", directory_fd, name);
    return count >= 0 && static_cast<size_t>(count) < capacity;
}

bool valid_cache_name(const Text &name) {
    size_t prefix_size = 0;
    if (name.size() == 4 + 32 + 4 && !memcmp(name.c_str(), "msg-", 4)) prefix_size = 4;
    if (name.size() == 6 + 32 + 4 && !memcmp(name.c_str(), "draft-", 6)) prefix_size = 6;
    if (name.size() == 5 + 32 + 4 && !memcmp(name.c_str(), "send-", 5)) prefix_size = 5;
    if (!prefix_size || memcmp(name.c_str() + name.size() - 4, ".bin", 4)) return false;
    for (size_t i = prefix_size; i < prefix_size + 32; ++i) {
        const char c = name.c_str()[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

void unlink_cache_file(int cache_fd, const Text &name) {
    if (valid_cache_name(name)) unlinkat(cache_fd, name.c_str(), 0);
}

bool collect_cache_names(sqlite3 *db, const char *sql, uint64_t account_id,
    Array<Text> &names, Text &error) {
    Statement query;
    if (!prepare(db, query, sql, error) ||
        (account_id && !bind_i64(query.value, 1, account_id, error))) return false;
    int code;
    while ((code = sqlite3_step(query.value)) == SQLITE_ROW) {
        Text name;
        if (!column_text(query.value, 0, name) || !names.push(static_cast<Text &&>(name)))
            return set_error(error, "out of memory reading cache index");
    }
    return code == SQLITE_DONE || set_error(error, "cannot read cache index");
}

bool add_cache_file(int directory_fd, const Text &name, const unsigned char *bytes,
    size_t size, Text &error) {
    detail::IoFaultPoint fault = detail::IoFaultPoint::None;
#ifdef RELIEFMAIL_TESTING
    if (consume_store_fault(StoreFaultPoint::CacheWrite)) fault = detail::IoFaultPoint::Write;
    else if (consume_store_fault(StoreFaultPoint::CacheRename)) fault = detail::IoFaultPoint::LinkOrRename;
#endif
    return detail::write_unique_file_at(directory_fd, name.c_str(), bytes, size, fault, error);
}

bool delete_cache_names(int directory_fd, const Array<Text> &names) {
    bool ok = true;
    for (size_t i = 0; i < names.size(); ++i) {
        if (!valid_cache_name(names[i])) continue;
        if (unlinkat(directory_fd, names[i].c_str(), 0) < 0 && errno != ENOENT) ok = false;
    }
    if (fsync(directory_fd) < 0) ok = false;
    return ok;
}

bool read_cache_file(int directory_fd, const Text &name, size_t maximum,
    Bytes &output, Text &error) {
    if (!valid_cache_name(name)) return set_error(error, "cache index contains an unsafe file name");
    unsigned char *bytes = nullptr;
    size_t size = 0;
    if (!detail::read_private_file_at(directory_fd, name.c_str(), maximum, &bytes, size, error)) return false;
    const bool copied = output.set(bytes, size);
    free(bytes);
    return copied || set_error(error, "out of memory reading cached content");
}

bool create_v1_schema(sqlite3 *db, Text &error) {
    const char *sql =
        "BEGIN IMMEDIATE;"
        "CREATE TABLE accounts(id INTEGER PRIMARY KEY,display_name TEXT NOT NULL,email_address TEXT NOT NULL,"
        "username TEXT NOT NULL,incoming_host TEXT NOT NULL,outgoing_host TEXT NOT NULL,incoming_port INTEGER NOT NULL,"
        "outgoing_port INTEGER NOT NULL,incoming_protocol INTEGER NOT NULL,incoming_security INTEGER NOT NULL,"
        "outgoing_security INTEGER NOT NULL,authentication INTEGER NOT NULL);"
        "PRAGMA user_version=1;COMMIT;";
    return exec_sql(db, sql, error, "cannot create initial profile schema");
}

bool migrate_v1_to_v2(sqlite3 *db, Text &error) {
    const char *sql =
        "BEGIN IMMEDIATE;"
        "ALTER TABLE accounts ADD COLUMN smtp_username TEXT NOT NULL DEFAULT '';"
        "UPDATE accounts SET smtp_username=username;"
        "ALTER TABLE accounts ADD COLUMN enabled INTEGER NOT NULL DEFAULT 1;"
        "ALTER TABLE accounts ADD COLUMN is_default INTEGER NOT NULL DEFAULT 0;"
        "ALTER TABLE accounts ADD COLUMN connect_timeout_ms INTEGER NOT NULL DEFAULT 15000;"
        "ALTER TABLE accounts ADD COLUMN io_timeout_ms INTEGER NOT NULL DEFAULT 30000;"
        "ALTER TABLE accounts ADD COLUMN poll_interval_minutes INTEGER NOT NULL DEFAULT 0;"
        "ALTER TABLE accounts ADD COLUMN cache_days INTEGER NOT NULL DEFAULT 30;"
        "ALTER TABLE accounts ADD COLUMN custom_ca_file TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE accounts ADD COLUMN inbox_folder TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE accounts ADD COLUMN sent_folder TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE accounts ADD COLUMN drafts_folder TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE accounts ADD COLUMN trash_folder TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE accounts ADD COLUMN junk_folder TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE accounts ADD COLUMN archive_folder TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE accounts ADD COLUMN signature_id INTEGER NOT NULL DEFAULT 0;"
        "CREATE UNIQUE INDEX one_default_account ON accounts(is_default) WHERE is_default=1;"
        "PRAGMA user_version=2;COMMIT;";
    return exec_sql(db, sql, error, "cannot migrate account settings");
}

bool migrate_v2_to_v3(sqlite3 *db, Text &error) {
    const char *sql =
        "BEGIN IMMEDIATE;"
        "CREATE TABLE folders(account_id INTEGER NOT NULL,server_name TEXT NOT NULL,display_name TEXT NOT NULL,"
        "role INTEGER NOT NULL,uidvalidity INTEGER NOT NULL,message_count INTEGER NOT NULL DEFAULT 0,"
        "unread_count INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(account_id,server_name),"
        "FOREIGN KEY(account_id) REFERENCES accounts(id) ON DELETE CASCADE);"
        "CREATE TABLE messages(account_id INTEGER NOT NULL,server_name TEXT NOT NULL,uidvalidity INTEGER NOT NULL,"
        "uid INTEGER NOT NULL,from_text TEXT NOT NULL,subject TEXT NOT NULL,internal_date INTEGER NOT NULL,"
        "size INTEGER NOT NULL,flags INTEGER NOT NULL,cache_name TEXT NOT NULL UNIQUE,"
        "PRIMARY KEY(account_id,server_name,uidvalidity,uid),"
        "FOREIGN KEY(account_id,server_name) REFERENCES folders(account_id,server_name) ON DELETE CASCADE);"
        "CREATE INDEX messages_folder_date ON messages(account_id,server_name,internal_date DESC,uid DESC);"
        "CREATE TABLE drafts(id INTEGER PRIMARY KEY AUTOINCREMENT,account_id INTEGER NOT NULL,"
        "to_input TEXT NOT NULL,cc_input TEXT NOT NULL,bcc_input TEXT NOT NULL,raw_recipient_input INTEGER NOT NULL,"
        "subject TEXT NOT NULL,body TEXT NOT NULL,html_body TEXT NOT NULL,editor_state BLOB NOT NULL DEFAULT X'',"
        "rich_format INTEGER NOT NULL,"
        "message_id TEXT NOT NULL,in_reply_to TEXT NOT NULL,references_text TEXT NOT NULL,send_stage INTEGER NOT NULL,"
        "updated_at INTEGER NOT NULL DEFAULT(unixepoch()),FOREIGN KEY(account_id) REFERENCES accounts(id) ON DELETE CASCADE);"
        "CREATE TABLE draft_addresses(draft_id INTEGER NOT NULL,kind INTEGER NOT NULL,position INTEGER NOT NULL,"
        "display_name TEXT NOT NULL,local_part TEXT NOT NULL,domain TEXT NOT NULL,display_quoted INTEGER NOT NULL,"
        "local_quoted INTEGER NOT NULL,PRIMARY KEY(draft_id,kind,position),"
        "FOREIGN KEY(draft_id) REFERENCES drafts(id) ON DELETE CASCADE);"
        "CREATE TABLE draft_attachments(draft_id INTEGER NOT NULL,position INTEGER NOT NULL,filename TEXT NOT NULL,"
        "content_type TEXT NOT NULL,content_id TEXT NOT NULL,inline_part INTEGER NOT NULL,size INTEGER NOT NULL,"
        "cache_name TEXT NOT NULL UNIQUE,PRIMARY KEY(draft_id,position),"
        "FOREIGN KEY(draft_id) REFERENCES drafts(id) ON DELETE CASCADE);"
        "CREATE TABLE contacts(id INTEGER PRIMARY KEY AUTOINCREMENT,display_name TEXT NOT NULL,"
        "organization TEXT NOT NULL,notes TEXT NOT NULL);"
        "CREATE TABLE contact_addresses(contact_id INTEGER NOT NULL,position INTEGER NOT NULL,display_name TEXT NOT NULL,"
        "local_part TEXT NOT NULL,domain TEXT NOT NULL,display_quoted INTEGER NOT NULL,local_quoted INTEGER NOT NULL,"
        "PRIMARY KEY(contact_id,position),FOREIGN KEY(contact_id) REFERENCES contacts(id) ON DELETE CASCADE);"
        "CREATE TABLE signatures(id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT NOT NULL,body TEXT NOT NULL,enabled INTEGER NOT NULL);"
        "CREATE TABLE templates(id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT NOT NULL,subject TEXT NOT NULL,body TEXT NOT NULL);"
        "CREATE TABLE rules(id INTEGER PRIMARY KEY AUTOINCREMENT,enabled INTEGER NOT NULL,field INTEGER NOT NULL,"
        "operation INTEGER NOT NULL,value TEXT NOT NULL,action INTEGER NOT NULL,destination_account_id INTEGER NOT NULL,"
        "destination_server_name TEXT NOT NULL);"
        "PRAGMA user_version=3;COMMIT;";
    return exec_sql(db, sql, error, "cannot create profile data tables");
}

bool migrate_v3_to_v4(sqlite3 *db, Text &error) {
    const char *sql =
        "BEGIN IMMEDIATE;"
        "ALTER TABLE accounts ADD COLUMN config_generation INTEGER NOT NULL DEFAULT 1;"
        "ALTER TABLE folders ADD COLUMN generation INTEGER NOT NULL DEFAULT 1;"
        "ALTER TABLE folders ADD COLUMN selectable INTEGER NOT NULL DEFAULT 1;"
        "ALTER TABLE messages ADD COLUMN headers_only INTEGER NOT NULL DEFAULT 0;"
        "ALTER TABLE drafts ADD COLUMN revision INTEGER NOT NULL DEFAULT 1;"
        "CREATE TABLE identity_sequence(id INTEGER PRIMARY KEY CHECK(id=1),next_value INTEGER NOT NULL CHECK(next_value>0));"
        "INSERT INTO identity_sequence(id,next_value) SELECT 1,MAX(v)+1 FROM ("
        "SELECT COALESCE(MAX(id),0) AS v FROM accounts UNION ALL SELECT COALESCE(MAX(account_id),0) FROM folders);"
        "UPDATE accounts SET config_generation=id;"
        "UPDATE folders SET generation=account_id;"
        "CREATE TABLE send_operations(id INTEGER PRIMARY KEY AUTOINCREMENT,account_id INTEGER NOT NULL,"
        "account_generation INTEGER NOT NULL,draft_id INTEGER NOT NULL,draft_revision INTEGER NOT NULL,"
        "message_id TEXT NOT NULL,sender TEXT NOT NULL,recipients TEXT NOT NULL,spool_name TEXT NOT NULL UNIQUE,"
        "spool_size INTEGER NOT NULL,stage INTEGER NOT NULL,delivery_count INTEGER NOT NULL DEFAULT 0,"
        "archive_folder TEXT NOT NULL DEFAULT '',FOREIGN KEY(account_id) REFERENCES accounts(id) ON DELETE CASCADE);"
        "CREATE INDEX send_operations_account_stage ON send_operations(account_id,stage,id);"
        "CREATE UNIQUE INDEX send_operations_draft_intent ON send_operations(draft_id,draft_revision);"
        "PRAGMA user_version=4;COMMIT;";
    return exec_sql(db, sql, error, "cannot migrate durable worker operations");
}

bool migrate_v4_to_v5(sqlite3 *db, Text &error) {
    const char *sql =
        "BEGIN IMMEDIATE;"
        "ALTER TABLE messages ADD COLUMN attachment_presence INTEGER NOT NULL DEFAULT 0 "
        "CHECK(attachment_presence BETWEEN 0 AND 2);"
        "PRAGMA user_version=5;COMMIT;";
    return exec_sql(db, sql, error, "cannot migrate message attachment coverage");
}

bool migrate_v5_to_v6(sqlite3 *db, Text &error) {
    const char *sql =
        "BEGIN IMMEDIATE;"
        "ALTER TABLE send_operations ADD COLUMN archive_review_generation INTEGER NOT NULL DEFAULT 0;"
        "ALTER TABLE send_operations ADD COLUMN archive_folder_generation INTEGER NOT NULL DEFAULT 0;"
        "ALTER TABLE send_operations ADD COLUMN archive_folder_uidvalidity INTEGER NOT NULL DEFAULT 0;"
        "ALTER TABLE send_operations ADD COLUMN last_diagnostic TEXT NOT NULL DEFAULT '';"
        "PRAGMA user_version=6;COMMIT;";
    return exec_sql(db, sql, error, "cannot migrate reviewed send archive state");
}

bool verified_database_backup(sqlite3 *source, int directory_fd, const char *prefix, Text &error) {
    Text name;
    if (!make_random_name(prefix, ".sqlite", name, error)) return false;
    int precreated;
    if (!detail::open_private_file_at(directory_fd, name.c_str(), O_RDWR | O_CREAT | O_EXCL,
            precreated, error)) return false;
    if (close(precreated) < 0) {
        unlinkat(directory_fd, name.c_str(), 0);
        return set_error(error, "cannot close schema backup file");
    }
    char path[PATH_MAX];
    if (!anchored_child_path(directory_fd, name.c_str(), path, sizeof(path))) {
        unlinkat(directory_fd, name.c_str(), 0);
        return set_error(error, "schema backup path is too long");
    }
    sqlite3 *destination = nullptr;
    int code = sqlite3_open_v2(path, &destination, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr);
    bool ok = code == SQLITE_OK && destination;
    if (ok) {
        sqlite3_backup *backup = sqlite3_backup_init(destination, "main", source, "main");
        if (!backup) ok = false;
        else {
            code = sqlite3_backup_step(backup, -1);
            const int finish_code = sqlite3_backup_finish(backup);
            ok = code == SQLITE_DONE && finish_code == SQLITE_OK;
        }
        if (ok) ok = integrity_ok(destination, error);
    }
    if (destination && sqlite3_close(destination) != SQLITE_OK) ok = false;
    int final_fd = openat(directory_fd, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (final_fd < 0 || fsync(final_fd) < 0) ok = false;
    if (final_fd >= 0 && close(final_fd) < 0) ok = false;
    if (!ok) {
        unlinkat(directory_fd, name.c_str(), 0);
        if (error.empty()) set_error(error, "cannot create a verified schema backup");
        return false;
    }
    return true;
}

bool initialize_schema(sqlite3 *db, int data_fd, Text &error) {
    int version = 0;
    if (!get_user_version(db, version, error)) return false;
    if (version < 0) return set_error(error, "profile schema version is invalid");
    if (version > CurrentSchemaVersion) {
        if (!verified_database_backup(db, data_fd, "mail.unsupported-", error)) return false;
        return set_error(error, "profile was created by a newer ReliefMail version");
    }
    if (version == 0 && !create_v1_schema(db, error)) return false;
    if (version > 0 && version < CurrentSchemaVersion &&
        !verified_database_backup(db, data_fd, "mail.pre-migration-", error)) return false;
    if (version == 0) version = 1;
    if (version == 1) {
        if (!migrate_v1_to_v2(db, error)) return false;
        version = 2;
    }
    if (version == 2) {
        if (!migrate_v2_to_v3(db, error)) return false;
        version = 3;
    }
    if (version == 3) {
        if (!migrate_v3_to_v4(db, error)) return false;
        version = 4;
    }
    if (version == 4) {
        if (!migrate_v4_to_v5(db, error)) return false;
        version = 5;
    }
    if (version == 5) {
        if (!migrate_v5_to_v6(db, error)) return false;
        version = 6;
    }
    if (version != CurrentSchemaVersion) return set_error(error, "profile schema migration is incomplete");
    return integrity_ok(db, error);
}

bool load_message_summary(sqlite3_stmt *statement, MessageSummary &summary) {
    MessageSummary next;
    next.key.folder.account_id = column_u64(statement, 0);
    if (!column_text(statement, 1, next.key.folder.server_name)) return false;
    next.key.uidvalidity = column_u64(statement, 2);
    next.key.uid = static_cast<uint32_t>(sqlite3_column_int64(statement, 3));
    if (!column_text(statement, 4, next.from) || !column_text(statement, 5, next.subject)) return false;
    sqlite3_int64 internal_date = 0;
    if (!column_i64(statement, 6, internal_date)) return false;
    next.internal_date = static_cast<int64_t>(internal_date);
    next.size = column_u64(statement, 7);
    next.flags = static_cast<uint32_t>(sqlite3_column_int64(statement, 8));
    if (!column_bool(statement, 9, next.headers_only)) return false;
    summary = static_cast<MessageSummary &&>(next);
    return true;
}

const char *MessageSelect =
    "SELECT account_id,server_name,uidvalidity,uid,from_text,subject,internal_date,size,flags,headers_only,cache_name FROM messages";

bool query_folder_uidvalidity(sqlite3 *db, const FolderKey &key, uint64_t &uidvalidity, bool &found, Text &error) {
    Statement query;
    if (!prepare(db, query, "SELECT uidvalidity FROM folders WHERE account_id=? AND server_name=?", error) ||
        !bind_folder_key(query.value, 1, key, error)) return false;
    const int code = sqlite3_step(query.value);
    if (code == SQLITE_DONE) { found = false; uidvalidity = 0; return true; }
    if (code != SQLITE_ROW) return set_error(error, "cannot read folder UIDVALIDITY");
    uidvalidity = column_u64(query.value, 0);
    found = true;
    return true;
}

bool append_text(sqlite3_stmt *query, int column, Array<Text> &names, Text &error) {
    Text name;
    if (!column_text(query, column, name) || !names.push(static_cast<Text &&>(name)))
        return set_error(error, "out of memory reading cache references");
    return true;
}

struct DraftFileSource { int fd; size_t size; size_t references; };

void retain_draft_file_source(void *context) {
    DraftFileSource *source = static_cast<DraftFileSource *>(context);
    if (source && source->references != SIZE_MAX) ++source->references;
}

void release_draft_file_source(void *context) {
    DraftFileSource *source = static_cast<DraftFileSource *>(context);
    if (!source || !source->references) return;
    if (--source->references) return;
    if (source->fd >= 0) ::close(source->fd);
    free(source);
}

bool read_draft_file(void *context, size_t offset, unsigned char *buffer,
    size_t capacity, size_t *read_size) {
    if (!context || (!buffer && capacity) || !read_size) return false;
    DraftFileSource *source = static_cast<DraftFileSource *>(context);
    *read_size = 0;
    if (offset > source->size || offset > static_cast<size_t>(INT64_MAX)) return false;
    size_t requested = source->size - offset;
    if (requested > capacity) requested = capacity;
    if (requested > static_cast<size_t>(SSIZE_MAX)) requested = static_cast<size_t>(SSIZE_MAX);
    while (requested) {
        const ssize_t count = pread(source->fd, buffer, requested, static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return false;
        *read_size = static_cast<size_t>(count);
        return true;
    }
    return true;
}

bool read_address(sqlite3_stmt *statement, int first, Address &address) {
    if (!column_text(statement, first, address.display_name) ||
        !column_text(statement, first + 1, address.local_part) ||
        !column_text(statement, first + 2, address.domain) ||
        !column_bool(statement, first + 3, address.display_name_quoted) ||
        !column_bool(statement, first + 4, address.local_part_quoted)) return false;
    return true;
}

bool bind_address(sqlite3_stmt *statement, int first, const Address &address, Text &error) {
    return bind_text(statement, first, address.display_name, error) &&
        bind_text(statement, first + 1, address.local_part, error) &&
        bind_text(statement, first + 2, address.domain, error) &&
        bind_bool(statement, first + 3, address.display_name_quoted, error) &&
        bind_bool(statement, first + 4, address.local_part_quoted, error);
}

bool insert_draft_addresses(sqlite3 *db, uint64_t draft_id, int kind,
    const Array<Address> &addresses, Text &error) {
    if (addresses.size() > MaxContactAddresses) return set_error(error, "draft has too many addresses");
    Statement insert;
    if (!prepare(db, insert, "INSERT INTO draft_addresses(draft_id,kind,position,display_name,local_part,domain,"
            "display_quoted,local_quoted) VALUES(?,?,?,?,?,?,?,?)", error)) return false;
    for (size_t at = 0; at < addresses.size(); ++at) {
        if (!bind_i64(insert.value, 1, draft_id, error) || !bind_i32(insert.value, 2, static_cast<uint32_t>(kind), error) ||
            !bind_i64(insert.value, 3, at, error) || !bind_address(insert.value, 4, addresses[at], error) ||
            sqlite3_step(insert.value) != SQLITE_DONE)
            return error.empty() ? set_error(error, "cannot save draft address") : false;
        sqlite3_reset(insert.value);
        sqlite3_clear_bindings(insert.value);
    }
    return true;
}

bool load_draft_addresses(sqlite3 *db, uint64_t draft_id, Draft &draft, Text &error) {
    Statement query;
    if (!prepare(db, query, "SELECT kind,position,display_name,local_part,domain,display_quoted,local_quoted "
            "FROM draft_addresses WHERE draft_id=? ORDER BY kind,position", error) ||
        !bind_i64(query.value, 1, draft_id, error)) return false;
    size_t expected_position[3] = {0, 0, 0};
    int code;
    while ((code = sqlite3_step(query.value)) == SQLITE_ROW) {
        sqlite3_int64 kind = 0, position = 0;
        if (!column_i64(query.value, 0, kind) || !column_i64(query.value, 1, position))
            return set_error(error, "draft address index is invalid");
        if (kind < 0 || kind > 2 || position < 0 ||
            static_cast<uint64_t>(position) != expected_position[kind] ||
            expected_position[kind] >= MaxContactAddresses)
            return set_error(error, "draft address index is invalid");
        Address address;
        Text serialized;
        if (!read_address(query.value, 2, address) || !serialize_address(address, serialized, error))
            return error.empty() ? set_error(error, "draft address is invalid") : false;
        Array<Address> *list = kind == 0 ? &draft.to : (kind == 1 ? &draft.cc : &draft.bcc);
        if (!list->push(static_cast<Address &&>(address))) return set_error(error, "out of memory loading draft addresses");
        ++expected_position[kind];
    }
    return code == SQLITE_DONE || set_error(error, "cannot load draft addresses");
}

bool load_draft_record(sqlite3_stmt *query, Draft &draft, Text &error) {
    Draft next;
    if (!column_u64_checked(query, 0, next.id) || !column_u64_checked(query, 1, next.account_id))
        return set_error(error, "draft identity is invalid");
    if (!next.id || !next.account_id || !column_bool(query, 5, next.raw_recipient_input) ||
        !column_bool(query, 10, next.rich_format) ||
        !column_text(query, 2, next.to_input) || !column_text(query, 3, next.cc_input) ||
        !column_text(query, 4, next.bcc_input)) return set_error(error, "draft recipient data is invalid");
    if (!column_text(query, 6, next.subject) || !column_text(query, 7, next.body) ||
        !column_text(query, 8, next.html_body) || !column_blob(query, 9, next.editor_state) ||
        !column_text(query, 11, next.message_id) || !column_text(query, 12, next.in_reply_to) ||
        !column_text(query, 13, next.references)) return set_error(error, "draft data is invalid");
    sqlite3_int64 stage = 0;
    if (!column_i64(query, 14, stage) || stage < 0 || stage > static_cast<int>(SendStage::ReviewedDelivered))
        return set_error(error, "draft send stage is invalid");
    next.send_stage = static_cast<SendStage>(stage);
    if (!column_u64_checked(query, 15, next.revision) || !next.revision)
        return set_error(error, "draft revision is invalid");
    draft = static_cast<Draft &&>(next);
    return true;
}

bool load_draft_attachments(sqlite3 *db, int cache_fd, uint64_t draft_id,
    Draft &draft, Text &error) {
    Statement query;
    if (!prepare(db, query, "SELECT position,filename,content_type,content_id,inline_part,size,cache_name "
            "FROM draft_attachments WHERE draft_id=? ORDER BY position", error) ||
        !bind_i64(query.value, 1, draft_id, error)) return false;
    size_t position = 0;
    size_t total_size = 0;
    int code;
    while ((code = sqlite3_step(query.value)) == SQLITE_ROW) {
        sqlite3_int64 row_position = 0;
        uint64_t raw_size = 0;
        bool inline_part = false;
        if (position >= MaxMimeParts || !column_i64(query.value, 0, row_position) || row_position < 0 ||
            static_cast<uint64_t>(row_position) != position ||
            !column_bool(query.value, 4, inline_part) ||
            !column_u64_checked(query.value, 5, raw_size) ||
            raw_size > DefaultMaxAttachmentBytes - total_size)
            return set_error(error, "draft attachment index is invalid");
        Attachment attachment;
        Text cache_name;
        if (!column_text(query.value, 1, attachment.filename) ||
            !column_text(query.value, 2, attachment.content_type) ||
            !column_text(query.value, 3, attachment.content_id) || !column_text(query.value, 6, cache_name))
            return set_error(error, "draft attachment metadata is invalid");
        attachment.inline_part = inline_part;
        int fd = -1;
        if (!valid_cache_name(cache_name)) return set_error(error, "draft attachment cache name is invalid");
        if (!detail::open_private_file_at(cache_fd, cache_name.c_str(), O_RDONLY, fd, error)) return false;
        struct stat info;
        if (fstat(fd, &info) < 0 || info.st_size < 0 || static_cast<uint64_t>(info.st_size) != raw_size) {
            close(fd);
            return set_error(error, "draft attachment cache is inconsistent");
        }
        DraftFileSource *source = static_cast<DraftFileSource *>(malloc(sizeof(DraftFileSource)));
        if (!source) { close(fd); return set_error(error, "out of memory loading draft attachment"); }
        source->fd = fd;
        source->size = static_cast<size_t>(raw_size);
        source->references = 1;
        attachment.source.set_owned(source, read_draft_file, source->size,
            retain_draft_file_source, release_draft_file_source);
        if (!draft.attachments.push(static_cast<Attachment &&>(attachment))) {
            return set_error(error, "too many draft attachments");
        }
        total_size += static_cast<size_t>(raw_size);
        ++position;
    }
    return code == SQLITE_DONE || set_error(error, "cannot load draft attachments");
}

} // namespace

#ifdef RELIEFMAIL_TESTING
void store_test_fail_next(StoreFaultPoint point) { next_store_fault = point; }
void store_test_set_backup_hook(void (*hook)(void *), void *context) {
    next_backup_hook = hook;
    next_backup_hook_context = context;
}
void store_test_set_restore_faults(uint32_t mask) { restore_fault_mask = mask; }
#endif

Store::Store(Store &&other) noexcept {
    database_ = other.database_; other.database_ = nullptr;
    config_fd_ = other.config_fd_; other.config_fd_ = -1;
    data_fd_ = other.data_fd_; other.data_fd_ = -1;
    cache_fd_ = other.cache_fd_; other.cache_fd_ = -1;
    lock_fd_ = other.lock_fd_; other.lock_fd_ = -1;
    mode_ = other.mode_;
    config_dir_path_ = static_cast<Text &&>(other.config_dir_path_);
    data_dir_path_ = static_cast<Text &&>(other.data_dir_path_);
    cache_dir_path_ = static_cast<Text &&>(other.cache_dir_path_);
}

Store &Store::operator=(Store &&other) noexcept {
    if (this == &other) return *this;
    close();
    database_ = other.database_; other.database_ = nullptr;
    config_fd_ = other.config_fd_; other.config_fd_ = -1;
    data_fd_ = other.data_fd_; other.data_fd_ = -1;
    cache_fd_ = other.cache_fd_; other.cache_fd_ = -1;
    lock_fd_ = other.lock_fd_; other.lock_fd_ = -1;
    mode_ = other.mode_;
    config_dir_path_ = static_cast<Text &&>(other.config_dir_path_);
    data_dir_path_ = static_cast<Text &&>(other.data_dir_path_);
    cache_dir_path_ = static_cast<Text &&>(other.cache_dir_path_);
    return *this;
}

Store::~Store() { close(); }

void Store::close() {
    if (database_) sqlite3_close(static_cast<sqlite3 *>(database_));
    database_ = nullptr;
    if (lock_fd_ >= 0) ::close(lock_fd_);
    if (config_fd_ >= 0) ::close(config_fd_);
    if (data_fd_ >= 0) ::close(data_fd_);
    if (cache_fd_ >= 0) ::close(cache_fd_);
    lock_fd_ = config_fd_ = data_fd_ = cache_fd_ = -1;
    config_dir_path_.clear();
    data_dir_path_.clear();
    cache_dir_path_.clear();
}

bool Store::open(const ProfilePaths &paths, StoreOpenMode mode, Text &error) {
    close();
    if (!detail::open_secure_profile_directory(paths.config_dir, config_fd_, error) ||
        !detail::open_secure_profile_directory(paths.data_dir, data_fd_, error) ||
        !detail::open_secure_profile_directory(paths.cache_dir, cache_fd_, error)) {
        close();
        return false;
    }
    config_dir_path_ = paths.config_dir;
    data_dir_path_ = paths.data_dir;
    cache_dir_path_ = paths.cache_dir;
    mode_ = mode;

    if (mode == StoreOpenMode::InstanceOwner) {
        if (!detail::open_private_file_at(config_fd_, "instance.lock", O_RDWR | O_CREAT,
                lock_fd_, error)) { close(); return false; }
        if (flock(lock_fd_, LOCK_EX | LOCK_NB) < 0) {
            close();
            return set_error(error, "another ReliefMail instance owns this profile");
        }
    }

    RestoreGate restore_gate;
    const bool coordinated_owner = mode == StoreOpenMode::CoordinatedOwner;
    if (!restore_gate.lock(config_fd_, mode == StoreOpenMode::InstanceOwner || coordinated_owner, error)) {
        close();
        return false;
    }
    if (mode == StoreOpenMode::InstanceOwner || coordinated_owner) {
        if (!recover_restore_journal(config_fd_, data_fd_, cache_fd_, error)) {
            close();
            return false;
        }
    } else {
        bool pending = false;
        if (!restore_journal_exists(config_fd_, pending, error)) {
            close();
            return false;
        }
        if (pending) {
            close();
            return set_error(error, "profile restore recovery is pending; an instance owner must recover it");
        }
    }

    int database_fd = -1;
    if (!detail::open_private_file_at(data_fd_, "mail.sqlite", O_RDWR | O_CREAT,
            database_fd, error)) { close(); return false; }
    ::close(database_fd);
    char path[PATH_MAX];
    if (!anchored_child_path(data_fd_, "mail.sqlite", path, sizeof(path))) {
        close();
        return set_error(error, "profile database path is too long");
    }
    sqlite3 *db = nullptr;
    int code = sqlite3_open_v2(path, &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
    if (code != SQLITE_OK || !db) {
        if (db) sqlite3_close(db);
        close();
        return set_error(error, "cannot open private profile database");
    }
    database_ = db;
    sqlite3_busy_timeout(db, 150);
    sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
    if (!exec_sql(db, "PRAGMA journal_mode=DELETE;PRAGMA synchronous=FULL;PRAGMA foreign_keys=ON;"
            "PRAGMA trusted_schema=OFF;PRAGMA secure_delete=ON;", error,
            "cannot configure durable profile database") || !initialize_schema(db, data_fd_, error)) {
        close();
        return false;
    }
    return true;
}

bool Store::save_account(Account &account, Text &error) {
    return save_account_guarded(account, 0, error);
}

bool Store::save_account_guarded(Account &account, uint64_t expected_generation, Text &error) {
    if (!database_) return set_error(error, "profile store is closed");
    Account candidate = account;
    if (!candidate.id) candidate.id = 1;
    if (!validate_account(candidate, error)) return false;
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    if (!transaction_begin(db, error)) return false;
    if (!account.id) {
        Statement count;
        if (!prepare(db, count, "SELECT COUNT(*) FROM accounts", error) || sqlite3_step(count.value) != SQLITE_ROW) {
            transaction_rollback(db);
            return set_error(error, "cannot count accounts");
        }
        if (sqlite3_column_int(count.value, 0) >= static_cast<int>(MaxAccounts)) {
            transaction_rollback(db);
            return set_error(error, "account limit reached");
        }
    } else {
        Statement exists;
        if (!prepare(db, exists, "SELECT config_generation FROM accounts WHERE id=?", error) ||
            !bind_i64(exists.value, 1, account.id, error)) {
            transaction_rollback(db);
            return false;
        }
        const int code = sqlite3_step(exists.value);
        if (code != SQLITE_ROW || (expected_generation &&
            static_cast<uint64_t>(sqlite3_column_int64(exists.value, 0)) != expected_generation)) {
            transaction_rollback(db);
            return set_error(error, code == SQLITE_DONE ? "account id was not found for update" :
                "account configuration generation changed before update");
        }
    }
    uint64_t new_generation = 0;
    if (!next_identity_generation(db, new_generation, error)) {
        transaction_rollback(db);
        return false;
    }
    account.generation = new_generation;
    if (account.is_default) {
        Statement clear_default;
        if (!prepare(db, clear_default, "UPDATE accounts SET is_default=0 WHERE id<>?", error) ||
            !bind_i64(clear_default.value, 1, account.id, error) ||
            sqlite3_step(clear_default.value) != SQLITE_DONE) {
            transaction_rollback(db);
            return set_error(error, "cannot update default account");
        }
    }
    const char *insert_new =
        "INSERT INTO accounts(display_name,email_address,username,smtp_username,incoming_host,outgoing_host,"
        "incoming_port,outgoing_port,incoming_protocol,incoming_security,outgoing_security,authentication,enabled,"
        "is_default,connect_timeout_ms,io_timeout_ms,poll_interval_minutes,cache_days,custom_ca_file,inbox_folder,"
        "sent_folder,drafts_folder,trash_folder,junk_folder,archive_folder,signature_id,config_generation)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
    const char *upsert =
        "INSERT INTO accounts(id,display_name,email_address,username,smtp_username,incoming_host,outgoing_host,"
        "incoming_port,outgoing_port,incoming_protocol,incoming_security,outgoing_security,authentication,enabled,"
        "is_default,connect_timeout_ms,io_timeout_ms,poll_interval_minutes,cache_days,custom_ca_file,inbox_folder,"
        "sent_folder,drafts_folder,trash_folder,junk_folder,archive_folder,signature_id,config_generation)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET "
        "display_name=excluded.display_name,email_address=excluded.email_address,username=excluded.username,"
        "smtp_username=excluded.smtp_username,incoming_host=excluded.incoming_host,outgoing_host=excluded.outgoing_host,"
        "incoming_port=excluded.incoming_port,outgoing_port=excluded.outgoing_port,"
        "incoming_protocol=excluded.incoming_protocol,incoming_security=excluded.incoming_security,"
        "outgoing_security=excluded.outgoing_security,authentication=excluded.authentication,enabled=excluded.enabled,"
        "is_default=excluded.is_default,connect_timeout_ms=excluded.connect_timeout_ms,"
        "io_timeout_ms=excluded.io_timeout_ms,poll_interval_minutes=excluded.poll_interval_minutes,"
        "cache_days=excluded.cache_days,custom_ca_file=excluded.custom_ca_file,inbox_folder=excluded.inbox_folder,"
        "sent_folder=excluded.sent_folder,drafts_folder=excluded.drafts_folder,trash_folder=excluded.trash_folder,"
        "junk_folder=excluded.junk_folder,archive_folder=excluded.archive_folder,signature_id=excluded.signature_id,"
        "config_generation=excluded.config_generation";
    Statement save;
    const bool existing_id = account.id != 0;
    if (!prepare(db, save, existing_id ? upsert : insert_new, error) ||
        (existing_id && !bind_i64(save.value, 1, account.id, error)) ||
        !bind_account_fields(save.value, existing_id ? 2 : 1, account, error) ||
        sqlite3_step(save.value) != SQLITE_DONE) {
        transaction_rollback(db);
        if (error.empty()) set_error(error, "cannot save account");
        return false;
    }
    const Id saved_id = existing_id ? account.id : static_cast<Id>(sqlite3_last_insert_rowid(db));
    if (!saved_id || !transaction_commit(db, error)) return false;
    account.id = saved_id;
    return true;
}

bool Store::get_account(Id id, Account &account, Text &error) {
    if (!database_ || !id) return set_error(error, "account id is invalid");
    char sql[1024];
    const int length = snprintf(sql, sizeof(sql), "%s WHERE id=?", AccountSelect);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(sql)) return set_error(error, "account query is too long");
    Statement query;
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    if (!prepare(db, query, sql, error) || !bind_i64(query.value, 1, id, error)) return false;
    const int code = sqlite3_step(query.value);
    if (code == SQLITE_DONE) return set_error(error, "account was not found");
    if (code != SQLITE_ROW) return set_error(error, "cannot read account");
    Account next;
    if (!read_account(query.value, next)) return set_error(error, "account data is invalid");
    account = static_cast<Account &&>(next);
    return true;
}

bool Store::list_accounts(AccountList &accounts, Text &error) {
    if (!database_) return set_error(error, "profile store is closed");
    AccountList next;
    Statement query;
    if (!prepare(static_cast<sqlite3 *>(database_), query,
            "SELECT id,display_name,email_address,username,smtp_username,incoming_host,outgoing_host,"
            "incoming_port,outgoing_port,incoming_protocol,incoming_security,outgoing_security,authentication,"
            "enabled,is_default,connect_timeout_ms,io_timeout_ms,poll_interval_minutes,cache_days,custom_ca_file,"
            "inbox_folder,sent_folder,drafts_folder,trash_folder,junk_folder,archive_folder,signature_id,config_generation "
            "FROM accounts ORDER BY id", error)) return false;
    int code;
    while ((code = sqlite3_step(query.value)) == SQLITE_ROW) {
        Account item;
        if (!read_account(query.value, item) || !next.push(static_cast<Account &&>(item)))
            return set_error(error, "account listing exceeds its limit or contains invalid data");
    }
    if (code != SQLITE_DONE) return set_error(error, "cannot list accounts");
    accounts = static_cast<AccountList &&>(next);
    return true;
}

bool Store::delete_account(Id id, Text &error) {
    return delete_account_guarded(id, 0, error);
}

bool Store::delete_account_guarded(Id id, uint64_t expected_generation, Text &error) {
    if (!database_ || !id) return set_error(error, "account id is invalid");
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    Array<Text> files;
    if (!transaction_begin(db, error)) return false;
    if (expected_generation) {
        Statement identity;
        if (!prepare(db, identity, "SELECT config_generation FROM accounts WHERE id=?", error) ||
            !bind_i64(identity.value, 1, id, error) || sqlite3_step(identity.value) != SQLITE_ROW ||
            static_cast<uint64_t>(sqlite3_column_int64(identity.value, 0)) != expected_generation) {
            transaction_rollback(db);
            return set_error(error, "account configuration changed before delete");
        }
    }
    if (!collect_cache_names(db, "SELECT cache_name FROM messages WHERE account_id=?", id, files, error) ||
        !collect_cache_names(db, "SELECT cache_name FROM draft_attachments WHERE draft_id IN "
            "(SELECT id FROM drafts WHERE account_id=?)", id, files, error) ||
        !collect_cache_names(db, "SELECT spool_name FROM send_operations WHERE account_id=?", id, files, error)) {
        transaction_rollback(db);
        return false;
    }
    Statement rules;
    if (!prepare(db, rules, "DELETE FROM rules WHERE destination_account_id=?", error) ||
        !bind_i64(rules.value, 1, id, error) || sqlite3_step(rules.value) != SQLITE_DONE) {
        transaction_rollback(db);
        return set_error(error, "cannot remove account rules");
    }
    Statement remove;
    if (!prepare(db, remove, "DELETE FROM accounts WHERE id=?", error) ||
        !bind_i64(remove.value, 1, id, error) || sqlite3_step(remove.value) != SQLITE_DONE ||
        sqlite3_changes(db) != 1) {
        transaction_rollback(db);
        return set_error(error, "cannot delete account");
    }
    if (!transaction_commit(db, error)) return false;
    delete_cache_names(cache_fd_, files);
    return true;
}

bool Store::save_folder(const Folder &folder, Text &error) {
    if (!folder.uidvalidity) return set_error(error, "selected folder UIDVALIDITY is required");
    return save_folder_metadata_guarded(folder, 0, UINT64_MAX, error);
}

bool Store::save_folder_metadata(const Folder &folder, Text &error) {
    return save_folder_metadata_guarded(folder, 0, UINT64_MAX, error);
}

bool Store::save_folder_metadata_guarded(const Folder &folder, uint64_t account_generation,
    uint64_t expected_folder_generation, Text &error) {
    if (!database_ || !valid_folder_key(folder.key) || !valid_text(folder.display_name) ||
        static_cast<unsigned>(folder.role) > static_cast<unsigned>(FolderRole::Archive) ||
        folder.uidvalidity > static_cast<uint64_t>(INT64_MAX) || folder.unread_count > folder.message_count)
        return set_error(error, "folder metadata is invalid");
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    Array<Text> stale_files;
    if (!transaction_begin(db, error)) return false;
    if (!account_generation_matches(db, folder.key.account_id, account_generation, error)) {
        transaction_rollback(db);
        return false;
    }
    uint64_t previous_uidvalidity = 0;
    uint64_t generation = 0;
    bool found = false;
    Statement old;
    if (!prepare(db, old, "SELECT uidvalidity,generation FROM folders WHERE account_id=? AND server_name=?", error) ||
        !bind_folder_key(old.value, 1, folder.key, error)) {
        transaction_rollback(db);
        return false;
    }
    const int old_code = sqlite3_step(old.value);
    if (old_code == SQLITE_ROW) {
        previous_uidvalidity = column_u64(old.value, 0);
        generation = column_u64(old.value, 1);
        found = true;
    } else if (old_code != SQLITE_DONE) {
        transaction_rollback(db);
        return set_error(error, "cannot inspect folder identity");
    }
    if (expected_folder_generation != UINT64_MAX &&
        ((expected_folder_generation == 0 && found) ||
         (expected_folder_generation != 0 && (!found || generation != expected_folder_generation)))) {
        transaction_rollback(db);
        return set_error(error, "folder identity changed before metadata refresh");
    }
    const uint64_t effective_uidvalidity = folder.uidvalidity ? folder.uidvalidity : previous_uidvalidity;
    if (!found) {
        Statement count;
        if (!prepare(db, count, "SELECT COUNT(*) FROM folders WHERE account_id=?", error) ||
            !bind_i64(count.value, 1, folder.key.account_id, error) || sqlite3_step(count.value) != SQLITE_ROW ||
            sqlite3_column_int64(count.value, 0) >= static_cast<sqlite3_int64>(MaxFoldersPerAccount)) {
            transaction_rollback(db);
            return set_error(error, "folder limit reached or account folder count is invalid");
        }
        if (!next_identity_generation(db, generation, error)) {
            transaction_rollback(db);
            return false;
        }
    }
    if (found && folder.uidvalidity && previous_uidvalidity != folder.uidvalidity) {
        Statement list;
        if (!prepare(db, list, "SELECT cache_name FROM messages WHERE account_id=? AND server_name=?", error) ||
            !bind_folder_key(list.value, 1, folder.key, error)) {
            transaction_rollback(db);
            return false;
        }
        int code;
        while ((code = sqlite3_step(list.value)) == SQLITE_ROW)
            if (!append_text(list.value, 0, stale_files, error)) { transaction_rollback(db); return false; }
        if (code != SQLITE_DONE) { transaction_rollback(db); return set_error(error, "cannot invalidate old folder cache"); }
        Statement remove_messages;
        if (!prepare(db, remove_messages, "DELETE FROM messages WHERE account_id=? AND server_name=?", error) ||
            !bind_folder_key(remove_messages.value, 1, folder.key, error) || sqlite3_step(remove_messages.value) != SQLITE_DONE) {
            transaction_rollback(db);
            return set_error(error, "cannot invalidate old folder messages");
        }
    }
    Statement save;
    if (!prepare(db, save, "INSERT INTO folders(account_id,server_name,display_name,role,uidvalidity,message_count,"
            "unread_count,generation,selectable) VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT(account_id,server_name) "
            "DO UPDATE SET display_name=excluded.display_name,role=excluded.role,uidvalidity=excluded.uidvalidity,"
            "message_count=excluded.message_count,unread_count=excluded.unread_count,selectable=excluded.selectable", error) ||
        !bind_folder_key(save.value, 1, folder.key, error) || !bind_text(save.value, 3, folder.display_name, error) ||
        !bind_i32(save.value, 4, static_cast<uint32_t>(folder.role), error) ||
        !bind_i64(save.value, 5, effective_uidvalidity, error) ||
        !bind_i32(save.value, 6, folder.message_count, error) || !bind_i32(save.value, 7, folder.unread_count, error) ||
        !bind_i64(save.value, 8, generation, error) || !bind_bool(save.value, 9, folder.selectable, error) ||
        sqlite3_step(save.value) != SQLITE_DONE) {
        transaction_rollback(db);
        if (error.empty()) set_error(error, "cannot save folder metadata");
        return false;
    }
    if (!transaction_commit(db, error)) return false;
    delete_cache_names(cache_fd_, stale_files);
    return true;
}

bool Store::update_folder_uidvalidity(const FolderKey &key, uint64_t uidvalidity, Text &error) {
    return update_folder_uidvalidity_guarded(key, uidvalidity, 0, 0, UINT64_MAX, error);
}

bool Store::update_folder_uidvalidity_guarded(const FolderKey &key, uint64_t uidvalidity,
    uint64_t account_generation, uint64_t folder_generation,
    uint64_t expected_current_uidvalidity, Text &error) {
    error.clear();
    if (!database_ || !valid_folder_key(key) || !uidvalidity)
        return set_error(error, "folder UIDVALIDITY update is invalid");
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    Array<Text> stale_files;
    if (!transaction_begin(db, error)) return false;
    if (!account_generation_matches(db, key.account_id, account_generation, error) ||
        (folder_generation && !folder_generation_matches(db, key, folder_generation, 0, error))) {
        transaction_rollback(db);
        return false;
    }
    uint64_t previous = 0;
    bool found = false;
    if (!query_folder_uidvalidity(db, key, previous, found, error) || !found) {
        transaction_rollback(db);
        if (error.empty()) set_error(error, "folder was not found");
        return false;
    }
    if (expected_current_uidvalidity != UINT64_MAX && previous != expected_current_uidvalidity) {
        transaction_rollback(db);
        return set_error(error, "folder UIDVALIDITY changed before the selected value could be saved");
    }
    if (previous != uidvalidity) {
        Statement files;
        if (!prepare(db, files, "SELECT cache_name FROM messages WHERE account_id=? AND server_name=?", error) ||
            !bind_folder_key(files.value, 1, key, error)) {
            transaction_rollback(db);
            return false;
        }
        int code;
        while ((code = sqlite3_step(files.value)) == SQLITE_ROW) {
            if (!append_text(files.value, 0, stale_files, error)) {
                transaction_rollback(db);
                return false;
            }
        }
        if (code != SQLITE_DONE) {
            transaction_rollback(db);
            return set_error(error, "cannot read stale folder cache references");
        }
        Statement remove;
        if (!prepare(db, remove, "DELETE FROM messages WHERE account_id=? AND server_name=?", error) ||
            !bind_folder_key(remove.value, 1, key, error) || sqlite3_step(remove.value) != SQLITE_DONE) {
            transaction_rollback(db);
            return set_error(error, "cannot invalidate stale folder mappings");
        }
    }
    Statement update;
    if (!prepare(db, update, "UPDATE folders SET uidvalidity=? WHERE account_id=? AND server_name=? AND selectable=1", error) ||
        !bind_i64(update.value, 1, uidvalidity, error) || !bind_folder_key(update.value, 2, key, error) ||
        sqlite3_step(update.value) != SQLITE_DONE || sqlite3_changes(db) != 1) {
        transaction_rollback(db);
        if (error.empty()) set_error(error, "cannot persist folder UIDVALIDITY");
        return false;
    }
    if (!transaction_commit(db, error)) return false;
    delete_cache_names(cache_fd_, stale_files);
    return true;
}

bool Store::get_folder(const FolderKey &key, Folder &folder, Text &error) {
    if (!database_ || !valid_folder_key(key)) return set_error(error, "folder key is invalid");
    Statement query;
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    if (!prepare(db, query, "SELECT account_id,server_name,display_name,role,uidvalidity,message_count,unread_count,"
            "generation,selectable "
            "FROM folders WHERE account_id=? AND server_name=?", error) ||
        !bind_folder_key(query.value, 1, key, error)) return false;
    const int code = sqlite3_step(query.value);
    if (code == SQLITE_DONE) return set_error(error, "folder was not found");
    if (code != SQLITE_ROW) return set_error(error, "cannot read folder");
    Folder next;
    next.key.account_id = column_u64(query.value, 0);
    if (!column_text(query.value, 1, next.key.server_name) ||
        !column_text(query.value, 2, next.display_name)) return set_error(error, "folder data is invalid");
    next.role = static_cast<FolderRole>(sqlite3_column_int(query.value, 3));
    next.uidvalidity = column_u64(query.value, 4);
    next.message_count = static_cast<uint32_t>(sqlite3_column_int64(query.value, 5));
    next.unread_count = static_cast<uint32_t>(sqlite3_column_int64(query.value, 6));
    next.generation = column_u64(query.value, 7);
    if (!column_bool(query.value, 8, next.selectable)) return set_error(error, "folder selectability is invalid");
    folder = static_cast<Folder &&>(next);
    return true;
}

bool Store::list_folders(Id account_id, FolderList &folders, Text &error) {
    if (!database_ || !account_id) return set_error(error, "account id is invalid");
    FolderList next;
    Statement query;
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    if (!prepare(db, query, "SELECT account_id,server_name,display_name,role,uidvalidity,message_count,unread_count,"
            "generation,selectable "
            "FROM folders WHERE account_id=? ORDER BY server_name", error) ||
        !bind_i64(query.value, 1, account_id, error)) return false;
    int code;
    while ((code = sqlite3_step(query.value)) == SQLITE_ROW) {
        Folder item;
        item.key.account_id = column_u64(query.value, 0);
        if (!column_text(query.value, 1, item.key.server_name) || !column_text(query.value, 2, item.display_name))
            return set_error(error, "folder data is invalid");
        item.role = static_cast<FolderRole>(sqlite3_column_int(query.value, 3));
        item.uidvalidity = column_u64(query.value, 4);
        item.message_count = static_cast<uint32_t>(sqlite3_column_int64(query.value, 5));
        item.unread_count = static_cast<uint32_t>(sqlite3_column_int64(query.value, 6));
        item.generation = column_u64(query.value, 7);
        if (!column_bool(query.value, 8, item.selectable)) return set_error(error, "folder selectability is invalid");
        if (!next.push(static_cast<Folder &&>(item))) return set_error(error, "folder listing exceeds its limit");
    }
    if (code != SQLITE_DONE) return set_error(error, "cannot list folders");
    folders = static_cast<FolderList &&>(next);
    return true;
}

bool Store::delete_folder(const FolderKey &key, Text &error) {
    return delete_folder_guarded(key, 0, 0, error);
}

bool Store::delete_folder_guarded(const FolderKey &key, uint64_t account_generation,
    uint64_t folder_generation, Text &error) {
    if (!database_ || !valid_folder_key(key)) return set_error(error, "folder key is invalid");
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    Array<Text> files;
    if (!transaction_begin(db, error)) return false;
    if (!account_generation_matches(db, key.account_id, account_generation, error) ||
        (folder_generation && !folder_generation_matches(db, key, folder_generation, 0, error))) {
        transaction_rollback(db);
        return false;
    }
    Statement list;
    if (!prepare(db, list, "SELECT cache_name FROM messages WHERE account_id=? AND server_name=?", error) ||
        !bind_folder_key(list.value, 1, key, error)) { transaction_rollback(db); return false; }
    int code;
    while ((code = sqlite3_step(list.value)) == SQLITE_ROW)
        if (!append_text(list.value, 0, files, error)) { transaction_rollback(db); return false; }
    if (code != SQLITE_DONE) { transaction_rollback(db); return set_error(error, "cannot read folder cache references"); }
    Statement remove;
    if (!prepare(db, remove, "DELETE FROM folders WHERE account_id=? AND server_name=?", error) ||
        !bind_folder_key(remove.value, 1, key, error) || sqlite3_step(remove.value) != SQLITE_DONE ||
        sqlite3_changes(db) != 1) {
        transaction_rollback(db);
        return set_error(error, "cannot delete folder");
    }
    if (!transaction_commit(db, error)) return false;
    delete_cache_names(cache_fd_, files);
    return true;
}

bool Store::save_message(const MessageSummary &summary, const Bytes &raw_message, Text &error) {
    return save_message_guarded(summary, raw_message, 0, 0, error);
}

bool Store::save_header_message_guarded(const MessageSummary &summary, const Bytes &header_bytes,
    uint64_t account_generation, uint64_t folder_generation, Text &error) {
    MessageSummary header = summary;
    header.headers_only = true;
    return save_message_guarded(header, header_bytes, account_generation, folder_generation, error);
}

bool Store::save_message_guarded(const MessageSummary &summary, const Bytes &raw_message,
    uint64_t account_generation, uint64_t folder_generation, Text &error) {
    return save_message_guarded(summary, raw_message, account_generation, folder_generation,
        AttachmentPresence::Unknown, error);
}

bool Store::save_message_guarded(const MessageSummary &summary, const Bytes &raw_message,
    uint64_t account_generation, uint64_t folder_generation,
    AttachmentPresence attachment_presence, Text &error) {
    if (!database_ || !valid_folder_key(summary.key.folder) || !summary.key.uidvalidity ||
        !summary.key.uid || raw_message.size() > MaxRawMessageBytes ||
        !valid_text(summary.from) || !valid_text(summary.subject) ||
        static_cast<uint8_t>(attachment_presence) > static_cast<uint8_t>(AttachmentPresence::Some) ||
        (summary.headers_only && attachment_presence != AttachmentPresence::Unknown))
        return set_error(error, "message data is invalid or exceeds its limit");
    Text cache_name;
    if (!make_random_name("msg-", ".bin", cache_name, error)) return false;
    if (!add_cache_file(cache_fd_, cache_name, raw_message.data(), raw_message.size(), error)) return false;

    sqlite3 *db = static_cast<sqlite3 *>(database_);
    Array<Text> old_files;
    if (!transaction_begin(db, error)) { unlink_cache_file(cache_fd_, cache_name); return false; }
    uint64_t uidvalidity = 0;
    bool found = false;
    if (!account_generation_matches(db, summary.key.folder.account_id, account_generation, error) ||
        (folder_generation && !folder_generation_matches(db, summary.key.folder, folder_generation,
            summary.key.uidvalidity, error)) ||
        !query_folder_uidvalidity(db, summary.key.folder, uidvalidity, found, error) ||
        !found || uidvalidity != summary.key.uidvalidity) {
        transaction_rollback(db);
        unlink_cache_file(cache_fd_, cache_name);
        return set_error(error, "message folder UIDVALIDITY changed");
    }
    Statement old;
    if (!prepare(db, old, "SELECT cache_name,headers_only FROM messages WHERE account_id=? AND server_name=? AND uidvalidity=? AND uid=?",
            error) || !bind_message_key(old.value, 1, summary.key, error)) {
        transaction_rollback(db);
        unlink_cache_file(cache_fd_, cache_name);
        return false;
    }
    const int old_code = sqlite3_step(old.value);
    bool keep_full_cache = false;
    if (old_code == SQLITE_ROW) {
        Text old_name;
        if (!column_text(old.value, 0, old_name) || !old_files.push(static_cast<Text &&>(old_name))) {
            transaction_rollback(db);
            unlink_cache_file(cache_fd_, cache_name);
            return set_error(error, "out of memory updating message cache");
        }
        bool existing_headers_only = true;
        if (!column_bool(old.value, 1, existing_headers_only)) {
            transaction_rollback(db);
            unlink_cache_file(cache_fd_, cache_name);
            return set_error(error, "cached message completeness marker is invalid");
        }
        keep_full_cache = summary.headers_only && !existing_headers_only;
    } else if (old_code != SQLITE_DONE) {
        transaction_rollback(db);
        unlink_cache_file(cache_fd_, cache_name);
        return set_error(error, "cannot inspect previous message cache");
    } else {
        Statement count;
        if (!prepare(db, count, "SELECT COUNT(*) FROM messages WHERE account_id=? AND server_name=?", error) ||
            !bind_folder_key(count.value, 1, summary.key.folder, error) || sqlite3_step(count.value) != SQLITE_ROW) {
            transaction_rollback(db);
            unlink_cache_file(cache_fd_, cache_name);
            return set_error(error, "cannot count indexed folder messages");
        }
        if (sqlite3_column_int64(count.value, 0) >= static_cast<sqlite3_int64>(MaxIndexedMessagesPerFolder)) {
            transaction_rollback(db);
            unlink_cache_file(cache_fd_, cache_name);
            return set_error(error, "indexed message limit reached");
        }
    }
    Statement save;
    const char *sql = keep_full_cache ?
        "UPDATE messages SET from_text=?,subject=?,internal_date=?,size=?,flags=? WHERE account_id=? AND server_name=? AND uidvalidity=? AND uid=?" :
        "INSERT INTO messages(account_id,server_name,uidvalidity,uid,from_text,subject,internal_date,size,flags,headers_only,cache_name,attachment_presence)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(account_id,server_name,uidvalidity,uid) DO UPDATE SET "
        "from_text=excluded.from_text,subject=excluded.subject,internal_date=excluded.internal_date,"
        "size=excluded.size,flags=excluded.flags,headers_only=excluded.headers_only,cache_name=excluded.cache_name,"
        "attachment_presence=excluded.attachment_presence";
    const bool prepared = prepare(db, save, sql, error);
    const bool bound = prepared && (keep_full_cache ?
        (bind_text(save.value, 1, summary.from, error) && bind_text(save.value, 2, summary.subject, error) &&
         bind_signed(save.value, 3, summary.internal_date, error) && bind_i64(save.value, 4, summary.size, error) &&
         bind_i32(save.value, 5, summary.flags, error) && bind_message_key(save.value, 6, summary.key, error)) :
        (bind_message_key(save.value, 1, summary.key, error) && bind_text(save.value, 5, summary.from, error) &&
         bind_text(save.value, 6, summary.subject, error) && bind_signed(save.value, 7, summary.internal_date, error) &&
         bind_i64(save.value, 8, summary.size, error) && bind_i32(save.value, 9, summary.flags, error) &&
         bind_bool(save.value, 10, summary.headers_only, error) && bind_text(save.value, 11, cache_name, error) &&
         bind_i32(save.value, 12, static_cast<int32_t>(attachment_presence), error)));
    if (!bound || sqlite3_step(save.value) != SQLITE_DONE) {
        transaction_rollback(db);
        unlink_cache_file(cache_fd_, cache_name);
        if (error.empty()) set_error(error, "cannot save message index");
        return false;
    }
    if (!transaction_commit(db, error)) {
        unlink_cache_file(cache_fd_, cache_name);
        return false;
    }
    if (keep_full_cache) unlink_cache_file(cache_fd_, cache_name);
    else delete_cache_names(cache_fd_, old_files);
    return true;
}

bool Store::set_message_attachment_presence_guarded(const MessageKey &key,
    AttachmentPresence attachment_presence, uint64_t account_generation,
    uint64_t folder_generation, Text &error) {
    if (!database_ || !valid_folder_key(key.folder) || !key.uidvalidity || !key.uid ||
        !account_generation || !folder_generation ||
        static_cast<uint8_t>(attachment_presence) > static_cast<uint8_t>(AttachmentPresence::Some))
        return set_error(error, "message attachment coverage identity is invalid");
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    if (!transaction_begin(db, error)) return false;
    if (!account_generation_matches(db, key.folder.account_id, account_generation, error) ||
        !folder_generation_matches(db, key.folder, folder_generation, key.uidvalidity, error)) {
        transaction_rollback(db);
        return false;
    }
    Statement update;
    if (!prepare(db, update, "UPDATE messages SET attachment_presence=? WHERE account_id=? AND server_name=? "
            "AND uidvalidity=? AND uid=? AND headers_only=0", error) ||
        !bind_i32(update.value, 1, static_cast<int32_t>(attachment_presence), error) ||
        !bind_message_key(update.value, 2, key, error) || sqlite3_step(update.value) != SQLITE_DONE ||
        sqlite3_changes(db) != 1) {
        transaction_rollback(db);
        if (error.empty()) set_error(error, "full cached message changed before attachment parsing completed");
        return false;
    }
    return transaction_commit(db, error);
}

bool Store::load_message(const MessageKey &key, MessageSummary &summary, Bytes &raw_message, Text &error) {
    if (!database_ || !valid_folder_key(key.folder) || !key.uidvalidity || !key.uid)
        return set_error(error, "message key is invalid");
    char sql[512];
    const int length = snprintf(sql, sizeof(sql), "%s WHERE account_id=? AND server_name=? AND uidvalidity=? AND uid=?",
        MessageSelect);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(sql)) return set_error(error, "message query is too long");
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    Statement query;
    if (!prepare(db, query, sql, error) || !bind_message_key(query.value, 1, key, error)) return false;
    const int code = sqlite3_step(query.value);
    if (code == SQLITE_DONE) return set_error(error, "message was not found in the current cache");
    if (code != SQLITE_ROW) return set_error(error, "cannot read message index");
    MessageSummary next;
    Text cache_name;
    if (!load_message_summary(query.value, next) || !column_text(query.value, 10, cache_name))
        return set_error(error, "message index is invalid");
    Bytes next_raw;
    if (!read_cache_file(cache_fd_, cache_name, MaxRawMessageBytes, next_raw, error)) return false;
    summary = static_cast<MessageSummary &&>(next);
    raw_message = static_cast<Bytes &&>(next_raw);
    return true;
}

bool Store::list_messages(const FolderKey &folder, size_t offset, MessagePage &messages, Text &error) {
    if (!database_ || !valid_folder_key(folder) || offset > static_cast<size_t>(INT64_MAX))
        return set_error(error, "message page request is invalid");
    MessagePage next;
    Statement query;
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    const char *sql =
        "SELECT account_id,server_name,uidvalidity,uid,from_text,subject,internal_date,size,flags,headers_only "
        "FROM messages WHERE account_id=? AND server_name=? ORDER BY internal_date DESC,uid DESC LIMIT ? OFFSET ?";
    if (!prepare(db, query, sql, error) || !bind_folder_key(query.value, 1, folder, error) ||
        !bind_i32(query.value, 3, static_cast<uint32_t>(MaxRowsPerPage), error) ||
        !sqlite_ok(sqlite3_bind_int64(query.value, 4, static_cast<sqlite3_int64>(offset)),
            error, "cannot bind message page offset")) return false;
    int code;
    while ((code = sqlite3_step(query.value)) == SQLITE_ROW) {
        MessageSummary item;
        if (!load_message_summary(query.value, item) || !next.push(static_cast<MessageSummary &&>(item)))
            return set_error(error, "message page exceeds its limit or contains invalid data");
    }
    if (code != SQLITE_DONE) return set_error(error, "cannot list messages");
    messages = static_cast<MessagePage &&>(next);
    return true;
}

bool make_search_pattern(const char *query, Text &pattern, Text &error) {
    if (!query) return set_error(error, "search query is missing");
    const size_t size = strlen(query);
    if (size > 1024 || !valid_utf8(query, size)) return set_error(error, "search query is invalid");
    Array<char> result;
    if (!result.push('%')) return set_error(error, "out of memory");
    for (size_t i = 0; i < size; ++i) {
        if (query[i] == '%' || query[i] == '_' || query[i] == '\\')
            if (!result.push('\\')) return set_error(error, "out of memory");
        if (!result.push(query[i])) return set_error(error, "out of memory");
    }
    if (!result.push('%')) return set_error(error, "out of memory");
    return pattern.set(result.data(), result.size()) || set_error(error, "out of memory");
}

bool Store::search_messages(const FolderKey &folder, const char *query_text, size_t offset,
    MessagePage &messages, Text &error) {
    if (!database_ || !valid_folder_key(folder) || offset > static_cast<size_t>(INT64_MAX))
        return set_error(error, "message search request is invalid");
    Text pattern;
    if (!make_search_pattern(query_text, pattern, error)) return false;
    MessagePage next;
    Statement query;
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    const char *sql =
        "SELECT account_id,server_name,uidvalidity,uid,from_text,subject,internal_date,size,flags,headers_only "
        "FROM messages WHERE account_id=? AND server_name=? AND (from_text LIKE ? ESCAPE '\\' OR subject LIKE ? ESCAPE '\\') "
        "ORDER BY internal_date DESC,uid DESC LIMIT ? OFFSET ?";
    if (!prepare(db, query, sql, error) || !bind_folder_key(query.value, 1, folder, error) ||
        !bind_text(query.value, 3, pattern, error) || !bind_text(query.value, 4, pattern, error) ||
        !bind_i32(query.value, 5, static_cast<uint32_t>(MaxRowsPerPage), error) ||
        !sqlite_ok(sqlite3_bind_int64(query.value, 6, static_cast<sqlite3_int64>(offset)),
            error, "cannot bind search page offset")) return false;
    int code;
    while ((code = sqlite3_step(query.value)) == SQLITE_ROW) {
        MessageSummary item;
        if (!load_message_summary(query.value, item) || !next.push(static_cast<MessageSummary &&>(item)))
            return set_error(error, "message search page exceeds its limit");
    }
    if (code != SQLITE_DONE) return set_error(error, "cannot search cached messages");
    messages = static_cast<MessagePage &&>(next);
    return true;
}

size_t Store::message_count(const FolderKey &folder, Text &error) {
    if (!database_ || !valid_folder_key(folder)) { set_error(error, "folder key is invalid"); return 0; }
    Statement query;
    if (!prepare(static_cast<sqlite3 *>(database_), query,
            "SELECT COUNT(*) FROM messages WHERE account_id=? AND server_name=?", error) ||
        !bind_folder_key(query.value, 1, folder, error) || sqlite3_step(query.value) != SQLITE_ROW) {
        set_error(error, "cannot count cached messages");
        return 0;
    }
    const sqlite3_int64 count = sqlite3_column_int64(query.value, 0);
    return count < 0 ? 0 : static_cast<size_t>(count);
}

bool Store::account_message_count(Id account_id, size_t &count, Text &error) {
    error.clear(); count = 0;
    if (!database_ || !account_id) return set_error(error, "account id is invalid");
    Statement query;
    if (!prepare(static_cast<sqlite3 *>(database_), query,
            "SELECT COUNT(*) FROM messages WHERE account_id=?", error) ||
        !bind_i64(query.value, 1, account_id, error) || sqlite3_step(query.value) != SQLITE_ROW)
        return set_error(error, "cannot count account messages");
    const sqlite3_int64 value = sqlite3_column_int64(query.value, 0);
    if (value < 0 || static_cast<uint64_t>(value) > SIZE_MAX) return set_error(error, "account message count is invalid");
    count = static_cast<size_t>(value);
    return true;
}

bool Store::query_message_page(const MessageQuerySpec &spec, MessageQueryPage &page, Text &error) {
    error.clear();
    if (!database_ || static_cast<uint8_t>(spec.scope) > static_cast<uint8_t>(MessageQueryScope::AllMail) ||
        static_cast<uint8_t>(spec.sort) > static_cast<uint8_t>(MessageQuerySort::Subject) ||
        static_cast<uint8_t>(spec.search_field) > static_cast<uint8_t>(MessageQueryField::MessageId) ||
        static_cast<uint8_t>(spec.attachment_filter) > static_cast<uint8_t>(AttachmentQueryFilter::Unknown) ||
        !spec.limit || spec.limit > MaxRowsPerPage || spec.offset > static_cast<uint64_t>(INT64_MAX) ||
        (spec.has_received_after && spec.has_received_before && spec.received_after > spec.received_before) ||
        !valid_text(spec.query, 1024) ||
        (spec.scope == MessageQueryScope::Folder && !valid_folder_key(spec.folder)) ||
        (spec.scope == MessageQueryScope::Account && !spec.account_id) ||
        ((spec.scope == MessageQueryScope::Unified || spec.scope == MessageQueryScope::AllMail) && spec.account_id))
        return set_error(error, "message query is invalid or exceeds its page limit");
    const char *scope_where = "";
    if (spec.scope == MessageQueryScope::Folder)
        scope_where = " WHERE m.account_id=? AND m.server_name=? AND (m.flags & ?)=? AND (m.flags & ?)=0";
    else if (spec.scope == MessageQueryScope::Account)
        scope_where = " WHERE m.account_id=? AND (m.flags & ?)=? AND (m.flags & ?)=0";
    else if (spec.scope == MessageQueryScope::Unified)
        scope_where = " WHERE ((a.inbox_folder<>'' AND m.server_name=a.inbox_folder) "
            "OR (a.inbox_folder='' AND f.role=?)) AND (m.flags & ?)=? AND (m.flags & ?)=0";
    else
        scope_where = " WHERE (m.flags & ?)=? AND (m.flags & ?)=0";
    char where[768];
    const int where_length = snprintf(where, sizeof(where),
        "%s AND (?=0 OR m.internal_date>=?) AND (?=0 OR m.internal_date<=?) "
        "AND (?=0 OR m.attachment_presence=?)", scope_where);
    if (where_length < 0 || static_cast<size_t>(where_length) >= sizeof(where))
        return set_error(error, "message query filters are too long");
    const char *order = spec.sort == MessageQuerySort::Oldest ?
        " ORDER BY m.internal_date ASC,m.uid ASC" : spec.sort == MessageQuerySort::Sender ?
        " ORDER BY m.from_text COLLATE NOCASE,m.internal_date DESC,m.uid DESC" :
        spec.sort == MessageQuerySort::Subject ?
        " ORDER BY m.subject COLLATE NOCASE,m.internal_date DESC,m.uid DESC" :
        " ORDER BY m.internal_date DESC,m.uid DESC";
    char sql[2048];
    const int length = snprintf(sql, sizeof(sql),
        "SELECT m.account_id,m.server_name,m.uidvalidity,m.uid,m.from_text,m.subject,m.internal_date,"
        "m.size,m.flags,m.headers_only,m.cache_name,f.display_name,a.config_generation,f.generation,"
        "m.attachment_presence FROM messages m "
        "JOIN folders f ON f.account_id=m.account_id AND f.server_name=m.server_name "
        "JOIN accounts a ON a.id=m.account_id%s%s", where, order);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(sql)) return set_error(error, "message query is too long");
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    Statement query;
    if (!prepare(db, query, sql, error)) return false;
    int bind = 1;
    if (spec.scope == MessageQueryScope::Folder) {
        if (!bind_i64(query.value, bind++, spec.folder.account_id, error) ||
            !bind_text(query.value, bind++, spec.folder.server_name, error)) return false;
    } else if (spec.scope == MessageQueryScope::Unified) {
        if (!bind_i32(query.value, bind++, static_cast<uint32_t>(FolderRole::Inbox), error)) return false;
    } else if (spec.scope == MessageQueryScope::Account) {
        if (!bind_i64(query.value, bind++, spec.account_id, error)) return false;
    }
    if (!bind_i32(query.value, bind++, spec.required_flags, error) ||
        !bind_i32(query.value, bind++, spec.required_flags, error) ||
        !bind_i32(query.value, bind++, spec.excluded_flags, error) ||
        !bind_i32(query.value, bind++, spec.has_received_after ? 1 : 0, error) ||
        !bind_signed(query.value, bind++, spec.received_after, error) ||
        !bind_i32(query.value, bind++, spec.has_received_before ? 1 : 0, error) ||
        !bind_signed(query.value, bind++, spec.received_before, error)) return false;
    int attachment_state = 0;
    switch (spec.attachment_filter) {
    case AttachmentQueryFilter::Any: break;
    case AttachmentQueryFilter::HasAttachments: attachment_state = static_cast<int>(AttachmentPresence::Some); break;
    case AttachmentQueryFilter::NoAttachments: attachment_state = static_cast<int>(AttachmentPresence::None); break;
    case AttachmentQueryFilter::Unknown: attachment_state = static_cast<int>(AttachmentPresence::Unknown); break;
    }
    if (!bind_i32(query.value, bind++, spec.attachment_filter == AttachmentQueryFilter::Any ? 0 : 1, error) ||
        !bind_i32(query.value, bind++, attachment_state, error)) return false;
    MessageQueryPage next;
    next.query = spec;
    const bool searching = !spec.query.empty();
    uint64_t matches = 0;
    int code;
    while ((code = sqlite3_step(query.value)) == SQLITE_ROW) {
        MessageSummary summary;
        Text cache_name, display_name;
        if (!load_message_summary(query.value, summary) || !column_text(query.value, 10, cache_name) ||
            !column_text(query.value, 11, display_name)) return set_error(error, "message query returned invalid cache metadata");
        const uint64_t account_generation = column_u64(query.value, 12);
        const uint64_t folder_generation = column_u64(query.value, 13);
        const sqlite3_int64 presence_value = sqlite3_column_int64(query.value, 14);
        if (!account_generation || !folder_generation || presence_value < 0 || presence_value > 2)
            return set_error(error, "message query returned invalid generation or attachment metadata");
        if (summary.headers_only) ++next.headers_only_count;
        else ++next.downloaded_body_count;
        bool matched = !searching;
        if (searching) {
            const bool from_match = (spec.search_field == MessageQueryField::All || spec.search_field == MessageQueryField::From) &&
                text_contains_ascii_case_insensitive(summary.from, spec.query);
            const bool subject_match = (spec.search_field == MessageQueryField::All || spec.search_field == MessageQueryField::Subject) &&
                text_contains_ascii_case_insensitive(summary.subject, spec.query);
            matched = from_match || subject_match;
            const bool need_mime = spec.search_field == MessageQueryField::Body ||
                spec.search_field == MessageQueryField::MessageId || spec.search_field == MessageQueryField::All;
            if (!matched && need_mime) {
                Bytes raw;
                if (!read_cache_file(cache_fd_, cache_name, MaxRawMessageBytes, raw, error)) return false;
                ParsedMessage parsed;
                Text parse_error;
                if (!parse_message(reinterpret_cast<const char *>(raw.data()), raw.size(), parsed, parse_error)) {
                    if (next.diagnostic.empty()) next.diagnostic.set("Some cached messages could not be parsed for this search.");
                } else if (spec.search_field == MessageQueryField::Body) {
                    matched = !summary.headers_only &&
                        (text_contains_ascii_case_insensitive(parsed.plain_text, spec.query) ||
                         text_contains_ascii_case_insensitive(parsed.html_text, spec.query));
                } else if (spec.search_field == MessageQueryField::MessageId) {
                    matched = text_contains_ascii_case_insensitive(parsed.message_id, spec.query);
                } else if (spec.search_field == MessageQueryField::All) {
                    matched = text_contains_ascii_case_insensitive(parsed.message_id, spec.query) ||
                        (!summary.headers_only && (text_contains_ascii_case_insensitive(parsed.plain_text, spec.query) ||
                         text_contains_ascii_case_insensitive(parsed.html_text, spec.query)));
                }
            }
        }
        if (!matched) continue;
        if (matches >= spec.offset && matches - spec.offset < spec.limit) {
            MessageQueryRow row;
            row.message = static_cast<MessageSummary &&>(summary);
            row.folder_display_name = static_cast<Text &&>(display_name);
            row.account_generation = account_generation;
            row.folder_generation = folder_generation;
            row.attachment_presence_known = presence_value != static_cast<sqlite3_int64>(AttachmentPresence::Unknown);
            row.has_attachments = presence_value == static_cast<sqlite3_int64>(AttachmentPresence::Some);
            if (!next.rows.push(static_cast<MessageQueryRow &&>(row))) return set_error(error, "message query page exceeds its bound");
        }
        if (matches == UINT64_MAX) return set_error(error, "message query result count overflow");
        ++matches;
    }
    if (code != SQLITE_DONE) return set_error(error, "cannot execute message query");
    next.total_matches = matches;
    next.has_more = spec.offset < matches && matches - spec.offset > next.rows.size();
    page = static_cast<MessageQueryPage &&>(next);
    return true;
}

bool Store::delete_message(const MessageKey &key, Text &error) {
    return delete_message_guarded(key, 0, 0, error);
}

bool Store::delete_message_guarded(const MessageKey &key, uint64_t account_generation,
    uint64_t folder_generation, Text &error) {
    if (!database_ || !valid_folder_key(key.folder) || !key.uidvalidity || !key.uid)
        return set_error(error, "message key is invalid");
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    Text cache_name;
    if (!transaction_begin(db, error)) return false;
    if (!account_generation_matches(db, key.folder.account_id, account_generation, error) ||
        (folder_generation && !folder_generation_matches(db, key.folder, folder_generation,
            key.uidvalidity, error))) {
        transaction_rollback(db);
        return false;
    }
    Statement lookup;
    if (!prepare(db, lookup, "SELECT cache_name FROM messages WHERE account_id=? AND server_name=? AND uidvalidity=? AND uid=?",
            error) || !bind_message_key(lookup.value, 1, key, error)) { transaction_rollback(db); return false; }
    const int lookup_code = sqlite3_step(lookup.value);
    if (lookup_code != SQLITE_ROW || !column_text(lookup.value, 0, cache_name)) {
        transaction_rollback(db);
        return set_error(error, "message was not found");
    }
    Statement remove;
    if (!prepare(db, remove, "DELETE FROM messages WHERE account_id=? AND server_name=? AND uidvalidity=? AND uid=?",
            error) || !bind_message_key(remove.value, 1, key, error) || sqlite3_step(remove.value) != SQLITE_DONE) {
        transaction_rollback(db);
        return set_error(error, "cannot delete cached message");
    }
    if (!transaction_commit(db, error)) return false;
    unlink_cache_file(cache_fd_, cache_name);
    return true;
}

bool Store::update_message_flags_guarded(const MessageKey &key, uint32_t add_flags,
    uint32_t remove_flags, uint64_t account_generation, uint64_t folder_generation, Text &error) {
    constexpr uint32_t KnownFlags = 0x1fu;
    if (!database_ || !valid_folder_key(key.folder) || !key.uidvalidity || !key.uid ||
        (add_flags & ~KnownFlags) || (remove_flags & ~KnownFlags) || (add_flags & remove_flags))
        return set_error(error, "message flag update is invalid");
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    if (!transaction_begin(db, error)) return false;
    if (!account_generation_matches(db, key.folder.account_id, account_generation, error) ||
        !folder_generation_matches(db, key.folder, folder_generation, key.uidvalidity, error)) {
        transaction_rollback(db);
        return false;
    }
    Statement update;
    if (!prepare(db, update, "UPDATE messages SET flags=(flags | ?) & ~? WHERE account_id=? AND server_name=? AND uidvalidity=? AND uid=?",
            error) || !bind_i32(update.value, 1, add_flags, error) || !bind_i32(update.value, 2, remove_flags, error) ||
        !bind_message_key(update.value, 3, key, error) || sqlite3_step(update.value) != SQLITE_DONE ||
        sqlite3_changes(db) != 1) {
        transaction_rollback(db);
        return set_error(error, "cannot update cached message flags");
    }
    return transaction_commit(db, error);
}

bool Store::save_draft(Draft &draft, Text &error) {
    if (draft.id || draft.revision)
        return set_error(error, "legacy save_draft accepts new drafts only; updates require a revision guard");
    return save_draft_guarded(draft, 0, 0, error);
}

bool Store::save_draft_guarded(Draft &draft, uint64_t account_generation,
    uint64_t expected_revision, Text &error) {
    error.clear();
    if ((draft.id && (!expected_revision || draft.revision != expected_revision)) ||
        (!draft.id && (expected_revision || draft.revision)))
        return set_error(error, "draft creation and revision-guarded update require distinct identities");
    if (!database_ || !draft.account_id || draft.account_id > static_cast<uint64_t>(INT64_MAX) ||
        !validate_draft(draft, error)) {
        if (error.empty()) set_error(error, "draft data is invalid");
        return false;
    }
    Array<Text> new_files;
    Array<Text> old_files;
    size_t total_attachment_bytes = 0;
    for (size_t i = 0; i < draft.attachments.size(); ++i) {
        const Attachment &attachment = draft.attachments[i];
        const size_t size = attachment.source.read ? attachment.source.size : attachment.data.size();
        if (size > DefaultMaxAttachmentBytes - total_attachment_bytes) {
            delete_cache_names(cache_fd_, new_files);
            return set_error(error, "draft attachments exceed the default size limit");
        }
        total_attachment_bytes += size;
        unsigned char *materialized = nullptr;
        const unsigned char *content = attachment.data.data();
        if (attachment.source.read) {
            materialized = size ? static_cast<unsigned char *>(malloc(size)) : nullptr;
            if (size && !materialized) {
                delete_cache_names(cache_fd_, new_files);
                return set_error(error, "out of memory materializing draft attachment");
            }
            size_t at = 0;
            bool read_ok = true;
            while (at < size) {
                const size_t request = size - at > 65536 ? 65536 : size - at;
                size_t read_size = 0;
                if (!attachment.source.read(attachment.source.context, at, materialized + at,
                        request, &read_size) || !read_size || read_size > request) {
                    read_ok = false;
                    break;
                }
                at += read_size;
            }
            if (!read_ok) {
                if (materialized) { detail::wipe_memory(materialized, size); free(materialized); }
                delete_cache_names(cache_fd_, new_files);
                return set_error(error, "cannot read draft attachment source");
            }
            content = materialized;
        }
        Text cache_name;
        if (!make_random_name("draft-", ".bin", cache_name, error) ||
            !add_cache_file(cache_fd_, cache_name, content, size, error)) {
            if (materialized) { detail::wipe_memory(materialized, size); free(materialized); }
            delete_cache_names(cache_fd_, new_files);
            return false;
        }
        if (materialized) { detail::wipe_memory(materialized, size); free(materialized); }
        if (!new_files.push(static_cast<Text &&>(cache_name))) {
            unlink_cache_file(cache_fd_, cache_name);
            delete_cache_names(cache_fd_, new_files);
            return set_error(error, "out of memory indexing draft attachments");
        }
    }

    sqlite3 *db = static_cast<sqlite3 *>(database_);
    if (!transaction_begin(db, error)) { delete_cache_names(cache_fd_, new_files); return false; }
    if (!account_generation_matches(db, draft.account_id, account_generation, error)) {
        transaction_rollback(db);
        delete_cache_names(cache_fd_, new_files);
        return false;
    }
    uint64_t new_revision = 1;
    if (draft.id) {
        Statement identity;
        sqlite3_int64 stored_revision = 0;
        if (!prepare(db, identity, "SELECT revision,account_id FROM drafts WHERE id=?", error) ||
            !bind_i64(identity.value, 1, draft.id, error) || sqlite3_step(identity.value) != SQLITE_ROW ||
            !column_i64(identity.value, 0, stored_revision) || stored_revision <= 0 ||
            static_cast<uint64_t>(sqlite3_column_int64(identity.value, 1)) != draft.account_id ||
            (expected_revision && static_cast<uint64_t>(stored_revision) != expected_revision) ||
            stored_revision == INT64_MAX) {
            transaction_rollback(db);
            delete_cache_names(cache_fd_, new_files);
            return set_error(error, "draft account or revision changed before save");
        }
        new_revision = static_cast<uint64_t>(stored_revision + 1);
        Statement old;
        if (!prepare(db, old, "SELECT cache_name FROM draft_attachments WHERE draft_id=?", error) ||
            !bind_i64(old.value, 1, draft.id, error)) {
            transaction_rollback(db);
            delete_cache_names(cache_fd_, new_files);
            return false;
        }
        int code;
        while ((code = sqlite3_step(old.value)) == SQLITE_ROW) {
            if (!append_text(old.value, 0, old_files, error)) {
                transaction_rollback(db);
                delete_cache_names(cache_fd_, new_files);
                return false;
            }
        }
        if (code != SQLITE_DONE) {
            transaction_rollback(db);
            delete_cache_names(cache_fd_, new_files);
            return set_error(error, "cannot inspect existing draft attachments");
        }
    }

    Statement save;
    const char *insert_sql = "INSERT INTO drafts(account_id,to_input,cc_input,bcc_input,raw_recipient_input,"
        "subject,body,html_body,editor_state,rich_format,message_id,in_reply_to,references_text,send_stage,revision) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
    const char *update_sql = "UPDATE drafts SET account_id=?,to_input=?,cc_input=?,bcc_input=?,"
        "raw_recipient_input=?,subject=?,body=?,html_body=?,editor_state=?,rich_format=?,message_id=?,"
        "in_reply_to=?,references_text=?,send_stage=?,revision=?,updated_at=unixepoch() WHERE id=?";
    if (!prepare(db, save, draft.id ? update_sql : insert_sql, error)) {
        transaction_rollback(db);
        delete_cache_names(cache_fd_, new_files);
        return false;
    }
    const int first = 1;
    if (!bind_i64(save.value, first, draft.account_id, error) ||
        !bind_text(save.value, first + 1, draft.to_input, error) ||
        !bind_text(save.value, first + 2, draft.cc_input, error) ||
        !bind_text(save.value, first + 3, draft.bcc_input, error) ||
        !bind_bool(save.value, first + 4, draft.raw_recipient_input, error) ||
        !bind_text(save.value, first + 5, draft.subject, error) ||
        !bind_text(save.value, first + 6, draft.body, error) ||
        !bind_text(save.value, first + 7, draft.html_body, error) ||
        !bind_blob(save.value, first + 8, draft.editor_state, error) ||
        !bind_bool(save.value, first + 9, draft.rich_format, error) ||
        !bind_text(save.value, first + 10, draft.message_id, error) ||
        !bind_text(save.value, first + 11, draft.in_reply_to, error) ||
        !bind_text(save.value, first + 12, draft.references, error) ||
        !bind_i32(save.value, first + 13, static_cast<uint32_t>(draft.send_stage), error) ||
        !bind_i64(save.value, first + 14, new_revision, error) ||
        (draft.id && !bind_i64(save.value, first + 15, draft.id, error)) ||
        sqlite3_step(save.value) != SQLITE_DONE) {
        transaction_rollback(db);
        delete_cache_names(cache_fd_, new_files);
        if (error.empty()) set_error(error, "cannot save draft");
        return false;
    }
    const uint64_t draft_id = draft.id ? draft.id : static_cast<uint64_t>(sqlite3_last_insert_rowid(db));
    if (draft.id && sqlite3_changes(db) != 1) {
        transaction_rollback(db);
        delete_cache_names(cache_fd_, new_files);
        return set_error(error, "draft was not found for update");
    }
    if (draft.id) {
        Statement clear_addresses;
        Statement clear_attachments;
        if (!prepare(db, clear_addresses, "DELETE FROM draft_addresses WHERE draft_id=?", error) ||
            !bind_i64(clear_addresses.value, 1, draft_id, error) ||
            sqlite3_step(clear_addresses.value) != SQLITE_DONE ||
            !prepare(db, clear_attachments, "DELETE FROM draft_attachments WHERE draft_id=?", error) ||
            !bind_i64(clear_attachments.value, 1, draft_id, error) ||
            sqlite3_step(clear_attachments.value) != SQLITE_DONE) {
            transaction_rollback(db);
            delete_cache_names(cache_fd_, new_files);
            if (error.empty()) set_error(error, "cannot replace draft details");
            return false;
        }
    }
    if (!insert_draft_addresses(db, draft_id, 0, draft.to, error) ||
        !insert_draft_addresses(db, draft_id, 1, draft.cc, error) ||
        !insert_draft_addresses(db, draft_id, 2, draft.bcc, error)) {
        transaction_rollback(db);
        delete_cache_names(cache_fd_, new_files);
        return false;
    }
    Statement insert_attachment;
    if (!prepare(db, insert_attachment, "INSERT INTO draft_attachments(draft_id,position,filename,content_type,"
            "content_id,inline_part,size,cache_name) VALUES(?,?,?,?,?,?,?,?)", error)) {
        transaction_rollback(db);
        delete_cache_names(cache_fd_, new_files);
        return false;
    }
    for (size_t i = 0; i < draft.attachments.size(); ++i) {
        const Attachment &attachment = draft.attachments[i];
        const size_t size = attachment.source.read ? attachment.source.size : attachment.data.size();
        if (!bind_i64(insert_attachment.value, 1, draft_id, error) ||
            !bind_i64(insert_attachment.value, 2, i, error) ||
            !bind_text(insert_attachment.value, 3, attachment.filename, error) ||
            !bind_text(insert_attachment.value, 4, attachment.content_type, error) ||
            !bind_text(insert_attachment.value, 5, attachment.content_id, error) ||
            !bind_bool(insert_attachment.value, 6, attachment.inline_part, error) ||
            !bind_i64(insert_attachment.value, 7, size, error) ||
            !bind_text(insert_attachment.value, 8, new_files[i], error) ||
            sqlite3_step(insert_attachment.value) != SQLITE_DONE) {
            transaction_rollback(db);
            delete_cache_names(cache_fd_, new_files);
            if (error.empty()) set_error(error, "cannot save draft attachment index");
            return false;
        }
        sqlite3_reset(insert_attachment.value);
        sqlite3_clear_bindings(insert_attachment.value);
    }
    if (!transaction_commit(db, error)) {
        delete_cache_names(cache_fd_, new_files);
        return false;
    }
    draft.id = draft_id;
    draft.revision = new_revision;
    delete_cache_names(cache_fd_, old_files);
    return true;
}

bool Store::get_draft(Id id, Draft &draft, Text &error) {
    error.clear();
    if (!database_ || !id) return set_error(error, "draft id is invalid");
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    Statement query;
    const char *sql = "SELECT id,account_id,to_input,cc_input,bcc_input,raw_recipient_input,subject,body,"
        "html_body,editor_state,rich_format,message_id,in_reply_to,references_text,send_stage,revision FROM drafts WHERE id=?";
    if (!prepare(db, query, sql, error) || !bind_i64(query.value, 1, id, error)) return false;
    const int code = sqlite3_step(query.value);
    if (code == SQLITE_DONE) return set_error(error, "draft was not found");
    if (code != SQLITE_ROW) return set_error(error, "cannot read draft");
    Draft next;
    if (!load_draft_record(query.value, next, error) || !load_draft_addresses(db, id, next, error) ||
        !load_draft_attachments(db, cache_fd_, id, next, error) || !validate_draft(next, error)) {
        release_draft_sources(next);
        return false;
    }
    draft = static_cast<Draft &&>(next);
    return true;
}

bool Store::list_drafts(Id account_id, DraftList &drafts, Text &error) {
    error.clear();
    if (!database_ || !account_id) return set_error(error, "account id is invalid");
    Array<uint64_t> ids;
    Statement query;
    if (!prepare(static_cast<sqlite3 *>(database_), query,
            "SELECT id FROM drafts WHERE account_id=? ORDER BY updated_at DESC,id DESC LIMIT ?", error) ||
        !bind_i64(query.value, 1, account_id, error) ||
        !bind_i32(query.value, 2, static_cast<uint32_t>(MaxRowsPerPage + 1), error)) return false;
    int code;
    while ((code = sqlite3_step(query.value)) == SQLITE_ROW) {
        const uint64_t id = column_u64(query.value, 0);
        if (!ids.push(id)) return set_error(error, "out of memory listing drafts");
    }
    if (code != SQLITE_DONE) return set_error(error, "cannot list drafts");
    if (ids.size() > MaxRowsPerPage) return set_error(error, "draft listing exceeds its page limit");
    DraftList next;
    for (size_t i = 0; i < ids.size(); ++i) {
        Draft item;
        if (!get_draft(ids[i], item, error)) {
            for (size_t j = 0; j < next.size(); ++j) release_draft_sources(next[j]);
            return false;
        }
        if (!next.push(static_cast<Draft &&>(item))) {
            release_draft_sources(item);
            for (size_t j = 0; j < next.size(); ++j) release_draft_sources(next[j]);
            return set_error(error, "draft listing exceeds its page limit");
        }
    }
    drafts = static_cast<DraftList &&>(next);
    return true;
}

void Store::release_draft_sources(Draft &draft) {
    for (size_t i = 0; i < draft.attachments.size(); ++i) {
        Attachment &attachment = draft.attachments[i];
        if (attachment.source.is_owned()) attachment.source.reset();
    }
}

bool Store::delete_draft(Id id, Text &error) {
    Draft snapshot;
    if (!get_draft(id, snapshot, error)) return false;
    const uint64_t revision = snapshot.revision;
    const uint64_t account_id = snapshot.account_id;
    release_draft_sources(snapshot);
    return delete_draft_guarded(id, account_id, 0, revision, error);
}

bool Store::delete_draft_guarded(Id id, uint64_t expected_account_id,
    uint64_t account_generation, uint64_t expected_revision, Text &error) {
    error.clear();
    if (!database_ || !id || !expected_account_id || !expected_revision)
        return set_error(error, "draft deletion requires account, ID and expected revision");
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    Array<Text> files;
    if (!transaction_begin(db, error)) return false;
    Statement identity;
    sqlite3_int64 revision = 0;
    uint64_t account_id = 0;
    if (!prepare(db, identity, "SELECT account_id,revision FROM drafts WHERE id=?", error) ||
        !bind_i64(identity.value, 1, id, error) || sqlite3_step(identity.value) != SQLITE_ROW ||
        !column_u64_checked(identity.value, 0, account_id) || !column_i64(identity.value, 1, revision) ||
        revision <= 0 || account_id != expected_account_id ||
        static_cast<uint64_t>(revision) != expected_revision ||
        !account_generation_matches(db, expected_account_id, account_generation, error)) {
        transaction_rollback(db);
        return set_error(error, "draft or account identity changed before delete");
    }
    Statement list;
    if (!prepare(db, list, "SELECT cache_name FROM draft_attachments WHERE draft_id=?", error) ||
        !bind_i64(list.value, 1, id, error)) { transaction_rollback(db); return false; }
    int code;
    while ((code = sqlite3_step(list.value)) == SQLITE_ROW)
        if (!append_text(list.value, 0, files, error)) { transaction_rollback(db); return false; }
    if (code != SQLITE_DONE) { transaction_rollback(db); return set_error(error, "cannot read draft attachment references"); }
    Statement remove;
    if (!prepare(db, remove, "DELETE FROM drafts WHERE id=?", error) ||
        !bind_i64(remove.value, 1, id, error) || sqlite3_step(remove.value) != SQLITE_DONE ||
        sqlite3_changes(db) != 1) {
        transaction_rollback(db);
        return set_error(error, "cannot delete draft");
    }
    if (!transaction_commit(db, error)) return false;
    delete_cache_names(cache_fd_, files);
    return true;
}

bool Store::set_send_stage(Id id, SendStage stage, Text &error) {
    error.clear();
    if (!database_ || !id || static_cast<unsigned>(stage) > static_cast<unsigned>(SendStage::ReviewedDelivered))
        return set_error(error, "send stage update is invalid");
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    if (!transaction_begin(db, error)) return false;
    Statement update;
    if (!prepare(db, update, "UPDATE drafts SET send_stage=?,updated_at=unixepoch() WHERE id=?", error) ||
        !bind_i32(update.value, 1, static_cast<uint32_t>(stage), error) ||
        !bind_i64(update.value, 2, id, error) || sqlite3_step(update.value) != SQLITE_DONE ||
        sqlite3_changes(db) != 1) {
        transaction_rollback(db);
        return set_error(error, "cannot update draft send stage");
    }
    return transaction_commit(db, error);
}

bool Store::recover_interrupted_sends(Text &error) {
    error.clear();
    if (!database_) return set_error(error, "profile store is closed");
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    if (!transaction_begin(db, error)) return false;
    const SendStage from[] = {SendStage::Connecting, SendStage::Authenticating, SendStage::SendingEnvelope,
        SendStage::SendingData, SendStage::AwaitingFinalResponse, SendStage::Sent, SendStage::Accepted};
    const SendStage to[] = {SendStage::Ready, SendStage::Ready, SendStage::Ready,
        SendStage::Uncertain, SendStage::Uncertain, SendStage::ArchivePending, SendStage::ArchivePending};
    const char *sql = "UPDATE drafts SET send_stage=CASE send_stage "
        "WHEN ? THEN ? WHEN ? THEN ? WHEN ? THEN ? WHEN ? THEN ? WHEN ? THEN ? WHEN ? THEN ? WHEN ? THEN ? "
        "ELSE send_stage END,updated_at=unixepoch() WHERE send_stage IN (?,?,?,?,?,?,?)";
    Statement update;
    if (!prepare(db, update, sql, error)) {
        transaction_rollback(db);
        return false;
    }
    for (size_t i = 0; i < sizeof(from) / sizeof(from[0]); ++i) {
        if (!bind_i32(update.value, static_cast<int>(i * 2 + 1), static_cast<uint32_t>(from[i]), error) ||
            !bind_i32(update.value, static_cast<int>(i * 2 + 2), static_cast<uint32_t>(to[i]), error) ||
            !bind_i32(update.value, static_cast<int>(15 + i), static_cast<uint32_t>(from[i]), error)) {
            transaction_rollback(db);
            return false;
        }
    }
    if (sqlite3_step(update.value) != SQLITE_DONE) {
        transaction_rollback(db);
        return set_error(error, "cannot recover interrupted sends");
    }
    Statement operations;
    const char *operation_sql = "UPDATE send_operations SET stage=CASE "
        "WHEN stage IN (?,?,?) THEN ? WHEN stage IN (?,?) THEN ? WHEN stage IN (?,?) THEN ? "
        "ELSE stage END WHERE stage IN (?,?,?,?,?,?,?)";
    if (!prepare(db, operations, operation_sql, error)) {
        transaction_rollback(db);
        return false;
    }
    const uint32_t params[] = {
        static_cast<uint32_t>(SendStage::Connecting), static_cast<uint32_t>(SendStage::Authenticating),
        static_cast<uint32_t>(SendStage::SendingEnvelope), static_cast<uint32_t>(SendStage::Ready),
        static_cast<uint32_t>(SendStage::SendingData), static_cast<uint32_t>(SendStage::AwaitingFinalResponse),
        static_cast<uint32_t>(SendStage::Uncertain), static_cast<uint32_t>(SendStage::Sent),
        static_cast<uint32_t>(SendStage::Accepted), static_cast<uint32_t>(SendStage::ArchivePending),
        static_cast<uint32_t>(SendStage::Connecting), static_cast<uint32_t>(SendStage::Authenticating),
        static_cast<uint32_t>(SendStage::SendingEnvelope), static_cast<uint32_t>(SendStage::SendingData),
        static_cast<uint32_t>(SendStage::AwaitingFinalResponse), static_cast<uint32_t>(SendStage::Sent),
        static_cast<uint32_t>(SendStage::Accepted)};
    for (size_t i = 0; i < sizeof(params) / sizeof(params[0]); ++i) {
        if (!bind_i32(operations.value, static_cast<int>(i + 1), params[i], error)) {
            transaction_rollback(db);
            return false;
        }
    }
    if (sqlite3_step(operations.value) != SQLITE_DONE) {
        transaction_rollback(db);
        return set_error(error, "cannot recover interrupted durable send operations");
    }
    return transaction_commit(db, error);
}

bool Store::create_send_operation(uint64_t account_id, uint64_t account_generation,
    uint64_t draft_id, uint64_t draft_revision, const Text &message_id,
    const Text &sender, const Text &recipients, const Text &archive_folder,
    const Bytes &spool, SendOperation &operation, Text &error) {
    error.clear();
    if (!database_ || !account_id || !account_generation || !draft_id || !draft_revision ||
        !valid_text(message_id, 998) || message_id.empty() || !valid_text(sender, 4096) || sender.empty() ||
        !valid_text(recipients, MaxEditableTextBytes) || recipients.empty() ||
        !valid_text(archive_folder, 4096) || spool.empty() || spool.size() > MaxRawMessageBytes)
        return set_error(error, "durable send operation identity or spool is invalid");
    Text spool_name;
    if (!make_random_name("send-", ".bin", spool_name, error) ||
        !add_cache_file(cache_fd_, spool_name, spool.data(), spool.size(), error)) return false;
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    if (!transaction_begin(db, error)) { unlink_cache_file(cache_fd_, spool_name); return false; }
    Statement draft;
    sqlite3_int64 stored_revision = 0;
    if (!account_generation_matches(db, account_id, account_generation, error) ||
        !prepare(db, draft, "SELECT revision FROM drafts WHERE id=? AND account_id=?", error) ||
        !bind_i64(draft.value, 1, draft_id, error) || !bind_i64(draft.value, 2, account_id, error) ||
        sqlite3_step(draft.value) != SQLITE_ROW || !column_i64(draft.value, 0, stored_revision) ||
        stored_revision <= 0 || static_cast<uint64_t>(stored_revision) != draft_revision) {
        transaction_rollback(db);
        unlink_cache_file(cache_fd_, spool_name);
        return set_error(error, "account or draft revision changed before queueing the send");
    }
    Statement insert;
    const char *sql = "INSERT INTO send_operations(account_id,account_generation,draft_id,draft_revision,"
        "message_id,sender,recipients,spool_name,spool_size,stage,delivery_count,archive_folder) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,0,?)";
    if (!prepare(db, insert, sql, error) || !bind_i64(insert.value, 1, account_id, error) ||
        !bind_i64(insert.value, 2, account_generation, error) || !bind_i64(insert.value, 3, draft_id, error) ||
        !bind_i64(insert.value, 4, draft_revision, error) || !bind_text(insert.value, 5, message_id, error) ||
        !bind_text(insert.value, 6, sender, error) || !bind_text(insert.value, 7, recipients, error) ||
        !bind_text(insert.value, 8, spool_name, error) || !bind_i64(insert.value, 9, spool.size(), error) ||
        !bind_i32(insert.value, 10, static_cast<uint32_t>(SendStage::Ready), error) ||
        !bind_text(insert.value, 11, archive_folder, error) || sqlite3_step(insert.value) != SQLITE_DONE) {
        transaction_rollback(db);
        unlink_cache_file(cache_fd_, spool_name);
        if (error.empty()) set_error(error, "cannot persist durable send operation");
        return false;
    }
    const uint64_t id = static_cast<uint64_t>(sqlite3_last_insert_rowid(db));
    if (!id || !transaction_commit(db, error)) {
        unlink_cache_file(cache_fd_, spool_name);
        return false;
    }
    SendOperation next;
    next.id = id; next.account_id = account_id; next.account_generation = account_generation;
    next.draft_id = draft_id; next.draft_revision = draft_revision; next.message_id = message_id;
    next.sender = sender; next.recipients = recipients; next.spool_name = spool_name;
    next.spool_size = spool.size(); next.stage = SendStage::Ready; next.archive_folder = archive_folder;
    operation = static_cast<SendOperation &&>(next);
    return true;
}

bool Store::find_send_operation_for_draft(uint64_t account_id, uint64_t draft_id,
    uint64_t draft_revision, bool &found, SendOperation &operation, Bytes &spool, Text &error) {
    error.clear();
    found = false;
    if (!database_ || !account_id || !draft_id || !draft_revision)
        return set_error(error, "send intent lookup identity is invalid");
    Statement query;
    if (!prepare(static_cast<sqlite3 *>(database_), query,
            "SELECT id FROM send_operations WHERE account_id=? AND draft_id=? AND draft_revision=?", error) ||
        !bind_i64(query.value, 1, account_id, error) || !bind_i64(query.value, 2, draft_id, error) ||
        !bind_i64(query.value, 3, draft_revision, error)) return false;
    const int code = sqlite3_step(query.value);
    if (code == SQLITE_DONE) return true;
    uint64_t id = 0;
    if (code != SQLITE_ROW || !column_u64_checked(query.value, 0, id) || !id)
        return set_error(error, "cannot read queued send intent");
    if (!get_send_operation(id, operation, spool, error)) return false;
    if (operation.account_id != account_id || operation.draft_id != draft_id ||
        operation.draft_revision != draft_revision) return set_error(error, "queued send intent identity is inconsistent");
    found = true;
    return true;
}

bool Store::get_send_operation(Id id, SendOperation &operation, Bytes &spool, Text &error) {
    error.clear();
    if (!database_ || !id) return set_error(error, "send operation id is invalid");
    Statement query;
    if (!prepare(static_cast<sqlite3 *>(database_), query,
            "SELECT id,account_id,account_generation,draft_id,draft_revision,message_id,sender,recipients,"
            "spool_name,spool_size,stage,delivery_count,archive_folder,archive_review_generation,"
            "archive_folder_generation,archive_folder_uidvalidity,last_diagnostic "
            "FROM send_operations WHERE id=?", error) ||
        !bind_i64(query.value, 1, id, error)) return false;
    if (sqlite3_step(query.value) != SQLITE_ROW) return set_error(error, "send operation was not found");
    SendOperation next;
    uint32_t stage = 0;
    if (!column_u64_checked(query.value, 0, next.id) || !column_u64_checked(query.value, 1, next.account_id) ||
        !column_u64_checked(query.value, 2, next.account_generation) || !column_u64_checked(query.value, 3, next.draft_id) ||
        !column_u64_checked(query.value, 4, next.draft_revision) || !column_text(query.value, 5, next.message_id) ||
        !column_text(query.value, 6, next.sender) || !column_text(query.value, 7, next.recipients) ||
        !column_text(query.value, 8, next.spool_name) || !column_u64_checked(query.value, 9, next.spool_size) ||
        !column_u32_checked(query.value, 10, stage) || stage > static_cast<uint32_t>(SendStage::ReviewedDelivered) ||
        !column_u32_checked(query.value, 11, next.delivery_count) || !column_text(query.value, 12, next.archive_folder) ||
        !column_u64_checked(query.value, 13, next.archive_review_generation) ||
        !column_u64_checked(query.value, 14, next.archive_folder_generation) ||
        !column_u64_checked(query.value, 15, next.archive_folder_uidvalidity) ||
        !column_text(query.value, 16, next.last_diagnostic) || !valid_text(next.last_diagnostic, 4096) ||
        !valid_text(next.message_id, 998) || !valid_text(next.sender, 4096) ||
        !valid_text(next.recipients, MaxEditableTextBytes) || !valid_text(next.archive_folder, 4096) ||
        !next.id || !next.account_id || !next.account_generation || !next.draft_id || !next.draft_revision ||
        !valid_cache_name(next.spool_name) || memcmp(next.spool_name.c_str(), "send-", 5) ||
        next.spool_size > MaxRawMessageBytes ||
        !send_operation_delivery_state_valid(static_cast<SendStage>(stage), next.delivery_count,
            next.archive_review_generation) ||
        (!next.archive_review_generation && (next.archive_folder_generation || next.archive_folder_uidvalidity)) ||
        (next.archive_review_generation && (!next.archive_folder_generation ||
            !next.archive_folder_uidvalidity || next.archive_folder.empty())))
        return set_error(error, "durable send operation metadata is invalid");
    next.stage = static_cast<SendStage>(stage);
    Bytes next_spool;
    if (!read_cache_file(cache_fd_, next.spool_name, MaxRawMessageBytes, next_spool, error) ||
        next_spool.size() != next.spool_size) return set_error(error, "immutable send spool is missing or changed");
    operation = static_cast<SendOperation &&>(next);
    spool = static_cast<Bytes &&>(next_spool);
    return true;
}

bool Store::get_send_operation_summary(Id id, SendOperationSummary &operation, Text &error) {
    error.clear();
    if (!database_ || !id) return set_error(error, "send operation ID is invalid");
    Statement query;
    const char *sql = "SELECT id,account_id,account_generation,draft_id,draft_revision,message_id,sender,"
        "recipients,stage,delivery_count,archive_folder,archive_review_generation,archive_folder_generation,"
        "archive_folder_uidvalidity,last_diagnostic FROM send_operations WHERE id=?";
    if (!prepare(static_cast<sqlite3 *>(database_), query, sql, error) ||
        !bind_i64(query.value, 1, id, error) || sqlite3_step(query.value) != SQLITE_ROW)
        return set_error(error, "send operation was not found");
    SendOperationSummary next;
    uint32_t stage = 0;
    if (!column_u64_checked(query.value, 0, next.id) || !next.id ||
        !column_u64_checked(query.value, 1, next.account_id) || !next.account_id ||
        !column_u64_checked(query.value, 2, next.account_generation) || !next.account_generation ||
        !column_u64_checked(query.value, 3, next.draft_id) || !next.draft_id ||
        !column_u64_checked(query.value, 4, next.draft_revision) || !next.draft_revision ||
        !column_text(query.value, 5, next.message_id) || !valid_text(next.message_id, 998) || next.message_id.empty() ||
        !column_text(query.value, 6, next.sender) || !valid_text(next.sender, 4096) || next.sender.empty() ||
        !column_text(query.value, 7, next.recipients) || !valid_text(next.recipients, MaxEditableTextBytes) || next.recipients.empty() ||
        !column_u32_checked(query.value, 8, stage) || stage > static_cast<uint32_t>(SendStage::ReviewedDelivered) ||
        !column_u32_checked(query.value, 9, next.delivery_count) ||
        !column_text(query.value, 10, next.archive_folder) || !valid_text(next.archive_folder, 4096) ||
        !column_u64_checked(query.value, 11, next.archive_review_generation) ||
        !column_u64_checked(query.value, 12, next.archive_folder_generation) ||
        !column_u64_checked(query.value, 13, next.archive_folder_uidvalidity) ||
        !column_text(query.value, 14, next.last_diagnostic) || !valid_text(next.last_diagnostic, 4096) ||
        !send_operation_delivery_state_valid(static_cast<SendStage>(stage), next.delivery_count,
            next.archive_review_generation) ||
        (!next.archive_review_generation && (next.archive_folder_generation || next.archive_folder_uidvalidity)) ||
        (next.archive_review_generation && (!next.archive_folder_generation ||
            !next.archive_folder_uidvalidity || next.archive_folder.empty())))
        return set_error(error, "send operation summary contains invalid identity data");
    next.stage = static_cast<SendStage>(stage);
    operation = static_cast<SendOperationSummary &&>(next);
    return true;
}

bool Store::list_send_operations(Id account_id, uint64_t offset, SendOperationPage &page, Text &error) {
    error.clear();
    if (!database_ || !account_id || offset > static_cast<uint64_t>(INT64_MAX))
        return set_error(error, "send operation page identity is invalid");
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    Statement count;
    if (!prepare(db, count, "SELECT COUNT(*) FROM send_operations WHERE account_id=?", error) ||
        !bind_i64(count.value, 1, account_id, error) || sqlite3_step(count.value) != SQLITE_ROW)
        return set_error(error, "cannot count send operation history");
    uint64_t total = 0;
    if (!column_u64_checked(count.value, 0, total))
        return set_error(error, "send operation history count is invalid");

    Statement query;
    const char *sql = "SELECT id,account_id,account_generation,draft_id,draft_revision,message_id,sender,"
        "recipients,stage,delivery_count,archive_folder,archive_review_generation,archive_folder_generation,"
        "archive_folder_uidvalidity,last_diagnostic FROM send_operations WHERE account_id=? "
        "ORDER BY id DESC LIMIT ? OFFSET ?";
    if (!prepare(db, query, sql, error) || !bind_i64(query.value, 1, account_id, error) ||
        !bind_i64(query.value, 2, MaxSendOperationPageSize, error) || !bind_i64(query.value, 3, offset, error))
        return false;

    SendOperationPage next;
    next.account_id = account_id;
    next.offset = offset;
    int code;
    while ((code = sqlite3_step(query.value)) == SQLITE_ROW) {
        SendOperationSummary operation;
        uint32_t stage = 0;
        if (!column_u64_checked(query.value, 0, operation.id) || !operation.id ||
            !column_u64_checked(query.value, 1, operation.account_id) || operation.account_id != account_id ||
            !column_u64_checked(query.value, 2, operation.account_generation) || !operation.account_generation ||
            !column_u64_checked(query.value, 3, operation.draft_id) || !operation.draft_id ||
            !column_u64_checked(query.value, 4, operation.draft_revision) || !operation.draft_revision ||
            !column_text(query.value, 5, operation.message_id) || !valid_text(operation.message_id, 998) || operation.message_id.empty() ||
            !column_text(query.value, 6, operation.sender) || !valid_text(operation.sender, 4096) || operation.sender.empty() ||
            !column_text(query.value, 7, operation.recipients) || !valid_text(operation.recipients, MaxEditableTextBytes) || operation.recipients.empty() ||
            !column_u32_checked(query.value, 8, stage) || stage > static_cast<uint32_t>(SendStage::ReviewedDelivered) ||
            !column_u32_checked(query.value, 9, operation.delivery_count) ||
            !column_text(query.value, 10, operation.archive_folder) || !valid_text(operation.archive_folder, 4096) ||
            !column_u64_checked(query.value, 11, operation.archive_review_generation) ||
            !column_u64_checked(query.value, 12, operation.archive_folder_generation) ||
            !column_u64_checked(query.value, 13, operation.archive_folder_uidvalidity) ||
            !column_text(query.value, 14, operation.last_diagnostic) || !valid_text(operation.last_diagnostic, 4096) ||
            !send_operation_delivery_state_valid(static_cast<SendStage>(stage), operation.delivery_count,
                operation.archive_review_generation) ||
            (!operation.archive_review_generation && (operation.archive_folder_generation ||
                operation.archive_folder_uidvalidity)) ||
            (operation.archive_review_generation && (!operation.archive_folder_generation ||
                !operation.archive_folder_uidvalidity || operation.archive_folder.empty())))
            return set_error(error, "send operation history contains invalid identity data");
        operation.stage = static_cast<SendStage>(stage);
        if (!next.operations.push(static_cast<SendOperationSummary &&>(operation)))
            return set_error(error, "send operation page exceeds its bound");
    }
    if (code != SQLITE_DONE) return set_error(error, "cannot read send operation history");
    next.has_more = offset + next.operations.size() < total;
    page = static_cast<SendOperationPage &&>(next);
    return true;
}

bool Store::review_send_archive_target(uint64_t operation_id, uint64_t account_id,
    uint64_t expected_operation_generation, uint64_t current_account_generation,
    const FolderKey &target_folder, uint64_t target_folder_generation,
    uint64_t target_folder_uidvalidity, Text &error) {
    error.clear();
    if (!database_ || !operation_id || !account_id || !expected_operation_generation ||
        !current_account_generation || target_folder.account_id != account_id ||
        target_folder.server_name.empty() || !target_folder_generation || !target_folder_uidvalidity)
        return set_error(error, "archive review identity is incomplete");
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    if (!transaction_begin(db, error)) return false;
    if (!account_generation_matches(db, account_id, current_account_generation, error)) {
        transaction_rollback(db);
        return false;
    }
    Statement folder;
    uint64_t folder_generation = 0, folder_uidvalidity = 0;
    bool selectable = false;
    if (!prepare(db, folder, "SELECT generation,uidvalidity,selectable FROM folders "
            "WHERE account_id=? AND server_name=?", error) ||
        !bind_i64(folder.value, 1, account_id, error) ||
        !bind_text(folder.value, 2, target_folder.server_name, error) ||
        sqlite3_step(folder.value) != SQLITE_ROW ||
        !column_u64_checked(folder.value, 0, folder_generation) ||
        !column_u64_checked(folder.value, 1, folder_uidvalidity) ||
        !column_bool(folder.value, 2, selectable) || !selectable ||
        folder_generation != target_folder_generation || folder_uidvalidity != target_folder_uidvalidity) {
        transaction_rollback(db);
        return set_error(error, "archive destination changed or is not selectable");
    }
    Statement operation;
    uint64_t stored_generation = 0, stored_review_generation = 0;
    uint32_t stage = 0, delivery_count = 0;
    if (!prepare(db, operation, "SELECT account_generation,archive_review_generation,stage,delivery_count FROM send_operations "
            "WHERE id=? AND account_id=?", error) ||
        !bind_i64(operation.value, 1, operation_id, error) || !bind_i64(operation.value, 2, account_id, error) ||
        sqlite3_step(operation.value) != SQLITE_ROW ||
        !column_u64_checked(operation.value, 0, stored_generation) ||
        !column_u64_checked(operation.value, 1, stored_review_generation) ||
        !column_u32_checked(operation.value, 2, stage) ||
        !column_u32_checked(operation.value, 3, delivery_count) ||
        stored_generation != expected_operation_generation ||
        !send_operation_delivery_state_valid(static_cast<SendStage>(stage), delivery_count,
            stored_review_generation) ||
        (stage != static_cast<uint32_t>(SendStage::Accepted) &&
         stage != static_cast<uint32_t>(SendStage::Sent) &&
         stage != static_cast<uint32_t>(SendStage::ArchivePending) &&
         stage != static_cast<uint32_t>(SendStage::ArchiveFailed) &&
         stage != static_cast<uint32_t>(SendStage::ReviewedDelivered)) ||
        (delivery_count == 0 && stored_review_generation != 0 &&
            stage != static_cast<uint32_t>(SendStage::ReviewedDelivered) &&
            stage != static_cast<uint32_t>(SendStage::ArchivePending) &&
            stage != static_cast<uint32_t>(SendStage::ArchiveFailed))) {
        transaction_rollback(db);
        return set_error(error, "only a confirmed delivered message awaiting archive can be reviewed");
    }
    Statement update;
    if (!prepare(db, update, "UPDATE send_operations SET archive_folder=?,archive_review_generation=?,"
            "archive_folder_generation=?,archive_folder_uidvalidity=? WHERE id=? AND account_id=? "
            "AND account_generation=? AND (delivery_count=1 OR (stage=? AND delivery_count=0) OR "
            "(delivery_count=0 AND archive_review_generation<>0 AND stage IN (?,?)))", error) ||
        !bind_text(update.value, 1, target_folder.server_name, error) ||
        !bind_i64(update.value, 2, current_account_generation, error) ||
        !bind_i64(update.value, 3, target_folder_generation, error) ||
        !bind_i64(update.value, 4, target_folder_uidvalidity, error) ||
        !bind_i64(update.value, 5, operation_id, error) || !bind_i64(update.value, 6, account_id, error) ||
        !bind_i64(update.value, 7, expected_operation_generation, error) ||
        !bind_i32(update.value, 8, static_cast<uint32_t>(SendStage::ReviewedDelivered), error) ||
        !bind_i32(update.value, 9, static_cast<uint32_t>(SendStage::ArchivePending), error) ||
        !bind_i32(update.value, 10, static_cast<uint32_t>(SendStage::ArchiveFailed), error) ||
        sqlite3_step(update.value) != SQLITE_DONE || sqlite3_changes(db) != 1) {
        transaction_rollback(db);
        return set_error(error, "cannot persist the reviewed archive destination");
    }
    return transaction_commit(db, error);
}

bool Store::resolve_uncertain_send(uint64_t operation_id, uint64_t account_id,
    uint64_t expected_operation_generation, uint64_t current_account_generation,
    UncertainSendDecision decision, Text &error) {
    error.clear();
    if (!database_ || !operation_id || !account_id || !expected_operation_generation ||
        !current_account_generation ||
        (decision != UncertainSendDecision::ConfirmDelivered &&
         decision != UncertainSendDecision::ConfirmNotDelivered))
        return set_error(error, "uncertain send review identity is invalid");
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    if (!transaction_begin(db, error)) return false;
    if (!account_generation_matches(db, account_id, current_account_generation, error)) {
        transaction_rollback(db);
        return false;
    }
    Statement operation;
    uint64_t stored_generation = 0;
    uint32_t stage = 0, delivery_count = 0;
    if (!prepare(db, operation, "SELECT account_generation,stage,delivery_count FROM send_operations "
            "WHERE id=? AND account_id=?", error) ||
        !bind_i64(operation.value, 1, operation_id, error) || !bind_i64(operation.value, 2, account_id, error) ||
        sqlite3_step(operation.value) != SQLITE_ROW ||
        !column_u64_checked(operation.value, 0, stored_generation) ||
        !column_u32_checked(operation.value, 1, stage) ||
        !column_u32_checked(operation.value, 2, delivery_count) ||
        stored_generation != expected_operation_generation ||
        stage != static_cast<uint32_t>(SendStage::Uncertain) || delivery_count != 0) {
        transaction_rollback(db);
        return set_error(error, "uncertain send changed before review");
    }
    const SendStage next_stage = decision == UncertainSendDecision::ConfirmDelivered ?
        SendStage::ReviewedDelivered : SendStage::Ready;
    Text diagnostic;
    if (!diagnostic.set(decision == UncertainSendDecision::ConfirmDelivered ?
            "User reviewed this uncertain delivery as delivered; SMTP acceptance remains unconfirmed." :
            "User reviewed this uncertain delivery as not delivered; an explicit SMTP retry is available.")) {
        transaction_rollback(db);
        return set_error(error, "cannot record uncertain send review");
    }
    Statement update;
    if (!prepare(db, update, "UPDATE send_operations SET stage=?,last_diagnostic=?,account_generation=? WHERE id=? "
            "AND account_id=? AND account_generation=? AND stage=? AND delivery_count=0", error) ||
        !bind_i32(update.value, 1, static_cast<uint32_t>(next_stage), error) ||
        !bind_text(update.value, 2, diagnostic, error) ||
        !bind_i64(update.value, 3, decision == UncertainSendDecision::ConfirmNotDelivered ?
            current_account_generation : expected_operation_generation, error) ||
        !bind_i64(update.value, 4, operation_id, error) || !bind_i64(update.value, 5, account_id, error) ||
        !bind_i64(update.value, 6, expected_operation_generation, error) ||
        !bind_i32(update.value, 7, static_cast<uint32_t>(SendStage::Uncertain), error) ||
        sqlite3_step(update.value) != SQLITE_DONE || sqlite3_changes(db) != 1) {
        transaction_rollback(db);
        return set_error(error, "cannot persist the uncertain send review");
    }
    return transaction_commit(db, error);
}

bool Store::set_send_operation_diagnostic(uint64_t operation_id, uint64_t account_generation,
    const Text &diagnostic, Text &error) {
    error.clear();
    if (!database_ || !operation_id || !account_generation || !valid_text(diagnostic, 4096))
        return set_error(error, "send operation diagnostic is invalid");
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    Statement update;
    if (!prepare(db, update, "UPDATE send_operations SET last_diagnostic=? WHERE id=? "
            "AND (account_generation=? OR archive_review_generation=?)", error) ||
        !bind_text(update.value, 1, diagnostic, error) || !bind_i64(update.value, 2, operation_id, error) ||
        !bind_i64(update.value, 3, account_generation, error) ||
        !bind_i64(update.value, 4, account_generation, error) || sqlite3_step(update.value) != SQLITE_DONE ||
        sqlite3_changes(db) != 1)
        return set_error(error, "send operation identity changed before recording its diagnostic");
    return true;
}

bool Store::advance_send_operation(Id id, uint64_t account_generation, SendStage stage,
    bool count_delivery, Text &error) {
    error.clear();
    if (!database_ || !id || !account_generation ||
        static_cast<unsigned>(stage) > static_cast<unsigned>(SendStage::ReviewedDelivered) ||
        (count_delivery && stage != SendStage::Accepted))
        return set_error(error, "send operation stage update is invalid");
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    if (!transaction_begin(db, error)) return false;
    Statement query;
    uint64_t stored_generation = 0, stored_review_generation = 0;
    uint32_t stored_stage = 0, delivery_count = 0;
    if (!prepare(db, query, "SELECT account_generation,archive_review_generation,stage,delivery_count "
            "FROM send_operations WHERE id=?", error) ||
        !bind_i64(query.value, 1, id, error) || sqlite3_step(query.value) != SQLITE_ROW ||
        !column_u64_checked(query.value, 0, stored_generation) ||
        !column_u64_checked(query.value, 1, stored_review_generation) ||
        !column_u32_checked(query.value, 2, stored_stage) ||
        !column_u32_checked(query.value, 3, delivery_count) ||
        stored_stage > static_cast<uint32_t>(SendStage::ReviewedDelivered) || delivery_count > 1) {
        transaction_rollback(db);
        return set_error(error, "send operation account identity changed or operation is missing");
    }
    const SendStage previous_stage = static_cast<SendStage>(stored_stage);
    const bool archive_transition = stage == SendStage::ArchivePending || stage == SendStage::ArchiveFailed ||
        stage == SendStage::Archived;
    const bool original_identity = stored_generation == account_generation &&
        (!stored_review_generation || stored_review_generation == stored_generation) &&
        !(previous_stage == SendStage::ReviewedDelivered && archive_transition &&
            !stored_review_generation);
    const bool reviewed_identity = stored_review_generation == account_generation && archive_transition &&
        (delivery_count == 1 || (previous_stage == SendStage::ReviewedDelivered && delivery_count == 0) ||
         (delivery_count == 0 && stored_review_generation != 0 &&
            (previous_stage == SendStage::ArchivePending || previous_stage == SendStage::ArchiveFailed))) &&
        (previous_stage == SendStage::Accepted || previous_stage == SendStage::Sent ||
         previous_stage == SendStage::ArchivePending || previous_stage == SendStage::ArchiveFailed ||
         previous_stage == SendStage::ReviewedDelivered);
    if (!original_identity && !reviewed_identity) {
        transaction_rollback(db);
        return set_error(error, "send operation account identity changed or requires archive review");
    }
    if (!send_transition_allowed(previous_stage, stage) ||
        (count_delivery && (previous_stage != SendStage::AwaitingFinalResponse || stage != SendStage::Accepted)) ||
        (!count_delivery && stage == SendStage::Accepted) ||
        (count_delivery && !original_identity) ||
        (reviewed_identity && !archive_transition)) {
        transaction_rollback(db);
        return set_error(error, "send operation stage transition is not permitted");
    }
    if (count_delivery && delivery_count != 0) {
        transaction_rollback(db);
        return set_error(error, "SMTP delivery was already recorded for this operation");
    }
    Statement update;
    if (!prepare(db, update, "UPDATE send_operations SET stage=?,delivery_count=delivery_count+? "
            "WHERE id=? AND delivery_count=? AND (account_generation=? OR archive_review_generation=?)", error) ||
        !bind_i32(update.value, 1, static_cast<uint32_t>(stage), error) ||
        !bind_i32(update.value, 2, count_delivery ? 1u : 0u, error) || !bind_i64(update.value, 3, id, error) ||
        !bind_i32(update.value, 4, delivery_count, error) ||
        !bind_i64(update.value, 5, account_generation, error) ||
        !bind_i64(update.value, 6, account_generation, error) ||
        sqlite3_step(update.value) != SQLITE_DONE || sqlite3_changes(db) != 1) {
        transaction_rollback(db);
        return set_error(error, "cannot persist send operation stage");
    }
    return transaction_commit(db, error);
}

bool Store::save_contact(Contact &contact, Text &error) {
    error.clear();
    if (!database_ || !valid_text(contact.display_name) || !valid_text(contact.organization) ||
        !valid_text(contact.notes) || contact.addresses.size() > MaxContactAddresses)
        return set_error(error, "contact data is invalid");
    for (size_t i = 0; i < contact.addresses.size(); ++i) {
        Text serialized;
        if (!serialize_address(contact.addresses[i], serialized, error)) return false;
    }
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    if (!transaction_begin(db, error)) return false;
    if (!contact.id) {
        Statement count;
        sqlite3_int64 current = 0;
        if (!prepare(db, count, "SELECT COUNT(*) FROM contacts", error) ||
            sqlite3_step(count.value) != SQLITE_ROW || !column_i64(count.value, 0, current) ||
            current >= static_cast<sqlite3_int64>(MaxStoredCatalogEntries)) {
            transaction_rollback(db);
            return set_error(error, "contact limit reached or count is invalid");
        }
    }
    Statement save;
    const char *sql = contact.id ?
        "UPDATE contacts SET display_name=?,organization=?,notes=? WHERE id=?" :
        "INSERT INTO contacts(display_name,organization,notes) VALUES(?,?,?)";
    if (!prepare(db, save, sql, error) || !bind_text(save.value, 1, contact.display_name, error) ||
        !bind_text(save.value, 2, contact.organization, error) || !bind_text(save.value, 3, contact.notes, error) ||
        (contact.id && !bind_i64(save.value, 4, contact.id, error)) ||
        sqlite3_step(save.value) != SQLITE_DONE) {
        transaction_rollback(db);
        if (error.empty()) set_error(error, "cannot save contact");
        return false;
    }
    const uint64_t id = contact.id ? contact.id : static_cast<uint64_t>(sqlite3_last_insert_rowid(db));
    if (contact.id && sqlite3_changes(db) != 1) {
        transaction_rollback(db);
        return set_error(error, "contact was not found for update");
    }
    if (contact.id) {
        Statement clear;
        if (!prepare(db, clear, "DELETE FROM contact_addresses WHERE contact_id=?", error) ||
            !bind_i64(clear.value, 1, id, error) || sqlite3_step(clear.value) != SQLITE_DONE) {
            transaction_rollback(db);
            return set_error(error, "cannot replace contact addresses");
        }
    }
    Statement address_insert;
    if (!prepare(db, address_insert, "INSERT INTO contact_addresses(contact_id,position,display_name,local_part,domain,"
            "display_quoted,local_quoted) VALUES(?,?,?,?,?,?,?)", error)) {
        transaction_rollback(db);
        return false;
    }
    for (size_t i = 0; i < contact.addresses.size(); ++i) {
        const Address &address = contact.addresses[i];
        if (!bind_i64(address_insert.value, 1, id, error) || !bind_i64(address_insert.value, 2, i, error) ||
            !bind_text(address_insert.value, 3, address.display_name, error) ||
            !bind_text(address_insert.value, 4, address.local_part, error) ||
            !bind_text(address_insert.value, 5, address.domain, error) ||
            !bind_bool(address_insert.value, 6, address.display_name_quoted, error) ||
            !bind_bool(address_insert.value, 7, address.local_part_quoted, error) ||
            sqlite3_step(address_insert.value) != SQLITE_DONE) {
            transaction_rollback(db);
            if (error.empty()) set_error(error, "cannot save contact address");
            return false;
        }
        sqlite3_reset(address_insert.value);
        sqlite3_clear_bindings(address_insert.value);
    }
    if (!transaction_commit(db, error)) return false;
    contact.id = id;
    return true;
}

bool Store::get_contact(Id id, Contact &contact, Text &error) {
    error.clear();
    if (!database_ || !id) return set_error(error, "contact id is invalid");
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    Statement query;
    if (!prepare(db, query, "SELECT id,display_name,organization,notes FROM contacts WHERE id=?", error) ||
        !bind_i64(query.value, 1, id, error)) return false;
    const int code = sqlite3_step(query.value);
    if (code == SQLITE_DONE) return set_error(error, "contact was not found");
    Contact next;
    next.id = column_u64(query.value, 0);
    if (code != SQLITE_ROW || !column_text(query.value, 1, next.display_name) ||
        !column_text(query.value, 2, next.organization) || !column_text(query.value, 3, next.notes))
        return set_error(error, "contact data is invalid");
    Statement addresses;
    if (!prepare(db, addresses, "SELECT position,display_name,local_part,domain,display_quoted,local_quoted "
            "FROM contact_addresses WHERE contact_id=? ORDER BY position", error) ||
        !bind_i64(addresses.value, 1, id, error)) return false;
    size_t position = 0;
    int address_code;
    while ((address_code = sqlite3_step(addresses.value)) == SQLITE_ROW) {
        const sqlite3_int64 stored_position = sqlite3_column_int64(addresses.value, 0);
        Address address;
        if (stored_position < 0 || static_cast<uint64_t>(stored_position) != position ||
            position >= 4096 || !read_address(addresses.value, 1, address) ||
            !next.addresses.push(static_cast<Address &&>(address)))
            return set_error(error, "contact address data is invalid");
        ++position;
    }
    if (address_code != SQLITE_DONE) return set_error(error, "cannot read contact addresses");
    contact = static_cast<Contact &&>(next);
    return true;
}

bool Store::list_contacts(ContactList &contacts, Text &error) {
    error.clear();
    if (!database_) return set_error(error, "profile store is closed");
    Statement query;
    if (!prepare(static_cast<sqlite3 *>(database_), query,
            "SELECT id FROM contacts ORDER BY display_name,id", error)) return false;
    Array<uint64_t> ids;
    int code;
    while ((code = sqlite3_step(query.value)) == SQLITE_ROW) {
        if (!ids.push(column_u64(query.value, 0))) return set_error(error, "out of memory listing contacts");
    }
    if (code != SQLITE_DONE) return set_error(error, "cannot list contacts");
    ContactList next;
    for (size_t i = 0; i < ids.size(); ++i) {
        Contact item;
        if (!get_contact(ids[i], item, error) || !next.push(static_cast<Contact &&>(item)))
            return error.empty() ? set_error(error, "out of memory listing contacts") : false;
    }
    contacts = static_cast<ContactList &&>(next);
    return true;
}

bool Store::delete_contact(Id id, Text &error) {
    error.clear();
    if (!database_ || !id) return set_error(error, "contact id is invalid");
    Statement remove;
    if (!prepare(static_cast<sqlite3 *>(database_), remove, "DELETE FROM contacts WHERE id=?", error) ||
        !bind_i64(remove.value, 1, id, error) || sqlite3_step(remove.value) != SQLITE_DONE ||
        sqlite3_changes(static_cast<sqlite3 *>(database_)) != 1)
        return set_error(error, "cannot delete contact");
    return true;
}

bool Store::save_signature(Signature &signature, Text &error) {
    error.clear();
    if (!database_ || !valid_text(signature.name) || !valid_text(signature.body))
        return set_error(error, "signature data is invalid");
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    if (!transaction_begin(db, error)) return false;
    if (!signature.id) {
        Statement count;
        sqlite3_int64 current = 0;
        if (!prepare(db, count, "SELECT COUNT(*) FROM signatures", error) ||
            sqlite3_step(count.value) != SQLITE_ROW || !column_i64(count.value, 0, current) ||
            current >= static_cast<sqlite3_int64>(MaxStoredCatalogEntries)) {
            transaction_rollback(db);
            return set_error(error, "signature limit reached or count is invalid");
        }
    }
    Statement save;
    const char *sql = signature.id ?
        "UPDATE signatures SET name=?,body=?,enabled=? WHERE id=?" :
        "INSERT INTO signatures(name,body,enabled) VALUES(?,?,?)";
    if (!prepare(db, save, sql, error) || !bind_text(save.value, 1, signature.name, error) ||
        !bind_text(save.value, 2, signature.body, error) || !bind_bool(save.value, 3, signature.enabled, error) ||
        (signature.id && !bind_i64(save.value, 4, signature.id, error)) || sqlite3_step(save.value) != SQLITE_DONE) {
        transaction_rollback(db);
        if (error.empty()) set_error(error, "cannot save signature");
        return false;
    }
    if (signature.id && sqlite3_changes(db) != 1) {
        transaction_rollback(db);
        return set_error(error, "signature was not found for update");
    }
    const uint64_t id = signature.id ? signature.id : static_cast<uint64_t>(sqlite3_last_insert_rowid(db));
    if (!transaction_commit(db, error)) return false;
    signature.id = id;
    return true;
}

bool Store::get_signature(Id id, Signature &signature, Text &error) {
    error.clear();
    if (!database_ || !id) return set_error(error, "signature id is invalid");
    Statement query;
    if (!prepare(static_cast<sqlite3 *>(database_), query,
            "SELECT id,name,body,enabled FROM signatures WHERE id=?", error) ||
        !bind_i64(query.value, 1, id, error)) return false;
    const int code = sqlite3_step(query.value);
    if (code == SQLITE_DONE) return set_error(error, "signature was not found");
    Signature next;
    next.id = column_u64(query.value, 0);
    if (code != SQLITE_ROW || !column_text(query.value, 1, next.name) ||
        !column_text(query.value, 2, next.body)) return set_error(error, "signature data is invalid");
    next.enabled = sqlite3_column_int(query.value, 3) != 0;
    signature = static_cast<Signature &&>(next);
    return true;
}

bool Store::list_signatures(SignatureList &signatures, Text &error) {
    error.clear();
    if (!database_) return set_error(error, "profile store is closed");
    Statement query;
    if (!prepare(static_cast<sqlite3 *>(database_), query,
            "SELECT id,name,body,enabled FROM signatures ORDER BY name,id", error)) return false;
    SignatureList next;
    int code;
    while ((code = sqlite3_step(query.value)) == SQLITE_ROW) {
        Signature item;
        item.id = column_u64(query.value, 0);
        if (!column_text(query.value, 1, item.name) || !column_text(query.value, 2, item.body))
            return set_error(error, "signature data is invalid");
        item.enabled = sqlite3_column_int(query.value, 3) != 0;
        if (!next.push(static_cast<Signature &&>(item))) return set_error(error, "out of memory listing signatures");
    }
    if (code != SQLITE_DONE) return set_error(error, "cannot list signatures");
    signatures = static_cast<SignatureList &&>(next);
    return true;
}

bool Store::delete_signature(Id id, Text &error) {
    error.clear();
    if (!database_ || !id) return set_error(error, "signature id is invalid");
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    Statement remove;
    if (!prepare(db, remove, "DELETE FROM signatures WHERE id=?", error) || !bind_i64(remove.value, 1, id, error) ||
        sqlite3_step(remove.value) != SQLITE_DONE || sqlite3_changes(db) != 1)
        return set_error(error, "cannot delete signature");
    return true;
}

bool Store::save_template(Template &message_template, Text &error) {
    error.clear();
    if (!database_ || !valid_text(message_template.name) || !valid_text(message_template.subject) ||
        !valid_text(message_template.body)) return set_error(error, "template data is invalid");
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    if (!transaction_begin(db, error)) return false;
    if (!message_template.id) {
        Statement count;
        sqlite3_int64 current = 0;
        if (!prepare(db, count, "SELECT COUNT(*) FROM templates", error) ||
            sqlite3_step(count.value) != SQLITE_ROW || !column_i64(count.value, 0, current) ||
            current >= static_cast<sqlite3_int64>(MaxStoredCatalogEntries)) {
            transaction_rollback(db);
            return set_error(error, "template limit reached or count is invalid");
        }
    }
    Statement save;
    const char *sql = message_template.id ?
        "UPDATE templates SET name=?,subject=?,body=? WHERE id=?" :
        "INSERT INTO templates(name,subject,body) VALUES(?,?,?)";
    if (!prepare(db, save, sql, error) || !bind_text(save.value, 1, message_template.name, error) ||
        !bind_text(save.value, 2, message_template.subject, error) || !bind_text(save.value, 3, message_template.body, error) ||
        (message_template.id && !bind_i64(save.value, 4, message_template.id, error)) ||
        sqlite3_step(save.value) != SQLITE_DONE) {
        transaction_rollback(db);
        if (error.empty()) set_error(error, "cannot save template");
        return false;
    }
    if (message_template.id && sqlite3_changes(db) != 1) {
        transaction_rollback(db);
        return set_error(error, "template was not found for update");
    }
    const uint64_t id = message_template.id ? message_template.id : static_cast<uint64_t>(sqlite3_last_insert_rowid(db));
    if (!transaction_commit(db, error)) return false;
    message_template.id = id;
    return true;
}

bool Store::get_template(Id id, Template &message_template, Text &error) {
    error.clear();
    if (!database_ || !id) return set_error(error, "template id is invalid");
    Statement query;
    if (!prepare(static_cast<sqlite3 *>(database_), query,
            "SELECT id,name,subject,body FROM templates WHERE id=?", error) ||
        !bind_i64(query.value, 1, id, error)) return false;
    const int code = sqlite3_step(query.value);
    if (code == SQLITE_DONE) return set_error(error, "template was not found");
    Template next;
    next.id = column_u64(query.value, 0);
    if (code != SQLITE_ROW || !column_text(query.value, 1, next.name) ||
        !column_text(query.value, 2, next.subject) || !column_text(query.value, 3, next.body))
        return set_error(error, "template data is invalid");
    message_template = static_cast<Template &&>(next);
    return true;
}

bool Store::list_templates(TemplateList &templates, Text &error) {
    error.clear();
    if (!database_) return set_error(error, "profile store is closed");
    Statement query;
    if (!prepare(static_cast<sqlite3 *>(database_), query,
            "SELECT id,name,subject,body FROM templates ORDER BY name,id", error)) return false;
    TemplateList next;
    int code;
    while ((code = sqlite3_step(query.value)) == SQLITE_ROW) {
        Template item;
        item.id = column_u64(query.value, 0);
        if (!column_text(query.value, 1, item.name) || !column_text(query.value, 2, item.subject) ||
            !column_text(query.value, 3, item.body) || !next.push(static_cast<Template &&>(item)))
            return set_error(error, "cannot list templates");
    }
    if (code != SQLITE_DONE) return set_error(error, "cannot list templates");
    templates = static_cast<TemplateList &&>(next);
    return true;
}

bool Store::delete_template(Id id, Text &error) {
    error.clear();
    if (!database_ || !id) return set_error(error, "template id is invalid");
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    Statement remove;
    if (!prepare(db, remove, "DELETE FROM templates WHERE id=?", error) || !bind_i64(remove.value, 1, id, error) ||
        sqlite3_step(remove.value) != SQLITE_DONE || sqlite3_changes(db) != 1)
        return set_error(error, "cannot delete template");
    return true;
}

bool Store::save_rule(Rule &rule, Text &error) {
    error.clear();
    if (!database_ || static_cast<unsigned>(rule.field) > static_cast<unsigned>(RuleField::Body) ||
        static_cast<unsigned>(rule.operation) > static_cast<unsigned>(RuleOperator::StartsWith) ||
        static_cast<unsigned>(rule.action) > static_cast<unsigned>(RuleAction::Delete) ||
        !valid_text(rule.value) || (rule.action == RuleAction::Move && !valid_folder_key(rule.destination)))
        return set_error(error, "rule data is invalid");
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    if (!transaction_begin(db, error)) return false;
    if (!rule.id) {
        Statement count;
        sqlite3_int64 current = 0;
        if (!prepare(db, count, "SELECT COUNT(*) FROM rules", error) ||
            sqlite3_step(count.value) != SQLITE_ROW || !column_i64(count.value, 0, current) ||
            current >= static_cast<sqlite3_int64>(MaxStoredCatalogEntries)) {
            transaction_rollback(db);
            return set_error(error, "rule limit reached or count is invalid");
        }
    }
    Statement save;
    const char *sql = rule.id ?
        "UPDATE rules SET enabled=?,field=?,operation=?,value=?,action=?,destination_account_id=?,"
        "destination_server_name=? WHERE id=?" :
        "INSERT INTO rules(enabled,field,operation,value,action,destination_account_id,destination_server_name) "
        "VALUES(?,?,?,?,?,?,?)";
    if (!prepare(db, save, sql, error) || !bind_bool(save.value, 1, rule.enabled, error) ||
        !bind_i32(save.value, 2, static_cast<uint32_t>(rule.field), error) ||
        !bind_i32(save.value, 3, static_cast<uint32_t>(rule.operation), error) ||
        !bind_text(save.value, 4, rule.value, error) ||
        !bind_i32(save.value, 5, static_cast<uint32_t>(rule.action), error) ||
        !bind_i64(save.value, 6, rule.destination.account_id, error) ||
        !bind_text(save.value, 7, rule.destination.server_name, error) ||
        (rule.id && !bind_i64(save.value, 8, rule.id, error)) || sqlite3_step(save.value) != SQLITE_DONE) {
        transaction_rollback(db);
        return error.empty() ? set_error(error, "cannot save rule") : false;
    }
    if (rule.id && sqlite3_changes(db) != 1) {
        transaction_rollback(db);
        return set_error(error, "rule was not found for update");
    }
    if (!transaction_commit(db, error)) return false;
    if (!rule.id) rule.id = static_cast<uint64_t>(sqlite3_last_insert_rowid(db));
    return true;
}

bool Store::get_rule(Id id, Rule &rule, Text &error) {
    error.clear();
    if (!database_ || !id) return set_error(error, "rule id is invalid");
    Statement query;
    if (!prepare(static_cast<sqlite3 *>(database_), query,
            "SELECT id,enabled,field,operation,value,action,destination_account_id,destination_server_name "
            "FROM rules WHERE id=?", error) || !bind_i64(query.value, 1, id, error)) return false;
    const int code = sqlite3_step(query.value);
    if (code == SQLITE_DONE) return set_error(error, "rule was not found");
    Rule next;
    const int field = sqlite3_column_int(query.value, 2);
    const int operation = sqlite3_column_int(query.value, 3);
    const int action = sqlite3_column_int(query.value, 5);
    if (code != SQLITE_ROW || field < 0 || field > static_cast<int>(RuleField::Body) ||
        operation < 0 || operation > static_cast<int>(RuleOperator::StartsWith) ||
        action < 0 || action > static_cast<int>(RuleAction::Delete))
        return set_error(error, "rule data is invalid");
    next.id = column_u64(query.value, 0);
    next.enabled = sqlite3_column_int(query.value, 1) != 0;
    next.field = static_cast<RuleField>(field);
    next.operation = static_cast<RuleOperator>(operation);
    if (!column_text(query.value, 4, next.value) || !column_text(query.value, 7, next.destination.server_name))
        return set_error(error, "rule data is invalid");
    next.action = static_cast<RuleAction>(action);
    next.destination.account_id = column_u64(query.value, 6);
    if (next.action == RuleAction::Move && !valid_folder_key(next.destination))
        return set_error(error, "rule destination is invalid");
    rule = static_cast<Rule &&>(next);
    return true;
}

bool Store::list_rules(RuleList &rules, Text &error) {
    error.clear();
    if (!database_) return set_error(error, "profile store is closed");
    Statement query;
    if (!prepare(static_cast<sqlite3 *>(database_), query,
            "SELECT id,enabled,field,operation,value,action,destination_account_id,destination_server_name "
            "FROM rules ORDER BY id", error)) return false;
    RuleList next;
    int code;
    while ((code = sqlite3_step(query.value)) == SQLITE_ROW) {
        const uint64_t id = column_u64(query.value, 0);
        Rule item;
        if (!get_rule(id, item, error) || !next.push(static_cast<Rule &&>(item)))
            return error.empty() ? set_error(error, "cannot list rules") : false;
    }
    if (code != SQLITE_DONE) return set_error(error, "cannot list rules");
    rules = static_cast<RuleList &&>(next);
    return true;
}

bool Store::delete_rule(Id id, Text &error) {
    error.clear();
    if (!database_ || !id) return set_error(error, "rule id is invalid");
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    Statement remove;
    if (!prepare(db, remove, "DELETE FROM rules WHERE id=?", error) || !bind_i64(remove.value, 1, id, error) ||
        sqlite3_step(remove.value) != SQLITE_DONE || sqlite3_changes(db) != 1)
        return set_error(error, "cannot delete rule");
    return true;
}

namespace {

struct PackageEntry {
    unsigned char type = 0;
    Text name;
    Bytes data;
    size_t references = 0;
};

bool package_entry_exists(const Array<PackageEntry> &entries, unsigned char type, const Text &name);

struct RestoreReplacement {
    int directory_fd = -1;
    Text target;
    Text temporary;
    Text rollback;
    bool had_old = false;
    bool installed = false;
    bool remove_target = false;
};

enum class RestoreDirectory : uint8_t { Config = 1, Data = 2, Cache = 3 };
enum class RestoreAction : uint8_t { Replace = 1, Remove = 2, DeleteAfterCommit = 3 };
enum class RestoreJournalState : uint8_t { Prepared = 1, Committed = 2, RolledBack = 3 };
enum class JournalWriteResult : uint8_t { Failed, Persisted, Uncertain };

struct RestoreJournalEntry {
    RestoreDirectory directory = RestoreDirectory::Config;
    RestoreAction action = RestoreAction::Replace;
    Text target;
    Text temporary;
    Text rollback;
    bool had_old = false;
};

struct RestoreJournal {
    RestoreJournalState state = RestoreJournalState::Prepared;
    Array<RestoreJournalEntry> entries;
};

constexpr size_t RestoreJournalHeaderBytes = 24;
constexpr size_t RestoreJournalRecordBytes = 10;
constexpr size_t MaxRestoreJournalBytes = 64u * 1024u * 1024u;
constexpr size_t MaxRestoreJournalEntries = 400002;
constexpr const char *RestoreJournalName = ".restore-journal";

int restore_directory_fd(RestoreDirectory directory, int config_fd, int data_fd, int cache_fd) {
    switch (directory) {
        case RestoreDirectory::Config: return config_fd;
        case RestoreDirectory::Data: return data_fd;
        case RestoreDirectory::Cache: return cache_fd;
    }
    return -1;
}

bool write_all_fd(int fd, const void *data, size_t size) {
    const unsigned char *bytes = static_cast<const unsigned char *>(data);
    size_t at = 0;
    while (at < size) {
        const ssize_t count = write(fd, bytes + at, size - at);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        at += static_cast<size_t>(count);
    }
    return true;
}

bool read_exact_fd(int fd, void *data, size_t size) {
    unsigned char *bytes = static_cast<unsigned char *>(data);
    size_t at = 0;
    while (at < size) {
        const ssize_t count = read(fd, bytes + at, size - at);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        at += static_cast<size_t>(count);
    }
    return true;
}

bool read_package_exact(int fd, uint64_t &remaining, void *data, size_t size) {
    if (static_cast<uint64_t>(size) > remaining || !read_exact_fd(fd, data, size)) return false;
    remaining -= size;
    return true;
}

void encode_u16(unsigned char *bytes, uint16_t value) {
    bytes[0] = static_cast<unsigned char>(value >> 8);
    bytes[1] = static_cast<unsigned char>(value);
}

uint16_t decode_u16(const unsigned char *bytes) {
    return static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]);
}

void encode_u64(unsigned char *bytes, uint64_t value) {
    for (unsigned index = 0; index < 8; ++index)
        bytes[index] = static_cast<unsigned char>(value >> (56u - index * 8u));
}

uint64_t decode_u64(const unsigned char *bytes) {
    uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) value = (value << 8) | bytes[index];
    return value;
}

void encode_u32(unsigned char *bytes, uint32_t value) {
    bytes[0] = static_cast<unsigned char>(value >> 24);
    bytes[1] = static_cast<unsigned char>(value >> 16);
    bytes[2] = static_cast<unsigned char>(value >> 8);
    bytes[3] = static_cast<unsigned char>(value);
}

uint32_t decode_u32(const unsigned char *bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) |
        (static_cast<uint32_t>(bytes[1]) << 16) |
        (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
}

uint32_t restore_journal_crc(const unsigned char *bytes, size_t size) {
    uint32_t crc = UINT32_C(0xffffffff);
    for (size_t i = 0; i < size; ++i) {
        const unsigned char value = (i >= 20 && i < 24) ? 0 : bytes[i];
        crc ^= value;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool valid_generated_name(const Text &name, const char *prefix, const char *suffix) {
    const size_t prefix_size = strlen(prefix);
    const size_t suffix_size = strlen(suffix);
    if (name.size() != prefix_size + 32 + suffix_size ||
        memcmp(name.c_str(), prefix, prefix_size) ||
        memcmp(name.c_str() + prefix_size + 32, suffix, suffix_size)) return false;
    for (size_t i = prefix_size; i < prefix_size + 32; ++i) {
        const char c = name.c_str()[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool valid_restore_journal_entry(const RestoreJournalEntry &entry) {
    const bool cache_target = entry.directory == RestoreDirectory::Cache && valid_cache_name(entry.target);
    const bool vault_target = entry.directory == RestoreDirectory::Config && entry.target == "credentials.vault";
    const bool database_target = entry.directory == RestoreDirectory::Data && entry.target == "mail.sqlite";
    if (!(cache_target || vault_target || database_target)) return false;
    if (entry.action == RestoreAction::Replace) {
        return valid_generated_name(entry.temporary, "restore-", ".tmp") &&
            (entry.had_old ? valid_generated_name(entry.rollback, "rollback-", ".bak") : entry.rollback.empty());
    }
    if (entry.action == RestoreAction::Remove)
        return vault_target && entry.had_old && entry.temporary.empty() &&
            valid_generated_name(entry.rollback, "rollback-", ".bak");
    if (entry.action == RestoreAction::DeleteAfterCommit)
        return cache_target && entry.had_old && entry.temporary.empty() && entry.rollback.empty();
    return false;
}

struct JournalNameRef {
    uint8_t directory;
    const Text *name;
};

int compare_journal_name_ref(const void *left_value, const void *right_value) {
    const JournalNameRef &left = *static_cast<const JournalNameRef *>(left_value);
    const JournalNameRef &right = *static_cast<const JournalNameRef *>(right_value);
    if (left.directory != right.directory) return left.directory < right.directory ? -1 : 1;
    const size_t common = left.name->size() < right.name->size() ? left.name->size() : right.name->size();
    const int bytes = common ? memcmp(left.name->c_str(), right.name->c_str(), common) : 0;
    if (bytes) return bytes;
    if (left.name->size() == right.name->size()) return 0;
    return left.name->size() < right.name->size() ? -1 : 1;
}

int compare_text_pointer(const void *left_value, const void *right_value) {
    const Text &left = **static_cast<const Text * const *>(left_value);
    const Text &right = **static_cast<const Text * const *>(right_value);
    const size_t common = left.size() < right.size() ? left.size() : right.size();
    const int bytes = common ? memcmp(left.c_str(), right.c_str(), common) : 0;
    if (bytes) return bytes;
    if (left.size() == right.size()) return 0;
    return left.size() < right.size() ? -1 : 1;
}

bool journal_entries_unique(const Array<RestoreJournalEntry> &entries, Text &error) {
    if (entries.size() > SIZE_MAX / (3 * sizeof(JournalNameRef)))
        return set_error(error, "restore journal name index is too large");
    JournalNameRef *names = static_cast<JournalNameRef *>(malloc(entries.size() * 3 * sizeof(JournalNameRef)));
    if (!names) return set_error(error, "out of memory validating restore journal names");
    size_t count = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        names[count++] = {static_cast<uint8_t>(entries[i].directory), &entries[i].target};
        if (!entries[i].temporary.empty())
            names[count++] = {static_cast<uint8_t>(entries[i].directory), &entries[i].temporary};
        if (!entries[i].rollback.empty())
            names[count++] = {static_cast<uint8_t>(entries[i].directory), &entries[i].rollback};
    }
    qsort(names, count, sizeof(JournalNameRef), compare_journal_name_ref);
    bool unique = true;
    for (size_t i = 1; i < count; ++i)
        if (compare_journal_name_ref(&names[i - 1], &names[i]) == 0) { unique = false; break; }
    free(names);
    return unique || set_error(error, "restore journal contains duplicate component names");
}

bool encode_restore_journal(const RestoreJournal &journal, Bytes &bytes, Text &error) {
    if (journal.entries.empty() || journal.entries.size() > MaxRestoreJournalEntries)
        return set_error(error, "restore journal entry count is invalid");
    if (!journal_entries_unique(journal.entries, error)) return false;
    size_t total = RestoreJournalHeaderBytes;
    for (size_t i = 0; i < journal.entries.size(); ++i) {
        const RestoreJournalEntry &entry = journal.entries[i];
        if (!valid_restore_journal_entry(entry) || entry.target.size() > UINT16_MAX ||
            entry.temporary.size() > UINT16_MAX || entry.rollback.size() > UINT16_MAX ||
            total > MaxRestoreJournalBytes - RestoreJournalRecordBytes - entry.target.size() -
                entry.temporary.size() - entry.rollback.size())
            return set_error(error, "restore journal component is invalid or exceeds its size limit");
        total += RestoreJournalRecordBytes + entry.target.size() + entry.temporary.size() + entry.rollback.size();
    }
    unsigned char *encoded = static_cast<unsigned char *>(calloc(1, total));
    if (!encoded) return set_error(error, "out of memory encoding restore journal");
    memcpy(encoded, "RMRJNL01", 8);
    encode_u16(encoded + 8, 1);
    encoded[10] = static_cast<unsigned char>(journal.state);
    encode_u32(encoded + 12, static_cast<uint32_t>(journal.entries.size()));
    encode_u32(encoded + 16, static_cast<uint32_t>(total - RestoreJournalHeaderBytes));
    size_t at = RestoreJournalHeaderBytes;
    for (size_t i = 0; i < journal.entries.size(); ++i) {
        const RestoreJournalEntry &entry = journal.entries[i];
        encoded[at++] = static_cast<unsigned char>(entry.directory);
        encoded[at++] = static_cast<unsigned char>(entry.action);
        encoded[at++] = entry.had_old ? 1 : 0;
        encoded[at++] = 0;
        encode_u16(encoded + at, static_cast<uint16_t>(entry.target.size())); at += 2;
        encode_u16(encoded + at, static_cast<uint16_t>(entry.temporary.size())); at += 2;
        encode_u16(encoded + at, static_cast<uint16_t>(entry.rollback.size())); at += 2;
        memcpy(encoded + at, entry.target.c_str(), entry.target.size()); at += entry.target.size();
        memcpy(encoded + at, entry.temporary.c_str(), entry.temporary.size()); at += entry.temporary.size();
        memcpy(encoded + at, entry.rollback.c_str(), entry.rollback.size()); at += entry.rollback.size();
    }
    encode_u32(encoded + 20, restore_journal_crc(encoded, total));
    const bool copied = bytes.set(encoded, total);
    free(encoded);
    return copied || set_error(error, "out of memory storing restore journal");
}

bool decode_restore_journal(const unsigned char *bytes, size_t size, RestoreJournal &journal, Text &error) {
    if (!bytes || size < RestoreJournalHeaderBytes || size > MaxRestoreJournalBytes ||
        memcmp(bytes, "RMRJNL01", 8) || decode_u16(bytes + 8) != 1 || bytes[11] != 0 ||
        decode_u32(bytes + 16) != size - RestoreJournalHeaderBytes ||
        decode_u32(bytes + 20) != restore_journal_crc(bytes, size))
        return set_error(error, "restore journal header or checksum is invalid");
    const uint8_t state = bytes[10];
    if (state < static_cast<uint8_t>(RestoreJournalState::Prepared) ||
        state > static_cast<uint8_t>(RestoreJournalState::RolledBack))
        return set_error(error, "restore journal state is invalid");
    journal.state = static_cast<RestoreJournalState>(state);
    const uint32_t count = decode_u32(bytes + 12);
    if (!count || count > MaxRestoreJournalEntries)
        return set_error(error, "restore journal entry count is invalid");
    size_t at = RestoreJournalHeaderBytes;
    for (uint32_t i = 0; i < count; ++i) {
        if (size - at < RestoreJournalRecordBytes)
            return set_error(error, "restore journal record is truncated");
        RestoreJournalEntry entry;
        entry.directory = static_cast<RestoreDirectory>(bytes[at++]);
        entry.action = static_cast<RestoreAction>(bytes[at++]);
        const uint8_t had_old = bytes[at++];
        const uint8_t reserved = bytes[at++];
        const uint16_t target_size = decode_u16(bytes + at); at += 2;
        const uint16_t temporary_size = decode_u16(bytes + at); at += 2;
        const uint16_t rollback_size = decode_u16(bytes + at); at += 2;
        const size_t names_size = static_cast<size_t>(target_size) + temporary_size + rollback_size;
        if (had_old > 1 || reserved || !target_size || target_size > NAME_MAX ||
            temporary_size > NAME_MAX || rollback_size > NAME_MAX || names_size > size - at)
            return set_error(error, "restore journal component lengths are invalid");
        if (memchr(bytes + at, 0, names_size))
            return set_error(error, "restore journal component contains NUL");
        if (!entry.target.set(reinterpret_cast<const char *>(bytes + at), target_size))
            return set_error(error, "out of memory reading restore journal");
        at += target_size;
        if (!entry.temporary.set(reinterpret_cast<const char *>(bytes + at), temporary_size))
            return set_error(error, "out of memory reading restore journal");
        at += temporary_size;
        if (!entry.rollback.set(reinterpret_cast<const char *>(bytes + at), rollback_size))
            return set_error(error, "out of memory reading restore journal");
        at += rollback_size;
        entry.had_old = had_old != 0;
        if (!valid_restore_journal_entry(entry) || !journal.entries.push(static_cast<RestoreJournalEntry &&>(entry)))
            return set_error(error, "restore journal component mapping is invalid");
    }
    if (at != size) return set_error(error, "restore journal has trailing data");
    if (!journal_entries_unique(journal.entries, error)) return false;
    return true;
}

bool restore_journal_exists(int config_fd, bool &exists, Text &error) {
    struct stat info;
    if (fstatat(config_fd, RestoreJournalName, &info, AT_SYMLINK_NOFOLLOW) < 0) {
        if (errno == ENOENT) { exists = false; return true; }
        return set_error(error, "cannot inspect profile restore journal");
    }
    exists = true;
    if (!S_ISREG(info.st_mode) || info.st_uid != geteuid() || (info.st_mode & 0777) != 0600)
        return set_error(error, "profile restore journal is unsafe");
    return true;
}

bool read_restore_journal(int config_fd, RestoreJournal &journal, Text &error) {
    int fd = openat(config_fd, RestoreJournalName, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return set_error(error, "cannot open profile restore journal safely");
    struct stat info;
    if (fstat(fd, &info) < 0 || !S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
        (info.st_mode & 0777) != 0600 || info.st_size < static_cast<off_t>(RestoreJournalHeaderBytes) ||
        static_cast<uint64_t>(info.st_size) > MaxRestoreJournalBytes) {
        ::close(fd);
        return set_error(error, "profile restore journal metadata is invalid");
    }
    const size_t size = static_cast<size_t>(info.st_size);
    unsigned char *bytes = static_cast<unsigned char *>(malloc(size));
    if (!bytes) { ::close(fd); return set_error(error, "out of memory reading profile restore journal"); }
    bool ok = read_exact_fd(fd, bytes, size);
    const int close_result = ::close(fd);
    if (!ok || close_result < 0) {
        free(bytes);
        return set_error(error, "profile restore journal could not be read completely");
    }
    ok = decode_restore_journal(bytes, size, journal, error);
    free(bytes);
    return ok;
}

JournalWriteResult persist_restore_journal(int config_fd, const RestoreJournal &journal, Text &error) {
    Bytes bytes;
    if (!encode_restore_journal(journal, bytes, error)) return JournalWriteResult::Failed;
    Text temporary;
    if (!make_random_name(".restore-journal-", ".tmp", temporary, error) ||
        !detail::write_unique_file_at(config_fd, temporary.c_str(), bytes.data(), bytes.size(),
            detail::IoFaultPoint::None, error)) return JournalWriteResult::Failed;
    if (renameat(config_fd, temporary.c_str(), config_fd, RestoreJournalName) < 0) {
        unlinkat(config_fd, temporary.c_str(), 0);
        return set_error(error, "cannot atomically publish profile restore journal"), JournalWriteResult::Failed;
    }
    if (fsync(config_fd) < 0) {
        set_error(error, "profile restore journal state may not be durable");
        return JournalWriteResult::Uncertain;
    }
    return JournalWriteResult::Persisted;
}

bool build_restore_journal(const Array<RestoreReplacement> &replacements,
    const Array<Text> &old_cache_files, const Array<PackageEntry> &package_entries,
    int config_fd, int data_fd, int cache_fd, RestoreJournal &journal, Text &error) {
    journal.state = RestoreJournalState::Prepared;
    for (size_t i = 0; i < replacements.size(); ++i) {
        const RestoreReplacement &replacement = replacements[i];
        RestoreJournalEntry entry;
        if (replacement.directory_fd == config_fd) entry.directory = RestoreDirectory::Config;
        else if (replacement.directory_fd == data_fd) entry.directory = RestoreDirectory::Data;
        else if (replacement.directory_fd == cache_fd) entry.directory = RestoreDirectory::Cache;
        else return set_error(error, "restore component directory is unknown");
        entry.action = replacement.remove_target ? RestoreAction::Remove : RestoreAction::Replace;
        entry.target = replacement.target;
        entry.temporary = replacement.temporary;
        entry.rollback = replacement.rollback;
        entry.had_old = replacement.had_old;
        if (!valid_restore_journal_entry(entry) || !journal.entries.push(static_cast<RestoreJournalEntry &&>(entry)))
            return set_error(error, "cannot map staged profile component into restore journal");
    }
    if (old_cache_files.size() > SIZE_MAX - package_entries.size())
        return set_error(error, "restore cache reference list is too large");
    const size_t max_refs = old_cache_files.size() + package_entries.size();
    if (max_refs > SIZE_MAX / sizeof(const Text *))
        return set_error(error, "restore cache reference list is too large");
    const Text **old_refs = max_refs ? static_cast<const Text **>(malloc(max_refs * sizeof(const Text *))) : nullptr;
    const Text **package_refs = max_refs ? static_cast<const Text **>(malloc(max_refs * sizeof(const Text *))) : nullptr;
    if (max_refs && (!old_refs || !package_refs)) {
        free(old_refs);
        free(package_refs);
        return set_error(error, "out of memory indexing restore cache references");
    }
    size_t old_count = 0, package_count = 0;
    for (size_t i = 0; i < old_cache_files.size(); ++i)
        if (valid_cache_name(old_cache_files[i])) old_refs[old_count++] = &old_cache_files[i];
    for (size_t i = 0; i < package_entries.size(); ++i)
        if (package_entries[i].type == 3) package_refs[package_count++] = &package_entries[i].name;
    if (old_count > 1) qsort(old_refs, old_count, sizeof(const Text *), compare_text_pointer);
    if (package_count > 1) qsort(package_refs, package_count, sizeof(const Text *), compare_text_pointer);
    size_t package_at = 0;
    for (size_t i = 0; i < old_count; ++i) {
        const Text &name = *old_refs[i];
        if (i && name == *old_refs[i - 1]) continue;
        while (package_at < package_count && compare_text_pointer(&package_refs[package_at], &old_refs[i]) < 0)
            ++package_at;
        if (package_at < package_count && *package_refs[package_at] == name) continue;
        RestoreJournalEntry entry;
        entry.directory = RestoreDirectory::Cache;
        entry.action = RestoreAction::DeleteAfterCommit;
        entry.target = name;
        entry.had_old = true;
        if (!journal.entries.push(static_cast<RestoreJournalEntry &&>(entry))) {
            free(old_refs);
            free(package_refs);
            return set_error(error, "out of memory listing obsolete cache files in restore journal");
        }
    }
    free(old_refs);
    free(package_refs);
    if (!journal_entries_unique(journal.entries, error)) return false;
    return true;
}

bool optional_private_regular_file(int directory_fd, const Text &name, bool &exists, Text &error) {
    struct stat info;
    if (fstatat(directory_fd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) < 0) {
        if (errno == ENOENT) { exists = false; return true; }
        return set_error(error, "cannot inspect a restore transaction component");
    }
    exists = true;
    if (!S_ISREG(info.st_mode) || info.st_uid != geteuid() || (info.st_mode & 0777) != 0600)
        return set_error(error, "restore transaction component is unsafe");
    return true;
}

bool validate_restore_journal_files(const RestoreJournal &journal,
    int config_fd, int data_fd, int cache_fd, Text &error) {
    for (size_t i = 0; i < journal.entries.size(); ++i) {
        const RestoreJournalEntry &entry = journal.entries[i];
        const int directory_fd = restore_directory_fd(entry.directory, config_fd, data_fd, cache_fd);
        if (directory_fd < 0) return set_error(error, "restore journal directory mapping is invalid");
        bool exists = false;
        if (!entry.temporary.empty() && !optional_private_regular_file(directory_fd, entry.temporary, exists, error))
            return false;
        if (!entry.rollback.empty()) {
            if (!optional_private_regular_file(directory_fd, entry.rollback, exists, error)) return false;
            if (journal.state == RestoreJournalState::Prepared && entry.had_old && !exists)
                return set_error(error, "restore journal is missing an old-component recovery copy");
        }
        if (entry.action == RestoreAction::DeleteAfterCommit &&
            !optional_private_regular_file(directory_fd, entry.target, exists, error)) return false;
    }
    return true;
}

#ifdef RELIEFMAIL_TESTING
bool fail_restore_test_sync(StoreRestoreFault point) {
    if (!take_restore_fault(point)) return false;
    errno = EIO;
    return true;
}
#endif

bool sync_restore_directory(int directory_fd, bool rollback_sync, Text &error) {
#ifdef RELIEFMAIL_TESTING
    if (rollback_sync && fail_restore_test_sync(StoreRestoreFault::FailRollbackSync))
        return set_error(error, "injected restore rollback directory sync failure");
#else
    (void)rollback_sync;
#endif
    return fsync(directory_fd) == 0 || set_error(error, "cannot synchronize restore transaction directory");
}

bool unlink_optional_restore_file(int directory_fd, const Text &name, bool &changed, Text &error) {
    bool exists = false;
    if (!optional_private_regular_file(directory_fd, name, exists, error)) return false;
    if (!exists) return true;
    if (unlinkat(directory_fd, name.c_str(), 0) < 0)
        return set_error(error, "cannot remove a restore transaction component");
    changed = true;
    return true;
}

bool rollback_restore_journal(const RestoreJournal &journal,
    int config_fd, int data_fd, int cache_fd, Text &error) {
    for (size_t at = journal.entries.size(); at > 0; --at) {
        const RestoreJournalEntry &entry = journal.entries[at - 1];
        if (entry.action == RestoreAction::DeleteAfterCommit) continue;
        const int directory_fd = restore_directory_fd(entry.directory, config_fd, data_fd, cache_fd);
        bool target_exists = false;
        if (!optional_private_regular_file(directory_fd, entry.target, target_exists, error)) return false;
        if (entry.had_old) {
            bool rollback_exists = false;
            if (!optional_private_regular_file(directory_fd, entry.rollback, rollback_exists, error) || !rollback_exists)
                return set_error(error, "restore rollback copy is missing or unsafe");
            struct stat target_info, rollback_info;
            const bool target_is_old = target_exists &&
                fstatat(directory_fd, entry.target.c_str(), &target_info, AT_SYMLINK_NOFOLLOW) == 0 &&
                fstatat(directory_fd, entry.rollback.c_str(), &rollback_info, AT_SYMLINK_NOFOLLOW) == 0 &&
                target_info.st_dev == rollback_info.st_dev && target_info.st_ino == rollback_info.st_ino;
            if (!target_is_old) {
                if (target_exists && unlinkat(directory_fd, entry.target.c_str(), 0) < 0)
                    return set_error(error, "cannot remove a partially installed restore target");
                if (linkat(directory_fd, entry.rollback.c_str(), directory_fd, entry.target.c_str(), 0) < 0)
                    return set_error(error, "cannot restore an old profile component");
            }
        } else if (target_exists && unlinkat(directory_fd, entry.target.c_str(), 0) < 0) {
            return set_error(error, "cannot remove a newly installed profile component");
        }
        if (!sync_restore_directory(directory_fd, true, error)) return false;
    }
    return true;
}

bool finish_restore_journal_cleanup(const RestoreJournal &journal,
    int config_fd, int data_fd, int cache_fd, Text &error) {
    bool dirty_config = false, dirty_data = false, dirty_cache = false;
    bool removed_any = false;
    for (size_t i = 0; i < journal.entries.size(); ++i) {
        const RestoreJournalEntry &entry = journal.entries[i];
        const int directory_fd = restore_directory_fd(entry.directory, config_fd, data_fd, cache_fd);
        bool changed = false;
        if (journal.state == RestoreJournalState::Committed &&
            entry.action == RestoreAction::DeleteAfterCommit) {
            if (!unlink_optional_restore_file(directory_fd, entry.target, changed, error)) return false;
        }
        if (!entry.temporary.empty() &&
            !unlink_optional_restore_file(directory_fd, entry.temporary, changed, error)) return false;
        if (!entry.rollback.empty() &&
            !unlink_optional_restore_file(directory_fd, entry.rollback, changed, error)) return false;
        if (changed) {
            removed_any = true;
            if (directory_fd == config_fd) dirty_config = true;
            else if (directory_fd == data_fd) dirty_data = true;
            else if (directory_fd == cache_fd) dirty_cache = true;
#ifdef RELIEFMAIL_TESTING
            crash_restore_test_process(journal.state == RestoreJournalState::Committed ?
                StoreRestoreFault::CrashDuringCommittedCleanup : StoreRestoreFault::CrashDuringRollbackCleanup);
#endif
        }
    }
    if ((dirty_config && fsync(config_fd) < 0) || (dirty_data && fsync(data_fd) < 0) ||
        (dirty_cache && fsync(cache_fd) < 0))
        return set_error(error, "cannot synchronize restore recovery cleanup");
    if (unlinkat(config_fd, RestoreJournalName, 0) < 0)
        return set_error(error, "cannot remove completed restore journal");
    if (fsync(config_fd) < 0) {
        (void)removed_any;
        return set_error(error, "cannot synchronize completed restore journal removal");
    }
    return true;
}

bool recover_restore_journal(int config_fd, int data_fd, int cache_fd, Text &error) {
    bool present = false;
    if (!restore_journal_exists(config_fd, present, error)) return false;
    if (!present) return true;
    RestoreJournal journal;
    if (!read_restore_journal(config_fd, journal, error) ||
        !validate_restore_journal_files(journal, config_fd, data_fd, cache_fd, error)) return false;
    if (journal.state == RestoreJournalState::Prepared) {
        if (!rollback_restore_journal(journal, config_fd, data_fd, cache_fd, error)) return false;
        journal.state = RestoreJournalState::RolledBack;
        if (persist_restore_journal(config_fd, journal, error) != JournalWriteResult::Persisted)
            return false;
    }
    return finish_restore_journal_cleanup(journal, config_fd, data_fd, cache_fd, error);
}

bool write_package_entry(int fd, unsigned char type, const char *name,
    const void *data, size_t size, Text &error) {
    const size_t name_size = strlen(name);
    if (!name_size || name_size > UINT16_MAX) return set_error(error, "backup component name is invalid");
    unsigned char header[11];
    header[0] = type;
    encode_u16(header + 1, static_cast<uint16_t>(name_size));
    encode_u64(header + 3, size);
    if (!write_all_fd(fd, header, sizeof(header)) || !write_all_fd(fd, name, name_size) ||
        !write_all_fd(fd, data, size)) return set_error(error, "cannot write profile backup");
    return true;
}

bool serialize_database(sqlite3 *source, sqlite3 **snapshot_output, Bytes &image, Text &error) {
    *snapshot_output = nullptr;
    sqlite3 *snapshot = nullptr;
    if (sqlite3_open_v2(":memory:", &snapshot, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
            nullptr) != SQLITE_OK || !snapshot) {
        if (snapshot) sqlite3_close(snapshot);
        return set_error(error, "cannot create database snapshot");
    }
    sqlite3_backup *backup = sqlite3_backup_init(snapshot, "main", source, "main");
    bool ok = backup != nullptr;
    int code = SQLITE_ERROR;
    if (backup) {
        code = sqlite3_backup_step(backup, -1);
        const int finish = sqlite3_backup_finish(backup);
        ok = code == SQLITE_DONE && finish == SQLITE_OK;
    }
    sqlite3_int64 image_size = 0;
    unsigned char *serialized = ok ? sqlite3_serialize(snapshot, "main", &image_size, 0) : nullptr;
    if (!serialized || image_size < 0 || static_cast<uint64_t>(image_size) > SIZE_MAX) ok = false;
    if (ok) ok = image.set(serialized, static_cast<size_t>(image_size));
    if (serialized) sqlite3_free(serialized);
    if (!ok) {
        sqlite3_close(snapshot);
        return set_error(error, "cannot serialize a consistent profile snapshot");
    }
    *snapshot_output = snapshot;
    return true;
}

PackageEntry *find_package_entry(Array<PackageEntry> &entries, unsigned char type, const Text &name) {
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].type == type && entries[i].name == name) return &entries[i];
    return nullptr;
}

bool validate_foreign_keys(sqlite3 *db, Text &error) {
    Statement query;
    if (!prepare(db, query, "PRAGMA foreign_key_check", error)) return false;
    const int code = sqlite3_step(query.value);
    if (code != SQLITE_DONE) return set_error(error, "backup contains broken foreign-key references");
    return true;
}

bool validate_account_rows(sqlite3 *db, Text &error) {
    Statement query;
    if (!prepare(db, query, AccountSelect, error)) return false;
    size_t count = 0;
    int code;
    while ((code = sqlite3_step(query.value)) == SQLITE_ROW) {
        Account account;
        if (++count > MaxAccounts || !read_account(query.value, account) ||
            !validate_account(account, error))
            return error.empty() ? set_error(error, "backup contains invalid account data") : false;
    }
    return code == SQLITE_DONE || set_error(error, "cannot validate backup accounts");
}

bool validate_identity_sequence(sqlite3 *db, Text &error) {
    Statement query;
    if (!prepare(db, query, "SELECT next_value FROM identity_sequence WHERE id=1", error) ||
        sqlite3_step(query.value) != SQLITE_ROW) return set_error(error, "backup identity sequence is missing");
    sqlite3_int64 next = 0;
    if (!column_i64(query.value, 0, next) || next <= 0 || sqlite3_step(query.value) != SQLITE_DONE)
        return set_error(error, "backup identity sequence is malformed");
    Statement maxima;
    if (!prepare(db, maxima, "SELECT MAX((SELECT COALESCE(MAX(config_generation),0) FROM accounts),"
            "(SELECT COALESCE(MAX(generation),0) FROM folders))", error) ||
        sqlite3_step(maxima.value) != SQLITE_ROW) return false;
    sqlite3_int64 maximum = 0;
    if (!column_i64(maxima.value, 0, maximum) || maximum < 0 || next <= maximum)
        return set_error(error, "backup identity sequence could recycle a generation");
    return true;
}

bool read_valid_text(sqlite3_stmt *query, int column, Text &value, size_t maximum = MaxStoredText);
bool validate_draft_attachment_rows(sqlite3 *db, Text &error);

bool validate_message_counts(sqlite3 *db, Text &error) {
    Statement query;
    if (!prepare(db, query, "SELECT account_id,server_name,COUNT(*) FROM messages "
            "GROUP BY account_id,server_name", error)) return false;
    int code;
    while ((code = sqlite3_step(query.value)) == SQLITE_ROW) {
        uint64_t account_id = 0;
        Text server_name;
        sqlite3_int64 count = 0;
        if (!column_u64_checked(query.value, 0, account_id) || !account_id ||
            !read_valid_text(query.value, 1, server_name, 4096) ||
            !column_i64(query.value, 2, count) || count < 0 ||
            static_cast<uint64_t>(count) > MaxIndexedMessagesPerFolder)
            return set_error(error, "backup exceeds the indexed-message limit");
    }
    return code == SQLITE_DONE || set_error(error, "cannot validate backup message counts");
}

bool validate_draft_rows(sqlite3 *db, Text &error) {
    Statement query;
    if (!prepare(db, query, "SELECT id,account_id,to_input,cc_input,bcc_input,raw_recipient_input,subject,body,"
            "html_body,editor_state,rich_format,message_id,in_reply_to,references_text,send_stage,revision,updated_at "
            "FROM drafts", error))
        return false;
    int code;
    while ((code = sqlite3_step(query.value)) == SQLITE_ROW) {
        Draft draft;
        sqlite3_int64 updated_at = 0;
        if (!load_draft_record(query.value, draft, error) ||
            !column_i64(query.value, 16, updated_at) || updated_at < 0 ||
            !load_draft_addresses(db, draft.id, draft, error) || !validate_draft(draft, error)) return false;
    }
    if (code != SQLITE_DONE) return set_error(error, "cannot validate backup drafts");
    return validate_draft_attachment_rows(db, error);
}

bool read_valid_text(sqlite3_stmt *query, int column, Text &value, size_t maximum) {
    return column_text(query, column, value) && valid_text(value, maximum);
}

bool validate_folder_rows(sqlite3 *db, Text &error) {
    Statement counts;
    if (!prepare(db, counts, "SELECT account_id,COUNT(*) FROM folders GROUP BY account_id", error)) return false;
    int code;
    while ((code = sqlite3_step(counts.value)) == SQLITE_ROW) {
        uint64_t account_id = 0;
        sqlite3_int64 count = 0;
        if (!column_u64_checked(counts.value, 0, account_id) || !account_id ||
            !column_i64(counts.value, 1, count) || count < 0 ||
            static_cast<uint64_t>(count) > MaxFoldersPerAccount)
            return set_error(error, "backup exceeds the folder limit");
    }
    if (code != SQLITE_DONE) return set_error(error, "cannot validate backup folder counts");

    Statement rows;
    if (!prepare(db, rows, "SELECT account_id,server_name,display_name,role,uidvalidity,message_count,unread_count,"
            "generation,selectable "
            "FROM folders", error)) return false;
    while ((code = sqlite3_step(rows.value)) == SQLITE_ROW) {
        Folder folder;
        uint32_t role = 0;
        uint64_t message_count = 0, unread_count = 0;
        bool selectable = true;
        if (!column_u64_checked(rows.value, 0, folder.key.account_id) ||
            !read_valid_text(rows.value, 1, folder.key.server_name, 4096) ||
            !read_valid_text(rows.value, 2, folder.display_name) ||
            !column_u32_checked(rows.value, 3, role) ||
            role > static_cast<uint32_t>(FolderRole::Archive) ||
            !column_u64_checked(rows.value, 4, folder.uidvalidity) ||
            !column_u64_checked(rows.value, 5, message_count) || message_count > UINT32_MAX ||
            !column_u64_checked(rows.value, 6, unread_count) || unread_count > message_count ||
            !column_u64_checked(rows.value, 7, folder.generation) || !folder.generation ||
            !column_bool(rows.value, 8, selectable) ||
            !valid_folder_key(folder.key))
            return set_error(error, "backup contains invalid folder data");
        folder.selectable = selectable;
    }
    return code == SQLITE_DONE || set_error(error, "cannot validate backup folders");
}

bool validate_message_rows(sqlite3 *db, Text &error) {
    Statement query;
    const char *sql = "SELECT m.account_id,m.server_name,m.uidvalidity,m.uid,m.from_text,m.subject,"
        "m.internal_date,m.size,m.flags,m.headers_only,m.cache_name,f.uidvalidity FROM messages m "
        "JOIN folders f ON f.account_id=m.account_id AND f.server_name=m.server_name";
    if (!prepare(db, query, sql, error)) return false;
    int code;
    while ((code = sqlite3_step(query.value)) == SQLITE_ROW) {
        FolderKey folder;
        Text from, subject, cache_name;
        uint64_t uidvalidity = 0, size = 0, current_uidvalidity = 0;
        sqlite3_int64 internal_date = 0;
        uint32_t uid = 0, flags = 0;
        bool headers_only = false;
        if (!column_u64_checked(query.value, 0, folder.account_id) ||
            !read_valid_text(query.value, 1, folder.server_name, 4096) ||
            !column_u64_checked(query.value, 2, uidvalidity) || !uidvalidity ||
            !column_u32_checked(query.value, 3, uid) || !uid ||
            !read_valid_text(query.value, 4, from) || !read_valid_text(query.value, 5, subject) ||
            !column_i64(query.value, 6, internal_date) ||
            !column_u64_checked(query.value, 7, size) || size > MaxRawMessageBytes ||
            !column_u32_checked(query.value, 8, flags) ||
            !column_bool(query.value, 9, headers_only) ||
            !read_valid_text(query.value, 10, cache_name, 128) || !valid_cache_name(cache_name) ||
            memcmp(cache_name.c_str(), "msg-", 4) ||
            !column_u64_checked(query.value, 11, current_uidvalidity) || uidvalidity != current_uidvalidity ||
            !valid_folder_key(folder))
            return set_error(error, "backup contains invalid message data");
    }
    return code == SQLITE_DONE || set_error(error, "cannot validate backup messages");
}

bool validate_send_operation_rows(sqlite3 *db, Text &error) {
    Statement query;
    const char *sql = "SELECT id,account_id,account_generation,draft_id,draft_revision,message_id,sender,"
        "recipients,spool_name,spool_size,stage,delivery_count,archive_folder,archive_review_generation,"
        "archive_folder_generation,archive_folder_uidvalidity,last_diagnostic FROM send_operations";
    if (!prepare(db, query, sql, error)) return false;
    int code;
    while ((code = sqlite3_step(query.value)) == SQLITE_ROW) {
        uint64_t id = 0, account_id = 0, account_generation = 0, draft_id = 0, revision = 0, spool_size = 0;
        uint64_t review_generation = 0, review_folder_generation = 0, review_folder_uidvalidity = 0;
        uint32_t stage = 0, delivery_count = 0;
        Text message_id, sender, recipients, spool_name, archive_folder, last_diagnostic;
        if (!column_u64_checked(query.value, 0, id) || !id ||
            !column_u64_checked(query.value, 1, account_id) || !account_id ||
            !column_u64_checked(query.value, 2, account_generation) || !account_generation ||
            !column_u64_checked(query.value, 3, draft_id) || !draft_id ||
            !column_u64_checked(query.value, 4, revision) || !revision ||
            !read_valid_text(query.value, 5, message_id, 998) || message_id.empty() ||
            !read_valid_text(query.value, 6, sender, 4096) || sender.empty() ||
            !read_valid_text(query.value, 7, recipients) || recipients.empty() ||
            !read_valid_text(query.value, 8, spool_name, 128) || !valid_cache_name(spool_name) ||
            memcmp(spool_name.c_str(), "send-", 5) ||
            !column_u64_checked(query.value, 9, spool_size) || !spool_size || spool_size > MaxRawMessageBytes ||
            !column_u32_checked(query.value, 10, stage) || stage > static_cast<uint32_t>(SendStage::ReviewedDelivered) ||
            !column_u32_checked(query.value, 11, delivery_count) ||
            !read_valid_text(query.value, 12, archive_folder, 4096) ||
            !column_u64_checked(query.value, 13, review_generation) ||
            !column_u64_checked(query.value, 14, review_folder_generation) ||
            !column_u64_checked(query.value, 15, review_folder_uidvalidity) ||
            !read_valid_text(query.value, 16, last_diagnostic, 4096))
            return set_error(error, "backup contains invalid durable send operation data");
        const bool archive_stage = stage == static_cast<uint32_t>(SendStage::Sent) ||
            stage == static_cast<uint32_t>(SendStage::Accepted) ||
            stage == static_cast<uint32_t>(SendStage::ArchivePending) ||
            stage == static_cast<uint32_t>(SendStage::ArchiveFailed) ||
            stage == static_cast<uint32_t>(SendStage::Archived);
        if (!send_operation_delivery_state_valid(static_cast<SendStage>(stage), delivery_count,
                review_generation))
            return set_error(error, "backup send operation delivery count conflicts with its stage");
        if ((!review_generation && (review_folder_generation || review_folder_uidvalidity)) ||
            (review_generation && (!review_folder_generation || !review_folder_uidvalidity ||
                archive_folder.empty() || !archive_stage)))
            return set_error(error, "backup contains an incomplete reviewed archive identity");
    }
    return code == SQLITE_DONE || set_error(error, "cannot validate backup send operations");
}

bool validate_stored_address(sqlite3_stmt *query, int first, Text &error) {
    Address address;
    Text serialized;
    return read_address(query, first, address) && serialize_address(address, serialized, error);
}

bool validate_contact_rows(sqlite3 *db, Text &error) {
    Statement contacts;
    if (!prepare(db, contacts, "SELECT id,display_name,organization,notes FROM contacts", error)) return false;
    size_t count = 0;
    int code;
    while ((code = sqlite3_step(contacts.value)) == SQLITE_ROW) {
        uint64_t id = 0;
        Text display, organization, notes;
        if (++count > MaxStoredCatalogEntries || !column_u64_checked(contacts.value, 0, id) || !id ||
            !read_valid_text(contacts.value, 1, display) ||
            !read_valid_text(contacts.value, 2, organization) || !read_valid_text(contacts.value, 3, notes))
            return set_error(error, "backup contains invalid or excessive contacts");
    }
    if (code != SQLITE_DONE) return set_error(error, "cannot validate backup contacts");

    Statement addresses;
    if (!prepare(db, addresses, "SELECT contact_id,position,display_name,local_part,domain,display_quoted,local_quoted "
            "FROM contact_addresses ORDER BY contact_id,position", error)) return false;
    uint64_t previous_id = 0;
    size_t expected_position = 0;
    while ((code = sqlite3_step(addresses.value)) == SQLITE_ROW) {
        uint64_t contact_id = 0;
        sqlite3_int64 position = 0;
        if (!column_u64_checked(addresses.value, 0, contact_id) || !contact_id ||
            !column_i64(addresses.value, 1, position) || position < 0)
            return set_error(error, "backup contains invalid contact address indexes");
        if (contact_id != previous_id) {
            previous_id = contact_id;
            expected_position = 0;
        }
        if (static_cast<uint64_t>(position) != expected_position || expected_position >= MaxContactAddresses ||
            !validate_stored_address(addresses.value, 2, error))
            return error.empty() ? set_error(error, "backup contains invalid contact addresses") : false;
        ++expected_position;
    }
    return code == SQLITE_DONE || set_error(error, "cannot validate backup contact addresses");
}

bool validate_signature_rows(sqlite3 *db, Text &error) {
    Statement query;
    if (!prepare(db, query, "SELECT id,name,body,enabled FROM signatures", error)) return false;
    size_t count = 0;
    int code;
    while ((code = sqlite3_step(query.value)) == SQLITE_ROW) {
        uint64_t id = 0;
        Text name, body;
        bool enabled = false;
        if (++count > MaxStoredCatalogEntries || !column_u64_checked(query.value, 0, id) || !id ||
            !read_valid_text(query.value, 1, name) || !read_valid_text(query.value, 2, body) ||
            !column_bool(query.value, 3, enabled))
            return set_error(error, "backup contains invalid or excessive signatures");
    }
    return code == SQLITE_DONE || set_error(error, "cannot validate backup signatures");
}

bool validate_template_rows(sqlite3 *db, Text &error) {
    Statement query;
    if (!prepare(db, query, "SELECT id,name,subject,body FROM templates", error)) return false;
    size_t count = 0;
    int code;
    while ((code = sqlite3_step(query.value)) == SQLITE_ROW) {
        uint64_t id = 0;
        Text name, subject, body;
        if (++count > MaxStoredCatalogEntries || !column_u64_checked(query.value, 0, id) || !id ||
            !read_valid_text(query.value, 1, name) || !read_valid_text(query.value, 2, subject) ||
            !read_valid_text(query.value, 3, body))
            return set_error(error, "backup contains invalid or excessive templates");
    }
    return code == SQLITE_DONE || set_error(error, "cannot validate backup templates");
}

bool validate_rule_rows(sqlite3 *db, Text &error) {
    Statement query;
    if (!prepare(db, query, "SELECT id,enabled,field,operation,value,action,destination_account_id,"
            "destination_server_name FROM rules", error)) return false;
    size_t count = 0;
    int code;
    while ((code = sqlite3_step(query.value)) == SQLITE_ROW) {
        uint64_t id = 0, destination_id = 0;
        bool enabled = false;
        uint32_t field = 0, operation = 0, action = 0;
        Text value, destination;
        if (++count > MaxStoredCatalogEntries || !column_u64_checked(query.value, 0, id) || !id ||
            !column_bool(query.value, 1, enabled) || !column_u32_checked(query.value, 2, field) ||
            field > static_cast<uint32_t>(RuleField::Body) ||
            !column_u32_checked(query.value, 3, operation) ||
            operation > static_cast<uint32_t>(RuleOperator::StartsWith) ||
            !read_valid_text(query.value, 4, value) || !column_u32_checked(query.value, 5, action) ||
            action > static_cast<uint32_t>(RuleAction::Delete) ||
            !column_u64_checked(query.value, 6, destination_id) ||
            !read_valid_text(query.value, 7, destination, 4096))
            return set_error(error, "backup contains invalid or excessive rules");
        if (action == static_cast<uint32_t>(RuleAction::Move)) {
            FolderKey key;
            key.account_id = destination_id;
            key.server_name = static_cast<Text &&>(destination);
            if (!valid_folder_key(key)) return set_error(error, "backup contains an invalid rule destination");
        }
    }
    return code == SQLITE_DONE || set_error(error, "cannot validate backup rules");
}

bool validate_draft_attachment_rows(sqlite3 *db, Text &error) {
    Statement query;
    if (!prepare(db, query, "SELECT draft_id,position,filename,content_type,content_id,inline_part,size,cache_name "
            "FROM draft_attachments ORDER BY draft_id,position", error)) return false;
    uint64_t previous_id = 0;
    size_t expected_position = 0;
    size_t total_size = 0;
    int code;
    while ((code = sqlite3_step(query.value)) == SQLITE_ROW) {
        uint64_t draft_id = 0, size = 0;
        sqlite3_int64 position = 0;
        bool inline_part = false;
        Text filename, content_type, content_id, cache_name;
        if (!column_u64_checked(query.value, 0, draft_id) || !draft_id ||
            !column_i64(query.value, 1, position) || position < 0)
            return set_error(error, "backup contains invalid draft attachment indexes");
        if (draft_id != previous_id) {
            previous_id = draft_id;
            expected_position = 0;
            total_size = 0;
        }
        if (static_cast<uint64_t>(position) != expected_position || expected_position >= MaxMimeParts ||
            !read_valid_text(query.value, 2, filename) || !read_valid_text(query.value, 3, content_type) ||
            !read_valid_text(query.value, 4, content_id) || !column_bool(query.value, 5, inline_part) ||
            !column_u64_checked(query.value, 6, size) || size > DefaultMaxAttachmentBytes - total_size ||
            !read_valid_text(query.value, 7, cache_name, 128) || !valid_cache_name(cache_name) ||
            memcmp(cache_name.c_str(), "draft-", 6))
            return set_error(error, "backup contains invalid draft attachment data");
        total_size += static_cast<size_t>(size);
        ++expected_position;
    }
    return code == SQLITE_DONE || set_error(error, "cannot validate backup draft attachments");
}

bool validate_cache_references(sqlite3 *db, Array<PackageEntry> &entries, Text &error) {
    Statement query;
    const char *sql = "SELECT cache_name,1,size FROM messages UNION ALL "
        "SELECT cache_name,2,size FROM draft_attachments UNION ALL "
        "SELECT spool_name,3,spool_size FROM send_operations";
    if (!prepare(db, query, sql, error)) return false;
    int code;
    while ((code = sqlite3_step(query.value)) == SQLITE_ROW) {
        Text name;
        if (!column_text(query.value, 0, name) || !valid_cache_name(name))
            return set_error(error, "backup database contains an unsafe cache reference");
        uint32_t kind = 0;
        uint64_t indexed_size = 0;
        if (!column_u32_checked(query.value, 1, kind) || !column_u64_checked(query.value, 2, indexed_size))
            return set_error(error, "backup cache metadata has invalid integer values");
        if ((kind == 1 && (memcmp(name.c_str(), "msg-", 4) || indexed_size > MaxRawMessageBytes)) ||
            (kind == 2 && (memcmp(name.c_str(), "draft-", 6) || indexed_size > DefaultMaxAttachmentBytes)) ||
            (kind == 3 && (memcmp(name.c_str(), "send-", 5) || indexed_size > MaxRawMessageBytes)))
            return set_error(error, "backup cache metadata exceeds its limit");
        PackageEntry *entry = find_package_entry(entries, 3, name);
        if (!entry) return set_error(error, "backup is missing a referenced cache component");
        ++entry->references;
        if ((kind == 2 || kind == 3) && entry->data.size() != indexed_size)
            return set_error(error, "backup draft attachment or send spool size does not match its index");
    }
    if (code != SQLITE_DONE) return set_error(error, "cannot validate backup cache references");
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].type == 3 && entries[i].references != 1)
            return set_error(error, "backup contains an unreferenced or duplicate cache component");
    return true;
}

bool validate_database_image(const Bytes &image, Array<PackageEntry> &entries, Text &error) {
    if (image.empty() || image.size() > static_cast<size_t>(INT64_MAX))
        return set_error(error, "backup database component has an invalid size");
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
            nullptr) != SQLITE_OK || !db) {
        if (db) sqlite3_close(db);
        return set_error(error, "cannot open backup database image");
    }
    unsigned char *copy = static_cast<unsigned char *>(sqlite3_malloc64(image.size()));
    if (!copy) { sqlite3_close(db); return set_error(error, "out of memory validating backup database"); }
    memcpy(copy, image.data(), image.size());
    const int deserialize = sqlite3_deserialize(db, "main", copy, static_cast<sqlite3_int64>(image.size()),
        static_cast<sqlite3_int64>(image.size()), SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_READONLY);
    if (deserialize != SQLITE_OK) {
        sqlite3_close(db);
        return set_error(error, "backup database image cannot be opened");
    }
    int version = 0;
    bool ok = integrity_ok(db, error) && get_user_version(db, version, error) &&
        version == CurrentSchemaVersion && validate_foreign_keys(db, error) &&
        validate_account_rows(db, error) && validate_identity_sequence(db, error) && validate_folder_rows(db, error) &&
        validate_message_counts(db, error) && validate_message_rows(db, error) &&
        validate_send_operation_rows(db, error) &&
        validate_draft_rows(db, error) && validate_contact_rows(db, error) &&
        validate_signature_rows(db, error) && validate_template_rows(db, error) &&
        validate_rule_rows(db, error) && validate_cache_references(db, entries, error);
    if (!ok && error.empty()) set_error(error, "backup database schema version is unsupported");
    if (sqlite3_close(db) != SQLITE_OK) ok = false;
    return ok;
}

bool package_entry_exists(const Array<PackageEntry> &entries, unsigned char type, const Text &name) {
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].type == type && entries[i].name == name) return true;
    return false;
}

bool cleanup_replacements(Array<RestoreReplacement> &replacements, bool rollback_installed) {
    bool ok = true;
    int dirty_directories[3] = {-1, -1, -1};
    for (size_t at = replacements.size(); at > 0; --at) {
        RestoreReplacement &item = replacements[at - 1];
        bool changed = false;
        if (rollback_installed && item.installed) {
            bool restored = false;
            if (item.had_old) {
                if (unlinkat(item.directory_fd, item.target.c_str(), 0) == 0 || errno == ENOENT) {
                    if (linkat(item.directory_fd, item.rollback.c_str(),
                            item.directory_fd, item.target.c_str(), 0) == 0) {
                        changed = true;
                        if (fsync(item.directory_fd) == 0) {
                            if (unlinkat(item.directory_fd, item.rollback.c_str(), 0) == 0) {
                                changed = true;
                                restored = fsync(item.directory_fd) == 0;
                                if (restored) item.rollback.clear();
                            }
                        }
                    }
                }
            } else {
                restored = unlinkat(item.directory_fd, item.target.c_str(), 0) == 0 || errno == ENOENT;
                changed = restored;
                if (restored) restored = fsync(item.directory_fd) == 0;
            }
            if (restored) item.installed = false;
            else ok = false;
        }
        if (!item.temporary.empty()) {
            if (unlinkat(item.directory_fd, item.temporary.c_str(), 0) == 0) changed = true;
            else if (errno != ENOENT) ok = false;
        }
        if ((!rollback_installed || !item.installed) && !item.rollback.empty()) {
            if (unlinkat(item.directory_fd, item.rollback.c_str(), 0) == 0) {
                item.rollback.clear();
                changed = true;
            } else if (errno != ENOENT) ok = false;
        }
        if (changed) {
            size_t index = 0;
            while (index < 3 && dirty_directories[index] != -1 &&
                dirty_directories[index] != item.directory_fd) ++index;
            if (index == 3) ok = false;
            else dirty_directories[index] = item.directory_fd;
        }
    }
    for (size_t i = 0; i < 3; ++i)
        if (dirty_directories[i] >= 0 && fsync(dirty_directories[i]) < 0) ok = false;
    return ok;
}

bool cleanup_uninstalled_replacement(RestoreReplacement &replacement) {
    bool ok = true;
    bool changed = false;
    if (!replacement.temporary.empty()) {
        if (unlinkat(replacement.directory_fd, replacement.temporary.c_str(), 0) == 0) {
            replacement.temporary.clear();
            changed = true;
        } else if (errno != ENOENT) ok = false;
    }
    if (!replacement.rollback.empty()) {
        if (unlinkat(replacement.directory_fd, replacement.rollback.c_str(), 0) == 0) {
            replacement.rollback.clear();
            changed = true;
        } else if (errno != ENOENT) ok = false;
    }
    if (changed && fsync(replacement.directory_fd) < 0) ok = false;
    return ok;
}

void cleanup_staging_or_report(Array<RestoreReplacement> &replacements, Text &error) {
    Text original = static_cast<Text &&>(error);
    if (!cleanup_replacements(replacements, false)) {
        set_error(error, "restore staging failed and temporary/rollback cleanup is pending");
        return;
    }
    error = static_cast<Text &&>(original);
}

bool prepare_replacement(int directory_fd, const Text &target, const void *bytes, size_t size,
    bool remove_target, RestoreReplacement &replacement, Text &error) {
    replacement.directory_fd = directory_fd;
    replacement.target = target;
    replacement.remove_target = remove_target;
    if (!remove_target) {
        if (!make_random_name("restore-", ".tmp", replacement.temporary, error) ||
            !detail::write_unique_file_at(directory_fd, replacement.temporary.c_str(), bytes, size,
                detail::IoFaultPoint::None, error)) {
            if (!cleanup_uninstalled_replacement(replacement))
                set_error(error, "restore staging failed and temporary-file cleanup is pending");
            return false;
        }
    }
    struct stat info;
    if (fstatat(directory_fd, target.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(info.st_mode) || info.st_uid != geteuid()) {
            if (!cleanup_uninstalled_replacement(replacement))
                return set_error(error, "restore target is unsafe and staging cleanup is pending");
            return set_error(error, "existing restore target is unsafe");
        }
        int checked_fd = -1;
        if (!detail::open_private_file_at(directory_fd, target.c_str(), O_RDONLY, checked_fd, error)) {
            if (!cleanup_uninstalled_replacement(replacement))
                return set_error(error, "restore target is unsafe and staging cleanup is pending");
            return false;
        }
        ::close(checked_fd);
        if (!make_random_name("rollback-", ".bak", replacement.rollback, error) ||
            linkat(directory_fd, target.c_str(), directory_fd, replacement.rollback.c_str(), 0) < 0) {
            const bool cleaned = cleanup_uninstalled_replacement(replacement);
            if (!cleaned) return set_error(error, "cannot preserve restore target; staging cleanup is pending");
            return set_error(error, "cannot preserve existing restore target");
        }
        replacement.had_old = true;
    } else if (errno != ENOENT) {
        if (!cleanup_uninstalled_replacement(replacement))
            return set_error(error, "cannot inspect restore target; staging cleanup is pending");
        return set_error(error, "cannot inspect existing restore target");
    }
    return true;
}

} // namespace

} // namespace rmail

namespace rmail {

bool Store::clear_cache(Text &error) {
    error.clear();
    if (!database_) return set_error(error, "profile store is closed");
    sqlite3 *db = static_cast<sqlite3 *>(database_);
    Array<Text> files;
    if (!transaction_begin(db, error)) return false;
    if (!collect_cache_names(db, "SELECT cache_name FROM messages", 0, files, error) ||
        !exec_sql(db, "DELETE FROM messages", error, "cannot clear cached message mappings")) {
        transaction_rollback(db);
        return false;
    }
    if (!transaction_commit(db, error)) return false;
    delete_cache_names(cache_fd_, files);
    return true;
}

bool Store::backup_profile(const char *destination_path, Text &error) {
    error.clear();
    if (!database_ || !destination_path || destination_path[0] != '/')
        return set_error(error, "profile backup destination is invalid");
    Bytes database_image;
    sqlite3 *snapshot = nullptr;
    if (!serialize_database(static_cast<sqlite3 *>(database_), &snapshot, database_image, error)) return false;
#ifdef RELIEFMAIL_TESTING
    void (*hook)(void *) = next_backup_hook;
    void *hook_context = next_backup_hook_context;
    next_backup_hook = nullptr;
    next_backup_hook_context = nullptr;
    if (hook) hook(hook_context);
#endif
    Array<Text> cache_names;
    const bool references_ok = collect_cache_names(snapshot, "SELECT cache_name FROM messages", 0,
            cache_names, error) &&
        collect_cache_names(snapshot, "SELECT cache_name FROM draft_attachments", 0, cache_names, error) &&
        collect_cache_names(snapshot, "SELECT spool_name FROM send_operations", 0, cache_names, error);
    const int snapshot_close = sqlite3_close(snapshot);
    snapshot = nullptr;
    if (!references_ok) return false;
    if (snapshot_close != SQLITE_OK) return set_error(error, "cannot close profile backup snapshot");
    for (size_t i = 0; i < cache_names.size(); ++i) {
        if (!valid_cache_name(cache_names[i])) return set_error(error, "profile contains an unsafe cache reference");
        for (size_t j = 0; j < i; ++j)
            if (cache_names[i] == cache_names[j]) return set_error(error, "profile contains duplicate cache references");
    }

    unsigned char *vault_bytes = nullptr;
    size_t vault_size = 0;
    bool has_vault = false;
    struct stat vault_info;
    if (fstatat(config_fd_, "credentials.vault", &vault_info, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(vault_info.st_mode) || vault_info.st_uid != geteuid() ||
            !detail::read_private_file_at(config_fd_, "credentials.vault", Vault::MaxFileBytes,
                &vault_bytes, vault_size, error)) return false;
        if (!Vault::validate_blob(vault_bytes, vault_size, error)) {
            detail::wipe_memory(vault_bytes, vault_size);
            free(vault_bytes);
            return false;
        }
        has_vault = true;
    } else if (errno != ENOENT) {
        return set_error(error, "cannot inspect credential vault for backup");
    }

    unsigned char random[16];
    if (!detail::random_bytes(random, sizeof(random), error)) {
        if (vault_bytes) { detail::wipe_memory(vault_bytes, vault_size); free(vault_bytes); }
        return false;
    }
    static const char hex[] = "0123456789abcdef";
    char temporary_path[PATH_MAX];
    const size_t destination_size = strlen(destination_path);
    if (destination_size + 5 + sizeof(random) * 2 >= sizeof(temporary_path)) {
        detail::wipe_memory(random, sizeof(random));
        if (vault_bytes) { detail::wipe_memory(vault_bytes, vault_size); free(vault_bytes); }
        return set_error(error, "profile backup destination path is too long");
    }
    memcpy(temporary_path, destination_path, destination_size);
    memcpy(temporary_path + destination_size, ".tmp-", 5);
    size_t suffix_at = destination_size + 5;
    for (size_t i = 0; i < sizeof(random); ++i) {
        temporary_path[suffix_at++] = hex[random[i] >> 4];
        temporary_path[suffix_at++] = hex[random[i] & 15];
    }
    temporary_path[suffix_at] = 0;
    detail::wipe_memory(random, sizeof(random));
    const int fd = ::open(temporary_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        if (vault_bytes) { detail::wipe_memory(vault_bytes, vault_size); free(vault_bytes); }
        return set_error(error, "cannot create temporary profile backup");
    }
    bool ok = fchmod(fd, 0600) == 0;
    const unsigned char magic[8] = {'R','M','B','A','C','K','0','1'};
    if (ok) ok = write_all_fd(fd, magic, sizeof(magic));
    if (ok) ok = write_package_entry(fd, 1, "mail.sqlite", database_image.data(), database_image.size(), error);
    if (ok && has_vault)
        ok = write_package_entry(fd, 2, "credentials.vault", vault_bytes, vault_size, error);
    for (size_t i = 0; ok && i < cache_names.size(); ++i) {
        Bytes content;
        if (!read_cache_file(cache_fd_, cache_names[i], MaxRawMessageBytes, content, error)) {
            ok = false;
            break;
        }
        ok = write_package_entry(fd, 3, cache_names[i].c_str(), content.data(), content.size(), error);
    }
    const unsigned char end = 0;
    if (ok && !write_all_fd(fd, &end, 1)) ok = set_error(error, "cannot finish profile backup");
    if (ok && fsync(fd) < 0) ok = set_error(error, "cannot synchronize profile backup");
    const int close_result = ::close(fd);
    if (close_result < 0 && ok) ok = set_error(error, "cannot close profile backup");
    if (vault_bytes) { detail::wipe_memory(vault_bytes, vault_size); free(vault_bytes); }
    if (!ok) {
        unlink(temporary_path);
        if (error.empty()) set_error(error, "cannot write profile backup");
        return false;
    }
    if (link(temporary_path, destination_path) < 0) {
        unlink(temporary_path);
        return set_error(error, "cannot publish profile backup without replacing an existing file");
    }
    char parent_path[PATH_MAX];
    const char *last_slash = strrchr(destination_path, '/');
    const size_t parent_size = last_slash == destination_path ? 1 :
        static_cast<size_t>(last_slash - destination_path);
    memcpy(parent_path, destination_path, parent_size);
    parent_path[parent_size] = 0;
    const int parent_fd = ::open(parent_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    bool directory_synced = parent_fd >= 0 && fsync(parent_fd) == 0;
    if (parent_fd >= 0 && ::close(parent_fd) < 0) directory_synced = false;
    const bool temp_removed = unlink(temporary_path) == 0;
    bool cleanup_synced = false;
    if (temp_removed) {
        const int cleanup_fd = ::open(parent_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        cleanup_synced = cleanup_fd >= 0 && fsync(cleanup_fd) == 0;
        if (cleanup_fd >= 0 && ::close(cleanup_fd) < 0) cleanup_synced = false;
    }
    if (!directory_synced || !temp_removed || !cleanup_synced) {
        const bool output_removed = unlink(destination_path) == 0 || errno == ENOENT;
        const int cleanup_fd = ::open(parent_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        const bool output_cleanup_synced = cleanup_fd >= 0 && fsync(cleanup_fd) == 0;
        if (cleanup_fd >= 0 && ::close(cleanup_fd) < 0)
            return set_error(error, "backup is complete but directory cleanup could not be confirmed");
        if (!output_removed || !output_cleanup_synced)
            return set_error(error, "backup is complete but directory cleanup could not be confirmed");
        return set_error(error, "cannot synchronize published profile backup");
    }
    return true;
}

bool Store::restore_profile(const char *source_path, Text &error) {
    SecretBytes no_backup_password;
    return restore_profile(source_path, static_cast<SecretBytes &&>(no_backup_password), error);
}

bool Store::restore_profile(const char *source_path, SecretBytes backup_master_password, Text &error) {
    const RestoreResult result = restore_profile_with_status(source_path,
        static_cast<SecretBytes &&>(backup_master_password), error);
    return result == RestoreResult::Restored || result == RestoreResult::RestoredCleanupPending;
}

RestoreResult Store::restore_profile_with_status(const char *source_path,
    SecretBytes backup_master_password, Text &error) {
    bool cleanup_pending = false;
    bool recovery_required = false;
    if (!restore_profile_impl(source_path, static_cast<SecretBytes &&>(backup_master_password),
            error, cleanup_pending, recovery_required))
        return recovery_required ? RestoreResult::RecoveryRequired : RestoreResult::Failed;
    return cleanup_pending ? RestoreResult::RestoredCleanupPending : RestoreResult::Restored;
}

bool Store::restore_profile_impl(const char *source_path, SecretBytes backup_master_password,
    Text &error, bool &cleanup_pending, bool &recovery_required) {
    cleanup_pending = false;
    recovery_required = false;
    error.clear();
    if (!database_ || !source_path || source_path[0] != '/')
        return set_error(error, "profile restore source is invalid");
    RestoreGate restore_gate;
    if (!restore_gate.lock(config_fd_, true, error)) return false;
    bool pending_journal = false;
    if (!restore_journal_exists(config_fd_, pending_journal, error)) return false;
    if (pending_journal) {
        sqlite3_close(static_cast<sqlite3 *>(database_));
        database_ = nullptr;
        recovery_required = true;
        return set_error(error, "profile restore recovery is pending; reopen as the instance owner");
    }
    const int source_fd = ::open(source_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (source_fd < 0) return set_error(error, "cannot open profile backup source");
    struct stat source_info;
    if (fstat(source_fd, &source_info) < 0 || !S_ISREG(source_info.st_mode) || source_info.st_size < 9) {
        ::close(source_fd);
        return set_error(error, "profile backup source is not a valid regular file");
    }
    uint64_t remaining = static_cast<uint64_t>(source_info.st_size);
    unsigned char magic[8];
    if (!read_package_exact(source_fd, remaining, magic, sizeof(magic)) ||
        memcmp(magic, "RMBACK01", sizeof(magic))) {
        ::close(source_fd);
        return set_error(error, "profile backup manifest is unsupported");
    }
    Array<PackageEntry> entries;
    bool has_database = false;
    bool has_vault = false;
    bool ended = false;
    size_t component_count = 0;
    while (!ended) {
        unsigned char type;
        if (!read_package_exact(source_fd, remaining, &type, 1)) {
            ::close(source_fd);
            return set_error(error, "profile backup manifest is truncated");
        }
        if (!type) {
            if (remaining != 0) {
                ::close(source_fd);
                return set_error(error, "profile backup has trailing data");
            }
            ended = true;
            break;
        }
        if (++component_count > 200001) {
            ::close(source_fd);
            return set_error(error, "profile backup has too many components");
        }
        unsigned char record[10];
        if (!read_package_exact(source_fd, remaining, record, sizeof(record))) {
            ::close(source_fd);
            return set_error(error, "profile backup component header is truncated");
        }
        const uint16_t name_size = decode_u16(record);
        const uint64_t content_size = decode_u64(record + 2);
        if (!name_size || name_size > NAME_MAX || content_size > remaining || content_size > SIZE_MAX) {
            ::close(source_fd);
            return set_error(error, "profile backup component size is invalid");
        }
        char component_name[NAME_MAX + 1];
        if (!read_package_exact(source_fd, remaining, component_name, name_size)) {
            ::close(source_fd);
            return set_error(error, "profile backup component name is truncated");
        }
        component_name[name_size] = 0;
        if (memchr(component_name, 0, name_size)) {
            ::close(source_fd);
            return set_error(error, "profile backup component name contains NUL");
        }
        PackageEntry entry;
        entry.type = type;
        if (!entry.name.set(component_name, name_size)) {
            ::close(source_fd);
            return set_error(error, "out of memory reading backup manifest");
        }
        const size_t size = static_cast<size_t>(content_size);
        unsigned char *content = size ? static_cast<unsigned char *>(malloc(size)) : nullptr;
        if (size && !content) {
            ::close(source_fd);
            return set_error(error, "out of memory reading backup component");
        }
        if (!read_package_exact(source_fd, remaining, content, size)) {
            free(content);
            ::close(source_fd);
            return set_error(error, "profile backup component is truncated");
        }
        if (!entry.data.set(content, size)) {
            free(content);
            ::close(source_fd);
            return set_error(error, "out of memory reading backup component");
        }
        free(content);
        if (type == 1) {
            if (has_database || !(entry.name == "mail.sqlite") || !size) {
                ::close(source_fd);
                return set_error(error, "profile backup database component is invalid");
            }
            has_database = true;
        } else if (type == 2) {
            if (has_vault || !(entry.name == "credentials.vault") || size > Vault::MaxFileBytes ||
                !Vault::validate_blob(entry.data.data(), entry.data.size(), error)) {
                ::close(source_fd);
                if (error.empty()) set_error(error, "profile backup vault component is invalid");
                return false;
            }
            has_vault = true;
        } else if (type == 3) {
            const bool message_cache = entry.name.size() >= 4 && !memcmp(entry.name.c_str(), "msg-", 4);
            const bool draft_cache = entry.name.size() >= 6 && !memcmp(entry.name.c_str(), "draft-", 6);
            const bool send_cache = entry.name.size() >= 5 && !memcmp(entry.name.c_str(), "send-", 5);
            const size_t maximum = draft_cache ? DefaultMaxAttachmentBytes : MaxRawMessageBytes;
            if (!valid_cache_name(entry.name) ||
                (!message_cache && !draft_cache && !send_cache) || size > maximum) {
                ::close(source_fd);
                return set_error(error, "profile backup cache component is invalid");
            }
        } else {
            ::close(source_fd);
            return set_error(error, "profile backup contains an unknown component type");
        }
        if (package_entry_exists(entries, type, entry.name) || !entries.push(static_cast<PackageEntry &&>(entry))) {
            ::close(source_fd);
            return set_error(error, "profile backup contains duplicate components or exceeds memory limits");
        }
    }
    unsigned char extra;
    const ssize_t extra_count = read(source_fd, &extra, 1);
    const int source_close = ::close(source_fd);
    if (!ended || remaining || extra_count != 0 || source_close < 0 || !has_database)
        return set_error(error, "profile backup is incomplete");
    Text database_name;
    if (!database_name.set("mail.sqlite")) return set_error(error, "out of memory");
    PackageEntry *database_entry = find_package_entry(entries, 1, database_name);
    if (!database_entry || !validate_database_image(database_entry->data, entries, error)) return false;

    Text vault_name;
    if (!vault_name.set("credentials.vault")) return set_error(error, "out of memory");
    PackageEntry *vault_entry = find_package_entry(entries, 2, vault_name);
    if (vault_entry && !Vault::authenticate_blob(vault_entry->data.data(), vault_entry->data.size(),
            static_cast<SecretBytes &&>(backup_master_password), error)) return false;

    Array<Text> old_cache_files;
    if (!collect_cache_names(static_cast<sqlite3 *>(database_), "SELECT cache_name FROM messages", 0,
            old_cache_files, error) ||
        !collect_cache_names(static_cast<sqlite3 *>(database_),
            "SELECT cache_name FROM draft_attachments", 0, old_cache_files, error) ||
        !collect_cache_names(static_cast<sqlite3 *>(database_),
            "SELECT spool_name FROM send_operations", 0, old_cache_files, error)) return false;

    Array<RestoreReplacement> replacements;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].type != 3) continue;
        RestoreReplacement replacement;
        if (!prepare_replacement(cache_fd_, entries[i].name, entries[i].data.data(),
                entries[i].data.size(), false, replacement, error)) {
            cleanup_staging_or_report(replacements, error);
            return false;
        }
        if (!replacements.push(static_cast<RestoreReplacement &&>(replacement))) {
            const bool current_clean = cleanup_uninstalled_replacement(replacement);
            cleanup_staging_or_report(replacements, error);
            if (!current_clean)
                return set_error(error, "restore staging allocation failed and temporary cleanup is pending");
            return set_error(error, "out of memory preparing profile restore");
        }
    }

    struct stat old_vault_info;
    const int old_vault_stat = fstatat(config_fd_, "credentials.vault", &old_vault_info, AT_SYMLINK_NOFOLLOW);
    if (old_vault_stat < 0 && errno != ENOENT) {
        cleanup_staging_or_report(replacements, error);
        return set_error(error, "cannot inspect current credential vault for restore");
    }
    const bool old_vault_exists = old_vault_stat == 0;
    if (old_vault_exists && !vault_entry) {
        RestoreReplacement replacement;
        if (!prepare_replacement(config_fd_, vault_name, nullptr, 0, true, replacement, error)) {
            cleanup_staging_or_report(replacements, error);
            return false;
        }
        if (!replacements.push(static_cast<RestoreReplacement &&>(replacement))) {
            const bool current_clean = cleanup_uninstalled_replacement(replacement);
            cleanup_staging_or_report(replacements, error);
            if (!current_clean)
                return set_error(error, "vault-removal staging allocation failed; cleanup is pending");
            return set_error(error, "cannot stage removal of the current vault");
        }
    } else if (vault_entry) {
        RestoreReplacement replacement;
        if (!prepare_replacement(config_fd_, vault_name, vault_entry->data.data(),
                vault_entry->data.size(), false, replacement, error)) {
            cleanup_staging_or_report(replacements, error);
            return false;
        }
        if (!replacements.push(static_cast<RestoreReplacement &&>(replacement))) {
            const bool current_clean = cleanup_uninstalled_replacement(replacement);
            cleanup_staging_or_report(replacements, error);
            if (!current_clean)
                return set_error(error, "vault staging allocation failed; cleanup is pending");
            return set_error(error, "cannot stage the restored vault");
        }
    }

    RestoreReplacement database_replacement;
    Text db_name;
    if (!db_name.set("mail.sqlite") || !prepare_replacement(data_fd_, db_name,
            database_entry->data.data(), database_entry->data.size(), false,
            database_replacement, error)) {
        cleanup_staging_or_report(replacements, error);
        if (error.empty()) set_error(error, "cannot stage restored database");
        return false;
    }
    if (!replacements.push(static_cast<RestoreReplacement &&>(database_replacement))) {
        const bool current_clean = cleanup_uninstalled_replacement(database_replacement);
        cleanup_staging_or_report(replacements, error);
        if (!current_clean)
            return set_error(error, "database staging allocation failed; cleanup is pending");
        return set_error(error, "cannot stage restored database");
    }

    auto open_database_again = [this, &error]() -> bool {
        char path[PATH_MAX];
        if (!anchored_child_path(data_fd_, "mail.sqlite", path, sizeof(path)))
            return set_error(error, "profile database path is too long");
        sqlite3 *db = nullptr;
        const int code = sqlite3_open_v2(path, &db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr);
        if (code != SQLITE_OK || !db) {
            if (db) sqlite3_close(db);
            return set_error(error, "cannot reopen restored profile database");
        }
        database_ = db;
        sqlite3_busy_timeout(db, 150);
        sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
        if (!exec_sql(db, "PRAGMA journal_mode=DELETE;PRAGMA synchronous=FULL;PRAGMA foreign_keys=ON;"
                "PRAGMA trusted_schema=OFF;PRAGMA secure_delete=ON;", error,
                "cannot configure restored profile database") || !initialize_schema(db, data_fd_, error)) {
            sqlite3_close(db);
            database_ = nullptr;
            return false;
        }
        return true;
    };

    bool synced_cache = false, synced_config = false, synced_data = false;
    for (size_t i = 0; i < replacements.size(); ++i) {
        const int directory_fd = replacements[i].directory_fd;
        bool *synced = directory_fd == cache_fd_ ? &synced_cache :
            directory_fd == config_fd_ ? &synced_config : &synced_data;
        if (!*synced) {
            if (fsync(directory_fd) < 0) {
                cleanup_staging_or_report(replacements, error);
                return set_error(error, "cannot synchronize staged restore components");
            }
            *synced = true;
        }
    }

    RestoreJournal journal;
    if (!build_restore_journal(replacements, old_cache_files, entries,
            config_fd_, data_fd_, cache_fd_, journal, error)) {
        cleanup_staging_or_report(replacements, error);
        return false;
    }
    sqlite3 *old_db = static_cast<sqlite3 *>(database_);
    if (sqlite3_close(old_db) != SQLITE_OK) {
        cleanup_staging_or_report(replacements, error);
        return set_error(error, "profile database still has active operations");
    }
    database_ = nullptr;
    const JournalWriteResult prepared = persist_restore_journal(config_fd_, journal, error);
    if (prepared != JournalWriteResult::Persisted) {
        if (prepared == JournalWriteResult::Uncertain) {
            recovery_required = true;
            return false;
        }
        Text failure = static_cast<Text &&>(error);
        error.clear();
        const bool cleaned = cleanup_replacements(replacements, false);
        const bool reopened = open_database_again();
        if (!reopened) {
            recovery_required = true;
            if (error.empty()) set_error(error, "restore was not started but the profile database could not be reopened");
            return false;
        }
        if (!cleaned) return set_error(error, "restore was not started; staged component cleanup is pending");
        error = static_cast<Text &&>(failure);
        return false;
    }

    bool install_ok = true;
    for (size_t i = 0; i < replacements.size(); ++i) {
        RestoreReplacement &replacement = replacements[i];
#ifdef RELIEFMAIL_TESTING
        if (replacement.target == "mail.sqlite" && take_restore_fault(StoreRestoreFault::FailDatabaseInstall)) {
            install_ok = false;
            set_error(error, "injected restored database install failure");
            break;
        }
#endif
        int result = 0;
        if (replacement.remove_target) {
            if (unlinkat(replacement.directory_fd, replacement.target.c_str(), 0) < 0 && errno != ENOENT)
                result = -1;
        } else {
            result = renameat(replacement.directory_fd, replacement.temporary.c_str(),
                replacement.directory_fd, replacement.target.c_str());
        }
        if (result < 0) {
            install_ok = false;
            set_error(error, "cannot install validated profile component");
            break;
        }
        replacement.installed = true;
        if (fsync(replacement.directory_fd) < 0) {
            install_ok = false;
            set_error(error, "cannot synchronize restored profile component");
            break;
        }
#ifdef RELIEFMAIL_TESTING
        if (replacement.target == "credentials.vault")
            crash_restore_test_process(StoreRestoreFault::CrashAfterVaultInstall);
        if (replacement.target == "mail.sqlite")
            crash_restore_test_process(StoreRestoreFault::CrashAfterDatabaseInstall);
#endif
    }

    if (install_ok && !open_database_again()) install_ok = false;
    if (!install_ok) {
        Text failure = static_cast<Text &&>(error);
        error.clear();
        if (!recover_restore_journal(config_fd_, data_fd_, cache_fd_, error)) {
            if (database_) sqlite3_close(static_cast<sqlite3 *>(database_));
            database_ = nullptr;
            recovery_required = true;
            if (error.empty()) set_error(error, "restore rollback needs owner recovery");
            return false;
        }
        error.clear();
        if (!open_database_again()) {
            if (database_) sqlite3_close(static_cast<sqlite3 *>(database_));
            database_ = nullptr;
            recovery_required = true;
            if (error.empty()) set_error(error, "old profile was restored but its database could not be reopened");
            return false;
        }
        error = static_cast<Text &&>(failure);
        return false;
    }

    journal.state = RestoreJournalState::Committed;
    const JournalWriteResult committed = persist_restore_journal(config_fd_, journal, error);
    if (committed == JournalWriteResult::Uncertain) {
        sqlite3_close(static_cast<sqlite3 *>(database_));
        database_ = nullptr;
        recovery_required = true;
        return false;
    }
    if (committed == JournalWriteResult::Failed) {
        Text failure = static_cast<Text &&>(error);
        error.clear();
        if (sqlite3_close(static_cast<sqlite3 *>(database_)) != SQLITE_OK) {
            sqlite3_close_v2(static_cast<sqlite3 *>(database_));
            database_ = nullptr;
            recovery_required = true;
            return set_error(error, "commit marker failed and the replacement database could not close safely");
        }
        database_ = nullptr;
        if (!recover_restore_journal(config_fd_, data_fd_, cache_fd_, error)) {
            recovery_required = true;
            return false;
        }
        if (!open_database_again()) {
            recovery_required = true;
            return false;
        }
        error = static_cast<Text &&>(failure);
        return false;
    }
#ifdef RELIEFMAIL_TESTING
    crash_restore_test_process(StoreRestoreFault::CrashAfterCommitPersisted);
#endif
    if (!finish_restore_journal_cleanup(journal, config_fd_, data_fd_, cache_fd_, error)) {
        cleanup_pending = true;
        set_error(error, "profile restore is committed and reopened; journal cleanup is pending");
        return true;
    }
    return true;
}


} // namespace rmail
