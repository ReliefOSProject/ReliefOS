#include "model.h"

namespace rw {
namespace {

constexpr size_t HistoryBudget = 64u * 1024u * 1024u;
constexpr uint16_t UnmappedStyle = UINT16_MAX;

bool set_error(Text &error, const char *message) {
    error.set(message, strlen(message));
    return false;
}

bool add_size(size_t a, size_t b, size_t &result) {
    if (b > SIZE_MAX - a) return false;
    result = a + b;
    return true;
}

bool valid_style(const TextStyle &style) {
    return style.foreground <= 0xffffff && style.background <= 0xffffff &&
        style.size >= 4 && style.size <= 288 &&
        static_cast<unsigned>(style.family) <= static_cast<unsigned>(FontFamily::Mono) &&
        style.bold <= 1 && style.italic <= 1 && style.underline <= 1 &&
        style.strike <= 1 && style.highlight <= 1;
}

bool valid_paragraph_style(const ParagraphStyle &style) {
    return static_cast<unsigned>(style.alignment) <= static_cast<unsigned>(Alignment::Justify) &&
        static_cast<unsigned>(style.list) <= static_cast<unsigned>(ListKind::Number) &&
        style.left_indent >= -720 && style.left_indent <= 720 &&
        style.right_indent >= -720 && style.right_indent <= 720 &&
        style.first_indent >= -720 && style.first_indent <= 720 &&
        style.before <= 720 && style.after <= 720 &&
        style.line_spacing >= 50 && style.line_spacing <= 500;
}

bool valid_page(const PageSettings &page) {
    if (page.width < 144 || page.height < 144 || page.width > 2000 || page.height > 2000)
        return false;
    const uint32_t horizontal = static_cast<uint32_t>(page.left) + page.right;
    const uint32_t vertical = static_cast<uint32_t>(page.top) + page.bottom;
    if (horizontal + 72 > page.width || vertical + 72 > page.height) return false;
    if (page.header.size() > 65536 || page.footer.size() > 65536) return false;
    return valid_utf8(page.header.c_str(), page.header.size()) &&
        valid_utf8(page.footer.c_str(), page.footer.size());
}

bool valid_metadata(const Text &text) {
    return text.size() <= 4096 && valid_utf8(text.c_str(), text.size());
}

bool validate_editor_document(Editor &editor) {
    Text validation;
    if (!editor.doc.validate(validation)) return set_error(editor.error, validation.c_str());
    if (editor.anchor > editor.doc.chars.size() || editor.caret > editor.doc.chars.size())
        return set_error(editor.error, "选区位置超出文档范围");
    return true;
}

bool is_space(uint32_t cp) {
    if (cp == 0x20 || (cp >= 0x09 && cp <= 0x0d) || cp == 0x85 || cp == 0xa0 ||
        cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 ||
        cp == 0x2029 || cp == 0x202f || cp == 0x205f || cp == 0x3000) return true;
    return false;
}

bool is_combining(uint32_t cp) {
    return (cp >= 0x0300 && cp <= 0x036f) || (cp >= 0x0483 && cp <= 0x0489) ||
        (cp >= 0x0591 && cp <= 0x05bd) || cp == 0x05bf ||
        (cp >= 0x05c1 && cp <= 0x05c2) || (cp >= 0x05c4 && cp <= 0x05c5) ||
        (cp >= 0x0610 && cp <= 0x061a) || (cp >= 0x064b && cp <= 0x065f) ||
        cp == 0x0670 || (cp >= 0x06d6 && cp <= 0x06ed) || cp == 0x0711 ||
        (cp >= 0x0730 && cp <= 0x074a) || (cp >= 0x07a6 && cp <= 0x07b0) ||
        (cp >= 0x0816 && cp <= 0x082d) || (cp >= 0x08d3 && cp <= 0x0902) ||
        (cp >= 0x093a && cp <= 0x093c) || (cp >= 0x093e && cp <= 0x094f) ||
        (cp >= 0x0951 && cp <= 0x0957) || (cp >= 0x0962 && cp <= 0x0963) ||
        (cp >= 0x1ab0 && cp <= 0x1aff) || (cp >= 0x1dc0 && cp <= 0x1dff) ||
        (cp >= 0x20d0 && cp <= 0x20ff) || (cp >= 0xfe20 && cp <= 0xfe2f);
}

bool is_variation_selector(uint32_t cp) {
    return (cp >= 0xfe00 && cp <= 0xfe0f) || (cp >= 0xe0100 && cp <= 0xe01ef);
}

bool is_emoji_modifier(uint32_t cp) { return cp >= 0x1f3fb && cp <= 0x1f3ff; }
bool is_emoji_tag(uint32_t cp) { return cp >= 0xe0020 && cp <= 0xe007f; }
bool is_extend(uint32_t cp) {
    return is_combining(cp) || is_variation_selector(cp) || is_emoji_modifier(cp) || is_emoji_tag(cp);
}
bool is_regional_indicator(uint32_t cp) { return cp >= 0x1f1e6 && cp <= 0x1f1ff; }

bool is_cjk_ideograph(uint32_t cp) {
    return (cp >= 0x3400 && cp <= 0x4dbf) || (cp >= 0x4e00 && cp <= 0x9fff) ||
        (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0x20000 && cp <= 0x2fa1f) ||
        (cp >= 0x30000 && cp <= 0x323af) || (cp >= 0x3040 && cp <= 0x30ff) ||
        (cp >= 0xac00 && cp <= 0xd7af);
}

bool is_word_run_scalar(uint32_t cp) {
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
        (cp >= '0' && cp <= '9') || cp == '_') return true;
    if ((cp >= 0x00c0 && cp <= 0x02ff) || (cp >= 0x0370 && cp <= 0x052f) ||
        (cp >= 0x0590 && cp <= 0x08ff) || (cp >= 0x0900 && cp <= 0x1fff) ||
        (cp >= 0x2c00 && cp <= 0x2dff) || (cp >= 0xa640 && cp <= 0xa7ff) ||
        (cp >= 0xab30 && cp <= 0xab6f)) return true;
    if ((cp >= 0x2000 && cp <= 0x2bff) || (cp >= 0x1f000 && cp <= 0x1faff)) return false;
    return false;
}

bool decode_one(const char *text, size_t length, size_t &at, uint32_t &cp) {
    if (at >= length) return false;
    const unsigned char first = static_cast<unsigned char>(text[at]);
    if (first <= 0x7f) {
        cp = first;
        ++at;
        return true;
    }
    unsigned count;
    uint32_t value;
    if (first >= 0xc2 && first <= 0xdf) {
        count = 2; value = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
        count = 3; value = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
        count = 4; value = first & 0x07;
    } else {
        return false;
    }
    if (count > length - at) return false;
    for (unsigned i = 1; i < count; ++i) {
        const unsigned char continuation = static_cast<unsigned char>(text[at + i]);
        if ((continuation & 0xc0) != 0x80) return false;
        if (i == 1) {
            if (first == 0xe0 && continuation < 0xa0) return false;
            if (first == 0xed && continuation > 0x9f) return false;
            if (first == 0xf0 && continuation < 0x90) return false;
            if (first == 0xf4 && continuation > 0x8f) return false;
        }
        value = (value << 6) | (continuation & 0x3f);
    }
    if (!valid_scalar(value)) return false;
    at += count;
    cp = value;
    return true;
}

bool normalize_newlines(const Array<uint32_t> &input, Array<uint32_t> &output) {
    Array<uint32_t> next;
    if (!next.reserve(input.size())) return false;
    for (size_t i = 0; i < input.size(); ++i) {
        uint32_t cp = input[i];
        if (cp == '\r') {
            if (i + 1 < input.size() && input[i + 1] == '\n') ++i;
            cp = '\n';
        }
        if (!next.push(cp)) return false;
    }
    output.swap(next);
    return true;
}

uint32_t folded(uint32_t cp, bool match_case) {
    if (!match_case && cp >= 'A' && cp <= 'Z') return cp + ('a' - 'A');
    return cp;
}

bool build_prefix(const Array<uint32_t> &query, bool match_case, Array<size_t> &prefix) {
    if (!query.size() || !prefix.resize(query.size())) return false;
    prefix[0] = 0;
    size_t matched = 0;
    for (size_t i = 1; i < query.size(); ++i) {
        const uint32_t current = folded(query[i], match_case);
        while (matched && current != folded(query[matched], match_case))
            matched = prefix[matched - 1];
        if (current == folded(query[matched], match_case)) ++matched;
        prefix[i] = matched;
    }
    return true;
}

bool collect_matches(const Document &doc, const Array<uint32_t> &query, bool match_case,
                     const Array<size_t> &prefix, Array<size_t> &matches) {
    size_t matched = 0;
    for (size_t i = 0; i < doc.chars.size(); ++i) {
        const uint32_t current = folded(doc.chars[i].codepoint, match_case);
        while (matched && current != folded(query[matched], match_case))
            matched = prefix[matched - 1];
        if (current == folded(query[matched], match_case)) ++matched;
        if (matched == query.size()) {
            if (!matches.push(i + 1 - query.size())) return false;
            matched = 0; // replace-all consumes matches from left to right without overlap.
        }
    }
    return true;
}

TextStyle masked_style(const TextStyle &base, const TextStyle &incoming, unsigned mask) {
    TextStyle result = base;
    if (mask & Size) result.size = incoming.size;
    if (mask & Family) result.family = incoming.family;
    if (mask & Bold) result.bold = incoming.bold;
    if (mask & Italic) result.italic = incoming.italic;
    if (mask & Underline) result.underline = incoming.underline;
    if (mask & Strike) result.strike = incoming.strike;
    if (mask & Foreground) result.foreground = incoming.foreground;
    if (mask & Highlight) {
        result.highlight = incoming.highlight;
        result.background = incoming.background;
    }
    return result;
}

ParagraphStyle masked_paragraph(const ParagraphStyle &base, const ParagraphStyle &incoming,
                                unsigned mask) {
    ParagraphStyle result = base;
    if (mask & ParaAlignment) result.alignment = incoming.alignment;
    if (mask & ParaList) result.list = incoming.list;
    if (mask & ParaIndent) {
        result.left_indent = incoming.left_indent;
        result.right_indent = incoming.right_indent;
        result.first_indent = incoming.first_indent;
    }
    if (mask & ParaSpacing) {
        result.before = incoming.before;
        result.after = incoming.after;
        result.line_spacing = incoming.line_spacing;
    }
    return result;
}

bool page_equal(const PageSettings &a, const PageSettings &b) {
    const bool headers_equal = a.header.size() == b.header.size() &&
        (!a.header.size() || !memcmp(a.header.c_str(), b.header.c_str(), a.header.size()));
    const bool footers_equal = a.footer.size() == b.footer.size() &&
        (!a.footer.size() || !memcmp(a.footer.c_str(), b.footer.c_str(), a.footer.size()));
    return a.width == b.width && a.height == b.height && a.left == b.left &&
        a.right == b.right && a.top == b.top && a.bottom == b.bottom &&
        a.page_numbers == b.page_numbers && headers_equal && footers_equal;
}

bool get_or_add_style(Document &doc, const TextStyle &style, uint16_t &index) {
    for (size_t i = 0; i < doc.styles.size(); ++i) {
        if (doc.styles[i] == style) {
            index = static_cast<uint16_t>(i);
            return true;
        }
    }
    if (doc.styles.size() >= MaxStyles || doc.styles.size() > UINT16_MAX) return false;
    index = static_cast<uint16_t>(doc.styles.size());
    return doc.styles.push(style);
}

bool append_glyphs(Array<Glyph> &destination, const Document &source, size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i)
        if (!destination.push(source.chars[i])) return false;
    return true;
}

bool append_mapped_glyphs(Array<Glyph> &destination, const Document &source,
                          const uint16_t *mapping) {
    for (size_t i = 0; i < source.chars.size(); ++i) {
        const Glyph &from = source.chars[i];
        if (from.style >= source.styles.size() || mapping[from.style] == UnmappedStyle) return false;
        if (!destination.push({from.codepoint, mapping[from.style]})) return false;
    }
    return true;
}

size_t snapshot_cost(const Document &doc) {
    size_t cost = sizeof(Document) + sizeof(size_t) * 2 + sizeof(TextStyle);
    size_t part;
    if (doc.chars.size() > SIZE_MAX / sizeof(Glyph)) return SIZE_MAX;
    part = doc.chars.size() * sizeof(Glyph);
    if (!add_size(cost, part, cost)) return SIZE_MAX;
    if (doc.styles.size() > SIZE_MAX / sizeof(TextStyle)) return SIZE_MAX;
    part = doc.styles.size() * sizeof(TextStyle);
    if (!add_size(cost, part, cost)) return SIZE_MAX;
    if (doc.paragraphs.size() > SIZE_MAX / sizeof(Paragraph)) return SIZE_MAX;
    part = doc.paragraphs.size() * sizeof(Paragraph);
    if (!add_size(cost, part, cost)) return SIZE_MAX;
    if (!add_size(cost, doc.title.size(), cost) || !add_size(cost, doc.author.size(), cost) ||
        !add_size(cost, doc.page.header.size(), cost) || !add_size(cost, doc.page.footer.size(), cost))
        return SIZE_MAX;
    // Array growth can reserve nearly twice its live payload. Budget conservatively.
    if (cost > (SIZE_MAX - 512) / 2) return SIZE_MAX;
    return cost * 2 + 512;
}

} // namespace

bool TextStyle::operator==(const TextStyle &other) const {
    return foreground == other.foreground && background == other.background && size == other.size &&
        family == other.family && bold == other.bold && italic == other.italic &&
        underline == other.underline && strike == other.strike && highlight == other.highlight;
}

bool ParagraphStyle::operator==(const ParagraphStyle &other) const {
    return alignment == other.alignment && list == other.list && left_indent == other.left_indent &&
        right_indent == other.right_indent && first_indent == other.first_indent &&
        before == other.before && after == other.after && line_spacing == other.line_spacing;
}

bool valid_scalar(uint32_t cp) {
    return cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff);
}

bool valid_utf8(const char *text, size_t length) {
    if (length && !text) return false;
    size_t at = 0;
    while (at < length) {
        uint32_t cp;
        if (!decode_one(text, length, at, cp)) return false;
    }
    return true;
}

bool decode_utf8(const char *text, size_t length, Array<uint32_t> &output, Text &error) {
    if (length && !text) return set_error(error, "UTF-8 输入为空");
    Array<uint32_t> next;
    if (!next.reserve(length)) return set_error(error, "内存不足，无法解码 UTF-8");
    size_t at = 0;
    while (at < length) {
        uint32_t cp;
        if (!decode_one(text, length, at, cp)) return set_error(error, "UTF-8 输入无效");
        if (!next.push(cp)) return set_error(error, "内存不足，无法解码 UTF-8");
    }
    output.swap(next);
    error.clear();
    return true;
}

unsigned encode_utf8(uint32_t cp, char output[4]) {
    if (!output || !valid_scalar(cp)) return 0;
    if (cp <= 0x7f) {
        output[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp <= 0x7ff) {
        output[0] = static_cast<char>(0xc0 | (cp >> 6));
        output[1] = static_cast<char>(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp <= 0xffff) {
        output[0] = static_cast<char>(0xe0 | (cp >> 12));
        output[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        output[2] = static_cast<char>(0x80 | (cp & 0x3f));
        return 3;
    }
    output[0] = static_cast<char>(0xf0 | (cp >> 18));
    output[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
    output[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
    output[3] = static_cast<char>(0x80 | (cp & 0x3f));
    return 4;
}

bool document_text(const Document &doc, size_t start, size_t end, Text &output) {
    if (start > end || end > doc.chars.size()) return false;
    size_t bytes = 0;
    for (size_t i = start; i < end; ++i) {
        char encoded[4];
        const unsigned width = encode_utf8(doc.chars[i].codepoint, encoded);
        if (!width || !add_size(bytes, width, bytes)) return false;
    }
    if (!bytes) {
        output.clear();
        return true;
    }
    char *buffer = static_cast<char *>(malloc(bytes));
    if (!buffer) return false;
    size_t at = 0;
    for (size_t i = start; i < end; ++i) {
        char encoded[4];
        const unsigned width = encode_utf8(doc.chars[i].codepoint, encoded);
        memcpy(buffer + at, encoded, width);
        at += width;
    }
    const bool ok = output.set(buffer, bytes);
    free(buffer);
    return ok;
}

bool Document::init() {
    Document next;
    if (!next.styles.push(TextStyle{}) || !next.paragraphs.push(Paragraph{0, ParagraphStyle{}}))
        return false;
    next.revision = 1;
    *this = static_cast<Document &&>(next);
    return true;
}

bool Document::clone_from(const Document &other) {
    Document next;
    if (!next.chars.clone_from(other.chars) || !next.styles.clone_from(other.styles) ||
        !next.paragraphs.clone_from(other.paragraphs)) return false;
    next.page = other.page;
    next.title = other.title;
    next.author = other.author;
    next.revision = other.revision;
    *this = static_cast<Document &&>(next);
    return true;
}

bool Document::validate(Text &error) const {
    if (chars.size() > MaxChars) return set_error(error, "文档字符数超过上限");
    if (!paragraphs.size() || paragraphs.size() > MaxParagraphs)
        return set_error(error, "文档段落数无效");
    if (!styles.size() || styles.size() > MaxStyles)
        return set_error(error, "文档字符样式数无效");
    if (!revision) return set_error(error, "文档版本号无效");
    if (!valid_page(page)) return set_error(error, "页面设置无效");
    if (!valid_metadata(title) || !valid_metadata(author))
        return set_error(error, "文档元信息无效");
    if (paragraphs[0].start != 0) return set_error(error, "首段起点必须为零");
    for (size_t i = 0; i < styles.size(); ++i)
        if (!valid_style(styles[i])) return set_error(error, "字符样式属性无效");
    for (size_t i = 0; i < paragraphs.size(); ++i)
        if (!valid_paragraph_style(paragraphs[i].style))
            return set_error(error, "段落格式属性无效");
    size_t paragraph = 1;
    for (size_t i = 0; i < chars.size(); ++i) {
        const Glyph &glyph = chars[i];
        if (!valid_scalar(glyph.codepoint)) return set_error(error, "文档含无效 Unicode 标量");
        if (glyph.codepoint == '\r') return set_error(error, "文档必须使用 LF 换行");
        if (glyph.style >= styles.size()) return set_error(error, "字符样式索引无效");
        if (glyph.codepoint == '\n') {
            if (paragraph >= paragraphs.size() || paragraphs[paragraph].start != i + 1)
                return set_error(error, "段落起点与 LF 不匹配");
            ++paragraph;
        }
    }
    if (paragraph != paragraphs.size()) return set_error(error, "段落起点数量与 LF 不匹配");
    error.clear();
    return true;
}

size_t Document::paragraph_at(size_t offset) const {
    if (!paragraphs.size()) return 0;
    if (offset > chars.size()) offset = chars.size();
    size_t low = 0, high = paragraphs.size();
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (paragraphs[middle].start <= offset) low = middle + 1;
        else high = middle;
    }
    return low ? low - 1 : 0;
}

const TextStyle &Document::style_at(size_t offset) const {
    static const TextStyle fallback{};
    if (offset < chars.size() && chars[offset].style < styles.size()) return styles[chars[offset].style];
    return styles.size() ? styles[0] : fallback;
}

size_t next_cluster(const Document &doc, size_t offset) {
    const size_t length = doc.chars.size();
    if (offset >= length) return length;
    size_t at = offset + 1;
    const uint32_t first = doc.chars[offset].codepoint;
    if (is_regional_indicator(first)) {
        if (at < length && is_regional_indicator(doc.chars[at].codepoint)) ++at;
        return at;
    }
    while (at < length && is_extend(doc.chars[at].codepoint)) ++at;
    while (at < length && doc.chars[at].codepoint == 0x200d) {
        ++at;
        if (at >= length) break;
        ++at;
        while (at < length && is_extend(doc.chars[at].codepoint)) ++at;
    }
    return at;
}

size_t previous_cluster(const Document &doc, size_t offset) {
    const size_t length = doc.chars.size();
    if (!offset || !length) return 0;
    if (offset > length) offset = length;
    size_t at = offset - 1;
    while (at > 0 && is_extend(doc.chars[at].codepoint)) --at;
    while (at > 0 && doc.chars[at - 1].codepoint == 0x200d) {
        if (at < 2) { at = 0; break; }
        at -= 2;
        while (at > 0 && is_extend(doc.chars[at].codepoint)) --at;
    }
    if (is_regional_indicator(doc.chars[at].codepoint)) {
        size_t run_start = at;
        while (run_start && is_regional_indicator(doc.chars[run_start - 1].codepoint)) --run_start;
        size_t run_count = offset - run_start;
        if (run_count >= 2) return run_start + (run_count % 2 ? run_count - 1 : run_count - 2);
        return run_start;
    }
    return at;
}

Statistics statistics(const Document &doc, size_t start, size_t end) {
    Statistics result;
    const size_t length = doc.chars.size();
    if (start > length) start = length;
    if (end == SIZE_MAX || end > length) end = length;
    if (end < start) end = start;
    result.characters = end - start;
    bool in_word_run = false;
    for (size_t i = start; i < end; ++i) {
        const uint32_t cp = doc.chars[i].codepoint;
        if (!is_space(cp)) ++result.nonspace;
        if (is_cjk_ideograph(cp)) {
            ++result.words;
            in_word_run = false;
        } else if (is_word_run_scalar(cp) || (is_extend(cp) && in_word_run)) {
            if (!in_word_run) ++result.words;
            in_word_run = true;
        } else {
            in_word_run = false;
        }
    }
    if (end == start) {
        result.paragraphs = doc.paragraphs.size() ? 1 : 0;
    } else {
        const size_t first = doc.paragraph_at(start);
        size_t last = doc.paragraph_at(end - 1);
        if (end == length && length && doc.chars[length - 1].codepoint == '\n' &&
            doc.paragraphs.size() > last + 1) ++last;
        result.paragraphs = last >= first ? last - first + 1 : 0;
    }
    return result;
}

struct Editor::History {
    struct Snapshot {
        Document document;
        size_t anchor = 0, caret = 0;
        TextStyle typing_style;
        uint64_t age = 0;
        size_t cost() const { return snapshot_cost(document); }
    };
    Array<Snapshot> undo, redo;
    size_t used = 0;
    uint64_t next_age = 1;

    static bool capture(const Editor &editor, Snapshot &out) {
        Snapshot next;
        if (!next.document.clone_from(editor.doc)) return false;
        next.anchor = editor.anchor;
        next.caret = editor.caret;
        next.typing_style = editor.typing_style;
        out = static_cast<Snapshot &&>(next);
        return true;
    }

    bool push(Array<Snapshot> &array, Snapshot &&snapshot) {
        const size_t bytes = snapshot.cost();
        if (!array.push(static_cast<Snapshot &&>(snapshot))) return false;
        if (bytes > SIZE_MAX - used) used = SIZE_MAX;
        else used += bytes;
        return true;
    }

    void clear(Array<Snapshot> &array) {
        for (size_t i = 0; i < array.size(); ++i) {
            const size_t bytes = array[i].cost();
            used = bytes > used ? 0 : used - bytes;
        }
        array.clear();
    }

    void erase(Array<Snapshot> &array, size_t index) {
        if (index >= array.size()) return;
        const size_t bytes = array[index].cost();
        used = bytes > used ? 0 : used - bytes;
        array.erase(index);
    }

    void trim() {
        while (used > HistoryBudget && (undo.size() || redo.size())) {
            Array<Snapshot> *oldest = nullptr;
            if (!undo.size()) oldest = &redo;
            else if (!redo.size()) oldest = &undo;
            else oldest = undo[0].age <= redo[0].age ? &undo : &redo;
            erase(*oldest, 0);
        }
    }

    bool commit(Editor &editor, Document &&document, size_t new_anchor, size_t new_caret,
                const TextStyle &new_typing_style) {
        const uint64_t high_water = editor.revision_high_water_ > editor.doc.revision
            ? editor.revision_high_water_ : editor.doc.revision;
        if (high_water == UINT64_MAX)
            return set_error(editor.error, "文档版本号已达上限");
        const uint64_t revision = high_water + 1;
        Snapshot previous;
        if (!capture(editor, previous)) return set_error(editor.error, "内存不足，无法保存撤销状态");
        previous.age = next_age;
        if (!push(undo, static_cast<Snapshot &&>(previous)))
            return set_error(editor.error, "内存不足，无法记录编辑历史");
        ++next_age;
        clear(redo);
        trim();
        document.revision = revision;
        editor.revision_high_water_ = revision;
        editor.doc = static_cast<Document &&>(document);
        editor.anchor = new_anchor;
        editor.caret = new_caret;
        editor.typing_style = new_typing_style;
        editor.error.clear();
        return true;
    }
};

Editor::Editor() {
    void *history_memory = malloc(sizeof(History));
    if (history_memory) history_ = new (history_memory, Placement{}) History;
    if (!doc.init()) set_error(error, "内存不足，无法创建文档");
    else if (doc.styles.size()) typing_style = doc.styles[0];
    if (!history_) set_error(error, "内存不足，无法创建撤销历史");
}

Editor::~Editor() {
    if (history_) {
        history_->~History();
        free(history_);
    }
}

void Editor::select(size_t start, size_t end) {
    if (start > doc.chars.size()) start = doc.chars.size();
    if (end > doc.chars.size()) end = doc.chars.size();
    anchor = start;
    caret = end;
    size_t sample = start < end ? start : end;
    if (sample == doc.chars.size() && sample) --sample;
    typing_style = doc.style_at(sample);
}

bool Editor::insert_utf8(const char *text, size_t length) {
    if (!validate_editor_document(*this)) return false;
    if (length > MaxChars * 4u) return set_error(error, "输入字符数超过上限");
    Array<uint32_t> decoded, normalized;
    if (!decode_utf8(text, length, decoded, error)) return false;
    if (!normalize_newlines(decoded, normalized)) return set_error(error, "内存不足，无法规范化换行");
    const size_t start = selection_start(), end = selection_end();
    size_t result_length;
    if (!add_size(doc.chars.size() - (end - start), normalized.size(), result_length) ||
        result_length > MaxChars) return set_error(error, "文档字符数超过上限");
    if (start == end && !normalized.size()) { error.clear(); return true; }
    const size_t lead_paragraph = doc.paragraph_at(start);
    const size_t lead_start = doc.paragraphs[lead_paragraph].start;
    const ParagraphStyle inherited = doc.paragraphs[lead_paragraph].style;
    TextStyle insertion_style = typing_style;
    if (start != end && start < doc.chars.size()) insertion_style = doc.style_at(start);
    if (!valid_style(insertion_style)) return set_error(error, "插入字符样式无效");

    Document next;
    if (!next.clone_from(doc)) return set_error(error, "内存不足，无法建立编辑副本");
    uint16_t insertion_style_index = 0;
    if (normalized.size() && !get_or_add_style(next, insertion_style, insertion_style_index))
        return set_error(error, "字符样式数超过上限或内存不足");
    if (!next.chars.reserve(result_length)) return set_error(error, "内存不足，无法扩展文档");
    next.chars.clear();
    for (size_t i = 0; i < start; ++i)
        if (!next.chars.push(doc.chars[i])) return set_error(error, "内存不足，无法替换文档内容");
    for (size_t i = 0; i < normalized.size(); ++i)
        if (!next.chars.push({normalized[i], insertion_style_index}))
            return set_error(error, "内存不足，无法插入文本");
    for (size_t i = end; i < doc.chars.size(); ++i)
        if (!next.chars.push(doc.chars[i])) return set_error(error, "内存不足，无法保留文档内容");

    size_t inserted_breaks = 0;
    for (size_t i = 0; i < normalized.size(); ++i)
        if (normalized[i] == '\n') ++inserted_breaks;
    size_t later_paragraphs = 0;
    for (size_t i = 0; i < doc.paragraphs.size(); ++i)
        if (doc.paragraphs[i].start > end) ++later_paragraphs;
    size_t before_paragraphs = 0;
    for (size_t i = 0; i < doc.paragraphs.size(); ++i)
        if (doc.paragraphs[i].start < lead_start) ++before_paragraphs;
    size_t paragraph_count = before_paragraphs + 1;
    if (!add_size(paragraph_count, inserted_breaks, paragraph_count) ||
        !add_size(paragraph_count, later_paragraphs, paragraph_count) ||
        paragraph_count > MaxParagraphs || !next.paragraphs.reserve(paragraph_count))
        return set_error(error, "段落数超过上限或内存不足");
    next.paragraphs.clear();
    for (size_t i = 0; i < doc.paragraphs.size(); ++i) {
        if (doc.paragraphs[i].start < lead_start &&
            !next.paragraphs.push(doc.paragraphs[i]))
            return set_error(error, "内存不足，无法重建段落");
    }
    if (!next.paragraphs.push({lead_start, inherited}))
        return set_error(error, "内存不足，无法重建段落");
    size_t last_inserted_break = SIZE_MAX;
    for (size_t i = 0; i < normalized.size(); ++i)
        if (normalized[i] == '\n') last_inserted_break = i;
    const ParagraphStyle suffix_style = end < doc.chars.size()
        ? doc.paragraphs[doc.paragraph_at(end)].style : inherited;
    for (size_t i = 0; i < normalized.size(); ++i) {
        if (normalized[i] == '\n') {
            const ParagraphStyle &split_style = i == last_inserted_break ? suffix_style : inherited;
            if (!next.paragraphs.push({start + i + 1, split_style}))
                return set_error(error, "内存不足，无法重建段落");
        }
    }
    for (size_t i = 0; i < doc.paragraphs.size(); ++i) {
        const Paragraph &paragraph = doc.paragraphs[i];
        if (paragraph.start > end) {
            const size_t mapped = start + normalized.size() + (paragraph.start - end);
            if (!next.paragraphs.push({mapped, paragraph.style}))
                return set_error(error, "内存不足，无法保留段落格式");
        }
    }
    Text validation;
    if (!next.validate(validation)) return set_error(error, validation.c_str());
    const size_t position = start + normalized.size();
    const TextStyle after_style = normalized.size() ? insertion_style : typing_style;
    if (!history_) return set_error(error, "撤销历史不可用");
    return history_->commit(*this, static_cast<Document &&>(next), position, position, after_style);
}

bool Editor::erase(bool backward) {
    if (!validate_editor_document(*this)) return false;
    size_t start = selection_start(), end = selection_end();
    if (start == end) {
        if (backward) {
            if (!start) { error.clear(); return true; }
            start = previous_cluster(doc, start);
        } else {
            if (end >= doc.chars.size()) { error.clear(); return true; }
            end = next_cluster(doc, end);
        }
    }
    const size_t old_anchor = anchor, old_caret = caret;
    const TextStyle old_typing_style = typing_style;
    const uint64_t old_revision = doc.revision;
    anchor = start;
    caret = end;
    if (!insert_utf8("", 0)) {
        anchor = old_anchor;
        caret = old_caret;
        typing_style = old_typing_style;
        return false;
    }
    if (doc.revision != old_revision && history_ && history_->undo.size()) {
        History::Snapshot &previous = history_->undo[history_->undo.size() - 1];
        previous.anchor = old_anchor;
        previous.caret = old_caret;
        previous.typing_style = old_typing_style;
    }
    return true;
}

bool Editor::apply_style(const TextStyle &style, unsigned mask) {
    if (!validate_editor_document(*this)) return false;
    if (mask & ~static_cast<unsigned>(AllStyle)) return set_error(error, "字符格式掩码无效");
    const TextStyle requested = masked_style(typing_style, style, mask);
    if (!valid_style(requested)) return set_error(error, "字符格式属性无效");
    const size_t start = selection_start(), end = selection_end();
    if (start == end) {
        typing_style = requested;
        error.clear();
        return true;
    }
    Document next;
    if (!next.clone_from(doc)) return set_error(error, "内存不足，无法建立编辑副本");
    bool changed = false;
    for (size_t i = start; i < end; ++i) {
        const TextStyle merged = masked_style(doc.styles[doc.chars[i].style], style, mask);
        if (!valid_style(merged)) return set_error(error, "字符格式属性无效");
        uint16_t index;
        if (!get_or_add_style(next, merged, index)) return set_error(error, "字符样式数超过上限或内存不足");
        if (next.chars[i].style != index) {
            next.chars[i].style = index;
            changed = true;
        }
    }
    const TextStyle after_typing = requested;
    if (!changed) {
        typing_style = after_typing;
        error.clear();
        return true;
    }
    if (!history_) return set_error(error, "撤销历史不可用");
    return history_->commit(*this, static_cast<Document &&>(next), anchor, caret, after_typing);
}

bool Editor::apply_paragraph(const ParagraphStyle &style, unsigned mask) {
    if (!validate_editor_document(*this)) return false;
    constexpr unsigned allowed = AllParagraph;
    if (mask & ~allowed) return set_error(error, "段落格式掩码无效");
    const size_t start = selection_start(), end = selection_end();
    const size_t first = doc.paragraph_at(start);
    const size_t last = start == end ? first : doc.paragraph_at(end - 1);
    Document next;
    if (!next.clone_from(doc)) return set_error(error, "内存不足，无法建立编辑副本");
    bool changed = false;
    for (size_t i = first; i <= last; ++i) {
        const ParagraphStyle merged = masked_paragraph(doc.paragraphs[i].style, style, mask);
        if (!valid_paragraph_style(merged)) return set_error(error, "段落格式属性无效");
        if (!(merged == doc.paragraphs[i].style)) {
            next.paragraphs[i].style = merged;
            changed = true;
        }
    }
    if (!changed) { error.clear(); return true; }
    if (!history_) return set_error(error, "撤销历史不可用");
    return history_->commit(*this, static_cast<Document &&>(next), anchor, caret, typing_style);
}

bool Editor::set_page(const PageSettings &page) {
    if (!validate_editor_document(*this)) return false;
    if (!valid_page(page)) return set_error(error, "页面设置无效");
    if (page_equal(doc.page, page)) { error.clear(); return true; }
    Document next;
    if (!next.clone_from(doc)) return set_error(error, "内存不足，无法建立编辑副本");
    next.page = page;
    if (!history_) return set_error(error, "撤销历史不可用");
    return history_->commit(*this, static_cast<Document &&>(next), anchor, caret, typing_style);
}

bool Editor::copy_selection(Document &output) {
    if (&output == &doc || selection_start() == selection_end())
        return set_error(error, "没有可复制的选区");
    if (!validate_editor_document(*this)) return false;
    Text validation;
    const size_t start = selection_start(), end = selection_end();
    Document next;
    if (!next.init()) return set_error(error, "内存不足，无法创建剪贴板内容");
    next.chars.clear();
    next.styles.clear();
    next.paragraphs.clear();
    uint16_t mapping[MaxStyles];
    for (size_t i = 0; i < MaxStyles; ++i) mapping[i] = UnmappedStyle;
    for (size_t i = start; i < end; ++i) {
        const Glyph &glyph = doc.chars[i];
        if (glyph.style >= doc.styles.size()) return set_error(error, "选区字符样式索引无效");
        if (mapping[glyph.style] == UnmappedStyle) {
            if (!next.styles.push(doc.styles[glyph.style]))
                return set_error(error, "内存不足，无法复制字符格式");
            mapping[glyph.style] = static_cast<uint16_t>(next.styles.size() - 1);
        }
        if (!next.chars.push({glyph.codepoint, mapping[glyph.style]}))
            return set_error(error, "内存不足，无法复制选区");
    }
    const size_t first_paragraph = doc.paragraph_at(start);
    if (!next.paragraphs.push({0, doc.paragraphs[first_paragraph].style}))
        return set_error(error, "内存不足，无法复制段落格式");
    for (size_t i = start; i < end; ++i) {
        if (doc.chars[i].codepoint == '\n') {
            const size_t source_next = doc.paragraph_at(i + 1);
            if (!next.paragraphs.push({i - start + 1, doc.paragraphs[source_next].style}))
                return set_error(error, "内存不足，无法复制段落格式");
        }
    }
    next.page = doc.page;
    next.revision = 1;
    if (!next.validate(validation)) return set_error(error, validation.c_str());
    output = static_cast<Document &&>(next);
    error.clear();
    return true;
}

bool Editor::paste_document(const Document &fragment) {
    if (!validate_editor_document(*this)) return false;
    Text validation;
    if (!fragment.validate(validation)) return set_error(error, validation.c_str());
    if (fragment.chars.empty()) return set_error(error, "剪贴板内容为空");
    const size_t start = selection_start(), end = selection_end();
    size_t result_length;
    if (!add_size(doc.chars.size() - (end - start), fragment.chars.size(), result_length) ||
        result_length > MaxChars) return set_error(error, "粘贴后文档字符数超过上限");
    const size_t lead_index = doc.paragraph_at(start);
    const size_t lead_start = doc.paragraphs[lead_index].start;

    Document next;
    if (!next.clone_from(doc)) return set_error(error, "内存不足，无法建立粘贴副本");
    uint16_t mapping[MaxStyles];
    for (size_t i = 0; i < MaxStyles; ++i) mapping[i] = UnmappedStyle;
    for (size_t i = 0; i < fragment.chars.size(); ++i) {
        const uint16_t source = fragment.chars[i].style;
        if (source >= fragment.styles.size()) return set_error(error, "剪贴板样式索引无效");
        if (mapping[source] != UnmappedStyle) continue;
        uint16_t destination;
        if (!get_or_add_style(next, fragment.styles[source], destination))
            return set_error(error, "粘贴后字符样式数超过上限或内存不足");
        mapping[source] = destination;
    }
    if (!next.chars.reserve(result_length)) return set_error(error, "内存不足，无法扩展文档");
    next.chars.clear();
    if (!append_glyphs(next.chars, doc, 0, start) || !append_mapped_glyphs(next.chars, fragment, mapping) ||
        !append_glyphs(next.chars, doc, end, doc.chars.size()))
        return set_error(error, "内存不足，无法粘贴文本");

    size_t before = 0, later = 0;
    for (size_t i = 0; i < doc.paragraphs.size(); ++i) {
        if (doc.paragraphs[i].start < lead_start) ++before;
        if (doc.paragraphs[i].start > end) ++later;
    }
    size_t paragraph_count = before + 1;
    if (!add_size(paragraph_count, fragment.paragraphs.size() - 1, paragraph_count) ||
        !add_size(paragraph_count, later, paragraph_count) || paragraph_count > MaxParagraphs ||
        !next.paragraphs.reserve(paragraph_count))
        return set_error(error, "粘贴后段落数超过上限或内存不足");
    next.paragraphs.clear();
    for (size_t i = 0; i < doc.paragraphs.size(); ++i)
        if (doc.paragraphs[i].start < lead_start && !next.paragraphs.push(doc.paragraphs[i]))
            return set_error(error, "内存不足，无法重建段落");
    if (!next.paragraphs.push({lead_start, fragment.paragraphs[0].style}))
        return set_error(error, "内存不足，无法重建段落");
    for (size_t i = 1; i < fragment.paragraphs.size(); ++i) {
        if (!next.paragraphs.push({start + fragment.paragraphs[i].start, fragment.paragraphs[i].style}))
            return set_error(error, "内存不足，无法重建段落");
    }
    for (size_t i = 0; i < doc.paragraphs.size(); ++i) {
        const Paragraph &paragraph = doc.paragraphs[i];
        if (paragraph.start > end) {
            const size_t mapped_start = start + fragment.chars.size() + (paragraph.start - end);
            if (!next.paragraphs.push({mapped_start, paragraph.style}))
                return set_error(error, "内存不足，无法保留段落格式");
        }
    }
    if (!next.validate(validation)) return set_error(error, validation.c_str());
    const size_t position = start + fragment.chars.size();
    TextStyle after = fragment.style_at(fragment.chars.size() - 1);
    if (!history_) return set_error(error, "撤销历史不可用");
    return history_->commit(*this, static_cast<Document &&>(next), position, position, after);
}

bool Editor::find(const char *query_text, bool match_case, size_t from,
                  size_t &start, size_t &end) const {
    if (!query_text) return false;
    const size_t length = strlen(query_text);
    if (length > MaxChars * 4u) return false;
    Array<uint32_t> query;
    Array<size_t> prefix;
    Text ignored;
    if (!decode_utf8(query_text, length, query, ignored) || query.empty() ||
        !build_prefix(query, match_case, prefix)) return false;
    if (from > doc.chars.size()) from = doc.chars.size();
    if (query.size() > doc.chars.size() - from) return false;
    size_t matched = 0;
    for (size_t at = from; at < doc.chars.size(); ++at) {
        const uint32_t current = folded(doc.chars[at].codepoint, match_case);
        while (matched && current != folded(query[matched], match_case))
            matched = prefix[matched - 1];
        if (current == folded(query[matched], match_case)) ++matched;
        if (matched == query.size()) {
            start = at + 1 - query.size();
            end = at + 1;
            return true;
        }
    }
    return false;
}

bool Editor::replace_all(const char *query_text, const char *replacement_text,
                         bool match_case, unsigned &count) {
    count = 0;
    if (!validate_editor_document(*this)) return false;
    if (!query_text || !replacement_text) return set_error(error, "查找或替换文本为空");
    const size_t query_bytes = strlen(query_text), replacement_bytes = strlen(replacement_text);
    if (query_bytes > MaxChars * 4u || replacement_bytes > MaxChars * 4u)
        return set_error(error, "查找或替换文本超过上限");
    Array<uint32_t> query, replacement_raw, replacement;
    Array<size_t> prefix, match_starts;
    if (!decode_utf8(query_text, query_bytes, query, error) || query.empty())
        return set_error(error, "查找文本必须是非空的有效 UTF-8");
    if (!decode_utf8(replacement_text, replacement_bytes, replacement_raw, error) ||
        !normalize_newlines(replacement_raw, replacement))
        return set_error(error, "替换文本不是有效 UTF-8 或内存不足");
    if (!build_prefix(query, match_case, prefix) ||
        !collect_matches(doc, query, match_case, prefix, match_starts))
        return set_error(error, "内存不足，无法搜索文档");
    const size_t matches = match_starts.size();
    if (!matches) { error.clear(); return true; }
    size_t removed;
    if (matches > SIZE_MAX / query.size()) return set_error(error, "替换结果超出上限");
    removed = matches * query.size();
    if (matches > SIZE_MAX / (replacement.size() ? replacement.size() : 1))
        return set_error(error, "替换结果超出上限");
    const size_t added = matches * replacement.size();
    if (removed > doc.chars.size() || added > SIZE_MAX - (doc.chars.size() - removed) ||
        doc.chars.size() - removed + added > MaxChars)
        return set_error(error, "替换后文档字符数超过上限");
    Document next;
    if (!next.clone_from(doc)) return set_error(error, "内存不足，无法建立替换副本");
    const size_t result_length = doc.chars.size() - removed + added;
    if (!next.chars.reserve(result_length)) return set_error(error, "内存不足，无法扩展文档");
    next.chars.clear();
    next.paragraphs.clear();
    if (!next.paragraphs.push({0, doc.paragraphs[0].style}))
        return set_error(error, "内存不足，无法重建段落");
    size_t at = 0, match_index = 0, final_caret = 0;
    TextStyle replacement_typing_style = typing_style;
    size_t last_replacement_break = SIZE_MAX;
    for (size_t i = 0; i < replacement.size(); ++i)
        if (replacement[i] == '\n') last_replacement_break = i;
    while (at < doc.chars.size()) {
        if (match_index < match_starts.size() && match_starts[match_index] == at) {
            const uint16_t inherited_style = doc.chars[at].style;
            const ParagraphStyle inherited_paragraph = doc.paragraphs[doc.paragraph_at(at)].style;
            const ParagraphStyle suffix_paragraph = doc.paragraphs[doc.paragraph_at(at + query.size())].style;
            replacement_typing_style = doc.styles[inherited_style];
            for (size_t i = 0; i < replacement.size(); ++i) {
                const uint32_t cp = replacement[i];
                if (!next.chars.push({cp, inherited_style}))
                    return set_error(error, "内存不足，无法替换文本");
                if (cp == '\n') {
                    const ParagraphStyle &split_style = i == last_replacement_break
                        ? suffix_paragraph : inherited_paragraph;
                    if (!next.paragraphs.push({next.chars.size(), split_style}))
                        return set_error(error, "内存不足，无法重建替换段落");
                }
            }
            final_caret = next.chars.size();
            ++match_index;
            at += query.size();
        } else {
            const Glyph glyph = doc.chars[at];
            if (!next.chars.push(glyph)) return set_error(error, "内存不足，无法保留文档内容");
            if (glyph.codepoint == '\n') {
                const ParagraphStyle following = doc.paragraphs[doc.paragraph_at(at + 1)].style;
                if (!next.paragraphs.push({next.chars.size(), following}))
                    return set_error(error, "内存不足，无法重建段落");
            }
            ++at;
        }
    }
    const size_t caret = final_caret;
    if (next.paragraphs.size() > MaxParagraphs) return set_error(error, "替换后段落数超过上限");
    Text validation;
    if (!next.validate(validation)) return set_error(error, validation.c_str());
    if (!history_) return set_error(error, "撤销历史不可用");
    if (!history_->commit(*this, static_cast<Document &&>(next), caret, caret,
                          replacement.size() ? replacement_typing_style : typing_style)) return false;
    count = static_cast<unsigned>(matches);
    return true;
}

bool Editor::undo() {
    if (!validate_editor_document(*this)) return false;
    if (!history_ || !history_->undo.size()) return false;
    History::Snapshot current;
    if (!History::capture(*this, current)) return set_error(error, "内存不足，无法撤销编辑");
    current.age = history_->next_age++;
    if (!history_->push(history_->redo, static_cast<History::Snapshot &&>(current)))
        return set_error(error, "内存不足，无法记录重做状态");
    const size_t index = history_->undo.size() - 1;
    History::Snapshot restored = static_cast<History::Snapshot &&>(history_->undo[index]);
    history_->erase(history_->undo, index);
    doc = static_cast<Document &&>(restored.document);
    anchor = restored.anchor;
    caret = restored.caret;
    typing_style = restored.typing_style;
    history_->trim();
    error.clear();
    return true;
}

bool Editor::redo() {
    if (!validate_editor_document(*this)) return false;
    if (!history_ || !history_->redo.size()) return false;
    History::Snapshot current;
    if (!History::capture(*this, current)) return set_error(error, "内存不足，无法重做编辑");
    current.age = history_->next_age++;
    if (!history_->push(history_->undo, static_cast<History::Snapshot &&>(current)))
        return set_error(error, "内存不足，无法记录撤销状态");
    const size_t index = history_->redo.size() - 1;
    History::Snapshot restored = static_cast<History::Snapshot &&>(history_->redo[index]);
    history_->erase(history_->redo, index);
    doc = static_cast<Document &&>(restored.document);
    anchor = restored.anchor;
    caret = restored.caret;
    typing_style = restored.typing_style;
    history_->trim();
    error.clear();
    return true;
}

bool Editor::can_undo() const { return history_ && history_->undo.size(); }
bool Editor::can_redo() const { return history_ && history_->redo.size(); }

bool Editor::replace_document(Document &&replacement) {
    if (&replacement == &doc) { error.clear(); return true; }
    if (!validate_editor_document(*this)) return false;
    Text validation;
    if (!replacement.validate(validation)) return set_error(error, validation.c_str());
    const uint64_t high_water = revision_high_water_ > doc.revision
        ? revision_high_water_ : doc.revision;
    if (high_water == UINT64_MAX)
        return set_error(error, "文档版本号已达上限");
    const uint64_t revision = high_water + 1;
    const TextStyle next_typing = replacement.styles.size() ? replacement.styles[0] : TextStyle{};
    replacement.revision = revision;
    doc = static_cast<Document &&>(replacement);
    anchor = 0;
    caret = 0;
    typing_style = next_typing;
    revision_high_water_ = revision;
    reset_history();
    error.clear();
    return true;
}

void Editor::reset_history() {
    if (history_) {
        history_->clear(history_->undo);
        history_->clear(history_->redo);
    }
}

} // namespace rw
