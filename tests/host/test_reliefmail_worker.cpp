#include "tasks.h"
#include "vault.h"
#include "mime.h"

#include <assert.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace rmail;

static bool remove_worker_tree(const char *path) {
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
        if (count < 0 || static_cast<size_t>(count) >= sizeof(child) || !remove_worker_tree(child)) ok = false;
    }
    if (closedir(directory) < 0) ok = false;
    if (rmdir(path) < 0) ok = false;
    return ok;
}

static SecretBytes worker_test_secret(const char *text) {
    SecretBytes secret;
    assert(secret.set(text, strlen(text)));
    return secret;
}

struct WorkerProfileFixture {
    char root[PATH_MAX]{};
    ProfilePaths paths;

    WorkerProfileFixture() {
        char current[PATH_MAX];
        assert(getcwd(current, sizeof(current)));
        const int count = snprintf(root, sizeof(root), "%s/out/tmp/reliefmail-worker-%ld-XXXXXX",
            current, static_cast<long>(getpid()));
        assert(count > 0 && static_cast<size_t>(count) < sizeof(root));
        assert(mkdtemp(root));
        Text error;
        assert(profile_paths_from_env(root, nullptr, nullptr, nullptr, paths, error));
    }

    ~WorkerProfileFixture() { assert(remove_worker_tree(root)); }
};

static void put_u32(unsigned char output[4], uint32_t value) {
    output[0] = static_cast<unsigned char>(value >> 24);
    output[1] = static_cast<unsigned char>(value >> 16);
    output[2] = static_cast<unsigned char>(value >> 8);
    output[3] = static_cast<unsigned char>(value);
}

static void test_versioned_request_frames_reject_bad_lengths_and_versions() {
    WorkerRequest input;
    input.request_id = 7;
    input.profile_epoch = 2;
    input.ui_generation = 31;
    input.account_id = 5;
    input.account_generation = 19;
    input.folder_server_name.set("INBOX");
    input.folder_generation = 12;
    input.folder_uidvalidity = 88;
    input.message_uid = 9;
    input.kind = WorkerTaskKind::TestProbe;
    const unsigned char bytes[] = {0, 0, 0, 12};
    assert(input.payload.set(bytes, sizeof(bytes)));

    Bytes encoded;
    Text error;
    assert(worker_encode_request(input, encoded, error));
    WorkerRequest decoded;
    assert(worker_decode_request(encoded.data(), encoded.size(), decoded, error));
    assert(decoded.request_id == input.request_id);
    assert(decoded.profile_epoch == input.profile_epoch);
    assert(decoded.ui_generation == input.ui_generation);
    assert(decoded.account_id == input.account_id);
    assert(decoded.account_generation == input.account_generation);
    assert(decoded.folder_server_name == "INBOX");
    assert(decoded.folder_generation == input.folder_generation);
    assert(decoded.folder_uidvalidity == input.folder_uidvalidity);
    assert(decoded.message_uid == input.message_uid);
    assert(decoded.payload == input.payload);

    unsigned char frame_length[4];
    put_u32(frame_length, static_cast<uint32_t>(MaxWorkerFrameBytes + 1));
    size_t decoded_length = 0;
    assert(!worker_decode_frame_length(frame_length, decoded_length, error));

    Bytes invalid(encoded);
    unsigned char *mutable_bytes = const_cast<unsigned char *>(invalid.data());
    assert(invalid.size() > 5);
    mutable_bytes[8] = 0xff;
    assert(!worker_decode_request(invalid.data(), invalid.size(), decoded, error));
}

static Account credential_test_account() {
    Account account;
    account.id = 71;
    account.generation = 9001;
    account.email_address.set("alice@example.test");
    account.username.set("imap-user");
    account.smtp_username.set("smtp-user");
    account.incoming_host.set("imap.example.test");
    account.outgoing_host.set("smtp.example.test");
    account.incoming_port = 993;
    account.outgoing_port = 465;
    account.inbox_folder.set("INBOX");
    account.sent_folder.set("Sent");
    return account;
}

static void test_public_task_result_codecs_are_bounded_and_typed() {
    Text error;
    MessageQueryPage page;
    page.query.scope = MessageQueryScope::Folder;
    page.query.sort = MessageQuerySort::Oldest;
    page.query.folder.account_id = 71;
    page.query.folder.server_name.set("INBOX");
    page.query.offset = 4;
    page.query.limit = 2;
    page.query.has_received_after = true;
    page.query.received_after = -11;
    page.query.has_received_before = true;
    page.query.received_before = 200;
    page.query.attachment_filter = AttachmentQueryFilter::Unknown;
    page.query.account_id = 71;
    page.folder_generation = 44;
    page.folder_uidvalidity = 77;
    page.total_matches = 5;
    page.has_more = true;
    page.downloaded_body_count = 1;
    page.headers_only_count = 1;
    MessageQueryRow row;
    row.message.key.folder = page.query.folder;
    row.message.key.uidvalidity = 77;
    row.message.key.uid = 9;
    row.message.internal_date = -11;
    row.message.size = 456;
    row.message.headers_only = true;
    row.message.from.set("sender@example.test");
    row.message.subject.set("header-only row");
    row.folder_display_name.set("Inbox");
    row.attachment_presence_known = true;
    row.profile_epoch = 73;
    row.ui_generation = 31;
    row.account_generation = 9001;
    row.folder_generation = 44;
    assert(page.rows.push(row));
    Bytes encoded_page;
    assert(worker_encode_message_page(page, encoded_page, error));
    MessageQueryPage decoded_page;
    assert(worker_decode_message_page(encoded_page.data(), encoded_page.size(), decoded_page, error));
    assert(decoded_page.rows.size() == 1 && decoded_page.has_more);
    assert(decoded_page.query.has_received_after && decoded_page.query.received_after == -11);
    assert(decoded_page.query.has_received_before && decoded_page.query.received_before == 200);
    assert(decoded_page.query.attachment_filter == AttachmentQueryFilter::Unknown);
    assert(decoded_page.folder_generation == 44 && decoded_page.folder_uidvalidity == 77);
    assert(decoded_page.rows[0].message.internal_date == -11);
    assert(decoded_page.rows[0].profile_epoch == 73 && decoded_page.rows[0].ui_generation == 31);
    assert(decoded_page.rows[0].account_generation == 9001 && decoded_page.rows[0].folder_generation == 44);
    assert(decoded_page.rows[0].message.headers_only && decoded_page.headers_only_count == 1);

    MessageQueryPage all_mail_page;
    all_mail_page.query.scope = MessageQueryScope::AllMail;
    all_mail_page.query.limit = 7;
    all_mail_page.total_matches = 23;
    Bytes all_mail_payload;
    assert(worker_encode_message_page(all_mail_page, all_mail_payload, error));
    MessageQueryPage decoded_all_mail;
    assert(worker_decode_message_page(all_mail_payload.data(), all_mail_payload.size(),
        decoded_all_mail, error));
    assert(decoded_all_mail.query.scope == MessageQueryScope::AllMail &&
        decoded_all_mail.total_matches == 23 && decoded_all_mail.rows.empty());

    WorkerReaderResult reader;
    reader.key = row.message.key;
    reader.headers_only = false;
    reader.from = row.message.from;
    reader.reply_to.set("reply-to@example.test");
    reader.subject = row.message.subject;
    reader.message_id.set("<reader@example.test>");
    reader.plain_text.set("bounded display text");
    reader.plain_text_truncated = true;
    reader.html_text.set("<p><a href=\"rmail-link://0\">open</a><img src=\"rmail-cid://0\"></p>");
    reader.html_warning = true;
    Text safe_link;
    assert(safe_link.set("https://mail.example.test/reader"));
    assert(reader.safe_links.push(static_cast<Text &&>(safe_link)));
    WorkerReaderInlineImage reader_image;
    reader_image.width = 1;
    reader_image.height = 1;
    reader_image.xpm.set("/* XPM */\nstatic const char *rmail_image[] = {\n\"1 1 1 1\",\n\"A c #FFFFFF\",\n\"A\",\n};\n");
    assert(reader.inline_images.push(static_cast<WorkerReaderInlineImage &&>(reader_image)));
    WorkerAttachmentMetadata attachment;
    attachment.filename.set("report.txt");
    attachment.content_type.set("text/plain");
    attachment.size = 4096;
    assert(reader.attachments.push(attachment));
    Bytes encoded_reader;
    assert(worker_encode_reader_result(reader, encoded_reader, error));
    WorkerReaderResult decoded_reader;
    assert(worker_decode_reader_result(encoded_reader.data(), encoded_reader.size(), decoded_reader, error));
    assert(decoded_reader.key == reader.key && !decoded_reader.headers_only);
    assert(decoded_reader.attachments.size() == 1 && decoded_reader.attachments[0].size == 4096);
    assert(decoded_reader.reply_to == reader.reply_to);
    assert(decoded_reader.plain_text == reader.plain_text);
    assert(decoded_reader.plain_text_truncated);
    assert(decoded_reader.html_text == reader.html_text && decoded_reader.html_warning);
    assert(decoded_reader.safe_links.size() == 1 && decoded_reader.safe_links[0] == reader.safe_links[0]);
    assert(decoded_reader.inline_images.size() == 1);
    assert(decoded_reader.inline_images[0].width == 1 && decoded_reader.inline_images[0].height == 1);
    assert(decoded_reader.inline_images[0].xpm == reader.inline_images[0].xpm);

    WorkerReaderResult oversized_reader;
    oversized_reader.key = reader.key;
    const size_t oversized_preview_bytes = MaxReaderPlainPreviewBytes + 1u;
    char *oversized_preview = static_cast<char *>(malloc(oversized_preview_bytes));
    assert(oversized_preview);
    memset(oversized_preview, 'x', oversized_preview_bytes);
    assert(oversized_reader.plain_text.set(oversized_preview, oversized_preview_bytes));
    free(oversized_preview);
    Bytes rejected_reader_payload;
    assert(!worker_encode_reader_result(oversized_reader, rejected_reader_payload, error));

    Bytes oversized_reader_wire;
    assert(oversized_reader_wire.set(encoded_reader.data(), encoded_reader.size()));
    size_t plain_length_offset = 6 + 8 + 4 + reader.key.folder.server_name.size() + 8 + 4 + 1;
    const Text *reader_prefix_fields[] = {&reader.from, &reader.reply_to, &reader.to, &reader.cc, &reader.bcc,
        &reader.subject, &reader.date, &reader.message_id, &reader.in_reply_to,
        &reader.references, &reader.warning};
    for (size_t i = 0; i < sizeof(reader_prefix_fields) / sizeof(reader_prefix_fields[0]); ++i)
        plain_length_offset += 4 + reader_prefix_fields[i]->size();
    assert(plain_length_offset + 4 <= oversized_reader_wire.size());
    put_u32(const_cast<unsigned char *>(oversized_reader_wire.data()) + plain_length_offset,
        static_cast<uint32_t>(MaxReaderPlainPreviewBytes + 1u));
    WorkerReaderResult rejected_reader;
    assert(!worker_decode_reader_result(oversized_reader_wire.data(), oversized_reader_wire.size(),
        rejected_reader, error));

    WorkerChunkResult chunk;
    chunk.key = row.message.key;
    chunk.attachment_index = 0;
    chunk.offset = 1024;
    chunk.total_size = 4096;
    const unsigned char sample[] = {1, 2, 3};
    assert(chunk.bytes.set(sample, sizeof(sample)));
    Bytes encoded_chunk;
    assert(worker_encode_chunk_result(chunk, encoded_chunk, error));
    WorkerChunkResult decoded_chunk;
    assert(worker_decode_chunk_result(encoded_chunk.data(), encoded_chunk.size(), decoded_chunk, error));
    assert(decoded_chunk.offset == 1024 && decoded_chunk.bytes.size() == 3);

    WorkerOperationResult operation;
    operation.operation_id = 3;
    operation.stage = SendStage::Accepted;
    operation.delivery_count = 1;
    operation.message_id.set("<op@example.test>");
    Bytes encoded_operation;
    assert(worker_encode_operation_result(operation, encoded_operation, error));
    WorkerOperationResult decoded_operation;
    assert(worker_decode_operation_result(encoded_operation.data(), encoded_operation.size(), decoded_operation, error));
    assert(decoded_operation.operation_id == 3 && decoded_operation.delivery_count == 1);

    WorkerDraftSnapshot snapshot;
    snapshot.draft.id = 8;
    snapshot.draft.revision = 2;
    snapshot.draft.account_id = 71;
    snapshot.account_generation = 9001;
    assert(snapshot.attachments.push(attachment));
    Bytes encoded_snapshot;
    assert(worker_encode_draft_snapshot(snapshot, encoded_snapshot, error));
    WorkerDraftSnapshot decoded_snapshot;
    assert(worker_decode_draft_snapshot(encoded_snapshot.data(), encoded_snapshot.size(), decoded_snapshot, error));
    assert(decoded_snapshot.draft.revision == 2 && decoded_snapshot.account_generation == 9001);
    assert(decoded_snapshot.attachments.size() == 1 && decoded_snapshot.attachments[0].size == 4096);

    WorkerTaskInput input;
    input.item_id = 8;
    input.item_revision = 2;
    input.has_draft = true;
    input.draft.id = 8;
    input.draft.revision = 2;
    input.draft.account_id = 71;
    WorkerDraftAttachmentRef reference;
    reference.kind = WorkerAttachmentRefKind::ExistingDraftPart;
    reference.source_draft_id = 8;
    reference.source_revision = 2;
    reference.source_position = 0;
    reference.filename.set("report.txt");
    assert(input.attachment_refs.push(reference));
    Bytes encoded_input;
    assert(worker_encode_task_input(WorkerTaskKind::SaveDraftRevision, input, encoded_input, error));
    WorkerTaskInput decoded_input;
    assert(worker_decode_task_input(WorkerTaskKind::SaveDraftRevision,
        encoded_input.data(), encoded_input.size(), decoded_input, error));
    assert(decoded_input.has_draft && decoded_input.draft.revision == 2);
    assert(decoded_input.attachment_refs.size() == 1 && decoded_input.attachment_refs[0].source_revision == 2);
}

static void test_reader_result_clone_preserves_reply_context_and_bounded_metadata() {
    WorkerReaderResult source;
    source.key.folder.account_id = 72;
    source.key.folder.server_name.set("INBOX");
    source.key.uidvalidity = 931;
    source.key.uid = 42;
    source.headers_only = false;
    source.reply_to.set("reply@example.test");
    source.message_id.set("<original@example.test>");
    source.references.set("<prior@example.test> <original@example.test>");
    source.plain_text.set("bounded display preview");
    source.plain_text_truncated = true;
    WorkerReaderInlineImage image;
    image.width = 4;
    image.height = 3;
    image.xpm.set("/* XPM */");
    assert(source.inline_images.push(image));
    Text safe_link;
    safe_link.set("https://example.invalid/original");
    assert(source.safe_links.push(static_cast<Text &&>(safe_link)));
    WorkerAttachmentMetadata attachment;
    attachment.filename.set("report.bin");
    attachment.content_type.set("application/octet-stream");
    attachment.size = 1234;
    assert(source.attachments.push(attachment));

    WorkerReaderResult selected;
    assert(worker_clone_reader_result(source, selected));
    assert(selected.key == source.key);
    assert(selected.reply_to == "reply@example.test");
    assert(selected.message_id == "<original@example.test>");
    assert(selected.references == "<prior@example.test> <original@example.test>");
    assert(selected.plain_text == "bounded display preview" && selected.plain_text_truncated);
    assert(selected.inline_images.size() == 1 && selected.inline_images[0].xpm == "/* XPM */");
    assert(selected.safe_links.size() == 1 && selected.safe_links[0] == "https://example.invalid/original");
    assert(selected.attachments.size() == 1 && selected.attachments[0].filename == "report.bin");

    source.reply_to.set("changed@example.test");
    source.attachments[0].filename.set("changed.bin");
    assert(selected.reply_to == "reply@example.test");
    assert(selected.attachments[0].filename == "report.bin");
    assert(worker_clone_reader_result(selected, selected));
}

static void test_credentials_bind_full_account_configuration_and_secret_purpose() {
    Account account = credential_test_account();
    const uint64_t current_epoch = 73;
    static const unsigned char secret_bytes[] = "bound-password";
    SecretBytes password;
    assert(password.set(secret_bytes, sizeof(secret_bytes) - 1));
    SecretBytes credentials;
    Text error;
    SecretBytes missing_epoch_password;
    assert(missing_epoch_password.set(secret_bytes, sizeof(secret_bytes) - 1));
    SecretBytes missing_epoch_packet;
    assert(!worker_encode_account_credential(account, CredentialPurpose::Imap, 0,
        static_cast<SecretBytes &&>(missing_epoch_password), missing_epoch_packet, error));
    assert(worker_encode_account_credential(account, CredentialPurpose::Imap,
        current_epoch, static_cast<SecretBytes &&>(password), credentials, error));
    SecretBytes recovered;
    assert(worker_find_credential(credentials, account, CredentialPurpose::Imap,
        account.username, current_epoch, recovered, error));
    assert(recovered.size() == sizeof(secret_bytes) - 1);
    assert(!memcmp(recovered.data(), secret_bytes, sizeof(secret_bytes) - 1));
    assert(!worker_find_credential(credentials, account, CredentialPurpose::Imap,
        account.username, current_epoch + 1, recovered, error));
    Account changed = account;
    changed.outgoing_host.set("changed.example.test");
    assert(!worker_find_credential(credentials, changed, CredentialPurpose::Imap,
        changed.username, current_epoch, recovered, error));
    changed = account;
    ++changed.generation;
    assert(!worker_find_credential(credentials, changed, CredentialPurpose::Imap,
        changed.username, current_epoch, recovered, error));
    assert(!worker_find_credential(credentials, account, CredentialPurpose::Smtp,
        account.smtp_username, current_epoch, recovered, error));

    CredentialInput empty_record;
    empty_record.configuration = account;
    empty_record.purpose = CredentialPurpose::Imap;
    empty_record.username = account.username;
    SecretBytes empty_packet;
    assert(!worker_encode_credentials(&empty_record, 1, current_epoch, empty_packet, error));

    SecretBytes master;
    assert(master.set("master", 6));
    SecretBytes packet;
    assert(worker_encode_secret(WorkerSecretPurpose::VaultMaster,
        static_cast<SecretBytes &&>(master), SecretBytes(), packet, error));
    SecretBytes decoded_master, decoded_second;
    assert(worker_decode_secret(static_cast<SecretBytes &&>(packet), WorkerSecretPurpose::VaultMaster,
        decoded_master, decoded_second, error));
    assert(decoded_master.size() == 6 && decoded_second.empty());

    SecretBytes empty_master;
    assert(!worker_encode_secret(WorkerSecretPurpose::VaultMaster,
        static_cast<SecretBytes &&>(empty_master), SecretBytes(), packet, error));
    SecretBytes old_password;
    assert(old_password.set("old", 3));
    assert(!worker_encode_secret(WorkerSecretPurpose::VaultPasswordChange,
        static_cast<SecretBytes &&>(old_password), SecretBytes(), packet, error));
    SecretBytes valid_master;
    assert(valid_master.set("master", 6));
    assert(worker_encode_secret(WorkerSecretPurpose::VaultMaster,
        static_cast<SecretBytes &&>(valid_master), SecretBytes(), packet, error));
    SecretBytes invalid_packet;
    assert(invalid_packet.set(packet.data(), packet.size()));
    const_cast<unsigned char *>(invalid_packet.data())[6] = 0;
    assert(!worker_decode_secret(static_cast<SecretBytes &&>(invalid_packet),
        static_cast<WorkerSecretPurpose>(0), decoded_master, decoded_second, error));
}

static bool wait_for_result(WorkerCoordinator &coordinator, WorkerResult &result, Text &error,
    uint32_t timeout_ms) {
    struct timespec start_time;
    assert(clock_gettime(CLOCK_MONOTONIC, &start_time) == 0);
    const uint64_t started = static_cast<uint64_t>(start_time.tv_sec) * 1000 + start_time.tv_nsec / 1000000;
    for (;;) {
        assert(coordinator.pump(error));
        if (coordinator.take_result(result)) return true;
        struct timespec now_time;
        assert(clock_gettime(CLOCK_MONOTONIC, &now_time) == 0);
        const uint64_t now = static_cast<uint64_t>(now_time.tv_sec) * 1000 + now_time.tv_nsec / 1000000;
        if (now >= started + timeout_ms) return false;
        usleep(1000);
    }
}

static bool wait_until_worker_idle_without_taking_results(WorkerCoordinator &coordinator,
    Text &error, uint32_t timeout_ms) {
    struct timespec start_time;
    assert(clock_gettime(CLOCK_MONOTONIC, &start_time) == 0);
    const uint64_t started = static_cast<uint64_t>(start_time.tv_sec) * 1000 + start_time.tv_nsec / 1000000;
    for (;;) {
        if (!coordinator.pump(error)) return false;
        WorkerCoordinatorStatus status;
        if (!coordinator.status(status)) return false;
        if (!status.queued && !status.active) return true;
        struct timespec now_time;
        assert(clock_gettime(CLOCK_MONOTONIC, &now_time) == 0);
        const uint64_t now = static_cast<uint64_t>(now_time.tv_sec) * 1000 + now_time.tv_nsec / 1000000;
        if (now >= started + timeout_ms) return false;
        usleep(1000);
    }
}

static WorkerRequest test_request(uint64_t id, WorkerTaskKind kind, int inherited_fd = -1) {
    WorkerRequest request;
    request.request_id = id;
    request.profile_epoch = 1;
    request.ui_generation = 44;
    request.kind = kind;
    if (kind == WorkerTaskKind::TestProbe) {
        unsigned char fd[4];
        put_u32(fd, inherited_fd < 0 ? UINT32_MAX : static_cast<uint32_t>(inherited_fd));
        request.payload.set(fd, sizeof(fd));
    }
    return request;
}

static bool submit_current_epoch(WorkerCoordinator &coordinator, WorkerRequest &request,
    SecretBytes secret, Text &error) {
    request.profile_epoch = coordinator.profile_epoch();
    return coordinator.submit(request, static_cast<SecretBytes &&>(secret), error);
}

static void submit_child_task(WorkerCoordinator &coordinator, uint64_t id,
    WorkerTaskKind kind, uint64_t account_id, uint64_t account_generation,
    const WorkerTaskInput *input, SecretBytes secret, WorkerResult &result) {
    WorkerRequest request = test_request(id, kind);
    request.profile_epoch = coordinator.profile_epoch();
    request.account_id = account_id;
    request.account_generation = account_generation;
    if (input) {
        Text error;
        assert(worker_encode_task_input(kind, *input, request.payload, error));
    }
    Text error;
    if (!coordinator.submit(request, static_cast<SecretBytes &&>(secret), error)) {
        fprintf(stderr, "worker submit %llu kind %u rejected: %s\n",
            static_cast<unsigned long long>(id), static_cast<unsigned>(kind), error.c_str());
        assert(false);
    }
    const bool completed = wait_for_result(coordinator, result, error, 10000);
    if (!completed) fprintf(stderr, "worker request timed out id=%llu kind=%u diagnostic=%s\n",
        static_cast<unsigned long long>(id), static_cast<unsigned>(kind), error.c_str());
    assert(completed);
    assert(result.request_id == id);
}

static Account task_candidate_account();

static SecretBytes legacy_remembered_credentials_v6(const SecretBytes &live_packet) {
    constexpr size_t outer_header_size = 18;
    assert(live_packet.size() >= outer_header_size);
    assert(!memcmp(live_packet.data(), "RMCL", 4));
    const unsigned char *outer = live_packet.data();
    const uint32_t payload_size = (static_cast<uint32_t>(outer[14]) << 24) |
        (static_cast<uint32_t>(outer[15]) << 16) |
        (static_cast<uint32_t>(outer[16]) << 8) | static_cast<uint32_t>(outer[17]);
    assert(payload_size == live_packet.size() - outer_header_size);
    SecretBytes legacy;
    assert(legacy.set(outer + outer_header_size, payload_size));
    assert(legacy.size() >= 6 && !memcmp(legacy.data(), "RMCV", 4));
    unsigned char *payload = const_cast<unsigned char *>(legacy.data());
    payload[4] = 0;
    payload[5] = 6;
    assert(payload[4] == 0 && payload[5] == 6);
    return legacy;
}

static void test_legacy_v6_vault_unlock_rebinds_and_reads_credentials() {
    WorkerProfileFixture profile;
    Text error;
    WorkerCoordinator coordinator;
    assert(coordinator.open("/proc/self/exe", profile.paths, error));

    WorkerTaskInput input;
    input.account = task_candidate_account();
    WorkerResult result;
    submit_child_task(coordinator, 1801, WorkerTaskKind::AccountCreate, 0, 0,
        &input, SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    WorkerAccountResult created;
    assert(worker_decode_account_result(result.payload.data(), result.payload.size(), created, error));
    Account alice = created.account;
    assert(alice.id && alice.generation && alice.is_default);

    static const unsigned char imap_bytes[] = "alice-imap-secret";
    static const unsigned char smtp_bytes[] = "alice-smtp-secret";
    CredentialInput credential_records[2];
    credential_records[0].configuration = alice;
    credential_records[0].purpose = CredentialPurpose::Imap;
    credential_records[0].username = alice.username;
    credential_records[0].secret = imap_bytes;
    credential_records[0].secret_size = sizeof(imap_bytes) - 1;
    credential_records[1].configuration = alice;
    credential_records[1].purpose = CredentialPurpose::Smtp;
    credential_records[1].username = alice.smtp_username;
    credential_records[1].secret = smtp_bytes;
    credential_records[1].secret_size = sizeof(smtp_bytes) - 1;
    SecretBytes original_live_packet;
    assert(worker_encode_credentials(credential_records, 2, coordinator.profile_epoch(),
        original_live_packet, error));
    SecretBytes legacy_payload = legacy_remembered_credentials_v6(original_live_packet);
    Vault legacy_vault;
    assert(legacy_vault.open(profile.paths, error));
    assert(legacy_vault.save(worker_test_secret("default-metadata master"),
        static_cast<SecretBytes &&>(legacy_payload), error));
    legacy_vault.close();

    input = WorkerTaskInput();
    input.account = task_candidate_account();
    input.account.display_name.set("Bob default account");
    input.account.email_address.set("bob@reliefmail.test");
    input.account.username.set("bob@reliefmail.test");
    input.account.smtp_username.set("bob@reliefmail.test");
    input.account.is_default = true;
    submit_child_task(coordinator, 1803, WorkerTaskKind::AccountCreate, 0, 0,
        &input, SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);

    Store store;
    assert(store.open(profile.paths, StoreOpenMode::Worker, error));
    Account current_alice;
    assert(store.get_account(alice.id, current_alice, error));
    store.close();
    assert(current_alice.generation == alice.generation && !current_alice.is_default);

    SecretBytes password;
    assert(!worker_find_credential(original_live_packet, current_alice, CredentialPurpose::Imap,
        current_alice.username, coordinator.profile_epoch(), password, error));

    SecretBytes unlock_secret;
    assert(worker_encode_secret(WorkerSecretPurpose::VaultMaster,
        worker_test_secret("default-metadata master"), SecretBytes(), unlock_secret, error));
    WorkerRequest unlock = test_request(1804, WorkerTaskKind::UnlockVault);
    assert(submit_current_epoch(coordinator, unlock, static_cast<SecretBytes &&>(unlock_secret), error));
    assert(wait_for_result(coordinator, result, error, 10000));
    if (result.status != WorkerStatus::Ok)
        fprintf(stderr, "legacy RMCV v6 vault unlock returned status %u: %s\n",
            static_cast<unsigned>(result.status), result.diagnostic.c_str());
    assert(result.status == WorkerStatus::Ok && !result.secret_payload.empty());
    assert(worker_find_credential(result.secret_payload, current_alice, CredentialPurpose::Imap,
        current_alice.username, coordinator.profile_epoch(), password, error));
    assert(password.size() == sizeof(imap_bytes) - 1 && !memcmp(password.data(), imap_bytes, sizeof(imap_bytes) - 1));
    const Text &smtp_user = current_alice.smtp_username.empty() ? current_alice.username : current_alice.smtp_username;
    assert(worker_find_credential(result.secret_payload, current_alice, CredentialPurpose::Smtp,
        smtp_user, coordinator.profile_epoch(), password, error));
    assert(password.size() == sizeof(smtp_bytes) - 1 && !memcmp(password.data(), smtp_bytes, sizeof(smtp_bytes) - 1));

    Account changed_configuration = current_alice;
    changed_configuration.incoming_host.set("changed-after-unlock.example.test");
    assert(!worker_find_credential(result.secret_payload, changed_configuration, CredentialPurpose::Imap,
        changed_configuration.username, coordinator.profile_epoch(), password, error));
    coordinator.close();
}

static WorkerResult execute_direct_task(const ProfilePaths &paths, WorkerRequest request,
    const WorkerTaskInput &input, SecretBytes secret = SecretBytes()) {
    request.profile_paths = paths;
    Text error;
    assert(worker_encode_task_input(request.kind, input, request.payload, error));
    OperationContext context;
    WorkerResult result;
    (void)worker_execute_task(request, static_cast<SecretBytes &&>(secret), context, result);
    return static_cast<WorkerResult &&>(result);
}

static size_t worker_process_fd_count() {
    DIR *directory = opendir("/proc/self/fd");
    assert(directory);
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)))
        if (entry->d_name[0] != '.') ++count;
    assert(closedir(directory) == 0);
    return count;
}

static void test_draft_tasks_require_existing_revision_and_account_identity() {
    WorkerProfileFixture profile;
    Text error;
    Store store;
    assert(store.open(profile.paths, StoreOpenMode::Worker, error));
    Account account = task_candidate_account();
    assert(store.save_account(account, error));
    Draft draft;
    draft.account_id = account.id;
    assert(draft.subject.set("worker guarded draft"));
    assert(draft.body.set("saved content"));
    Attachment source_attachment;
    assert(source_attachment.filename.set("cross-account-source.bin"));
    assert(source_attachment.content_type.set("application/octet-stream"));
    assert(source_attachment.data.set("source bytes", sizeof("source bytes") - 1));
    assert(draft.attachments.push(static_cast<Attachment &&>(source_attachment)));
    assert(store.save_draft_guarded(draft, account.generation, 0, error));

    WorkerRequest list_request = test_request(1909, WorkerTaskKind::ListDrafts);
    list_request.account_id = account.id;
    list_request.account_generation = account.generation;
    WorkerResult list_result = execute_direct_task(profile.paths, list_request, WorkerTaskInput());
    assert(list_result.status == WorkerStatus::Ok);
    WorkerDraftList listed_drafts;
    assert(worker_decode_draft_list(list_result.payload.data(), list_result.payload.size(), listed_drafts, error));
    assert(listed_drafts.account_generation == account.generation);
    assert(listed_drafts.drafts.size() == 1);
    assert(listed_drafts.drafts[0].id == draft.id && listed_drafts.drafts[0].revision == 1);
    assert(listed_drafts.drafts[0].subject == "worker guarded draft");

    WorkerRequest wrong_account_list = test_request(1910, WorkerTaskKind::ListDrafts);
    wrong_account_list.account_id = account.id + 10000;
    wrong_account_list.account_generation = account.generation;
    list_result = execute_direct_task(profile.paths, wrong_account_list, WorkerTaskInput());
    assert(list_result.status == WorkerStatus::InvalidRequest);
    assert(list_result.payload.empty());

    WorkerRequest update = test_request(1901, WorkerTaskKind::SaveDraftRevision);
    update.account_id = account.id;
    update.account_generation = account.generation;
    WorkerTaskInput input;
    input.item_id = draft.id;
    input.item_revision = 0;
    input.has_draft = true;
    input.draft.id = draft.id;
    input.draft.account_id = account.id;
    input.draft.subject = draft.subject;
    input.draft.body.set("zero revision overwrite");
    WorkerResult result = execute_direct_task(profile.paths, update, input);
    assert(result.status == WorkerStatus::InvalidRequest);
    Draft unchanged;
    assert(store.get_draft(draft.id, unchanged, error));
    assert(unchanged.revision == 1 && unchanged.body == "saved content");
    store.release_draft_sources(unchanged);

    Account other = task_candidate_account();
    other.email_address.set("other@reliefmail.test");
    other.username.set("other-user");
    other.smtp_username.set("other-user");
    assert(store.save_account(other, error));
    WorkerRequest load_other = test_request(1908, WorkerTaskKind::LoadDraft);
    load_other.account_id = other.id;
    load_other.account_generation = other.generation;
    WorkerTaskInput load_other_input;
    load_other_input.item_id = draft.id;
    const size_t fd_count_before_rejected_load = worker_process_fd_count();
    result = execute_direct_task(profile.paths, load_other, load_other_input);
    assert(result.status == WorkerStatus::InvalidRequest);
    assert(result.payload.empty() && result.secret_payload.empty());
    assert(worker_process_fd_count() == fd_count_before_rejected_load);

    update.request_id = 1902;
    update.account_id = other.id;
    update.account_generation = other.generation;
    input.item_revision = 1;
    input.draft.account_id = other.id;
    input.draft.revision = 1;
    result = execute_direct_task(profile.paths, update, input);
    assert(result.status == WorkerStatus::InvalidRequest);
    assert(store.get_draft(draft.id, unchanged, error));
    assert(unchanged.account_id == account.id && unchanged.body == "saved content");
    store.release_draft_sources(unchanged);

    update.request_id = 1904;
    update.account_id = account.id;
    update.account_generation = account.generation;
    input.item_revision = 1;
    input.draft.account_id = account.id;
    input.draft.revision = 1;
    input.draft.body.set("valid update");
    result = execute_direct_task(profile.paths, update, input);
    assert(result.status == WorkerStatus::Ok);
    assert(store.get_draft(draft.id, unchanged, error));
    assert(unchanged.revision == 2 && unchanged.body == "valid update");
    store.release_draft_sources(unchanged);

    update.request_id = 1905;
    input.draft.body.set("stale update");
    result = execute_direct_task(profile.paths, update, input);
    assert(result.status == WorkerStatus::InvalidRequest);
    assert(store.get_draft(draft.id, unchanged, error));
    assert(unchanged.revision == 2 && unchanged.body == "valid update");
    store.release_draft_sources(unchanged);

    WorkerRequest deletion = test_request(1903, WorkerTaskKind::DeleteDraft);
    deletion.account_id = account.id;
    deletion.account_generation = account.generation;
    WorkerTaskInput delete_input;
    delete_input.item_id = draft.id;
    delete_input.item_revision = 0;
    result = execute_direct_task(profile.paths, deletion, delete_input);
    assert(result.status == WorkerStatus::InvalidRequest);
    delete_input.item_revision = 1;
    deletion.request_id = 1906;
    result = execute_direct_task(profile.paths, deletion, delete_input);
    assert(result.status == WorkerStatus::InvalidRequest);
    assert(store.get_draft(draft.id, unchanged, error));
    assert(unchanged.revision == 2 && unchanged.body == "valid update");
    store.release_draft_sources(unchanged);
    delete_input.item_revision = 2;
    deletion.request_id = 1907;
    result = execute_direct_task(profile.paths, deletion, delete_input);
    assert(result.status == WorkerStatus::Ok);
    assert(!store.get_draft(draft.id, unchanged, error));
    store.close();
}

static void test_forward_as_attachment_uses_exact_full_cached_original() {
    WorkerProfileFixture profile;
    Text error;
    Store store;
    assert(store.open(profile.paths, StoreOpenMode::Worker, error));
    Account account = task_candidate_account();
    assert(store.save_account(account, error));
    Folder folder;
    folder.key.account_id = account.id;
    folder.key.server_name.set("INBOX");
    folder.display_name.set("Inbox");
    folder.uidvalidity = 337;
    assert(store.save_folder_metadata_guarded(folder, account.generation, 0, error));
    assert(store.get_folder(folder.key, folder, error));

    MessageSummary original;
    original.key.folder = folder.key;
    original.key.uidvalidity = folder.uidvalidity;
    original.key.uid = 61;
    original.from.set("sender@example.test");
    original.subject.set("Full original source");
    const char raw_bytes[] = "From: sender@example.test\r\nSubject: Full original source\r\n\r\n"
        "preview prefix\n\xe4\xb8\xad\xe6\x96\x87 original body with trailing bytes\r\n";
    Bytes raw;
    assert(raw.set(raw_bytes, sizeof(raw_bytes) - 1));
    original.size = raw.size();
    assert(store.save_message_guarded(original, raw, account.generation, folder.generation, error));
    store.close();

    WorkerRequest request = test_request(1920, WorkerTaskKind::SaveForwardDraft);
    request.account_id = account.id;
    request.account_generation = account.generation;
    WorkerTaskInput input;
    input.has_draft = true;
    input.message_key = original.key;
    input.draft.account_id = account.id;
    input.draft.subject.set("Fwd: Full original source");
    input.draft.raw_recipient_input = true;
    assert(input.draft.to_input.set("bob@example.test"));
    input.draft.body.set("Forwarded display body; original attached separately.");
    WorkerResult result = execute_direct_task(profile.paths, request, input);
    assert(result.status == WorkerStatus::Ok);
    WorkerDraftSnapshot saved;
    assert(worker_decode_draft_snapshot(result.payload.data(), result.payload.size(), saved, error));
    assert(saved.draft.id && saved.draft.revision == 1 && saved.attachments.size() == 1);
    assert(saved.attachments[0].filename == "forwarded-message.eml");
    assert(saved.attachments[0].content_type == "application/octet-stream");
    assert(saved.attachments[0].size == raw.size());

    assert(store.open(profile.paths, StoreOpenMode::Worker, error));
    Draft persisted;
    assert(store.get_draft(saved.draft.id, persisted, error));
    assert(persisted.attachments.size() == 1);
    const Attachment &stored_attachment = persisted.attachments[0];
    const size_t stored_size = stored_attachment.source.read ?
        stored_attachment.source.size : stored_attachment.data.size();
    assert(stored_size == raw.size());
    unsigned char *materialized = static_cast<unsigned char *>(malloc(stored_size));
    assert(materialized);
    size_t offset = 0;
    while (offset < stored_size) {
        const size_t capacity = stored_size - offset < 64 ? stored_size - offset : 64;
        size_t count = 0;
        if (stored_attachment.source.read) {
            assert(stored_attachment.source.read(stored_attachment.source.context,
                offset, materialized + offset, capacity, &count));
        } else {
            count = capacity;
            memcpy(materialized + offset, stored_attachment.data.data() + offset, count);
        }
        assert(count && count <= capacity);
        offset += count;
    }
    assert(!memcmp(materialized, raw.data(), raw.size()));
    free(materialized);
    store.release_draft_sources(persisted);
    store.close();
}

static void test_copy_draft_to_another_account_preserves_source_and_attachment() {
    WorkerProfileFixture profile;
    Text error;
    Store store;
    assert(store.open(profile.paths, StoreOpenMode::Worker, error));
    Account source_account = task_candidate_account();
    assert(store.save_account(source_account, error));
    Account target_account = task_candidate_account();
    target_account.email_address.set("copy-target@reliefmail.test");
    target_account.username.set("copy-target");
    target_account.smtp_username.set("copy-target");
    target_account.is_default = false;
    assert(store.save_account(target_account, error));

    Draft source;
    source.account_id = source_account.id;
    source.raw_recipient_input = true;
    assert(source.to_input.set("incomplete recipient @"));
    assert(source.subject.set("Cross-account draft copy"));
    assert(source.body.set("unsent source body"));
    Attachment original_part;
    assert(original_part.filename.set("evidence.bin"));
    assert(original_part.content_type.set("application/octet-stream"));
    const unsigned char original_bytes[] = {0, 1, 2, 0xff, 0x80, 0x0a};
    assert(original_part.data.set(original_bytes, sizeof(original_bytes)));
    assert(source.attachments.push(static_cast<Attachment &&>(original_part)));
    assert(store.save_draft_guarded(source, source_account.generation, 0, error));
    store.close();

    WorkerRequest request = test_request(1921, WorkerTaskKind::SaveDraftCopy);
    request.account_id = target_account.id;
    request.account_generation = target_account.generation;
    WorkerTaskInput input;
    input.item_id = source.id;
    input.item_revision = source.revision;
    input.account = source_account;
    input.has_draft = true;
    input.draft.account_id = target_account.id;
    input.draft.raw_recipient_input = true;
    assert(input.draft.to_input.set("incomplete recipient @"));
    assert(input.draft.subject.set("Cross-account draft copy"));
    assert(input.draft.body.set("edited copy body"));
    WorkerDraftAttachmentRef source_part;
    source_part.kind = WorkerAttachmentRefKind::ExistingDraftPart;
    source_part.source_draft_id = source.id;
    source_part.source_revision = source.revision;
    source_part.source_position = 0;
    assert(input.attachment_refs.push(static_cast<WorkerDraftAttachmentRef &&>(source_part)));
    WorkerResult result = execute_direct_task(profile.paths, request, input);
    assert(result.status == WorkerStatus::Ok);
    WorkerDraftSnapshot copied;
    assert(worker_decode_draft_snapshot(result.payload.data(), result.payload.size(), copied, error));
    assert(copied.account_generation == target_account.generation);
    assert(copied.draft.account_id == target_account.id && copied.draft.id &&
        copied.draft.id != source.id && copied.draft.revision == 1);
    assert(copied.draft.raw_recipient_input && copied.draft.to_input == "incomplete recipient @");
    assert(copied.draft.body == "edited copy body");
    assert(copied.attachments.size() == 1 && copied.attachments[0].filename == "evidence.bin");
    assert(copied.attachments[0].size == sizeof(original_bytes));

    assert(store.open(profile.paths, StoreOpenMode::Worker, error));
    Draft unchanged;
    assert(store.get_draft(source.id, unchanged, error));
    assert(unchanged.account_id == source_account.id && unchanged.revision == source.revision);
    assert(unchanged.body == "unsent source body" && unchanged.raw_recipient_input &&
        unchanged.to_input == "incomplete recipient @");
    store.release_draft_sources(unchanged);
    store.close();
}

static void test_compose_catalog_returns_account_signature_templates_and_contacts() {
    WorkerProfileFixture profile;
    Text error;
    Store store;
    assert(store.open(profile.paths, StoreOpenMode::Worker, error));
    Account account = task_candidate_account();
    assert(store.save_account(account, error));
    Signature signature;
    assert(signature.name.set("Relief signature"));
    assert(signature.body.set("With care,\nRelief Team"));
    assert(store.save_signature(signature, error));
    account.signature_id = signature.id;
    assert(store.save_account(account, error));
    Template message_template;
    assert(message_template.name.set("Aid update"));
    assert(message_template.subject.set("Relief update"));
    assert(message_template.body.set("Current project update"));
    assert(store.save_template(message_template, error));
    Contact contact;
    assert(contact.display_name.set("Relief Coordinator"));
    Address address;
    assert(address.display_name.set("Relief Coordinator"));
    assert(address.local_part.set("coord"));
    assert(address.domain.set("reliefmail.test"));
    assert(contact.addresses.push(static_cast<Address &&>(address)));
    assert(store.save_contact(contact, error));
    store.close();

    WorkerRequest request = test_request(1922, WorkerTaskKind::ComposeCatalog);
    request.account_id = account.id;
    request.account_generation = account.generation;
    WorkerTaskInput input;
    assert(input.query.set("relief"));
    WorkerResult result = execute_direct_task(profile.paths, request, input);
    assert(result.status == WorkerStatus::Ok);
    WorkerComposeCatalog catalog;
    assert(worker_decode_compose_catalog(result.payload.data(), result.payload.size(), catalog, error));
    assert(catalog.account_generation == account.generation && catalog.has_signature);
    assert(catalog.signature_id == signature.id && catalog.signature_body == "With care,\nRelief Team");
    assert(catalog.templates.size() == 1 && catalog.templates[0].id == message_template.id);
    assert(catalog.templates[0].name == "Aid update" && catalog.templates[0].subject == "Relief update");
    assert(catalog.contacts.size() == 1 && catalog.contacts[0].display_name == "Relief Coordinator");
    assert(catalog.contacts[0].address == "Relief Coordinator <coord@reliefmail.test>");

    request.request_id++;
    input = WorkerTaskInput();
    input.item_id = message_template.id;
    result = execute_direct_task(profile.paths, request, input);
    assert(result.status == WorkerStatus::Ok);
    WorkerComposeCatalog selected;
    assert(worker_decode_compose_catalog(result.payload.data(), result.payload.size(), selected, error));
    assert(selected.selected_template_id == message_template.id);
    assert(selected.selected_template_subject == "Relief update");
    assert(selected.selected_template_body == "Current project update");
}

static void test_folder_worker_requests_reject_missing_and_contradictory_guards() {
    WorkerProfileFixture profile;
    Text error;
    Store store;
    assert(store.open(profile.paths, StoreOpenMode::Worker, error));
    Account account = task_candidate_account();
    assert(store.save_account(account, error));
    Folder inbox;
    inbox.key.account_id = account.id;
    inbox.key.server_name.set("INBOX");
    inbox.display_name.set("Inbox");
    inbox.uidvalidity = 4242;
    assert(store.save_folder_metadata_guarded(inbox, account.generation, 0, error));
    FolderKey inbox_key = inbox.key;
    assert(store.get_folder(inbox_key, inbox, error));
    Folder container;
    container.key.account_id = account.id;
    container.key.server_name.set("Projects");
    container.display_name.set("Projects");
    container.selectable = false;
    assert(store.save_folder_metadata_guarded(container, account.generation, 0, error));
    FolderKey container_key = container.key;
    assert(store.get_folder(container_key, container, error));
    Folder archive;
    archive.key.account_id = account.id;
    archive.key.server_name.set("Archive");
    archive.display_name.set("Archive");
    archive.uidvalidity = 9001;
    assert(store.save_folder_metadata_guarded(archive, account.generation, 0, error));
    FolderKey archive_key = archive.key;
    assert(store.get_folder(archive_key, archive, error));
    MessageSummary summary;
    summary.key.folder = inbox.key;
    summary.key.uidvalidity = inbox.uidvalidity;
    summary.key.uid = 7;
    summary.from.set("sender@example.test");
    summary.subject.set("cached reader item");
    Bytes raw;
    static const char raw_message[] = "From: sender@example.test\r\nSubject: cached reader item\r\n\r\nbody\r\n";
    assert(raw.set(raw_message, sizeof(raw_message) - 1));
    assert(store.save_message_guarded(summary, raw, account.generation, inbox.generation, error));

    WorkerRequest read = test_request(1911, WorkerTaskKind::ReadMessageChunk);
    read.account_id = account.id;
    read.account_generation = account.generation;
    WorkerTaskInput read_input;
    read_input.message_key = summary.key;
    read_input.chunk_size = 16;
    WorkerResult result = execute_direct_task(profile.paths, read, read_input);
    assert(result.status == WorkerStatus::InvalidRequest);

    WorkerRequest page = test_request(1912, WorkerTaskKind::LocalPage);
    page.account_id = account.id;
    page.account_generation = account.generation;
    WorkerTaskInput page_input;
    page_input.page_query.scope = MessageQueryScope::Folder;
    page_input.page_query.account_id = account.id;
    page_input.page_query.folder = inbox.key;
    result = execute_direct_task(profile.paths, page, page_input);
    assert(result.status == WorkerStatus::InvalidRequest);

    WorkerRequest sync = test_request(1913, WorkerTaskKind::HeaderSync);
    sync.account_id = account.id;
    sync.account_generation = account.generation;
    sync.folder_server_name = inbox.key.server_name;
    sync.folder_uidvalidity = inbox.uidvalidity;
    WorkerTaskInput sync_input;
    sync_input.message_key.folder = inbox.key;
    result = execute_direct_task(profile.paths, sync, sync_input);
    assert(result.status == WorkerStatus::InvalidRequest);

    WorkerRequest flags = test_request(1914, WorkerTaskKind::SetFlags);
    flags.account_id = account.id;
    flags.account_generation = account.generation;
    flags.folder_server_name = inbox.key.server_name;
    flags.folder_uidvalidity = inbox.uidvalidity;
    flags.message_uid = summary.key.uid;
    WorkerTaskInput flags_input;
    flags_input.message_key = summary.key;
    flags_input.flags_add = 1;
    result = execute_direct_task(profile.paths, flags, flags_input);
    assert(result.status == WorkerStatus::InvalidRequest);

    WorkerRequest copy = test_request(1915, WorkerTaskKind::CopyMessage);
    copy.account_id = account.id;
    copy.account_generation = account.generation;
    copy.folder_server_name = inbox.key.server_name;
    copy.folder_generation = inbox.generation;
    copy.folder_uidvalidity = inbox.uidvalidity;
    copy.message_uid = summary.key.uid;
    WorkerTaskInput copy_input;
    copy_input.message_key = summary.key;
    copy_input.destination_folder = archive.key;
    copy_input.destination_folder_uidvalidity = archive.uidvalidity;
    result = execute_direct_task(profile.paths, copy, copy_input);
    assert(result.status == WorkerStatus::InvalidRequest);
    copy_input.destination_folder_generation = archive.generation;
    copy_input.destination_folder_uidvalidity = 0;
    copy.request_id = 1918;
    result = execute_direct_task(profile.paths, copy, copy_input);
    assert(result.status == WorkerStatus::InvalidRequest);
    copy_input.destination_folder_uidvalidity = archive.uidvalidity;
    copy.request_id = 1921;
    result = execute_direct_task(profile.paths, copy, copy_input);
    assert(result.status == WorkerStatus::Failed); // Identity passed; no secret was supplied for network work.

    Folder unknown_destination;
    unknown_destination.key.account_id = account.id;
    unknown_destination.key.server_name.set("UnknownDestination");
    unknown_destination.display_name.set("Unknown destination");
    unknown_destination.selectable = true;
    assert(store.save_folder_metadata_guarded(unknown_destination, account.generation, 0, error));
    FolderKey unknown_destination_key = unknown_destination.key;
    assert(store.get_folder(unknown_destination_key, unknown_destination, error));
    const WorkerTaskKind destination_kinds[] = {
        WorkerTaskKind::MoveMessage, WorkerTaskKind::CopyMessage
    };
    bool both_unknown_destinations_rejected = true;
    for (size_t i = 0; i < sizeof(destination_kinds) / sizeof(destination_kinds[0]); ++i) {
        WorkerRequest unknown_destination_request = test_request(1922 + i, destination_kinds[i]);
        unknown_destination_request.account_id = account.id;
        unknown_destination_request.account_generation = account.generation;
        unknown_destination_request.folder_server_name = inbox.key.server_name;
        unknown_destination_request.folder_generation = inbox.generation;
        unknown_destination_request.folder_uidvalidity = inbox.uidvalidity;
        unknown_destination_request.message_uid = summary.key.uid;
        WorkerTaskInput unknown_destination_input;
        unknown_destination_input.message_key = summary.key;
        unknown_destination_input.destination_folder = unknown_destination.key;
        unknown_destination_input.destination_folder_generation = unknown_destination.generation;
        unknown_destination_input.destination_folder_uidvalidity = 0;
        result = execute_direct_task(profile.paths, unknown_destination_request, unknown_destination_input);
        if (result.status != WorkerStatus::InvalidRequest) {
            both_unknown_destinations_rejected = false;
            fprintf(stderr, "Move/Copy unknown destination status: kind=%u status=%u\n",
                static_cast<unsigned>(destination_kinds[i]), static_cast<unsigned>(result.status));
        }
    }
    assert(both_unknown_destinations_rejected);

    WorkerRequest known_folder_operation = test_request(1919, WorkerTaskKind::FolderOperation);
    known_folder_operation.account_id = account.id;
    known_folder_operation.account_generation = account.generation;
    known_folder_operation.folder_server_name = inbox.key.server_name;
    known_folder_operation.folder_generation = inbox.generation;
    known_folder_operation.folder_uidvalidity = 0;
    WorkerTaskInput known_folder_operation_input;
    known_folder_operation_input.folder_action = WorkerFolderAction::Unsubscribe;
    assert(known_folder_operation_input.query.set("INBOX"));
    result = execute_direct_task(profile.paths, known_folder_operation, known_folder_operation_input);
    assert(result.status == WorkerStatus::InvalidRequest);

    Folder discovery;
    discovery.key.account_id = account.id;
    discovery.key.server_name.set("Discovery");
    discovery.display_name.set("Discovery");
    discovery.selectable = true;
    assert(store.save_folder_metadata_guarded(discovery, account.generation, 0, error));
    FolderKey discovery_key = discovery.key;
    assert(store.get_folder(discovery_key, discovery, error));
    WorkerRequest unknown_folder_operation = test_request(1920, WorkerTaskKind::FolderOperation);
    unknown_folder_operation.account_id = account.id;
    unknown_folder_operation.account_generation = account.generation;
    unknown_folder_operation.folder_server_name = discovery.key.server_name;
    unknown_folder_operation.folder_generation = discovery.generation;
    unknown_folder_operation.folder_uidvalidity = 0;
    WorkerTaskInput unknown_folder_operation_input;
    unknown_folder_operation_input.folder_action = WorkerFolderAction::Unsubscribe;
    assert(unknown_folder_operation_input.query.set("Discovery"));
    result = execute_direct_task(profile.paths, unknown_folder_operation, unknown_folder_operation_input);
    assert(result.status == WorkerStatus::Failed);

    WorkerRequest folder = test_request(1916, WorkerTaskKind::FolderOperation);
    folder.profile_epoch = 1;
    folder.account_id = account.id;
    folder.account_generation = account.generation;
    folder.folder_server_name = container.key.server_name;
    folder.folder_generation = container.generation;
    folder.folder_uidvalidity = container.uidvalidity;
    WorkerTaskInput folder_input;
    folder_input.folder_action = WorkerFolderAction::Unsubscribe;
    assert(folder_input.query.set("Projects"));
    result = execute_direct_task(profile.paths, folder, folder_input);
    if (result.status != WorkerStatus::InvalidRequest)
        fprintf(stderr, "nonselectable folder mutation returned status %u: %s\n",
            static_cast<unsigned>(result.status), result.diagnostic.c_str());
    assert(result.status == WorkerStatus::InvalidRequest);

    Folder protected_special;
    protected_special.key.account_id = account.id;
    protected_special.key.server_name.set("ProviderSent");
    protected_special.display_name.set("Provider Sent");
    protected_special.role = FolderRole::Sent;
    protected_special.uidvalidity = 7101;
    protected_special.selectable = true;
    assert(store.save_folder_metadata_guarded(protected_special, account.generation, 0, error));
    FolderKey protected_special_key = protected_special.key;
    assert(store.get_folder(protected_special_key, protected_special, error));
    const WorkerFolderAction protected_actions[] = {
        WorkerFolderAction::Rename, WorkerFolderAction::Delete
    };
    for (size_t i = 0; i < sizeof(protected_actions) / sizeof(protected_actions[0]); ++i) {
        WorkerRequest protected_request = test_request(1924 + i, WorkerTaskKind::FolderOperation);
        protected_request.profile_epoch = 1;
        protected_request.account_id = account.id;
        protected_request.account_generation = account.generation;
        protected_request.folder_server_name = protected_special.key.server_name;
        protected_request.folder_generation = protected_special.generation;
        protected_request.folder_uidvalidity = protected_special.uidvalidity;
        WorkerTaskInput protected_input;
        protected_input.folder_action = protected_actions[i];
        if (protected_actions[i] == WorkerFolderAction::Rename)
            assert(protected_input.query.set("Renamed provider folder"));
        result = execute_direct_task(profile.paths, protected_request, protected_input);
        if (result.status != WorkerStatus::InvalidRequest)
            fprintf(stderr, "protected special-role folder mutation returned status %u: %s\n",
                static_cast<unsigned>(result.status), result.diagnostic.c_str());
        assert(result.status == WorkerStatus::InvalidRequest);
    }

    WorkerRequest mismatch = test_request(1917, WorkerTaskKind::HeaderSync);
    mismatch.account_id = account.id;
    mismatch.account_generation = account.generation;
    mismatch.folder_server_name = inbox.key.server_name;
    mismatch.folder_generation = inbox.generation;
    mismatch.folder_uidvalidity = inbox.uidvalidity;
    WorkerTaskInput mismatch_input;
    mismatch_input.message_key.folder = container.key;
    result = execute_direct_task(profile.paths, mismatch, mismatch_input);
    assert(result.status == WorkerStatus::InvalidRequest);
    store.close();
}

static void submit_folder_task(WorkerCoordinator &coordinator, uint64_t id,
    WorkerTaskKind kind, const Account &account, const Folder &folder,
    const MessageKey &key, WorkerTaskInput &input, SecretBytes secret, WorkerResult &result) {
    WorkerRequest request = test_request(id, kind);
    request.profile_epoch = coordinator.profile_epoch();
    request.account_id = account.id;
    request.account_generation = account.generation;
    request.folder_server_name = folder.key.server_name;
    request.folder_generation = folder.generation;
    request.folder_uidvalidity = key.uidvalidity;
    request.message_uid = key.uid;
    input.message_key = key;
    Text codec_error;
    assert(worker_encode_task_input(kind, input, request.payload, codec_error));
    Text error;
    if (!coordinator.submit(request, static_cast<SecretBytes &&>(secret), error)) {
        fprintf(stderr, "folder task submit %llu kind %u rejected: %s\n",
            static_cast<unsigned long long>(id), static_cast<unsigned>(kind), error.c_str());
        assert(false);
    }
    assert(wait_for_result(coordinator, result, error, 10000));
    assert(result.request_id == id);
}

static void test_large_reader_result_is_a_utf8_preview_and_raw_chunks_are_exact() {
    WorkerProfileFixture profile;
    Text error;
    Store store;
    assert(store.open(profile.paths, StoreOpenMode::Worker, error));
    Account account = task_candidate_account();
    assert(store.save_account(account, error));
    Folder folder;
    folder.key.account_id = account.id;
    assert(folder.key.server_name.set("Large reader"));
    assert(folder.display_name.set("Large reader"));
    folder.uidvalidity = 998877;
    assert(store.save_folder_metadata_guarded(folder, account.generation, 0, error));
    FolderKey folder_key = folder.key;
    assert(store.get_folder(folder_key, folder, error));

    static const char header[] =
        "From: sender@reliefmail.test\r\n"
        "To: alice@reliefmail.test\r\n"
        "Subject: large reader preview boundary\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n\r\n";
    const size_t body_size = 3u * 1024u * 1024u;
    const size_t header_size = sizeof(header) - 1;
    char *raw_buffer = static_cast<char *>(malloc(header_size + body_size));
    assert(raw_buffer);
    memcpy(raw_buffer, header, header_size);
    memset(raw_buffer + header_size, 'a', body_size);
    static const char chinese[] = "\xe4\xb8\xad";
    memcpy(raw_buffer + header_size + MaxReaderPlainPreviewBytes - 1, chinese, sizeof(chinese) - 1);
    Bytes raw;
    assert(raw.set(raw_buffer, header_size + body_size));
    free(raw_buffer);

    MessageSummary summary;
    summary.key.folder = folder.key;
    summary.key.uidvalidity = folder.uidvalidity;
    summary.key.uid = 1;
    summary.from.set("sender@reliefmail.test");
    summary.subject.set("large reader preview boundary");
    summary.size = raw.size();
    assert(store.save_message_guarded(summary, raw, account.generation, folder.generation, error));
    store.close();

    WorkerTaskInput parse_input;
    parse_input.message_key = summary.key;
    WorkerRequest parse = test_request(1981, WorkerTaskKind::ParseMessage);
    parse.account_id = account.id;
    parse.account_generation = account.generation;
    parse.folder_server_name = folder.key.server_name;
    parse.folder_generation = folder.generation;
    parse.folder_uidvalidity = folder.uidvalidity;
    parse.message_uid = summary.key.uid;
    WorkerResult result = execute_direct_task(profile.paths, parse, parse_input);
    assert(result.status == WorkerStatus::Ok);
    WorkerReaderResult reader;
    assert(worker_decode_reader_result(result.payload.data(), result.payload.size(), reader, error));
    assert(reader.plain_text_truncated);
    assert(reader.plain_text.size() == MaxReaderPlainPreviewBytes - 1);
    assert(valid_utf8(reader.plain_text.c_str(), reader.plain_text.size()));
    assert(!memcmp(reader.plain_text.c_str(), raw.data() + header_size,
        reader.plain_text.size()));

    uint64_t offset = 0;
    uint64_t request_id = 1982;
    const uint32_t chunk_size = 256u * 1024u;
    while (offset < raw.size()) {
        WorkerRequest read = test_request(request_id++, WorkerTaskKind::ReadMessageChunk);
        read.account_id = account.id;
        read.account_generation = account.generation;
        read.folder_server_name = folder.key.server_name;
        read.folder_generation = folder.generation;
        read.folder_uidvalidity = folder.uidvalidity;
        read.message_uid = summary.key.uid;
        WorkerTaskInput read_input;
        read_input.message_key = summary.key;
        read_input.chunk_offset = offset;
        read_input.chunk_size = chunk_size;
        result = execute_direct_task(profile.paths, read, read_input);
        assert(result.status == WorkerStatus::Ok);
        WorkerChunkResult chunk;
        assert(worker_decode_chunk_result(result.payload.data(), result.payload.size(), chunk, error));
        assert(chunk.key == summary.key && chunk.offset == offset && chunk.total_size == raw.size());
        assert(chunk.bytes.size() &&
            !memcmp(chunk.bytes.data(), raw.data() + offset, chunk.bytes.size()));
        offset += chunk.bytes.size();
    }
    assert(offset == raw.size());
}

static Bytes worker_fetch_fixture_message() {
    static const char message[] =
        "From: fixture@reliefmail.test\r\n"
        "To: alice@reliefmail.test\r\n"
        "Subject: full body from real fixture\r\n"
        "Date: Sun, 18 Oct 2026 14:17:44 +0000\r\n"
        "Message-ID: <fetch-worker@example.test>\r\n"
        "MIME-Version: 1.0\r\n"
        "Content-Type: multipart/mixed; boundary=\"task5-fetch-boundary\"\r\n"
        "\r\n"
        "--task5-fetch-boundary\r\n"
        "Content-Type: text/plain; charset=UTF-8\r\n"
        "Content-Transfer-Encoding: 8bit\r\n"
        "\r\n"
        "full body from real fixture\r\n"
        "--task5-fetch-boundary\r\n"
        "Content-Type: application/octet-stream; name=\"task5.bin\"\r\n"
        "Content-Disposition: attachment; filename=\"task5.bin\"\r\n"
        "Content-Transfer-Encoding: base64\r\n"
        "\r\n"
        "YXR0YWNobWVudA==\r\n"
        "--task5-fetch-boundary--\r\n";
    Bytes output;
    assert(output.set(message, sizeof(message) - 1));
    return output;
}

static void test_fetch_message_is_a_public_framed_task() {
    WorkerRequest request = test_request(701, WorkerTaskKind::FetchMessage);
    request.account_id = 71;
    request.account_generation = 9001;
    request.folder_server_name.set("INBOX");
    request.folder_generation = 3;
    request.folder_uidvalidity = 4242;
    request.message_uid = 7;
    WorkerTaskInput input;
    input.message_key.folder.account_id = request.account_id;
    input.message_key.folder.server_name.set("INBOX");
    input.message_key.uidvalidity = request.folder_uidvalidity;
    input.message_key.uid = request.message_uid;
    Bytes task_payload;
    Text error;
    assert(worker_encode_task_input(request.kind, input, task_payload, error));
    request.payload = static_cast<Bytes &&>(task_payload);
    Bytes frame;
    assert(worker_encode_request(request, frame, error));
    WorkerRequest decoded;
    assert(worker_decode_request(frame.data(), frame.size(), decoded, error));
    assert(decoded.kind == WorkerTaskKind::FetchMessage && decoded.account_generation == 9001);
    WorkerTaskInput decoded_input;
    assert(worker_decode_task_input(decoded.kind, decoded.payload.data(), decoded.payload.size(),
        decoded_input, error));
    assert(decoded_input.message_key == input.message_key);
}

static Account task_candidate_account() {
    Account account;
    account.display_name.set("Worker account");
    account.email_address.set("alice@reliefmail.test");
    account.username.set("alice@reliefmail.test");
    account.smtp_username.set("alice@reliefmail.test");
    account.incoming_host.set("localhost");
    account.outgoing_host.set("localhost");
    account.incoming_port = 993;
    account.outgoing_port = 465;
    account.incoming_protocol = IncomingProtocol::Imap;
    account.incoming_security = TransportSecurity::ImplicitTls;
    account.outgoing_security = TransportSecurity::ImplicitTls;
    account.inbox_folder.set("INBOX");
    account.sent_folder.set("Sent");
    account.drafts_folder.set("Drafts");
    account.trash_folder.set("Trash");
    account.junk_folder.set("Junk");
    account.archive_folder.set("Archive");
    account.enabled = true;
    account.is_default = true;
    return account;
}

static uint32_t fixture_endpoint_port(const char *ready_path, const char *wanted) {
    FILE *file = fopen(ready_path, "r");
    assert(file);
    char name[96];
    unsigned int port = 0;
    unsigned int found = 0;
    while (fscanf(file, "%95s %u", name, &port) == 2)
        if (!strcmp(name, wanted)) { found = port; break; }
    fclose(file);
    assert(found > 0 && found <= 65535);
    return found;
}

static Account fixture_account(uint32_t imap_port, uint32_t smtp_port, const char *ca_path) {
    Account account = task_candidate_account();
    account.incoming_port = imap_port;
    account.outgoing_port = smtp_port;
    assert(account.custom_ca_file.set(ca_path));
    return account;
}

static SecretBytes fixture_credentials(const WorkerCoordinator &coordinator,
    const Account &account, bool with_imap, bool with_smtp) {
    static const unsigned char password[] = "phase-a-fixture-password";
    CredentialInput records[2];
    size_t count = 0;
    if (with_imap) {
        records[count].configuration = account;
        records[count].purpose = CredentialPurpose::Imap;
        records[count].username = account.username;
        records[count].secret = password;
        records[count].secret_size = sizeof(password) - 1;
        ++count;
    }
    if (with_smtp) {
        records[count].configuration = account;
        records[count].purpose = CredentialPurpose::Smtp;
        records[count].username = account.smtp_username.empty() ? account.username : account.smtp_username;
        records[count].secret = password;
        records[count].secret_size = sizeof(password) - 1;
        ++count;
    }
    SecretBytes packet;
    Text error;
    assert(worker_encode_credentials(records, count, coordinator.profile_epoch(), packet, error));
    return packet;
}

static void write_fixture_attachment(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    assert(fd >= 0);
    unsigned char bytes[4096];
    memset(bytes, 'A', sizeof(bytes));
    size_t offset = 0;
    while (offset < sizeof(bytes)) {
        const ssize_t count = write(fd, bytes + offset, sizeof(bytes) - offset);
        assert(count > 0);
        offset += static_cast<size_t>(count);
    }
    assert(close(fd) == 0);
}

static void fixture_worker_roundtrip(const char *ready_path, const char *ca_path) {
    WorkerProfileFixture profile;
    Text error;
    WorkerCoordinator coordinator;
    assert(coordinator.open("/proc/self/exe", profile.paths, error));
    WorkerResult result;
    Account candidate = fixture_account(fixture_endpoint_port(ready_path, "imap-worker-mail"),
        fixture_endpoint_port(ready_path, "smtp-worker"), ca_path);

    WorkerTaskInput input;
    input.account = candidate;
    submit_child_task(coordinator, 1, WorkerTaskKind::AccountTestImap, 0, 0,
        &input, fixture_credentials(coordinator, candidate, true, false), result);
    assert(result.status == WorkerStatus::Ok);
    submit_child_task(coordinator, 2, WorkerTaskKind::AccountTestSmtp, 0, 0,
        &input, fixture_credentials(coordinator, candidate, false, true), result);
    assert(result.status == WorkerStatus::Ok);

    submit_child_task(coordinator, 3, WorkerTaskKind::Initialize, 0, 0, nullptr,
        SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    WorkerStartupSnapshot startup;
    assert(worker_decode_startup_snapshot(result.payload.data(), result.payload.size(), startup, error));
    assert(startup.accounts.empty()); // Testing unsaved candidates must never persist them.

    input = WorkerTaskInput();
    input.account = candidate;
    submit_child_task(coordinator, 4, WorkerTaskKind::AccountCreate, 0, 0, &input,
        SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    WorkerAccountResult account_result;
    assert(worker_decode_account_result(result.payload.data(), result.payload.size(), account_result, error));
    Account account = account_result.account;
    assert(account.id && account.generation);

    submit_child_task(coordinator, 5, WorkerTaskKind::FolderList, account.id, account.generation,
        nullptr, fixture_credentials(coordinator, account, true, false), result);
    assert(result.status == WorkerStatus::Ok);
    assert(worker_decode_startup_snapshot(result.payload.data(), result.payload.size(), startup, error));
    WorkerStartupSnapshot cached_account_view;
    submit_child_task(coordinator, 6, WorkerTaskKind::AccountList, account.id, account.generation,
        nullptr, SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    assert(worker_decode_startup_snapshot(result.payload.data(), result.payload.size(), cached_account_view, error));
    assert(cached_account_view.selected_account_id == account.id);
    assert(cached_account_view.folders.size() == startup.folders.size());
    WorkerVaultStatus vault_status;
    submit_child_task(coordinator, 7, WorkerTaskKind::VaultStatus, 0, 0,
        nullptr, SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    assert(worker_decode_vault_status(result.payload.data(), result.payload.size(), vault_status, error));
    assert(!vault_status.exists);
    SecretBytes vault_save;
    assert(worker_encode_secret(WorkerSecretPurpose::VaultSave, worker_test_secret("GUI fixture vault"),
        fixture_credentials(coordinator, account, true, true), vault_save, error));
    submit_child_task(coordinator, 8, WorkerTaskKind::SaveVault, 0, 0,
        nullptr, static_cast<SecretBytes &&>(vault_save), result);
    assert(result.status == WorkerStatus::Ok);
    submit_child_task(coordinator, 9, WorkerTaskKind::VaultStatus, 0, 0,
        nullptr, SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    assert(worker_decode_vault_status(result.payload.data(), result.payload.size(), vault_status, error));
    assert(vault_status.exists);
    bool saw_container = false, saw_selectable = false;
    Folder container;
    for (size_t i = 0; i < startup.folders.size(); ++i) {
        const Folder &folder = startup.folders[i];
        if (folder.key.server_name == "Projects") {
            container = folder;
            saw_container = !folder.selectable;
        }
        if (folder.key.server_name == "Projects/Inbox") saw_selectable = folder.selectable;
    }
    assert(saw_container && saw_selectable);
    assert(container.generation && !container.uidvalidity);

    Folder inbox, project_inbox;
    bool saw_inbox = false, saw_project_inbox = false;
    for (size_t i = 0; i < startup.folders.size(); ++i) {
        if (startup.folders[i].key.server_name == "INBOX") {
            inbox = startup.folders[i];
            saw_inbox = true;
        } else if (startup.folders[i].key.server_name == "Projects/Inbox") {
            project_inbox = startup.folders[i];
            saw_project_inbox = true;
        }
    }
    assert(saw_inbox && inbox.generation && inbox.selectable);
    assert(saw_project_inbox && project_inbox.generation && project_inbox.selectable);
    assert(!inbox.uidvalidity && !project_inbox.uidvalidity);

    WorkerRequest hierarchy_operation = test_request(60, WorkerTaskKind::FolderOperation);
    hierarchy_operation.account_id = account.id;
    hierarchy_operation.account_generation = account.generation;
    hierarchy_operation.folder_server_name = container.key.server_name;
    hierarchy_operation.folder_generation = container.generation;
    hierarchy_operation.folder_uidvalidity = container.uidvalidity;
    WorkerTaskInput hierarchy_input;
    hierarchy_input.folder_action = WorkerFolderAction::Unsubscribe;
    assert(hierarchy_input.query.set("Projects"));
    assert(worker_encode_task_input(hierarchy_operation.kind, hierarchy_input,
        hierarchy_operation.payload, error));
    SecretBytes hierarchy_secret = fixture_credentials(coordinator, account, true, false);
    assert(submit_current_epoch(coordinator, hierarchy_operation,
        static_cast<SecretBytes &&>(hierarchy_secret), error));
    assert(wait_for_result(coordinator, result, error, 10000));
    if (result.status != WorkerStatus::InvalidRequest)
        fprintf(stderr, "nonselectable hierarchy operation returned status %u: %s\n",
            static_cast<unsigned>(result.status), result.diagnostic.c_str());
    assert(result.request_id == hierarchy_operation.request_id &&
        result.status == WorkerStatus::InvalidRequest);
    MessageKey folder_identity;
    folder_identity.folder = inbox.key;
    WorkerTaskInput sync_input;
    submit_folder_task(coordinator, 61, WorkerTaskKind::HeaderSync, account, inbox,
        folder_identity, sync_input, fixture_credentials(coordinator, account, true, false), result);
    assert(result.status == WorkerStatus::Ok);
    WorkerMessagePage page;
    assert(worker_decode_message_page(result.payload.data(), result.payload.size(), page, error));
    assert(page.rows.size() == 1 && page.rows[0].message.key.uid == 7);
    assert(page.folder_generation == inbox.generation && page.folder_uidvalidity == 4242);
    assert(page.rows[0].message.headers_only);
    const MessageKey fetched_key = page.rows[0].message.key;
    assert(fetched_key.uidvalidity == 4242);

    WorkerTaskInput fetch_input;
    submit_folder_task(coordinator, 62, WorkerTaskKind::FetchMessage, account, inbox,
        fetched_key, fetch_input, fixture_credentials(coordinator, account, true, false), result);
    assert(result.status == WorkerStatus::Ok);
    WorkerReaderResult fetched_reader;
    assert(worker_decode_reader_result(result.payload.data(), result.payload.size(), fetched_reader, error));
    assert(fetched_reader.key == fetched_key && !fetched_reader.headers_only);
    assert(fetched_reader.subject == "full body from real fixture");
    assert(fetched_reader.plain_text == "full body from real fixture");
    assert(fetched_reader.attachments.size() == 1);
    assert(fetched_reader.attachments[0].filename == "task5.bin");
    assert(fetched_reader.attachments[0].size == 10);

    WorkerTaskInput attachment_filter_query;
    attachment_filter_query.page_query.scope = MessageQueryScope::Folder;
    attachment_filter_query.page_query.account_id = account.id;
    attachment_filter_query.page_query.folder = inbox.key;
    attachment_filter_query.page_query.limit = 20;
    submit_folder_task(coordinator, 63, WorkerTaskKind::MessagePage, account, inbox,
        fetched_key, attachment_filter_query, SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    assert(worker_decode_message_page(result.payload.data(), result.payload.size(), page, error));
    assert(page.rows.size() == 1 && page.rows[0].message.key == fetched_key);
    assert(page.rows[0].attachment_presence_known && page.rows[0].has_attachments);

    WorkerTaskInput attachment_read;
    attachment_read.message_key = fetched_key;
    attachment_read.attachment_index = 0;
    attachment_read.chunk_size = 64;
    submit_folder_task(coordinator, 64, WorkerTaskKind::ReadMessageChunk, account, inbox,
        fetched_key, attachment_read, SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    WorkerChunkResult attachment_chunk;
    assert(worker_decode_chunk_result(result.payload.data(), result.payload.size(), attachment_chunk, error));
    static const char expected_attachment[] = "attachment";
    assert(attachment_chunk.bytes.size() == sizeof(expected_attachment) - 1);
    assert(!memcmp(attachment_chunk.bytes.data(), expected_attachment, sizeof(expected_attachment) - 1));

    folder_identity.uidvalidity = fetched_key.uidvalidity;
    WorkerTaskInput refresh_input;
    submit_folder_task(coordinator, 65, WorkerTaskKind::HeaderSync, account, inbox,
        folder_identity, refresh_input, fixture_credentials(coordinator, account, true, false), result);
    assert(result.status == WorkerStatus::Ok);
    assert(worker_decode_message_page(result.payload.data(), result.payload.size(), page, error));
    assert(page.rows.size() == 1 && !page.rows[0].message.headers_only);
    assert(page.rows[0].message.key == fetched_key);

    WorkerTaskInput original_read;
    original_read.message_key = fetched_key;
    original_read.chunk_size = 64u * 1024u;
    submit_folder_task(coordinator, 66, WorkerTaskKind::ReadMessageChunk, account, inbox,
        fetched_key, original_read, SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    WorkerChunkResult original_chunk;
    assert(worker_decode_chunk_result(result.payload.data(), result.payload.size(), original_chunk, error));
    Bytes expected_original = worker_fetch_fixture_message();
    assert(!original_chunk.headers_only && original_chunk.total_size == expected_original.size());
    assert(original_chunk.bytes.size() == expected_original.size());
    assert(!memcmp(original_chunk.bytes.data(), expected_original.data(), expected_original.size()));

    WorkerTaskInput refreshed_attachment_read;
    refreshed_attachment_read.message_key = fetched_key;
    refreshed_attachment_read.attachment_index = 0;
    refreshed_attachment_read.chunk_size = 64;
    submit_folder_task(coordinator, 67, WorkerTaskKind::ReadMessageChunk, account, inbox,
        fetched_key, refreshed_attachment_read, SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    assert(worker_decode_chunk_result(result.payload.data(), result.payload.size(), attachment_chunk, error));
    assert(attachment_chunk.bytes.size() == sizeof(expected_attachment) - 1);
    assert(!memcmp(attachment_chunk.bytes.data(), expected_attachment, sizeof(expected_attachment) - 1));

    MessageKey stale_key = fetched_key;
    ++stale_key.uidvalidity;
    WorkerTaskInput stale_input;
    submit_folder_task(coordinator, 68, WorkerTaskKind::FetchMessage, account, inbox,
        stale_key, stale_input, fixture_credentials(coordinator, account, true, false), result);
    assert(result.status == WorkerStatus::InvalidRequest);
    WorkerTaskInput after_stale_read;
    after_stale_read.message_key = fetched_key;
    after_stale_read.chunk_size = 64u * 1024u;
    submit_folder_task(coordinator, 69, WorkerTaskKind::ReadMessageChunk, account, inbox,
        fetched_key, after_stale_read, SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    assert(worker_decode_chunk_result(result.payload.data(), result.payload.size(), original_chunk, error));
    assert(original_chunk.bytes.size() == expected_original.size());
    assert(!memcmp(original_chunk.bytes.data(), expected_original.data(), expected_original.size()));

    MessageKey project_folder_identity;
    project_folder_identity.folder = project_inbox.key;
    WorkerTaskInput project_sync;
    submit_folder_task(coordinator, 70, WorkerTaskKind::HeaderSync, account, project_inbox,
        project_folder_identity, project_sync, fixture_credentials(coordinator, account, true, false), result);
    assert(result.status == WorkerStatus::Ok);
    assert(worker_decode_message_page(result.payload.data(), result.payload.size(), page, error));
    assert(page.rows.size() == 1 && page.rows[0].message.headers_only);
    const MessageKey old_project_key = page.rows[0].message.key;
    assert(old_project_key.uidvalidity == 4242);
    WorkerTaskInput project_fetch;
    submit_folder_task(coordinator, 71, WorkerTaskKind::FetchMessage, account, project_inbox,
        old_project_key, project_fetch, fixture_credentials(coordinator, account, true, false), result);
    assert(result.status == WorkerStatus::InvalidRequest);
    WorkerTaskInput invalidated_read;
    invalidated_read.message_key = old_project_key;
    invalidated_read.chunk_size = 64u * 1024u;
    submit_folder_task(coordinator, 72, WorkerTaskKind::ReadMessageChunk, account, project_inbox,
        old_project_key, invalidated_read, SecretBytes(), result);
    assert(result.status == WorkerStatus::InvalidRequest);

    char missing_path[PATH_MAX], attachment_path[PATH_MAX];
    assert(snprintf(missing_path, sizeof(missing_path), "%s/task5-missing.bin",
        profile.paths.data_dir.c_str()) > 0);
    assert(snprintf(attachment_path, sizeof(attachment_path), "%s/task5-attachment.bin",
        profile.paths.data_dir.c_str()) > 0);
    write_fixture_attachment(attachment_path);
    Draft draft;
    draft.account_id = account.id;
    draft.raw_recipient_input = true;
    assert(draft.to_input.set("bob@reliefmail.test"));
    assert(draft.subject.set("Task5 fixture send"));
    assert(draft.body.set("worker materialized the selected file"));
    assert(draft.message_id.set("<task5-send@example.test>"));
    input = WorkerTaskInput();
    input.has_draft = true;
    input.draft = static_cast<Draft &&>(draft);
    WorkerDraftAttachmentRef attachment;
    attachment.kind = WorkerAttachmentRefKind::SelectedFile;
    assert(attachment.path.set(missing_path));
    assert(attachment.filename.set("task5.bin"));
    assert(attachment.content_type.set("application/octet-stream"));
    assert(input.attachment_refs.push(attachment));
    submit_child_task(coordinator, 73, WorkerTaskKind::SaveDraft, account.id, account.generation,
        &input, SecretBytes(), result);
    assert(result.status == WorkerStatus::Failed); // Invalid attachment is rejected before draft commit.

    input.attachment_refs.clear();
    assert(attachment.path.set(attachment_path));
    assert(input.attachment_refs.push(attachment));
    submit_child_task(coordinator, 74, WorkerTaskKind::SaveDraft, account.id, account.generation,
        &input, SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    WorkerDraftSnapshot snapshot;
    assert(worker_decode_draft_snapshot(result.payload.data(), result.payload.size(), snapshot, error));
    assert(snapshot.draft.id && snapshot.draft.revision == 1 && snapshot.attachments.size() == 1);
    assert(snapshot.attachments[0].size == 4096);

    input = WorkerTaskInput();
    input.has_draft = true;
    const uint64_t draft_id = snapshot.draft.id;
    const uint64_t draft_revision = snapshot.draft.revision;
    input.item_id = draft_id;
    input.item_revision = draft_revision;
    input.draft = static_cast<Draft &&>(snapshot.draft);
    input.draft.body.set("revision two retains the worker-owned part");
    WorkerDraftAttachmentRef existing;
    existing.kind = WorkerAttachmentRefKind::ExistingDraftPart;
    existing.source_draft_id = draft_id;
    existing.source_revision = draft_revision;
    existing.source_position = 0;
    assert(input.attachment_refs.push(existing));
    submit_child_task(coordinator, 75, WorkerTaskKind::SaveDraftRevision, account.id, account.generation,
        &input, SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    assert(worker_decode_draft_snapshot(result.payload.data(), result.payload.size(), snapshot, error));
    assert(snapshot.draft.revision == 2 && snapshot.attachments.size() == 1 && snapshot.attachments[0].size == 4096);

    input = WorkerTaskInput();
    input.item_id = draft_id;
    input.item_revision = 2;
    submit_child_task(coordinator, 76, WorkerTaskKind::QueueSend, account.id, account.generation,
        &input, SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    WorkerOperationResult operation;
    assert(worker_decode_operation_result(result.payload.data(), result.payload.size(), operation, error));
    assert(operation.stage == SendStage::Ready && operation.delivery_count == 0);
    const uint64_t operation_id = operation.operation_id;

    input = WorkerTaskInput();
    input.item_id = operation_id;
    submit_child_task(coordinator, 77, WorkerTaskKind::SendQueued, account.id, account.generation,
        &input, fixture_credentials(coordinator, account, true, true), result);
    assert(result.status == WorkerStatus::CommittedWarning);
    assert(worker_decode_operation_result(result.payload.data(), result.payload.size(), operation, error));
    assert(operation.stage == SendStage::ArchiveFailed && operation.delivery_count == 1);
    coordinator.close();

    assert(coordinator.open("/proc/self/exe", profile.paths, error));
    submit_child_task(coordinator, 1, WorkerTaskKind::Initialize, 0, 0, nullptr,
        SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    input = WorkerTaskInput();
    input.item_id = operation_id;
    submit_child_task(coordinator, 2, WorkerTaskKind::RetryArchive, account.id, account.generation,
        &input, fixture_credentials(coordinator, account, true, false), result);
    if (result.status != WorkerStatus::Ok)
        fprintf(stderr, "RetryArchive returned status %u: %s\n",
            static_cast<unsigned>(result.status), result.diagnostic.c_str());
    assert(result.status == WorkerStatus::Ok);
    assert(worker_decode_operation_result(result.payload.data(), result.payload.size(), operation, error));
    assert(operation.stage == SendStage::Archived && operation.delivery_count == 1);
    coordinator.close();

    account = fixture_account(fixture_endpoint_port(ready_path, "imap-worker-mail"),
        fixture_endpoint_port(ready_path, "smtp-worker-lost"), ca_path);
    account.is_default = false;
    assert(coordinator.open("/proc/self/exe", profile.paths, error));
    input = WorkerTaskInput();
    input.account = account;
    submit_child_task(coordinator, 1, WorkerTaskKind::AccountCreate, 0, 0, &input,
        SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    assert(worker_decode_account_result(result.payload.data(), result.payload.size(), account_result, error));
    account = account_result.account;
    Draft uncertain_draft;
    uncertain_draft.account_id = account.id;
    uncertain_draft.raw_recipient_input = true;
    uncertain_draft.to_input.set("bob@reliefmail.test");
    uncertain_draft.subject.set("uncertain worker send");
    uncertain_draft.body.set("must not be automatically submitted again");
    input = WorkerTaskInput();
    input.has_draft = true;
    input.draft = static_cast<Draft &&>(uncertain_draft);
    submit_child_task(coordinator, 2, WorkerTaskKind::SaveDraft, account.id, account.generation,
        &input, SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    assert(worker_decode_draft_snapshot(result.payload.data(), result.payload.size(), snapshot, error));
    input = WorkerTaskInput();
    input.item_id = snapshot.draft.id;
    input.item_revision = snapshot.draft.revision;
    submit_child_task(coordinator, 3, WorkerTaskKind::QueueSend, account.id, account.generation,
        &input, SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    assert(worker_decode_operation_result(result.payload.data(), result.payload.size(), operation, error));
    const uint64_t uncertain_operation_id = operation.operation_id;
    input = WorkerTaskInput();
    input.item_id = uncertain_operation_id;
    submit_child_task(coordinator, 4, WorkerTaskKind::SendQueued, account.id, account.generation,
        &input, fixture_credentials(coordinator, account, false, true), result);
    assert(result.status == WorkerStatus::Uncertain);
    assert(worker_decode_operation_result(result.payload.data(), result.payload.size(), operation, error));
    assert(operation.stage == SendStage::Uncertain && operation.delivery_count == 0);
    coordinator.close();

    assert(coordinator.open("/proc/self/exe", profile.paths, error));
    submit_child_task(coordinator, 1, WorkerTaskKind::Initialize, 0, 0, nullptr,
        SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    input = WorkerTaskInput();
    input.item_id = uncertain_operation_id;
    submit_child_task(coordinator, 2, WorkerTaskKind::SendQueued, account.id, account.generation,
        &input, fixture_credentials(coordinator, account, false, true), result);
    assert(result.status == WorkerStatus::Uncertain);
    assert(worker_decode_operation_result(result.payload.data(), result.payload.size(), operation, error));
    assert(operation.stage == SendStage::Uncertain && operation.delivery_count == 0);
    coordinator.close();
}

static void test_protocol_fixture_worker(int argc, char **argv) {
    assert(argc == 4 && !strcmp(argv[1], "--fixture"));
    fixture_worker_roundtrip(argv[2], argv[3]);
    puts("ReliefMail protocol-backed worker suite passed");
}

static void test_real_sequential_account_tasks_reuse_worker_slots() {
    WorkerProfileFixture profile;
    Text error;
    WorkerCoordinator coordinator;
    assert(coordinator.open("/proc/self/exe", profile.paths, error));
    WorkerResult result;
    submit_child_task(coordinator, 1, WorkerTaskKind::Initialize, 0, 0, nullptr,
        SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    WorkerStartupSnapshot startup;
    assert(worker_decode_startup_snapshot(result.payload.data(), result.payload.size(), startup, error));
    assert(startup.accounts.empty());

    WorkerTaskInput input;
    input.account = task_candidate_account();
    submit_child_task(coordinator, 2, WorkerTaskKind::AccountCreate, 0, 0, &input,
        SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    WorkerAccountResult account_result;
    assert(worker_decode_account_result(result.payload.data(), result.payload.size(), account_result, error));
    assert(account_result.account.id && account_result.account.generation);
    const uint64_t account_id = account_result.account.id;
    const uint64_t first_generation = account_result.account.generation;

    input = WorkerTaskInput();
    input.account = account_result.account;
    input.account.display_name.set("Updated worker account");
    submit_child_task(coordinator, 3, WorkerTaskKind::AccountUpdate, account_id, first_generation,
        &input, SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    assert(worker_decode_account_result(result.payload.data(), result.payload.size(), account_result, error));
    assert(account_result.account.generation && account_result.account.generation != first_generation);
    const uint64_t second_generation = account_result.account.generation;

    input.account = account_result.account;
    submit_child_task(coordinator, 4, WorkerTaskKind::AccountUpdate, account_id, first_generation,
        &input, SecretBytes(), result);
    assert(result.status != WorkerStatus::Ok);

    submit_child_task(coordinator, 5, WorkerTaskKind::AccountList, 0, 0, nullptr,
        SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    assert(worker_decode_startup_snapshot(result.payload.data(), result.payload.size(), startup, error));
    assert(startup.accounts.size() == 1 && startup.accounts[0].generation == second_generation);
    assert(startup.accounts[0].display_name == "Updated worker account");

    submit_child_task(coordinator, 6, WorkerTaskKind::AccountDelete, account_id, second_generation,
        nullptr, SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    input = WorkerTaskInput();
    input.account = task_candidate_account();
    input.account.display_name.set("Recreated worker account");
    submit_child_task(coordinator, 7, WorkerTaskKind::AccountCreate, 0, 0, &input,
        SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    assert(worker_decode_account_result(result.payload.data(), result.payload.size(), account_result, error));
    assert(account_result.account.id == account_id);
    assert(account_result.account.generation != first_generation);
    coordinator.close();
}

static void test_queue_send_cancel_after_commit_keeps_durable_success() {
    WorkerProfileFixture profile;
    Text error;
    WorkerCoordinator coordinator;
    assert(coordinator.open("/proc/self/exe", profile.paths, error));
    WorkerResult result;
    WorkerTaskInput input;
    input.account = task_candidate_account();
    submit_child_task(coordinator, 1601, WorkerTaskKind::AccountCreate, 0, 0,
        &input, SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    WorkerAccountResult account_result;
    assert(worker_decode_account_result(result.payload.data(), result.payload.size(), account_result, error));
    const Account account = account_result.account;

    Draft draft;
    draft.account_id = account.id;
    draft.raw_recipient_input = true;
    assert(draft.to_input.set("bob@reliefmail.test"));
    assert(draft.subject.set("commit before cancel"));
    assert(draft.body.set("queued exactly once"));
    input = WorkerTaskInput();
    input.has_draft = true;
    input.draft = static_cast<Draft &&>(draft);
    submit_child_task(coordinator, 1602, WorkerTaskKind::SaveDraft, account.id, account.generation,
        &input, SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    WorkerDraftSnapshot snapshot;
    assert(worker_decode_draft_snapshot(result.payload.data(), result.payload.size(), snapshot, error));

    WorkerTaskInput queue_input;
    queue_input.item_id = snapshot.draft.id;
    queue_input.item_revision = snapshot.draft.revision;
    queue_input.test_pause_after_durable_commit = true;
    char queue_commit_marker[PATH_MAX];
    const int queue_marker_length = snprintf(queue_commit_marker, sizeof(queue_commit_marker),
        "%s/queue-send-commit.signal", profile.root);
    assert(queue_marker_length > 0 && static_cast<size_t>(queue_marker_length) < sizeof(queue_commit_marker));
    assert(queue_input.test_commit_marker_path.set(queue_commit_marker));
    WorkerRequest request = test_request(1603, WorkerTaskKind::QueueSend);
    request.account_id = account.id;
    request.account_generation = account.generation;
    assert(worker_encode_task_input(request.kind, queue_input, request.payload, error));
    SecretBytes no_secret;
    assert(submit_current_epoch(coordinator, request, static_cast<SecretBytes &&>(no_secret), error));

    bool durable_operation_visible = false;
    for (unsigned attempt = 0; attempt < 10000 && !durable_operation_visible; ++attempt) {
        assert(coordinator.pump(error));
        durable_operation_visible = access(queue_commit_marker, F_OK) == 0;
        if (!durable_operation_visible) usleep(1000);
    }
    assert(durable_operation_visible);

    assert(coordinator.cancel(request.request_id, error));
    assert(wait_for_result(coordinator, result, error, 5000));
    assert(result.request_id == request.request_id && result.status == WorkerStatus::Ok);
    WorkerOperationResult operation;
    assert(worker_decode_operation_result(result.payload.data(), result.payload.size(), operation, error));
    assert(operation.stage == SendStage::Ready && operation.delivery_count == 0);
    assert(operation.operation_id);

    Store verify;
    assert(verify.open(profile.paths, StoreOpenMode::Worker, error));
    SendOperation persisted;
    Bytes persisted_spool;
    bool found = false;
    assert(verify.find_send_operation_for_draft(account.id, snapshot.draft.id,
        snapshot.draft.revision, found, persisted, persisted_spool, error));
    assert(found && persisted.id == operation.operation_id && persisted.stage == SendStage::Ready &&
        persisted.delivery_count == 0 && !persisted_spool.empty());
    verify.close();
    coordinator.close();
}

static void test_stale_ready_queue_operation_can_only_be_locally_cancelled() {
    WorkerProfileFixture profile;
    Text error;
    WorkerCoordinator coordinator;
    assert(coordinator.open("/proc/self/exe", profile.paths, error));
    WorkerResult result;
    WorkerTaskInput input;
    input.account = task_candidate_account();
    submit_child_task(coordinator, 1610, WorkerTaskKind::AccountCreate, 0, 0,
        &input, SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    WorkerAccountResult account_result;
    assert(worker_decode_account_result(result.payload.data(), result.payload.size(), account_result, error));
    Account account = account_result.account;
    const uint64_t accepted_generation = account.generation;

    Draft draft;
    draft.account_id = account.id;
    draft.raw_recipient_input = true;
    assert(draft.to_input.set("bob@reliefmail.test"));
    assert(draft.subject.set("old-generation ready operation"));
    assert(draft.body.set("cancellation must never submit SMTP"));
    input = WorkerTaskInput();
    input.has_draft = true;
    input.draft = static_cast<Draft &&>(draft);
    submit_child_task(coordinator, 1611, WorkerTaskKind::SaveDraft, account.id, accepted_generation,
        &input, SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    WorkerDraftSnapshot saved;
    assert(worker_decode_draft_snapshot(result.payload.data(), result.payload.size(), saved, error));

    input = WorkerTaskInput();
    input.item_id = saved.draft.id;
    input.item_revision = saved.draft.revision;
    submit_child_task(coordinator, 1612, WorkerTaskKind::QueueSend, account.id, accepted_generation,
        &input, SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    WorkerOperationResult operation;
    assert(worker_decode_operation_result(result.payload.data(), result.payload.size(), operation, error));
    assert(operation.stage == SendStage::Ready && operation.delivery_count == 0);
    const uint64_t operation_id = operation.operation_id;

    input = WorkerTaskInput();
    input.account = account;
    assert(input.account.display_name.set("changed account settings"));
    submit_child_task(coordinator, 1613, WorkerTaskKind::AccountUpdate, account.id, accepted_generation,
        &input, SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    assert(worker_decode_account_result(result.payload.data(), result.payload.size(), account_result, error));
    account = account_result.account;
    assert(account.generation != accepted_generation);

    input = WorkerTaskInput();
    input.item_id = operation_id;
    submit_child_task(coordinator, 1614, WorkerTaskKind::SendQueued, account.id, account.generation,
        &input, SecretBytes(), result);
    assert(result.status == WorkerStatus::InvalidRequest);

    submit_child_task(coordinator, 1615, WorkerTaskKind::CancelQueuedSend, account.id, account.generation,
        &input, SecretBytes(), result);
    if (result.status != WorkerStatus::Ok)
        fprintf(stderr, "stale Ready CancelQueuedSend returned status %u: %s\n",
            static_cast<unsigned>(result.status), result.diagnostic.c_str());
    assert(result.status == WorkerStatus::Ok);
    assert(worker_decode_operation_result(result.payload.data(), result.payload.size(), operation, error));
    assert(operation.operation_id == operation_id);
    assert(operation.stage == SendStage::Failed && operation.delivery_count == 0);

    Store verify;
    assert(verify.open(profile.paths, StoreOpenMode::Worker, error));
    SendOperation persisted;
    Bytes spool;
    assert(verify.get_send_operation(operation_id, persisted, spool, error));
    assert(persisted.account_id == account.id && persisted.account_generation == accepted_generation);
    assert(persisted.stage == SendStage::Failed && persisted.delivery_count == 0 && !spool.empty());
    verify.close();
    coordinator.close();
}

static void test_archive_pending_old_generation_requires_confirmed_exact_target_review() {
    WorkerProfileFixture profile;
    Text error;
    WorkerCoordinator coordinator;
    assert(coordinator.open("/proc/self/exe", profile.paths, error));
    WorkerResult result;
    WorkerTaskInput input;
    input.account = task_candidate_account();
    input.account.sent_folder.clear();
    submit_child_task(coordinator, 1620, WorkerTaskKind::AccountCreate, 0, 0,
        &input, SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    WorkerAccountResult account_result;
    assert(worker_decode_account_result(result.payload.data(), result.payload.size(), account_result, error));
    Account account = account_result.account;
    const uint64_t accepted_generation = account.generation;

    Store store;
    assert(store.open(profile.paths, StoreOpenMode::Worker, error));
    Draft draft;
    draft.account_id = account.id;
    draft.raw_recipient_input = true;
    assert(draft.to_input.set("bob@reliefmail.test"));
    assert(draft.subject.set("archive pending before Sent mapping"));
    assert(draft.body.set("SMTP delivery is already durably accepted"));
    assert(store.save_draft_guarded(draft, accepted_generation, 0, error));
    Bytes spool;
    assert(spool.set("immutable accepted message bytes", 31));
    Text message_id, sender, recipients, no_archive_folder;
    assert(message_id.set("<accepted-before-sent-map@example.test>"));
    assert(sender.set("alice@reliefmail.test"));
    assert(recipients.set("bob@reliefmail.test"));
    SendOperation accepted;
    assert(store.create_send_operation(account.id, accepted_generation, draft.id, draft.revision,
        message_id, sender, recipients, no_archive_folder, spool, accepted, error));
    assert(store.advance_send_operation(accepted.id, accepted_generation, SendStage::Connecting, false, error));
    assert(store.advance_send_operation(accepted.id, accepted_generation, SendStage::Authenticating, false, error));
    assert(store.advance_send_operation(accepted.id, accepted_generation, SendStage::SendingEnvelope, false, error));
    assert(store.advance_send_operation(accepted.id, accepted_generation, SendStage::SendingData, false, error));
    assert(store.advance_send_operation(accepted.id, accepted_generation, SendStage::AwaitingFinalResponse, false, error));
    assert(store.advance_send_operation(accepted.id, accepted_generation, SendStage::Accepted, true, error));
    assert(store.advance_send_operation(accepted.id, accepted_generation, SendStage::ArchivePending, false, error));
    store.close();

    input = WorkerTaskInput();
    input.account = account;
    assert(input.account.sent_folder.set("Sent"));
    submit_child_task(coordinator, 1621, WorkerTaskKind::AccountUpdate, account.id, accepted_generation,
        &input, SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    assert(worker_decode_account_result(result.payload.data(), result.payload.size(), account_result, error));
    account = account_result.account;
    assert(account.generation != accepted_generation && account.sent_folder == "Sent");

    Folder sent;
    sent.key.account_id = account.id;
    assert(sent.key.server_name.set("Sent"));
    assert(sent.display_name.set("Sent"));
    sent.role = FolderRole::Sent;
    sent.uidvalidity = 814;
    sent.selectable = true;
    assert(store.open(profile.paths, StoreOpenMode::Worker, error));
    assert(store.save_folder_metadata_guarded(sent, account.generation, 0, error));
    assert(store.get_folder(sent.key, sent, error));
    assert(sent.generation && sent.uidvalidity == 814 && sent.selectable);
    store.close();

    WorkerTaskInput review;
    review.item_id = accepted.id;
    review.item_revision = accepted_generation;
    review.send_review_action = WorkerSendReviewAction::SetArchiveTarget;
    review.destination_folder = sent.key;
    review.destination_folder_generation = sent.generation;
    review.destination_folder_uidvalidity = sent.uidvalidity;
    Bytes rejected_payload;
    assert(!worker_encode_task_input(WorkerTaskKind::ReviewArchiveTarget, review, rejected_payload, error));
    review.review_confirmed = true;

    WorkerTaskInput wrong_source;
    wrong_source.item_id = accepted.id;
    wrong_source.item_revision = accepted_generation + 1;
    wrong_source.send_review_action = WorkerSendReviewAction::SetArchiveTarget;
    wrong_source.review_confirmed = true;
    wrong_source.destination_folder = sent.key;
    wrong_source.destination_folder_generation = sent.generation;
    wrong_source.destination_folder_uidvalidity = sent.uidvalidity;
    submit_child_task(coordinator, 1622, WorkerTaskKind::ReviewArchiveTarget, account.id, account.generation,
        &wrong_source, SecretBytes(), result);
    assert(result.status == WorkerStatus::InvalidRequest);

    WorkerTaskInput stale_target;
    stale_target.item_id = accepted.id;
    stale_target.item_revision = accepted_generation;
    stale_target.send_review_action = WorkerSendReviewAction::SetArchiveTarget;
    stale_target.review_confirmed = true;
    stale_target.destination_folder = sent.key;
    stale_target.destination_folder_generation = sent.generation + 1;
    stale_target.destination_folder_uidvalidity = sent.uidvalidity;
    submit_child_task(coordinator, 1623, WorkerTaskKind::ReviewArchiveTarget, account.id, account.generation,
        &stale_target, SecretBytes(), result);
    assert(result.status == WorkerStatus::InvalidRequest);

    submit_child_task(coordinator, 1624, WorkerTaskKind::ReviewArchiveTarget, account.id, account.generation,
        &review, SecretBytes(), result);
    if (result.status != WorkerStatus::Ok)
        fprintf(stderr, "stage-11 old-generation archive review returned status %u: %s\n",
            static_cast<unsigned>(result.status), result.diagnostic.c_str());
    assert(result.status == WorkerStatus::Ok);
    WorkerOperationResult reviewed;
    assert(worker_decode_operation_result(result.payload.data(), result.payload.size(), reviewed, error));
    assert(reviewed.operation_id == accepted.id && reviewed.stage == SendStage::ArchivePending &&
        reviewed.delivery_count == 1);

    assert(store.open(profile.paths, StoreOpenMode::Worker, error));
    SendOperation persisted;
    Bytes persisted_spool;
    assert(store.get_send_operation(accepted.id, persisted, persisted_spool, error));
    assert(persisted.account_id == account.id && persisted.account_generation == accepted_generation);
    assert(persisted.stage == SendStage::ArchivePending && persisted.delivery_count == 1);
    assert(persisted.archive_review_generation == account.generation && persisted.archive_folder == "Sent");
    assert(persisted.archive_folder_generation == sent.generation &&
        persisted.archive_folder_uidvalidity == sent.uidvalidity && persisted_spool == spool);
    store.close();
    coordinator.close();
}

static void prepare_sender_identity_draft(Draft &draft, const Account &account,
    const char *message_id, const char *subject) {
    draft.account_id = account.id;
    draft.raw_recipient_input = true;
    assert(draft.to_input.set("bob@reliefmail.test"));
    assert(draft.subject.set(subject));
    assert(draft.body.set("local queue validation fixture"));
    assert(draft.message_id.set(message_id));
}

static bool from_header_has_fold(const Bytes &message) {
    size_t at = 0;
    bool in_from = false;
    while (at + 1 < message.size()) {
        size_t end = at;
        while (end + 1 < message.size() &&
            !(message.data()[end] == '\r' && message.data()[end + 1] == '\n')) ++end;
        if (end + 1 >= message.size() || end == at) break;
        if (message.data()[at] == ' ' || message.data()[at] == '\t') {
            if (in_from) return true;
        } else {
            in_from = false;
            size_t colon = at;
            while (colon < end && message.data()[colon] != ':') ++colon;
            if (colon - at == 4) {
                static const char from_name[] = "from";
                in_from = true;
                for (size_t i = 0; i < 4; ++i) {
                    unsigned char actual = message.data()[at + i];
                    if (actual >= 'A' && actual <= 'Z') actual += 'a' - 'A';
                    if (actual != static_cast<unsigned char>(from_name[i])) in_from = false;
                }
                in_from = in_from && colon < end;
            }
        }
        at = end + 2;
    }
    return false;
}

static bool byte_sequence_contains(const Bytes &bytes, const char *needle) {
    const size_t needle_size = strlen(needle);
    if (!needle_size || needle_size > bytes.size()) return false;
    for (size_t at = 0; at <= bytes.size() - needle_size; ++at)
        if (!memcmp(bytes.data() + at, needle, needle_size)) return true;
    return false;
}

static bool queue_saved_sender_draft(const ProfilePaths &paths, const Account &account,
    const Draft &draft, uint64_t request_id, WorkerOperationResult &operation) {
    WorkerTaskInput input;
    input.item_id = draft.id;
    input.item_revision = draft.revision;
    WorkerRequest request = test_request(request_id, WorkerTaskKind::QueueSend);
    request.account_id = account.id;
    request.account_generation = account.generation;
    WorkerResult result = execute_direct_task(paths, request, input);
    Text error;
    if (result.status != WorkerStatus::Ok ||
        !worker_decode_operation_result(result.payload.data(), result.payload.size(), operation, error)) {
        fprintf(stderr, "QueueSend sender identity request %llu returned status %u: %s\n",
            static_cast<unsigned long long>(request_id), static_cast<unsigned>(result.status),
            result.diagnostic.empty() ? error.c_str() : result.diagnostic.c_str());
        return false;
    }
    return operation.stage == SendStage::Ready && operation.delivery_count == 0;
}

static void assert_queued_sender_spool(const ProfilePaths &paths, const Account &account,
    const Draft &draft, const WorkerOperationResult &operation, bool expect_fold) {
    Text error;
    Store verify;
    assert(verify.open(paths, StoreOpenMode::Worker, error));
    SendOperation persisted;
    Bytes spool;
    bool found = false;
    assert(verify.find_send_operation_for_draft(account.id, draft.id, draft.revision,
        found, persisted, spool, error));
    assert(found && persisted.id == operation.operation_id &&
        persisted.stage == SendStage::Ready && persisted.delivery_count == 0);
    assert(spool.size() == persisted.spool_size && spool.size() > 0);
    assert(from_header_has_fold(spool) == expect_fold);
    assert(byte_sequence_contains(spool, "=?UTF-8?B?"));
    verify.close();
}

static SendOperation create_tampered_sender_operation(Store &store, const Account &account,
    const Draft &draft, bool change_mailbox, Text &error) {
    Bytes expected_spool;
    Envelope expected_envelope;
    assert(encode_message(draft, account, expected_spool, expected_envelope, error));
    Text sender, recipients;
    assert(serialize_address(expected_envelope.sender, sender, error));
    assert(expected_envelope.recipients.size() == 1 &&
        serialize_address(expected_envelope.recipients[0], recipients, error));

    Account altered = account;
    if (change_mailbox) assert(altered.email_address.set("attacker@reliefmail.test"));
    else assert(altered.display_name.set("冒充发件人"));
    Bytes tampered_spool;
    Envelope tampered_envelope;
    assert(encode_message(draft, altered, tampered_spool, tampered_envelope, error));
    SendOperation operation;
    assert(store.create_send_operation(account.id, account.generation, draft.id, draft.revision,
        draft.message_id, sender, recipients, account.sent_folder, tampered_spool,
        operation, error));
    return operation;
}

static void assert_tampered_sender_is_rejected(const ProfilePaths &paths,
    const Account &account, const SendOperation &operation, uint64_t request_id) {
    WorkerTaskInput input;
    input.item_id = operation.id;
    WorkerRequest request = test_request(request_id, WorkerTaskKind::SendQueued);
    request.account_id = account.id;
    request.account_generation = account.generation;
    WorkerResult result = execute_direct_task(paths, request, input);
    assert(result.status == WorkerStatus::InvalidRequest);

    Text error;
    Store verify;
    assert(verify.open(paths, StoreOpenMode::Worker, error));
    SendOperation persisted;
    Bytes spool;
    assert(verify.get_send_operation(operation.id, persisted, spool, error));
    assert(persisted.stage == SendStage::Ready && persisted.delivery_count == 0);
    verify.close();
}

static bool replace_spool_header_value(const Bytes &source, const char *original,
    const char *replacement, Bytes &output) {
    const size_t original_size = strlen(original);
    const size_t replacement_size = strlen(replacement);
    size_t offset = SIZE_MAX;
    for (size_t i = 0; i + original_size <= source.size(); ++i) {
        if (!memcmp(source.data() + i, original, original_size)) {
            if (offset != SIZE_MAX) return false;
            offset = i;
        }
    }
    if (offset == SIZE_MAX || source.size() - original_size > SIZE_MAX - replacement_size)
        return false;
    const size_t output_size = source.size() - original_size + replacement_size;
    unsigned char *buffer = static_cast<unsigned char *>(malloc(output_size));
    if (!buffer) return false;
    memcpy(buffer, source.data(), offset);
    memcpy(buffer + offset, replacement, replacement_size);
    const size_t suffix_at = offset + original_size;
    memcpy(buffer + offset + replacement_size, source.data() + suffix_at,
        source.size() - suffix_at);
    const bool copied = output.set(buffer, output_size);
    free(buffer);
    return copied;
}

static void test_send_spool_rejects_encoded_word_inside_from_addr_spec() {
    WorkerProfileFixture profile;
    Text error;
    Store store;
    assert(store.open(profile.paths, StoreOpenMode::Worker, error));
    Account account = task_candidate_account();
    assert(store.save_account(account, error));

    Draft draft;
    prepare_sender_identity_draft(draft, account,
        "<task7-encoded-mailbox@example.test>", "Encoded mailbox attack");
    assert(store.save_draft_guarded(draft, account.generation, 0, error));
    Bytes valid_spool;
    Envelope envelope;
    assert(encode_message(draft, account, valid_spool, envelope, error));
    Text sender, recipients;
    assert(sender.set("alice@reliefmail.test"));
    assert(envelope.recipients.size() == 1 &&
        serialize_address(envelope.recipients[0], recipients, error));

    Bytes forged_spool;
    assert(replace_spool_header_value(valid_spool,
        "From: Worker account <alice@reliefmail.test>\r\n",
        "From: =?UTF-8?B?YWxpY2U=?=@reliefmail.test\r\n", forged_spool));
    SendOperation forged;
    assert(store.create_send_operation(account.id, account.generation, draft.id, draft.revision,
        draft.message_id, sender, recipients, account.sent_folder, forged_spool, forged, error));
    store.close();

    WorkerTaskInput input;
    input.item_id = forged.id;
    WorkerRequest request = test_request(1608, WorkerTaskKind::SendQueued);
    request.account_id = account.id;
    request.account_generation = account.generation;
    WorkerResult result = execute_direct_task(profile.paths, request, input);
    assert(result.status == WorkerStatus::InvalidRequest);

    Store verify;
    assert(verify.open(profile.paths, StoreOpenMode::Worker, error));
    SendOperation persisted;
    Bytes persisted_spool;
    assert(verify.get_send_operation(forged.id, persisted, persisted_spool, error));
    assert(persisted.stage == SendStage::Ready && persisted.delivery_count == 0);
    verify.close();
}

static void test_queue_send_validates_unicode_folded_sender_and_rejects_tampering() {
    WorkerProfileFixture profile;
    Text error;
    Store store;
    assert(store.open(profile.paths, StoreOpenMode::Worker, error));
    Account account = task_candidate_account();
    assert(account.display_name.set("王晓"));
    assert(store.save_account(account, error));

    Draft chinese_draft;
    prepare_sender_identity_draft(chinese_draft, account,
        "<task7-sender-zh@example.test>", "Chinese sender");
    assert(store.save_draft_guarded(chinese_draft, account.generation, 0, error));
    store.close();

    WorkerOperationResult chinese_operation;
    assert(queue_saved_sender_draft(profile.paths, account, chinese_draft, 1604,
        chinese_operation));
    assert_queued_sender_spool(profile.paths, account, chinese_draft,
        chinese_operation, false);
    assert(chinese_operation.message_id == "<task7-sender-zh@example.test>");

    assert(store.open(profile.paths, StoreOpenMode::Worker, error));
    static const char long_chinese_name[] =
        "王晓救援协调员王晓救援协调员王晓救援协调员王晓救援协调员"
        "王晓救援协调员王晓救援协调员王晓救援协调员王晓救援协调员"
        "王晓救援协调员王晓救援协调员王晓救援协调员王晓救援协调员";
    assert(account.display_name.set(long_chinese_name));
    assert(store.save_account_guarded(account, account.generation, error));
    Draft folded_draft;
    prepare_sender_identity_draft(folded_draft, account,
        "<task7-sender-folded@example.test>", "Folded sender");
    assert(store.save_draft_guarded(folded_draft, account.generation, 0, error));
    store.close();

    WorkerOperationResult folded_operation;
    assert(queue_saved_sender_draft(profile.paths, account, folded_draft, 1605,
        folded_operation));
    assert_queued_sender_spool(profile.paths, account, folded_draft,
        folded_operation, true);

    assert(store.open(profile.paths, StoreOpenMode::Worker, error));
    Draft altered_display_draft;
    prepare_sender_identity_draft(altered_display_draft, account,
        "<task7-sender-tampered-display@example.test>", "Altered display");
    assert(store.save_draft_guarded(altered_display_draft, account.generation, 0, error));
    SendOperation altered_display_operation = create_tampered_sender_operation(store,
        account, altered_display_draft, false, error);
    store.close();
    assert_tampered_sender_is_rejected(profile.paths, account, altered_display_operation, 1606);

    assert(store.open(profile.paths, StoreOpenMode::Worker, error));
    Draft altered_mailbox_draft;
    prepare_sender_identity_draft(altered_mailbox_draft, account,
        "<task7-sender-tampered-mailbox@example.test>", "Altered mailbox");
    assert(store.save_draft_guarded(altered_mailbox_draft, account.generation, 0, error));
    SendOperation altered_mailbox_operation = create_tampered_sender_operation(store,
        account, altered_mailbox_draft, true, error);
    store.close();
    assert_tampered_sender_is_rejected(profile.paths, account, altered_mailbox_operation, 1607);
}

static void test_restore_invalidates_unconsumed_unlock_and_preserves_commit_on_cancel() {
    WorkerProfileFixture profile;
    Text error;
    WorkerCoordinator coordinator;
    assert(coordinator.open("/proc/self/exe", profile.paths, error));
    WorkerResult result;
    submit_child_task(coordinator, 1701, WorkerTaskKind::Initialize, 0, 0, nullptr,
        SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);

    WorkerTaskInput input;
    input.account = task_candidate_account();
    submit_child_task(coordinator, 1702, WorkerTaskKind::AccountCreate, 0, 0, &input,
        SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    WorkerAccountResult account_result;
    assert(worker_decode_account_result(result.payload.data(), result.payload.size(), account_result, error));
    const Account account = account_result.account;
    const uint64_t old_epoch = coordinator.profile_epoch();

    static const char mailbox_password[] = "remembered account password";
    SecretBytes live_packet;
    SecretBytes mailbox_secret;
    assert(mailbox_secret.set(mailbox_password, sizeof(mailbox_password) - 1));
    assert(worker_encode_account_credential(account, CredentialPurpose::Imap, old_epoch,
        static_cast<SecretBytes &&>(mailbox_secret), live_packet, error));
    SecretBytes stale_live_packet;
    SecretBytes stale_password;
    assert(stale_password.set(mailbox_password, sizeof(mailbox_password) - 1));
    assert(worker_encode_account_credential(account, CredentialPurpose::Imap, old_epoch,
        static_cast<SecretBytes &&>(stale_password), stale_live_packet, error));
    SecretBytes save_vault_packet;
    assert(worker_encode_secret(WorkerSecretPurpose::VaultSave, worker_test_secret("vault master"),
        static_cast<SecretBytes &&>(live_packet), save_vault_packet, error));
    submit_child_task(coordinator, 1703, WorkerTaskKind::SaveVault, 0, 0, nullptr,
        static_cast<SecretBytes &&>(save_vault_packet), result);
    assert(result.status == WorkerStatus::Ok);

    char backup[PATH_MAX];
    const int backup_length = snprintf(backup, sizeof(backup), "%s/task5-before-restore.rmb", profile.root);
    assert(backup_length > 0 && static_cast<size_t>(backup_length) < sizeof(backup));
    input = WorkerTaskInput();
    assert(input.path.set(backup));
    submit_child_task(coordinator, 1704, WorkerTaskKind::BackupProfile, 0, 0,
        &input, SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);

    input = WorkerTaskInput();
    input.account = task_candidate_account();
    input.account.display_name.set("created after backup");
    input.account.email_address.set("after-backup@reliefmail.test");
    input.account.username.set("after-backup@reliefmail.test");
    input.account.smtp_username.set("after-backup@reliefmail.test");
    submit_child_task(coordinator, 1705, WorkerTaskKind::AccountCreate, 0, 0, &input,
        SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    WorkerAccountResult after_backup_result;
    assert(worker_decode_account_result(result.payload.data(), result.payload.size(), after_backup_result, error));

    SecretBytes unlock_packet;
    assert(worker_encode_secret(WorkerSecretPurpose::VaultMaster, worker_test_secret("vault master"),
        SecretBytes(), unlock_packet, error));
    WorkerRequest unlock = test_request(1706, WorkerTaskKind::UnlockVault);
    assert(submit_current_epoch(coordinator, unlock, static_cast<SecretBytes &&>(unlock_packet), error));
    assert(wait_until_worker_idle_without_taking_results(coordinator, error, 10000));

    WorkerTaskInput restore_input;
    assert(restore_input.path.set(backup));
    restore_input.test_pause_after_durable_commit = true;
    char restore_commit_marker[PATH_MAX];
    const int restore_marker_length = snprintf(restore_commit_marker, sizeof(restore_commit_marker),
        "%s/restore-commit.signal", profile.root);
    assert(restore_marker_length > 0 && static_cast<size_t>(restore_marker_length) < sizeof(restore_commit_marker));
    assert(restore_input.test_commit_marker_path.set(restore_commit_marker));
    WorkerRequest restore = test_request(1707, WorkerTaskKind::RestoreProfile);
    assert(worker_encode_task_input(restore.kind, restore_input, restore.payload, error));
    SecretBytes restore_secret;
    assert(worker_encode_secret(WorkerSecretPurpose::BackupMaster,
        worker_test_secret("vault master"), SecretBytes(), restore_secret, error));
    assert(submit_current_epoch(coordinator, restore,
        static_cast<SecretBytes &&>(restore_secret), error));

    bool restore_commit_visible = false;
    for (unsigned attempt = 0; attempt < 10000 && !restore_commit_visible; ++attempt) {
        assert(coordinator.pump(error));
        restore_commit_visible = access(restore_commit_marker, F_OK) == 0;
        if (!restore_commit_visible) usleep(1000);
    }
    if (!restore_commit_visible) {
        WorkerCoordinatorStatus failed_state;
        (void)coordinator.status(failed_state);
        if (failed_state.active) {
            Text cancel_error;
            (void)coordinator.cancel(restore.request_id, cancel_error);
            for (unsigned attempt = 0; attempt < 5000; ++attempt) {
                (void)coordinator.pump(error);
                (void)coordinator.status(failed_state);
                if (!failed_state.active) break;
                usleep(1000);
            }
        }
        WorkerResult diagnostic;
        while (coordinator.take_result(diagnostic))
            fprintf(stderr, "restore debug result id=%llu kind=%u status=%u diagnostic=%s secret=%zu\n",
                static_cast<unsigned long long>(diagnostic.request_id), static_cast<unsigned>(diagnostic.kind),
                static_cast<unsigned>(diagnostic.status), diagnostic.diagnostic.c_str(), diagnostic.secret_payload.size());
        fprintf(stderr, "restore commit marker was not observed: %s\n", restore_commit_marker);
    }
    assert(restore_commit_visible);
    assert(coordinator.cancel(restore.request_id, error));

    WorkerCoordinatorStatus state;
    for (unsigned attempt = 0; attempt < 10000; ++attempt) {
        assert(coordinator.pump(error));
        assert(coordinator.status(state));
        if (!state.active && state.profile_epoch != old_epoch) break;
        usleep(1000);
    }
    assert(!state.active && state.profile_epoch != old_epoch && state.recovery_required);

    WorkerResult invalidated_unlock;
    assert(coordinator.take_result(invalidated_unlock));
    assert(invalidated_unlock.request_id == unlock.request_id);
    assert(invalidated_unlock.status == WorkerStatus::Cancelled && invalidated_unlock.secret_payload.empty());
    WorkerResult restore_result;
    assert(coordinator.take_result(restore_result));
    assert(restore_result.request_id == restore.request_id && restore_result.status == WorkerStatus::Ok);

    Store restored_store;
    assert(restored_store.open(profile.paths, StoreOpenMode::Worker, error));
    Account restored_account, removed_account;
    assert(restored_store.get_account(account.id, restored_account, error));
    assert(!restored_store.get_account(after_backup_result.account.id, removed_account, error));
    restored_store.close();

    SecretBytes stale_lookup;
    assert(!worker_find_credential(stale_live_packet, account, CredentialPurpose::Imap,
        account.username, coordinator.profile_epoch(), stale_lookup, error));

    WorkerRequest initialize = test_request(1708, WorkerTaskKind::Initialize);
    SecretBytes no_init_secret;
    assert(submit_current_epoch(coordinator, initialize, static_cast<SecretBytes &&>(no_init_secret), error));
    assert(wait_for_result(coordinator, result, error, 10000));
    assert(result.status == WorkerStatus::Ok);

    SecretBytes second_unlock_packet;
    assert(worker_encode_secret(WorkerSecretPurpose::VaultMaster, worker_test_secret("vault master"),
        SecretBytes(), second_unlock_packet, error));
    WorkerRequest second_unlock = test_request(1709, WorkerTaskKind::UnlockVault);
    assert(submit_current_epoch(coordinator, second_unlock,
        static_cast<SecretBytes &&>(second_unlock_packet), error));
    WorkerResult authenticated_unlock;
    assert(wait_for_result(coordinator, authenticated_unlock, error, 10000));
    assert(authenticated_unlock.status == WorkerStatus::Ok && !authenticated_unlock.secret_payload.empty());
    SecretBytes recovered_password;
    assert(worker_find_credential(authenticated_unlock.secret_payload, account, CredentialPurpose::Imap,
        account.username, coordinator.profile_epoch(), recovered_password, error));
    assert(recovered_password.size() == sizeof(mailbox_password) - 1 &&
        !memcmp(recovered_password.data(), mailbox_password, sizeof(mailbox_password) - 1));

    const uint64_t unlocked_epoch = coordinator.profile_epoch();
    coordinator.close();
    assert(coordinator.open("/proc/self/exe", profile.paths, error));
    assert(coordinator.profile_epoch() != unlocked_epoch);
    assert(!worker_find_credential(authenticated_unlock.secret_payload, account,
        CredentialPurpose::Imap, account.username, coordinator.profile_epoch(), stale_lookup, error));
    coordinator.close();
}

static void test_scheduler_rechecks_alternate_caps_and_maintenance_barrier() {
    WorkerProfileFixture profile;
    Text error;
    WorkerCoordinator coordinator;
    assert(coordinator.open("/proc/self/exe", profile.paths, error));

    const uint64_t ids[] = {31, 32, 33, 34, 35};
    const WorkerTaskKind kinds[] = {
        WorkerTaskKind::TestNetworkMutationWait,
        WorkerTaskKind::TestNetworkWait,
        WorkerTaskKind::TestMutationWait,
        WorkerTaskKind::TestNetworkWait,
        WorkerTaskKind::TestMaintenanceWait
    };
    const uint64_t accounts[] = {101, 202, 101, 303, 0};
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
        WorkerRequest request = test_request(ids[i], kinds[i]);
        request.account_id = accounts[i];
        SecretBytes empty;
        assert(submit_current_epoch(coordinator, request, static_cast<SecretBytes &&>(empty), error));
    }
    assert(coordinator.pump(error));
    WorkerCoordinatorStatus observed;
    assert(coordinator.status(observed));

    /* Cancel and reap every child before asserting, even when this regression fails. */
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
        Text cancel_error;
        (void)coordinator.cancel(ids[i], cancel_error);
    }
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
        WorkerResult result;
        assert(wait_for_result(coordinator, result, error, 5000));
    }
    coordinator.close();

    assert(observed.active == 2);
    assert(observed.active_network == 2);
    assert(observed.queued == 3);
    assert(observed.maintenance_waiting);
}

static WorkerRequest scheduler_fetch_request(uint64_t id, uint64_t account_id) {
    WorkerRequest request = test_request(id, WorkerTaskKind::FetchMessage);
    request.account_id = account_id;
    request.account_generation = 1;
    request.folder_server_name.set("INBOX");
    request.folder_generation = 1;
    request.folder_uidvalidity = 4242;
    request.message_uid = 7;
    WorkerTaskInput input;
    input.message_key.folder.account_id = account_id;
    input.message_key.folder.server_name.set("INBOX");
    input.message_key.uidvalidity = 4242;
    input.message_key.uid = 7;
    Text error;
    assert(worker_encode_task_input(request.kind, input, request.payload, error));
    return request;
}

static void test_fetch_message_uses_network_and_account_mutation_slots() {
    WorkerProfileFixture profile;
    Text error;
    WorkerCoordinator coordinator;
    assert(coordinator.open("/proc/self/exe", profile.paths, error));
    const uint64_t network_ids[] = {81, 82};
    for (size_t i = 0; i < 2; ++i) {
        WorkerRequest request = test_request(network_ids[i], WorkerTaskKind::TestNetworkWait);
        request.account_id = 200 + i;
        SecretBytes empty;
        assert(submit_current_epoch(coordinator, request, static_cast<SecretBytes &&>(empty), error));
    }
    assert(coordinator.pump(error));
    WorkerCoordinatorStatus status;
    assert(coordinator.status(status) && status.active_network == 2);
    WorkerRequest capped_fetch = scheduler_fetch_request(83, 203);
    SecretBytes empty_fetch;
    assert(submit_current_epoch(coordinator, capped_fetch, static_cast<SecretBytes &&>(empty_fetch), error));
    assert(coordinator.pump(error));
    assert(coordinator.status(status));
    assert(status.active == 2 && status.active_network == 2 && status.queued == 1);
    const uint64_t capped_ids[] = {81, 82, 83};
    for (size_t i = 0; i < sizeof(capped_ids) / sizeof(capped_ids[0]); ++i) {
        Text ignored;
        (void)coordinator.cancel(capped_ids[i], ignored);
    }
    for (unsigned i = 0; i < 3; ++i) {
        WorkerResult result;
        assert(wait_for_result(coordinator, result, error, 5000));
    }

    WorkerRequest account_mutation = test_request(84, WorkerTaskKind::TestMutationWait);
    account_mutation.account_id = 204;
    SecretBytes mutation_secret;
    assert(submit_current_epoch(coordinator, account_mutation,
        static_cast<SecretBytes &&>(mutation_secret), error));
    assert(coordinator.pump(error));
    assert(coordinator.status(status) && status.active == 1 && status.active_network == 0);
    WorkerRequest conflicting_fetch = scheduler_fetch_request(85, 204);
    SecretBytes conflict_secret;
    assert(submit_current_epoch(coordinator, conflicting_fetch,
        static_cast<SecretBytes &&>(conflict_secret), error));
    assert(coordinator.pump(error));
    assert(coordinator.status(status));
    assert(status.active == 1 && status.active_network == 0 && status.queued == 1);
    const uint64_t conflict_ids[] = {84, 85};
    for (size_t i = 0; i < sizeof(conflict_ids) / sizeof(conflict_ids[0]); ++i) {
        Text ignored;
        (void)coordinator.cancel(conflict_ids[i], ignored);
    }
    for (unsigned i = 0; i < 2; ++i) {
        WorkerResult result;
        assert(wait_for_result(coordinator, result, error, 5000));
    }
    coordinator.close();
}

static void test_exec_secret_pipe_fd_hygiene_and_child_failure() {
    int inherited = open("/dev/null", O_RDONLY);
    assert(inherited >= 0);
    inherited = fcntl(inherited, F_DUPFD, 10);
    assert(inherited >= 10);
    const int old_flags = fcntl(inherited, F_GETFD);
    assert(old_flags >= 0);
    assert(fcntl(inherited, F_SETFD, old_flags & ~FD_CLOEXEC) == 0);

    ProfilePaths paths;
    Text error;
    WorkerCoordinator coordinator;
    assert(coordinator.open("/proc/self/exe", paths, error));
    WorkerRequest probe = test_request(1, WorkerTaskKind::TestProbe, inherited);
    SecretBytes secret;
    static const char secret_text[] = "worker-private-secret";
    assert(secret.set(secret_text, sizeof(secret_text) - 1));
    assert(submit_current_epoch(coordinator, probe, static_cast<SecretBytes &&>(secret), error));

    WorkerResult result;
    assert(wait_for_result(coordinator, result, error, 5000));
    assert(result.request_id == probe.request_id);
    assert(result.status == WorkerStatus::Ok);
    assert(result.payload.size() == 3);
    assert(result.payload.data()[0] == 1); // Secret arrived over fd 3 and matched.
    assert(result.payload.data()[1] == 0); // Unrelated inherited fd was closed before exec.
    assert(result.payload.data()[2] == 0); // Secret did not appear in argv or environment.

    WorkerRequest secret_output = test_request(2, WorkerTaskKind::TestSecretOutput);
    SecretBytes output_input;
    assert(output_input.set(secret_text, sizeof(secret_text) - 1));
    assert(submit_current_epoch(coordinator, secret_output,
        static_cast<SecretBytes &&>(output_input), error));
    assert(wait_for_result(coordinator, result, error, 5000));
    assert(result.request_id == secret_output.request_id && result.status == WorkerStatus::Ok);
    assert(result.secret_payload.size() == sizeof(secret_text) - 1);
    assert(!memcmp(result.secret_payload.data(), secret_text, sizeof(secret_text) - 1));

    WorkerRequest crash = test_request(3, WorkerTaskKind::TestCrash);
    SecretBytes empty;
    assert(submit_current_epoch(coordinator, crash, static_cast<SecretBytes &&>(empty), error));
    assert(wait_for_result(coordinator, result, error, 5000));
    assert(result.request_id == crash.request_id);
    assert(result.status == WorkerStatus::Failed);
    WorkerRequest after_eof = test_request(4, WorkerTaskKind::TestProbe, -1);
    SecretBytes next_secret;
    assert(next_secret.set(secret_text, sizeof(secret_text) - 1));
    assert(submit_current_epoch(coordinator, after_eof, static_cast<SecretBytes &&>(next_secret), error));
    assert(wait_for_result(coordinator, result, error, 5000));
    assert(result.request_id == after_eof.request_id && result.status == WorkerStatus::Ok);
    assert(result.payload.size() == 3 && result.payload.data()[0] == 1);

    WorkerRequest interleaved = test_request(5, WorkerTaskKind::TestSecretInterleave);
    SecretBytes interleaved_input;
    assert(submit_current_epoch(coordinator, interleaved,
        static_cast<SecretBytes &&>(interleaved_input), error));
    assert(wait_for_result(coordinator, result, error, 5000));
    static const unsigned char expected_ordinary[] = {0x52, 0x4d, 0x57, 0x52, 0x41, 0x42, 0x43};
    static const char expected_secret[] = "secret-before-ordinary-completion";
    assert(result.request_id == interleaved.request_id && result.status == WorkerStatus::Ok);
    assert(result.payload.size() == sizeof(expected_ordinary));
    assert(!memcmp(result.payload.data(), expected_ordinary, sizeof(expected_ordinary)));
    assert(result.secret_payload.size() == sizeof(expected_secret) - 1);
    assert(!memcmp(result.secret_payload.data(), expected_secret, sizeof(expected_secret) - 1));

    WorkerRequest blocked_secret = test_request(6, WorkerTaskKind::TestSecretPipeBlock);
    SecretBytes blocked_input;
    unsigned char blocked_bytes[4096];
    memset(blocked_bytes, 'S', sizeof(blocked_bytes));
    assert(blocked_input.set(blocked_bytes, sizeof(blocked_bytes)));
    assert(submit_current_epoch(coordinator, blocked_secret,
        static_cast<SecretBytes &&>(blocked_input), error));
    assert(wait_for_result(coordinator, result, error, 5000));
    assert(result.request_id == blocked_secret.request_id && result.status == WorkerStatus::Ok);
    assert(result.payload.size() == 1 && result.payload.data()[0] == 1);
    close(inherited);
    coordinator.close();
}

static void test_duplicate_ids_and_cancellation_escalation() {
    WorkerProfileFixture profile;
    Text error;
    WorkerCoordinator coordinator;
    assert(coordinator.open("/proc/self/exe", profile.paths, error));
    WorkerCoordinator singleton_probe;
    assert(!singleton_probe.open("/proc/self/exe", profile.paths, error));
    WorkerRequest wait = test_request(10, WorkerTaskKind::TestWait);
    SecretBytes empty;
    assert(submit_current_epoch(coordinator, wait, static_cast<SecretBytes &&>(empty), error));
    SecretBytes duplicate_secret;
    assert(!submit_current_epoch(coordinator, wait, static_cast<SecretBytes &&>(duplicate_secret), error));
    for (unsigned i = 0; i < 100; ++i) {
        assert(coordinator.pump(error));
        usleep(1000);
    }
    assert(coordinator.cancel(wait.request_id, error));
    WorkerResult result;
    assert(wait_for_result(coordinator, result, error, 5000));
    assert(result.request_id == wait.request_id);
    assert(result.status == WorkerStatus::Cancelled);
    assert(!submit_current_epoch(coordinator, wait, static_cast<SecretBytes &&>(duplicate_secret), error));

    WorkerRequest after_cancel = test_request(11, WorkerTaskKind::TestProbe, -1);
    SecretBytes probe_secret;
    static const char private_text[] = "worker-private-secret";
    assert(probe_secret.set(private_text, sizeof(private_text) - 1));
    assert(submit_current_epoch(coordinator, after_cancel,
        static_cast<SecretBytes &&>(probe_secret), error));
    assert(wait_for_result(coordinator, result, error, 5000));
    assert(result.request_id == after_cancel.request_id && result.status == WorkerStatus::Ok);

    WorkerRequest during_shutdown = test_request(12, WorkerTaskKind::TestWait);
    SecretBytes shutdown_secret;
    assert(submit_current_epoch(coordinator, during_shutdown,
        static_cast<SecretBytes &&>(shutdown_secret), error));
    assert(coordinator.pump(error));
    WorkerCoordinatorStatus snapshot;
    assert(coordinator.status(snapshot) && snapshot.active == 1);
    assert(coordinator.begin_shutdown(error));
    assert(coordinator.status(snapshot) && snapshot.shutting_down);
    WorkerRequest rejected = test_request(13, WorkerTaskKind::TestProbe);
    SecretBytes rejected_secret;
    assert(!submit_current_epoch(coordinator, rejected, static_cast<SecretBytes &&>(rejected_secret), error));
    for (unsigned i = 0; !coordinator.shutdown_complete() && i < 5000; ++i) {
        assert(coordinator.pump(error));
        usleep(1000);
    }
    assert(coordinator.shutdown_complete());
    assert(coordinator.take_result(result));
    assert(result.request_id == during_shutdown.request_id && result.status == WorkerStatus::Cancelled);
    assert(coordinator.finish_shutdown(error));

    assert(coordinator.open("/proc/self/exe", profile.paths, error));
    submit_child_task(coordinator, 1, WorkerTaskKind::Initialize, 0, 0, nullptr,
        SecretBytes(), result);
    assert(result.status == WorkerStatus::Ok);
    assert(coordinator.begin_shutdown(error));
    assert(coordinator.shutdown_complete());
    assert(coordinator.finish_shutdown(error));
}

static void test_coordinator_rejects_missing_profile_epoch() {
    WorkerProfileFixture profile;
    Text error;
    WorkerCoordinator coordinator;
    assert(coordinator.open("/proc/self/exe", profile.paths, error));
    const uint64_t first_epoch = coordinator.profile_epoch();
    assert(first_epoch);
    WorkerRequest missing_epoch = test_request(1401, WorkerTaskKind::TestProbe, -1);
    missing_epoch.profile_epoch = 0;
    SecretBytes empty;
    const bool accepted = coordinator.submit(missing_epoch, static_cast<SecretBytes &&>(empty), error);
    assert(!accepted);

    WorkerRequest stale_epoch = test_request(1402, WorkerTaskKind::TestProbe, -1);
    stale_epoch.profile_epoch = first_epoch + 1;
    SecretBytes stale_secret;
    assert(!coordinator.submit(stale_epoch, static_cast<SecretBytes &&>(stale_secret), error));

    Account account = credential_test_account();
    SecretBytes password;
    assert(password.set("epoch-bound-password", 20));
    SecretBytes old_packet;
    assert(worker_encode_account_credential(account, CredentialPurpose::Imap, first_epoch,
        static_cast<SecretBytes &&>(password), old_packet, error));
    coordinator.close();
    assert(coordinator.open("/proc/self/exe", profile.paths, error));
    const uint64_t reopened_epoch = coordinator.profile_epoch();
    assert(reopened_epoch && reopened_epoch != first_epoch);
    SecretBytes recovered;
    assert(!worker_find_credential(old_packet, account, CredentialPurpose::Imap,
        account.username, reopened_epoch, recovered, error));
    coordinator.close();
}

static void test_recovery_required_gates_until_store_and_vault_reopen() {
    WorkerProfileFixture profile;
    Text error;
    WorkerCoordinator coordinator;
    assert(coordinator.open("/proc/self/exe", profile.paths, error));
    const uint64_t initial_epoch = coordinator.profile_epoch();

    char marker[PATH_MAX];
    assert(snprintf(marker, sizeof(marker), "%s/.credentials.vault.rollback",
        profile.paths.config_dir.c_str()) > 0);
    assert(symlink("unsafe-marker-target", marker) == 0);
    WorkerRequest recovery = test_request(1501, WorkerTaskKind::ChangeVaultPassword);
    SecretBytes password_change;
    assert(worker_encode_secret(WorkerSecretPurpose::VaultPasswordChange,
        worker_test_secret("old master"), worker_test_secret("new master"), password_change, error));
    assert(submit_current_epoch(coordinator, recovery,
        static_cast<SecretBytes &&>(password_change), error));
    WorkerResult result;
    assert(wait_for_result(coordinator, result, error, 5000));
    assert(result.status == WorkerStatus::RecoveryRequired);
    const uint64_t gated_epoch = coordinator.profile_epoch();
    assert(gated_epoch && gated_epoch != initial_epoch);
    WorkerCoordinatorStatus snapshot;
    assert(coordinator.status(snapshot) && snapshot.recovery_required);

    WorkerRequest blocked = test_request(1502, WorkerTaskKind::AccountList);
    SecretBytes blocked_secret;
    assert(!submit_current_epoch(coordinator, blocked, static_cast<SecretBytes &&>(blocked_secret), error));

    WorkerRequest failed_initialize = test_request(1503, WorkerTaskKind::Initialize);
    SecretBytes failed_secret;
    assert(submit_current_epoch(coordinator, failed_initialize,
        static_cast<SecretBytes &&>(failed_secret), error));
    assert(wait_for_result(coordinator, result, error, 5000));
    assert(result.status == WorkerStatus::RecoveryRequired);
    const uint64_t still_gated_epoch = coordinator.profile_epoch();
    assert(still_gated_epoch && still_gated_epoch != gated_epoch);
    assert(coordinator.status(snapshot) && snapshot.recovery_required);
    assert(unlink(marker) == 0);

    WorkerRequest initialize = test_request(1504, WorkerTaskKind::Initialize);
    SecretBytes init_secret;
    assert(submit_current_epoch(coordinator, initialize, static_cast<SecretBytes &&>(init_secret), error));
    assert(wait_for_result(coordinator, result, error, 5000));
    assert(result.status == WorkerStatus::Ok);
    assert(coordinator.status(snapshot) && !snapshot.recovery_required);

    WorkerRequest reopened = test_request(1505, WorkerTaskKind::AccountList);
    SecretBytes reopened_secret;
    assert(submit_current_epoch(coordinator, reopened, static_cast<SecretBytes &&>(reopened_secret), error));
    assert(wait_for_result(coordinator, result, error, 5000));
    assert(result.status == WorkerStatus::Ok);
    coordinator.close();
}

static void test_worker_password_change_keeps_committed_cleanup_warning() {
    WorkerProfileFixture profile;
    Text error;
    Vault original;
    assert(original.open(profile.paths, error));
    assert(original.save(worker_test_secret("old master"),
        worker_test_secret("remembered payload"), error));
    original.close();

    WorkerCoordinator coordinator;
    assert(coordinator.open("/proc/self/exe", profile.paths, error));
    const uint64_t epoch = coordinator.profile_epoch();
    WorkerTaskInput fault_input;
    fault_input.test_vault_fault = static_cast<uint8_t>(VaultFaultPoint::CleanupUnlink);
    SecretBytes packet;
    assert(worker_encode_secret(WorkerSecretPurpose::VaultPasswordChange,
        worker_test_secret("old master"), worker_test_secret("new master"), packet, error));
    WorkerResult result;
    submit_child_task(coordinator, 1510, WorkerTaskKind::ChangeVaultPassword, 0, 0,
        &fault_input, static_cast<SecretBytes &&>(packet), result);
    if (result.status != WorkerStatus::CommittedWarning)
        fprintf(stderr, "password-change worker status=%u diagnostic=%s\n",
            static_cast<unsigned>(result.status), result.diagnostic.c_str());
    assert(result.status == WorkerStatus::CommittedWarning);
    assert(!result.diagnostic.empty());
    assert(coordinator.profile_epoch() == epoch);
    WorkerCoordinatorStatus state;
    assert(coordinator.status(state) && !state.recovery_required);
    coordinator.close();

    Vault reopened;
    assert(reopened.open(profile.paths, error));
    SecretBytes remembered;
    assert(!reopened.unlock(worker_test_secret("old master"), remembered, error));
    assert(reopened.unlock(worker_test_secret("new master"), remembered, error));
    assert(remembered.size() == strlen("remembered payload") &&
        !memcmp(remembered.data(), "remembered payload", remembered.size()));
}

static void test_invalid_live_vault_payload_preserves_existing_vault() {
    WorkerProfileFixture profile;
    Text error;
    Vault original;
    assert(original.open(profile.paths, error));
    assert(original.save(worker_test_secret("old vault master"),
        worker_test_secret("original remembered payload"), error));
    original.close();

    WorkerCoordinator coordinator;
    assert(coordinator.open("/proc/self/exe", profile.paths, error));

    Account account = credential_test_account();
    SecretBytes account_password;
    assert(account_password.set("account password", strlen("account password")));
    SecretBytes live_packet;
    assert(worker_encode_account_credential(account, CredentialPurpose::Imap, coordinator.profile_epoch(),
        static_cast<SecretBytes &&>(account_password), live_packet, error));
    bool found_inner = false;
    for (size_t i = 0; i + 4 <= live_packet.size(); ++i) {
        if (!memcmp(live_packet.data() + i, "RMCV", 4)) {
            const_cast<unsigned char *>(live_packet.data())[i] = 'X';
            found_inner = true;
            break;
        }
    }
    assert(found_inner);
    SecretBytes save_packet;
    assert(worker_encode_secret(WorkerSecretPurpose::VaultSave,
        worker_test_secret("replacement master"), static_cast<SecretBytes &&>(live_packet),
        save_packet, error));

    WorkerResult result;
    submit_child_task(coordinator, 1520, WorkerTaskKind::SaveVault, 0, 0,
        nullptr, static_cast<SecretBytes &&>(save_packet), result);
    assert(result.status == WorkerStatus::InvalidRequest);
    coordinator.close();

    Vault reopened;
    assert(reopened.open(profile.paths, error));
    SecretBytes remembered;
    assert(reopened.unlock(worker_test_secret("old vault master"), remembered, error));
    assert(remembered.size() == strlen("original remembered payload") &&
        !memcmp(remembered.data(), "original remembered payload", remembered.size()));
    assert(!reopened.unlock(worker_test_secret("replacement master"), remembered, error));
}

int main(int argc, char **argv) {
    if (worker_mode_requested(argc, const_cast<const char *const *>(argv)))
        return worker_entry_main();
    if (argc == 4 && !strcmp(argv[1], "--fixture")) {
        test_protocol_fixture_worker(argc, argv);
        return 0;
    }
    test_versioned_request_frames_reject_bad_lengths_and_versions();
    test_fetch_message_is_a_public_framed_task();
    test_public_task_result_codecs_are_bounded_and_typed();
    test_reader_result_clone_preserves_reply_context_and_bounded_metadata();
    test_credentials_bind_full_account_configuration_and_secret_purpose();
    test_legacy_v6_vault_unlock_rebinds_and_reads_credentials();
    test_draft_tasks_require_existing_revision_and_account_identity();
    test_forward_as_attachment_uses_exact_full_cached_original();
    test_copy_draft_to_another_account_preserves_source_and_attachment();
    test_compose_catalog_returns_account_signature_templates_and_contacts();
    test_folder_worker_requests_reject_missing_and_contradictory_guards();
    test_large_reader_result_is_a_utf8_preview_and_raw_chunks_are_exact();
    test_exec_secret_pipe_fd_hygiene_and_child_failure();
    test_real_sequential_account_tasks_reuse_worker_slots();
    test_queue_send_validates_unicode_folded_sender_and_rejects_tampering();
    test_send_spool_rejects_encoded_word_inside_from_addr_spec();
    test_queue_send_cancel_after_commit_keeps_durable_success();
    test_stale_ready_queue_operation_can_only_be_locally_cancelled();
    test_archive_pending_old_generation_requires_confirmed_exact_target_review();
    test_restore_invalidates_unconsumed_unlock_and_preserves_commit_on_cancel();
    test_scheduler_rechecks_alternate_caps_and_maintenance_barrier();
    test_fetch_message_uses_network_and_account_mutation_slots();
    test_duplicate_ids_and_cancellation_escalation();
    test_coordinator_rejects_missing_profile_epoch();
    test_recovery_required_gates_until_store_and_vault_reopen();
    test_worker_password_change_keeps_committed_cleanup_warning();
    test_invalid_live_vault_payload_preserves_existing_vault();
    puts("ReliefMail worker suite passed");
    return 0;
}
