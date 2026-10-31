#include "reader_render.h"
#include "html.h"

#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace rmail {
namespace {

constexpr char XpmAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
constexpr size_t XpmAlphabetSize = sizeof(XpmAlphabet) - 1;
constexpr size_t PaletteTableSize = 32768;

bool set_error(Text &error, const char *message) {
    error.set(message);
    return false;
}

unsigned char lower_ascii(unsigned char value) {
    return value >= 'A' && value <= 'Z' ? static_cast<unsigned char>(value + ('a' - 'A')) : value;
}

bool equal_ci(const char *left, size_t left_size, const char *right) {
    if (strlen(right) != left_size) return false;
    for (size_t i = 0; i < left_size; ++i)
        if (lower_ascii(static_cast<unsigned char>(left[i])) !=
            lower_ascii(static_cast<unsigned char>(right[i]))) return false;
    return true;
}

bool starts_ci(const char *text, size_t size, const char *prefix) {
    const size_t prefix_size = strlen(prefix);
    return size >= prefix_size && equal_ci(text, prefix_size, prefix);
}

class RenderBuffer {
    char *bytes_ = nullptr;
    size_t size_ = 0;
    size_t capacity_ = 0;
    size_t limit_;

public:
    explicit RenderBuffer(size_t limit) : limit_(limit) {}
    ~RenderBuffer() { free(bytes_); }
    size_t size() const { return size_; }
    const char *data() const { return bytes_ ? bytes_ : ""; }

    bool append(const char *bytes, size_t size) {
        if ((!bytes && size) || size > limit_ - size_) return false;
        if (!size) return true;
        const size_t required = size_ + size;
        if (required > capacity_) {
            size_t next = capacity_ ? capacity_ : 256;
            while (next < required) {
                if (next > SIZE_MAX / 2) { next = required; break; }
                next *= 2;
            }
            if (next > limit_) next = limit_;
            if (next < required) return false;
            void *memory = realloc(bytes_, next);
            if (!memory) return false;
            bytes_ = static_cast<char *>(memory);
            capacity_ = next;
        }
        memcpy(bytes_ + size_, bytes, size);
        size_ = required;
        return true;
    }

    bool append_char(char byte) { return append(&byte, 1); }
    bool append_cstr(const char *value) { return append(value, strlen(value)); }
    bool finish(Text &text) const { return text.set(data(), size_); }
};

bool append_codepoint(RenderBuffer &output, uint32_t cp) {
    char encoded[4];
    size_t size = 0;
    if (cp <= 0x7f) { encoded[0] = static_cast<char>(cp); size = 1; }
    else if (cp <= 0x7ff) {
        encoded[0] = static_cast<char>(0xc0 | (cp >> 6));
        encoded[1] = static_cast<char>(0x80 | (cp & 0x3f));
        size = 2;
    } else if (cp <= 0xffff && !(cp >= 0xd800 && cp <= 0xdfff)) {
        encoded[0] = static_cast<char>(0xe0 | (cp >> 12));
        encoded[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        encoded[2] = static_cast<char>(0x80 | (cp & 0x3f));
        size = 3;
    } else if (cp <= 0x10ffff) {
        encoded[0] = static_cast<char>(0xf0 | (cp >> 18));
        encoded[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        encoded[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        encoded[3] = static_cast<char>(0x80 | (cp & 0x3f));
        size = 4;
    } else return false;
    return output.append(encoded, size);
}

int hex_value(unsigned char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool decode_attribute_entities(const char *bytes, size_t size, RenderBuffer &output) {
    for (size_t at = 0; at < size;) {
        if (bytes[at] != '&') {
            if (!output.append_char(bytes[at++])) return false;
            continue;
        }
        size_t end = at + 1;
        while (end < size && end - at <= 16 && bytes[end] != ';' &&
            ((bytes[end] >= 'a' && bytes[end] <= 'z') || (bytes[end] >= 'A' && bytes[end] <= 'Z') ||
             (bytes[end] >= '0' && bytes[end] <= '9') || bytes[end] == '#')) ++end;
        if (end >= size || bytes[end] != ';' || end == at + 1 || end - at > 16) {
            if (!output.append_char(bytes[at++])) return false;
            continue;
        }
        uint32_t cp = 0;
        bool known = true;
        const size_t name_size = end - at - 1;
        if (bytes[at + 1] == '#') {
            size_t digit = at + 2;
            int base = 10;
            if (digit < end && (bytes[digit] == 'x' || bytes[digit] == 'X')) { base = 16; ++digit; }
            if (digit == end) known = false;
            for (; known && digit < end; ++digit) {
                const int value = base == 16 ? hex_value(static_cast<unsigned char>(bytes[digit])) :
                    ((bytes[digit] >= '0' && bytes[digit] <= '9') ? bytes[digit] - '0' : -1);
                if (value < 0 || cp > (0x10ffffu - static_cast<uint32_t>(value)) / static_cast<uint32_t>(base))
                    known = false;
                else cp = cp * static_cast<uint32_t>(base) + static_cast<uint32_t>(value);
            }
            if (!cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) known = false;
        } else if (equal_ci(bytes + at + 1, name_size, "amp")) cp = '&';
        else if (equal_ci(bytes + at + 1, name_size, "quot")) cp = '"';
        else if (equal_ci(bytes + at + 1, name_size, "apos")) cp = '\'';
        else if (equal_ci(bytes + at + 1, name_size, "lt")) cp = '<';
        else if (equal_ci(bytes + at + 1, name_size, "gt")) cp = '>';
        else known = false;
        if (known) {
            if (!append_codepoint(output, cp)) return false;
        } else if (!output.append(bytes + at, end + 1 - at)) return false;
        at = end + 1;
    }
    return true;
}

bool find_tag_end(const char *html, size_t size, size_t start, size_t &end) {
    char quote = 0;
    for (size_t at = start; at < size; ++at) {
        if (quote) {
            if (html[at] == quote) quote = 0;
        } else if (html[at] == '"' || html[at] == '\'') quote = html[at];
        else if (html[at] == '>') { end = at; return true; }
    }
    return false;
}

struct HtmlAttributeView { const char *value = nullptr; size_t size = 0; bool present = false; };

bool read_attribute(const char *html, size_t end, size_t &at, HtmlAttributeView &attribute,
    const char *wanted) {
    while (at < end && (html[at] == ' ' || html[at] == '\t' || html[at] == '\r' || html[at] == '\n')) ++at;
    if (at >= end || html[at] == '/' || html[at] == '>') return true;
    const size_t name_start = at;
    while (at < end && ((html[at] >= 'a' && html[at] <= 'z') || (html[at] >= 'A' && html[at] <= 'Z') ||
        (html[at] >= '0' && html[at] <= '9') || html[at] == '-' || html[at] == '_')) ++at;
    if (at == name_start) { ++at; return true; }
    const size_t name_size = at - name_start;
    while (at < end && (html[at] == ' ' || html[at] == '\t')) ++at;
    if (at >= end || html[at] != '=') return true;
    ++at;
    while (at < end && (html[at] == ' ' || html[at] == '\t')) ++at;
    if (at >= end || html[at] != '"') return true;
    ++at;
    const size_t value_start = at;
    while (at < end && html[at] != '"') ++at;
    if (at >= end) return false;
    const size_t value_size = at - value_start;
    ++at;
    if (equal_ci(html + name_start, name_size, wanted)) {
        attribute.value = html + value_start;
        attribute.size = value_size;
        attribute.present = true;
    }
    return true;
}

bool get_attribute(const char *html, size_t tag_start, size_t tag_end, const char *tag_name,
    const char *attribute_name, HtmlAttributeView &attribute) {
    size_t at = tag_start + 1;
    while (at < tag_end && html[at] != ' ' && html[at] != '\t' && html[at] != '\r' && html[at] != '\n' && html[at] != '/') ++at;
    if (!equal_ci(html + tag_start + 1, at - tag_start - 1, tag_name)) return false;
    while (at < tag_end) {
        const size_t old = at;
        if (!read_attribute(html, tag_end, at, attribute, attribute_name)) return false;
        if (attribute.present || at >= tag_end) break;
        if (at <= old) ++at;
    }
    return true;
}

bool append_attribute_literal(RenderBuffer &output, const HtmlAttributeView &attribute) {
    return attribute.present && output.append(attribute.value, attribute.size);
}

bool find_safe_link(const char *value, size_t size, ReaderRenderedLinks &links,
    size_t &index) {
    RenderBuffer decoded(256);
    if (!decode_attribute_entities(value, size, decoded) || !decoded.size() || decoded.size() > 160 ||
        !is_safe_external_web_link(decoded.data(), decoded.size())) return false;
    for (size_t i = 0; i < links.size(); ++i) {
        if (links[i].size() == decoded.size() && !memcmp(links[i].c_str(), decoded.data(), decoded.size())) {
            index = i;
            return true;
        }
    }
    Text link;
    if (!link.set(decoded.data(), decoded.size()) || !links.push(static_cast<Text &&>(link)))
        return false;
    index = links.size() - 1;
    return true;
}

bool canonical_cid(const char *value, size_t size, Text &cid) {
    if (!value || size < 5 || !starts_ci(value, size, "cid:")) return false;
    size_t start = 4;
    for (size_t i = start; i < size; ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '+' || c == '@' || c == '!'))
            return false;
    }
    return size > start && cid.set(value + start, size - start);
}

bool same_cid(const Text &left, const Text &right) {
    size_t left_start = 0, left_size = left.size();
    size_t right_start = 0, right_size = right.size();
    if (left_size >= 2 && left.c_str()[0] == '<' && left.c_str()[left_size - 1] == '>') {
        ++left_start;
        left_size -= 2;
    }
    if (right_size >= 2 && right.c_str()[0] == '<' && right.c_str()[right_size - 1] == '>') {
        ++right_start;
        right_size -= 2;
    }
    return left_size == right_size && !memcmp(left.c_str() + left_start, right.c_str() + right_start, left_size);
}

uint32_t flatten_channel(unsigned char channel, unsigned char alpha) {
    return (static_cast<uint32_t>(channel) * alpha + 255u * (255u - alpha) + 127u) / 255u;
}

struct PaletteEntry {
    uint32_t rgb = 0;
    uint16_t index = 0;
    bool used = false;
};

bool append_xpm_code(RenderBuffer &output, uint16_t index, unsigned chars) {
    char code[3];
    for (unsigned i = 0; i < chars; ++i) {
        code[chars - 1 - i] = XpmAlphabet[index % XpmAlphabetSize];
        index = static_cast<uint16_t>(index / XpmAlphabetSize);
    }
    return output.append(code, chars);
}

enum class PngRenderResult : uint8_t { Ready, Unsupported, Failed };

PngRenderResult make_bounded_xpm(const Attachment &attachment, WorkerReaderInlineImage &image) {
    if (attachment.data.empty() || attachment.data.size() > MaxReaderInlinePngBytes) return PngRenderResult::Unsupported;
    png_image source{};
    source.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&source, attachment.data.data(), attachment.data.size())) {
        png_image_free(&source);
        return PngRenderResult::Unsupported;
    }
    if (!source.width || !source.height || source.width > MaxReaderInlineImageDimension ||
        source.height > MaxReaderInlineImageDimension ||
        static_cast<uint64_t>(source.width) * source.height > MaxReaderInlineImagePixels) {
        png_image_free(&source);
        return PngRenderResult::Unsupported;
    }
    const uint32_t width = source.width;
    const uint32_t height = source.height;
    source.format = PNG_FORMAT_RGBA;
    const size_t pixel_count = static_cast<size_t>(source.width) * source.height;
    const png_alloc_size_t rgba_size = PNG_IMAGE_SIZE(source);
    if (!rgba_size || rgba_size > MaxReaderInlineImagePixels * 4u) {
        png_image_free(&source);
        return PngRenderResult::Unsupported;
    }
    unsigned char *rgba = static_cast<unsigned char *>(malloc(rgba_size));
    if (!rgba) { png_image_free(&source); return PngRenderResult::Failed; }
    if (!png_image_finish_read(&source, nullptr, rgba, 0, nullptr)) {
        free(rgba);
        png_image_free(&source);
        return PngRenderResult::Unsupported;
    }
    png_image_free(&source);

    PaletteEntry *palette = static_cast<PaletteEntry *>(calloc(PaletteTableSize, sizeof(PaletteEntry)));
    uint16_t *pixel_indexes = static_cast<uint16_t *>(malloc(pixel_count * sizeof(uint16_t)));
    if (!palette || !pixel_indexes) {
        free(palette);
        free(pixel_indexes);
        free(rgba);
        return PngRenderResult::Failed;
    }
    uint32_t palette_count = 0;
    for (size_t i = 0; i < pixel_count; ++i) {
        const uint32_t alpha = rgba[i * 4 + 3];
        const uint32_t red = flatten_channel(rgba[i * 4], static_cast<unsigned char>(alpha));
        const uint32_t green = flatten_channel(rgba[i * 4 + 1], static_cast<unsigned char>(alpha));
        const uint32_t blue = flatten_channel(rgba[i * 4 + 2], static_cast<unsigned char>(alpha));
        const uint32_t rgb = (red << 16) | (green << 8) | blue;
        size_t slot = (static_cast<size_t>(rgb) * 2654435761u) & (PaletteTableSize - 1);
        while (palette[slot].used && palette[slot].rgb != rgb) slot = (slot + 1) & (PaletteTableSize - 1);
        if (!palette[slot].used) {
            if (palette_count >= MaxReaderInlineImagePixels) {
                free(palette); free(pixel_indexes); free(rgba);
                return PngRenderResult::Unsupported;
            }
            palette[slot].used = true;
            palette[slot].rgb = rgb;
            palette[slot].index = static_cast<uint16_t>(palette_count++);
        }
        pixel_indexes[i] = palette[slot].index;
    }

    unsigned chars = 1;
    size_t capacity = XpmAlphabetSize;
    while (capacity < palette_count) { ++chars; capacity *= XpmAlphabetSize; }
    RenderBuffer xpm(MaxReaderXpmBytes);
    char line[96];
    int count = snprintf(line, sizeof(line), "/* XPM */\nstatic const char *rmail_image[] = {\n\"%u %u %u %u\",\n",
        width, height, palette_count, chars);
    bool ok = count > 0 && static_cast<size_t>(count) < sizeof(line) && xpm.append(line, static_cast<size_t>(count));
    for (size_t slot = 0; ok && slot < PaletteTableSize; ++slot) {
        if (!palette[slot].used) continue;
        count = snprintf(line, sizeof(line), "\"");
        ok = count == 1 && xpm.append(line, 1) && append_xpm_code(xpm, palette[slot].index, chars);
        if (!ok) break;
        count = snprintf(line, sizeof(line), " c #%06X\",\n", palette[slot].rgb);
        ok = count > 0 && static_cast<size_t>(count) < sizeof(line) && xpm.append(line, static_cast<size_t>(count));
    }
    size_t pixel = 0;
    for (uint32_t y = 0; ok && y < height; ++y) {
        ok = xpm.append_char('"');
        for (uint32_t x = 0; ok && x < width; ++x)
            ok = append_xpm_code(xpm, pixel_indexes[pixel++], chars);
        if (ok) ok = xpm.append_cstr("\",\n");
    }
    if (ok) ok = xpm.append_cstr("};\n");
    free(palette);
    free(pixel_indexes);
    free(rgba);
    if (!ok || !xpm.finish(image.xpm)) return PngRenderResult::Unsupported;
    image.width = static_cast<uint16_t>(width);
    image.height = static_cast<uint16_t>(height);
    return PngRenderResult::Ready;
}

bool image_content_type(const Text &type) { return equal_ci(type.c_str(), type.size(), "image/png"); }

int find_or_render_image(const Text &cid, const AttachmentList &attachments,
    ReaderRenderedDocument &document, size_t &total_xpm, bool &changed) {
    for (size_t i = 0; i < document.images.size(); ++i)
        if (same_cid(document.images[i].content_id, cid)) return static_cast<int>(i);
    if (document.images.size() >= MaxReaderInlineImages) { changed = true; return -1; }
    for (size_t i = 0; i < attachments.size(); ++i) {
        const Attachment &part = attachments[i];
        if (!part.inline_part || !image_content_type(part.content_type) || !same_cid(part.content_id, cid)) continue;
        WorkerReaderInlineImage rendered;
        const PngRenderResult result = make_bounded_xpm(part, rendered);
        if (result == PngRenderResult::Failed) return -2;
        if (result != PngRenderResult::Ready || rendered.xpm.size() > MaxReaderXpmTotalBytes - total_xpm) {
            changed = true;
            return -1;
        }
        ReaderRenderedImage image;
        image.content_id = cid;
        image.width = rendered.width;
        image.height = rendered.height;
        image.xpm = static_cast<Text &&>(rendered.xpm);
        total_xpm += image.xpm.size();
        if (!document.images.push(static_cast<ReaderRenderedImage &&>(image))) return -2;
        return static_cast<int>(document.images.size() - 1);
    }
    changed = true;
    return -1;
}

bool append_unavailable_image(RenderBuffer &output, const HtmlAttributeView &alt) {
    if (!output.append_cstr("<span>")) return false;
    if (alt.present && !output.append(alt.value, alt.size)) return false;
    return output.append_cstr("</span>");
}

bool render_anchor(RenderBuffer &output, const char *html, size_t start, size_t end,
    ReaderRenderedDocument &document) {
    if (html[start + 1] == '/') return output.append_cstr("</a>");
    HtmlAttributeView href;
    if (!get_attribute(html, start, end, "a", "href", href)) return false;
    size_t link_index = 0;
    if (!href.present || !find_safe_link(href.value, href.size, document.links, link_index))
        return output.append_cstr("<a>");
    char token[64];
    const int size = snprintf(token, sizeof(token), "<a href=\"rmail-link://%u\">",
        static_cast<unsigned>(link_index));
    return size > 0 && static_cast<size_t>(size) < sizeof(token) && output.append(token, static_cast<size_t>(size));
}

bool render_image(RenderBuffer &output, const char *html, size_t start, size_t end,
    const AttachmentList &attachments, ReaderRenderedDocument &document,
    size_t &total_xpm, bool &changed) {
    HtmlAttributeView src, alt;
    if (!get_attribute(html, start, end, "img", "src", src) ||
        !get_attribute(html, start, end, "img", "alt", alt)) return false;
    Text cid;
    if (!src.present || !canonical_cid(src.value, src.size, cid)) {
        changed = true;
        return append_unavailable_image(output, alt);
    }
    const int image_index = find_or_render_image(cid, attachments, document, total_xpm, changed);
    if (image_index < 0) {
        if (image_index == -2) return false;
        return append_unavailable_image(output, alt);
    }
    char token[96];
    const ReaderRenderedImage &image = document.images[static_cast<size_t>(image_index)];
    const int token_size = snprintf(token, sizeof(token), "<img src=\"rmail-cid://%u\" width=\"%u\" height=\"%u\"",
        static_cast<unsigned>(image_index), static_cast<unsigned>(image.width),
        static_cast<unsigned>(image.height));
    if (token_size <= 0 || static_cast<size_t>(token_size) >= sizeof(token) ||
        !output.append(token, static_cast<size_t>(token_size))) return false;
    if (alt.present && (!output.append_cstr(" alt=\"") || !append_attribute_literal(output, alt) || !output.append_char('"'))) return false;
    return output.append_cstr("/>");
}

bool is_tag(const char *html, size_t start, size_t end, const char *tag, bool &closing) {
    size_t at = start + 1;
    closing = at < end && html[at] == '/';
    if (closing) ++at;
    const size_t name_start = at;
    while (at < end && ((html[at] >= 'a' && html[at] <= 'z') ||
        (html[at] >= 'A' && html[at] <= 'Z') || (html[at] >= '0' && html[at] <= '9') || html[at] == '-')) ++at;
    return equal_ci(html + name_start, at - name_start, tag);
}

int code_value(char c) {
    for (size_t i = 0; i < XpmAlphabetSize; ++i) if (XpmAlphabet[i] == c) return static_cast<int>(i);
    return -1;
}

bool parse_number(const char *bytes, size_t size, size_t &at, uint32_t &number) {
    if (at >= size || bytes[at] < '0' || bytes[at] > '9') return false;
    number = 0;
    while (at < size && bytes[at] >= '0' && bytes[at] <= '9') {
        const uint32_t digit = static_cast<uint32_t>(bytes[at++] - '0');
        if (number > (UINT32_MAX - digit) / 10) return false;
        number = number * 10 + digit;
    }
    return true;
}

bool next_line(const char *bytes, size_t size, size_t &at, const char *&line, size_t &line_size) {
    if (at >= size) return false;
    const size_t start = at;
    while (at < size && bytes[at] != '\n') ++at;
    if (at >= size) return false;
    line = bytes + start;
    line_size = at - start;
    ++at;
    return true;
}

bool parse_xpm_string(const char *line, size_t size, const char *&value, size_t &value_size,
    bool comma_required) {
    if (size < 3 || line[0] != '"' || line[size - 1] != (comma_required ? ',' : '"')) return false;
    if (comma_required) {
        if (size < 4 || line[size - 2] != '"') return false;
        value_size = size - 3;
    } else value_size = size - 2;
    value = line + 1;
    return true;
}

bool parse_xpm_code(const char *code, size_t size, uint32_t &value) {
    if (!size || size > 3) return false;
    value = 0;
    for (size_t i = 0; i < size; ++i) {
        const int digit = code_value(code[i]);
        if (digit < 0) return false;
        value = value * static_cast<uint32_t>(XpmAlphabetSize) + static_cast<uint32_t>(digit);
    }
    return true;
}

} // namespace

bool render_safe_reader_html(const char *html, size_t size, const AttachmentList &attachments,
    ReaderRenderedDocument &document, Text &error) {
    error.clear();
    if ((!html && size) || size > MaxEditableTextBytes)
        return set_error(error, "HTML reader input is missing or exceeds the 1 MiB MIME-text limit");
    Text sanitized, sanitizer_warning;
    if (!sanitize_html(html, size, sanitized, sanitizer_warning, error)) return false;
    if (sanitized.size() > MaxReaderHtmlBytes)
        return set_error(error, "safe HTML exceeds the 256 KiB reader display limit");
    ReaderRenderedDocument next;
    RenderBuffer output(MaxReaderHtmlBytes);
    size_t total_xpm = 0;
    bool changed = !sanitizer_warning.empty();
    size_t at = 0;
    while (at < sanitized.size()) {
        if (sanitized.c_str()[at] != '<') {
            const size_t start = at;
            while (at < sanitized.size() && sanitized.c_str()[at] != '<') ++at;
            if (!output.append(sanitized.c_str() + start, at - start))
                return set_error(error, "safe HTML exceeds the reader display limit");
            continue;
        }
        size_t end = 0;
        if (!find_tag_end(sanitized.c_str(), sanitized.size(), at + 1, end))
            return set_error(error, "sanitized HTML contains an incomplete tag");
        bool closing = false;
        if (is_tag(sanitized.c_str(), at, end, "a", closing)) {
            if (!render_anchor(output, sanitized.c_str(), at, end, next))
                return set_error(error, "could not prepare a safe HTML link");
        } else if (is_tag(sanitized.c_str(), at, end, "img", closing)) {
            if (closing) changed = true;
            else if (!render_image(output, sanitized.c_str(), at, end, attachments, next, total_xpm, changed))
                return set_error(error, "could not prepare a bounded inline image");
        } else if (!output.append(sanitized.c_str() + at, end + 1 - at)) {
            return set_error(error, "safe HTML exceeds the reader display limit");
        }
        at = end + 1;
    }
    if (!output.finish(next.html)) return set_error(error, "out of memory preparing safe HTML");
    if (changed && !next.warning.set("Some HTML formatting or resources were removed or could not be displayed."))
        return set_error(error, "out of memory describing safe HTML limits");
    document = static_cast<ReaderRenderedDocument &&>(next);
    return true;
}

bool validate_reader_xpm(const char *xpm, size_t size, uint16_t expected_width,
    uint16_t expected_height, Text &error) {
    error.clear();
    if (!xpm || !size || size > MaxReaderXpmBytes || !expected_width || !expected_height ||
        expected_width > MaxReaderInlineImageDimension || expected_height > MaxReaderInlineImageDimension ||
        static_cast<size_t>(expected_width) * expected_height > MaxReaderInlineImagePixels)
        return set_error(error, "inline XPM is missing or exceeds its declared bounds");
    for (size_t i = 0; i < size; ++i) {
        const unsigned char c = static_cast<unsigned char>(xpm[i]);
        if (c == 0 || (c < 0x20 && c != '\n') || c > 0x7e)
            return set_error(error, "inline XPM contains a non-ASCII or control byte");
    }
    size_t at = 0, line_size = 0;
    const char *line = nullptr;
    if (!next_line(xpm, size, at, line, line_size) || line_size != 9 || memcmp(line, "/* XPM */", 9) ||
        !next_line(xpm, size, at, line, line_size) || line_size != 36 ||
        memcmp(line, "static const char *rmail_image[] = {", 36))
        return set_error(error, "inline XPM header is invalid");
    if (!next_line(xpm, size, at, line, line_size)) return set_error(error, "inline XPM has no dimension record");
    const char *header = nullptr;
    size_t header_size = 0;
    if (!parse_xpm_string(line, line_size, header, header_size, true))
        return set_error(error, "inline XPM dimension record is invalid");
    size_t cursor = 0;
    uint32_t width = 0, height = 0, colors = 0, chars = 0;
    if (!parse_number(header, header_size, cursor, width) || cursor >= header_size || header[cursor++] != ' ' ||
        !parse_number(header, header_size, cursor, height) || cursor >= header_size || header[cursor++] != ' ' ||
        !parse_number(header, header_size, cursor, colors) || cursor >= header_size || header[cursor++] != ' ' ||
        !parse_number(header, header_size, cursor, chars) || cursor != header_size ||
        width != expected_width || height != expected_height || !colors || colors > width * height ||
        chars < 1 || chars > 3 || (chars == 1 && colors > XpmAlphabetSize) ||
        (chars == 2 && colors > XpmAlphabetSize * XpmAlphabetSize) ||
        (chars == 3 && colors > XpmAlphabetSize * XpmAlphabetSize * XpmAlphabetSize))
        return set_error(error, "inline XPM dimensions or palette exceed the declared bounds");
    unsigned char *seen = static_cast<unsigned char *>(calloc(colors, 1));
    if (!seen) return set_error(error, "out of memory checking inline XPM palette");
    bool valid = true;
    for (uint32_t i = 0; valid && i < colors; ++i) {
        if (!next_line(xpm, size, at, line, line_size)) { valid = false; break; }
        const char *entry = nullptr;
        size_t entry_size = 0;
        if (!parse_xpm_string(line, line_size, entry, entry_size, true) ||
            entry_size != chars + 10 || memcmp(entry + chars, " c #", 4)) { valid = false; break; }
        uint32_t index = 0;
        if (!parse_xpm_code(entry, chars, index) || index >= colors || seen[index]) { valid = false; break; }
        seen[index] = 1;
        for (size_t color = chars + 4; color < chars + 10; ++color)
            if (hex_value(static_cast<unsigned char>(entry[color])) < 0) valid = false;
    }
    free(seen);
    for (uint32_t row = 0; valid && row < height; ++row) {
        if (!next_line(xpm, size, at, line, line_size)) { valid = false; break; }
        const char *pixels = nullptr;
        size_t pixel_size = 0;
        if (!parse_xpm_string(line, line_size, pixels, pixel_size, true) || pixel_size != width * chars) {
            valid = false;
            break;
        }
        for (uint32_t column = 0; column < width; ++column) {
            uint32_t index = 0;
            if (!parse_xpm_code(pixels + column * chars, chars, index) || index >= colors) { valid = false; break; }
        }
    }
    if (!valid || !next_line(xpm, size, at, line, line_size) || line_size != 2 || memcmp(line, "};", 2) || at != size)
        return set_error(error, "inline XPM palette or pixel data is malformed");
    return true;
}

} // namespace rmail
