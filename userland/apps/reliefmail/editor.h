#ifndef RELIEFMAIL_EDITOR_H
#define RELIEFMAIL_EDITOR_H

#include "model.h"
#include "../reliefword/layout.h"

namespace rmail {

constexpr size_t MaxComposeTextBytes = 1u * 1024u * 1024u;
constexpr size_t MaxComposeLinks = 4096;

struct MailLinkRange {
    uint32_t start = 0;
    uint32_t end = 0;
    Text target;
};

enum class EditorStateLoad : uint8_t {
    Loaded,
    PlainDraft,
    Unsupported,
    Corrupt
};

/* Mail keeps hyperlink ranges and its versioned opaque state beside the real
 * Word document/editor/layout model. The Word model and Office serialization
 * remain unchanged. */
class MailEditor {
    struct Snapshot {
        rw::Document document;
        Array<MailLinkRange> links;
        size_t text_bytes = 0;
        size_t anchor = 0;
        size_t caret = 0;
        rw::TextStyle typing_style;
        uint64_t age = 0;
        bool rich_format = true;
        size_t cost() const;
    };
    rw::Editor editor_;
    Array<MailLinkRange> links_;
    Array<Snapshot> undo_;
    Array<Snapshot> redo_;
    Snapshot pending_snapshot_;
    bool pending_snapshot_valid_ = false;
    size_t history_bytes_ = 0;
    uint64_t next_history_age_ = 1;
    size_t text_bytes_ = 0;
    bool rich_format_ = true;
    bool opaque_state_blocked_ = false;
    bool typing_group_open_ = false;
    mutable Text error_;

    bool capture(Snapshot &snapshot) const;
    bool remember_before_edit(bool typing);
    bool commit_edit(bool typing, bool changed);
    bool restore(Snapshot &snapshot);
    void clear_redo();
    void trim_history();
    bool transform_links(size_t start, size_t end, size_t inserted);
    bool recompute_text_bytes(size_t &bytes) const;
    bool load_plain(const Draft &draft, Text &error);

public:
    MailEditor();
    ~MailEditor();
    MailEditor(const MailEditor &) = delete;
    MailEditor &operator=(const MailEditor &) = delete;

    rw::Editor &word_editor() { return editor_; }
    const rw::Editor &word_editor() const { return editor_; }
    const Array<MailLinkRange> &links() const { return links_; }
    size_t text_bytes() const { return text_bytes_; }
    bool rich_format() const { return rich_format_; }
    void end_typing_group() { typing_group_open_ = false; }
    bool editing_allowed() const { return !opaque_state_blocked_; }
    const Text &error() const { return error_; }

    bool load_draft(const Draft &draft, EditorStateLoad &state, Text &error);
    bool accept_plain_fallback(const Draft &draft, Text &error);
    bool save_into_draft(Draft &draft, Text &error);
    bool insert_utf8(const char *text, size_t length, bool coalesce_typing = false);
    bool erase(bool backward);
    bool paste_document(const rw::Document &fragment, const Array<MailLinkRange> &fragment_links);
    bool apply_character_style(const rw::TextStyle &style, unsigned mask = rw::AllStyle);
    bool apply_paragraph_style(const rw::ParagraphStyle &style, unsigned mask = rw::AllParagraph);
    bool set_link(const char *target, size_t length);
    bool remove_links();
    bool copy_selection(rw::Document &document, Array<MailLinkRange> &links);
    bool select_all();
    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
    bool undo();
    bool redo();
    bool find(const char *query, bool match_case, size_t from, size_t &start, size_t &end) const;
    bool set_rich_format(bool rich);
    bool export_plain(Text &output) const;
    bool export_safe_html(Text &output) const;
    bool encode_state(Bytes &output, Text &error) const;
};

bool compose_reply_draft(const Draft &source, const Account &account,
    const Text &reply_to, const Text &from, const Text &to, const Text &cc,
    const Text &subject, const Text &message_id, const Text &references,
    const Text &plain_preview, bool preview_truncated, bool reply_all,
    Draft &draft, Text &error);

bool compose_forward_draft(const Draft &source, const Text &from, const Text &to,
    const Text &cc, const Text &subject, const Text &plain_preview,
    bool preview_truncated, Draft &draft, Text &error);

}

#endif
