#include "imap.h"
#include "mime.h"
#include "smtp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

using namespace rmail;

static uint32_t endpoint_port(const char *ready_file, const char *wanted) {
    FILE *file = fopen(ready_file, "r");
    assert(file);
    char name[96];
    unsigned int port = 0;
    unsigned int found = 0;
    while (fscanf(file, "%95s %u", name, &port) == 2) {
        if (!strcmp(name, wanted)) {
            found = port;
            break;
        }
    }
    fclose(file);
    assert(found > 0 && found <= 65535);
    return found;
}

static bool text_is(const Text &text, const char *expected) {
    const size_t length = strlen(expected);
    return text.size() == length && (!length || !memcmp(text.c_str(), expected, length));
}

static bool bytes_contain(const Bytes &bytes, const char *needle) {
    const size_t length = strlen(needle);
    if (!length || length > bytes.size()) return false;
    for (size_t i = 0; i + length <= bytes.size(); ++i)
        if (!memcmp(bytes.data() + i, needle, length)) return true;
    return false;
}

static uint64_t monotonic_ms();

static OperationContext test_context() {
    OperationContext context;
    context.deadline_monotonic_ms = monotonic_ms() + 30000;
    return context;
}

static uint64_t monotonic_ms() {
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return static_cast<uint64_t>(now.tv_sec) * 1000u +
        static_cast<uint64_t>(now.tv_nsec / 1000000u);
}

static bool cancel_at_time(void *opaque) {
    const uint64_t *cancel_at = static_cast<const uint64_t *>(opaque);
    return cancel_at && monotonic_ms() >= *cancel_at;
}

static SecretView fixture_password() {
    static const unsigned char password[] = "phase-a-fixture-password";
    SecretView view;
    view.data = password;
    view.size = sizeof(password) - 1;
    return view;
}

static Account imap_account(uint32_t port, TransportSecurity security, const char *ca_path) {
    Account account;
    account.id = 42;
    account.incoming_protocol = IncomingProtocol::Imap;
    account.incoming_security = security;
    account.incoming_port = port;
    assert(account.incoming_host.set("localhost"));
    assert(account.username.set("alice@reliefmail.test"));
    assert(account.custom_ca_file.set(ca_path));
    return account;
}

static Account smtp_account(uint32_t port, TransportSecurity security, const char *ca_path) {
    Account account;
    account.id = 77;
    account.outgoing_security = security;
    account.outgoing_port = port;
    assert(account.outgoing_host.set("localhost"));
    assert(account.username.set("alice@reliefmail.test"));
    assert(account.smtp_username.set("alice@reliefmail.test"));
    assert(account.custom_ca_file.set(ca_path));
    return account;
}

static bool cancelled_now(void *) { return true; }

/* A bundle can contain an unsupported certificate alongside a trusted root.
 * Rejecting mbedTLS's positive partial-success return must not prevent a
 * verified connection through the usable root, nor bypass identity checks. */
static void test_transport_accepts_usable_roots_in_a_partial_bundle(const char *ready,
    const char *ca_path) {
    char path[4096];
    const char *temporary = getenv("TMPDIR");
    assert(temporary && *temporary);
    assert(snprintf(path, sizeof(path), "%s/mail-partial-ca.XXXXXX", temporary) > 0);
    const int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *output = fdopen(fd, "wb");
    FILE *input = fopen(ca_path, "rb");
    assert(output && input);
    unsigned char buffer[4096];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), input)) > 0)
        assert(fwrite(buffer, 1, count, output) == count);
    assert(!ferror(input));
    assert(fclose(input) == 0);
    assert(fputs("\n-----BEGIN CERTIFICATE-----\nAQID\n-----END CERTIFICATE-----\n", output) >= 0);
    assert(fclose(output) == 0);

    Text error;
    Stream valid;
    const bool connected = valid.connect("localhost", endpoint_port(ready, "tls-valid"),
        TransportSecurity::ImplicitTls, path, test_context(), error);
    if (!connected) fprintf(stderr, "partial CA bundle: %s\n", error.c_str());
    assert(connected);
    TransportDiagnostics diagnostics;
    assert(valid.diagnostics(diagnostics) && diagnostics.certificate_verified);
    assert(diagnostics.certificate_verify_flags == 0);
    valid.close();

    Stream wrong_host;
    assert(!wrong_host.connect("127.0.0.1", endpoint_port(ready, "tls-valid"),
        TransportSecurity::ImplicitTls, path, test_context(), error));
    assert(strstr(error.c_str(), "hostname"));
    assert(transport_problem(error) == TransportProblem::HostnameMismatch);
    assert(unlink(path) == 0);
}

static void test_transport_reports_unusable_ca_files_without_hanging(const char *ready) {
    char folder[4096];
    assert(snprintf(folder, sizeof(folder), "%s/mail-ca-files.XXXXXX", getenv("TMPDIR")) > 0);
    assert(mkdtemp(folder));
    const char *names[] = {"missing", "empty", "invalid", "fifo", "large"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char path[4608];
        assert(snprintf(path, sizeof(path), "%s/%s", folder, names[i]) > 0);
        if (i == 3) assert(mkfifo(path, 0600) == 0);
        else if (i != 0) {
            const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
            assert(fd >= 0);
            if (i == 2) assert(write(fd, "not a CA bundle", 15) == 15);
            if (i == 4) assert(ftruncate(fd, 4 * 1024 * 1024 + 1) == 0);
            assert(close(fd) == 0);
        }
        Text error;
        Stream stream;
        const uint64_t before = monotonic_ms();
        assert(!stream.connect("localhost", endpoint_port(ready, "tls-valid"),
            TransportSecurity::ImplicitTls, path, test_context(), error));
        assert(transport_problem(error) == (i == 0 ? TransportProblem::CaUnavailable : TransportProblem::CaInvalid));
        assert(monotonic_ms() - before < 5000);
        if (i != 0) assert(unlink(path) == 0);
    }
    assert(rmdir(folder) == 0);
}

static void test_transport_verifies_identity_and_cancels_reads(const char *ready,
    const char *ca_path, const char *untrusted_ca_path) {
    const OperationContext context = test_context();
    Text error;
    Stream good;
    assert(good.connect("localhost", endpoint_port(ready, "tls-valid"),
        TransportSecurity::ImplicitTls, ca_path, context, error));
    assert(good.connected() && good.secure());
    TransportDiagnostics transport_info;
    assert(good.diagnostics(transport_info));
    assert(transport_info.tls_active && transport_info.certificate_verified);
    assert(transport_info.certificate_verify_flags == 0);
    assert(text_is(transport_info.tls_version, "TLSv1.2"));
    assert(!transport_info.cipher_suite.empty());
    assert(!transport_info.peer_certificate_details.empty());
    assert(strstr(transport_info.peer_certificate_details.c_str(), "localhost"));
    good.close();

    Stream wrong_host;
    assert(!wrong_host.connect("127.0.0.1", endpoint_port(ready, "tls-valid"),
        TransportSecurity::ImplicitTls, ca_path, context, error));
    assert(strstr(error.c_str(), "hostname"));

    Stream wrong_ca;
    assert(!wrong_ca.connect("localhost", endpoint_port(ready, "tls-valid"),
        TransportSecurity::ImplicitTls, untrusted_ca_path, context, error));
    assert(strstr(error.c_str(), "trusted"));
    assert(transport_problem(error) == TransportProblem::UntrustedCertificate);

    Stream expired;
    assert(!expired.connect("localhost", endpoint_port(ready, "tls-expired"),
        TransportSecurity::ImplicitTls, ca_path, context, error));
    assert(strstr(error.c_str(), "expired"));
    assert(transport_problem(error) == TransportProblem::CertificateTime);

    Stream plain;
    assert(!plain.connect("localhost", endpoint_port(ready, "tls-not-tls"),
        TransportSecurity::ImplicitTls, ca_path, context, error));
    assert(transport_problem(error) == TransportProblem::Handshake);

    Stream slow;
    assert(slow.connect("localhost", endpoint_port(ready, "tls-slow"),
        TransportSecurity::ImplicitTls, ca_path, context, error));
    OperationContext cancelled = context;
    cancelled.is_cancelled = cancelled_now;
    unsigned char byte = 0;
    size_t read_size = 0;
    assert(!slow.read(&byte, 1, &read_size, cancelled, error));
    slow.close();
}

static void test_starttls_is_mandatory_and_never_downgrades(const char *ready,
    const char *ca_path, const char *untrusted_ca_path) {
    const OperationContext context = test_context();
    Text error;

    ImapSession secure;
    Account account = imap_account(endpoint_port(ready, "imap-starttls"),
        TransportSecurity::StartTls, ca_path);
    assert(secure.connect(account, context, error));
    assert(secure.login(fixture_password(), context, error));
    secure.close();

    ImapSession unavailable;
    Account no_starttls = imap_account(endpoint_port(ready, "imap-starttls-no-extension"),
        TransportSecurity::StartTls, ca_path);
    assert(!unavailable.connect(no_starttls, context, error));
    assert(!unavailable.login(fixture_password(), context, error));

    ImapSession untrusted;
    Account bad_ca = imap_account(endpoint_port(ready, "imap-starttls-untrusted"),
        TransportSecurity::StartTls, untrusted_ca_path);
    assert(!untrusted.connect(bad_ca, context, error));
    assert(!untrusted.login(fixture_password(), context, error));
}

static FolderKey folder_key(uint64_t account_id, const char *server_name) {
    FolderKey key;
    key.account_id = account_id;
    assert(key.server_name.set(server_name));
    return key;
}

static Address address(const char *local, const char *domain) {
    Address value;
    assert(value.local_part.set(local));
    assert(value.domain.set(domain));
    return value;
}

struct SourceBytes {
    const unsigned char *data;
    size_t size;
    size_t max_chunk;
};

static bool read_source(const void *opaque, size_t offset, unsigned char *buffer,
    size_t capacity, size_t *read_size) {
    const SourceBytes *source = static_cast<const SourceBytes *>(opaque);
    if (!read_size || (!buffer && capacity) || offset > source->size) return false;
    if (offset == source->size) {
        *read_size = 0;
        return true;
    }
    size_t count = source->size - offset;
    if (count > capacity) count = capacity;
    if (count > source->max_chunk) count = source->max_chunk;
    if (!count) return false;
    memcpy(buffer, source->data + offset, count);
    *read_size = count;
    return true;
}

static MessageSource message_source(const SourceBytes &bytes) {
    MessageSource source;
    source.context = &bytes;
    source.read = read_source;
    source.size = bytes.size;
    return source;
}

static void test_split_imap_literals_and_uid_identity(const char *ready,
    const char *ca_path) {
    const OperationContext context = test_context();
    Account account = imap_account(endpoint_port(ready, "imap-split-literal"),
        TransportSecurity::ImplicitTls, ca_path);
    ImapSession session;
    Text error;
    const bool connected = session.connect(account, context, error);
    if (!connected) fprintf(stderr, "split IMAP connect: %s\n", error.c_str());
    assert(connected);
    assert(session.login(fixture_password(), context, error));

    ImapCapabilities caps;
    assert(session.capabilities(caps));
    assert(caps.id && caps.uidplus && caps.move && caps.auth_plain);
    TransportDiagnostics transport_info;
    assert(session.diagnostics(transport_info));
    assert(transport_info.certificate_verified && !transport_info.peer_certificate_details.empty());

    FolderList folders;
    const bool listed = session.list(folders, context, error);
    if (!listed) fprintf(stderr, "split IMAP LIST: %s\n", error.c_str());
    assert(listed);
    assert(folders.size() == 3);
    const Folder *inbox = nullptr;
    const Folder *quoted = nullptr;
    const Folder *project = nullptr;
    for (size_t i = 0; i < folders.size(); ++i) {
        assert(folders[i].uidvalidity == 0);
        if (folders[i].key.server_name == "INBOX") inbox = &folders[i];
        if (folders[i].key.server_name == "A \"quoted\" folder") quoted = &folders[i];
        if (folders[i].key.server_name == "&mHl27g-") project = &folders[i];
    }
    assert(inbox && text_is(inbox->display_name, "Inbox"));
    assert(quoted && text_is(quoted->display_name, "A \"quoted\" folder"));
    assert(project && text_is(project->display_name, "项目"));

    Folder selected;
    assert(session.select(project->key, selected, context, error));
    assert(selected.key == project->key);
    assert(selected.uidvalidity == 4242);

    Array<MessageKey> keys;
    assert(session.search(project->key, ImapSearchMode::All, keys, context, error));
    assert(keys.size() == 2);
    for (size_t i = 0; i < keys.size(); ++i) {
        assert(keys[i].folder == project->key);
        assert(keys[i].uidvalidity == 4242);
    }
    assert(keys[0].uid == 7 && keys[1].uid == 8);

    Text message_id;
    assert(message_id.set("<sent-candidate@example.test>"));
    Array<MessageKey> message_id_keys;
    assert(session.search_message_id(project->key, message_id, message_id_keys, context, error));
    assert(message_id_keys.size() == 2);

    Text subject_query;
    assert(subject_query.set("a \"quoted\" \\ phrase"));
    Array<MessageKey> text_keys;
    assert(session.search_text(project->key, ImapTextSearchField::Subject,
        subject_query, text_keys, context, error));
    assert(text_keys.size() == 2);
    Text from_query;
    assert(from_query.set("alice@reliefmail.test"));
    assert(session.search_text(project->key, ImapTextSearchField::From,
        from_query, text_keys, context, error));
    assert(text_keys.size() == 2);
    Text body_query;
    assert(body_query.set("body phrase"));
    assert(session.search_text(project->key, ImapTextSearchField::Body,
        body_query, text_keys, context, error));
    assert(text_keys.size() == 2);
    Text injected_query;
    assert(injected_query.set("x\r\nUID STORE 7 +FLAGS (\\Deleted)"));
    assert(!session.search_text(project->key, ImapTextSearchField::Subject,
        injected_query, text_keys, context, error));

    MessageKey stale = keys[0];
    stale.folder.account_id += 1;
    Array<ImapMessage> ignored;
    assert(!session.fetch(&stale, 1, ignored, context, error));
    assert(!session.fetch(nullptr, MaxImapUidBatch + 1, ignored, context, error));

    Array<ImapMessage> messages;
    assert(session.fetch(keys.data(), keys.size(), messages, context, error));
    assert(messages.size() == 2);
    for (size_t i = 0; i < messages.size(); ++i) {
        assert(messages[i].key == keys[i]);
        assert(!messages[i].headers_only);
        assert(messages[i].flags & ImapFlagSeen);
        assert(messages[i].flags & ImapFlagFlagged);
        assert(messages[i].internal_date > 0);
        assert(messages[i].size >= messages[i].raw.size());
        assert(bytes_contain(messages[i].raw, "Subject: fixture-"));
        assert(bytes_contain(messages[i].raw, "\r\nbody\r\n"));
        assert(bytes_contain(messages[i].raw,
            "Received: from reliefmail.local (unknown [172.17.0.1])\r\n\tby mail.reliefmail.test"));
        ParsedMessage parsed;
        Text parse_error;
        assert(parse_message(reinterpret_cast<const char *>(messages[i].raw.data()),
            messages[i].raw.size(), parsed, parse_error));
        assert(text_is(parsed.subject, i == 0 ? "fixture-7" : "fixture-8"));
    }

    Array<ImapMessage> headers;
    assert(session.fetch_headers(keys.data(), keys.size(), headers, context, error));
    assert(headers.size() == 2);
    for (size_t i = 0; i < headers.size(); ++i) {
        assert(headers[i].headers_only);
        assert(headers[i].flags & ImapFlagSeen);
        assert(headers[i].internal_date > 0);
        assert(headers[i].size == messages[i].size);
        assert(bytes_contain(headers[i].raw, "Subject: fixture-"));
        assert(!bytes_contain(headers[i].raw, "\r\nbody\r\n"));
    }

    Text encoded;
    assert(encode_modified_utf7("新建", strlen("新建"), encoded, error));
    assert(text_is(encoded, "&ZbBe+g-"));
    FolderKey create_context;
    create_context.account_id = account.id;
    Text new_name;
    assert(new_name.set("新建"));
    assert(session.folder_operation(ImapFolderOperation::Create, create_context,
        new_name, context, error));
    session.close();
}

static void test_imap_status_matches_only_requested_mailbox(const char *ready,
    const char *ca_path) {
    const OperationContext context = test_context();
    Account account = imap_account(endpoint_port(ready, "imap-status-unrequested"),
        TransportSecurity::ImplicitTls, ca_path);
    ImapSession session;
    Text error;
    assert(session.connect(account, context, error));
    assert(session.login(fixture_password(), context, error));
    FolderList folders;
    assert(session.list(folders, context, error));
    assert(folders.size() == 3);
    for (size_t i = 0; i < folders.size(); ++i) {
        assert(folders[i].message_count == 2);
        assert(folders[i].unread_count == 1);
        /* STATUS UIDVALIDITY does not convert LIST's unknown value to a reset. */
        assert(folders[i].uidvalidity == 0);
    }
    session.close();
}

static void test_imap_login_charset_refusal_and_fetch_budget(const char *ready,
    const char *ca_path) {
    const OperationContext context = test_context();
    Text error;
    Account login_account = imap_account(endpoint_port(ready, "imap-login"),
        TransportSecurity::ImplicitTls, ca_path);
    ImapSession login;
    assert(login.connect(login_account, context, error));
    assert(login.login(fixture_password(), context, error));
    login.close();

    Account failed_account = imap_account(endpoint_port(ready, "imap-auth-failure"),
        TransportSecurity::ImplicitTls, ca_path);
    ImapSession failed;
    assert(failed.connect(failed_account, context, error));
    static const unsigned char wrong_password[] = "wrong fixture password";
    SecretView wrong;
    wrong.data = wrong_password;
    wrong.size = sizeof(wrong_password) - 1;
    assert(!failed.login(wrong, context, error));
    failed.close();

    Account charset_account = imap_account(endpoint_port(ready, "imap-charset-refused"),
        TransportSecurity::ImplicitTls, ca_path);
    ImapSession charset;
    assert(charset.connect(charset_account, context, error));
    assert(charset.login(fixture_password(), context, error));
    FolderKey inbox = folder_key(charset_account.id, "INBOX");
    Text unicode_query;
    assert(unicode_query.set("项目"));
    Array<MessageKey> keys;
    assert(!charset.search_text(inbox, ImapTextSearchField::Subject,
        unicode_query, keys, context, error));
    assert(strstr(error.c_str(), "CHARSET UTF-8") != nullptr);
    charset.close();

    Account budget_account = imap_account(endpoint_port(ready, "imap-fetch-budget"),
        TransportSecurity::ImplicitTls, ca_path);
    ImapSession budget;
    assert(budget.connect(budget_account, context, error));
    assert(budget.login(fixture_password(), context, error));
    Folder selected;
    assert(budget.select(folder_key(budget_account.id, "INBOX"), selected, context, error));
    MessageKey pair[2];
    pair[0].folder = selected.key;
    pair[0].uidvalidity = selected.uidvalidity;
    pair[0].uid = 7;
    pair[1] = pair[0];
    pair[1].uid = 8;
    Array<ImapMessage> messages;
    assert(!budget.fetch(pair, 2, messages, context, error));
    assert(strstr(error.c_str(), "aggregate response budget") != nullptr);
    budget.close();
}

static void test_move_fallback_requires_targeted_expunge(const char *ready,
    const char *ca_path) {
    const OperationContext context = test_context();
    Text error;
    Account account = imap_account(endpoint_port(ready, "imap-safe-move"),
        TransportSecurity::ImplicitTls, ca_path);
    ImapSession safe;
    assert(safe.connect(account, context, error));
    assert(safe.login(fixture_password(), context, error));
    Folder selected;
    FolderKey inbox = folder_key(account.id, "INBOX");
    assert(safe.select(inbox, selected, context, error));
    MessageKey key;
    key.folder = inbox;
    key.uidvalidity = selected.uidvalidity;
    key.uid = 5;
    FolderKey archive = folder_key(account.id, "Archive");
    assert(safe.store(key, ImapFlagSeen, 0, context, error));
    assert(safe.move(key, archive, context, error));
    safe.close();

    Account limited_account = imap_account(endpoint_port(ready, "imap-unsafe-move"),
        TransportSecurity::ImplicitTls, ca_path);
    ImapSession limited;
    assert(limited.connect(limited_account, context, error));
    assert(limited.login(fixture_password(), context, error));
    assert(limited.select(folder_key(limited_account.id, "INBOX"), selected, context, error));
    key.folder = folder_key(limited_account.id, "INBOX");
    key.uidvalidity = selected.uidvalidity;
    assert(!limited.move(key, archive, context, error));
    limited.close();
}

static void test_targeted_delete_requires_uidplus_and_exact_identity(const char *ready,
    const char *ca_path) {
    const OperationContext context = test_context();
    Text error;
    Account account = imap_account(endpoint_port(ready, "imap-delete"),
        TransportSecurity::ImplicitTls, ca_path);
    ImapSession session;
    assert(session.connect(account, context, error));
    assert(session.login(fixture_password(), context, error));
    Folder selected;
    FolderKey inbox = folder_key(account.id, "INBOX");
    assert(session.select(inbox, selected, context, error));
    MessageKey key;
    key.folder = inbox;
    key.uidvalidity = selected.uidvalidity;
    key.uid = 23;
    assert(session.delete_message(key, context, error));
    MessageKey stale = key;
    ++stale.uidvalidity;
    assert(!session.delete_message(stale, context, error));
    session.close();

    Account limited_account = imap_account(endpoint_port(ready, "imap-no-uidplus-delete"),
        TransportSecurity::ImplicitTls, ca_path);
    ImapSession limited;
    assert(limited.connect(limited_account, context, error));
    assert(limited.login(fixture_password(), context, error));
    assert(limited.select(folder_key(limited_account.id, "INBOX"), selected, context, error));
    key.folder.account_id = limited_account.id;
    key.uidvalidity = selected.uidvalidity;
    assert(!limited.delete_message(key, context, error));
    limited.close();
}

static void test_authenticated_capabilities_refresh_before_safe_move_and_delete(
    const char *ready, const char *ca_path) {
    const OperationContext context = test_context();
    Text error;
    Account account = imap_account(endpoint_port(ready, "imap-auth-capability-transition"),
        TransportSecurity::ImplicitTls, ca_path);
    ImapSession session;
    assert(session.connect(account, context, error));
    ImapCapabilities capabilities;
    assert(session.capabilities(capabilities));
    assert(!capabilities.move && !capabilities.uidplus);
    assert(session.login(fixture_password(), context, error));
    assert(session.capabilities(capabilities));
    assert(capabilities.move && capabilities.uidplus);

    FolderKey inbox = folder_key(account.id, "INBOX");
    Folder selected;
    assert(session.select(inbox, selected, context, error));
    MessageKey key;
    key.folder = inbox;
    key.uidvalidity = selected.uidvalidity;
    key.uid = 5;
    assert(session.move(key, folder_key(account.id, "Archive"), context, error));
    key.uid = 23;
    assert(session.delete_message(key, context, error));
    session.close();
}

static void test_authenticated_capability_refresh_failures_close_the_session(
    const char *ready, const char *ca_path) {
    const OperationContext context = test_context();
    Text error;
    Account rejected_account = imap_account(
        endpoint_port(ready, "imap-auth-capability-reject"),
        TransportSecurity::ImplicitTls, ca_path);
    ImapSession rejected;
    assert(rejected.connect(rejected_account, context, error));
    assert(!rejected.login(fixture_password(), context, error));
    assert(!error.empty());
    ImapCapabilities capabilities;
    assert(!rejected.capabilities(capabilities));
    MessageKey key;
    key.folder = folder_key(rejected_account.id, "INBOX");
    key.uidvalidity = 4242;
    key.uid = 23;
    assert(!rejected.delete_message(key, context, error));
    rejected.close();

    Account timeout_account = imap_account(
        endpoint_port(ready, "imap-auth-capability-timeout"),
        TransportSecurity::ImplicitTls, ca_path);
    ImapSession timeout;
    assert(timeout.connect(timeout_account, context, error));
    OperationContext deadline = context;
    deadline.deadline_monotonic_ms = monotonic_ms() + 500;
    assert(!timeout.login(fixture_password(), deadline, error));
    assert(strstr(error.c_str(), "deadline expired") || strstr(error.c_str(), "cancelled"));
    assert(!timeout.capabilities(capabilities));
    key.folder.account_id = timeout_account.id;
    assert(!timeout.delete_message(key, context, error));
    timeout.close();

    Account cancelled_account = imap_account(
        endpoint_port(ready, "imap-auth-capability-cancel"),
        TransportSecurity::ImplicitTls, ca_path);
    ImapSession cancelled;
    assert(cancelled.connect(cancelled_account, context, error));
    uint64_t cancel_at = monotonic_ms() + 500;
    OperationContext cancellation = context;
    cancellation.is_cancelled = cancel_at_time;
    cancellation.cancellation_context = &cancel_at;
    assert(!cancelled.login(fixture_password(), cancellation, error));
    assert(strstr(error.c_str(), "cancelled"));
    assert(!cancelled.capabilities(capabilities));
    key.folder.account_id = cancelled_account.id;
    assert(!cancelled.delete_message(key, context, error));
    cancelled.close();
}

static void test_imap_append_reports_acceptance(const char *ready, const char *ca_path) {
    static const unsigned char raw[] = "From: alice@reliefmail.test\r\n\r\nfixture\r\n";
    const SourceBytes bytes = {raw, sizeof(raw) - 1, 5};
    const MessageSource source = message_source(bytes);
    const OperationContext context = test_context();
    Account account = imap_account(endpoint_port(ready, "imap-append"),
        TransportSecurity::ImplicitTls, ca_path);
    ImapSession session;
    Text error;
    assert(session.connect(account, context, error));
    assert(session.login(fixture_password(), context, error));
    const ImapAppendOutcome outcome = session.append(folder_key(account.id, "Sent"),
        source, context, error);
    assert(outcome == ImapAppendOutcome::Accepted);
    session.close();
}

struct ObserverState {
    bool refuse_data = false;
    bool saw_data = false;
    bool saw_awaiting = false;
    bool saw_accepted = false;
};

static bool observe_send(void *opaque, SendStage stage) {
    ObserverState *state = static_cast<ObserverState *>(opaque);
    if (stage == SendStage::SendingData) {
        state->saw_data = true;
        return !state->refuse_data;
    }
    if (stage == SendStage::AwaitingFinalResponse) state->saw_awaiting = true;
    if (stage == SendStage::Accepted) state->saw_accepted = true;
    return true;
}

static Envelope fixture_envelope(bool refuse_second) {
    Envelope envelope;
    envelope.sender = address("alice", "reliefmail.test");
    assert(envelope.recipients.push(address("bob", "reliefmail.test")));
    if (refuse_second)
        assert(envelope.recipients.push(address("refused", "reliefmail.test")));
    return envelope;
}

static SubmitOutcome submit_fixture(const char *ready, const char *scenario,
    const char *ca_path, bool refuse_second, bool refuse_data,
    ObserverState &observer_state, Text &error) {
    static const unsigned char raw[] =
        "From: alice@reliefmail.test\n"
        "To: bob@reliefmail.test\n"
        "Subject: phase-a fixture\n"
        "\n"
        ".leading-dot\n"
        "final line\n";
    const SourceBytes source_bytes = {raw, sizeof(raw) - 1, 13};
    const MessageSource source = message_source(source_bytes);
    const OperationContext context = test_context();
    Account account = smtp_account(endpoint_port(ready, scenario),
        TransportSecurity::ImplicitTls, ca_path);
    SmtpSession session;
    assert(session.connect(account, context, error));
    assert(session.authenticate(fixture_password(), context, error));
    Envelope envelope = fixture_envelope(refuse_second);
    observer_state.refuse_data = refuse_data;
    SendObserver observer;
    observer.context = &observer_state;
    observer.advance = observe_send;
    const SubmitOutcome outcome = session.submit(envelope, source, observer, context, error);
    session.close();
    return outcome;
}

static SubmitOutcome submit_on_session(SmtpSession &session, const Envelope &envelope,
    const unsigned char *raw, size_t raw_size, bool refuse_data,
    ObserverState &observer_state, Text &error) {
    const SourceBytes source_bytes = {raw, raw_size, 13};
    const MessageSource source = message_source(source_bytes);
    const OperationContext context = test_context();
    observer_state.refuse_data = refuse_data;
    SendObserver observer;
    observer.context = &observer_state;
    observer.advance = observe_send;
    return session.submit(envelope, source, observer, context, error);
}

static bool open_smtp_fixture(SmtpSession &session, const char *ready,
    const char *scenario, const char *ca_path, Text &error) {
    const OperationContext context = test_context();
    Account account = smtp_account(endpoint_port(ready, scenario),
        TransportSecurity::ImplicitTls, ca_path);
    return session.connect(account, context, error) &&
        session.authenticate(fixture_password(), context, error);
}

static void test_smtp_recipient_validation_and_data_outcomes(const char *ready,
    const char *ca_path) {
    Text error;
    ObserverState accepted_observer;
    assert(submit_fixture(ready, "smtp-success", ca_path, false, false,
        accepted_observer, error) == SubmitOutcome::Accepted);
    assert(accepted_observer.saw_data && accepted_observer.saw_awaiting);
    assert(accepted_observer.saw_accepted);

    ObserverState rejected_observer;
    assert(submit_fixture(ready, "smtp-recipient-rejected", ca_path, true, false,
        rejected_observer, error) == SubmitOutcome::Rejected);
    assert(!rejected_observer.saw_data);

    ObserverState observer_refusal;
    assert(submit_fixture(ready, "smtp-observer-refused", ca_path, false, true,
        observer_refusal, error) == SubmitOutcome::Rejected);
    assert(observer_refusal.saw_data && !observer_refusal.saw_awaiting);

    ObserverState lost_final;
    assert(submit_fixture(ready, "smtp-final-lost", ca_path, false, false,
        lost_final, error) == SubmitOutcome::Uncertain);
    assert(lost_final.saw_data && lost_final.saw_awaiting && !lost_final.saw_accepted);
}

static void test_smtp_login_authentication_fallback(const char *ready,
    const char *ca_path) {
    const OperationContext context = test_context();
    Account account = smtp_account(endpoint_port(ready, "smtp-login"),
        TransportSecurity::ImplicitTls, ca_path);
    SmtpSession session;
    Text error;
    assert(session.connect(account, context, error));
    assert(session.authenticate(fixture_password(), context, error));
    SmtpCapabilities caps;
    assert(session.capabilities(caps));
    assert(caps.auth_login && caps.size && caps.maximum_size == 1048576 && caps.eight_bit_mime);
    assert(caps.smtp_utf8);
    TransportDiagnostics info;
    assert(session.diagnostics(info));
    assert(info.certificate_verified && !info.tls_version.empty());
    session.close();
}

static void test_smtp_starttls_and_authentication_fail_closed(const char *ready,
    const char *ca_path) {
    const OperationContext context = test_context();
    Text error;
    Account starttls_account = smtp_account(endpoint_port(ready, "smtp-starttls"),
        TransportSecurity::StartTls, ca_path);
    SmtpSession starttls;
    assert(starttls.connect(starttls_account, context, error));
    TransportDiagnostics diagnostics;
    assert(starttls.diagnostics(diagnostics) && diagnostics.certificate_verified);
    assert(starttls.authenticate(fixture_password(), context, error));
    starttls.close();

    Account unavailable_account = smtp_account(endpoint_port(ready, "smtp-starttls-no-extension"),
        TransportSecurity::StartTls, ca_path);
    SmtpSession unavailable;
    assert(!unavailable.connect(unavailable_account, context, error));
    assert(!unavailable.authenticate(fixture_password(), context, error));

    Account failed_account = smtp_account(endpoint_port(ready, "smtp-auth-failure"),
        TransportSecurity::ImplicitTls, ca_path);
    SmtpSession failed;
    assert(failed.connect(failed_account, context, error));
    static const unsigned char wrong_password[] = "wrong fixture password";
    SecretView wrong;
    wrong.data = wrong_password;
    wrong.size = sizeof(wrong_password) - 1;
    assert(!failed.authenticate(wrong, context, error));
    failed.close();
}

static void test_smtp_explicit_final_rejection(const char *ready, const char *ca_path) {
    Text error;
    ObserverState observer_state;
    assert(submit_fixture(ready, "smtp-final-rejected", ca_path, false, false,
        observer_state, error) == SubmitOutcome::Rejected);
    assert(observer_state.saw_data && observer_state.saw_awaiting && !observer_state.saw_accepted);
}

static void assert_smtp_session_invalidated(SmtpSession &session, Text &error) {
    SmtpCapabilities capabilities;
    assert(!session.capabilities(capabilities));
    ObserverState observer;
    static const unsigned char raw[] = "From: alice@reliefmail.test\r\n\r\nnext\r\n";
    assert(submit_on_session(session, fixture_envelope(false), raw, sizeof(raw) - 1,
        false, observer, error) == SubmitOutcome::Rejected);
    assert(!observer.saw_data);
}

static void test_smtp_untrustworthy_sessions_are_closed(const char *ready,
    const char *ca_path) {
    struct Case { const char *scenario; bool refuse_second; bool refuse_data; SubmitOutcome expected; };
    static const Case cases[] = {
        {"smtp-final-lost", false, false, SubmitOutcome::Uncertain},
        {"smtp-final-inconclusive", false, false, SubmitOutcome::Uncertain},
        {"smtp-malformed-mail", false, false, SubmitOutcome::Rejected},
        {"smtp-malformed-data", false, false, SubmitOutcome::Rejected},
        {"smtp-rset-negative-recipient", true, false, SubmitOutcome::Rejected},
        {"smtp-rset-lost-observer", false, true, SubmitOutcome::Rejected},
    };
    static const unsigned char raw[] =
        "From: alice@reliefmail.test\r\nTo: bob@reliefmail.test\r\n"
        "Subject: phase-a fixture\r\n\r\nbody\r\n";
    const Envelope two_recipients = fixture_envelope(true);
    const Envelope one_recipient = fixture_envelope(false);
    const char *focus = getenv("RELIEFMAIL_PROTOCOL_FOCUS");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const bool rset_case = !strncmp(cases[i].scenario, "smtp-rset-", 10);
        if (focus && ((!strcmp(focus, "smtp-rset") && !rset_case) ||
            (!strcmp(focus, "smtp-terminal") && rset_case))) continue;
        SmtpSession session;
        Text error;
        assert(open_smtp_fixture(session, ready, cases[i].scenario, ca_path, error));
        ObserverState observer;
        const Envelope &envelope = cases[i].refuse_second ? two_recipients : one_recipient;
        assert(submit_on_session(session, envelope, raw, sizeof(raw) - 1,
            cases[i].refuse_data, observer, error) == cases[i].expected);
        if (!strcmp(cases[i].scenario, "smtp-rset-negative-recipient")) {
            assert(strstr(error.c_str(), "rejected one or more recipients"));
            assert(strstr(error.c_str(), "RSET was rejected (code 554)"));
        } else if (!strcmp(cases[i].scenario, "smtp-rset-lost-observer")) {
            assert(strstr(error.c_str(), "observer refused before SMTP DATA"));
            assert(strstr(error.c_str(), "SMTP RSET failed"));
        }
        assert_smtp_session_invalidated(session, error);
        session.close();
    }
}

static void test_smtp_rset_success_keeps_session_synchronized(const char *ready,
    const char *ca_path) {
    static const unsigned char raw[] =
        "From: alice@reliefmail.test\r\nTo: bob@reliefmail.test\r\n"
        "Subject: phase-a fixture\r\n\r\nbody\r\n";
    Text error;
    for (unsigned int observer_refusal = 0; observer_refusal < 2; ++observer_refusal) {
        const char *scenario = observer_refusal ? "smtp-observer-refused" : "smtp-recipient-rejected";
        SmtpSession session;
        assert(open_smtp_fixture(session, ready, scenario, ca_path, error));
        ObserverState first;
        const Envelope initial = fixture_envelope(observer_refusal == 0);
        const SubmitOutcome rejected = submit_on_session(session, initial, raw, sizeof(raw) - 1,
            observer_refusal != 0, first, error);
        assert(rejected == SubmitOutcome::Rejected);
        SmtpCapabilities capabilities;
        assert(session.capabilities(capabilities));
        ObserverState second;
        assert(submit_on_session(session, fixture_envelope(false), raw, sizeof(raw) - 1,
            false, second, error) == SubmitOutcome::Accepted);
        assert(second.saw_data && second.saw_accepted);
        session.close();
    }
}

static void test_smtp_8bit_mail_parameters_and_capability_refusal(const char *ready,
    const char *ca_path) {
    static const unsigned char header_and_body_8bit[] =
        "From: alice@reliefmail.test\r\nTo: bob@reliefmail.test\r\n"
        "Subject: caf\xc3\xa9\r\nContent-Type: text/plain; charset=UTF-8\r\n"
        "Content-Transfer-Encoding: 8bit\r\n\r\ncaf\xc3\xa9 body\r\n";
    static const unsigned char body_8bit_only[] =
        "From: alice@reliefmail.test\r\nTo: bob@reliefmail.test\r\n"
        "Subject: plain\r\nContent-Type: text/plain; charset=UTF-8\r\n"
        "Content-Transfer-Encoding: 8bit\r\n\r\ncaf\xc3\xa9 body\r\n";
    Text error;
    SmtpSession accepted_session;
    assert(open_smtp_fixture(accepted_session, ready, "smtp-8bit", ca_path, error));
    ObserverState accepted;
    assert(submit_on_session(accepted_session, fixture_envelope(false), header_and_body_8bit,
        sizeof(header_and_body_8bit) - 1, false, accepted, error) == SubmitOutcome::Accepted);
    assert(accepted.saw_accepted);
    accepted_session.close();

    struct Case { const char *scenario; const unsigned char *raw; size_t size; const char *required; };
    const Case cases[] = {
        {"smtp-no-smtputf8", header_and_body_8bit, sizeof(header_and_body_8bit) - 1, "SMTPUTF8"},
        {"smtp-no-8bitmime", body_8bit_only, sizeof(body_8bit_only) - 1, "8BITMIME"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        SmtpSession session;
        assert(open_smtp_fixture(session, ready, cases[i].scenario, ca_path, error));
        ObserverState observer;
        assert(submit_on_session(session, fixture_envelope(false), cases[i].raw,
            cases[i].size, false, observer, error) == SubmitOutcome::Rejected);
        assert(strstr(error.c_str(), cases[i].required));
        SmtpCapabilities capabilities;
        assert(session.capabilities(capabilities));
        session.close();
    }
}

int main(int argc, char **argv) {
    assert(argc == 4);
    const char *focus = getenv("RELIEFMAIL_PROTOCOL_FOCUS");
    if (focus) {
        if (!strcmp(focus, "ca")) {
            test_transport_accepts_usable_roots_in_a_partial_bundle(argv[1], argv[2]);
        } else if (!strcmp(focus, "status")) {
            test_imap_status_matches_only_requested_mailbox(argv[1], argv[2]);
        } else if (!strcmp(focus, "smtp-terminal") || !strcmp(focus, "smtp-rset")) {
            test_smtp_untrustworthy_sessions_are_closed(argv[1], argv[2]);
        } else if (!strcmp(focus, "smtp-parameters")) {
            test_smtp_8bit_mail_parameters_and_capability_refusal(argv[1], argv[2]);
        } else {
            fprintf(stderr, "unknown RELIEFMAIL_PROTOCOL_FOCUS value\n");
            return 2;
        }
        puts("Focused ReliefMail protocol regression passed");
        return 0;
    }
    test_transport_accepts_usable_roots_in_a_partial_bundle(argv[1], argv[2]);
    test_transport_reports_unusable_ca_files_without_hanging(argv[1]);
    test_transport_verifies_identity_and_cancels_reads(argv[1], argv[2], argv[3]);
    test_starttls_is_mandatory_and_never_downgrades(argv[1], argv[2], argv[3]);
    test_split_imap_literals_and_uid_identity(argv[1], argv[2]);
    test_imap_status_matches_only_requested_mailbox(argv[1], argv[2]);
    test_imap_login_charset_refusal_and_fetch_budget(argv[1], argv[2]);
    test_move_fallback_requires_targeted_expunge(argv[1], argv[2]);
    test_targeted_delete_requires_uidplus_and_exact_identity(argv[1], argv[2]);
    test_imap_append_reports_acceptance(argv[1], argv[2]);
    test_smtp_recipient_validation_and_data_outcomes(argv[1], argv[2]);
    test_smtp_login_authentication_fallback(argv[1], argv[2]);
    test_smtp_starttls_and_authentication_fail_closed(argv[1], argv[2]);
    test_smtp_explicit_final_rejection(argv[1], argv[2]);
    test_smtp_untrustworthy_sessions_are_closed(argv[1], argv[2]);
    test_smtp_rset_success_keeps_session_synchronized(argv[1], argv[2]);
    test_smtp_8bit_mail_parameters_and_capability_refusal(argv[1], argv[2]);
    test_authenticated_capabilities_refresh_before_safe_move_and_delete(argv[1], argv[2]);
    test_authenticated_capability_refresh_failures_close_the_session(argv[1], argv[2]);
    puts("ReliefMail protocol suite passed");
    return 0;
}
