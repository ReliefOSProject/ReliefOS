#include "model.h"

namespace rmail {
namespace {

bool set_error(Error &error, ErrorCode code, const char *message) {
    error.code = code;
    error.message.clear();
    if (!error.message.set(message)) error.code = ErrorCode::OutOfMemory;
    return false;
}

void clear_error(Error &error) {
    error.code = ErrorCode::None;
    error.message.clear();
}

bool copy_error(Text &output, Error &error, bool result) {
    output = static_cast<Text &&>(error.message);
    return result;
}

bool decode_utf8_one(const char *bytes, size_t length, size_t &at, uint32_t &codepoint) {
    if (at >= length) return false;
    const unsigned char first = static_cast<unsigned char>(bytes[at]);
    if (first <= 0x7f) {
        codepoint = first;
        ++at;
        return true;
    }

    unsigned count;
    uint32_t value;
    if (first >= 0xc2 && first <= 0xdf) {
        count = 2;
        value = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
        count = 3;
        value = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
        count = 4;
        value = first & 0x07;
    } else {
        return false;
    }
    if (count > length - at) return false;
    for (unsigned i = 1; i < count; ++i) {
        const unsigned char continuation = static_cast<unsigned char>(bytes[at + i]);
        if ((continuation & 0xc0) != 0x80) return false;
        if (i == 1) {
            if (first == 0xe0 && continuation < 0xa0) return false;
            if (first == 0xed && continuation > 0x9f) return false;
            if (first == 0xf0 && continuation < 0x90) return false;
            if (first == 0xf4 && continuation > 0x8f) return false;
        }
        value = (value << 6) | (continuation & 0x3f);
    }
    if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    at += count;
    codepoint = value;
    return true;
}

bool has_line_break_or_nul(const char *bytes, size_t length) {
    for (size_t i = 0; i < length; ++i)
        if (!bytes[i] || bytes[i] == '\r' || bytes[i] == '\n') return true;
    return false;
}

bool has_forbidden_control(const Text &text, bool allow_newlines) {
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char byte = static_cast<unsigned char>(text.c_str()[i]);
        if (!byte) return true;
        if (byte < 0x20 && !(allow_newlines && (byte == '\r' || byte == '\n' || byte == '\t')))
            return true;
        if (byte == 0x7f) return true;
    }
    return false;
}

bool valid_field_text(const Text &text, bool allow_newlines) {
    return text.size() <= MaxEditableTextBytes && valid_utf8(text.c_str(), text.size()) &&
        !has_forbidden_control(text, allow_newlines);
}

bool is_atext(unsigned char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9') || byte == '!' || byte == '#' || byte == '$' ||
        byte == '%' || byte == '&' || byte == '\'' || byte == '*' || byte == '+' ||
        byte == '-' || byte == '/' || byte == '=' || byte == '?' || byte == '^' ||
        byte == '_' || byte == '`' || byte == '{' || byte == '|' || byte == '}' || byte == '~';
}

bool is_dot_atom(const char *bytes, size_t length, bool domain) {
    if (!length || bytes[0] == '.' || bytes[length - 1] == '.') return false;
    bool previous_dot = false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char byte = static_cast<unsigned char>(bytes[i]);
        if (byte >= 0x80) return false;
        if (byte == '.') {
            if (previous_dot) return false;
            previous_dot = true;
            continue;
        }
        previous_dot = false;
        if (domain) {
            if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                    (byte >= '0' && byte <= '9') || byte == '-')) return false;
        } else if (!is_atext(byte)) {
            return false;
        }
    }
    if (domain) {
        size_t label_start = 0;
        for (size_t i = 0; i <= length; ++i) {
            if (i == length || bytes[i] == '.') {
                const size_t label_length = i - label_start;
                if (!label_length || label_length > 63 || bytes[label_start] == '-' ||
                    bytes[i - 1] == '-') return false;
                label_start = i + 1;
            }
        }
    }
    return true;
}

struct Builder {
    Array<char> bytes;

    bool append(const char *value, size_t length) {
        if (length > SIZE_MAX - bytes.size()) return false;
        const size_t old_size = bytes.size();
        if (!bytes.resize(old_size + length)) return false;
        if (length) memcpy(bytes.data() + old_size, value, length);
        return true;
    }

    bool append_byte(char value) { return append(&value, 1); }
    bool finish(Text &output) const {
        if (bytes.empty()) {
            output.clear();
            return true;
        }
        return output.set(bytes.data(), bytes.size());
    }
};

bool copy_slice(Text &output, const char *bytes, size_t begin, size_t end) {
    if (end < begin) return false;
    return output.set(bytes + begin, end - begin);
}

bool is_ows(char byte) { return byte == ' ' || byte == '\t'; }

void trim_ows(const char *bytes, size_t &begin, size_t &end) {
    while (begin < end && is_ows(bytes[begin])) ++begin;
    while (end > begin && is_ows(bytes[end - 1])) --end;
}

bool valid_display_phrase(const char *bytes, size_t length) {
    if (!length || bytes[0] == ' ' || bytes[length - 1] == ' ') return false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char byte = static_cast<unsigned char>(bytes[i]);
        if (byte >= 0x80) continue;
        if (byte == ' ') continue;
        if (byte < 0x21 || byte == 0x7f || byte == '(' || byte == ')' || byte == '<' ||
            byte == '>' || byte == '[' || byte == ']' || byte == ':' || byte == ';' ||
            byte == '@' || byte == '\\' || byte == ',' || byte == '"') return false;
    }
    return true;
}

bool parse_quoted_text(const char *bytes, size_t begin, size_t end, Text &output,
                       Error &error, bool local_part) {
    if (end - begin < 2 || bytes[begin] != '"' || bytes[end - 1] != '"')
        return set_error(error, ErrorCode::InvalidInput, "invalid quoted string");
    Builder decoded;
    for (size_t i = begin + 1; i + 1 < end; ++i) {
        unsigned char byte = static_cast<unsigned char>(bytes[i]);
        if (byte == '\\') {
            if (++i + 1 >= end)
                return set_error(error, ErrorCode::InvalidInput, "invalid quoted escape");
            byte = static_cast<unsigned char>(bytes[i]);
            if (byte >= 0x80 && local_part)
                return set_error(error, ErrorCode::UnsupportedInternationalizedAddress,
                    "unsupported internationalized address");
            if (byte < 0x20 || byte > 0x7e)
                return set_error(error, ErrorCode::InvalidInput, "invalid quoted escape");
        } else if (byte >= 0x80 && local_part) {
            return set_error(error, ErrorCode::UnsupportedInternationalizedAddress,
                "unsupported internationalized address");
        } else if (byte < 0x20 || byte == 0x7f || byte == '"') {
            return set_error(error, ErrorCode::InvalidInput, "invalid quoted string");
        }
        if (!decoded.append_byte(static_cast<char>(byte)))
            return set_error(error, ErrorCode::OutOfMemory, "out of memory");
    }
    if (!decoded.finish(output)) return set_error(error, ErrorCode::OutOfMemory, "out of memory");
    return true;
}

bool validate_domain(const char *domain, size_t length, Error &error) {
    if (!length || length > 255)
        return set_error(error, ErrorCode::InvalidInput, "invalid address domain");
    for (size_t i = 0; i < length; ++i) {
        if (static_cast<unsigned char>(domain[i]) >= 0x80)
            return set_error(error, ErrorCode::UnsupportedInternationalizedAddress,
                "unsupported internationalized address");
    }
    if (domain[0] == '[') {
        if (length < 3 || domain[length - 1] != ']')
            return set_error(error, ErrorCode::InvalidInput, "invalid address domain literal");
        for (size_t i = 1; i + 1 < length; ++i) {
            const unsigned char byte = static_cast<unsigned char>(domain[i]);
            if (byte < 33 || byte > 126 || byte == '[' || byte == ']' || byte == '\\')
                return set_error(error, ErrorCode::InvalidInput, "invalid address domain literal");
        }
        return true;
    }
    if (!is_dot_atom(domain, length, true))
        return set_error(error, ErrorCode::InvalidInput, "invalid address domain");
    return true;
}

bool parse_addr_spec(const char *bytes, size_t length, Address &address, Error &error) {
    if (!length || length > 254)
        return set_error(error, ErrorCode::InvalidInput, "invalid address syntax");
    bool quoted = false;
    bool escaped = false;
    size_t at_sign = SIZE_MAX;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char byte = static_cast<unsigned char>(bytes[i]);
        if (quoted && escaped) {
            escaped = false;
            continue;
        }
        if (quoted && byte == '\\') {
            escaped = true;
            continue;
        }
        if (byte == '"') {
            quoted = !quoted;
            continue;
        }
        if (!quoted && byte == '@') {
            if (at_sign != SIZE_MAX)
                return set_error(error, ErrorCode::InvalidInput, "invalid address syntax");
            at_sign = i;
        }
    }
    if (quoted || escaped || at_sign == SIZE_MAX || !at_sign || at_sign + 1 >= length)
        return set_error(error, ErrorCode::InvalidInput, "invalid address syntax");

    const size_t local_length = at_sign;
    const char *local = bytes;
    Text local_part;
    bool local_quoted = false;
    if (local[0] == '"') {
        local_quoted = true;
        if (!parse_quoted_text(local, 0, local_length, local_part, error, true)) return false;
        size_t wire_length = 2;
        for (size_t i = 0; i < local_part.size(); ++i) {
            const unsigned char byte = static_cast<unsigned char>(local_part.c_str()[i]);
            if (byte == '"' || byte == '\\') {
                if (wire_length == SIZE_MAX)
                    return set_error(error, ErrorCode::LimitExceeded, "address is too long");
                ++wire_length;
            }
            if (wire_length == SIZE_MAX)
                return set_error(error, ErrorCode::LimitExceeded, "address is too long");
            ++wire_length;
        }
        if (!local_part.size() || wire_length > 64)
            return set_error(error, ErrorCode::InvalidInput, "invalid address local part");
    } else {
        if (local_length > 64)
            return set_error(error, ErrorCode::InvalidInput, "invalid address local part");
        for (size_t i = 0; i < local_length; ++i) {
            if (static_cast<unsigned char>(local[i]) >= 0x80)
                return set_error(error, ErrorCode::UnsupportedInternationalizedAddress,
                    "unsupported internationalized address");
        }
        if (!is_dot_atom(local, local_length, false))
            return set_error(error, ErrorCode::InvalidInput, "invalid address local part");
        if (!local_part.set(local, local_length))
            return set_error(error, ErrorCode::OutOfMemory, "out of memory");
    }

    const char *domain = bytes + at_sign + 1;
    const size_t domain_length = length - at_sign - 1;
    if (!validate_domain(domain, domain_length, error)) return false;
    if (!address.domain.set(domain, domain_length) ||
        !address.local_part.set(local_part.c_str(), local_part.size()))
        return set_error(error, ErrorCode::OutOfMemory, "out of memory");
    address.local_part_quoted = local_quoted;
    return true;
}

bool parse_mailbox(const char *bytes, size_t length, Address &address, Error &error) {
    size_t begin = 0;
    size_t end = length;
    trim_ows(bytes, begin, end);
    if (begin == end) return set_error(error, ErrorCode::EmptyRecipient, "empty recipient");

    bool quoted = false;
    bool escaped = false;
    size_t angle = SIZE_MAX;
    for (size_t i = begin; i < end; ++i) {
        const char byte = bytes[i];
        if (quoted && escaped) {
            escaped = false;
            continue;
        }
        if (quoted && byte == '\\') {
            escaped = true;
            continue;
        }
        if (byte == '"') {
            quoted = !quoted;
            continue;
        }
        if (!quoted && byte == '<') {
            angle = i;
            break;
        }
        if (!quoted && byte == '>')
            return set_error(error, ErrorCode::InvalidInput, "invalid address syntax");
    }

    size_t address_begin = begin;
    size_t address_end = end;
    if (angle != SIZE_MAX) {
        size_t display_begin = begin;
        size_t display_end = angle;
        trim_ows(bytes, display_begin, display_end);
        if (display_begin < display_end) {
            if (bytes[display_begin] == '"') {
                address.display_name_quoted = true;
                if (!parse_quoted_text(bytes, display_begin, display_end, address.display_name,
                        error, false)) return false;
            } else {
                for (size_t i = display_begin; i < display_end; ++i) {
                    const unsigned char byte = static_cast<unsigned char>(bytes[i]);
                    if (byte < 0x20 && byte != '\t')
                        return set_error(error, ErrorCode::HeaderInjection,
                            "header injection is not allowed");
                }
                if (!valid_display_phrase(bytes + display_begin, display_end - display_begin))
                    return set_error(error, ErrorCode::InvalidInput, "invalid display name");
                if (!copy_slice(address.display_name, bytes, display_begin, display_end))
                    return set_error(error, ErrorCode::OutOfMemory, "out of memory");
            }
        }

        bool in_quote = false;
        bool in_escape = false;
        size_t close = SIZE_MAX;
        for (size_t i = angle + 1; i < end; ++i) {
            const char byte = bytes[i];
            if (in_quote && in_escape) {
                in_escape = false;
                continue;
            }
            if (in_quote && byte == '\\') {
                in_escape = true;
                continue;
            }
            if (byte == '"') {
                in_quote = !in_quote;
                continue;
            }
            if (!in_quote && byte == '>') {
                close = i;
                break;
            }
        }
        if (close == SIZE_MAX) return set_error(error, ErrorCode::InvalidInput, "missing angle bracket");
        for (size_t i = close + 1; i < end; ++i)
            if (!is_ows(bytes[i]))
                return set_error(error, ErrorCode::InvalidInput, "unexpected text after address");
        address_begin = angle + 1;
        address_end = close;
    }

    return parse_addr_spec(bytes + address_begin, address_end - address_begin, address, error);
}

bool parse_addresses_sized(const char *input, size_t length, Array<Address> &output, Error &error) {
    clear_error(error);
    if (!input && length) return set_error(error, ErrorCode::InvalidInput, "address input is missing");
    if (!length) return set_error(error, ErrorCode::EmptyRecipient, "empty recipient");
    if (has_line_break_or_nul(input, length))
        return set_error(error, ErrorCode::HeaderInjection, "header injection is not allowed");
    if (!valid_utf8(input, length)) return set_error(error, ErrorCode::InvalidUtf8, "invalid UTF-8");

    Array<Address> parsed;
    size_t item_start = 0;
    bool quoted = false;
    bool escaped = false;
    bool in_angle = false;
    for (size_t i = 0; i <= length; ++i) {
        const bool at_end = i == length;
        const char byte = at_end ? 0 : input[i];
        if (!at_end && quoted && escaped) {
            escaped = false;
            continue;
        }
        if (!at_end && quoted && byte == '\\') {
            escaped = true;
            continue;
        }
        if (!at_end && byte == '"') {
            quoted = !quoted;
            continue;
        }
        if (!at_end && !quoted && byte == '<') {
            if (in_angle) return set_error(error, ErrorCode::InvalidInput, "invalid address syntax");
            in_angle = true;
            continue;
        }
        if (!at_end && !quoted && byte == '>') {
            if (!in_angle) return set_error(error, ErrorCode::InvalidInput, "invalid address syntax");
            in_angle = false;
            continue;
        }
        if (at_end || (!quoted && !in_angle && byte == ',')) {
            size_t start = item_start;
            size_t end = i;
            trim_ows(input, start, end);
            if (start == end)
                return set_error(error, ErrorCode::EmptyRecipient, "empty recipient");
            Address address;
            if (!parse_mailbox(input + start, end - start, address, error)) return false;
            if (!parsed.push(static_cast<Address &&>(address)))
                return set_error(error, ErrorCode::OutOfMemory, "out of memory");
            item_start = i + 1;
        }
    }
    if (quoted || escaped || in_angle)
        return set_error(error, ErrorCode::InvalidInput, "unterminated address syntax");
    output.swap(parsed);
    return true;
}

bool validate_address(const Address &address, Error &error) {
    if (!valid_field_text(address.display_name, false))
        return set_error(error, ErrorCode::HeaderInjection, "header injection is not allowed");
    if (!address.local_part.size() || address.local_part.size() > 64)
        return set_error(error, ErrorCode::InvalidInput, "invalid address local part");
    for (size_t i = 0; i < address.local_part.size(); ++i) {
        if (static_cast<unsigned char>(address.local_part.c_str()[i]) >= 0x80)
            return set_error(error, ErrorCode::UnsupportedInternationalizedAddress,
                "unsupported internationalized address");
    }
    if (address.local_part_quoted) {
        size_t wire_length = 2;
        for (size_t i = 0; i < address.local_part.size(); ++i) {
            const unsigned char byte = static_cast<unsigned char>(address.local_part.c_str()[i]);
            if (byte < 0x20 || byte > 0x7e)
                return set_error(error, ErrorCode::InvalidInput, "invalid address local part");
            if (byte == '"' || byte == '\\') ++wire_length;
            ++wire_length;
        }
        if (wire_length > 64)
            return set_error(error, ErrorCode::InvalidInput, "invalid address local part");
    } else if (address.local_part.size() > 64 ||
        !is_dot_atom(address.local_part.c_str(), address.local_part.size(), false)) {
        return set_error(error, ErrorCode::InvalidInput, "invalid address local part");
    }
    if (!validate_domain(address.domain.c_str(), address.domain.size(), error)) return false;
    if (address.display_name.size() > 1024)
        return set_error(error, ErrorCode::LimitExceeded, "display name is too long");
    return true;
}

bool append_escaped_quoted(Builder &builder, const char *bytes, size_t length) {
    if (!builder.append_byte('"')) return false;
    for (size_t i = 0; i < length; ++i) {
        if ((bytes[i] == '"' || bytes[i] == '\\') && !builder.append_byte('\\')) return false;
        if (!builder.append_byte(bytes[i])) return false;
    }
    return builder.append_byte('"');
}

bool serialize_address_impl(const Address &address, Text &output, Error &error) {
    clear_error(error);
    if (!validate_address(address, error)) return false;
    Builder serialized;
    if (!address.display_name.empty()) {
        if (address.display_name_quoted ||
            !valid_display_phrase(address.display_name.c_str(), address.display_name.size())) {
            if (!append_escaped_quoted(serialized, address.display_name.c_str(),
                    address.display_name.size()))
                return set_error(error, ErrorCode::OutOfMemory, "out of memory");
        } else if (!serialized.append(address.display_name.c_str(), address.display_name.size())) {
            return set_error(error, ErrorCode::OutOfMemory, "out of memory");
        }
        if (!serialized.append(" <", 2)) return set_error(error, ErrorCode::OutOfMemory, "out of memory");
    }
    if (address.local_part_quoted) {
        if (!append_escaped_quoted(serialized, address.local_part.c_str(), address.local_part.size()))
            return set_error(error, ErrorCode::OutOfMemory, "out of memory");
    } else if (!serialized.append(address.local_part.c_str(), address.local_part.size())) {
        return set_error(error, ErrorCode::OutOfMemory, "out of memory");
    }
    if (!serialized.append_byte('@') ||
        !serialized.append(address.domain.c_str(), address.domain.size()))
        return set_error(error, ErrorCode::OutOfMemory, "out of memory");
    if (!address.display_name.empty() && !serialized.append_byte('>'))
        return set_error(error, ErrorCode::OutOfMemory, "out of memory");
    if (!serialized.finish(output)) return set_error(error, ErrorCode::OutOfMemory, "out of memory");
    return true;
}

bool ascii_equal_fold(const char *left, size_t left_length, const char *right) {
    const size_t right_length = strlen(right);
    if (left_length != right_length) return false;
    for (size_t i = 0; i < left_length; ++i) {
        unsigned char a = static_cast<unsigned char>(left[i]);
        unsigned char b = static_cast<unsigned char>(right[i]);
        if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = static_cast<unsigned char>(b + ('a' - 'A'));
        if (a != b) return false;
    }
    return true;
}

int hex_value(unsigned char byte) {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
    return -1;
}

bool percent_decode(const char *input, size_t length, Array<char> &output, Error &error) {
    Array<char> decoded;
    if (!decoded.reserve(length)) return set_error(error, ErrorCode::OutOfMemory, "out of memory");
    for (size_t i = 0; i < length; ++i) {
        unsigned char byte = static_cast<unsigned char>(input[i]);
        if (byte == '%') {
            if (i + 2 >= length)
                return set_error(error, ErrorCode::InvalidMailto, "invalid percent escape in mailto URI");
            const int high = hex_value(static_cast<unsigned char>(input[i + 1]));
            const int low = hex_value(static_cast<unsigned char>(input[i + 2]));
            if (high < 0 || low < 0)
                return set_error(error, ErrorCode::InvalidMailto, "invalid percent escape in mailto URI");
            byte = static_cast<unsigned char>((high << 4) | low);
            i += 2;
        }
        if (!byte) return set_error(error, ErrorCode::HeaderInjection, "header injection is not allowed");
        if (!decoded.push(static_cast<char>(byte)))
            return set_error(error, ErrorCode::OutOfMemory, "out of memory");
    }
    if (!valid_utf8(decoded.data(), decoded.size()))
        return set_error(error, ErrorCode::InvalidUtf8, "invalid UTF-8 in mailto URI");
    output.swap(decoded);
    return true;
}

bool append_address_value(const Array<char> &value, Array<Address> &target, Error &error) {
    if (!value.size()) return set_error(error, ErrorCode::EmptyRecipient, "empty recipient");
    Array<Address> parsed;
    if (!parse_addresses_sized(value.data(), value.size(), parsed, error)) return false;
    for (size_t i = 0; i < parsed.size(); ++i) {
        if (!target.push(static_cast<Address &&>(parsed[i])))
            return set_error(error, ErrorCode::OutOfMemory, "out of memory");
    }
    return true;
}

bool parse_mailto_sized(const char *uri, size_t length, Draft &draft, Error &error) {
    clear_error(error);
    if (!uri && length) return set_error(error, ErrorCode::InvalidMailto, "mailto URI is missing");
    if (length < 7) return set_error(error, ErrorCode::InvalidMailto, "invalid mailto URI");
    if (has_line_break_or_nul(uri, length))
        return set_error(error, ErrorCode::HeaderInjection, "header injection is not allowed");
    if (!valid_utf8(uri, length)) return set_error(error, ErrorCode::InvalidUtf8, "invalid UTF-8 in mailto URI");
    const char scheme[] = "mailto:";
    for (size_t i = 0; i < sizeof(scheme) - 1; ++i) {
        unsigned char actual = static_cast<unsigned char>(uri[i]);
        if (actual >= 'A' && actual <= 'Z') actual = static_cast<unsigned char>(actual + ('a' - 'A'));
        if (actual != static_cast<unsigned char>(scheme[i]))
            return set_error(error, ErrorCode::InvalidMailto, "invalid mailto URI scheme");
    }

    size_t uri_end = length;
    for (size_t i = 7; i < uri_end; ++i) {
        if (uri[i] == '#') {
            uri_end = i;
            break;
        }
    }
    size_t query = uri_end;
    for (size_t i = 7; i < uri_end; ++i) {
        if (uri[i] == '?') {
            query = i;
            break;
        }
    }

    Draft next;
    if (query > 7) {
        Array<char> recipients;
        if (!percent_decode(uri + 7, query - 7, recipients, error)) return false;
        if (recipients.size() && !append_address_value(recipients, next.to, error)) return false;
    }

    size_t field_start = query < uri_end ? query + 1 : uri_end;
    while (field_start < uri_end) {
        size_t field_end = field_start;
        while (field_end < uri_end && uri[field_end] != '&') ++field_end;
        size_t equals = field_start;
        while (equals < field_end && uri[equals] != '=') ++equals;
        if (equals == field_end)
            return set_error(error, ErrorCode::InvalidMailto, "invalid mailto header field");
        Array<char> name;
        Array<char> value;
        if (!percent_decode(uri + field_start, equals - field_start, name, error) ||
            !percent_decode(uri + equals + 1, field_end - equals - 1, value, error)) return false;
        if (ascii_equal_fold(name.data(), name.size(), "to")) {
            if (!append_address_value(value, next.to, error)) return false;
        } else if (ascii_equal_fold(name.data(), name.size(), "cc")) {
            if (!append_address_value(value, next.cc, error)) return false;
        } else if (ascii_equal_fold(name.data(), name.size(), "bcc")) {
            if (!append_address_value(value, next.bcc, error)) return false;
        } else if (ascii_equal_fold(name.data(), name.size(), "subject")) {
            Text subject;
            if (!subject.set(value.data(), value.size()))
                return set_error(error, ErrorCode::OutOfMemory, "out of memory");
            if (subject.size() > MaxEditableTextBytes)
                return set_error(error, ErrorCode::LimitExceeded, "mailto subject exceeds the editable text limit");
            if (!valid_field_text(subject, false))
                return set_error(error, ErrorCode::HeaderInjection, "header injection is not allowed");
            next.subject = static_cast<Text &&>(subject);
        } else if (ascii_equal_fold(name.data(), name.size(), "body")) {
            Text body;
            if (!body.set(value.data(), value.size()))
                return set_error(error, ErrorCode::OutOfMemory, "out of memory");
            if (body.size() > MaxEditableTextBytes || !valid_field_text(body, true))
                return set_error(error, ErrorCode::LimitExceeded, "mailto body is invalid or too large");
            next.body = static_cast<Text &&>(body);
        }
        field_start = field_end + 1;
    }
    next.send_stage = SendStage::Draft;
    draft = static_cast<Draft &&>(next);
    return true;
}

bool safe_host(const Text &host) {
    if (!host.size() || host.size() > 255 || !valid_utf8(host.c_str(), host.size())) return false;
    for (size_t i = 0; i < host.size(); ++i) {
        const unsigned char byte = static_cast<unsigned char>(host.c_str()[i]);
        if (!byte || byte < 0x21 || byte == 0x7f) return false;
    }
    return true;
}

bool valid_enum_values(const Account &account) {
    return static_cast<unsigned>(account.incoming_protocol) <= static_cast<unsigned>(IncomingProtocol::Pop3) &&
        static_cast<unsigned>(account.incoming_security) <= static_cast<unsigned>(TransportSecurity::StartTls) &&
        static_cast<unsigned>(account.outgoing_security) <= static_cast<unsigned>(TransportSecurity::StartTls) &&
        static_cast<unsigned>(account.authentication) <= static_cast<unsigned>(AuthenticationMethod::ApplicationPassword);
}

bool validate_account_impl(const Account &account, Error &error) {
    clear_error(error);
    if (!account.id || !valid_enum_values(account) || account.display_name.empty() ||
        account.username.empty())
        return set_error(error, ErrorCode::InvalidAccount, "invalid account identity");
    if (has_line_break_or_nul(account.display_name.c_str(), account.display_name.size()) ||
        has_line_break_or_nul(account.username.c_str(), account.username.size()))
        return set_error(error, ErrorCode::HeaderInjection, "header injection is not allowed");
    if (!valid_field_text(account.display_name, false) || !valid_field_text(account.username, false))
        return set_error(error, ErrorCode::InvalidAccount, "invalid account identity");
    if ((!account.smtp_username.empty() &&
            (!valid_field_text(account.smtp_username, false) ||
                has_line_break_or_nul(account.smtp_username.c_str(), account.smtp_username.size()))) ||
        (!account.custom_ca_file.empty() &&
            (account.custom_ca_file.size() > 4096 || !valid_field_text(account.custom_ca_file, false))) ||
        account.connect_timeout_ms < 1000 || account.connect_timeout_ms > 120000 ||
        account.io_timeout_ms < 1000 || account.io_timeout_ms > 120000 ||
        account.poll_interval_minutes > 1440 || account.cache_days > 3650)
        return set_error(error, ErrorCode::InvalidAccount, "invalid account preferences");
    const Text *special_folders[] = {&account.inbox_folder, &account.sent_folder,
        &account.drafts_folder, &account.trash_folder, &account.junk_folder,
        &account.archive_folder};
    for (size_t i = 0; i < sizeof(special_folders) / sizeof(special_folders[0]); ++i)
        if ((!special_folders[i]->empty() &&
                (!valid_field_text(*special_folders[i], false) ||
                    has_line_break_or_nul(special_folders[i]->c_str(), special_folders[i]->size()))))
            return set_error(error, ErrorCode::InvalidAccount, "invalid account folder mapping");
    if (!account.incoming_port || account.incoming_port > 65535 ||
        !account.outgoing_port || account.outgoing_port > 65535)
        return set_error(error, ErrorCode::InvalidPort, "invalid server port");
    if (!safe_host(account.incoming_host) || !safe_host(account.outgoing_host))
        return set_error(error, ErrorCode::InvalidAccount, "invalid server host");

    Array<Address> parsed;
    if (!parse_addresses_sized(account.email_address.c_str(), account.email_address.size(), parsed, error))
        return false;
    if (parsed.size() != 1 || !parsed[0].display_name.empty())
        return set_error(error, ErrorCode::InvalidAccount, "invalid account email address");
    return true;
}

bool validate_draft_impl(const Draft &draft, Error &error) {
    clear_error(error);
    if (draft.subject.size() > MaxEditableTextBytes || draft.body.size() > MaxEditableTextBytes ||
        draft.html_body.size() > MaxEditableTextBytes || draft.to_input.size() > MaxEditableTextBytes ||
        draft.cc_input.size() > MaxEditableTextBytes || draft.bcc_input.size() > MaxEditableTextBytes ||
        draft.editor_state.size() > MaxEditorStateBytes)
        return set_error(error, ErrorCode::LimitExceeded, "draft data exceeds its configured limit");
    if (!valid_field_text(draft.subject, false))
        return set_error(error, ErrorCode::HeaderInjection, "header injection is not allowed");
    if (!valid_utf8(draft.body.c_str(), draft.body.size()))
        return set_error(error, ErrorCode::InvalidUtf8, "draft body is not valid UTF-8");
    if (has_forbidden_control(draft.body, true))
        return set_error(error, ErrorCode::HeaderInjection, "draft body contains forbidden control bytes");
    if (!valid_utf8(draft.html_body.c_str(), draft.html_body.size()))
        return set_error(error, ErrorCode::InvalidUtf8, "draft HTML body is not valid UTF-8");
    if (has_forbidden_control(draft.html_body, true))
        return set_error(error, ErrorCode::HeaderInjection, "draft HTML body contains forbidden control bytes");
    if (!valid_field_text(draft.message_id, false) || !valid_field_text(draft.in_reply_to, false) ||
        !valid_field_text(draft.references, false))
        return set_error(error, ErrorCode::HeaderInjection, "header injection is not allowed");
    if (static_cast<unsigned>(draft.send_stage) > static_cast<unsigned>(SendStage::Archived))
        return set_error(error, ErrorCode::InvalidInput, "invalid send stage");

    if (!draft.raw_recipient_input) {
        const Array<Address> *address_lists[3] = {&draft.to, &draft.cc, &draft.bcc};
        for (size_t list = 0; list < 3; ++list) {
            for (size_t i = 0; i < address_lists[list]->size(); ++i)
                if (!validate_address((*address_lists[list])[i], error)) return false;
        }
    }

    size_t total_attachment_bytes = 0;
    for (size_t i = 0; i < draft.attachments.size(); ++i) {
        const Attachment &attachment = draft.attachments[i];
        if (!valid_field_text(attachment.filename, false) ||
            !valid_field_text(attachment.content_type, false) ||
            !valid_field_text(attachment.content_id, false))
            return set_error(error, ErrorCode::HeaderInjection, "invalid attachment metadata");
        if ((attachment.source.read && !attachment.data.empty()) ||
            (!attachment.source.read && (attachment.source.context || attachment.source.size)))
            return set_error(error, ErrorCode::InvalidInput, "invalid attachment source");
        const size_t attachment_size = attachment.source.read ? attachment.source.size : attachment.data.size();
        if (attachment_size > DefaultMaxAttachmentBytes - total_attachment_bytes)
            return set_error(error, ErrorCode::LimitExceeded, "attachment data exceeds the default limit");
        total_attachment_bytes += attachment_size;
    }
    return true;
}

}

bool valid_utf8(const char *bytes, size_t length) {
    if (!bytes && length) return false;
    size_t at = 0;
    while (at < length) {
        uint32_t codepoint;
        if (!decode_utf8_one(bytes, length, at, codepoint)) return false;
    }
    return true;
}

bool parse_addresses(const char *input, Array<Address> &output, Error &error) {
    if (!input) {
        clear_error(error);
        return set_error(error, ErrorCode::InvalidInput, "address input is missing");
    }
    return parse_addresses_sized(input, strlen(input), output, error);
}

bool parse_addresses(const char *input, size_t length, Array<Address> &output, Error &error) {
    return parse_addresses_sized(input, length, output, error);
}

bool parse_addresses(const char *input, Array<Address> &output, Text &error) {
    Error detailed;
    const bool result = parse_addresses(input, output, detailed);
    return copy_error(error, detailed, result);
}

bool parse_addresses(const char *input, size_t length, Array<Address> &output, Text &error) {
    Error detailed;
    const bool result = parse_addresses(input, length, output, detailed);
    return copy_error(error, detailed, result);
}

bool serialize_address(const Address &address, Text &output, Error &error) {
    return serialize_address_impl(address, output, error);
}

bool serialize_address(const Address &address, Text &output, Text &error) {
    Error detailed;
    const bool result = serialize_address(address, output, detailed);
    return copy_error(error, detailed, result);
}

bool parse_mailto(const char *uri, Draft &draft, Error &error) {
    if (!uri) {
        clear_error(error);
        return set_error(error, ErrorCode::InvalidMailto, "mailto URI is missing");
    }
    return parse_mailto_sized(uri, strlen(uri), draft, error);
}

bool parse_mailto(const char *uri, size_t length, Draft &draft, Error &error) {
    return parse_mailto_sized(uri, length, draft, error);
}

bool parse_mailto(const char *uri, Draft &draft, Text &error) {
    Error detailed;
    const bool result = parse_mailto(uri, draft, detailed);
    return copy_error(error, detailed, result);
}

bool parse_mailto(const char *uri, size_t length, Draft &draft, Text &error) {
    Error detailed;
    const bool result = parse_mailto(uri, length, draft, detailed);
    return copy_error(error, detailed, result);
}

bool validate_account(const Account &account, Error &error) {
    return validate_account_impl(account, error);
}

bool validate_account(const Account &account, Text &error) {
    Error detailed;
    const bool result = validate_account(account, detailed);
    return copy_error(error, detailed, result);
}

bool validate_draft(const Draft &draft, Error &error) {
    return validate_draft_impl(draft, error);
}

bool validate_draft(const Draft &draft, Text &error) {
    Error detailed;
    const bool result = validate_draft(draft, detailed);
    return copy_error(error, detailed, result);
}

template<class Container>
static bool clone_array(const Container &source, Container &destination) {
    for (size_t i = 0; i < source.size(); ++i)
        if (!destination.push(source[i])) return false;
    return true;
}

bool clone_draft(const Draft &source, Draft &destination, Text &error) {
    error.clear();
    Draft next;
    next.id = source.id;
    next.revision = source.revision;
    next.account_id = source.account_id;
    next.to_input = source.to_input;
    next.cc_input = source.cc_input;
    next.bcc_input = source.bcc_input;
    next.raw_recipient_input = source.raw_recipient_input;
    next.subject = source.subject;
    next.body = source.body;
    next.html_body = source.html_body;
    next.editor_state = source.editor_state;
    next.rich_format = source.rich_format;
    next.message_id = source.message_id;
    next.in_reply_to = source.in_reply_to;
    next.references = source.references;
    next.send_stage = source.send_stage;
    if (!clone_array(source.to, next.to) || !clone_array(source.cc, next.cc) ||
        !clone_array(source.bcc, next.bcc) || !clone_array(source.attachments, next.attachments)) {
        error.set("out of memory cloning draft");
        return false;
    }
    destination = static_cast<Draft &&>(next);
    return true;
}

bool set_raw_recipient_inputs(Draft &draft,
    const char *to, size_t to_length,
    const char *cc, size_t cc_length,
    const char *bcc, size_t bcc_length,
    Text &error) {
    error.clear();
    if ((to_length && !to) || (cc_length && !cc) || (bcc_length && !bcc)) {
        error.set("recipient input is missing");
        return false;
    }
    if (to_length > MaxEditableTextBytes || cc_length > MaxEditableTextBytes ||
        bcc_length > MaxEditableTextBytes) {
        error.set("recipient input exceeds the editable text limit");
        return false;
    }
    Text next_to, next_cc, next_bcc;
    if (!next_to.set(to, to_length) || !next_cc.set(cc, cc_length) || !next_bcc.set(bcc, bcc_length)) {
        error.set("out of memory");
        return false;
    }
    draft.to_input = static_cast<Text &&>(next_to);
    draft.cc_input = static_cast<Text &&>(next_cc);
    draft.bcc_input = static_cast<Text &&>(next_bcc);
    draft.raw_recipient_input = true;
    return true;
}

}
