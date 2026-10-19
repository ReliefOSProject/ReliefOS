#include "vault.h"
#include "sqlite3.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace rmail;

static void copy_text(Text &out, const char *value) { assert(out.set(value)); }

static bool same_text(const Text &value, const char *expected) { return value == expected; }

static SecretBytes make_secret(const char *value) {
    SecretBytes result;
    assert(result.set(value, strlen(value)));
    return result;
}

static bool read_bytes_path(const char *path, Bytes &output) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat info;
    if (fstat(fd, &info) < 0 || !S_ISREG(info.st_mode) || info.st_size < 0) {
        close(fd);
        return false;
    }
    const size_t size = static_cast<size_t>(info.st_size);
    unsigned char *bytes = size ? static_cast<unsigned char *>(malloc(size)) : nullptr;
    if (size && !bytes) { close(fd); return false; }
    size_t at = 0;
    while (at < size) {
        const ssize_t count = read(fd, bytes + at, size - at);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { free(bytes); close(fd); return false; }
        at += static_cast<size_t>(count);
    }
    const bool ok = output.set(bytes, size);
    free(bytes);
    close(fd);
    return ok;
}

static bool make_tampered_vault_backup(const char *source_path, const char *destination_path) {
    Bytes package;
    if (!read_bytes_path(source_path, package) || package.size() < 9) return false;
    unsigned char *copy = static_cast<unsigned char *>(malloc(package.size()));
    if (!copy) return false;
    memcpy(copy, package.data(), package.size());
    size_t at = 8;
    bool changed = false;
    while (at < package.size()) {
        const unsigned char type = copy[at++];
        if (!type) break;
        if (package.size() - at < 10) break;
        const size_t name_size = (static_cast<size_t>(copy[at]) << 8) | copy[at + 1];
        uint64_t content_size = 0;
        for (size_t i = 0; i < 8; ++i) content_size = (content_size << 8) | copy[at + 2 + i];
        at += 10;
        if (name_size > package.size() - at) break;
        const bool is_vault = type == 2 && name_size == strlen("credentials.vault") &&
            !memcmp(copy + at, "credentials.vault", name_size);
        at += name_size;
        if (content_size > package.size() - at) break;
        if (is_vault) {
            if (content_size < 1) break;
            copy[at + static_cast<size_t>(content_size) - 1] ^= 0x80;
            changed = true;
            break;
        }
        at += static_cast<size_t>(content_size);
    }
    bool ok = changed;
    int fd = -1;
    if (ok) fd = open(destination_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) ok = false;
    size_t written = 0;
    while (ok && written < package.size()) {
        const ssize_t count = write(fd, copy + written, package.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { ok = false; break; }
        written += static_cast<size_t>(count);
    }
    if (fd >= 0 && close(fd) < 0) ok = false;
    free(copy);
    if (!ok) unlink(destination_path);
    return ok;
}

static bool remove_tree(const char *path) {
    struct stat info;
    if (lstat(path, &info) < 0) return errno == ENOENT;
    if (!S_ISDIR(info.st_mode)) return unlink(path) == 0;
    DIR *directory = opendir(path);
    if (!directory) return false;
    bool ok = true;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char child[PATH_MAX];
        const int count = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (count < 0 || static_cast<size_t>(count) >= sizeof(child) || !remove_tree(child)) ok = false;
    }
    if (closedir(directory) < 0) ok = false;
    if (rmdir(path) < 0) ok = false;
    return ok;
}

struct ProfileFixture {
    char root[PATH_MAX]{};
    ProfilePaths paths;

    explicit ProfileFixture(const char *tag) {
        char current[PATH_MAX];
        assert(getcwd(current, sizeof(current)));
        const int count = snprintf(root, sizeof(root), "%s/out/tmp/reliefmail-%s-XXXXXX", current, tag);
        assert(count > 0 && static_cast<size_t>(count) < sizeof(root));
        assert(mkdtemp(root));
        Text error;
        assert(profile_paths_from_env(root, nullptr, nullptr, nullptr, paths, error));
    }

    ~ProfileFixture() { assert(remove_tree(root)); }
};

static void create_data_profile_directory(const ProfileFixture &fixture) {
    char local[PATH_MAX];
    char share[PATH_MAX];
    assert(snprintf(local, sizeof(local), "%s/.local", fixture.root) > 0);
    assert(snprintf(share, sizeof(share), "%s/.local/share", fixture.root) > 0);
    assert(mkdir(local, 0700) == 0);
    assert(mkdir(share, 0700) == 0);
    assert(mkdir(fixture.paths.data_dir.c_str(), 0700) == 0);
}

static Account sample_account() {
    Account account;
    copy_text(account.display_name, "林小明");
    copy_text(account.email_address, "ming@example.cn");
    copy_text(account.username, "imap-user");
    copy_text(account.smtp_username, "smtp-user");
    copy_text(account.incoming_host, "imap.example.cn");
    copy_text(account.outgoing_host, "smtp.example.cn");
    copy_text(account.custom_ca_file, "/home/test/ca.pem");
    copy_text(account.inbox_folder, "INBOX");
    copy_text(account.sent_folder, "Sent Items");
    copy_text(account.drafts_folder, "Drafts");
    copy_text(account.trash_folder, "Deleted");
    copy_text(account.junk_folder, "Junk");
    copy_text(account.archive_folder, "Archive");
    account.incoming_port = 993;
    account.outgoing_port = 465;
    account.incoming_protocol = IncomingProtocol::Imap;
    account.incoming_security = TransportSecurity::ImplicitTls;
    account.outgoing_security = TransportSecurity::ImplicitTls;
    account.authentication = AuthenticationMethod::ApplicationPassword;
    account.enabled = true;
    account.is_default = true;
    account.connect_timeout_ms = 17000;
    account.io_timeout_ms = 31000;
    account.poll_interval_minutes = 12;
    account.cache_days = 45;
    account.signature_id = 9;
    return account;
}

static void test_profile_paths_follow_xdg_and_home_fallback() {
    ProfilePaths paths;
    Text error;
    assert(profile_paths_from_env("/home/mail-user", nullptr, nullptr, nullptr, paths, error));
    assert(same_text(paths.config_dir, "/home/mail-user/.config/reliefmail"));
    assert(same_text(paths.data_dir, "/home/mail-user/.local/share/reliefmail"));
    assert(same_text(paths.cache_dir, "/home/mail-user/.cache/reliefmail"));
    assert(profile_paths_from_env("/home/mail-user", "/tmp/xdg-c", "/tmp/xdg-d",
        "/tmp/xdg-k", paths, error));
    assert(same_text(paths.config_dir, "/tmp/xdg-c/reliefmail"));
    assert(same_text(paths.data_dir, "/tmp/xdg-d/reliefmail"));
    assert(same_text(paths.cache_dir, "/tmp/xdg-k/reliefmail"));
    assert(!profile_paths_from_env(nullptr, nullptr, nullptr, nullptr, paths, error));
}

static void test_profile_modes_and_owner_worker_locking() {
    ProfileFixture fixture("store-mode");
    Text error;
    Store owner;
    assert(owner.open(fixture.paths, StoreOpenMode::InstanceOwner, error));

    struct stat info;
    assert(stat(fixture.paths.config_dir.c_str(), &info) == 0);
    assert((info.st_mode & 0777) == 0700);
    assert(stat(fixture.paths.data_dir.c_str(), &info) == 0);
    assert((info.st_mode & 0777) == 0700);
    assert(stat(fixture.paths.cache_dir.c_str(), &info) == 0);
    assert((info.st_mode & 0777) == 0700);

    char database[PATH_MAX];
    assert(snprintf(database, sizeof(database), "%s/mail.sqlite", fixture.paths.data_dir.c_str()) > 0);
    assert(stat(database, &info) == 0 && S_ISREG(info.st_mode));
    assert((info.st_mode & 0777) == 0600);
    char lock_path[PATH_MAX];
    assert(snprintf(lock_path, sizeof(lock_path), "%s/instance.lock", fixture.paths.config_dir.c_str()) > 0);
    assert(stat(lock_path, &info) == 0 && (info.st_mode & 0777) == 0600);

    Store second_owner;
    assert(!second_owner.open(fixture.paths, StoreOpenMode::InstanceOwner, error));
    Store worker;
    assert(worker.open(fixture.paths, StoreOpenMode::Worker, error));
}

static void test_store_rejects_symlink_and_unowned_profile_directories() {
    ProfileFixture fixture("store-unsafe");
    char target[PATH_MAX];
    char link_path[PATH_MAX];
    assert(snprintf(target, sizeof(target), "%s/target", fixture.root) > 0);
    assert(snprintf(link_path, sizeof(link_path), "%s/xdg", fixture.root) > 0);
    assert(mkdir(target, 0700) == 0);
    assert(symlink(target, link_path) == 0);

    ProfilePaths paths;
    Text error;
    assert(profile_paths_from_env(fixture.root, nullptr, link_path, nullptr, paths, error));
    Store symlink_store;
    assert(!symlink_store.open(paths, StoreOpenMode::Worker, error));

    if (geteuid() != 0) {
        ProfilePaths foreign;
        assert(foreign.config_dir.set("/etc"));
        assert(foreign.data_dir.set("/etc"));
        assert(foreign.cache_dir.set("/etc"));
        Store foreign_store;
        assert(!foreign_store.open(foreign, StoreOpenMode::Worker, error));
        assert(strstr(error.c_str(), "profile"));
    }
}

static void test_account_folder_message_and_search_crud() {
    ProfileFixture fixture("store-crud");
    Text error;
    Store store;
    assert(store.open(fixture.paths, StoreOpenMode::Worker, error));

    Account account = sample_account();
    assert(store.save_account(account, error));
    assert(account.id != 0);
    Account loaded_account;
    assert(store.get_account(account.id, loaded_account, error));
    assert(same_text(loaded_account.smtp_username, "smtp-user"));
    assert(loaded_account.enabled && loaded_account.is_default);
    assert(loaded_account.connect_timeout_ms == 17000 && loaded_account.io_timeout_ms == 31000);
    assert(loaded_account.poll_interval_minutes == 12 && loaded_account.cache_days == 45);
    assert(same_text(loaded_account.sent_folder, "Sent Items"));
    assert(loaded_account.signature_id == 9);
    AccountList accounts;
    assert(store.list_accounts(accounts, error) && accounts.size() == 1);

    Folder folder;
    folder.key.account_id = account.id;
    copy_text(folder.key.server_name, "Archive & Records");
    copy_text(folder.display_name, "归档与记录");
    folder.role = FolderRole::Archive;
    folder.uidvalidity = 71;
    assert(store.save_folder(folder, error));
    Folder found_folder;
    assert(store.get_folder(folder.key, found_folder, error));
    assert(same_text(found_folder.key.server_name, "Archive & Records"));
    assert(same_text(found_folder.display_name, "归档与记录"));
    assert(found_folder.uidvalidity == 71);

    MessageSummary summary;
    summary.key.folder = folder.key;
    summary.key.uidvalidity = 71;
    summary.key.uid = 43;
    copy_text(summary.from, "支持@example.cn");
    copy_text(summary.subject, "项目进展与预算");
    summary.internal_date = 1792300123;
    summary.size = 4;
    summary.flags = 5;
    Bytes raw;
    assert(raw.set("EML1", 4));
    assert(store.save_message(summary, raw, error));

    MessageSummary loaded_summary;
    Bytes loaded_raw;
    assert(store.load_message(summary.key, loaded_summary, loaded_raw, error));
    assert(same_text(loaded_summary.subject, "项目进展与预算"));
    assert(loaded_raw == raw);
    MessagePage page;
    assert(store.search_messages(folder.key, "进展", 0, page, error));
    assert(page.size() == 1 && page[0].key.uid == 43);
    assert(store.search_messages(folder.key, "%' OR 1=1 --", 0, page, error));
    assert(page.empty());

    folder.uidvalidity = 72;
    assert(store.save_folder(folder, error));
    assert(!store.load_message(summary.key, loaded_summary, loaded_raw, error));
    assert(store.message_count(folder.key, error) == 0);

    summary.key.uidvalidity = 72;
    assert(store.save_message(summary, raw, error));
    assert(store.delete_message(summary.key, error));
    assert(store.message_count(folder.key, error) == 0);
    assert(store.delete_folder(folder.key, error));
    assert(store.delete_account(account.id, error));
    assert(!store.get_account(account.id, loaded_account, error));
}

static void test_account_folder_and_message_write_limits_are_atomic() {
    ProfileFixture fixture("store-write-limits");
    Text error;
    Store store;
    assert(store.open(fixture.paths, StoreOpenMode::Worker, error));

    Account accounts[MaxAccounts];
    for (size_t i = 0; i < MaxAccounts; ++i) {
        accounts[i] = sample_account();
        accounts[i].is_default = i == 0;
        char value[48];
        assert(snprintf(value, sizeof(value), "account-%zu@example.test", i) > 0);
        copy_text(accounts[i].email_address, value);
        assert(snprintf(value, sizeof(value), "user-%zu", i) > 0);
        copy_text(accounts[i].username, value);
        assert(snprintf(value, sizeof(value), "smtp-%zu", i) > 0);
        copy_text(accounts[i].smtp_username, value);
        assert(store.save_account(accounts[i], error));
    }
    Account missing_id = sample_account();
    missing_id.id = UINT64_C(900000);
    missing_id.is_default = false;
    assert(!store.save_account(missing_id, error));
    AccountList account_rows;
    assert(store.list_accounts(account_rows, error) && account_rows.size() == MaxAccounts);
    copy_text(accounts[0].display_name, "updated at account limit");
    assert(store.save_account(accounts[0], error));
    Account extra = sample_account();
    extra.is_default = false;
    assert(!store.save_account(extra, error));
    assert(store.list_accounts(account_rows, error) && account_rows.size() == MaxAccounts);

    char database[PATH_MAX];
    assert(snprintf(database, sizeof(database), "%s/mail.sqlite", fixture.paths.data_dir.c_str()) > 0);
    sqlite3 *seed = nullptr;
    assert(sqlite3_open(database, &seed) == SQLITE_OK);
    assert(sqlite3_exec(seed, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_stmt *insert_folder = nullptr;
    assert(sqlite3_prepare_v2(seed, "INSERT INTO folders(account_id,server_name,display_name,role,uidvalidity,"
        "message_count,unread_count) VALUES(?,?,?,?,1,0,0)", -1, &insert_folder, nullptr) == SQLITE_OK);
    for (size_t i = 0; i < MaxFoldersPerAccount; ++i) {
        char name[48];
        assert(snprintf(name, sizeof(name), "bulk-folder-%04zu", i) > 0);
        assert(sqlite3_bind_int64(insert_folder, 1, static_cast<sqlite3_int64>(accounts[0].id)) == SQLITE_OK);
        assert(sqlite3_bind_text(insert_folder, 2, name, -1, SQLITE_TRANSIENT) == SQLITE_OK);
        assert(sqlite3_bind_text(insert_folder, 3, name, -1, SQLITE_TRANSIENT) == SQLITE_OK);
        assert(sqlite3_bind_int(insert_folder, 4, static_cast<int>(FolderRole::None)) == SQLITE_OK);
        assert(sqlite3_step(insert_folder) == SQLITE_DONE);
        assert(sqlite3_reset(insert_folder) == SQLITE_OK);
        assert(sqlite3_clear_bindings(insert_folder) == SQLITE_OK);
    }
    assert(sqlite3_finalize(insert_folder) == SQLITE_OK);
    assert(sqlite3_exec(seed, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK);

    Folder folder;
    folder.key.account_id = accounts[0].id;
    copy_text(folder.key.server_name, "bulk-folder-0000");
    copy_text(folder.display_name, "folder update at limit");
    folder.uidvalidity = 1;
    assert(store.save_folder(folder, error));
    folder.key.server_name.clear();
    copy_text(folder.key.server_name, "over-limit-folder");
    assert(!store.save_folder(folder, error));
    FolderList folder_rows;
    assert(store.list_folders(accounts[0].id, folder_rows, error) &&
        folder_rows.size() == MaxFoldersPerAccount);

    assert(sqlite3_exec(seed, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_stmt *insert_message = nullptr;
    assert(sqlite3_prepare_v2(seed, "INSERT INTO messages(account_id,server_name,uidvalidity,uid,from_text,"
        "subject,internal_date,size,flags,cache_name) VALUES(?,?,1,?,'bulk@example.test','seed',0,0,0,?)",
        -1, &insert_message, nullptr) == SQLITE_OK);
    for (size_t i = 1; i <= MaxIndexedMessagesPerFolder; ++i) {
        char cache_name[48];
        assert(snprintf(cache_name, sizeof(cache_name), "msg-%032zx.bin", i) > 0);
        assert(sqlite3_bind_int64(insert_message, 1, static_cast<sqlite3_int64>(accounts[0].id)) == SQLITE_OK);
        assert(sqlite3_bind_text(insert_message, 2, "bulk-folder-0000", -1, SQLITE_STATIC) == SQLITE_OK);
        assert(sqlite3_bind_int64(insert_message, 3, static_cast<sqlite3_int64>(i)) == SQLITE_OK);
        assert(sqlite3_bind_text(insert_message, 4, cache_name, -1, SQLITE_TRANSIENT) == SQLITE_OK);
        assert(sqlite3_step(insert_message) == SQLITE_DONE);
        assert(sqlite3_reset(insert_message) == SQLITE_OK);
        assert(sqlite3_clear_bindings(insert_message) == SQLITE_OK);
    }
    assert(sqlite3_finalize(insert_message) == SQLITE_OK);
    assert(sqlite3_exec(seed, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK);
    assert(sqlite3_close(seed) == SQLITE_OK);

    MessageSummary existing;
    existing.key.folder.account_id = accounts[0].id;
    copy_text(existing.key.folder.server_name, "bulk-folder-0000");
    existing.key.uidvalidity = 1;
    existing.key.uid = 1;
    copy_text(existing.subject, "updated at message limit");
    Bytes message;
    assert(message.set("replacement", sizeof("replacement") - 1));
    assert(store.save_message(existing, message, error));
    MessageSummary additional = existing;
    additional.key.uid = static_cast<uint32_t>(MaxIndexedMessagesPerFolder + 1);
    assert(!store.save_message(additional, message, error));
    assert(store.message_count(existing.key.folder, error) == MaxIndexedMessagesPerFolder);
}

static void test_empty_uidvalidity_replacement_is_explicit_and_atomic() {
    ProfileFixture fixture("store-empty-uidvalidity");
    Text error;
    Store store;
    assert(store.open(fixture.paths, StoreOpenMode::Worker, error));
    Account account = sample_account();
    assert(store.save_account(account, error));

    Folder folder;
    folder.key.account_id = account.id;
    copy_text(folder.key.server_name, "Archive");
    copy_text(folder.display_name, "归档");
    folder.uidvalidity = 44;
    assert(store.save_folder(folder, error));

    MessageSummary old_message;
    old_message.key.folder = folder.key;
    old_message.key.uidvalidity = 44;
    old_message.key.uid = 7;
    copy_text(old_message.subject, "old mapping");
    Bytes content;
    assert(content.set("old", 3));
    assert(store.save_message(old_message, content, error));

    /* SELECT can return no messages; its changed UIDVALIDITY still commits and purges. */
    assert(store.update_folder_uidvalidity(folder.key, 45, error));
    Folder current;
    assert(store.get_folder(folder.key, current, error));
    assert(current.uidvalidity == 45);
    assert(store.message_count(folder.key, error) == 0);
    MessageSummary loaded;
    Bytes loaded_content;
    assert(!store.load_message(old_message.key, loaded, loaded_content, error));
}

static void test_task5_stale_account_and_folder_generations_are_rejected() {
    ProfileFixture fixture("task5-identities");
    Text error;
    Store store;
    assert(store.open(fixture.paths, StoreOpenMode::Worker, error));
    Account original = sample_account();
    assert(store.save_account(original, error));
    const uint64_t old_id = original.id;
    const uint64_t old_account_generation = original.generation;
    Folder old_folder;
    old_folder.key.account_id = old_id;
    copy_text(old_folder.key.server_name, "INBOX");
    copy_text(old_folder.display_name, "Inbox");
    old_folder.uidvalidity = 81;
    assert(store.save_folder_metadata(old_folder, error));
    assert(store.get_folder(old_folder.key, old_folder, error));
    const uint64_t old_folder_generation = old_folder.generation;

    MessageSummary summary;
    summary.key.folder = old_folder.key;
    summary.key.uidvalidity = old_folder.uidvalidity;
    summary.key.uid = 7;
    copy_text(summary.subject, "old account message");
    Bytes content;
    assert(content.set("old body", 8));
    assert(store.save_message(summary, content, error));
    assert(store.delete_account_guarded(old_id, old_account_generation, error));

    Account replacement = sample_account();
    assert(store.save_account(replacement, error));
    assert(replacement.id == old_id); // SQLite INTEGER PRIMARY KEY recycles the deleted highest ID.
    assert(replacement.generation != old_account_generation);
    assert(!store.save_account_guarded(original, old_account_generation, error));
    Folder replacement_folder;
    replacement_folder.key.account_id = replacement.id;
    copy_text(replacement_folder.key.server_name, "INBOX");
    copy_text(replacement_folder.display_name, "Inbox");
    replacement_folder.uidvalidity = 81;
    assert(store.save_folder_metadata(replacement_folder, error));
    assert(store.get_folder(replacement_folder.key, replacement_folder, error));
    assert(replacement_folder.generation != old_folder_generation);
    assert(!store.save_message_guarded(summary, content, old_account_generation,
        old_folder_generation, error));

    const uint64_t replacement_generation = replacement.generation;
    const uint64_t replacement_folder_generation = replacement_folder.generation;
    assert(!store.update_folder_uidvalidity_guarded(replacement_folder.key, 82,
        replacement_generation, replacement_folder_generation, 80, error));
    Folder unchanged;
    assert(store.get_folder(replacement_folder.key, unchanged, error) && unchanged.uidvalidity == 81);
    assert(store.update_folder_uidvalidity_guarded(replacement_folder.key, 82,
        replacement_generation, replacement_folder_generation, 81, error));
    assert(store.delete_folder_guarded(replacement_folder.key, replacement_generation,
        replacement_folder_generation, error));
    Folder recreated;
    recreated.key = replacement_folder.key;
    copy_text(recreated.display_name, "Inbox");
    recreated.uidvalidity = 82;
    assert(store.save_folder_metadata(recreated, error));
    assert(store.get_folder(recreated.key, recreated, error));
    assert(recreated.generation != replacement_folder_generation);
    summary.key.folder = recreated.key;
    summary.key.uidvalidity = recreated.uidvalidity;
    assert(!store.save_message_guarded(summary, content, replacement_generation,
        replacement_folder_generation, error));
}

static void test_task5_header_coverage_signed_dates_and_send_recovery_backup() {
    ProfileFixture fixture("task5-durable");
    Text error;
    Store store;
    assert(store.open(fixture.paths, StoreOpenMode::Worker, error));
    Account account = sample_account();
    assert(store.save_account(account, error));
    Folder folder;
    folder.key.account_id = account.id;
    copy_text(folder.key.server_name, "INBOX");
    copy_text(folder.display_name, "Inbox");
    folder.uidvalidity = 19;
    assert(store.save_folder(folder, error));
    assert(store.get_folder(folder.key, folder, error));

    MessageSummary full;
    full.key.folder = folder.key; full.key.uidvalidity = 19; full.key.uid = 1;
    full.internal_date = -1; full.size = 114;
    copy_text(full.from, "alice@example.test"); copy_text(full.subject, "full cache");
    const char full_eml[] = "From: alice@example.test\r\nSubject: full cache\r\nMessage-ID: <full@example.test>\r\n\r\nbody";
    Bytes full_bytes;
    assert(full_bytes.set(full_eml, sizeof(full_eml) - 1));
    full.size = full_bytes.size();
    assert(store.save_message_guarded(full, full_bytes, account.generation, folder.generation, error));
    MessageSummary header = full;
    header.headers_only = true;
    const char header_eml[] = "From: alice@example.test\r\nSubject: full cache\r\nMessage-ID: <full@example.test>\r\n\r\n";
    Bytes header_bytes;
    assert(header_bytes.set(header_eml, sizeof(header_eml) - 1));
    assert(store.save_header_message_guarded(header, header_bytes, account.generation, folder.generation, error));
    MessageSummary loaded;
    Bytes loaded_bytes;
    assert(store.load_message(full.key, loaded, loaded_bytes, error));
    assert(!loaded.headers_only && loaded.internal_date == -1 && loaded_bytes == full_bytes);

    MessageSummary headers_only = full;
    headers_only.key.uid = 2;
    headers_only.internal_date = 0;
    headers_only.flags = 1;
    headers_only.headers_only = true;
    assert(store.save_header_message_guarded(headers_only, header_bytes,
        account.generation, folder.generation, error));
    MessageSummary body_match = full;
    body_match.key.uid = 3;
    body_match.internal_date = 1;
    body_match.flags = 1;
    copy_text(body_match.subject, "body match");
    const char matching_eml[] = "From: alice@example.test\r\nSubject: body match\r\n\r\nneedle in body";
    Bytes matching_bytes;
    assert(matching_bytes.set(matching_eml, sizeof(matching_eml) - 1));
    body_match.size = matching_bytes.size();
    assert(store.save_message_guarded(body_match, matching_bytes,
        account.generation, folder.generation, error));
    MessageSummary body_nonmatch = full;
    body_nonmatch.key.uid = 4;
    body_nonmatch.internal_date = 2;
    body_nonmatch.flags = 2;
    copy_text(body_nonmatch.subject, "body nonmatch");
    const char nonmatching_eml[] = "From: alice@example.test\r\nSubject: body nonmatch\r\n\r\nother text";
    Bytes nonmatching_bytes;
    assert(nonmatching_bytes.set(nonmatching_eml, sizeof(nonmatching_eml) - 1));
    body_nonmatch.size = nonmatching_bytes.size();
    assert(store.save_message_guarded(body_nonmatch, nonmatching_bytes,
        account.generation, folder.generation, error));
    MessageQuerySpec query;
    query.scope = MessageQueryScope::Folder;
    query.folder = folder.key;
    query.sort = MessageQuerySort::Oldest;
    MessageQueryPage page;
    assert(store.query_message_page(query, page, error));
    assert(page.total_matches == 4 && page.rows.size() == 4);
    assert(page.rows[0].message.key.uid == 1 && page.rows[0].message.internal_date == -1);
    assert(page.downloaded_body_count == 3 && page.headers_only_count == 1);
    query.search_field = MessageQueryField::Body;
    assert(query.query.set("needle"));
    query.required_flags = 1;
    query.limit = 1;
    assert(store.query_message_page(query, page, error));
    assert(page.total_matches == 1 && page.rows.size() == 1 && page.rows[0].message.key.uid == 3);
    assert(page.downloaded_body_count == 1 && page.headers_only_count == 1);
    query.offset = 1;
    assert(store.query_message_page(query, page, error));
    assert(page.total_matches == 1 && page.rows.empty() && !page.has_more);
    assert(page.downloaded_body_count == 1 && page.headers_only_count == 1);
    query.offset = 0;
    query.required_flags = 0;
    query.excluded_flags = 2;
    query.query.set("not-present");
    assert(store.query_message_page(query, page, error));
    assert(page.total_matches == 0 && page.rows.empty());
    assert(page.downloaded_body_count == 2 && page.headers_only_count == 1);

    Draft draft;
    draft.account_id = account.id;
    copy_text(draft.subject, "queued");
    assert(store.save_draft_guarded(draft, account.generation, 0, error));
    assert(draft.id && draft.revision == 1);
    copy_text(draft.body, "edited revision");
    assert(store.save_draft_guarded(draft, account.generation, draft.revision, error));
    assert(draft.revision == 2);
    copy_text(draft.body, "stale write");
    assert(!store.save_draft_guarded(draft, account.generation, 1, error));

    Draft guard_draft;
    guard_draft.account_id = account.id;
    copy_text(guard_draft.subject, "concurrent revision guards");
    assert(store.save_draft_guarded(guard_draft, account.generation, 0, error));
    Draft older_window;
    Draft newer_window;
    assert(store.get_draft(guard_draft.id, older_window, error));
    assert(store.get_draft(guard_draft.id, newer_window, error));
    copy_text(newer_window.body, "newer content");
    assert(store.save_draft_guarded(newer_window, account.generation,
        newer_window.revision, error));
    copy_text(older_window.body, "stale content");
    assert(!store.save_draft_guarded(older_window, account.generation,
        older_window.revision, error));
    Draft unchanged;
    assert(store.get_draft(guard_draft.id, unchanged, error));
    assert(unchanged.revision == newer_window.revision && unchanged.body == "newer content");
    copy_text(unchanged.body, "unguarded overwrite");
    assert(!store.save_draft_guarded(unchanged, account.generation, 0, error));
    assert(store.get_draft(guard_draft.id, unchanged, error));
    assert(unchanged.body == "newer content");

    Account other_account = sample_account();
    copy_text(other_account.email_address, "other@example.test");
    copy_text(other_account.username, "other-user");
    assert(store.save_account(other_account, error));
    Draft wrong_account;
    wrong_account.id = unchanged.id;
    wrong_account.revision = unchanged.revision;
    wrong_account.account_id = other_account.id;
    assert(!store.save_draft_guarded(wrong_account, other_account.generation,
        unchanged.revision, error));
    assert(store.get_draft(guard_draft.id, unchanged, error));
    assert(unchanged.body == "newer content");

    Draft deletion_draft;
    deletion_draft.account_id = account.id;
    copy_text(deletion_draft.subject, "revision-guarded delete");
    assert(store.save_draft_guarded(deletion_draft, account.generation, 0, error));
    assert(!store.delete_draft_guarded(deletion_draft.id, account.id, account.generation, 0, error));
    assert(!store.delete_draft_guarded(deletion_draft.id, other_account.id, other_account.generation,
        deletion_draft.revision, error));
    assert(!store.delete_draft_guarded(deletion_draft.id, account.id, account.generation,
        deletion_draft.revision + 1, error));
    assert(store.delete_draft_guarded(deletion_draft.id, account.id, account.generation,
        deletion_draft.revision, error));

    const char spool_text[] = "immutable finalized MIME bytes";
    Bytes spool;
    assert(spool.set(spool_text, sizeof(spool_text) - 1));
    SendOperation operation;
    Text message_id, sender, recipients;
    copy_text(message_id, "<task5@example.test>");
    copy_text(sender, "alice@example.test");
    copy_text(recipients, "bob@example.test");
    assert(store.create_send_operation(account.id, account.generation, draft.id, draft.revision,
        message_id, sender, recipients, account.sent_folder, spool, operation, error));
    assert(operation.stage == SendStage::Ready && operation.delivery_count == 0);
    SendOperation duplicate;
    Bytes duplicate_spool;
    bool found = false;
    assert(store.find_send_operation_for_draft(account.id, draft.id, draft.revision,
        found, duplicate, duplicate_spool, error));
    assert(found && duplicate.id == operation.id && duplicate_spool == spool);
    assert(!store.advance_send_operation(operation.id, account.generation,
        SendStage::AwaitingFinalResponse, false, error));
    assert(store.advance_send_operation(operation.id, account.generation, SendStage::Connecting, false, error));
    assert(store.advance_send_operation(operation.id, account.generation, SendStage::Authenticating, false, error));
    assert(store.advance_send_operation(operation.id, account.generation, SendStage::SendingEnvelope, false, error));
    assert(store.advance_send_operation(operation.id, account.generation, SendStage::SendingData, false, error));
    assert(store.advance_send_operation(operation.id, account.generation, SendStage::AwaitingFinalResponse, false, error));
    assert(store.advance_send_operation(operation.id, account.generation, SendStage::Accepted, true, error));
    assert(store.advance_send_operation(operation.id, account.generation, SendStage::ArchivePending, false, error));
    assert(store.advance_send_operation(operation.id, account.generation, SendStage::ArchiveFailed, false, error));
    assert(store.advance_send_operation(operation.id, account.generation, SendStage::ArchivePending, false, error));
    assert(store.advance_send_operation(operation.id, account.generation, SendStage::Archived, false, error));
    assert(!store.advance_send_operation(operation.id, account.generation, SendStage::Accepted, true, error));

    char backup_path[PATH_MAX];
    assert(snprintf(backup_path, sizeof(backup_path), "%s/task5.rmbak", fixture.root) > 0);
    assert(store.backup_profile(backup_path, error));
    assert(store.clear_cache(error));
    SendOperation after_clear;
    Bytes after_clear_spool;
    assert(store.get_send_operation(operation.id, after_clear, after_clear_spool, error));
    assert(after_clear.stage == SendStage::Archived && after_clear.delivery_count == 1);
    assert(after_clear_spool == spool);
    assert(store.restore_profile(backup_path, error));
    SendOperation after_restore;
    Bytes after_restore_spool;
    assert(store.get_send_operation(operation.id, after_restore, after_restore_spool, error));
    assert(after_restore.stage == SendStage::Archived && after_restore.delivery_count == 1);
    assert(after_restore_spool == spool);
}

static void test_task8a_archive_review_identity_is_durable() {
    ProfileFixture fixture("task8a-archive-review-schema");
    Text error;
    Store store;
    assert(store.open(fixture.paths, StoreOpenMode::Worker, error));

    char database[PATH_MAX];
    assert(snprintf(database, sizeof(database), "%s/mail.sqlite", fixture.paths.data_dir.c_str()) > 0);
    sqlite3 *raw = nullptr;
    assert(sqlite3_open(database, &raw) == SQLITE_OK);
    sqlite3_stmt *statement = nullptr;
    const char *sql = "SELECT archive_review_generation,archive_folder_generation,"
        "archive_folder_uidvalidity,last_diagnostic FROM send_operations LIMIT 0";
    const int status = sqlite3_prepare_v2(raw, sql, -1, &statement, nullptr);
    if (status != SQLITE_OK)
        fprintf(stderr, "Task8A archive review columns are unavailable: %s\n", sqlite3_errmsg(raw));
    assert(status == SQLITE_OK);
    assert(sqlite3_finalize(statement) == SQLITE_OK);
    assert(sqlite3_close(raw) == SQLITE_OK);

    Account account = sample_account();
    account.sent_folder.clear();
    assert(store.save_account(account, error));
    const uint64_t original_generation = account.generation;
    Folder sent;
    sent.key.account_id = account.id;
    copy_text(sent.key.server_name, "Sent");
    copy_text(sent.display_name, "Sent");
    sent.uidvalidity = 13;
    assert(store.save_folder_metadata(sent, error));
    assert(store.get_folder(sent.key, sent, error));

    Draft draft;
    draft.account_id = account.id;
    copy_text(draft.subject, "archive review identity");
    assert(store.save_draft_guarded(draft, original_generation, 0, error));
    Bytes spool;
    assert(spool.set("immutable delivered bytes", 25));
    SendOperation operation;
    Text message_id, sender, recipients;
    copy_text(message_id, "<archive-review@example.test>");
    copy_text(sender, "alice@example.test");
    copy_text(recipients, "bob@example.test");
    assert(store.create_send_operation(account.id, original_generation, draft.id, draft.revision,
        message_id, sender, recipients, account.sent_folder, spool, operation, error));
    assert(store.advance_send_operation(operation.id, original_generation, SendStage::Connecting, false, error));
    assert(store.advance_send_operation(operation.id, original_generation, SendStage::Authenticating, false, error));
    assert(store.advance_send_operation(operation.id, original_generation, SendStage::SendingEnvelope, false, error));
    assert(store.advance_send_operation(operation.id, original_generation, SendStage::SendingData, false, error));
    assert(store.advance_send_operation(operation.id, original_generation, SendStage::AwaitingFinalResponse, false, error));
    assert(store.advance_send_operation(operation.id, original_generation, SendStage::Accepted, true, error));

    account.sent_folder.set("Sent");
    assert(store.save_account_guarded(account, original_generation, error));
    assert(account.generation != original_generation);
    assert(!store.review_send_archive_target(operation.id, account.id, original_generation,
        original_generation, sent.key, sent.generation, sent.uidvalidity, error));

    Folder wrong_account_folder = sent;
    ++wrong_account_folder.key.account_id;
    assert(!store.review_send_archive_target(operation.id, account.id, original_generation,
        account.generation, wrong_account_folder.key, sent.generation, sent.uidvalidity, error));
    assert(!store.review_send_archive_target(operation.id, account.id, original_generation,
        account.generation, sent.key, sent.generation + 1, sent.uidvalidity, error));
    assert(store.review_send_archive_target(operation.id, account.id, original_generation,
        account.generation, sent.key, sent.generation, sent.uidvalidity, error));
    const uint64_t first_review_generation = account.generation;

    SendOperation reviewed;
    Bytes reviewed_spool;
    assert(store.get_send_operation(operation.id, reviewed, reviewed_spool, error));
    assert(reviewed.account_generation == original_generation);
    assert(reviewed.archive_review_generation == account.generation);
    assert(reviewed.archive_folder == "Sent");
    assert(reviewed.archive_folder_generation == sent.generation);
    assert(reviewed.archive_folder_uidvalidity == sent.uidvalidity);
    assert(reviewed.delivery_count == 1 && reviewed_spool == spool);
    assert(!store.advance_send_operation(operation.id, original_generation,
        SendStage::ArchivePending, false, error));
    assert(store.advance_send_operation(operation.id, account.generation,
        SendStage::ArchivePending, false, error));
    assert(store.advance_send_operation(operation.id, account.generation,
        SendStage::ArchiveFailed, false, error));
    Text diagnostic;
    copy_text(diagnostic, "Sent APPEND timed out; SMTP was not repeated");
    assert(store.set_send_operation_diagnostic(operation.id, account.generation, diagnostic, error));

    /* A user-reviewed uncertain delivery is eligible for a separately bound
     * archive destination, while delivery_count stays zero and SMTP remains
     * distinguishable from server-confirmed acceptance. */
    Draft uncertain_draft;
    uncertain_draft.account_id = account.id;
    copy_text(uncertain_draft.subject, "explicitly reviewed uncertain delivery");
    assert(store.save_draft_guarded(uncertain_draft, account.generation, 0, error));
    Bytes uncertain_spool;
    assert(uncertain_spool.set("reviewed delivered bytes", 24));
    SendOperation uncertain;
    copy_text(message_id, "<reviewed-uncertain@example.test>");
    assert(store.create_send_operation(account.id, account.generation, uncertain_draft.id,
        uncertain_draft.revision, message_id, sender, recipients, account.sent_folder,
        uncertain_spool, uncertain, error));
    assert(store.advance_send_operation(uncertain.id, account.generation,
        SendStage::Connecting, false, error));
    assert(store.advance_send_operation(uncertain.id, account.generation,
        SendStage::Authenticating, false, error));
    assert(store.advance_send_operation(uncertain.id, account.generation,
        SendStage::SendingEnvelope, false, error));
    assert(store.advance_send_operation(uncertain.id, account.generation,
        SendStage::SendingData, false, error));
    assert(store.advance_send_operation(uncertain.id, account.generation,
        SendStage::Uncertain, false, error));
    const uint64_t uncertain_operation_generation = account.generation;
    assert(account.display_name.set("Alice after Sent settings edit"));
    assert(store.save_account_guarded(account, uncertain_operation_generation, error));
    assert(account.generation != uncertain_operation_generation);
    assert(store.resolve_uncertain_send(uncertain.id, account.id, uncertain_operation_generation,
        account.generation,
        UncertainSendDecision::ConfirmDelivered, error));
    SendOperationSummary uncertain_summary;
    assert(store.get_send_operation_summary(uncertain.id, uncertain_summary, error));
    assert(uncertain_summary.stage == SendStage::ReviewedDelivered &&
        uncertain_summary.delivery_count == 0);
    assert(store.review_send_archive_target(uncertain.id, account.id, uncertain_operation_generation,
        account.generation, sent.key, sent.generation, sent.uidvalidity, error));
    assert(store.advance_send_operation(uncertain.id, account.generation,
        SendStage::ArchivePending, false, error));
    assert(store.get_send_operation_summary(uncertain.id, uncertain_summary, error));
    assert(uncertain_summary.stage == SendStage::ArchivePending &&
        uncertain_summary.delivery_count == 0 &&
        uncertain_summary.archive_review_generation == account.generation);
    assert(store.advance_send_operation(uncertain.id, account.generation,
        SendStage::ArchiveFailed, false, error));
    assert(store.get_send_operation_summary(uncertain.id, uncertain_summary, error));
    assert(uncertain_summary.stage == SendStage::ArchiveFailed && uncertain_summary.delivery_count == 0);
    assert(store.review_send_archive_target(uncertain.id, account.id, uncertain_operation_generation,
        account.generation, sent.key, sent.generation, sent.uidvalidity, error));
    assert(store.advance_send_operation(uncertain.id, account.generation,
        SendStage::ArchivePending, false, error));
    assert(store.get_send_operation_summary(uncertain.id, uncertain_summary, error));
    assert(uncertain_summary.stage == SendStage::ArchivePending && uncertain_summary.delivery_count == 0);

    SendOperationPage page;
    assert(store.list_send_operations(account.id, 0, page, error));
    assert(page.operations.size() == 2 && !page.has_more);
    bool found_original = false, found_reviewed_uncertain = false;
    for (size_t i = 0; i < page.operations.size(); ++i) {
        if (page.operations[i].id == operation.id) {
            found_original = true;
            assert(page.operations[i].archive_review_generation == first_review_generation);
            assert(page.operations[i].last_diagnostic == diagnostic);
        }
        if (page.operations[i].id == uncertain.id) {
            found_reviewed_uncertain = true;
            assert(page.operations[i].stage == SendStage::ArchivePending &&
                page.operations[i].delivery_count == 0);
        }
    }
    assert(found_original && found_reviewed_uncertain);

    char backup_path[PATH_MAX];
    assert(snprintf(backup_path, sizeof(backup_path), "%s/task8a-review.rmbak", fixture.root) > 0);
    assert(store.backup_profile(backup_path, error));
    if (!store.restore_profile(backup_path, error)) {
        fprintf(stderr, "Task8A send-review restore failed: %s\n", error.c_str());
        assert(false);
    }
    SendOperation restored;
    Bytes restored_spool;
    assert(store.get_send_operation(operation.id, restored, restored_spool, error));
    assert(restored.stage == SendStage::ArchiveFailed && restored.delivery_count == 1);
    assert(restored.archive_review_generation == first_review_generation &&
        restored.archive_folder_generation == sent.generation &&
        restored.archive_folder_uidvalidity == sent.uidvalidity);
    assert(restored.last_diagnostic == diagnostic && restored_spool == spool);
    store.close();
}

static void save_task6_query_message(Store &store, const Account &account, const Folder &folder,
    uint32_t uid, int64_t received_date, const char *subject, bool headers_only,
    AttachmentPresence attachments, Text &error) {
    MessageSummary summary;
    summary.key.folder = folder.key;
    summary.key.uidvalidity = folder.uidvalidity;
    summary.key.uid = uid;
    summary.from.set("sender@example.test");
    summary.subject.set(subject);
    summary.internal_date = received_date;
    summary.flags = 0;
    summary.headers_only = headers_only;
    const char full[] = "From: sender@example.test\r\nSubject: message\r\nContent-Type: text/plain; charset=utf-8\r\n\r\nbody";
    const char header[] = "From: sender@example.test\r\nSubject: header only\r\n\r\n";
    const char *bytes = headers_only ? header : full;
    const size_t size = headers_only ? sizeof(header) - 1 : sizeof(full) - 1;
    Bytes raw;
    assert(raw.set(bytes, size));
    summary.size = size;
    if (headers_only) {
        assert(store.save_header_message_guarded(summary, raw,
            account.generation, folder.generation, error));
    } else {
        assert(store.save_message_guarded(summary, raw, account.generation,
            folder.generation, attachments, error));
    }
}

static void test_task6_whole_query_filters_and_captured_row_generations() {
    ProfileFixture fixture("task6-query");
    Text error;
    Store store;
    assert(store.open(fixture.paths, StoreOpenMode::InstanceOwner, error));

    Account alice = sample_account();
    alice.email_address.set("alice@example.test");
    alice.username.set("alice");
    assert(store.save_account(alice, error));
    Account bob = sample_account();
    bob.email_address.set("bob@example.test");
    bob.username.set("bob");
    bob.display_name.set("Bob");
    bob.inbox_folder.set("Primary");
    bob.is_default = false;
    assert(store.save_account(bob, error));

    Folder alice_inbox;
    alice_inbox.key.account_id = alice.id;
    alice_inbox.key.server_name.set("INBOX");
    alice_inbox.display_name.set("收件箱");
    alice_inbox.role = FolderRole::Inbox;
    alice_inbox.uidvalidity = 2001;
    assert(store.save_folder(alice_inbox, error));
    assert(store.get_folder(alice_inbox.key, alice_inbox, error));
    Folder bob_inbox;
    bob_inbox.key.account_id = bob.id;
    bob_inbox.key.server_name.set("Primary");
    bob_inbox.display_name.set("Primary mailbox");
    bob_inbox.uidvalidity = 3001;
    assert(store.save_folder(bob_inbox, error));
    assert(store.get_folder(bob_inbox.key, bob_inbox, error));
    Folder bob_server_inbox;
    bob_server_inbox.key.account_id = bob.id;
    bob_server_inbox.key.server_name.set("INBOX");
    bob_server_inbox.display_name.set("Server Inbox alias");
    bob_server_inbox.role = FolderRole::Inbox;
    bob_server_inbox.uidvalidity = 3002;
    assert(store.save_folder(bob_server_inbox, error));
    assert(store.get_folder(bob_server_inbox.key, bob_server_inbox, error));
    Folder alice_reader_checks;
    alice_reader_checks.key.account_id = alice.id;
    alice_reader_checks.key.server_name.set("Reader checks");
    alice_reader_checks.display_name.set("Reader checks");
    alice_reader_checks.uidvalidity = 2002;
    assert(store.save_folder(alice_reader_checks, error));
    assert(store.get_folder(alice_reader_checks.key, alice_reader_checks, error));

    save_task6_query_message(store, alice, alice_inbox, 1, -5, "old signed date",
        false, AttachmentPresence::Some, error);
    save_task6_query_message(store, alice, alice_inbox, 2, 100, "no attachment",
        false, AttachmentPresence::None, error);
    save_task6_query_message(store, alice, alice_inbox, 3, 200, "header only",
        true, AttachmentPresence::Unknown, error);
    save_task6_query_message(store, bob, bob_inbox, 1, 150, "other account attachment",
        false, AttachmentPresence::Some, error);
    save_task6_query_message(store, bob, bob_server_inbox, 2, 175, "mapped account also has server Inbox",
        false, AttachmentPresence::None, error);
    save_task6_query_message(store, alice, alice_reader_checks, 4, 125, "non-Inbox folder must not join unified",
        false, AttachmentPresence::None, error);

    MessageQuerySpec all_mail_query;
    all_mail_query.scope = MessageQueryScope::AllMail;
    all_mail_query.limit = 10;
    MessageQueryPage all_mail_page;
    assert(store.query_message_page(all_mail_query, all_mail_page, error));
    assert(all_mail_page.total_matches == 6 && all_mail_page.rows.size() == 6);
    bool all_mail_has_reader_checks = false;
    bool all_mail_has_server_inbox = false;
    for (size_t i = 0; i < all_mail_page.rows.size(); ++i) {
        const MessageKey &key = all_mail_page.rows[i].message.key;
        if (key.folder == alice_reader_checks.key && key.uid == 4) all_mail_has_reader_checks = true;
        if (key.folder == bob_server_inbox.key && key.uid == 2) all_mail_has_server_inbox = true;
    }
    assert(all_mail_has_reader_checks && all_mail_has_server_inbox);

    MessageQuerySpec query;
    query.scope = MessageQueryScope::Unified;
    query.sort = MessageQuerySort::Oldest;
    query.has_received_after = true;
    query.received_after = 0;
    query.has_received_before = true;
    query.received_before = 200;
    query.limit = 1;
    MessageQueryPage page;
    assert(store.query_message_page(query, page, error));
    assert(page.total_matches == 3 && page.rows.size() == 1 && page.has_more);
    assert(page.rows[0].message.internal_date == 100);
    assert(page.downloaded_body_count == 2 && page.headers_only_count == 1);
    assert(page.rows[0].profile_epoch == 0 && page.rows[0].ui_generation == 0);
    assert(page.rows[0].account_generation == alice.generation);
    assert(page.rows[0].folder_generation == alice_inbox.generation);

    query.offset = 1;
    assert(store.query_message_page(query, page, error));
    assert(page.rows.size() == 1 && page.rows[0].message.key.folder.account_id == bob.id);
    assert(page.rows[0].message.key.folder == bob_inbox.key);
    assert(page.rows[0].account_generation == bob.generation);
    assert(page.rows[0].folder_generation == bob_inbox.generation);

    query.offset = 0;
    query.limit = 10;
    query.attachment_filter = AttachmentQueryFilter::HasAttachments;
    assert(store.query_message_page(query, page, error));
    assert(page.total_matches == 1 && page.rows.size() == 1);
    assert(page.rows[0].message.key.folder.account_id == bob.id);
    assert(page.rows[0].attachment_presence_known && page.rows[0].has_attachments);
    const MessageQueryRow stale_account_row = page.rows[0];

    query.attachment_filter = AttachmentQueryFilter::NoAttachments;
    assert(store.query_message_page(query, page, error));
    assert(page.total_matches == 1 && page.rows.size() == 1);
    assert(page.rows[0].message.key.uid == 2);
    assert(page.rows[0].attachment_presence_known && !page.rows[0].has_attachments);
    const MessageQueryRow stale_folder_row = page.rows[0];

    query.attachment_filter = AttachmentQueryFilter::Unknown;
    assert(store.query_message_page(query, page, error));
    assert(page.total_matches == 1 && page.rows.size() == 1);
    assert(page.rows[0].message.key.uid == 3 && page.rows[0].message.headers_only);
    assert(!page.rows[0].attachment_presence_known && !page.rows[0].has_attachments);

    query.attachment_filter = AttachmentQueryFilter::Any;
    query.received_after = -5;
    query.received_before = -5;
    assert(store.query_message_page(query, page, error));
    assert(page.total_matches == 1 && page.rows[0].message.internal_date == -5);
    query.received_after = 201;
    query.received_before = 200;
    assert(!store.query_message_page(query, page, error));

    bob.display_name.set("Bob renamed");
    const uint64_t old_bob_generation = bob.generation;
    assert(store.save_account_guarded(bob, old_bob_generation, error));
    assert(bob.generation != stale_account_row.account_generation);
    assert(!store.update_message_flags_guarded(stale_account_row.message.key, 1, 0,
        stale_account_row.account_generation, stale_account_row.folder_generation, error));

    const uint64_t old_folder_generation = alice_inbox.generation;
    assert(store.delete_folder_guarded(alice_inbox.key, alice.generation,
        old_folder_generation, error));
    Folder recreated = alice_inbox;
    recreated.generation = 0;
    assert(store.save_folder(recreated, error));
    assert(recreated.generation != stale_folder_row.folder_generation);
    assert(!store.update_message_flags_guarded(stale_folder_row.message.key, 1, 0,
        stale_folder_row.account_generation, stale_folder_row.folder_generation, error));
    store.close();
}

static bool attachment_source(void *context, size_t offset, unsigned char *buffer,
    size_t capacity, size_t *read_size) {
    const char *bytes = static_cast<const char *>(context);
    const size_t total = strlen(bytes);
    if (offset > total) return false;
    size_t count = total - offset;
    if (count > capacity) count = capacity;
    if (count) memcpy(buffer, bytes + offset, count);
    *read_size = count;
    return true;
}

static void test_incomplete_drafts_and_attachment_sources_survive_cache_clear() {
    ProfileFixture fixture("store-draft");
    Text error;
    Store store;
    assert(store.open(fixture.paths, StoreOpenMode::Worker, error));
    Account account = sample_account();
    assert(store.save_account(account, error));

    Folder folder;
    folder.key.account_id = account.id;
    copy_text(folder.key.server_name, "INBOX");
    copy_text(folder.display_name, "收件箱");
    folder.uidvalidity = 9;
    assert(store.save_folder(folder, error));
    MessageSummary summary;
    summary.key.folder = folder.key;
    summary.key.uidvalidity = 9;
    summary.key.uid = 1;
    copy_text(summary.subject, "cached");
    Bytes raw;
    assert(raw.set("body", 4));
    assert(store.save_message(summary, raw, error));

    Draft draft;
    draft.account_id = account.id;
    draft.raw_recipient_input = true;
    copy_text(draft.to_input, "李华 <unfinished");
    copy_text(draft.cc_input, "");
    copy_text(draft.bcc_input, "hidden@example.cn,");
    copy_text(draft.body, "尚未写完");
    copy_text(draft.html_body, "<p>草稿</p>");
    copy_text(draft.message_id, "<draft@example.cn>");
    draft.rich_format = true;
    draft.send_stage = SendStage::ArchiveFailed;
    const unsigned char opaque_editor_state[] = {'W', 0, 'D', 0xff, 'L'};
    assert(draft.editor_state.set(opaque_editor_state, sizeof(opaque_editor_state)));

    Attachment attachment;
    copy_text(attachment.filename, "附件.txt");
    copy_text(attachment.content_type, "text/plain");
    attachment.source.context = const_cast<char *>("draft attachment bytes");
    attachment.source.read = attachment_source;
    attachment.source.size = strlen(static_cast<const char *>(attachment.source.context));
    assert(draft.attachments.push(static_cast<Attachment &&>(attachment)));
    assert(store.save_draft(draft, error));
    assert(draft.id != 0);

    Draft loaded;
    assert(store.get_draft(draft.id, loaded, error));
    assert(loaded.raw_recipient_input && same_text(loaded.to_input, "李华 <unfinished"));
    assert(same_text(loaded.bcc_input, "hidden@example.cn,"));
    assert(loaded.subject.empty() && same_text(loaded.body, "尚未写完"));
    assert(loaded.rich_format && loaded.send_stage == SendStage::ArchiveFailed);
    assert(loaded.editor_state.size() == sizeof(opaque_editor_state));
    assert(memcmp(loaded.editor_state.data(), opaque_editor_state, sizeof(opaque_editor_state)) == 0);
    assert(loaded.attachments.size() == 1 && loaded.attachments[0].data.empty());
    assert(loaded.attachments[0].source.read && loaded.attachments[0].source.size == 22);
    unsigned char bytes[32]{};
    size_t read_size = 0;
    assert(loaded.attachments[0].source.read(loaded.attachments[0].source.context,
        0, bytes, sizeof(bytes), &read_size));
    assert(read_size == 22 && memcmp(bytes, "draft attachment bytes", 22) == 0);
    store.release_draft_sources(loaded);

    assert(store.clear_cache(error));
    assert(store.message_count(folder.key, error) == 0);
    Draft after_clear;
    assert(store.get_draft(draft.id, after_clear, error));
    assert(after_clear.send_stage == SendStage::ArchiveFailed);
    assert(same_text(after_clear.to_input, "李华 <unfinished"));
    assert(after_clear.attachments.size() == 1 && after_clear.attachments[0].source.read);
    store.release_draft_sources(after_clear);
}

static size_t process_fd_count() {
    DIR *directory = opendir("/proc/self/fd");
    assert(directory);
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)))
        if (entry->d_name[0] != '.') ++count;
    assert(closedir(directory) == 0);
    return count;
}

static void test_store_attachment_sources_have_shared_raii_ownership() {
    ProfileFixture fixture("store-draft-owner");
    Text error;
    Store store;
    assert(store.open(fixture.paths, StoreOpenMode::Worker, error));
    Account account = sample_account();
    assert(store.save_account(account, error));
    Draft draft;
    draft.account_id = account.id;
    copy_text(draft.subject, "owned attachment source");
    for (unsigned i = 0; i < 2; ++i) {
        Attachment attachment;
        copy_text(attachment.filename, i ? "second.txt" : "first.txt");
        copy_text(attachment.content_type, "text/plain");
        attachment.source.context = const_cast<char *>(i ? "second payload" : "first payload");
        attachment.source.read = attachment_source;
        attachment.source.size = strlen(static_cast<const char *>(attachment.source.context));
        assert(draft.attachments.push(static_cast<Attachment &&>(attachment)));
    }
    assert(store.save_draft(draft, error));

    Draft reused;
    assert(store.get_draft(draft.id, reused, error));
    const size_t after_first_get = process_fd_count();
    assert(store.get_draft(draft.id, reused, error));
    assert(process_fd_count() == after_first_get);
    Attachment source_copy = reused.attachments[0];
    Draft cloned;
    assert(clone_draft(reused, cloned, error));
    store.release_draft_sources(reused);

    DraftList listed;
    assert(store.list_drafts(account.id, listed, error));
    const size_t after_first_list = process_fd_count();
    assert(store.list_drafts(account.id, listed, error));
    assert(process_fd_count() == after_first_list);

    char database[PATH_MAX];
    assert(snprintf(database, sizeof(database), "%s/mail.sqlite", fixture.paths.data_dir.c_str()) > 0);
    sqlite3 *raw_db = nullptr;
    assert(sqlite3_open(database, &raw_db) == SQLITE_OK);
    sqlite3_stmt *query = nullptr;
    assert(sqlite3_prepare_v2(raw_db, "SELECT cache_name FROM draft_attachments WHERE draft_id=? "
        "ORDER BY position DESC LIMIT 1", -1, &query, nullptr) == SQLITE_OK);
    assert(sqlite3_bind_int64(query, 1, static_cast<sqlite3_int64>(draft.id)) == SQLITE_OK);
    assert(sqlite3_step(query) == SQLITE_ROW);
    char original_second_name[128];
    const unsigned char *stored_name = sqlite3_column_text(query, 0);
    const int stored_name_size = sqlite3_column_bytes(query, 0);
    assert(stored_name && stored_name_size > 0 && static_cast<size_t>(stored_name_size) < sizeof(original_second_name));
    memcpy(original_second_name, stored_name, static_cast<size_t>(stored_name_size));
    original_second_name[stored_name_size] = 0;
    assert(sqlite3_finalize(query) == SQLITE_OK);
    assert(sqlite3_exec(raw_db, "UPDATE draft_attachments SET cache_name='draft-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.bin' "
        "WHERE draft_id=(SELECT MAX(id) FROM drafts) AND position=1", nullptr, nullptr, nullptr) == SQLITE_OK);
    assert(sqlite3_close(raw_db) == SQLITE_OK);
    Draft failure_output;
    copy_text(failure_output.subject, "retain on failed load");
    const size_t before_failed_load = process_fd_count();
    assert(!store.get_draft(draft.id, failure_output, error));
    assert(same_text(failure_output.subject, "retain on failed load"));
    assert(process_fd_count() == before_failed_load);
    assert(sqlite3_open(database, &raw_db) == SQLITE_OK);
    char repair_sql[256];
    assert(snprintf(repair_sql, sizeof(repair_sql), "UPDATE draft_attachments SET cache_name='%s' "
        "WHERE draft_id=%llu AND position=1", original_second_name,
        static_cast<unsigned long long>(draft.id)) > 0);
    assert(sqlite3_exec(raw_db, repair_sql, nullptr, nullptr, nullptr) == SQLITE_OK);
    assert(sqlite3_close(raw_db) == SQLITE_OK);

    Draft moved = static_cast<Draft &&>(cloned);
    store.close();
    unsigned char bytes[32]{};
    size_t read_size = 0;
    assert(moved.attachments[0].source.read(moved.attachments[0].source.context,
        0, bytes, sizeof(bytes), &read_size));
    assert(read_size == strlen("first payload") && !memcmp(bytes, "first payload", read_size));
    memset(bytes, 0, sizeof(bytes));
    assert(source_copy.source.read(source_copy.source.context, 0, bytes, sizeof(bytes), &read_size));
    assert(read_size == strlen("first payload") && !memcmp(bytes, "first payload", read_size));
}

static void test_contacts_signatures_templates_and_rules_crud() {
    ProfileFixture fixture("store-tools");
    Text error;
    Store store;
    assert(store.open(fixture.paths, StoreOpenMode::Worker, error));

    Contact contact;
    copy_text(contact.display_name, "陈静");
    copy_text(contact.organization, "示例公司");
    copy_text(contact.notes, "中文备注");
    Address address;
    copy_text(address.local_part, "jing");
    copy_text(address.domain, "example.cn");
    assert(contact.addresses.push(static_cast<Address &&>(address)));
    assert(store.save_contact(contact, error));
    Contact loaded_contact;
    assert(store.get_contact(contact.id, loaded_contact, error));
    assert(same_text(loaded_contact.notes, "中文备注") && loaded_contact.addresses.size() == 1);
    ContactList contacts;
    assert(store.list_contacts(contacts, error) && contacts.size() == 1);
    assert(store.delete_contact(contact.id, error));

    Signature signature;
    copy_text(signature.name, "工作");
    copy_text(signature.body, "此致\n陈静");
    signature.enabled = true;
    assert(store.save_signature(signature, error));
    Signature loaded_signature;
    assert(store.get_signature(signature.id, loaded_signature, error));
    assert(same_text(loaded_signature.body, "此致\n陈静"));
    SignatureList signatures;
    assert(store.list_signatures(signatures, error) && signatures.size() == 1);
    assert(store.delete_signature(signature.id, error));

    Template message_template;
    copy_text(message_template.name, "回执");
    copy_text(message_template.subject, "已收到");
    copy_text(message_template.body, "谢谢");
    assert(store.save_template(message_template, error));
    Template loaded_template;
    assert(store.get_template(message_template.id, loaded_template, error));
    assert(same_text(loaded_template.subject, "已收到"));
    TemplateList templates;
    assert(store.list_templates(templates, error) && templates.size() == 1);
    assert(store.delete_template(message_template.id, error));

    Account account = sample_account();
    assert(store.save_account(account, error));
    Rule rule;
    rule.enabled = true;
    rule.field = RuleField::Subject;
    rule.operation = RuleOperator::Contains;
    copy_text(rule.value, "发票");
    rule.action = RuleAction::Move;
    rule.destination.account_id = account.id;
    copy_text(rule.destination.server_name, "收件箱/财务");
    assert(store.save_rule(rule, error));
    Rule loaded_rule;
    assert(store.get_rule(rule.id, loaded_rule, error));
    assert(same_text(loaded_rule.destination.server_name, "收件箱/财务"));
    RuleList rules;
    assert(store.list_rules(rules, error) && rules.size() == 1);
    assert(store.delete_rule(rule.id, error));
}

static void test_send_recovery_and_faults_preserve_committed_data() {
    ProfileFixture fixture("store-recovery");
    Text error;
    Store store;
    assert(store.open(fixture.paths, StoreOpenMode::Worker, error));
    Account account = sample_account();
    assert(store.save_account(account, error));
    Draft draft;
    draft.account_id = account.id;
    copy_text(draft.to_input, "not yet a complete address");
    draft.raw_recipient_input = true;
    assert(store.save_draft(draft, error));

    assert(store.set_send_stage(draft.id, SendStage::SendingData, error));
    assert(store.recover_interrupted_sends(error));
    Draft loaded;
    assert(store.get_draft(draft.id, loaded, error));
    assert(loaded.send_stage == SendStage::Uncertain);

    Folder folder;
    folder.key.account_id = account.id;
    copy_text(folder.key.server_name, "INBOX");
    copy_text(folder.display_name, "收件箱");
    folder.uidvalidity = 10;
    assert(store.save_folder(folder, error));
    MessageSummary summary;
    summary.key.folder = folder.key;
    summary.key.uidvalidity = 10;
    summary.key.uid = 8;
    Bytes old_raw;
    assert(old_raw.set("old", 3));
    assert(store.save_message(summary, old_raw, error));

    const StoreFaultPoint points[] = {StoreFaultPoint::CacheWrite,
        StoreFaultPoint::CacheRename, StoreFaultPoint::DatabaseCommit};
    for (size_t i = 0; i < 3; ++i) {
        Bytes replacement;
        assert(replacement.set("new", 3));
        store_test_fail_next(points[i]);
        assert(!store.save_message(summary, replacement, error));
        MessageSummary still_summary;
        Bytes still_raw;
        assert(store.load_message(summary.key, still_summary, still_raw, error));
        assert(still_raw == old_raw);
    }

    assert(store.set_send_stage(draft.id, SendStage::Accepted, error));
    store_test_fail_next(StoreFaultPoint::DatabaseCommit);
    assert(!store.set_send_stage(draft.id, SendStage::Archived, error));
    assert(store.get_draft(draft.id, loaded, error));
    assert(loaded.send_stage == SendStage::Accepted);
}

static void test_restore_rejects_malformed_persisted_model_rows_without_replacing_profile() {
    ProfileFixture source_fixture("restore-invalid-source");
    Text error;
    Store source;
    assert(source.open(source_fixture.paths, StoreOpenMode::Worker, error));
    Account source_account = sample_account();
    assert(source.save_account(source_account, error));

    Folder folder;
    folder.key.account_id = source_account.id;
    copy_text(folder.key.server_name, "INBOX");
    copy_text(folder.display_name, "Inbox");
    folder.role = FolderRole::Inbox;
    folder.uidvalidity = 1;
    assert(source.save_folder(folder, error));
    MessageSummary message;
    message.key.folder = folder.key;
    message.key.uidvalidity = 1;
    message.key.uid = 1;
    copy_text(message.from, "from@example.test");
    copy_text(message.subject, "valid message");
    Bytes raw;
    assert(raw.set("message", 7));
    assert(source.save_message(message, raw, error));

    Contact contact;
    copy_text(contact.display_name, "Valid Contact");
    copy_text(contact.organization, "Org");
    copy_text(contact.notes, "Notes");
    assert(source.save_contact(contact, error));
    Signature signature;
    copy_text(signature.name, "Valid Signature");
    copy_text(signature.body, "Best regards");
    assert(source.save_signature(signature, error));
    Template message_template;
    copy_text(message_template.name, "Valid Template");
    copy_text(message_template.subject, "Subject");
    copy_text(message_template.body, "Body");
    assert(source.save_template(message_template, error));
    Rule rule;
    copy_text(rule.value, "needle");
    assert(source.save_rule(rule, error));

    ProfileFixture destination_fixture("restore-invalid-destination");
    Store destination;
    assert(destination.open(destination_fixture.paths, StoreOpenMode::Worker, error));
    Account sentinel = sample_account();
    sentinel.is_default = true;
    copy_text(sentinel.display_name, "Keep this profile");
    assert(destination.save_account(sentinel, error));

    const char *invalid_sql[] = {
        "UPDATE folders SET role=99",
        "UPDATE messages SET subject=char(0)",
        "UPDATE contacts SET display_name=CAST(x'ff' AS TEXT)",
        "UPDATE signatures SET enabled=2",
        "UPDATE templates SET body=char(0)",
        "UPDATE rules SET action=99"
    };
    const char *repair_sql[] = {
        "UPDATE folders SET role=1",
        "UPDATE messages SET subject='valid message'",
        "UPDATE contacts SET display_name='Valid Contact'",
        "UPDATE signatures SET enabled=1",
        "UPDATE templates SET body='Body'",
        "UPDATE rules SET action=0"
    };
    char database[PATH_MAX];
    assert(snprintf(database, sizeof(database), "%s/mail.sqlite", source_fixture.paths.data_dir.c_str()) > 0);
    char backup_path[PATH_MAX];
    for (size_t i = 0; i < sizeof(invalid_sql) / sizeof(invalid_sql[0]); ++i) {
        sqlite3 *raw_db = nullptr;
        assert(sqlite3_open(database, &raw_db) == SQLITE_OK);
        assert(sqlite3_exec(raw_db, invalid_sql[i], nullptr, nullptr, nullptr) == SQLITE_OK);
        assert(sqlite3_close(raw_db) == SQLITE_OK);
        assert(snprintf(backup_path, sizeof(backup_path), "%s/invalid-%zu.rmbak", source_fixture.root, i) > 0);
        assert(source.backup_profile(backup_path, error));
        assert(!destination.restore_profile(backup_path, error));
        Account preserved;
        assert(destination.get_account(sentinel.id, preserved, error));
        assert(same_text(preserved.display_name, "Keep this profile"));
        assert(sqlite3_open(database, &raw_db) == SQLITE_OK);
        assert(sqlite3_exec(raw_db, repair_sql[i], nullptr, nullptr, nullptr) == SQLITE_OK);
        assert(sqlite3_close(raw_db) == SQLITE_OK);
        assert(unlink(backup_path) == 0);
    }

    sqlite3 *raw_db = nullptr;
    assert(sqlite3_open(database, &raw_db) == SQLITE_OK);
    assert(sqlite3_exec(raw_db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_stmt *insert_folder = nullptr;
    assert(sqlite3_prepare_v2(raw_db, "INSERT INTO folders(account_id,server_name,display_name,role,uidvalidity,"
        "message_count,unread_count) VALUES(?,?,?,0,1,0,0)", -1, &insert_folder, nullptr) == SQLITE_OK);
    for (size_t i = 0; i < MaxFoldersPerAccount; ++i) {
        char name[48];
        assert(snprintf(name, sizeof(name), "restore-folder-%04zu", i) > 0);
        assert(sqlite3_bind_int64(insert_folder, 1, static_cast<sqlite3_int64>(source_account.id)) == SQLITE_OK);
        assert(sqlite3_bind_text(insert_folder, 2, name, -1, SQLITE_TRANSIENT) == SQLITE_OK);
        assert(sqlite3_bind_text(insert_folder, 3, name, -1, SQLITE_TRANSIENT) == SQLITE_OK);
        assert(sqlite3_step(insert_folder) == SQLITE_DONE);
        assert(sqlite3_reset(insert_folder) == SQLITE_OK);
        assert(sqlite3_clear_bindings(insert_folder) == SQLITE_OK);
    }
    assert(sqlite3_finalize(insert_folder) == SQLITE_OK);
    assert(sqlite3_exec(raw_db, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK);
    assert(sqlite3_close(raw_db) == SQLITE_OK);
    assert(snprintf(backup_path, sizeof(backup_path), "%s/too-many-folders.rmbak", source_fixture.root) > 0);
    assert(source.backup_profile(backup_path, error));
    assert(!destination.restore_profile(backup_path, error));
    Account preserved;
    assert(destination.get_account(sentinel.id, preserved, error));
    assert(same_text(preserved.display_name, "Keep this profile"));
}

static bool has_prefixed_entry(const char *path, const char *prefix) {
    DIR *directory = opendir(path);
    assert(directory);
    bool found = false;
    struct dirent *entry;
    while ((entry = readdir(directory)))
        if (!strncmp(entry->d_name, prefix, strlen(prefix))) found = true;
    assert(closedir(directory) == 0);
    return found;
}

static void test_migration_backup_newer_schema_refusal_and_restore() {
    ProfileFixture fixture("store-migrate");
    Text error;
    create_data_profile_directory(fixture);
    char database[PATH_MAX];
    assert(snprintf(database, sizeof(database), "%s/mail.sqlite", fixture.paths.data_dir.c_str()) > 0);
    sqlite3 *legacy = nullptr;
    assert(sqlite3_open(database, &legacy) == SQLITE_OK);
    const char *legacy_sql =
        "CREATE TABLE accounts(id INTEGER PRIMARY KEY,display_name TEXT NOT NULL,email_address TEXT NOT NULL,"
        "username TEXT NOT NULL,incoming_host TEXT NOT NULL,outgoing_host TEXT NOT NULL,incoming_port INTEGER NOT NULL,"
        "outgoing_port INTEGER NOT NULL,incoming_protocol INTEGER NOT NULL,incoming_security INTEGER NOT NULL,"
        "outgoing_security INTEGER NOT NULL,authentication INTEGER NOT NULL);"
        "INSERT INTO accounts VALUES(17,'Legacy','legacy@example.cn','legacy','imap.example.cn','smtp.example.cn',"
        "993,587,0,0,1,0);PRAGMA user_version=1;";
    assert(sqlite3_exec(legacy, legacy_sql, nullptr, nullptr, nullptr) == SQLITE_OK);
    assert(sqlite3_close(legacy) == SQLITE_OK);

    Store migrated;
    assert(migrated.open(fixture.paths, StoreOpenMode::Worker, error));
    assert(has_prefixed_entry(fixture.paths.data_dir.c_str(), "mail.pre-migration-"));
    Account old;
    assert(migrated.get_account(17, old, error));
    assert(same_text(old.email_address, "legacy@example.cn"));
    assert(same_text(old.smtp_username, "legacy"));
    migrated.close();

    ProfileFixture future("store-future");
    create_data_profile_directory(future);
    assert(snprintf(database, sizeof(database), "%s/mail.sqlite", future.paths.data_dir.c_str()) > 0);
    assert(sqlite3_open(database, &legacy) == SQLITE_OK);
    assert(sqlite3_exec(legacy, "CREATE TABLE marker(value TEXT);INSERT INTO marker VALUES('future');"
        "PRAGMA user_version=999;", nullptr, nullptr, nullptr) == SQLITE_OK);
    assert(sqlite3_close(legacy) == SQLITE_OK);
    Store refused;
    assert(!refused.open(future.paths, StoreOpenMode::Worker, error));
    assert(has_prefixed_entry(future.paths.data_dir.c_str(), "mail.unsupported-"));

    ProfileFixture backup_fixture("store-backup");
    Store backup_store;
    assert(backup_store.open(backup_fixture.paths, StoreOpenMode::Worker, error));
    Account account = sample_account();
    assert(backup_store.save_account(account, error));
    char backup_path[PATH_MAX];
    assert(snprintf(backup_path, sizeof(backup_path), "%s/export.rmbak", backup_fixture.root) > 0);
    assert(backup_store.backup_profile(backup_path, error));
    assert(backup_store.delete_account(account.id, error));
    assert(backup_store.restore_profile(backup_path, error));
    Account restored;
    assert(backup_store.get_account(account.id, restored, error));
    assert(same_text(restored.display_name, "林小明"));
}

static void test_vault_backup_restore_requires_authentication_before_replacement() {
    Text error;
    ProfileFixture source_fixture("restore-vault-source");
    Store source;
    assert(source.open(source_fixture.paths, StoreOpenMode::Worker, error));
    Account source_account = sample_account();
    copy_text(source_account.display_name, "Authenticated source profile");
    assert(source.save_account(source_account, error));
    Folder source_folder;
    source_folder.key.account_id = source_account.id;
    copy_text(source_folder.key.server_name, "INBOX");
    copy_text(source_folder.display_name, "Inbox");
    source_folder.role = FolderRole::Inbox;
    source_folder.uidvalidity = 71;
    assert(source.save_folder(source_folder, error));
    MessageSummary source_message;
    source_message.key.folder = source_folder.key;
    source_message.key.uidvalidity = source_folder.uidvalidity;
    source_message.key.uid = 7;
    copy_text(source_message.from, "source@example.test");
    copy_text(source_message.subject, "restored cache");
    Bytes source_message_bytes;
    assert(source_message_bytes.set("source cache bytes", 18));
    assert(source.save_message(source_message, source_message_bytes, error));
    Vault source_vault;
    assert(source_vault.open(source_fixture.paths, error));
    SecretBytes source_credentials = make_secret("source credentials");
    assert(source_vault.save(make_secret("source master"), static_cast<SecretBytes &&>(source_credentials), error));
    char backup_path[PATH_MAX];
    char tampered_path[PATH_MAX];
    assert(snprintf(backup_path, sizeof(backup_path), "%s/source.rmbak", source_fixture.root) > 0);
    assert(snprintf(tampered_path, sizeof(tampered_path), "%s/tampered.rmbak", source_fixture.root) > 0);
    assert(source.backup_profile(backup_path, error));
    char source_database[PATH_MAX];
    assert(snprintf(source_database, sizeof(source_database), "%s/mail.sqlite",
        source_fixture.paths.data_dir.c_str()) > 0);
    sqlite3 *source_raw_db = nullptr;
    assert(sqlite3_open(source_database, &source_raw_db) == SQLITE_OK);
    sqlite3_stmt *source_cache_query = nullptr;
    assert(sqlite3_prepare_v2(source_raw_db, "SELECT cache_name FROM messages WHERE uid=7", -1,
        &source_cache_query, nullptr) == SQLITE_OK);
    assert(sqlite3_step(source_cache_query) == SQLITE_ROW);
    Text source_cache_name;
    const unsigned char *source_cache_text = sqlite3_column_text(source_cache_query, 0);
    const int source_cache_size = sqlite3_column_bytes(source_cache_query, 0);
    assert(source_cache_text && source_cache_size > 0 &&
        source_cache_name.set(reinterpret_cast<const char *>(source_cache_text), static_cast<size_t>(source_cache_size)));
    assert(sqlite3_finalize(source_cache_query) == SQLITE_OK);
    assert(sqlite3_close(source_raw_db) == SQLITE_OK);

    ProfileFixture destination_fixture("restore-vault-destination");
    Store destination;
    assert(destination.open(destination_fixture.paths, StoreOpenMode::Worker, error));
    Account sentinel = sample_account();
    copy_text(sentinel.display_name, "Keep destination profile");
    assert(destination.save_account(sentinel, error));
    Folder old_folder;
    old_folder.key.account_id = sentinel.id;
    copy_text(old_folder.key.server_name, "INBOX");
    copy_text(old_folder.display_name, "Old Inbox");
    old_folder.role = FolderRole::Inbox;
    old_folder.uidvalidity = 9;
    assert(destination.save_folder(old_folder, error));
    MessageSummary old_message;
    old_message.key.folder = old_folder.key;
    old_message.key.uidvalidity = old_folder.uidvalidity;
    old_message.key.uid = 2;
    copy_text(old_message.from, "old@example.test");
    copy_text(old_message.subject, "obsolete cache");
    Bytes old_message_bytes;
    assert(old_message_bytes.set("old cache bytes", 15));
    assert(destination.save_message(old_message, old_message_bytes, error));
    Vault destination_vault;
    assert(destination_vault.open(destination_fixture.paths, error));
    SecretBytes old_credentials = make_secret("old credentials");
    assert(destination_vault.save(make_secret("old master"), static_cast<SecretBytes &&>(old_credentials), error));
    char destination_db_path[PATH_MAX];
    char destination_vault_path[PATH_MAX];
    assert(snprintf(destination_db_path, sizeof(destination_db_path), "%s/mail.sqlite",
        destination_fixture.paths.data_dir.c_str()) > 0);
    assert(snprintf(destination_vault_path, sizeof(destination_vault_path), "%s/credentials.vault",
        destination_fixture.paths.config_dir.c_str()) > 0);
    char destination_database[PATH_MAX];
    assert(snprintf(destination_database, sizeof(destination_database), "%s/mail.sqlite",
        destination_fixture.paths.data_dir.c_str()) > 0);
    sqlite3 *destination_raw_db = nullptr;
    assert(sqlite3_open(destination_database, &destination_raw_db) == SQLITE_OK);
    sqlite3_stmt *destination_cache_query = nullptr;
    assert(sqlite3_prepare_v2(destination_raw_db, "SELECT cache_name FROM messages WHERE uid=2", -1,
        &destination_cache_query, nullptr) == SQLITE_OK);
    assert(sqlite3_step(destination_cache_query) == SQLITE_ROW);
    Text old_cache_name;
    const unsigned char *old_cache_text = sqlite3_column_text(destination_cache_query, 0);
    const int old_cache_size = sqlite3_column_bytes(destination_cache_query, 0);
    assert(old_cache_text && old_cache_size > 0 &&
        old_cache_name.set(reinterpret_cast<const char *>(old_cache_text), static_cast<size_t>(old_cache_size)));
    assert(sqlite3_finalize(destination_cache_query) == SQLITE_OK);
    assert(sqlite3_close(destination_raw_db) == SQLITE_OK);
    Bytes original_database;
    Bytes original_vault;
    assert(read_bytes_path(destination_db_path, original_database));
    assert(read_bytes_path(destination_vault_path, original_vault));

    assert(!destination.restore_profile(backup_path, error));
    Bytes unchanged_database;
    Bytes unchanged_vault;
    assert(read_bytes_path(destination_db_path, unchanged_database));
    assert(read_bytes_path(destination_vault_path, unchanged_vault));
    assert(unchanged_database == original_database && unchanged_vault == original_vault);

    assert(!destination.restore_profile(backup_path, make_secret("wrong master"), error));
    Bytes after_wrong_database;
    Bytes after_wrong_vault;
    assert(read_bytes_path(destination_db_path, after_wrong_database));
    assert(read_bytes_path(destination_vault_path, after_wrong_vault));
    assert(after_wrong_database == original_database && after_wrong_vault == original_vault);

    assert(make_tampered_vault_backup(backup_path, tampered_path));
    assert(!destination.restore_profile(tampered_path, make_secret("source master"), error));
    Bytes after_tamper_database;
    Bytes after_tamper_vault;
    assert(read_bytes_path(destination_db_path, after_tamper_database));
    assert(read_bytes_path(destination_vault_path, after_tamper_vault));
    assert(after_tamper_database == original_database && after_tamper_vault == original_vault);

    assert(destination.restore_profile_with_status(backup_path, make_secret("source master"), error) ==
        RestoreResult::Restored);
    Account restored;
    assert(destination.get_account(source_account.id, restored, error));
    assert(same_text(restored.display_name, "Authenticated source profile"));
    SecretBytes restored_credentials;
    assert(destination_vault.unlock(make_secret("source master"), restored_credentials, error));
    assert(restored_credentials.size() == strlen("source credentials"));
    assert(!memcmp(restored_credentials.data(), "source credentials", restored_credentials.size()));
    char old_cache_path[PATH_MAX];
    char source_cache_path[PATH_MAX];
    assert(snprintf(old_cache_path, sizeof(old_cache_path), "%s/%s", destination_fixture.paths.cache_dir.c_str(),
        old_cache_name.c_str()) > 0);
    assert(snprintf(source_cache_path, sizeof(source_cache_path), "%s/%s", destination_fixture.paths.cache_dir.c_str(),
        source_cache_name.c_str()) > 0);
    assert(access(old_cache_path, F_OK) < 0 && errno == ENOENT);
    assert(access(source_cache_path, F_OK) == 0);
    assert(!has_prefixed_entry(destination_fixture.paths.config_dir.c_str(), "rollback-"));
    assert(!has_prefixed_entry(destination_fixture.paths.data_dir.c_str(), "rollback-"));
    assert(!has_prefixed_entry(destination_fixture.paths.cache_dir.c_str(), "rollback-"));
    assert(!has_prefixed_entry(destination_fixture.paths.config_dir.c_str(), "restore-"));
    assert(!has_prefixed_entry(destination_fixture.paths.data_dir.c_str(), "restore-"));
    assert(!has_prefixed_entry(destination_fixture.paths.cache_dir.c_str(), "restore-"));
}

static void create_restore_crash_backup(ProfileFixture &fixture, char *backup_path, size_t backup_capacity) {
    Text error;
    Store source;
    assert(source.open(fixture.paths, StoreOpenMode::Worker, error));
    Account account = sample_account();
    copy_text(account.display_name, "new restore profile");
    assert(source.save_account(account, error));
    Folder folder;
    folder.key.account_id = account.id;
    copy_text(folder.key.server_name, "INBOX");
    copy_text(folder.display_name, "New Inbox");
    folder.role = FolderRole::Inbox;
    folder.uidvalidity = 71;
    assert(source.save_folder(folder, error));
    MessageSummary message;
    message.key.folder = folder.key;
    message.key.uidvalidity = folder.uidvalidity;
    message.key.uid = 7;
    copy_text(message.subject, "new committed message");
    Bytes message_bytes;
    assert(message_bytes.set("new cache", 9));
    assert(source.save_message(message, message_bytes, error));
    Vault vault;
    assert(vault.open(fixture.paths, error));
    assert(vault.save(make_secret("new master"), make_secret("new credentials"), error));
    assert(snprintf(backup_path, backup_capacity, "%s/restore-source.rmbak", fixture.root) > 0);
    assert(source.backup_profile(backup_path, error));
}

static void create_old_restore_profile(ProfileFixture &fixture) {
    Text error;
    Store old;
    assert(old.open(fixture.paths, StoreOpenMode::InstanceOwner, error));
    Account account = sample_account();
    copy_text(account.display_name, "old restore profile");
    assert(old.save_account(account, error));
    Folder folder;
    folder.key.account_id = account.id;
    copy_text(folder.key.server_name, "INBOX");
    copy_text(folder.display_name, "Old Inbox");
    folder.role = FolderRole::Inbox;
    folder.uidvalidity = 9;
    assert(old.save_folder(folder, error));
    MessageSummary message;
    message.key.folder = folder.key;
    message.key.uidvalidity = folder.uidvalidity;
    message.key.uid = 2;
    copy_text(message.subject, "old retained message");
    Bytes message_bytes;
    assert(message_bytes.set("old cache", 9));
    assert(old.save_message(message, message_bytes, error));
    Vault vault;
    assert(vault.open(fixture.paths, error));
    assert(vault.save(make_secret("old master"), make_secret("old credentials"), error));
    old.close();
}

static void run_restore_child_until_killed(const ProfilePaths &paths, const char *backup_path,
    uint32_t fault_mask) {
    const pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        Store restore;
        Text error;
        if (!restore.open(paths, StoreOpenMode::InstanceOwner, error)) _exit(31);
        store_test_set_restore_faults(fault_mask);
        (void)restore.restore_profile_with_status(backup_path, make_secret("new master"), error);
        _exit(32);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
}

static void run_owner_recovery_child_until_killed(const ProfilePaths &paths, uint32_t fault_mask) {
    const pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        store_test_set_restore_faults(fault_mask);
        Store owner;
        Text error;
        if (!owner.open(paths, StoreOpenMode::InstanceOwner, error)) _exit(41);
        _exit(42);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
}

static uint32_t test_restore_journal_crc(const unsigned char *bytes, size_t size) {
    uint32_t crc = UINT32_C(0xffffffff);
    for (size_t i = 0; i < size; ++i) {
        const unsigned char value = (i >= 20 && i < 24) ? 0 : bytes[i];
        crc ^= value;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0u - (crc & 1u)));
    }
    return ~crc;
}

static bool rewrite_journal_with_unsafe_target(const char *path) {
    Bytes contents;
    if (!read_bytes_path(path, contents) || contents.size() < 24 ||
        memcmp(contents.data(), "RMRJNL01", 8)) return false;
    unsigned char *copy = static_cast<unsigned char *>(malloc(contents.size()));
    if (!copy) return false;
    memcpy(copy, contents.data(), contents.size());
    const uint32_t count = (static_cast<uint32_t>(copy[12]) << 24) |
        (static_cast<uint32_t>(copy[13]) << 16) | (static_cast<uint32_t>(copy[14]) << 8) | copy[15];
    size_t at = 24;
    bool changed = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (contents.size() - at < 10) break;
        const unsigned char directory = copy[at];
        const size_t target_size = (static_cast<size_t>(copy[at + 4]) << 8) | copy[at + 5];
        const size_t temporary_size = (static_cast<size_t>(copy[at + 6]) << 8) | copy[at + 7];
        const size_t rollback_size = (static_cast<size_t>(copy[at + 8]) << 8) | copy[at + 9];
        at += 10;
        const size_t names_size = target_size + temporary_size + rollback_size;
        if (names_size > contents.size() - at) break;
        if (directory == 2 && target_size == strlen("mail.sqlite") &&
            !memcmp(copy + at, "mail.sqlite", target_size)) {
            static const char unsafe[] = "../evil.txt";
            if (sizeof(unsafe) - 1 == target_size) {
                memcpy(copy + at, unsafe, target_size);
                changed = true;
                break;
            }
        }
        at += names_size;
    }
    if (changed) {
        copy[20] = copy[21] = copy[22] = copy[23] = 0;
        const uint32_t crc = test_restore_journal_crc(copy, contents.size());
        copy[20] = static_cast<unsigned char>(crc >> 24);
        copy[21] = static_cast<unsigned char>(crc >> 16);
        copy[22] = static_cast<unsigned char>(crc >> 8);
        copy[23] = static_cast<unsigned char>(crc);
    }
    int fd = changed ? open(path, O_WRONLY | O_TRUNC | O_CLOEXEC | O_NOFOLLOW) : -1;
    bool ok = fd >= 0;
    size_t written = 0;
    while (ok && written < contents.size()) {
        const ssize_t amount = write(fd, copy + written, contents.size() - written);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) { ok = false; break; }
        written += static_cast<size_t>(amount);
    }
    if (ok && fsync(fd) < 0) ok = false;
    if (fd >= 0 && close(fd) < 0) ok = false;
    free(copy);
    return ok;
}

static void assert_no_restore_artifacts(const ProfilePaths &paths) {
    const char *directories[] = {paths.config_dir.c_str(), paths.data_dir.c_str(), paths.cache_dir.c_str()};
    size_t cache_files = 0;
    for (size_t directory_index = 0; directory_index < 3; ++directory_index) {
        DIR *directory = opendir(directories[directory_index]);
        assert(directory);
        struct dirent *entry;
        while ((entry = readdir(directory))) {
            assert(strncmp(entry->d_name, "restore-", 8));
            assert(strncmp(entry->d_name, "rollback-", 9));
            assert(strncmp(entry->d_name, ".restore-journal-", 17));
            const size_t length = strlen(entry->d_name);
            if (directory_index == 2 && length >= 4 && !strcmp(entry->d_name + length - 4, ".bin"))
                ++cache_files;
        }
        assert(closedir(directory) == 0);
    }
    assert(cache_files == 1);
}

static void assert_restore_profile_state(const ProfilePaths &paths, bool expect_new) {
    Text error;
    Store owner;
    assert(owner.open(paths, StoreOpenMode::InstanceOwner, error));
    Account account;
    assert(owner.get_account(1, account, error));
    assert(same_text(account.display_name, expect_new ? "new restore profile" : "old restore profile"));
    FolderKey folder;
    folder.account_id = account.id;
    copy_text(folder.server_name, "INBOX");
    assert(owner.message_count(folder, error) == 1);
    MessageKey message_key;
    message_key.folder = folder;
    message_key.uidvalidity = expect_new ? 71 : 9;
    message_key.uid = expect_new ? 7 : 2;
    MessageSummary message;
    Bytes message_bytes;
    assert(owner.load_message(message_key, message, message_bytes, error));
    assert(same_text(message.subject, expect_new ? "new committed message" : "old retained message"));
    const char *expected_bytes = expect_new ? "new cache" : "old cache";
    assert(message_bytes.size() == strlen(expected_bytes));
    assert(!memcmp(message_bytes.data(), expected_bytes, message_bytes.size()));
    Vault vault;
    assert(vault.open(paths, error));
    SecretBytes credentials;
    const char *password = expect_new ? "new master" : "old master";
    const char *expected_credentials = expect_new ? "new credentials" : "old credentials";
    assert(vault.unlock(make_secret(password), credentials, error));
    assert(credentials.size() == strlen(expected_credentials));
    assert(!memcmp(credentials.data(), expected_credentials, credentials.size()));
    char journal[PATH_MAX];
    assert(snprintf(journal, sizeof(journal), "%s/.restore-journal", paths.config_dir.c_str()) > 0);
    struct stat info;
    assert(lstat(journal, &info) < 0 && errno == ENOENT);
    assert_no_restore_artifacts(paths);
}

static void test_restore_journal_recovers_killed_and_interrupted_rollback() {
    ProfileFixture source_fixture("restore-journal-source");
    char backup_path[PATH_MAX];
    create_restore_crash_backup(source_fixture, backup_path, sizeof(backup_path));
    ProfileFixture destination_fixture("restore-journal-rollback");
    create_old_restore_profile(destination_fixture);

    run_restore_child_until_killed(destination_fixture.paths, backup_path,
        static_cast<uint32_t>(StoreRestoreFault::CrashAfterVaultInstall));
    Text error;
    Store worker;
    assert(!worker.open(destination_fixture.paths, StoreOpenMode::Worker, error));
    assert(strstr(error.c_str(), "restore"));

    run_owner_recovery_child_until_killed(destination_fixture.paths,
        static_cast<uint32_t>(StoreRestoreFault::CrashDuringRollbackCleanup));
    Store still_pending_worker;
    assert(!still_pending_worker.open(destination_fixture.paths, StoreOpenMode::Worker, error));
    assert(strstr(error.c_str(), "restore"));

    assert_restore_profile_state(destination_fixture.paths, false);
    Store worker_after_recovery;
    assert(worker_after_recovery.open(destination_fixture.paths, StoreOpenMode::Worker, error));
}

static void test_restore_journal_recovers_commit_and_cleanup_interruptions() {
    ProfileFixture source_fixture("restore-journal-commit-source");
    char backup_path[PATH_MAX];
    create_restore_crash_backup(source_fixture, backup_path, sizeof(backup_path));
    const uint32_t crash_points[] = {
        static_cast<uint32_t>(StoreRestoreFault::CrashAfterDatabaseInstall),
        static_cast<uint32_t>(StoreRestoreFault::CrashAfterCommitPersisted),
        static_cast<uint32_t>(StoreRestoreFault::CrashDuringCommittedCleanup)
    };
    const char *tags[] = {"restore-journal-before-commit", "restore-journal-committed",
        "restore-journal-cleanup"};
    for (size_t index = 0; index < sizeof(crash_points) / sizeof(crash_points[0]); ++index) {
        ProfileFixture destination_fixture(tags[index]);
        create_old_restore_profile(destination_fixture);
        run_restore_child_until_killed(destination_fixture.paths, backup_path, crash_points[index]);
        if (index == 1)
            run_owner_recovery_child_until_killed(destination_fixture.paths,
                static_cast<uint32_t>(StoreRestoreFault::CrashDuringCommittedCleanup));
        assert_restore_profile_state(destination_fixture.paths, index != 0);
    }
}

static void test_restore_journal_incomplete_rollback_is_explicit_and_resumable() {
    ProfileFixture source_fixture("restore-journal-failed-source");
    char backup_path[PATH_MAX];
    create_restore_crash_backup(source_fixture, backup_path, sizeof(backup_path));
    ProfileFixture destination_fixture("restore-journal-failed-destination");
    create_old_restore_profile(destination_fixture);
    Text error;
    Store destination;
    assert(destination.open(destination_fixture.paths, StoreOpenMode::InstanceOwner, error));
    store_test_set_restore_faults(static_cast<uint32_t>(StoreRestoreFault::FailDatabaseInstall) |
        static_cast<uint32_t>(StoreRestoreFault::FailRollbackSync));
    assert(destination.restore_profile_with_status(backup_path, make_secret("new master"), error) ==
        RestoreResult::RecoveryRequired);
    assert(!destination.is_open());
    assert(strstr(error.c_str(), "rollback"));
    destination.close();

    Store worker;
    assert(!worker.open(destination_fixture.paths, StoreOpenMode::Worker, error));
    assert(strstr(error.c_str(), "restore"));
    assert_restore_profile_state(destination_fixture.paths, false);

    ProfileFixture bool_fixture("restore-journal-bool-wrapper");
    create_old_restore_profile(bool_fixture);
    Store bool_destination;
    assert(bool_destination.open(bool_fixture.paths, StoreOpenMode::InstanceOwner, error));
    store_test_set_restore_faults(static_cast<uint32_t>(StoreRestoreFault::FailDatabaseInstall) |
        static_cast<uint32_t>(StoreRestoreFault::FailRollbackSync));
    assert(!bool_destination.restore_profile(backup_path, make_secret("new master"), error));
    assert(!bool_destination.is_open());
    bool_destination.close();
    assert_restore_profile_state(bool_fixture.paths, false);
}

static void test_restore_journal_malformed_marker_blocks_owner_and_worker() {
    ProfileFixture fixture("restore-journal-malformed");
    Text error;
    Store initial;
    assert(initial.open(fixture.paths, StoreOpenMode::Worker, error));
    initial.close();
    char journal[PATH_MAX];
    assert(snprintf(journal, sizeof(journal), "%s/.restore-journal", fixture.paths.config_dir.c_str()) > 0);
    int fd = open(journal, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    assert(fd >= 0);
    const char malformed[] = "not a restore journal";
    assert(write(fd, malformed, sizeof(malformed) - 1) == static_cast<ssize_t>(sizeof(malformed) - 1));
    assert(fsync(fd) == 0 && close(fd) == 0);
    Store worker;
    assert(!worker.open(fixture.paths, StoreOpenMode::Worker, error));
    Store owner;
    assert(!owner.open(fixture.paths, StoreOpenMode::InstanceOwner, error));
}

static void test_restore_journal_rejects_valid_checksum_unsafe_component_path() {
    ProfileFixture source_fixture("restore-journal-unsafe-source");
    char backup_path[PATH_MAX];
    create_restore_crash_backup(source_fixture, backup_path, sizeof(backup_path));
    ProfileFixture destination_fixture("restore-journal-unsafe-destination");
    create_old_restore_profile(destination_fixture);
    run_restore_child_until_killed(destination_fixture.paths, backup_path,
        static_cast<uint32_t>(StoreRestoreFault::CrashAfterVaultInstall));
    char journal[PATH_MAX];
    assert(snprintf(journal, sizeof(journal), "%s/.restore-journal",
        destination_fixture.paths.config_dir.c_str()) > 0);
    assert(rewrite_journal_with_unsafe_target(journal));
    Text error;
    Store owner;
    assert(!owner.open(destination_fixture.paths, StoreOpenMode::InstanceOwner, error));
    assert(strstr(error.c_str(), "journal") || strstr(error.c_str(), "component"));
    Store worker;
    assert(!worker.open(destination_fixture.paths, StoreOpenMode::Worker, error));
    char escaped[PATH_MAX];
    assert(snprintf(escaped, sizeof(escaped), "%s/../evil.txt",
        destination_fixture.paths.config_dir.c_str()) > 0);
    struct stat info;
    assert(lstat(escaped, &info) < 0 && errno == ENOENT);
}

struct BackupRacePipes { int request_fd; int response_fd; };

static void replace_cache_after_snapshot(void *opaque) {
    BackupRacePipes *pipes = static_cast<BackupRacePipes *>(opaque);
    const char request = 'R';
    assert(write(pipes->request_fd, &request, 1) == 1);
    char response = 0;
    assert(read(pipes->response_fd, &response, 1) == 1 && response == 'D');
}

static void test_backup_snapshot_cache_race_fails_without_publishing() {
    ProfileFixture fixture("store-backup-race");
    Text error;
    Store owner;
    assert(owner.open(fixture.paths, StoreOpenMode::Worker, error));
    Account account = sample_account();
    assert(owner.save_account(account, error));
    Folder folder;
    folder.key.account_id = account.id;
    copy_text(folder.key.server_name, "INBOX");
    copy_text(folder.display_name, "Inbox");
    folder.role = FolderRole::Inbox;
    folder.uidvalidity = 5;
    assert(owner.save_folder(folder, error));
    MessageSummary summary;
    summary.key.folder = folder.key;
    summary.key.uidvalidity = folder.uidvalidity;
    summary.key.uid = 19;
    copy_text(summary.from, "sender@example.test");
    copy_text(summary.subject, "snapshot version");
    Bytes original;
    assert(original.set("old", 3));
    assert(owner.save_message(summary, original, error));

    int request_pipe[2], response_pipe[2];
    assert(pipe(request_pipe) == 0 && pipe(response_pipe) == 0);
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(request_pipe[1]);
        close(response_pipe[0]);
        char request = 0;
        if (read(request_pipe[0], &request, 1) != 1 || request != 'R') _exit(21);
        Store updater;
        Text child_error;
        if (!updater.open(fixture.paths, StoreOpenMode::Worker, child_error)) _exit(22);
        Bytes replacement;
        if (!replacement.set("new version", 11) || !updater.save_message(summary, replacement, child_error)) _exit(23);
        const char response = 'D';
        if (write(response_pipe[1], &response, 1) != 1) _exit(24);
        updater.close();
        _exit(0);
    }
    close(request_pipe[0]);
    close(response_pipe[1]);
    BackupRacePipes pipes{request_pipe[1], response_pipe[0]};
    store_test_set_backup_hook(replace_cache_after_snapshot, &pipes);
    char backup_path[PATH_MAX];
    assert(snprintf(backup_path, sizeof(backup_path), "%s/raced.rmbak", fixture.root) > 0);
    assert(!owner.backup_profile(backup_path, error));
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(request_pipe[1]);
    close(response_pipe[0]);
    struct stat backup_info;
    assert(lstat(backup_path, &backup_info) < 0 && errno == ENOENT);
}

static void test_sqlite_real_process_lock_and_killed_writer_rollback() {
    ProfileFixture fixture("store-process");
    Text error;
    Store store;
    assert(store.open(fixture.paths, StoreOpenMode::InstanceOwner, error));
    Account account = sample_account();
    assert(store.save_account(account, error));
    Draft draft;
    draft.account_id = account.id;
    copy_text(draft.subject, "durable accepted state");
    assert(store.save_draft(draft, error));
    assert(store.set_send_stage(draft.id, SendStage::Accepted, error));

    char database[PATH_MAX];
    assert(snprintf(database, sizeof(database), "%s/mail.sqlite", fixture.paths.data_dir.c_str()) > 0);
    int ready[2];
    assert(pipe(ready) == 0);
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(ready[0]);
        sqlite3 *db = nullptr;
        if (sqlite3_open(database, &db) != SQLITE_OK) _exit(10);
        sqlite3_busy_timeout(db, 1000);
        if (sqlite3_exec(db, "BEGIN IMMEDIATE;UPDATE drafts SET subject='uncommitted child value';",
                nullptr, nullptr, nullptr) != SQLITE_OK) _exit(11);
        const char ready_byte = 'L';
        if (write(ready[1], &ready_byte, 1) != 1) _exit(12);
        for (;;) pause();
    }
    close(ready[1]);
    char ready_byte = 0;
    assert(read(ready[0], &ready_byte, 1) == 1 && ready_byte == 'L');
    close(ready[0]);

    assert(!store.set_send_stage(draft.id, SendStage::Archived, error));
    assert(kill(child, SIGKILL) == 0);
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status));
    assert(store.set_send_stage(draft.id, SendStage::Accepted, error));
    store.close();

    Store reopened;
    assert(reopened.open(fixture.paths, StoreOpenMode::Worker, error));
    Draft recovered;
    assert(reopened.get_draft(draft.id, recovered, error));
    assert(same_text(recovered.subject, "durable accepted state"));
    assert(recovered.send_stage == SendStage::Accepted);
    assert(reopened.recover_interrupted_sends(error));
    assert(reopened.get_draft(draft.id, recovered, error));
    assert(recovered.send_stage == SendStage::ArchivePending);
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--task8a")) {
        test_task8a_archive_review_identity_is_durable();
        puts("ReliefMail Task8A store suite passed");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--task5")) {
        test_task6_whole_query_filters_and_captured_row_generations();
        test_task5_stale_account_and_folder_generations_are_rejected();
        test_task5_header_coverage_signed_dates_and_send_recovery_backup();
        test_send_recovery_and_faults_preserve_committed_data();
        test_migration_backup_newer_schema_refusal_and_restore();
        test_restore_rejects_malformed_persisted_model_rows_without_replacing_profile();
        test_restore_journal_recovers_commit_and_cleanup_interruptions();
        test_restore_journal_incomplete_rollback_is_explicit_and_resumable();
        test_restore_journal_malformed_marker_blocks_owner_and_worker();
        test_restore_journal_rejects_valid_checksum_unsafe_component_path();
        test_backup_snapshot_cache_race_fails_without_publishing();
        puts("ReliefMail Task5 store suite passed");
        return 0;
    }
    test_profile_paths_follow_xdg_and_home_fallback();
    test_profile_modes_and_owner_worker_locking();
    test_store_rejects_symlink_and_unowned_profile_directories();
    test_account_folder_message_and_search_crud();
    test_account_folder_and_message_write_limits_are_atomic();
    test_empty_uidvalidity_replacement_is_explicit_and_atomic();
    test_incomplete_drafts_and_attachment_sources_survive_cache_clear();
    test_store_attachment_sources_have_shared_raii_ownership();
    test_contacts_signatures_templates_and_rules_crud();
    test_send_recovery_and_faults_preserve_committed_data();
    test_restore_rejects_malformed_persisted_model_rows_without_replacing_profile();
    test_migration_backup_newer_schema_refusal_and_restore();
    test_vault_backup_restore_requires_authentication_before_replacement();
    test_restore_journal_recovers_killed_and_interrupted_rollback();
    test_restore_journal_recovers_commit_and_cleanup_interruptions();
    test_restore_journal_incomplete_rollback_is_explicit_and_resumable();
    test_restore_journal_malformed_marker_blocks_owner_and_worker();
    test_restore_journal_rejects_valid_checksum_unsafe_component_path();
    test_backup_snapshot_cache_race_fails_without_publishing();
    test_sqlite_real_process_lock_and_killed_writer_rollback();
    puts("ReliefMail store suite passed");
    return 0;
}
