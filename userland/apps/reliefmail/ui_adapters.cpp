#include "ui_adapters.h"
#include "html.h"

#include <stdlib.h>
#include <string.h>

namespace rmail {
namespace {

bool is_message_task(WorkerTaskKind kind) {
    switch (kind) {
    case WorkerTaskKind::ParseMessage:
    case WorkerTaskKind::FetchMessage:
    case WorkerTaskKind::ReadMessageChunk:
    case WorkerTaskKind::SetFlags:
    case WorkerTaskKind::MoveMessage:
    case WorkerTaskKind::CopyMessage:
    case WorkerTaskKind::DeleteMessage:
        return true;
    default:
        return false;
    }
}

bool transition_status(WorkerStatus status) {
    return status == WorkerStatus::Ok || status == WorkerStatus::CommittedWarning ||
        status == WorkerStatus::RecoveryRequired;
}

bool result_uses_current_view_generation(WorkerTaskKind kind) {
    switch (kind) {
    case WorkerTaskKind::AccountTestImap:
    case WorkerTaskKind::AccountTestSmtp:
    case WorkerTaskKind::AccountCreate:
    case WorkerTaskKind::AccountUpdate:
    case WorkerTaskKind::AccountDelete:
    case WorkerTaskKind::AccountSetDefault:
    case WorkerTaskKind::VaultStatus:
    case WorkerTaskKind::UnlockVault:
    case WorkerTaskKind::SaveVault:
    case WorkerTaskKind::ChangeVaultPassword:
    case WorkerTaskKind::SaveDraft:
    case WorkerTaskKind::SaveDraftRevision:
    case WorkerTaskKind::SaveForwardDraft:
    case WorkerTaskKind::SaveDraftCopy:
    case WorkerTaskKind::ComposeCatalog:
    case WorkerTaskKind::LoadDraft:
    case WorkerTaskKind::DeleteDraft:
    case WorkerTaskKind::QueueSend:
    case WorkerTaskKind::SendQueued:
    case WorkerTaskKind::RetryArchive:
    case WorkerTaskKind::CancelQueuedSend:
    case WorkerTaskKind::ListSendOperations:
    case WorkerTaskKind::ReviewArchiveTarget:
    case WorkerTaskKind::ResolveUncertainSend:
    case WorkerTaskKind::ListDrafts:
    case WorkerTaskKind::FolderSelect:
    case WorkerTaskKind::FolderOperation:
    case WorkerTaskKind::SetFlags:
    case WorkerTaskKind::MoveMessage:
    case WorkerTaskKind::CopyMessage:
    case WorkerTaskKind::DeleteMessage:
        return false;
    default:
        return true;
    }
}

bool parse_two_digits(const char *text, unsigned &value) {
    if (text[0] < '0' || text[0] > '9' || text[1] < '0' || text[1] > '9') return false;
    value = static_cast<unsigned>((text[0] - '0') * 10 + (text[1] - '0'));
    return true;
}

bool parse_four_digits(const char *text, unsigned &value) {
    value = 0;
    for (size_t i = 0; i < 4; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + static_cast<unsigned>(text[i] - '0');
    }
    return true;
}

bool leap_year(unsigned year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

bool read_utf8_scalar(const unsigned char *bytes, size_t size, size_t &offset,
    uint32_t &scalar, size_t &sequence_size) {
    const unsigned char lead = bytes[offset];
    if (lead < 0x80) {
        scalar = lead;
        sequence_size = 1;
        return true;
    }
    if (lead >= 0xc2 && lead <= 0xdf) {
        sequence_size = 2;
        scalar = lead & 0x1f;
    } else if (lead >= 0xe0 && lead <= 0xef) {
        sequence_size = 3;
        scalar = lead & 0x0f;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
        sequence_size = 4;
        scalar = lead & 0x07;
    } else {
        sequence_size = 0;
        return false;
    }
    if (sequence_size > size - offset) return false;
    for (size_t i = 1; i < sequence_size; ++i) {
        const unsigned char next = bytes[offset + i];
        if ((next & 0xc0) != 0x80) return false;
        scalar = (scalar << 6) | (next & 0x3f);
    }
    if ((sequence_size == 2 && scalar < 0x80) ||
        (sequence_size == 3 && scalar < 0x800) ||
        (sequence_size == 4 && scalar < 0x10000) || scalar > 0x10ffff ||
        (scalar >= 0xd800 && scalar <= 0xdfff)) return false;
    if (sequence_size == 3 && lead == 0xe0 && bytes[offset + 1] < 0xa0) return false;
    if (sequence_size == 3 && lead == 0xed && bytes[offset + 1] >= 0xa0) return false;
    if (sequence_size == 4 && lead == 0xf0 && bytes[offset + 1] < 0x90) return false;
    if (sequence_size == 4 && lead == 0xf4 && bytes[offset + 1] >= 0x90) return false;
    return true;
}

int64_t days_from_civil(unsigned year, unsigned month, unsigned day) {
    int64_t adjusted_year = static_cast<int64_t>(year) - (month <= 2 ? 1 : 0);
    const int64_t era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(adjusted_year - era * 400);
    const unsigned adjusted_month = month > 2 ? month - 3 : month + 9;
    const unsigned day_of_year = (153 * adjusted_month + 2) / 5 + day - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + static_cast<int64_t>(day_of_era) - 719468;
}

}

bool task6_build_message_request(const MessageQueryRow &row, uint64_t request_id,
    WorkerTaskKind kind, uint64_t current_profile_epoch, uint64_t current_ui_generation,
    WorkerRequest &request, Text &error) {
    error.clear();
    if (!request_id || !is_message_task(kind) || !current_profile_epoch ||
        row.profile_epoch != current_profile_epoch || !current_ui_generation ||
        row.ui_generation != current_ui_generation || !row.account_generation ||
        !row.folder_generation || !row.message.key.folder.account_id ||
        row.message.key.folder.server_name.empty() || !row.message.key.uidvalidity ||
        !row.message.key.uid) {
        error.set("message row is stale or lacks its captured identity");
        return false;
    }
    WorkerRequest next;
    next.request_id = request_id;
    next.profile_epoch = row.profile_epoch;
    next.ui_generation = row.ui_generation;
    next.account_id = row.message.key.folder.account_id;
    next.account_generation = row.account_generation;
    next.folder_server_name = row.message.key.folder.server_name;
    next.folder_generation = row.folder_generation;
    next.folder_uidvalidity = row.message.key.uidvalidity;
    next.message_uid = row.message.key.uid;
    next.kind = kind;
    request = static_cast<WorkerRequest &&>(next);
    return true;
}

Task6ResultDisposition task6_classify_result(const Task6PendingRequest &pending,
    const WorkerResult &result, uint64_t current_profile_epoch,
    uint64_t current_ui_generation) {
    if (!pending.request_id || !pending.profile_epoch || !pending.ui_generation ||
        result.request_id != pending.request_id || result.kind != pending.kind ||
        result.profile_epoch != pending.profile_epoch)
        return Task6ResultDisposition::Ignore;
    if (result.status == WorkerStatus::RecoveryRequired ||
        (result.kind == WorkerTaskKind::RestoreProfile && transition_status(result.status)))
        return Task6ResultDisposition::ProfileTransition;
    if (current_profile_epoch != pending.profile_epoch ||
        result.ui_generation != pending.ui_generation ||
        (result_uses_current_view_generation(pending.kind) &&
         current_ui_generation != pending.ui_generation))
        return Task6ResultDisposition::Ignore;
    return Task6ResultDisposition::Apply;
}

bool task6_is_safe_web_link(const char *target, size_t length) {
    return is_safe_external_web_link(target, length);
}

bool task6_uri_handoff_succeeded(int fltk_result) {
    return fltk_result == 1;
}

bool task6_sanitize_display_text(const char *data, size_t size, Text &display) {
    if ((!data && size) || size == SIZE_MAX) return false;
    char *safe = static_cast<char *>(malloc(size + 1));
    if (!safe) return false;
    const unsigned char *bytes = reinterpret_cast<const unsigned char *>(data);
    size_t input = 0;
    size_t output = 0;
    while (input < size) {
        uint32_t scalar = 0;
        size_t sequence_size = 0;
        if (!read_utf8_scalar(bytes, size, input, scalar, sequence_size)) {
            safe[output++] = '?';
            ++input;
            continue;
        }
        if (scalar == 0 || (scalar < 0x20 && scalar != '\n' && scalar != '\r' && scalar != '\t') ||
            (scalar >= 0x7f && scalar <= 0x9f)) {
            safe[output++] = ' ';
        } else {
            memcpy(safe + output, data + input, sequence_size);
            output += sequence_size;
        }
        input += sequence_size;
    }
    safe[output] = 0;
    Text next;
    const bool ok = next.set(safe, output);
    free(safe);
    if (!ok) return false;
    display = static_cast<Text &&>(next);
    return true;
}

bool task6_sanitize_single_line_display_text(const char *data, size_t size, Text &display) {
    if ((!data && size) || size > 4096) return false;
    Text safe;
    if (!task6_sanitize_display_text(data, size, safe)) return false;
    char *buffer = static_cast<char *>(malloc(safe.size() + 1));
    if (!buffer) return false;
    for (size_t i = 0; i < safe.size(); ++i) {
        const char value = safe.c_str()[i];
        buffer[i] = value == '\n' || value == '\r' || value == '\t' ? ' ' : value;
    }
    buffer[safe.size()] = 0;
    Text next;
    const bool ok = next.set(buffer, safe.size());
    free(buffer);
    if (!ok) return false;
    display = static_cast<Text &&>(next);
    return true;
}

bool task6_escape_fltk_label_symbols(const char *data, size_t size, Text &escaped) {
    if ((!data && size) || size > 8192) return false;
    size_t at_count = 0;
    for (size_t i = 0; i < size; ++i) if (data[i] == '@') ++at_count;
    if (at_count > SIZE_MAX - size - 1) return false;
    char *buffer = static_cast<char *>(malloc(size + at_count + 1));
    if (!buffer) return false;
    size_t output = 0;
    for (size_t i = 0; i < size; ++i) {
        buffer[output++] = data[i];
        if (data[i] == '@') buffer[output++] = '@';
    }
    buffer[output] = 0;
    Text next;
    const bool ok = next.set(buffer, output);
    free(buffer);
    if (!ok) return false;
    escaped = static_cast<Text &&>(next);
    return true;
}

bool task6_parse_received_date(const char *text, int64_t &unix_seconds, Text &error) {
    error.clear();
    if (!text || strlen(text) != 10 || text[4] != '-' || text[7] != '-') {
        error.set("Use a received date in YYYY-MM-DD format.");
        return false;
    }
    unsigned year = 0, month = 0, day = 0;
    if (!parse_four_digits(text, year) || !parse_two_digits(text + 5, month) ||
        !parse_two_digits(text + 8, day) || !year || month < 1 || month > 12) {
        error.set("The received date is not a valid calendar date.");
        return false;
    }
    static const unsigned month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    unsigned last_day = month_days[month - 1];
    if (month == 2 && leap_year(year)) ++last_day;
    if (!day || day > last_day) {
        error.set("The received date is not a valid calendar date.");
        return false;
    }
    const int64_t days = days_from_civil(year, month, day);
    unix_seconds = days * 86400;
    return true;
}

bool task6_validate_server_search_query(const MessageQuerySpec &query, Text &error) {
    error.clear();
    if (query.scope != MessageQueryScope::Folder || !query.folder.account_id ||
        query.folder.server_name.empty() || query.folder.server_name.size() > 4096 ||
        !valid_utf8(query.folder.server_name.c_str(), query.folder.server_name.size()) ||
        memchr(query.folder.server_name.c_str(), 0, query.folder.server_name.size()) ||
        query.account_id != query.folder.account_id) {
        error.set("Server search requires one selected folder.");
        return false;
    }
    if ((query.sort != MessageQuerySort::Newest && query.sort != MessageQuerySort::Oldest) ||
        query.required_flags || query.excluded_flags || query.has_received_after ||
        query.has_received_before || query.attachment_filter != AttachmentQueryFilter::Any) {
        error.set("Received-date, unread, starred, sender/subject sorting, and attachment filters are local-only.");
        return false;
    }
    if (static_cast<uint8_t>(query.search_field) > static_cast<uint8_t>(MessageQueryField::MessageId) ||
        !query.limit || query.limit > MaxRowsPerPage || query.offset > static_cast<uint64_t>(INT64_MAX) ||
        query.query.size() > 1024 || !valid_utf8(query.query.c_str(), query.query.size())) {
        error.set("The server-search query is invalid or exceeds its limit.");
        return false;
    }
    return true;
}

bool task6_selection_result_is_current(uint64_t request_selection_generation,
    uint64_t current_selection_generation) {
    return !request_selection_generation || request_selection_generation == current_selection_generation;
}

Task6StartupQueryAction task6_startup_query_action(Task6StartupVaultCheck check) {
    return check == Task6StartupVaultCheck::Pending ?
        Task6StartupQueryAction::WaitForVaultCheck : Task6StartupQueryAction::SubmitQueryNow;
}

bool task6_extension_allows_application_close(Task6ApplicationCloseGuard guard,
    void *context, Text &reason) {
    reason.clear();
    if (!guard) return true;
    if (!guard(context, reason)) return false;
    reason.clear();
    return true;
}

bool task6_apply_reader_result_to_row(const WorkerReaderResult &reader,
    MessageQueryRow &row) {
    if (reader.key != row.message.key) return false;
    row.message.headers_only = reader.headers_only;
    if (!reader.subject.empty()) row.message.subject = reader.subject;
    if (!reader.from.empty()) row.message.from = reader.from;
    if (!reader.headers_only) {
        row.attachment_presence_known = true;
        row.has_attachments = !reader.attachments.empty();
    }
    return true;
}

bool task6_apply_folder_sync_identity(WorkerStartupSnapshot &startup,
    Folder &selected_folder, bool selected_folder_valid, const Folder &requested_folder,
    uint64_t requested_account_generation, uint64_t result_account_generation,
    const MessageQueryPage &page, Text &error) {
    error.clear();
    if (!requested_account_generation || requested_account_generation != result_account_generation ||
        !requested_folder.key.account_id || requested_folder.key.server_name.empty() ||
        !requested_folder.generation || !requested_folder.selectable ||
        page.query.scope != MessageQueryScope::Folder ||
        page.query.account_id != requested_folder.key.account_id ||
        page.query.folder != requested_folder.key ||
        page.folder_generation != requested_folder.generation || !page.folder_uidvalidity ||
        !selected_folder_valid || selected_folder.key != requested_folder.key ||
        selected_folder.generation != requested_folder.generation) {
        error.set("header sync result does not match the selected folder identity");
        return false;
    }

    bool account_matches = false;
    for (size_t i = 0; i < startup.accounts.size(); ++i) {
        if (startup.accounts[i].id == requested_folder.key.account_id &&
            startup.accounts[i].generation == requested_account_generation) {
            account_matches = true;
            break;
        }
    }
    if (!account_matches) {
        error.set("account configuration changed before the folder identity could be refreshed");
        return false;
    }

    size_t folder_index = startup.folders.size();
    for (size_t i = 0; i < startup.folders.size(); ++i) {
        if (startup.folders[i].key == requested_folder.key) {
            folder_index = i;
            break;
        }
    }
    if (folder_index == startup.folders.size() ||
        startup.folders[folder_index].generation != requested_folder.generation ||
        !startup.folders[folder_index].selectable) {
        error.set("folder listing changed before its selected identity could be refreshed");
        return false;
    }
    for (size_t i = 0; i < page.rows.size(); ++i) {
        const MessageQueryRow &row = page.rows[i];
        if (row.message.key.folder != requested_folder.key ||
            row.message.key.uidvalidity != page.folder_uidvalidity ||
            row.account_generation != requested_account_generation ||
            row.folder_generation != requested_folder.generation) {
            error.set("header sync rows do not match the selected folder identity");
            return false;
        }
    }

    startup.folders[folder_index].uidvalidity = page.folder_uidvalidity;
    selected_folder.uidvalidity = page.folder_uidvalidity;
    error.clear();
    return true;
}

bool task6_account_secret_identity_unchanged(const Account &before,
    const Account &after, CredentialPurpose purpose) {
    if (!before.id || before.id != after.id || !before.generation || !after.generation ||
        before.email_address != after.email_address ||
        before.authentication != after.authentication || before.custom_ca_file != after.custom_ca_file)
        return false;
    if (purpose == CredentialPurpose::Imap) {
        return before.username == after.username && before.incoming_host == after.incoming_host &&
            before.incoming_port == after.incoming_port &&
            before.incoming_protocol == after.incoming_protocol &&
            before.incoming_security == after.incoming_security;
    }
    if (purpose == CredentialPurpose::Smtp) {
        const Text &before_username = before.smtp_username.empty() ? before.username : before.smtp_username;
        const Text &after_username = after.smtp_username.empty() ? after.username : after.smtp_username;
        return before_username == after_username && before.outgoing_host == after.outgoing_host &&
            before.outgoing_port == after.outgoing_port && before.outgoing_security == after.outgoing_security;
    }
    return false;
}

bool task6_validate_account_form(const Account &form, Text &error) {
    if (form.id) return validate_account(form, error);
    Account validation_copy = form;
    validation_copy.id = 1; // Validation only; the AccountCreate payload remains ID 0.
    return validate_account(validation_copy, error);
}

}
