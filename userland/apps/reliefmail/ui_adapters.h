#ifndef RELIEFMAIL_UI_ADAPTERS_H
#define RELIEFMAIL_UI_ADAPTERS_H

#include "tasks.h"

namespace rmail {

struct Task6PendingRequest {
    uint64_t request_id = 0;
    WorkerTaskKind kind = WorkerTaskKind::Initialize;
    uint64_t profile_epoch = 0;
    uint64_t ui_generation = 0;
};

enum class Task6ResultDisposition : uint8_t { Ignore, Apply, ProfileTransition };

enum class Task6StartupVaultCheck : uint8_t { Pending, Complete, NotStarted };
enum class Task6StartupQueryAction : uint8_t { WaitForVaultCheck, SubmitQueryNow };
using Task6ApplicationCloseGuard = bool (*)(void *context, Text &reason);

/* The initial local query must not cross the coordinator's maintenance gate. */
Task6StartupQueryAction task6_startup_query_action(Task6StartupVaultCheck check);

/* Optional Task7/Task8 windows can veto application shutdown with a user-facing
 * reason while they resolve their own save/discard state. */
bool task6_extension_allows_application_close(Task6ApplicationCloseGuard guard,
    void *context, Text &reason);

/* Copies row-captured generations and identity into a guarded worker request.
 * It never consults a fresh account/folder snapshot to rewrite the row. */
bool task6_build_message_request(const MessageQueryRow &row, uint64_t request_id,
    WorkerTaskKind kind, uint64_t current_profile_epoch, uint64_t current_ui_generation,
    WorkerRequest &request, Text &error);

/* A matching restore/recovery control result is recognized before current-epoch
 * and view-generation filtering; ordinary data still requires both to match. */
Task6ResultDisposition task6_classify_result(const Task6PendingRequest &pending,
    const WorkerResult &result, uint64_t current_profile_epoch,
    uint64_t current_ui_generation);

/* Fl_Help_View stores link targets in a 192-byte buffer and supports file:.
 * Only bounded web URLs are eligible for explicit, user-confirmed opening. */
bool task6_is_safe_web_link(const char *target, size_t length);

/* FLTK 1.3 fl_open_uri returns 1 on success and 0 on failure. */
bool task6_uri_handoff_succeeded(int fltk_result);

/* Converts untrusted display bytes to bounded-widget-safe UTF-8 without
 * changing the raw source retained for export. Invalid UTF-8 and controls
 * become visible/reviewable replacement characters or spaces. */
bool task6_sanitize_display_text(const char *data, size_t size, Text &display);
/* File names and other one-line metadata must not retain line breaks/tabs. */
bool task6_sanitize_single_line_display_text(const char *data, size_t size, Text &display);

/* Escape FLTK's label-symbol marker in sanitized menu text so literal mail
 * filenames such as @B or @C1 remain visible in native choices. */
bool task6_escape_fltk_label_symbols(const char *data, size_t size, Text &escaped);

/* Received-date filters use inclusive UTC calendar days against IMAP
 * INTERNALDATE. Empty values are handled by the caller as unbounded. */
bool task6_parse_received_date(const char *text, int64_t &unix_seconds, Text &error);

/* Server search is available only when the backend can represent the whole
 * query; local-only date/flag/attachment filters stay in the Store query. */
bool task6_validate_server_search_query(const MessageQuerySpec &query, Text &error);

/* Query results have no selection token (zero); reader results carry the
 * token captured when their message was selected. */
bool task6_selection_result_is_current(uint64_t request_selection_generation,
    uint64_t current_selection_generation);

/* Merge a parsed/fetched reader DTO into the row that initiated it. The key
 * must still match; attachment absence is only known after a complete body. */
bool task6_apply_reader_result_to_row(const WorkerReaderResult &reader,
    MessageQueryRow &row);

/* Applies UIDVALIDITY learned by a successful guarded HeaderSync only to the
 * matching account/folder generations. Returned rows keep their captured IDs. */
bool task6_apply_folder_sync_identity(WorkerStartupSnapshot &startup,
    Folder &selected_folder, bool selected_folder_valid, const Folder &requested_folder,
    uint64_t requested_account_generation, uint64_t result_account_generation,
    const MessageQueryPage &page, Text &error);

/* Reuse a session credential across an account edit only when the same
 * account's authentication identity for this transport is unchanged. */
bool task6_account_secret_identity_unchanged(const Account &before,
    const Account &after, CredentialPurpose purpose);

/* AccountCreate assigns its durable ID in the worker. Validate a new dialog's
 * fields on a private copy so the model/Store still reject an unassigned ID. */
bool task6_validate_account_form(const Account &form, Text &error);

}

#endif
