#include "html.h"
#include <stdio.h>

namespace rmail {
namespace {

bool set_error(Text &error, const char *message) {
    error.set(message);
    return false;
}

class HtmlBuffer {
    char *bytes_ = nullptr;
    size_t size_ = 0;
    size_t capacity_ = 0;
    size_t limit_;
public:
    explicit HtmlBuffer(size_t limit) : limit_(limit) {}
    ~HtmlBuffer() { free(bytes_); }
    size_t size() const { return size_; }
    const char *data() const { return bytes_ ? bytes_ : ""; }
    bool append(const char *bytes, size_t length) {
        if (length > limit_ - size_) return false;
        if (!length) return true;
        if (!bytes || length > SIZE_MAX - size_) return false;
        const size_t required = size_ + length;
        if (required > capacity_) {
            size_t next = capacity_ ? capacity_ : 256;
            while (next < required) {
                if (next > SIZE_MAX / 2) { next = required; break; }
                next *= 2;
            }
            if (next > limit_) next = limit_;
            if (next < required) return false;
            void *next_bytes = realloc(bytes_, next);
            if (!next_bytes) return false;
            bytes_ = static_cast<char *>(next_bytes);
            capacity_ = next;
        }
        memcpy(bytes_ + size_, bytes, length);
        size_ = required;
        return true;
    }
    bool append_byte(char byte) { return append(&byte, 1); }
    bool append_cstr(const char *value) { return append(value, strlen(value)); }
    bool finish(Text &text) const { return text.set(data(), size_); }
};

bool is_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f'; }
unsigned char lower(unsigned char c) { return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + 32) : c; }
bool equal_ci(const char *a, size_t length, const char *b) {
    if (strlen(b) != length) return false;
    for (size_t i = 0; i < length; ++i)
        if (lower(static_cast<unsigned char>(a[i])) != lower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}
bool starts_ci(const char *a, size_t length, const char *prefix) {
    const size_t prefix_length = strlen(prefix);
    return length >= prefix_length && equal_ci(a, prefix_length, prefix);
}

bool append_codepoint(HtmlBuffer &output, uint32_t codepoint) {
    char bytes[4];
    size_t length;
    if (codepoint <= 0x7f) { bytes[0] = static_cast<char>(codepoint); length = 1; }
    else if (codepoint <= 0x7ff) {
        bytes[0] = static_cast<char>(0xc0 | (codepoint >> 6));
        bytes[1] = static_cast<char>(0x80 | (codepoint & 0x3f));
        length = 2;
    } else if (codepoint <= 0xffff && !(codepoint >= 0xd800 && codepoint <= 0xdfff)) {
        bytes[0] = static_cast<char>(0xe0 | (codepoint >> 12));
        bytes[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        bytes[2] = static_cast<char>(0x80 | (codepoint & 0x3f));
        length = 3;
    } else if (codepoint <= 0x10ffff) {
        bytes[0] = static_cast<char>(0xf0 | (codepoint >> 18));
        bytes[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
        bytes[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        bytes[3] = static_cast<char>(0x80 | (codepoint & 0x3f));
        length = 4;
    } else return false;
    return output.append(bytes, length);
}

int hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool decode_entity(const char *bytes, size_t length, size_t &at, HtmlBuffer &output) {
    if (at >= length || bytes[at] != '&') return false;
    size_t end = at + 1;
    while (end < length && end - at <= 16 && bytes[end] != ';' &&
        ((bytes[end] >= 'A' && bytes[end] <= 'Z') || (bytes[end] >= 'a' && bytes[end] <= 'z') ||
         (bytes[end] >= '0' && bytes[end] <= '9') || bytes[end] == '#')) ++end;
    if (end >= length || bytes[end] != ';' || end == at + 1 || end - at > 16) {
        return output.append_byte('&');
    }
    uint32_t codepoint = 0;
    bool known = true;
    if (bytes[at + 1] == '#') {
        size_t digit = at + 2;
        int base = 10;
        if (digit < end && (bytes[digit] == 'x' || bytes[digit] == 'X')) { base = 16; ++digit; }
        if (digit == end) known = false;
        for (; known && digit < end; ++digit) {
            int value = base == 16 ? hex(static_cast<unsigned char>(bytes[digit])) :
                ((bytes[digit] >= '0' && bytes[digit] <= '9') ? bytes[digit] - '0' : -1);
            if (value < 0 || codepoint > (0x10ffffu - static_cast<uint32_t>(value)) / static_cast<uint32_t>(base))
                known = false;
            else codepoint = codepoint * static_cast<uint32_t>(base) + static_cast<uint32_t>(value);
        }
        if (!codepoint || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) known = false;
    } else {
        const char *name = bytes + at + 1;
        const size_t name_length = end - at - 1;
        if (equal_ci(name, name_length, "amp")) codepoint = '&';
        else if (equal_ci(name, name_length, "lt")) codepoint = '<';
        else if (equal_ci(name, name_length, "gt")) codepoint = '>';
        else if (equal_ci(name, name_length, "quot")) codepoint = '"';
        else if (equal_ci(name, name_length, "apos")) codepoint = '\'';
        else if (equal_ci(name, name_length, "colon")) codepoint = ':';
        else if (equal_ci(name, name_length, "sol")) codepoint = '/';
        else if (equal_ci(name, name_length, "bsol")) codepoint = '\\';
        else if (equal_ci(name, name_length, "percnt")) codepoint = '%';
        else if (equal_ci(name, name_length, "tab")) codepoint = '\t';
        else if (equal_ci(name, name_length, "newline")) codepoint = '\n';
        else known = false;
    }
    if (!known) return output.append(bytes + at, end + 1 - at);
    at = end + 1;
    return append_codepoint(output, codepoint);
}

bool decode_entities(const char *bytes, size_t length, HtmlBuffer &output) {
    for (size_t i = 0; i < length;) {
        if (bytes[i] == '&') {
            const size_t before = i;
            if (!decode_entity(bytes, length, i, output)) return false;
            if (i == before) ++i;
        } else if (!output.append_byte(bytes[i++])) return false;
    }
    return true;
}

bool append_escaped_attribute(HtmlBuffer &output, const char *bytes, size_t length, bool &changed) {
    static const char replacement[] = "\xef\xbf\xbd";
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (c == 0x7f || (c < 0x20 && c != '\t' && c != '\r' && c != '\n')) {
            if (!output.append(replacement, sizeof(replacement) - 1)) return false;
            changed = true;
            continue;
        }
        switch (bytes[i]) {
        case '&': if (!output.append_cstr("&amp;")) return false; break;
        case '"': if (!output.append_cstr("&quot;")) return false; break;
        case '<': if (!output.append_cstr("&lt;")) return false; break;
        case '>': if (!output.append_cstr("&gt;")) return false; break;
        default: if (!output.append_byte(bytes[i])) return false;
        }
    }
    return true;
}

bool is_allowed_tag(const char *tag, size_t length) {
    static const char *const names[] = {
        "a", "b", "blockquote", "br", "caption", "code", "dd", "div", "dl", "dt", "em",
        "h1", "h2", "h3", "h4", "h5", "h6", "hr", "i", "img", "li", "ol", "p",
        "font", "pre", "s", "small", "span", "strike", "strong", "sub", "sup", "table", "tbody",
        "td", "th", "thead", "tr", "u", "ul"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (equal_ci(tag, length, names[i])) return true;
    return false;
}

bool is_document_wrapper(const char *tag, size_t length) {
    return equal_ci(tag, length, "html") || equal_ci(tag, length, "body");
}

bool is_void_tag(const char *tag, size_t length) {
    return equal_ci(tag, length, "br") || equal_ci(tag, length, "hr") || equal_ci(tag, length, "img");
}

bool is_blocked_tag(const char *tag, size_t length) {
    static const char *const names[] = {
        "script", "style", "form", "iframe", "frame", "frameset", "object", "embed",
        "svg", "math", "video", "audio", "source", "track", "link", "meta", "base"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (equal_ci(tag, length, names[i])) return true;
    return false;
}

size_t skip_spaces(const char *bytes, size_t length, size_t at) {
    while (at < length && is_space(static_cast<unsigned char>(bytes[at]))) ++at;
    return at;
}

bool parse_tag_end(const char *bytes, size_t length, size_t start, size_t &end) {
    char quote = 0;
    for (size_t i = start; i < length; ++i) {
        if (quote) {
            if (bytes[i] == quote) quote = 0;
        } else if (bytes[i] == '\'' || bytes[i] == '"') quote = bytes[i];
        else if (bytes[i] == '>') { end = i; return true; }
    }
    return false;
}

size_t find_closing_tag(const char *bytes, size_t length, size_t start, const char *tag, size_t tag_length) {
    for (size_t i = start; i + tag_length + 2 <= length; ++i) {
        if (bytes[i] != '<' || bytes[i + 1] != '/') continue;
        bool same = true;
        for (size_t j = 0; j < tag_length; ++j)
            if (lower(static_cast<unsigned char>(bytes[i + 2 + j])) !=
                lower(static_cast<unsigned char>(tag[j]))) { same = false; break; }
        if (!same) continue;
        const size_t after = i + 2 + tag_length;
        if (after < length && !is_space(static_cast<unsigned char>(bytes[after])) && bytes[after] != '>') continue;
        return i;
    }
    return length;
}

bool uri_decode_for_check(const char *bytes, size_t length, HtmlBuffer &decoded) {
    HtmlBuffer entities(MaxEditableTextBytes * 6u);
    if (!decode_entities(bytes, length, entities)) return false;
    for (size_t i = 0; i < entities.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(entities.data()[i]);
        if (c == '%' && i + 2 < entities.size()) {
            const int hi = hex(static_cast<unsigned char>(entities.data()[i + 1]));
            const int lo = hex(static_cast<unsigned char>(entities.data()[i + 2]));
            if (hi >= 0 && lo >= 0) {
                const char value = static_cast<char>((hi << 4) | lo);
                if (!decoded.append_byte(value)) return false;
                i += 2;
                continue;
            }
        }
        if (!decoded.append_byte(static_cast<char>(c))) return false;
    }
    return true;
}

bool valid_cid(const char *bytes, size_t length) {
    if (length < 5 || !starts_ci(bytes, length, "cid:")) return false;
    if (length == 4) return false;
    for (size_t i = 4; i < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '+' ||
            c == '@' || c == '!')) return false;
    }
    return true;
}

bool safe_href(const char *bytes, size_t length) {
    if (!length || length > MaxEditableTextBytes) return false;
    HtmlBuffer normalized(MaxEditableTextBytes * 6u);
    if (!uri_decode_for_check(bytes, length, normalized)) return false;
    HtmlBuffer compact(MaxEditableTextBytes * 6u);
    for (size_t i = 0; i < normalized.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(normalized.data()[i]);
        if (c == '\\' || c == 0x7f) return false;
        if (c < 0x20 || is_space(c)) continue;
        if (!compact.append_byte(static_cast<char>(lower(c)))) return false;
    }
    if (!compact.size()) return false;
    size_t colon = 0;
    while (colon < compact.size() && compact.data()[colon] != ':' &&
        compact.data()[colon] != '/' && compact.data()[colon] != '?' && compact.data()[colon] != '#') ++colon;
    if (colon < compact.size() && compact.data()[colon] == ':') {
        if (colon == 4 && !memcmp(compact.data(), "http", 4))
            return colon + 2 < compact.size() && compact.data()[colon + 1] == '/' && compact.data()[colon + 2] == '/';
        if (colon == 5 && !memcmp(compact.data(), "https", 5))
            return colon + 2 < compact.size() && compact.data()[colon + 1] == '/' && compact.data()[colon + 2] == '/';
        if (colon == 6 && !memcmp(compact.data(), "mailto", 6))
            return colon + 1 < compact.size() && compact.data()[colon + 1] != '/';
        return false;
    }
    return compact.data()[0] != '/' || (compact.size() > 1 && compact.data()[1] != '/');
}

bool safe_cid_source(const char *bytes, size_t length, HtmlBuffer &canonical) {
    HtmlBuffer decoded(MaxEditableTextBytes * 6u);
    if (!uri_decode_for_check(bytes, length, decoded) || !valid_cid(decoded.data(), decoded.size())) return false;
    if (!canonical.append_cstr("cid:")) return false;
    return canonical.append(decoded.data() + 4, decoded.size() - 4);
}

bool output_attribute(HtmlBuffer &output, const char *name, const char *value, size_t length,
                      bool &changed) {
    return output.append_byte(' ') && output.append_cstr(name) && output.append_cstr("=\"") &&
        append_escaped_attribute(output, value, length, changed) && output.append_byte('"');
}

bool numeric_dimension(const char *value, size_t length) {
    if (!length || length > 4) return false;
    unsigned number = 0;
    for (size_t i = 0; i < length; ++i) {
        if (value[i] < '0' || value[i] > '9') return false;
        number = number * 10 + static_cast<unsigned>(value[i] - '0');
    }
    return number > 0 && number <= 2048;
}

enum class SafeStyleProperty : uint8_t {
    FontFamily, FontSize, Color, FontWeight, FontStyle, TextDecoration,
    TextAlign, MarginLeft, MarginRight, TextIndent, ListStyleType, Count
};

struct SafeStyleValue {
    bool present = false;
    char value[96]{};
};

const char *safe_style_name(SafeStyleProperty property) {
    switch (property) {
    case SafeStyleProperty::FontFamily: return "font-family";
    case SafeStyleProperty::FontSize: return "font-size";
    case SafeStyleProperty::Color: return "color";
    case SafeStyleProperty::FontWeight: return "font-weight";
    case SafeStyleProperty::FontStyle: return "font-style";
    case SafeStyleProperty::TextDecoration: return "text-decoration";
    case SafeStyleProperty::TextAlign: return "text-align";
    case SafeStyleProperty::MarginLeft: return "margin-left";
    case SafeStyleProperty::MarginRight: return "margin-right";
    case SafeStyleProperty::TextIndent: return "text-indent";
    case SafeStyleProperty::ListStyleType: return "list-style-type";
    case SafeStyleProperty::Count: break;
    }
    return "";
}

bool allowed_style_property(const char *tag, size_t tag_length, SafeStyleProperty property) {
    const bool font_tag = equal_ci(tag, tag_length, "font") || equal_ci(tag, tag_length, "span");
    const bool paragraph_tag = equal_ci(tag, tag_length, "p") || equal_ci(tag, tag_length, "div") ||
        equal_ci(tag, tag_length, "li") || equal_ci(tag, tag_length, "blockquote");
    const bool list_tag = equal_ci(tag, tag_length, "ul") || equal_ci(tag, tag_length, "ol") ||
        equal_ci(tag, tag_length, "li");
    switch (property) {
    case SafeStyleProperty::FontFamily:
    case SafeStyleProperty::FontSize:
    case SafeStyleProperty::Color:
    case SafeStyleProperty::FontWeight:
    case SafeStyleProperty::FontStyle:
    case SafeStyleProperty::TextDecoration:
        return font_tag;
    case SafeStyleProperty::TextAlign:
    case SafeStyleProperty::MarginLeft:
    case SafeStyleProperty::MarginRight:
    case SafeStyleProperty::TextIndent:
        return paragraph_tag || (list_tag && property != SafeStyleProperty::TextAlign &&
            property != SafeStyleProperty::TextIndent);
    case SafeStyleProperty::ListStyleType:
        return list_tag;
    case SafeStyleProperty::Count:
        return false;
    }
    return false;
}

bool normalized_family(const char *value, size_t length, char output[96]) {
    if (length >= 2 && ((value[0] == '"' && value[length - 1] == '"') ||
        (value[0] == '\'' && value[length - 1] == '\''))) {
        ++value;
        length -= 2;
    }
    const char *canonical = nullptr;
    if (equal_ci(value, length, "SimSun") || (length == strlen("宋体") && !memcmp(value, "宋体", length))) canonical = "SimSun";
    else if (equal_ci(value, length, "Arial")) canonical = "Arial";
    else if (equal_ci(value, length, "Liberation Sans")) canonical = "\"Liberation Sans\"";
    else if (equal_ci(value, length, "Liberation Serif")) canonical = "\"Liberation Serif\"";
    else if (equal_ci(value, length, "Liberation Mono")) canonical = "\"Liberation Mono\"";
    else if (equal_ci(value, length, "Times New Roman")) canonical = "\"Times New Roman\"";
    else if (equal_ci(value, length, "Courier New")) canonical = "\"Courier New\"";
    else if (equal_ci(value, length, "serif")) canonical = "serif";
    else if (equal_ci(value, length, "sans-serif")) canonical = "sans-serif";
    else if (equal_ci(value, length, "monospace")) canonical = "monospace";
    if (!canonical || strlen(canonical) >= 96) return false;
    memcpy(output, canonical, strlen(canonical) + 1);
    return true;
}

bool normalized_pt(const char *value, size_t length, int minimum, int maximum, char output[96]) {
    if (length < 3 || value[length - 2] != 'p' || value[length - 1] != 't') return false;
    size_t digits = length - 2;
    bool negative = value[0] == '-';
    size_t at = negative ? 1 : 0;
    if (at == digits) return false;
    unsigned magnitude = 0;
    for (; at < digits; ++at) {
        if (value[at] < '0' || value[at] > '9') return false;
        if (magnitude > 10000) return false;
        magnitude = magnitude * 10 + static_cast<unsigned>(value[at] - '0');
    }
    if (digits - (negative ? 1 : 0) > 1 && value[negative ? 1 : 0] == '0') return false;
    if (negative && !magnitude) return false;
    const int number = negative ? -static_cast<int>(magnitude) : static_cast<int>(magnitude);
    if (number < minimum || number > maximum) return false;
    const int written = snprintf(output, 96, "%dpt", number);
    return written > 0 && written < 96;
}

SafeStyleProperty identify_style_property(const char *name, size_t length) {
    if (equal_ci(name, length, "font-family")) return SafeStyleProperty::FontFamily;
    if (equal_ci(name, length, "font-size")) return SafeStyleProperty::FontSize;
    if (equal_ci(name, length, "color")) return SafeStyleProperty::Color;
    if (equal_ci(name, length, "font-weight")) return SafeStyleProperty::FontWeight;
    if (equal_ci(name, length, "font-style")) return SafeStyleProperty::FontStyle;
    if (equal_ci(name, length, "text-decoration")) return SafeStyleProperty::TextDecoration;
    if (equal_ci(name, length, "text-align")) return SafeStyleProperty::TextAlign;
    if (equal_ci(name, length, "margin-left")) return SafeStyleProperty::MarginLeft;
    if (equal_ci(name, length, "margin-right")) return SafeStyleProperty::MarginRight;
    if (equal_ci(name, length, "text-indent")) return SafeStyleProperty::TextIndent;
    if (equal_ci(name, length, "list-style-type")) return SafeStyleProperty::ListStyleType;
    return SafeStyleProperty::Count;
}

bool normalize_style_value(SafeStyleProperty property, const char *value, size_t length,
                           char output[96]) {
    while (length && is_space(static_cast<unsigned char>(*value))) { ++value; --length; }
    while (length && is_space(static_cast<unsigned char>(value[length - 1]))) --length;
    if (!length) return false;
    if (property == SafeStyleProperty::FontFamily) return normalized_family(value, length, output);
    if (property == SafeStyleProperty::FontSize) return normalized_pt(value, length, 6, 96, output);
    if (property == SafeStyleProperty::MarginLeft || property == SafeStyleProperty::MarginRight ||
        property == SafeStyleProperty::TextIndent)
        return normalized_pt(value, length, -720, 720, output);
    if (property == SafeStyleProperty::Color) {
        if (length != 7 || value[0] != '#') return false;
        output[0] = '#';
        for (size_t i = 1; i < 7; ++i) {
            if (hex(static_cast<unsigned char>(value[i])) < 0) return false;
            output[i] = static_cast<char>(lower(static_cast<unsigned char>(value[i])));
        }
        output[7] = 0;
        return true;
    }
    const char *allowed[] = {"bold", "normal", "italic", "underline", "line-through", "none",
        "left", "center", "right", "justify", "disc", "decimal"};
    SafeStyleProperty owner[] = {SafeStyleProperty::FontWeight, SafeStyleProperty::FontWeight,
        SafeStyleProperty::FontStyle, SafeStyleProperty::TextDecoration,
        SafeStyleProperty::TextDecoration, SafeStyleProperty::TextDecoration,
        SafeStyleProperty::TextAlign, SafeStyleProperty::TextAlign, SafeStyleProperty::TextAlign,
        SafeStyleProperty::TextAlign, SafeStyleProperty::ListStyleType,
        SafeStyleProperty::ListStyleType};
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
        if (owner[i] != property || !equal_ci(value, length, allowed[i])) continue;
        memcpy(output, allowed[i], strlen(allowed[i]) + 1);
        return true;
    }
    return false;
}

bool sanitize_style_attribute(const char *value, size_t length, const char *tag, size_t tag_length,
                              HtmlBuffer &output, bool &changed) {
    HtmlBuffer decoded(MaxEditableTextBytes * 6u);
    if (!decode_entities(value, length, decoded)) return false;
    SafeStyleValue properties[static_cast<size_t>(SafeStyleProperty::Count)]{};
    size_t at = 0;
    while (at < decoded.size()) {
        size_t separator = at;
        while (separator < decoded.size() && decoded.data()[separator] != ';') ++separator;
        size_t start = at, finish = separator, colon = separator;
        while (start < finish && is_space(static_cast<unsigned char>(decoded.data()[start]))) ++start;
        while (finish > start && is_space(static_cast<unsigned char>(decoded.data()[finish - 1]))) --finish;
        for (size_t i = start; i < finish; ++i) if (decoded.data()[i] == ':') { colon = i; break; }
        if (colon == finish) {
            if (finish > start) changed = true;
        } else {
            size_t name_end = colon;
            while (name_end > start && is_space(static_cast<unsigned char>(decoded.data()[name_end - 1]))) --name_end;
            const SafeStyleProperty property = identify_style_property(decoded.data() + start, name_end - start);
            const size_t index = static_cast<size_t>(property);
            char normalized[96]{};
            if (property == SafeStyleProperty::Count || !allowed_style_property(tag, tag_length, property) ||
                !normalize_style_value(property, decoded.data() + colon + 1, finish - colon - 1, normalized)) {
                changed = true;
            } else {
                if (properties[index].present) changed = true;
                memcpy(properties[index].value, normalized, strlen(normalized) + 1);
                properties[index].present = true;
            }
        }
        at = separator < decoded.size() ? separator + 1 : decoded.size();
    }
    HtmlBuffer canonical(MaxEditableTextBytes * 6u);
    for (size_t i = 0; i < static_cast<size_t>(SafeStyleProperty::Count); ++i) {
        if (!properties[i].present) continue;
        if (canonical.size() && !canonical.append_byte(';')) return false;
        if (!canonical.append_cstr(safe_style_name(static_cast<SafeStyleProperty>(i))) ||
            !canonical.append_byte(':') || !canonical.append_cstr(properties[i].value)) return false;
    }
    if (!canonical.size()) { changed = true; return true; }
    return output_attribute(output, "style", canonical.data(), canonical.size(), changed);
}

bool normalized_font_color(const char *value, size_t length, char output[8]) {
    if (length != 7 || value[0] != '#') return false;
    output[0] = '#';
    for (size_t i = 1; i < 7; ++i) {
        if (hex(static_cast<unsigned char>(value[i])) < 0) return false;
        output[i] = static_cast<char>(lower(static_cast<unsigned char>(value[i])));
    }
    output[7] = 0;
    return true;
}

bool parse_attribute(const char *bytes, size_t end, size_t &at,
                     const char *tag, size_t tag_length, bool image, HtmlBuffer &output, bool &changed) {
    at = skip_spaces(bytes, end, at);
    if (at >= end || bytes[at] == '/' || bytes[at] == '>') return true;
    const size_t name_start = at;
    while (at < end && ((bytes[at] >= 'a' && bytes[at] <= 'z') ||
        (bytes[at] >= 'A' && bytes[at] <= 'Z') || (bytes[at] >= '0' && bytes[at] <= '9') ||
        bytes[at] == '-' || bytes[at] == '_')) ++at;
    if (at == name_start) { ++at; return true; }
    const size_t name_length = at - name_start;
    at = skip_spaces(bytes, end, at);
    const char *value = "";
    size_t value_length = 0;
    if (at < end && bytes[at] == '=') {
        ++at;
        at = skip_spaces(bytes, end, at);
        if (at < end && (bytes[at] == '"' || bytes[at] == '\'')) {
            const char quote = bytes[at++];
            const size_t value_start = at;
            while (at < end && bytes[at] != quote) ++at;
            value = bytes + value_start;
            value_length = at - value_start;
            if (at < end) ++at;
        } else {
            const size_t value_start = at;
            while (at < end && !is_space(static_cast<unsigned char>(bytes[at])) && bytes[at] != '>') ++at;
            value = bytes + value_start;
            value_length = at - value_start;
        }
    }
    if (starts_ci(bytes + name_start, name_length, "on")) {
        changed = true;
        return true;
    }
    if (equal_ci(bytes + name_start, name_length, "style"))
        return sanitize_style_attribute(value, value_length, tag, tag_length, output, changed);
    if (equal_ci(tag, tag_length, "font") && equal_ci(bytes + name_start, name_length, "face")) {
        char family[96]{};
        if (!normalized_family(value, value_length, family)) { changed = true; return true; }
        return output_attribute(output, "face", family, strlen(family), changed);
    }
    if (equal_ci(tag, tag_length, "font") && equal_ci(bytes + name_start, name_length, "size")) {
        if (value_length != 1 || value[0] < '1' || value[0] > '7') { changed = true; return true; }
        return output_attribute(output, "size", value, value_length, changed);
    }
    if (equal_ci(tag, tag_length, "font") && equal_ci(bytes + name_start, name_length, "color")) {
        char color[8]{};
        if (!normalized_font_color(value, value_length, color)) { changed = true; return true; }
        return output_attribute(output, "color", color, 7, changed);
    }
    if (equal_ci(bytes + name_start, name_length, "title") || (image && equal_ci(bytes + name_start, name_length, "alt"))) {
        HtmlBuffer decoded(MaxEditableTextBytes * 6u);
        if (!decode_entities(value, value_length, decoded)) return false;
        return output_attribute(output, equal_ci(bytes + name_start, name_length, "title") ? "title" : "alt",
            decoded.data(), decoded.size(), changed);
    }
    if (equal_ci(tag, tag_length, "a") && equal_ci(bytes + name_start, name_length, "href")) {
        HtmlBuffer decoded(MaxEditableTextBytes * 6u);
        if (!decode_entities(value, value_length, decoded)) return false;
        bool has_control = false;
        for (size_t i = 0; i < decoded.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(decoded.data()[i]);
            if (c < 0x20 || c == 0x7f) { has_control = true; break; }
        }
        if (!has_control && safe_href(decoded.data(), decoded.size()))
            return output_attribute(output, "href", decoded.data(), decoded.size(), changed);
        changed = true;
        return true;
    }
    if (image && equal_ci(bytes + name_start, name_length, "src")) {
        HtmlBuffer canonical(MaxEditableTextBytes * 6u);
        if (!safe_cid_source(value, value_length, canonical)) { changed = true; return true; }
        return output_attribute(output, "src", canonical.data(), canonical.size(), changed);
    }
    if (image && (equal_ci(bytes + name_start, name_length, "width") ||
        equal_ci(bytes + name_start, name_length, "height")) && numeric_dimension(value, value_length)) {
        return output_attribute(output, equal_ci(bytes + name_start, name_length, "width") ? "width" : "height",
            value, value_length, changed);
    }
    if ((equal_ci(bytes + name_start, name_length, "colspan") ||
        equal_ci(bytes + name_start, name_length, "rowspan")) && numeric_dimension(value, value_length)) {
        return output_attribute(output, equal_ci(bytes + name_start, name_length, "colspan") ? "colspan" : "rowspan",
            value, value_length, changed);
    }
    changed = true;
    return true;
}

bool append_sanitized_text(const char *bytes, size_t length, HtmlBuffer &output, bool &changed) {
    // Literal tag openers are tokenized before this helper; entities stay text and cannot create new tags.
    static const char replacement[] = "\xef\xbf\xbd";
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (c == 0x7f || (c < 0x20 && c != '\t' && c != '\r' && c != '\n')) {
            if (!output.append(replacement, sizeof(replacement) - 1)) return false;
            changed = true;
        } else if (!output.append_byte(bytes[i])) return false;
    }
    return true;
}

} // namespace

bool sanitize_html(const char *bytes, size_t length, Text &safe_html,
                   Text &warning, Text &error) {
    error.clear();
    warning.clear();
    if ((!bytes && length) || length > MaxEditableTextBytes)
        return set_error(error, "HTML input is missing or exceeds the 1 MiB limit");
    if (!valid_utf8(bytes, length)) return set_error(error, "HTML input is not valid UTF-8");
    HtmlBuffer output(MaxEditableTextBytes * 6u);
    bool changed = false;
    size_t at = 0;
    while (at < length) {
        if (bytes[at] != '<') {
            const size_t start = at;
            while (at < length && bytes[at] != '<') ++at;
            if (!append_sanitized_text(bytes + start, at - start, output, changed))
                return set_error(error, "sanitized HTML exceeds the output limit");
            continue;
        }
        if (at + 3 < length && !memcmp(bytes + at, "<!--", 4)) {
            size_t close = at + 4;
            while (close + 2 < length && memcmp(bytes + close, "-->", 3)) ++close;
            at = close + 2 < length ? close + 3 : length;
            changed = true;
            continue;
        }
        size_t tag_end = 0;
        if (!parse_tag_end(bytes, length, at + 1, tag_end)) {
            if (!output.append_cstr("&lt;")) return set_error(error, "sanitized HTML exceeds the output limit");
            ++at;
            changed = true;
            continue;
        }
        size_t cursor = at + 1;
        cursor = skip_spaces(bytes, tag_end, cursor);
        bool closing = false;
        if (cursor < tag_end && bytes[cursor] == '/') { closing = true; ++cursor; }
        cursor = skip_spaces(bytes, tag_end, cursor);
        const size_t name_start = cursor;
        while (cursor < tag_end && ((bytes[cursor] >= 'a' && bytes[cursor] <= 'z') ||
            (bytes[cursor] >= 'A' && bytes[cursor] <= 'Z') ||
            (bytes[cursor] >= '0' && bytes[cursor] <= '9') || bytes[cursor] == '-')) ++cursor;
        if (cursor == name_start) {
            if (!output.append_cstr("&lt;")) return set_error(error, "sanitized HTML exceeds the output limit");
            ++at;
            changed = true;
            continue;
        }
        const size_t name_length = cursor - name_start;
        if (is_blocked_tag(bytes + name_start, name_length)) {
            changed = true;
            if (!closing) {
                const size_t close = find_closing_tag(bytes, length, tag_end + 1, bytes + name_start, name_length);
                if (close < length) {
                    size_t after = close + 2 + name_length;
                    while (after < length && bytes[after] != '>') ++after;
                    at = after < length ? after + 1 : length;
                } else at = length;
            } else at = tag_end + 1;
            continue;
        }
        if (!is_allowed_tag(bytes + name_start, name_length)) {
            // HTML mail commonly wraps its actual content in these two structural
            // elements. Removing them is a normalization, not lost message content.
            const bool plain_wrapper = is_document_wrapper(bytes + name_start, name_length) &&
                skip_spaces(bytes, tag_end, cursor) == tag_end;
            if (!plain_wrapper) changed = true;
            at = tag_end + 1;
            continue;
        }
        const bool image = equal_ci(bytes + name_start, name_length, "img");
        if (!output.append_byte('<') || (closing && !output.append_byte('/')) ||
            !output.append(bytes + name_start, name_length)) return set_error(error, "sanitized HTML exceeds the output limit");
        if (!closing) {
            size_t attr = cursor;
            while (attr < tag_end) {
                const size_t old = attr;
                if (!parse_attribute(bytes, tag_end, attr, bytes + name_start, name_length, image, output, changed))
                    return set_error(error, "sanitized HTML exceeds the output limit");
                if (attr <= old) ++attr;
            }
        }
        const bool is_void = is_void_tag(bytes + name_start, name_length);
        if (!output.append_cstr(!closing && is_void ? "/>" : ">"))
            return set_error(error, "sanitized HTML exceeds the output limit");
        if (closing && is_void) changed = true;
        at = tag_end + 1;
    }
    Text next;
    if (!output.finish(next)) return set_error(error, "out of memory");
    safe_html = static_cast<Text &&>(next);
    if (changed) warning.set("Unsafe or unsupported HTML content was removed.");
    return true;
}

bool sanitize_html_for_reader(const char *bytes, size_t length, Text &safe_html,
    Text &warning, Text &error) {
    Text sanitized, sanitizer_warning;
    if (!sanitize_html(bytes, length, sanitized, sanitizer_warning, error)) return false;
    HtmlBuffer output(MaxEditableTextBytes * 6u);
    bool removed_image = false;
    size_t at = 0;
    while (at < sanitized.size()) {
        if (sanitized.c_str()[at] != '<') {
            const size_t start = at;
            while (at < sanitized.size() && sanitized.c_str()[at] != '<') ++at;
            if (!output.append(sanitized.c_str() + start, at - start))
                return set_error(error, "reader HTML exceeds its output limit");
            continue;
        }
        size_t end = 0;
        if (!parse_tag_end(sanitized.c_str(), sanitized.size(), at + 1, end))
            return set_error(error, "sanitized reader HTML contains an incomplete tag");
        size_t name = skip_spaces(sanitized.c_str(), end, at + 1);
        if (name < end && sanitized.c_str()[name] == '/') {
            ++name;
            name = skip_spaces(sanitized.c_str(), end, name);
        }
        const size_t name_start = name;
        while (name < end && ((sanitized.c_str()[name] >= 'a' && sanitized.c_str()[name] <= 'z') ||
            (sanitized.c_str()[name] >= 'A' && sanitized.c_str()[name] <= 'Z'))) ++name;
        if (name > name_start && equal_ci(sanitized.c_str() + name_start, name - name_start, "img")) {
            removed_image = true;
        } else if (!output.append(sanitized.c_str() + at, end + 1 - at)) {
            return set_error(error, "reader HTML exceeds its output limit");
        }
        at = end + 1;
    }
    Text next;
    if (!output.finish(next)) return set_error(error, "out of memory preparing reader HTML");
    safe_html = static_cast<Text &&>(next);
    warning.clear();
    if (removed_image) {
        if (!warning.set("Inline images are hidden in this preview; unsafe HTML content was removed."))
            return set_error(error, "out of memory describing reader HTML limits");
    } else if (!sanitizer_warning.empty()) {
        warning = static_cast<Text &&>(sanitizer_warning);
    }
    error.clear();
    return true;
}

bool is_safe_external_web_link(const char *target, size_t length) {
    if (!target || length < 8 || length > 160 || !valid_utf8(target, length)) return false;
    size_t scheme_length = 0;
    if (length >= 7 && starts_ci(target, length, "http://")) scheme_length = 7;
    else if (length >= 8 && starts_ci(target, length, "https://")) scheme_length = 8;
    else return false;
    size_t authority_end = length;
    for (size_t i = scheme_length; i < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(target[i]);
        if (c <= 0x20 || c == 0x7f || c == '\\') return false;
        if (authority_end == length && (c == '/' || c == '?' || c == '#')) authority_end = i;
    }
    if (authority_end <= scheme_length) return false;
    for (size_t i = scheme_length; i < authority_end; ++i) if (target[i] == '@') return false;
    return true;
}

} // namespace rmail
