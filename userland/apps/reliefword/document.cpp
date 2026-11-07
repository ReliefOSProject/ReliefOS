#include "document.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace rw {
namespace {

constexpr size_t MaxFileBytes = 16U * 1024U * 1024U;
constexpr size_t IoQuantum = 64U * 1024U;
constexpr size_t MaxPathBytes = 4000;
constexpr uint16_t NativeVersion = 1;

bool set_error(Text &error, const char *message) {
    if (!error.set(message)) error.clear();
    return false;
}

bool set_errno_error(Text &error, const char *operation) {
    const int saved_errno = errno;
    const char *detail = strerror(saved_errno);
    char message[256];
    int n = snprintf(message, sizeof(message), "%s: %s", operation,
                     detail ? detail : "I/O error");
    errno = saved_errno;
    if (n < 0 || (size_t)n >= sizeof(message)) return set_error(error, operation);
    return set_error(error, message);
}

bool valid_file_text(const char *text, size_t length) {
    if ((!text && length) || length > MaxFileBytes ||
        !valid_utf8(text ? text : "", length)) return false;
    for (size_t i = 0; i < length; ++i) if (!text[i]) return false;
    return true;
}

bool progress_call(Progress progress, void *context, size_t done, size_t total) {
    if (!progress) return true;
    unsigned completed = (unsigned)(done > UINT_MAX ? UINT_MAX : done);
    unsigned amount = (unsigned)(total > UINT_MAX ? UINT_MAX : total);
    return progress(context, completed, amount);
}

struct ByteBuffer {
    char *data = nullptr;
    size_t size = 0, capacity = 0;
    ~ByteBuffer() { free(data); }

    bool reserve(size_t requested) {
        if (requested <= capacity) return true;
        if (requested > MaxFileBytes) return false;
        size_t grown = capacity ? capacity : 256;
        while (grown < requested) {
            if (grown > MaxFileBytes / 2) { grown = requested; break; }
            grown *= 2;
        }
        if (grown > MaxFileBytes) grown = requested;
        char *replacement = (char *)realloc(data, grown);
        if (!replacement) return false;
        data = replacement;
        capacity = grown;
        return true;
    }
    bool append(const void *bytes, size_t length) {
        if (length > MaxFileBytes - size || !reserve(size + length)) return false;
        if (length) memcpy(data + size, bytes, length);
        size += length;
        return true;
    }
    bool byte(char value) { return append(&value, 1); }
    bool text(const char *value) { return append(value, strlen(value)); }
};

void store_u16(unsigned char *at, uint16_t value) {
    at[0] = (unsigned char)value;
    at[1] = (unsigned char)(value >> 8);
}

void store_u32(unsigned char *at, uint32_t value) {
    at[0] = (unsigned char)value;
    at[1] = (unsigned char)(value >> 8);
    at[2] = (unsigned char)(value >> 16);
    at[3] = (unsigned char)(value >> 24);
}

void store_u64(unsigned char *at, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) at[i] = (unsigned char)(value >> (8 * i));
}

uint16_t load_u16(const unsigned char *at) {
    return (uint16_t)at[0] | ((uint16_t)at[1] << 8);
}

uint32_t load_u32(const unsigned char *at) {
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) |
        ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}

uint64_t load_u64(const unsigned char *at) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)at[i] << (8 * i);
    return value;
}

int16_t decode_i16(uint16_t value) {
    int32_t signed_value = value <= INT16_MAX ? (int32_t)value : (int32_t)value - 65536;
    return (int16_t)signed_value;
}

bool append_u8(ByteBuffer &out, uint8_t value) { return out.byte((char)value); }
bool append_u16(ByteBuffer &out, uint16_t value) {
    unsigned char bytes[2]; store_u16(bytes, value); return out.append(bytes, sizeof(bytes));
}
bool append_u32(ByteBuffer &out, uint32_t value) {
    unsigned char bytes[4]; store_u32(bytes, value); return out.append(bytes, sizeof(bytes));
}
bool append_u64(ByteBuffer &out, uint64_t value) {
    unsigned char bytes[8]; store_u64(bytes, value); return out.append(bytes, sizeof(bytes));
}

bool append_string(ByteBuffer &out, const Text &value) {
    return value.size() <= UINT32_MAX && append_u32(out, (uint32_t)value.size()) &&
        out.append(value.c_str(), value.size());
}

bool document_ready(const Document &document, Text &error) {
    if (!document.validate(error)) return false;
    if (document.chars.size() > MaxChars || document.paragraphs.size() > MaxParagraphs ||
        !document.styles.size() || document.styles.size() > MaxStyles ||
        document.title.size() > MaxFileBytes || document.author.size() > MaxFileBytes ||
        document.page.header.size() > MaxFileBytes || document.page.footer.size() > MaxFileBytes)
        return set_error(error, "Document exceeds file resource limits");
    if (!valid_file_text(document.title.c_str(), document.title.size()) ||
        !valid_file_text(document.author.c_str(), document.author.size()) ||
        !valid_file_text(document.page.header.c_str(), document.page.header.size()) ||
        !valid_file_text(document.page.footer.c_str(), document.page.footer.size()))
        return set_error(error, "Document metadata is not valid UTF-8 text");
    for (size_t i = 0; i < document.chars.size(); ++i)
        if (!valid_scalar(document.chars[i].codepoint) ||
            document.chars[i].style >= document.styles.size())
            return set_error(error, "Document contains an invalid character or style reference");
    return true;
}

void replace_document(Document &target, Document &source) {
    target.chars.swap(source.chars);
    target.styles.swap(source.styles);
    target.paragraphs.swap(source.paragraphs);
    target.page = static_cast<PageSettings &&>(source.page);
    target.title = static_cast<Text &&>(source.title);
    target.author = static_cast<Text &&>(source.author);
    target.revision = source.revision;
}

bool append_style(ByteBuffer &out, const TextStyle &style) {
    return append_u32(out, style.foreground) && append_u32(out, style.background) &&
        append_u16(out, style.size) && append_u8(out, (uint8_t)style.family) &&
        append_u8(out, style.bold) && append_u8(out, style.italic) &&
        append_u8(out, style.underline) && append_u8(out, style.strike) &&
        append_u8(out, style.highlight);
}

bool append_paragraph(ByteBuffer &out, const Paragraph &paragraph) {
    const ParagraphStyle &style = paragraph.style;
    return paragraph.start <= UINT32_MAX && append_u32(out, (uint32_t)paragraph.start) &&
        append_u8(out, (uint8_t)style.alignment) && append_u8(out, (uint8_t)style.list) &&
        append_u16(out, (uint16_t)style.left_indent) &&
        append_u16(out, (uint16_t)style.right_indent) &&
        append_u16(out, (uint16_t)style.first_indent) && append_u16(out, style.before) &&
        append_u16(out, style.after) && append_u16(out, style.line_spacing);
}

bool encode_native(const Document &document, Text &output, Text &error) {
    if (!document_ready(document, error)) return false;
    ByteBuffer bytes;
    unsigned char header[16] = {'R','W','O','R',0,0,0,0,0,0,0,0,0,0,0,0};
    if (!bytes.append(header, sizeof(header)) || !append_u64(bytes, document.revision) ||
        !append_string(bytes, document.title) || !append_string(bytes, document.author) ||
        !append_u16(bytes, document.page.width) || !append_u16(bytes, document.page.height) ||
        !append_u16(bytes, document.page.left) || !append_u16(bytes, document.page.right) ||
        !append_u16(bytes, document.page.top) || !append_u16(bytes, document.page.bottom) ||
        !append_u8(bytes, document.page.page_numbers ? 1 : 0) ||
        !append_string(bytes, document.page.header) || !append_string(bytes, document.page.footer) ||
        !append_u32(bytes, (uint32_t)document.styles.size()))
        return set_error(error, "Not enough memory to encode document");
    for (size_t i = 0; i < document.styles.size(); ++i)
        if (!append_style(bytes, document.styles[i]))
            return set_error(error, "Not enough memory to encode document styles");
    if (!append_u32(bytes, (uint32_t)document.chars.size()))
        return set_error(error, "Not enough memory to encode document characters");
    for (size_t i = 0; i < document.chars.size(); ++i)
        if (!append_u32(bytes, document.chars[i].codepoint) ||
            !append_u16(bytes, document.chars[i].style))
            return set_error(error, "Not enough memory to encode document characters");
    if (!append_u32(bytes, (uint32_t)document.paragraphs.size()))
        return set_error(error, "Not enough memory to encode paragraphs");
    for (size_t i = 0; i < document.paragraphs.size(); ++i)
        if (!append_paragraph(bytes, document.paragraphs[i]))
            return set_error(error, "Not enough memory to encode paragraphs");

    if (bytes.size < sizeof(header))
        return set_error(error, "Encoded document length is invalid");
    memcpy(bytes.data, "RWOR", 4);
    store_u16((unsigned char *)bytes.data + 4, NativeVersion);
    store_u16((unsigned char *)bytes.data + 6, 0);
    store_u64((unsigned char *)bytes.data + 8, (uint64_t)(bytes.size - sizeof(header)));
    if (!output.set(bytes.data, bytes.size)) return set_error(error, "Not enough memory to return document");
    error.clear();
    return true;
}

struct Reader {
    const unsigned char *bytes = nullptr;
    size_t size = 0, at = 0;
    Text *error = nullptr;

    bool read(void *target, size_t count) {
        if (count > size - at) return set_error(*error, "Document file is truncated");
        if (count) memcpy(target, bytes + at, count);
        at += count;
        return true;
    }
    bool u8(uint8_t &value) { return read(&value, 1); }
    bool u16(uint16_t &value) {
        unsigned char b[2]; if (!read(b, sizeof(b))) return false;
        value = load_u16(b); return true;
    }
    bool u32(uint32_t &value) {
        unsigned char b[4]; if (!read(b, sizeof(b))) return false;
        value = load_u32(b); return true;
    }
    bool u64(uint64_t &value) {
        unsigned char b[8]; if (!read(b, sizeof(b))) return false;
        value = load_u64(b); return true;
    }
    bool string(Text &value) {
        uint32_t length;
        if (!u32(length)) return false;
        if (length > MaxFileBytes || length > size - at ||
            !valid_file_text((const char *)bytes + at, length))
            return set_error(*error, "Document contains invalid or oversized UTF-8 text");
        if (!value.set((const char *)bytes + at, length))
            return set_error(*error, "Not enough memory to read document text");
        at += length;
        return true;
    }
};

bool decode_native(const char *input, size_t length, Document &output, Text &error) {
    if (!input || length < 16 || length > MaxFileBytes)
        return set_error(error, "Native document has an invalid size");
    const unsigned char *bytes = (const unsigned char *)input;
    uint16_t version = load_u16(bytes + 4), flags = load_u16(bytes + 6);
    uint64_t payload = load_u64(bytes + 8);
    if (memcmp(bytes, "RWOR", 4) || flags || payload != length - 16)
        return set_error(error, "Native document signature or length is invalid");
    if (version != NativeVersion) return set_error(error, "Unsupported native document version");

    Document candidate;
    if (!candidate.init()) return set_error(error, "Not enough memory to create document");
    Reader reader{bytes, length, 16, &error};
    uint64_t revision;
    if (!reader.u64(revision) || !reader.string(candidate.title) || !reader.string(candidate.author))
        return false;
    candidate.revision = revision;
    PageSettings page;
    uint8_t page_numbers;
    if (!reader.u16(page.width) || !reader.u16(page.height) || !reader.u16(page.left) ||
        !reader.u16(page.right) || !reader.u16(page.top) || !reader.u16(page.bottom) ||
        !reader.u8(page_numbers) || page_numbers > 1 || !reader.string(page.header) ||
        !reader.string(page.footer)) {
        if (error.empty()) set_error(error, "Invalid page settings in native document");
        return false;
    }
    page.page_numbers = page_numbers != 0;
    candidate.page = static_cast<PageSettings &&>(page);

    uint32_t count;
    if (!reader.u32(count) || !count || count > MaxStyles || !candidate.styles.resize(0) ||
        !candidate.styles.reserve(count)) {
        if (error.empty()) set_error(error, "Invalid native document style count");
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        TextStyle style;
        uint8_t family, flags[5];
        if (!reader.u32(style.foreground) || !reader.u32(style.background) ||
            !reader.u16(style.size) || !reader.u8(family)) return false;
        for (unsigned j = 0; j < 5; ++j) if (!reader.u8(flags[j])) return false;
        if (family > (uint8_t)FontFamily::Mono || flags[0] > 1 || flags[1] > 1 ||
            flags[2] > 1 || flags[3] > 1 || flags[4] > 1) {
            return set_error(error, "Native document contains an invalid text style");
        }
        style.family = (FontFamily)family;
        style.bold = flags[0]; style.italic = flags[1]; style.underline = flags[2];
        style.strike = flags[3]; style.highlight = flags[4];
        if (!candidate.styles.push(style)) return set_error(error, "Not enough memory to read styles");
    }

    if (!reader.u32(count) || count > MaxChars || count > (reader.size - reader.at) / 6 ||
        !candidate.chars.reserve(count)) {
        if (error.empty()) set_error(error, "Invalid native document character count");
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        Glyph glyph;
        if (!reader.u32(glyph.codepoint) || !reader.u16(glyph.style)) return false;
        if (!valid_scalar(glyph.codepoint) || glyph.style >= candidate.styles.size())
            return set_error(error, "Native document contains an invalid character or style reference");
        if (!candidate.chars.push(glyph)) return set_error(error, "Not enough memory to read characters");
    }

    if (!reader.u32(count) || !count || count > MaxParagraphs ||
        count > (reader.size - reader.at) / 18 || !candidate.paragraphs.resize(0) ||
        !candidate.paragraphs.reserve(count)) {
        if (error.empty()) set_error(error, "Invalid native document paragraph count");
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        Paragraph paragraph;
        uint32_t start;
        uint8_t alignment, list;
        uint16_t left, right, first;
        if (!reader.u32(start) || !reader.u8(alignment) || !reader.u8(list) ||
            !reader.u16(left) || !reader.u16(right) || !reader.u16(first) ||
            !reader.u16(paragraph.style.before) || !reader.u16(paragraph.style.after) ||
            !reader.u16(paragraph.style.line_spacing)) return false;
        paragraph.start = start;
        paragraph.style.alignment = (Alignment)alignment;
        paragraph.style.list = (ListKind)list;
        paragraph.style.left_indent = decode_i16(left);
        paragraph.style.right_indent = decode_i16(right);
        paragraph.style.first_indent = decode_i16(first);
        if (alignment > (uint8_t)Alignment::Justify || list > (uint8_t)ListKind::Number)
            return set_error(error, "Native document contains invalid paragraph settings");
        if (!candidate.paragraphs.push(paragraph))
            return set_error(error, "Not enough memory to read paragraphs");
    }
    if (reader.at != reader.size) return set_error(error, "Native document has trailing data");
    if (!document_ready(candidate, error)) return false;
    replace_document(output, candidate);
    error.clear();
    return true;
}

bool build_plain_text(const char *input, size_t length, Document &output, Text &error) {
    if ((!input && length) || length > MaxFileBytes)
        return set_error(error, "Text file exceeds the size limit");
    size_t start = length >= 3 && (unsigned char)input[0] == 0xef &&
        (unsigned char)input[1] == 0xbb && (unsigned char)input[2] == 0xbf ? 3 : 0;
    if (!valid_utf8(input ? input + start : "", length - start))
        return set_error(error, "Text file is not valid UTF-8");

    ByteBuffer normalized;
    for (size_t i = start; i < length; ++i) {
        if (input[i] == '\r') {
            if (!normalized.byte('\n')) return set_error(error, "Not enough memory to normalize text");
            if (i + 1 < length && input[i + 1] == '\n') ++i;
        } else if (!normalized.byte(input[i])) {
            return set_error(error, "Not enough memory to normalize text");
        }
    }

    Array<uint32_t> codepoints;
    if (!decode_utf8(normalized.data ? normalized.data : "", normalized.size, codepoints, error))
        return false;
    size_t paragraph_count = 1;
    for (size_t i = 0; i < codepoints.size(); ++i)
        if (codepoints[i] == '\n') ++paragraph_count;
    if (codepoints.size() > MaxChars || paragraph_count > MaxParagraphs)
        return set_error(error, "Text file exceeds document character limits");

    Document candidate;
    if (!candidate.init()) return set_error(error, "Not enough memory to create document");
    if (!candidate.chars.reserve(codepoints.size()) ||
        !candidate.paragraphs.reserve(paragraph_count))
        return set_error(error, "Not enough memory to import text");
    for (size_t i = 0; i < codepoints.size(); ++i) {
        if (!candidate.chars.push({codepoints[i], 0}))
            return set_error(error, "Not enough memory to import text");
        if (codepoints[i] == '\n') {
            Paragraph next;
            next.start = i + 1;
            if (!candidate.paragraphs.push(next))
                return set_error(error, "Not enough memory to import paragraphs");
        }
    }
    if (!document_ready(candidate, error)) return false;
    replace_document(output, candidate);
    error.clear();
    return true;
}

bool encode_text(const Document &document, Text &output, Text &error) {
    if (!document_ready(document, error)) return false;
    Text text;
    if (!document_text(document, 0, document.chars.size(), text))
        return set_error(error, "Not enough memory to export text");
    if (text.size() > MaxFileBytes) return set_error(error, "Text output exceeds the file limit");
    output = static_cast<Text &&>(text);
    error.clear();
    return true;
}

bool next_utf8(const char *bytes, size_t length, size_t &at, uint32_t &codepoint) {
    if (at >= length) return false;
    const unsigned char first = (unsigned char)bytes[at++];
    if (first < 0x80) { codepoint = first; return true; }
    unsigned needed = first < 0xe0 ? 1 : first < 0xf0 ? 2 : 3;
    codepoint = first & (needed == 1 ? 0x1f : needed == 2 ? 0x0f : 0x07);
    for (unsigned i = 0; i < needed; ++i)
        codepoint = (codepoint << 6) | ((unsigned char)bytes[at++] & 0x3f);
    return true;
}

bool append_html_codepoint(ByteBuffer &out, uint32_t cp, bool attribute) {
    const char *entity = nullptr;
    char numeric[16];
    if (cp == '&') entity = "&amp;";
    else if (cp == '<') entity = "&lt;";
    else if (cp == '>') entity = "&gt;";
    else if (attribute && cp == '"') entity = "&quot;";
    else if (attribute && cp == '\'') entity = "&#39;";
    else if (cp == ' ') entity = "&#32;";
    else if (cp == '\t') entity = "&#9;";
    else if (cp == '\n') entity = "&#10;";
    else if (cp == '\r') entity = "&#13;";
    else if (cp < 0x20 || cp == 0x7f) {
        int n = snprintf(numeric, sizeof(numeric), "&#%u;", (unsigned)cp);
        return n > 0 && (size_t)n < sizeof(numeric) && out.append(numeric, (size_t)n);
    }
    if (entity) return out.text(entity);
    char encoded[4];
    unsigned count = encode_utf8(cp, encoded);
    return count && out.append(encoded, count);
}

bool append_html_string(ByteBuffer &out, const char *text, size_t length, bool attribute) {
    if (!valid_utf8(text ? text : "", length)) return false;
    size_t at = 0;
    while (at < length) {
        uint32_t cp;
        if (!next_utf8(text, length, at, cp) || !append_html_codepoint(out, cp, attribute))
            return false;
    }
    return true;
}

const char *family_css(FontFamily family) {
    switch (family) {
    case FontFamily::Sans: return "sans-serif";
    case FontFamily::Mono: return "monospace";
    default: return "serif";
    }
}

const char *alignment_css(Alignment alignment) {
    switch (alignment) {
    case Alignment::Center: return "center";
    case Alignment::Right: return "right";
    case Alignment::Justify: return "justify";
    default: return "left";
    }
}

bool append_html_style(ByteBuffer &out, const TextStyle &style) {
    char attributes[512];
    int n = snprintf(attributes, sizeof(attributes),
        "<span style=\"font-family:%s;font-size:%upt;color:#%06x;background-color:#%06x;"
        "font-weight:%s;font-style:%s;text-decoration:%s%s\" "
        "data-rw-foreground=\"%08x\" data-rw-background=\"%08x\" "
        "data-rw-highlight=\"%u\">",
        family_css(style.family), (unsigned)style.size,
        (unsigned)(style.foreground & 0xffffff), (unsigned)(style.background & 0xffffff),
        style.bold ? "bold" : "normal", style.italic ? "italic" : "normal",
        style.underline ? "underline" : "none",
        style.strike ? " line-through" : "",
        (unsigned)style.foreground, (unsigned)style.background, (unsigned)style.highlight);
    return n > 0 && (size_t)n < sizeof(attributes) && out.append(attributes, (size_t)n);
}

bool encode_html(const Document &document, Text &output, Text &error) {
    if (!document_ready(document, error)) return false;
    ByteBuffer bytes;
    if (!bytes.text("<!doctype html><html><head><meta charset=\"UTF-8\"><meta name=\"author\" content=\""))
        return set_error(error, "Not enough memory to encode HTML");
    if (!append_html_string(bytes, document.author.c_str(), document.author.size(), true) ||
        !bytes.text("\"><title>") ||
        !append_html_string(bytes, document.title.c_str(), document.title.size(), false) ||
        !bytes.text("</title></head><body data-rw-width=\""))
        return set_error(error, "Not enough memory to encode HTML metadata");
    char body[512];
    int body_length = snprintf(body, sizeof(body),
        "%u\" data-rw-height=\"%u\" data-rw-left=\"%u\" data-rw-right=\"%u\" "
        "data-rw-top=\"%u\" data-rw-bottom=\"%u\" data-rw-page-numbers=\"%u\" "
        "data-rw-header=\"",
        (unsigned)document.page.width, (unsigned)document.page.height,
        (unsigned)document.page.left, (unsigned)document.page.right,
        (unsigned)document.page.top, (unsigned)document.page.bottom,
        document.page.page_numbers ? 1U : 0U);
    if (body_length < 0 || (size_t)body_length >= sizeof(body) ||
        !bytes.append(body, (size_t)body_length) ||
        !append_html_string(bytes, document.page.header.c_str(), document.page.header.size(), true) ||
        !bytes.text("\" data-rw-footer=\"") ||
        !append_html_string(bytes, document.page.footer.c_str(), document.page.footer.size(), true) ||
        !bytes.text("\">") )
        return set_error(error, "Not enough memory to encode HTML page settings");

    for (size_t p = 0; p < document.paragraphs.size(); ++p) {
        const Paragraph &paragraph = document.paragraphs[p];
        size_t begin = paragraph.start;
        size_t end = p + 1 < document.paragraphs.size() ?
            document.paragraphs[p + 1].start - 1 : document.chars.size();
        const ParagraphStyle &style = paragraph.style;
        char opening[768];
        const char *list = style.list == ListKind::Bullet ? "bullet" :
            style.list == ListKind::Number ? "number" : "none";
        int n = snprintf(opening, sizeof(opening),
            "<p style=\"text-align:%s;margin-left:%dpt;margin-right:%dpt;text-indent:%dpt;"
            "margin-top:%upt;margin-bottom:%upt;line-height:%u%%\" "
            "data-rw-list=\"%s\" data-rw-left=\"%d\" data-rw-right=\"%d\" "
            "data-rw-first=\"%d\" data-rw-before=\"%u\" data-rw-after=\"%u\" "
            "data-rw-line=\"%u\">",
            alignment_css(style.alignment), (int)style.left_indent, (int)style.right_indent,
            (int)style.first_indent, (unsigned)style.before, (unsigned)style.after,
            (unsigned)style.line_spacing, list, (int)style.left_indent, (int)style.right_indent,
            (int)style.first_indent, (unsigned)style.before, (unsigned)style.after,
            (unsigned)style.line_spacing);
        if (n < 0 || (size_t)n >= sizeof(opening) || !bytes.append(opening, (size_t)n))
            return set_error(error, "Not enough memory to encode HTML paragraph");
        size_t at = begin;
        while (at < end) {
            uint16_t style_index = document.chars[at].style;
            size_t run_end = at + 1;
            while (run_end < end && document.chars[run_end].style == style_index) ++run_end;
            if (!append_html_style(bytes, document.styles[style_index]))
                return set_error(error, "Not enough memory to encode HTML character style");
            while (at < run_end) {
                if (!append_html_codepoint(bytes, document.chars[at].codepoint, false))
                    return set_error(error, "Not enough memory to encode HTML text");
                ++at;
            }
            if (!bytes.text("</span>")) return set_error(error, "Not enough memory to encode HTML");
        }
        if (!bytes.text("</p>")) return set_error(error, "Not enough memory to encode HTML paragraph");
    }
    if (!bytes.text("</body></html>")) return set_error(error, "Not enough memory to finish HTML");
    if (!output.set(bytes.data ? bytes.data : "", bytes.size))
        return set_error(error, "Not enough memory to return HTML");
    error.clear();
    return true;
}

int ascii_lower(int c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }

bool equal_ascii(const char *a, size_t length, const char *b) {
    size_t n = strlen(b);
    if (length != n) return false;
    for (size_t i = 0; i < n; ++i)
        if (ascii_lower((unsigned char)a[i]) != (unsigned char)b[i]) return false;
    return true;
}

bool starts_ascii(const char *a, size_t length, const char *b) {
    size_t n = strlen(b);
    if (length < n) return false;
    return equal_ascii(a, n, b);
}

bool contains_ascii(const char *text, size_t length, const char *needle) {
    size_t n = strlen(needle);
    if (n > length) return false;
    for (size_t i = 0; i <= length - n; ++i)
        if (equal_ascii(text + i, n, needle)) return true;
    return false;
}

bool ascii_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

void trim_ascii(const char *&text, size_t &length) {
    while (length && ascii_space(*text)) { ++text; --length; }
    while (length && ascii_space(text[length - 1])) --length;
}

struct HtmlAttribute { const char *name = nullptr; size_t name_length = 0; const char *value = nullptr; size_t value_length = 0; };
struct HtmlTag {
    char name[32] = {};
    bool closing = false, self_closing = false;
    HtmlAttribute attributes[32];
    size_t attribute_count = 0;
    size_t after = 0;
};

bool parse_tag(const char *html, size_t length, size_t at, HtmlTag &tag) {
    if (at >= length || html[at] != '<') return false;
    size_t i = at + 1;
    if (i < length && html[i] == '/') { tag.closing = true; ++i; }
    while (i < length && ascii_space(html[i])) ++i;
    size_t name_start = i;
    while (i < length && ((html[i] >= 'A' && html[i] <= 'Z') ||
           (html[i] >= 'a' && html[i] <= 'z') || (html[i] >= '0' && html[i] <= '9') ||
           html[i] == '-' || html[i] == ':' || html[i] == '_')) ++i;
    if (i == name_start) return false;
    size_t name_length = i - name_start;
    size_t saved_name = name_length < sizeof(tag.name) - 1 ? name_length : sizeof(tag.name) - 1;
    for (size_t j = 0; j < saved_name; ++j) tag.name[j] = (char)ascii_lower((unsigned char)html[name_start + j]);
    tag.name[saved_name] = 0;

    while (i < length) {
        while (i < length && ascii_space(html[i])) ++i;
        if (i >= length) return false;
        if (html[i] == '>') { tag.after = i + 1; return true; }
        if (html[i] == '/' && i + 1 < length && html[i + 1] == '>') {
            tag.self_closing = true; tag.after = i + 2; return true;
        }
        if (tag.closing) { ++i; continue; }
        size_t attr_start = i;
        while (i < length && !ascii_space(html[i]) && html[i] != '=' && html[i] != '>' && html[i] != '/') ++i;
        if (i == attr_start) { ++i; continue; }
        size_t attr_length = i - attr_start;
        const char *value = "";
        size_t value_length = 0;
        while (i < length && ascii_space(html[i])) ++i;
        if (i < length && html[i] == '=') {
            ++i;
            while (i < length && ascii_space(html[i])) ++i;
            if (i < length && (html[i] == '\'' || html[i] == '"')) {
                char quote = html[i++];
                size_t value_start = i;
                while (i < length && html[i] != quote) ++i;
                value = html + value_start; value_length = i - value_start;
                if (i < length) ++i;
            } else {
                size_t value_start = i;
                while (i < length && !ascii_space(html[i]) && html[i] != '>') ++i;
                value = html + value_start; value_length = i - value_start;
            }
        }
        if (tag.attribute_count < sizeof(tag.attributes) / sizeof(tag.attributes[0])) {
            HtmlAttribute &attribute = tag.attributes[tag.attribute_count++];
            attribute.name = html + attr_start;
            attribute.name_length = attr_length;
            attribute.value = value;
            attribute.value_length = value_length;
        }
    }
    return false;
}

const HtmlAttribute *find_attribute(const HtmlTag &tag, const char *name) {
    for (size_t i = 0; i < tag.attribute_count; ++i)
        if (equal_ascii(tag.attributes[i].name, tag.attributes[i].name_length, name))
            return &tag.attributes[i];
    return nullptr;
}

bool parse_signed(const char *text, size_t length, int min_value, int max_value, int &value,
                  const char *suffix = nullptr) {
    trim_ascii(text, length);
    size_t suffix_length = suffix ? strlen(suffix) : 0;
    if (suffix_length && length >= suffix_length &&
        equal_ascii(text + length - suffix_length, suffix_length, suffix)) length -= suffix_length;
    if (!length) return false;
    bool negative = text[0] == '-';
    size_t at = negative || text[0] == '+' ? 1 : 0;
    if (at == length) return false;
    int number = 0;
    for (; at < length; ++at) {
        if (text[at] < '0' || text[at] > '9') return false;
        int digit = text[at] - '0';
        if (number > (INT_MAX - digit) / 10) return false;
        number = number * 10 + digit;
    }
    if (negative) number = -number;
    if (number < min_value || number > max_value) return false;
    value = number;
    return true;
}

bool parse_color(const char *text, size_t length, uint32_t &color) {
    trim_ascii(text, length);
    if (length && text[0] == '#') { ++text; --length; }
    if (length != 6 && length != 8) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)text[i];
        unsigned digit = c >= '0' && c <= '9' ? c - '0' :
            ascii_lower(c) >= 'a' && ascii_lower(c) <= 'f' ? ascii_lower(c) - 'a' + 10 : 16;
        if (digit > 15) return false;
        value = (value << 4) | digit;
    }
    color = value;
    return true;
}

void apply_css(const char *css, size_t length, TextStyle &character, ParagraphStyle &paragraph) {
    size_t at = 0;
    while (at < length) {
        size_t end = at;
        while (end < length && css[end] != ';') ++end;
        size_t colon = at;
        while (colon < end && css[colon] != ':') ++colon;
        if (colon < end) {
            const char *name = css + at; size_t name_length = colon - at;
            const char *value = css + colon + 1; size_t value_length = end - colon - 1;
            trim_ascii(name, name_length); trim_ascii(value, value_length);
            int number;
            if (equal_ascii(name, name_length, "font-family")) {
                if (starts_ascii(value, value_length, "sans")) character.family = FontFamily::Sans;
                else if (starts_ascii(value, value_length, "mono")) character.family = FontFamily::Mono;
                else if (starts_ascii(value, value_length, "serif")) character.family = FontFamily::Serif;
            } else if (equal_ascii(name, name_length, "font-size") &&
                       parse_signed(value, value_length, 4, 288, number, "pt")) {
                character.size = (uint16_t)number;
            } else if (equal_ascii(name, name_length, "color")) {
                uint32_t color; if (parse_color(value, value_length, color)) character.foreground = color;
            } else if (equal_ascii(name, name_length, "background-color")) {
                uint32_t color; if (parse_color(value, value_length, color)) character.background = color;
            } else if (equal_ascii(name, name_length, "font-weight")) {
                character.bold = equal_ascii(value, value_length, "bold") ||
                    equal_ascii(value, value_length, "bolder") ||
                    (parse_signed(value, value_length, 600, 1000, number) && number >= 600);
            } else if (equal_ascii(name, name_length, "font-style")) {
                character.italic = equal_ascii(value, value_length, "italic") ||
                    equal_ascii(value, value_length, "oblique");
            } else if (equal_ascii(name, name_length, "text-decoration") ||
                       equal_ascii(name, name_length, "text-decoration-line")) {
                character.underline = starts_ascii(value, value_length, "underline") ||
                    contains_ascii(value, value_length, "underline");
                character.strike = contains_ascii(value, value_length, "line-through");
                if (equal_ascii(value, value_length, "none")) character.underline = character.strike = 0;
            } else if (equal_ascii(name, name_length, "text-align")) {
                if (equal_ascii(value, value_length, "center")) paragraph.alignment = Alignment::Center;
                else if (equal_ascii(value, value_length, "right")) paragraph.alignment = Alignment::Right;
                else if (equal_ascii(value, value_length, "justify")) paragraph.alignment = Alignment::Justify;
                else if (equal_ascii(value, value_length, "left")) paragraph.alignment = Alignment::Left;
            } else if ((equal_ascii(name, name_length, "margin-left") ||
                        equal_ascii(name, name_length, "margin-right") ||
                        equal_ascii(name, name_length, "text-indent") ||
                        equal_ascii(name, name_length, "margin-top") ||
                        equal_ascii(name, name_length, "margin-bottom")) &&
                       parse_signed(value, value_length, -32768, 32767, number, "pt")) {
                if (equal_ascii(name, name_length, "margin-left")) paragraph.left_indent = (int16_t)number;
                else if (equal_ascii(name, name_length, "margin-right")) paragraph.right_indent = (int16_t)number;
                else if (equal_ascii(name, name_length, "text-indent")) paragraph.first_indent = (int16_t)number;
                else if (equal_ascii(name, name_length, "margin-top")) paragraph.before = (uint16_t)(number < 0 ? 0 : number);
                else paragraph.after = (uint16_t)(number < 0 ? 0 : number);
            } else if (equal_ascii(name, name_length, "line-height") &&
                       parse_signed(value, value_length, 1, 1000, number, "%")) {
                paragraph.line_spacing = (uint16_t)number;
            } else if (equal_ascii(name, name_length, "--rw-highlight") &&
                       parse_signed(value, value_length, 0, 1, number)) {
                character.highlight = (uint8_t)number;
            }
        }
        at = end < length ? end + 1 : end;
    }
}

bool append_utf8(ByteBuffer &out, uint32_t cp) {
    if (!valid_scalar(cp)) return false;
    char encoded[4];
    unsigned count = encode_utf8(cp, encoded);
    return count && out.append(encoded, count);
}

bool decode_entity(const char *html, size_t length, size_t &at, uint32_t &codepoint) {
    if (at >= length || html[at] != '&') return false;
    size_t semicolon = at + 1;
    while (semicolon < length && semicolon - at <= 16 && html[semicolon] != ';' &&
           !ascii_space(html[semicolon]) && html[semicolon] != '<' && html[semicolon] != '&') ++semicolon;
    if (semicolon >= length || html[semicolon] != ';') return false;
    const char *entity = html + at + 1;
    size_t entity_length = semicolon - at - 1;
    if (entity_length == 3 && !memcmp(entity, "amp", 3)) codepoint = '&';
    else if (entity_length == 2 && !memcmp(entity, "lt", 2)) codepoint = '<';
    else if (entity_length == 2 && !memcmp(entity, "gt", 2)) codepoint = '>';
    else if (entity_length == 4 && !memcmp(entity, "quot", 4)) codepoint = '"';
    else if (entity_length == 4 && !memcmp(entity, "apos", 4)) codepoint = '\'';
    else if (entity_length == 4 && !memcmp(entity, "nbsp", 4)) codepoint = 0xa0;
    else if (entity_length == 5 && !memcmp(entity, "mdash", 5)) codepoint = 0x2014;
    else if (entity_length == 5 && !memcmp(entity, "ndash", 5)) codepoint = 0x2013;
    else if (entity_length && entity[0] == '#') {
        unsigned base = 10;
        size_t digit_at = 1;
        if (digit_at < entity_length && (entity[digit_at] == 'x' || entity[digit_at] == 'X')) {
            base = 16; ++digit_at;
        }
        if (digit_at == entity_length) return false;
        uint32_t value = 0;
        for (; digit_at < entity_length; ++digit_at) {
            unsigned char c = (unsigned char)entity[digit_at];
            unsigned digit = c >= '0' && c <= '9' ? c - '0' :
                base == 16 && ascii_lower(c) >= 'a' && ascii_lower(c) <= 'f' ? ascii_lower(c) - 'a' + 10 : base;
            if (digit >= base || value > (0x10ffffU - digit) / base) return false;
            value = value * base + digit;
        }
        if (!valid_scalar(value)) return false;
        codepoint = value;
    } else return false;
    at = semicolon + 1;
    return true;
}

bool decode_html_attribute(const HtmlAttribute &attribute, Text &output) {
    ByteBuffer decoded;
    size_t at = 0;
    while (at < attribute.value_length) {
        if (attribute.value[at] == '&') {
            size_t entity_at = at;
            uint32_t cp;
            if (decode_entity(attribute.value, attribute.value_length, at, cp)) {
                if (!append_utf8(decoded, cp)) return false;
            } else {
                at = entity_at + 1;
                if (!decoded.byte('&')) return false;
            }
        } else {
            uint32_t cp;
            if (!next_utf8(attribute.value, attribute.value_length, at, cp) ||
                !append_utf8(decoded, cp)) return false;
        }
    }
    return valid_file_text(decoded.data ? decoded.data : "", decoded.size) &&
        output.set(decoded.data ? decoded.data : "", decoded.size);
}

struct HtmlFrame {
    char name[32] = {};
    TextStyle character;
    ParagraphStyle paragraph;
    ListKind list = ListKind::Plain;
    bool preformatted = false;
    size_t block_content_start = 0;
};

struct HtmlImport {
    Document document;
    Text &error;
    Text *warning;
    TextStyle character;
    ParagraphStyle paragraph;
    ListKind list = ListKind::Plain;
    HtmlFrame frames[128];
    size_t depth = 0;
    bool any_block = false;
    bool paragraph_has_content = false;
    bool empty_semantic_paragraph_pending = false;
    bool preformatted = false;
    bool in_head = false;
    bool in_title = false;
    bool pending_space = false;
    ByteBuffer title_text;

    HtmlImport(Text &target_error, Text *target_warning) : error(target_error), warning(target_warning) {}

    bool warn_unsupported() {
        if (warning && warning->empty() &&
            !warning->set("Some HTML links, tables, or embedded objects were flattened or skipped"))
            return set_error(error, "Not enough memory to record HTML import warning");
        return true;
    }

    bool intern_style(uint16_t &index) {
        for (size_t i = 0; i < document.styles.size(); ++i) {
            if (document.styles[i] == character) { index = (uint16_t)i; return true; }
        }
        if (document.styles.size() >= MaxStyles)
            return set_error(error, "HTML uses too many distinct text styles");
        if (!document.styles.push(character)) return set_error(error, "Not enough memory to import HTML styles");
        index = (uint16_t)(document.styles.size() - 1);
        return true;
    }

    bool append_character(uint32_t codepoint) {
        if (!valid_scalar(codepoint)) return set_error(error, "HTML contains an invalid Unicode scalar");
        if (document.chars.size() >= MaxChars)
            return set_error(error, "HTML exceeds the document character limit");
        uint16_t style;
        if (!intern_style(style) || !document.chars.push({codepoint, style}))
            return set_error(error, "Not enough memory to import HTML text");
        paragraph_has_content = codepoint != '\n';
        if (codepoint == '\n') {
            if (document.paragraphs.size() >= MaxParagraphs)
                return set_error(error, "HTML exceeds the paragraph limit");
            Paragraph next;
            next.start = document.chars.size();
            next.style = paragraph;
            if (!document.paragraphs.push(next))
                return set_error(error, "Not enough memory to import HTML paragraphs");
        }
        return true;
    }

    bool set_current_paragraph() {
        if (!document.paragraphs.size()) return set_error(error, "HTML paragraph state is invalid");
        document.paragraphs[document.paragraphs.size() - 1].style = paragraph;
        return true;
    }

    bool is_semantic_block_name(const char *name) const {
        return !strcmp(name, "p") || !strcmp(name, "li") ||
            !strcmp(name, "blockquote") || !strcmp(name, "pre") ||
            (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]);
    }

    bool begin_block() {
        // Transparent wrappers (notably div around p) must not create an
        // empty paragraph before their first real content.
        if (paragraph_has_content || empty_semantic_paragraph_pending) {
            if (!append_character('\n')) return false;
        }
        empty_semantic_paragraph_pending = false;
        any_block = true;
        return set_current_paragraph();
    }

    bool push_frame(const HtmlTag &tag) {
        if (depth >= sizeof(frames) / sizeof(frames[0]))
            return set_error(error, "HTML nesting exceeds the supported depth");
        HtmlFrame &frame = frames[depth++];
        memcpy(frame.name, tag.name, sizeof(frame.name));
        frame.character = character;
        frame.paragraph = paragraph;
        frame.list = list;
        frame.preformatted = preformatted;
        frame.block_content_start = document.chars.size();
        return true;
    }

    bool close_frame(const char *name) {
        size_t found = depth;
        while (found && strcmp(frames[found - 1].name, name)) --found;
        if (!found) return true;
        while (depth >= found) {
            HtmlFrame &frame = frames[--depth];
            // A block is empty only when it emitted no glyph at all. The
            // current-line flag alone is insufficient because a trailing br
            // emits LF and leaves the new paragraph empty.
            if (is_semantic_block_name(frame.name) &&
                document.chars.size() == frame.block_content_start)
                empty_semantic_paragraph_pending = true;
            character = frame.character;
            paragraph = frame.paragraph;
            list = frame.list;
            preformatted = frame.preformatted;
        }
        return true;
    }

    bool set_text_from_attribute(Text &target, const HtmlTag &tag, const char *name) {
        const HtmlAttribute *attribute = find_attribute(tag, name);
        if (!attribute) return true;
        if (!decode_html_attribute(*attribute, target))
            return set_error(error, "HTML attribute contains invalid text");
        return true;
    }

    bool number_attribute(const HtmlTag &tag, const char *name, int minimum, int maximum, int &out) {
        const HtmlAttribute *attribute = find_attribute(tag, name);
        return !attribute || parse_signed(attribute->value, attribute->value_length,
                                           minimum, maximum, out);
    }

    bool read_body_settings(const HtmlTag &tag) {
        int value;
        if (!number_attribute(tag, "data-rw-width", 100, 5000, value)) return false;
        if (find_attribute(tag, "data-rw-width")) document.page.width = (uint16_t)value;
        if (!number_attribute(tag, "data-rw-height", 100, 5000, value)) return false;
        if (find_attribute(tag, "data-rw-height")) document.page.height = (uint16_t)value;
        if (!number_attribute(tag, "data-rw-left", 0, 2000, value)) return false;
        if (find_attribute(tag, "data-rw-left")) document.page.left = (uint16_t)value;
        if (!number_attribute(tag, "data-rw-right", 0, 2000, value)) return false;
        if (find_attribute(tag, "data-rw-right")) document.page.right = (uint16_t)value;
        if (!number_attribute(tag, "data-rw-top", 0, 2000, value)) return false;
        if (find_attribute(tag, "data-rw-top")) document.page.top = (uint16_t)value;
        if (!number_attribute(tag, "data-rw-bottom", 0, 2000, value)) return false;
        if (find_attribute(tag, "data-rw-bottom")) document.page.bottom = (uint16_t)value;
        if (!number_attribute(tag, "data-rw-page-numbers", 0, 1, value)) return false;
        if (find_attribute(tag, "data-rw-page-numbers")) document.page.page_numbers = value != 0;
        if (!set_text_from_attribute(document.page.header, tag, "data-rw-header") ||
            !set_text_from_attribute(document.page.footer, tag, "data-rw-footer")) return false;
        return true;
    }

    void apply_custom_style(const HtmlTag &tag) {
        const HtmlAttribute *attribute;
        uint32_t color;
        int value;
        if ((attribute = find_attribute(tag, "data-rw-foreground")) &&
            parse_color(attribute->value, attribute->value_length, color)) character.foreground = color;
        if ((attribute = find_attribute(tag, "data-rw-background")) &&
            parse_color(attribute->value, attribute->value_length, color)) character.background = color;
        if ((attribute = find_attribute(tag, "data-rw-highlight")) &&
            parse_signed(attribute->value, attribute->value_length, 0, 1, value))
            character.highlight = (uint8_t)value;
        if ((attribute = find_attribute(tag, "data-rw-list"))) {
            if (equal_ascii(attribute->value, attribute->value_length, "bullet")) paragraph.list = ListKind::Bullet;
            else if (equal_ascii(attribute->value, attribute->value_length, "number")) paragraph.list = ListKind::Number;
            else if (equal_ascii(attribute->value, attribute->value_length, "none")) paragraph.list = ListKind::Plain;
        }
        struct NumericStyle { const char *name; int minimum, maximum; enum Field { Left, Right, First, Before, After, Line } field; };
        static const NumericStyle numeric[] = {
            {"data-rw-left", -32768, 32767, NumericStyle::Left},
            {"data-rw-right", -32768, 32767, NumericStyle::Right},
            {"data-rw-first", -32768, 32767, NumericStyle::First},
            {"data-rw-before", 0, 65535, NumericStyle::Before},
            {"data-rw-after", 0, 65535, NumericStyle::After},
            {"data-rw-line", 1, 1000, NumericStyle::Line}
        };
        for (size_t i = 0; i < sizeof(numeric) / sizeof(numeric[0]); ++i) {
            attribute = find_attribute(tag, numeric[i].name);
            if (!attribute || !parse_signed(attribute->value, attribute->value_length,
                                             numeric[i].minimum, numeric[i].maximum, value)) continue;
            switch (numeric[i].field) {
            case NumericStyle::Left: paragraph.left_indent = (int16_t)value; break;
            case NumericStyle::Right: paragraph.right_indent = (int16_t)value; break;
            case NumericStyle::First: paragraph.first_indent = (int16_t)value; break;
            case NumericStyle::Before: paragraph.before = (uint16_t)value; break;
            case NumericStyle::After: paragraph.after = (uint16_t)value; break;
            case NumericStyle::Line: paragraph.line_spacing = (uint16_t)value; break;
            }
        }
    }

    bool apply_tag(const HtmlTag &tag) {
        if (tag.closing) {
            if (!strcmp(tag.name, "title") && in_title) {
                in_title = false;
                if (!document.title.set(title_text.data ? title_text.data : "", title_text.size))
                    return set_error(error, "Not enough memory to import HTML title");
            }
            if (!strcmp(tag.name, "head")) in_head = false;
            return close_frame(tag.name);
        }

        if (!strcmp(tag.name, "script") || !strcmp(tag.name, "style")) return true;
        if (!strcmp(tag.name, "head")) { in_head = true; return true; }
        if (!strcmp(tag.name, "body")) {
            in_head = false;
            if (!read_body_settings(tag)) return set_error(error, "Invalid HTML page settings");
            return true;
        }
        if (!strcmp(tag.name, "meta")) {
            const HtmlAttribute *name = find_attribute(tag, "name");
            if (name && equal_ascii(name->value, name->value_length, "author"))
                return set_text_from_attribute(document.author, tag, "content");
            return true;
        }
        if (!strcmp(tag.name, "title")) { in_title = true; title_text.size = 0; return true; }
        if (in_head) return true;

        bool structural_unsupported = !strcmp(tag.name, "table") || !strcmp(tag.name, "tr") ||
            !strcmp(tag.name, "td") || !strcmp(tag.name, "th") || !strcmp(tag.name, "img") ||
            !strcmp(tag.name, "a") ||
            !strcmp(tag.name, "iframe") || !strcmp(tag.name, "video") || !strcmp(tag.name, "audio") ||
            !strcmp(tag.name, "svg") || !strcmp(tag.name, "canvas") || !strcmp(tag.name, "object") ||
            !strcmp(tag.name, "embed") || !strcmp(tag.name, "form") || !strcmp(tag.name, "input");
        if (structural_unsupported && !warn_unsupported()) return false;
        if (!strcmp(tag.name, "img") || !strcmp(tag.name, "iframe") || !strcmp(tag.name, "video") ||
            !strcmp(tag.name, "audio") || !strcmp(tag.name, "svg") || !strcmp(tag.name, "canvas") ||
            !strcmp(tag.name, "object") || !strcmp(tag.name, "embed") || !strcmp(tag.name, "input"))
            return true;

        const bool block = !strcmp(tag.name, "p") || !strcmp(tag.name, "div") ||
            !strcmp(tag.name, "li") || !strcmp(tag.name, "blockquote") ||
            !strcmp(tag.name, "pre") ||
            (tag.name[0] == 'h' && tag.name[1] >= '1' && tag.name[1] <= '6' && !tag.name[2]);
        if (!tag.self_closing && strcmp(tag.name, "br") && strcmp(tag.name, "hr") &&
            strcmp(tag.name, "meta") && strcmp(tag.name, "link") && !push_frame(tag)) return false;

        if (!strcmp(tag.name, "ul")) list = ListKind::Bullet;
        else if (!strcmp(tag.name, "ol")) list = ListKind::Number;

        if (block) {
            if (!begin_block()) return false;
            if (!tag.self_closing && depth && !strcmp(frames[depth - 1].name, tag.name))
                frames[depth - 1].block_content_start = document.chars.size();
            paragraph.list = list;
            if (!strcmp(tag.name, "pre")) character.family = FontFamily::Mono;
            if (tag.name[0] == 'h' && tag.name[1] >= '1' && tag.name[1] <= '6' && !tag.name[2]) {
                static const uint16_t sizes[] = {0,24,18,16,14,13,12};
                character.size = sizes[tag.name[1] - '0'];
                character.bold = 1;
            }
            if (!strcmp(tag.name, "li") && list == ListKind::Plain) paragraph.list = ListKind::Bullet;
        }
        if (!strcmp(tag.name, "b") || !strcmp(tag.name, "strong")) character.bold = 1;
        else if (!strcmp(tag.name, "i") || !strcmp(tag.name, "em")) character.italic = 1;
        else if (!strcmp(tag.name, "u")) character.underline = 1;
        else if (!strcmp(tag.name, "s") || !strcmp(tag.name, "del") || !strcmp(tag.name, "strike")) character.strike = 1;
        else if (!strcmp(tag.name, "mark")) character.highlight = 1;

        const HtmlAttribute *css = find_attribute(tag, "style");
        if (css) apply_css(css->value, css->value_length, character, paragraph);
        apply_custom_style(tag);
        if (!strcmp(tag.name, "pre")) preformatted = true;
        if (block && !set_current_paragraph()) return false;
        if (!strcmp(tag.name, "br") || !strcmp(tag.name, "hr")) {
            if (!any_block) any_block = true;
            if (!append_character('\n')) return false;
            if (!set_current_paragraph()) return false;
        }
        return true;
    }

    bool append_text(uint32_t cp, bool preserve_whitespace = false) {
        if (in_title) return append_utf8(title_text, cp) ||
            set_error(error, "Not enough memory to read HTML title");
        if (in_head) return true;
        if (!preserve_whitespace && ascii_space((char)cp) && cp < 0x80) {
            pending_space = document.chars.size() != 0;
            return true;
        }
        if (!any_block && !begin_block()) return false;
        if (empty_semantic_paragraph_pending) {
            if (!append_character('\n')) return false;
            empty_semantic_paragraph_pending = false;
        }
        if (pending_space && document.chars.size() &&
            document.chars[document.chars.size() - 1].codepoint != '\n' &&
            !append_character(' ')) return false;
        pending_space = false;
        return append_character(cp);
    }

    bool parse(const char *html, size_t length) {
        if (!document.init()) return set_error(error, "Not enough memory to create HTML document");
        if (length >= 3 && (unsigned char)html[0] == 0xef &&
            (unsigned char)html[1] == 0xbb && (unsigned char)html[2] == 0xbf) {
            html += 3; length -= 3;
        }
        if (!valid_utf8(html, length)) return set_error(error, "HTML file is not valid UTF-8");
        size_t at = 0;
        while (at < length) {
            if (html[at] == '<' && at + 3 < length && !memcmp(html + at, "<!--", 4)) {
                size_t end = at + 4;
                while (end + 2 < length && memcmp(html + end, "-->", 3)) ++end;
                at = end + 2 < length ? end + 3 : length;
                continue;
            }
            if (html[at] == '<' && at + 1 < length &&
                (html[at + 1] == '!' || html[at + 1] == '?')) {
                size_t end = at + 2;
                while (end < length && html[end] != '>') ++end;
                at = end < length ? end + 1 : length;
                continue;
            }
            if (html[at] == '<') {
                HtmlTag tag;
                if (parse_tag(html, length, at, tag)) {
                    at = tag.after;
                    bool discard_contents = !strcmp(tag.name, "script") || !strcmp(tag.name, "style") ||
                        !strcmp(tag.name, "iframe") || !strcmp(tag.name, "video") ||
                        !strcmp(tag.name, "audio") || !strcmp(tag.name, "svg") ||
                        !strcmp(tag.name, "canvas") || !strcmp(tag.name, "object");
                    if (!tag.closing && discard_contents) {
                        size_t close = at;
                        while (close < length) {
                            if (html[close] == '<') {
                                HtmlTag ending;
                                if (parse_tag(html, length, close, ending) && ending.closing &&
                                    !strcmp(ending.name, tag.name)) { at = ending.after; break; }
                            }
                            ++close;
                        }
                        if (close >= length) at = length;
                        continue;
                    }
                    if (!apply_tag(tag)) return false;
                    continue;
                }
                if (!append_text('<')) return false;
                ++at;
                continue;
            }
            if (html[at] == '&') {
                size_t entity_at = at;
                uint32_t cp;
                if (decode_entity(html, length, at, cp)) {
                    if (!append_text(cp, true)) return false;
                } else {
                    at = entity_at + 1;
                    if (!append_text('&')) return false;
                }
                continue;
            }
            uint32_t cp;
            if (!next_utf8(html, length, at, cp))
                return error.empty() ? set_error(error, "HTML contains invalid UTF-8 text") : false;
            if (preformatted && cp == '\r') {
                cp = '\n';
                if (at < length && html[at] == '\n') ++at;
            }
            if (!append_text(cp, preformatted))
                return error.empty() ? set_error(error, "HTML contains invalid UTF-8 text") : false;
        }
        if (in_title && !document.title.set(title_text.data ? title_text.data : "", title_text.size))
            return set_error(error, "Not enough memory to import HTML title");
        if (!document_ready(document, error)) return false;
        return true;
    }
};

bool decode_html(const char *html, size_t length, Document &output, Text &error, Text *warning) {
    if ((!html && length) || length > MaxFileBytes)
        return set_error(error, "HTML file exceeds the size limit");
    if (warning) warning->clear();
    HtmlImport importer(error, warning);
    if (!importer.parse(html ? html : "", length)) return false;
    replace_document(output, importer.document);
    error.clear();
    return true;
}

bool sync_parent_directory(const char *path, Text &error) {
    size_t length = strlen(path);
    if (!length || length > MaxPathBytes) return set_error(error, "Save path is too long");
    char *directory = (char *)malloc(length + 2);
    if (!directory) return set_error(error, "Not enough memory to sync save directory");
    memcpy(directory, path, length + 1);
    char *slash = strrchr(directory, '/');
    if (!slash) strcpy(directory, ".");
    else if (slash == directory) slash[1] = 0;
    else *slash = 0;
#ifdef O_DIRECTORY
    int fd = open(directory, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
#else
    int fd = open(directory, O_RDONLY | O_CLOEXEC);
#endif
    free(directory);
    if (fd < 0) return set_errno_error(error, "Could not open save directory");
    bool ok = fsync(fd) == 0;
    int saved = errno;
    if (close(fd) < 0 && ok) { saved = errno; ok = false; }
    if (!ok) { errno = saved; return set_errno_error(error, "Could not sync save directory"); }
    return true;
}

bool make_temp_path(const char *path, char **out, Text &error) {
    static uint32_t serial = 0;
    size_t length = strlen(path);
    if (!length || length > MaxPathBytes) return set_error(error, "Save path is too long");
    char *temporary = (char *)malloc(length + 64);
    if (!temporary) return set_error(error, "Not enough memory to save document");
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        uint32_t id = ++serial;
        int n = snprintf(temporary, length + 64, "%s.tmp.%ld.%u", path, (long)getpid(), id);
        if (n < 0 || (size_t)n >= length + 64) break;
        *out = temporary;
        return true;
    }
    free(temporary);
    return set_error(error, "Could not make a temporary save path");
}

bool stat_matches_stamp(const struct stat &st, const FileStamp &stamp) {
    if (!S_ISREG(st.st_mode) || (uint64_t)st.st_dev != stamp.device ||
        (uint64_t)st.st_ino != stamp.inode || (uint64_t)st.st_size != stamp.size) return false;
#if defined(__APPLE__)
    return (int64_t)st.st_mtimespec.tv_sec == stamp.seconds &&
        (int64_t)st.st_mtimespec.tv_nsec == stamp.nanoseconds &&
        (int64_t)st.st_ctimespec.tv_sec == stamp.change_seconds &&
        (int64_t)st.st_ctimespec.tv_nsec == stamp.change_nanoseconds;
#else
    return (int64_t)st.st_mtim.tv_sec == stamp.seconds &&
        (int64_t)st.st_mtim.tv_nsec == stamp.nanoseconds &&
        (int64_t)st.st_ctim.tv_sec == stamp.change_seconds &&
        (int64_t)st.st_ctim.tv_nsec == stamp.change_nanoseconds;
#endif
}

bool make_rollback_copy(const char *path, const FileStamp &expected, char **out,
                        mode_t &original_mode, Text &error, Progress progress,
                        void *context, size_t progress_base, size_t progress_total) {
    *out = nullptr;
    if (!expected.exists) return true;
    int source = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (source < 0) return set_errno_error(error, "Could not open existing save target");
    struct stat source_stat;
    if (fstat(source, &source_stat) < 0) {
        int saved = errno;
        close(source); errno = saved;
        return set_errno_error(error, "Could not inspect existing save target");
    }
    if (!stat_matches_stamp(source_stat, expected)) {
        close(source);
        return set_error(error, "The file changed outside ReliefWord; the original was preserved");
    }
    if (source_stat.st_size < 0 || (uint64_t)source_stat.st_size > MaxFileBytes) {
        close(source);
        return set_error(error, "Existing save target exceeds the rollback size limit");
    }
    original_mode = source_stat.st_mode & 0777;

    size_t path_length = strlen(path);
    if (path_length > MaxPathBytes) { close(source); return set_error(error, "Save path is too long"); }
    char *rollback = (char *)malloc(path_length + 64);
    if (!rollback) { close(source); return set_error(error, "Not enough memory to preserve save target"); }
    static uint32_t serial = 0;
    int target = -1;
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        uint32_t id = ++serial;
        int n = snprintf(rollback, path_length + 64, "%s.rollback.%ld.%u", path, (long)getpid(), id);
        if (n < 0 || (size_t)n >= path_length + 64) break;
        target = open(rollback, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (target >= 0) break;
        if (errno != EEXIST) break;
    }
    if (target < 0) {
        int saved = errno; free(rollback); close(source); errno = saved;
        return set_errno_error(error, "Could not create rollback copy");
    }
    bool ok = fchmod(target, 0600) == 0;
    if (!ok) set_errno_error(error, "Could not secure rollback copy");
    unsigned char buffer[32U * 1024U];
    size_t remaining = (size_t)source_stat.st_size;
    size_t copied = 0, next_progress = IoQuantum;
    while (ok && remaining) {
        size_t request = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        ssize_t got = read(source, buffer, request);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            if (!got) set_error(error, "Existing save target changed while preserving it");
            else set_errno_error(error, "Could not read existing save target");
            ok = false;
            break;
        }
        size_t offset = 0;
        while (offset < (size_t)got) {
            ssize_t written = write(target, buffer + offset, (size_t)got - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) {
                set_errno_error(error, "Could not write rollback copy");
                ok = false;
                break;
            }
            offset += (size_t)written;
        }
        remaining -= (size_t)got;
        copied += (size_t)got;
        if (progress && (copied >= next_progress || copied == (size_t)source_stat.st_size)) {
            if (!progress_call(progress, context, progress_base + copied, progress_total)) {
                set_error(error, "Operation cancelled");
                ok = false;
                break;
            }
            next_progress = copied + IoQuantum;
        }
    }
    struct stat source_after;
    if (ok && fstat(source, &source_after) < 0)
        ok = set_errno_error(error, "Could not recheck existing save target");
    else if (ok && !stat_matches_stamp(source_after, expected))
        ok = set_error(error, "The file changed outside ReliefWord; the original was preserved");
    if (ok && fsync(target) < 0) { set_errno_error(error, "Could not sync rollback copy"); ok = false; }
    int saved = errno;
    if (close(source) < 0 && ok) { set_errno_error(error, "Could not close existing save target"); ok = false; }
    if (close(target) < 0 && ok) { set_errno_error(error, "Could not close rollback copy"); ok = false; }
    else errno = saved;
    FileStamp current;
    if (ok && (!file_stamp(path, current, error) || !same_stamp(expected, current))) {
        if (error.empty()) set_error(error, "The file changed outside ReliefWord; the original was preserved");
        ok = false;
    }
    if (!ok) { unlink(rollback); free(rollback); return false; }
    *out = rollback;
    return true;
}

bool create_directories(const char *path, Text &error) {
    size_t length = strlen(path);
    if (!length || path[0] != '/' || length > MaxPathBytes)
        return set_error(error, "Recovery base directory must be an absolute path");
    char *copy = (char *)malloc(length + 1);
    if (!copy) return set_error(error, "Not enough memory to create recovery directory");
    memcpy(copy, path, length + 1);
    bool ok = true;
    for (size_t i = 1; i <= length; ++i) {
        if (copy[i] && copy[i] != '/') continue;
        if (i == 1) continue;
        char saved = copy[i];
        copy[i] = 0;
        struct stat st;
        if (lstat(copy, &st) < 0) {
            if (errno != ENOENT || mkdir(copy, 0700) < 0) {
                if (errno == EEXIST && lstat(copy, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
                    copy[i] = saved;
                    continue;
                }
                ok = set_errno_error(error, "Could not create recovery directory");
                copy[i] = saved;
                break;
            }
            if (lstat(copy, &st) < 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
                ok = set_error(error, "Recovery path contains a non-directory component");
                copy[i] = saved;
                break;
            }
        } else if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
            ok = set_error(error, "Recovery path contains a non-directory component");
            copy[i] = saved;
            break;
        }
        copy[i] = saved;
    }
    free(copy);
    return ok;
}

bool private_directory(const char *path, Text &error) {
    struct stat st;
    if (lstat(path, &st) < 0) {
        if (errno != ENOENT || mkdir(path, 0700) < 0)
            return set_errno_error(error, "Could not create recovery directory");
        if (lstat(path, &st) < 0) return set_errno_error(error, "Could not inspect recovery directory");
    }
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
        return set_error(error, "Recovery path is not a directory");
    if (st.st_uid != geteuid()) return set_error(error, "Recovery directory belongs to another user");
    if (chmod(path, 0700) < 0) return set_errno_error(error, "Could not secure recovery directory");
    return true;
}

} // namespace

bool file_stamp(const char *path, FileStamp &stamp, Text &error) {
    stamp = FileStamp{};
    if (!path || !*path) return set_error(error, "File path is empty");
    struct stat st;
    if (lstat(path, &st) < 0) {
        if (errno == ENOENT) { error.clear(); return true; }
        return set_errno_error(error, "Could not inspect file");
    }
    if (!S_ISREG(st.st_mode) || S_ISLNK(st.st_mode))
        return set_error(error, "File stamp requires a regular non-symlink file");
    stamp.exists = true;
    stamp.device = (uint64_t)st.st_dev;
    stamp.inode = (uint64_t)st.st_ino;
    stamp.size = (uint64_t)st.st_size;
#if defined(__APPLE__)
    stamp.seconds = (int64_t)st.st_mtimespec.tv_sec;
    stamp.nanoseconds = (int64_t)st.st_mtimespec.tv_nsec;
    stamp.change_seconds = (int64_t)st.st_ctimespec.tv_sec;
    stamp.change_nanoseconds = (int64_t)st.st_ctimespec.tv_nsec;
#else
    stamp.seconds = (int64_t)st.st_mtim.tv_sec;
    stamp.nanoseconds = (int64_t)st.st_mtim.tv_nsec;
    stamp.change_seconds = (int64_t)st.st_ctim.tv_sec;
    stamp.change_nanoseconds = (int64_t)st.st_ctim.tv_nsec;
#endif
    error.clear();
    return true;
}

bool same_stamp(const FileStamp &a, const FileStamp &b) {
    return a.exists == b.exists && (!a.exists ||
        (a.device == b.device && a.inode == b.inode && a.size == b.size &&
         a.seconds == b.seconds && a.nanoseconds == b.nanoseconds &&
         a.change_seconds == b.change_seconds && a.change_nanoseconds == b.change_nanoseconds));
}

bool decode_document(const char *bytes, size_t length, FileFormat format, Document &output,
                     Text &error, Text *warning) {
    if (warning) warning->clear();
    if (length > MaxFileBytes) return set_error(error, "Document file exceeds the 16 MiB limit");
    switch (format) {
    case FileFormat::Native: return decode_native(bytes, length, output, error);
    case FileFormat::Text: return build_plain_text(bytes, length, output, error);
    case FileFormat::Html: return decode_html(bytes, length, output, error, warning);
    }
    return set_error(error, "Unknown document format");
}

bool encode_document(const Document &document, FileFormat format, Text &output, Text &error) {
    switch (format) {
    case FileFormat::Native: return encode_native(document, output, error);
    case FileFormat::Text: return encode_text(document, output, error);
    case FileFormat::Html: return encode_html(document, output, error);
    }
    return set_error(error, "Unknown document format");
}

bool load_document(const char *path, FileFormat format, Document &output, Text &error,
                   Text *warning, Progress progress, void *context) {
    if (warning) warning->clear();
    if (!path || !*path) return set_error(error, "Document path is empty");
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return set_errno_error(error, "Could not open document");
    struct stat st;
    if (fstat(fd, &st) < 0) {
        int saved = errno; close(fd); errno = saved;
        return set_errno_error(error, "Could not inspect document");
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > MaxFileBytes) {
        close(fd);
        return set_error(error, "Document file has an invalid type or exceeds 16 MiB");
    }
    size_t size = (size_t)st.st_size;
    char *bytes = (char *)malloc(size ? size : 1);
    if (!bytes) { close(fd); return set_error(error, "Not enough memory to read document"); }
    bool ok = progress_call(progress, context, 0, size);
    if (!ok) set_error(error, "Operation cancelled");
    size_t offset = 0, next_progress = IoQuantum;
    while (ok && offset < size) {
        size_t request = size - offset;
        if (request > IoQuantum) request = IoQuantum;
        ssize_t got = read(fd, bytes + offset, request);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            if (!got) set_error(error, "Document file was truncated while reading");
            else set_errno_error(error, "Could not read document");
            ok = false;
            break;
        }
        offset += (size_t)got;
        if (progress && (offset >= next_progress || offset == size)) {
            if (!progress_call(progress, context, offset, size)) {
                set_error(error, "Operation cancelled");
                ok = false;
                break;
            }
            next_progress = offset + IoQuantum;
        }
    }
    int saved = errno;
    if (close(fd) < 0 && ok) { errno = saved; set_errno_error(error, "Could not close document"); ok = false; }
    if (ok) ok = decode_document(bytes, size, format, output, error, warning);
    free(bytes);
    if (ok) error.clear();
    return ok;
}

bool save_document(const char *path, const Document &document, FileFormat format, Text &error,
                   const FileStamp *expected, Progress progress, void *context) {
    if (!path || !*path) return set_error(error, "Save path is empty");
    Text encoded;
    if (!encode_document(document, format, encoded, error)) return false;
    FileStamp initial;
    if (!file_stamp(path, initial, error)) return false;
    if (expected && !same_stamp(*expected, initial))
        return set_error(error, "The file changed outside ReliefWord; the original was preserved");
    if (initial.exists && initial.size > MaxFileBytes)
        return set_error(error, "Existing save target exceeds the rollback size limit");
    size_t rollback_size = initial.exists ? (size_t)initial.size : 0;
    size_t progress_total = encoded.size() + rollback_size;
    if (!progress_call(progress, context, 0, progress_total))
        return set_error(error, "Operation cancelled");
    FileStamp after_callback;
    if (!file_stamp(path, after_callback, error)) return false;
    if (!same_stamp(initial, after_callback))
        return set_error(error, "The file changed outside ReliefWord; the original was preserved");

    char *rollback = nullptr;
    mode_t original_mode = 0600;
    if (!make_rollback_copy(path, initial, &rollback, original_mode, error,
                            progress, context, 0, progress_total)) return false;

    char *temporary = nullptr;
    if (!make_temp_path(path, &temporary, error)) {
        if (rollback) { unlink(rollback); free(rollback); }
        return false;
    }
    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        free(temporary);
        if (rollback) { unlink(rollback); free(rollback); }
        return set_errno_error(error, "Could not create temporary document");
    }
    bool ok = fchmod(fd, 0600) == 0;
    if (!ok) set_errno_error(error, "Could not secure temporary document");
    size_t offset = 0, next_progress = IoQuantum;
    while (ok && offset < encoded.size()) {
        size_t request = encoded.size() - offset;
        if (request > 32U * 1024U) request = 32U * 1024U;
        ssize_t written = write(fd, encoded.c_str() + offset, request);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            set_errno_error(error, "Could not write temporary document");
            ok = false;
            break;
        }
        offset += (size_t)written;
        if (progress && (offset >= next_progress || offset == encoded.size())) {
            if (!progress_call(progress, context, rollback_size + offset, progress_total)) {
                set_error(error, "Operation cancelled");
                ok = false;
                break;
            }
            next_progress = offset + IoQuantum;
        }
    }
    if (ok && offset != encoded.size()) ok = set_error(error, "Temporary document write was incomplete");
    if (ok && fsync(fd) < 0) { set_errno_error(error, "Could not sync temporary document"); ok = false; }
    int saved_errno = errno;
    if (close(fd) < 0 && ok) { set_errno_error(error, "Could not close temporary document"); ok = false; }
    else errno = saved_errno;

    FileStamp latest;
    if (ok && !file_stamp(path, latest, error)) ok = false;
    if (ok && !same_stamp(initial, latest))
        ok = set_error(error, "The file changed outside ReliefWord; the original was preserved");
    FileStamp before_rename;
    if (ok && !file_stamp(path, before_rename, error)) ok = false;
    if (ok && !same_stamp(initial, before_rename))
        ok = set_error(error, "The file changed outside ReliefWord; the original was preserved");

    bool renamed = false;
    if (ok) {
        if (rename(temporary, path) < 0) { set_errno_error(error, "Could not replace document"); ok = false; }
        else renamed = true;
    }
    if (renamed && !sync_parent_directory(path, error)) {
        Text commit_error = error;
        bool restored = false;
        if (rollback) {
            int restore_fd = open(rollback, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (restore_fd >= 0) {
                fchmod(restore_fd, original_mode);
                fsync(restore_fd);
                close(restore_fd);
            }
            if (rename(rollback, path) == 0) {
                free(rollback);
                rollback = nullptr;
                restored = true;
            }
        } else if (unlink(path) == 0 || errno == ENOENT) {
            restored = true;
        }
        if (restored) {
            Text ignored;
            sync_parent_directory(path, ignored);
            error = static_cast<Text &&>(commit_error);
        } else {
            set_error(error, "Could not sync the saved document or restore the original");
        }
        ok = false;
    } else if (renamed) {
        ok = true;
        if (rollback) {
            if (unlink(rollback) == 0 || errno == ENOENT) {
                free(rollback);
                rollback = nullptr;
                Text ignored;
                sync_parent_directory(path, ignored);
            }
        }
    }
    if (unlink(temporary) < 0 && errno != ENOENT && ok) {
        set_errno_error(error, "Could not remove temporary document");
        ok = false;
    }
    if (rollback) {
        if (!renamed || ok) unlink(rollback);
        free(rollback);
    }
    free(temporary);
    if (ok) error.clear();
    return ok;
}

bool recovery_directory(Text &output, Text &error) {
    const char *base = getenv("XDG_STATE_HOME");
    char *fallback = nullptr;
    if (!base || !*base || base[0] != '/') {
        const char *home = getenv("HOME");
        if (!home || home[0] != '/') return set_error(error, "No per-user state directory is available");
        size_t length = strlen(home);
        if (length > MaxPathBytes - sizeof("/.local/state"))
            return set_error(error, "State directory path is too long");
        fallback = (char *)malloc(length + sizeof("/.local/state"));
        if (!fallback) return set_error(error, "Not enough memory to locate recovery directory");
        snprintf(fallback, length + sizeof("/.local/state"), "%s/.local/state", home);
        base = fallback;
    }
    if (!create_directories(base, error)) { free(fallback); return false; }
    size_t length = strlen(base);
    if (length > MaxPathBytes - sizeof("/reliefword/recovery")) {
        free(fallback); return set_error(error, "State directory path is too long");
    }
    char *app = (char *)malloc(length + sizeof("/reliefword"));
    char *recovery = (char *)malloc(length + sizeof("/reliefword/recovery"));
    if (!app || !recovery) {
        free(app); free(recovery); free(fallback);
        return set_error(error, "Not enough memory to locate recovery directory");
    }
    snprintf(app, length + sizeof("/reliefword"), "%s/reliefword", base);
    snprintf(recovery, length + sizeof("/reliefword/recovery"), "%s/reliefword/recovery", base);
    bool ok = private_directory(app, error) && private_directory(recovery, error);
    if (ok && !output.set(recovery)) ok = set_error(error, "Not enough memory to return recovery path");
    free(app); free(recovery); free(fallback);
    if (ok) error.clear();
    return ok;
}

bool reserve_recovery_path(Text &output, Text &error) {
    Text directory;
    if (!recovery_directory(directory, error)) return false;
    size_t length = directory.size();
    static const char suffix[] = "/session-XXXXXX";
    if (length > MaxPathBytes - sizeof(suffix) - sizeof(".rword"))
        return set_error(error, "Recovery path is too long");
    size_t capacity = length + sizeof(suffix) + sizeof(".rword");
    char *temporary = (char *)malloc(capacity);
    char *reserved = (char *)malloc(capacity);
    if (!temporary || !reserved) {
        free(temporary); free(reserved);
        return set_error(error, "Not enough memory to reserve recovery file");
    }
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        snprintf(temporary, capacity, "%s%s", directory.c_str(), suffix);
        int fd = mkstemp(temporary);
        if (fd < 0) {
            int saved = errno; free(temporary); free(reserved); errno = saved;
            return set_errno_error(error, "Could not reserve recovery file");
        }
        bool ok = fchmod(fd, 0600) == 0;
        struct stat st;
        if (ok && fstat(fd, &st) < 0) ok = false;
        if (ok && (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0777) != 0600)) {
            errno = EPERM; ok = false;
        }
        int saved_errno = errno;
        if (close(fd) < 0 && ok) { saved_errno = errno; ok = false; }
        if (!ok) {
            unlink(temporary); free(temporary); free(reserved); errno = saved_errno;
            return set_errno_error(error, "Could not secure recovery file");
        }
        size_t temp_length = strlen(temporary);
        memcpy(reserved, temporary, temp_length);
        memcpy(reserved + temp_length, ".rword", sizeof(".rword"));
        if (link(temporary, reserved) < 0) {
            saved_errno = errno;
            if (unlink(temporary) < 0) {
                int cleanup_errno = errno; free(temporary); free(reserved); errno = cleanup_errno;
                return set_errno_error(error, "Could not clean recovery reservation");
            }
            if (saved_errno == EEXIST) continue;
            free(temporary); free(reserved); errno = saved_errno;
            return set_errno_error(error, "Could not create recovery file");
        }
        if (unlink(temporary) < 0) {
            saved_errno = errno; unlink(reserved); free(temporary); free(reserved); errno = saved_errno;
            return set_errno_error(error, "Could not finalize recovery file");
        }
        if (!output.set(reserved)) {
            unlink(reserved); free(temporary); free(reserved);
            return set_error(error, "Not enough memory to return recovery path");
        }
        free(temporary); free(reserved);
        error.clear();
        return true;
    }
    free(temporary); free(reserved);
    return set_error(error, "Could not reserve a unique recovery file");
}

bool lock_recovery_path(const char *path, int &descriptor, Text &error) {
    descriptor = -1;
    if (!path || !*path) return set_error(error, "Recovery path is empty");
    size_t length = strlen(path);
    if (length > MaxPathBytes - sizeof(".lock")) return set_error(error, "Recovery lock path is too long");
    char *lock_path = (char *)malloc(length + sizeof(".lock"));
    if (!lock_path) return set_error(error, "Not enough memory to create recovery lock path");
    memcpy(lock_path, path, length);
    memcpy(lock_path + length, ".lock", sizeof(".lock"));
    bool created = false;
    int fd = open(lock_path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0) created = true;
    else if (errno == EEXIST) fd = open(lock_path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        int saved_errno = errno; free(lock_path); errno = saved_errno;
        return set_errno_error(error, "Could not open recovery lock");
    }
    int failure = 0;
    if (created && fchmod(fd, 0600) < 0) failure = errno;
    struct stat st;
    if (!failure && fstat(fd, &st) < 0) failure = errno;
    if (!failure && (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0777) != 0600)) {
        close(fd); free(lock_path);
        return set_error(error, "Recovery lock must be a private regular file");
    }
    if (failure) {
        close(fd); free(lock_path); errno = failure;
        return set_errno_error(error, "Could not secure recovery lock");
    }
    int result;
    do { result = flock(fd, LOCK_EX | LOCK_NB); } while (result < 0 && errno == EINTR);
    if (result < 0) {
        int saved_errno = errno; close(fd); free(lock_path); errno = saved_errno;
        if (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN)
            return set_error(error, "Recovery file is already in use");
        return set_errno_error(error, "Could not lock recovery file");
    }
    free(lock_path);
    descriptor = fd;
    error.clear();
    return true;
}

void unlock_recovery_path(int &descriptor) {
    int fd = descriptor;
    descriptor = -1;
    if (fd >= 0) close(fd);
}

} // namespace rw
