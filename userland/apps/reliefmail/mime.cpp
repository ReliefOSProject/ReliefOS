#include "mime.h"
#include "html.h"
#include <errno.h>
#include <iconv.h>
#include <stdio.h>
#include <time.h>

namespace rmail {
namespace {

const unsigned char ReplacementUtf8[] = {0xef, 0xbf, 0xbd};

class Buffer {
    unsigned char *bytes_ = nullptr;
    size_t size_ = 0;
    size_t capacity_ = 0;
    size_t limit_;
public:
    explicit Buffer(size_t limit = SIZE_MAX) : limit_(limit) {}
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
    ~Buffer() { free(bytes_); }
    const unsigned char *data() const { return bytes_; }
    unsigned char *data() { return bytes_; }
    size_t size() const { return size_; }
    bool append(const void *source, size_t length) {
        if (length > limit_ - size_) return false;
        if (!length) return true;
        if (!source || length > SIZE_MAX - size_) return false;
        const size_t required = size_ + length;
        if (required > capacity_) {
            size_t next = capacity_ ? capacity_ : 256;
            while (next < required) {
                if (next > SIZE_MAX / 2) { next = required; break; }
                next *= 2;
            }
            if (next > limit_) next = limit_;
            if (next < required) return false;
            void *replacement = realloc(bytes_, next);
            if (!replacement) return false;
            bytes_ = static_cast<unsigned char *>(replacement);
            capacity_ = next;
        }
        memcpy(bytes_ + size_, source, length);
        size_ = required;
        return true;
    }
    bool append_byte(unsigned char byte) { return append(&byte, 1); }
    bool append_cstr(const char *value) { return append(value, strlen(value)); }
    void clear() { free(bytes_); bytes_ = nullptr; size_ = 0; capacity_ = 0; }
    bool to_text(Text &output) const {
        return output.set(reinterpret_cast<const char *>(bytes_), size_);
    }
};

bool ascii_equal(const char *a, size_t a_length, const char *b) {
    if (a_length != strlen(b)) return false;
    for (size_t i = 0; i < a_length; ++i) {
        unsigned char x = static_cast<unsigned char>(a[i]);
        unsigned char y = static_cast<unsigned char>(b[i]);
        if (x >= 'A' && x <= 'Z') x = static_cast<unsigned char>(x + ('a' - 'A'));
        if (y >= 'A' && y <= 'Z') y = static_cast<unsigned char>(y + ('a' - 'A'));
        if (x != y) return false;
    }
    return true;
}

unsigned char ascii_lower(unsigned char byte) {
    return byte >= 'A' && byte <= 'Z' ? static_cast<unsigned char>(byte + ('a' - 'A')) : byte;
}
bool is_ows(unsigned char byte) { return byte == ' ' || byte == '\t'; }
void trim_ows(const char *bytes, size_t &begin, size_t &end) {
    while (begin < end && is_ows(static_cast<unsigned char>(bytes[begin]))) ++begin;
    while (end > begin && is_ows(static_cast<unsigned char>(bytes[end - 1]))) --end;
}
bool set_error(Text &error, const char *message) { error.set(message); return false; }

bool append_utf8_codepoint(Buffer &output, uint32_t codepoint) {
    unsigned char bytes[4];
    size_t length;
    if (codepoint <= 0x7f) {
        bytes[0] = static_cast<unsigned char>(codepoint); length = 1;
    } else if (codepoint <= 0x7ff) {
        bytes[0] = static_cast<unsigned char>(0xc0 | (codepoint >> 6));
        bytes[1] = static_cast<unsigned char>(0x80 | (codepoint & 0x3f)); length = 2;
    } else if (codepoint <= 0xffff && !(codepoint >= 0xd800 && codepoint <= 0xdfff)) {
        bytes[0] = static_cast<unsigned char>(0xe0 | (codepoint >> 12));
        bytes[1] = static_cast<unsigned char>(0x80 | ((codepoint >> 6) & 0x3f));
        bytes[2] = static_cast<unsigned char>(0x80 | (codepoint & 0x3f)); length = 3;
    } else if (codepoint <= 0x10ffff) {
        bytes[0] = static_cast<unsigned char>(0xf0 | (codepoint >> 18));
        bytes[1] = static_cast<unsigned char>(0x80 | ((codepoint >> 12) & 0x3f));
        bytes[2] = static_cast<unsigned char>(0x80 | ((codepoint >> 6) & 0x3f));
        bytes[3] = static_cast<unsigned char>(0x80 | (codepoint & 0x3f)); length = 4;
    } else return false;
    return output.append(bytes, length);
}

bool utf8_sequence_length(const unsigned char *bytes, size_t length, size_t at, size_t &count) {
    const unsigned char first = bytes[at];
    if (first < 0x80) { count = 1; return true; }
    if (first >= 0xc2 && first <= 0xdf) count = 2;
    else if (first >= 0xe0 && first <= 0xef) count = 3;
    else if (first >= 0xf0 && first <= 0xf4) count = 4;
    else return false;
    if (count > length - at) return false;
    for (size_t i = 1; i < count; ++i)
        if ((bytes[at + i] & 0xc0) != 0x80) return false;
    if (count == 3 && ((first == 0xe0 && bytes[at + 1] < 0xa0) ||
        (first == 0xed && bytes[at + 1] >= 0xa0))) return false;
    if (count == 4 && ((first == 0xf0 && bytes[at + 1] < 0x90) ||
        (first == 0xf4 && bytes[at + 1] >= 0x90))) return false;
    return true;
}

bool append_utf8_repaired(const unsigned char *bytes, size_t length, Buffer &output) {
    size_t at = 0;
    while (at < length) {
        size_t count = 0;
        if (utf8_sequence_length(bytes, length, at, count)) {
            if (!output.append(bytes + at, count)) return false;
            at += count;
        } else {
            if (!output.append(ReplacementUtf8, sizeof(ReplacementUtf8))) return false;
            ++at;
        }
    }
    return true;
}

bool charset_name_allowed(const char *charset) {
    const size_t length = strlen(charset);
    if (!length || length > 63) return false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(charset[i]);
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return false;
    }
    return true;
}

bool convert_to_utf8(const unsigned char *bytes, size_t length, const char *charset,
                     Buffer &output, bool &known_charset, bool &lossy_conversion) {
    known_charset = false;
    lossy_conversion = false;
    if (!charset_name_allowed(charset)) return append_utf8_repaired(bytes, length, output);
    iconv_t conversion = iconv_open("UTF-8", charset);
    if (conversion == reinterpret_cast<iconv_t>(-1))
        return append_utf8_repaired(bytes, length, output);
    known_charset = true;
    char *input = const_cast<char *>(reinterpret_cast<const char *>(bytes));
    size_t input_left = length;
    while (input_left) {
        char out[4096];
        char *out_at = out;
        size_t out_left = sizeof(out);
        errno = 0;
        const size_t result = iconv(conversion, &input, &input_left, &out_at, &out_left);
        const size_t produced = sizeof(out) - out_left;
        if (produced && !output.append(out, produced)) { iconv_close(conversion); return false; }
        if (result != static_cast<size_t>(-1)) continue;
        if (errno == E2BIG) continue;
        if (errno == EILSEQ || errno == EINVAL) {
            if (!output.append(ReplacementUtf8, sizeof(ReplacementUtf8))) {
                iconv_close(conversion); return false;
            }
            lossy_conversion = true;
            ++input; --input_left;
            iconv(conversion, nullptr, nullptr, nullptr, nullptr);
            continue;
        }
        iconv_close(conversion);
        return false;
    }
    iconv_close(conversion);
    return true;
}

int hex_value(unsigned char byte) {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
    return -1;
}
int base64_value(unsigned char byte) {
    if (byte >= 'A' && byte <= 'Z') return byte - 'A';
    if (byte >= 'a' && byte <= 'z') return byte - 'a' + 26;
    if (byte >= '0' && byte <= '9') return byte - '0' + 52;
    if (byte == '+') return 62;
    if (byte == '/') return 63;
    return -1;
}

bool decode_base64(const unsigned char *bytes, size_t length, Buffer &output) {
    int quartet[4];
    size_t count = 0;
    bool padded = false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char byte = bytes[i];
        if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n') continue;
        if (padded) return false;
        if (byte == '=') quartet[count++] = -2;
        else {
            const int value = base64_value(byte);
            if (value < 0) return false;
            quartet[count++] = value;
        }
        if (count == 4) {
            if (quartet[0] < 0 || quartet[1] < 0 || (quartet[2] == -2 && quartet[3] != -2)) return false;
            const unsigned char decoded[3] = {
                static_cast<unsigned char>((quartet[0] << 2) | (quartet[1] >> 4)),
                static_cast<unsigned char>((quartet[1] << 4) | ((quartet[2] < 0 ? 0 : quartet[2]) >> 2)),
                static_cast<unsigned char>(((quartet[2] < 0 ? 0 : quartet[2]) << 6) |
                    (quartet[3] < 0 ? 0 : quartet[3]))
            };
            size_t out_count = quartet[2] == -2 ? 1 : (quartet[3] == -2 ? 2 : 3);
            if (quartet[2] < -2 || quartet[3] < -2 || !output.append(decoded, out_count)) return false;
            padded = out_count != 3;
            count = 0;
        }
    }
    if (count == 1) return false;
    if (count == 2) {
        if (quartet[0] < 0 || quartet[1] < 0) return false;
        const unsigned char decoded = static_cast<unsigned char>((quartet[0] << 2) | (quartet[1] >> 4));
        return output.append_byte(decoded);
    }
    if (count == 3) {
        if (quartet[0] < 0 || quartet[1] < 0 || quartet[2] < 0) return false;
        const unsigned char decoded[2] = {
            static_cast<unsigned char>((quartet[0] << 2) | (quartet[1] >> 4)),
            static_cast<unsigned char>((quartet[1] << 4) | (quartet[2] >> 2))
        };
        return output.append(decoded, sizeof(decoded));
    }
    return true;
}

bool decode_quoted_printable(const unsigned char *bytes, size_t length, Buffer &output) {
    for (size_t i = 0; i < length; ++i) {
        if (bytes[i] != '=') { if (!output.append_byte(bytes[i])) return false; continue; }
        if (i + 1 < length && bytes[i + 1] == '\n') { ++i; continue; }
        if (i + 2 < length && bytes[i + 1] == '\r' && bytes[i + 2] == '\n') { i += 2; continue; }
        if (i + 2 < length) {
            const int hi = hex_value(bytes[i + 1]), lo = hex_value(bytes[i + 2]);
            if (hi >= 0 && lo >= 0) {
                if (!output.append_byte(static_cast<unsigned char>((hi << 4) | lo))) return false;
                i += 2; continue;
            }
        }
        if (!output.append_byte('=')) return false;
    }
    return true;
}

bool decode_transfer(const unsigned char *bytes, size_t length, const Text &encoding,
                     Buffer &output, bool &known) {
    known = true;
    if (encoding.empty() || encoding == "7bit" || encoding == "8bit" || encoding == "binary")
        return output.append(bytes, length);
    if (ascii_equal(encoding.c_str(), encoding.size(), "base64"))
        return decode_base64(bytes, length, output);
    if (ascii_equal(encoding.c_str(), encoding.size(), "quoted-printable"))
        return decode_quoted_printable(bytes, length, output);
    known = false;
    return output.append(bytes, length);
}

enum class HeaderId : uint8_t {
    Unknown, From, ReplyTo, To, Cc, Bcc, Subject, Date, MessageId, InReplyTo, References,
    ContentType, ContentTransferEncoding, ContentDisposition, ContentId
};
HeaderId identify_header(const char *name, size_t length) {
    if (ascii_equal(name, length, "from")) return HeaderId::From;
    if (ascii_equal(name, length, "reply-to")) return HeaderId::ReplyTo;
    if (ascii_equal(name, length, "to")) return HeaderId::To;
    if (ascii_equal(name, length, "cc")) return HeaderId::Cc;
    if (ascii_equal(name, length, "bcc")) return HeaderId::Bcc;
    if (ascii_equal(name, length, "subject")) return HeaderId::Subject;
    if (ascii_equal(name, length, "date")) return HeaderId::Date;
    if (ascii_equal(name, length, "message-id")) return HeaderId::MessageId;
    if (ascii_equal(name, length, "in-reply-to")) return HeaderId::InReplyTo;
    if (ascii_equal(name, length, "references")) return HeaderId::References;
    if (ascii_equal(name, length, "content-type")) return HeaderId::ContentType;
    if (ascii_equal(name, length, "content-transfer-encoding")) return HeaderId::ContentTransferEncoding;
    if (ascii_equal(name, length, "content-disposition")) return HeaderId::ContentDisposition;
    if (ascii_equal(name, length, "content-id")) return HeaderId::ContentId;
    return HeaderId::Unknown;
}

struct Headers {
    Text from, reply_to, to, cc, bcc, subject, date, message_id, in_reply_to, references;
    Text content_type, transfer_encoding, disposition, content_id;
};
Text *header_slot(Headers &headers, HeaderId id) {
    switch (id) {
    case HeaderId::From: return &headers.from;
    case HeaderId::ReplyTo: return &headers.reply_to;
    case HeaderId::To: return &headers.to;
    case HeaderId::Cc: return &headers.cc;
    case HeaderId::Bcc: return &headers.bcc;
    case HeaderId::Subject: return &headers.subject;
    case HeaderId::Date: return &headers.date;
    case HeaderId::MessageId: return &headers.message_id;
    case HeaderId::InReplyTo: return &headers.in_reply_to;
    case HeaderId::References: return &headers.references;
    case HeaderId::ContentType: return &headers.content_type;
    case HeaderId::ContentTransferEncoding: return &headers.transfer_encoding;
    case HeaderId::ContentDisposition: return &headers.disposition;
    case HeaderId::ContentId: return &headers.content_id;
    default: return nullptr;
    }
}
bool append_header_value(Text &slot, const unsigned char *bytes, size_t length, bool list_value) {
    if (slot.empty()) return slot.set(reinterpret_cast<const char *>(bytes), length);
    Buffer combined(MaxHeaderBytes);
    if (!combined.append(slot.c_str(), slot.size()) ||
        !combined.append_cstr(list_value ? ", " : " ") || !combined.append(bytes, length)) return false;
    return combined.to_text(slot);
}
bool flush_header(Headers &headers, HeaderId id, Buffer &value) {
    Text *slot = header_slot(headers, id);
    if (!slot) return true;
    const bool list = id == HeaderId::ReplyTo || id == HeaderId::To || id == HeaderId::Cc || id == HeaderId::Bcc ||
        id == HeaderId::References || id == HeaderId::InReplyTo;
    return append_header_value(*slot, value.data(), value.size(), list);
}

bool valid_header_field_value(const unsigned char *bytes, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = bytes[i];
        if ((c < 0x20 && c != '\t') || c == 0x7f) return false;
    }
    return true;
}

bool parse_headers(const unsigned char *bytes, size_t start, size_t end,
                   size_t &body_start, size_t &header_bytes, Headers &headers) {
    size_t separator = SIZE_MAX, separator_size = 0;
    if (start + 1 < end && bytes[start] == '\r' && bytes[start + 1] == '\n') {
        separator = start; separator_size = 2;
    } else if (start < end && bytes[start] == '\n') {
        separator = start; separator_size = 1;
    }
    for (size_t i = start; separator == SIZE_MAX && i < end; ++i) {
        if (bytes[i] == '\n' && i > start && bytes[i - 1] == '\r') {
            if (i + 1 < end && bytes[i + 1] == '\r' && i + 2 < end && bytes[i + 2] == '\n') {
                separator = i - 1; separator_size = 4;
            }
        } else if (bytes[i] == '\n' && i + 1 < end && bytes[i + 1] == '\n') {
            separator = i; separator_size = 2;
        }
    }
    if (separator == SIZE_MAX) return false;
    body_start = separator + separator_size;
    header_bytes = body_start - start;
    HeaderId current = HeaderId::Unknown;
    bool have_current_field = false;
    Buffer value(MaxHeaderBytes);
    size_t at = start;
    while (at < separator) {
        const size_t line_start = at;
        while (at < separator && bytes[at] != '\n') ++at;
        size_t line_end = at;
        if (line_end > line_start && bytes[line_end - 1] == '\r') --line_end;
        if (at < separator) ++at;
        if (line_start < line_end && (bytes[line_start] == ' ' || bytes[line_start] == '\t')) {
            if (!have_current_field || !value.append_byte(' ')) return false;
            size_t cont = line_start;
            while (cont < line_end && is_ows(bytes[cont])) ++cont;
            if (!valid_header_field_value(bytes + cont, line_end - cont)) return false;
            if (!value.append(bytes + cont, line_end - cont)) return false;
            continue;
        }
        if (!flush_header(headers, current, value)) return false;
        value.clear();
        current = HeaderId::Unknown;
        have_current_field = false;
        size_t colon = line_start;
        while (colon < line_end && bytes[colon] != ':') ++colon;
        if (colon == line_end || colon == line_start) return false;
        for (size_t i = line_start; i < colon; ++i) {
            const unsigned char c = bytes[i];
            if (c <= 32 || c >= 127 || c == ':' || c == '(' || c == ')' || c == '<' || c == '>') return false;
        }
        current = identify_header(reinterpret_cast<const char *>(bytes + line_start), colon - line_start);
        size_t value_start = colon + 1;
        while (value_start < line_end && is_ows(bytes[value_start])) ++value_start;
        size_t value_end = line_end;
        while (value_end > value_start && is_ows(bytes[value_end - 1])) --value_end;
        if (!valid_header_field_value(bytes + value_start, value_end - value_start)) return false;
        if (!value.append(bytes + value_start, value_end - value_start)) return false;
        have_current_field = true;
    }
    return flush_header(headers, current, value);
}

struct Parameter { Text name; Text value; };
struct ParameterList { Array<Parameter> items; };
bool parse_parameters(const Text &field, ParameterList &parameters) {
    const char *bytes = field.c_str();
    const size_t length = field.size();
    size_t at = 0;
    while (at < length && bytes[at] != ';') ++at;
    while (at < length) {
        if (bytes[at++] != ';') continue;
        while (at < length && is_ows(static_cast<unsigned char>(bytes[at]))) ++at;
        const size_t name_start = at;
        while (at < length && bytes[at] != '=' && bytes[at] != ';') ++at;
        size_t name_end = at;
        while (name_end > name_start && is_ows(static_cast<unsigned char>(bytes[name_end - 1]))) --name_end;
        if (at == length || bytes[at] != '=') continue;
        ++at;
        while (at < length && is_ows(static_cast<unsigned char>(bytes[at]))) ++at;
        Buffer value(MaxHeaderBytes);
        if (at < length && bytes[at] == '"') {
            ++at;
            while (at < length && bytes[at] != '"') {
                if (bytes[at] == '\\' && at + 1 < length) ++at;
                if (!value.append(bytes + at, 1)) return false;
                ++at;
            }
            if (at < length && bytes[at] == '"') ++at;
            while (at < length && bytes[at] != ';') ++at;
        } else {
            const size_t value_start = at;
            while (at < length && bytes[at] != ';') ++at;
            size_t value_end = at;
            while (value_end > value_start && is_ows(static_cast<unsigned char>(bytes[value_end - 1]))) --value_end;
            if (!value.append(bytes + value_start, value_end - value_start)) return false;
        }
        Parameter parameter;
        Buffer lower_name(MaxHeaderBytes);
        for (size_t i = name_start; i < name_end; ++i)
            if (!lower_name.append_byte(ascii_lower(static_cast<unsigned char>(bytes[i])))) return false;
        if (!lower_name.to_text(parameter.name) || !value.to_text(parameter.value) ||
            !parameters.items.push(static_cast<Parameter &&>(parameter))) return false;
    }
    return true;
}
const Text *find_parameter(const ParameterList &parameters, const char *name) {
    for (size_t i = 0; i < parameters.items.size(); ++i)
        if (parameters.items[i].name == name) return &parameters.items[i].value;
    return nullptr;
}

struct MediaType {
    Text type, subtype, charset, boundary;
    ParameterList parameters;
    bool valid = true;
};
bool parse_media_type(const Text &field, MediaType &media) {
    size_t semicolon = 0;
    while (semicolon < field.size() && field.c_str()[semicolon] != ';') ++semicolon;
    size_t begin = 0, media_end = semicolon;
    trim_ows(field.c_str(), begin, media_end);
    size_t slash = begin;
    while (slash < media_end && field.c_str()[slash] != '/') ++slash;
    if (slash == media_end || slash == begin || slash + 1 == media_end) { media.valid = false; return true; }
    Buffer type, subtype;
    for (size_t i = begin; i < slash; ++i)
        if (!type.append_byte(ascii_lower(static_cast<unsigned char>(field.c_str()[i])))) return false;
    for (size_t i = slash + 1; i < media_end; ++i)
        if (!subtype.append_byte(ascii_lower(static_cast<unsigned char>(field.c_str()[i])))) return false;
    if (!type.to_text(media.type) || !subtype.to_text(media.subtype) ||
        !parse_parameters(field, media.parameters)) return false;
    const Text *charset = find_parameter(media.parameters, "charset");
    const Text *boundary = find_parameter(media.parameters, "boundary");
    if (charset) media.charset = *charset;
    if (boundary) media.boundary = *boundary;
    return true;
}
bool is_media(const MediaType &media, const char *type, const char *subtype) {
    return media.type == type && media.subtype == subtype;
}
bool is_multipart(const MediaType &media) { return media.type == "multipart"; }

struct Disposition { Text kind; ParameterList parameters; };
bool parse_disposition(const Text &field, Disposition &disposition) {
    size_t end = 0;
    while (end < field.size() && field.c_str()[end] != ';') ++end;
    size_t begin = 0;
    trim_ows(field.c_str(), begin, end);
    Buffer lower;
    for (size_t i = begin; i < end; ++i)
        if (!lower.append_byte(ascii_lower(static_cast<unsigned char>(field.c_str()[i])))) return false;
    return lower.to_text(disposition.kind) && parse_parameters(field, disposition.parameters);
}

struct ExtendedSegment { size_t index = 0; const Text *value = nullptr; bool encoded = false; };
int compare_extended_segment(const void *left, const void *right) {
    const ExtendedSegment *a = static_cast<const ExtendedSegment *>(left);
    const ExtendedSegment *b = static_cast<const ExtendedSegment *>(right);
    return a->index < b->index ? -1 : (a->index > b->index ? 1 : 0);
}

class Parser {
    const unsigned char *raw_;
    size_t raw_length_;
    ParsedMessage &message_;
    Text &error_;
    size_t header_bytes_ = 0;
    size_t part_count_ = 0;
    size_t attachment_bytes_ = 0;
    size_t initial_depth_ = 1;

    bool fail(const char *message) { return set_error(error_, message); }
    void warn() {
        if (message_.warning.empty())
            message_.warning.set("Some message content could not be fully interpreted.");
    }
    bool header_to_text(const Text &raw, Text &decoded);
    bool decode_extended(const Text &value, Text &output);
    bool filename_from_params(const ParameterList &parameters, Text &filename);
    bool append_attachment(const MediaType &media, const Disposition &disposition,
        const Headers &headers, const unsigned char *body, size_t body_length);
    bool find_boundary(const unsigned char *boundary, size_t boundary_length,
        size_t start, size_t end, size_t &line_start, size_t &line_after, bool &closing);
    bool parse_entity(size_t start, size_t end, size_t depth, bool top_level);

public:
    Parser(const unsigned char *raw, size_t length, ParsedMessage &message, Text &error)
        : raw_(raw), raw_length_(length), message_(message), error_(error) {}
    Parser(const unsigned char *raw, size_t length, ParsedMessage &message, Text &error, size_t initial_depth)
        : raw_(raw), raw_length_(length), message_(message), error_(error), initial_depth_(initial_depth) {}
    bool parse() {
        if (raw_length_ > MaxRawMessageBytes) return fail("raw message exceeds the 64 MiB limit");
        if (!message_.original_message.set(raw_, raw_length_)) return fail("out of memory");
        return parse_entity(0, raw_length_, initial_depth_, true);
    }
    size_t header_bytes() const { return header_bytes_; }
    size_t part_count() const { return part_count_; }
    size_t attachment_bytes() const { return attachment_bytes_; }
};

bool Parser::header_to_text(const Text &raw, Text &decoded) {
    if (raw.empty()) { decoded.clear(); return true; }
    Buffer output(MaxEditableTextBytes);
    size_t at = 0;
    bool previous_encoded = false;
    while (at < raw.size()) {
        size_t marker = at;
        while (marker + 1 < raw.size() && !(raw.c_str()[marker] == '=' && raw.c_str()[marker + 1] == '?'))
            ++marker;
        if (marker + 1 >= raw.size()) marker = raw.size();
        if (marker > at) {
            bool only_space = true;
            for (size_t i = at; i < marker; ++i)
                if (!is_ows(static_cast<unsigned char>(raw.c_str()[i]))) only_space = false;
            if (!(previous_encoded && only_space)) {
                Buffer repaired(MaxEditableTextBytes);
                if (!append_utf8_repaired(reinterpret_cast<const unsigned char *>(raw.c_str() + at),
                    marker - at, repaired) || !output.append(repaired.data(), repaired.size())) return false;
            }
            previous_encoded = false;
            at = marker;
        }
        if (at >= raw.size()) break;
        size_t charset_end = at + 2;
        while (charset_end < raw.size() && raw.c_str()[charset_end] != '?') ++charset_end;
        if (charset_end == raw.size() || charset_end + 2 >= raw.size() || raw.c_str()[charset_end + 2] != '?') {
            if (!output.append_byte(static_cast<unsigned char>(raw.c_str()[at++]))) return false;
            previous_encoded = false;
            continue;
        }
        const char encoding = ascii_lower(static_cast<unsigned char>(raw.c_str()[charset_end + 1]));
        size_t word_end = charset_end + 3;
        while (word_end + 1 < raw.size() &&
            !(raw.c_str()[word_end] == '?' && raw.c_str()[word_end + 1] == '=')) ++word_end;
        if (word_end + 1 >= raw.size() || (encoding != 'b' && encoding != 'q')) {
            if (!output.append_byte(static_cast<unsigned char>(raw.c_str()[at++]))) return false;
            previous_encoded = false;
            continue;
        }
        Buffer encoded(MaxHeaderBytes);
        if (encoding == 'q') {
            for (size_t i = charset_end + 3; i < word_end; ++i) {
                unsigned char c = static_cast<unsigned char>(raw.c_str()[i]);
                if (c == '_') c = ' ';
                else if (c == '=' && i + 2 < word_end) {
                    const int hi = hex_value(static_cast<unsigned char>(raw.c_str()[i + 1]));
                    const int lo = hex_value(static_cast<unsigned char>(raw.c_str()[i + 2]));
                    if (hi >= 0 && lo >= 0) {
                        c = static_cast<unsigned char>((hi << 4) | lo);
                        i += 2;
                    }
                }
                if (!encoded.append_byte(c)) return false;
            }
        } else if (!decode_base64(
            reinterpret_cast<const unsigned char *>(raw.c_str() + charset_end + 3),
            word_end - (charset_end + 3), encoded)) {
            if (!output.append(raw.c_str() + at, word_end + 2 - at)) return false;
            at = word_end + 2;
            previous_encoded = false;
            continue;
        }
        Buffer converted(MaxEditableTextBytes);
        char charset[64];
        const size_t charset_length = charset_end - (at + 2);
        if (!charset_length || charset_length >= sizeof(charset)) {
            warn();
            if (!append_utf8_repaired(encoded.data(), encoded.size(), converted)) return false;
        } else {
            memcpy(charset, raw.c_str() + at + 2, charset_length);
            charset[charset_length] = 0;
            bool known = false, lossy = false;
            if (!convert_to_utf8(encoded.data(), encoded.size(), charset, converted, known, lossy)) return false;
            if (!known || lossy) warn();
        }
        if (!output.append(converted.data(), converted.size())) return false;
        at = word_end + 2;
        previous_encoded = true;
    }
    Buffer safe(MaxEditableTextBytes);
    for (size_t i = 0; i < output.size(); ++i) {
        const unsigned char c = output.data()[i];
        if (!c || c == '\r' || c == '\n' || (c < 0x20 && c != '\t') || c == 0x7f) {
            if (!safe.append(ReplacementUtf8, sizeof(ReplacementUtf8))) return false;
        } else if (!safe.append_byte(c)) return false;
    }
    return safe.to_text(decoded);
}

bool Parser::decode_extended(const Text &value, Text &output) {
    const char *bytes = value.c_str();
    const size_t length = value.size();
    size_t data_start = 0;
    char charset[64] = "UTF-8";
    size_t quote1 = 0;
    while (quote1 < length && bytes[quote1] != '\'') ++quote1;
    if (quote1 < length) {
        size_t quote2 = quote1 + 1;
        while (quote2 < length && bytes[quote2] != '\'') ++quote2;
        if (quote2 < length) {
            if (quote1 && quote1 < sizeof(charset)) {
                memcpy(charset, bytes, quote1);
                charset[quote1] = 0;
            }
            data_start = quote2 + 1;
        }
    }
    Buffer octets(MaxHeaderBytes);
    for (size_t i = data_start; i < length; ++i) {
        if (bytes[i] == '%' && i + 2 < length) {
            const int hi = hex_value(static_cast<unsigned char>(bytes[i + 1]));
            const int lo = hex_value(static_cast<unsigned char>(bytes[i + 2]));
            if (hi >= 0 && lo >= 0) {
                if (!octets.append_byte(static_cast<unsigned char>((hi << 4) | lo))) return false;
                i += 2;
                continue;
            }
        }
        if (!octets.append_byte(static_cast<unsigned char>(bytes[i]))) return false;
    }
    Buffer converted(MaxHeaderBytes);
    bool known = false, lossy = false;
    if (!convert_to_utf8(octets.data(), octets.size(), charset, converted, known, lossy)) return false;
    if (!known || lossy) warn();
    return converted.to_text(output);
}

bool Parser::filename_from_params(const ParameterList &parameters, Text &filename) {
    const Text *ordinary = find_parameter(parameters, "filename");
    if (!ordinary) ordinary = find_parameter(parameters, "name");
    const Text *simple_extended = find_parameter(parameters, "filename*");
    if (!simple_extended) simple_extended = find_parameter(parameters, "name*");
    if (simple_extended) return decode_extended(*simple_extended, filename);
    Array<ExtendedSegment> segments;
    for (size_t i = 0; i < parameters.items.size(); ++i) {
        const Text &name = parameters.items[i].name;
        size_t base_length = 0;
        if (name.size() > 10 && !memcmp(name.c_str(), "filename*", 9)) base_length = 8;
        else if (name.size() > 6 && !memcmp(name.c_str(), "name*", 5)) base_length = 4;
        if (!base_length) continue;
        size_t at = base_length + 1;
        if (at >= name.size() || name.c_str()[at] < '0' || name.c_str()[at] > '9') continue;
        size_t index = 0;
        while (at < name.size() && name.c_str()[at] >= '0' && name.c_str()[at] <= '9') {
            const size_t digit = static_cast<size_t>(name.c_str()[at] - '0');
            if (index > (SIZE_MAX - digit) / 10) return false;
            index = index * 10 + digit;
            ++at;
        }
        const bool encoded = at < name.size() && name.c_str()[at] == '*';
        if (encoded) ++at;
        if (at != name.size()) continue;
        ExtendedSegment segment;
        segment.index = index;
        segment.value = &parameters.items[i].value;
        segment.encoded = encoded;
        if (!segments.push(segment)) return false;
    }
    if (!segments.empty()) {
        qsort(segments.data(), segments.size(), sizeof(ExtendedSegment), compare_extended_segment);
        if (segments[0].index != 0) return false;
        Buffer joined(MaxHeaderBytes);
        for (size_t i = 0; i < segments.size(); ++i) {
            if (segments[i].index != i) return false;
            const Text &part = *segments[i].value;
            size_t begin = 0;
            if (i == 0 && segments[i].encoded) {
                size_t first = 0;
                while (first < part.size() && part.c_str()[first] != '\'') ++first;
                size_t second = first + 1;
                while (second < part.size() && part.c_str()[second] != '\'') ++second;
                if (second < part.size()) begin = second + 1;
            }
            for (size_t p = begin; p < part.size(); ++p) {
                unsigned char byte = static_cast<unsigned char>(part.c_str()[p]);
                if (segments[i].encoded && byte == '%' && p + 2 < part.size()) {
                    const int hi = hex_value(static_cast<unsigned char>(part.c_str()[p + 1]));
                    const int lo = hex_value(static_cast<unsigned char>(part.c_str()[p + 2]));
                    if (hi >= 0 && lo >= 0) {
                        byte = static_cast<unsigned char>((hi << 4) | lo);
                        p += 2;
                    }
                }
                if (!joined.append_byte(byte)) return false;
            }
        }
        char charset[64] = "UTF-8";
        if (segments[0].encoded) {
            const Text &first = *segments[0].value;
            size_t quote = 0;
            while (quote < first.size() && first.c_str()[quote] != '\'') ++quote;
            if (quote && quote < sizeof(charset)) {
                memcpy(charset, first.c_str(), quote);
                charset[quote] = 0;
            }
        }
        Buffer converted(MaxHeaderBytes);
        bool known = false, lossy = false;
        if (!convert_to_utf8(joined.data(), joined.size(), charset, converted, known, lossy)) return false;
        if (!known || lossy) warn();
        return converted.to_text(filename);
    }
    return ordinary ? header_to_text(*ordinary, filename) : true;
}

bool Parser::append_attachment(const MediaType &media, const Disposition &disposition,
                               const Headers &headers, const unsigned char *body, size_t body_length) {
    const size_t remaining = DefaultMaxAttachmentBytes - attachment_bytes_;
    Buffer decoded(remaining);
    bool known_transfer = false;
    if (!decode_transfer(body, body_length, headers.transfer_encoding, decoded, known_transfer))
        return fail("invalid or oversized MIME attachment data");
    if (!known_transfer) warn();

    Attachment attachment;
    if (!filename_from_params(disposition.parameters, attachment.filename))
        return fail("invalid attachment filename");
    Buffer type(MaxHeaderBytes);
    if (!type.append(media.type.c_str(), media.type.size()) ||
        (!media.subtype.empty() && (!type.append_byte('/') || !type.append(media.subtype.c_str(), media.subtype.size()))) ||
        !type.to_text(attachment.content_type)) return fail("out of memory while reading attachment metadata");

    size_t cid_start = 0, cid_end = headers.content_id.size();
    trim_ows(headers.content_id.c_str(), cid_start, cid_end);
    if (cid_end >= cid_start + 2 && headers.content_id.c_str()[cid_start] == '<' &&
        headers.content_id.c_str()[cid_end - 1] == '>') {
        ++cid_start;
        --cid_end;
    }
    if (!attachment.content_id.set(headers.content_id.c_str() + cid_start, cid_end - cid_start))
        return fail("out of memory");
    attachment.inline_part = disposition.kind == "inline" || !attachment.content_id.empty();
    if (!attachment.data.set(decoded.data(), decoded.size())) return fail("out of memory");
    attachment_bytes_ += decoded.size();
    if (!message_.attachments.push(static_cast<Attachment &&>(attachment)))
        return fail("too many MIME attachments");
    return true;
}

bool Parser::find_boundary(const unsigned char *boundary, size_t boundary_length,
                           size_t start, size_t end, size_t &line_start,
                           size_t &line_after, bool &closing) {
    size_t at = start;
    while (at < end) {
        const size_t begin = at;
        while (at < end && raw_[at] != '\n') ++at;
        const size_t after = at < end ? at + 1 : at;
        size_t line_end = at;
        if (line_end > begin && raw_[line_end - 1] == '\r') --line_end;
        if (line_end - begin >= boundary_length + 2 && raw_[begin] == '-' && raw_[begin + 1] == '-' &&
            !memcmp(raw_ + begin + 2, boundary, boundary_length)) {
            size_t tail = begin + boundary_length + 2;
            closing = false;
            if (tail + 1 < line_end && raw_[tail] == '-' && raw_[tail + 1] == '-') {
                closing = true;
                tail += 2;
            }
            while (tail < line_end && (raw_[tail] == ' ' || raw_[tail] == '\t')) ++tail;
            if (tail == line_end) {
                line_start = begin;
                line_after = after;
                return true;
            }
        }
        at = after;
    }
    return false;
}

bool Parser::parse_entity(size_t start, size_t end, size_t depth, bool top_level) {
    if (depth > MaxMimeDepth) return fail("MIME nesting exceeds the depth limit");
    if (++part_count_ > MaxMimeParts) return fail("MIME part count exceeds the limit");
    if (start > end || end > raw_length_) return fail("invalid MIME part range");

    Headers headers;
    size_t body_start = 0, header_size = 0;
    if (!parse_headers(raw_, start, end, body_start, header_size, headers))
        return fail("malformed or incomplete MIME headers");
    if (header_size > MaxHeaderBytes - header_bytes_) return fail("MIME headers exceed the 256 KiB limit");
    header_bytes_ += header_size;

    if (top_level) {
        if (!header_to_text(headers.from, message_.from) || !header_to_text(headers.reply_to, message_.reply_to) ||
            !header_to_text(headers.to, message_.to) ||
            !header_to_text(headers.cc, message_.cc) || !header_to_text(headers.bcc, message_.bcc) ||
            !header_to_text(headers.subject, message_.subject) || !header_to_text(headers.date, message_.date) ||
            !header_to_text(headers.message_id, message_.message_id) ||
            !header_to_text(headers.in_reply_to, message_.in_reply_to) ||
            !header_to_text(headers.references, message_.references))
            return fail("out of memory while decoding headers");
    }

    MediaType media;
    if (headers.content_type.empty()) {
        media.type.set("text", 4);
        media.subtype.set("plain", 5);
        media.charset.set("us-ascii", 8);
    } else if (!parse_media_type(headers.content_type, media)) {
        return fail("out of memory while reading content type");
    }
    if (!media.valid) {
        warn();
        media.type.set("application", 11);
        media.subtype.set("octet-stream", 12);
    }
    Disposition disposition;
    if (!parse_disposition(headers.disposition, disposition))
        return fail("out of memory while reading disposition");
    const unsigned char *body = raw_ + body_start;
    const size_t body_length = end - body_start;

    if (is_multipart(media)) {
        if (media.boundary.empty() || media.boundary.size() > 70) { warn(); return true; }
        for (size_t i = 0; i < media.boundary.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(media.boundary.c_str()[i]);
            if (c < 33 || c > 126 || c == '"') { warn(); return true; }
        }
        size_t delimiter_start = 0, delimiter_after = 0;
        bool closing = false;
        if (!find_boundary(reinterpret_cast<const unsigned char *>(media.boundary.c_str()),
            media.boundary.size(), body_start, end, delimiter_start, delimiter_after, closing)) {
            warn();
            return true;
        }
        if (closing) return true;
        size_t child_start = delimiter_after;
        for (;;) {
            size_t next_start = 0, next_after = 0;
            bool next_closing = false;
            const bool found = find_boundary(reinterpret_cast<const unsigned char *>(media.boundary.c_str()),
                media.boundary.size(), child_start, end, next_start, next_after, next_closing);
            size_t child_end = found ? next_start : end;
            if (found && child_end > child_start && raw_[child_end - 1] == '\n') {
                --child_end;
                if (child_end > child_start && raw_[child_end - 1] == '\r') --child_end;
            }
            if (!parse_entity(child_start, child_end, depth + 1, false)) return false;
            if (!found) { warn(); return true; }
            if (next_closing) return true;
            child_start = next_after;
        }
    }

    bool has_filename = find_parameter(disposition.parameters, "filename") ||
        find_parameter(disposition.parameters, "filename*") ||
        find_parameter(disposition.parameters, "name") || find_parameter(disposition.parameters, "name*");
    if (!has_filename) {
        for (size_t i = 0; i < disposition.parameters.items.size(); ++i) {
            const Text &name = disposition.parameters.items[i].name;
            if ((name.size() > 10 && !memcmp(name.c_str(), "filename*", 9)) ||
                (name.size() > 6 && !memcmp(name.c_str(), "name*", 5))) { has_filename = true; break; }
        }
    }
    const bool is_message = media.type == "message" && media.subtype == "rfc822";
    const bool is_text = media.type == "text" && (media.subtype == "plain" || media.subtype == "html");
    const bool is_attachment = disposition.kind == "attachment" || has_filename || (!is_message && !is_text);

    if (is_message && disposition.kind != "attachment") {
        Buffer nested(MaxRawMessageBytes);
        bool known_transfer = false;
        if (!decode_transfer(body, body_length, headers.transfer_encoding, nested, known_transfer))
            return fail("invalid nested message transfer encoding");
        if (!known_transfer) warn();
        ParsedMessage nested_result;
        Text nested_error;
        Parser inner(nested.data(), nested.size(), nested_result, nested_error, depth + 1);
        if (!inner.parse()) return fail("invalid nested message");
        if (inner.part_count() > MaxMimeParts - part_count_ ||
            inner.header_bytes() > MaxHeaderBytes - header_bytes_ ||
            inner.attachment_bytes() > DefaultMaxAttachmentBytes - attachment_bytes_)
            return fail("nested message exceeds MIME resource limits");
        part_count_ += inner.part_count();
        header_bytes_ += inner.header_bytes();
        attachment_bytes_ += inner.attachment_bytes();
        for (size_t i = 0; i < nested_result.attachments.size(); ++i) {
            Attachment attachment;
            attachment.filename = nested_result.attachments[i].filename;
            attachment.content_type = nested_result.attachments[i].content_type;
            attachment.content_id = nested_result.attachments[i].content_id;
            attachment.data = nested_result.attachments[i].data;
            attachment.inline_part = nested_result.attachments[i].inline_part;
            if (!message_.attachments.push(static_cast<Attachment &&>(attachment)))
                return fail("too many MIME attachments");
        }
        if (message_.plain_text.empty() && !nested_result.plain_text.empty())
            message_.plain_text = nested_result.plain_text;
        if (message_.html_text.empty() && !nested_result.html_text.empty())
            message_.html_text = nested_result.html_text;
        if (!nested_result.warning.empty()) warn();
        return true;
    }
    if (is_attachment) return append_attachment(media, disposition, headers, body, body_length);

    Buffer transfer(MaxRawMessageBytes);
    bool known_transfer = false;
    if (!decode_transfer(body, body_length, headers.transfer_encoding, transfer, known_transfer))
        return fail("invalid MIME text transfer encoding");
    if (!known_transfer) warn();
    const char *charset = media.charset.empty() ? "us-ascii" : media.charset.c_str();
    Buffer converted(MaxRawMessageBytes);
    bool known_charset = false, lossy_conversion = false;
    if (!convert_to_utf8(transfer.data(), transfer.size(), charset, converted,
        known_charset, lossy_conversion))
        return fail("decoded MIME text exceeds the 64 MiB message limit");
    if (!known_charset || lossy_conversion) warn();
    Text decoded;
    if (!converted.to_text(decoded)) return fail("out of memory");
    if (is_media(media, "text", "plain")) {
        if (message_.plain_text.empty()) message_.plain_text = static_cast<Text &&>(decoded);
    } else if (is_media(media, "text", "html")) {
        if (message_.html_text.empty()) {
            Text safe_html, html_warning, html_error;
            if (!sanitize_html(decoded.c_str(), decoded.size(), safe_html, html_warning, html_error))
                return fail("HTML MIME part could not be sanitized");
            if (!html_warning.empty()) warn();
            message_.html_text = static_cast<Text &&>(safe_html);
        }
    } else warn();
    return true;
}

} // namespace

bool parse_message(const char *bytes, size_t length, ParsedMessage &output, Text &error) {
    error.clear();
    if (!bytes && length) return set_error(error, "message bytes are missing");
    ParsedMessage parsed;
    Parser parser(reinterpret_cast<const unsigned char *>(bytes), length, parsed, error);
    if (!parser.parse()) return false;
    output = static_cast<ParsedMessage &&>(parsed);
    return true;
}

bool decode_modified_utf7(const char *bytes, size_t length, Text &output, Text &error) {
    error.clear();
    if ((!bytes && length) || length > MaxEditableTextBytes)
        return set_error(error, "invalid or oversized modified UTF-7 folder name");
    Buffer decoded(MaxEditableTextBytes);
    size_t at = 0;
    while (at < length) {
        const unsigned char c = static_cast<unsigned char>(bytes[at]);
        if (c != '&') {
            if (c < 0x20 || c > 0x7e) return set_error(error, "invalid modified UTF-7 folder name");
            if (!decoded.append_byte(c)) return set_error(error, "folder name exceeds the text limit");
            ++at;
            continue;
        }
        size_t end = at + 1;
        while (end < length && bytes[end] != '-') ++end;
        if (end == length) return set_error(error, "invalid modified UTF-7 folder name");
        if (end == at + 1) {
            if (!decoded.append_byte('&')) return set_error(error, "folder name exceeds the text limit");
            at = end + 1;
            continue;
        }
        Buffer utf16((end - at) * 2);
        int quartet[4];
        size_t count = 0;
        for (size_t i = at + 1; i < end; ++i) {
            unsigned char encoded = static_cast<unsigned char>(bytes[i]);
            if (encoded == ',') encoded = '/';
            const int value = base64_value(encoded);
            if (value < 0) return set_error(error, "invalid modified UTF-7 folder name");
            quartet[count++] = value;
            if (count == 4) {
                const unsigned char octets[3] = {
                    static_cast<unsigned char>((quartet[0] << 2) | (quartet[1] >> 4)),
                    static_cast<unsigned char>((quartet[1] << 4) | (quartet[2] >> 2)),
                    static_cast<unsigned char>((quartet[2] << 6) | quartet[3])
                };
                if (!utf16.append(octets, sizeof(octets))) return set_error(error, "folder name is too large");
                count = 0;
            }
        }
        if (count == 2) {
            const unsigned char octet = static_cast<unsigned char>((quartet[0] << 2) | (quartet[1] >> 4));
            if (!utf16.append_byte(octet)) return set_error(error, "folder name is too large");
        } else if (count == 3) {
            const unsigned char octets[2] = {
                static_cast<unsigned char>((quartet[0] << 2) | (quartet[1] >> 4)),
                static_cast<unsigned char>((quartet[1] << 4) | (quartet[2] >> 2))
            };
            if (!utf16.append(octets, sizeof(octets))) return set_error(error, "folder name is too large");
        } else if (count != 0) return set_error(error, "invalid modified UTF-7 folder name");
        if (utf16.size() & 1) return set_error(error, "invalid modified UTF-7 folder name");
        for (size_t i = 0; i < utf16.size(); i += 2) {
            uint32_t codepoint = (static_cast<uint32_t>(utf16.data()[i]) << 8) | utf16.data()[i + 1];
            if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                if (i + 3 >= utf16.size()) return set_error(error, "invalid modified UTF-7 surrogate pair");
                const uint32_t low = (static_cast<uint32_t>(utf16.data()[i + 2]) << 8) | utf16.data()[i + 3];
                if (low < 0xdc00 || low > 0xdfff) return set_error(error, "invalid modified UTF-7 surrogate pair");
                codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                i += 2;
            } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff)
                return set_error(error, "invalid modified UTF-7 surrogate pair");
            if (!append_utf8_codepoint(decoded, codepoint))
                return set_error(error, "folder name exceeds the text limit");
        }
        at = end + 1;
    }
    Text next;
    if (!decoded.to_text(next)) return set_error(error, "out of memory");
    output = static_cast<Text &&>(next);
    return true;
}

namespace {

class Writer {
    MimeSink &sink_;
    unsigned char buffer_[4096];
    size_t used_ = 0;
    size_t header_bytes_ = 0;
    bool writing_headers_ = false;
public:
    explicit Writer(MimeSink &sink) : sink_(sink) {}
    bool flush() {
        if (!used_) return true;
        if (sink_.bytes_written > MaxRawMessageBytes ||
            used_ > MaxRawMessageBytes - sink_.bytes_written) {
            sink_.error.set("serialized message exceeds the 64 MiB limit");
            return false;
        }
        if (!sink_.write || !sink_.write(sink_.context, buffer_, used_)) {
            if (sink_.error.empty()) sink_.error.set("MIME output sink rejected data");
            return false;
        }
        sink_.bytes_written += used_;
        used_ = 0;
        return true;
    }
    bool write(const void *bytes, size_t length) {
        if (writing_headers_ && (header_bytes_ > MaxHeaderBytes ||
            length > MaxHeaderBytes - header_bytes_)) {
            sink_.error.set("serialized MIME headers exceed the 256 KiB limit");
            return false;
        }
        if (writing_headers_) header_bytes_ += length;
        const unsigned char *source = static_cast<const unsigned char *>(bytes);
        while (length) {
            const size_t room = sizeof(buffer_) - used_;
            const size_t count = length < room ? length : room;
            memcpy(buffer_ + used_, source, count);
            used_ += count;
            source += count;
            length -= count;
            if (used_ == sizeof(buffer_) && !flush()) return false;
        }
        return true;
    }
    bool cstr(const char *text) { return write(text, strlen(text)); }
    bool finish() { return flush(); }
    void set_error(const char *message) { sink_.error.set(message); }
    bool begin_headers() {
        if (writing_headers_) return false;
        writing_headers_ = true;
        return true;
    }
    bool end_headers() {
        if (!writing_headers_) return false;
        writing_headers_ = false;
        return true;
    }
};

struct RecipientLists { Array<Address> to, cc, bcc; };

bool format_origination_date(Text &header) {
    static const char *weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const time_t now = time(nullptr);
    if (now == static_cast<time_t>(-1)) return false;
    struct tm utc;
    if (!gmtime_r(&now, &utc) || utc.tm_wday < 0 || utc.tm_wday > 6 ||
        utc.tm_mon < 0 || utc.tm_mon > 11 || utc.tm_mday < 1 || utc.tm_mday > 31 ||
        utc.tm_hour < 0 || utc.tm_hour > 23 || utc.tm_min < 0 || utc.tm_min > 59 ||
        utc.tm_sec < 0 || utc.tm_sec > 60) return false;
    const int year = utc.tm_year + 1900;
    if (year < 0 || year > 9999) return false;
    char bytes[64];
    const int length = snprintf(bytes, sizeof(bytes), "Date: %s, %d %s %04d %02d:%02d:%02d +0000\r\n",
        weekdays[utc.tm_wday], utc.tm_mday, months[utc.tm_mon], year,
        utc.tm_hour, utc.tm_min, utc.tm_sec);
    return length > 0 && static_cast<size_t>(length) < sizeof(bytes) &&
        header.set(bytes, static_cast<size_t>(length));
}

bool raw_recipients_empty(const Text &input) {
    for (size_t i = 0; i < input.size(); ++i)
        if (!is_ows(static_cast<unsigned char>(input.c_str()[i]))) return false;
    return true;
}

bool parse_raw_recipient(const Text &input, Array<Address> &addresses, Text &error) {
    if (raw_recipients_empty(input)) return true;
    return parse_addresses(input.c_str(), input.size(), addresses, error);
}

bool resolve_recipients(const Draft &draft, RecipientLists &recipients, Text &error) {
    if (draft.raw_recipient_input) {
        return parse_raw_recipient(draft.to_input, recipients.to, error) &&
            parse_raw_recipient(draft.cc_input, recipients.cc, error) &&
            parse_raw_recipient(draft.bcc_input, recipients.bcc, error);
    }
    if (!recipients.to.clone_from(draft.to) || !recipients.cc.clone_from(draft.cc) ||
        !recipients.bcc.clone_from(draft.bcc)) {
        error.set("out of memory while preparing recipients");
        return false;
    }
    return true;
}

bool contains_non_ascii(const Text &text) {
    for (size_t i = 0; i < text.size(); ++i)
        if (static_cast<unsigned char>(text.c_str()[i]) >= 0x80) return true;
    return false;
}

bool write_encoded_word(Writer &writer, const Text &text, bool force_encoding = false) {
    if (!contains_non_ascii(text) && !force_encoding) return writer.write(text.c_str(), text.size());
    size_t offset = 0;
    bool first = true;
    while (offset < text.size()) {
        size_t end = offset, byte_count = 0;
        while (end < text.size()) {
            const unsigned char c = static_cast<unsigned char>(text.c_str()[end]);
            const size_t scalar = c < 0x80 ? 1 : ((c & 0xe0) == 0xc0 ? 2 : ((c & 0xf0) == 0xe0 ? 3 : 4));
            if (byte_count + scalar > 42 && end > offset) break;
            end += scalar;
            byte_count += scalar;
        }
        if (!first && !writer.cstr("\r\n ")) return false;
        if (!writer.cstr("=?UTF-8?B?")) return false;
        for (size_t i = offset; i < end;) {
            const unsigned char a = static_cast<unsigned char>(text.c_str()[i++]);
            const bool have_b = i < end;
            const unsigned char b = have_b ? static_cast<unsigned char>(text.c_str()[i++]) : 0;
            const bool have_c = i < end;
            const unsigned char c = have_c ? static_cast<unsigned char>(text.c_str()[i++]) : 0;
            static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            const unsigned char encoded[4] = {
                static_cast<unsigned char>(alphabet[a >> 2]),
                static_cast<unsigned char>(alphabet[((a & 3) << 4) | (b >> 4)]),
                static_cast<unsigned char>(have_b ? alphabet[((b & 15) << 2) | (c >> 6)] : '='),
                static_cast<unsigned char>(have_c ? alphabet[c & 63] : '=')
            };
            if (!writer.write(encoded, sizeof(encoded))) return false;
        }
        if (!writer.cstr("?=")) return false;
        offset = end;
        first = false;
    }
    return true;
}

bool write_mailbox(Writer &writer, const Address &address) {
    Text serialized, error;
    if (!contains_non_ascii(address.display_name) && address.display_name.size() <= 60) {
        if (!serialize_address(address, serialized, error)) return false;
        return writer.write(serialized.c_str(), serialized.size());
    }
    if (!write_encoded_word(writer, address.display_name, true) || !writer.cstr(" <")) return false;
    if (address.local_part_quoted && !writer.cstr("\"")) return false;
    for (size_t i = 0; i < address.local_part.size(); ++i) {
        const char c = address.local_part.c_str()[i];
        if (address.local_part_quoted && (c == '"' || c == '\\') && !writer.cstr("\\")) return false;
        if (!writer.write(&c, 1)) return false;
    }
    if (address.local_part_quoted && !writer.cstr("\"")) return false;
    return writer.cstr("@") && writer.write(address.domain.c_str(), address.domain.size()) && writer.cstr(">");
}

bool write_address_header(Writer &writer, const char *name, const Array<Address> &addresses) {
    if (addresses.empty()) return true;
    if (!writer.cstr(name) || !writer.cstr(": ")) return false;
    for (size_t i = 0; i < addresses.size(); ++i) {
        if (i && !writer.cstr(",\r\n ")) return false;
        if (!write_mailbox(writer, addresses[i])) return false;
    }
    return writer.cstr("\r\n");
}

bool write_single_address_header(Writer &writer, const char *name, const Address &address) {
    return writer.cstr(name) && writer.cstr(": ") && write_mailbox(writer, address) && writer.cstr("\r\n");
}

bool write_ascii_header(Writer &writer, const char *name, const Text &value) {
    if (value.empty()) return true;
    return writer.cstr(name) && writer.cstr(": ") && writer.write(value.c_str(), value.size()) &&
        writer.cstr("\r\n");
}

bool write_subject_header(Writer &writer, const Text &subject) {
    return writer.cstr("Subject: ") && write_encoded_word(writer, subject, subject.size() > 60) &&
        writer.cstr("\r\n");
}

bool build_message_id(Text &id) {
    static unsigned long long sequence = 0;
    const unsigned long long serial = __atomic_add_fetch(&sequence, 1ULL, __ATOMIC_RELAXED);
    struct timespec now;
    unsigned long long seconds = 0, nanos = 0;
    if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
        seconds = static_cast<unsigned long long>(now.tv_sec);
        nanos = static_cast<unsigned long long>(now.tv_nsec) ^ serial;
    } else {
        seconds = serial;
        nanos = reinterpret_cast<uintptr_t>(&id) ^ serial;
    }
    char value[96];
    const int length = snprintf(value, sizeof(value), "<%llx.%llx@reliefos.local>", seconds, nanos ^ sequence);
    return length > 0 && static_cast<size_t>(length) < sizeof(value) && id.set(value, static_cast<size_t>(length));
}

bool make_boundary(const char *kind, Text &boundary) {
    static unsigned long long sequence = 0;
    const unsigned long long serial = __atomic_add_fetch(&sequence, 1ULL, __ATOMIC_RELAXED);
    struct timespec now;
    unsigned long long seconds = 0, nanos = 0;
    if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
        seconds = static_cast<unsigned long long>(now.tv_sec);
        nanos = static_cast<unsigned long long>(now.tv_nsec) ^ serial;
    } else {
        seconds = serial;
        nanos = reinterpret_cast<uintptr_t>(&boundary) ^ serial;
    }
    char value[128];
    const int length = snprintf(value, sizeof(value), "=_ReliefOS_%s_%llx_%llx",
        kind, seconds, nanos ^ sequence);
    return length > 0 && static_cast<size_t>(length) < sizeof(value) &&
        boundary.set(value, static_cast<size_t>(length));
}

bool write_qp_byte(Writer &writer, unsigned char byte, size_t &column) {
    char encoded[3];
    size_t length = 1;
    if ((byte >= 33 && byte <= 60) || (byte >= 62 && byte <= 126)) encoded[0] = static_cast<char>(byte);
    else {
        static const char hex[] = "0123456789ABCDEF";
        encoded[0] = '=';
        encoded[1] = hex[byte >> 4];
        encoded[2] = hex[byte & 15];
        length = 3;
    }
    if (column + length > 75) {
        if (!writer.cstr("=\r\n")) return false;
        column = 0;
    }
    if (!writer.write(encoded, length)) return false;
    column += length;
    return true;
}

bool write_qp_text(Writer &writer, const Text &text) {
    size_t column = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text.c_str()[i]);
        if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < text.size() && text.c_str()[i + 1] == '\n') ++i;
            if (!writer.cstr("\r\n")) return false;
            column = 0;
        } else if (!write_qp_byte(writer, c, column)) return false;
    }
    return writer.cstr("\r\n");
}

bool write_text_part(Writer &writer, const char *boundary, const char *subtype, const Text &text) {
    if (!writer.cstr("--") || !writer.cstr(boundary) || !writer.cstr("\r\n") ||
        !writer.begin_headers() || !writer.cstr("Content-Type: text/") || !writer.cstr(subtype) ||
        !writer.cstr("; charset=UTF-8\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\n") ||
        !writer.end_headers()) return false;
    return write_qp_text(writer, text);
}

void encode_triplet(const unsigned char *bytes, size_t length, unsigned char output[4]) {
    static const unsigned char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char a = bytes[0];
    const unsigned char b = length > 1 ? bytes[1] : 0;
    const unsigned char c = length > 2 ? bytes[2] : 0;
    output[0] = alphabet[a >> 2];
    output[1] = alphabet[((a & 3) << 4) | (b >> 4)];
    output[2] = length > 1 ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
    output[3] = length > 2 ? alphabet[c & 63] : '=';
}

class Base64Writer {
    Writer &writer_;
    size_t column_ = 0;
public:
    explicit Base64Writer(Writer &writer) : writer_(writer) {}
    bool write_triplet(const unsigned char *bytes, size_t length) {
        unsigned char encoded[4];
        encode_triplet(bytes, length, encoded);
        if (column_ + 4 > 76) {
            if (!writer_.cstr("\r\n")) return false;
            column_ = 0;
        }
        if (!writer_.write(encoded, sizeof(encoded))) return false;
        column_ += sizeof(encoded);
        return true;
    }
    bool finish() { return writer_.cstr("\r\n"); }
};

bool stream_attachment_data(Writer &writer, const Attachment &attachment) {
    Base64Writer encoded(writer);
    unsigned char input[3072];
    unsigned char carry[3];
    size_t carry_size = 0, offset = 0;
    const size_t source_size = attachment.source.read ? attachment.source.size : attachment.data.size();
    while (offset < source_size) {
        size_t count = 0;
        if (attachment.source.read) {
            const size_t request = source_size - offset < sizeof(input) ? source_size - offset : sizeof(input);
            if (!attachment.source.read(attachment.source.context, offset, input, request, &count) ||
                !count || count > request) {
                writer.set_error("attachment source read failed");
                return false;
            }
        } else {
            count = source_size - offset < sizeof(input) ? source_size - offset : sizeof(input);
            memcpy(input, attachment.data.data() + offset, count);
        }
        offset += count;
        for (size_t i = 0; i < count; ++i) {
            carry[carry_size++] = input[i];
            if (carry_size == 3) {
                if (!encoded.write_triplet(carry, 3)) return false;
                carry_size = 0;
            }
        }
    }
    if (carry_size && !encoded.write_triplet(carry, carry_size)) return false;
    return encoded.finish();
}

bool safe_media_type(const Text &media) {
    if (media.empty() || media.size() > 127) return false;
    bool slash = false;
    for (size_t i = 0; i < media.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(media.c_str()[i]);
        if (c == '/' && !slash && i && i + 1 < media.size()) { slash = true; continue; }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '!' || c == '#' || c == '$' || c == '&' ||
            c == '^' || c == '_' || c == '.' || c == '+' || c == '-')) return false;
    }
    return slash;
}

bool filename_attr_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '!' || c == '#' || c == '$' || c == '&' ||
        c == '+' || c == '-' || c == '.' || c == '^' || c == '_' || c == 96 || c == '|' || c == '~';
}

size_t filename_segment_count(const Text &filename) {
    size_t count = 1, column = 0;
    for (size_t i = 0; i < filename.size(); ++i) {
        const size_t unit = filename_attr_char(static_cast<unsigned char>(filename.c_str()[i])) ? 1 : 3;
        if (column && column + unit > 42) { ++count; column = 0; }
        column += unit;
    }
    return count;
}

bool write_filename_parameters(Writer &writer, const Text &filename, const char *disposition) {
    if (!writer.cstr("Content-Disposition: ") || !writer.cstr(disposition)) return false;
    if (filename.empty()) return writer.cstr("\r\n");
    const size_t segments = filename_segment_count(filename);
    size_t offset = 0;
    for (size_t part = 0; part < segments; ++part) {
        char prefix[48];
        const int prefix_length = snprintf(prefix, sizeof(prefix),
            part ? "\r\n filename*%zu*=" : ";\r\n filename*%zu*=", part);
        if (prefix_length <= 0 || static_cast<size_t>(prefix_length) >= sizeof(prefix) ||
            !writer.write(prefix, static_cast<size_t>(prefix_length))) return false;
        if (part == 0 && !writer.cstr("UTF-8''")) return false;
        size_t column = 0;
        while (offset < filename.size()) {
            const unsigned char c = static_cast<unsigned char>(filename.c_str()[offset]);
            const size_t unit = filename_attr_char(c) ? 1 : 3;
            if (column && column + unit > 42) break;
            if (unit == 1) {
                if (!writer.write(&c, 1)) return false;
            } else {
                static const char hex[] = "0123456789ABCDEF";
                const char escaped[3] = {'%', hex[c >> 4], hex[c & 15]};
                if (!writer.write(escaped, sizeof(escaped))) return false;
            }
            column += unit;
            ++offset;
        }
        if (!writer.cstr(part + 1 == segments ? "\r\n" : ";")) return false;
    }
    return true;
}

bool write_attachment_headers(Writer &writer, const Attachment &attachment) {
    if (!writer.begin_headers()) return false;
    Text fallback_type;
    const Text *media = &attachment.content_type;
    if (media->empty() || ascii_equal(media->c_str(), media->size(), "message/rfc822")) {
        if (!fallback_type.set("application/octet-stream")) return false;
        media = &fallback_type;
    }
    if (!safe_media_type(*media) || !valid_utf8(attachment.filename.c_str(), attachment.filename.size()) ||
        !valid_utf8(attachment.content_id.c_str(), attachment.content_id.size())) return false;
    if (!writer.cstr("Content-Type: ") || !writer.write(media->c_str(), media->size()) ||
        !writer.cstr("\r\nContent-Transfer-Encoding: base64\r\n")) return false;
    if (!write_filename_parameters(writer, attachment.filename, attachment.inline_part ? "inline" : "attachment"))
        return false;
    if (!attachment.content_id.empty()) {
        for (size_t i = 0; i < attachment.content_id.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(attachment.content_id.c_str()[i]);
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '+' ||
                c == '@' || c == '!')) return false;
        }
        if (!writer.cstr("Content-ID: <") ||
            !writer.write(attachment.content_id.c_str(), attachment.content_id.size()) ||
            !writer.cstr(">\r\n")) return false;
    }
    return writer.cstr("\r\n") && writer.end_headers();
}

bool valid_message_id_token(const char *bytes, size_t length) {
    if (length < 5 || bytes[0] != '<' || bytes[length - 1] != '>') return false;
    size_t at_sign = SIZE_MAX;
    for (size_t i = 1; i + 1 < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (c < 0x21 || c > 0x7e || c == '<' || c == '>') return false;
        if (c == '@') {
            if (at_sign != SIZE_MAX || i == 1 || i + 2 == length) return false;
            at_sign = i;
        }
    }
    return at_sign != SIZE_MAX;
}

bool valid_message_id_list(const Text &value, bool exactly_one) {
    if (value.empty()) return !exactly_one;
    size_t at = 0, count = 0;
    while (at < value.size()) {
        while (at < value.size() && value.c_str()[at] == ' ') ++at;
        if (at == value.size()) break;
        const size_t begin = at;
        while (at < value.size() && value.c_str()[at] != ' ') ++at;
        if (at - begin > 900 || !valid_message_id_token(value.c_str() + begin, at - begin)) return false;
        ++count;
    }
    return count && (!exactly_one || count == 1);
}

bool write_id_list_header(Writer &writer, const char *name, const Text &value) {
    if (value.empty()) return true;
    if (!writer.cstr(name) || !writer.cstr(": ")) return false;
    size_t column = strlen(name) + 2;
    size_t at = 0;
    bool first = true;
    while (at < value.size()) {
        while (at < value.size() && value.c_str()[at] == ' ') ++at;
        if (at == value.size()) break;
        const size_t begin = at;
        while (at < value.size() && value.c_str()[at] != ' ') ++at;
        const size_t token_length = at - begin;
        if (!first) {
            if (column + 1 + token_length > 78) {
                if (!writer.cstr("\r\n ")) return false;
                column = 1;
            } else {
                if (!writer.cstr(" ")) return false;
                ++column;
            }
        }
        if (!writer.write(value.c_str() + begin, token_length)) return false;
        column += token_length;
        first = false;
    }
    return writer.cstr("\r\n");
}

bool valid_attachment_headers(const Attachment &attachment) {
    Text fallback_type;
    const Text *media = &attachment.content_type;
    if (media->empty()) {
        if (!fallback_type.set("application/octet-stream")) return false;
        media = &fallback_type;
    }
    if (!safe_media_type(*media) || attachment.filename.size() > MaxEditableTextBytes ||
        attachment.content_id.size() > MaxEditableTextBytes ||
        !valid_utf8(attachment.filename.c_str(), attachment.filename.size()) ||
        !valid_utf8(attachment.content_id.c_str(), attachment.content_id.size())) return false;
    for (size_t i = 0; i < attachment.content_id.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(attachment.content_id.c_str()[i]);
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '+' ||
            c == '@' || c == '!')) return false;
    }
    return true;
}

bool append_recipients(Envelope &envelope, const Array<Address> &addresses) {
    for (size_t i = 0; i < addresses.size(); ++i)
        if (!envelope.recipients.push(addresses[i])) return false;
    return true;
}

bool write_top_headers(Writer &writer, const Address &sender, const RecipientLists &recipients,
                       const Text &date_header, const Text &subject, const Text &message_id, const Text &in_reply_to,
                       const Text &references, bool has_attachments, bool rich,
                       const Text &mixed_boundary, const Text &alternative_boundary) {
    if (!writer.begin_headers() || !write_single_address_header(writer, "From", sender) ||
        !writer.write(date_header.c_str(), date_header.size()) ||
        !write_address_header(writer, "To", recipients.to) ||
        !write_address_header(writer, "Cc", recipients.cc) ||
        !write_subject_header(writer, subject) ||
        !write_ascii_header(writer, "Message-ID", message_id) ||
        !write_id_list_header(writer, "In-Reply-To", in_reply_to) ||
        !write_id_list_header(writer, "References", references) ||
        !writer.cstr("MIME-Version: 1.0\r\n")) return false;
    if (has_attachments) {
        if (!writer.cstr("Content-Type: multipart/mixed; boundary=\"") ||
            !writer.write(mixed_boundary.c_str(), mixed_boundary.size()) || !writer.cstr("\"\r\n")) return false;
    } else if (rich) {
        if (!writer.cstr("Content-Type: multipart/alternative; boundary=\"") ||
            !writer.write(alternative_boundary.c_str(), alternative_boundary.size()) || !writer.cstr("\"\r\n")) return false;
    } else if (!writer.cstr("Content-Type: text/plain; charset=UTF-8\r\n") ||
               !writer.cstr("Content-Transfer-Encoding: quoted-printable\r\n")) return false;
    return writer.cstr("\r\n") && writer.end_headers();
}

bool write_alternative_body(Writer &writer, const Text &boundary, const Text &plain,
                            const Text &html) {
    return write_text_part(writer, boundary.c_str(), "plain", plain) &&
        write_text_part(writer, boundary.c_str(), "html", html) &&
        writer.cstr("--") && writer.cstr(boundary.c_str()) && writer.cstr("--\r\n");
}

bool write_attachment_part(Writer &writer, const Text &boundary, const Attachment &attachment) {
    if (!writer.cstr("--") || !writer.cstr(boundary.c_str()) || !writer.cstr("\r\n") ||
        !write_attachment_headers(writer, attachment)) return false;
    return stream_attachment_data(writer, attachment);
}

bool write_mixed_body(Writer &writer, const Draft &draft, const Text &html,
                      const Text &mixed_boundary, const Text &alternative_boundary) {
    if (draft.rich_format) {
        if (!writer.cstr("--") || !writer.cstr(mixed_boundary.c_str()) || !writer.cstr("\r\n") ||
            !writer.begin_headers() ||
            !writer.cstr("Content-Type: multipart/alternative; boundary=\"") ||
            !writer.write(alternative_boundary.c_str(), alternative_boundary.size()) ||
            !writer.cstr("\"\r\n\r\n") || !writer.end_headers() ||
            !write_alternative_body(writer, alternative_boundary, draft.body, html)) return false;
    } else if (!write_text_part(writer, mixed_boundary.c_str(), "plain", draft.body)) return false;

    for (size_t i = 0; i < draft.attachments.size(); ++i)
        if (!write_attachment_part(writer, mixed_boundary, draft.attachments[i])) return false;
    return writer.cstr("--") && writer.cstr(mixed_boundary.c_str()) && writer.cstr("--\r\n");
}

bool write_message_body(Writer &writer, const Draft &draft, const Text &html,
                        const Text &mixed_boundary, const Text &alternative_boundary) {
    if (!draft.attachments.empty())
        return write_mixed_body(writer, draft, html, mixed_boundary, alternative_boundary);
    if (draft.rich_format)
        return write_alternative_body(writer, alternative_boundary, draft.body, html);
    return write_qp_text(writer, draft.body);
}

bool fail_before_stream(MimeSink &sink, Text &error, const char *message) {
    sink.error.set(message);
    error = sink.error;
    return false;
}

bool fail_stream(MimeSink &sink, Text &error) {
    if (sink.error.empty()) sink.error.set("MIME serialization failed");
    error = sink.error;
    return false;
}

struct BytesSinkContext { Buffer buffer{MaxRawMessageBytes}; Text error; };

bool append_to_bytes_sink(void *context, const unsigned char *bytes, size_t size) {
    BytesSinkContext *target = static_cast<BytesSinkContext *>(context);
    if (target->buffer.append(bytes, size)) return true;
    target->error.set("out of memory or serialized message exceeds the 64 MiB limit");
    return false;
}

bool valid_sender_display(const Text &name) {
    if (name.size() > MaxEditableTextBytes || !valid_utf8(name.c_str(), name.size())) return false;
    for (size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name.c_str()[i]);
        if (!c || c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

} // namespace

bool encode_message(const Draft &draft, const Account &account, MimeSink &sink,
                    Envelope &envelope, Text &error) {
    sink.bytes_written = 0;
    sink.error.clear();
    error.clear();
    if (!sink.write) return fail_before_stream(sink, error, "MIME output sink is missing");
    if (draft.account_id != account.id)
        return fail_before_stream(sink, error, "draft and sender account do not match");

    Error detailed;
    if (!validate_draft(draft, detailed)) {
        sink.error = detailed.message;
        error = sink.error;
        return false;
    }
    if (!valid_sender_display(account.display_name))
        return fail_before_stream(sink, error, "sender display name is invalid");

    Array<Address> account_addresses;
    if (!parse_addresses(account.email_address.c_str(), account.email_address.size(),
        account_addresses, error) || account_addresses.size() != 1 ||
        !account_addresses[0].display_name.empty()) {
        if (error.empty()) error.set("sender email address is invalid");
        sink.error = error;
        return false;
    }
    Address sender = static_cast<Address &&>(account_addresses[0]);
    sender.display_name = account.display_name;
    sender.display_name_quoted = false;
    Text sender_validation;
    if (!serialize_address(sender, sender_validation, detailed)) {
        sink.error = detailed.message;
        error = sink.error;
        return false;
    }

    RecipientLists recipients;
    if (!resolve_recipients(draft, recipients, error)) {
        sink.error = error;
        return false;
    }
    if (recipients.to.empty() && recipients.cc.empty() && recipients.bcc.empty())
        return fail_before_stream(sink, error, "at least one recipient is required");

    if ((!draft.message_id.empty() && !valid_message_id_list(draft.message_id, true)) ||
        !valid_message_id_list(draft.in_reply_to, false) ||
        !valid_message_id_list(draft.references, false))
        return fail_before_stream(sink, error, "invalid Message-ID, In-Reply-To, or References value");
    Text message_id;
    if (draft.message_id.empty()) {
        if (!build_message_id(message_id)) return fail_before_stream(sink, error, "out of memory");
    } else message_id = draft.message_id;

    Text safe_html, html_warning;
    if (draft.rich_format && !sanitize_html(draft.html_body.c_str(), draft.html_body.size(),
        safe_html, html_warning, error)) {
        sink.error = error;
        return false;
    }
    for (size_t i = 0; i < draft.attachments.size(); ++i) {
        if (!valid_attachment_headers(draft.attachments[i]))
            return fail_before_stream(sink, error, "invalid attachment metadata");
    }

    Envelope next_envelope;
    next_envelope.sender = sender;
    if (!append_recipients(next_envelope, recipients.to) ||
        !append_recipients(next_envelope, recipients.cc) ||
        !append_recipients(next_envelope, recipients.bcc))
        return fail_before_stream(sink, error, "out of memory while preparing envelope recipients");

    const bool has_attachments = !draft.attachments.empty();
    Text mixed_boundary, alternative_boundary;
    if (has_attachments && !make_boundary("mixed", mixed_boundary))
        return fail_before_stream(sink, error, "could not create MIME boundary");
    if (draft.rich_format && !make_boundary("alternative", alternative_boundary))
        return fail_before_stream(sink, error, "could not create MIME boundary");

    Text date_header;
    if (!format_origination_date(date_header))
        return fail_before_stream(sink, error, "could not prepare the message Date header");

    /* Prepare the complete outer header block before touching the caller's sink.
     * This makes Date generation and the header limit one atomic preflight. */
    BytesSinkContext prepared_context;
    MimeSink prepared_sink;
    prepared_sink.write = append_to_bytes_sink;
    prepared_sink.context = &prepared_context;
    Writer prepared_writer(prepared_sink);
    if (!write_top_headers(prepared_writer, sender, recipients, date_header, draft.subject,
        message_id, draft.in_reply_to, draft.references, has_attachments, draft.rich_format,
        mixed_boundary, alternative_boundary) || !prepared_writer.finish()) {
        sink.error = prepared_sink.error.empty() ? prepared_context.error : prepared_sink.error;
        if (sink.error.empty()) sink.error.set("could not prepare message headers");
        error = sink.error;
        return false;
    }

    Writer writer(sink);
    if (!writer.begin_headers() ||
        !writer.write(prepared_context.buffer.data(), prepared_context.buffer.size()) ||
        !writer.end_headers() ||
        !write_message_body(writer, draft, safe_html, mixed_boundary, alternative_boundary) ||
        !writer.finish()) return fail_stream(sink, error);
    envelope = static_cast<Envelope &&>(next_envelope);
    return true;
}

bool encode_message(const Draft &draft, const Account &account, Bytes &output,
                    Envelope &envelope, Text &error) {
    BytesSinkContext context;
    MimeSink sink;
    sink.write = append_to_bytes_sink;
    sink.context = &context;
    Envelope next_envelope;
    if (!encode_message(draft, account, sink, next_envelope, error)) {
        if (!context.error.empty()) error = context.error;
        return false;
    }
    Bytes next_output;
    if (!next_output.set(context.buffer.data(), context.buffer.size())) {
        error.set("out of memory while creating serialized message");
        return false;
    }
    output = static_cast<Bytes &&>(next_output);
    envelope = static_cast<Envelope &&>(next_envelope);
    return true;
}

} // namespace rmail
