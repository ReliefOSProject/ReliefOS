#ifndef RELIEFMAIL_COMPOSE_STATE_H
#define RELIEFMAIL_COMPOSE_STATE_H

#include "tasks.h"

namespace rmail {

enum class ComposeClipboardOperation : uint8_t {
    MouseSelectionCopy,
    MiddleMousePaste,
    KeyboardCopy,
    KeyboardCut,
    KeyboardPaste
};

/* FLTK buffer 0 is X11 PRIMARY; buffer 1 is the standard copy/paste clipboard. */
int compose_clipboard_buffer(ComposeClipboardOperation operation);
bool compose_confirmation_accepted(int result);
struct ComposeQueueHandoffState {
    uint64_t operation_id = 0;
    bool controls_enabled = false;
    bool send_after_save = false;
    bool send_in_progress = false;
};
void compose_queue_send_preflight_failed(ComposeQueueHandoffState &state);
struct ComposeAccountChoice {
    int menu_index = -1;
    uint64_t account_id = 0;
    uint64_t account_generation = 0;
};
using ComposeAccountChoiceMap = BoundedArray<ComposeAccountChoice, MaxAccounts>;
bool compose_account_choice_record(ComposeAccountChoiceMap &choices, int menu_index,
    uint64_t account_id, uint64_t account_generation);
bool compose_account_choice_identity(const ComposeAccountChoiceMap &choices, int menu_index,
    uint64_t &account_id, uint64_t &account_generation);
bool compose_account_choice_live_index(const ComposeAccountChoiceMap &choices, int menu_index,
    const AccountList &live_accounts, size_t &account_index);
bool compose_account_choice_menu_label(const char *data, size_t size, Text &label);
using ComposePasteRequester = void (*)(void *context, void *target, int clipboard_buffer);
bool compose_request_paste(void *target, ComposeClipboardOperation operation,
    ComposePasteRequester request, void *context);
bool compose_confirmation_text(const Account &account, const Draft &draft,
    size_t attachment_count, Text &confirmation, Text &error);

struct ComposeSaveToken {
    uint64_t serial = 0;
    uint64_t edit_generation = 0;
    uint64_t account_id = 0;
    uint64_t account_generation = 0;
    uint64_t draft_id = 0;
    uint64_t draft_revision = 0;
    WorkerTaskKind kind = WorkerTaskKind::SaveDraft;
};

/* Serializes autosaves and merges only durable identity/revision into the live
 * draft. Text and recipient edits made while a save is in flight stay local. */
class ComposeSaveGate {
    uint64_t edit_generation_ = 1;
    uint64_t saved_generation_ = 1;
    uint64_t next_serial_ = 1;
    uint64_t pending_serial_ = 0;
    ComposeSaveToken pending_;

public:
    void reset_for_open(const Draft &draft);
    bool note_edit();
    bool dirty() const { return edit_generation_ != saved_generation_; }
    bool pending() const { return pending_serial_ != 0; }
    uint64_t edit_generation() const { return edit_generation_; }

    bool begin(const Draft &draft, uint64_t account_generation,
        const WorkerDraftAttachmentRefs &attachments, WorkerTaskInput &input,
        ComposeSaveToken &token, Text &error,
        WorkerTaskKind create_kind = WorkerTaskKind::SaveDraft,
        const MessageKey *source_message = nullptr);
    bool complete(const ComposeSaveToken &token, bool succeeded,
        const WorkerDraftSnapshot *saved, Draft &live_draft,
        bool &save_again, Text &error);
};

}

#endif
