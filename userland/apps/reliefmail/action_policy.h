#ifndef RELIEFMAIL_ACTION_POLICY_H
#define RELIEFMAIL_ACTION_POLICY_H

#include "worker.h"

namespace rmail {

inline bool task8a_archive_operation_reviewable(const SendOperationSummary &operation) {
    if (!operation.id || !operation.account_id || !operation.account_generation ||
        operation.delivery_count > 1) return false;
    switch (operation.stage) {
    case SendStage::Accepted:
    case SendStage::Sent:
    case SendStage::ArchivePending:
    case SendStage::ArchiveFailed:
    case SendStage::ReviewedDelivered:
        return true;
    default:
        return false;
    }
}

inline bool task8a_archive_review_target_valid(const SendOperationSummary &operation,
    uint64_t account_id, const Folder *target) {
    return task8a_archive_operation_reviewable(operation) && operation.account_id == account_id &&
        target && target->selectable && target->generation && target->uidvalidity &&
        target->key.account_id == account_id;
}

inline bool task8a_archive_retryable(const SendOperationSummary &operation,
    uint64_t account_id, uint64_t account_generation) {
    if (!task8a_archive_operation_reviewable(operation) || operation.account_id != account_id ||
        !account_generation || operation.archive_review_generation != account_generation ||
        operation.archive_folder.empty() || !operation.archive_folder_generation ||
        !operation.archive_folder_uidvalidity) return false;
    return true;
}

inline bool task8a_archive_retry_completed(WorkerStatus status, SendStage stage) {
    return status == WorkerStatus::Ok && stage == SendStage::Archived;
}

inline const char *task8a_action_refresh_status(const Text &action_diagnostic,
    const Text &refresh_status) {
    return action_diagnostic.empty() ? refresh_status.c_str() : action_diagnostic.c_str();
}

/* This seam keeps the completion ordering for destination discovery
 * host-testable. FolderSelect refreshes the visible query; FolderList does
 * not. The controller supplies the identity checks and exact captured action. */
template <typename ContinueAction, typename RefreshView>
bool task8a_resume_folder_action(bool has_action_continuation, bool refresh_view,
    bool account_is_current, bool captured_context_is_current,
    ContinueAction continue_action, RefreshView refresh) {
    if (has_action_continuation) {
        /* Destination discovery is internal: resume the exact captured action
         * before a query can change the UI generation or checked rows. */
        continue_action();
        if (!account_is_current || !captured_context_is_current) return false;
    }
    if (refresh_view && account_is_current) refresh();
    return account_is_current && (!has_action_continuation || captured_context_is_current);
}

/* Validate a selection retained across asynchronous destination discovery.
 * The UI may have changed its visible selection while the worker refreshed a
 * folder list; only the captured identities remain eligible for continuation. */
inline bool task8a_retained_action_selection_valid(
    const BoundedArray<MessageQueryRow, MaxWorkerOutstanding> &rows,
    const FolderList &folders, uint64_t captured_profile_epoch,
    uint64_t captured_ui_generation, uint64_t current_profile_epoch,
    uint64_t current_ui_generation, uint64_t account_id,
    uint64_t account_generation, Text &error) {
    error.clear();
    auto reject = [&](const char *message) {
        error.set(message);
        return false;
    };
    if (!rows.size() || !captured_profile_epoch || !captured_ui_generation ||
        captured_profile_epoch != current_profile_epoch ||
        captured_ui_generation != current_ui_generation || !account_id || !account_generation)
        return reject("the original selection no longer matches the current profile or view");
    for (size_t i = 0; i < rows.size(); ++i) {
        const MessageQueryRow &row = rows[i];
        if (row.profile_epoch != captured_profile_epoch || row.ui_generation != captured_ui_generation ||
            row.account_generation != account_generation ||
            row.message.key.folder.account_id != account_id ||
            row.message.key.folder.server_name.empty() || !row.folder_generation ||
            !row.message.key.uidvalidity || !row.message.key.uid)
            return reject("the original message identity is incomplete or stale");
        const Folder *folder = nullptr;
        for (size_t j = 0; j < folders.size(); ++j)
            if (folders[j].key == row.message.key.folder) { folder = &folders[j]; break; }
        if (!folder || !folder->selectable || folder->generation != row.folder_generation ||
            folder->uidvalidity != row.message.key.uidvalidity)
            return reject("a source folder changed while its destination was being refreshed");
        for (size_t previous = 0; previous < i; ++previous)
            if (rows[previous].message.key == row.message.key)
                return reject("the original selection contains a duplicate server identity");
    }
    return true;
}

/* Build an all-or-nothing selection for a server mutation. Checked rows take
 * precedence over the focused row. Every identity must belong to the same
 * current profile/view/account generation before any request is submitted. */
inline bool task8a_collect_action_rows(
    const BoundedArray<MessageQueryRow, MaxRowsPerPage> &rows,
    const BoundedArray<uint32_t, MaxRowsPerPage> &checked_rows,
    const MessageQueryRow *focused_row, uint64_t profile_epoch,
    uint64_t ui_generation, uint64_t account_id, uint64_t account_generation,
    BoundedArray<MessageQueryRow, MaxWorkerOutstanding> &selected, Text &error) {
    error.clear();
    auto reject = [&](const char *message) {
        error.set(message);
        return false;
    };
    if (!profile_epoch || !ui_generation || !account_id || !account_generation)
        return reject("message action is missing its live profile or account identity");
    const size_t count = checked_rows.empty() ? (focused_row ? 1u : 0u) : checked_rows.size();
    if (!count) return reject("select at least one message");
    if (count > MaxWorkerOutstanding)
        return reject("select no more messages than the background worker can accept at once");

    BoundedArray<MessageQueryRow, MaxWorkerOutstanding> next;
    for (size_t item = 0; item < count; ++item) {
        const MessageQueryRow *row = focused_row;
        if (!checked_rows.empty()) {
            const uint32_t index = checked_rows[item];
            if (index >= rows.size()) return reject("the checked message selection is stale");
            row = &rows[index];
            for (size_t previous = 0; previous < item; ++previous)
                if (checked_rows[previous] == index)
                    return reject("the checked message selection contains a duplicate row");
        }
        if (!row || row->profile_epoch != profile_epoch || row->ui_generation != ui_generation ||
            row->account_generation != account_generation || row->folder_generation == 0 ||
            row->message.key.folder.account_id != account_id ||
            row->message.key.folder.server_name.empty() || !row->message.key.uidvalidity ||
            !row->message.key.uid)
            return reject("a selected message no longer matches the current profile, account, folder, or UID identity");
        for (size_t previous = 0; previous < next.size(); ++previous)
            if (next[previous].message.key == row->message.key)
                return reject("the selected messages contain a duplicate server identity");
        if (!next.push(*row)) return reject("the selected message batch exceeds its safe bound");
    }
    selected = static_cast<BoundedArray<MessageQueryRow, MaxWorkerOutstanding> &&>(next);
    return true;
}

}

#endif
