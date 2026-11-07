#include "editor.h"
#include "compose_state.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace rmail;

static bool contains(const Text &text, const char *needle);

static void test_compose_confirmation_uses_literal_confirmation_result_contract() {
    assert(!compose_confirmation_accepted(0));
    assert(compose_confirmation_accepted(1));
    assert(!compose_confirmation_accepted(-1));
}

static void test_queued_send_preflight_failure_restores_compose_controls_without_losing_ready_operation() {
    ComposeQueueHandoffState state;
    state.operation_id = 44;
    state.controls_enabled = false;
    state.send_after_save = true;
    state.send_in_progress = false;

    compose_queue_send_preflight_failed(state);

    assert(state.controls_enabled);
    assert(!state.send_after_save && !state.send_in_progress);
    assert(state.operation_id == 44);
}

static void test_sender_choices_use_actual_menu_indices_and_literal_labels() {
    ComposeAccountChoiceMap choices;
    assert(compose_account_choice_record(choices, 3, 82, 7));
    assert(compose_account_choice_record(choices, 8, 21, 42));
    uint64_t account_id = 0;
    uint64_t account_generation = 0;
    assert(compose_account_choice_identity(choices, 8, account_id, account_generation));
    assert(account_id == 21 && account_generation == 42);
    assert(compose_account_choice_identity(choices, 3, account_id, account_generation));
    assert(account_id == 82 && account_generation == 7);
    assert(!compose_account_choice_identity(choices, 4, account_id, account_generation));
    assert(!compose_account_choice_record(choices, 8, 55, 2));

    const char label[] = "Ops/Shared|Alice <alice+alerts@reliefmail.test> & Bob";
    Text menu_label;
    assert(compose_account_choice_menu_label(label, sizeof(label) - 1, menu_label));
    assert(menu_label == "Ops/Shared|Alice <alice+alerts@@reliefmail.test> & Bob");
}

static void test_sender_choices_revalidate_against_live_account_generations() {
    ComposeAccountChoiceMap choices;
    assert(compose_account_choice_record(choices, 7, 82, 7));

    AccountList live_accounts;
    Account alice;
    alice.id = 21;
    alice.generation = 5;
    Account bob;
    bob.id = 82;
    bob.generation = 7;
    assert(live_accounts.push(static_cast<Account &&>(alice)));
    assert(live_accounts.push(static_cast<Account &&>(bob)));

    size_t live_index = SIZE_MAX;
    assert(compose_account_choice_live_index(choices, 7, live_accounts, live_index));
    assert(live_index == 1);

    live_accounts[1].generation = 8;
    assert(!compose_account_choice_live_index(choices, 7, live_accounts, live_index));
    assert(live_index == 1);

    live_accounts[1].generation = 7;
    live_accounts.erase(1);
    assert(!compose_account_choice_live_index(choices, 7, live_accounts, live_index));
    live_accounts.erase(0);

    Account reordered_bob;
    reordered_bob.id = 82;
    reordered_bob.generation = 7;
    Account reordered_alice;
    reordered_alice.id = 21;
    reordered_alice.generation = 5;
    assert(live_accounts.push(static_cast<Account &&>(reordered_bob)));
    assert(live_accounts.push(static_cast<Account &&>(reordered_alice)));
    assert(compose_account_choice_live_index(choices, 7, live_accounts, live_index));
    assert(live_index == 0);
}

static void test_mail_editor_history_capabilities_follow_mail_undo_redo() {
    Draft draft;
    MailEditor editor;
    EditorStateLoad state;
    Text error;
    assert(editor.load_draft(draft, state, error));
    assert(!editor.can_undo() && !editor.can_redo());

    assert(editor.insert_utf8("first", 5));
    assert(editor.can_undo() && !editor.can_redo());
    assert(editor.undo());
    assert(!editor.can_undo() && editor.can_redo());
    assert(editor.redo());
    assert(editor.can_undo() && !editor.can_redo());

    assert(editor.undo());
    assert(!editor.can_undo() && editor.can_redo());
    assert(editor.insert_utf8("new", 3));
    assert(editor.can_undo() && !editor.can_redo());
}

struct ClipboardPasteCapture {
    void *target = nullptr;
    int buffer = -1;
    unsigned calls = 0;
};

static void capture_clipboard_paste(void *context, void *target, int buffer) {
    ClipboardPasteCapture *capture = static_cast<ClipboardPasteCapture *>(context);
    capture->target = target;
    capture->buffer = buffer;
    ++capture->calls;
}

static void test_keyboard_clipboard_operations_use_system_clipboard() {
    assert(compose_clipboard_buffer(ComposeClipboardOperation::MouseSelectionCopy) == 0);
    assert(compose_clipboard_buffer(ComposeClipboardOperation::MiddleMousePaste) == 0);
    assert(compose_clipboard_buffer(ComposeClipboardOperation::KeyboardCopy) == 1);
    assert(compose_clipboard_buffer(ComposeClipboardOperation::KeyboardCut) == 1);
    assert(compose_clipboard_buffer(ComposeClipboardOperation::KeyboardPaste) == 1);
    ClipboardPasteCapture capture;
    int canvas_token = 0;
    assert(compose_request_paste(&canvas_token, ComposeClipboardOperation::KeyboardPaste,
        capture_clipboard_paste, &capture));
    assert(capture.calls == 1 && capture.target == &canvas_token && capture.buffer == 1);
    capture.calls = 0;
    assert(compose_request_paste(&canvas_token, ComposeClipboardOperation::MiddleMousePaste,
        capture_clipboard_paste, &capture));
    assert(capture.calls == 1 && capture.target == &canvas_token && capture.buffer == 0);
    assert(!compose_request_paste(&canvas_token, static_cast<ComposeClipboardOperation>(255),
        capture_clipboard_paste, &capture));
    assert(capture.calls == 1);
}

static void test_send_confirmation_retains_complete_raw_recipient_fields() {
    Account account;
    account.id = 52;
    assert(account.display_name.set("Relief Coordinator"));
    assert(account.email_address.set("alice@reliefmail.test"));
    Draft draft;
    draft.account_id = account.id;
    draft.raw_recipient_input = true;
    assert(draft.to_input.set("bob@reliefmail.test"));
    assert(draft.cc_input.set(""));
    char *long_bcc = static_cast<char *>(malloc(12001));
    assert(long_bcc);
    memset(long_bcc, 'x', 12000);
    long_bcc[12000] = 0;
    assert(draft.bcc_input.set(long_bcc));
    assert(draft.subject.set("review-complete-subject"));
    Text confirmation, error;
    assert(compose_confirmation_text(account, draft, 3, confirmation, error));
    assert(confirmation.size() > 8192);
    assert(contains(confirmation, long_bcc));
    assert(contains(confirmation, "review-complete-subject"));
    free(long_bcc);
}

static bool contains(const Text &text, const char *needle) {
    const size_t n = strlen(needle);
    if (n > text.size()) return false;
    for (size_t i = 0; i + n <= text.size(); ++i)
        if (!memcmp(text.c_str() + i, needle, n)) return true;
    return false;
}

static size_t occurrences(const Text &text, const char *needle) {
    const size_t n = strlen(needle);
    if (!n || n > text.size()) return 0;
    size_t count = 0;
    for (size_t i = 0; i + n <= text.size();) {
        if (!memcmp(text.c_str() + i, needle, n)) { ++count; i += n; }
        else ++i;
    }
    return count;
}

static void test_rich_round_trip_uses_word_model_and_versioned_links() {
    Draft draft;
    MailEditor editor;
    EditorStateLoad state = EditorStateLoad::Corrupt;
    Text error;
    assert(editor.load_draft(draft, state, error));
    assert(state == EditorStateLoad::PlainDraft);
    assert(editor.insert_utf8("中文 ReliefMail", 17));
    editor.word_editor().select(0, 2);
    rw::TextStyle style = editor.word_editor().typing_style;
    style.bold = 1;
    style.family = rw::FontFamily::Sans;
    style.size = 14;
    assert(editor.apply_character_style(style, rw::Bold | rw::Family | rw::Size));
    rw::ParagraphStyle paragraph;
    paragraph.alignment = rw::Alignment::Center;
    paragraph.left_indent = 18;
    paragraph.first_indent = -4;
    assert(editor.apply_paragraph_style(paragraph, rw::ParaAlignment | rw::ParaIndent));
    editor.word_editor().select(3, 13);
    const char url[] = "https://example.invalid/reliefmail/project";
    assert(editor.set_link(url, sizeof(url) - 1));
    Text html, plain;
    assert(editor.export_plain(plain));
    assert(plain == "中文 ReliefMail");
    assert(editor.export_safe_html(html));
    assert(contains(html, "font-size:14pt"));
    assert(contains(html, "font-weight:bold"));
    assert(contains(html, "text-align:center"));
    assert(contains(html, "margin-left:18pt"));
    assert(contains(html, "text-indent:-4pt"));
    assert(contains(html, "href=\"https://example.invalid/reliefmail/project\""));

    assert(editor.save_into_draft(draft, error));
    assert(!draft.editor_state.empty());
    MailEditor reopened;
    assert(reopened.load_draft(draft, state, error));
    assert(state == EditorStateLoad::Loaded);
    assert(reopened.links().size() == 1);
    assert(reopened.links()[0].start == 3 && reopened.links()[0].end == 13);
    assert(reopened.export_safe_html(html));
    assert(contains(html, "font-family:Arial"));
    assert(contains(html, "text-align:center"));
    assert(contains(html, "href=\"https://example.invalid/reliefmail/project\""));
}

static void test_numbered_list_export_groups_items_and_keeps_paragraph_style_after_reopen() {
    Draft draft;
    MailEditor editor;
    EditorStateLoad state = EditorStateLoad::Corrupt;
    Text error;
    assert(editor.load_draft(draft, state, error));
    const char body[] = "First\nSecond\nThird";
    assert(editor.insert_utf8(body, sizeof(body) - 1));
    const rw::Document &document = editor.word_editor().doc;
    assert(document.paragraphs.size() == 3);

    const rw::Alignment alignments[] = {
        rw::Alignment::Center, rw::Alignment::Right, rw::Alignment::Left
    };
    const int16_t indents[] = {-4, 3, 5};
    for (size_t i = 0; i < document.paragraphs.size(); ++i) {
        const size_t start = document.paragraphs[i].start;
        const size_t end = i + 1 < document.paragraphs.size() ?
            document.paragraphs[i + 1].start - 1 : document.chars.size();
        rw::ParagraphStyle style = document.paragraphs[i].style;
        style.list = rw::ListKind::Number;
        style.alignment = alignments[i];
        style.left_indent = static_cast<int16_t>(12 + i * 3);
        style.first_indent = indents[i];
        editor.word_editor().select(start, end);
        assert(editor.apply_paragraph_style(style,
            rw::ParaList | rw::ParaAlignment | rw::ParaIndent));
    }

    Text html;
    assert(editor.export_safe_html(html));
    assert(occurrences(html, "<ol") == 1);
    assert(occurrences(html, "</ol>") == 1);
    assert(occurrences(html, "<li ") == 3);
    assert(contains(html, "<li style=\"text-align:center;margin-left:12pt"));
    assert(contains(html, "text-indent:-4pt"));
    assert(contains(html, "<li style=\"text-align:right;margin-left:15pt"));
    assert(contains(html, "text-indent:3pt"));
    assert(contains(html, "<li style=\"text-align:left;margin-left:18pt"));
    assert(contains(html, "text-indent:5pt"));

    assert(editor.save_into_draft(draft, error));
    MailEditor reopened;
    assert(reopened.load_draft(draft, state, error));
    assert(state == EditorStateLoad::Loaded);
    assert(reopened.export_safe_html(html));
    assert(occurrences(html, "<ol") == 1);
    assert(occurrences(html, "</ol>") == 1);
    assert(occurrences(html, "<li ") == 3);
    assert(contains(html, "<li style=\"text-align:center;margin-left:12pt"));
    assert(contains(html, "text-indent:-4pt"));
    assert(contains(html, "<li style=\"text-align:right;margin-left:15pt"));
    assert(contains(html, "text-indent:3pt"));
}

static void test_link_ranges_follow_edits_and_undo_redo() {
    Draft draft;
    MailEditor editor;
    EditorStateLoad state;
    Text error;
    assert(editor.load_draft(draft, state, error));
    assert(editor.insert_utf8("abcdef", 6));
    editor.word_editor().select(1, 5);
    const char target[] = "https://example.invalid/a";
    assert(editor.set_link(target, sizeof(target) - 1));
    editor.word_editor().select(2, 3);
    assert(editor.insert_utf8("X", 1));
    Text plain;
    assert(editor.export_plain(plain));
    assert(plain == "abXdef");
    assert(editor.links().size() == 1 && editor.links()[0].end == 5);
    assert(editor.undo());
    assert(editor.export_plain(plain) && plain == "abcdef");
    assert(editor.links().size() == 1 && editor.links()[0].start == 1 && editor.links()[0].end == 5);
    assert(editor.redo());
    assert(editor.export_plain(plain) && plain == "abXdef");
    assert(editor.links().size() == 1 && editor.links()[0].end == 5);

    editor.word_editor().select(0, 1);
    assert(editor.erase(false));
    assert(editor.links().size() == 1 && editor.links()[0].start == 0 && editor.links()[0].end == 4);
    assert(editor.undo());
    assert(editor.links()[0].start == 1 && editor.links()[0].end == 5);
    editor.word_editor().select(1, 2);
    assert(editor.remove_links());
    assert(editor.links().empty());
    assert(editor.undo());
    assert(editor.links().size() == 1);
}

static void test_plain_conversion_preserves_unknown_state_until_confirmed() {
    Draft draft;
    assert(draft.body.set("plain fallback"));
    const unsigned char future_state[] = {'R','M','E','D',0,9,0,0};
    assert(draft.editor_state.set(future_state, sizeof(future_state)));
    MailEditor editor;
    EditorStateLoad state;
    Text error;
    assert(editor.load_draft(draft, state, error));
    assert(state == EditorStateLoad::Unsupported);
    assert(!editor.editing_allowed());
    assert(!editor.insert_utf8("overwrite", 9));
    Draft unchanged;
    assert(clone_draft(draft, unchanged, error));
    assert(!editor.save_into_draft(unchanged, error));
    assert(unchanged.editor_state == draft.editor_state);
    assert(editor.accept_plain_fallback(draft, error));
    assert(editor.editing_allowed());
    Text converted;
    assert(editor.export_plain(converted));
    assert(converted == "plain fallback");
    assert(editor.save_into_draft(unchanged, error));
    assert(unchanged.editor_state != draft.editor_state);
}

static void test_mail_utf8_and_inherited_word_scalar_limits_fail_without_mutation() {
    Draft draft;
    MailEditor editor;
    EditorStateLoad state;
    Text error, plain;
    assert(editor.load_draft(draft, state, error));
    const size_t chinese_count = 349525;
    char *chinese = static_cast<char *>(malloc(chinese_count * 3));
    assert(chinese);
    for (size_t i = 0; i < chinese_count; ++i) memcpy(chinese + i * 3, "中", 3);
    assert(editor.insert_utf8(chinese, chinese_count * 3));
    assert(editor.text_bytes() == chinese_count * 3);
    assert(!editor.insert_utf8("中", 3));
    assert(editor.text_bytes() == chinese_count * 3);
    free(chinese);

    Draft scalar_draft;
    MailEditor scalar_editor;
    assert(scalar_editor.load_draft(scalar_draft, state, error));
    char *ascii = static_cast<char *>(malloc(rw::MaxChars));
    assert(ascii);
    memset(ascii, 'x', rw::MaxChars);
    assert(scalar_editor.insert_utf8(ascii, rw::MaxChars));
    assert(!scalar_editor.insert_utf8("x", 1));
    assert(scalar_editor.text_bytes() == rw::MaxChars);
    free(ascii);
}

static void test_reply_to_reply_all_and_truncated_quote() {
    Account account;
    assert(account.email_address.set("alice@reliefmail.test"));
    Draft source, reply, forward;
    Text error;
    Text reply_to, from, to, cc, subject, message_id, references, preview;
    assert(reply_to.set("reply@example.net"));
    assert(from.set("sender@example.net"));
    assert(to.set("bob@reliefmail.test, Alice <alice@reliefmail.test>"));
    assert(cc.set("carol@example.net, bob@reliefmail.test"));
    assert(subject.set("Relief exercise"));
    assert(message_id.set("<source@example.net>"));
    assert(references.set("<earlier@example.net>"));
    assert(preview.set("你好\nbody"));
    assert(compose_reply_draft(source, account, reply_to, from, to, cc, subject,
        message_id, references, preview, true, true, reply, error));
    assert(reply.to_input == "reply@example.net");
    assert(reply.cc_input == "bob@reliefmail.test, carol@example.net");
    assert(reply.in_reply_to == "<source@example.net>");
    assert(contains(reply.body, "preview was truncated"));
    assert(contains(reply.body, "> 你好"));
    assert(contains(reply.references, "<earlier@example.net> <source@example.net>"));

    assert(compose_forward_draft(source, from, to, cc, subject, preview, false, forward, error));
    assert(forward.subject == "Fwd: Relief exercise");
    assert(contains(forward.body, "Forwarded message"));
    assert(contains(forward.body, "From: sender@example.net"));
}

static void test_reply_all_excludes_sender_and_deduplicates_original_mailboxes() {
    Account account;
    assert(account.email_address.set("alice@reliefmail.test"));
    Draft source, reply;
    Text error;
    Text reply_to, from, to, cc, subject, message_id, references, preview;
    assert(reply_to.set("reply@example.net"));
    assert(from.set("sender@example.net"));
    assert(to.set("bob@example.net, Bob <bob@example.net>, alice@reliefmail.test"));
    assert(cc.set("reply@example.net, bob@example.net, carol@example.net, alice@reliefmail.test"));
    assert(subject.set("Status"));
    assert(preview.set("body"));
    assert(compose_reply_draft(source, account, reply_to, from, to, cc, subject,
        message_id, references, preview, false, true, reply, error));
    assert(reply.to_input == "reply@example.net");
    assert(reply.cc_input == "bob@example.net, carol@example.net");
}

static void test_first_save_identity_merge_and_coalesced_revision() {
    Draft clone_source, clone_target;
    clone_source.id = 70;
    clone_source.revision = 4;
    clone_source.account_id = 44;
    Text clone_error;
    assert(clone_draft(clone_source, clone_target, clone_error));
    assert(clone_target.id == 70 && clone_target.revision == 4);

    Draft live;
    live.account_id = 44;
    assert(live.body.set("first snapshot"));
    ComposeSaveGate gate;
    gate.reset_for_open(live);
    assert(gate.note_edit());
    WorkerDraftAttachmentRefs attachments;
    WorkerTaskInput request;
    ComposeSaveToken token;
    Text error;
    assert(gate.begin(live, 900, attachments, request, token, error));
    assert(token.kind == WorkerTaskKind::SaveDraft && request.item_id == 0 && request.item_revision == 0);
    assert(request.draft.id == 0 && request.draft.revision == 0);
    assert(live.body.set("newer text typed while first save is pending"));
    assert(gate.note_edit());

    WorkerDraftSnapshot saved;
    saved.account_generation = 900;
    saved.draft.account_id = 44;
    saved.draft.id = 71;
    saved.draft.revision = 1;
    assert(saved.draft.body.set("first snapshot"));
    bool save_again = false;
    assert(gate.complete(token, true, &saved, live, save_again, error));
    assert(live.id == 71 && live.revision == 1);
    assert(live.body == "newer text typed while first save is pending");
    assert(save_again && gate.dirty() && !gate.pending());

    request = WorkerTaskInput();
    assert(gate.begin(live, 900, attachments, request, token, error));
    assert(token.kind == WorkerTaskKind::SaveDraftRevision);
    assert(request.item_id == 71 && request.item_revision == 1);
    assert(request.draft.id == 71 && request.draft.revision == 1);
    WorkerDraftSnapshot updated;
    updated.account_generation = 900;
    updated.draft.account_id = 44;
    updated.draft.id = 71;
    updated.draft.revision = 2;
    assert(updated.draft.body.set("newer text typed while first save is pending"));
    assert(gate.complete(token, true, &updated, live, save_again, error));
    assert(live.id == 71 && live.revision == 2);
    assert(!save_again && !gate.dirty());
}

static void test_forward_first_save_uses_distinct_kind_then_exact_revision_update() {
    Draft live;
    live.account_id = 44;
    assert(live.body.set("quoted preview only"));
    ComposeSaveGate gate;
    gate.reset_for_open(live);
    assert(gate.note_edit());
    WorkerDraftAttachmentRefs attachments;
    WorkerTaskInput request;
    ComposeSaveToken token;
    Text error;
    MessageKey source;
    source.folder.account_id = 44;
    source.folder.server_name.set("INBOX");
    source.uidvalidity = 731;
    source.uid = 9;
    assert(gate.begin(live, 900, attachments, request, token, error,
        WorkerTaskKind::SaveForwardDraft, &source));
    assert(token.kind == WorkerTaskKind::SaveForwardDraft);
    assert(request.item_id == 0 && request.item_revision == 0);
    assert(request.message_key == source);

    WorkerDraftSnapshot saved;
    saved.account_generation = 900;
    saved.draft.account_id = 44;
    saved.draft.id = 81;
    saved.draft.revision = 1;
    bool save_again = false;
    assert(gate.complete(token, true, &saved, live, save_again, error));
    assert(live.id == 81 && live.revision == 1 && !save_again);
    assert(gate.note_edit());
    request = WorkerTaskInput();
    assert(gate.begin(live, 900, attachments, request, token, error));
    assert(token.kind == WorkerTaskKind::SaveDraftRevision);
    assert(request.item_id == 81 && request.item_revision == 1);
    assert(request.message_key.folder.server_name.empty());
}

int main() {
    test_compose_confirmation_uses_literal_confirmation_result_contract();
    test_queued_send_preflight_failure_restores_compose_controls_without_losing_ready_operation();
    test_sender_choices_use_actual_menu_indices_and_literal_labels();
    test_sender_choices_revalidate_against_live_account_generations();
    test_mail_editor_history_capabilities_follow_mail_undo_redo();
    test_keyboard_clipboard_operations_use_system_clipboard();
    test_send_confirmation_retains_complete_raw_recipient_fields();
    test_rich_round_trip_uses_word_model_and_versioned_links();
    test_numbered_list_export_groups_items_and_keeps_paragraph_style_after_reopen();
    test_link_ranges_follow_edits_and_undo_redo();
    test_plain_conversion_preserves_unknown_state_until_confirmed();
    test_mail_utf8_and_inherited_word_scalar_limits_fail_without_mutation();
    test_reply_to_reply_all_and_truncated_quote();
    test_reply_all_excludes_sender_and_deduplicates_original_mailboxes();
    test_first_save_identity_merge_and_coalesced_revision();
    test_forward_first_save_uses_distinct_kind_then_exact_revision_update();
    puts("ReliefMail rich editor suite passed");
}
