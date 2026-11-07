#include "compose_state.h"
#include <stdlib.h>
#include <string.h>

namespace rmail {
namespace {

bool set_error(Text &error, const char *message) {
    if (!error.set(message)) error.set("out of memory");
    return false;
}

}

int compose_clipboard_buffer(ComposeClipboardOperation operation) {
    switch (operation) {
    case ComposeClipboardOperation::MouseSelectionCopy:
    case ComposeClipboardOperation::MiddleMousePaste:
        return 0;
    case ComposeClipboardOperation::KeyboardCopy:
    case ComposeClipboardOperation::KeyboardCut:
    case ComposeClipboardOperation::KeyboardPaste:
        return 1;
    }
    return -1;
}

bool compose_confirmation_accepted(int result) {
    return result == 1;
}

void compose_queue_send_preflight_failed(ComposeQueueHandoffState &state) {
    state.controls_enabled = true;
    state.send_after_save = false;
    state.send_in_progress = false;
}

bool compose_account_choice_record(ComposeAccountChoiceMap &choices, int menu_index,
    uint64_t account_id, uint64_t account_generation) {
    if (menu_index < 0 || !account_id || !account_generation) return false;
    for (size_t i = 0; i < choices.size(); ++i) {
        if (choices[i].menu_index == menu_index || choices[i].account_id == account_id)
            return false;
    }
    ComposeAccountChoice choice;
    choice.menu_index = menu_index;
    choice.account_id = account_id;
    choice.account_generation = account_generation;
    return choices.push(choice);
}

bool compose_account_choice_identity(const ComposeAccountChoiceMap &choices, int menu_index,
    uint64_t &account_id, uint64_t &account_generation) {
    if (menu_index < 0) return false;
    for (size_t i = 0; i < choices.size(); ++i) {
        if (choices[i].menu_index != menu_index) continue;
        if (!choices[i].account_id || !choices[i].account_generation) return false;
        account_id = choices[i].account_id;
        account_generation = choices[i].account_generation;
        return true;
    }
    return false;
}

bool compose_account_choice_live_index(const ComposeAccountChoiceMap &choices, int menu_index,
    const AccountList &live_accounts, size_t &account_index) {
    uint64_t account_id = 0;
    uint64_t account_generation = 0;
    if (!compose_account_choice_identity(choices, menu_index, account_id, account_generation))
        return false;
    size_t match = SIZE_MAX;
    for (size_t i = 0; i < live_accounts.size(); ++i) {
        if (live_accounts[i].id != account_id) continue;
        if (live_accounts[i].generation != account_generation || match != SIZE_MAX)
            return false;
        match = i;
    }
    if (match == SIZE_MAX) return false;
    account_index = match;
    return true;
}

bool compose_account_choice_menu_label(const char *data, size_t size, Text &label) {
    if ((!data && size) || size > 8192 || !valid_utf8(data, size) ||
        (size && memchr(data, 0, size))) return false;
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
    label = static_cast<Text &&>(next);
    return true;
}

bool compose_request_paste(void *target, ComposeClipboardOperation operation,
    ComposePasteRequester request, void *context) {
    const int buffer = compose_clipboard_buffer(operation);
    if (!target || !request || buffer < 0 ||
        (operation != ComposeClipboardOperation::MiddleMousePaste &&
         operation != ComposeClipboardOperation::KeyboardPaste)) return false;
    request(context, target, buffer);
    return true;
}

bool compose_confirmation_text(const Account &account, const Draft &draft,
    size_t attachment_count, Text &confirmation, Text &error) {
    error.clear();
    constexpr size_t MaxComposeConfirmationBytes = 6u * MaxEditableTextBytes + 8192u;
    if (!account.id || draft.account_id != account.id || !draft.raw_recipient_input ||
        attachment_count > MaxMimeParts)
        return set_error(error, "compose confirmation is missing its account or raw recipient identity");
    const Text *fields[] = {&account.display_name, &account.email_address, &draft.to_input,
        &draft.cc_input, &draft.bcc_input, &draft.subject};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        if (fields[i]->size() > MaxEditableTextBytes ||
            !valid_utf8(fields[i]->c_str(), fields[i]->size()) ||
            (fields[i]->size() && memchr(fields[i]->c_str(), 0, fields[i]->size())))
            return set_error(error, "compose confirmation text is invalid or exceeds its limit");
    }

    Array<char> output;
    auto append = [&](const char *bytes, size_t length) {
        if (length > MaxComposeConfirmationBytes - output.size()) return false;
        for (size_t i = 0; i < length; ++i)
            if (!output.push(bytes[i])) return false;
        return true;
    };
    auto append_literal = [&](const char *value) { return append(value, strlen(value)); };
    auto append_text = [&](const Text &value) { return append(value.c_str(), value.size()); };
    char count[3 * sizeof(size_t) + 1];
    size_t count_size = 0;
    size_t remaining = attachment_count;
    do {
        count[count_size++] = static_cast<char>('0' + remaining % 10);
        remaining /= 10;
    } while (remaining && count_size < sizeof(count));
    for (size_t i = 0; i < count_size / 2; ++i) {
        const char swap = count[i];
        count[i] = count[count_size - i - 1];
        count[count_size - i - 1] = swap;
    }
    if (!append_literal("From: ") || !append_text(account.display_name) ||
        !append_literal(" <") || !append_text(account.email_address) ||
        !append_literal(">\nTo: ") || !append_text(draft.to_input) ||
        !append_literal("\nCc: ") || !append_text(draft.cc_input) ||
        !append_literal("\nBcc: ") || !append_text(draft.bcc_input) ||
        !append_literal("\nSubject: ") || !append_text(draft.subject) ||
        !append_literal("\nAttachments: ") || !append(count, count_size))
        return set_error(error, "compose confirmation exceeds its bounded display limit");
    Text next;
    if (!next.set(output.data(), output.size()))
        return set_error(error, "could not retain the complete compose confirmation");
    confirmation = static_cast<Text &&>(next);
    return true;
}

void ComposeSaveGate::reset_for_open(const Draft &) {
    edit_generation_ = 1;
    saved_generation_ = 1;
    pending_serial_ = 0;
    pending_ = ComposeSaveToken();
}

bool ComposeSaveGate::note_edit() {
    if (edit_generation_ == UINT64_MAX) return false;
    ++edit_generation_;
    return true;
}

bool ComposeSaveGate::begin(const Draft &draft, uint64_t account_generation,
    const WorkerDraftAttachmentRefs &attachments, WorkerTaskInput &input,
    ComposeSaveToken &token, Text &error, WorkerTaskKind create_kind,
    const MessageKey *source_message) {
    error.clear();
    if (pending()) return set_error(error, "a draft save is already in progress");
    if (!dirty()) return set_error(error, "draft has no unsaved edits");
    if (!draft.account_id || !account_generation ||
        (draft.id && !draft.revision) || (!draft.id && draft.revision) ||
        attachments.size() > MaxMimeParts ||
        (!draft.id && create_kind != WorkerTaskKind::SaveDraft &&
            create_kind != WorkerTaskKind::SaveForwardDraft) ||
        (draft.id && (create_kind != WorkerTaskKind::SaveDraft || source_message)) ||
        (create_kind == WorkerTaskKind::SaveForwardDraft &&
            (!source_message || source_message->folder.account_id != draft.account_id ||
             source_message->folder.server_name.empty() || !source_message->uidvalidity ||
             !source_message->uid)) ||
        (create_kind != WorkerTaskKind::SaveForwardDraft && source_message))
        return set_error(error, "draft account, revision, or attachment identity is invalid");
    if (!next_serial_ || next_serial_ == UINT64_MAX)
        return set_error(error, "draft save request identity reached its limit");

    WorkerTaskInput next;
    next.item_id = draft.id;
    next.item_revision = draft.revision;
    if (source_message) next.message_key = *source_message;
    next.has_draft = true;
    Text clone_error;
    if (!clone_draft(draft, next.draft, clone_error))
        return set_error(error, clone_error.empty() ? "could not capture the draft for saving" : clone_error.c_str());
    /* Attachment sources are never serialized inside Draft. Each attachment
     * is represented by a bounded worker-side reference captured separately. */
    next.draft.attachments.clear();
    for (size_t i = 0; i < attachments.size(); ++i)
        if (!next.attachment_refs.push(attachments[i]))
            return set_error(error, "draft attachment references exceed their limit");

    ComposeSaveToken next_token;
    next_token.serial = next_serial_++;
    next_token.edit_generation = edit_generation_;
    next_token.account_id = draft.account_id;
    next_token.account_generation = account_generation;
    next_token.draft_id = draft.id;
    next_token.draft_revision = draft.revision;
    next_token.kind = draft.id ? WorkerTaskKind::SaveDraftRevision : create_kind;
    input = static_cast<WorkerTaskInput &&>(next);
    token = next_token;
    pending_ = next_token;
    pending_serial_ = next_token.serial;
    return true;
}

bool ComposeSaveGate::complete(const ComposeSaveToken &token, bool succeeded,
    const WorkerDraftSnapshot *saved, Draft &live_draft, bool &save_again, Text &error) {
    error.clear();
    save_again = false;
    if (!pending_serial_ || token.serial != pending_serial_ || token.serial != pending_.serial ||
        token.account_id != pending_.account_id || token.account_generation != pending_.account_generation ||
        token.edit_generation != pending_.edit_generation || token.kind != pending_.kind)
        return set_error(error, "stale draft save result was ignored");
    pending_serial_ = 0;
    pending_ = ComposeSaveToken();
    if (!succeeded) {
        save_again = dirty();
        return set_error(error, "draft save failed; local edits are still unsaved");
    }
    if (!saved || saved->account_generation != token.account_generation ||
        saved->draft.account_id != token.account_id || !saved->draft.id || !saved->draft.revision ||
        ((token.kind == WorkerTaskKind::SaveDraft || token.kind == WorkerTaskKind::SaveForwardDraft) &&
            token.draft_id) ||
        (token.kind != WorkerTaskKind::SaveDraft && token.kind != WorkerTaskKind::SaveForwardDraft &&
            token.kind != WorkerTaskKind::SaveDraftRevision) ||
        (token.kind == WorkerTaskKind::SaveDraftRevision &&
            (saved->draft.id != token.draft_id || saved->draft.revision <= token.draft_revision))) {
        save_again = dirty();
        return set_error(error, "saved draft identity does not match its request");
    }
    if (live_draft.account_id != token.account_id || live_draft.id != token.draft_id ||
        live_draft.revision != token.draft_revision) {
        save_again = dirty();
        return set_error(error, "draft identity changed while its save was in flight");
    }
    /* Merge only durable identity. The response body belongs to an older UI
     * generation and must never replace text typed after submission. */
    live_draft.id = saved->draft.id;
    live_draft.revision = saved->draft.revision;
    saved_generation_ = token.edit_generation;
    save_again = dirty();
    return true;
}

}
