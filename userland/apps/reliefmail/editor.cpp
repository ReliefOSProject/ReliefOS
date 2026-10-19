#include "editor.h"
#include "html.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

namespace rmail {
namespace {

constexpr uint16_t EditorStateVersion = 1;
constexpr size_t EditorHistoryBudget = 64u * 1024u * 1024u;
constexpr size_t MaxHtmlBytes = MaxEditableTextBytes;
constexpr size_t MaxLinkTargetBytes = 2048;

bool set_error(Text &error, const char *message) {
    if (!error.set(message)) error.set("out of memory");
    return false;
}

bool style_equal(const rw::TextStyle &a, const rw::TextStyle &b) {
    return a == b;
}

size_t scalar_utf8_bytes(uint32_t cp) {
    return cp <= 0x7f ? 1 : cp <= 0x7ff ? 2 : cp <= 0xffff ? 3 : 4;
}

bool utf8_byte_count(const rw::Document &document, size_t start, size_t end, size_t &bytes) {
    if (start > end || end > document.chars.size()) return false;
    bytes = 0;
    for (size_t i = start; i < end; ++i) {
        const size_t count = scalar_utf8_bytes(document.chars[i].codepoint);
        if (count > SIZE_MAX - bytes) return false;
        bytes += count;
    }
    return true;
}

class Buffer {
    char *data_ = nullptr;
    size_t size_ = 0;
    size_t capacity_ = 0;
    size_t limit_;
public:
    explicit Buffer(size_t limit) : limit_(limit) {}
    ~Buffer() { free(data_); }
    size_t size() const { return size_; }
    const char *data() const { return data_; }
    bool append(const char *bytes, size_t length) {
        if ((!bytes && length) || length > limit_ - size_) return false;
        const size_t wanted = size_ + length;
        if (wanted > capacity_) {
            size_t next = capacity_ ? capacity_ : 256;
            while (next < wanted) {
                if (next > limit_ / 2) { next = wanted; break; }
                next *= 2;
            }
            if (next > limit_) next = limit_;
            char *replacement = static_cast<char *>(realloc(data_, next ? next : 1));
            if (!replacement) return false;
            data_ = replacement;
            capacity_ = next;
        }
        if (length) memcpy(data_ + size_, bytes, length);
        size_ = wanted;
        return true;
    }
    bool cstr(const char *text) { return append(text, strlen(text)); }
    bool finish(Text &output) const { return output.set(data_, size_); }
};

class StateWriter {
    unsigned char *data_ = nullptr;
    size_t size_ = 0;
    size_t capacity_ = 0;
public:
    ~StateWriter() { free(data_); }
    bool raw(const void *bytes, size_t count) {
        if ((!bytes && count) || count > MaxEditorStateBytes - size_) return false;
        const size_t wanted = size_ + count;
        if (wanted > capacity_) {
            size_t next = capacity_ ? capacity_ : 256;
            while (next < wanted) {
                if (next > MaxEditorStateBytes / 2) { next = wanted; break; }
                next *= 2;
            }
            if (next > MaxEditorStateBytes) next = MaxEditorStateBytes;
            unsigned char *replacement = static_cast<unsigned char *>(realloc(data_, next));
            if (!replacement) return false;
            data_ = replacement;
            capacity_ = next;
        }
        if (count) memcpy(data_ + size_, bytes, count);
        size_ = wanted;
        return true;
    }
    bool u8(uint8_t value) { return raw(&value, 1); }
    bool u16(uint16_t value) {
        unsigned char bytes[2] = {static_cast<unsigned char>(value), static_cast<unsigned char>(value >> 8)};
        return raw(bytes, sizeof(bytes));
    }
    bool u32(uint32_t value) {
        unsigned char bytes[4] = {static_cast<unsigned char>(value), static_cast<unsigned char>(value >> 8),
            static_cast<unsigned char>(value >> 16), static_cast<unsigned char>(value >> 24)};
        return raw(bytes, sizeof(bytes));
    }
    bool finish(Bytes &output) const { return output.set(data_, size_); }
};

class StateReader {
    const unsigned char *data_;
    size_t size_;
    size_t at_ = 0;
public:
    StateReader(const unsigned char *data, size_t size) : data_(data), size_(size) {}
    size_t remaining() const { return at_ <= size_ ? size_ - at_ : 0; }
    bool raw(void *output, size_t count) {
        if (count > remaining()) return false;
        if (count) memcpy(output, data_ + at_, count);
        at_ += count;
        return true;
    }
    bool u8(uint8_t &value) { return raw(&value, 1); }
    bool u16(uint16_t &value) {
        unsigned char bytes[2];
        if (!raw(bytes, sizeof(bytes))) return false;
        value = static_cast<uint16_t>(bytes[0] | (static_cast<uint16_t>(bytes[1]) << 8));
        return true;
    }
    bool u32(uint32_t &value) {
        unsigned char bytes[4];
        if (!raw(bytes, sizeof(bytes))) return false;
        value = static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
            (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
        return true;
    }
    bool text(Text &value, size_t length) {
        if (length > remaining() || !valid_utf8(reinterpret_cast<const char *>(data_ + at_), length)) return false;
        const bool ok = value.set(reinterpret_cast<const char *>(data_ + at_), length);
        if (ok) at_ += length;
        return ok;
    }
};

bool safe_link_target(const char *target, size_t length) {
    if (!target || !length || length > MaxLinkTargetBytes || !valid_utf8(target, length)) return false;
    const bool web = (length >= 7 && !memcmp(target, "http://", 7)) ||
        (length >= 8 && !memcmp(target, "https://", 8));
    const bool mail = length >= 7 && !memcmp(target, "mailto:", 7);
    if (!web && !mail) return false;
    if (web) {
        size_t host = length >= 8 && !memcmp(target, "https://", 8) ? 8 : 7;
        if (host == length || target[host] == '/' || target[host] == '?' || target[host] == '#') return false;
    }
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(target[i]);
        if (c <= 0x20 || c == 0x7f || c == '"' || c == '<' || c == '>' || c == '`') return false;
    }
    return true;
}

bool copy_links(const Array<MailLinkRange> &from, Array<MailLinkRange> &to) {
    if (!to.reserve(from.size())) return false;
    for (size_t i = 0; i < from.size(); ++i)
        if (!to.push(from[i])) return false;
    return true;
}

bool map_link_ranges(const Array<MailLinkRange> &source, size_t start, size_t end,
                     size_t inserted, Array<MailLinkRange> &output) {
    if (start > end || inserted > rw::MaxChars) return false;
    const size_t removed = end - start;
    const int64_t delta = static_cast<int64_t>(inserted) - static_cast<int64_t>(removed);
    if (!output.reserve(source.size())) return false;
    for (size_t i = 0; i < source.size(); ++i) {
        MailLinkRange range = source[i];
        if (start == end) {
            if (range.end <= start) {
                /* Insertion at a link's right edge is outside the link. */
            } else if (range.start >= start) {
                range.start += inserted;
                range.end += inserted;
            } else {
                range.end += inserted;
            }
        } else if (range.end <= start) {
            /* Entirely before the edit. */
        } else if (range.start >= end) {
            range.start = static_cast<uint32_t>(static_cast<int64_t>(range.start) + delta);
            range.end = static_cast<uint32_t>(static_cast<int64_t>(range.end) + delta);
        } else {
            const size_t mapped_start = range.start < start ? range.start : start;
            const size_t mapped_end = range.end > end ?
                static_cast<size_t>(static_cast<int64_t>(range.end) + delta) : start + inserted;
            if (mapped_end <= mapped_start) continue;
            range.start = static_cast<uint32_t>(mapped_start);
            range.end = static_cast<uint32_t>(mapped_end);
        }
        if (!output.push(static_cast<MailLinkRange &&>(range))) return false;
    }
    return true;
}

bool append_html_text(Buffer &output, uint32_t codepoint) {
    if (codepoint == '&') return output.cstr("&amp;");
    if (codepoint == '<') return output.cstr("&lt;");
    if (codepoint == '>') return output.cstr("&gt;");
    if (codepoint == '"') return output.cstr("&quot;");
    if (codepoint == '\'') return output.cstr("&#39;");
    if (codepoint == '\t') return output.cstr("&#9;");
    char encoded[4];
    const unsigned count = rw::encode_utf8(codepoint, encoded);
    return count && output.append(encoded, count);
}

const char *css_family(rw::FontFamily family) {
    switch (family) {
    case rw::FontFamily::Sans: return "Arial";
    case rw::FontFamily::Serif: return "SimSun";
    case rw::FontFamily::Mono: return "monospace";
    }
    return "SimSun";
}

bool append_char_style(Buffer &output, const rw::TextStyle &style) {
    char css[256];
    const int size = snprintf(css, sizeof(css),
        "<span style=\"font-family:%s;font-size:%upt;color:#%06X%s%s%s\">",
        css_family(style.family), static_cast<unsigned>(style.size),
        static_cast<unsigned>(style.foreground), style.bold ? ";font-weight:bold" : "",
        style.italic ? ";font-style:italic" : "",
        style.underline ? ";text-decoration:underline" : "");
    return size > 0 && static_cast<size_t>(size) < sizeof(css) && output.append(css, static_cast<size_t>(size));
}

bool append_paragraph_style(Buffer &output, const rw::ParagraphStyle &style) {
    const char *align = style.alignment == rw::Alignment::Center ? "center" :
        style.alignment == rw::Alignment::Right ? "right" :
        style.alignment == rw::Alignment::Justify ? "justify" : "left";
    char css[256];
    const int size = snprintf(css, sizeof(css),
        " style=\"text-align:%s;margin-left:%dpt;margin-right:%dpt;text-indent:%dpt\"",
        align, static_cast<int>(style.left_indent), static_cast<int>(style.right_indent),
        static_cast<int>(style.first_indent));
    return size > 0 && static_cast<size_t>(size) < sizeof(css) && output.append(css, static_cast<size_t>(size));
}

bool append_inline_html(const rw::Document &document, const Array<MailLinkRange> &links,
                        size_t start, size_t end, Buffer &output) {
    size_t active_link = SIZE_MAX;
    bool span_open = false;
    rw::TextStyle active_style;
    bool have_style = false;
    for (size_t at = start; at < end; ++at) {
        size_t link_index = SIZE_MAX;
        for (size_t i = 0; i < links.size(); ++i) {
            if (at >= links[i].start && at < links[i].end) { link_index = i; break; }
            if (links[i].start > at) break;
        }
        const rw::TextStyle &style = document.styles[document.chars[at].style];
        if (link_index != active_link) {
            if (span_open && !output.cstr("</span>")) return false;
            span_open = false;
            have_style = false;
            if (active_link != SIZE_MAX && !output.cstr("</a>")) return false;
            active_link = link_index;
            if (active_link != SIZE_MAX) {
                if (!output.cstr("<a href=\"")) return false;
                const Text &target = links[active_link].target;
                for (size_t i = 0; i < target.size(); ++i) {
                    const char c = target.c_str()[i];
                    if (c == '&') { if (!output.cstr("&amp;")) return false; }
                    else if (c == '"') { if (!output.cstr("&quot;")) return false; }
                    else if (!output.append(&c, 1)) return false;
                }
                if (!output.cstr("\">")) return false;
            }
        }
        if (!have_style || !style_equal(active_style, style)) {
            if (span_open && !output.cstr("</span>")) return false;
            if (!append_char_style(output, style)) return false;
            active_style = style;
            have_style = true;
            span_open = true;
        }
        if (!append_html_text(output, document.chars[at].codepoint)) return false;
    }
    if (span_open && !output.cstr("</span>")) return false;
    if (active_link != SIZE_MAX && !output.cstr("</a>")) return false;
    return true;
}

bool parse_editor_state(const Bytes &bytes, rw::Document &document,
                        Array<MailLinkRange> &links, size_t &text_bytes,
                        size_t &anchor, size_t &caret, rw::TextStyle &typing_style,
                        bool &rich_format, EditorStateLoad &state, Text &error) {
    state = EditorStateLoad::Corrupt;
    if (bytes.size() < 6 || memcmp(bytes.data(), "RMES", 4)) {
        state = EditorStateLoad::Unsupported;
        return true;
    }
    StateReader reader(bytes.data(), bytes.size());
    unsigned char magic[4];
    uint16_t version = 0, style_count = 0;
    uint8_t rich = 0;
    uint32_t stored_bytes = 0, glyph_count = 0, paragraph_count = 0, link_count = 0;
    uint32_t anchor32 = 0, caret32 = 0;
    if (!reader.raw(magic, sizeof(magic)) || !reader.u16(version))
        return set_error(error, "editor state header is incomplete");
    if (version != EditorStateVersion) { state = EditorStateLoad::Unsupported; return true; }
    if (!reader.u8(rich) || rich > 1 || !reader.u32(stored_bytes) ||
        !reader.u32(anchor32) || !reader.u32(caret32) || !reader.u32(glyph_count) ||
        !reader.u16(style_count) || !reader.u32(paragraph_count) || !reader.u32(link_count) ||
        stored_bytes > MaxComposeTextBytes || glyph_count > rw::MaxChars ||
        !style_count || style_count > rw::MaxStyles || !paragraph_count ||
        paragraph_count > rw::MaxParagraphs || link_count > MaxComposeLinks)
        return set_error(error, "editor state header is invalid");

    rw::Document next;
    if (!next.init()) return set_error(error, "out of memory creating the editor document");
    next.chars.clear(); next.styles.clear(); next.paragraphs.clear();
    for (uint16_t i = 0; i < style_count; ++i) {
        rw::TextStyle style;
        uint8_t family = 0, flags = 0;
        if (!reader.u32(style.foreground) || !reader.u32(style.background) || !reader.u16(style.size) ||
            !reader.u8(family) || !reader.u8(flags) || family > static_cast<uint8_t>(rw::FontFamily::Mono) ||
            (flags & 0xf0u) || style.foreground > 0xffffff || style.background > 0xffffff ||
            style.size < 6 || style.size > 96)
            return set_error(error, "editor state contains an invalid character style");
        style.family = static_cast<rw::FontFamily>(family);
        style.bold = flags & 1u; style.italic = (flags >> 1) & 1u;
        style.underline = (flags >> 2) & 1u; style.strike = (flags >> 3) & 1u;
        if (!next.styles.push(static_cast<rw::TextStyle &&>(style)))
            return set_error(error, "out of memory loading character styles");
    }
    if (!reader.u32(typing_style.foreground) || !reader.u32(typing_style.background) ||
        !reader.u16(typing_style.size)) return set_error(error, "editor typing style is incomplete");
    uint8_t typing_family = 0, typing_flags = 0;
    if (!reader.u8(typing_family) || !reader.u8(typing_flags) ||
        typing_family > static_cast<uint8_t>(rw::FontFamily::Mono) || (typing_flags & 0xf0u) ||
        typing_style.foreground > 0xffffff || typing_style.background > 0xffffff ||
        typing_style.size < 6 || typing_style.size > 96)
        return set_error(error, "editor typing style is invalid");
    typing_style.family = static_cast<rw::FontFamily>(typing_family);
    typing_style.bold = typing_flags & 1u; typing_style.italic = (typing_flags >> 1) & 1u;
    typing_style.underline = (typing_flags >> 2) & 1u; typing_style.strike = (typing_flags >> 3) & 1u;

    if (!next.chars.reserve(glyph_count) || !next.paragraphs.reserve(paragraph_count))
        return set_error(error, "out of memory loading editor content");
    for (uint32_t i = 0; i < glyph_count; ++i) {
        uint32_t codepoint = 0;
        uint16_t style = 0;
        if (!reader.u32(codepoint) || !reader.u16(style) || style >= style_count || !rw::valid_scalar(codepoint) ||
            codepoint == '\r' || !next.chars.push({codepoint, style}))
            return set_error(error, "editor state contains an invalid glyph");
    }
    for (uint32_t i = 0; i < paragraph_count; ++i) {
        rw::Paragraph paragraph;
        uint32_t start = 0;
        uint8_t alignment = 0, list = 0;
        uint16_t left = 0, right = 0, first = 0;
        if (!reader.u32(start) || !reader.u8(alignment) || !reader.u8(list) ||
            !reader.u16(left) || !reader.u16(right) || !reader.u16(first) ||
            !reader.u16(paragraph.style.before) || !reader.u16(paragraph.style.after) ||
            !reader.u16(paragraph.style.line_spacing) ||
            alignment > static_cast<uint8_t>(rw::Alignment::Justify) ||
            list > static_cast<uint8_t>(rw::ListKind::Number))
            return set_error(error, "editor state contains an invalid paragraph");
        paragraph.start = start;
        paragraph.style.alignment = static_cast<rw::Alignment>(alignment);
        paragraph.style.list = static_cast<rw::ListKind>(list);
        paragraph.style.left_indent = static_cast<int16_t>(left);
        paragraph.style.right_indent = static_cast<int16_t>(right);
        paragraph.style.first_indent = static_cast<int16_t>(first);
        if (!next.paragraphs.push(static_cast<rw::Paragraph &&>(paragraph)))
            return set_error(error, "out of memory loading paragraphs");
    }
    if (!links.reserve(link_count)) return set_error(error, "out of memory loading hyperlinks");
    for (uint32_t i = 0; i < link_count; ++i) {
        MailLinkRange range;
        uint16_t length = 0;
        if (!reader.u32(range.start) || !reader.u32(range.end) || !reader.u16(length) ||
            !length || length > MaxLinkTargetBytes || !reader.text(range.target, length) ||
            range.start >= range.end || range.end > glyph_count ||
            !safe_link_target(range.target.c_str(), range.target.size()) ||
            (i && links[i - 1].end > range.start) || !links.push(static_cast<MailLinkRange &&>(range)))
            return set_error(error, "editor state contains an invalid hyperlink range");
    }
    if (reader.remaining() || anchor32 > glyph_count || caret32 > glyph_count)
        return set_error(error, "editor state has trailing data or an invalid selection");
    next.page = rw::PageSettings{};
    rw::Text validation;
    if (!next.validate(validation)) return set_error(error, validation.c_str());
    size_t actual_bytes = 0;
    if (!utf8_byte_count(next, 0, next.chars.size(), actual_bytes) || actual_bytes != stored_bytes)
        return set_error(error, "editor state text byte count does not match its contents");
    document = static_cast<rw::Document &&>(next);
    text_bytes = actual_bytes;
    anchor = anchor32; caret = caret32; rich_format = rich != 0;
    state = EditorStateLoad::Loaded;
    return true;
}

bool append_text(Buffer &output, const Text &text) { return output.append(text.c_str(), text.size()); }

bool same_mailbox(const Address &left, const Address &right) {
    return left.local_part.size() == right.local_part.size() &&
        left.domain.size() == right.domain.size() &&
        !strcasecmp(left.local_part.c_str(), right.local_part.c_str()) &&
        !strcasecmp(left.domain.c_str(), right.domain.c_str());
}

bool append_unique_recipients(Buffer &output, const Array<Address> &addresses,
                              Array<Address> &seen) {
    for (size_t i = 0; i < addresses.size(); ++i) {
        const Address &address = addresses[i];
        bool duplicate = false;
        for (size_t j = 0; j < seen.size(); ++j) {
            if (same_mailbox(address, seen[j])) { duplicate = true; break; }
        }
        if (duplicate) continue;
        Text mailbox, error;
        if (!serialize_address(address, mailbox, error)) return false;
        if (output.size() && !output.cstr(", ")) return false;
        if (!output.append(mailbox.c_str(), mailbox.size()) || !seen.push(address)) return false;
    }
    return true;
}

bool valid_message_id_list(const Text &value, bool allow_many) {
    size_t at = 0, count = 0;
    while (at < value.size()) {
        while (at < value.size() && (value.c_str()[at] == ' ' || value.c_str()[at] == '\t')) ++at;
        if (at == value.size()) break;
        const size_t start = at;
        while (at < value.size() && value.c_str()[at] != ' ' && value.c_str()[at] != '\t') ++at;
        const size_t length = at - start;
        if (length < 5 || length > 900 || value.c_str()[start] != '<' || value.c_str()[at - 1] != '>') return false;
        size_t inner_at = SIZE_MAX;
        for (size_t i = start + 1; i + 1 < at; ++i) {
            const unsigned char c = static_cast<unsigned char>(value.c_str()[i]);
            if (c == '@') { if (inner_at != SIZE_MAX) return false; inner_at = i; }
            if (c <= 0x20 || c == '<' || c == '>' || c == '\\') return false;
        }
        if (inner_at == SIZE_MAX || inner_at == start + 1 || inner_at + 1 == at - 1) return false;
        ++count;
    }
    return allow_many ? count > 0 : count == 1;
}

bool append_prefixed_subject(const Text &subject, Text &output) {
    const bool already = subject.size() >= 3 &&
        (subject.c_str()[0] == 'R' || subject.c_str()[0] == 'r') &&
        (subject.c_str()[1] == 'E' || subject.c_str()[1] == 'e') && subject.c_str()[2] == ':';
    Buffer result(MaxEditableTextBytes);
    if ((!already && !result.cstr("Re: ")) || !append_text(result, subject) || !result.finish(output)) return false;
    return true;
}

bool append_forward_subject(const Text &subject, Text &output) {
    const bool already = (subject.size() >= 4 &&
        (subject.c_str()[0] == 'F' || subject.c_str()[0] == 'f') &&
        (subject.c_str()[1] == 'W' || subject.c_str()[1] == 'w') &&
        (subject.c_str()[2] == 'D' || subject.c_str()[2] == 'd') && subject.c_str()[3] == ':') ||
        (subject.size() >= 3 &&
        (subject.c_str()[0] == 'F' || subject.c_str()[0] == 'f') &&
        (subject.c_str()[1] == 'W' || subject.c_str()[1] == 'w') && subject.c_str()[2] == ':');
    Buffer result(MaxEditableTextBytes);
    if ((!already && !result.cstr("Fwd: ")) || !append_text(result, subject) || !result.finish(output))
        return false;
    return true;
}

bool append_quoted_body(const Text &preview, bool truncated, Text &output) {
    Buffer result(MaxComposeTextBytes);
    if (truncated && !result.cstr("[Quoted content incomplete: the original message preview was truncated.]\n\n")) return false;
    for (size_t i = 0; i < preview.size(); ++i) {
        if (i == 0 || preview.c_str()[i - 1] == '\n')
            if (!result.cstr("> ")) return false;
        const char c = preview.c_str()[i];
        if (c == '\r') continue;
        if (!result.append(&c, 1)) return false;
    }
    if (preview.empty() || preview.c_str()[preview.size() - 1] != '\n') {
        if (!result.cstr("\n")) return false;
    }
    if (!result.finish(output)) return false;
    return true;
}

} // namespace

MailEditor::MailEditor() {
    (void)links_.reserve(8);
    (void)undo_.reserve(16);
    (void)redo_.reserve(16);
}

MailEditor::~MailEditor() = default;

bool MailEditor::capture(Snapshot &snapshot) const {
    Snapshot next;
    if (!next.document.clone_from(editor_.doc) || !copy_links(links_, next.links)) return false;
    next.text_bytes = text_bytes_;
    next.anchor = editor_.anchor;
    next.caret = editor_.caret;
    next.typing_style = editor_.typing_style;
    next.rich_format = rich_format_;
    snapshot = static_cast<Snapshot &&>(next);
    return true;
}

bool MailEditor::remember_before_edit(bool typing) {
    pending_snapshot_valid_ = false;
    if (opaque_state_blocked_) return set_error(error_, "confirm plain-text conversion before editing this draft");
    if (typing && typing_group_open_) return true;
    if (!typing) typing_group_open_ = false;
    if (!undo_.reserve(undo_.size() + 1) || !capture(pending_snapshot_))
        return set_error(error_, "not enough memory to preserve the undo state");
    pending_snapshot_valid_ = true;
    return true;
}

size_t MailEditor::Snapshot::cost() const {
    size_t total = document.chars.size() * sizeof(rw::Glyph) +
        document.styles.size() * sizeof(rw::TextStyle) +
        document.paragraphs.size() * sizeof(rw::Paragraph) + links.size() * sizeof(MailLinkRange);
    for (size_t i = 0; i < links.size(); ++i) {
        if (links[i].target.size() > SIZE_MAX - total) return SIZE_MAX;
        total += links[i].target.size();
    }
    return total;
}

bool MailEditor::commit_edit(bool typing, bool changed) {
    if (!changed) {
        pending_snapshot_valid_ = false;
        if (!typing) typing_group_open_ = false;
        error_.clear();
        return true;
    }
    if (pending_snapshot_valid_) {
        pending_snapshot_.age = next_history_age_++;
        const size_t cost = pending_snapshot_.cost();
        if (!undo_.push(static_cast<Snapshot &&>(pending_snapshot_)))
            return set_error(error_, "not enough memory to store the undo state");
        pending_snapshot_valid_ = false;
        if (cost > SIZE_MAX - history_bytes_) history_bytes_ = SIZE_MAX;
        else history_bytes_ += cost;
        clear_redo();
        trim_history();
    }
    editor_.reset_history();
    typing_group_open_ = typing;
    error_.clear();
    return true;
}

bool MailEditor::restore(Snapshot &snapshot) {
    if (!editor_.replace_document(static_cast<rw::Document &&>(snapshot.document)))
        return set_error(error_, "could not restore the editor document");
    editor_.anchor = snapshot.anchor;
    editor_.caret = snapshot.caret;
    editor_.typing_style = snapshot.typing_style;
    links_.swap(snapshot.links);
    text_bytes_ = snapshot.text_bytes;
    rich_format_ = snapshot.rich_format;
    typing_group_open_ = false;
    return true;
}

void MailEditor::clear_redo() {
    for (size_t i = 0; i < redo_.size(); ++i) {
        const size_t cost = redo_[i].cost();
        history_bytes_ = cost > history_bytes_ ? 0 : history_bytes_ - cost;
    }
    redo_.clear();
}

void MailEditor::trim_history() {
    while (history_bytes_ > EditorHistoryBudget && (undo_.size() || redo_.size())) {
        Array<Snapshot> *oldest = nullptr;
        if (!undo_.size()) oldest = &redo_;
        else if (!redo_.size()) oldest = &undo_;
        else oldest = undo_[0].age <= redo_[0].age ? &undo_ : &redo_;
        const size_t cost = (*oldest)[0].cost();
        history_bytes_ = cost > history_bytes_ ? 0 : history_bytes_ - cost;
        oldest->erase(0);
    }
}

bool MailEditor::transform_links(size_t start, size_t end, size_t inserted) {
    Array<MailLinkRange> next;
    if (!map_link_ranges(links_, start, end, inserted, next))
        return set_error(error_, "not enough memory to update hyperlinks");
    links_.swap(next);
    return true;
}

bool MailEditor::recompute_text_bytes(size_t &bytes) const {
    return utf8_byte_count(editor_.doc, 0, editor_.doc.chars.size(), bytes);
}

bool MailEditor::load_plain(const Draft &draft, Text &error) {
    if (draft.body.size() > MaxComposeTextBytes || !rw::valid_utf8(draft.body.c_str(), draft.body.size()))
        return set_error(error, "draft plain text is invalid or exceeds 1 MiB");
    rw::Document empty;
    if (!empty.init() || !editor_.replace_document(static_cast<rw::Document &&>(empty)))
        return set_error(error, "could not initialize the mail editor");
    if (!draft.body.empty() && !editor_.insert_utf8(draft.body.c_str(), draft.body.size()))
        return set_error(error, editor_.error.c_str());
    editor_.reset_history();
    editor_.select(editor_.doc.chars.size(), editor_.doc.chars.size());
    links_.clear();
    text_bytes_ = draft.body.size();
    rich_format_ = draft.rich_format;
    error.clear();
    return true;
}

bool MailEditor::load_draft(const Draft &draft, EditorStateLoad &state, Text &error) {
    error.clear();
    if (draft.editor_state.empty()) {
        if (draft.rich_format && !draft.html_body.empty()) {
            state = EditorStateLoad::Unsupported;
            opaque_state_blocked_ = true;
            if (!load_plain(draft, error)) return false;
            opaque_state_blocked_ = true;
            error.set("legacy rich draft has no versioned editor state; confirm plain-text conversion before editing");
            return true;
        }
        opaque_state_blocked_ = false;
        state = EditorStateLoad::PlainDraft;
        return load_plain(draft, error);
    }
    rw::Document document;
    Array<MailLinkRange> links;
    size_t bytes = 0, anchor = 0, caret = 0;
    rw::TextStyle typing;
    bool rich = true;
    if (!parse_editor_state(draft.editor_state, document, links, bytes, anchor, caret,
            typing, rich, state, error)) {
        opaque_state_blocked_ = true;
        Text preview_error;
        if (!load_plain(draft, preview_error)) return false;
        opaque_state_blocked_ = true;
        return true;
    }
    if (state == EditorStateLoad::Unsupported) {
        opaque_state_blocked_ = true;
        if (!load_plain(draft, error)) return false;
        opaque_state_blocked_ = true;
        error.set("editor state version is not supported; confirm plain-text conversion before editing");
        return true;
    }
    if (!editor_.replace_document(static_cast<rw::Document &&>(document))) {
        state = EditorStateLoad::Corrupt;
        opaque_state_blocked_ = true;
        return set_error(error, "could not install the saved editor document");
    }
    links_.swap(links);
    editor_.anchor = anchor;
    editor_.caret = caret;
    editor_.typing_style = typing;
    editor_.reset_history();
    text_bytes_ = bytes;
    rich_format_ = rich;
    opaque_state_blocked_ = false;
    undo_.clear(); redo_.clear(); history_bytes_ = 0;
    error.clear();
    return true;
}

bool MailEditor::accept_plain_fallback(const Draft &draft, Text &error) {
    if (!load_plain(draft, error)) return false;
    opaque_state_blocked_ = false;
    error.clear();
    return true;
}

bool MailEditor::save_into_draft(Draft &draft, Text &error) {
    error.clear();
    if (opaque_state_blocked_)
        return set_error(error, "unsupported editor state is preserved; explicitly convert to plain text before saving");
    Text body, html;
    Bytes state;
    if (!export_plain(body) || !export_safe_html(html) || !encode_state(state, error)) {
        if (error.empty()) error.set("could not export the editor content");
        return false;
    }
    if (body.size() > MaxEditableTextBytes || html.size() > MaxEditableTextBytes ||
        state.size() > MaxEditorStateBytes)
        return set_error(error, "formatted draft exceeds a configured storage limit");
    draft.body = static_cast<Text &&>(body);
    draft.html_body = static_cast<Text &&>(html);
    draft.editor_state = static_cast<Bytes &&>(state);
    draft.rich_format = rich_format_;
    return true;
}

bool MailEditor::insert_utf8(const char *text, size_t length, bool coalesce_typing) {
    if ((!text && length) || length > MaxComposeTextBytes || !rw::valid_utf8(text, length))
        return set_error(error_, "text must be valid UTF-8 no larger than 1 MiB");
    rw::Array<uint32_t> decoded;
    rw::Text decode_error;
    if (!rw::decode_utf8(text, length, decoded, decode_error)) return set_error(error_, decode_error.c_str());
    size_t normalized_bytes = 0, normalized_chars = 0;
    for (size_t i = 0; i < decoded.size(); ++i) {
        const uint32_t cp = decoded[i];
        if (cp == '\r') {
            if (i + 1 < decoded.size() && decoded[i + 1] == '\n') ++i;
            ++normalized_chars;
            ++normalized_bytes;
        } else {
            ++normalized_chars;
            normalized_bytes += scalar_utf8_bytes(cp);
        }
    }
    const size_t start = editor_.selection_start(), end = editor_.selection_end();
    size_t removed_bytes = 0;
    if (!utf8_byte_count(editor_.doc, start, end, removed_bytes) || removed_bytes > text_bytes_ ||
        normalized_bytes > MaxComposeTextBytes - (text_bytes_ - removed_bytes))
        return set_error(error_, "edit would exceed the 1 MiB UTF-8 body limit");
    if (start == end && !normalized_chars) { error_.clear(); return true; }
    Array<MailLinkRange> next_links;
    if (!map_link_ranges(links_, start, end, normalized_chars, next_links))
        return set_error(error_, "not enough memory to update hyperlinks");
    if (!remember_before_edit(coalesce_typing)) return false;
    const uint64_t prior_revision = editor_.doc.revision;
    const rw::TextStyle prior_typing = editor_.typing_style;
    if (!editor_.insert_utf8(text, length)) {
        pending_snapshot_valid_ = false;
        return set_error(error_, editor_.error.c_str());
    }
    links_.swap(next_links);
    text_bytes_ = text_bytes_ - removed_bytes + normalized_bytes;
    const bool changed = prior_revision != editor_.doc.revision || !style_equal(prior_typing, editor_.typing_style);
    return commit_edit(coalesce_typing, changed);
}

bool MailEditor::erase(bool backward) {
    if (!editing_allowed()) return set_error(error_, "confirm plain-text conversion before editing this draft");
    size_t start = editor_.selection_start(), end = editor_.selection_end();
    if (start == end) {
        if (backward) {
            if (!start) { error_.clear(); return true; }
            start = rw::previous_cluster(editor_.doc, start);
        } else {
            if (end >= editor_.doc.chars.size()) { error_.clear(); return true; }
            end = rw::next_cluster(editor_.doc, end);
        }
    }
    size_t removed_bytes = 0;
    if (!utf8_byte_count(editor_.doc, start, end, removed_bytes) || removed_bytes > text_bytes_)
        return set_error(error_, "editor selection is invalid");
    Array<MailLinkRange> next_links;
    if (!map_link_ranges(links_, start, end, 0, next_links))
        return set_error(error_, "not enough memory to update hyperlinks");
    if (!remember_before_edit(false)) return false;
    const uint64_t revision = editor_.doc.revision;
    if (!editor_.erase(backward)) {
        pending_snapshot_valid_ = false;
        return set_error(error_, editor_.error.c_str());
    }
    if (editor_.doc.revision == revision) { pending_snapshot_valid_ = false; error_.clear(); return true; }
    links_.swap(next_links);
    text_bytes_ -= removed_bytes;
    return commit_edit(false, true);
}

bool MailEditor::paste_document(const rw::Document &fragment, const Array<MailLinkRange> &fragment_links) {
    if (!editing_allowed()) return set_error(error_, "confirm plain-text conversion before editing this draft");
    rw::Text validation;
    if (!fragment.validate(validation)) return set_error(error_, validation.c_str());
    size_t paste_bytes = 0;
    if (!utf8_byte_count(fragment, 0, fragment.chars.size(), paste_bytes))
        return set_error(error_, "clipboard content has invalid text positions");
    const size_t start = editor_.selection_start(), end = editor_.selection_end();
    size_t removed_bytes = 0;
    if (!utf8_byte_count(editor_.doc, start, end, removed_bytes) || removed_bytes > text_bytes_ ||
        paste_bytes > MaxComposeTextBytes - (text_bytes_ - removed_bytes))
        return set_error(error_, "paste would exceed the 1 MiB UTF-8 body limit");
    Array<MailLinkRange> next_links;
    if (!map_link_ranges(links_, start, end, fragment.chars.size(), next_links))
        return set_error(error_, "not enough memory to update hyperlinks");
    for (size_t i = 0; i < fragment_links.size(); ++i) {
        const MailLinkRange &source = fragment_links[i];
        if (source.start >= source.end || source.end > fragment.chars.size() ||
            !safe_link_target(source.target.c_str(), source.target.size()))
            return set_error(error_, "clipboard contains an invalid hyperlink");
        MailLinkRange link;
        link.start = static_cast<uint32_t>(start + source.start);
        link.end = static_cast<uint32_t>(start + source.end);
        link.target = source.target;
        if (!next_links.push(static_cast<MailLinkRange &&>(link)))
            return set_error(error_, "not enough memory to paste hyperlinks");
    }
    if (!remember_before_edit(false)) return false;
    const uint64_t revision = editor_.doc.revision;
    if (!editor_.paste_document(fragment)) {
        pending_snapshot_valid_ = false;
        return set_error(error_, editor_.error.c_str());
    }
    links_.swap(next_links);
    text_bytes_ = text_bytes_ - removed_bytes + paste_bytes;
    return commit_edit(false, editor_.doc.revision != revision);
}

bool MailEditor::apply_character_style(const rw::TextStyle &style, unsigned mask) {
    if (!editing_allowed()) return set_error(error_, "confirm plain-text conversion before editing this draft");
    if (style.size < 6 || style.size > 96 || style.foreground > 0xffffff || style.background > 0xffffff)
        return set_error(error_, "mail font size must be between 6 and 96 pt with a valid color");
    if (!remember_before_edit(false)) return false;
    const uint64_t revision = editor_.doc.revision;
    const rw::TextStyle typing = editor_.typing_style;
    if (!editor_.apply_style(style, mask)) {
        pending_snapshot_valid_ = false;
        return set_error(error_, editor_.error.c_str());
    }
    const bool changed = revision != editor_.doc.revision || !style_equal(typing, editor_.typing_style);
    return commit_edit(false, changed);
}

bool MailEditor::apply_paragraph_style(const rw::ParagraphStyle &style, unsigned mask) {
    if (!editing_allowed()) return set_error(error_, "confirm plain-text conversion before editing this draft");
    if (!remember_before_edit(false)) return false;
    const uint64_t revision = editor_.doc.revision;
    if (!editor_.apply_paragraph(style, mask)) {
        pending_snapshot_valid_ = false;
        return set_error(error_, editor_.error.c_str());
    }
    return commit_edit(false, revision != editor_.doc.revision);
}

bool MailEditor::set_link(const char *target, size_t length) {
    if (!editing_allowed()) return set_error(error_, "confirm plain-text conversion before editing this draft");
    if (!safe_link_target(target, length)) return set_error(error_, "link must use a safe HTTP, HTTPS, or mailto address");
    const size_t start = editor_.selection_start(), end = editor_.selection_end();
    if (start == end || end > UINT32_MAX) return set_error(error_, "select text before adding a link");
    Array<MailLinkRange> next;
    if (!next.reserve(links_.size() + 1)) return set_error(error_, "not enough memory to add the link");
    MailLinkRange added;
    added.start = static_cast<uint32_t>(start); added.end = static_cast<uint32_t>(end);
    if (!added.target.set(target, length)) return set_error(error_, "not enough memory to add the link");
    size_t insert_at = 0;
    for (size_t i = 0; i < links_.size(); ++i) {
        if (links_[i].end <= start) {
            if (!next.push(links_[i])) return set_error(error_, "not enough memory to add the link");
            insert_at = next.size();
        } else if (links_[i].start >= end) {
            /* Kept after the new range below. */
        }
    }
    if (!next.insert(insert_at, added)) return set_error(error_, "not enough memory to add the link");
    for (size_t i = 0; i < links_.size(); ++i)
        if (links_[i].start >= end && !next.push(links_[i]))
            return set_error(error_, "not enough memory to add the link");
    if (!remember_before_edit(false)) return false;
    links_.swap(next);
    return commit_edit(false, true);
}

bool MailEditor::remove_links() {
    if (!editing_allowed()) return set_error(error_, "confirm plain-text conversion before editing this draft");
    const size_t start = editor_.selection_start(), end = editor_.selection_end();
    Array<MailLinkRange> next;
    if (!next.reserve(links_.size())) return set_error(error_, "not enough memory to update links");
    bool changed = false;
    for (size_t i = 0; i < links_.size(); ++i) {
        const bool remove = start == end ? start >= links_[i].start && start < links_[i].end :
            links_[i].start < end && links_[i].end > start;
        if (remove) changed = true;
        else if (!next.push(links_[i])) return set_error(error_, "not enough memory to update links");
    }
    if (!changed) { error_.clear(); return true; }
    if (!remember_before_edit(false)) return false;
    links_.swap(next);
    return commit_edit(false, true);
}

bool MailEditor::copy_selection(rw::Document &document, Array<MailLinkRange> &links) {
    const size_t start = editor_.selection_start(), end = editor_.selection_end();
    if (start == end || !editor_.copy_selection(document))
        return set_error(error_, editor_.error.empty() ? "select text before copying" : editor_.error.c_str());
    Array<MailLinkRange> copied;
    if (!copied.reserve(links_.size())) return set_error(error_, "not enough memory to copy links");
    for (size_t i = 0; i < links_.size(); ++i) {
        const size_t clipped_start = links_[i].start > start ? links_[i].start : start;
        const size_t clipped_end = links_[i].end < end ? links_[i].end : end;
        if (clipped_start >= clipped_end) continue;
        MailLinkRange range;
        range.start = static_cast<uint32_t>(clipped_start - start);
        range.end = static_cast<uint32_t>(clipped_end - start);
        range.target = links_[i].target;
        if (!copied.push(static_cast<MailLinkRange &&>(range))) return set_error(error_, "not enough memory to copy links");
    }
    links.swap(copied);
    error_.clear();
    return true;
}

bool MailEditor::select_all() {
    typing_group_open_ = false;
    editor_.select(0, editor_.doc.chars.size());
    error_.clear();
    return true;
}

bool MailEditor::undo() {
    typing_group_open_ = false;
    if (undo_.empty()) return false;
    Snapshot current;
    if (!capture(current) || !redo_.reserve(redo_.size() + 1)) return set_error(error_, "not enough memory to undo");
    Snapshot target = static_cast<Snapshot &&>(undo_[undo_.size() - 1]);
    undo_.erase(undo_.size() - 1);
    const size_t target_cost = target.cost();
    if (!restore(target)) return false;
    history_bytes_ = target_cost > history_bytes_ ? 0 : history_bytes_ - target_cost;
    current.age = next_history_age_++;
    const size_t current_cost = current.cost();
    if (!redo_.push(static_cast<Snapshot &&>(current))) return set_error(error_, "not enough memory to save redo state");
    if (current_cost > SIZE_MAX - history_bytes_) history_bytes_ = SIZE_MAX;
    else history_bytes_ += current_cost;
    trim_history();
    error_.clear();
    return true;
}

bool MailEditor::redo() {
    typing_group_open_ = false;
    if (redo_.empty()) return false;
    Snapshot current;
    if (!capture(current) || !undo_.reserve(undo_.size() + 1)) return set_error(error_, "not enough memory to redo");
    Snapshot target = static_cast<Snapshot &&>(redo_[redo_.size() - 1]);
    redo_.erase(redo_.size() - 1);
    const size_t target_cost = target.cost();
    if (!restore(target)) return false;
    history_bytes_ = target_cost > history_bytes_ ? 0 : history_bytes_ - target_cost;
    current.age = next_history_age_++;
    const size_t current_cost = current.cost();
    if (!undo_.push(static_cast<Snapshot &&>(current))) return set_error(error_, "not enough memory to save undo state");
    if (current_cost > SIZE_MAX - history_bytes_) history_bytes_ = SIZE_MAX;
    else history_bytes_ += current_cost;
    trim_history();
    error_.clear();
    return true;
}

bool MailEditor::find(const char *query, bool match_case, size_t from, size_t &start, size_t &end) const {
    return editor_.find(query, match_case, from, start, end);
}

bool MailEditor::set_rich_format(bool rich) {
    if (!editing_allowed()) return set_error(error_, "confirm plain-text conversion before editing this draft");
    if (rich_format_ == rich) { error_.clear(); return true; }
    if (!remember_before_edit(false)) return false;
    rich_format_ = rich;
    return commit_edit(false, true);
}

bool MailEditor::export_plain(Text &output) const {
    rw::Text word_text;
    if (!rw::document_text(editor_.doc, 0, editor_.doc.chars.size(), word_text))
        return set_error(error_, "could not export plain text");
    if (!output.set(word_text.c_str(), word_text.size()))
        return set_error(error_, "not enough memory to export plain text");
    return true;
}

bool MailEditor::export_safe_html(Text &output) const {
    Buffer raw(MaxHtmlBytes);
    size_t paragraph_index = 0;
    while (paragraph_index < editor_.doc.paragraphs.size()) {
        const rw::Paragraph &paragraph = editor_.doc.paragraphs[paragraph_index];
        const size_t start = paragraph.start;
        const size_t end = paragraph_index + 1 < editor_.doc.paragraphs.size() ?
            editor_.doc.paragraphs[paragraph_index + 1].start - 1 : editor_.doc.chars.size();
        const rw::ListKind list = paragraph.style.list;
        if (list == rw::ListKind::Bullet || list == rw::ListKind::Number) {
            const char *tag = list == rw::ListKind::Bullet ? "ul" : "ol";
            if (!raw.cstr("<") || !raw.cstr(tag) || !raw.cstr(">"))
                return set_error(error_, "formatted HTML exceeds the 1 MiB draft limit");
            while (paragraph_index < editor_.doc.paragraphs.size() &&
                editor_.doc.paragraphs[paragraph_index].style.list == list) {
                const rw::Paragraph &item = editor_.doc.paragraphs[paragraph_index];
                const size_t item_start = item.start;
                const size_t item_end = paragraph_index + 1 < editor_.doc.paragraphs.size() ?
                    editor_.doc.paragraphs[paragraph_index + 1].start - 1 : editor_.doc.chars.size();
                if (!raw.cstr("<li") || !append_paragraph_style(raw, item.style) || !raw.cstr(">") ||
                    !append_inline_html(editor_.doc, links_, item_start, item_end, raw) ||
                    !raw.cstr("</li>"))
                    return set_error(error_, "formatted HTML exceeds the 1 MiB draft limit");
                ++paragraph_index;
            }
            if (!raw.cstr("</") || !raw.cstr(tag) || !raw.cstr(">"))
                return set_error(error_, "formatted HTML exceeds the 1 MiB draft limit");
        } else {
            if (!raw.cstr("<p") || !append_paragraph_style(raw, paragraph.style) || !raw.cstr(">") ||
                !append_inline_html(editor_.doc, links_, start, end, raw) || !raw.cstr("</p>"))
                return set_error(error_, "formatted HTML exceeds the 1 MiB draft limit");
            ++paragraph_index;
        }
    }
    Text source, warning, error;
    if (!raw.finish(source) || !sanitize_html(source.c_str(), source.size(), output, warning, error))
        return set_error(error_, error.empty() ? "could not sanitize formatted HTML" : error.c_str());
    if (!warning.empty()) return set_error(error_, "generated formatting was rejected by the safe HTML policy");
    if (output.size() > MaxHtmlBytes) return set_error(error_, "formatted HTML exceeds the 1 MiB draft limit");
    return true;
}

bool MailEditor::encode_state(Bytes &output, Text &error) const {
    error.clear();
    const rw::Document &document = editor_.doc;
    if (document.chars.size() > rw::MaxChars || document.styles.empty() ||
        document.styles.size() > rw::MaxStyles || document.paragraphs.empty() ||
        document.paragraphs.size() > rw::MaxParagraphs || links_.size() > MaxComposeLinks ||
        editor_.anchor > document.chars.size() || editor_.caret > document.chars.size() ||
        text_bytes_ > MaxComposeTextBytes)
        return set_error(error, "editor state exceeds a configured limit");
    StateWriter writer;
    if (!writer.raw("RMES", 4) || !writer.u16(EditorStateVersion) || !writer.u8(rich_format_ ? 1 : 0) ||
        !writer.u32(static_cast<uint32_t>(text_bytes_)) ||
        !writer.u32(static_cast<uint32_t>(editor_.anchor)) ||
        !writer.u32(static_cast<uint32_t>(editor_.caret)) ||
        !writer.u32(static_cast<uint32_t>(document.chars.size())) ||
        !writer.u16(static_cast<uint16_t>(document.styles.size())) ||
        !writer.u32(static_cast<uint32_t>(document.paragraphs.size())) ||
        !writer.u32(static_cast<uint32_t>(links_.size())))
        return set_error(error, "editor state exceeds its 16 MiB limit");
    for (size_t i = 0; i < document.styles.size(); ++i) {
        const rw::TextStyle &style = document.styles[i];
        if (style.size < 6 || style.size > 96 || style.foreground > 0xffffff || style.background > 0xffffff)
            return set_error(error, "mail font size or color cannot be stored safely");
        const uint8_t flags = (style.bold ? 1u : 0u) | (style.italic ? 2u : 0u) |
            (style.underline ? 4u : 0u) | (style.strike ? 8u : 0u);
        if (!writer.u32(style.foreground) || !writer.u32(style.background) || !writer.u16(style.size) ||
            !writer.u8(static_cast<uint8_t>(style.family)) || !writer.u8(flags))
            return set_error(error, "editor state exceeds its 16 MiB limit");
    }
    const rw::TextStyle &typing = editor_.typing_style;
    if (typing.size < 6 || typing.size > 96 || typing.foreground > 0xffffff || typing.background > 0xffffff)
        return set_error(error, "mail typing style cannot be stored safely");
    const uint8_t typing_flags = (typing.bold ? 1u : 0u) | (typing.italic ? 2u : 0u) |
        (typing.underline ? 4u : 0u) | (typing.strike ? 8u : 0u);
    if (!writer.u32(typing.foreground) || !writer.u32(typing.background) || !writer.u16(typing.size) ||
        !writer.u8(static_cast<uint8_t>(typing.family)) || !writer.u8(typing_flags))
        return set_error(error, "editor state exceeds its 16 MiB limit");
    for (size_t i = 0; i < document.chars.size(); ++i)
        if (!writer.u32(document.chars[i].codepoint) || !writer.u16(document.chars[i].style))
            return set_error(error, "editor state exceeds its 16 MiB limit");
    for (size_t i = 0; i < document.paragraphs.size(); ++i) {
        const rw::Paragraph &paragraph = document.paragraphs[i];
        if (paragraph.start > UINT32_MAX || !writer.u32(static_cast<uint32_t>(paragraph.start)) ||
            !writer.u8(static_cast<uint8_t>(paragraph.style.alignment)) ||
            !writer.u8(static_cast<uint8_t>(paragraph.style.list)) ||
            !writer.u16(static_cast<uint16_t>(paragraph.style.left_indent)) ||
            !writer.u16(static_cast<uint16_t>(paragraph.style.right_indent)) ||
            !writer.u16(static_cast<uint16_t>(paragraph.style.first_indent)) ||
            !writer.u16(paragraph.style.before) || !writer.u16(paragraph.style.after) ||
            !writer.u16(paragraph.style.line_spacing))
            return set_error(error, "editor state exceeds its 16 MiB limit");
    }
    for (size_t i = 0; i < links_.size(); ++i) {
        const MailLinkRange &link = links_[i];
        if (link.start >= link.end || link.end > document.chars.size() ||
            link.target.size() > MaxLinkTargetBytes || !safe_link_target(link.target.c_str(), link.target.size()) ||
            (i && links_[i - 1].end > link.start) ||
            !writer.u32(link.start) || !writer.u32(link.end) ||
            !writer.u16(static_cast<uint16_t>(link.target.size())) ||
            !writer.raw(link.target.c_str(), link.target.size()))
            return set_error(error, "editor contains an invalid or overlapping hyperlink range");
    }
    if (!writer.finish(output)) return set_error(error, "out of memory saving editor state");
    return true;
}

bool compose_reply_draft(const Draft &, const Account &account,
    const Text &reply_to, const Text &from, const Text &to, const Text &cc,
    const Text &subject, const Text &message_id, const Text &references,
    const Text &plain_preview, bool preview_truncated, bool reply_all,
    Draft &draft, Text &error) {
    error.clear();
    Draft next;
    next.account_id = account.id;
    Text primary = reply_to;
    Array<Address> primary_addresses;
    Text parse_error;
    if (primary.empty() || !parse_addresses(primary.c_str(), primary.size(), primary_addresses, parse_error) ||
        primary_addresses.empty()) {
        primary = from;
        primary_addresses.clear();
        if (primary.empty() || !parse_addresses(primary.c_str(), primary.size(), primary_addresses, parse_error) ||
            primary_addresses.empty())
            return set_error(error, "the original message has no valid reply target");
    }
    Buffer to_text(MaxEditableTextBytes), cc_text(MaxEditableTextBytes);
    Array<Address> all_to, all_cc, own, seen_to, seen_cc;
    if (!seen_to.reserve(primary_addresses.size()) ||
        !append_unique_recipients(to_text, primary_addresses, seen_to))
        return set_error(error, "could not build the reply recipient list");
    if (reply_all) {
        if ((!to.empty() && !parse_addresses(to.c_str(), to.size(), all_to, parse_error)) ||
            (!cc.empty() && !parse_addresses(cc.c_str(), cc.size(), all_cc, parse_error)))
            return set_error(error, "the original recipient headers cannot be used for Reply All");
        if (!account.email_address.empty() &&
            !parse_addresses(account.email_address.c_str(), account.email_address.size(), own, parse_error))
            return set_error(error, "the sending account has no valid mailbox address");
        if (!seen_cc.reserve(all_to.size() + all_cc.size() + own.size() + primary_addresses.size()))
            return set_error(error, "could not allocate the reply-all recipient list");
        for (size_t i = 0; i < primary_addresses.size(); ++i)
            if (!seen_cc.push(primary_addresses[i])) return set_error(error, "could not allocate the reply-all recipient list");
        for (size_t i = 0; i < own.size(); ++i)
            if (!seen_cc.push(own[i])) return set_error(error, "could not allocate the reply-all recipient list");
        if (!append_unique_recipients(cc_text, all_to, seen_cc))
            return set_error(error, "could not build the reply-all recipient list");
        if (!append_unique_recipients(cc_text, all_cc, seen_cc))
            return set_error(error, "could not build the reply-all recipient list");
    }
    Text to_value, cc_value;
    if (!to_text.finish(to_value) || !cc_text.finish(cc_value) ||
        !append_prefixed_subject(subject, next.subject) ||
        !set_raw_recipient_inputs(next, to_value.c_str(), to_value.size(),
            cc_value.c_str(), cc_value.size(), "", 0, error))
        return set_error(error, "could not initialize reply headers");
    if (!message_id.empty()) {
        if (!valid_message_id_list(message_id, false)) return set_error(error, "original Message-ID is invalid");
        next.in_reply_to = message_id;
        if (references.empty()) next.references = message_id;
        else {
            if (!valid_message_id_list(references, true)) return set_error(error, "original References header is invalid");
            Buffer joined(MaxEditableTextBytes);
            if (!joined.append(references.c_str(), references.size()) || !joined.cstr(" ") ||
                !joined.append(message_id.c_str(), message_id.size()) || !joined.finish(next.references))
                return set_error(error, "reply References header exceeds its limit");
        }
    }
    if (!append_quoted_body(plain_preview, preview_truncated, next.body))
        return set_error(error, "quoted message exceeds the 1 MiB body limit");
    draft = static_cast<Draft &&>(next);
    error.clear();
    return true;
}

bool compose_forward_draft(const Draft &, const Text &from, const Text &to,
    const Text &cc, const Text &subject, const Text &plain_preview,
    bool preview_truncated, Draft &draft, Text &error) {
    error.clear();
    Draft next;
    if (!set_raw_recipient_inputs(next, "", 0, "", 0, "", 0, error)) return false;
    if (!append_forward_subject(subject, next.subject)) return set_error(error, "forward subject exceeds its limit");
    Buffer body(MaxComposeTextBytes);
    if (!body.cstr("---------- Forwarded message ----------\n") || !body.cstr("From: ") ||
        !append_text(body, from) || !body.cstr("\nTo: ") || !append_text(body, to) ||
        (!cc.empty() && (!body.cstr("\nCc: ") || !append_text(body, cc))) ||
        !body.cstr("\nSubject: ") || !append_text(body, subject) || !body.cstr("\n\n") ||
        (preview_truncated && !body.cstr("[Forwarded preview is incomplete; the original message was truncated.]\n\n")) ||
        !append_text(body, plain_preview) || !body.finish(next.body))
        return set_error(error, "forwarded message exceeds the 1 MiB body limit");
    draft = static_cast<Draft &&>(next);
    return true;
}

} // namespace rmail
