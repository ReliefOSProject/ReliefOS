#include "action_policy.h"
#include <assert.h>
#include <stdio.h>

using namespace rmail;

namespace rmail {
bool task8a_archive_retry_completed(WorkerStatus status, SendStage stage);
bool task8a_retained_action_selection_valid(
    const BoundedArray<MessageQueryRow, MaxWorkerOutstanding> &rows,
    const FolderList &folders, uint64_t captured_profile_epoch,
    uint64_t captured_ui_generation, uint64_t current_profile_epoch,
    uint64_t current_ui_generation, uint64_t account_id,
    uint64_t account_generation, Text &error);
const char *task8a_action_refresh_status(const Text &action_diagnostic,
    const Text &refresh_status);
}

namespace {
MessageQueryRow row(uint64_t account, uint64_t account_generation, uint64_t folder_generation,
    uint64_t epoch, uint64_t view, const char *folder, uint64_t uidvalidity, uint32_t uid) {
    MessageQueryRow value;
    value.message.key.folder.account_id = account;
    assert(value.message.key.folder.server_name.set(folder));
    value.message.key.uidvalidity = uidvalidity;
    value.message.key.uid = uid;
    value.account_generation = account_generation;
    value.folder_generation = folder_generation;
    value.profile_epoch = epoch;
    value.ui_generation = view;
    return value;
}

void test_selection_uses_checked_rows_and_exact_live_identity() {
    BoundedArray<MessageQueryRow, MaxRowsPerPage> rows;
    assert(rows.push(row(7, 3, 4, 8, 11, "INBOX", 19, 21)));
    assert(rows.push(row(7, 3, 4, 8, 11, "INBOX", 19, 22)));
    BoundedArray<uint32_t, MaxRowsPerPage> checked;
    assert(checked.push(1));
    assert(checked.push(0));
    BoundedArray<MessageQueryRow, MaxWorkerOutstanding> selected;
    Text error;
    assert(task8a_collect_action_rows(rows, checked, nullptr, 8, 11, 7, 3, selected, error));
    assert(selected.size() == 2);
    assert(selected[0].message.key.uid == 22 && selected[1].message.key.uid == 21);

    checked.clear();
    assert(task8a_collect_action_rows(rows, checked, &rows[1], 8, 11, 7, 3, selected, error));
    assert(selected.size() == 1 && selected[0].message.key.uid == 22);
}

void test_selection_rejects_stale_cross_account_and_incomplete_rows_atomically() {
    BoundedArray<MessageQueryRow, MaxRowsPerPage> rows;
    assert(rows.push(row(7, 3, 4, 8, 11, "INBOX", 19, 21)));
    assert(rows.push(row(8, 3, 4, 8, 11, "INBOX", 19, 22)));
    BoundedArray<uint32_t, MaxRowsPerPage> checked;
    assert(checked.push(0));
    assert(checked.push(1));
    BoundedArray<MessageQueryRow, MaxWorkerOutstanding> selected;
    MessageQueryRow sentinel = row(99, 1, 1, 1, 1, "sentinel", 1, 1);
    assert(selected.push(sentinel));
    Text error;
    assert(!task8a_collect_action_rows(rows, checked, nullptr, 8, 11, 7, 3, selected, error));
    assert(selected.size() == 1 && selected[0].message.key == sentinel.message.key);
    assert(!error.empty());

    checked.clear();
    assert(checked.push(0));
    assert(!task8a_collect_action_rows(rows, checked, nullptr, 8, 12, 7, 3, selected, error));
    assert(selected.size() == 1);
    assert(!task8a_collect_action_rows(rows, checked, nullptr, 9, 11, 7, 3, selected, error));
    assert(selected.size() == 1);

    checked.clear();
    MessageQueryRow incomplete = row(7, 3, 0, 8, 11, "INBOX", 19, 21);
    assert(!task8a_collect_action_rows(rows, checked, &incomplete, 8, 11, 7, 3, selected, error));
    assert(selected.size() == 1);
}

void test_selection_rejects_bad_checked_indices_and_empty_identity() {
    BoundedArray<MessageQueryRow, MaxRowsPerPage> rows;
    assert(rows.push(row(7, 3, 4, 8, 11, "INBOX", 19, 21)));
    BoundedArray<uint32_t, MaxRowsPerPage> checked;
    assert(checked.push(4));
    BoundedArray<MessageQueryRow, MaxWorkerOutstanding> selected;
    Text error;
    assert(!task8a_collect_action_rows(rows, checked, nullptr, 8, 11, 7, 3, selected, error));
    assert(selected.empty());
    checked.clear();
    MessageQueryRow missing = row(7, 3, 4, 8, 11, "", 19, 21);
    assert(!task8a_collect_action_rows(rows, checked, &missing, 8, 11, 7, 3, selected, error));
}

void test_accepted_archive_from_old_account_generation_requires_review_before_imap_retry() {
    SendOperationSummary operation;
    operation.id = 1;
    operation.account_id = 82;
    operation.account_generation = 1;
    operation.stage = SendStage::ArchivePending;
    operation.delivery_count = 1;
    Folder target;
    target.key.account_id = 82;
    assert(target.key.server_name.set("Sent"));
    target.generation = 7;
    target.uidvalidity = 55;
    target.selectable = true;

    /* rfix09 retained operation1 at source generation1 while Alice is now
     * generation22. Review the current exact target before archive-only retry. */
    assert(task8a_archive_review_target_valid(operation, 82, &target));
    assert(!task8a_archive_retryable(operation, 82, 22));
    assert(!task8a_archive_review_target_valid(operation, 83, &target));
    target.selectable = false;
    assert(!task8a_archive_review_target_valid(operation, 82, &target));
    target.selectable = true;

    operation.stage = SendStage::ArchiveFailed;
    assert(task8a_archive_review_target_valid(operation, 82, &target));
    operation.archive_folder = target.key.server_name;
    operation.archive_review_generation = 22;
    operation.archive_folder_generation = target.generation;
    operation.archive_folder_uidvalidity = target.uidvalidity;
    assert(task8a_archive_retryable(operation, 82, 22));
    assert(!task8a_archive_retryable(operation, 82, 23));

    operation.stage = SendStage::Archived;
    assert(!task8a_archive_review_target_valid(operation, 82, &target));
    assert(!task8a_archive_retryable(operation, 82, 22));
    operation.stage = SendStage::Uncertain;
    operation.archive_folder.clear();
    operation.archive_review_generation = 0;
    operation.archive_folder_generation = 0;
    operation.archive_folder_uidvalidity = 0;
    assert(!task8a_archive_review_target_valid(operation, 82, &target));
}

void test_archive_retry_completion_requires_an_archived_result() {
    assert(!task8a_archive_retry_completed(WorkerStatus::CommittedWarning, SendStage::ArchiveFailed));
    assert(!task8a_archive_retry_completed(WorkerStatus::Ok, SendStage::ArchiveFailed));
    assert(!task8a_archive_retry_completed(WorkerStatus::CommittedWarning, SendStage::Archived));
    assert(task8a_archive_retry_completed(WorkerStatus::Ok, SendStage::Archived));
}

void test_action_failure_remains_visible_after_followup_refresh() {
    Text action_diagnostic;
    Text refresh_status;
    assert(action_diagnostic.set("message UID identity is stale or inconsistent"));
    assert(refresh_status.set("3 messages"));
    assert(strcmp(task8a_action_refresh_status(action_diagnostic, refresh_status),
        "message UID identity is stale or inconsistent") == 0);

    assert(refresh_status.set("folder refresh failed"));
    assert(strcmp(task8a_action_refresh_status(action_diagnostic, refresh_status),
        "message UID identity is stale or inconsistent") == 0);

    action_diagnostic.clear();
    assert(strcmp(task8a_action_refresh_status(action_diagnostic, refresh_status),
        "folder refresh failed") == 0);
}

void test_async_folder_continuation_keeps_and_revalidates_the_original_exact_selection() {
    BoundedArray<MessageQueryRow, MaxRowsPerPage> visible_rows;
    assert(visible_rows.push(row(7, 3, 4, 8, 11, "INBOX", 19, 21)));
    assert(visible_rows.push(row(7, 3, 4, 8, 11, "INBOX", 19, 22)));
    BoundedArray<uint32_t, MaxRowsPerPage> checked_rows;
    assert(checked_rows.push(0));
    assert(checked_rows.push(1));
    BoundedArray<MessageQueryRow, MaxWorkerOutstanding> captured_rows;
    Text error;
    assert(task8a_collect_action_rows(visible_rows, checked_rows, nullptr,
        8, 11, 7, 3, captured_rows, error));
    BoundedArray<MessageQueryRow, MaxWorkerOutstanding> retained;
    assert(retained.clone_from(captured_rows));
    const MessageQueryRow original_first = retained[0];
    const MessageQueryRow original_second = retained[1];

    /* A FolderList/FolderSelect continuation may clear or replace the UI's
     * current selection. The captured set remains the operation authority. */
    BoundedArray<MessageQueryRow, MaxWorkerOutstanding> current_selection;
    assert(current_selection.push(row(7, 3, 4, 8, 11, "INBOX", 19, 99)));
    FolderList current_folders;
    Folder inbox;
    inbox.key.account_id = 7;
    assert(inbox.key.server_name.set("INBOX"));
    inbox.generation = 4;
    inbox.uidvalidity = 19;
    inbox.selectable = true;
    assert(current_folders.push(inbox));
    assert(task8a_retained_action_selection_valid(retained, current_folders,
        8, 11, 8, 11, 7, 3, error));
    assert(retained.size() == 2);
    assert(retained[0].message.key.uid == original_first.message.key.uid);
    assert(retained[1].message.key.uid == original_second.message.key.uid);
    assert(current_selection.size() == 1 && current_selection[0].message.key.uid == 99);

    current_folders[0].generation = 5;
    assert(!task8a_retained_action_selection_valid(retained, current_folders,
        8, 11, 8, 11, 7, 3, error));
    current_folders[0].generation = 4;
    current_folders[0].uidvalidity = 20;
    assert(!task8a_retained_action_selection_valid(retained, current_folders,
        8, 11, 8, 11, 7, 3, error));
    current_folders[0].uidvalidity = 19;
    current_folders[0].selectable = false;
    assert(!task8a_retained_action_selection_valid(retained, current_folders,
        8, 11, 8, 11, 7, 3, error));
    current_folders[0].selectable = true;
    assert(!task8a_retained_action_selection_valid(retained, current_folders,
        8, 11, 9, 11, 7, 3, error));
    assert(!task8a_retained_action_selection_valid(retained, current_folders,
        8, 11, 8, 12, 7, 3, error));
    assert(!task8a_retained_action_selection_valid(retained, current_folders,
        8, 11, 8, 11, 7, 4, error));
}

void test_folder_discovery_continues_exact_selection_before_select_query() {
    BoundedArray<MessageQueryRow, MaxWorkerOutstanding> retained;
    assert(retained.push(row(7, 3, 4, 8, 11, "INBOX", 19, 21)));
    assert(retained.push(row(7, 3, 4, 8, 11, "INBOX", 19, 22)));
    FolderList folders;
    Folder inbox;
    inbox.key.account_id = 7;
    assert(inbox.key.server_name.set("INBOX"));
    inbox.generation = 4;
    inbox.uidvalidity = 19;
    inbox.selectable = true;
    assert(folders.push(inbox));

    for (int select_path = 0; select_path != 2; ++select_path) {
        uint64_t current_view = 11;
        BoundedArray<uint32_t, MaxRowsPerPage> checked;
        assert(checked.push(0));
        assert(checked.push(1));
        BoundedArray<MessageQueryRow, MaxWorkerOutstanding> submitted;
        bool continuation_valid = false;
        bool query_refreshed = false;
        int order = 0;
        Text error;
        auto continue_action = [&]() {
            assert(order == 0);
            order = 1;
            continuation_valid = task8a_retained_action_selection_valid(retained, folders,
                8, 11, 8, current_view, 7, 3, error);
            if (continuation_valid) assert(submitted.clone_from(retained));
        };
        auto refresh_query = [&]() {
            assert(order == 1);
            order = 2;
            query_refreshed = true;
            ++current_view;
            checked.clear();
        };

        assert(task8a_resume_folder_action(true, select_path != 0, true, true,
            continue_action, refresh_query));
        assert(order == (select_path ? 2 : 1));
        assert(continuation_valid);
        assert(submitted.size() == 2);
        assert(submitted[0].message.key.uid == 21 && submitted[1].message.key.uid == 22);
        assert(query_refreshed == (select_path != 0));
        assert(checked.empty() == (select_path != 0));
    }
}

void test_folder_continuation_cancels_real_view_or_source_changes_with_visible_reason() {
    BoundedArray<MessageQueryRow, MaxWorkerOutstanding> retained;
    assert(retained.push(row(7, 3, 4, 8, 11, "INBOX", 19, 21)));
    assert(retained.push(row(7, 3, 4, 8, 11, "INBOX", 19, 22)));
    FolderList folders;
    Folder inbox;
    inbox.key.account_id = 7;
    assert(inbox.key.server_name.set("INBOX"));
    inbox.generation = 4;
    inbox.uidvalidity = 19;
    inbox.selectable = true;
    assert(folders.push(inbox));

    uint64_t current_view = 12; // the user changed the view while FolderSelect was pending
    bool query_refreshed = false;
    bool continuation_valid = true;
    Text error, action_diagnostic, query_status;
    auto continue_action = [&]() {
        continuation_valid = task8a_retained_action_selection_valid(retained, folders,
            8, 11, 8, current_view, 7, 3, error);
        if (!continuation_valid)
            assert(action_diagnostic.set("The original message selection changed while refreshing the destination."));
    };
    auto refresh_query = [&]() { query_refreshed = true; };
    assert(!task8a_resume_folder_action(true, true, true, false,
        continue_action, refresh_query));
    assert(!continuation_valid && !query_refreshed);
    assert(query_status.set("3 messages"));
    assert(strcmp(task8a_action_refresh_status(action_diagnostic, query_status),
        "The original message selection changed while refreshing the destination.") == 0);

    current_view = 11;
    folders[0].generation = 5;
    continuation_valid = true;
    assert(task8a_resume_folder_action(true, true, true, true, [&]() {
        continuation_valid = task8a_retained_action_selection_valid(retained, folders,
            8, 11, 8, current_view, 7, 3, error);
        if (!continuation_valid) action_diagnostic.set("source folder generation changed");
    }, refresh_query));
    assert(!continuation_valid);
    assert(query_refreshed);
    assert(strcmp(task8a_action_refresh_status(action_diagnostic, query_status),
        "source folder generation changed") == 0);
    folders[0].generation = 4;
    folders[0].uidvalidity = 20;
    assert(!task8a_retained_action_selection_valid(retained, folders,
        8, 11, 8, current_view, 7, 3, error));
}
}

int main() {
    test_selection_uses_checked_rows_and_exact_live_identity();
    test_selection_rejects_stale_cross_account_and_incomplete_rows_atomically();
    test_selection_rejects_bad_checked_indices_and_empty_identity();
    test_accepted_archive_from_old_account_generation_requires_review_before_imap_retry();
    test_archive_retry_completion_requires_an_archived_result();
    test_action_failure_remains_visible_after_followup_refresh();
    test_async_folder_continuation_keeps_and_revalidates_the_original_exact_selection();
    test_folder_discovery_continues_exact_selection_before_select_query();
    test_folder_continuation_cancels_real_view_or_source_changes_with_visible_reason();
    puts("ReliefMail message-action identity policy suite passed");
}
