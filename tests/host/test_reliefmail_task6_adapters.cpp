#include "ui_adapters.h"

#include <assert.h>
#include <string.h>

using namespace rmail;

static void test_startup_query_respects_and_releases_vault_maintenance_barrier() {
    assert(task6_startup_query_action(Task6StartupVaultCheck::Pending) ==
        Task6StartupQueryAction::WaitForVaultCheck);
    assert(task6_startup_query_action(Task6StartupVaultCheck::Complete) ==
        Task6StartupQueryAction::SubmitQueryNow);
    assert(task6_startup_query_action(Task6StartupVaultCheck::NotStarted) ==
        Task6StartupQueryAction::SubmitQueryNow);
}

struct CloseGuardFixture {
    bool allow = false;
    const char *reason = nullptr;
    unsigned calls = 0;
};

static bool close_guard_fixture(void *context, Text &reason) {
    CloseGuardFixture *fixture = static_cast<CloseGuardFixture *>(context);
    ++fixture->calls;
    if (fixture->reason) assert(reason.set(fixture->reason));
    return fixture->allow;
}

static void test_extension_close_guard_can_veto_shutdown_with_a_reason() {
    Text reason;
    assert(task6_extension_allows_application_close(nullptr, nullptr, reason));
    assert(reason.empty());

    CloseGuardFixture fixture;
    fixture.allow = false;
    fixture.reason = "Resolve the open draft before closing.";
    assert(!task6_extension_allows_application_close(close_guard_fixture, &fixture, reason));
    assert(fixture.calls == 1);
    assert(strcmp(reason.c_str(), fixture.reason) == 0);

    fixture.allow = true;
    assert(task6_extension_allows_application_close(close_guard_fixture, &fixture, reason));
    assert(fixture.calls == 2);
    assert(reason.empty());
}

static void test_row_action_keeps_the_query_captured_identity() {
    WorkerPageRow row;
    row.profile_epoch = 91;
    row.ui_generation = 18;
    row.account_generation = 1001;
    row.folder_generation = 77;
    row.message.key.folder.account_id = 42;
    assert(row.message.key.folder.server_name.set("Projects/Inbox"));
    row.message.key.uidvalidity = 808;
    row.message.key.uid = 19;

    WorkerRequest request;
    Text error;
    assert(task6_build_message_request(row, 500, WorkerTaskKind::FetchMessage,
        91, 18, request, error));
    assert(request.request_id == 500);
    assert(request.profile_epoch == 91 && request.ui_generation == 18);
    assert(request.account_id == 42 && request.account_generation == 1001);
    assert(request.folder_server_name == "Projects/Inbox");
    assert(request.folder_generation == 77 && request.folder_uidvalidity == 808);
    assert(request.message_uid == 19);

    assert(!task6_build_message_request(row, 501, WorkerTaskKind::FetchMessage,
        92, 18, request, error));
    row.account_generation = 0;
    assert(!task6_build_message_request(row, 502, WorkerTaskKind::SetFlags,
        91, 18, request, error));
}

static void test_matching_restore_transition_precedes_stale_epoch_filter() {
    Task6PendingRequest pending;
    pending.request_id = 70;
    pending.kind = WorkerTaskKind::RestoreProfile;
    pending.profile_epoch = 12;
    pending.ui_generation = 4;

    WorkerResult result;
    result.request_id = 70;
    result.kind = WorkerTaskKind::RestoreProfile;
    result.profile_epoch = 12;
    result.ui_generation = 4;
    result.status = WorkerStatus::CommittedWarning;
    assert(task6_classify_result(pending, result, 13, 8) ==
        Task6ResultDisposition::ProfileTransition);

    result.request_id = 71;
    assert(task6_classify_result(pending, result, 13, 8) ==
        Task6ResultDisposition::Ignore);
    result.request_id = 70;
    result.kind = WorkerTaskKind::MessagePage;
    result.status = WorkerStatus::Ok;
    assert(task6_classify_result(pending, result, 13, 8) ==
        Task6ResultDisposition::Ignore);

    pending.kind = WorkerTaskKind::MessagePage;
    result.kind = WorkerTaskKind::MessagePage;
    result.profile_epoch = 13;
    result.ui_generation = 8;
    assert(task6_classify_result(pending, result, 13, 8) ==
        Task6ResultDisposition::Ignore);
    pending.profile_epoch = 13;
    pending.ui_generation = 8;
    assert(task6_classify_result(pending, result, 13, 8) ==
        Task6ResultDisposition::Apply);
}

static void test_vault_control_results_survive_view_generation_changes_only() {
    const WorkerTaskKind controls[] = {WorkerTaskKind::VaultStatus,
        WorkerTaskKind::UnlockVault, WorkerTaskKind::SaveVault,
        WorkerTaskKind::ChangeVaultPassword, WorkerTaskKind::AccountTestImap,
        WorkerTaskKind::AccountTestSmtp, WorkerTaskKind::AccountCreate,
        WorkerTaskKind::AccountUpdate, WorkerTaskKind::AccountDelete,
        WorkerTaskKind::AccountSetDefault};
    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i) {
        Task6PendingRequest pending;
        pending.request_id = 91 + i;
        pending.kind = controls[i];
        pending.profile_epoch = 12;
        pending.ui_generation = 4;
        WorkerResult result;
        result.request_id = pending.request_id;
        result.kind = pending.kind;
        result.profile_epoch = 12;
        result.ui_generation = 4;
        result.status = WorkerStatus::Ok;
        assert(task6_classify_result(pending, result, 12, 8) ==
            Task6ResultDisposition::Apply);
        result.status = WorkerStatus::Failed;
        assert(task6_classify_result(pending, result, 12, 8) ==
            Task6ResultDisposition::Apply);
        result.ui_generation = 5;
        assert(task6_classify_result(pending, result, 12, 8) ==
            Task6ResultDisposition::Ignore);
        result.ui_generation = 4;
        assert(task6_classify_result(pending, result, 13, 8) ==
            Task6ResultDisposition::Ignore);
    }

    Task6PendingRequest query;
    query.request_id = 117;
    query.kind = WorkerTaskKind::MessagePage;
    query.profile_epoch = 12;
    query.ui_generation = 4;
    WorkerResult page;
    page.request_id = query.request_id;
    page.kind = query.kind;
    page.profile_epoch = 12;
    page.ui_generation = 4;
    page.status = WorkerStatus::Ok;
    assert(task6_classify_result(query, page, 12, 8) ==
        Task6ResultDisposition::Ignore);

    const WorkerTaskKind compose_kinds[] = {WorkerTaskKind::SaveDraft,
        WorkerTaskKind::SaveDraftRevision, WorkerTaskKind::SaveForwardDraft,
        WorkerTaskKind::SaveDraftCopy, WorkerTaskKind::LoadDraft,
        WorkerTaskKind::QueueSend, WorkerTaskKind::SendQueued, WorkerTaskKind::ListDrafts,
        WorkerTaskKind::ComposeCatalog};
    for (size_t i = 0; i < sizeof(compose_kinds) / sizeof(compose_kinds[0]); ++i) {
        Task6PendingRequest compose;
        compose.request_id = 200 + i;
        compose.kind = compose_kinds[i];
        compose.profile_epoch = 12;
        compose.ui_generation = 4;
        WorkerResult saved;
        saved.request_id = compose.request_id;
        saved.kind = compose.kind;
        saved.profile_epoch = 12;
        saved.ui_generation = 4;
        saved.status = WorkerStatus::Ok;
        assert(task6_classify_result(compose, saved, 12, 8) == Task6ResultDisposition::Apply);
        saved.ui_generation = 5;
        assert(task6_classify_result(compose, saved, 12, 8) == Task6ResultDisposition::Ignore);
    }
}

static void test_archive_retry_result_survives_mail_view_changes() {
    Task6PendingRequest pending;
    pending.request_id = 480;
    pending.kind = WorkerTaskKind::RetryArchive;
    pending.profile_epoch = 31;
    pending.ui_generation = 7;

    WorkerResult result;
    result.request_id = pending.request_id;
    result.kind = pending.kind;
    result.profile_epoch = pending.profile_epoch;
    result.ui_generation = pending.ui_generation;
    result.status = WorkerStatus::CommittedWarning;

    // A matching durable queue result must refresh its review state even if
    // the user changed the mail query while IMAP archive work was running.
    assert(task6_classify_result(pending, result, 31, 8) ==
        Task6ResultDisposition::Apply);
    assert(task6_classify_result(pending, result, 32, 8) ==
        Task6ResultDisposition::Ignore);
}

static void test_only_bounded_http_targets_can_leave_the_reader() {
    static const char https[] = "https://mail.example.test/a?b=1";
    static const char http[] = "HTTP://example.test/";
    static const char file[] = "file:///etc/passwd";
    static const char cid[] = "cid:part@example.test";
    static const char script[] = "javascript:alert(1)";
    static const char space[] = "https://example.test/a b";
    static const char newline[] = "https://example.test/\n";
    assert(task6_is_safe_web_link(https, sizeof(https) - 1));
    assert(task6_is_safe_web_link(http, sizeof(http) - 1));
    assert(!task6_is_safe_web_link(file, sizeof(file) - 1));
    assert(!task6_is_safe_web_link(cid, sizeof(cid) - 1));
    assert(!task6_is_safe_web_link(script, sizeof(script) - 1));
    assert(!task6_is_safe_web_link(space, sizeof(space) - 1));
    assert(!task6_is_safe_web_link(newline, sizeof(newline) - 1));
}

static void test_fltk_uri_result_is_interpreted_as_success_one_failure_zero() {
    assert(task6_uri_handoff_succeeded(1));
    assert(!task6_uri_handoff_succeeded(0));
    assert(!task6_uri_handoff_succeeded(-1));
}

static void test_reader_display_preserves_utf8_and_replaces_invalid_bytes_and_controls() {
    static const char input[] = {
        '\xe4', '\xb8', '\xad', '\xe6', '\x96', '\x87',
        static_cast<char>(0xff), '\0', 'A', '\r', '\n', '\t', '\x1f',
        static_cast<char>(0xf0), static_cast<char>(0x9f), static_cast<char>(0x98), static_cast<char>(0x80),
        static_cast<char>(0xc0), static_cast<char>(0xaf)
    };
    static const char expected[] = "\xe4\xb8\xad\xe6\x96\x87? A\r\n\t \xf0\x9f\x98\x80??";
    Text display;
    assert(task6_sanitize_display_text(input, sizeof(input), display));
    assert(display.size() == sizeof(expected) - 1);
    assert(memcmp(display.c_str(), expected, sizeof(expected) - 1) == 0);
    assert(valid_utf8(display.c_str(), display.size()));

    Text empty;
    assert(task6_sanitize_display_text(nullptr, 0, empty));
    assert(empty.empty());
}

static void test_fltk_menu_labels_escape_literal_at_symbols() {
    static const char filename[] = "附件：@B资源 @C1检查.txt & more";
    Text escaped;
    assert(task6_escape_fltk_label_symbols(filename, sizeof(filename) - 1, escaped));
    assert(escaped == "附件：@@B资源 @@C1检查.txt & more");
    assert(task6_escape_fltk_label_symbols(nullptr, 0, escaped));
    assert(escaped.empty());
    assert(!task6_escape_fltk_label_symbols(nullptr, 1, escaped));
}

static void test_attachment_filename_display_is_one_safe_line() {
    static const char input[] = "联调\n进度\t@B.csv";
    Text display;
    assert(task6_sanitize_single_line_display_text(input, sizeof(input) - 1, display));
    assert(display == "联调 进度 @B.csv");
}

static void test_received_date_is_signed_utc_and_calendar_valid() {
    int64_t timestamp = 0;
    Text error;
    assert(task6_parse_received_date("1969-12-31", timestamp, error));
    assert(timestamp == -86400);
    assert(task6_parse_received_date("1970-01-01", timestamp, error));
    assert(timestamp == 0);
    assert(task6_parse_received_date("2000-02-29", timestamp, error));
    assert(timestamp == 951782400);
    assert(task6_parse_received_date("2024-02-29", timestamp, error));
    assert(timestamp == 1709164800);
    assert(!task6_parse_received_date("1900-02-29", timestamp, error));
    assert(!task6_parse_received_date("2023-02-29", timestamp, error));
    assert(!task6_parse_received_date("2026-13-01", timestamp, error));
    assert(!task6_parse_received_date("2026-1-01", timestamp, error));
}

static void test_server_search_rejects_local_only_filters_and_stale_reader_results() {
    MessageQuerySpec query;
    query.scope = MessageQueryScope::Folder;
    query.account_id = 4;
    query.folder.account_id = 4;
    assert(query.folder.server_name.set("INBOX"));
    Text error;
    assert(task6_validate_server_search_query(query, error));
    query.has_received_after = true;
    query.received_after = -1;
    assert(!task6_validate_server_search_query(query, error));
    query.has_received_after = false;
    query.attachment_filter = AttachmentQueryFilter::Unknown;
    assert(!task6_validate_server_search_query(query, error));
    query.attachment_filter = AttachmentQueryFilter::Any;
    query.required_flags = 1;
    assert(!task6_validate_server_search_query(query, error));
    query.required_flags = 0;
    query.scope = MessageQueryScope::Unified;
    assert(!task6_validate_server_search_query(query, error));
    query.scope = MessageQueryScope::AllMail;
    assert(!task6_validate_server_search_query(query, error));
    assert(task6_selection_result_is_current(0, 19));
    assert(task6_selection_result_is_current(19, 19));
    assert(!task6_selection_result_is_current(18, 19));
}

static void test_reader_result_updates_only_the_captured_matching_row() {
    MessageQueryRow row;
    row.message.key.folder.account_id = 7;
    assert(row.message.key.folder.server_name.set("INBOX"));
    row.message.key.uidvalidity = 31;
    row.message.key.uid = 9;
    row.message.headers_only = true;
    assert(row.message.subject.set("Cached headers"));
    assert(row.message.from.set("old@example.test"));
    row.attachment_presence_known = false;
    WorkerReaderResult reader;
    reader.key = row.message.key;
    reader.headers_only = false;
    assert(reader.subject.set("Decoded subject"));
    assert(reader.from.set("new@example.test"));
    assert(reader.attachments.push(WorkerAttachmentMetadata()));
    assert(task6_apply_reader_result_to_row(reader, row));
    assert(!row.message.headers_only);
    assert(row.message.subject == "Decoded subject");
    assert(row.message.from == "new@example.test");
    assert(row.attachment_presence_known && row.has_attachments);

    MessageQueryRow headers;
    headers.message.key = reader.key;
    headers.message.headers_only = true;
    headers.attachment_presence_known = false;
    WorkerReaderResult header_result;
    header_result.key = headers.message.key;
    header_result.headers_only = true;
    assert(task6_apply_reader_result_to_row(header_result, headers));
    assert(headers.message.headers_only);
    assert(!headers.attachment_presence_known);

    MessageQueryRow other = row;
    ++other.message.key.uid;
    other.message.headers_only = true;
    other.attachment_presence_known = false;
    assert(!task6_apply_reader_result_to_row(reader, other));
    assert(other.message.headers_only);
    assert(!other.attachment_presence_known);
}

static void test_header_sync_identity_refreshes_only_the_matching_folder_snapshot() {
    WorkerStartupSnapshot startup;
    Account account;
    account.id = 41;
    account.generation = 73;
    assert(startup.accounts.push(account));
    Folder folder;
    folder.key.account_id = account.id;
    assert(folder.key.server_name.set("Reader checks"));
    folder.generation = 88;
    folder.uidvalidity = 0; // LIST can discover the name before SELECT learns UIDVALIDITY.
    folder.selectable = true;
    assert(startup.folders.push(folder));
    Folder selected = folder;
    bool selected_valid = true;

    MessageQueryPage page;
    page.query.scope = MessageQueryScope::Folder;
    page.query.account_id = account.id;
    page.query.folder = folder.key;
    page.folder_generation = folder.generation;
    page.folder_uidvalidity = 4242;
    // An empty folder still returns its verified SELECT identity.
    Text error;
    assert(task6_apply_folder_sync_identity(startup, selected, selected_valid,
        folder, account.generation, account.generation, page, error));
    assert(startup.folders[0].uidvalidity == 4242);
    assert(selected.uidvalidity == 4242 && selected.generation == folder.generation);

    MessageQueryRow verified_row;
    verified_row.message.key.folder = folder.key;
    verified_row.message.key.uidvalidity = 4242;
    verified_row.message.key.uid = 2;
    verified_row.account_generation = account.generation;
    verified_row.folder_generation = folder.generation;
    assert(page.rows.push(verified_row));
    assert(task6_apply_folder_sync_identity(startup, selected, selected_valid,
        folder, account.generation, account.generation, page, error));
    page.rows[0].message.key.uidvalidity = 0;
    assert(!task6_apply_folder_sync_identity(startup, selected, selected_valid,
        folder, account.generation, account.generation, page, error));
    assert(page.rows[0].message.key.uidvalidity == 0); // Captured row identity is never rewritten.
    page.rows.clear();

    Folder replacement = startup.folders[0];
    replacement.generation = folder.generation + 1;
    replacement.uidvalidity = 9000;
    startup.folders[0] = replacement;
    selected = replacement;
    const uint64_t before = startup.folders[0].uidvalidity;
    assert(!task6_apply_folder_sync_identity(startup, selected, selected_valid,
        folder, account.generation, account.generation, page, error));
    assert(startup.folders[0].uidvalidity == before);
    assert(selected.uidvalidity == before);

    startup.folders[0] = folder;
    selected = folder;
    assert(!task6_apply_folder_sync_identity(startup, selected, selected_valid,
        folder, account.generation, account.generation + 1, page, error));
    assert(startup.folders[0].uidvalidity == 0 && selected.uidvalidity == 0);

    page.folder_uidvalidity = 0;
    assert(!task6_apply_folder_sync_identity(startup, selected, selected_valid,
        folder, account.generation, account.generation, page, error));
    assert(startup.folders[0].uidvalidity == 0 && selected.uidvalidity == 0);
}

static void test_account_secret_reuse_is_scoped_to_the_unchanged_transport_identity() {
    Account before;
    before.id = 7;
    before.generation = 11;
    assert(before.email_address.set("alice@example.test"));
    assert(before.username.set("alice@example.test"));
    assert(before.smtp_username.set("smtp-alice@example.test"));
    assert(before.incoming_host.set("imap.example.test"));
    assert(before.outgoing_host.set("smtp.example.test"));

    Account after = before;
    after.generation = 12;
    assert(task6_account_secret_identity_unchanged(before, after, CredentialPurpose::Imap));
    assert(task6_account_secret_identity_unchanged(before, after, CredentialPurpose::Smtp));

    after.incoming_host.set("imap.other.test");
    assert(!task6_account_secret_identity_unchanged(before, after, CredentialPurpose::Imap));
    after = before;
    after.username.set("alice2@example.test");
    assert(!task6_account_secret_identity_unchanged(before, after, CredentialPurpose::Imap));
    after = before;
    after.outgoing_host.set("smtp.other.test");
    assert(!task6_account_secret_identity_unchanged(before, after, CredentialPurpose::Smtp));
    after = before;
    after.smtp_username.set("smtp-alice2@example.test");
    assert(!task6_account_secret_identity_unchanged(before, after, CredentialPurpose::Smtp));

    after = before;
    after.id = 8;
    assert(!task6_account_secret_identity_unchanged(before, after, CredentialPurpose::Imap));
    assert(!task6_account_secret_identity_unchanged(before, after, CredentialPurpose::Shared));
}

static void test_new_account_form_validation_keeps_unassigned_id_for_worker_create() {
    Account draft;
    assert(draft.display_name.set("CRUD @B & literal"));
    assert(draft.email_address.set("crud@reliefmail.test"));
    assert(draft.username.set("crud@reliefmail.test"));
    assert(draft.smtp_username.set("crud@reliefmail.test"));
    assert(draft.incoming_host.set("127.0.0.1"));
    assert(draft.outgoing_host.set("127.0.0.1"));
    Text error;

    // The GUI validates fields before AccountCreate assigns the durable ID.
    // Model validation must continue to reject an unassigned account.
    assert(!validate_account(draft, error));
    assert(strcmp(error.c_str(), "invalid account identity") == 0);
    assert(task6_validate_account_form(draft, error));
    assert(draft.id == 0);

    Account saved = draft;
    saved.id = 17;
    assert(validate_account(saved, error));
    assert(task6_validate_account_form(saved, error));

    draft.incoming_host.clear();
    assert(!task6_validate_account_form(draft, error));
}

int main() {
    test_startup_query_respects_and_releases_vault_maintenance_barrier();
    test_extension_close_guard_can_veto_shutdown_with_a_reason();
    test_row_action_keeps_the_query_captured_identity();
    test_matching_restore_transition_precedes_stale_epoch_filter();
    test_vault_control_results_survive_view_generation_changes_only();
    test_archive_retry_result_survives_mail_view_changes();
    test_only_bounded_http_targets_can_leave_the_reader();
    test_fltk_uri_result_is_interpreted_as_success_one_failure_zero();
    test_reader_display_preserves_utf8_and_replaces_invalid_bytes_and_controls();
    test_fltk_menu_labels_escape_literal_at_symbols();
    test_attachment_filename_display_is_one_safe_line();
    test_received_date_is_signed_utc_and_calendar_valid();
    test_server_search_rejects_local_only_filters_and_stale_reader_results();
    test_reader_result_updates_only_the_captured_matching_row();
    test_header_sync_identity_refreshes_only_the_matching_folder_snapshot();
    test_account_secret_reuse_is_scoped_to_the_unchanged_transport_identity();
    test_new_account_form_validation_keeps_unassigned_id_for_worker_create();
    return 0;
}
